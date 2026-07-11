#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "builtins.h"
#include "regex_engine.h"
#include "json.h"
#include "common.h"
#include "memory.h"
#include "object.h"
#include "value_table.h"
#include "vm.h"

static Value clockNative(int argCount, Value* args) {
  return DOUBLE_VAL((double)clock() / CLOCKS_PER_SEC);
}

// --- Type guard helpers ---

static bool checkArrayTypeGuard(ObjArray* array, Value value) {
  if (array->elementType == NULL) return true;
  if (valueMatchesTypeDescriptor(value, array->elementType)) return true;
  ObjString* actual = typeOfValue(value);
  runtimeError("Type error: array expects %s but got %s.",
               array->elementType->name->chars, actual->chars);
  return false;
}

static bool checkMapKeyGuard(ObjMap* map, Value key) {
  if (map->keyType == NULL) return true;
  if (valueMatchesTypeDescriptor(key, map->keyType)) return true;
  ObjString* actual = typeOfValue(key);
  runtimeError("Type error: map key expects %s but got %s.",
               map->keyType->name->chars, actual->chars);
  return false;
}

static bool checkMapValueGuard(ObjMap* map, Value value) {
  if (map->valueType == NULL) return true;
  if (valueMatchesTypeDescriptor(value, map->valueType)) return true;
  ObjString* actual = typeOfValue(value);
  runtimeError("Type error: map value expects %s but got %s.",
               map->valueType->name->chars, actual->chars);
  return false;
}

// --- Array native methods ---

