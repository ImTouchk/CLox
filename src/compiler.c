#include "common.h"
#include "object.h"
#include "memory.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

#include "scanner.h"
#include "compiler.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
} Parser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * /
    PREC_UNARY,       // ! -
    PREC_CALL,        // . ()
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool can_assign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

typedef struct {
    Token name;
    int depth;
    bool is_captured;
} Local;

typedef struct {
    uint8_t index;
    bool is_local;
} Upvalue;

typedef enum {
    TYPE_SCRIPT,
    TYPE_FUNCTION,
    TYPE_METHOD,
    TYPE_INITIALIZER,
} FunctionType;

typedef struct Compiler {
    ObjectFunction* function;
    FunctionType type;

    Upvalue upvalues[UINT8_COUNT];
    Local locals[UINT8_COUNT];
    int local_count;
    int scope_depth;
    struct Compiler* enclosing;
} Compiler;

typedef struct ClassCompiler {
    Token name;
    bool has_superclass;
    struct ClassCompiler* enclosing;
} ClassCompiler;

static Parser parser;
static Chunk* compiling_chunk;
static ClassCompiler* current_class = NULL;
static Compiler* current = NULL;

static Chunk* current_chunk()
{
    return &current->function->chunk;
}

static void error_at(Token* token, const char* message)
{
    if (parser.panicMode)
        return;

    parser.panicMode = true;

    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    }
    else if (token->type == TOKEN_ERROR) {
        // Nothing.
    }
    else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char* message)
{
    error_at(&parser.previous, message);
}

static void error_at_current(const char* message)
{
    error_at(&parser.current, message);
}

static void advance()
{
    parser.previous = parser.current;

    for (;;) {
        parser.current = scan_token();
        if (parser.current.type != TOKEN_ERROR) break;
        error_at_current(parser.current.start);
    }
}

static void consume(TokenType type, const char* message)
{
    if (parser.current.type == type) {
        advance();
        return;
    }

    error_at_current(message);
}

static bool check(TokenType type)
{
    return parser.current.type == type;
}

static bool match(TokenType type)
{
    if (!check(type)) return false;
    advance();
    return true;
}

static uint8_t make_constant(Value value)
{
    int constant = add_constant(current_chunk(), value);
    if (constant > UINT8_MAX) {
        error("Too many constants in one chunk.");
        return 0;
    }

    return (uint8_t)constant;
}

static void emit_byte(uint8_t byte)
{
    write_chunk(current_chunk(), byte, parser.previous.line);
}

static void emit_bytes(uint8_t byte1, uint8_t byte2)
{
    emit_byte(byte1);
    emit_byte(byte2);
}

static void emit_loop(int loop_start)
{
    emit_byte(OP_LOOP);
    
    int offset = current_chunk()->count - loop_start + 2;
    
    if (offset > UINT16_MAX) 
        error("Loop body too large.");

    emit_byte((offset >> 8) & 0xff);
    emit_byte(offset & 0xff);
}

static int emit_jump(uint8_t instruction)
{
    emit_byte(instruction);
    emit_byte(0xff);
    emit_byte(0xff);
    return current_chunk()->count - 1;
}

static void emit_constant(Value value)
{
    emit_bytes(OP_CONSTANT, make_constant(value));
}

static void patch_jump(int offset)
{
    // -2 to adjust for the bytecode for the jump offset itself.
    int jump = current_chunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    current_chunk()->code[offset]     = (jump >> 8) & 0xff;
    current_chunk()->code[offset + 1] = jump & 0xff;
}

static void emit_return()
{
    if (current->type == TYPE_INITIALIZER) {
        emit_bytes(OP_GET_LOCAL, 0);
    } else emit_byte(OP_NIL);
    emit_byte(OP_RETURN);
}

static void init_compiler(Compiler* compiler, FunctionType type)
{
    compiler->enclosing   = current;
    compiler->type        = type;
    compiler->function    = NULL;
    compiler->local_count = 0;
    compiler->scope_depth = 0;
    compiler->function    = new_function();

    current = compiler;

    if (type != TYPE_SCRIPT) {
        current->function->name = copy_string(parser.previous.start, parser.previous.length);
    }

    Local* local = &current->locals[current->local_count++];
    local->is_captured = false;
    local->depth       = 0;

    if (type != TYPE_FUNCTION) {
        local->name.start = "this";
        local->name.length = 4;
    } else {
        local->name.start  = "";
        local->name.length = 0;
    }
}

