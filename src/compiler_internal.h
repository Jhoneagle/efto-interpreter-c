#ifndef clox_compiler_internal_h
#define clox_compiler_internal_h

#include "common.h"
#include "compiler.h"
#include "scanner.h"
#include "chunk.h"

#define MAX_LOOP_JUMPS       256
#define MAX_DESTRUCTURE_VARS 256
#define MAX_SWITCH_CASES     256
#define MAX_MODULE_PATH_LEN  256
#define MAX_IMPORT_NAMES     256
#define MAX_FINALLY_JUMPS    256

typedef struct {
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
} Parser;

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,  // =
  PREC_TERNARY,     // ?:
  PREC_OR,          // or
  PREC_AND,         // and
  PREC_EQUALITY,    // == !=
  PREC_COMPARISON,  // < > <= >=
  PREC_TERM,        // + -
  PREC_FACTOR,      // * /
  PREC_UNARY,       // ! -
  PREC_CALL,        // . ()
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
  ParseFn prefix;
  ParseFn infix;
  Precedence precedence;
} ParseRule;

typedef struct {
  Token name;
  int depth;
  bool isCaptured;
} Local;

typedef struct {
  uint8_t index;
  bool isLocal;
} Upvalue;

typedef enum {
  TYPE_FUNCTION,
  TYPE_INITIALIZER,
  TYPE_METHOD,
  TYPE_SCRIPT,
  TYPE_ARROW
} FunctionType;

typedef struct Compiler {
  struct Compiler* enclosing;
  ObjFunction* function;
  FunctionType type;

  Local locals[UINT8_COUNT];
  int localCount;
  Upvalue upvalues[UINT8_COUNT];
  int scopeDepth;
} Compiler;

typedef struct ClassCompiler {
  struct ClassCompiler* enclosing;
  bool hasSuperclass;
} ClassCompiler;

typedef struct Loop {
  struct Loop* enclosing;
  int scopeDepth;
  int breakJumps[MAX_LOOP_JUMPS];
  int breakCount;
  int continueTarget;
  bool hasContinueTarget;
  int continueJumps[MAX_LOOP_JUMPS];
  int continueCount;
} Loop;

typedef struct FinallyContext {
  struct FinallyContext* enclosing;
  int scopeDepth;
  int enterJumps[MAX_FINALLY_JUMPS];
  int enterJumpCount;
} FinallyContext;

/* Global state — defined in compiler.c */
extern Parser parser;
extern Compiler* current;
extern ClassCompiler* currentClass;
extern Loop* currentLoop;
extern FinallyContext* currentFinally;

/* Functions defined in compiler.c, used by compiler_stmt.c */
Chunk* currentChunk(void);
void errorAt(Token* token, const char* message);
void error(const char* message);
void errorAtCurrent(const char* message);
void advance(void);
void consume(TokenType type, const char* message);
bool check(TokenType type);
bool match(TokenType type);
void emitByte(uint8_t byte);
void emitBytes(uint8_t byte1, uint8_t byte2);
int emitJump(uint8_t instruction);
void emitLoop(int loopStart);
void emitReturn(void);
uint8_t makeConstant(Value value);
void emitConstant(Value value);
void patchJump(int offset);
void initCompiler(Compiler* compiler, FunctionType type);
ObjFunction* endCompiler(void);
void beginScope(void);
void endScope(void);
uint8_t identifierConstant(Token* name);
bool identifiersEqual(Token* a, Token* b);
void addLocal(Token name);
void declareVariable(void);
void declareVariableByName(Token* name);
uint8_t parseVariable(const char* errorMessage);
void markInitialized(void);
void defineVariable(uint8_t global);
void namedVariable(Token name, bool canAssign);
Token syntheticToken(const char* text);
void emitStringConstant(const char* text, int len);
void expression(void);
void block(void);
void parsePrecedence(Precedence precedence);
ParseRule* getRule(TokenType type);
bool isCompoundAssign(TokenType type);
uint8_t compoundOp(TokenType type);
void parseParameterList(void);
void compileFunction(FunctionType type);

/* Functions defined in compiler_stmt.c, used by compiler.c */
void declaration(void);
void statement(void);

#endif
