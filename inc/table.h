#ifndef TABLE_H
#define TABLE_H

#include "common.h"
#include "value.h"

struct ObjectString;

typedef struct {
    ObjectString* key;
    Value value;
} Entry;

typedef struct {
    int count;
    int capacity;
    Entry* entries;
} Table;

void table_add_all(Table* from, Table* to);
bool table_del(Table* table, ObjectString* key);
bool table_get(Table* table, ObjectString* key, Value* value);
bool table_set(Table* table, ObjectString* key, Value value);
void init_table(Table* table);
void free_table(Table* table);
void mark_table(Table* table);

ObjectString* table_find_string(Table* table, const char* chars, int length, uint32_t hash);
void table_remove_white(Table* table);

#endif
