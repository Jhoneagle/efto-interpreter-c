#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "memory.h"
#include "value_table.h"
#include "vm.h"

VM vm;

static void runtimeError(const char* format, ...);

static Value clockNative(int argCount, Value* args) {
  return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

// --- Array native methods ---

static bool arrayPush(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  writeValueArray(&array->elements, args[0]);
  *result = NIL_VAL;
  return true;
}

static bool arrayPop(Value receiver, int argCount, Value* args,
                     Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  if (array->elements.count == 0) {
    runtimeError("Cannot pop from an empty array.");
    return false;
  }
  *result = array->elements.values[--array->elements.count];
  return true;
}

static bool arraySlice(Value receiver, int argCount, Value* args,
                       Value* result) {
  ObjArray* array = AS_ARRAY(receiver);

  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    runtimeError("Slice arguments must be numbers.");
    return false;
  }

  int start = (int)AS_NUMBER(args[0]);
  int end = (int)AS_NUMBER(args[1]);
  int length = array->elements.count;

  if (start < 0) start = 0;
  if (end > length) end = length;
  if (start > end) start = end;

  ObjArray* sliced = newArray();
  push(OBJ_VAL(sliced)); // GC protection
  for (int i = start; i < end; i++) {
    writeValueArray(&sliced->elements, array->elements.values[i]);
  }
  pop(); // remove GC protection

  *result = OBJ_VAL(sliced);
  return true;
}

// --- Map native methods ---

static bool mapHas(Value receiver, int argCount, Value* args,
                   Value* result) {
  ObjMap* map = AS_MAP(receiver);
  Value dummy;
  *result = BOOL_VAL(valueTableGet(&map->entries, args[0], &dummy));
  return true;
}

static bool mapKeys(Value receiver, int argCount, Value* args,
                    Value* result) {
  ObjMap* map = AS_MAP(receiver);
  ObjArray* keys = newArray();
  push(OBJ_VAL(keys)); // GC protection
  for (int i = 0; i < map->entries.capacity; i++) {
    ValueEntry* entry = &map->entries.entries[i];
    if (entry->occupied) {
      writeValueArray(&keys->elements, entry->key);
    }
  }
  pop();
  *result = OBJ_VAL(keys);
  return true;
}

static bool mapValues(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjMap* map = AS_MAP(receiver);
  ObjArray* values = newArray();
  push(OBJ_VAL(values)); // GC protection
  for (int i = 0; i < map->entries.capacity; i++) {
    ValueEntry* entry = &map->entries.entries[i];
    if (entry->occupied) {
      writeValueArray(&values->elements, entry->value);
    }
  }
  pop();
  *result = OBJ_VAL(values);
  return true;
}

static bool mapRemove(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjMap* map = AS_MAP(receiver);
  *result = BOOL_VAL(valueTableDelete(&map->entries, args[0]));
  return true;
}

// --- String native methods ---

static bool stringSubstring(Value receiver, int argCount, Value* args,
                            Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    runtimeError("Substring arguments must be numbers.");
    return false;
  }
  int start = (int)AS_NUMBER(args[0]);
  int end = (int)AS_NUMBER(args[1]);
  if (start < 0) start = 0;
  if (end > str->length) end = str->length;
  if (start > end) start = end;
  *result = OBJ_VAL(copyString(str->chars + start, end - start));
  return true;
}

static bool stringIndexOf(Value receiver, int argCount, Value* args,
                           Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_STRING(args[0])) {
    runtimeError("indexOf argument must be a string.");
    return false;
  }
  ObjString* search = AS_STRING(args[0]);
  char* found = strstr(str->chars, search->chars);
  *result = NUMBER_VAL(found == NULL ? -1 : (double)(found - str->chars));
  return true;
}

static bool stringToUpper(Value receiver, int argCount, Value* args,
                           Value* result) {
  ObjString* str = AS_STRING(receiver);
  char* upper = ALLOCATE(char, str->length + 1);
  for (int i = 0; i < str->length; i++) {
    upper[i] = (char)toupper((unsigned char)str->chars[i]);
  }
  upper[str->length] = '\0';
  *result = OBJ_VAL(takeString(upper, str->length));
  return true;
}

static bool stringToLower(Value receiver, int argCount, Value* args,
                           Value* result) {
  ObjString* str = AS_STRING(receiver);
  char* lower = ALLOCATE(char, str->length + 1);
  for (int i = 0; i < str->length; i++) {
    lower[i] = (char)tolower((unsigned char)str->chars[i]);
  }
  lower[str->length] = '\0';
  *result = OBJ_VAL(takeString(lower, str->length));
  return true;
}

