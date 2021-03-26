#ifndef DEBUG_H
#define DEBUG_H

#include "chunk.h"

int disassemble_instruction(Chunk* chunk, int offset);
void disassemble_chunk(Chunk* chunk, const char* name);

#endif
