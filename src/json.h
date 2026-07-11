#ifndef clox_json_h
#define clox_json_h

#include "value.h"

// json.stringify(value, indent?) -> string
Value jsonStringifyNative(int argCount, Value* args);

// json.parse(string) -> value
Value jsonParseNative(int argCount, Value* args);

#endif