static ObjectFunction* end_compiler()
{
    emit_return();
    ObjectFunction* function = current->function;
#   ifdef DEBUG_PRINT_CODE
    if (!parser.hadError) {
        disassemble_chunk(current_chunk(), function->name != NULL ? function->name->chars : "<script>");
    }
#   endif

    current = current->enclosing;
    return function;
}

static void begin_scope()
{
    current->scope_depth++;
}

static void end_scope()
{
    current->scope_depth--;

    while (current->local_count > 0 && current->locals[current->local_count - 1].depth > current->scope_depth) {
        if (current->locals[current->local_count - 1].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE);
        } else {
            emit_byte(OP_POP);
        }

        current->local_count--;
    }
}

static void statement();
static void expression();
static void declaration();
static void parse_precedence(Precedence precedence);
static ParseRule* get_rule(TokenType type);


static void parse_precedence(Precedence precedence)
{
    advance();
    ParseFn prefix_rule = get_rule(parser.previous.type)->prefix;
    if (prefix_rule == NULL) {
        error("Expected expression.");
        return;
    }

    bool can_assign = precedence <= PREC_ASSIGNMENT;
    prefix_rule(can_assign);

    while (precedence <= get_rule(parser.current.type)->precedence) {
        advance();
        ParseFn infix_rule = get_rule(parser.previous.type)->infix;
        infix_rule(can_assign);
    }

    if (can_assign && match(TOKEN_EQUAL)) {
        error("Invalid assignment target.");
    }
}

static uint8_t identifier_constant(Token* name)
{
    return make_constant(
        OBJECT_VALUE(copy_string(name->start, name->length))
    );
}

static bool identifiers_equal(Token* a, Token* b)
{
    if (a->length != b->length)
        return false;
    return memcmp(a->start, b->start, b->length) == 0;
}

static int resolve_local(Compiler* compiler, Token* name)
{
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiers_equal(name, &local->name)) {
            if (local->depth == -1) {
                error("Can't read local variable in its own initializer.");
            }
            return i;
        }
    }

    return -1;
}

static int add_upvalue(Compiler* compiler, uint8_t index, bool is_local)
{
    int upvalue_count = compiler->function->upvalue_count;

    for (int i = 0; i < upvalue_count; i++) {
        Upvalue* upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->is_local == is_local)
            return i;
    }

    if (upvalue_count == UINT8_COUNT) {
        error("Too many closure variables in a function.");
        return 0;
    }

    compiler->upvalues[upvalue_count].is_local = is_local;
    compiler->upvalues[upvalue_count].index    = index;
    return compiler->function->upvalue_count++;
}

static int resolve_upvalue(Compiler* compiler, Token* name)
{
    if (compiler->enclosing == NULL) return -1;

    int local = resolve_local(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].is_captured = true;
        return add_upvalue(compiler, (uint8_t)local, true);
    }

    int upvalue = resolve_upvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return add_upvalue(compiler, (uint8_t)upvalue, false);
    }

    return -1;
}

static void add_local(Token name)
{
    if (current->local_count == UINT8_COUNT) {
        error("Too many local variables in a function.");
        return;
    }

    Local* local = &current->locals[current->local_count++];
    local->is_captured = false;
    local->name        = name;
    local->depth       = -1;
}

static void declare_variable()
{
    if (current->scope_depth == 0)
        return;

    Token* name = &parser.previous;
    for (int i = current->local_count - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scope_depth)
            break;

        if (identifiers_equal(name, &local->name)) {
            error("Variable with the same name already declared in scope.");
        }
    }
    add_local(*name);
}

static uint8_t parse_variable(const char* error_message)
{
    consume(TOKEN_IDENTIFIER, error_message);
    declare_variable();

    if (current->scope_depth > 0) 
        return 0;

    return identifier_constant(&parser.previous);
}

static void mark_initialized()
{
    if (current->scope_depth == 0)
        return;
    current->locals[current->local_count - 1].depth = current->scope_depth;
}

static void define_variable(uint8_t global)
{
    if (current->scope_depth > 0) {
        mark_initialized();
        return;
    }

    emit_bytes(OP_DEFINE_GLOBAL, global);
}

