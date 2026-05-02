#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "memory.h"
#include "value_table.h"
#include "vm.h"

VM vm;

static void closeUpvalues(Value* last);
ObjString* stringify(Value value);
static InterpretResult run(int baseFrame);

// Builtin functions and methods are in builtins.c.
// vmCallValue, getCallableArity, callValue, and runtimeError
// are declared in vm.h for use by builtins.c.

static void resetStack() {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
  vm.exceptionHandlerCount = 0;
  vm.currentException = NIL_VAL;
}

void runtimeError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame* frame = &vm.frames[i];
    ObjFunction* function = frame->closure->function;
    size_t instruction = frame->ip - function->chunk.code - 1;
    fprintf(stderr, "[line %d] in ",
            function->chunk.lines[instruction]);
    if (function->name == NULL) {
      fprintf(stderr, "script\n");
    } else {
      fprintf(stderr, "%s()\n", function->name->chars);
    }
  }

  resetStack();
}

static bool throwException(Value value) {
  vm.currentException = value;

  while (vm.exceptionHandlerCount > 0) {
    ExceptionHandler* handler =
        &vm.exceptionHandlers[vm.exceptionHandlerCount - 1];
    vm.exceptionHandlerCount--;

    // Restore VM state.
    vm.stackTop = handler->stackTop;
    vm.frameCount = handler->frameCount;
    closeUpvalues(vm.stackTop);

    if (handler->type == HANDLER_CATCH) {
      // Push exception value for catch block.
      push(value);
    } else {
      // HANDLER_FINALLY: push completion state (exception tag + value).
      push(NUMBER_VAL(COMPLETE_EXCEPTION));
      push(value);
    }

    handler->frame->ip = handler->handlerIp;
    return true;
  }

  // No handler found — print as runtime error.
  if (IS_STRING(value)) {
    runtimeError("%s", AS_STRING(value)->chars);
  } else {
    ObjString* str = stringify(value);
    runtimeError("%s", str->chars);
  }
  return false;
}

ObjString* stringify(Value value) {
  if (IS_STRING(value)) return AS_STRING(value);

  if (IS_NIL(value)) return copyString("nil", 3);
  if (IS_BOOL(value)) return AS_BOOL(value) ?
      copyString("true", 4) : copyString("false", 5);

  if (IS_NUMBER(value)) {
    char buffer[24];
    int len = snprintf(buffer, sizeof(buffer), "%g", AS_NUMBER(value));
    return copyString(buffer, len);
  }

  if (IS_OBJ(value)) {
    switch (OBJ_TYPE(value)) {
      case OBJ_CLASS:
        return AS_CLASS(value)->name;
      case OBJ_FILE:
        return AS_FILE(value)->path;
      case OBJ_MODULE:
        return AS_MODULE(value)->name;
      case OBJ_INSTANCE: {
        ObjString* name = AS_INSTANCE(value)->klass->name;
        int totalLen = name->length + 9;
        char* chars = ALLOCATE(char, totalLen + 1);
        memcpy(chars, name->chars, name->length);
        memcpy(chars + name->length, " instance", 9);
        chars[totalLen] = '\0';
        return takeString(chars, totalLen);
      }
      default:
        return copyString("<object>", 8);
    }
  }
  return copyString("<unknown>", 9);
}

char* readFile(const char* path) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) return NULL;

  fseek(file, 0L, SEEK_END);
  size_t fileSize = ftell(file);
  rewind(file);

  // Use malloc (not ALLOCATE) -- temporary buffer freed by the caller,
  // not a GC-managed object.
  char* buffer = (char*)malloc(fileSize + 1);
  if (buffer == NULL) { fclose(file); return NULL; }

  size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
  fclose(file);

  if (bytesRead < fileSize) { free(buffer); return NULL; }

  buffer[bytesRead] = '\0';
  return buffer;
}

static void copyNatives(Table* dest) {
  for (int i = 0; i < vm.globals.capacity; i++) {
    Entry* entry = &vm.globals.entries[i];
    if (entry->key != NULL && IS_OBJ(entry->value) &&
        IS_NATIVE(entry->value)) {
      tableSet(dest, entry->key, entry->value);
    }
  }
}

