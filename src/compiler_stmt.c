#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compiler_internal.h"
#include "memory.h"

static void method() {
  consume(TOKEN_IDENTIFIER, "Expect method name.");
  uint8_t constant = identifierConstant(&parser.previous);

  FunctionType type = TYPE_METHOD;
  if (parser.previous.length == 4 &&
      memcmp(parser.previous.start, "init", 4) == 0) {
    type = TYPE_INITIALIZER;
  }

  compileFunction(type);
  emitBytes(OP_METHOD, constant);
}

static void classDeclaration() {
  consume(TOKEN_IDENTIFIER, "Expect class name.");
  Token className = parser.previous;
  uint8_t nameConstant = identifierConstant(&parser.previous);
  declareVariable();

  emitBytes(OP_CLASS, nameConstant);
  defineVariable(nameConstant);

  ClassCompiler classCompiler;
  classCompiler.hasSuperclass = false;
  classCompiler.enclosing = currentClass;
  currentClass = &classCompiler;

  if (match(TOKEN_LESS)) {
    consume(TOKEN_IDENTIFIER, "Expect superclass name.");
    namedVariable(parser.previous, false);

    if (identifiersEqual(&className, &parser.previous)) {
      error("A class can't inherit from itself.");
    }

    beginScope();
    addLocal(syntheticToken("super"));
    defineVariable(0);

    namedVariable(className, false);
    emitByte(OP_INHERIT);
    classCompiler.hasSuperclass = true;
  }

  namedVariable(className, false);
  consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");

  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    method();
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
  emitByte(OP_POP);

  if (classCompiler.hasSuperclass) {
    endScope();
  }

  currentClass = currentClass->enclosing;
}

static void funDeclaration() {
  uint8_t global = parseVariable("Expect function name.");
  markInitialized();
  compileFunction(TYPE_FUNCTION);
  defineVariable(global);
}

static void arrayDestructure() {
  advance(); // consume '['

  Token names[MAX_DESTRUCTURE_VARS];
  bool skip[MAX_DESTRUCTURE_VARS];
  int count = 0;

  if (!check(TOKEN_RIGHT_BRACKET)) {
    do {
      if (count >= MAX_DESTRUCTURE_VARS) {
        error("Too many variables in destructuring pattern.");
        return;
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
    return;
  }
  consume(TOKEN_EQUAL, "Destructuring requires an initializer.");
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

  bool isLocal = (current->scopeDepth > 0);

  if (isLocal) {
    addLocal(syntheticToken(""));
    markInitialized();
    int hiddenSlot = current->localCount - 1;

    for (int i = 0; i < count; i++) {
      if (skip[i]) continue;
      emitBytes(OP_GET_LOCAL, (uint8_t)hiddenSlot);
      emitConstant(NUMBER_VAL(i));
      emitByte(OP_INDEX_GET);
      declareVariableByName(&names[i]);
      markInitialized();
    }
  } else {
    // Global scope: DUP-based approach.
    // Find last non-skipped index.
    int lastNonSkipped = -1;
    for (int i = count - 1; i >= 0; i--) {
      if (!skip[i]) { lastNonSkipped = i; break; }
    }

    for (int i = 0; i < count; i++) {
      if (skip[i]) continue;
      if (i != lastNonSkipped) {
        emitBytes(OP_DUP, 0);
      }
      emitConstant(NUMBER_VAL(i));
      emitByte(OP_INDEX_GET);
      uint8_t nameConst = identifierConstant(&names[i]);
      emitBytes(OP_DEFINE_GLOBAL, nameConst);
    }

    if (lastNonSkipped == -1) {
      emitByte(OP_POP);
    }
  }
}

static void mapDestructure() {
  advance(); // consume '{'

  Token keys[256];
  Token varNames[256];
  int count = 0;

  if (!check(TOKEN_RIGHT_BRACE)) {
    do {
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
    return;
  }
  consume(TOKEN_EQUAL, "Destructuring requires an initializer.");
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

  bool isLocal = (current->scopeDepth > 0);

  if (isLocal) {
    addLocal(syntheticToken(""));
    markInitialized();
    int hiddenSlot = current->localCount - 1;

    for (int i = 0; i < count; i++) {
      emitBytes(OP_GET_LOCAL, (uint8_t)hiddenSlot);
      emitConstant(OBJ_VAL(copyString(keys[i].start, keys[i].length)));
      emitByte(OP_INDEX_GET);
      declareVariableByName(&varNames[i]);
      markInitialized();
    }
  } else {
    for (int i = 0; i < count; i++) {
      if (i < count - 1) {
        emitBytes(OP_DUP, 0);
      }
      emitConstant(OBJ_VAL(copyString(keys[i].start, keys[i].length)));
      emitByte(OP_INDEX_GET);
      uint8_t nameConst = identifierConstant(&varNames[i]);
      emitBytes(OP_DEFINE_GLOBAL, nameConst);
    }
  }
}

static void varDeclaration() {
  if (check(TOKEN_LEFT_BRACKET)) {
    arrayDestructure();
    return;
  }
  if (check(TOKEN_LEFT_BRACE)) {
    mapDestructure();
    return;
  }

  uint8_t global = parseVariable("Expect variable name.");

  if (match(TOKEN_EQUAL)) {
    expression();
  } else {
    emitByte(OP_NIL);
  }
  consume(TOKEN_SEMICOLON,
          "Expect ';' after variable declaration.");

  defineVariable(global);
}

static void expressionStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(OP_POP);
}

static void ifStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int thenJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();

  int elseJump = emitJump(OP_JUMP);

  patchJump(thenJump);
  emitByte(OP_POP);

  if (match(TOKEN_ELSE)) statement();
  patchJump(elseJump);
}

static void printStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value.");
  emitByte(OP_PRINT);
}

