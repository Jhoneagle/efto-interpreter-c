#ifndef clox_vm_h
#define clox_vm_h

#include "chunk.h"
#include "object.h"
#include "table.h"
#include "value.h"

#define FRAMES_MAX 64
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)
#define EXCEPTION_HANDLER_MAX 64

typedef struct {
  ObjClosure* closure;
  uint8_t* ip;
  Value* slots;
  int argCount;
} CallFrame;

typedef struct {
  int frameCount;
  Value* stackTop;
  uint8_t* catchIp;
  CallFrame* frame;
} ExceptionHandler;

typedef struct {
  CallFrame frames[FRAMES_MAX];
  int frameCount;
  Value stack[STACK_MAX];
  Value* stackTop;
  Table globals;
  Table strings;
  ObjString* initString;
  ObjClass* arrayMethods;
  ObjClass* mapMethods;
  ObjClass* stringMethods;
  ObjUpvalue* openUpvalues;
  Table importCache;
  ObjString* searchPaths[8];
  int searchPathCount;
  size_t bytesAllocated;
  size_t nextGC;
  ExceptionHandler exceptionHandlers[EXCEPTION_HANDLER_MAX];
  int exceptionHandlerCount;
  Value currentException;
  Obj* objects;
  int grayCount;
  int grayCapacity;
  Obj** grayStack;
} VM;

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR
} InterpretResult;

extern VM vm;

void initVM();
void freeVM();
InterpretResult interpret(const char* source);
void push(Value value);
Value pop();
char* readFile(const char* path);

#endif