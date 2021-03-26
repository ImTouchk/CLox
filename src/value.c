#include "object.h"
#include "common.h"
#include "memory.h"
#include "value.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void print_value(Value value)
{
    switch (value.type) {
    case VALUE_NIL: printf("nil"); break;
    case VALUE_BOOL: printf(AS_BOOL(value) ? "true" : "false"); break;
    case VALUE_NUMBER: printf("%g", AS_NUMBER(value)); break;
    case VALUE_OBJECT: print_object(value); break;
    }
}

bool values_equal(Value a, Value b)
{
    if (a.type != b.type) return false;

    switch (a.type) {
    case VALUE_NIL:     return true;
    case VALUE_BOOL:    return AS_BOOL(a) == AS_BOOL(b);
    case VALUE_NUMBER:  return AS_NUMBER(a) == AS_NUMBER(b);
    case VALUE_OBJECT:  return AS_OBJECT(a) == AS_OBJECT(b);
    default: return false;
    }
}

void init_value_array(ValueArray* array)
{
    array->count = 0;
    array->capacity = 0;
    array->values = NULL;
}

void write_value_array(ValueArray* array, Value value)
{
    if (array->capacity < array->count + 1) {
        int old_c = array->capacity;
        array->capacity = GROW_CAPACITY(old_c);
        array->values = GROW_ARRAY(Value, array->values, old_c, array->capacity);
    }

    array->values[array->count] = value;
    array->count++;
}

void free_value_array(ValueArray* array)
{
    FREE_ARRAY(Value, array->values, array->capacity);
    init_value_array(array);
}
