#ifndef MEMORY_H
#define MEMORY_H

#include "common.h"
#include "object.h"

#define GROW_CAPACITY(capacity) ((capacity) < 8 ? 8 : (capacity * 2))
#define GROW_ARRAY(type, pointer, old_c, new_c) \
        (type*)reallocate(pointer, sizeof(type) * old_c, sizeof(type) * new_c)
#define FREE_ARRAY(type, pointer, old_c) \
        reallocate(pointer, sizeof(type) * old_c, 0)
#define FREE(type, pointer) reallocate(pointer, sizeof(type), 0);
#define ALLOCATE(type, count) \
        (type*)reallocate(NULL, 0, sizeof(type) * count)

void free_objects();
void* reallocate(void* pointer, size_t old_size, size_t new_size);
void mark_object(Object* object);
void mark_value(Value value);
void collect_garbage();

#endif
