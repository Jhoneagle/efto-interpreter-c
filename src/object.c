#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "value_table.h"
#include "vm.h"

#define ALLOCATE_OBJ(type, objectType) \
    (type*)allocateObject(sizeof(type), objectType)

static Obj* allocateObject(size_t size, ObjType type) {
  Obj* object = (Obj*)reallocate(NULL, 0, size);
  object->type = type;
  object->isMarked = false;

  object->next = vm.objects;
  vm.objects = object;

  #ifdef DEBUG_LOG_GC
  printf("%p allocate %zu for %d\n", (void*)object, size, type);
  #endif

  return object;
}

ObjArray* newArray() {
  ObjArray* array = ALLOCATE_OBJ(ObjArray, OBJ_ARRAY);
  initValueArray(&array->elements);
  array->elementType = NULL;
  return array;
}

ObjFile* newFile(FILE* file, ObjString* path, ObjString* mode) {
  ObjFile* objFile = ALLOCATE_OBJ(ObjFile, OBJ_FILE);
  objFile->file = file;
  objFile->isOpen = true;
  objFile->path = path;
  objFile->mode = mode;
  return objFile;
}

ObjMap* newMap() {
  ObjMap* map = ALLOCATE_OBJ(ObjMap, OBJ_MAP);
  initValueTable(&map->entries);
  map->keyType = NULL;
  map->valueType = NULL;
  return map;
}

ObjModule* newModule(ObjString* name, ObjString* path) {
  ObjModule* module = ALLOCATE_OBJ(ObjModule, OBJ_MODULE);
  module->name = name;
  module->path = path;
  initTable(&module->values);
  return module;
}

ObjBoundMethod* newBoundMethod(Value receiver, ObjClosure* method) {
  ObjBoundMethod* bound = ALLOCATE_OBJ(ObjBoundMethod, OBJ_BOUND_METHOD);
  bound->receiver = receiver;
  bound->method = method;
  return bound;
}

ObjClass* newClass(ObjString* name) {
  ObjClass* klass = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
  klass->name = name;
  klass->superclass = NULL;
  initTable(&klass->methods);
  return klass;
}

ObjClosure* newClosure(ObjFunction* function) {
  ObjUpvalue** upvalues = ALLOCATE(ObjUpvalue*,
                                   function->upvalueCount);
  for (int i = 0; i < function->upvalueCount; i++) {
    upvalues[i] = NULL;
  }

  ObjClosure* closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);
  closure->function = function;
  closure->upvalues = upvalues;
  closure->upvalueCount = function->upvalueCount;
  closure->globals = &vm.globals;
  closure->globalsOwner = NULL;
  return closure;
}

ObjFunction* newFunction() {
  ObjFunction* function = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);
  function->arity = 0;
  function->minArity = 0;
  function->upvalueCount = 0;
  function->hasRest = false;
  function->name = NULL;
  initChunk(&function->chunk);
  return function;
}

ObjInstance* newInstance(ObjClass* klass) {
  ObjInstance* instance = ALLOCATE_OBJ(ObjInstance, OBJ_INSTANCE);
  instance->klass = klass;
  initTable(&instance->fields);
  return instance;
}

ObjNative* newNative(NativeFn function) {
  ObjNative* native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
  native->function = function;
  return native;
}

ObjNativeMethod* newNativeMethod(NativeMethodFn function, int arity) {
  ObjNativeMethod* method = ALLOCATE_OBJ(ObjNativeMethod,
                                          OBJ_NATIVE_METHOD);
  method->function = function;
  method->arity = arity;
  method->minArity = arity;
  return method;
}

ObjTypeDescriptor* newTypeDescriptor(TypeTag tag, ObjString* name,
                                     ObjClass* classRef) {
  ObjTypeDescriptor* desc = ALLOCATE_OBJ(ObjTypeDescriptor,
                                          OBJ_TYPE_DESCRIPTOR);
  desc->tag = tag;
  desc->name = name;
  desc->classRef = classRef;
  desc->validatorFn = NIL_VAL;
  return desc;
}

ObjTypeDescriptor* newCustomTypeDescriptor(ObjString* name,
                                           Value validatorFn) {
  ObjTypeDescriptor* desc = ALLOCATE_OBJ(ObjTypeDescriptor,
                                          OBJ_TYPE_DESCRIPTOR);
  desc->tag = TYPETAG_CUSTOM;
  desc->name = name;
  desc->classRef = NULL;
  desc->validatorFn = validatorFn;
  return desc;
}

ObjUpvalue* newUpvalue(Value* slot) {
  ObjUpvalue* upvalue = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
  upvalue->closed = NIL_VAL;
  upvalue->location = slot;
  upvalue->next = NULL;
  return upvalue;
}

