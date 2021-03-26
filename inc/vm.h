#ifndef VM_H
#define VM_H

#include "chunk.h"
#include "table.h"
#include "object.h"

#define FRAMES_MAX 64
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)

typedef struct {
    ObjectClosure* closure;
    uint8_t* ip;
    Value* slots;
} CallFrame;

typedef struct {
    int frame_count;
    CallFrame frames[STACK_MAX];
    Value stack[STACK_MAX];
    Value* stack_top;
    Object* objects;
    Table strings;
    Table globals;
    ObjectUpvalue* open_upvalues;
    int gray_count;
    int gray_capacity;
    Object** gray_stack;
    ObjectString* init_string;

    size_t bytes_allocated;
    size_t next_gc;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_RUNTIME_ERROR,
    INTERPRET_COMPILE_ERROR,
} InterpretResult;

void init_vm();
void free_vm();
void stack_push(Value value);
Value stack_pop();
InterpretResult interpret(const char* source);

extern VM vm;

#endif