static bool arrayPush(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  if (!checkArrayTypeGuard(array, args[0])) return false;
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

  if (!IS_INT(args[0]) || !IS_INT(args[1])) {
    runtimeError("Slice arguments must be integers.");
    return false;
  }

  int start = (int)AS_INT(args[0]);
  int end = (int)AS_INT(args[1]);
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

static int sortTypeOrdinal(Value v) {
  if (IS_NIL(v)) return 0;
  if (IS_BOOL(v)) return 1;
  if (IS_NUMBER(v)) return 2;
  if (IS_STRING(v)) return 3;
  return 4; // non-sortable
}

static int sortCompare(const void* a, const void* b) {
  Value va = *(const Value*)a;
  Value vb = *(const Value*)b;
  int ta = sortTypeOrdinal(va);
  int tb = sortTypeOrdinal(vb);

  if (ta != tb) return (ta < tb) ? -1 : 1;

  switch (ta) {
    case 0: return 0; // nil == nil
    case 1: {
      bool ba = AS_BOOL(va), bb = AS_BOOL(vb);
      return (ba == bb) ? 0 : (ba ? 1 : -1);
    }
    case 2: {
      double da = AS_DOUBLE_COERCE(va), db = AS_DOUBLE_COERCE(vb);
      return (da < db) ? -1 : (da > db) ? 1 : 0;
    }
    case 3:
      return strcmp(AS_CSTRING(va), AS_CSTRING(vb));
    default:
      return 0; // should not be reached
  }
}

static bool needsMagicSort(ObjArray* array) {
  int i;
  for (i = 0; i < array->elements.count; i++) {
    Value v = array->elements.values[i];
    if (IS_INSTANCE(v)) {
      Value method;
      if (tableGet(&AS_INSTANCE(v)->klass->methods,
                   vm.magicCmp, &method)) {
        return true;
      }
    }
  }
  return false;
}

static bool arraySort(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjArray* array = AS_ARRAY(receiver);

  if (needsMagicSort(array)) {
    // Insertion sort with __cmp__ dispatch.
    int i;
    push(receiver); // GC protect — stack: [receiver]
    for (i = 1; i < AS_ARRAY(vm.stackTop[-1])->elements.count; i++) {
      ObjArray* arr = AS_ARRAY(vm.stackTop[-1]);
      Value key = arr->elements.values[i];
      push(key); // GC protect key — stack: [receiver, key]
      int j = i - 1;
      while (j >= 0) {
        arr = AS_ARRAY(vm.stackTop[-2]);
        Value left = arr->elements.values[j];
        Value keyVal = vm.stackTop[-1];
        int cmp;
        Value cmpResult;
        if (IS_INSTANCE(left) &&
            callMagicBinary(left, vm.magicCmp, keyVal, &cmpResult)) {
          if (!IS_NUMBER(cmpResult)) {
            pop(); pop();
            runtimeError("__cmp__ must return a number.");
            return false;
          }
          double d = AS_DOUBLE_COERCE(cmpResult);
          cmp = (d < 0) ? -1 : (d > 0) ? 1 : 0;
        } else if (vm.frameCount == 0) {
          return false;
        } else {
          cmp = sortCompare(&left, &keyVal);
        }
        if (cmp <= 0) break;
        arr = AS_ARRAY(vm.stackTop[-2]);
        arr->elements.values[j + 1] = arr->elements.values[j];
        j--;
      }
      arr = AS_ARRAY(vm.stackTop[-2]);
      arr->elements.values[j + 1] = vm.stackTop[-1];
      pop(); // key
    }
    pop(); // receiver
    *result = receiver;
    return true;
  }

  // Fast path: validate all elements are sortable primitives.
  {
    int i;
    for (i = 0; i < array->elements.count; i++) {
      Value v = array->elements.values[i];
      if (sortTypeOrdinal(v) >= 4) {
        const char* typeName = "unknown";
        if (IS_OBJ(v)) {
          switch (OBJ_TYPE(v)) {
            case OBJ_ARRAY: typeName = "array"; break;
            case OBJ_MAP: typeName = "map"; break;
            case OBJ_SET: typeName = "set"; break;
            case OBJ_CLASS: typeName = "class"; break;
            case OBJ_INSTANCE: typeName = "instance"; break;
            case OBJ_FUNCTION: case OBJ_CLOSURE:
            case OBJ_NATIVE: case OBJ_NATIVE_METHOD:
              typeName = "function"; break;
            case OBJ_FILE: typeName = "file"; break;
            case OBJ_MODULE: typeName = "module"; break;
            default: break;
          }
        }
        runtimeError("Cannot compare values of type '%s'.", typeName);
        return false;
      }
    }
  }

  qsort(array->elements.values, array->elements.count,
        sizeof(Value), sortCompare);
  *result = receiver;
  return true;
}

static bool arrayReverse(Value receiver, int argCount, Value* args,
                         Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  int count = array->elements.count;
  for (int i = 0; i < count / 2; i++) {
    Value tmp = array->elements.values[i];
    array->elements.values[i] = array->elements.values[count - 1 - i];
    array->elements.values[count - 1 - i] = tmp;
  }
  *result = receiver;
  return true;
}

static bool arrayJoin(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  if (!IS_STRING(args[0])) {
    runtimeError("join separator must be a string.");
    return false;
  }
  ObjString* sep = AS_STRING(args[0]);

  if (array->elements.count == 0) {
    *result = OBJ_VAL(copyString("", 0));
    return true;
  }

  // GC protect receiver and separator.
  push(receiver);
  push(args[0]);

  // Stringify all elements and compute total length.
  int count = array->elements.count;
  ObjString** parts = (ObjString**)malloc(sizeof(ObjString*) * count);
  int totalLen = 0;
  for (int i = 0; i < count; i++) {
    parts[i] = stringify(array->elements.values[i]);
    push(OBJ_VAL(parts[i])); // GC protect each part
    totalLen += parts[i]->length;
    if (i > 0) totalLen += sep->length;
  }

  char* buffer = ALLOCATE(char, totalLen + 1);
  char* dst = buffer;
  for (int i = 0; i < count; i++) {
    if (i > 0) {
      memcpy(dst, sep->chars, sep->length);
      dst += sep->length;
    }
    memcpy(dst, parts[i]->chars, parts[i]->length);
    dst += parts[i]->length;
  }
  buffer[totalLen] = '\0';

  // Pop GC protections: count parts + separator + receiver.
  for (int i = 0; i < count + 2; i++) pop();
  free(parts);

  *result = OBJ_VAL(takeString(buffer, totalLen));
  return true;
}

static bool arrayFlat(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  ObjArray* flat = newArray();
  push(OBJ_VAL(flat)); // GC protection

  for (int i = 0; i < array->elements.count; i++) {
    Value elem = array->elements.values[i];
    if (IS_ARRAY(elem)) {
      ObjArray* inner = AS_ARRAY(elem);
      for (int j = 0; j < inner->elements.count; j++) {
        writeValueArray(&flat->elements, inner->elements.values[j]);
      }
    } else {
      writeValueArray(&flat->elements, elem);
    }
  }

  pop(); // remove GC protection
  *result = OBJ_VAL(flat);
  return true;
}

static bool arrayIndexOf(Value receiver, int argCount, Value* args,
                         Value* result) {
  ObjArray* array = AS_ARRAY(receiver);
  Value search = args[0];
  for (int i = 0; i < array->elements.count; i++) {
    if (valuesEqual(array->elements.values[i], search)) {
      *result = INT_VAL((int64_t)i);
      return true;
    }
  }
  *result = INT_VAL(-1);
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

// Invoke a callback with the given arguments via the VM call stack.
static bool invokeCallback(Value callback, int argCount,
                           Value* args, Value* result) {
    push(callback);
    for (int i = 0; i < argCount; i++) {
        push(args[i]);
    }
    return vmCallValue(callback, argCount, result);
}

// --- Higher-order map methods ---

static bool mapForEach(Value receiver, int argCount, Value* args,
                       Value* result) {
  ObjMap* map = AS_MAP(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;

  push(receiver);
  push(callback);

  for (int i = 0; i < map->entries.capacity; i++) {
    ObjMap* src = AS_MAP(vm.stackTop[-2]);
    ValueEntry* entry = &src->entries.entries[i];
    if (!entry->occupied) continue;

    Value cb = vm.stackTop[-1];
    Value cbArgs[2] = { entry->key, entry->value };

    Value dummy;
    if (!invokeCallback(cb, passArgs, cbArgs, &dummy)) {
      vm.stackTop -= 2;
      return false;
    }
  }

  vm.stackTop -= 2;
  *result = NIL_VAL;
  return true;
}

static bool mapMapMethod(Value receiver, int argCount, Value* args,
                         Value* result) {
  ObjMap* map = AS_MAP(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;

  ObjMap* resultMap = newMap();

  push(receiver);
  push(callback);
  push(OBJ_VAL(resultMap));

  for (int i = 0; i < map->entries.capacity; i++) {
    ObjMap* src = AS_MAP(vm.stackTop[-3]);
    ValueEntry* entry = &src->entries.entries[i];
    if (!entry->occupied) continue;

    Value cb = vm.stackTop[-2];
    Value cbArgs[2] = { entry->key, entry->value };

    Value mapped;
    if (!invokeCallback(cb, passArgs, cbArgs, &mapped)) {
      vm.stackTop -= 3;
      return false;
    }

    ObjMap* dst = AS_MAP(vm.stackTop[-1]);
    src = AS_MAP(vm.stackTop[-3]);
    valueTableSet(&dst->entries, src->entries.entries[i].key, mapped);
  }

  *result = vm.stackTop[-1];
  vm.stackTop -= 3;
  return true;
}

static bool mapFilter(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjMap* map = AS_MAP(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;

  ObjMap* resultMap = newMap();

  push(receiver);
  push(callback);
  push(OBJ_VAL(resultMap));

  for (int i = 0; i < map->entries.capacity; i++) {
    ObjMap* src = AS_MAP(vm.stackTop[-3]);
    ValueEntry* entry = &src->entries.entries[i];
    if (!entry->occupied) continue;

    Value cb = vm.stackTop[-2];
    Value cbArgs[2] = { entry->key, entry->value };

    Value test;
    if (!invokeCallback(cb, passArgs, cbArgs, &test)) {
      vm.stackTop -= 3;
      return false;
    }

    if (!IS_NIL(test) && !(IS_BOOL(test) && !AS_BOOL(test))) {
      ObjMap* dst = AS_MAP(vm.stackTop[-1]);
      src = AS_MAP(vm.stackTop[-3]);
      valueTableSet(&dst->entries, src->entries.entries[i].key,
                    src->entries.entries[i].value);
    }
  }

  *result = vm.stackTop[-1];
  vm.stackTop -= 3;
  return true;
}

static bool mapReduce(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjMap* map = AS_MAP(receiver);
  Value callback = args[0];
  Value accumulator;
  bool hasInit = (argCount == 2);

  if (hasInit) {
    accumulator = args[1];
  } else {
    if (map->entries.liveCount == 0) {
      runtimeError("reduce() of empty map with no initial value.");
      return false;
    }
    // Use first entry's value as initial accumulator.
    for (int i = 0; i < map->entries.capacity; i++) {
      if (map->entries.entries[i].occupied) {
        accumulator = map->entries.entries[i].value;
        break;
      }
    }
  }

  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 3) ? 3 : 2;
  bool skipFirst = !hasInit;

  push(receiver);
  push(callback);
  push(accumulator);

  for (int i = 0; i < map->entries.capacity; i++) {
    ObjMap* src = AS_MAP(vm.stackTop[-3]);
    ValueEntry* entry = &src->entries.entries[i];
    if (!entry->occupied) continue;

    if (skipFirst) {
      skipFirst = false;
      continue;
    }

    Value cb = vm.stackTop[-2];
    Value acc = vm.stackTop[-1];
    Value cbArgs[3] = { acc, entry->key, entry->value };

    Value reduced;
    if (!invokeCallback(cb, passArgs, cbArgs, &reduced)) {
      vm.stackTop -= 3;
      return false;
    }

    vm.stackTop[-1] = reduced;
  }

  *result = vm.stackTop[-1];
  vm.stackTop -= 3;
  return true;
}

static bool mapFind(Value receiver, int argCount, Value* args,
                    Value* result) {
  ObjMap* map = AS_MAP(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;

  push(receiver);
  push(callback);

  for (int i = 0; i < map->entries.capacity; i++) {
    ObjMap* src = AS_MAP(vm.stackTop[-2]);
    ValueEntry* entry = &src->entries.entries[i];
    if (!entry->occupied) continue;

    Value cb = vm.stackTop[-1];
    Value cbArgs[2] = { entry->key, entry->value };

    Value test;
    if (!invokeCallback(cb, passArgs, cbArgs, &test)) {
      vm.stackTop -= 2;
      return false;
    }

    if (!IS_NIL(test) && !(IS_BOOL(test) && !AS_BOOL(test))) {
      src = AS_MAP(vm.stackTop[-2]);
      *result = src->entries.entries[i].value;
      vm.stackTop -= 2;
      return true;
    }
  }

  vm.stackTop -= 2;
  *result = NIL_VAL;
  return true;
}

static bool mapAny(Value receiver, int argCount, Value* args,
                   Value* result) {
  ObjMap* map = AS_MAP(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;

  push(receiver);
  push(callback);

  for (int i = 0; i < map->entries.capacity; i++) {
    ObjMap* src = AS_MAP(vm.stackTop[-2]);
    ValueEntry* entry = &src->entries.entries[i];
    if (!entry->occupied) continue;

    Value cb = vm.stackTop[-1];
    Value cbArgs[2] = { entry->key, entry->value };

    Value test;
    if (!invokeCallback(cb, passArgs, cbArgs, &test)) {
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

static bool mapAll(Value receiver, int argCount, Value* args,
                   Value* result) {
  ObjMap* map = AS_MAP(receiver);
  Value callback = args[0];
  int cbArity = getCallableArity(callback);
  int passArgs = (cbArity >= 2) ? 2 : 1;

  push(receiver);
  push(callback);

  for (int i = 0; i < map->entries.capacity; i++) {
    ObjMap* src = AS_MAP(vm.stackTop[-2]);
    ValueEntry* entry = &src->entries.entries[i];
    if (!entry->occupied) continue;

    Value cb = vm.stackTop[-1];
    Value cbArgs[2] = { entry->key, entry->value };

    Value test;
    if (!invokeCallback(cb, passArgs, cbArgs, &test)) {
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

// --- String native methods ---

static bool stringSubstring(Value receiver, int argCount, Value* args,
                            Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_INT(args[0]) || !IS_INT(args[1])) {
    runtimeError("Substring arguments must be integers.");
    return false;
  }
  int start = (int)AS_INT(args[0]);
  int end = (int)AS_INT(args[1]);
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
  *result = INT_VAL(found == NULL ? -1 : (int64_t)(found - str->chars));
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

static bool stringReplace(Value receiver, int argCount, Value* args,
                          Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_STRING(args[0]) || !IS_STRING(args[1])) {
    runtimeError("replace arguments must be strings.");
    return false;
  }
  ObjString* search = AS_STRING(args[0]);
  ObjString* repl = AS_STRING(args[1]);

  if (search->length == 0) {
    *result = receiver;
    return true;
  }

  // First pass: count occurrences.
  int count = 0;
  const char* pos = str->chars;
  while ((pos = strstr(pos, search->chars)) != NULL) {
    count++;
    pos += search->length;
  }

  if (count == 0) {
    *result = receiver;
    return true;
  }

  // Compute new length and allocate.
  int newLen = str->length + count * (repl->length - search->length);
  char* buffer = ALLOCATE(char, newLen + 1);

  // Second pass: build result.
  const char* src = str->chars;
  char* dst = buffer;
  while ((pos = strstr(src, search->chars)) != NULL) {
    int segLen = (int)(pos - src);
    memcpy(dst, src, segLen);
    dst += segLen;
    memcpy(dst, repl->chars, repl->length);
    dst += repl->length;
    src = pos + search->length;
  }
  // Copy remainder.
  int remaining = (int)(str->chars + str->length - src);
  memcpy(dst, src, remaining);
  dst[remaining] = '\0';

  *result = OBJ_VAL(takeString(buffer, newLen));
  return true;
}

static bool stringStartsWith(Value receiver, int argCount, Value* args,
                              Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_STRING(args[0])) {
    runtimeError("startsWith argument must be a string.");
    return false;
  }
  ObjString* prefix = AS_STRING(args[0]);
  if (prefix->length > str->length) {
    *result = BOOL_VAL(false);
    return true;
  }
  *result = BOOL_VAL(memcmp(str->chars, prefix->chars, prefix->length) == 0);
  return true;
}

static bool stringEndsWith(Value receiver, int argCount, Value* args,
                            Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_STRING(args[0])) {
    runtimeError("endsWith argument must be a string.");
    return false;
  }
  ObjString* suffix = AS_STRING(args[0]);
  if (suffix->length > str->length) {
    *result = BOOL_VAL(false);
    return true;
  }
  *result = BOOL_VAL(memcmp(str->chars + str->length - suffix->length,
                            suffix->chars, suffix->length) == 0);
  return true;
}

static bool stringContains(Value receiver, int argCount, Value* args,
                            Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_STRING(args[0])) {
    runtimeError("contains argument must be a string.");
    return false;
  }
  ObjString* search = AS_STRING(args[0]);
  *result = BOOL_VAL(strstr(str->chars, search->chars) != NULL);
  return true;
}

static bool stringRepeat(Value receiver, int argCount, Value* args,
                          Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_INT(args[0])) {
    runtimeError("repeat argument must be an integer.");
    return false;
  }
  int n = (int)AS_INT(args[0]);
  if (n < 0) {
    runtimeError("repeat count must be non-negative.");
    return false;
  }
  if (n == 0 || str->length == 0) {
    *result = OBJ_VAL(copyString("", 0));
    return true;
  }

  int len = str->length;
  if (n > 0 && len > INT_MAX / n) {
    runtimeError("repeat() result too large.");
    return false;
  }
  int newLen = len * n;
  char* buffer = ALLOCATE(char, newLen + 1);
  for (int i = 0; i < n; i++) {
    memcpy(buffer + i * len, str->chars, len);
  }
  buffer[newLen] = '\0';
  *result = OBJ_VAL(takeString(buffer, newLen));
  return true;
}

static bool stringCharAt(Value receiver, int argCount, Value* args,
                          Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_INT(args[0])) {
    runtimeError("charAt argument must be an integer.");
    return false;
  }
  int index = (int)AS_INT(args[0]);
  if (index < 0 || index >= str->length) {
    runtimeError("String index %d out of bounds [0, %d).",
                 index, str->length);
    return false;
  }
  *result = OBJ_VAL(copyString(str->chars + index, 1));
  return true;
}

static bool stringPadStart(Value receiver, int argCount, Value* args,
                           Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_INT(args[0])) {
    runtimeError("padStart() first argument must be an integer.");
    return false;
  }
  int targetLen = (int)AS_INT(args[0]);
  const char* pad = " ";
  int padLen = 1;
  if (argCount == 2) {
    if (!IS_STRING(args[1])) {
      runtimeError("padStart() second argument must be a string.");
      return false;
    }
    pad = AS_STRING(args[1])->chars;
    padLen = AS_STRING(args[1])->length;
  }
  if (targetLen <= str->length || padLen == 0) {
    *result = receiver;
    return true;
  }
  if (targetLen > INT_MAX - 1) {
    runtimeError("padStart() result too large.");
    return false;
  }
  int fillLen = targetLen - str->length;
  char* buffer = ALLOCATE(char, targetLen + 1);
  for (int i = 0; i < fillLen; i++) {
    buffer[i] = pad[i % padLen];
  }
  memcpy(buffer + fillLen, str->chars, str->length);
  buffer[targetLen] = '\0';
  *result = OBJ_VAL(takeString(buffer, targetLen));
  return true;
}

static bool stringPadEnd(Value receiver, int argCount, Value* args,
                         Value* result) {
  ObjString* str = AS_STRING(receiver);
  if (!IS_INT(args[0])) {
    runtimeError("padEnd() first argument must be an integer.");
    return false;
  }
  int targetLen = (int)AS_INT(args[0]);
  const char* pad = " ";
  int padLen = 1;
  if (argCount == 2) {
    if (!IS_STRING(args[1])) {
      runtimeError("padEnd() second argument must be a string.");
      return false;
    }
    pad = AS_STRING(args[1])->chars;
    padLen = AS_STRING(args[1])->length;
  }
  if (targetLen <= str->length || padLen == 0) {
    *result = receiver;
    return true;
  }
  if (targetLen > INT_MAX - 1) {
    runtimeError("padEnd() result too large.");
    return false;
  }
  int fillLen = targetLen - str->length;
  char* buffer = ALLOCATE(char, targetLen + 1);
  memcpy(buffer, str->chars, str->length);
  for (int i = 0; i < fillLen; i++) {
    buffer[str->length + i] = pad[i % padLen];
  }
  buffer[targetLen] = '\0';
  *result = OBJ_VAL(takeString(buffer, targetLen));
  return true;
}

static bool stringTrimStart(Value receiver, int argCount, Value* args,
                            Value* result) {
  ObjString* str = AS_STRING(receiver);
  const char* start = str->chars;
  const char* end = str->chars + str->length;
  while (start < end && (*start == ' ' || *start == '\t' ||
                          *start == '\r' || *start == '\n')) start++;
  *result = OBJ_VAL(copyString(start, (int)(end - start)));
  return true;
}

static bool stringTrimEnd(Value receiver, int argCount, Value* args,
                          Value* result) {
  ObjString* str = AS_STRING(receiver);
  const char* start = str->chars;
  const char* end = str->chars + str->length;
  while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n')) end--;
  *result = OBJ_VAL(copyString(start, (int)(end - start)));
  return true;
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
    Value cbArgs[2] = { src->elements.values[i], INT_VAL((int64_t)i) };

    Value mapped;
    if (!invokeCallback(cb, passArgs, cbArgs, &mapped)) {
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
    Value cbArgs[2] = { src->elements.values[i], INT_VAL((int64_t)i) };

    Value test;
    if (!invokeCallback(cb, passArgs, cbArgs, &test)) {
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
    Value cbArgs[3] = { acc, src->elements.values[i], INT_VAL((int64_t)i) };

    Value reduced;
    if (!invokeCallback(cb, passArgs, cbArgs, &reduced)) {
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
    Value cbArgs[2] = { src->elements.values[i], INT_VAL((int64_t)i) };

    Value dummy;
    if (!invokeCallback(cb, passArgs, cbArgs, &dummy)) {
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
    Value cbArgs[2] = { src->elements.values[i], INT_VAL((int64_t)i) };

    Value test;
    if (!invokeCallback(cb, passArgs, cbArgs, &test)) {
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
    Value cbArgs[2] = { src->elements.values[i], INT_VAL((int64_t)i) };

    Value test;
    if (!invokeCallback(cb, passArgs, cbArgs, &test)) {
      vm.stackTop -= 2;
      return false;
    }

    if (!IS_NIL(test) && !(IS_BOOL(test) && !AS_BOOL(test))) {
      vm.stackTop -= 2;
      *result = INT_VAL((int64_t)i);
      return true;
    }
  }

  vm.stackTop -= 2;
  *result = INT_VAL(-1);
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
    Value cbArgs[2] = { src->elements.values[i], INT_VAL((int64_t)i) };

    Value test;
    if (!invokeCallback(cb, passArgs, cbArgs, &test)) {
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
    Value cbArgs[2] = { src->elements.values[i], INT_VAL((int64_t)i) };

    Value test;
    if (!invokeCallback(cb, passArgs, cbArgs, &test)) {
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

ObjString* typeOfValue(Value value) {
  if (IS_NIL(value))    return copyString("nil", 3);
  if (IS_BOOL(value))   return copyString("bool", 4);
  if (IS_INT(value))    return copyString("int", 3);
  if (IS_DOUBLE(value)) return copyString("double", 6);

  if (IS_OBJ(value)) {
    switch (OBJ_TYPE(value)) {
      case OBJ_STRING:        return copyString("string", 6);
      case OBJ_ARRAY:         return copyString("array", 5);
      case OBJ_BYTES:         return copyString("bytes", 5);
      case OBJ_MAP:           return copyString("map", 3);
      case OBJ_SET:           return copyString("set", 3);
      case OBJ_CLASS:         return copyString("class", 5);
      case OBJ_FILE:          return copyString("file", 4);
      case OBJ_ITERATOR:      return copyString("iterator", 8);
      case OBJ_REGEX:         return copyString("regex", 5);
      case OBJ_INSTANCE:      return copyString("instance", 8);
      case OBJ_MODULE:        return copyString("module", 6);
      case OBJ_FUNCTION:
      case OBJ_CLOSURE:
      case OBJ_NATIVE:
      case OBJ_NATIVE_METHOD:
      case OBJ_BOUND_METHOD:  return copyString("function", 8);
      case OBJ_TYPE_DESCRIPTOR: return copyString("type", 4);
      default: break;
    }
  }
  return copyString("unknown", 7);
}

static Value typeNative(int argCount, Value* args) {
  if (argCount == 0) return OBJ_VAL(typeOfValue(NIL_VAL));
  return OBJ_VAL(typeOfValue(args[0]));
}

static Value toStringNative(int argCount, Value* args) {
  if (argCount != 1) {
    runtimeError("toString() expects 1 argument.");
    return NIL_VAL;
  }
  return OBJ_VAL(stringify(args[0]));
}

static Value assertNative(int argCount, Value* args) {
  if (argCount < 1) {
    runtimeError("assert() expects at least 1 argument.");
    return NIL_VAL;
  }
  if (isFalsey(args[0])) {
    if (argCount >= 2) {
      // A non-string message is rendered via stringify. raiseError copies
      // the text before allocating, so passing msg->chars is GC-safe.
      ObjString* msg = IS_STRING(args[1]) ? AS_STRING(args[1])
                                          : stringify(args[1]);
      raiseError(vm.assertionErrorClass, "%s", msg->chars);
    } else {
      raiseError(vm.assertionErrorClass, "assertion failed");
    }
  }
  return NIL_VAL;
}

// --- write()/println() natives and format() ---
//
// These are first-class global functions (unlike the `print` statement keyword,
// which appends a newline and can't be passed as a value). Both space-join their
// stringified arguments; `println(...)` appends a newline, `write(...)` does not.
// The plan called the no-newline variant `print()`, but the `print` statement
// keyword shadows any global of that name, so it's exposed as `write()` — a
// reachable, callback-friendly primitive (e.g. `arr.forEach(println)`).

static void printSpaceSeparated(int argCount, Value* args) {
  for (int i = 0; i < argCount; i++) {
    if (i > 0) fputc(' ', stdout);
    ObjString* s = stringify(args[i]);
    fwrite(s->chars, 1, (size_t)s->length, stdout);
  }
}

static Value writeNative(int argCount, Value* args) {
  printSpaceSeparated(argCount, args);
  return NIL_VAL;
}

static Value printlnNative(int argCount, Value* args) {
  printSpaceSeparated(argCount, args);
  fputc('\n', stdout);
  return NIL_VAL;
}

// Small growable text buffer for format() (plain malloc; copied out via
// copyString, so no GC-object ownership is involved).
typedef struct {
  char* data;
  int len;
  int cap;
} FmtBuf;

static void fbInit(FmtBuf* b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void fbFree(FmtBuf* b) { free(b->data); fbInit(b); }

static void fbEnsure(FmtBuf* b, int extra) {
  if (b->len + extra <= b->cap) return;
  int cap = b->cap < 16 ? 16 : b->cap;
  while (cap < b->len + extra) cap *= 2;
  b->data = (char*)realloc(b->data, cap);
  b->cap = cap;
}

static void fbPut(FmtBuf* b, const char* s, int n) {
  fbEnsure(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
}

static void fbPutc(FmtBuf* b, char c) {
  fbEnsure(b, 1);
  b->data[b->len++] = c;
}

static void fbPad(FmtBuf* b, char pad, int n) {
  for (int i = 0; i < n; i++) fbPutc(b, pad);
}

// Emit already-rendered text with width padding / alignment / zero-pad. For
// zero-pad, any leading sign stays in front of the zeros.
static void fbEmitPadded(FmtBuf* b, const char* text, int len,
                         int width, bool leftAlign, bool zeroPad) {
  int pad = width - len;
  if (pad < 0) pad = 0;
  if (leftAlign) {
    fbPut(b, text, len);
    fbPad(b, ' ', pad);
  } else if (zeroPad) {
    int signLen = (len > 0 && (text[0] == '-' || text[0] == '+')) ? 1 : 0;
    fbPut(b, text, signLen);
    fbPad(b, '0', pad);
    fbPut(b, text + signLen, len - signLen);
  } else {
    fbPad(b, ' ', pad);
    fbPut(b, text, len);
  }
}

// Render one placeholder value per its spec. Returns false (after raiseError)
// on a spec/argument mismatch.
static bool formatOne(FmtBuf* b, Value val, const char* spec, int speclen) {
  bool leftAlign = false, zeroPad = false;
  int width = 0, precision = -1;
  char type = '\0';
  int p = 0;

  while (p < speclen && (spec[p] == '-' || spec[p] == '0')) {
    if (spec[p] == '-') leftAlign = true; else zeroPad = true;
    p++;
  }
  while (p < speclen && spec[p] >= '0' && spec[p] <= '9') {
    width = width * 10 + (spec[p] - '0');
    if (width > 4096) {
      raiseError(vm.valueErrorClass, "format: width too large.");
      return false;
    }
    p++;
  }
  if (p < speclen && spec[p] == '.') {
    p++;
    precision = 0;
    while (p < speclen && spec[p] >= '0' && spec[p] <= '9') {
      precision = precision * 10 + (spec[p] - '0');
      if (precision > 4096) {
        raiseError(vm.valueErrorClass, "format: precision too large.");
        return false;
      }
      p++;
    }
  }
  if (p < speclen) type = spec[p++];
  if (p != speclen) {
    raiseError(vm.valueErrorClass, "format: invalid format spec.");
    return false;
  }

  // With no explicit type, an int formats as decimal (so width/zero-pad apply
  // naturally); everything else stringifies.
  char effType = type;
  if (effType == '\0' && IS_INT(val)) effType = 'd';

  // Integer conversions: d (decimal), x/X (hex), o (octal), b (binary).
  if (effType == 'd' || effType == 'x' || effType == 'X' ||
      effType == 'o' || effType == 'b') {
    if (!IS_INT(val)) {
      raiseError(vm.valueErrorClass,
                 "format: '%c' expects an integer argument.", type);
      return false;
    }
    int64_t n = AS_INT(val);
    char digits[80];
    int dlen;
    if (effType == 'b') {
      uint64_t u = (uint64_t)n;
      char rev[64];
      int rc = 0;
      if (u == 0) rev[rc++] = '0';
      while (u) { rev[rc++] = (char)('0' + (u & 1)); u >>= 1; }
      dlen = 0;
      for (int k = rc - 1; k >= 0; k--) digits[dlen++] = rev[k];
    } else if (effType == 'd') {
      dlen = snprintf(digits, sizeof(digits), "%" PRId64, n);
    } else {
      const char* cf = effType == 'x' ? "%" PRIx64
                     : effType == 'X' ? "%" PRIX64
                     : "%" PRIo64;
      dlen = snprintf(digits, sizeof(digits), cf, (uint64_t)n);
    }
    fbEmitPadded(b, digits, dlen, width, leftAlign, zeroPad);
    return true;
  }

  // Floating-point conversion.
  if (effType == 'f') {
    if (!IS_NUMBER(val)) {
      raiseError(vm.valueErrorClass, "format: 'f' expects a number argument.");
      return false;
    }
    int prec = precision < 0 ? 6 : precision;
    char cf[24];
    snprintf(cf, sizeof(cf), "%%.%df", prec);
    char num[512];
    int nlen = snprintf(num, sizeof(num), cf, AS_DOUBLE_COERCE(val));
    if (nlen < 0) nlen = 0;
    if (nlen > (int)sizeof(num) - 1) nlen = (int)sizeof(num) - 1;
    fbEmitPadded(b, num, nlen, width, leftAlign, zeroPad);
    return true;
  }

  // String / default: stringify, optional precision truncation, width padding.
  if (effType == 's' || effType == '\0') {
    ObjString* s = stringify(val);
    int slen = s->length;
    if (precision >= 0 && precision < slen) slen = precision;
    int pad = width - slen;
    if (pad < 0) pad = 0;
    if (leftAlign) {
      fbPut(b, s->chars, slen);
      fbPad(b, ' ', pad);
    } else {
      fbPad(b, ' ', pad);
      fbPut(b, s->chars, slen);
    }
    return true;
  }

  raiseError(vm.valueErrorClass, "format: unknown format type '%c'.", type);
  return false;
}

static Value formatNative(int argCount, Value* args) {
  if (argCount < 1 || !IS_STRING(args[0])) {
    runtimeError("format() expects a format string as the first argument.");
    return NIL_VAL;
  }
  ObjString* fmt = AS_STRING(args[0]);
  const char* f = fmt->chars;
  int flen = fmt->length;

  FmtBuf b;
  fbInit(&b);
  int argIndex = 1;  // next positional argument (args[0] is the format string)

  for (int i = 0; i < flen; ) {
    char c = f[i];
    if (c == '{') {
      if (i + 1 < flen && f[i + 1] == '{') { fbPutc(&b, '{'); i += 2; continue; }
      int j = i + 1;
      while (j < flen && f[j] != '}') j++;
      if (j >= flen) {
        fbFree(&b);
        raiseError(vm.valueErrorClass,
                   "format: unmatched '{' in format string.");
        return NIL_VAL;
      }
      const char* content = f + i + 1;
      int contentLen = j - (i + 1);
      const char* spec = "";
      int speclen = 0;
      if (contentLen > 0) {
        if (content[0] != ':') {
          fbFree(&b);
          raiseError(vm.valueErrorClass,
                     "format: invalid placeholder '{%.*s}'.",
                     contentLen, content);
          return NIL_VAL;
        }
        spec = content + 1;
        speclen = contentLen - 1;
      }
      if (argIndex >= argCount) {
        fbFree(&b);
        raiseError(vm.valueErrorClass,
                   "format: not enough arguments for format string.");
        return NIL_VAL;
      }
      if (!formatOne(&b, args[argIndex++], spec, speclen)) {
        fbFree(&b);
        return NIL_VAL;  // formatOne raised the pending exception
      }
      i = j + 1;
    } else if (c == '}') {
      if (i + 1 < flen && f[i + 1] == '}') { fbPutc(&b, '}'); i += 2; continue; }
      fbFree(&b);
      raiseError(vm.valueErrorClass, "format: unmatched '}' in format string.");
      return NIL_VAL;
    } else {
      fbPutc(&b, c);
      i++;
    }
  }

  ObjString* result = copyString(b.data ? b.data : "", b.len);
  fbFree(&b);
  return OBJ_VAL(result);
}

static Value toIntNative(int argCount, Value* args) {
  if (argCount != 1) {
    runtimeError("toInt() expects 1 argument.");
    return NIL_VAL;
  }
  Value v = args[0];
  if (IS_INT(v)) return v;
  if (IS_DOUBLE(v)) {
    double d = AS_DOUBLE(v);
    if (!isfinite(d) || d > (double)INT64_MAX || d < (double)INT64_MIN) {
      runtimeError("Cannot convert %g to int.", d);
      return NIL_VAL;
    }
    return INT_VAL((int64_t)d);
  }
  if (IS_BOOL(v)) return INT_VAL(AS_BOOL(v) ? 1 : 0);
  if (IS_NIL(v)) return INT_VAL(0);
  if (IS_STRING(v)) {
    char* end;
    errno = 0;
    long long ll = strtoll(AS_CSTRING(v), &end, 10);
    if (end == AS_CSTRING(v) || *end != '\0' || errno == ERANGE) {
      runtimeError("Cannot convert '%s' to int.", AS_CSTRING(v));
      return NIL_VAL;
    }
    return INT_VAL((int64_t)ll);
  }
  runtimeError("Cannot convert to int.");
  return NIL_VAL;
}

static Value toDoubleNative(int argCount, Value* args) {
  if (argCount != 1) {
    runtimeError("toDouble() expects 1 argument.");
    return NIL_VAL;
  }
  Value v = args[0];
  if (IS_DOUBLE(v)) return v;
  if (IS_INT(v)) return DOUBLE_VAL((double)AS_INT(v));
  if (IS_BOOL(v)) return DOUBLE_VAL(AS_BOOL(v) ? 1.0 : 0.0);
  if (IS_NIL(v)) return DOUBLE_VAL(0.0);
  if (IS_STRING(v)) {
    char* end;
    double d = strtod(AS_CSTRING(v), &end);
    if (end == AS_CSTRING(v) || *end != '\0') {
      runtimeError("Cannot convert '%s' to double.", AS_CSTRING(v));
      return NIL_VAL;
    }
    return DOUBLE_VAL(d);
  }
  if (IS_INSTANCE(v)) {
    Value result;
    if (callMagicUnary(v, vm.magicToNumber, &result)) return result;
  }
  runtimeError("Cannot convert to double.");
  return NIL_VAL;
}

static Value toBoolNative(int argCount, Value* args) {
  if (argCount != 1) {
    runtimeError("toBool() expects 1 argument.");
    return NIL_VAL;
  }
  Value v = args[0];
  if (IS_BOOL(v)) return v;
  if (IS_NIL(v)) return BOOL_VAL(false);
  if (IS_INSTANCE(v)) {
    Value result;
    if (callMagicUnary(v, vm.magicToBool, &result)) return result;
  }
  // Everything else is truthy (numbers, strings, arrays, etc.)
  return BOOL_VAL(true);
}

static Value sqrtNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("sqrt() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(sqrt(AS_DOUBLE_COERCE(args[0])));
}

static Value absNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("abs() expects a number argument.");
    return NIL_VAL;
  }
  if (IS_INT(args[0])) {
    int64_t a = AS_INT(args[0]);
    if (a == INT64_MIN) {
      runtimeError("Integer overflow.");
      return NIL_VAL;
    }
    return INT_VAL(a < 0 ? -a : a);
  }
  return DOUBLE_VAL(fabs(AS_DOUBLE(args[0])));
}

static Value floorNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("floor() expects a number argument.");
    return NIL_VAL;
  }
  if (IS_INT(args[0])) return args[0];
  double d = floor(AS_DOUBLE(args[0]));
  if (!isfinite(d) || d > (double)INT64_MAX || d < (double)INT64_MIN) {
    return DOUBLE_VAL(d);
  }
  return INT_VAL((int64_t)d);
}

static Value ceilNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("ceil() expects a number argument.");
    return NIL_VAL;
  }
  if (IS_INT(args[0])) return args[0];
  double d = ceil(AS_DOUBLE(args[0]));
  if (!isfinite(d) || d > (double)INT64_MAX || d < (double)INT64_MIN) {
    return DOUBLE_VAL(d);
  }
  return INT_VAL((int64_t)d);
}

static Value roundNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("round() expects a number argument.");
    return NIL_VAL;
  }
  if (IS_INT(args[0])) return args[0];
  double d = round(AS_DOUBLE(args[0]));
  if (!isfinite(d) || d > (double)INT64_MAX || d < (double)INT64_MIN) {
    return DOUBLE_VAL(d);
  }
  return INT_VAL((int64_t)d);
}

static Value minNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    runtimeError("min() expects two number arguments.");
    return NIL_VAL;
  }
  if (IS_INT(args[0]) && IS_INT(args[1])) {
    int64_t a = AS_INT(args[0]), b = AS_INT(args[1]);
    return INT_VAL(a < b ? a : b);
  }
  double a = AS_DOUBLE_COERCE(args[0]), b = AS_DOUBLE_COERCE(args[1]);
  return DOUBLE_VAL(a < b ? a : b);
}

static Value maxNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    runtimeError("max() expects two number arguments.");
    return NIL_VAL;
  }
  if (IS_INT(args[0]) && IS_INT(args[1])) {
    int64_t a = AS_INT(args[0]), b = AS_INT(args[1]);
    return INT_VAL(a > b ? a : b);
  }
  double a = AS_DOUBLE_COERCE(args[0]), b = AS_DOUBLE_COERCE(args[1]);
  return DOUBLE_VAL(a > b ? a : b);
}

static Value powNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    runtimeError("pow() expects two number arguments.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(pow(AS_DOUBLE_COERCE(args[0]), AS_DOUBLE_COERCE(args[1])));
}

static Value randomNative(int argCount, Value* args) {
  return DOUBLE_VAL((double)rand() / RAND_MAX);
}

static Value parseNumberNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("parseNumber() expects a string argument.");
    return NIL_VAL;
  }
  const char* str = AS_STRING(args[0])->chars;
  // Try integer first if no '.', 'e', 'E', or 'i'/'n' (for inf/nan).
  bool hasDecimal = false;
  for (const char* p = str; *p; p++) {
    if (*p == '.' || *p == 'e' || *p == 'E') { hasDecimal = true; break; }
  }
  if (!hasDecimal) {
    char* end;
    errno = 0;
    long long ll = strtoll(str, &end, 10);
    if (end != str && *end == '\0' && errno != ERANGE) {
      return INT_VAL((int64_t)ll);
    }
  }
  char* end;
  double result = strtod(str, &end);
  if (end == str || *end != '\0') return NIL_VAL;
  return DOUBLE_VAL(result);
}

