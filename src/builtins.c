#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "builtins.h"
#include "common.h"
#include "memory.h"
#include "object.h"
#include "value_table.h"
#include "vm.h"

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
      double da = AS_NUMBER(va), db = AS_NUMBER(vb);
      return (da < db) ? -1 : (da > db) ? 1 : 0;
    }
    case 3:
      return strcmp(AS_CSTRING(va), AS_CSTRING(vb));
    default:
      return 0; // should not be reached
  }
}

static bool arraySort(Value receiver, int argCount, Value* args,
                      Value* result) {
  ObjArray* array = AS_ARRAY(receiver);

  // Validate all elements are sortable primitives.
  for (int i = 0; i < array->elements.count; i++) {
    Value v = array->elements.values[i];
    if (sortTypeOrdinal(v) >= 4) {
      const char* typeName = "unknown";
      if (IS_OBJ(v)) {
        switch (OBJ_TYPE(v)) {
          case OBJ_ARRAY: typeName = "array"; break;
          case OBJ_MAP: typeName = "map"; break;
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
      *result = NUMBER_VAL((double)i);
      return true;
    }
  }
  *result = NUMBER_VAL(-1);
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
  if (!IS_NUMBER(args[0])) {
    runtimeError("repeat argument must be a number.");
    return false;
  }
  int n = (int)AS_NUMBER(args[0]);
  if (n < 0) {
    runtimeError("repeat count must be non-negative.");
    return false;
  }
  if (n == 0 || str->length == 0) {
    *result = OBJ_VAL(copyString("", 0));
    return true;
  }

  int len = str->length;
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
  if (!IS_NUMBER(args[0])) {
    runtimeError("charAt argument must be a number.");
    return false;
  }
  int index = (int)AS_NUMBER(args[0]);
  if (index < 0 || index >= str->length) {
    runtimeError("String index %d out of bounds [0, %d).",
                 index, str->length);
    return false;
  }
  *result = OBJ_VAL(copyString(str->chars + index, 1));
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
    Value cbArgs[2] = { src->elements.values[i], NUMBER_VAL(i) };

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
    Value cbArgs[2] = { src->elements.values[i], NUMBER_VAL(i) };

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
    Value cbArgs[3] = { acc, src->elements.values[i], NUMBER_VAL(i) };

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
    Value cbArgs[2] = { src->elements.values[i], NUMBER_VAL(i) };

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
    Value cbArgs[2] = { src->elements.values[i], NUMBER_VAL(i) };

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
    Value cbArgs[2] = { src->elements.values[i], NUMBER_VAL(i) };

    Value test;
    if (!invokeCallback(cb, passArgs, cbArgs, &test)) {
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
    Value cbArgs[2] = { src->elements.values[i], NUMBER_VAL(i) };

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
    Value cbArgs[2] = { src->elements.values[i], NUMBER_VAL(i) };

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
  if (IS_NUMBER(value)) return copyString("number", 6);

  if (IS_OBJ(value)) {
    switch (OBJ_TYPE(value)) {
      case OBJ_STRING:        return copyString("string", 6);
      case OBJ_ARRAY:         return copyString("array", 5);
      case OBJ_MAP:           return copyString("map", 3);
      case OBJ_CLASS:         return copyString("class", 5);
      case OBJ_FILE:          return copyString("file", 4);
      case OBJ_INSTANCE:      return copyString("instance", 8);
      case OBJ_MODULE:        return copyString("module", 6);
      case OBJ_FUNCTION:
      case OBJ_CLOSURE:
      case OBJ_NATIVE:
      case OBJ_NATIVE_METHOD:
      case OBJ_BOUND_METHOD:  return copyString("function", 8);
      default: break;
    }
  }
  return copyString("unknown", 7);
}

static Value typeNative(int argCount, Value* args) {
  if (argCount == 0) return OBJ_VAL(typeOfValue(NIL_VAL));
  return OBJ_VAL(typeOfValue(args[0]));
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
    runtimeError("Could not read file '%s'.", AS_CSTRING(args[0]));
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

// --- Registration helpers ---

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

  defineNative("type", typeNative);

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
  defineModuleNative(mathModule, "parseInt", parseIntNative);

  // Built-in 'time' module.
  ObjModule* timeModule = registerBuiltinModule("time");
  defineModuleNative(timeModule, "clock", clockNative);

  // Built-in 'io' module.
  ObjModule* ioModule = registerBuiltinModule("io");
  defineModuleNative(ioModule, "input", ioInputNative);
  defineModuleNative(ioModule, "readFile", ioReadFileNative);
  defineModuleNative(ioModule, "writeFile", ioWriteFileNative);
  defineModuleNative(ioModule, "appendFile", ioAppendFileNative);
  defineModuleNative(ioModule, "fileExists", ioFileExistsNative);
  defineModuleNative(ioModule, "deleteFile", ioDeleteFileNative);
  defineModuleNative(ioModule, "open", ioOpenNative);

  srand((unsigned int)time(NULL));
}
