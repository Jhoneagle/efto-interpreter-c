#include <inttypes.h>
#include <limits.h>
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

// Overflow-checked int64 arithmetic. Returns true on overflow.
static inline bool int64AddOverflow(int64_t a, int64_t b, int64_t* r) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_add_overflow(a, b, r);
#else
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
    return true;
  *r = a + b; return false;
#endif
}

static inline bool int64SubOverflow(int64_t a, int64_t b, int64_t* r) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_sub_overflow(a, b, r);
#else
  if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
    return true;
  *r = a - b; return false;
#endif
}

static inline bool int64MulOverflow(int64_t a, int64_t b, int64_t* r) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_mul_overflow(a, b, r);
#else
  if (a > 0) {
    if (b > 0) { if (a > INT64_MAX / b) return true; }
    else       { if (b < INT64_MIN / a) return true; }
  } else {
    if (b > 0) { if (a < INT64_MIN / b) return true; }
    else       { if (a != 0 && b < INT64_MAX / a) return true; }
  }
  *r = a * b; return false;
#endif
}

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
  vm.nativeError = false;
  vm.nativeErrorValue = NIL_VAL;
  vm.pendingUnwind = false;
}

// Raise a catchable Error instance from a native function. Sets the
// vm.nativeError flag; the native must return promptly (false for method
// natives, any value for global natives) so the call site can convert this
// into a real exception via throwException. Mirrors errorInit's field setup.
bool raiseError(ObjClass* klass, const char* format, ...) {
  if (klass == NULL) klass = vm.errorClass;

  char buffer[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  ObjInstance* instance = newInstance(klass);
  push(OBJ_VAL(instance)); // GC protect while building message
  ObjString* msg = copyString(buffer, (int)strlen(buffer));
  push(OBJ_VAL(msg));
  ObjString* msgKey = copyString("message", 7);
  push(OBJ_VAL(msgKey));
  tableSet(&instance->fields, msgKey, OBJ_VAL(msg));
  pop(); // msgKey
  pop(); // msg
  pop(); // instance

  vm.nativeError = true;
  vm.nativeErrorValue = OBJ_VAL(instance);
  return false;
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

// Begin throwing an exception. This does NOT unwind the stack immediately:
// unwinding is deferred so that intermediate native frames (e.g. a forEach
// running a callback via a nested run()) can return through the C stack and
// perform their own cleanup before we reset stackTop/frameCount. The run()
// loop drives the actual landing via tryLandUnwind(), one run level at a time.
//
// Returns false only when there is no handler at all (uncaught): in that case
// it prints the error and resets the stack. Otherwise it sets vm.pendingUnwind
// and returns true; the caller must then attempt to land (tryLandUnwind).
// True if `value` is an instance of Error or one of its subclasses.
static bool isErrorInstance(Value value) {
  if (!IS_INSTANCE(value)) return false;
  ObjClass* klass = AS_INSTANCE(value)->klass;
  while (klass != NULL) {
    if (klass == vm.errorClass) return true;
    klass = klass->superclass;
  }
  return false;
}

// Stamp `.line` (source line of the throw site) and `.stack` (a text traceback)
// onto an Error instance as it is thrown. No-op for non-Error values, and it
// never overwrites an already-stamped error, so a rethrow preserves the
// original throw site. Frames must still be intact (called before unwinding).
static void annotateError(Value value) {
  if (!isErrorInstance(value)) return;
  ObjInstance* inst = AS_INSTANCE(value);

  push(value); // GC-protect the instance across the allocations below
  ObjString* lineKey = copyString("line", 4);
  push(OBJ_VAL(lineKey));

  Value existing;
  if (tableGet(&inst->fields, lineKey, &existing)) {
    pop(); // lineKey
    pop(); // value
    return; // already stamped — preserve the original site (rethrow)
  }

  // Line of the topmost (throwing) frame; build the traceback top-to-bottom.
  int line = 0;
  char buffer[1024];
  int off = 0;
  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame* f = &vm.frames[i];
    ObjFunction* fn = f->closure->function;
    size_t idx = (f->ip > fn->chunk.code)
                     ? (size_t)(f->ip - fn->chunk.code - 1) : 0;
    int ln = fn->chunk.lines[idx];
    if (i == vm.frameCount - 1) line = ln;
    const char* name = fn->name != NULL ? fn->name->chars : "script";
    int n = snprintf(buffer + off, sizeof(buffer) - off,
                     "[line %d] in %s\n", ln, name);
    if (n < 0 || off + n >= (int)sizeof(buffer)) break;
    off += n;
  }

  ObjString* stackStr = copyString(buffer, off);
  push(OBJ_VAL(stackStr));
  tableSet(&inst->fields, lineKey, INT_VAL((int64_t)line));
  ObjString* stackKey = copyString("stack", 5);
  push(OBJ_VAL(stackKey));
  tableSet(&inst->fields, stackKey, OBJ_VAL(stackStr));

  pop(); // stackKey
  pop(); // stackStr
  pop(); // lineKey
  pop(); // value
}

static bool throwException(Value value) {
  annotateError(value);

  if (vm.exceptionHandlerCount == 0) {
    // No handler anywhere — print as runtime error (frames still intact).
    if (IS_STRING(value)) {
      runtimeError("%s", AS_STRING(value)->chars);
    } else {
      ObjString* str = stringify(value);
      runtimeError("%s", str->chars);
    }
    return false;
  }

  vm.currentException = value;
  vm.pendingUnwind = true;
  return true;
}

// While an unwind is pending, land on the topmost handler if it is owned by the
// run() whose base is baseFrame (i.e. installed above baseFrame). If the
// topmost handler belongs to an outer run, return INTERPRET_UNWIND so this
// run() aborts and the outer run() lands instead. On landing, restores the
// stack/frame, pushes the exception (catch) or completion (finally), points the
// handler frame's ip at the handler body, and updates *framePtr.
static InterpretResult tryLandUnwind(CallFrame** framePtr, int baseFrame) {
  ExceptionHandler* handler =
      &vm.exceptionHandlers[vm.exceptionHandlerCount - 1];

  if (handler->frameCount <= baseFrame) {
    return INTERPRET_UNWIND; // handler owned by an outer run — keep unwinding
  }

  vm.exceptionHandlerCount--;
  vm.frameCount = handler->frameCount;
  closeUpvalues(handler->stackTop);
  vm.stackTop = handler->stackTop;

  Value value = vm.currentException;
  if (handler->type == HANDLER_CATCH) {
    push(value);
  } else {
    // HANDLER_FINALLY: push completion state (exception tag + value).
    push(INT_VAL(COMPLETE_EXCEPTION));
    push(value);
  }

  handler->frame->ip = handler->handlerIp;
  vm.pendingUnwind = false;
  *framePtr = &vm.frames[vm.frameCount - 1];
  return INTERPRET_OK;
}

// Handle a false return from a call/invoke/magic dispatch at a run() call site.
// Converts a pending native error into a throw, then either lands on a handler
// owned by this run (returns INTERPRET_OK — caller reloads frame and resumes),
// propagates the unwind (INTERPRET_UNWIND), or reports a genuine runtime error
// (INTERPRET_RUNTIME_ERROR, already printed by runtimeError).
static InterpretResult postCallFailure(CallFrame** framePtr, int baseFrame) {
  if (vm.nativeError) {
    vm.nativeError = false;
    if (!throwException(vm.nativeErrorValue)) return INTERPRET_RUNTIME_ERROR;
  }
  if (!vm.pendingUnwind) {
    return INTERPRET_RUNTIME_ERROR; // genuine error; stack already reset
  }
  return tryLandUnwind(framePtr, baseFrame);
}

ObjString* stringify(Value value) {
  if (IS_STRING(value)) return AS_STRING(value);

  if (IS_NIL(value)) return copyString("nil", 3);
  if (IS_BOOL(value)) return AS_BOOL(value) ?
      copyString("true", 4) : copyString("false", 5);

  if (IS_INT(value)) {
    char buffer[24];
    int len = snprintf(buffer, sizeof(buffer), "%" PRId64, AS_INT(value));
    return copyString(buffer, len);
  }

  if (IS_DOUBLE(value)) {
    char buffer[24];
    int len = snprintf(buffer, sizeof(buffer), "%g", AS_DOUBLE(value));
    return copyString(buffer, len);
  }

  if (IS_OBJ(value)) {
    switch (OBJ_TYPE(value)) {
      case OBJ_CLASS:
        return AS_CLASS(value)->name;
      case OBJ_BYTES: {
        char buffer[32];
        int len = snprintf(buffer, sizeof(buffer), "bytes(len=%d)",
                           AS_BYTES(value)->length);
        return copyString(buffer, len);
      }
      case OBJ_FILE:
        return AS_FILE(value)->path;
      case OBJ_MODULE:
        return AS_MODULE(value)->name;
      case OBJ_TYPE_DESCRIPTOR: {
        ObjTypeDescriptor* desc = AS_TYPE_DESCRIPTOR(value);
        int totalLen = desc->name->length + 7;
        char* chars = ALLOCATE(char, totalLen + 1);
        memcpy(chars, "<type ", 6);
        memcpy(chars + 6, desc->name->chars, desc->name->length);
        chars[totalLen - 1] = '>';
        chars[totalLen] = '\0';
        return takeString(chars, totalLen);
      }
      case OBJ_SET: {
        ObjSet* set = AS_SET(value);
        // Build "Set{elem1, elem2, ...}" string.
        push(value); // GC protect
        // First pass: stringify elements and compute length.
        int count = 0;
        int totalLen = 4; // "Set{" + "}"
        ObjString** parts = NULL;
        if (set->entries.liveCount > 0) {
          parts = (ObjString**)malloc(sizeof(ObjString*) *
                                      set->entries.liveCount);
          for (int i = 0; i < set->entries.capacity; i++) {
            ValueEntry* entry = &set->entries.entries[i];
            if (!entry->occupied) continue;
            parts[count] = stringify(entry->key);
            push(OBJ_VAL(parts[count])); // GC protect
            totalLen += parts[count]->length;
            if (count > 0) totalLen += 2; // ", "
            count++;
          }
        }
        char* chars = ALLOCATE(char, totalLen + 1);
        memcpy(chars, "Set{", 4);
        int pos = 4;
        for (int i = 0; i < count; i++) {
          if (i > 0) { chars[pos++] = ','; chars[pos++] = ' '; }
          memcpy(chars + pos, parts[i]->chars, parts[i]->length);
          pos += parts[i]->length;
        }
        chars[pos++] = '}';
        chars[pos] = '\0';
        // Pop GC protections: count parts + value.
        for (int i = 0; i < count + 1; i++) pop();
        if (parts) free(parts);
        return takeString(chars, totalLen);
      }
      case OBJ_INSTANCE: {
        Value toStrMethod;
        if (tableGet(&AS_INSTANCE(value)->klass->methods,
                     vm.magicToString, &toStrMethod)) {
          if (IS_CLOSURE(toStrMethod) &&
              AS_CLOSURE(toStrMethod)->function->arity == 0) {
            Value strResult;
            if (callMagicUnary(value, vm.magicToString, &strResult)) {
              if (IS_STRING(strResult)) return AS_STRING(strResult);
            }
          } else if (IS_NATIVE_METHOD(toStrMethod)) {
            Value strResult;
            if (AS_NATIVE_METHOD(toStrMethod)->function(
                    value, 0, NULL, &strResult) &&
                IS_STRING(strResult)) {
              return AS_STRING(strResult);
            }
          }
        }
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
    // Inhibit collection until every builtin root is wired up (see VM.gcInhibit).
    vm.gcInhibit = true;
    resetStack();
    vm.objects = NULL;

    vm.bytesAllocated = 0;
    vm.nextGC = 1024 * 1024;

    vm.grayCount = 0;
    vm.grayCapacity = 0;
    vm.grayStack = NULL;

    vm.argc = 0;
    vm.argv = NULL;

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

    vm.magicAdd = NULL;
    vm.magicAdd = copyString("__add__", 7);
    vm.magicSub = NULL;
    vm.magicSub = copyString("__sub__", 7);
    vm.magicMul = NULL;
    vm.magicMul = copyString("__mul__", 7);
    vm.magicDiv = NULL;
    vm.magicDiv = copyString("__div__", 7);
    vm.magicMod = NULL;
    vm.magicMod = copyString("__mod__", 7);
    vm.magicNeg = NULL;
    vm.magicNeg = copyString("__neg__", 7);
    vm.magicEq = NULL;
    vm.magicEq = copyString("__eq__", 6);
    vm.magicCmp = NULL;
    vm.magicCmp = copyString("__cmp__", 7);
    vm.magicToString = NULL;
    vm.magicToString = copyString("__toString__", 12);
    vm.magicToNumber = NULL;
    vm.magicToNumber = copyString("__toNumber__", 12);
    vm.magicToBool = NULL;
    vm.magicToBool = copyString("__toBool__", 10);
    vm.magicIter = NULL;
    vm.magicIter = copyString("__iter__", 8);
    vm.magicNext = NULL;
    vm.magicNext = copyString("__next__", 8);

    registerBuiltins();

    // Bootstrap complete: every builtin is now reachable from a root. Allow GC.
    vm.gcInhibit = false;
}

void freeVM() {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  freeTable(&vm.importCache);
  vm.initString = NULL;
  vm.lengthString = NULL;
  vm.sizeString = NULL;
  vm.magicAdd = NULL;
  vm.magicSub = NULL;
  vm.magicMul = NULL;
  vm.magicDiv = NULL;
  vm.magicMod = NULL;
  vm.magicNeg = NULL;
  vm.magicEq = NULL;
  vm.magicCmp = NULL;
  vm.magicToString = NULL;
  vm.magicToNumber = NULL;
  vm.magicToBool = NULL;
  vm.magicIter = NULL;
  vm.magicNext = NULL;
  vm.iteratorMethods = NULL;
  vm.regexMethods = NULL;
  vm.stopIterationClass = NULL;
  freeObjects();
}

void push(Value value) {
  if (vm.stackTop >= vm.stack + STACK_MAX) {
    fprintf(stderr, "Stack overflow.\n");
    exit(70);
  }
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

  // Pad missing optional args with the MISSING sentinel; each such slot has a
  // matching OP_DEFAULT_ARG that fills it before any user code runs.
  for (int i = argCount; i < arity; i++) {
    push(MISSING_VAL);
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
          if (IS_NATIVE_METHOD(initializer)) {
            ObjNativeMethod* native = AS_NATIVE_METHOD(initializer);
            if (argCount < native->minArity ||
                argCount > native->arity) {
              runtimeError("Expected %d arguments but got %d.",
                           native->arity, argCount);
              return false;
            }
            Value result;
            Value receiver = vm.stackTop[-argCount - 1];
            if (!native->function(receiver, argCount,
                                  vm.stackTop - argCount, &result)) {
              if (vm.nativeError) {
                // Discard args + instance; the run() call site throws.
                vm.stackTop -= argCount + 1;
              }
              return false;
            }
            vm.stackTop -= argCount; // pop args, leave instance
            return true;
          }
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
        vm.stackTop -= argCount + 1; // discard args + callee
        if (vm.nativeError) {
          // Leave vm.nativeError set; the run() call site turns it into a
          // catchable throw (with the correct run baseFrame for unwinding).
          return false;
        }
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

// Return the ObjFunction underlying a keyword-callable value, or NULL if the
// callee cannot accept keyword arguments (native, or a class without a
// user-defined init).
static ObjFunction* keywordCalleeFunction(Value callee) {
  if (IS_CLOSURE(callee)) return AS_CLOSURE(callee)->function;
  if (IS_BOUND_METHOD(callee)) {
    return AS_BOUND_METHOD(callee)->method->function;
  }
  if (IS_CLASS(callee)) {
    Value init;
    if (tableGet(&AS_CLASS(callee)->methods, vm.initString, &init) &&
        IS_CLOSURE(init)) {
      return AS_CLOSURE(init)->function;
    }
  }
  return NULL;
}

// Handle OP_CALL_KW. The stack holds, above the callee:
//   pos_0 .. pos_{P-1}, name_0, val_0, .., name_{K-1}, val_{K-1}
// We reorder these into a positional frame (arity slots, MISSING where
// unsupplied), replace the argument region with it, then dispatch through the
// normal callValue path (which handles receiver/instance placement). Errors are
// raised catchably via raiseError. Returns false on any failure.
static bool callKeyword(int posCount, int namedCount) {
  int total = posCount + 2 * namedCount;
  Value callee = peek(total);

  ObjFunction* fn = keywordCalleeFunction(callee);
  if (fn == NULL) {
    raiseError(vm.typeErrorClass,
               "Keyword arguments require a function with named parameters.");
    return false;
  }
  if (fn->hasRest) {
    raiseError(vm.typeErrorClass,
               "Cannot use keyword arguments with a rest parameter.");
    return false;
  }

  int arity = fn->arity;
  Value* base = vm.stackTop - total; // points at pos_0

  if (posCount > arity) {
    raiseError(vm.typeErrorClass,
               "Expected at most %d arguments but got %d positional.", arity,
               posCount);
    return false;
  }

  Value ordered[256];
  for (int i = 0; i < arity; i++) ordered[i] = MISSING_VAL;
  for (int i = 0; i < posCount; i++) ordered[i] = base[i];

  for (int j = 0; j < namedCount; j++) {
    ObjString* name = AS_STRING(base[posCount + 2 * j]);
    Value value = base[posCount + 2 * j + 1];
    int idx = -1;
    for (int p = 0; p < fn->paramCount; p++) {
      if (fn->paramNames[p] == name) { idx = p; break; } // interned: ptr eq
    }
    if (idx < 0) {
      raiseError(vm.typeErrorClass, "Unknown keyword argument '%s'.",
                 name->chars);
      return false;
    }
    if (!IS_MISSING(ordered[idx])) {
      raiseError(vm.typeErrorClass, "Duplicate argument '%s'.", name->chars);
      return false;
    }
    ordered[idx] = value;
  }

  // Every required parameter (those before the first default) must be filled.
  for (int i = 0; i < fn->minArity; i++) {
    if (IS_MISSING(ordered[i])) {
      const char* nm = (i < fn->paramCount && fn->paramNames[i] != NULL)
                           ? fn->paramNames[i]->chars
                           : "(positional)";
      raiseError(vm.typeErrorClass, "Missing required argument '%s'.", nm);
      return false;
    }
  }

  // Replace [pos + named pairs] with the ordered positional slots, then let the
  // normal dispatch path bind them. MISSING slots are filled by OP_DEFAULT_ARG.
  vm.stackTop -= total;
  for (int i = 0; i < arity; i++) push(ordered[i]);
  return callValue(peek(arity), arity);
}

bool vmCallValue(Value callee, int argCount, Value* result) {
  // Stack level below the callee + its arguments — where a normal call leaves
  // the stack after its result is popped.
  Value* savedTop = vm.stackTop - argCount - 1;
  int framesBefore = vm.frameCount;
  if (!callValue(callee, argCount)) return false;
  if (vm.frameCount > framesBefore) {
    InterpretResult r = run(framesBefore);
    if (r == INTERPRET_UNWIND) {
      // An exception is unwinding toward a handler owned by an outer run().
      // Reset to the pre-call state so the native that invoked us sees a clean
      // stack for its own cleanup; the owning run() then lands on the handler
      // (which restores its own saved stackTop). Without this the abandoned
      // callback frame + slots would corrupt the native's stack bookkeeping.
      closeUpvalues(savedTop);
      vm.stackTop = savedTop;
      vm.frameCount = framesBefore;
      return false;
    }
    if (r != INTERPRET_OK) return false; // hard error: stack already reset
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

// --- Magic method dispatch helpers (zero-allocation) ---

static bool lookupMagicMethod(Value value, ObjString* methodName,
                              Value* method) {
  if (!IS_INSTANCE(value)) return false;
  return tableGet(&AS_INSTANCE(value)->klass->methods,
                  methodName, method);
}

bool callMagicBinary(Value left, ObjString* name,
                     Value right, Value* result) {
  Value method;
  if (!lookupMagicMethod(left, name, &method)) return false;
  Value* savedTop = vm.stackTop;
  push(left);
  push(right);
  int framesBefore = vm.frameCount;
  if (!call(AS_CLOSURE(method), 1)) {
    // call() failed and called runtimeError() which reset the stack.
    // Don't pop — the pushed values are already gone.
    return false;
  }
  if (vm.frameCount > framesBefore) {
    InterpretResult r = run(framesBefore);
    if (r == INTERPRET_UNWIND) {
      // Unwind propagating past this magic call: restore pre-call state so the
      // magic-op call site (and the handler landing) sees a consistent stack.
      closeUpvalues(savedTop);
      vm.stackTop = savedTop;
      vm.frameCount = framesBefore;
      return false;
    }
    if (r != INTERPRET_OK) return false; // hard error: stack already reset
  }
  *result = pop();
  return true;
}

bool callMagicUnary(Value operand, ObjString* name, Value* result) {
  Value method;
  if (!lookupMagicMethod(operand, name, &method)) return false;
  Value* savedTop = vm.stackTop;
  push(operand);
  int framesBefore = vm.frameCount;
  if (!call(AS_CLOSURE(method), 0)) {
    // call() failed and called runtimeError() which reset the stack.
    // Don't pop — the pushed value is already gone.
    return false;
  }
  if (vm.frameCount > framesBefore) {
    InterpretResult r = run(framesBefore);
    if (r == INTERPRET_UNWIND) {
      closeUpvalues(savedTop);
      vm.stackTop = savedTop;
      vm.frameCount = framesBefore;
      return false;
    }
    if (r != INTERPRET_OK) return false; // hard error: stack already reset
  }
  *result = pop();
  return true;
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
      if (vm.nativeError) {
        // Discard args + receiver; the run() call site throws.
        vm.stackTop -= argCount + 1;
      }
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

  if (IS_BYTES(receiver)) {
    return invokeFromClass(vm.bytesMethods, name, argCount);
  }

  if (IS_MAP(receiver)) {
    return invokeFromClass(vm.mapMethods, name, argCount);
  }

  if (IS_SET(receiver)) {
    return invokeFromClass(vm.setMethods, name, argCount);
  }

  if (IS_FILE(receiver)) {
    return invokeFromClass(vm.fileMethods, name, argCount);
  }

  if (IS_ITERATOR(receiver)) {
    return invokeFromClass(vm.iteratorMethods, name, argCount);
  }

  if (IS_REGEX(receiver)) {
    return invokeFromClass(vm.regexMethods, name, argCount);
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

bool isFalsey(Value value) {
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
      push(INT_VAL((int64_t)string->length));
      return true;
    }

    runtimeError("String has no property '%s'.", name->chars);
    return false;
  }

  if (IS_ARRAY(peek(0))) {
    ObjArray* array = AS_ARRAY(peek(0));

    if (name == vm.lengthString) {
      pop();
      push(INT_VAL((int64_t)array->elements.count));
      return true;
    }

    runtimeError("Array has no property '%s'.", name->chars);
    return false;
  }

  if (IS_BYTES(peek(0))) {
    ObjBytes* bytes = AS_BYTES(peek(0));

    if (name == vm.lengthString) {
      pop();
      push(INT_VAL((int64_t)bytes->length));
      return true;
    }

    runtimeError("Bytes has no property '%s'.", name->chars);
    return false;
  }

  if (IS_MAP(peek(0))) {
    ObjMap* map = AS_MAP(peek(0));

    if (name == vm.sizeString) {
      pop();
      push(INT_VAL((int64_t)map->entries.liveCount));
      return true;
    }

    runtimeError("Map has no property '%s'.", name->chars);
    return false;
  }

  if (IS_SET(peek(0))) {
    ObjSet* set = AS_SET(peek(0));

    if (name == vm.sizeString) {
      pop();
      push(INT_VAL((int64_t)set->entries.liveCount));
      return true;
    }

    runtimeError("Set has no property '%s'.", name->chars);
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
    if (!IS_INT(index)) {
      runtimeError("Array index must be an integer.");
      return false;
    }

    ObjArray* array = AS_ARRAY(receiver);
    int i = (int)AS_INT(index);

    if (i < 0 || i >= array->elements.count) {
      runtimeError("Array index %d out of bounds [0, %d).",
                   i, array->elements.count);
      return false;
    }

    push(array->elements.values[i]);
  } else if (IS_BYTES(receiver)) {
    if (!IS_INT(index)) {
      raiseError(vm.typeErrorClass, "bytes index must be an integer.");
      return false;
    }
    ObjBytes* bytes = AS_BYTES(receiver);
    int i = (int)AS_INT(index);
    if (i < 0 || i >= bytes->length) {
      raiseError(vm.rangeErrorClass, "bytes index %d out of bounds [0, %d).",
                 i, bytes->length);
      return false;
    }
    push(INT_VAL((int64_t)bytes->bytes[i]));
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
  // Keep the operands on the stack (rooted) while we work: a map insertion can
  // grow the table and trigger a GC, which would otherwise free a freshly-built
  // key/value that is no longer referenced anywhere else.
  Value value = peek(0);
  Value index = peek(1);
  Value receiver = peek(2);

  if (IS_ARRAY(receiver)) {
    if (!IS_INT(index)) {
      runtimeError("Array index must be an integer.");
      return false;
    }

    ObjArray* array = AS_ARRAY(receiver);
    int i = (int)AS_INT(index);

    if (i < 0 || i >= array->elements.count) {
      runtimeError("Array index %d out of bounds [0, %d).",
                   i, array->elements.count);
      return false;
    }

    if (array->elementType != NULL &&
        !valueMatchesTypeDescriptor(value, array->elementType)) {
      ObjString* actual = typeOfValue(value);
      runtimeError("Type error: array expects %s but got %s.",
                   array->elementType->name->chars, actual->chars);
      return false;
    }
    array->elements.values[i] = value;
  } else if (IS_MAP(receiver)) {
    ObjMap* map = AS_MAP(receiver);
    if (map->keyType != NULL &&
        !valueMatchesTypeDescriptor(index, map->keyType)) {
      ObjString* actual = typeOfValue(index);
      runtimeError("Type error: map key expects %s but got %s.",
                   map->keyType->name->chars, actual->chars);
      return false;
    }
    if (map->valueType != NULL &&
        !valueMatchesTypeDescriptor(value, map->valueType)) {
      ObjString* actual = typeOfValue(value);
      runtimeError("Type error: map value expects %s but got %s.",
                   map->valueType->name->chars, actual->chars);
      return false;
    }
    valueTableSet(&map->entries, index, value);
  } else if (IS_BYTES(receiver)) {
    raiseError(vm.typeErrorClass, "bytes are immutable.");
    return false;
  } else {
    runtimeError("Only arrays and maps support index assignment.");
    return false;
  }

  vm.stackTop -= 3; // pop receiver, index, value
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

  // Create closure with module's own globals. Root modFunc across newClosure's
  // allocations (it is otherwise unreferenced and would be collected).
  push(OBJ_VAL(modFunc)); // GC protect
  ObjClosure* modClosure = newClosure(modFunc);
  modClosure->globals = &module->values;
  modClosure->globalsOwner = (Obj*)module;
  pop(); // modFunc (now held by the closure)
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
        if (valuesEqual(a, b)) {
          push(BOOL_VAL(true));
        } else {
          Value result;
          if (callMagicBinary(a, vm.magicEq, b, &result)) {
            push(BOOL_VAL(!IS_NIL(result) &&
                 !(IS_BOOL(result) && !AS_BOOL(result))));
          } else if (vm.nativeError || vm.pendingUnwind) {
            InterpretResult mir = postCallFailure(&frame, baseFrame);
            if (mir != INTERPRET_OK) return mir;
          } else if (vm.frameCount == 0) {
            return INTERPRET_RUNTIME_ERROR;
          } else {
            push(BOOL_VAL(false));
          }
        }
        break;
      }
      case OP_GREATER: {
        if (IS_INT(peek(0)) && IS_INT(peek(1))) {
          int64_t b = AS_INT(pop());
          int64_t a = AS_INT(pop());
          push(BOOL_VAL(a > b));
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_DOUBLE_COERCE(pop());
          double a = AS_DOUBLE_COERCE(pop());
          push(BOOL_VAL(a > b));
        } else {
          Value b = pop();
          Value a = pop();
          Value result;
          if (callMagicBinary(a, vm.magicCmp, b, &result)) {
            if (!IS_NUMBER(result)) {
              runtimeError("__cmp__ must return a number.");
              return INTERPRET_RUNTIME_ERROR;
            }
            push(BOOL_VAL(AS_DOUBLE_COERCE(result) > 0));
          } else if (vm.nativeError || vm.pendingUnwind) {
            InterpretResult mir = postCallFailure(&frame, baseFrame);
            if (mir != INTERPRET_OK) return mir;
          } else if (vm.frameCount == 0) {
            return INTERPRET_RUNTIME_ERROR;
          } else {
            runtimeError("Operands must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
          }
        }
        break;
      }
      case OP_LESS: {
        if (IS_INT(peek(0)) && IS_INT(peek(1))) {
          int64_t b = AS_INT(pop());
          int64_t a = AS_INT(pop());
          push(BOOL_VAL(a < b));
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_DOUBLE_COERCE(pop());
          double a = AS_DOUBLE_COERCE(pop());
          push(BOOL_VAL(a < b));
        } else {
          Value b = pop();
          Value a = pop();
          Value result;
          if (callMagicBinary(a, vm.magicCmp, b, &result)) {
            if (!IS_NUMBER(result)) {
              runtimeError("__cmp__ must return a number.");
              return INTERPRET_RUNTIME_ERROR;
            }
            push(BOOL_VAL(AS_DOUBLE_COERCE(result) < 0));
          } else if (vm.nativeError || vm.pendingUnwind) {
            InterpretResult mir = postCallFailure(&frame, baseFrame);
            if (mir != INTERPRET_OK) return mir;
          } else if (vm.frameCount == 0) {
            return INTERPRET_RUNTIME_ERROR;
          } else {
            runtimeError("Operands must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
          }
        }
        break;
      }
      case OP_ADD: {
        if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
          concatenate();
        } else if (IS_INT(peek(0)) && IS_INT(peek(1))) {
          int64_t b = AS_INT(pop());
          int64_t a = AS_INT(pop());
          int64_t r;
          if (int64AddOverflow(a, b, &r)) {
            runtimeError("Integer overflow.");
            return INTERPRET_RUNTIME_ERROR;
          }
          push(INT_VAL(r));
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_DOUBLE_COERCE(pop());
          double a = AS_DOUBLE_COERCE(pop());
          push(DOUBLE_VAL(a + b));
        } else {
          Value b = pop();
          Value a = pop();
          Value result;
          if (callMagicBinary(a, vm.magicAdd, b, &result)) {
            push(result);
          } else if (vm.nativeError || vm.pendingUnwind) {
            InterpretResult mir = postCallFailure(&frame, baseFrame);
            if (mir != INTERPRET_OK) return mir;
          } else if (vm.frameCount == 0) {
            return INTERPRET_RUNTIME_ERROR;
          } else {
            runtimeError(
                "Operands must be two numbers or two strings.");
            return INTERPRET_RUNTIME_ERROR;
          }
        }
        break;
      }
      case OP_SUBTRACT: {
        if (IS_INT(peek(0)) && IS_INT(peek(1))) {
          int64_t b = AS_INT(pop());
          int64_t a = AS_INT(pop());
          int64_t r;
          if (int64SubOverflow(a, b, &r)) {
            runtimeError("Integer overflow.");
            return INTERPRET_RUNTIME_ERROR;
          }
          push(INT_VAL(r));
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_DOUBLE_COERCE(pop());
          double a = AS_DOUBLE_COERCE(pop());
          push(DOUBLE_VAL(a - b));
        } else {
          Value b = pop();
          Value a = pop();
          Value result;
          if (callMagicBinary(a, vm.magicSub, b, &result)) {
            push(result);
          } else if (vm.nativeError || vm.pendingUnwind) {
            InterpretResult mir = postCallFailure(&frame, baseFrame);
            if (mir != INTERPRET_OK) return mir;
          } else if (vm.frameCount == 0) {
            return INTERPRET_RUNTIME_ERROR;
          } else {
            runtimeError("Operands must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
          }
        }
        break;
      }
      case OP_MULTIPLY: {
        if (IS_INT(peek(0)) && IS_INT(peek(1))) {
          int64_t b = AS_INT(pop());
          int64_t a = AS_INT(pop());
          int64_t r;
          if (int64MulOverflow(a, b, &r)) {
            runtimeError("Integer overflow.");
            return INTERPRET_RUNTIME_ERROR;
          }
          push(INT_VAL(r));
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_DOUBLE_COERCE(pop());
          double a = AS_DOUBLE_COERCE(pop());
          push(DOUBLE_VAL(a * b));
        } else {
          Value b = pop();
          Value a = pop();
          Value result;
          if (callMagicBinary(a, vm.magicMul, b, &result)) {
            push(result);
          } else if (vm.nativeError || vm.pendingUnwind) {
            InterpretResult mir = postCallFailure(&frame, baseFrame);
            if (mir != INTERPRET_OK) return mir;
          } else if (vm.frameCount == 0) {
            return INTERPRET_RUNTIME_ERROR;
          } else {
            runtimeError("Operands must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
          }
        }
        break;
      }
      case OP_DIVIDE: {
        if (IS_INT(peek(0)) && IS_INT(peek(1))) {
          int64_t b = AS_INT(pop());
          int64_t a = AS_INT(pop());
          if (b == 0) {
            runtimeError("Division by zero.");
            return INTERPRET_RUNTIME_ERROR;
          }
          if (a == INT64_MIN && b == -1) {
            runtimeError("Integer overflow.");
            return INTERPRET_RUNTIME_ERROR;
          }
          push(INT_VAL(a / b));
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_DOUBLE_COERCE(pop());
          double a = AS_DOUBLE_COERCE(pop());
          push(DOUBLE_VAL(a / b));
        } else {
          Value b = pop();
          Value a = pop();
          Value result;
          if (callMagicBinary(a, vm.magicDiv, b, &result)) {
            push(result);
          } else if (vm.nativeError || vm.pendingUnwind) {
            InterpretResult mir = postCallFailure(&frame, baseFrame);
            if (mir != INTERPRET_OK) return mir;
          } else if (vm.frameCount == 0) {
            return INTERPRET_RUNTIME_ERROR;
          } else {
            runtimeError("Operands must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
          }
        }
        break;
      }
      case OP_MODULO: {
        if (IS_INT(peek(0)) && IS_INT(peek(1))) {
          int64_t b = AS_INT(pop());
          int64_t a = AS_INT(pop());
          if (b == 0) {
            runtimeError("Division by zero.");
            return INTERPRET_RUNTIME_ERROR;
          }
          push(INT_VAL(a % b));
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_DOUBLE_COERCE(pop());
          double a = AS_DOUBLE_COERCE(pop());
          push(DOUBLE_VAL(fmod(a, b)));
        } else {
          Value b = pop();
          Value a = pop();
          Value result;
          if (callMagicBinary(a, vm.magicMod, b, &result)) {
            push(result);
          } else if (vm.nativeError || vm.pendingUnwind) {
            InterpretResult mir = postCallFailure(&frame, baseFrame);
            if (mir != INTERPRET_OK) return mir;
          } else if (vm.frameCount == 0) {
            return INTERPRET_RUNTIME_ERROR;
          } else {
            runtimeError("Operands must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
          }
        }
        break;
      }
      case OP_NOT:
        push(BOOL_VAL(isFalsey(pop())));
        break;
      case OP_NEGATE: {
        if (IS_INT(peek(0))) {
          int64_t a = AS_INT(pop());
          if (a == INT64_MIN) {
            runtimeError("Integer overflow.");
            return INTERPRET_RUNTIME_ERROR;
          }
          push(INT_VAL(-a));
        } else if (IS_DOUBLE(peek(0))) {
          push(DOUBLE_VAL(-AS_DOUBLE(pop())));
        } else {
          Value operand = pop();
          Value result;
          if (callMagicUnary(operand, vm.magicNeg, &result)) {
            push(result);
          } else if (vm.nativeError || vm.pendingUnwind) {
            InterpretResult mir = postCallFailure(&frame, baseFrame);
            if (mir != INTERPRET_OK) return mir;
          } else if (vm.frameCount == 0) {
            return INTERPRET_RUNTIME_ERROR;
          } else {
            runtimeError("Operand must be a number.");
            return INTERPRET_RUNTIME_ERROR;
          }
        }
        break;
      }
      case OP_BITWISE_AND: {
        if (!IS_INT(peek(0)) || !IS_INT(peek(1))) {
          runtimeError("Bitwise operands must be integers.");
          return INTERPRET_RUNTIME_ERROR;
        }
        int64_t b = AS_INT(pop());
        int64_t a = AS_INT(pop());
        push(INT_VAL(a & b));
        break;
      }
      case OP_BITWISE_OR: {
        if (!IS_INT(peek(0)) || !IS_INT(peek(1))) {
          runtimeError("Bitwise operands must be integers.");
          return INTERPRET_RUNTIME_ERROR;
        }
        int64_t b = AS_INT(pop());
        int64_t a = AS_INT(pop());
        push(INT_VAL(a | b));
        break;
      }
      case OP_BITWISE_XOR: {
        if (!IS_INT(peek(0)) || !IS_INT(peek(1))) {
          runtimeError("Bitwise operands must be integers.");
          return INTERPRET_RUNTIME_ERROR;
        }
        int64_t b = AS_INT(pop());
        int64_t a = AS_INT(pop());
        push(INT_VAL(a ^ b));
        break;
      }
      case OP_BITWISE_NOT: {
        if (!IS_INT(peek(0))) {
          runtimeError("Bitwise operand must be an integer.");
          return INTERPRET_RUNTIME_ERROR;
        }
        int64_t a = AS_INT(pop());
        push(INT_VAL(~a));
        break;
      }
      case OP_LEFT_SHIFT: {
        if (!IS_INT(peek(0)) || !IS_INT(peek(1))) {
          runtimeError("Bitwise operands must be integers.");
          return INTERPRET_RUNTIME_ERROR;
        }
        int64_t b = AS_INT(pop()) & 63;
        int64_t a = AS_INT(pop());
        push(INT_VAL((int64_t)((uint64_t)a << b)));
        break;
      }
      case OP_RIGHT_SHIFT: {
        if (!IS_INT(peek(0)) || !IS_INT(peek(1))) {
          runtimeError("Bitwise operands must be integers.");
          return INTERPRET_RUNTIME_ERROR;
        }
        int64_t b = AS_INT(pop()) & 63;
        int64_t a = AS_INT(pop());
        push(INT_VAL(a >> b));
        break;
      }
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
          InterpretResult ir = postCallFailure(&frame, baseFrame);
          if (ir != INTERPRET_OK) return ir;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_CALL_KW: {
        int posCount = READ_BYTE();
        int namedCount = READ_BYTE();
        if (!callKeyword(posCount, namedCount)) {
          InterpretResult ir = postCallFailure(&frame, baseFrame);
          if (ir != INTERPRET_OK) return ir;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_INVOKE: {
        ObjString* method = READ_STRING();
        int argCount = READ_BYTE();
        if (!invoke(method, argCount)) {
          InterpretResult ir = postCallFailure(&frame, baseFrame);
          if (ir != INTERPRET_OK) return ir;
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
        if (vm.stackTop + argCount > vm.stack + STACK_MAX) {
          runtimeError("Stack overflow: too many spread arguments.");
          return INTERPRET_RUNTIME_ERROR;
        }
        for (int i = 0; i < argCount; i++) {
          push(argsArr->elements.values[i]);
        }
        if (!callValue(peek(argCount), argCount)) {
          InterpretResult ir = postCallFailure(&frame, baseFrame);
          if (ir != INTERPRET_OK) return ir;
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
        if (vm.stackTop + argCount > vm.stack + STACK_MAX) {
          runtimeError("Stack overflow: too many spread arguments.");
          return INTERPRET_RUNTIME_ERROR;
        }
        for (int i = 0; i < argCount; i++) {
          push(argsArr->elements.values[i]);
        }
        if (!invoke(method, argCount)) {
          InterpretResult ir = postCallFailure(&frame, baseFrame);
          if (ir != INTERPRET_OK) return ir;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_CHECK_TYPE: {
        Value typeVal = pop();
        Value instance = pop();
        bool match = false;
        if (IS_TYPE_DESCRIPTOR(typeVal)) {
          match = valueMatchesTypeDescriptor(instance,
                      AS_TYPE_DESCRIPTOR(typeVal));
        } else if (IS_CLASS(typeVal)) {
          if (IS_INSTANCE(instance)) {
            ObjClass* klass = AS_INSTANCE(instance)->klass;
            ObjClass* target = AS_CLASS(typeVal);
            while (klass != NULL) {
              if (klass == target) { match = true; break; }
              klass = klass->superclass;
            }
          }
        } else {
          runtimeError("Type pattern requires a type or class.");
          return INTERPRET_RUNTIME_ERROR;
        }
        push(BOOL_VAL(match));
        break;
      }
      case OP_ITERATE: {
        // Fast-path for built-in iterators. Pushes next value on success.
        // Throws StopIteration when exhausted (caught by compiler-emitted
        // try/catch in forEachStatement).
        Value iterVal = peek(0);

        if (IS_INSTANCE(iterVal)) {
          // User-defined iterator: dispatch __next__() via invoke.
          // The method call executes normally in the dispatch loop.
          // If __next__() throws StopIteration, the compiler-emitted
          // try/catch in forEachStatement() catches it.
          if (!invoke(vm.magicNext, 0)) {
            return INTERPRET_RUNTIME_ERROR;
          }
          frame = &vm.frames[vm.frameCount - 1];
          break;
        }

        if (!IS_ITERATOR(iterVal)) {
          runtimeError("Value is not iterable.");
          return INTERPRET_RUNTIME_ERROR;
        }

        ObjIterator* iter = AS_ITERATOR(iterVal);
        Value nextVal;
        bool hasNext = false;

        if (IS_ARRAY(iter->source)) {
          ObjArray* arr = AS_ARRAY(iter->source);
          if (iter->index < arr->elements.count) {
            nextVal = arr->elements.values[iter->index++];
            hasNext = true;
          }
        } else if (IS_BYTES(iter->source)) {
          ObjBytes* bytes = AS_BYTES(iter->source);
          if (iter->index < bytes->length) {
            nextVal = INT_VAL((int64_t)bytes->bytes[iter->index++]);
            hasNext = true;
          }
        } else if (IS_MAP(iter->source)) {
          ObjMap* map = AS_MAP(iter->source);
          while (iter->index < map->entries.capacity) {
            ValueEntry* entry = &map->entries.entries[iter->index++];
            if (entry->occupied) {
              nextVal = entry->key;
              hasNext = true;
              break;
            }
          }
        } else if (IS_SET(iter->source)) {
          ObjSet* set = AS_SET(iter->source);
          while (iter->index < set->entries.capacity) {
            ValueEntry* entry = &set->entries.entries[iter->index++];
            if (entry->occupied) {
              nextVal = entry->key;
              hasNext = true;
              break;
            }
          }
        }

        if (hasNext) {
          pop(); // pop iterator
          push(nextVal);
        } else {
          // Exhausted: throw StopIteration.
          pop(); // pop iterator
          ObjInstance* stopInst = newInstance(vm.stopIterationClass);
          push(OBJ_VAL(stopInst));
          tableSet(&stopInst->fields,
                   copyString("message", 7),
                   OBJ_VAL(copyString("iterator exhausted", 18)));
          Value exc = pop();
          if (!throwException(exc)) {
            return INTERPRET_RUNTIME_ERROR;
          }
          InterpretResult ir = tryLandUnwind(&frame, baseFrame);
          if (ir != INTERPRET_OK) return ir;
        }
        break;
      }
      case OP_SUPER_INVOKE: {
        ObjString* method = READ_STRING();
        int argCount = READ_BYTE();
        ObjClass* superclass = AS_CLASS(pop());
        if (!invokeFromClass(superclass, method, argCount)) {
          InterpretResult ir = postCallFailure(&frame, baseFrame);
          if (ir != INTERPRET_OK) return ir;
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
      case OP_MATCH_FAIL: {
        // Reached only when a match expression had no matching arm.
        raiseError(vm.valueErrorClass, "no match arm matched.");
        InterpretResult r = postCallFailure(&frame, baseFrame);
        if (r != INTERPRET_OK) return r;
        break;
      }
      case OP_INDEX_GET: {
        // executeIndexGet may raise a catchable error (e.g. bytes bounds via
        // raiseError) or abort via runtimeError; postCallFailure handles both.
        if (!executeIndexGet()) {
          InterpretResult r = postCallFailure(&frame, baseFrame);
          if (r != INTERPRET_OK) return r;
        }
        break;
      }
      case OP_INDEX_SET: {
        if (!executeIndexSet()) {
          InterpretResult r = postCallFailure(&frame, baseFrame);
          if (r != INTERPRET_OK) return r;
        }
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
        subclass->superclass = AS_CLASS(superclass);
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
        InterpretResult ir = tryLandUnwind(&frame, baseFrame);
        if (ir != INTERPRET_OK) return ir;
        break;
      }
      case OP_DEFAULT_ARG: {
        // operand = local slot of the optional parameter. Run the default
        // expression only if the caller left the slot unfilled (MISSING).
        uint8_t slot = READ_BYTE();
        uint16_t jump = READ_SHORT();
        if (!IS_MISSING(frame->slots[slot])) {
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
        push(INT_VAL((int64_t)completionType));
        push(payload);
        break;
      }
      case OP_END_FINALLY: {
        Value payload = pop();
        int tag = (int)AS_INT(pop());
        switch (tag) {
          case COMPLETE_NORMAL:
            break;
          case COMPLETE_EXCEPTION: {
            if (!throwException(payload)) {
              return INTERPRET_RUNTIME_ERROR;
            }
            InterpretResult ir = tryLandUnwind(&frame, baseFrame);
            if (ir != INTERPRET_OK) return ir;
            break;
          }
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
                        (int)AS_INT(payload);
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
