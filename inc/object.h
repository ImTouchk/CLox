#ifndef OBJECT_H
#define OBJECT_H

#include "common.h"
#include "value.h"
#include "chunk.h"
#include "table.h"

typedef enum {
    OBJECT_CLASS,
    OBJECT_STRING,
    OBJECT_NATIVE,
    OBJECT_FUNCTION,
    OBJECT_INSTANCE,
    OBJECT_CLOSURE,
    OBJECT_UPVALUE,
    OBJECT_BOUND_METHOD,
} ObjectType;

struct Object {
    ObjectType type;
    bool is_marked;
    struct Object* next;
};

typedef Value (*NativeFn)(int arg_count, Value* args);

typedef struct {
    Object object;
    NativeFn function;
} ObjectNative;

struct ObjectString {
    Object object;
    int length;
    char* chars;
    uint32_t hash;
};

typedef struct {
    Object object;
    Value* location;
    Value closed;
    struct ObjectUpvalue* next;
} ObjectUpvalue;

typedef struct {
    Object object;
    int arity;
    int upvalue_count;
    Chunk chunk;
    ObjectString* name;
} ObjectFunction;

typedef struct {
    Object object;
    ObjectFunction* function;
    ObjectUpvalue** upvalues;
    int upvalue_count;
} ObjectClosure;

typedef struct {
    Object object;
    ObjectString* name;
    Table methods;
} ObjectClass;

typedef struct {
    Object object;
    ObjectClass* klass;
    Table fields;
} ObjectInstance;

typedef struct {
    Object object;
    Value reciever;
    ObjectClosure* method;
} ObjectBoundMethod;

void print_object(Value value);

ObjectFunction*    new_function();
ObjectUpvalue*     new_upvalue(Value* slot);
ObjectClosure*     new_closure(ObjectFunction* function);
ObjectNative*      new_native(NativeFn function);
ObjectClass*       new_class(ObjectString* name);
ObjectInstance*    new_instance(ObjectClass* klass);
ObjectBoundMethod* new_bound_method(Value reciever, ObjectClosure* method);

ObjectString* take_string(char* chars, int length);
ObjectString* copy_string(const char* chars, int length);

static inline bool is_object_type(Value value, ObjectType type)
{
    return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

#define OBJECT_TYPE(value) (AS_OBJECT(value)->type)

#define IS_CLASS(value)        is_object_type(value, OBJECT_CLASS)
#define IS_STRING(value)       is_object_type(value, OBJECT_STRING)
#define IS_NATIVE(value)       is_object_type(value, OBJECT_NATIVE)
#define IS_CLOSURE(value)      is_object_type(value, OBJECT_CLOSURE)
#define IS_FUNCTION(value)     is_object_type(value, OBJECT_FUNCTION)
#define IS_INSTANCE(value)     is_object_type(value, OBJECT_INSTANCE)
#define IS_BOUND_METHOD(value) is_object_type(value, OBJECT_BOUND_METHOD)

#define AS_CLASS(value)        ((ObjectClass*)AS_OBJECT(value))
#define AS_STRING(value)       ((ObjectString*)AS_OBJECT(value))
#define AS_CLOSURE(value)      ((ObjectClosure*)AS_OBJECT(value))
#define AS_FUNCTION(value)     ((ObjectFunction*)AS_OBJECT(value))
#define AS_CSTRING(value)      (((ObjectString*)AS_OBJECT(value))->chars)
#define AS_NATIVE(value)       (((ObjectNative*)AS_OBJECT(value))->function)
#define AS_INSTANCE(value)     ((ObjectInstance*)AS_OBJECT(value))
#define AS_BOUND_METHOD(value) ((ObjectBoundMethod*)AS_OBJECT(value))

#endif
