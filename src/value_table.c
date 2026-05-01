#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "value_table.h"

#define TABLE_MAX_LOAD 0.75

void initValueTable(ValueTable* table) {
  table->count = 0;
  table->capacity = 0;
  table->entries = NULL;
}

void freeValueTable(ValueTable* table) {
  FREE_ARRAY(ValueEntry, table->entries, table->capacity);
  initValueTable(table);
}

uint32_t hashValue(Value value) {
#ifdef NAN_BOXING
  if (IS_NIL(value)) return 0;
  if (IS_BOOL(value)) return AS_BOOL(value) ? 1 : 0;
  if (IS_NUMBER(value)) {
    double num = AS_NUMBER(value);
    uint64_t bits;
    memcpy(&bits, &num, sizeof(bits));
    return (uint32_t)(bits ^ (bits >> 32));
  }
  if (IS_OBJ(value)) {
    Obj* obj = AS_OBJ(value);
    if (obj->type == OBJ_STRING) {
      return ((ObjString*)obj)->hash;
    }
    // Identity hash for other objects.
    uintptr_t ptr = (uintptr_t)obj;
    return (uint32_t)(ptr ^ (ptr >> 16));
  }
  return 0;
#else
  switch (value.type) {
    case VAL_NIL: return 0;
    case VAL_BOOL: return value.as.boolean ? 1 : 0;
    case VAL_NUMBER: {
      uint64_t bits;
      memcpy(&bits, &value.as.number, sizeof(bits));
      return (uint32_t)(bits ^ (bits >> 32));
    }
    case VAL_OBJ: {
      if (value.as.obj->type == OBJ_STRING) {
        return ((ObjString*)value.as.obj)->hash;
      }
      // Identity hash for other objects.
      uintptr_t ptr = (uintptr_t)value.as.obj;
      return (uint32_t)(ptr ^ (ptr >> 16));
    }
  }
  return 0;
#endif
}

static ValueEntry* findEntry(ValueEntry* entries, int capacity,
                             Value key) {
  uint32_t index = hashValue(key) & (capacity - 1);
  ValueEntry* tombstone = NULL;

  for (;;) {
    ValueEntry* entry = &entries[index];
    if (!entry->occupied) {
      if (IS_NIL(entry->value)) {
        // Empty entry.
        return tombstone != NULL ? tombstone : entry;
      } else {
        // Tombstone.
        if (tombstone == NULL) tombstone = entry;
      }
    } else if (valuesEqual(entry->key, key)) {
      return entry;
    }

    index = (index + 1) & (capacity - 1);
  }
}

bool valueTableGet(ValueTable* table, Value key, Value* value) {
  if (table->count == 0) return false;

  ValueEntry* entry = findEntry(table->entries, table->capacity, key);
  if (!entry->occupied) return false;

  *value = entry->value;
  return true;
}

static void adjustCapacity(ValueTable* table, int capacity) {
  ValueEntry* entries = ALLOCATE(ValueEntry, capacity);
  for (int i = 0; i < capacity; i++) {
    entries[i].key = NIL_VAL;
    entries[i].value = NIL_VAL;
    entries[i].occupied = false;
  }

  table->count = 0;
  for (int i = 0; i < table->capacity; i++) {
    ValueEntry* entry = &table->entries[i];
    if (!entry->occupied) continue;

    ValueEntry* dest = findEntry(entries, capacity, entry->key);
    dest->key = entry->key;
    dest->value = entry->value;
    dest->occupied = true;
    table->count++;
  }

  FREE_ARRAY(ValueEntry, table->entries, table->capacity);
  table->entries = entries;
  table->capacity = capacity;
}

bool valueTableSet(ValueTable* table, Value key, Value value) {
  if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
    int capacity = GROW_CAPACITY(table->capacity);
    adjustCapacity(table, capacity);
  }

  ValueEntry* entry = findEntry(table->entries, table->capacity, key);
  bool isNewKey = !entry->occupied;

  if (isNewKey && IS_NIL(entry->value)) table->count++;

  entry->key = key;
  entry->value = value;
  entry->occupied = true;
  return isNewKey;
}

bool valueTableDelete(ValueTable* table, Value key) {
  if (table->count == 0) return false;

  ValueEntry* entry = findEntry(table->entries, table->capacity, key);
  if (!entry->occupied) return false;

  // Place a tombstone.
  entry->key = NIL_VAL;
  entry->value = BOOL_VAL(true);
  entry->occupied = false;
  return true;
}

void markValueTable(ValueTable* table) {
  for (int i = 0; i < table->capacity; i++) {
    ValueEntry* entry = &table->entries[i];
    if (entry->occupied) {
      markValue(entry->key);
      markValue(entry->value);
    }
  }
}
