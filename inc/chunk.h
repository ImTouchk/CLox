#ifndef CHUNK_H
#define CHUNK_H

#include "common.h"
#include "value.h"

#define UINT8_COUNT (UINT8_MAX + 1)

typedef enum {
    OP_CALL,
    OP_INVOKE,
    OP_SUPER_INVOKE,
    OP_RETURN,

    OP_POP,
    OP_PRINT,

    OP_LOOP,
    OP_JUMP,
    OP_JUMP_IF_FALSE,

    OP_INHERIT,
    OP_CLASS,
    OP_METHOD,
    OP_CLOSURE,
    OP_CONSTANT,
    
    OP_SET_LOCAL,
    OP_SET_GLOBAL,
    OP_SET_UPVALUE,
    OP_SET_PROPERTY,

    OP_GET_LOCAL,
    OP_GET_GLOBAL,
    OP_GET_UPVALUE,
    OP_GET_PROPERTY,
    OP_GET_SUPER,

    OP_CLOSE_UPVALUE,
    OP_DEFINE_GLOBAL,

    OP_NIL,
    OP_TRUE,
    OP_FALSE,

    OP_NOT,
    OP_ADD,
    OP_NEGATE,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_MODULO,
    OP_DIVIDE,
    OP_EQUAL,
    OP_LESS,
    OP_GREATER,
} OPCode;

typedef struct {
    int count;
    int capacity;
    int* lines;
    uint8_t* code;
    ValueArray constants;
} Chunk;

void init_chunk(Chunk* chunk);
int add_constant(Chunk* chunk, Value value);
void write_chunk(Chunk* chunk, uint8_t byte, int line);
void free_chunk(Chunk* chunk);

#endif
