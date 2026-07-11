#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_internal.h"
#include "memory.h"
#include "vm.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

Parser parser;
Compiler* current = NULL;
ClassCompiler* currentClass = NULL;
Loop* currentLoop = NULL;
FinallyContext* currentFinally = NULL;
Chunk* compilingChunk;

// Set of global names declared `const` (keys only; value is unused). Populated
// during a single compile() pass and consulted by namedVariable() to reject
// reassignment. Global name→constant-index is not 1:1, so we key by the interned
// name string rather than by constant index.
Table constGlobals;

Chunk* currentChunk() {
  return &current->function->chunk;
}

void errorAt(Token* token, const char* message) {
  if (parser.panicMode) return;
  parser.panicMode = true;
  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
    // Nothing.
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
}

void error(const char* message) {
  errorAt(&parser.previous, message);
}

void errorAtCurrent(const char* message) {
  errorAt(&parser.current, message);
}

void advance() {
  parser.previous = parser.current;

  for (;;) {
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR) break;

    errorAtCurrent(parser.current.start);
  }
}

void consume(TokenType type, const char* message) {
  if (parser.current.type == type) {
    advance();
    return;
  }

  errorAtCurrent(message);
}

bool check(TokenType type) {
  return parser.current.type == type;
}

bool match(TokenType type) {
  if (!check(type)) return false;
  advance();
  return true;
}

void emitByte(uint8_t byte) {
  writeChunk(currentChunk(), byte, parser.previous.line);
}

void emitBytes(uint8_t byte1, uint8_t byte2) {
  emitByte(byte1);
  emitByte(byte2);
}

int emitJump(uint8_t instruction) {
  emitByte(instruction);
  emitByte(0xff);
  emitByte(0xff);
  return currentChunk()->count - 2;
}

void emitLoop(int loopStart) {
  emitByte(OP_LOOP);

  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) error("Loop body too large.");

  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}

void emitReturn() {
  if (current->type == TYPE_INITIALIZER) {
    emitBytes(OP_GET_LOCAL, 0);
  } else {
    emitByte(OP_NIL);
  }

  emitByte(OP_RETURN);
}

uint8_t makeConstant(Value value) {
  int constant = addConstant(currentChunk(), value);
  if (constant > UINT8_MAX) {
    error("Too many constants in one chunk.");
    return 0;
  }

  return (uint8_t)constant;
}

void emitConstant(Value value) {
  emitBytes(OP_CONSTANT, makeConstant(value));
}

void patchJump(int offset) {
  // -2 to adjust for the bytecode for the jump offset itself.
  int jump = currentChunk()->count - offset - 2;

  if (jump > UINT16_MAX) {
    error("Too much code to jump over.");
  }

  currentChunk()->code[offset] = (jump >> 8) & 0xff;
  currentChunk()->code[offset + 1] = jump & 0xff;
}

void initCompiler(Compiler* compiler, FunctionType type) {
  compiler->enclosing = current;
  compiler->function = NULL;
  compiler->type = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  compiler->function = newFunction();

  // A nested function must not inherit the enclosing function's loop/finally
  // contexts: its return/break/continue are local to it. Save and reset.
  compiler->enclosingLoop = currentLoop;
  compiler->enclosingFinally = currentFinally;
  currentLoop = NULL;
  currentFinally = NULL;

  current = compiler;

  if (type != TYPE_SCRIPT && type != TYPE_ARROW) {
    current->function->name = copyString(parser.previous.start,
                                         parser.previous.length);
  }

  Local* local = &current->locals[current->localCount++];
  local->depth = 0;
  local->isCaptured = false;
  local->isConst = false;

  if (type != TYPE_FUNCTION && type != TYPE_ARROW) {
    local->name.start = "this";
    local->name.length = 4;
  } else {
    local->name.start = "";
    local->name.length = 0;
  }
}

ObjFunction* endCompiler() {
  emitReturn();
  ObjFunction* function = current->function;

  #ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
    disassembleChunk(currentChunk(), function->name != NULL
        ? function->name->chars : "<script>");
  }
  #endif

  // Restore the enclosing function's loop/finally contexts.
  currentLoop = current->enclosingLoop;
  currentFinally = current->enclosingFinally;

  current = current->enclosing;
  return function;
}

void beginScope() {
  current->scopeDepth++;
}

void endScope() {
  current->scopeDepth--;

  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth >
            current->scopeDepth) {

    if (current->locals[current->localCount - 1].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }

    current->localCount--;
  }
}

// Like endScope(), but preserves the value currently on top of the stack (e.g. a
// match-expression arm's result). The scope's locals sit below that value; we
// copy the result down into the lowest discarded slot (OP_SET_LOCAL does not
// pop), then run the normal per-local pop/close sequence. Because there is one
// extra physical stack entry (the result) above the N tracked locals, emitting N
// pops removes the result-copy plus N-1 locals and leaves the result sitting in
// the lowest slot as the new top of stack.
void endScopeKeepingTop() {
  current->scopeDepth--;

  // Lowest local slot being discarded in this scope.
  int lowest = -1;
  for (int i = current->localCount - 1; i >= 0; i--) {
    if (current->locals[i].depth <= current->scopeDepth) break;
    lowest = i;
  }

  if (lowest == -1) {
    return;  // no locals to discard; the result is already on top
  }

  emitBytes(OP_SET_LOCAL, (uint8_t)lowest);

  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth >
            current->scopeDepth) {
    if (current->locals[current->localCount - 1].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }
    current->localCount--;
  }
}