static void whileStatement() {
  int loopStart = currentChunk()->count;
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int exitJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);

  Loop loop;
  loop.enclosing = currentLoop;
  loop.scopeDepth = current->scopeDepth;
  loop.breakCount = 0;
  loop.continueTarget = loopStart;
  loop.hasContinueTarget = true;
  loop.continueCount = 0;
  currentLoop = &loop;

  statement();
  emitLoop(loopStart);

  patchJump(exitJump);
  emitByte(OP_POP);

  for (int i = 0; i < loop.breakCount; i++) {
    patchJump(loop.breakJumps[i]);
  }

  currentLoop = loop.enclosing;
}

static void forEachStatement() {
  // for (var item in collection) { body }
  // Caller already consumed 'for', '(', 'var', <identifier>, 'in'.
  // parser.previous is set to the iterator variable name.
  beginScope();

  Token iteratorName = parser.previous;

  // Compile the collection expression -> hidden local (slot C).
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after for-each collection.");

  addLocal(syntheticToken(""));
  markInitialized();
  int collectionSlot = current->localCount - 1;

  // Hidden index local initialized to 0 (slot I).
  emitConstant(NUMBER_VAL(0));
  addLocal(syntheticToken(""));
  markInitialized();
  int indexSlot = current->localCount - 1;

  // Loop start: check index < collection.length
  int loopStart = currentChunk()->count;

  emitBytes(OP_GET_LOCAL, (uint8_t)indexSlot);
  emitBytes(OP_GET_LOCAL, (uint8_t)collectionSlot);
  uint8_t lengthConstant = makeConstant(OBJ_VAL(
      copyString("length", 6)));
  emitBytes(OP_GET_PROPERTY, lengthConstant);
  emitByte(OP_LESS);

  int exitJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP); // Pop the true.

  // Set up loop context. scopeDepth is the outer for-each scope.
  Loop loop;
  loop.enclosing = currentLoop;
  loop.scopeDepth = current->scopeDepth;
  loop.breakCount = 0;
  loop.hasContinueTarget = false;
  loop.continueCount = 0;
  currentLoop = &loop;

  // Inner scope for the user's variable.
  beginScope();

  // var item = collection[index]
  emitBytes(OP_GET_LOCAL, (uint8_t)collectionSlot);
  emitBytes(OP_GET_LOCAL, (uint8_t)indexSlot);
  emitByte(OP_INDEX_GET);
  addLocal(iteratorName);
  markInitialized();

  // Compile the body.
  statement();

  endScope(); // Pops user's variable.

  // Patch continue jumps to here (before increment).
  for (int i = 0; i < loop.continueCount; i++) {
    patchJump(loop.continueJumps[i]);
  }

  // Increment index: index = index + 1
  emitBytes(OP_GET_LOCAL, (uint8_t)indexSlot);
  emitConstant(NUMBER_VAL(1));
  emitByte(OP_ADD);
  emitBytes(OP_SET_LOCAL, (uint8_t)indexSlot);
  emitByte(OP_POP);

  emitLoop(loopStart);

  patchJump(exitJump);
  emitByte(OP_POP); // Pop the false.

  // Patch break jumps to here.
  for (int i = 0; i < loop.breakCount; i++) {
    patchJump(loop.breakJumps[i]);
  }

  currentLoop = loop.enclosing;
  endScope(); // Pops hidden locals.
}