// --- Math extension functions ---

static Value sinNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("sin() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(sin(AS_DOUBLE_COERCE(args[0])));
}

static Value cosNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("cos() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(cos(AS_DOUBLE_COERCE(args[0])));
}

static Value tanNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("tan() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(tan(AS_DOUBLE_COERCE(args[0])));
}

static Value asinNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("asin() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(asin(AS_DOUBLE_COERCE(args[0])));
}

static Value acosNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("acos() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(acos(AS_DOUBLE_COERCE(args[0])));
}

static Value atanNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("atan() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(atan(AS_DOUBLE_COERCE(args[0])));
}

static Value atan2Native(int argCount, Value* args) {
  if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    runtimeError("atan2() expects two number arguments.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(atan2(AS_DOUBLE_COERCE(args[0]), AS_DOUBLE_COERCE(args[1])));
}

static Value logNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("log() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(log(AS_DOUBLE_COERCE(args[0])));
}

static Value log10Native(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("log10() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(log10(AS_DOUBLE_COERCE(args[0])));
}

static Value log2Native(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("log2() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(log2(AS_DOUBLE_COERCE(args[0])));
}

static Value expNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("exp() expects a number argument.");
    return NIL_VAL;
  }
  return DOUBLE_VAL(exp(AS_DOUBLE_COERCE(args[0])));
}

static Value clampNative(int argCount, Value* args) {
  if (argCount != 3 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1]) ||
      !IS_NUMBER(args[2])) {
    runtimeError("clamp() expects three number arguments.");
    return NIL_VAL;
  }
  if (IS_INT(args[0]) && IS_INT(args[1]) && IS_INT(args[2])) {
    int64_t val = AS_INT(args[0]);
    int64_t lo = AS_INT(args[1]);
    int64_t hi = AS_INT(args[2]);
    if (val < lo) return INT_VAL(lo);
    if (val > hi) return INT_VAL(hi);
    return INT_VAL(val);
  }
  double val = AS_DOUBLE_COERCE(args[0]);
  double lo = AS_DOUBLE_COERCE(args[1]);
  double hi = AS_DOUBLE_COERCE(args[2]);
  if (val < lo) return DOUBLE_VAL(lo);
  if (val > hi) return DOUBLE_VAL(hi);
  return DOUBLE_VAL(val);
}