static ObjString* allocateString(char* chars, int length, uint32_t hash) {
  ObjString* string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
  string->length = length;
  string->chars = chars;
  string->hash = hash;

  push(OBJ_VAL(string));
  tableSet(&vm.strings, string, NIL_VAL);
  pop();
  
  return string;
}

static uint32_t hashString(const char* key, int length) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < length; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 16777619;
  }
  return hash;
}

ObjString* takeString(char* chars, int length) {
  uint32_t hash = hashString(chars, length);

  ObjString* interned = tableFindString(&vm.strings, chars, length, hash);
  if (interned != NULL) {
    FREE_ARRAY(char, chars, length + 1);
    return interned;
  }

  return allocateString(chars, length, hash);
}

ObjString* copyString(const char* chars, int length) {
  uint32_t hash = hashString(chars, length);

  ObjString* interned = tableFindString(&vm.strings, chars, length, hash);
  if (interned != NULL) return interned;

  char* heapChars = ALLOCATE(char, length + 1);
  memcpy(heapChars, chars, length);
  heapChars[length] = '\0';
  return allocateString(heapChars, length, hash);
}

static void printFunction(ObjFunction* function) {
  if (function->name == NULL) {
    printf("<script>");
    return;
  }

  printf("<fn %s>", function->name->chars);
}

void printObject(Value value) {
  switch (OBJ_TYPE(value)) {
    case OBJ_ARRAY: {
      ObjArray* array = AS_ARRAY(value);
      printf("[");
      for (int i = 0; i < array->elements.count; i++) {
        if (i > 0) printf(", ");
        printValue(array->elements.values[i]);
      }
      printf("]");
      break;
    }
    case OBJ_BOUND_METHOD:
      printFunction(AS_BOUND_METHOD(value)->method->function);
      break;
    case OBJ_FILE:
      printf("<file \"%s\">", AS_FILE(value)->path->chars);
      break;
    case OBJ_CLASS:
      printf("%s", AS_CLASS(value)->name->chars);
      break;
    case OBJ_CLOSURE:
      printFunction(AS_CLOSURE(value)->function);
      break;
    case OBJ_FUNCTION:
      printFunction(AS_FUNCTION(value));
      break;
    case OBJ_INSTANCE: {
      ObjString* str = stringify(value);
      printf("%s", str->chars);
      break;
    }
    case OBJ_MODULE:
      printf("<module %s>", AS_MODULE(value)->name->chars);
      break;
    case OBJ_MAP: {
      ObjMap* map = AS_MAP(value);
      printf("{");
      bool first = true;
      for (int i = 0; i < map->entries.capacity; i++) {
        ValueEntry* entry = &map->entries.entries[i];
        if (!entry->occupied) continue;
        if (!first) printf(", ");
        printValue(entry->key);
        printf(": ");
        printValue(entry->value);
        first = false;
      }
      printf("}");
      break;
    }
    case OBJ_NATIVE:
      printf("<native fn>");
      break;
    case OBJ_NATIVE_METHOD:
      printf("<native method>");
      break;
    case OBJ_TYPE_DESCRIPTOR:
      printf("<type %s>", AS_TYPE_DESCRIPTOR(value)->name->chars);
      break;
    case OBJ_UPVALUE:
      printf("upvalue");
      break;
    case OBJ_STRING:
      printf("%s", AS_CSTRING(value));
      break;
  }
}

bool valueMatchesTypeDescriptor(Value value, ObjTypeDescriptor* desc) {
  switch (desc->tag) {
    case TYPETAG_ANY:      return true;
    case TYPETAG_NUMBER:   return IS_NUMBER(value);
    case TYPETAG_STRING:   return IS_STRING(value);
    case TYPETAG_BOOL:     return IS_BOOL(value);
    case TYPETAG_NIL:      return IS_NIL(value);
    case TYPETAG_ARRAY:    return IS_ARRAY(value);
    case TYPETAG_MAP:      return IS_MAP(value);
    case TYPETAG_FUNCTION:
      return IS_CLOSURE(value) || IS_NATIVE(value)
             || IS_BOUND_METHOD(value);
    case TYPETAG_CLASS_REF: {
      if (!IS_INSTANCE(value)) return false;
      ObjClass* klass = AS_INSTANCE(value)->klass;
      while (klass != NULL) {
        if (klass == desc->classRef) return true;
        klass = klass->superclass;
      }
      return false;
    }
    case TYPETAG_CUSTOM: {
      Value result;
      push(desc->validatorFn);
      push(value);
      if (!vmCallValue(desc->validatorFn, 1, &result)) return false;
      return !IS_NIL(result) && !(IS_BOOL(result) && !AS_BOOL(result));
    }
  }
  return false;
}