static char* processEscapes(const char* source, int sourceLen,
                            int* outLen) {
  char* result = (char*)malloc(sourceLen + 1);
  int j = 0;
  for (int i = 0; i < sourceLen; i++) {
    if (source[i] == '\\' && i + 1 < sourceLen) {
      switch (source[i + 1]) {
        case 'n':  result[j++] = '\n'; i++; break;
        case 't':  result[j++] = '\t'; i++; break;
        case 'r':  result[j++] = '\r'; i++; break;
        case '\\': result[j++] = '\\'; i++; break;
        case '"':  result[j++] = '"';  i++; break;
        case '0':  result[j++] = '\0'; i++; break;
        default:   result[j++] = source[i]; break;
      }
    } else {
      result[j++] = source[i];
    }
  }
  result[j] = '\0';
  *outLen = j;
  return result;
}

void emitStringConstant(const char* text, int len) {
  int escapedLen;
  char* escaped = processEscapes(text, len, &escapedLen);
  emitConstant(OBJ_VAL(copyString(escaped, escapedLen)));
  free(escaped);
}

static void binary(bool canAssign) {
  TokenType operatorType = parser.previous.type;
  ParseRule* rule = getRule(operatorType);
  parsePrecedence((Precedence)(rule->precedence + 1));

  switch (operatorType) {
    case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
    case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
    case TOKEN_GREATER:       emitByte(OP_GREATER); break;
    case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
    case TOKEN_LESS:          emitByte(OP_LESS); break;
    case TOKEN_LESS_EQUAL:    emitBytes(OP_GREATER, OP_NOT); break;
    case TOKEN_PLUS:          emitByte(OP_ADD); break;
    case TOKEN_MINUS:         emitByte(OP_SUBTRACT); break;
    case TOKEN_STAR:          emitByte(OP_MULTIPLY); break;
    case TOKEN_SLASH:         emitByte(OP_DIVIDE); break;
    case TOKEN_PERCENT:       emitByte(OP_MODULO); break;
    case TOKEN_IS:            emitByte(OP_CHECK_TYPE); break;
    case TOKEN_AMPERSAND:     emitByte(OP_BITWISE_AND); break;
    case TOKEN_PIPE:          emitByte(OP_BITWISE_OR); break;
    case TOKEN_CARET:         emitByte(OP_BITWISE_XOR); break;
    case TOKEN_LEFT_SHIFT:    emitByte(OP_LEFT_SHIFT); break;
    case TOKEN_RIGHT_SHIFT:   emitByte(OP_RIGHT_SHIFT); break;
    default: return; // Unreachable.
  }
}

static void literal(bool canAssign) {
  switch (parser.previous.type) {
    case TOKEN_FALSE: emitByte(OP_FALSE); break;
    case TOKEN_NIL: emitByte(OP_NIL); break;
    case TOKEN_TRUE: emitByte(OP_TRUE); break;
    default: return; // Unreachable.
  }
}

static bool checkForArrow() {
  // Save scanner state; parser.current/previous are untouched.
  ScannerState saved = saveScannerState();

  // depth starts at 1 for the '(' in parser.previous.
  // We must account for parser.current before scanning further,
  // since the scanner is already positioned AFTER parser.current.
  int depth = 1;

  // Include parser.current in the depth count.
  if (parser.current.type == TOKEN_LEFT_PAREN) depth++;
  else if (parser.current.type == TOKEN_RIGHT_PAREN) {
    depth--;
    if (depth == 0) {
      Token tok = scanToken();
      bool isArrow = (tok.type == TOKEN_ARROW);
      restoreScannerState(saved);
      return isArrow;
    }
  }

  // Scan ahead counting paren depth to find matching ')'.
  Token tok;
  for (;;) {
    tok = scanToken();
    if (tok.type == TOKEN_LEFT_PAREN) depth++;
    else if (tok.type == TOKEN_RIGHT_PAREN) {
      depth--;
      if (depth == 0) break;
    }
    else if (tok.type == TOKEN_EOF) {
      restoreScannerState(saved);
      return false;
    }
  }

  // Check if next token after ')' is '=>'.
  tok = scanToken();
  bool isArrow = (tok.type == TOKEN_ARROW);

  restoreScannerState(saved);
  return isArrow;
}

static void arrowFunction() {
  // parser.previous is '('. parser.current is first token inside parens.
  Compiler compiler;
  initCompiler(&compiler, TYPE_ARROW);
  beginScope();

  parseParameterList();

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(TOKEN_ARROW, "Expect '=>' after parameters.");

  if (match(TOKEN_LEFT_BRACE)) {
    // Block body: explicit return.
    block();
  } else {
    // Expression body: implicit return.
    expression();
    emitByte(OP_RETURN);
  }

  ObjFunction* fn = endCompiler();
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(fn)));

  for (int i = 0; i < fn->upvalueCount; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
    emitByte(compiler.upvalues[i].index);
  }
}

