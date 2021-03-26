#ifndef VALUE_H
#define VALUE_H

#include "common.h"

typedef struct Object Object;
typedef struct ObjectString ObjectString;

typedef enum {
    VALUE_NIL,
    VALUE_BOOL,
    VALUE_NUMBER,
    VALUE_OBJECT,
} ValueType;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        double number;
        Object* object;
    } as;
} Value;

#define IS_NIL(value)  ((value).type == VALUE_NIL)
#define IS_BOOL(value) ((value).type == VALUE_BOOL)
#define IS_NUMBER(value) ((value).type == VALUE_NUMBER)
#define IS_OBJECT(value) ((value).type == VALUE_OBJECT)

#define NIL_VALUE           ((Value){VALUE_NIL, { .number = 0 }})
#define BOOL_VALUE(value)   ((Value){VALUE_BOOL, { .boolean = value }})
#define NUMBER_VALUE(value) ((Value){VALUE_NUMBER, { .number = value }})
#define OBJECT_VALUE(value) ((Value){VALUE_OBJECT, { .object = (Object*)value }})

#define AS_BOOL(value)   ((value).as.boolean)
#define AS_NUMBER(value) ((value).as.number)
#define AS_OBJECT(value) ((value).as.object)

typedef struct {
    int count;
    int capacity;
    Value* values;
} ValueArray;

bool values_equal(Value a, Value b);
void print_value(Value value);
void init_value_array(ValueArray* array);
void write_value_array(ValueArray* array, Value value);
void free_value_array(ValueArray* array);

#endif