static Value lerpNative(int argCount, Value* args) {
  if (argCount != 3 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1]) ||
      !IS_NUMBER(args[2])) {
    runtimeError("lerp() expects three number arguments.");
    return NIL_VAL;
  }
  double a = AS_DOUBLE_COERCE(args[0]);
  double b = AS_DOUBLE_COERCE(args[1]);
  double t = AS_DOUBLE_COERCE(args[2]);
  return DOUBLE_VAL(a + (b - a) * t);
}

static Value signNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("sign() expects a number argument.");
    return NIL_VAL;
  }
  double x = AS_DOUBLE_COERCE(args[0]);
  if (x > 0) return INT_VAL(1);
  if (x < 0) return INT_VAL(-1);
  return INT_VAL(0);
}

// --- Time extension functions ---

static Value timestampNative(int argCount, Value* args) {
  return INT_VAL((int64_t)time(NULL));
}

static Value sleepNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    runtimeError("sleep() expects a number argument (seconds).");
    return NIL_VAL;
  }
  double seconds = AS_DOUBLE_COERCE(args[0]);
  if (seconds < 0) {
    runtimeError("sleep() argument must be non-negative.");
    return NIL_VAL;
  }
#ifdef _WIN32
  Sleep((DWORD)(seconds * 1000));
#else
  struct timespec ts;
  ts.tv_sec = (time_t)seconds;
  ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
  nanosleep(&ts, NULL);
#endif
  return NIL_VAL;
}

static Value datePartsNative(int argCount, Value* args) {
  time_t t;
  if (argCount == 0) {
    t = time(NULL);
  } else if (argCount == 1 && IS_NUMBER(args[0])) {
    t = (time_t)AS_DOUBLE_COERCE(args[0]);
  } else {
    runtimeError("dateParts() expects 0 or 1 number argument.");
    return NIL_VAL;
  }

  struct tm* tm = localtime(&t);
  if (tm == NULL) {
    runtimeError("dateParts() failed to convert timestamp.");
    return NIL_VAL;
  }

  ObjMap* map = newMap();
  push(OBJ_VAL(map)); // GC protect

  ObjString* k;
  k = copyString("year", 4);
  push(OBJ_VAL(k));
  valueTableSet(&map->entries, OBJ_VAL(k), INT_VAL((int64_t)(tm->tm_year + 1900)));
  pop();

  k = copyString("month", 5);
  push(OBJ_VAL(k));
  valueTableSet(&map->entries, OBJ_VAL(k), INT_VAL((int64_t)(tm->tm_mon + 1)));
  pop();

  k = copyString("day", 3);
  push(OBJ_VAL(k));
  valueTableSet(&map->entries, OBJ_VAL(k), INT_VAL((int64_t)tm->tm_mday));
  pop();

  k = copyString("hour", 4);
  push(OBJ_VAL(k));
  valueTableSet(&map->entries, OBJ_VAL(k), INT_VAL((int64_t)tm->tm_hour));
  pop();

  k = copyString("minute", 6);
  push(OBJ_VAL(k));
  valueTableSet(&map->entries, OBJ_VAL(k), INT_VAL((int64_t)tm->tm_min));
  pop();

  k = copyString("second", 6);
  push(OBJ_VAL(k));
  valueTableSet(&map->entries, OBJ_VAL(k), INT_VAL((int64_t)tm->tm_sec));
  pop();

  k = copyString("weekday", 7);
  push(OBJ_VAL(k));
  valueTableSet(&map->entries, OBJ_VAL(k), INT_VAL((int64_t)tm->tm_wday));
  pop();

  pop(); // map GC protection
  return OBJ_VAL(map);
}

static Value formatDateNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_STRING(args[1])) {
    runtimeError("formatDate() expects (timestamp, formatString).");
    return NIL_VAL;
  }

  time_t t = (time_t)AS_DOUBLE_COERCE(args[0]);
  const char* fmt = AS_CSTRING(args[1]);

  struct tm* tm = localtime(&t);
  if (tm == NULL) {
    runtimeError("formatDate() failed to convert timestamp.");
    return NIL_VAL;
  }

  char buffer[256];
  size_t len = strftime(buffer, sizeof(buffer), fmt, tm);
  if (len == 0) {
    runtimeError("formatDate() format produced empty or too-long result.");
    return NIL_VAL;
  }

  return OBJ_VAL(copyString(buffer, (int)len));
}

// --- Set native methods ---

static bool checkSetTypeGuard(ObjSet* set, Value value) {
  if (set->elementType == NULL) return true;
  if (valueMatchesTypeDescriptor(value, set->elementType)) return true;
  ObjString* actual = typeOfValue(value);
  runtimeError("Type error: set expects %s but got %s.",
               set->elementType->name->chars, actual->chars);
  return false;
}

static bool setAdd(Value receiver, int argCount, Value* args,
                   Value* result) {
  ObjSet* set = AS_SET(receiver);
  if (!checkSetTypeGuard(set, args[0])) return false;
  valueTableSet(&set->entries, args[0], BOOL_VAL(true));
  *result = NIL_VAL;
  return true;
}

static bool setRemove(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjSet* set = AS_SET(receiver);
  *result = BOOL_VAL(valueTableDelete(&set->entries, args[0]));
  return true;
}

static bool setContains(Value receiver, int argCount, Value* args,
                        Value* result) {
  ObjSet* set = AS_SET(receiver);
  Value dummy;
  *result = BOOL_VAL(valueTableGet(&set->entries, args[0], &dummy));
  return true;
}

static bool setClear(Value receiver, int argCount, Value* args,
                     Value* result) {
  ObjSet* set = AS_SET(receiver);
  freeValueTable(&set->entries);
  initValueTable(&set->entries);
  *result = NIL_VAL;
  return true;
}

static bool setToArray(Value receiver, int argCount, Value* args,
                       Value* result) {
  ObjSet* set = AS_SET(receiver);
  ObjArray* arr = newArray();
  push(OBJ_VAL(arr)); // GC protect
  for (int i = 0; i < set->entries.capacity; i++) {
    ValueEntry* entry = &set->entries.entries[i];
    if (!entry->occupied) continue;
    writeValueArray(&arr->elements, entry->key);
  }
  pop();
  *result = OBJ_VAL(arr);
  return true;
}

static bool setForEach(Value receiver, int argCount, Value* args,
                       Value* result) {
  ObjSet* set = AS_SET(receiver);
  Value callback = args[0];

  push(receiver);
  push(callback);

  for (int i = 0; i < set->entries.capacity; i++) {
    ObjSet* s = AS_SET(vm.stackTop[-2]);
    ValueEntry* entry = &s->entries.entries[i];
    if (!entry->occupied) continue;

    Value cb = vm.stackTop[-1];
    Value cbArgs[1] = { entry->key };

    Value dummy;
    if (!invokeCallback(cb, 1, cbArgs, &dummy)) {
      vm.stackTop -= 2;
      return false;
    }
  }

  vm.stackTop -= 2;
  *result = NIL_VAL;
  return true;
}

static bool setUnion(Value receiver, int argCount, Value* args,
                     Value* result) {
  if (!IS_SET(args[0])) {
    runtimeError("union() argument must be a set.");
    return false;
  }
  ObjSet* a = AS_SET(receiver);
  ObjSet* b = AS_SET(args[0]);
  ObjSet* out = newSet();

  push(receiver);
  push(args[0]);
  push(OBJ_VAL(out));

  for (int i = 0; i < a->entries.capacity; i++) {
    if (a->entries.entries[i].occupied) {
      out = AS_SET(vm.stackTop[-1]);
      valueTableSet(&out->entries, a->entries.entries[i].key,
                    BOOL_VAL(true));
    }
  }
  b = AS_SET(vm.stackTop[-2]);
  for (int i = 0; i < b->entries.capacity; i++) {
    if (b->entries.entries[i].occupied) {
      out = AS_SET(vm.stackTop[-1]);
      valueTableSet(&out->entries, b->entries.entries[i].key,
                    BOOL_VAL(true));
    }
  }

  *result = vm.stackTop[-1];
  vm.stackTop -= 3;
  return true;
}

static bool setIntersection(Value receiver, int argCount, Value* args,
                            Value* result) {
  if (!IS_SET(args[0])) {
    runtimeError("intersection() argument must be a set.");
    return false;
  }
  ObjSet* a = AS_SET(receiver);
  ObjSet* b = AS_SET(args[0]);
  ObjSet* out = newSet();

  push(receiver);
  push(args[0]);
  push(OBJ_VAL(out));

  for (int i = 0; i < a->entries.capacity; i++) {
    if (a->entries.entries[i].occupied) {
      Value dummy;
      b = AS_SET(vm.stackTop[-2]);
      if (valueTableGet(&b->entries, a->entries.entries[i].key, &dummy)) {
        out = AS_SET(vm.stackTop[-1]);
        valueTableSet(&out->entries, a->entries.entries[i].key,
                      BOOL_VAL(true));
      }
    }
  }

  *result = vm.stackTop[-1];
  vm.stackTop -= 3;
  return true;
}

static bool setDifference(Value receiver, int argCount, Value* args,
                          Value* result) {
  if (!IS_SET(args[0])) {
    runtimeError("difference() argument must be a set.");
    return false;
  }
  ObjSet* a = AS_SET(receiver);
  ObjSet* b = AS_SET(args[0]);
  ObjSet* out = newSet();

  push(receiver);
  push(args[0]);
  push(OBJ_VAL(out));

  for (int i = 0; i < a->entries.capacity; i++) {
    if (a->entries.entries[i].occupied) {
      Value dummy;
      b = AS_SET(vm.stackTop[-2]);
      if (!valueTableGet(&b->entries, a->entries.entries[i].key, &dummy)) {
        out = AS_SET(vm.stackTop[-1]);
        valueTableSet(&out->entries, a->entries.entries[i].key,
                      BOOL_VAL(true));
      }
    }
  }

  *result = vm.stackTop[-1];
  vm.stackTop -= 3;
  return true;
}

// --- OS module functions ---

static Value osEnvNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("env() expects a string argument.");
    return NIL_VAL;
  }
  const char* val = getenv(AS_CSTRING(args[0]));
  if (val == NULL) return NIL_VAL;
  return OBJ_VAL(copyString(val, (int)strlen(val)));
}

static Value osSetEnvNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
    runtimeError("setEnv() expects (name, value) string arguments.");
    return NIL_VAL;
  }
#ifdef _WIN32
  _putenv_s(AS_CSTRING(args[0]), AS_CSTRING(args[1]));
#else
  setenv(AS_CSTRING(args[0]), AS_CSTRING(args[1]), 1);
#endif
  return NIL_VAL;
}

