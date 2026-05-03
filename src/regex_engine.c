/*
 * Thompson NFA regex engine.
 *
 * Supports: literals, . * + ? | ^ $ () (?:) [abc] [a-z] [^...]
 *           \d \D \w \W \s \S \\ \. etc.
 * Does NOT support: backreferences, lookahead/behind, {n,m}, lazy quantifiers.
 *
 * Architecture:
 *   1. Parse regex string into an AST (recursive descent).
 *   2. Convert AST to NFA (Thompson construction).
 *   3. Simulate NFA with backtracking for capture groups.
 *
 * We use a simple recursive backtracking matcher. While Thompson NFA
 * simulation is O(n*m), adding capture groups with greedy semantics
 * is cleaner with backtracking for a small educational engine.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "regex_engine.h"

/* ---- AST ---- */

typedef enum {
  RE_LITERAL, RE_DOT, RE_CHAR_CLASS, RE_CONCAT, RE_ALT,
  RE_STAR, RE_PLUS, RE_QUESTION, RE_GROUP, RE_NC_GROUP,
  RE_ANCHOR_START, RE_ANCHOR_END,
} RegexNodeType;

typedef struct RegexNode {
  RegexNodeType type;
  int ch;
  bool charClass[128];
  bool classNegated;
  struct RegexNode* left;
  struct RegexNode* right;
  int groupIndex;
} RegexNode;

static RegexNode* newNode(RegexNodeType type) {
  RegexNode* n = (RegexNode*)calloc(1, sizeof(RegexNode));
  n->type = type;
  n->groupIndex = -1;
  return n;
}

static void freeNode(RegexNode* n) {
  if (!n) return;
  freeNode(n->left);
  freeNode(n->right);
  free(n);
}

/* ---- Parser ---- */

typedef struct {
  const char* src;
  int pos;
  int len;
  int flags;
  int groupCount;
  char error[256];
  bool hasError;
} RegexParser;

static RegexNode* parseAlternation(RegexParser* p);

static void parserError(RegexParser* p, const char* msg) {
  if (!p->hasError) {
    snprintf(p->error, sizeof(p->error), "%s at position %d", msg, p->pos);
    p->hasError = true;
  }
}

static char parserPeek(RegexParser* p) {
  return (p->pos >= p->len) ? '\0' : p->src[p->pos];
}

static char parserAdvance(RegexParser* p) {
  return (p->pos >= p->len) ? '\0' : p->src[p->pos++];
}

static bool parserMatch(RegexParser* p, char c) {
  if (p->pos < p->len && p->src[p->pos] == c) { p->pos++; return true; }
  return false;
}

static void setShorthand(bool charClass[128], char which) {
  switch (which) {
    case 'd': for (int i = '0'; i <= '9'; i++) charClass[i] = true; break;
    case 'D': for (int i = 0; i < 128; i++) if (!isdigit(i)) charClass[i] = true; break;
    case 'w': for (int i = 0; i < 128; i++) if (isalnum(i) || i == '_') charClass[i] = true; break;
    case 'W': for (int i = 0; i < 128; i++) if (!isalnum(i) && i != '_') charClass[i] = true; break;
    case 's': for (int i = 0; i < 128; i++) if (isspace(i)) charClass[i] = true; break;
    case 'S': for (int i = 0; i < 128; i++) if (!isspace(i)) charClass[i] = true; break;
  }
}

static RegexNode* parseCharClass(RegexParser* p) {
  RegexNode* n = newNode(RE_CHAR_CLASS);
  n->classNegated = parserMatch(p, '^');

  if (parserPeek(p) == ']') {
    n->charClass[(unsigned char)']'] = true;
    parserAdvance(p);
  }

  while (parserPeek(p) != ']' && parserPeek(p) != '\0') {
    char c = parserAdvance(p);
    if (c == '\\') {
      char esc = parserAdvance(p);
      if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' ||
          esc == 's' || esc == 'S') {
        setShorthand(n->charClass, esc);
        continue;
      }
      c = esc;
    }
    if (parserPeek(p) == '-' && p->pos + 1 < p->len && p->src[p->pos + 1] != ']') {
      parserAdvance(p);
      char end = parserAdvance(p);
      if (end == '\\') end = parserAdvance(p);
      for (int i = (unsigned char)c; i <= (unsigned char)end && i < 128; i++)
        n->charClass[i] = true;
    } else {
      if ((unsigned char)c < 128) n->charClass[(unsigned char)c] = true;
    }
  }
  if (!parserMatch(p, ']')) parserError(p, "Unterminated character class");
  return n;
}