static void parenOrArrow(bool canAssign) {
  // parser.previous is '('.
  if (check(TOKEN_RIGHT_PAREN)) {
    // () => ... — no params arrow.
    arrowFunction();
    return;
  }

  if (checkForArrow()) {
    arrowFunction();
    return;
  }

  // Regular grouping.
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool canAssign) {
  // Check if the literal contains a '.' to distinguish int vs double.
  bool isDouble = false;
  for (int i = 0; i < parser.previous.length; i++) {
    if (parser.previous.start[i] == '.') { isDouble = true; break; }
  }
  if (isDouble) {
    double value = strtod(parser.previous.start, NULL);
    emitConstant(DOUBLE_VAL(value));
  } else {
    errno = 0;
    int64_t value = strtoll(parser.previous.start, NULL, 10);
    if (errno == ERANGE) {
      error("Integer literal out of range.");
      return;
    }
    emitConstant(INT_VAL(value));
  }
}

static void string(bool canAssign) {
  emitStringConstant(parser.previous.start + 1,
                     parser.previous.length - 2);
}

static void interpolation(bool canAssign) {
  // parser.previous is TOKEN_INTERPOLATION.
  // Extract the text part: if starts with '"', skip it (initial part).
  const char* text = parser.previous.start;
  int len = parser.previous.length;
  if (len > 0 && text[0] == '"') {
    text++;
    len--;
  }

  // Emit the leading text as a string constant.
  emitStringConstant(text, len);

  // Parse the interpolated expression.
  expression();
  emitByte(OP_STRINGIFY);
  emitByte(OP_ADD);

  // Handle subsequent interpolation parts.
  while (match(TOKEN_INTERPOLATION)) {
    const char* partText = parser.previous.start;
    int partLen = parser.previous.length;
    if (partLen > 0) {
      emitStringConstant(partText, partLen);
      emitByte(OP_ADD);
    }
    expression();
    emitByte(OP_STRINGIFY);
    emitByte(OP_ADD);
  }

  // Final string part (TOKEN_STRING from string continuation).
  consume(TOKEN_STRING, "Expect end of interpolated string.");
  const char* endText = parser.previous.start;
  int endLen = parser.previous.length;
  // The continuation TOKEN_STRING includes the closing '"'.
  if (endLen > 0 && endText[endLen - 1] == '"') {
    endLen--;
  }
  if (endLen > 0) {
    emitStringConstant(endText, endLen);
    emitByte(OP_ADD);
  }
}

uint8_t identifierConstant(Token* name) {
  return makeConstant(OBJ_VAL(copyString(name->start,
                                         name->length)));
}

void declareConstGlobalName(const char* chars, int length) {
  ObjString* str = copyString(chars, length);
  // Protect the (possibly freshly allocated) name across tableSet's own
  // allocation, which can trigger a GC that would otherwise sweep it.
  push(OBJ_VAL(str));
  tableSet(&constGlobals, str, BOOL_VAL(true));
  pop();
}

void declareConstGlobal(Token* name) {
  declareConstGlobalName(name->start, name->length);
}

static bool isConstGlobal(Token* name) {
  ObjString* str = copyString(name->start, name->length);
  Value unused;
  return tableGet(&constGlobals, str, &unused);
}

bool identifiersEqual(Token* a, Token* b) {
  if (a->length != b->length) return false;
  return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler* compiler, Token* name) {
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local* local = &compiler->locals[i];
    if (identifiersEqual(name, &local->name)) {
      if (local->depth == -1) {
        error("Can't read local variable in its own initializer.");
      }
      return i;
    }
  }

  return -1;
}

static int addUpvalue(Compiler* compiler, uint8_t index,
                      bool isLocal, bool isConst) {
  int upvalueCount = compiler->function->upvalueCount;

  for (int i = 0; i < upvalueCount; i++) {
    Upvalue* upvalue = &compiler->upvalues[i];
    if (upvalue->index == index && upvalue->isLocal == isLocal) {
      return i;
    }
  }

  if (upvalueCount == UINT8_COUNT) {
    error("Too many closure variables in function.");
    return 0;
  }

  compiler->upvalues[upvalueCount].isLocal = isLocal;
  compiler->upvalues[upvalueCount].index = index;
  compiler->upvalues[upvalueCount].isConst = isConst;
  return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Compiler* compiler, Token* name) {
  if (compiler->enclosing == NULL) return -1;

  int local = resolveLocal(compiler->enclosing, name);
  if (local != -1) {
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (uint8_t)local, true,
                      compiler->enclosing->locals[local].isConst);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1) {
    return addUpvalue(compiler, (uint8_t)upvalue, false,
                      compiler->enclosing->upvalues[upvalue].isConst);
  }

  return -1;
}

bool isCompoundAssign(TokenType type) {
  return type == TOKEN_PLUS_EQUAL || type == TOKEN_MINUS_EQUAL ||
         type == TOKEN_STAR_EQUAL || type == TOKEN_SLASH_EQUAL ||
         type == TOKEN_PERCENT_EQUAL ||
         type == TOKEN_AMPERSAND_EQUAL || type == TOKEN_PIPE_EQUAL ||
         type == TOKEN_CARET_EQUAL || type == TOKEN_LEFT_SHIFT_EQUAL ||
         type == TOKEN_RIGHT_SHIFT_EQUAL;
}