static bool stringSplit(Value receiver, int argCount, Value* args,
                        Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_STRING(args[0])) {
    runtimeError("Split delimiter must be a string.");
    return false;
  }
  ObjString* delim = AS_STRING(args[0]);

  ObjArray* array = newArray();
  push(OBJ_VAL(array)); // GC protection

  if (delim->length == 0) {
    for (int i = 0; i < str->length; i++) {
      writeValueArray(&array->elements,
                      OBJ_VAL(copyString(str->chars + i, 1)));
    }
  } else {
    const char* start = str->chars;
    const char* end = str->chars + str->length;
    const char* pos;
    while ((pos = strstr(start, delim->chars)) != NULL) {
      writeValueArray(&array->elements,
                      OBJ_VAL(copyString(start, (int)(pos - start))));
      start = pos + delim->length;
    }
    writeValueArray(&array->elements,
                    OBJ_VAL(copyString(start, (int)(end - start))));
  }

  pop(); // remove GC protection
  *result = OBJ_VAL(array);
  return true;
}

static bool stringTrim(Value receiver, int argCount, Value* args,
                       Value* result) {
  ObjString* str = AS_STRING(receiver);
  const char* start = str->chars;
  const char* end = str->chars + str->length;

  while (start < end && (*start == ' ' || *start == '\t' ||
                          *start == '\r' || *start == '\n')) start++;
  while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n')) end--;

  *result = OBJ_VAL(copyString(start, (int)(end - start)));
  return true;
}

// --- Native functions ---

static Value typeNative(int argCount, Value* args) {
  if (argCount == 0) return OBJ_VAL(copyString("nil", 3));
  Value value = args[0];

  if (IS_NIL(value))    return OBJ_VAL(copyString("nil", 3));
  if (IS_BOOL(value))   return OBJ_VAL(copyString("bool", 4));
  if (IS_NUMBER(value)) return OBJ_VAL(copyString("number", 6));

  if (IS_OBJ(value)) {
    switch (OBJ_TYPE(value)) {
      case OBJ_STRING:        return OBJ_VAL(copyString("string", 6));
      case OBJ_ARRAY:         return OBJ_VAL(copyString("array", 5));
      case OBJ_MAP:           return OBJ_VAL(copyString("map", 3));
      case OBJ_CLASS:         return OBJ_VAL(copyString("class", 5));
      case OBJ_INSTANCE:      return OBJ_VAL(copyString("instance", 8));
      case OBJ_FUNCTION:
      case OBJ_CLOSURE:
      case OBJ_NATIVE:
      case OBJ_NATIVE_METHOD:
      case OBJ_BOUND_METHOD:  return OBJ_VAL(copyString("function", 8));
      default: break;
    }
  }
  return OBJ_VAL(copyString("unknown", 7));
}

static void resetStack() {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
}

static void runtimeError(const char* format, ...) {
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

static void defineNative(const char* name, NativeFn function) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNative(function)));
  tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
  pop();
  pop();
}

static void defineNativeMethod(ObjClass* klass, const char* name,
                               NativeMethodFn function, int arity) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNativeMethod(function, arity)));
  tableSet(&klass->methods, AS_STRING(vm.stack[0]), vm.stack[1]);
  pop();
  pop();
}

static ObjString* stringify(Value value) {
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

    vm.initString = NULL;
    vm.initString = copyString("init", 4);

    vm.arrayMethods = NULL;
    vm.arrayMethods = newClass(copyString("Array", 5));
    defineNativeMethod(vm.arrayMethods, "push", arrayPush, 1);
    defineNativeMethod(vm.arrayMethods, "pop", arrayPop, 0);
    defineNativeMethod(vm.arrayMethods, "slice", arraySlice, 2);

    vm.mapMethods = NULL;
    vm.mapMethods = newClass(copyString("Map", 3));
    defineNativeMethod(vm.mapMethods, "has", mapHas, 1);
    defineNativeMethod(vm.mapMethods, "keys", mapKeys, 0);
    defineNativeMethod(vm.mapMethods, "values", mapValues, 0);
    defineNativeMethod(vm.mapMethods, "remove", mapRemove, 1);

    vm.stringMethods = NULL;
    vm.stringMethods = newClass(copyString("String", 6));
    defineNativeMethod(vm.stringMethods, "substring", stringSubstring, 2);
    defineNativeMethod(vm.stringMethods, "indexOf", stringIndexOf, 1);
    defineNativeMethod(vm.stringMethods, "toUpper", stringToUpper, 0);
    defineNativeMethod(vm.stringMethods, "toLower", stringToLower, 0);
    defineNativeMethod(vm.stringMethods, "split", stringSplit, 1);
    defineNativeMethod(vm.stringMethods, "trim", stringTrim, 0);

    defineNative("clock", clockNative);
    defineNative("type", typeNative);
}

void freeVM() {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  vm.initString = NULL;
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
  if (argCount != closure->function->arity) {
    runtimeError("Expected %d arguments but got %d.",
        closure->function->arity, argCount);
    return false;
  }

  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  CallFrame* frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = closure->function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;
  return true;
}