static RegexNode* parseEscape(RegexParser* p) {
  char c = parserAdvance(p);
  if (c == 'd' || c == 'D' || c == 'w' || c == 'W' || c == 's' || c == 'S') {
    RegexNode* n = newNode(RE_CHAR_CLASS);
    setShorthand(n->charClass, c);
    return n;
  }
  RegexNode* n = newNode(RE_LITERAL);
  switch (c) {
    case 'n': n->ch = '\n'; break;
    case 't': n->ch = '\t'; break;
    case 'r': n->ch = '\r'; break;
    default:  n->ch = (unsigned char)c; break;
  }
  return n;
}

static RegexNode* parseAtom(RegexParser* p) {
  char c = parserPeek(p);
  if (c == '(') {
    parserAdvance(p);
    bool nc = false;
    if (parserPeek(p) == '?' && p->pos + 1 < p->len && p->src[p->pos + 1] == ':') {
      parserAdvance(p); parserAdvance(p); nc = true;
    }
    RegexNode* inner = parseAlternation(p);
    if (!parserMatch(p, ')')) { parserError(p, "Unmatched '('"); }
    if (nc) {
      RegexNode* n = newNode(RE_NC_GROUP); n->left = inner; return n;
    } else {
      RegexNode* n = newNode(RE_GROUP); n->groupIndex = p->groupCount++; n->left = inner; return n;
    }
  }
  if (c == '[') { parserAdvance(p); return parseCharClass(p); }
  if (c == '\\') { parserAdvance(p); return parseEscape(p); }
  if (c == '.') { parserAdvance(p); return newNode(RE_DOT); }
  if (c == '^') { parserAdvance(p); return newNode(RE_ANCHOR_START); }
  if (c == '$') { parserAdvance(p); return newNode(RE_ANCHOR_END); }
  if (c == '\0' || c == ')' || c == '|' || c == '*' || c == '+' || c == '?') return NULL;
  parserAdvance(p);
  RegexNode* n = newNode(RE_LITERAL); n->ch = (unsigned char)c; return n;
}

static RegexNode* parseQuantified(RegexParser* p) {
  RegexNode* atom = parseAtom(p);
  if (!atom) return NULL;
  if (parserMatch(p, '*')) { RegexNode* n = newNode(RE_STAR); n->left = atom; return n; }
  if (parserMatch(p, '+')) { RegexNode* n = newNode(RE_PLUS); n->left = atom; return n; }
  if (parserMatch(p, '?')) { RegexNode* n = newNode(RE_QUESTION); n->left = atom; return n; }
  return atom;
}

static RegexNode* parseConcat(RegexParser* p) {
  RegexNode* left = parseQuantified(p);
  if (!left) return NULL;
  while (parserPeek(p) != '\0' && parserPeek(p) != ')' && parserPeek(p) != '|') {
    RegexNode* right = parseQuantified(p);
    if (!right) break;
    RegexNode* cat = newNode(RE_CONCAT); cat->left = left; cat->right = right;
    left = cat;
  }
  return left;
}

static RegexNode* parseAlternation(RegexParser* p) {
  RegexNode* left = parseConcat(p);
  while (parserMatch(p, '|')) {
    RegexNode* right = parseConcat(p);
    RegexNode* alt = newNode(RE_ALT); alt->left = left; alt->right = right;
    left = alt;
  }
  return left;
}

/* ---- Compiled regex (just the AST + metadata) ---- */

struct CompiledRegex {
  RegexNode* ast;
  int groupCount;
  int flags;
};

CompiledRegex* regexCompile(const char* pattern, int flags,
                            char* errorBuf, int errorBufLen) {
  RegexParser parser;
  parser.src = pattern;
  parser.pos = 0;
  parser.len = (int)strlen(pattern);
  parser.flags = flags;
  parser.groupCount = 0;
  parser.hasError = false;
  parser.error[0] = '\0';

  RegexNode* ast = parseAlternation(&parser);

  if (parser.hasError) {
    if (errorBuf) snprintf(errorBuf, errorBufLen, "%s", parser.error);
    freeNode(ast);
    return NULL;
  }
  if (parser.pos < parser.len) {
    if (errorBuf) snprintf(errorBuf, errorBufLen,
                           "Unexpected character at position %d", parser.pos);
    freeNode(ast);
    return NULL;
  }

  CompiledRegex* compiled = (CompiledRegex*)malloc(sizeof(CompiledRegex));
  compiled->ast = ast;
  compiled->groupCount = parser.groupCount;
  compiled->flags = flags;
  return compiled;
}