uint8_t compoundOp(TokenType type) {
  switch (type) {
    case TOKEN_PLUS_EQUAL:    return OP_ADD;
    case TOKEN_MINUS_EQUAL:   return OP_SUBTRACT;
    case TOKEN_STAR_EQUAL:    return OP_MULTIPLY;
    case TOKEN_SLASH_EQUAL:   return OP_DIVIDE;
    case TOKEN_PERCENT_EQUAL:      return OP_MODULO;
    case TOKEN_AMPERSAND_EQUAL:    return OP_BITWISE_AND;
    case TOKEN_PIPE_EQUAL:         return OP_BITWISE_OR;
    case TOKEN_CARET_EQUAL:        return OP_BITWISE_XOR;
    case TOKEN_LEFT_SHIFT_EQUAL:   return OP_LEFT_SHIFT;
    case TOKEN_RIGHT_SHIFT_EQUAL:  return OP_RIGHT_SHIFT;
    default: return 0;
  }
}

void namedVariable(Token name, bool canAssign) {
  uint8_t getOp, setOp;
  bool isConst = false;
  int arg = resolveLocal(current, &name);
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
    isConst = current->locals[arg].isConst;
  } else if ((arg = resolveUpvalue(current, &name)) != -1) {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
    isConst = current->upvalues[arg].isConst;
  } else {
    arg = identifierConstant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
    isConst = isConstGlobal(&name);
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    if (isConst) error("Cannot assign to const variable.");
    expression();
    emitBytes(setOp, (uint8_t)arg);
  } else if (canAssign && isCompoundAssign(parser.current.type)) {
    if (isConst) error("Cannot assign to const variable.");
    TokenType opType = parser.current.type;
    advance();
    emitBytes(getOp, (uint8_t)arg);
    expression();
    emitByte(compoundOp(opType));
    emitBytes(setOp, (uint8_t)arg);
  } else {
    emitBytes(getOp, (uint8_t)arg);
  }
}

static void variable(bool canAssign) {
  namedVariable(parser.previous, canAssign);
}

static void this_(bool canAssign) {
  if (currentClass == NULL) {
    error("Can't use 'this' outside of a class.");
    return;
  }

  variable(false);
}

Token syntheticToken(const char* text) {
  Token token;
  token.start = text;
  token.length = (int)strlen(text);
  return token;
}

static void unary(bool canAssign) {
  TokenType operatorType = parser.previous.type;

  // Compile the operand.
  parsePrecedence(PREC_UNARY);

  // Emit the operator instruction.
  switch (operatorType) {
    case TOKEN_BANG: emitByte(OP_NOT); break;
    case TOKEN_MINUS: emitByte(OP_NEGATE); break;
    case TOKEN_TILDE: emitByte(OP_BITWISE_NOT); break;
    default: return; // Unreachable.
  }
}

static void typeof_(bool canAssign) {
  parsePrecedence(PREC_UNARY);
  emitByte(OP_TYPEOF);
}

static void and_(bool canAssign) {
  int endJump = emitJump(OP_JUMP_IF_FALSE);

  emitByte(OP_POP);
  parsePrecedence(PREC_AND);

  patchJump(endJump);
}

static void or_(bool canAssign) {
  int elseJump = emitJump(OP_JUMP_IF_FALSE);
  int endJump = emitJump(OP_JUMP);

  patchJump(elseJump);
  emitByte(OP_POP);

  parsePrecedence(PREC_OR);
  patchJump(endJump);
}

static void ternary(bool canAssign) {
  // Condition already compiled as left operand.
  int thenJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  parsePrecedence(PREC_TERNARY);

  int elseJump = emitJump(OP_JUMP);
  patchJump(thenJump);
  emitByte(OP_POP);

  consume(TOKEN_COLON, "Expect ':' in ternary expression.");
  parsePrecedence(PREC_TERNARY);
  patchJump(elseJump);
}

#define ARG_SPREAD_SENTINEL 255

// True if the upcoming tokens form a named argument `identifier :`. Uses a
// two-token lookahead (parser.current is the identifier; peek the one after).
static bool checkNamedArg() {
  if (!check(TOKEN_IDENTIFIER)) return false;
  ScannerState saved = saveScannerState();
  Token next = scanToken();
  restoreScannerState(saved);
  return next.type == TOKEN_COLON;
}

// Compiles a call's argument list. Positional args are emitted first; if
// `allowNamed` is set, trailing `name: value` pairs are emitted as
// (nameConstant, value) and *namedCount receives their count. Returns the
// positional count, or ARG_SPREAD_SENTINEL when the call spreads an array.
static uint8_t argumentList(int* namedCount, bool allowNamed) {
  uint8_t argCount = 0;
  int named = 0;
  bool hasSpread = false;
  bool sawNamed = false;

  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      if (check(TOKEN_DOT_DOT_DOT)) {
        if (sawNamed) {
          error("Spread argument cannot follow a named argument.");
        }
        if (!hasSpread) {
          // Package args so far into an array.
          emitBytes(OP_BUILD_ARRAY, argCount);
          argCount = 0;
          hasSpread = true;
        }
        advance(); // consume ...
        expression();
        emitByte(OP_SPREAD_ARRAY);
      } else if (checkNamedArg()) {
        if (!allowNamed) {
          error("Named arguments are only supported on direct function calls.");
        }
        if (hasSpread) {
          error("Named argument cannot be combined with a spread argument.");
        }
        advance(); // consume the name identifier
        Token nameTok = parser.previous;
        consume(TOKEN_COLON, "Expect ':' after argument name.");
        emitConstant(OBJ_VAL(copyString(nameTok.start, nameTok.length)));
        expression();
        if (named == 254) {
          error("Can't have more than 254 named arguments.");
        }
        named++;
        sawNamed = true;
      } else {
        if (sawNamed) {
          error("Positional argument cannot follow a named argument.");
        }
        expression();
        if (hasSpread) {
          emitByte(OP_ARRAY_APPEND);
        } else {
          if (argCount == 254) {
            error("Can't have more than 254 arguments.");
          }
          argCount++;
        }
      }
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");

  *namedCount = named;
  if (hasSpread) {
    return ARG_SPREAD_SENTINEL;
  }
  return argCount;
}

