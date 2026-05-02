#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_internal.h"
#include "memory.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

Parser parser;
Compiler* current = NULL;
ClassCompiler* currentClass = NULL;
Loop* currentLoop = NULL;
FinallyContext* currentFinally = NULL;
Chunk* compilingChunk;

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
  current = compiler;

  if (type != TYPE_SCRIPT && type != TYPE_ARROW) {
    current->function->name = copyString(parser.previous.start,
                                         parser.previous.length);
  }

  Local* local = &current->locals[current->localCount++];
  local->depth = 0;
  local->isCaptured = false;

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
  double value = strtod(parser.previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
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
                      bool isLocal) {
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
  return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Compiler* compiler, Token* name) {
  if (compiler->enclosing == NULL) return -1;

  int local = resolveLocal(compiler->enclosing, name);
  if (local != -1) {
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (uint8_t)local, true);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1) {
    return addUpvalue(compiler, (uint8_t)upvalue, false);
  }

  return -1;
}

bool isCompoundAssign(TokenType type) {
  return type == TOKEN_PLUS_EQUAL || type == TOKEN_MINUS_EQUAL ||
         type == TOKEN_STAR_EQUAL || type == TOKEN_SLASH_EQUAL ||
         type == TOKEN_PERCENT_EQUAL;
}

uint8_t compoundOp(TokenType type) {
  switch (type) {
    case TOKEN_PLUS_EQUAL:    return OP_ADD;
    case TOKEN_MINUS_EQUAL:   return OP_SUBTRACT;
    case TOKEN_STAR_EQUAL:    return OP_MULTIPLY;
    case TOKEN_SLASH_EQUAL:   return OP_DIVIDE;
    case TOKEN_PERCENT_EQUAL: return OP_MODULO;
    default: return 0;
  }
}

void namedVariable(Token name, bool canAssign) {
  uint8_t getOp, setOp;
  int arg = resolveLocal(current, &name);
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else if ((arg = resolveUpvalue(current, &name)) != -1) {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  } else {
    arg = identifierConstant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(setOp, (uint8_t)arg);
  } else if (canAssign && isCompoundAssign(parser.current.type)) {
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
    default: return; // Unreachable.
  }
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

static uint8_t argumentList() {
  uint8_t argCount = 0;

  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      expression();
      if (argCount == 255) {
        error("Can't have more than 255 arguments.");
      }
      argCount++;
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");

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
    uint8_t argCount = argumentList();
    emitBytes(OP_INVOKE, name);
    emitByte(argCount);
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
    uint8_t argCount = argumentList();
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_SUPER_INVOKE, name);
    emitByte(argCount);
  } else {
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_GET_SUPER, name);
  }
}

static void call(bool canAssign) {
  uint8_t argCount = argumentList();
  emitBytes(OP_CALL, argCount);
}

static void arrayLiteral(bool canAssign) {
  uint8_t elementCount = 0;

  if (!check(TOKEN_RIGHT_BRACKET)) {
    do {
      expression();
      if (elementCount == 255) {
        error("Can't have more than 255 elements in an array literal.");
      }
      elementCount++;
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_BRACKET, "Expect ']' after array elements.");
  emitBytes(OP_BUILD_ARRAY, elementCount);
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
      match(TOKEN_PERCENT_EQUAL))) {
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

void parseParameterList() {
  bool hasDefault = false;
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      current->function->arity++;
      if (current->function->arity > 255) {
        errorAtCurrent("Can't have more than 255 parameters.");
      }
      uint8_t constant = parseVariable("Expect parameter name.");
      defineVariable(constant);

      if (match(TOKEN_EQUAL)) {
        if (!hasDefault) {
          current->function->minArity = current->function->arity - 1;
          hasDefault = true;
        }
        // Emit OP_DEFAULT_ARG <paramIndex> <jump placeholder>.
        int paramIndex = current->function->arity - 1;
        int slot = current->localCount - 1;
        emitByte(OP_DEFAULT_ARG);
        emitByte((uint8_t)paramIndex);
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
  if (!hasDefault) {
    current->function->minArity = current->function->arity;
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

  parser.hadError = false;
  parser.panicMode = false;

  advance();

  while (!match(TOKEN_EOF)) {
    declaration();
  }

  ObjFunction* function = endCompiler();
  return parser.hadError ? NULL : function;
}

void markCompilerRoots() {
  Compiler* compiler = current;
  while (compiler != NULL) {
    markObject((Obj*)compiler->function);
    compiler = compiler->enclosing;
  }
}