static char* buildModulePath(const char* baseDir,
                             const char* moduleName, int nameLen) {
  int baseDirLen = baseDir ? (int)strlen(baseDir) : 0;
  int maxLen = baseDirLen + 1 + nameLen + 5 + 1;
  // Use malloc -- temporary path string freed by resolveModulePath or
  // OP_IMPORT caller.
  char* path = (char*)malloc(maxLen);

  int pos = 0;
  if (baseDirLen > 0) {
    memcpy(path, baseDir, baseDirLen);
    pos = baseDirLen;
    if (path[pos - 1] != '/' && path[pos - 1] != '\\') {
      path[pos++] = '/';
    }
  }

  for (int i = 0; i < nameLen; i++) {
    path[pos++] = (moduleName[i] == '.') ? '/' : moduleName[i];
  }

  memcpy(path + pos, ".efto", 5);
  pos += 5;
  path[pos] = '\0';
  return path;
}

// Try each search path in order, return the first path where the file exists.
static char* resolveModulePath(const char* moduleName, int nameLen) {
  for (int i = 0; i < vm.searchPathCount; i++) {
    const char* dir = vm.searchPaths[i] ? vm.searchPaths[i]->chars : ".";
    char* candidate = buildModulePath(dir, moduleName, nameLen);
    FILE* f = fopen(candidate, "rb");
    if (f != NULL) {
      fclose(f);
      return candidate;
    }
    free(candidate);
  }
  return NULL;
}

void initVM() {
    resetStack();
    vm.objects = NULL;

    vm.bytesAllocated = 0;
    vm.nextGC = 1024 * 1024;

    vm.grayCount = 0;
    vm.grayCapacity = 0;
    vm.grayStack = NULL;

    initTable(&vm.globals);
    initTable(&vm.strings);
    initTable(&vm.importCache);
    vm.searchPathCount = 0;
    for (int i = 0; i < 8; i++) vm.searchPaths[i] = NULL;

    vm.initString = NULL;
    vm.initString = copyString("init", 4);
    vm.lengthString = NULL;
    vm.lengthString = copyString("length", 6);
    vm.sizeString = NULL;
    vm.sizeString = copyString("size", 4);

    registerBuiltins();
}

void freeVM() {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  freeTable(&vm.importCache);
  vm.initString = NULL;
  vm.lengthString = NULL;
  vm.sizeString = NULL;
  freeObjects();
}

void push(Value value) {
  *vm.stackTop = value;
  vm.stackTop++;
}

Value pop() {
  vm.stackTop--;
  return *vm.stackTop;
}

static Value peek(int distance) {
  return vm.stackTop[-1 - distance];
}

static bool call(ObjClosure* closure, int argCount) {
  bool hasRest = closure->function->hasRest;
  int arity = closure->function->arity;
  int minArity = closure->function->minArity;

  if (hasRest) {
    // Rest functions accept minArity or more arguments.
    if (argCount < minArity) {
      runtimeError("Expected at least %d arguments but got %d.",
          minArity, argCount);
      return false;
    }

    // Collect excess args into a rest array.
    int restStart = arity - 1; // index of rest parameter
    int restCount = (argCount > restStart) ? argCount - restStart : 0;

    ObjArray* restArray = newArray();
    push(OBJ_VAL(restArray)); // GC protect

    // Copy rest args into the array. They are below our GC-protection push.
    // Stack: [..., callee, arg0, ..., argN-1, restArray]
    // Rest args start at index restStart within args.
    for (int i = 0; i < restCount; i++) {
      writeValueArray(&restArray->elements,
                      vm.stackTop[-1 - argCount + restStart + i]);
    }

    pop(); // remove GC protection

    // Remove restCount values and push the rest array in their place.
    vm.stackTop -= restCount;
    push(OBJ_VAL(restArray));
    argCount = arity;
  } else {
    if (argCount < minArity || argCount > arity) {
      if (minArity == arity) {
        runtimeError("Expected %d arguments but got %d.",
            arity, argCount);
      } else {
        runtimeError("Expected %d to %d arguments but got %d.",
            minArity, arity, argCount);
      }
      return false;
    }
  }

  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  // Pad missing optional args with nil.
  for (int i = argCount; i < arity; i++) {
    push(NIL_VAL);
  }

  CallFrame* frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = closure->function->chunk.code;
  frame->slots = vm.stackTop - arity - 1;
  frame->argCount = argCount;
  return true;
}