static void forStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

  // Check for for-each: for (var <name> in <expr>)
  if (match(TOKEN_VAR)) {
    if (check(TOKEN_IDENTIFIER) && parser.current.type == TOKEN_IDENTIFIER) {
      // Save the identifier token, consume it.
      advance();
      Token name = parser.previous;

      if (match(TOKEN_IN)) {
        // It's a for-each. parser.previous was the identifier.
        // Temporarily set parser.previous to the name so
        // forEachStatement can use it.
        parser.previous = name;
        forEachStatement();
        return;
      }

      // Not for-each. We consumed 'var' and '<identifier>'.
      // Continue as a regular for with var declaration.
      beginScope();
      declareVariable();
      if (current->scopeDepth > 0) {
        if (match(TOKEN_EQUAL)) {
          expression();
        } else {
          emitByte(OP_NIL);
        }
        consume(TOKEN_SEMICOLON,
                "Expect ';' after variable declaration.");
        markInitialized();
      } else {
        uint8_t global = identifierConstant(&name);
        if (match(TOKEN_EQUAL)) {
          expression();
        } else {
          emitByte(OP_NIL);
        }
        consume(TOKEN_SEMICOLON,
                "Expect ';' after variable declaration.");
        emitBytes(OP_DEFINE_GLOBAL, global);
      }
      goto forBody;
    }
    // 'var' but not an identifier next — shouldn't happen, but
    // let varDeclaration handle the error.
    beginScope();
    varDeclaration();
    goto forBody;
  }

  // Regular for loop (no var).
  beginScope();

  if (match(TOKEN_SEMICOLON)) {
    // No initializer.
  } else {
    expressionStatement();
  }

forBody:;
  int loopStart = currentChunk()->count;

  int exitJump = -1;
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

    // Jump out of the loop if the condition is false.
    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // Condition.
  }

  if (!match(TOKEN_RIGHT_PAREN)) {
    int bodyJump = emitJump(OP_JUMP);
    int incrementStart = currentChunk()->count;
    expression();
    emitByte(OP_POP);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    emitLoop(loopStart);
    loopStart = incrementStart;
    patchJump(bodyJump);
  }

  Loop loop;
  loop.enclosing = currentLoop;
  loop.scopeDepth = current->scopeDepth;
  loop.breakCount = 0;
  loop.continueTarget = loopStart;
  loop.hasContinueTarget = true;
  loop.continueCount = 0;
  currentLoop = &loop;

  statement();
  emitLoop(loopStart);

  if (exitJump != -1) {
    patchJump(exitJump);
    emitByte(OP_POP); // Condition.
  }

  for (int i = 0; i < loop.breakCount; i++) {
    patchJump(loop.breakJumps[i]);
  }

  currentLoop = loop.enclosing;
  endScope();
}

static void switchStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'switch'.");

  // Wrap in a scope so the switch value becomes a hidden local.
  beginScope();
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after switch value.");
  consume(TOKEN_LEFT_BRACE, "Expect '{' before switch cases.");

  // The switch value is on the stack as a hidden local.
  // Register it so we can read it back with OP_GET_LOCAL.
  addLocal(syntheticToken(""));
  markInitialized();
  int switchLocal = current->localCount - 1;

  int caseCount = 0;
  int caseEnds[MAX_SWITCH_CASES];
  bool hasDefault = false;

  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    if (match(TOKEN_CASE)) {
      if (hasDefault) {
        error("Can't have a case after 'default'.");
      }

      // Push the switch value, then the case value, then compare.
      emitBytes(OP_GET_LOCAL, (uint8_t)switchLocal);
      expression();
      consume(TOKEN_COLON, "Expect ':' after case value.");

      emitByte(OP_EQUAL);
      int skipJump = emitJump(OP_JUMP_IF_FALSE);
      emitByte(OP_POP); // Pop the true result.

      // Compile the case body.
      statement();

      // Jump to end of switch after the case body.
      if (caseCount < MAX_SWITCH_CASES) {
        caseEnds[caseCount++] = emitJump(OP_JUMP);
      } else {
        error("Too many cases in switch statement.");
      }

      patchJump(skipJump);
      emitByte(OP_POP); // Pop the false result.
    } else if (match(TOKEN_DEFAULT)) {
      if (hasDefault) {
        error("Already have a default case.");
      }
      hasDefault = true;

      consume(TOKEN_COLON, "Expect ':' after 'default'.");
      statement();
    } else {
      error("Expect 'case' or 'default' in switch.");
      return;
    }
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after switch cases.");

  // Patch all case end jumps to here.
  for (int i = 0; i < caseCount; i++) {
    patchJump(caseEnds[i]);
  }

  endScope();
}