static Value osExitNative(int argCount, Value* args) {
  int code = 0;
  if (argCount == 1) {
    if (!IS_NUMBER(args[0])) {
      runtimeError("exit() expects a number argument.");
      return NIL_VAL;
    }
    code = (int)AS_DOUBLE_COERCE(args[0]);
  } else if (argCount > 1) {
    runtimeError("exit() expects 0 or 1 arguments.");
    return NIL_VAL;
  }
  exit(code);
  return NIL_VAL; // unreachable
}

static Value osPlatformNative(int argCount, Value* args) {
#ifdef _WIN32
  return OBJ_VAL(copyString("windows", 7));
#elif defined(__APPLE__)
  return OBJ_VAL(copyString("macos", 5));
#else
  return OBJ_VAL(copyString("linux", 5));
#endif
}

static Value osExecNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("exec() expects a string argument.");
    return NIL_VAL;
  }

#ifdef _WIN32
  FILE* pipe = _popen(AS_CSTRING(args[0]), "r");
#else
  FILE* pipe = popen(AS_CSTRING(args[0]), "r");
#endif
  if (pipe == NULL) return NIL_VAL;

  char buffer[4096];
  size_t totalLen = 0;
  size_t capacity = 4096;
  char* output = (char*)malloc(capacity);
  if (output == NULL) {
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return NIL_VAL;
  }

  size_t bytesRead;
  while ((bytesRead = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    if (totalLen + bytesRead >= capacity) {
      capacity *= 2;
      char* newOutput = (char*)realloc(output, capacity);
      if (newOutput == NULL) {
        free(output);
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        return NIL_VAL;
      }
      output = newOutput;
    }
    memcpy(output + totalLen, buffer, bytesRead);
    totalLen += bytesRead;
  }

#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif

  // Trim trailing newline.
  if (totalLen > 0 && output[totalLen - 1] == '\n') {
    totalLen--;
    if (totalLen > 0 && output[totalLen - 1] == '\r') totalLen--;
  }

  ObjString* result = copyString(output, (int)totalLen);
  free(output);
  return OBJ_VAL(result);
}

static Value osArgsNative(int argCount, Value* args) {
  ObjArray* arr = newArray();
  push(OBJ_VAL(arr)); // GC protect
  for (int i = 0; i < vm.argc; i++) {
    ObjString* s = copyString(vm.argv[i], (int)strlen(vm.argv[i]));
    push(OBJ_VAL(s));
    writeValueArray(&arr->elements, OBJ_VAL(s));
    pop();
  }
  pop(); // arr
  return OBJ_VAL(arr);
}

// --- os filesystem operations ---

static Value osCwdNative(int argCount, Value* args) {
  char buf[1024];
#ifdef _WIN32
  if (GetCurrentDirectoryA(sizeof(buf), buf) == 0) {
    raiseError(vm.ioErrorClass, "cwd: could not read current directory.");
    return NIL_VAL;
  }
#else
  if (getcwd(buf, sizeof(buf)) == NULL) {
    raiseError(vm.ioErrorClass, "cwd: could not read current directory.");
    return NIL_VAL;
  }
#endif
  return OBJ_VAL(copyString(buf, (int)strlen(buf)));
}

static Value osIsDirNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("isDir() expects a string path argument.");
    return NIL_VAL;
  }
  const char* path = AS_CSTRING(args[0]);
#ifdef _WIN32
  DWORD attr = GetFileAttributesA(path);
  bool result = (attr != INVALID_FILE_ATTRIBUTES) &&
                (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  bool result = (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
#endif
  return BOOL_VAL(result);
}

static Value osIsFileNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("isFile() expects a string path argument.");
    return NIL_VAL;
  }
  const char* path = AS_CSTRING(args[0]);
#ifdef _WIN32
  DWORD attr = GetFileAttributesA(path);
  bool result = (attr != INVALID_FILE_ATTRIBUTES) &&
                !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  bool result = (stat(path, &st) == 0) && S_ISREG(st.st_mode);
#endif
  return BOOL_VAL(result);
}

// Create a single directory. Returns true on success or if it already exists.
static bool osMakeOneDir(const char* path) {
#ifdef _WIN32
  if (CreateDirectoryA(path, NULL)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
#else
  if (mkdir(path, 0777) == 0) return true;
  return errno == EEXIST;
#endif
}

static Value osMkdirNative(int argCount, Value* args) {
  if (argCount < 1 || argCount > 2 || !IS_STRING(args[0])) {
    runtimeError("mkdir() expects (path, recursive?) with a string path.");
    return NIL_VAL;
  }
  bool recursive = (argCount == 2) && !isFalsey(args[1]);
  ObjString* pathStr = AS_STRING(args[0]);
  const char* path = pathStr->chars;

  if (!recursive) {
    if (!osMakeOneDir(path)) {
      raiseError(vm.ioErrorClass, "mkdir: could not create '%s'.", path);
    }
    return NIL_VAL;
  }

  // Recursive: create each parent prefix in turn (like `mkdir -p`).
  int len = pathStr->length;
  char* buf = (char*)malloc(len + 1);
  memcpy(buf, path, len);
  buf[len] = '\0';

  bool ok = true;
  for (int i = 1; i < len && ok; i++) {
    if (buf[i] == '/' || buf[i] == '\\') {
      char saved = buf[i];
      buf[i] = '\0';
      if (buf[0] != '\0') ok = osMakeOneDir(buf);
      buf[i] = saved;
    }
  }
  if (ok) ok = osMakeOneDir(buf);
  free(buf);

  if (!ok) {
    raiseError(vm.ioErrorClass, "mkdir: could not create '%s'.", path);
  }
  return NIL_VAL;
}

static Value osRemoveDirNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("removeDir() expects a string path argument.");
    return NIL_VAL;
  }
  const char* path = AS_CSTRING(args[0]);
#ifdef _WIN32
  if (!RemoveDirectoryA(path)) {
#else
  if (rmdir(path) != 0) {
#endif
    raiseError(vm.ioErrorClass, "removeDir: could not remove '%s'.", path);
  }
  return NIL_VAL;
}

static Value osRenameNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
    runtimeError("rename() expects (from, to) string arguments.");
    return NIL_VAL;
  }
  if (rename(AS_CSTRING(args[0]), AS_CSTRING(args[1])) != 0) {
    raiseError(vm.ioErrorClass, "rename: could not rename '%s' to '%s'.",
               AS_CSTRING(args[0]), AS_CSTRING(args[1]));
  }
  return NIL_VAL;
}

static Value osCopyFileNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
    runtimeError("copyFile() expects (from, to) string arguments.");
    return NIL_VAL;
  }
  const char* from = AS_CSTRING(args[0]);
  const char* to = AS_CSTRING(args[1]);
#ifdef _WIN32
  if (!CopyFileA(from, to, FALSE)) {
    raiseError(vm.ioErrorClass, "copyFile: could not copy '%s' to '%s'.",
               from, to);
  }
#else
  FILE* in = fopen(from, "rb");
  if (in == NULL) {
    raiseError(vm.ioErrorClass, "copyFile: could not open '%s'.", from);
    return NIL_VAL;
  }
  FILE* out = fopen(to, "wb");
  if (out == NULL) {
    fclose(in);
    raiseError(vm.ioErrorClass, "copyFile: could not open '%s'.", to);
    return NIL_VAL;
  }
  char chunk[8192];
  size_t n;
  bool writeErr = false;
  while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) {
    if (fwrite(chunk, 1, n, out) != n) { writeErr = true; break; }
  }
  fclose(in);
  fclose(out);
  if (writeErr) {
    raiseError(vm.ioErrorClass, "copyFile: could not write '%s'.", to);
  }
#endif
  return NIL_VAL;
}

static Value osListDirNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("listDir() expects a string path argument.");
    return NIL_VAL;
  }
  const char* path = AS_CSTRING(args[0]);

#ifdef _WIN32
  int plen = AS_STRING(args[0])->length;
  char* pattern = (char*)malloc(plen + 3);
  memcpy(pattern, path, plen);
  pattern[plen] = '\\';
  pattern[plen + 1] = '*';
  pattern[plen + 2] = '\0';

  WIN32_FIND_DATAA fd;
  HANDLE handle = FindFirstFileA(pattern, &fd);
  free(pattern);
  if (handle == INVALID_HANDLE_VALUE) {
    raiseError(vm.ioErrorClass, "listDir: could not open '%s'.", path);
    return NIL_VAL;
  }

  ObjArray* arr = newArray();
  push(OBJ_VAL(arr)); // GC protect
  do {
    const char* name = fd.cFileName;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    ObjString* s = copyString(name, (int)strlen(name));
    push(OBJ_VAL(s));
    writeValueArray(&arr->elements, OBJ_VAL(s));
    pop();
  } while (FindNextFileA(handle, &fd));
  FindClose(handle);
  pop(); // arr
  return OBJ_VAL(arr);
#else
  DIR* dir = opendir(path);
  if (dir == NULL) {
    raiseError(vm.ioErrorClass, "listDir: could not open '%s'.", path);
    return NIL_VAL;
  }

  ObjArray* arr = newArray();
  push(OBJ_VAL(arr)); // GC protect
  struct dirent* ent;
  while ((ent = readdir(dir)) != NULL) {
    const char* name = ent->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    ObjString* s = copyString(name, (int)strlen(name));
    push(OBJ_VAL(s));
    writeValueArray(&arr->elements, OBJ_VAL(s));
    pop();
  }
  closedir(dir);
  pop(); // arr
  return OBJ_VAL(arr);
#endif
}

// --- Collections module functions ---

static Value collectionsRangeNative(int argCount, Value* args) {
  // Check all args are numbers.
  for (int a = 0; a < argCount; a++) {
    if (!IS_NUMBER(args[a])) {
      runtimeError("range() arguments must be numbers.");
      return NIL_VAL;
    }
  }
  if (argCount < 1 || argCount > 3) {
    runtimeError("range() expects 1 to 3 arguments.");
    return NIL_VAL;
  }

  // Determine if all args are int.
  bool allInt = true;
  for (int a = 0; a < argCount; a++) {
    if (!IS_INT(args[a])) { allInt = false; break; }
  }

  if (allInt) {
    int64_t start = 0, end, step;
    if (argCount == 1) { end = AS_INT(args[0]); }
    else if (argCount == 2) { start = AS_INT(args[0]); end = AS_INT(args[1]); }
    else { start = AS_INT(args[0]); end = AS_INT(args[1]); step = AS_INT(args[2]); }
    if (argCount < 3) step = (start <= end) ? 1 : -1;
    if (step == 0) { runtimeError("range() step cannot be zero."); return NIL_VAL; }
    if ((step > 0 && start > end) || (step < 0 && start < end)) {
      runtimeError("range() step direction conflicts with start/end.");
      return NIL_VAL;
    }
    double countD = ceil((double)(end - start) / (double)step);
    if (countD < 0) countD = 0;
    if (countD > INT_MAX) { runtimeError("range() produces too many elements."); return NIL_VAL; }
    int count = (int)countD;
    ObjArray* arr = newArray();
    push(OBJ_VAL(arr));
    for (int i = 0; i < count; i++) {
      writeValueArray(&arr->elements, INT_VAL(start + (int64_t)i * step));
    }
    pop();
    return OBJ_VAL(arr);
  }

  // Double path.
  double start = 0, end, step;
  bool hasStep = (argCount == 3);
  if (argCount == 1) { end = AS_DOUBLE_COERCE(args[0]); }
  else if (argCount == 2) { start = AS_DOUBLE_COERCE(args[0]); end = AS_DOUBLE_COERCE(args[1]); }
  else { start = AS_DOUBLE_COERCE(args[0]); end = AS_DOUBLE_COERCE(args[1]); step = AS_DOUBLE_COERCE(args[2]); }
  if (!hasStep) step = (start <= end) ? 1.0 : -1.0;
  if (step == 0) { runtimeError("range() step cannot be zero."); return NIL_VAL; }
  if ((step > 0 && start > end) || (step < 0 && start < end)) {
    runtimeError("range() step direction conflicts with start/end.");
    return NIL_VAL;
  }
  double countD = ceil((end - start) / step);
  if (countD < 0) countD = 0;
  if (countD > INT_MAX) { runtimeError("range() produces too many elements."); return NIL_VAL; }
  int count = (int)countD;
  ObjArray* arr = newArray();
  push(OBJ_VAL(arr));
  for (int i = 0; i < count; i++) {
    writeValueArray(&arr->elements, DOUBLE_VAL(start + i * step));
  }
  pop();
  return OBJ_VAL(arr);
}

static Value collectionsZipNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_ARRAY(args[0]) || !IS_ARRAY(args[1])) {
    runtimeError("zip() expects two array arguments.");
    return NIL_VAL;
  }

  ObjArray* a = AS_ARRAY(args[0]);
  ObjArray* b = AS_ARRAY(args[1]);
  int len = a->elements.count < b->elements.count ?
            a->elements.count : b->elements.count;

  ObjArray* result = newArray();
  push(OBJ_VAL(result)); // GC protect

  for (int i = 0; i < len; i++) {
    ObjArray* pair = newArray();
    push(OBJ_VAL(pair)); // GC protect pair
    writeValueArray(&pair->elements, a->elements.values[i]);
    writeValueArray(&pair->elements, b->elements.values[i]);
    result = AS_ARRAY(vm.stackTop[-2]); // re-read after possible GC
    writeValueArray(&result->elements, OBJ_VAL(pair));
    pop(); // pair
  }

  pop(); // result
  return OBJ_VAL(result);
}

static Value collectionsEnumerateNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_ARRAY(args[0])) {
    runtimeError("enumerate() expects an array argument.");
    return NIL_VAL;
  }

  ObjArray* src = AS_ARRAY(args[0]);
  ObjArray* result = newArray();
  push(OBJ_VAL(result)); // GC protect

  for (int i = 0; i < src->elements.count; i++) {
    ObjArray* pair = newArray();
    push(OBJ_VAL(pair)); // GC protect pair
    writeValueArray(&pair->elements, INT_VAL((int64_t)i));
    writeValueArray(&pair->elements, src->elements.values[i]);
    result = AS_ARRAY(vm.stackTop[-2]); // re-read after possible GC
    writeValueArray(&result->elements, OBJ_VAL(pair));
    pop(); // pair
  }

  pop(); // result
  return OBJ_VAL(result);
}