bool callValue(Value callee, int argCount) {
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
      case OBJ_BOUND_METHOD: {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        vm.stackTop[-argCount - 1] = bound->receiver;
        return call(bound->method, argCount);
      }
      case OBJ_CLASS: {
        ObjClass* klass = AS_CLASS(callee);
        vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));

        Value initializer;
        if (tableGet(&klass->methods, vm.initString,
                     &initializer)) {
          return call(AS_CLOSURE(initializer), argCount);
        } else if (argCount != 0) {
          runtimeError("Expected 0 arguments but got %d.",
                       argCount);
          return false;
        }

        return true;
      }
      case OBJ_CLOSURE:
        return call(AS_CLOSURE(callee), argCount);
      case OBJ_NATIVE: {
        NativeFn native = AS_NATIVE(callee);
        Value result = native(argCount, vm.stackTop - argCount);
        vm.stackTop -= argCount + 1;
        push(result);
        return true;
      }
      default:
        break; // Non-callable object type.
    }
  }
  runtimeError("Can only call functions and classes.");
  return false;
}

bool vmCallValue(Value callee, int argCount, Value* result) {
  int framesBefore = vm.frameCount;
  if (!callValue(callee, argCount)) return false;
  if (vm.frameCount > framesBefore) {
    InterpretResult r = run(framesBefore);
    if (r != INTERPRET_OK) return false;
  }
  *result = pop();
  return true;
}

int getCallableArity(Value callee) {
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
      case OBJ_CLOSURE:
        return AS_CLOSURE(callee)->function->arity;
      case OBJ_BOUND_METHOD:
        return AS_BOUND_METHOD(callee)->method->function->arity;
      default: break;
    }
  }
  return -1;
}

static bool invokeFromClass(ObjClass* klass, ObjString* name, int argCount) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }

  if (IS_NATIVE_METHOD(method)) {
    ObjNativeMethod* native = AS_NATIVE_METHOD(method);
    if (argCount < native->minArity || argCount > native->arity) {
      if (native->minArity == native->arity) {
        runtimeError("Expected %d arguments but got %d.",
                     native->arity, argCount);
      } else {
        runtimeError("Expected %d to %d arguments but got %d.",
                     native->minArity, native->arity, argCount);
      }
      return false;
    }

    Value receiver = peek(argCount);
    Value result;
    if (!native->function(receiver, argCount,
                          vm.stackTop - argCount, &result)) {
      return false;
    }
    vm.stackTop -= argCount + 1;
    push(result);
    return true;
  }

  return call(AS_CLOSURE(method), argCount);
}

static bool invoke(ObjString* name, int argCount) {
  Value receiver = peek(argCount);

  if (IS_MODULE(receiver)) {
    ObjModule* module = AS_MODULE(receiver);
    Value value;
    if (!tableGet(&module->values, name, &value)) {
      runtimeError("Module '%s' has no member '%s'.",
                   module->name->chars, name->chars);
      return false;
    }
    vm.stackTop[-argCount - 1] = value;
    return callValue(value, argCount);
  }

  if (IS_STRING(receiver)) {
    return invokeFromClass(vm.stringMethods, name, argCount);
  }

  if (IS_ARRAY(receiver)) {
    return invokeFromClass(vm.arrayMethods, name, argCount);
  }

  if (IS_MAP(receiver)) {
    return invokeFromClass(vm.mapMethods, name, argCount);
  }

  if (IS_FILE(receiver)) {
    return invokeFromClass(vm.fileMethods, name, argCount);
  }

  if (!IS_INSTANCE(receiver)) {
    runtimeError("Only instances have methods.");
    return false;
  }

  ObjInstance* instance = AS_INSTANCE(receiver);

  Value value;
  if (tableGet(&instance->fields, name, &value)) {
    vm.stackTop[-argCount - 1] = value;
    return callValue(value, argCount);
  }

  return invokeFromClass(instance->klass, name, argCount);
}

static bool bindMethod(ObjClass* klass, ObjString* name) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }

  ObjBoundMethod* bound = newBoundMethod(peek(0), AS_CLOSURE(method));
  pop();
  push(OBJ_VAL(bound));
  return true;
}

static ObjUpvalue* captureUpvalue(Value* local) {
  ObjUpvalue* prevUpvalue = NULL;
  ObjUpvalue* upvalue = vm.openUpvalues;
  while (upvalue != NULL && upvalue->location > local) {
    prevUpvalue = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->location == local) {
    return upvalue;
  }

  ObjUpvalue* createdUpvalue = newUpvalue(local);

  createdUpvalue->next = upvalue;

  if (prevUpvalue == NULL) {
    vm.openUpvalues = createdUpvalue;
  } else {
    prevUpvalue->next = createdUpvalue;
  }

  return createdUpvalue;
}

static void closeUpvalues(Value* last) {
  while (vm.openUpvalues != NULL &&
         vm.openUpvalues->location >= last) {
    ObjUpvalue* upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm.openUpvalues = upvalue->next;
  }
}