static void emitCleanupToScope(int targetDepth) {
  for (int i = current->localCount - 1; i >= 0; i--) {
    if (current->locals[i].depth != -1 &&
        current->locals[i].depth <= targetDepth) break;
    if (current->locals[i].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }
  }
}

static void returnStatement() {
  if (current->type == TYPE_SCRIPT) {
    error("Can't return from top-level code.");
  }

  if (match(TOKEN_SEMICOLON)) {
    if (currentFinally != NULL) {
      emitByte(OP_NIL);
      emitBytes(OP_ENTER_FINALLY, COMPLETE_RETURN);
      currentFinally->enterJumps[currentFinally->enterJumpCount++] =
          emitJump(OP_JUMP);
    } else {
      emitReturn();
    }
  } else {
    if (current->type == TYPE_INITIALIZER) {
      error("Can't return a value from an initializer.");
    }

    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after return value.");

    if (currentFinally != NULL) {
      emitBytes(OP_ENTER_FINALLY, COMPLETE_RETURN);
      currentFinally->enterJumps[currentFinally->enterJumpCount++] =
          emitJump(OP_JUMP);
    } else {
      emitByte(OP_RETURN);
    }
  }
}

static void breakStatement() {
  if (currentLoop == NULL) {
    error("Can't use 'break' outside of a loop.");
    return;
  }

  consume(TOKEN_SEMICOLON, "Expect ';' after 'break'.");

  // Pop locals down to the loop's scope depth.
  for (int i = current->localCount - 1; i >= 0; i--) {
    if (current->locals[i].depth != -1 &&
        current->locals[i].depth <= currentLoop->scopeDepth) break;
    if (current->locals[i].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }
  }

  if (currentLoop->breakCount < MAX_LOOP_JUMPS) {
    currentLoop->breakJumps[currentLoop->breakCount++] =
        emitJump(OP_JUMP);
  } else {
    error("Too many break statements in loop.");
  }
}

static void continueStatement() {
  if (currentLoop == NULL) {
    error("Can't use 'continue' outside of a loop.");
    return;
  }

  consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");

  // Pop locals down to the loop's scope depth.
  for (int i = current->localCount - 1; i >= 0; i--) {
    if (current->locals[i].depth != -1 &&
        current->locals[i].depth <= currentLoop->scopeDepth) break;
    if (current->locals[i].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }
  }

  if (currentLoop->hasContinueTarget) {
    emitLoop(currentLoop->continueTarget);
  } else {
    if (currentLoop->continueCount < MAX_LOOP_JUMPS) {
      currentLoop->continueJumps[currentLoop->continueCount++] =
          emitJump(OP_JUMP);
    } else {
      error("Too many continue statements in loop.");
    }
  }
}

static void throwStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after throw value.");
  emitByte(OP_THROW);
}