static void dot(bool canAssign) {
  consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
  uint8_t name = identifierConstant(&parser.previous);

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(OP_SET_PROPERTY, name);
  } else if (canAssign && isCompoundAssign(parser.current.type)) {
    TokenType opType = parser.current.type;
    advance();
    emitBytes(OP_DUP, 0);
    emitBytes(OP_GET_PROPERTY, name);
    expression();
    emitByte(compoundOp(opType));
    emitBytes(OP_SET_PROPERTY, name);
  } else if (match(TOKEN_LEFT_PAREN)) {
    int namedCount = 0;
    uint8_t argCount = argumentList(&namedCount, false);
    if (argCount == ARG_SPREAD_SENTINEL) {
      emitBytes(OP_INVOKE_SPREAD, name);
    } else {
      emitBytes(OP_INVOKE, name);
      emitByte(argCount);
    }
  } else {
    emitBytes(OP_GET_PROPERTY, name);
  }
}

static void super_(bool canAssign) {
  if (currentClass == NULL) {
    error("Can't use 'super' outside of a class.");
  } else if (!currentClass->hasSuperclass) {
    error("Can't use 'super' in a class with no superclass.");
  }

  consume(TOKEN_DOT, "Expect '.' after 'super'.");
  consume(TOKEN_IDENTIFIER, "Expect superclass method name.");
  uint8_t name = identifierConstant(&parser.previous);

  namedVariable(syntheticToken("this"), false);

  if (match(TOKEN_LEFT_PAREN)) {
    int namedCount = 0;
    uint8_t argCount = argumentList(&namedCount, false);
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_SUPER_INVOKE, name);
    emitByte(argCount);
  } else {
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_GET_SUPER, name);
  }
}

static void call(bool canAssign) {
  int namedCount = 0;
  uint8_t argCount = argumentList(&namedCount, true);
  if (argCount == ARG_SPREAD_SENTINEL) {
    emitByte(OP_CALL_SPREAD);
  } else if (namedCount > 0) {
    emitByte(OP_CALL_KW);
    emitByte(argCount);
    emitByte((uint8_t)namedCount);
  } else {
    emitBytes(OP_CALL, argCount);
  }
}