static void defineMethod(ObjString* name) {
  Value method = peek(0);
  ObjClass* klass = AS_CLASS(peek(1));
  tableSet(&klass->methods, name, method);
  pop();
}

static bool isFalsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {
  ObjString* b = AS_STRING(peek(0));
  ObjString* a = AS_STRING(peek(1));

  int length = a->length + b->length;
  char* chars = ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';

  ObjString* result = takeString(chars, length);
  pop();
  pop();
  push(OBJ_VAL(result));
}

static bool executeGetProperty(ObjString* name) {
  if (IS_MODULE(peek(0))) {
    ObjModule* module = AS_MODULE(peek(0));
    Value value;
    if (tableGet(&module->values, name, &value)) {
      pop();
      push(value);
      return true;
    }
    runtimeError("Module '%s' has no member '%s'.",
                 module->name->chars, name->chars);
    return false;
  }

  if (IS_STRING(peek(0))) {
    ObjString* string = AS_STRING(peek(0));

    if (name == vm.lengthString) {
      pop();
      push(NUMBER_VAL(string->length));
      return true;
    }

    runtimeError("String has no property '%s'.", name->chars);
    return false;
  }

  if (IS_ARRAY(peek(0))) {
    ObjArray* array = AS_ARRAY(peek(0));

    if (name == vm.lengthString) {
      pop();
      push(NUMBER_VAL(array->elements.count));
      return true;
    }

    runtimeError("Array has no property '%s'.", name->chars);
    return false;
  }

  if (IS_MAP(peek(0))) {
    ObjMap* map = AS_MAP(peek(0));

    if (name == vm.sizeString) {
      pop();
      push(NUMBER_VAL(map->entries.liveCount));
      return true;
    }

    runtimeError("Map has no property '%s'.", name->chars);
    return false;
  }

  if (!IS_INSTANCE(peek(0))) {
    runtimeError("Only instances have properties.");
    return false;
  }

  ObjInstance* instance = AS_INSTANCE(peek(0));

  Value value;
  if (tableGet(&instance->fields, name, &value)) {
    pop(); // Instance.
    push(value);
    return true;
  }

  if (!bindMethod(instance->klass, name)) {
    return false;
  }
  return true;
}

static bool executeIndexGet(void) {
  Value index = pop();
  Value receiver = pop();

  if (IS_ARRAY(receiver)) {
    if (!IS_NUMBER(index)) {
      runtimeError("Array index must be a number.");
      return false;
    }

    ObjArray* array = AS_ARRAY(receiver);
    int i = (int)AS_NUMBER(index);

    if (i < 0 || i >= array->elements.count) {
      runtimeError("Array index %d out of bounds [0, %d).",
                   i, array->elements.count);
      return false;
    }

    push(array->elements.values[i]);
  } else if (IS_MAP(receiver)) {
    ObjMap* map = AS_MAP(receiver);
    Value value;

    if (!valueTableGet(&map->entries, index, &value)) {
      runtimeError("Key not found in map.");
      return false;
    }

    push(value);
  } else {
    runtimeError("Only arrays and maps support indexing.");
    return false;
  }
  return true;
}

static bool executeIndexSet(void) {
  Value value = pop();
  Value index = pop();
  Value receiver = pop();

  if (IS_ARRAY(receiver)) {
    if (!IS_NUMBER(index)) {
      runtimeError("Array index must be a number.");
      return false;
    }

    ObjArray* array = AS_ARRAY(receiver);
    int i = (int)AS_NUMBER(index);

    if (i < 0 || i >= array->elements.count) {
      runtimeError("Array index %d out of bounds [0, %d).",
                   i, array->elements.count);
      return false;
    }

    array->elements.values[i] = value;
  } else if (IS_MAP(receiver)) {
    ObjMap* map = AS_MAP(receiver);
    valueTableSet(&map->entries, index, value);
  } else {
    runtimeError("Only arrays and maps support index assignment.");
    return false;
  }

  push(value);
  return true;
}

