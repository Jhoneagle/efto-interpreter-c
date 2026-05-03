#ifndef clox_regex_engine_h
#define clox_regex_engine_h

#include <stdbool.h>

#define REGEX_ICASE     1
#define REGEX_MULTILINE 2
#define REGEX_DOTALL    4

#define MAX_REGEX_GROUPS 16

typedef struct CompiledRegex CompiledRegex;

typedef struct {
  bool matched;
  int matchStart;
  int matchEnd;
  int groupCount;
  int groupStart[MAX_REGEX_GROUPS];
  int groupEnd[MAX_REGEX_GROUPS];
} RegexResult;

// Compile a regex pattern. Returns NULL on error, sets errorBuf.
CompiledRegex* regexCompile(const char* pattern, int flags,
                            char* errorBuf, int errorBufLen);

// Execute regex on text starting at startPos. Returns first match.
RegexResult regexExec(CompiledRegex* compiled, const char* text,
                      int startPos);

// Free compiled regex.
void regexFree(CompiledRegex* compiled);

// Get number of capture groups.
int regexGroupCount(CompiledRegex* compiled);

#endif