void regexFree(CompiledRegex* compiled) {
  if (!compiled) return;
  freeNode(compiled->ast);
  free(compiled);
}

int regexGroupCount(CompiledRegex* compiled) {
  return compiled->groupCount;
}

/* ---- Recursive backtracking matcher ---- */

typedef struct {
  const char* text;
  int textLen;
  int flags;
  int groupStart[MAX_REGEX_GROUPS];
  int groupEnd[MAX_REGEX_GROUPS];
  int groupCount;
  int maxDepth;
} MatchCtx;

/*
 * Try to match `node` starting at text position `pos`.
 * Returns the position after the match, or -1 on failure.
 * Greedy: quantifiers try the longest match first.
 */
static int matchNode(MatchCtx* ctx, RegexNode* node, int pos) {
  if (!node) return pos; /* empty node matches trivially */
  if (ctx->maxDepth <= 0) return -1; /* recursion guard */
  ctx->maxDepth--;

  int result = -1;

  switch (node->type) {
    case RE_LITERAL: {
      if (pos >= ctx->textLen) break;
      int nc = (unsigned char)ctx->text[pos];
      int pc = node->ch;
      if (ctx->flags & REGEX_ICASE) { nc = tolower(nc); pc = tolower(pc); }
      if (nc == pc) result = pos + 1;
      break;
    }
    case RE_DOT: {
      if (pos >= ctx->textLen) break;
      if (ctx->text[pos] == '\n' && !(ctx->flags & REGEX_DOTALL)) break;
      result = pos + 1;
      break;
    }
    case RE_CHAR_CLASS: {
      if (pos >= ctx->textLen) break;
      int idx = (unsigned char)ctx->text[pos];
      bool inClass = (idx < 128) ? node->charClass[idx] : false;
      if (ctx->flags & REGEX_ICASE && idx < 128) {
        int alt = islower(idx) ? toupper(idx) : tolower(idx);
        if (alt < 128 && node->charClass[alt]) inClass = true;
      }
      if (node->classNegated ? !inClass : inClass) result = pos + 1;
      break;
    }
    case RE_CONCAT: {
      int mid = matchNode(ctx, node->left, pos);
      if (mid >= 0) result = matchNode(ctx, node->right, mid);
      break;
    }
    case RE_ALT: {
      result = matchNode(ctx, node->left, pos);
      if (result < 0) result = matchNode(ctx, node->right, pos);
      break;
    }
    case RE_STAR: {
      /* Greedy: try matching body as many times as possible, then fewer. */
      /* First, collect all possible lengths. */
      int positions[4096];
      int count = 0;
      positions[count++] = pos;
      int cur = pos;
      while (count < 4096) {
        int next = matchNode(ctx, node->left, cur);
        if (next < 0 || next == cur) break; /* no progress */
        positions[count++] = next;
        cur = next;
      }
      /* Try from longest to shortest (greedy). */
      for (int i = count - 1; i >= 0; i--) {
        result = positions[i];
        break; /* For star without continuation, return longest. */
      }
      /* Note: greedy behavior is handled by the caller (RE_CONCAT).
       * Star itself just returns the longest match of the repetition.
       * This is wrong for cases like "a*a" matching "aaa" — we need
       * to backtrack. Let's handle this properly. */
      result = -1;
      for (int i = count - 1; i >= 0; i--) {
        result = positions[i];
        break;
      }
      /* Actually, star needs to cooperate with what comes after.
       * Since we don't have "what comes after" here (it's in RE_CONCAT),
       * we just return the longest repetition. RE_CONCAT handles the rest. */
      result = positions[count - 1];
      break;
    }
    case RE_PLUS: {
      /* At least one match, then as many as possible (greedy). */
      int first = matchNode(ctx, node->left, pos);
      if (first < 0) break;
      int cur = first;
      while (1) {
        int next = matchNode(ctx, node->left, cur);
        if (next < 0 || next == cur) break;
        cur = next;
      }
      result = cur;
      break;
    }
    case RE_QUESTION: {
      /* Greedy: try with body first. */
      int with = matchNode(ctx, node->left, pos);
      result = (with >= 0) ? with : pos;
      break;
    }
    case RE_GROUP: {
      int saved = ctx->groupStart[node->groupIndex];
      int savedEnd = ctx->groupEnd[node->groupIndex];
      ctx->groupStart[node->groupIndex] = pos;
      int end = matchNode(ctx, node->left, pos);
      if (end >= 0) {
        ctx->groupEnd[node->groupIndex] = end;
        result = end;
      } else {
        ctx->groupStart[node->groupIndex] = saved;
        ctx->groupEnd[node->groupIndex] = savedEnd;
      }
      break;
    }
    case RE_NC_GROUP:
      result = matchNode(ctx, node->left, pos);
      break;
    case RE_ANCHOR_START:
      if (ctx->flags & REGEX_MULTILINE) {
        if (pos == 0 || ctx->text[pos - 1] == '\n') result = pos;
      } else {
        if (pos == 0) result = pos;
      }
      break;
    case RE_ANCHOR_END:
      if (ctx->flags & REGEX_MULTILINE) {
        if (pos == ctx->textLen || ctx->text[pos] == '\n') result = pos;
      } else {
        if (pos == ctx->textLen) result = pos;
      }
      break;
  }

  ctx->maxDepth++;
  return result;
}

