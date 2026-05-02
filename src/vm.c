#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
static void closeUpvalues(Value* last);
static ObjString* stringify(Value value);
static bool callValue(Value callee, int argCount);
static InterpretResult run(int baseFrame);

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

// --- VM callback helpers ---

static bool vmCallValue(Value callee, int argCount, Value* result) {
  int framesBefore = vm.frameCount;
  if (!callValue(callee, argCount)) return false;
  if (vm.frameCount > framesBefore) {
    InterpretResult r = run(framesBefore);
    if (r != INTERPRET_OK) return false;
  }
  *result = pop();
  return true;
}

static int getCallableArity(Value callee) {
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

// --- Higher-order array methods ---

static bool arrayMap(Value receiver, int argCount, Value* args,
                     Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;

  int length = array->elements.count;
  ObjArray* resultArray = newArray();

  push(receiver);
  push(callback);
  push(OBJ_VAL(resultArray));

  for (int i = 0; i < length; i++) {
    ObjArray* src = AS_ARRAY(vm.stackTop[-3]);
    Value cb = vm.stackTop[-2];

    push(cb);
    push(src->elements.values[i]);
    if (passArgs >= 2) push(NUMBER_VAL(i));

    Value mapped;
    if (!vmCallValue(cb, passArgs, &mapped)) {
      vm.stackTop -= 3;
      return false;
    }

    ObjArray* dst = AS_ARRAY(vm.stackTop[-1]);
    writeValueArray(&dst->elements, mapped);
  }

  *result = vm.stackTop[-1];
  vm.stackTop -= 3;
  return true;
}

static bool arrayFilter(Value receiver, int argCount, Value* args,
                        Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;

  int length = array->elements.count;
  ObjArray* resultArray = newArray();

  push(receiver);
  push(callback);
  push(OBJ_VAL(resultArray));

  for (int i = 0; i < length; i++) {
    ObjArray* src = AS_ARRAY(vm.stackTop[-3]);
    Value cb = vm.stackTop[-2];
    Value element = src->elements.values[i];

    push(cb);
    push(element);
    if (passArgs >= 2) push(NUMBER_VAL(i));

    Value test;
    if (!vmCallValue(cb, passArgs, &test)) {
      vm.stackTop -= 3;
      return false;
    }

    if (!IS_NIL(test) && !(IS_BOOL(test) && !AS_BOOL(test))) {
      ObjArray* dst = AS_ARRAY(vm.stackTop[-1]);
      src = AS_ARRAY(vm.stackTop[-3]);
      writeValueArray(&dst->elements, src->elements.values[i]);
    }
  }

  *result = vm.stackTop[-1];
  vm.stackTop -= 3;
  return true;
}

static bool arrayReduce(Value receiver, int argCount, Value* args,
                        Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  Value callback = args[0];
  int startIndex;
  Value accumulator;

  if (argCount == 2) {
    accumulator = args[1];
    startIndex = 0;
  } else {
    if (array->elements.count == 0) {
      runtimeError("reduce() of empty array with no initial value.");
      return false;
    }
    accumulator = array->elements.values[0];
    startIndex = 1;
  }

  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 3) ? 3 : 2;
  int length = array->elements.count;

  push(receiver);
  push(callback);
  push(accumulator);

  for (int i = startIndex; i < length; i++) {
    ObjArray* src = AS_ARRAY(vm.stackTop[-3]);
    Value cb = vm.stackTop[-2];
    Value acc = vm.stackTop[-1];

    push(cb);
    push(acc);
    push(src->elements.values[i]);
    if (passArgs >= 3) push(NUMBER_VAL(i));

    Value reduced;
    if (!vmCallValue(cb, passArgs, &reduced)) {
      vm.stackTop -= 3;
      return false;
    }

    vm.stackTop[-1] = reduced;
  }

  *result = vm.stackTop[-1];
  vm.stackTop -= 3;
  return true;
}

static bool arrayForEach(Value receiver, int argCount, Value* args,
                         Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;
  int length = array->elements.count;

  push(receiver);
  push(callback);

  for (int i = 0; i < length; i++) {
    ObjArray* src = AS_ARRAY(vm.stackTop[-2]);
    Value cb = vm.stackTop[-1];

    push(cb);
    push(src->elements.values[i]);
    if (passArgs >= 2) push(NUMBER_VAL(i));

    Value dummy;
    if (!vmCallValue(cb, passArgs, &dummy)) {
      vm.stackTop -= 2;
      return false;
    }
  }

  vm.stackTop -= 2;
  *result = NIL_VAL;
  return true;
}

