#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "object.h"
#include "memory.h"
#include "value.h"

void initValueArray(ValueArray* array) {
  array->values = NULL;
  array->capacity = 0;
  array->count = 0;
}

void writeValueArray(ValueArray* array, Value value) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->values = GROW_ARRAY(Value, array->values,
                               oldCapacity, array->capacity);
  }

  array->values[array->count] = value;
  array->count++;
}

void freeValueArray(ValueArray* array) {
  FREE_ARRAY(Value, array->values, array->capacity);
  initValueArray(array);
}

void printValue(Value value) {
  switch (value.type) {
    case VAL_BOOL:
      printf(AS_BOOL(value) ? "true" : "false");
      break;
    case VAL_NIL: printf("nil"); break;
    case VAL_INT: printf("%" PRId64, AS_INT(value)); break;
    case VAL_DOUBLE: printf("%g", AS_DOUBLE(value)); break;
    case VAL_OBJ: printObject(value); break;
  }
}

bool valuesEqual(Value a, Value b) {
  // Cross-type numeric equality: 1 == 1.0 is true.
  if (IS_NUMBER(a) && IS_NUMBER(b)) {
    return AS_DOUBLE_COERCE(a) == AS_DOUBLE_COERCE(b);
  }
  if (a.type != b.type) return false;
  switch (a.type) {
    case VAL_BOOL:   return AS_BOOL(a) == AS_BOOL(b);
    case VAL_NIL:    return true;
    case VAL_OBJ:
      // bytes compare by content (they are not interned like strings).
      if (IS_BYTES(a) && IS_BYTES(b)) {
        ObjBytes* ba = AS_BYTES(a);
        ObjBytes* bb = AS_BYTES(b);
        return ba->length == bb->length &&
               memcmp(ba->bytes, bb->bytes, ba->length) == 0;
      }
      return AS_OBJ(a) == AS_OBJ(b);
    default:         return false;
  }
}