static Value collectionsSortedNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_ARRAY(args[0])) {
    runtimeError("sorted() expects an array argument.");
    return NIL_VAL;
  }

  ObjArray* src = AS_ARRAY(args[0]);
  ObjArray* copy = newArray();
  push(OBJ_VAL(copy)); // GC protect

  // Copy elements.
  for (int i = 0; i < src->elements.count; i++) {
    writeValueArray(&copy->elements, src->elements.values[i]);
  }

  // Validate all elements are sortable primitives.
  for (int i = 0; i < copy->elements.count; i++) {
    if (sortTypeOrdinal(copy->elements.values[i]) >= 4) {
      pop();
      runtimeError("sorted() array contains non-comparable values.");
      return NIL_VAL;
    }
  }

  qsort(copy->elements.values, copy->elements.count,
        sizeof(Value), sortCompare);

  pop();
  return OBJ_VAL(copy);
}

static Value collectionsReversedNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_ARRAY(args[0])) {
    runtimeError("reversed() expects an array argument.");
    return NIL_VAL;
  }

  ObjArray* src = AS_ARRAY(args[0]);
  ObjArray* copy = newArray();
  push(OBJ_VAL(copy)); // GC protect

  for (int i = src->elements.count - 1; i >= 0; i--) {
    writeValueArray(&copy->elements, src->elements.values[i]);
  }

  pop();
  return OBJ_VAL(copy);
}

// --- File handle methods ---

static bool fileRead(Value receiver, int argCount, Value* args,
                     Value* result) {
  ObjFile* file = AS_FILE(receiver);
  if (!file->isOpen) {
    runtimeError("Cannot read from a closed file.");
    return false;
  }

  long current = ftell(file->file);
  fseek(file->file, 0L, SEEK_END);
  long end = ftell(file->file);
  fseek(file->file, current, SEEK_SET);

  size_t remaining = (size_t)(end - current);
  char* buffer = ALLOCATE(char, remaining + 1);
  size_t bytesRead = fread(buffer, 1, remaining, file->file);
  buffer[bytesRead] = '\0';

  *result = OBJ_VAL(takeString(buffer, (int)bytesRead));
  return true;
}

static bool fileReadLine(Value receiver, int argCount, Value* args,
                         Value* result) {
  ObjFile* file = AS_FILE(receiver);
  if (!file->isOpen) {
    runtimeError("Cannot read from a closed file.");
    return false;
  }

  char buffer[4096];
  if (fgets(buffer, sizeof(buffer), file->file) == NULL) {
    *result = NIL_VAL;
    return true;
  }

  // Strip trailing newline.
  int len = (int)strlen(buffer);
  if (len > 0 && buffer[len - 1] == '\n') {
    len--;
    if (len > 0 && buffer[len - 1] == '\r') len--;
  }

  *result = OBJ_VAL(copyString(buffer, len));
  return true;
}

static bool fileWrite(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjFile* file = AS_FILE(receiver);
  if (!file->isOpen) {
    runtimeError("Cannot write to a closed file.");
    return false;
  }
  if (!IS_STRING(args[0])) {
    runtimeError("write() expects a string argument.");
    return false;
  }

  ObjString* str = AS_STRING(args[0]);
  fwrite(str->chars, 1, str->length, file->file);
  fflush(file->file);
  *result = NIL_VAL;
  return true;
}

static bool fileClose(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjFile* file = AS_FILE(receiver);
  if (file->isOpen && file->file != NULL) {
    fclose(file->file);
    file->file = NULL;
    file->isOpen = false;
  }
  *result = NIL_VAL;
  return true;
}

// --- I/O native functions ---

static Value ioInputNative(int argCount, Value* args) {
  if (argCount > 0 && IS_STRING(args[0])) {
    printf("%s", AS_STRING(args[0])->chars);
    fflush(stdout);
  }

  char buffer[4096];
  if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
    return NIL_VAL;
  }

  int len = (int)strlen(buffer);
  if (len > 0 && buffer[len - 1] == '\n') {
    len--;
    if (len > 0 && buffer[len - 1] == '\r') len--;
  }

  return OBJ_VAL(copyString(buffer, len));
}

static Value ioReadFileNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("readFile() expects a string path argument.");
    return NIL_VAL;
  }

  FILE* file = fopen(AS_CSTRING(args[0]), "r");
  if (file == NULL) {
    raiseError(vm.ioErrorClass, "Could not read file '%s'.",
               AS_CSTRING(args[0]));
    return NIL_VAL;
  }

  fseek(file, 0L, SEEK_END);
  long fileSize = ftell(file);
  rewind(file);

  char* buffer = ALLOCATE(char, fileSize + 1);
  size_t bytesRead = fread(buffer, 1, fileSize, file);
  fclose(file);
  buffer[bytesRead] = '\0';

  return OBJ_VAL(takeString(buffer, (int)bytesRead));
}

static Value ioWriteFileNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
    runtimeError("writeFile() expects (path, content) string arguments.");
    return NIL_VAL;
  }

  FILE* file = fopen(AS_CSTRING(args[0]), "w");
  if (file == NULL) {
    runtimeError("Could not open file '%s' for writing.",
                 AS_CSTRING(args[0]));
    return NIL_VAL;
  }

  ObjString* content = AS_STRING(args[1]);
  fwrite(content->chars, 1, content->length, file);
  fclose(file);
  return NIL_VAL;
}

static Value ioAppendFileNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
    runtimeError("appendFile() expects (path, content) string arguments.");
    return NIL_VAL;
  }

  FILE* file = fopen(AS_CSTRING(args[0]), "a");
  if (file == NULL) {
    runtimeError("Could not open file '%s' for appending.",
                 AS_CSTRING(args[0]));
    return NIL_VAL;
  }

  ObjString* content = AS_STRING(args[1]);
  fwrite(content->chars, 1, content->length, file);
  fclose(file);
  return NIL_VAL;
}

static Value ioFileExistsNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("fileExists() expects a string path argument.");
    return NIL_VAL;
  }

  FILE* file = fopen(AS_CSTRING(args[0]), "r");
  if (file != NULL) {
    fclose(file);
    return BOOL_VAL(true);
  }
  return BOOL_VAL(false);
}

static Value ioDeleteFileNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("deleteFile() expects a string path argument.");
    return NIL_VAL;
  }

  return BOOL_VAL(remove(AS_CSTRING(args[0])) == 0);
}

static Value ioOpenNative(int argCount, Value* args) {
  if (argCount != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
    runtimeError("open() expects (path, mode) string arguments.");
    return NIL_VAL;
  }

  ObjString* path = AS_STRING(args[0]);
  ObjString* mode = AS_STRING(args[1]);

  // Validate mode.
  const char* m = mode->chars;
  if (strcmp(m, "r") != 0 && strcmp(m, "w") != 0 && strcmp(m, "a") != 0) {
    runtimeError("open() mode must be \"r\", \"w\", or \"a\".");
    return NIL_VAL;
  }

  FILE* file = fopen(path->chars, m);
  if (file == NULL) {
    runtimeError("Could not open file '%s' with mode '%s'.",
                 path->chars, mode->chars);
    return NIL_VAL;
  }

  push(OBJ_VAL(path));  // GC protect
  push(OBJ_VAL(mode));  // GC protect
  ObjFile* objFile = newFile(file, path, mode);
  pop(); // mode
  pop(); // path
  return OBJ_VAL(objFile);
}

// --- Error module ---

static ObjClass* errorBaseClass = NULL;

static bool errorInit(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjInstance* instance = AS_INSTANCE(receiver);
  ObjString* msgKey = copyString("message", 7);
  push(OBJ_VAL(msgKey));
  tableSet(&instance->fields, msgKey, args[0]);
  pop();
  *result = receiver;
  return true;
}

static bool errorToString(Value receiver, int argCount, Value* args,
                          Value* result) {
  ObjInstance* instance = AS_INSTANCE(receiver);
  ObjString* msgKey = copyString("message", 7);
  push(OBJ_VAL(msgKey));

  const char* typeName = instance->klass->name->chars;
  int typeLen = instance->klass->name->length;
  const char* msg = "";
  int msgLen = 0;
  Value msgVal;
  if (tableGet(&instance->fields, msgKey, &msgVal) && IS_STRING(msgVal)) {
    msg = AS_STRING(msgVal)->chars;
    msgLen = AS_STRING(msgVal)->length;
  }

  int totalLen = typeLen + 2 + msgLen;
  char* buf = ALLOCATE(char, totalLen + 1);
  memcpy(buf, typeName, typeLen);
  buf[typeLen] = ':';
  buf[typeLen + 1] = ' ';
  memcpy(buf + typeLen + 2, msg, msgLen);
  buf[totalLen] = '\0';

  pop(); // msgKey
  *result = OBJ_VAL(takeString(buf, totalLen));
  return true;
}

static ObjClass* createErrorSubclass(const char* name,
                                     ObjClass* superclass) {
  ObjString* nameStr = copyString(name, (int)strlen(name));
  push(OBJ_VAL(nameStr));
  ObjClass* klass = newClass(nameStr);
  push(OBJ_VAL(klass));
  klass->superclass = superclass;
  tableAddAll(&superclass->methods, &klass->methods);
  pop();
  pop();
  return klass;
}

static Value isErrorNative(int argCount, Value* args) {
  if (argCount != 1) {
    runtimeError("isError() expects 1 argument.");
    return NIL_VAL;
  }
  if (!IS_INSTANCE(args[0])) return BOOL_VAL(false);
  ObjClass* klass = AS_INSTANCE(args[0])->klass;
  while (klass != NULL) {
    if (klass == errorBaseClass) return BOOL_VAL(true);
    klass = klass->superclass;
  }
  return BOOL_VAL(false);
}

// --- Registration helpers ---

static Value typeDescriptorNative(int argCount, Value* args) {
  if (argCount != 2) {
    runtimeError("TypeDescriptor() expects 2 arguments: name and validator.");
    return NIL_VAL;
  }
  if (!IS_STRING(args[0])) {
    runtimeError("TypeDescriptor name must be a string.");
    return NIL_VAL;
  }
  if (!IS_CLOSURE(args[1]) && !IS_NATIVE(args[1]) &&
      !IS_BOUND_METHOD(args[1])) {
    runtimeError("TypeDescriptor validator must be a function.");
    return NIL_VAL;
  }
  ObjString* name = AS_STRING(args[0]);
  push(args[0]); push(args[1]);
  ObjTypeDescriptor* desc = newCustomTypeDescriptor(name, args[1]);
  pop(); pop();
  return OBJ_VAL(desc);
}

static ObjTypeDescriptor* resolveTypeArg(Value arg) {
  if (IS_TYPE_DESCRIPTOR(arg)) return AS_TYPE_DESCRIPTOR(arg);
  if (IS_CLASS(arg)) {
    ObjClass* klass = AS_CLASS(arg);
    push(arg); // GC protect
    ObjTypeDescriptor* desc = newTypeDescriptor(
        TYPETAG_CLASS_REF, klass->name, klass);
    pop();
    return desc;
  }
  return NULL;
}

static Value arrayConstructorNative(int argCount, Value* args) {
  if (argCount == 0) return OBJ_VAL(newArray());
  if (argCount == 1) {
    ObjTypeDescriptor* desc = resolveTypeArg(args[0]);
    if (desc == NULL) {
      runtimeError("Array() argument must be a type or class.");
      return NIL_VAL;
    }
    push(OBJ_VAL(desc)); // GC protect desc across newArray()
    ObjArray* array = newArray();
    array->elementType = desc;
    pop(); // desc
    return OBJ_VAL(array);
  }
  runtimeError("Array() expects 0 or 1 arguments.");
  return NIL_VAL;
}

static Value setConstructorNative(int argCount, Value* args) {
  if (argCount == 0) return OBJ_VAL(newSet());
  if (argCount == 1) {
    ObjTypeDescriptor* desc = resolveTypeArg(args[0]);
    if (desc == NULL) {
      runtimeError("Set() argument must be a type or class.");
      return NIL_VAL;
    }
    push(OBJ_VAL(desc));
    ObjSet* set = newSet();
    set->elementType = desc;
    pop();
    return OBJ_VAL(set);
  }
  runtimeError("Set() expects 0 or 1 arguments.");
  return NIL_VAL;
}

static Value mapConstructorNative(int argCount, Value* args) {
  if (argCount == 0) return OBJ_VAL(newMap());
  if (argCount == 2) {
    ObjTypeDescriptor* kDesc = resolveTypeArg(args[0]);
    if (kDesc == NULL) {
      runtimeError("Map() arguments must be types or classes.");
      return NIL_VAL;
    }
    push(OBJ_VAL(kDesc)); // GC protect across second resolveTypeArg + newMap
    ObjTypeDescriptor* vDesc = resolveTypeArg(args[1]);
    if (vDesc == NULL) {
      pop(); // kDesc
      runtimeError("Map() arguments must be types or classes.");
      return NIL_VAL;
    }
    push(OBJ_VAL(vDesc)); // GC protect across newMap
    ObjMap* map = newMap();
    map->keyType = kDesc;
    map->valueType = vDesc;
    pop(); // vDesc
    pop(); // kDesc
    return OBJ_VAL(map);
  }
  runtimeError("Map() expects 0 or 2 arguments.");
  return NIL_VAL;
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

static ObjModule* registerBuiltinModule(const char* name) {
  ObjString* nameStr = copyString(name, (int)strlen(name));
  push(OBJ_VAL(nameStr)); // GC protect
  ObjString* pathStr = copyString("<builtin>", 9);
  push(OBJ_VAL(pathStr)); // GC protect
  ObjModule* module = newModule(nameStr, pathStr);
  push(OBJ_VAL(module)); // GC protect
  tableSet(&vm.importCache, nameStr, OBJ_VAL(module));
  pop(); // module
  pop(); // pathStr
  pop(); // nameStr
  return module;
}

static void defineModuleNative(ObjModule* module, const char* name,
                               NativeFn function) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNative(function)));
  tableSet(&module->values, AS_STRING(vm.stack[0]), vm.stack[1]);
  pop();
  pop();
}

