#include "object.h"
#include "memory.h"
#include "value.h"
#include "table.h"

#include <string.h>

#define TABLE_MAX_LOAD 0.75

static Entry* find_entry(Entry* entries, int capacity, ObjectString* key)
{
    uint32_t index = key->hash % capacity;
    Entry* tombstone = NULL;
    
    for (;;) {
        Entry* entry = &entries[index];
        if (entry->key == NULL) {
            if (IS_NIL(entry->value)) {
                // Empty entry.
                return tombstone != NULL ? tombstone : entry;
            }
            else {
                // We found a tombstone.
                if (tombstone == NULL) tombstone = entry;
            }
        }
        else if (entry->key == key) {
            // We found the key.
            return entry;
        }
        index = (index + 1) % capacity;
    }
}

static void adjust_capacity(Table* table, int capacity)
{
    Entry* entries = ALLOCATE(Entry, capacity);
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NULL;
        entries[i].value = NIL_VALUE;
    }
    
    table->count = 0;
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key == NULL) continue;

        Entry* dest = find_entry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }

    FREE_ARRAY(Entry, table->entries, table->capacity);
    table->entries = entries;
    table->capacity = capacity;
}

void init_table(Table* table)
{
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void free_table(Table* table)
{
    FREE_ARRAY(Entry, table->entries, table->capacity);
    init_table(table);
}

void mark_table(Table* table)
{
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        mark_object((Object*)entry->key);
        mark_value(entry->value);
    }
}

void table_add_all(Table* from, Table* to)
{
    for (int i = 0; i < from->capacity; i++) {
        Entry* entry = &from->entries[i];
        if (entry->key != NULL) {
            table_set(to, entry->key, entry->value);
        }
    }
}

bool table_del(Table* table, ObjectString* key)
{
    if (table->count == 0) return false;

    // Find the entry.
    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;

    // Place a tombstone in the entry.
    entry->key = NULL;
    entry->value = BOOL_VALUE(true);

    return true;
}

bool table_get(Table* table, ObjectString* key, Value* value)
{
    if (table->count == 0) return false;
    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;
    *value = entry->value;
    return true;
}

bool table_set(Table* table, ObjectString* key, Value value)
{
    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
        int capacity = GROW_CAPACITY(table->capacity);
        adjust_capacity(table, capacity);
    }

    Entry* entry = find_entry(table->entries, table->capacity, key);
    bool is_new_key = entry->key == NULL;
    if (is_new_key && IS_NIL(entry->value)) table->count++;
    entry->key = key;
    entry->value = value;
    return is_new_key;
}

ObjectString* table_find_string(Table* table, const char* chars, int length, uint32_t hash)
{
    if (table->count == 0) return NULL;

    uint32_t index = hash % table->capacity;

    for (;;) {
        Entry* entry = &table->entries[index];

        if (entry->key == NULL) {
            // Stop if we find an empty non-tombstone entry.
            if (IS_NIL(entry->value)) return NULL;
        }
        else if (entry->key->length == length &&
            entry->key->hash == hash &&
            memcmp(entry->key->chars, chars, length) == 0) {
            // We found it.
            return entry->key;
        }

        index = (index + 1) % table->capacity;
    }
}

void table_remove_white(Table* table)
{
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL && !entry->key->object.is_marked)
            table_del(table, entry->key);
    }
}