static bool callValue(Value callee, int argCount) {
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

static bool invokeFromClass(ObjClass* klass, ObjString* name, int argCount) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }

  if (IS_NATIVE_METHOD(method)) {
    ObjNativeMethod* native = AS_NATIVE_METHOD(method);
    if (argCount != native->arity) {
      runtimeError("Expected %d arguments but got %d.",
                   native->arity, argCount);
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

  if (IS_STRING(receiver)) {
    return invokeFromClass(vm.stringMethods, name, argCount);
  }

  if (IS_ARRAY(receiver)) {
    return invokeFromClass(vm.arrayMethods, name, argCount);
  }

  if (IS_MAP(receiver)) {
    return invokeFromClass(vm.mapMethods, name, argCount);
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

static InterpretResult run() {
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
        if (!tableGet(&vm.globals, name, &value)) {
          runtimeError("Undefined variable '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERROR;
        }
        push(value);
        break;
      }
      case OP_DEFINE_GLOBAL: {
        ObjString* name = READ_STRING();
        tableSet(&vm.globals, name, peek(0));
        pop();
        break;
      }
      case OP_SET_GLOBAL: {
        ObjString* name = READ_STRING();
        if (tableSet(&vm.globals, name, peek(0))) {
          tableDelete(&vm.globals, name); 
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
        if (IS_STRING(peek(0))) {
          ObjString* string = AS_STRING(peek(0));
          ObjString* name = READ_STRING();

          if (name->length == 6 &&
              memcmp(name->chars, "length", 6) == 0) {
            pop();
            push(NUMBER_VAL(string->length));
            break;
          }

          runtimeError("String has no property '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERROR;
        }

        if (IS_ARRAY(peek(0))) {
          ObjArray* array = AS_ARRAY(peek(0));
          ObjString* name = READ_STRING();

          if (name->length == 6 &&
              memcmp(name->chars, "length", 6) == 0) {
            pop();
            push(NUMBER_VAL(array->elements.count));
            break;
          }

          runtimeError("Array has no property '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERROR;
        }

        if (IS_MAP(peek(0))) {
          ObjMap* map = AS_MAP(peek(0));
          ObjString* name = READ_STRING();

          if (name->length == 4 &&
              memcmp(name->chars, "size", 4) == 0) {
            pop();
            push(NUMBER_VAL(map->entries.liveCount));
            break;
          }

          runtimeError("Map has no property '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERROR;
        }

        if (!IS_INSTANCE(peek(0))) {
          runtimeError("Only instances have properties.");
          return INTERPRET_RUNTIME_ERROR;
        }

        ObjInstance* instance = AS_INSTANCE(peek(0));
        ObjString* name = READ_STRING();

        Value value;
        if (tableGet(&instance->fields, name, &value)) {
          pop(); // Instance.
          push(value);
          break;
        }

        if (!bindMethod(instance->klass, name)) {
          return INTERPRET_RUNTIME_ERROR;
        }
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
      case OP_INDEX_GET: {
        Value index = pop();
        Value receiver = pop();

        if (IS_ARRAY(receiver)) {
          if (!IS_NUMBER(index)) {
            runtimeError("Array index must be a number.");
            return INTERPRET_RUNTIME_ERROR;
          }

          ObjArray* array = AS_ARRAY(receiver);
          int i = (int)AS_NUMBER(index);

          if (i < 0 || i >= array->elements.count) {
            runtimeError("Array index %d out of bounds [0, %d).",
                         i, array->elements.count);
            return INTERPRET_RUNTIME_ERROR;
          }

          push(array->elements.values[i]);
        } else if (IS_MAP(receiver)) {
          ObjMap* map = AS_MAP(receiver);
          Value value;

          if (!valueTableGet(&map->entries, index, &value)) {
            runtimeError("Key not found in map.");
            return INTERPRET_RUNTIME_ERROR;
          }

          push(value);
        } else {
          runtimeError("Only arrays and maps support indexing.");
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_INDEX_SET: {
        Value value = pop();
        Value index = pop();
        Value receiver = pop();

        if (IS_ARRAY(receiver)) {
          if (!IS_NUMBER(index)) {
            runtimeError("Array index must be a number.");
            return INTERPRET_RUNTIME_ERROR;
          }

          ObjArray* array = AS_ARRAY(receiver);
          int i = (int)AS_NUMBER(index);

          if (i < 0 || i >= array->elements.count) {
            runtimeError("Array index %d out of bounds [0, %d).",
                         i, array->elements.count);
            return INTERPRET_RUNTIME_ERROR;
          }

          array->elements.values[i] = value;
        } else if (IS_MAP(receiver)) {
          ObjMap* map = AS_MAP(receiver);
          valueTableSet(&map->entries, index, value);
        } else {
          runtimeError("Only arrays and maps support index assignment.");
          return INTERPRET_RUNTIME_ERROR;
        }

        push(value);
        break;
      }
      case OP_CLOSURE: {
        ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure* closure = newClosure(function);
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
        if (vm.frameCount == 0) {
          pop();
          return INTERPRET_OK;
        }

        vm.stackTop = frame->slots;
        push(result);
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

  return run();
}