static void named_variable(Token name, bool can_assign)
{
    uint8_t getOp, setOp;
    int arg = resolve_local(current, &name);
    if (arg != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    }
    else if ((arg = resolve_upvalue(current, &name)) != -1) {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    }
    else {
        arg = identifier_constant(&name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (can_assign && match(TOKEN_EQUAL)) {
        expression();
        emit_bytes(setOp, (uint8_t)arg);
    }
    else emit_bytes(getOp, (uint8_t)arg);
}

static uint8_t argument_list()
{
    uint8_t arg_count = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            expression();

            if (arg_count == 255) {
                error("Can't have more than 255 characters.");
            }

            arg_count++;
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expected ')' after arguments.");
    return arg_count;
}

static void and_(bool can_assign)
{
    int end_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    parse_precedence(PREC_AND);
    patch_jump(end_jump);
}

static void or_(bool can_assign)
{
    int else_jump = emit_jump(OP_JUMP_IF_FALSE);
    int end_jump  = emit_jump(OP_JUMP);
    patch_jump(else_jump);
    emit_byte(OP_POP);

    parse_precedence(PREC_OR);
    patch_jump(end_jump);
}


static Token syntethic_token(const char* text)
{
    Token token;
    token.length = (int)strlen(text);
    token.start = text;
    return token;
}

static void expression()
{
    parse_precedence(PREC_ASSIGNMENT);
}

static void block()
{
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }
    consume(TOKEN_RIGHT_BRACE, "Expected '}' after block.");
}

static void function(FunctionType type)
{
    Compiler compiler;
    init_compiler(&compiler, type);
    begin_scope();

    consume(TOKEN_LEFT_PAREN, "Expected '(' after function name.");

    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            current->function->arity++;
            if (current->function->arity > 255) {
                error_at_current("Can't have more than 255 parameters.");
            }

            uint8_t param_constant = parse_variable("Expected parameter name.");
            define_variable(param_constant);
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expected ')' after parameters.");
    consume(TOKEN_LEFT_BRACE, "Expected '{' before function body.");
    block();

    ObjectFunction* function = end_compiler();
    emit_bytes(OP_CLOSURE, make_constant(OBJECT_VALUE(function)));

    for (int i = 0; i < function->upvalue_count; i++) {
        emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
        emit_byte(compiler.upvalues[i].index);
    }
}

static void method()
{
    consume(TOKEN_IDENTIFIER, "Expected method name.");

    uint8_t constant = identifier_constant(&parser.previous);

    FunctionType type = TYPE_METHOD;
    if (parser.previous.length == 4 && memcmp(parser.previous.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }

    function(type);

    emit_bytes(OP_METHOD, constant);
}

static void variable(bool can_assign)
{
    named_variable(parser.previous, can_assign);
}

static void var_declaration()
{
    uint8_t global = parse_variable("Expected variable name.");
    if (match(TOKEN_EQUAL)) {
        expression();
    } else emit_byte(OP_NIL);

    consume(TOKEN_SEMICOLON, "Expected ';' after variable declaration.");
    define_variable(global);
}

static void fun_declaration()
{
    uint8_t global = parse_variable("Expected function name.");
    mark_initialized();
    function(TYPE_FUNCTION);
    define_variable(global);
}

static void class_declaration()
{
    consume(TOKEN_IDENTIFIER, "Expected class name.");

    Token class_name = parser.previous;

    uint8_t name_constant = identifier_constant(&parser.previous);
    declare_variable();

    emit_bytes(OP_CLASS, name_constant);
    define_variable(name_constant);

    ClassCompiler class_compiler;
    class_compiler.has_superclass = false;
    class_compiler.enclosing      = current_class;
    class_compiler.name           = parser.previous;
    current_class = &class_compiler;

    if (match(TOKEN_LESS)) {
        consume(TOKEN_IDENTIFIER, "Expected superclass name.");
        variable(false);

        if (identifiers_equal(&class_name, &parser.previous)) {
            error("A class can't inherit from itself.");
        }

        begin_scope();
        add_local(syntethic_token("super"));
        define_variable(0);
            
        named_variable(class_name, false);
        emit_byte(OP_INHERIT);

        class_compiler.has_superclass = true;
    }

    named_variable(class_name, false);

    consume(TOKEN_LEFT_BRACE, "Expected '{' before class body.");

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        method();
    }

    consume(TOKEN_RIGHT_BRACE, "Expected '}' after class body.");
    emit_byte(OP_POP);

    if (class_compiler.has_superclass) {
        end_scope();
    }

    current_class = current_class->enclosing;
}

static void binary(bool can_assign)
{
    // Remember the operator.
    TokenType op_type = parser.previous.type;

    // Compile the right operand.
    ParseRule* rule = get_rule(op_type);
    parse_precedence((Precedence)(rule->precedence + 1));

    // Emit the operator instruction.
    switch (op_type) {
    case TOKEN_PLUS:          emit_byte(OP_ADD);              break;
    case TOKEN_MINUS:         emit_byte(OP_SUBTRACT);         break;
    case TOKEN_STAR:          emit_byte(OP_MULTIPLY);         break;
    case TOKEN_SLASH:         emit_byte(OP_DIVIDE);           break;
    case TOKEN_PERCENT:       emit_byte(OP_MODULO);           break;
    case TOKEN_BANG_EQUAL:    emit_bytes(OP_EQUAL, OP_NOT);   break;
    case TOKEN_EQUAL_EQUAL:   emit_byte(OP_EQUAL);            break;
    case TOKEN_GREATER:       emit_byte(OP_GREATER);          break;
    case TOKEN_GREATER_EQUAL: emit_bytes(OP_LESS, OP_NOT);    break;
    case TOKEN_LESS:          emit_byte(OP_LESS);             break;
    case TOKEN_LESS_EQUAL:    emit_bytes(OP_GREATER, OP_NOT); break;
    default:
        return; // Unreachable.
    }
}

static void call(bool can_assign)
{
    uint8_t arg_count = argument_list();
    emit_bytes(OP_CALL, arg_count);
}

static void dot(bool can_assign)
{
    consume(TOKEN_IDENTIFIER, "Expected property name after '.'.");
    
    uint8_t name = identifier_constant(&parser.previous);
    if (can_assign && match(TOKEN_EQUAL)) {
        expression();
        emit_bytes(OP_SET_PROPERTY, name);
    } else if(match(TOKEN_LEFT_PAREN)) {
        uint8_t arg_count = argument_list();
        emit_bytes(OP_INVOKE, name);
        emit_byte(arg_count);
    } else {
        emit_bytes(OP_GET_PROPERTY, name);
    }
}

static void unary(bool can_assign)
{
    TokenType operator_type = parser.previous.type;
    parse_precedence(PREC_UNARY);
    switch (operator_type) {
    case TOKEN_BANG:  emit_byte(OP_NOT);    break;
    case TOKEN_MINUS: emit_byte(OP_NEGATE); break;
    default: return;
    }
}

static void number(bool can_assign)
{
    double value = strtod(parser.previous.start, NULL);
    emit_constant(NUMBER_VALUE(value));
}

static void grouping(bool can_assign)
{
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after expression.");
}

static void literal(bool can_assign)
{
    switch (parser.previous.type) {
    case TOKEN_FALSE: emit_byte(OP_FALSE); break;
    case TOKEN_TRUE:  emit_byte(OP_TRUE);  break;
    case TOKEN_NIL:   emit_byte(OP_NIL);   break;
    default: return;
    }
}

static void string(bool can_assign)
{
    emit_constant(OBJECT_VALUE(
        copy_string(parser.previous.start + 1, parser.previous.length - 2)
    ));
}

static void super_(bool can_assign)
{
    if (current_class == NULL) {
        error("Can't use 'super' outside of class.");
    } else if(!current_class->has_superclass) {
        error("Can't use 'super' in a class with no superclass.");
    }

    consume(TOKEN_DOT, "Expected '.' after 'super'.");
    consume(TOKEN_IDENTIFIER, "Expected superclass method name.");

    uint8_t name = identifier_constant(&parser.previous);
    named_variable(syntethic_token("this"), false);

    if (match(TOKEN_LEFT_PAREN)) {
        uint8_t arg_count = argument_list();
        named_variable(syntethic_token("super"), false);
        emit_bytes(OP_SUPER_INVOKE, name);
        emit_byte(arg_count);
    } else {
        named_variable(syntethic_token("super"), false);
        emit_bytes(OP_GET_SUPER, name);
    }
}

static void this_(bool can_assign)
{
    if (current_class == NULL) {
        error("Can't use 'this' outside of a class.");
        return;
    }
    variable(false);
}

static void print_statement()
{
    expression();
    consume(TOKEN_SEMICOLON, "Expected ';' after value.");
    emit_byte(OP_PRINT);
}

static void return_statement()
{
    if (current->type == TYPE_SCRIPT) {
        error("Can't return from top-level code.");
    }

    if (match(TOKEN_SEMICOLON)) 
        emit_return();
    else {
        if (current->type == TYPE_INITIALIZER) {
            error("Can't return from an initializer.");
        }

        expression();
        consume(TOKEN_SEMICOLON, "Expected ';' after return value.");
        emit_byte(OP_RETURN);
    }
}

static void expression_statement()
{
    expression();
    consume(TOKEN_SEMICOLON, "Expected ';' after expression.");
    emit_byte(OP_POP);
}

static void if_statement()
{
    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'if'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after condition.");

    int then_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    statement();

    int else_jump = emit_jump(OP_JUMP);

    patch_jump(then_jump);
    emit_byte(OP_POP);

    if (match(TOKEN_ELSE))
        statement();
    patch_jump(else_jump);
}

static void while_statement()
{
    int loop_start = current_chunk()->count;

    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'while'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after condition.");

    int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    statement();
    emit_loop(loop_start);

    patch_jump(exit_jump);
    emit_byte(OP_POP);
}

static void for_statement()
{
    begin_scope();

    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'for'.");
    if (match(TOKEN_SEMICOLON)) {
        // No initializer.
    }
    else if (match(TOKEN_VAR)) {
        var_declaration();
    }
    else {
        expression_statement();
    }

    int loop_start = current_chunk()->count;
    
    int exit_jump  = -1;
    if (!match(TOKEN_SEMICOLON)) {
        expression();
        consume(TOKEN_SEMICOLON, "Expected ';' after loop condition.");

        // Jump out of the loop if the condition is false
        exit_jump = emit_jump(OP_JUMP_IF_FALSE);
        emit_byte(OP_POP); // Condition
    }

    if (!match(TOKEN_RIGHT_PAREN)) {
        int body_jump = emit_jump(OP_JUMP);
        int inc_start = current_chunk()->count;

        expression();
        emit_byte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "Expected ')' after for clauses.");
        
        emit_loop(loop_start);
        loop_start = inc_start;
        patch_jump(body_jump);
    }

    statement();

    emit_loop(loop_start);

    if (exit_jump != -1) {
        patch_jump(exit_jump);
        emit_byte(OP_POP);
    }

    end_scope();
}

