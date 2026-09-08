#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct Parser Parser;

Parser* parser_init(Lexer* lexer);
// Bug #4 fix: parser now remembers which file it's parsing so that syntax
// errors include the file path (matching semantic.c's format). The legacy
// parser_init() above keeps path = NULL ("<input>") for backwards
// compatibility; new callers should use parser_init_with_file().
Parser* parser_init_with_file(Lexer* lexer, const char* file_path);
void parser_free(Parser* p);
ASTNode* parse_expression(Parser* p);
ASTNode* parse_statement(Parser* p);
ASTProgram* parse_program_v2(Parser* p);

// Registra um erro sintático sem chamar exit(). O parser tenta se recuperar
// sincronizando no próximo ; ou } e continua o parse. Use parser_had_error()
// depois de parse_program_v2 para saber se algum erro ocorreu.
void parser_error(Parser* p, const char* message);

/* Sprint 4: like parser_error, but with an optional hint printed below
 * the source snippet. The hint is shown as "hint: <text>" and is meant
 * to give actionable advice (e.g. "did you forget a value after '='?").
 * Pass NULL when no hint is appropriate. */
void parser_error_with_hint(Parser* p, const char* message, const char* hint);

// Retorna 1 se o parser registrou pelo menos um erro desde a inicialização.
int parser_had_error(const Parser* p);

// Número absoluto de erros registrados.
int parser_error_count(const Parser* p);

// Retorna o path do arquivo que este parser está parseando, ou NULL se
// parser_init() foi usado (caso legado). O caller não deve liberar o
// ponteiro.
const char* parser_file_path(const Parser* p);

#endif