static void defineModuleConstant(ObjModule* module, const char* name,
                                 Value value) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(value);
  tableSet(&module->values, AS_STRING(vm.stackTop[-2]), vm.stackTop[-1]);
  pop();
  pop();
}

static void defineModuleValue(ObjModule* module, const char* name,
                              Value value) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(value);
  tableSet(&module->values, AS_STRING(vm.stackTop[-2]), vm.stackTop[-1]);
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

// --- Public registration function ---

// --- Regex native methods ---

static Value regexCompileNative(int argCount, Value* args) {
  if (argCount < 1 || !IS_STRING(args[0])) {
    runtimeError("regex.compile() expects a string pattern.");
    return NIL_VAL;
  }
  ObjString* pattern = AS_STRING(args[0]);
  int flags = 0;
  if (argCount >= 2 && IS_STRING(args[1])) {
    const char* f = AS_CSTRING(args[1]);
    for (int i = 0; f[i]; i++) {
      if (f[i] == 'i') flags |= REGEX_ICASE;
      if (f[i] == 'm') flags |= REGEX_MULTILINE;
      if (f[i] == 's') flags |= REGEX_DOTALL;
    }
  }
  char errorBuf[256];
  CompiledRegex* compiled =
      regexCompile(pattern->chars, flags, errorBuf, sizeof(errorBuf));
  if (compiled == NULL) {
    runtimeError("Regex compilation error: %s", errorBuf);
    return NIL_VAL;
  }
  push(OBJ_VAL(pattern));
  ObjRegex* regex = newRegex(pattern, flags, compiled);
  pop();
  return OBJ_VAL(regex);
}

static bool regexTestNative(Value receiver, int argCount, Value* args,
                            Value* result) {
  ObjRegex* re = AS_REGEX(receiver);
  if (argCount < 1 || !IS_STRING(args[0])) {
    runtimeError("regex.test() expects a string.");
    return false;
  }
  RegexResult r = regexExec(re->compiled, AS_CSTRING(args[0]), 0);
  *result = BOOL_VAL(r.matched);
  return true;
}

static bool regexMatchNative(Value receiver, int argCount, Value* args,
                             Value* result) {
  ObjRegex* re = AS_REGEX(receiver);
  if (argCount < 1 || !IS_STRING(args[0])) {
    runtimeError("regex.find() expects a string.");
    return false;
  }
  const char* text = AS_CSTRING(args[0]);
  RegexResult r = regexExec(re->compiled, text, 0);
  if (!r.matched) {
    *result = NIL_VAL;
    return true;
  }

  ObjMap* map = newMap();
  push(OBJ_VAL(map));

  ObjString* matchStr =
      copyString(text + r.matchStart, r.matchEnd - r.matchStart);
  push(OBJ_VAL(matchStr));
  valueTableSet(&map->entries,
                OBJ_VAL(copyString("match", 5)), OBJ_VAL(matchStr));
  pop();

  valueTableSet(&map->entries,
                OBJ_VAL(copyString("index", 5)),
                INT_VAL((int64_t)r.matchStart));

  ObjArray* groups = newArray();
  push(OBJ_VAL(groups));
  for (int g = 0; g < r.groupCount; g++) {
    if (r.groupStart[g] >= 0 && r.groupEnd[g] >= 0) {
      ObjString* gs = copyString(text + r.groupStart[g],
                                  r.groupEnd[g] - r.groupStart[g]);
      writeValueArray(&groups->elements, OBJ_VAL(gs));
    } else {
      writeValueArray(&groups->elements, NIL_VAL);
    }
  }
  valueTableSet(&map->entries,
                OBJ_VAL(copyString("groups", 6)), OBJ_VAL(groups));
  pop(); // groups
  pop(); // map
  *result = OBJ_VAL(map);
  return true;
}

static bool regexMatchAllNative(Value receiver, int argCount, Value* args,
                                Value* result) {
  ObjRegex* re = AS_REGEX(receiver);
  if (argCount < 1 || !IS_STRING(args[0])) {
    runtimeError("regex.findAll() expects a string.");
    return false;
  }
  const char* text = AS_CSTRING(args[0]);
  int textLen = (int)strlen(text);

  ObjArray* results = newArray();
  push(OBJ_VAL(results));

  int pos = 0;
  while (pos <= textLen) {
    RegexResult r = regexExec(re->compiled, text, pos);
    if (!r.matched) break;

    ObjMap* map = newMap();
    push(OBJ_VAL(map));

    ObjString* matchStr =
        copyString(text + r.matchStart, r.matchEnd - r.matchStart);
    push(OBJ_VAL(matchStr));
    valueTableSet(&map->entries,
                  OBJ_VAL(copyString("match", 5)), OBJ_VAL(matchStr));
    pop();
    valueTableSet(&map->entries,
                  OBJ_VAL(copyString("index", 5)),
                  INT_VAL((int64_t)r.matchStart));

    ObjArray* groups = newArray();
    push(OBJ_VAL(groups));
    for (int g = 0; g < r.groupCount; g++) {
      if (r.groupStart[g] >= 0 && r.groupEnd[g] >= 0) {
        ObjString* gs = copyString(text + r.groupStart[g],
                                    r.groupEnd[g] - r.groupStart[g]);
        writeValueArray(&groups->elements, OBJ_VAL(gs));
      } else {
        writeValueArray(&groups->elements, NIL_VAL);
      }
    }
    valueTableSet(&map->entries,
                  OBJ_VAL(copyString("groups", 6)), OBJ_VAL(groups));
    pop(); // groups
    pop(); // map

    writeValueArray(&results->elements, OBJ_VAL(map));

    // Advance past this match (at least 1 char to avoid infinite loop).
    pos = r.matchEnd > pos ? r.matchEnd : pos + 1;
  }

  pop(); // results
  *result = OBJ_VAL(results);
  return true;
}

static bool regexReplaceNative(Value receiver, int argCount, Value* args,
                               Value* result) {
  ObjRegex* re = AS_REGEX(receiver);
  if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
    runtimeError("regex.replace() expects two strings.");
    return false;
  }
  const char* text = AS_CSTRING(args[0]);
  const char* replacement = AS_CSTRING(args[1]);
  int repLen = (int)strlen(replacement);

  RegexResult r = regexExec(re->compiled, text, 0);
  if (!r.matched) {
    *result = OBJ_VAL(copyString(text, (int)strlen(text)));
    return true;
  }

  int prefixLen = r.matchStart;
  int suffixLen = (int)strlen(text) - r.matchEnd;
  int totalLen = prefixLen + repLen + suffixLen;

  char* buf = (char*)malloc(totalLen + 1);
  memcpy(buf, text, prefixLen);
  memcpy(buf + prefixLen, replacement, repLen);
  memcpy(buf + prefixLen + repLen, text + r.matchEnd, suffixLen);
  buf[totalLen] = '\0';

  *result = OBJ_VAL(takeString(buf, totalLen));
  return true;
}

static bool regexSplitNative(Value receiver, int argCount, Value* args,
                             Value* result) {
  ObjRegex* re = AS_REGEX(receiver);
  if (argCount < 1 || !IS_STRING(args[0])) {
    runtimeError("regex.split() expects a string.");
    return false;
  }
  const char* text = AS_CSTRING(args[0]);
  int textLen = (int)strlen(text);

  ObjArray* parts = newArray();
  push(OBJ_VAL(parts));

  int pos = 0;
  while (pos <= textLen) {
    RegexResult r = regexExec(re->compiled, text, pos);
    if (!r.matched) {
      ObjString* rest = copyString(text + pos, textLen - pos);
      writeValueArray(&parts->elements, OBJ_VAL(rest));
      break;
    }

    ObjString* before = copyString(text + pos, r.matchStart - pos);
    writeValueArray(&parts->elements, OBJ_VAL(before));

    pos = r.matchEnd > pos ? r.matchEnd : pos + 1;
  }

  pop(); // parts
  *result = OBJ_VAL(parts);
  return true;
}

// --- Iterator native methods ---

static bool collectionIter(Value receiver, int argCount, Value* args,
                           Value* result) {
  push(receiver);
  ObjIterator* iter = newIterator(receiver);
  pop();
  *result = OBJ_VAL(iter);
  return true;
}

static bool iteratorSelf(Value receiver, int argCount, Value* args,
                         Value* result) {
  *result = receiver;
  return true;
}


static Value iterNative(int argCount, Value* args) {
  if (argCount < 1) {
    runtimeError("iter() expects 1 argument.");
    return NIL_VAL;
  }
  Value collection = args[0];
  push(collection);
  ObjIterator* iter = newIterator(collection);
  pop();
  return OBJ_VAL(iter);
}

// --- bytes native methods & constructors ---

static bool bytesSlice(Value receiver, int argCount, Value* args,
                       Value* result) {
  if (!IS_INT(args[0]) || !IS_INT(args[1])) {
    runtimeError("bytes.slice() expects integer start and end.");
    return false;
  }
  ObjBytes* bytes = AS_BYTES(receiver);
  int len = bytes->length;
  int start = (int)AS_INT(args[0]);
  int end = (int)AS_INT(args[1]);
  // Negative indices count from the end, then clamp to [0, len].
  if (start < 0) start += len;
  if (end < 0) end += len;
  if (start < 0) start = 0;
  if (start > len) start = len;
  if (end < 0) end = 0;
  if (end > len) end = len;
  if (start > end) start = end;

  push(receiver);  // GC protect source across newBytes
  ObjBytes* out = newBytes(bytes->bytes + start, end - start);
  pop();
  *result = OBJ_VAL(out);
  return true;
}

static bool bytesIndexOf(Value receiver, int argCount, Value* args,
                         Value* result) {
  if (!IS_INT(args[0])) {
    runtimeError("bytes.indexOf() expects an integer byte value.");
    return false;
  }
  ObjBytes* bytes = AS_BYTES(receiver);
  int64_t target = AS_INT(args[0]);
  if (target >= 0 && target <= 255) {
    for (int i = 0; i < bytes->length; i++) {
      if (bytes->bytes[i] == (uint8_t)target) {
        *result = INT_VAL((int64_t)i);
        return true;
      }
    }
  }
  *result = INT_VAL(-1);
  return true;
}

static bool bytesToArray(Value receiver, int argCount, Value* args,
                         Value* result) {
  ObjBytes* bytes = AS_BYTES(receiver);
  push(receiver);       // GC protect source
  ObjArray* arr = newArray();
  push(OBJ_VAL(arr));   // GC protect array while filling
  for (int i = 0; i < bytes->length; i++) {
    writeValueArray(&arr->elements, INT_VAL((int64_t)bytes->bytes[i]));
  }
  pop();  // arr
  pop();  // receiver
  *result = OBJ_VAL(arr);
  return true;
}

static bool bytesHex(Value receiver, int argCount, Value* args,
                     Value* result) {
  static const char HEX[] = "0123456789abcdef";
  ObjBytes* bytes = AS_BYTES(receiver);
  int n = bytes->length;
  push(receiver);  // GC protect source across ALLOCATE
  char* buf = ALLOCATE(char, n * 2 + 1);
  for (int i = 0; i < n; i++) {
    buf[i * 2] = HEX[bytes->bytes[i] >> 4];
    buf[i * 2 + 1] = HEX[bytes->bytes[i] & 0x0F];
  }
  buf[n * 2] = '\0';
  ObjString* s = takeString(buf, n * 2);
  pop();  // receiver
  *result = OBJ_VAL(s);
  return true;
}

static Value bytesConstructorNative(int argCount, Value* args) {
  if (argCount == 0) return OBJ_VAL(newBytes(NULL, 0));
  if (argCount != 1) {
    runtimeError("Bytes() expects 0 or 1 arguments.");
    return NIL_VAL;
  }

  if (IS_STRING(args[0])) {
    ObjString* s = AS_STRING(args[0]);
    return OBJ_VAL(newBytes((const uint8_t*)s->chars, s->length));
  }

  if (IS_ARRAY(args[0])) {
    ObjArray* arr = AS_ARRAY(args[0]);
    int n = arr->elements.count;
    uint8_t* tmp = (uint8_t*)malloc(n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
      Value e = arr->elements.values[i];
      if (!IS_INT(e)) {
        free(tmp);
        raiseError(vm.typeErrorClass,
                   "Bytes(): array elements must be integers.");
        return NIL_VAL;
      }
      int64_t v = AS_INT(e);
      if (v < 0 || v > 255) {
        free(tmp);
        raiseError(vm.valueErrorClass,
                   "Bytes(): byte value %lld out of range [0, 255].",
                   (long long)v);
        return NIL_VAL;
      }
      tmp[i] = (uint8_t)v;
    }
    ObjBytes* out = newBytes(tmp, n);
    free(tmp);
    return OBJ_VAL(out);
  }

  raiseError(vm.typeErrorClass,
             "Bytes() expects a string or an array of integers.");
  return NIL_VAL;
}

static int bytesHexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static Value bytesFromHexNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("bytesFromHex() expects a string argument.");
    return NIL_VAL;
  }
  ObjString* s = AS_STRING(args[0]);
  if (s->length % 2 != 0) {
    raiseError(vm.valueErrorClass,
               "bytesFromHex(): hex string must have even length.");
    return NIL_VAL;
  }
  int n = s->length / 2;
  uint8_t* tmp = (uint8_t*)malloc(n > 0 ? n : 1);
  for (int i = 0; i < n; i++) {
    int hi = bytesHexDigit(s->chars[i * 2]);
    int lo = bytesHexDigit(s->chars[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      free(tmp);
      raiseError(vm.valueErrorClass, "bytesFromHex(): invalid hex digit.");
      return NIL_VAL;
    }
    tmp[i] = (uint8_t)((hi << 4) | lo);
  }
  ObjBytes* out = newBytes(tmp, n);
  free(tmp);
  return OBJ_VAL(out);
}