static void synchronize()
{
    parser.panicMode = false;
    while (parser.current.type != TOKEN_EOF) {
        if (parser.current.type == TOKEN_SEMICOLON)
            return;

        switch (parser.current.type) {
        case TOKEN_CLASS:
        case TOKEN_FUN:
        case TOKEN_VAR:
        case TOKEN_FOR:
        case TOKEN_IF:
        case TOKEN_WHILE:
        case TOKEN_PRINT:
        case TOKEN_RETURN:
            return;

        default:
            // Do nothing.
            ;
        }

        advance();
    }
}

static void statement()
{
    if (match(TOKEN_PRINT)) {
        print_statement();
    } else if (match(TOKEN_FOR)) {
        for_statement();
    } else if (match(TOKEN_IF)) {
        if_statement();
    } else if (match(TOKEN_RETURN)) {
        return_statement();
    } else if (match(TOKEN_WHILE)) {
        while_statement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        begin_scope();
        block();
        end_scope();
    } else expression_statement();
}

static void declaration()
{
    if (match(TOKEN_CLASS)) {
        class_declaration();
    } else if (match(TOKEN_FUN)) {
        fun_declaration();
    } else if (match(TOKEN_VAR)) {
        var_declaration();
    } else statement();
    if (parser.panicMode) synchronize();
}