static InterpretResult executeImport(ObjString* moduleName,
                                     CallFrame** framePtr,
                                     int baseFrame) {
  // Check for built-in module by name.
  Value builtinCached;
  if (tableGet(&vm.importCache, moduleName, &builtinCached) &&
      IS_MODULE(builtinCached)) {
    push(builtinCached);
    return INTERPRET_OK;
  }

  // Resolve path by searching all search paths.
  char* resolvedPath = resolveModulePath(
      moduleName->chars, moduleName->length);
  if (resolvedPath == NULL) {
    runtimeError("Could not find module '%s'.",
                 moduleName->chars);
    return INTERPRET_RUNTIME_ERROR;
  }

  ObjString* pathStr = copyString(resolvedPath,
                                  (int)strlen(resolvedPath));
  push(OBJ_VAL(pathStr)); // GC protect

  // Check cache.
  Value cached;
  if (tableGet(&vm.importCache, pathStr, &cached)) {
    pop(); // pathStr
    free(resolvedPath);
    if (IS_BOOL(cached)) {
      runtimeError("Circular import detected: '%s'.",
                   moduleName->chars);
      return INTERPRET_RUNTIME_ERROR;
    }
    push(cached); // push cached ObjModule
    return INTERPRET_OK;
  }

  // Mark as loading (sentinel for circular import detection).
  tableSet(&vm.importCache, pathStr, BOOL_VAL(true));

  // Read source file.
  char* source = readFile(resolvedPath);
  if (source == NULL) {
    pop(); // pathStr
    free(resolvedPath);
    runtimeError("Could not load module '%s'.",
                 moduleName->chars);
    return INTERPRET_RUNTIME_ERROR;
  }

  // Extract base name from dotted module name.
  const char* baseName = moduleName->chars;
  int baseLen = moduleName->length;
  for (int i = moduleName->length - 1; i >= 0; i--) {
    if (moduleName->chars[i] == '.') {
      baseName = moduleName->chars + i + 1;
      baseLen = moduleName->length - i - 1;
      break;
    }
  }

  // Create module object.
  ObjString* nameStr = copyString(baseName, baseLen);
  ObjModule* module = newModule(nameStr, pathStr);
  push(OBJ_VAL(module)); // GC protect

  // Copy native functions into module globals.
  copyNatives(&module->values);

  // Compile module source.
  ObjFunction* modFunc = compile(source);
  free(source);
  if (modFunc == NULL) {
    pop(); // module
    pop(); // pathStr
    free(resolvedPath);
    runtimeError("Compilation error in module '%s'.",
                 moduleName->chars);
    return INTERPRET_RUNTIME_ERROR;
  }

  // Create closure with module's own globals.
  ObjClosure* modClosure = newClosure(modFunc);
  modClosure->globals = &module->values;
  modClosure->globalsOwner = (Obj*)module;
  free(resolvedPath);

  // Execute module.
  push(OBJ_VAL(modClosure));
  call(modClosure, 0);
  int savedFrameCount = vm.frameCount - 1;

  InterpretResult modResult = run(savedFrameCount);

  if (modResult != INTERPRET_OK) {
    return modResult;
  }

  pop(); // Discard module's return value.

  // Refresh frame pointer after recursive run().
  *framePtr = &vm.frames[vm.frameCount - 1];

  // Cache the module (replace sentinel).
  tableSet(&vm.importCache, pathStr, OBJ_VAL(module));

  // Clean up stack: pop module, pop pathStr, push module.
  pop(); // module (GC protect)
  pop(); // pathStr (GC protect)
  push(OBJ_VAL(module));
  return INTERPRET_OK;
}

