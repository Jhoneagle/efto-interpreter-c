#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <libgen.h>
#endif

static ObjString* getExeDir() {
  char buf[1024];
#ifdef _WIN32
  DWORD len = GetModuleFileNameA(NULL, buf, sizeof(buf));
  if (len == 0 || len >= sizeof(buf)) return copyString(".", 1);
#else
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len < 0) return copyString(".", 1);
  buf[len] = '\0';
#endif
  // Find last separator.
  const char* lastSlash = NULL;
  for (const char* p = buf; *p; p++) {
    if (*p == '/' || *p == '\\') lastSlash = p;
  }
  if (lastSlash) {
    return copyString(buf, (int)(lastSlash - buf));
  }
  return copyString(".", 1);
}

static ObjString* getCwd() {
  char buf[1024];
#ifdef _WIN32
  if (GetCurrentDirectoryA(sizeof(buf), buf) == 0)
    return copyString(".", 1);
#else
  if (getcwd(buf, sizeof(buf)) == NULL)
    return copyString(".", 1);
#endif
  return copyString(buf, (int)strlen(buf));
}

static ObjString* getDirFromPath(const char* path) {
  const char* lastSlash = NULL;
  for (const char* p = path; *p; p++) {
    if (*p == '/' || *p == '\\') lastSlash = p;
  }
  if (lastSlash) {
    return copyString(path, (int)(lastSlash - path));
  }
  return copyString(".", 1);
}

static void setupSearchPaths(const char* scriptPath) {
  vm.searchPathCount = 0;

  // 1. Script's directory (project root).
  vm.searchPaths[vm.searchPathCount++] = getDirFromPath(scriptPath);

  // 2. Current working directory.
  vm.searchPaths[vm.searchPathCount++] = getCwd();

  // 3. Interpreter binary's directory (for standard library).
  vm.searchPaths[vm.searchPathCount++] = getExeDir();
}

// True when accumulated REPL input forms a complete statement: all (), [], {}
// are balanced and no string literal is left open. Bracket/quote characters
// inside string literals and // line comments are ignored. Returns true on a
// surplus of closers too, so the compiler can report the error.
static bool replInputComplete(const char* src) {
  int depth = 0;
  bool inString = false;
  for (const char* p = src; *p; p++) {
    char c = *p;
    if (inString) {
      if (c == '\\' && p[1]) { p++; continue; }
      if (c == '"') inString = false;
      continue;
    }
    if (c == '"') { inString = true; continue; }
    if (c == '/' && p[1] == '/') {
      while (p[1] && p[1] != '\n') p++;
      continue;
    }
    if (c == '{' || c == '(' || c == '[') depth++;
    else if (c == '}' || c == ')' || c == ']') depth--;
  }
  return !inString && depth <= 0;
}

static void repl() {
  vm.searchPathCount = 0;
  vm.searchPaths[vm.searchPathCount++] = getCwd();
  vm.searchPaths[vm.searchPathCount++] = getExeDir();

  char buffer[65536];
  size_t len = 0;
  buffer[0] = '\0';
  char line[1024];

  for (;;) {
    // Secondary prompt while a multi-line statement (fun/class/match/…) is
    // still open.
    printf(len == 0 ? "> " : "... ");

    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }

    size_t lineLen = strlen(line);
    if (len + lineLen >= sizeof(buffer)) {
      fprintf(stderr, "Input too long; discarding.\n");
      len = 0;
      buffer[0] = '\0';
      continue;
    }
    memcpy(buffer + len, line, lineLen + 1);
    len += lineLen;

    // Keep buffering until the input balances (or a string closes).
    if (!replInputComplete(buffer)) continue;

    interpret(buffer);
    len = 0;
    buffer[0] = '\0';
  }
}

static void runFile(const char* path) {
  setupSearchPaths(path);

  char* source = readFile(path);
  if (source == NULL) {
    fprintf(stderr, "Could not open file \"%s\".\n", path);
    exit(74);
  }

  InterpretResult result = interpret(source);
  free(source);

  if (result == INTERPRET_COMPILE_ERROR) exit(65);
  if (result == INTERPRET_RUNTIME_ERROR) exit(70);
}

int main(int argc, const char* argv[]) {
  initVM();
  vm.argc = argc;
  vm.argv = argv;

  if (argc == 1) {
    repl();
  } else if (argc == 2) {
    runFile(argv[1]);
  } else {
    fprintf(stderr, "Usage: clox [path]\n");
    exit(64);
  }

  freeVM();
  return 0;
}