static void arrayLiteral(bool canAssign) {
  uint8_t elementCount = 0;
  bool hasSpread = false;

  if (!check(TOKEN_RIGHT_BRACKET)) {
    do {
      if (check(TOKEN_DOT_DOT_DOT)) {
        if (!hasSpread) {
          // Package elements compiled so far into an array.
          emitBytes(OP_BUILD_ARRAY, elementCount);
          elementCount = 0;
          hasSpread = true;
        }
        advance(); // consume ...
        expression();
        emitByte(OP_SPREAD_ARRAY);
      } else {
        expression();
        if (hasSpread) {
          emitByte(OP_ARRAY_APPEND);
        } else {
          if (elementCount == 255) {
            error("Can't have more than 255 elements in an array literal.");
          }
          elementCount++;
        }
      }
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_BRACKET, "Expect ']' after array elements.");
  if (!hasSpread) {
    emitBytes(OP_BUILD_ARRAY, elementCount);
  }
}

static void mapLiteral(bool canAssign) {
  uint8_t entryCount = 0;

  if (!check(TOKEN_RIGHT_BRACE)) {
    do {
      expression();
      consume(TOKEN_COLON, "Expect ':' after map key.");
      expression();
      if (entryCount == 255) {
        error("Can't have more than 255 entries in a map literal.");
      }
      entryCount++;
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after map entries.");
  emitBytes(OP_BUILD_MAP, entryCount);
}

static void subscript(bool canAssign) {
  expression();
  consume(TOKEN_RIGHT_BRACKET, "Expect ']' after index.");

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitByte(OP_INDEX_SET);
  } else if (canAssign && isCompoundAssign(parser.current.type)) {
    TokenType opType = parser.current.type;
    advance();
    // Stack: [receiver, index]. Duplicate both for GET then SET.
    emitBytes(OP_DUP, 1); // copy receiver
    emitBytes(OP_DUP, 1); // copy index
    emitByte(OP_INDEX_GET);
    expression();
    emitByte(compoundOp(opType));
    emitByte(OP_INDEX_SET);
  } else {
    emitByte(OP_INDEX_GET);
  }
}

ParseRule rules[] = {
  [TOKEN_LEFT_PAREN]    = {parenOrArrow, call, PREC_CALL},
  [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_LEFT_BRACE]    = {mapLiteral, NULL, PREC_NONE},
  [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
  [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
  [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
  [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
  [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
  [TOKEN_PERCENT]       = {NULL,     binary, PREC_FACTOR},
  [TOKEN_BANG]          = {unary,     NULL,   PREC_NONE},
  [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_PLUS_EQUAL]    = {NULL,     NULL,   PREC_NONE},
  [TOKEN_MINUS_EQUAL]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_STAR_EQUAL]    = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SLASH_EQUAL]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_PERCENT_EQUAL] = {NULL,     NULL,   PREC_NONE},
  [TOKEN_AMPERSAND]        = {NULL,  binary, PREC_BITWISE_AND},
  [TOKEN_AMPERSAND_EQUAL]  = {NULL,  NULL,   PREC_NONE},
  [TOKEN_PIPE]             = {NULL,  binary, PREC_BITWISE_OR},
  [TOKEN_PIPE_EQUAL]       = {NULL,  NULL,   PREC_NONE},
  [TOKEN_CARET]            = {NULL,  binary, PREC_BITWISE_XOR},
  [TOKEN_CARET_EQUAL]      = {NULL,  NULL,   PREC_NONE},
  [TOKEN_TILDE]            = {unary, NULL,   PREC_NONE},
  [TOKEN_LEFT_SHIFT]       = {NULL,  binary, PREC_SHIFT},
  [TOKEN_LEFT_SHIFT_EQUAL] = {NULL,  NULL,   PREC_NONE},
  [TOKEN_RIGHT_SHIFT]      = {NULL,  binary, PREC_SHIFT},
  [TOKEN_RIGHT_SHIFT_EQUAL]= {NULL,  NULL,   PREC_NONE},
  [TOKEN_ARROW]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_IDENTIFIER]    = {variable,     NULL,   PREC_NONE},
  [TOKEN_STRING]        = {string,     NULL,   PREC_NONE},
  [TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
  [TOKEN_INTERPOLATION] = {interpolation, NULL, PREC_NONE},
  [TOKEN_AND]           = {NULL,     and_,   PREC_AND},
  [TOKEN_BREAK]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_CONTINUE]      = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FALSE]         = {literal,     NULL,   PREC_NONE},
  [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FUN]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
  [TOKEN_NIL]           = {literal,     NULL,   PREC_NONE},
  [TOKEN_OR]            = {NULL,     or_,    PREC_OR},
  [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SUPER]         = {super_,   NULL,   PREC_NONE},
  [TOKEN_THIS]          = {this_,    NULL,   PREC_NONE},
  [TOKEN_TRUE]          = {literal,     NULL,   PREC_NONE},
  [TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SWITCH]        = {NULL,     NULL,   PREC_NONE},
  [TOKEN_CASE]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_DEFAULT]       = {NULL,     NULL,   PREC_NONE},
  [TOKEN_IN]            = {NULL,     NULL,   PREC_NONE},
  [TOKEN_IMPORT]        = {NULL,     NULL,   PREC_NONE},
  [TOKEN_AS]            = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FROM]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_TRY]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_CATCH]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_THROW]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FINALLY]       = {NULL,     NULL,   PREC_NONE},
  [TOKEN_TYPEOF]        = {typeof_,  NULL,   PREC_NONE},
  [TOKEN_MATCH]         = {matchExpression, NULL, PREC_NONE},
  [TOKEN_IS]            = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_DOT_DOT_DOT]  = {NULL,     NULL,   PREC_NONE},
  [TOKEN_QUESTION]      = {NULL,     ternary, PREC_TERNARY},
  [TOKEN_COLON]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_LEFT_BRACKET]  = {arrayLiteral, subscript, PREC_CALL},
  [TOKEN_RIGHT_BRACKET] = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
};

void parsePrecedence(Precedence precedence) {
  advance();
  ParseFn prefixRule = getRule(parser.previous.type)->prefix;
  if (prefixRule == NULL) {
    error("Expect expression.");
    return;
  }

  bool canAssign = precedence <= PREC_ASSIGNMENT;
  prefixRule(canAssign);

  while (precedence <= getRule(parser.current.type)->precedence) {
    advance();
    ParseFn infixRule = getRule(parser.previous.type)->infix;
    infixRule(canAssign);
  }

  if (canAssign && (match(TOKEN_EQUAL) ||
      match(TOKEN_PLUS_EQUAL) || match(TOKEN_MINUS_EQUAL) ||
      match(TOKEN_STAR_EQUAL) || match(TOKEN_SLASH_EQUAL) ||
      match(TOKEN_PERCENT_EQUAL) ||
      match(TOKEN_AMPERSAND_EQUAL) || match(TOKEN_PIPE_EQUAL) ||
      match(TOKEN_CARET_EQUAL) || match(TOKEN_LEFT_SHIFT_EQUAL) ||
      match(TOKEN_RIGHT_SHIFT_EQUAL))) {
    error("Invalid assignment target.");
  }
}

void addLocal(Token name) {
  if (current->localCount == UINT8_COUNT) {
    error("Too many local variables in function.");
    return;
  }

  Local* local = &current->locals[current->localCount++];
  local->name = name;
  local->depth = -1;
  local->isCaptured = false;
  local->isConst = false;
  // local->depth = current->scopeDepth;
}

void declareVariableByName(Token* name) {
  if (current->scopeDepth == 0) return;

  for (int i = current->localCount - 1; i >= 0; i--) {
    Local* local = &current->locals[i];
    if (local->depth != -1 && local->depth < current->scopeDepth) {
      break;
    }

    if (identifiersEqual(name, &local->name)) {
      error("Already a variable with this name in this scope.");
    }
  }

  addLocal(*name);
}

