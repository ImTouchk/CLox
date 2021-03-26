#include "vm.h"
#include "object.h"
#include "table.h"
#include "memory.h"

#include <stdio.h>
#include <string.h>

#define ALLOCATE_OBJECT(type, objectType) \
        (type*)allocate_object(sizeof(type), objectType)

static Object* allocate_object(size_t size, ObjectType type)
{
    Object* object = (Object*)reallocate(NULL, 0, size);
    object->is_marked = false;
    object->next      = vm.objects;
    object->type      = type;
    vm.objects = object;

#ifdef DEBUG_LOG_GC
    printf("%p allocate %zu for %d\n", (void*)object, size, type);
#endif

    return object;
}

static ObjectString* allocate_string(char* chars, int length, uint32_t hash)
{
    ObjectString* string = ALLOCATE_OBJECT(ObjectString, OBJECT_STRING);
    string->length = length;
    string->chars = chars;
    string->hash = hash;

    stack_push(OBJECT_VALUE(string));
    table_set(&vm.strings, string, NIL_VALUE);
    stack_pop();

    return string;
}

static uint32_t hash_string(const char* key, int length)
{
    uint32_t hash = 2166136261u;

    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }

    return hash;
}

ObjectString* take_string(char* chars, int length)
{
    uint32_t hash = hash_string(chars, length);
    ObjectString* interned = table_find_string(&vm.strings, chars, length, hash);
    if (interned != NULL) {
        FREE_ARRAY(char, chars, length + 1);
        return interned;
    }
    return allocate_string(chars, length, hash);
}

ObjectString* copy_string(const char* chars, int length)
{
    uint32_t hash = hash_string(chars, length);
    ObjectString* interned = table_find_string(&vm.strings, chars, length, hash);
    if (interned != NULL) return interned;

    char* heap_chars = ALLOCATE(char, length + 1);
    memcpy(heap_chars, chars, length);
    heap_chars[length] = '\0';
    return allocate_string(heap_chars, length, hash);
}

ObjectFunction* new_function()
{
    ObjectFunction* function = ALLOCATE_OBJECT(ObjectFunction, OBJECT_FUNCTION);
    function->arity = 0;
    function->name = NULL;
    function->upvalue_count = 0;
    init_chunk(&function->chunk);
    return function;
}

ObjectUpvalue* new_upvalue(Value* slot)
{
    ObjectUpvalue* upvalue = ALLOCATE_OBJECT(ObjectUpvalue, OBJECT_UPVALUE);
    upvalue->location = slot;
    upvalue->next     = NULL;
    upvalue->closed   = NIL_VALUE;
    return upvalue;
}

ObjectClosure* new_closure(ObjectFunction* function)
{
    ObjectUpvalue** upvalues = ALLOCATE(ObjectUpvalue*, function->upvalue_count);
    for (int i = 0; i < function->upvalue_count; i++) {
        upvalues[i] = NULL;
    }

    ObjectClosure* closure = ALLOCATE_OBJECT(ObjectClosure, OBJECT_CLOSURE);
    closure->upvalue_count = function->upvalue_count;
    closure->function      = function;
    closure->upvalues      = upvalues;
    return closure;
}

ObjectClass* new_class(ObjectString* name)
{
    ObjectClass* klass = ALLOCATE_OBJECT(ObjectClass, OBJECT_CLASS);
    klass->name = name;
    init_table(&klass->methods);
    return klass;
}

ObjectInstance* new_instance(ObjectClass* klass)
{
    ObjectInstance* instance = ALLOCATE_OBJECT(ObjectInstance, OBJECT_INSTANCE);
    instance->klass = klass;
    init_table(&instance->fields);
    return instance;
}

ObjectBoundMethod* new_bound_method(Value reciever, ObjectClosure* method)
{
    ObjectBoundMethod* bound_method = ALLOCATE_OBJECT(ObjectBoundMethod, OBJECT_BOUND_METHOD);
    bound_method->reciever = reciever;
    bound_method->method   = method;
    return bound_method;
}

ObjectNative* new_native(NativeFn function)
{
    ObjectNative* native = ALLOCATE_OBJECT(ObjectNative, OBJECT_NATIVE);
    native->function = function;
    return native;
}

static void print_function(ObjectFunction* function)
{
    if (function->name == NULL) {
        printf("<script>");
        return;
    }

    printf("<fn %s>", function->name->chars);
}

void print_object(Value value)
{
    switch (OBJECT_TYPE(value)) {
    case OBJECT_INSTANCE: {
        printf("<instance of %s>", AS_INSTANCE(value)->klass->name->chars);
        break;
    }
    case OBJECT_FUNCTION: {
        print_function(AS_FUNCTION(value));
        break;
    }
    case OBJECT_BOUND_METHOD: {
        print_function(AS_BOUND_METHOD(value)->method->function);
        break;
    }
    case OBJECT_CLOSURE: {
        print_function(AS_CLOSURE(value)->function);
        break;
    }
    case OBJECT_CLASS: {
        printf("<class %s>", AS_CLASS(value)->name->chars);
        break;
    }
    case OBJECT_UPVALUE: {
        printf("upvalue");
        break;
    }
    case OBJECT_NATIVE: printf("<native fn>"); break;
    case OBJECT_STRING: printf("%s", AS_CSTRING(value)); break;
    default: printf("UNKNOWN_OBJECT"); break;
    }
}