void registerBuiltins(void) {
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
  defineNativeMethod(vm.arrayMethods, "sort", arraySort, 0);
  defineNativeMethod(vm.arrayMethods, "reverse", arrayReverse, 0);
  defineNativeMethod(vm.arrayMethods, "join", arrayJoin, 1);
  defineNativeMethod(vm.arrayMethods, "flat", arrayFlat, 0);
  defineNativeMethod(vm.arrayMethods, "indexOf", arrayIndexOf, 1);
  defineNativeMethod(vm.arrayMethods, "__iter__", collectionIter, 0);

  vm.mapMethods = NULL;
  vm.mapMethods = newClass(copyString("Map", 3));
  defineNativeMethod(vm.mapMethods, "containsKey", mapHas, 1);
  defineNativeMethod(vm.mapMethods, "keys", mapKeys, 0);
  defineNativeMethod(vm.mapMethods, "values", mapValues, 0);
  defineNativeMethod(vm.mapMethods, "remove", mapRemove, 1);
  defineNativeMethod(vm.mapMethods, "forEach", mapForEach, 1);
  defineNativeMethod(vm.mapMethods, "map", mapMapMethod, 1);
  defineNativeMethod(vm.mapMethods, "filter", mapFilter, 1);
  defineNativeMethodVar(vm.mapMethods, "reduce", mapReduce, 1, 2);
  defineNativeMethod(vm.mapMethods, "find", mapFind, 1);
  defineNativeMethod(vm.mapMethods, "any", mapAny, 1);
  defineNativeMethod(vm.mapMethods, "all", mapAll, 1);
  defineNativeMethod(vm.mapMethods, "__iter__", collectionIter, 0);

  vm.setMethods = NULL;
  vm.setMethods = newClass(copyString("Set", 3));
  defineNativeMethod(vm.setMethods, "add", setAdd, 1);
  defineNativeMethod(vm.setMethods, "remove", setRemove, 1);
  defineNativeMethod(vm.setMethods, "contains", setContains, 1);
  defineNativeMethod(vm.setMethods, "clear", setClear, 0);
  defineNativeMethod(vm.setMethods, "toArray", setToArray, 0);
  defineNativeMethod(vm.setMethods, "forEach", setForEach, 1);
  defineNativeMethod(vm.setMethods, "union", setUnion, 1);
  defineNativeMethod(vm.setMethods, "intersection", setIntersection, 1);
  defineNativeMethod(vm.setMethods, "difference", setDifference, 1);
  defineNativeMethod(vm.setMethods, "__iter__", collectionIter, 0);

  vm.bytesMethods = NULL;
  vm.bytesMethods = newClass(copyString("Bytes", 5));
  defineNativeMethod(vm.bytesMethods, "slice", bytesSlice, 2);
  defineNativeMethod(vm.bytesMethods, "indexOf", bytesIndexOf, 1);
  defineNativeMethod(vm.bytesMethods, "toArray", bytesToArray, 0);
  defineNativeMethod(vm.bytesMethods, "hex", bytesHex, 0);
  defineNativeMethod(vm.bytesMethods, "__iter__", collectionIter, 0);

  vm.iteratorMethods = NULL;
  vm.iteratorMethods = newClass(copyString("Iterator", 8));
  defineNativeMethod(vm.iteratorMethods, "__iter__", iteratorSelf, 0);

  vm.fileMethods = NULL;
  vm.fileMethods = newClass(copyString("File", 4));
  defineNativeMethod(vm.fileMethods, "read", fileRead, 0);
  defineNativeMethod(vm.fileMethods, "readLine", fileReadLine, 0);
  defineNativeMethod(vm.fileMethods, "write", fileWrite, 1);
  defineNativeMethod(vm.fileMethods, "close", fileClose, 0);

  vm.stringMethods = NULL;
  vm.stringMethods = newClass(copyString("String", 6));
  defineNativeMethod(vm.stringMethods, "substring", stringSubstring, 2);
  defineNativeMethod(vm.stringMethods, "indexOf", stringIndexOf, 1);
  defineNativeMethod(vm.stringMethods, "toUpper", stringToUpper, 0);
  defineNativeMethod(vm.stringMethods, "toLower", stringToLower, 0);
  defineNativeMethod(vm.stringMethods, "split", stringSplit, 1);
  defineNativeMethod(vm.stringMethods, "trim", stringTrim, 0);
  defineNativeMethod(vm.stringMethods, "replace", stringReplace, 2);
  defineNativeMethod(vm.stringMethods, "startsWith", stringStartsWith, 1);
  defineNativeMethod(vm.stringMethods, "endsWith", stringEndsWith, 1);
  defineNativeMethod(vm.stringMethods, "contains", stringContains, 1);
  defineNativeMethod(vm.stringMethods, "repeat", stringRepeat, 1);
  defineNativeMethod(vm.stringMethods, "charAt", stringCharAt, 1);
  defineNativeMethodVar(vm.stringMethods, "padStart", stringPadStart, 1, 2);
  defineNativeMethodVar(vm.stringMethods, "padEnd", stringPadEnd, 1, 2);
  defineNativeMethod(vm.stringMethods, "trimStart", stringTrimStart, 0);
  defineNativeMethod(vm.stringMethods, "trimEnd", stringTrimEnd, 0);

  defineNative("type", typeNative);
  defineNative("assert", assertNative);
  defineNative("write", writeNative);
  defineNative("println", printlnNative);
  defineNative("format", formatNative);
  defineNative("TypeDescriptor", typeDescriptorNative);
  defineNative("Array", arrayConstructorNative);
  defineNative("Map", mapConstructorNative);
  defineNative("Set", setConstructorNative);
  defineNative("Bytes", bytesConstructorNative);
  defineNative("bytesFromHex", bytesFromHexNative);
  defineNative("toString", toStringNative);
  defineNative("toInt", toIntNative);
  defineNative("toDouble", toDoubleNative);
  defineNative("toBool", toBoolNative);
  defineNative("iter", iterNative);

  // Register primitive type descriptors as globals.
  // Note: nil/true/false are keywords so they can't be identifiers.
  {
    const char* typeNames[] = {
      "int", "double", "number", "string", "bool",
      "array", "map", "set", "function"
    };
    int typeLens[] = {3, 6, 6, 6, 4, 5, 3, 3, 8};
    TypeTag typeTags[] = {
      TYPETAG_INT, TYPETAG_DOUBLE, TYPETAG_NUMBER,
      TYPETAG_STRING, TYPETAG_BOOL, TYPETAG_ARRAY,
      TYPETAG_MAP, TYPETAG_SET, TYPETAG_FUNCTION
    };
    int i;
    for (i = 0; i < 9; i++) {
      ObjString* tname = copyString(typeNames[i], typeLens[i]);
      push(OBJ_VAL(tname));
      ObjTypeDescriptor* desc = newTypeDescriptor(typeTags[i], tname, NULL);
      push(OBJ_VAL(desc));
      tableSet(&vm.globals, tname, OBJ_VAL(desc));
      pop(); pop();
    }
  }

  // Built-in 'math' module.
  ObjModule* mathModule = registerBuiltinModule("math");
  defineModuleNative(mathModule, "sqrt", sqrtNative);
  defineModuleNative(mathModule, "abs", absNative);
  defineModuleNative(mathModule, "floor", floorNative);
  defineModuleNative(mathModule, "ceil", ceilNative);
  defineModuleNative(mathModule, "round", roundNative);
  defineModuleNative(mathModule, "min", minNative);
  defineModuleNative(mathModule, "max", maxNative);
  defineModuleNative(mathModule, "pow", powNative);
  defineModuleNative(mathModule, "random", randomNative);
  defineModuleNative(mathModule, "parseNumber", parseNumberNative);
  defineModuleNative(mathModule, "sin", sinNative);
  defineModuleNative(mathModule, "cos", cosNative);
  defineModuleNative(mathModule, "tan", tanNative);
  defineModuleNative(mathModule, "asin", asinNative);
  defineModuleNative(mathModule, "acos", acosNative);
  defineModuleNative(mathModule, "atan", atanNative);
  defineModuleNative(mathModule, "atan2", atan2Native);
  defineModuleNative(mathModule, "log", logNative);
  defineModuleNative(mathModule, "log10", log10Native);
  defineModuleNative(mathModule, "log2", log2Native);
  defineModuleNative(mathModule, "exp", expNative);
  defineModuleNative(mathModule, "clamp", clampNative);
  defineModuleNative(mathModule, "lerp", lerpNative);
  defineModuleNative(mathModule, "sign", signNative);
  defineModuleConstant(mathModule, "PI",
                       DOUBLE_VAL(3.14159265358979323846));
  defineModuleConstant(mathModule, "E",
                       DOUBLE_VAL(2.71828182845904523536));
  defineModuleConstant(mathModule, "INF", DOUBLE_VAL(INFINITY));

  // Built-in 'time' module.
  ObjModule* timeModule = registerBuiltinModule("time");
  defineModuleNative(timeModule, "clock", clockNative);
  defineModuleNative(timeModule, "timestamp", timestampNative);
  defineModuleNative(timeModule, "sleep", sleepNative);
  defineModuleNative(timeModule, "dateParts", datePartsNative);
  defineModuleNative(timeModule, "formatDate", formatDateNative);

  // Built-in 'io' module.
  ObjModule* ioModule = registerBuiltinModule("io");
  defineModuleNative(ioModule, "input", ioInputNative);
  defineModuleNative(ioModule, "readFile", ioReadFileNative);
  defineModuleNative(ioModule, "writeFile", ioWriteFileNative);
  defineModuleNative(ioModule, "appendFile", ioAppendFileNative);
  defineModuleNative(ioModule, "fileExists", ioFileExistsNative);
  defineModuleNative(ioModule, "deleteFile", ioDeleteFileNative);
  defineModuleNative(ioModule, "open", ioOpenNative);

  // Built-in 'os' module.
  ObjModule* osModule = registerBuiltinModule("os");
  defineModuleNative(osModule, "env", osEnvNative);
  defineModuleNative(osModule, "setEnv", osSetEnvNative);
  defineModuleNative(osModule, "exit", osExitNative);
  defineModuleNative(osModule, "platform", osPlatformNative);
  defineModuleNative(osModule, "exec", osExecNative);
  defineModuleNative(osModule, "args", osArgsNative);
  defineModuleNative(osModule, "cwd", osCwdNative);
  defineModuleNative(osModule, "listDir", osListDirNative);
  defineModuleNative(osModule, "mkdir", osMkdirNative);
  defineModuleNative(osModule, "removeDir", osRemoveDirNative);
  defineModuleNative(osModule, "isDir", osIsDirNative);
  defineModuleNative(osModule, "isFile", osIsFileNative);
  defineModuleNative(osModule, "rename", osRenameNative);
  defineModuleNative(osModule, "copyFile", osCopyFileNative);

  // Built-in 'collections' module.
  ObjModule* collectionsModule = registerBuiltinModule("collections");
  defineModuleNative(collectionsModule, "range", collectionsRangeNative);
  defineModuleNative(collectionsModule, "zip", collectionsZipNative);
  defineModuleNative(collectionsModule, "enumerate",
                     collectionsEnumerateNative);
  defineModuleNative(collectionsModule, "sorted", collectionsSortedNative);
  defineModuleNative(collectionsModule, "reversed",
                     collectionsReversedNative);

  // Built-in 'error' module.
  // NOTE: defineNativeMethod uses vm.stack[0] and vm.stack[1] (absolute),
  // so the stack MUST be empty when calling it.
  ObjModule* errorModule = registerBuiltinModule("error");
  errorBaseClass = newClass(copyString("Error", 5));
  defineNativeMethod(errorBaseClass, "init", errorInit, 1);
  defineNativeMethod(errorBaseClass, "__toString__", errorToString, 0);
  defineModuleValue(errorModule, "Error", OBJ_VAL(errorBaseClass));
  vm.errorClass = errorBaseClass;

  {
    ObjClass* typeError = createErrorSubclass("TypeError", errorBaseClass);
    defineModuleValue(errorModule, "TypeError", OBJ_VAL(typeError));
    vm.typeErrorClass = typeError;

    ObjClass* valueError = createErrorSubclass("ValueError", errorBaseClass);
    defineModuleValue(errorModule, "ValueError", OBJ_VAL(valueError));
    vm.valueErrorClass = valueError;

    ObjClass* rangeError = createErrorSubclass("RangeError", errorBaseClass);
    defineModuleValue(errorModule, "RangeError", OBJ_VAL(rangeError));
    vm.rangeErrorClass = rangeError;

    ObjClass* ioError = createErrorSubclass("IOError", errorBaseClass);
    defineModuleValue(errorModule, "IOError", OBJ_VAL(ioError));
    vm.ioErrorClass = ioError;

    ObjClass* assertionError =
        createErrorSubclass("AssertionError", errorBaseClass);
    defineModuleValue(errorModule, "AssertionError",
                      OBJ_VAL(assertionError));
    vm.assertionErrorClass = assertionError;

    vm.stopIterationClass =
        createErrorSubclass("StopIteration", errorBaseClass);
    defineModuleValue(errorModule, "StopIteration",
                      OBJ_VAL(vm.stopIterationClass));

    defineModuleNative(errorModule, "isError", isErrorNative);
  }

  // --- json module ---
  ObjModule* jsonModule = registerBuiltinModule("json");
  defineModuleNative(jsonModule, "stringify", jsonStringifyNative);
  defineModuleNative(jsonModule, "parse", jsonParseNative);

  // --- regex module ---
  ObjModule* regexModule = registerBuiltinModule("regex");
  defineModuleNative(regexModule, "compile", regexCompileNative);

  vm.regexMethods = NULL;
  vm.regexMethods = newClass(copyString("Regex", 5));
  defineNativeMethod(vm.regexMethods, "test", regexTestNative, 1);
  defineNativeMethod(vm.regexMethods, "find", regexMatchNative, 1);
  defineNativeMethod(vm.regexMethods, "findAll", regexMatchAllNative, 1);
  defineNativeMethod(vm.regexMethods, "replace", regexReplaceNative, 2);
  defineNativeMethod(vm.regexMethods, "split", regexSplitNative, 1);

  srand((unsigned int)time(NULL));
}
