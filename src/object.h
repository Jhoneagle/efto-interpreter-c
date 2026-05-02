#ifndef clox_object_h
#define clox_object_h

#include <stdio.h>

#include "common.h"
#include "chunk.h"
#include "table.h"
#include "value.h"
#include "value_table.h"

#define OBJ_TYPE(value)        (AS_OBJ(value)->type)

#define IS_ARRAY(value)        isObjType(value, OBJ_ARRAY)
#define IS_FILE(value)         isObjType(value, OBJ_FILE)
#define IS_MAP(value)          isObjType(value, OBJ_MAP)
#define IS_MODULE(value)       isObjType(value, OBJ_MODULE)
#define IS_BOUND_METHOD(value) isObjType(value, OBJ_BOUND_METHOD)
#define IS_CLASS(value)        isObjType(value, OBJ_CLASS)
#define IS_CLOSURE(value)      isObjType(value, OBJ_CLOSURE)
#define IS_FUNCTION(value)     isObjType(value, OBJ_FUNCTION)
#define IS_INSTANCE(value)     isObjType(value, OBJ_INSTANCE)
#define IS_NATIVE(value)       isObjType(value, OBJ_NATIVE)
#define IS_NATIVE_METHOD(value) isObjType(value, OBJ_NATIVE_METHOD)
#define IS_STRING(value)       isObjType(value, OBJ_STRING)

#define AS_ARRAY(value)        ((ObjArray*)AS_OBJ(value))
#define AS_FILE(value)         ((ObjFile*)AS_OBJ(value))
#define AS_MAP(value)          ((ObjMap*)AS_OBJ(value))
#define AS_MODULE(value)       ((ObjModule*)AS_OBJ(value))
#define AS_BOUND_METHOD(value) ((ObjBoundMethod*)AS_OBJ(value))
#define AS_CLASS(value)        ((ObjClass*)AS_OBJ(value))
#define AS_CLOSURE(value)      ((ObjClosure*)AS_OBJ(value))
#define AS_FUNCTION(value)     ((ObjFunction*)AS_OBJ(value))
#define AS_INSTANCE(value)     ((ObjInstance*)AS_OBJ(value))
#define AS_NATIVE(value) \
    (((ObjNative*)AS_OBJ(value))->function)
#define AS_NATIVE_METHOD(value) ((ObjNativeMethod*)AS_OBJ(value))
#define AS_STRING(value)       ((ObjString*)AS_OBJ(value))
#define AS_CSTRING(value)      (((ObjString*)AS_OBJ(value))->chars)

typedef enum {
  OBJ_ARRAY,
  OBJ_BOUND_METHOD,
  OBJ_FILE,
  OBJ_MODULE,
  OBJ_CLASS,
  OBJ_CLOSURE,
  OBJ_FUNCTION,
  OBJ_INSTANCE,
  OBJ_MAP,
  OBJ_NATIVE,
  OBJ_NATIVE_METHOD,
  OBJ_UPVALUE,
  OBJ_STRING,
} ObjType;

struct Obj {
  ObjType type;
  bool isMarked;
  struct Obj* next;
};

typedef struct {
  Obj obj;
  ValueArray elements;
} ObjArray;

typedef struct {
  Obj obj;
  ValueTable entries;
} ObjMap;

typedef struct {
  Obj obj;
  FILE* file;
  bool isOpen;
  ObjString* mode;
  ObjString* path;
} ObjFile;

typedef struct {
  Obj obj;
  int arity;
  int minArity;
  int upvalueCount;
  Chunk chunk;
  ObjString* name;
} ObjFunction;

typedef Value (*NativeFn)(int argCount, Value* args);

typedef struct {
  Obj obj;
  NativeFn function;
} ObjNative;

typedef bool (*NativeMethodFn)(Value receiver, int argCount,
                               Value* args, Value* result);

typedef struct {
  Obj obj;
  NativeMethodFn function;
  int arity;
  int minArity;
} ObjNativeMethod;

struct ObjString {
  Obj obj;
  int length;
  char* chars;
  uint32_t hash;
};

typedef struct ObjUpvalue {
  Obj obj;
  Value* location;
  Value closed;
  struct ObjUpvalue* next;
} ObjUpvalue;

typedef struct {
  Obj obj;
  ObjString* name;
  ObjString* path;
  Table values;
} ObjModule;

typedef struct {
  Obj obj;
  ObjFunction* function;
  ObjUpvalue** upvalues;
  int upvalueCount;
  Table* globals;
  Obj* globalsOwner;
} ObjClosure;

typedef struct {
  Obj obj;
  ObjString* name;
  Table methods;
} ObjClass;

typedef struct {
  Obj obj;
  ObjClass* klass;
  Table fields; 
} ObjInstance;

typedef struct {
  Obj obj;
  Value receiver;
  ObjClosure* method;
} ObjBoundMethod;

ObjArray* newArray();
ObjFile* newFile(FILE* file, ObjString* path, ObjString* mode);
ObjMap* newMap();
ObjModule* newModule(ObjString* name, ObjString* path);
ObjBoundMethod* newBoundMethod(Value receiver,
                               ObjClosure* method);
ObjClass* newClass(ObjString* name);
ObjClosure* newClosure(ObjFunction* function);
ObjFunction* newFunction();
ObjInstance* newInstance(ObjClass* klass);
ObjNative* newNative(NativeFn function);
ObjNativeMethod* newNativeMethod(NativeMethodFn function, int arity);
ObjUpvalue* newUpvalue(Value* slot);
ObjString* takeString(char* chars, int length);
ObjString* copyString(const char* chars, int length);
void printObject(Value value);

static inline bool isObjType(Value value, ObjType type) {
  return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

#endif