static void tryStatement() {
  // Speculatively emit OP_SETUP_FINALLY (NOP'd out if no finally).
  int setupFinallyPos = currentChunk()->count;
  emitByte(OP_SETUP_FINALLY);
  emitByte(0xff);
  emitByte(0xff);

  // Speculatively emit OP_TRY (NOP'd out if no catch).
  int setupTryPos = currentChunk()->count;
  emitByte(OP_TRY);
  emitByte(0xff);
  emitByte(0xff);

  // Push finally context.
  FinallyContext finallyCtx;
  finallyCtx.enclosing = currentFinally;
  finallyCtx.scopeDepth = current->scopeDepth;
  finallyCtx.enterJumpCount = 0;
  currentFinally = &finallyCtx;

  // Compile try body.
  consume(TOKEN_LEFT_BRACE, "Expect '{' after 'try'.");
  beginScope();
  block();
  endScope();

  // Normal exit: pop catch handler if it was pushed.
  int endTryPos = currentChunk()->count;
  emitByte(OP_END_TRY);

  // Jump over catch block.
  int skipCatch = emitJump(OP_JUMP);

  int catchStart = currentChunk()->count;

  bool hasCatch = false;
  if (match(TOKEN_CATCH)) {
    hasCatch = true;

    // Patch OP_TRY to point to catch start.
    int tryOffset = catchStart - (setupTryPos + 3);
    currentChunk()->code[setupTryPos + 1] = (tryOffset >> 8) & 0xff;
    currentChunk()->code[setupTryPos + 2] = tryOffset & 0xff;

    beginScope();
    if (match(TOKEN_LEFT_PAREN)) {
      consume(TOKEN_IDENTIFIER, "Expect variable name in catch.");
      addLocal(parser.previous);
      markInitialized();
      consume(TOKEN_RIGHT_PAREN, "Expect ')' after catch variable.");
    } else {
      emitByte(OP_POP);
    }

    consume(TOKEN_LEFT_BRACE, "Expect '{' after catch.");
    block();
    endScope();
  }

  patchJump(skipCatch);

  if (!hasCatch) {
    // NOP out OP_TRY and OP_END_TRY.
    currentChunk()->code[setupTryPos] = OP_NOP;
    currentChunk()->code[setupTryPos + 1] = OP_NOP;
    currentChunk()->code[setupTryPos + 2] = OP_NOP;
    currentChunk()->code[endTryPos] = OP_NOP;
  }

  bool hasFinally = false;
  if (match(TOKEN_FINALLY)) {
    hasFinally = true;

    // Emit OP_ENTER_FINALLY for normal path.
    emitBytes(OP_ENTER_FINALLY, COMPLETE_NORMAL);

    // Patch all enter-finally jumps (return/break/continue through finally).
    for (int i = 0; i < finallyCtx.enterJumpCount; i++) {
      patchJump(finallyCtx.enterJumps[i]);
    }

    // Patch OP_SETUP_FINALLY to point here (start of finally body).
    int finallyStart = currentChunk()->count;
    int finallyOffset = finallyStart - (setupFinallyPos + 3);
    currentChunk()->code[setupFinallyPos + 1] =
        (finallyOffset >> 8) & 0xff;
    currentChunk()->code[setupFinallyPos + 2] =
        finallyOffset & 0xff;

    consume(TOKEN_LEFT_BRACE, "Expect '{' after 'finally'.");
    beginScope();
    block();
    endScope();

    emitByte(OP_END_FINALLY);
  }

  if (!hasFinally) {
    // NOP out OP_SETUP_FINALLY.
    currentChunk()->code[setupFinallyPos] = OP_NOP;
    currentChunk()->code[setupFinallyPos + 1] = OP_NOP;
    currentChunk()->code[setupFinallyPos + 2] = OP_NOP;
  }

  // Pop finally context.
  currentFinally = finallyCtx.enclosing;

  if (!hasCatch && !hasFinally) {
    error("Expect 'catch' or 'finally' after try block.");
  }
}

static void synchronize() {
  parser.panicMode = false;

  while (parser.current.type != TOKEN_EOF) {
    if (parser.previous.type == TOKEN_SEMICOLON) return;
    switch (parser.current.type) {
      case TOKEN_CLASS:
      case TOKEN_FUN:
      case TOKEN_VAR:
      case TOKEN_FOR:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_PRINT:
      case TOKEN_RETURN:
      case TOKEN_SWITCH:
      case TOKEN_BREAK:
      case TOKEN_CONTINUE:
      case TOKEN_IMPORT:
      case TOKEN_TRY:
      case TOKEN_THROW:
        return;

      default:
        ; // Do nothing.
    }

    advance();
  }
}