static InterpretResult run(int baseFrame) {
  CallFrame* frame = &vm.frames[vm.frameCount - 1];

  #define READ_BYTE() (*frame->ip++)

  #define READ_SHORT() \
    (frame->ip += 2, \
    (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

  #define READ_CONSTANT() \
    (frame->closure->function->chunk.constants.values[READ_BYTE()])

  #define READ_STRING() AS_STRING(READ_CONSTANT())

  #define BINARY_OP(valueType, op) \
    do { \
      if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
        runtimeError("Operands must be numbers."); \
        return INTERPRET_RUNTIME_ERROR; \
      } \
      double b = AS_NUMBER(pop()); \
      double a = AS_NUMBER(pop()); \
      push(valueType(a op b)); \
    } while (false)

  for (;;) {
    #ifdef DEBUG_TRACE_EXECUTION
    printf("          ");
    for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
      printf("[ ");
      printValue(*slot);
      printf(" ]");
    }
    printf("\n");

    disassembleInstruction(&frame->closure->function->chunk,
        (int)(frame->ip - frame->closure->function->chunk.code));
    #endif

    uint8_t instruction;
    switch (instruction = READ_BYTE()) {
      case OP_CONSTANT: {
        Value constant = READ_CONSTANT();
        push(constant);
        break;
      }
      case OP_NIL: push(NIL_VAL); break;
      case OP_TRUE: push(BOOL_VAL(true)); break;
      case OP_FALSE: push(BOOL_VAL(false)); break;
      case OP_POP: pop(); break;
      case OP_GET_LOCAL: {
        uint8_t slot = READ_BYTE();
        push(frame->slots[slot]);
        break;
      }
      case OP_SET_LOCAL: {
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = peek(0);
        break;
      }
      case OP_GET_GLOBAL: {
        ObjString* name = READ_STRING();
        Value value;
        if (!tableGet(frame->closure->globals, name, &value)) {
          runtimeError("Undefined variable '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERROR;
        }
        push(value);
        break;
      }
      case OP_DEFINE_GLOBAL: {
        ObjString* name = READ_STRING();
        tableSet(frame->closure->globals, name, peek(0));
        pop();
        break;
      }
      case OP_SET_GLOBAL: {
        ObjString* name = READ_STRING();
        if (tableSet(frame->closure->globals, name, peek(0))) {
          tableDelete(frame->closure->globals, name);
          runtimeError("Undefined variable '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_GET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        push(*frame->closure->upvalues[slot]->location);
        break;
      }
      case OP_SET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        *frame->closure->upvalues[slot]->location = peek(0);
        break;
      }
      case OP_GET_PROPERTY: {
        ObjString* name = READ_STRING();
        if (!executeGetProperty(name)) return INTERPRET_RUNTIME_ERROR;
        break;
      }
      case OP_SET_PROPERTY: {
        if (!IS_INSTANCE(peek(1))) {
          runtimeError("Only instances have fields.");
          return INTERPRET_RUNTIME_ERROR;
        }

        ObjInstance* instance = AS_INSTANCE(peek(1));
        tableSet(&instance->fields, READ_STRING(), peek(0));
        Value value = pop();
        pop();
        push(value);
        break;
      }
      case OP_GET_SUPER: {
        ObjString* name = READ_STRING();
        ObjClass* superclass = AS_CLASS(pop());

        if (!bindMethod(superclass, name)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_EQUAL: {
        Value b = pop();
        Value a = pop();
        push(BOOL_VAL(valuesEqual(a, b)));
        break;
      }
      case OP_GREATER:  BINARY_OP(BOOL_VAL, >); break;
      case OP_LESS:     BINARY_OP(BOOL_VAL, <); break;
      case OP_ADD: {
        if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
          concatenate();
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_NUMBER(pop());
          double a = AS_NUMBER(pop());
          push(NUMBER_VAL(a + b));
        } else {
          runtimeError(
              "Operands must be two numbers or two strings.");
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
      case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
      case OP_DIVIDE:   BINARY_OP(NUMBER_VAL, /); break;
      case OP_MODULO: {
        if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
          runtimeError("Operands must be numbers.");
          return INTERPRET_RUNTIME_ERROR;
        }
        double b = AS_NUMBER(pop());
        double a = AS_NUMBER(pop());
        push(NUMBER_VAL(fmod(a, b)));
        break;
      }
      case OP_NOT:
        push(BOOL_VAL(isFalsey(pop())));
        break;
      case OP_NEGATE:
        if (!IS_NUMBER(peek(0))) {
          runtimeError("Operand must be a number.");
          return INTERPRET_RUNTIME_ERROR;
        }
        push(NUMBER_VAL(-AS_NUMBER(pop())));
        break;
      case OP_TYPEOF: {
        Value value = pop();
        push(OBJ_VAL(typeOfValue(value)));
        break;
      }
      case OP_PRINT: {
        printValue(pop());
        printf("\n");
        break;
      }
      case OP_JUMP: {
        uint16_t offset = READ_SHORT();
        frame->ip += offset;
        break;
      }
      case OP_JUMP_IF_FALSE: {
        uint16_t offset = READ_SHORT();
        if (isFalsey(peek(0))) frame->ip += offset;
        break;
      }
      case OP_LOOP: {
        uint16_t offset = READ_SHORT();
        frame->ip -= offset;
        break;
      }
      case OP_CALL: {
        int argCount = READ_BYTE();
        if (!callValue(peek(argCount), argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_INVOKE: {
        ObjString* method = READ_STRING();
        int argCount = READ_BYTE();
        if (!invoke(method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_CALL_SPREAD: {
        // Stack: [callee, argsArray]
        Value argsVal = pop();
        if (!IS_ARRAY(argsVal)) {
          runtimeError("Spread call requires an array.");
          return INTERPRET_RUNTIME_ERROR;
        }
        ObjArray* argsArr = AS_ARRAY(argsVal);
        int argCount = argsArr->elements.count;
        for (int i = 0; i < argCount; i++) {
          push(argsArr->elements.values[i]);
        }
        if (!callValue(peek(argCount), argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_INVOKE_SPREAD: {
        ObjString* method = READ_STRING();
        // Stack: [receiver, argsArray]
        Value argsVal = pop();
        if (!IS_ARRAY(argsVal)) {
          runtimeError("Spread invoke requires an array.");
          return INTERPRET_RUNTIME_ERROR;
        }
        ObjArray* argsArr = AS_ARRAY(argsVal);
        int argCount = argsArr->elements.count;
        for (int i = 0; i < argCount; i++) {
          push(argsArr->elements.values[i]);
        }
        if (!invoke(method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_SUPER_INVOKE: {
        ObjString* method = READ_STRING();
        int argCount = READ_BYTE();
        ObjClass* superclass = AS_CLASS(pop());
        if (!invokeFromClass(superclass, method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_BUILD_MAP: {
        uint8_t entryCount = READ_BYTE();
        ObjMap* map = newMap();
        // Push the map so GC can find it.
        push(OBJ_VAL(map));
        // Entries are on the stack as key, value, key, value, ...
        // They sit below the map we just pushed.
        for (int i = entryCount; i > 0; i--) {
          Value val = vm.stackTop[-1 - (2 * i - 1)];
          Value key = vm.stackTop[-1 - (2 * i)];
          valueTableSet(&map->entries, key, val);
        }
        // Remove the key-value pairs from under the map.
        vm.stackTop[-1 - 2 * entryCount] = OBJ_VAL(map);
        vm.stackTop -= 2 * entryCount;
        break;
      }
      case OP_BUILD_ARRAY: {
        uint8_t elementCount = READ_BYTE();
        ObjArray* array = newArray();
        // Push the array first so GC can find it, then populate.
        push(OBJ_VAL(array));
        for (int i = elementCount; i > 0; i--) {
          writeValueArray(&array->elements,
                          vm.stackTop[-1 - i]);
        }
        // Remove the elements and the temporary array from stack.
        // Move elements out from under the array.
        vm.stackTop[-1 - elementCount] = OBJ_VAL(array);
        vm.stackTop -= elementCount;
        break;
      }
      case OP_ARRAY_APPEND: {
        Value val = pop();
        ObjArray* arr = AS_ARRAY(peek(0));
        writeValueArray(&arr->elements, val);
        break;
      }
      case OP_SPREAD_ARRAY: {
        Value source = pop();
        if (!IS_ARRAY(source)) {
          runtimeError("Spread operator requires an array.");
          return INTERPRET_RUNTIME_ERROR;
        }
        ObjArray* src = AS_ARRAY(source);
        ObjArray* dest = AS_ARRAY(peek(0));
        for (int i = 0; i < src->elements.count; i++) {
          writeValueArray(&dest->elements, src->elements.values[i]);
        }
        break;
      }
      case OP_INDEX_GET: {
        if (!executeIndexGet()) return INTERPRET_RUNTIME_ERROR;
        break;
      }
      case OP_INDEX_SET: {
        if (!executeIndexSet()) return INTERPRET_RUNTIME_ERROR;
        break;
      }
      case OP_CLOSURE: {
        ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure* closure = newClosure(function);
        closure->globals = frame->closure->globals;
        closure->globalsOwner = frame->closure->globalsOwner;
        push(OBJ_VAL(closure));

        for (int i = 0; i < closure->upvalueCount; i++) {
          uint8_t isLocal = READ_BYTE();
          uint8_t index = READ_BYTE();
          if (isLocal) {
            closure->upvalues[i] =
                captureUpvalue(frame->slots + index);
          } else {
            closure->upvalues[i] = frame->closure->upvalues[index];
          }
        }

        break;
      }
      case OP_DUP: {
        uint8_t offset = READ_BYTE();
        push(peek(offset));
        break;
      }
      case OP_STRINGIFY: {
        Value value = pop();
        push(OBJ_VAL(stringify(value)));
        break;
      }
      case OP_CLOSE_UPVALUE:
        closeUpvalues(vm.stackTop - 1);
        pop();
        break;
      case OP_RETURN: {
        Value result = pop();
        closeUpvalues(frame->slots);
        vm.frameCount--;

        vm.stackTop = frame->slots;
        push(result);

        if (vm.frameCount == baseFrame) {
          return INTERPRET_OK;
        }

        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_CLASS:
        push(OBJ_VAL(newClass(READ_STRING())));
        break;
      case OP_INHERIT: {
        Value superclass = peek(1);

        if (!IS_CLASS(superclass)) {
          runtimeError("Superclass must be a class.");
          return INTERPRET_RUNTIME_ERROR;
        }

        ObjClass* subclass = AS_CLASS(peek(0));
        tableAddAll(&AS_CLASS(superclass)->methods,
                    &subclass->methods);
        pop(); // Subclass.
        break;
      }
      case OP_METHOD:
        defineMethod(READ_STRING());
        break;
      case OP_IMPORT: {
        ObjString* moduleName = READ_STRING();
        InterpretResult r = executeImport(moduleName, &frame, baseFrame);
        if (r != INTERPRET_OK) return r;
        break;
      }
      case OP_TRY: {
        uint16_t catchOffset = READ_SHORT();
        if (vm.exceptionHandlerCount >= EXCEPTION_HANDLER_MAX) {
          runtimeError("Too many nested exception handlers.");
          return INTERPRET_RUNTIME_ERROR;
        }
        ExceptionHandler* handler =
            &vm.exceptionHandlers[vm.exceptionHandlerCount++];
        handler->type = HANDLER_CATCH;
        handler->frameCount = vm.frameCount;
        handler->stackTop = vm.stackTop;
        handler->handlerIp = frame->ip + catchOffset;
        handler->frame = frame;
        break;
      }
      case OP_END_TRY: {
        vm.exceptionHandlerCount--;
        break;
      }
      case OP_THROW: {
        Value value = pop();
        if (!throwException(value)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_DEFAULT_ARG: {
        uint8_t slot = READ_BYTE();
        uint16_t jump = READ_SHORT();
        if (frame->argCount > slot) {
          frame->ip += jump;
        }
        break;
      }
      case OP_SETUP_FINALLY: {
        uint16_t finallyOffset = READ_SHORT();
        if (vm.exceptionHandlerCount >= EXCEPTION_HANDLER_MAX) {
          runtimeError("Too many nested exception handlers.");
          return INTERPRET_RUNTIME_ERROR;
        }
        ExceptionHandler* handler =
            &vm.exceptionHandlers[vm.exceptionHandlerCount++];
        handler->type = HANDLER_FINALLY;
        handler->frameCount = vm.frameCount;
        handler->stackTop = vm.stackTop;
        handler->handlerIp = frame->ip + finallyOffset;
        handler->frame = frame;
        break;
      }
      case OP_ENTER_FINALLY: {
        uint8_t completionType = READ_BYTE();
        ExceptionHandler* handler =
            &vm.exceptionHandlers[vm.exceptionHandlerCount - 1];
        vm.exceptionHandlerCount--;
        Value payload = NIL_VAL;
        if (completionType == COMPLETE_RETURN ||
            completionType == COMPLETE_BREAK ||
            completionType == COMPLETE_CONTINUE) {
          payload = pop();
          // Restore stack to handler level (discard try/catch locals).
          closeUpvalues(handler->stackTop);
          vm.stackTop = handler->stackTop;
        }
        push(NUMBER_VAL((double)completionType));
        push(payload);
        break;
      }
      case OP_END_FINALLY: {
        Value payload = pop();
        int tag = (int)AS_NUMBER(pop());
        switch (tag) {
          case COMPLETE_NORMAL:
            break;
          case COMPLETE_EXCEPTION:
            if (!throwException(payload)) {
              return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frameCount - 1];
            break;
          case COMPLETE_RETURN: {
            closeUpvalues(frame->slots);
            vm.frameCount--;
            if (vm.frameCount == baseFrame) {
              pop();
              return INTERPRET_OK;
            }
            vm.stackTop = frame->slots;
            push(payload);
            frame = &vm.frames[vm.frameCount - 1];
            break;
          }
          case COMPLETE_BREAK:
          case COMPLETE_CONTINUE:
            frame->ip = frame->closure->function->chunk.code +
                        (int)AS_NUMBER(payload);
            break;
        }
        break;
      }
      case OP_NOP:
        break;
    }
  }

  #undef READ_BYTE
  #undef READ_SHORT
  #undef READ_CONSTANT
  #undef READ_STRING
  #undef BINARY_OP
}

InterpretResult interpret(const char* source) {
  ObjFunction* function = compile(source);
  if (function == NULL) return INTERPRET_COMPILE_ERROR;

  push(OBJ_VAL(function));
  ObjClosure* closure = newClosure(function);
  pop();
  push(OBJ_VAL(closure));
  call(closure, 0);

  InterpretResult result = run(0);
  if (result == INTERPRET_OK) pop();
  return result;
}
