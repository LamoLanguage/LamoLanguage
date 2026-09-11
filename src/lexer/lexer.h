#ifndef LEXER_H
#define LEXER_H

#define _POSIX_C_SOURCE 200809L

typedef enum {
    // Keywords
    TOKEN_LET, TOKEN_FN, TOKEN_RETURN, TOKEN_IF, TOKEN_ELSE, TOKEN_WHILE, TOKEN_FOR,
    TOKEN_TRUE, TOKEN_FALSE, TOKEN_IMPORT,
    TOKEN_BREAK, TOKEN_CONTINUE,
    TOKEN_AS,  /* Sprint 4: `as` keyword for `import "..." as alias;` */
    /* Phase 2: struct / impl / enum / match keywords. */
    TOKEN_STRUCT, TOKEN_IMPL, TOKEN_ENUM, TOKEN_MATCH,
    /* 2.9.0: `trait` keyword — trait declarations (`trait Name { ... }`)
     * and trait-constrained type parameters (`T: Shape`). */
    TOKEN_TRAIT,

    // Literals & Identifiers
    TOKEN_IDENTIFIER, TOKEN_INT, TOKEN_FLOAT, TOKEN_STRING,

    // Operators
    TOKEN_EQUALS, TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
    TOKEN_EQ_EQ, TOKEN_BANG_EQ, TOKEN_LT, TOKEN_GT, TOKEN_LT_EQ, TOKEN_GT_EQ,
    TOKEN_AND_AND, TOKEN_OR_OR, TOKEN_BANG,
    TOKEN_PLUS_EQ, TOKEN_MINUS_EQ, TOKEN_PLUS_PLUS, TOKEN_MINUS_MINUS,
    TOKEN_ARROW,  /* Sprint 3: `->` for optional function return-type annotations */
    TOKEN_FAT_ARROW,  /* Phase 2: `=>` for match arms */

    // Delimiters
    TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACE, TOKEN_RBRACE,
    TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_COMMA, TOKEN_SEMICOLON, TOKEN_COLON, TOKEN_DOT,
    /* 2.7.0 (FU4): `::` variant qualification — `Enum::Variant`.
     * Lexed as ONE token so the parser can distinguish qualified
     * variant references from two unrelated colons (SPEC §3.5). */
    TOKEN_COLON_COLON,

    // System
    TOKEN_EOF, TOKEN_UNKNOWN
} LamoTokenType;

typedef struct {
    LamoTokenType type;
    char* value;
    int line;
    int column;
    /* Perf pass 2: 1 when `value` points to heap memory owned by this
     * token (malloc'd by the lexer or strdup'd by a speculative parser
     * probe) and must be released by token_free(). 0 when `value` points
     * to a static constant string (punctuators, "EOF") — freeing it
     * would be UB. Assigning static strings to the ~half of all tokens
     * that are punctuators removes one malloc+free pair per token from
     * the hottest loop in the compiler. Consumers must never modify
     * `value` in place, owned or not. */
    unsigned char owns_value;
} Token;

typedef struct {
    char* source;
    int pos;
    int line;
    int column;
} Lexer;

// Funções públicas
Lexer* lexer_init(char* source);
void lexer_free(Lexer* lexer);
Token lexer_next_token(Lexer* lexer);
void token_free(Token t);
const char* token_type_name(LamoTokenType type);

// Retorna 1 se o nome corresponde a uma builtin da linguagem (print, input, ...).
// Estes nomes são tratados como identificadores comuns no lexer, mas o codegen
// e o semântico precisam saber que existem.
int lexer_is_builtin_name(const char* name);

#endif
