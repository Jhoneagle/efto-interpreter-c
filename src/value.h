#ifndef clox_value_h
#define clox_value_h

#include <string.h>

#include "common.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;

typedef enum {
  VAL_BOOL,
  VAL_NIL,
  VAL_INT,
  VAL_DOUBLE,
  VAL_OBJ,
  // Internal sentinel for an unsupplied optional argument. Lives transiently in
  // a parameter slot until OP_DEFAULT_ARG fills it; never observable by user
  // code and never produced by any literal or expression.
  VAL_MISSING
} ValueType;

typedef struct {
  ValueType type;
  union {
    bool boolean;
    int64_t integer;
    double floating;
    Obj* obj;
  } as;
} Value;

#define IS_BOOL(value)    ((value).type == VAL_BOOL)
#define IS_NIL(value)     ((value).type == VAL_NIL)
#define IS_INT(value)     ((value).type == VAL_INT)
#define IS_DOUBLE(value)  ((value).type == VAL_DOUBLE)
#define IS_NUMBER(value)  ((value).type == VAL_INT || (value).type == VAL_DOUBLE)
#define IS_OBJ(value)     ((value).type == VAL_OBJ)
#define IS_MISSING(value) ((value).type == VAL_MISSING)

#define AS_OBJ(value)     ((value).as.obj)
#define AS_BOOL(value)    ((value).as.boolean)
#define AS_INT(value)     ((value).as.integer)
#define AS_DOUBLE(value)  ((value).as.floating)

#define BOOL_VAL(value)   ((Value){VAL_BOOL, {.boolean = value}})
#define NIL_VAL           ((Value){VAL_NIL, {.integer = 0}})
#define INT_VAL(value)    ((Value){VAL_INT, {.integer = (value)}})
#define DOUBLE_VAL(value) ((Value){VAL_DOUBLE, {.floating = (value)}})
#define OBJ_VAL(object)   ((Value){VAL_OBJ, {.obj = (Obj*)object}})
#define MISSING_VAL       ((Value){VAL_MISSING, {.integer = 0}})

static inline double AS_DOUBLE_COERCE(Value v) {
  return IS_INT(v) ? (double)v.as.integer : v.as.floating;
}

typedef struct {
  int capacity;
  int count;
  Value* values;
} ValueArray;

bool valuesEqual(Value a, Value b);
void initValueArray(ValueArray* array);
void writeValueArray(ValueArray* array, Value value);
void freeValueArray(ValueArray* array);
void printValue(Value value);

#endif