static ParseRule rules[] = {
  [TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL},
  [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE},
  [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
  [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
  [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
  [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
  [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
  [TOKEN_PERCENT]       = {NULL,     binary, PREC_FACTOR},
  [TOKEN_BANG]          = {unary,    NULL,   PREC_NONE},
  [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
  [TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
  [TOKEN_NUMBER]        = {number,   and_,   PREC_AND},
  [TOKEN_AND]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
  [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FUN]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
  [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
  [TOKEN_OR]            = {NULL,     or_,    PREC_OR},
  [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SUPER]         = {super_,   NULL,   PREC_NONE},
  [TOKEN_THIS]          = {this_,    NULL,   PREC_NONE},
  [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
  [TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
};

static ParseRule* get_rule(TokenType type)
{
    return &rules[type];
}

ObjectFunction* compile(const char* source)
{
    init_scanner(source);

    Compiler compiler;
    init_compiler(&compiler, TYPE_SCRIPT);

    parser.hadError  = false;
    parser.panicMode = false;

    advance();

    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjectFunction* function = end_compiler();

    return parser.hadError ? NULL : function;
}

void mark_compiler_roots()
{
    Compiler* compiler = current;
    while (compiler != NULL) {
        mark_object((Object*)compiler->function);
        compiler = compiler->enclosing;
    }
}
