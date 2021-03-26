#include "compiler.h"
#include "memory.h"
#include "object.h"
#include "common.h"
#include "debug.h"
#include "value.h"
#include "vm.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

VM vm;

static Value clock_native(int arg_count, Value* args)
{
    return NUMBER_VALUE((double)clock() / CLOCKS_PER_SEC);
}

static void reset_stack()
{
    vm.open_upvalues = NULL;
    vm.stack_top     = vm.stack;
    vm.frame_count   = 0;
}

void stack_push(Value value)
{
    *vm.stack_top = value;
    vm.stack_top++;
}

Value stack_pop()
{
    vm.stack_top--;
    return *vm.stack_top;
}

static void runtime_error(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm.frame_count - 1; i >= 0; i--) {
        CallFrame* frame = &vm.frames[i];
        ObjectFunction* function = frame->closure->function;
        // -1 because the IP is sitting on the next instruction to be executed.
        size_t instruction = frame->ip - function->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }

    reset_stack();
}

static void define_native(const char* name, NativeFn function)
{
    stack_push(OBJECT_VALUE(copy_string(name, (int)strlen(name))));
    stack_push(OBJECT_VALUE(new_native(function)));
    table_set(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
    stack_pop();
    stack_pop();
}

static Value peek(int distance)
{
    return vm.stack_top[-1 - distance];
}

static bool call(ObjectClosure* closure, int arg_count)
{
    if (arg_count != closure->function->arity) {
        runtime_error("Expected %d arguments but got %d instead.", closure->function->arity, arg_count);
        return false;
    }

    if (vm.frame_count == FRAMES_MAX) {
        runtime_error("Stack overflow.");
        return false;
    }

    CallFrame* frame = &vm.frames[vm.frame_count++];
    frame->closure  = closure;
    frame->slots    = vm.stack_top - arg_count - 1;
    frame->ip       = closure->function->chunk.code;
    return true;
}

static bool call_value(Value callee, int arg_count)
{
    if (IS_OBJECT(callee)) {
        switch (OBJECT_TYPE(callee)) {
        case OBJECT_BOUND_METHOD: {
            ObjectBoundMethod* bound = AS_BOUND_METHOD(callee);
            vm.stack_top[-arg_count - 1] = bound->reciever;
            return call(bound->method, arg_count);
        }
        case OBJECT_CLASS: {
            ObjectClass* klass = AS_CLASS(callee);
            vm.stack_top[-arg_count - 1] = OBJECT_VALUE(new_instance(klass));
            
            Value initializer;
            if (table_get(&klass->methods, vm.init_string, &initializer)) {
                return call(AS_CLOSURE(initializer), arg_count);
            } else if(arg_count != 0) {
                runtime_error("Expected 0 arguments but got %d.", arg_count);
            }
            return true;
        }

        case OBJECT_CLOSURE: {
            return call(AS_CLOSURE(callee), arg_count);
        }
        case OBJECT_NATIVE: {
            NativeFn native = AS_NATIVE(callee);
            Value result = native(arg_count, vm.stack_top - arg_count);
            vm.stack_top -= arg_count + 1;
            stack_push(result);
            return true;
        }
        default: break;
        }
    } 

    runtime_error("You can only call functions and classes.");
    return false;
}

static bool invoke_from_class(ObjectClass* klass, ObjectString* name, int arg_count)
{
    Value method;
    if (!table_get(&klass->methods, name, &method)) {
        runtime_error("Undefined property '%s'.", name->chars);
        return false;
    }

    return call(AS_CLOSURE(method), arg_count);
}

static bool invoke(ObjectString* name, int arg_count)
{
    Value reciever = peek(arg_count);

    if (!IS_INSTANCE(reciever)) {
        runtime_error("Only instances have methods.");
        return false;
    }

    ObjectInstance* instance = AS_INSTANCE(reciever);

    Value value;
    if (table_get(&instance->fields, name, &value)) {
        vm.stack_top[-arg_count - 1] = value;
        return call_value(value, arg_count);
    }

    return invoke_from_class(instance->klass, name, arg_count);
}

static bool bind_method(ObjectClass* klass, ObjectString* name)
{
    Value method;
    if (!table_get(&klass->methods, name, &method)) {
        runtime_error("Undefined property '%s'.", name->chars);
        return false;
    }

    ObjectBoundMethod* bound = new_bound_method(peek(0), AS_CLOSURE(method));
    stack_pop();
    stack_push(OBJECT_VALUE(bound));
    return true;
}

static ObjectUpvalue* capture_upvalue(Value* local)
{
    ObjectUpvalue* previous = NULL;
    ObjectUpvalue* upvalue  = vm.open_upvalues;

    while (upvalue != NULL && upvalue->location > local) {
        previous = upvalue;
        upvalue  = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    ObjectUpvalue* created_upvalue = new_upvalue(local);
    created_upvalue->next = upvalue;

    if (previous == NULL) {
        vm.open_upvalues = created_upvalue;
    } else {
        previous->next = created_upvalue;
    }
    return created_upvalue;
}

static void close_upvalues(Value* last)
{
    while (vm.open_upvalues != NULL && vm.open_upvalues->location >= last) {
        ObjectUpvalue* upvalue = vm.open_upvalues;
        upvalue->closed   = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm.open_upvalues  = upvalue->next;
    }
}

static void define_method(ObjectString* name)
{
    Value method = peek(0);
    ObjectClass* klass = AS_CLASS(peek(1));
    table_set(&klass->methods, name, method);
    stack_pop();
}

static bool is_falsey(Value value)
{
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate()
{
    ObjectString* b = AS_STRING(peek(0));
    ObjectString* a = AS_STRING(peek(1));

    int length = a->length + b->length;
    char* chars = ALLOCATE(char, length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjectString* result = take_string(chars, length);
    stack_pop(); 
    stack_pop();
    stack_push(OBJECT_VALUE(result));
}

static InterpretResult run()
{
    CallFrame* frame = &vm.frames[vm.frame_count - 1];

#   define READ_BYTE()     (*frame->ip++)
#   define READ_SHORT()    ( frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#   define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
#   define READ_STRING() AS_STRING(READ_CONSTANT())
#   define BINARY_OP(valueType, op) \
    do { \
        if(!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
            runtime_error("Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        double b = AS_NUMBER(stack_pop()); \
        double a = AS_NUMBER(stack_pop()); \
        stack_push(valueType(a op b)); \
    } while(false)

    for (;;) {
#       ifdef DEBUG_TRACE_EXECUTION
        printf("          ");
        for (Value* slot = vm.stack + 1; slot < vm.stack_top; slot++) {
            printf("[ ");
            print_value(*slot);
            printf(" ]");
        }
        printf("\n");
        disassemble_instruction(&frame->closure->function->chunk, (int)(frame->ip - frame->closure->function->chunk.code));
#       endif

        uint8_t instr;
        switch (instr = READ_BYTE()) {

        case OP_ADD: {
            if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
                concatenate();
            } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                double b = AS_NUMBER(stack_pop());
                double a = AS_NUMBER(stack_pop());
                stack_push(NUMBER_VALUE(a + b));
            } else {
                runtime_error("Operands must be either 2 numbers or 2 strings.");
                return INTERPRET_RUNTIME_ERROR;
            }
            break; 
        }
        case OP_SUBTRACT: { BINARY_OP(NUMBER_VALUE, -); break; }
        case OP_MULTIPLY: { BINARY_OP(NUMBER_VALUE, *); break; }
        case OP_DIVIDE:   { BINARY_OP(NUMBER_VALUE, /); break; }
        case OP_MODULO: {
            if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtime_error("Operands must be numbers.");
                    return INTERPRET_RUNTIME_ERROR;
            }
            int b = (int)AS_NUMBER(stack_pop());
            int a = (int)AS_NUMBER(stack_pop());
            stack_push(NUMBER_VALUE(a % b));
            break;
        }
        case OP_NEGATE:   { 
            if (!IS_NUMBER(peek(0))) {
                runtime_error("Operand must be a number.");
                return INTERPRET_RUNTIME_ERROR;
            }

            stack_push(NUMBER_VALUE(-AS_NUMBER(stack_pop()))); 
            break;
        }
        case OP_CONSTANT: { stack_push(READ_CONSTANT()); break; }
        case OP_NIL:   { stack_push(NIL_VALUE); break; }
        case OP_TRUE:  { stack_push(BOOL_VALUE(true)); break; }
        case OP_FALSE: { stack_push(BOOL_VALUE(false)); break; }
        case OP_NOT: {
            stack_push(BOOL_VALUE(is_falsey(stack_pop())));
            break;
        }

        case OP_POP: { stack_pop(); break; }
        case OP_EQUAL: {
            Value b = stack_pop();
            Value a = stack_pop();
            stack_push(BOOL_VALUE(values_equal(a, b)));
            break;
        }

        case OP_GREATER: { BINARY_OP(BOOL_VALUE, >); break; }
        case OP_LESS:    { BINARY_OP(BOOL_VALUE, <); break; }

        case OP_LOOP: {
            uint16_t offset = READ_SHORT();
            frame->ip -= offset;
            break;
        }

        case OP_RETURN: {
            Value result = stack_pop();
            
            close_upvalues(frame->slots);

            vm.frame_count--;
            if (vm.frame_count == 0) {
                stack_pop();
                return INTERPRET_OK;
            }

            vm.stack_top = frame->slots;
            stack_push(result);

            frame = &vm.frames[vm.frame_count - 1];
            break;
        }
        case OP_CALL: {
            int arg_count = READ_BYTE();
            if (!call_value(peek(arg_count), arg_count)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }
        case OP_JUMP: {
            uint16_t offset = READ_SHORT();
            frame->ip += offset;
            break;
        }

        case OP_JUMP_IF_FALSE: {
            uint16_t offset = READ_SHORT();
            if (is_falsey(peek(0))) 
                frame->ip += offset;
            break;
        }

        case OP_PRINT: {
            print_value(stack_pop());
            printf("\n");
            break;
        }
        
        case OP_DEFINE_GLOBAL: {
            ObjectString* name = READ_STRING();
            table_set(&vm.globals, name, peek(0));
            break;
        }
        
        case OP_GET_LOCAL: {
            uint8_t slot = READ_BYTE();
            stack_push(frame->slots[slot]);
            break;
        }

        case OP_SET_LOCAL: {
            uint8_t slot = READ_BYTE();
            frame->slots[slot] = peek(0);
            break;
        }

        case OP_SET_GLOBAL: {
            ObjectString* name = READ_STRING();
            if (table_set(&vm.globals, name, peek(0))) {
                table_del(&vm.globals, name);
                runtime_error("Undefined variable '%s'.\n", name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_GET_GLOBAL: {
            ObjectString* name = READ_STRING();
            Value value;
            if (!table_get(&vm.globals, name, &value)) {
                runtime_error("Undefined variable '%s'.\n", name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            stack_push(value);
            break;
        }

        case OP_GET_UPVALUE: {
            uint8_t slot = READ_BYTE();
            stack_push(*frame->closure->upvalues[slot]->location);
            break;
        }

        case OP_SET_UPVALUE: {
            uint8_t slot = READ_BYTE();
            *frame->closure->upvalues[slot]->location = peek(0);
            break;
        }

        case OP_CLOSE_UPVALUE: {
            close_upvalues(vm.stack_top - 1);
            stack_pop();
            break;
        }

        case OP_CLOSURE: {
            ObjectFunction* function = AS_FUNCTION(READ_CONSTANT());
            ObjectClosure* closure = new_closure(function);
            stack_push(OBJECT_VALUE(closure));
            for (int i = 0; i < closure->upvalue_count; i++) {
                uint8_t is_local = READ_BYTE();
                uint8_t index    = READ_BYTE();
                if (is_local) {
                    closure->upvalues[i] = capture_upvalue(frame->slots + index);
                } else {
                    closure->upvalues[i] = frame->closure->upvalues[index];
                }
            }
            break;
        }

        case OP_SET_PROPERTY: {
            if (!IS_INSTANCE(peek(1))) {
                runtime_error("Only instances have fields.");
                return INTERPRET_RUNTIME_ERROR;
            }

            ObjectInstance* instance = AS_INSTANCE(peek(1));
            table_set(&instance->fields, READ_STRING(), peek(0));

            Value value = stack_pop();
            stack_pop();
            stack_push(value);
            break;
        }

        case OP_GET_PROPERTY: {
            if (!IS_INSTANCE(peek(0))) {
                runtime_error("Only instances have properties.");
                return INTERPRET_RUNTIME_ERROR;
            }

            ObjectInstance* instance = AS_INSTANCE(peek(0));
            ObjectString* name       = READ_STRING();

            Value value;
            if (table_get(&instance->fields, name, &value)) {
                stack_pop();
                stack_push(value);
                break;
            }

            if (!bind_method(instance->klass, name)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }

        case OP_GET_SUPER: {
            ObjectString* name = READ_STRING();
            ObjectClass*  super_class = AS_CLASS(stack_pop());
            if (!bind_method(super_class, name)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }

        case OP_CLASS: {
            stack_push(OBJECT_VALUE(new_class(READ_STRING())));
            break;
        }

        case OP_METHOD: {
            define_method(READ_STRING());
            break;
        }

        case OP_INVOKE: {
            ObjectString* method = READ_STRING();
            int arg_count        = READ_BYTE();
            if (!invoke(method, arg_count)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        case OP_INHERIT: {
            Value superclass = peek(1);
            if (!IS_CLASS(superclass)) {
                runtime_error("Superclass must be a class.");
                return INTERPRET_RUNTIME_ERROR;
            }

            ObjectClass* subclass = AS_CLASS(peek(0));
            table_add_all(&AS_CLASS(superclass)->methods, &subclass->methods);
            stack_pop(); // subclass
            break;
        }

        case OP_SUPER_INVOKE: {
            ObjectString* method = READ_STRING();
            int arg_count        = READ_BYTE();
            ObjectClass* super_class = AS_CLASS(stack_pop());
            if (!invoke_from_class(super_class, method, arg_count)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }
        }
    }

#   undef BINARY_OP
#   undef READ_BYTE
#   undef READ_SHORT
#   undef READ_STRING
#   undef READ_CONSTANT
}

void init_vm()
{
    reset_stack();
    vm.bytes_allocated = 0;
    vm.gray_capacity   = 0;
    vm.gray_count      = 0;
    vm.gray_stack      = NULL;
    vm.objects         = NULL;
    vm.next_gc         = 1024 * 1024;
    init_table(&vm.strings);
    init_table(&vm.globals);

    vm.init_string = NULL;
    vm.init_string = copy_string("init", 4);

    define_native("clock", clock_native);
}

void free_vm()
{   
    free_table(&vm.globals);
    free_table(&vm.strings);

    vm.init_string = NULL;

    free_objects();
}

InterpretResult interpret(const char* source)
{
    ObjectFunction* function = compile(source);
    if (function == NULL) {
        return INTERPRET_COMPILE_ERROR;
    }

    //stack_push(OBJECT_VALUE(function));
    //call_value(OBJECT_VALUE(function), 0);

    ObjectClosure* closure = new_closure(function);
    stack_pop();
    stack_push(OBJECT_VALUE(closure));
    call_value(OBJECT_VALUE(closure), 0);

    return run();
}
