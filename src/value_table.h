#ifndef clox_value_table_h
#define clox_value_table_h

#include "common.h"
#include "value.h"

typedef struct {
  Value key;
  Value value;
  bool occupied;
} ValueEntry;

typedef struct {
  int count;
  int capacity;
  ValueEntry* entries;
} ValueTable;

void initValueTable(ValueTable* table);
void freeValueTable(ValueTable* table);
bool valueTableGet(ValueTable* table, Value key, Value* value);
bool valueTableSet(ValueTable* table, Value key, Value value);
bool valueTableDelete(ValueTable* table, Value key);
void markValueTable(ValueTable* table);
uint32_t hashValue(Value value);

#endif