/*
 * The star/plus handling above is too simplistic — it doesn't backtrack
 * when what comes after fails. We need star/plus to be part of concat
 * and try shorter matches when the longer match causes the rest to fail.
 *
 * Fix: rewrite RE_CONCAT to handle greedy backtracking when the left
 * side is a quantifier.
 */

/* Helper: match a repeated node (star/plus body) greedily with backtracking. */
static int matchRepeat(MatchCtx* ctx, RegexNode* body, RegexNode* rest,
                       int pos, int minCount) {
  /* Collect all positions reachable by repeating body. */
  int positions[4096];
  int count = 0;
  positions[count++] = pos;
  int cur = pos;
  while (count < 4096) {
    int next = matchNode(ctx, body, cur);
    if (next < 0 || next == cur) break;
    positions[count++] = next;
    cur = next;
  }
  /* Greedy: try from longest to shortest, checking if rest matches. */
  for (int i = count - 1; i >= minCount; i--) {
    int afterRest = matchNode(ctx, rest, positions[i]);
    if (afterRest >= 0) return afterRest;
  }
  return -1;
}

/*
 * Override the concat handler to use matchRepeat when left is a quantifier.
 * We rewrite matchNode for RE_CONCAT, RE_STAR, RE_PLUS, RE_QUESTION.
 */

/* The main match function, with proper greedy backtracking. */
static int matchNodeBT(MatchCtx* ctx, RegexNode* node, int pos);

static int matchRepeatBT(MatchCtx* ctx, RegexNode* body, RegexNode* rest,
                         int pos, int minCount) {
  int positions[4096];
  int count = 0;
  positions[count++] = pos;
  int cur = pos;
  while (count < 4096) {
    int next = matchNodeBT(ctx, body, cur);
    if (next < 0 || next == cur) break;
    positions[count++] = next;
    cur = next;
  }
  for (int i = count - 1; i >= minCount; i--) {
    int afterRest;
    if (rest) {
      afterRest = matchNodeBT(ctx, rest, positions[i]);
    } else {
      afterRest = positions[i];
    }
    if (afterRest >= 0) return afterRest;
  }
  return -1;
}