static bool arrayFind(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;
  int length = array->elements.count;

  push(receiver);
  push(callback);

  for (int i = 0; i < length; i++) {
    ObjArray* src = AS_ARRAY(vm.stackTop[-2]);
    Value cb = vm.stackTop[-1];
    Value element = src->elements.values[i];

    push(cb);
    push(element);
    if (passArgs >= 2) push(NUMBER_VAL(i));

    Value test;
    if (!vmCallValue(cb, passArgs, &test)) {
      vm.stackTop -= 2;
      return false;
    }

    if (!IS_NIL(test) && !(IS_BOOL(test) && !AS_BOOL(test))) {
      src = AS_ARRAY(vm.stackTop[-2]);
      *result = src->elements.values[i];
      vm.stackTop -= 2;
      return true;
    }
  }

  vm.stackTop -= 2;
  *result = NIL_VAL;
  return true;
}

static bool arrayFindIndex(Value receiver, int argCount, Value* args,
                           Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;
  int length = array->elements.count;

  push(receiver);
  push(callback);

  for (int i = 0; i < length; i++) {
    ObjArray* src = AS_ARRAY(vm.stackTop[-2]);
    Value cb = vm.stackTop[-1];

    push(cb);
    push(src->elements.values[i]);
    if (passArgs >= 2) push(NUMBER_VAL(i));

    Value test;
    if (!vmCallValue(cb, passArgs, &test)) {
      vm.stackTop -= 2;
      return false;
    }

    if (!IS_NIL(test) && !(IS_BOOL(test) && !AS_BOOL(test))) {
      vm.stackTop -= 2;
      *result = NUMBER_VAL(i);
      return true;
    }
  }

  vm.stackTop -= 2;
  *result = NUMBER_VAL(-1);
  return true;
}

static bool arrayAll(Value receiver, int argCount, Value* args,
                     Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;
  int length = array->elements.count;

  push(receiver);
  push(callback);

  for (int i = 0; i < length; i++) {
    ObjArray* src = AS_ARRAY(vm.stackTop[-2]);
    Value cb = vm.stackTop[-1];

    push(cb);
    push(src->elements.values[i]);
    if (passArgs >= 2) push(NUMBER_VAL(i));

    Value test;
    if (!vmCallValue(cb, passArgs, &test)) {
      vm.stackTop -= 2;
      return false;
    }

    if (IS_NIL(test) || (IS_BOOL(test) && !AS_BOOL(test))) {
      vm.stackTop -= 2;
      *result = BOOL_VAL(false);
      return true;
    }
  }

  vm.stackTop -= 2;
  *result = BOOL_VAL(true);
  return true;
}

static bool arrayAny(Value receiver, int argCount, Value* args,
                     Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;
  int length = array->elements.count;

  push(receiver);
  push(callback);

  for (int i = 0; i < length; i++) {
    ObjArray* src = AS_ARRAY(vm.stackTop[-2]);
    Value cb = vm.stackTop[-1];

    push(cb);
    push(src->elements.values[i]);
    if (passArgs >= 2) push(NUMBER_VAL(i));

    Value test;
    if (!vmCallValue(cb, passArgs, &test)) {
      vm.stackTop -= 2;
      return false;
    }

    if (!IS_NIL(test) && !(IS_BOOL(test) && !AS_BOOL(test))) {
      vm.stackTop -= 2;
      *result = BOOL_VAL(true);
      return true;
    }
  }

  vm.stackTop -= 2;
  *result = BOOL_VAL(false);
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
      case OBJ_MODULE:        return OBJ_VAL(copyString("module", 6));
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

static Value sqrtNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("sqrt() expects a number argument.");
    return NIL_VAL;
  }
  return NUMBER_VAL(sqrt(AS_NUMBER(args[0])));
}

static Value absNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("abs() expects a number argument.");
    return NIL_VAL;
  }
  return NUMBER_VAL(fabs(AS_NUMBER(args[0])));
}

static Value floorNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("floor() expects a number argument.");
    return NIL_VAL;
  }
  return NUMBER_VAL(floor(AS_NUMBER(args[0])));
}

static Value ceilNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("ceil() expects a number argument.");
    return NIL_VAL;
  }
  return NUMBER_VAL(ceil(AS_NUMBER(args[0])));
}

static Value roundNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("round() expects a number argument.");
    return NIL_VAL;
  }
  return NUMBER_VAL(round(AS_NUMBER(args[0])));
}

