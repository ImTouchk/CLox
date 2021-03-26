#ifndef COMPILER_H
#define COMPILER_H

#include "chunk.h"
#include "vm.h"

ObjectFunction* compile(const char* source);
void mark_compiler_roots();

#endif
