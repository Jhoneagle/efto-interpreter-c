#ifndef clox_builtins_h
#define clox_builtins_h

#include "object.h"
#include "value.h"

// Registers all built-in methods, functions, and modules.
// Called once from initVM().
void registerBuiltins(void);

// Returns the type name of a value as an ObjString*.
// Used by both the type() function and the typeof operator.
ObjString* typeOfValue(Value value);

#endif