static void importDeclaration() {
  if (current->scopeDepth > 0) {
    error("Import declarations must be at top level.");
    return;
  }

  if (match(TOKEN_LEFT_BRACE)) {
    // Form 3: import { name1, name2 } from module;
    uint8_t names[MAX_IMPORT_NAMES];
    int nameCount = 0;

    do {
      consume(TOKEN_IDENTIFIER, "Expect import name.");
      if (nameCount >= MAX_IMPORT_NAMES) {
        error("Too many names in import.");
        return;
      }
      names[nameCount++] = identifierConstant(&parser.previous);
    } while (match(TOKEN_COMMA));

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after import names.");
    consume(TOKEN_FROM, "Expect 'from' after import names.");

    // Parse dotted module name.
    consume(TOKEN_IDENTIFIER, "Expect module name.");
    char modulePath[MAX_MODULE_PATH_LEN];
    int pathLen = parser.previous.length;
    if (pathLen >= MAX_MODULE_PATH_LEN) {
      error("Module path too long.");
      return;
    }
    memcpy(modulePath, parser.previous.start, pathLen);

    while (match(TOKEN_DOT)) {
      consume(TOKEN_IDENTIFIER, "Expect identifier after '.'.");
      if (pathLen + 1 + parser.previous.length >= MAX_MODULE_PATH_LEN) {
        error("Module path too long.");
        return;
      }
      modulePath[pathLen++] = '.';
      memcpy(modulePath + pathLen, parser.previous.start,
             parser.previous.length);
      pathLen += parser.previous.length;
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after import declaration.");

    uint8_t pathConst = makeConstant(
        OBJ_VAL(copyString(modulePath, pathLen)));
    emitBytes(OP_IMPORT, pathConst);

    // Extract each name from the module.
    for (int i = 0; i < nameCount; i++) {
      if (i < nameCount - 1) {
        emitBytes(OP_DUP, 0);
      }
      emitBytes(OP_GET_PROPERTY, names[i]);
      emitBytes(OP_DEFINE_GLOBAL, names[i]);
    }
  } else {
    // Form 1/2: import name; or import name as alias;
    consume(TOKEN_IDENTIFIER, "Expect module name after 'import'.");

    char modulePath[MAX_MODULE_PATH_LEN];
    int pathLen = parser.previous.length;
    if (pathLen >= MAX_MODULE_PATH_LEN) {
      error("Module path too long.");
      return;
    }
    memcpy(modulePath, parser.previous.start, pathLen);

    const char* nameStart = parser.previous.start;
    int nameLen = parser.previous.length;

    while (match(TOKEN_DOT)) {
      consume(TOKEN_IDENTIFIER, "Expect identifier after '.'.");
      if (pathLen + 1 + parser.previous.length >= MAX_MODULE_PATH_LEN) {
        error("Module path too long.");
        return;
      }
      modulePath[pathLen++] = '.';
      memcpy(modulePath + pathLen, parser.previous.start,
             parser.previous.length);
      pathLen += parser.previous.length;
      nameStart = parser.previous.start;
      nameLen = parser.previous.length;
    }

    if (match(TOKEN_AS)) {
      consume(TOKEN_IDENTIFIER, "Expect alias after 'as'.");
      nameStart = parser.previous.start;
      nameLen = parser.previous.length;
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after import declaration.");

    uint8_t pathConst = makeConstant(
        OBJ_VAL(copyString(modulePath, pathLen)));
    emitBytes(OP_IMPORT, pathConst);

    uint8_t nameConst = makeConstant(
        OBJ_VAL(copyString(nameStart, nameLen)));
    emitBytes(OP_DEFINE_GLOBAL, nameConst);
  }
}

void declaration() {
  if (match(TOKEN_CLASS)) {
    classDeclaration();
  } else if (match(TOKEN_FUN)) {
    funDeclaration();
  } else if (match(TOKEN_VAR)) {
    varDeclaration();
  } else if (match(TOKEN_IMPORT)) {
    importDeclaration();
  } else {
    statement();
  }

  if (parser.panicMode) synchronize();
}

void statement() {
  if (match(TOKEN_PRINT)) {
    printStatement();
  } else if (match(TOKEN_IF)) {
    ifStatement();
  } else if (match(TOKEN_RETURN)) {
    returnStatement();
  } else if (match(TOKEN_WHILE)) {
    whileStatement();
  } else if (match(TOKEN_FOR)) {
    forStatement();
  } else if (match(TOKEN_SWITCH)) {
    switchStatement();
  } else if (match(TOKEN_BREAK)) {
    breakStatement();
  } else if (match(TOKEN_CONTINUE)) {
    continueStatement();
  } else if (match(TOKEN_TRY)) {
    tryStatement();
  } else if (match(TOKEN_THROW)) {
    throwStatement();
  } else if (match(TOKEN_LEFT_BRACE)) {
    beginScope();
    block();
    endScope();
  } else {
    expressionStatement();
  }
}