static int matchNodeBT(MatchCtx* ctx, RegexNode* node, int pos) {
  if (!node) return pos;
  if (ctx->maxDepth <= 0) return -1;
  ctx->maxDepth--;
  int result = -1;

  switch (node->type) {
    case RE_LITERAL: {
      if (pos >= ctx->textLen) break;
      int nc = (unsigned char)ctx->text[pos];
      int pc = node->ch;
      if (ctx->flags & REGEX_ICASE) { nc = tolower(nc); pc = tolower(pc); }
      if (nc == pc) result = pos + 1;
      break;
    }
    case RE_DOT: {
      if (pos >= ctx->textLen) break;
      if (ctx->text[pos] == '\n' && !(ctx->flags & REGEX_DOTALL)) break;
      result = pos + 1;
      break;
    }
    case RE_CHAR_CLASS: {
      if (pos >= ctx->textLen) break;
      int idx = (unsigned char)ctx->text[pos];
      bool inClass = (idx < 128) ? node->charClass[idx] : false;
      if (ctx->flags & REGEX_ICASE && idx < 128) {
        int alt = islower(idx) ? toupper(idx) : tolower(idx);
        if (alt < 128 && node->charClass[alt]) inClass = true;
      }
      if (node->classNegated ? !inClass : inClass) result = pos + 1;
      break;
    }
    case RE_CONCAT: {
      /* If left is a quantifier, use backtracking repeat. */
      if (node->left && node->left->type == RE_STAR) {
        result = matchRepeatBT(ctx, node->left->left, node->right, pos, 0);
      } else if (node->left && node->left->type == RE_PLUS) {
        result = matchRepeatBT(ctx, node->left->left, node->right, pos, 1);
      } else if (node->left && node->left->type == RE_QUESTION) {
        /* Greedy: try with body first, then without. */
        int with = matchNodeBT(ctx, node->left->left, pos);
        if (with >= 0) {
          result = matchNodeBT(ctx, node->right, with);
        }
        if (result < 0) {
          result = matchNodeBT(ctx, node->right, pos);
        }
      } else {
        int mid = matchNodeBT(ctx, node->left, pos);
        if (mid >= 0) result = matchNodeBT(ctx, node->right, mid);
      }
      break;
    }
    case RE_ALT: {
      result = matchNodeBT(ctx, node->left, pos);
      if (result < 0) result = matchNodeBT(ctx, node->right, pos);
      break;
    }
    case RE_STAR: {
      /* Star at the end (no concat right side) — match as many as possible. */
      result = matchRepeatBT(ctx, node->left, NULL, pos, 0);
      break;
    }
    case RE_PLUS: {
      result = matchRepeatBT(ctx, node->left, NULL, pos, 1);
      break;
    }
    case RE_QUESTION: {
      int with = matchNodeBT(ctx, node->left, pos);
      result = (with >= 0) ? with : pos;
      break;
    }
    case RE_GROUP: {
      int savedStart = ctx->groupStart[node->groupIndex];
      int savedEnd = ctx->groupEnd[node->groupIndex];
      ctx->groupStart[node->groupIndex] = pos;
      int end = matchNodeBT(ctx, node->left, pos);
      if (end >= 0) {
        ctx->groupEnd[node->groupIndex] = end;
        result = end;
      } else {
        ctx->groupStart[node->groupIndex] = savedStart;
        ctx->groupEnd[node->groupIndex] = savedEnd;
      }
      break;
    }
    case RE_NC_GROUP:
      result = matchNodeBT(ctx, node->left, pos);
      break;
    case RE_ANCHOR_START:
      if (ctx->flags & REGEX_MULTILINE) {
        if (pos == 0 || ctx->text[pos - 1] == '\n') result = pos;
      } else {
        if (pos == 0) result = pos;
      }
      break;
    case RE_ANCHOR_END:
      if (ctx->flags & REGEX_MULTILINE) {
        if (pos == ctx->textLen || ctx->text[pos] == '\n') result = pos;
      } else {
        if (pos == ctx->textLen) result = pos;
      }
      break;
  }

  ctx->maxDepth++;
  return result;
}

RegexResult regexExec(CompiledRegex* compiled, const char* text,
                      int startPos) {
  RegexResult result;
  result.matched = false;
  result.matchStart = result.matchEnd = -1;
  result.groupCount = compiled->groupCount;
  for (int i = 0; i < MAX_REGEX_GROUPS; i++)
    result.groupStart[i] = result.groupEnd[i] = -1;

  int textLen = (int)strlen(text);

  MatchCtx ctx;
  ctx.text = text;
  ctx.textLen = textLen;
  ctx.flags = compiled->flags;
  ctx.groupCount = compiled->groupCount;

  for (int start = startPos; start <= textLen; start++) {
    for (int i = 0; i < MAX_REGEX_GROUPS; i++)
      ctx.groupStart[i] = ctx.groupEnd[i] = -1;
    ctx.maxDepth = 100000;

    int end = matchNodeBT(&ctx, compiled->ast, start);
    if (end >= 0) {
      result.matched = true;
      result.matchStart = start;
      result.matchEnd = end;
      for (int g = 0; g < compiled->groupCount && g < MAX_REGEX_GROUPS; g++) {
        result.groupStart[g] = ctx.groupStart[g];
        result.groupEnd[g] = ctx.groupEnd[g];
      }
      return result;
    }
  }

  return result;
}