void declareVariable() {
  declareVariableByName(&parser.previous);
}

uint8_t parseVariable(const char* errorMessage) {
  consume(TOKEN_IDENTIFIER, errorMessage);

  declareVariable();
  if (current->scopeDepth > 0) return 0;

  return identifierConstant(&parser.previous);
}

void markInitialized() {
  if (current->scopeDepth == 0) return;

  current->locals[current->localCount - 1].depth =
      current->scopeDepth;
}

void defineVariable(uint8_t global) {
  if (current->scopeDepth > 0) {
    markInitialized();
    return;
  }

  emitBytes(OP_DEFINE_GLOBAL, global);
}

ParseRule* getRule(TokenType type) {
  return &rules[type];
}

void expression() {
  parsePrecedence(PREC_ASSIGNMENT);
}

void block() {
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    declaration();
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

int parseArrayPatternNames(Token* names, bool* skip, int maxVars) {
  advance(); // consume '['

  int count = 0;
  if (!check(TOKEN_RIGHT_BRACKET)) {
    do {
      if (count >= maxVars) {
        error("Too many variables in destructuring pattern.");
        return count;
      }
      if (check(TOKEN_COMMA) || check(TOKEN_RIGHT_BRACKET)) {
        skip[count] = true;
        names[count] = syntheticToken("");
      } else {
        consume(TOKEN_IDENTIFIER, "Expect variable name.");
        skip[count] = false;
        names[count] = parser.previous;
      }
      count++;
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_BRACKET, "Expect ']' after destructuring pattern.");
  if (count == 0) {
    error("Expect at least one variable in destructuring pattern.");
  }
  return count;
}

int parseMapPatternNames(Token* keys, Token* varNames, int maxVars) {
  advance(); // consume '{'

  int count = 0;
  if (!check(TOKEN_RIGHT_BRACE)) {
    do {
      if (count >= maxVars) {
        error("Too many variables in destructuring pattern.");
        return count;
      }
      consume(TOKEN_IDENTIFIER, "Expect property name.");
      keys[count] = parser.previous;
      varNames[count] = parser.previous;

      if (match(TOKEN_COLON)) {
        consume(TOKEN_IDENTIFIER, "Expect variable name after ':'.");
        varNames[count] = parser.previous;
      }
      count++;
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after destructuring pattern.");
  if (count == 0) {
    error("Expect at least one variable in destructuring pattern.");
  }
  return count;
}

void emitArrayPatternBindings(int srcSlot, Token* names, bool* skip, int count,
                              bool isConst) {
  for (int i = 0; i < count; i++) {
    if (skip[i]) continue;
    emitBytes(OP_GET_LOCAL, (uint8_t)srcSlot);
    emitConstant(INT_VAL(i));
    emitByte(OP_INDEX_GET);
    declareVariableByName(&names[i]);
    markInitialized();
    if (isConst) current->locals[current->localCount - 1].isConst = true;
  }
}

void emitMapPatternBindings(int srcSlot, Token* keys, Token* varNames, int count,
                            bool isConst) {
  for (int i = 0; i < count; i++) {
    emitBytes(OP_GET_LOCAL, (uint8_t)srcSlot);
    emitConstant(OBJ_VAL(copyString(keys[i].start, keys[i].length)));
    emitByte(OP_INDEX_GET);
    declareVariableByName(&varNames[i]);
    markInitialized();
    if (isConst) current->locals[current->localCount - 1].isConst = true;
  }
}

void parseParameterList() {
  bool hasDefault = false;
  // Destructured parameters ([a,b] / {x,y}) bind a synthetic param local that
  // receives the passed argument; the extraction into pattern names is deferred
  // until after every param slot is declared, so it never interleaves with the
  // positional slots the caller fills. Flat arrays hold every pattern's vars.
  Token dNames[MAX_DESTRUCTURE_VARS];
  bool dSkip[MAX_DESTRUCTURE_VARS];
  Token dKeys[MAX_DESTRUCTURE_VARS];
  Token dVarNames[MAX_DESTRUCTURE_VARS];
  int dPatIsMap[MAX_DESTRUCTURE_VARS];
  int dPatSlot[MAX_DESTRUCTURE_VARS];
  int dPatStart[MAX_DESTRUCTURE_VARS];
  int dPatLen[MAX_DESTRUCTURE_VARS];
  int patternCount = 0;
  int varTotal = 0;
  // Parameter names, index-aligned with param slots, recorded to build the
  // function's paramNames table (for keyword arguments). A nameless entry marks
  // a slot that cannot be named (a destructured param's synthetic local).
  Token pNameTok[256];
  bool pNameless[256];
  int pNameCount = 0;
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      // Rest parameter: ...name
      if (match(TOKEN_DOT_DOT_DOT)) {
        current->function->hasRest = true;
        current->function->arity++;
        if (current->function->arity > 255) {
          errorAtCurrent("Can't have more than 255 parameters.");
        }
        uint8_t constant = parseVariable("Expect rest parameter name.");
        if (pNameCount < 256) {
          pNameTok[pNameCount] = parser.previous;
          pNameless[pNameCount] = false;
          pNameCount++;
        }
        defineVariable(constant);
        // Rest must be last parameter.
        break;
      }

      // Destructured parameter: [a, b] or {x, y}. The passed argument lands in
      // a synthetic param local; extraction is deferred (see below).
      if (check(TOKEN_LEFT_BRACKET) || check(TOKEN_LEFT_BRACE)) {
        if (hasDefault) {
          error("Non-default parameter after default parameter.");
        }
        current->function->arity++;
        if (current->function->arity > 255) {
          errorAtCurrent("Can't have more than 255 parameters.");
        }
        addLocal(syntheticToken(""));
        markInitialized();
        bool isMap = check(TOKEN_LEFT_BRACE);
        int start = varTotal;
        int remaining = MAX_DESTRUCTURE_VARS - start;
        int n;
        if (isMap) {
          n = parseMapPatternNames(&dKeys[start], &dVarNames[start], remaining);
        } else {
          n = parseArrayPatternNames(&dNames[start], &dSkip[start], remaining);
        }
        dPatIsMap[patternCount] = isMap;
        dPatSlot[patternCount] = current->localCount - 1;
        dPatStart[patternCount] = start;
        dPatLen[patternCount] = n;
        patternCount++;
        varTotal += n;
        if (pNameCount < 256) {
          pNameTok[pNameCount] = syntheticToken("");
          pNameless[pNameCount] = true; // destructured param has no callable name
          pNameCount++;
        }
        if (check(TOKEN_EQUAL)) {
          error("Destructured parameter can't have a default value.");
        }
        continue;
      }

      current->function->arity++;
      if (current->function->arity > 255) {
        errorAtCurrent("Can't have more than 255 parameters.");
      }
      uint8_t constant = parseVariable("Expect parameter name.");
      if (pNameCount < 256) {
        pNameTok[pNameCount] = parser.previous;
        pNameless[pNameCount] = false;
        pNameCount++;
      }
      defineVariable(constant);

      if (match(TOKEN_EQUAL)) {
        if (!hasDefault) {
          current->function->minArity = current->function->arity - 1;
          hasDefault = true;
        }
        // Emit OP_DEFAULT_ARG <localSlot> <jump placeholder>. The operand is the
        // parameter's local slot; the VM runs the default iff that slot is
        // MISSING (unsupplied), which is what lets keyword calls skip middles.
        int slot = current->localCount - 1;
        emitByte(OP_DEFAULT_ARG);
        emitByte((uint8_t)slot);
        // Emit jump placeholder (2 bytes) using same pattern as emitJump.
        emitByte(0xff);
        emitByte(0xff);
        int jump = currentChunk()->count - 2;

        // Compile the default expression.
        expression();
        emitBytes(OP_SET_LOCAL, (uint8_t)slot);
        emitByte(OP_POP);

        // Patch the jump to skip over the default expression.
        patchJump(jump);
      } else if (hasDefault) {
        error("Non-default parameter after default parameter.");
      }
    } while (match(TOKEN_COMMA));
  }

  if (current->function->hasRest) {
    if (!hasDefault) {
      current->function->minArity = current->function->arity - 1;
    }
  } else if (!hasDefault) {
    current->function->minArity = current->function->arity;
  }

  // Now that every parameter (and the rest param) owns its slot, extract each
  // destructured param's bindings. Declaring them here places the extraction
  // locals after all param slots, so they never collide with positional args.
  for (int p = 0; p < patternCount; p++) {
    int start = dPatStart[p];
    if (dPatIsMap[p]) {
      emitMapPatternBindings(dPatSlot[p], &dKeys[start], &dVarNames[start],
                             dPatLen[p], false);
    } else {
      emitArrayPatternBindings(dPatSlot[p], &dNames[start], &dSkip[start],
                               dPatLen[p], false);
    }
  }

  // Record parameter names for keyword-argument resolution. The array is
  // assigned before the copyString loop and NULL-initialized so a GC triggered
  // mid-build (via copyString) sees only rooted / NULL entries.
  if (pNameCount > 0) {
    ObjFunction* fn = current->function;
    fn->paramNames = ALLOCATE(ObjString*, pNameCount);
    fn->paramCount = pNameCount;
    for (int i = 0; i < pNameCount; i++) fn->paramNames[i] = NULL;
    for (int i = 0; i < pNameCount; i++) {
      if (!pNameless[i]) {
        fn->paramNames[i] =
            copyString(pNameTok[i].start, pNameTok[i].length);
      }
    }
  }
}

void compileFunction(FunctionType type) {
  Compiler compiler;
  initCompiler(&compiler, type);
  beginScope();

  consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
  parseParameterList();

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  block();

  ObjFunction* function = endCompiler();
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

  for (int i = 0; i < function->upvalueCount; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
    emitByte(compiler.upvalues[i].index);
  }
}

ObjFunction* compile(const char* source) {
  initScanner(source);

  Compiler compiler;
  initCompiler(&compiler, TYPE_SCRIPT);

  initTable(&constGlobals);

  parser.hadError = false;
  parser.panicMode = false;

  advance();

  while (!match(TOKEN_EOF)) {
    declaration();
  }

  ObjFunction* function = endCompiler();
  freeTable(&constGlobals);
  return parser.hadError ? NULL : function;
}

void markCompilerRoots() {
  Compiler* compiler = current;
  while (compiler != NULL) {
    markObject((Obj*)compiler->function);
    compiler = compiler->enclosing;
  }
  // Keep const-global name keys alive during compilation. Safe outside a
  // compile() pass too: freeTable() leaves the table zeroed, so this is a no-op.
  markTable(&constGlobals);
}