static Value minNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    runtimeError("min() expects two number arguments.");
    return NIL_VAL;
  }
  double a = AS_NUMBER(args[0]), b = AS_NUMBER(args[1]);
  return NUMBER_VAL(a < b ? a : b);
}

static Value maxNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    runtimeError("max() expects two number arguments.");
    return NIL_VAL;
  }
  double a = AS_NUMBER(args[0]), b = AS_NUMBER(args[1]);
  return NUMBER_VAL(a > b ? a : b);
}

static Value powNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    runtimeError("pow() expects two number arguments.");
    return NIL_VAL;
  }
  return NUMBER_VAL(pow(AS_NUMBER(args[0]), AS_NUMBER(args[1])));
}

static Value randomNative(int argCount, Value* args) {
  return NUMBER_VAL((double)rand() / RAND_MAX);
}

static Value parseIntNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("parseInt() expects a string argument.");
    return NIL_VAL;
  }
  char* end;
  double result = strtod(AS_STRING(args[0])->chars, &end);
  if (end == AS_STRING(args[0])->chars) return NIL_VAL;
  return NUMBER_VAL(result);
}

static void resetStack() {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
  vm.exceptionHandlerCount = 0;
  vm.currentException = NIL_VAL;
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

static void defineNativeMethodVar(ObjClass* klass, const char* name,
                                  NativeMethodFn function,
                                  int minArity, int maxArity) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNativeMethod(function, maxArity)));
  AS_NATIVE_METHOD(vm.stack[1])->minArity = minArity;
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

    vm.arrayMethods = NULL;
    vm.arrayMethods = newClass(copyString("Array", 5));
    defineNativeMethod(vm.arrayMethods, "push", arrayPush, 1);
    defineNativeMethod(vm.arrayMethods, "pop", arrayPop, 0);
    defineNativeMethod(vm.arrayMethods, "slice", arraySlice, 2);
    defineNativeMethod(vm.arrayMethods, "map", arrayMap, 1);
    defineNativeMethod(vm.arrayMethods, "filter", arrayFilter, 1);
    defineNativeMethodVar(vm.arrayMethods, "reduce", arrayReduce, 1, 2);
    defineNativeMethod(vm.arrayMethods, "forEach", arrayForEach, 1);
    defineNativeMethod(vm.arrayMethods, "find", arrayFind, 1);
    defineNativeMethod(vm.arrayMethods, "findIndex", arrayFindIndex, 1);
    defineNativeMethod(vm.arrayMethods, "all", arrayAll, 1);
    defineNativeMethod(vm.arrayMethods, "any", arrayAny, 1);

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
    defineNative("sqrt", sqrtNative);
    defineNative("abs", absNative);
    defineNative("floor", floorNative);
    defineNative("ceil", ceilNative);
    defineNative("round", roundNative);
    defineNative("min", minNative);
    defineNative("max", maxNative);
    defineNative("pow", powNative);
    defineNative("random", randomNative);
    defineNative("parseInt", parseIntNative);

    srand((unsigned int)time(NULL));
}

void freeVM() {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  freeTable(&vm.importCache);
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
  if (argCount < closure->function->minArity ||
      argCount > closure->function->arity) {
    if (closure->function->minArity == closure->function->arity) {
      runtimeError("Expected %d arguments but got %d.",
          closure->function->arity, argCount);
    } else {
      runtimeError("Expected %d to %d arguments but got %d.",
          closure->function->minArity, closure->function->arity,
          argCount);
    }
    return false;
  }

  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  // Pad missing optional args with nil.
  for (int i = argCount; i < closure->function->arity; i++) {
    push(NIL_VAL);
  }

  CallFrame* frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = closure->function->chunk.code;
  frame->slots = vm.stackTop - closure->function->arity - 1;
  frame->argCount = argCount;
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
        if (IS_MODULE(peek(0))) {
          ObjModule* module = AS_MODULE(peek(0));
          ObjString* name = READ_STRING();
          Value value;
          if (tableGet(&module->values, name, &value)) {
            pop();
            push(value);
            break;
          }
          runtimeError("Module '%s' has no member '%s'.",
                       module->name->chars, name->chars);
          return INTERPRET_RUNTIME_ERROR;
        }

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
          break;
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
        frame = &vm.frames[vm.frameCount - 1];

        // Cache the module (replace sentinel).
        tableSet(&vm.importCache, pathStr, OBJ_VAL(module));

        // Clean up stack: pop module, pop pathStr, push module.
        pop(); // module (GC protect)
        pop(); // pathStr (GC protect)
        push(OBJ_VAL(module));
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