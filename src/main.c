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

static void repl() {
  vm.searchPathCount = 0;
  vm.searchPaths[vm.searchPathCount++] = getCwd();
  vm.searchPaths[vm.searchPathCount++] = getExeDir();

  char line[1024];
  for (;;) {
    printf("> ");

    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }

    interpret(line);
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
