#include "compiler.h"
#include "common.h"
#include "memory.h"
#include "object.h"
#include "vm.h"

#ifdef DEBUG_LOG_GC
#include <stdio.h>
#include "debug.h"
#endif

#define GC_HEAP_GROW_FACTOR 2

#include <stdlib.h>

static void free_object(Object* object)
{
#ifdef DEBUG_LOG_GC
    printf("%p free type %d\n", (void*)object, object->type);
#endif

    switch (object->type) {
    case OBJECT_INSTANCE: {
        ObjectInstance* instance = (ObjectInstance*)object;
        free_table(&instance->fields);
        FREE(ObjectInstance, object);
        break;
    }

    case OBJECT_STRING: {
        ObjectString* string = (ObjectString*)object;
        FREE_ARRAY(char, string->chars, string->length + 1);
        FREE(ObjectString, object);
        break;
    }
    case OBJECT_FUNCTION: {
        ObjectFunction* function = (ObjectFunction*)object;
        free_chunk(&function->chunk);
        FREE(ObjectFunction, object);
        break;
    }
    case OBJECT_BOUND_METHOD: {
        FREE(ObjectBoundMethod, object);
        break;
    }

    case OBJECT_CLOSURE: {
        ObjectClosure* closure = (ObjectClosure*)object;
        FREE_ARRAY(ObjectUpvalue*, closure->upvalues, closure->upvalue_count);
        FREE(ObjectClosure, object);
        break;
    }
    case OBJECT_UPVALUE: {
        FREE(ObjectUpvalue, object);
        break;
    }
    case OBJECT_NATIVE: {
        FREE(ObjectNative, object);
        break;
    }
    case OBJECT_CLASS: {
        ObjectClass* klass = (ObjectClass*)object;
        free_table(&klass->methods);
        FREE(ObjectClass, object);
        break;
    }
    }
}

void free_objects()
{
    Object* object = vm.objects;
    while (object != NULL) {
        Object* next = object->next;
        free_object(object);
        object = next;
    }

    free(vm.gray_stack);
}

void* reallocate(void* pointer, size_t old_size, size_t new_size)
{
    vm.bytes_allocated += new_size - old_size;

    if (new_size > old_size) {
#       ifdef DEBUG_STRESS_GC
        collect_garbage();
#       endif

        if (vm.bytes_allocated > vm.next_gc) {
            collect_garbage();
        }
    }

    if (new_size == 0) {
        free(pointer);
        return NULL;
    }

    void* result = realloc(pointer, new_size);
    if (result == NULL) {
        exit(-1);
    }
    return result;
}

void mark_object(Object* object)
{
    if (object == NULL)
        return;

    if (object->is_marked)
        return;

#ifdef DEBUG_LOG_GC
    printf("%p mark ", (void*)object);
    print_value(OBJECT_VALUE(object));
    printf("\n");
#endif

    object->is_marked = true;

    if (vm.gray_capacity < vm.gray_count + 1) {
        vm.gray_capacity = GROW_CAPACITY(vm.gray_capacity);
        vm.gray_stack = (Object**)realloc(vm.gray_stack, sizeof(Object*) * vm.gray_capacity);
    }

    vm.gray_stack[vm.gray_count++] = object;

    if (vm.gray_stack == NULL)
        exit(1);
}

void mark_value(Value value)
{
    if (!IS_OBJECT(value))
        return;
    mark_object(AS_OBJECT(value));
}

static void mark_array(ValueArray* array)
{
    for (int i = 0; i < array->count; i++) {
        mark_value(array->values[i]);
    }
}

static void mark_roots()
{
    for (Value* slot = vm.stack; slot < vm.stack_top; slot++) {
        mark_value(*slot);
    }

    for (int i = 0; i < vm.frame_count; i++) {
        mark_object((Object*)vm.frames[i].closure);
    }

    for (ObjectUpvalue* upvalue = vm.open_upvalues; upvalue != NULL; upvalue = upvalue->next) {
        mark_object((Object*)upvalue);
    }

    mark_table(&vm.globals);
    mark_compiler_roots();
    mark_object((Object*)vm.init_string);
}

static void blacken_object(Object* object)
{
#ifdef DEBUG_LOG_GC
    printf("%p blacken ", (void*)object);
    print_value(OBJECT_VALUE(object));
    printf("\n");
#endif

    switch (object->type) {
    case OBJECT_INSTANCE: {
        ObjectInstance* instance = (ObjectInstance*)object;
        mark_object((Object*)instance->klass);
        mark_table(&instance->fields);
        break;
    }

    case OBJECT_CLOSURE: {
        ObjectClosure* closure = (ObjectClosure*)object;
        mark_object((Object*)closure->function);
        for (int i = 0; i < closure->upvalue_count; i++) {
            mark_object((Object*)closure->upvalues[i]);
        }
        break;
    }

    case OBJECT_FUNCTION: {
        ObjectFunction* function = (ObjectFunction*)object;
        mark_object((Object*)function->name);
        mark_array(&function->chunk.constants);
        break;
    }

    case OBJECT_BOUND_METHOD: {
        ObjectBoundMethod* bound_method = (ObjectBoundMethod*)object;
        mark_value(bound_method->reciever);
        mark_object((Object*)bound_method->method);
        break;
    }

    case OBJECT_UPVALUE: {
        mark_value(((ObjectUpvalue*)object)->closed);
        break;
    }

    case OBJECT_CLASS: {
        ObjectClass* klass = (ObjectClass*)object;
        mark_object((Object*)klass->name);
        mark_table(&klass->methods);
        break;
    }

    case OBJECT_NATIVE:
    case OBJECT_STRING:
        break;
    }
}

static void trace_references()
{
    while (vm.gray_count > 0) {
        Object* object = vm.gray_stack[--vm.gray_count];
        blacken_object(object);
    }
}

static void sweep()
{
    Object* previous = NULL;
    Object* object   = vm.objects;
    while (object != NULL) {
        if (object->is_marked) {
            object->is_marked = false;
            previous = object;
            object   = object->next;
        } else {
            Object* unreached = object;
            object = object->next;
            if (previous != NULL) {
                previous->next = object;
            } else {
                vm.objects = object;
            }

            free_object(unreached);
        }
    }
}

void collect_garbage()
{
#ifdef DEBUG_LOG_GC
    printf("-- gc begin\n");
    size_t before = vm.bytes_allocated;

#endif

    mark_roots();
    trace_references();
    table_remove_white(&vm.strings);
    sweep();

    vm.next_gc = vm.bytes_allocated * GC_HEAP_GROW_FACTOR;

#ifdef DEBUG_LOG_GC
    printf("-- gc end\n");
    printf("   collected %zu bytes (from %zu to %zu) next at %zu\n", before - vm.bytes_allocated, before, vm.bytes_allocated, vm.next_gc);
#endif
}
