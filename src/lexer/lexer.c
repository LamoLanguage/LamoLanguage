#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"
#include "../builtins.h"

static char* my_strndup(const char* s, size_t n) {
    char* res = malloc(n + 1);
    if (res) {
        memcpy(res, s, n);
        res[n] = '\0';
    }
    return res;
}

Lexer* lexer_init(char* source) {
    Lexer* l = malloc(sizeof(Lexer));
    if (!l) {
        perror("Failed to allocate Lexer");
        exit(EXIT_FAILURE);
    }
    l->source = source;
    l->pos = 0;
    l->line = 1;
    l->column = 1;
    return l;
}

void lexer_free(Lexer* lexer) {
    free(lexer);
}

static char peek(Lexer* l) {
    return l->source[l->pos];
}

static char peek_at(Lexer* l, int offset) {
    return l->source[l->pos + offset];
}

static char advance(Lexer* l) {
    char c = l->source[l->pos++];
    if (c == '\r') {
        l->line++;
        l->column = 1;
        // Skip following \n for CRLF sequences
        if (l->source[l->pos] == '\n') {
            l->pos++;
        }
    } else if (c == '\n') {
        // Handle standalone \n (Unix line endings)
        l->line++;
        l->column = 1;
    } else {
        l->column++;
    }
    return c;
}

static void skip_whitespace(Lexer* l) {
    while (1) {
        char c = peek(l);
        if (isspace((unsigned char)c) || c == '\r') {
            advance(l);
        } else if (c == '/' && l->source[l->pos + 1] == '/') {
            while (peek(l) != '\n' && peek(l) != '\r' && peek(l) != '\0') advance(l);
        } else if (c == '/' && l->source[l->pos + 1] == '*') {
            advance(l); advance(l);
            while (!(peek(l) == '*' && l->source[l->pos + 1] == '/') && peek(l) != '\0') advance(l);
            if (peek(l) != '\0') { advance(l); advance(l); }
        } else {
            break;
        }
    }
}

// Remove underscores de um literal numérico, copiando para `dst`.
// `dst` deve ter espaço para pelo menos `len + 1` caracteres.
static void strip_underscores(const char* src, size_t len, char* dst) {
    size_t i;
    size_t j = 0;
    for (i = 0; i < len; i++) {
        if (src[i] != '_') {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

// Decodifica escapes de string a partir de `src` (tamanho `len`) escrevendo em `dst`.
// Retorna o tamanho da string decodificada (sem terminador). `dst` precisa de pelo
// menos `len + 1` bytes. Suporta: \n \t \r \\ \" \0 \xNN
static size_t decode_string_escapes(const char* src, size_t len, char* dst) {
    size_t i;
    size_t j = 0;
    for (i = 0; i < len; i++) {
        char c = src[i];
        if (c != '\\') {
            dst[j++] = c;
            continue;
        }
        if (i + 1 >= len) {
            // backslash isolado no final: mantém literal
            dst[j++] = '\\';
            break;
        }
        char next = src[i + 1];
        switch (next) {
            case 'n':  dst[j++] = '\n'; i++; break;
            case 't':  dst[j++] = '\t'; i++; break;
            case 'r':  dst[j++] = '\r'; i++; break;
            case '\\': dst[j++] = '\\'; i++; break;
            case '"':  dst[j++] = '"';  i++; break;
            case '\'': dst[j++] = '\''; i++; break;
            case '0':  dst[j++] = '\0'; i++; break;
            case 'x': {
                if (i + 3 < len) {
                    int hi = src[i + 2];
                    int lo = src[i + 3];
                    int hv = (hi >= '0' && hi <= '9') ? hi - '0' :
                             (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 :
                             (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
                    int lv = (lo >= '0' && lo <= '9') ? lo - '0' :
                             (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 :
                             (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
                    if (hv >= 0 && lv >= 0) {
                        dst[j++] = (char)((hv << 4) | lv);
                        i += 3;
                        break;
                    }
                }
                // fallback: mantém \x literal
                dst[j++] = '\\';
                dst[j++] = 'x';
                i++;
                break;
            }
            default:
                // escape desconhecido: mantém a barra e o caractere
                dst[j++] = '\\';
                dst[j++] = next;
                i++;
                break;
        }
    }
    dst[j] = '\0';
    return j;
}

// Lê um número decimal/hex/binário/float. Já posicionada no primeiro dígito.
static Token lex_number(Lexer* l, Token t) {
    int start = l->pos;
    int is_float = 0;

    // hex ou binário
    if (peek(l) == '0' && (peek_at(l, 1) == 'x' || peek_at(l, 1) == 'X')) {
        advance(l); advance(l); // 0x
        while (isxdigit((unsigned char)peek(l)) || peek(l) == '_') advance(l);
        t.type = TOKEN_INT;
        // remove "0x" e underscores do valor
        {
            size_t raw_len = (size_t)(l->pos - start);
            char* stripped = malloc(raw_len + 1);
            strip_underscores(l->source + start, raw_len, stripped);
            t.value = stripped;
        }
        return t;
    }

    if (peek(l) == '0' && (peek_at(l, 1) == 'b' || peek_at(l, 1) == 'B')) {
        advance(l); advance(l); // 0b
        while (peek(l) == '0' || peek(l) == '1' || peek(l) == '_') advance(l);
        t.type = TOKEN_INT;
        {
            size_t raw_len = (size_t)(l->pos - start);
            char* stripped = malloc(raw_len + 1);
            strip_underscores(l->source + start, raw_len, stripped);
            t.value = stripped;
        }
        return t;
    }

    // decimal
    while (isdigit((unsigned char)peek(l)) || peek(l) == '_') advance(l);

    // parte fracionária
    if (peek(l) == '.' && isdigit((unsigned char)peek_at(l, 1))) {
        is_float = 1;
        advance(l); // .
        while (isdigit((unsigned char)peek(l)) || peek(l) == '_') advance(l);
    }

    // expoente: e[+-]?digits
    if (peek(l) == 'e' || peek(l) == 'E') {
        is_float = 1;
        advance(l);
        if (peek(l) == '+' || peek(l) == '-') advance(l);
        while (isdigit((unsigned char)peek(l)) || peek(l) == '_') advance(l);
    }

    t.type = is_float ? TOKEN_FLOAT : TOKEN_INT;
    {
        size_t raw_len = (size_t)(l->pos - start);
        char* stripped = malloc(raw_len + 1);
        strip_underscores(l->source + start, raw_len, stripped);
        t.value = stripped;
    }
    return t;
}

/* Perf pass 2: keyword recognition. The previous implementation ran a
 * linear chain of up to 18 strcmp() calls for EVERY identifier token —
 * and identifiers are the most common token in real source. This helper
 * dispatches on the first character (a perfect split: no first letter is
 * shared by more than 3 keywords) and only strcmp()s within that bucket,
 * usually resolving in 0–2 comparisons instead of 9 on average.
 * Returns TOKEN_IDENTIFIER when the word is not a keyword. */
static LamoTokenType keyword_or_identifier(const char* s) {
    switch (s[0]) {
        case 'a':
            if (strcmp(s, "as") == 0) return TOKEN_AS;            /* Sprint 4 */
            break;
        case 'b':
            if (strcmp(s, "break") == 0) return TOKEN_BREAK;
            break;
        case 'c':
            if (strcmp(s, "continue") == 0) return TOKEN_CONTINUE;
            break;
        case 'e':
            if (strcmp(s, "else") == 0) return TOKEN_ELSE;
            if (strcmp(s, "enum") == 0) return TOKEN_ENUM;
            break;
        case 'f':
            if (strcmp(s, "fn") == 0) return TOKEN_FN;
            if (strcmp(s, "for") == 0) return TOKEN_FOR;
            if (strcmp(s, "false") == 0) return TOKEN_FALSE;
            break;
        case 'i':
            if (strcmp(s, "if") == 0) return TOKEN_IF;
            if (strcmp(s, "import") == 0) return TOKEN_IMPORT;
            if (strcmp(s, "impl") == 0) return TOKEN_IMPL;
            break;
        case 'l':
            if (strcmp(s, "let") == 0) return TOKEN_LET;
            break;
        case 'm':
            if (strcmp(s, "match") == 0) return TOKEN_MATCH;
            break;
        case 'r':
            if (strcmp(s, "return") == 0) return TOKEN_RETURN;
            break;
        case 's':
            if (strcmp(s, "struct") == 0) return TOKEN_STRUCT;
            break;
        case 't':
            if (strcmp(s, "true") == 0) return TOKEN_TRUE;
            if (strcmp(s, "trait") == 0) return TOKEN_TRAIT;   /* 2.9.0 */
            break;
        case 'w':
            if (strcmp(s, "while") == 0) return TOKEN_WHILE;
            break;
        default:
            break;
    }
    /* print, input, isnumber, isstring, exit, abs são identificadores
     * comuns: resolvidos como builtins na tabela de símbolos e no
     * codegen. self também é um identificador comum — o parser/seântico
     * tratam disso implicitamente dentro de métodos `impl Type { fn ...
     * self ... }`. */
    return TOKEN_IDENTIFIER;
}

/* Static token text for punctuators. Every '(' token in a program shares
 * this one literal instead of malloc'ing its own copy; token_free() skips
 * them via owns_value == 0. */
#define STATIC_VALUE(s) ((char*)(s))

Token lexer_next_token(Lexer* l) {
    skip_whitespace(l);

    Token t;
    t.line = l->line;
    t.column = l->column;
    t.value = NULL;
    t.owns_value = 0;

    char c = peek(l);
    if (c == '\0') {
        t.type = TOKEN_EOF;
        t.value = STATIC_VALUE("EOF");
        return t;
    }

    if (isdigit((unsigned char)c)) {
        return lex_number(l, t);
    }

    if (c == '.' && isdigit((unsigned char)l->source[l->pos + 1])) {
        return lex_number(l, t);
    }

    if (isalpha((unsigned char)c) || c == '_') {
        int start = l->pos;
        while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
        t.value = my_strndup(&l->source[start], (size_t)(l->pos - start));
        t.owns_value = 1;
        t.type = keyword_or_identifier(t.value);
        return t;
    }

    if (c == '"') {
        advance(l);
        int start = l->pos;
        while (peek(l) != '\0') {
            if (peek(l) == '\\') {
                // Pula o próximo caractere (escape) sem encerrar a string.
                advance(l);
                if (peek(l) != '\0') advance(l);
            } else if (peek(l) == '"') {
                break;
            } else if (peek(l) == '\n' || peek(l) == '\r') {
                // Strings não podem conter newlines reais.
                break;
            } else {
                advance(l);
            }
        }
        size_t raw_len = (size_t)(l->pos - start);
        char* decoded = malloc(raw_len + 1);
        if (!decoded) {
            t.type = TOKEN_STRING;
            t.value = STATIC_VALUE("");
            return t;
        }
        decode_string_escapes(l->source + start, raw_len, decoded);
        if (peek(l) == '"') advance(l);
        t.type = TOKEN_STRING;
        t.value = decoded;
        t.owns_value = 1;
        return t;
    }

    advance(l);
    /* Perf pass 2: every punctuator below assigns a static literal and
     * leaves owns_value == 0 (set at the top of this function). */
    switch (c) {
        case '(': t.type = TOKEN_LPAREN; t.value = STATIC_VALUE("("); break;
        case ')': t.type = TOKEN_RPAREN; t.value = STATIC_VALUE(")"); break;
        case '{': t.type = TOKEN_LBRACE; t.value = STATIC_VALUE("{"); break;
        case '}': t.type = TOKEN_RBRACE; t.value = STATIC_VALUE("}"); break;
        case '[': t.type = TOKEN_LBRACKET; t.value = STATIC_VALUE("["); break;
        case ']': t.type = TOKEN_RBRACKET; t.value = STATIC_VALUE("]"); break;
        case ',': t.type = TOKEN_COMMA; t.value = STATIC_VALUE(","); break;
        case ';': t.type = TOKEN_SEMICOLON; t.value = STATIC_VALUE(";"); break;
        case ':':
            /* 2.7.0 (FU4): `::` variant qualification lexes as ONE token
             * (mirrors the `->` / `=>` two-char lookahead above). */
            if (peek(l) == ':') { advance(l); t.type = TOKEN_COLON_COLON; t.value = STATIC_VALUE("::"); }
            else { t.type = TOKEN_COLON; t.value = STATIC_VALUE(":"); }
            break;
        case '.': t.type = TOKEN_DOT; t.value = STATIC_VALUE("."); break;
        case '+':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_PLUS_EQ; t.value = STATIC_VALUE("+="); }
            else if (peek(l) == '+') { advance(l); t.type = TOKEN_PLUS_PLUS; t.value = STATIC_VALUE("++"); }
            else { t.type = TOKEN_PLUS; t.value = STATIC_VALUE("+"); }
            break;
        case '-':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_MINUS_EQ; t.value = STATIC_VALUE("-="); }
            else if (peek(l) == '-') { advance(l); t.type = TOKEN_MINUS_MINUS; t.value = STATIC_VALUE("--"); }
            else if (peek(l) == '>') { advance(l); t.type = TOKEN_ARROW; t.value = STATIC_VALUE("->"); }
            else { t.type = TOKEN_MINUS; t.value = STATIC_VALUE("-"); }
            break;
        case '*': t.type = TOKEN_STAR; t.value = STATIC_VALUE("*"); break;
        case '/': t.type = TOKEN_SLASH; t.value = STATIC_VALUE("/"); break;
        case '%': t.type = TOKEN_PERCENT; t.value = STATIC_VALUE("%"); break;
        case '=':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_EQ_EQ; t.value = STATIC_VALUE("=="); }
            else if (peek(l) == '>') { advance(l); t.type = TOKEN_FAT_ARROW; t.value = STATIC_VALUE("=>"); }
            else { t.type = TOKEN_EQUALS; t.value = STATIC_VALUE("="); }
            break;
        case '!':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_BANG_EQ; t.value = STATIC_VALUE("!="); }
            else { t.type = TOKEN_BANG; t.value = STATIC_VALUE("!"); }
            break;
        case '<':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_LT_EQ; t.value = STATIC_VALUE("<="); }
            else { t.type = TOKEN_LT; t.value = STATIC_VALUE("<"); }
            break;
        case '>':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_GT_EQ; t.value = STATIC_VALUE(">="); }
            else { t.type = TOKEN_GT; t.value = STATIC_VALUE(">"); }
            break;
        case '&':
            if (peek(l) == '&') { advance(l); t.type = TOKEN_AND_AND; t.value = STATIC_VALUE("&&"); }
            else { t.type = TOKEN_UNKNOWN; t.value = STATIC_VALUE("&"); }
            break;
        case '|':
            if (peek(l) == '|') { advance(l); t.type = TOKEN_OR_OR; t.value = STATIC_VALUE("||"); }
            else { t.type = TOKEN_UNKNOWN; t.value = STATIC_VALUE("|"); }
            break;
        default:
            t.type = TOKEN_UNKNOWN;
            t.value = malloc(2);
            if (t.value) {
                t.value[0] = c;
                t.value[1] = '\0';
                t.owns_value = 1;
            }
            break;
    }
    return t;
}

void token_free(Token t) {
    /* Perf pass 2: only release heap-owned values. Punctuator tokens and
     * EOF carry static strings (owns_value == 0) and must not be freed. */
    if (t.owns_value) {
        free(t.value);
    }
}

int lexer_is_builtin_name(const char* name) {
    /* Sprint 2 refactor: delegates to the shared table in builtins.h.
     * Note that this only reports LANG builtins (print, input, ...), not
     * GUI/HTTP builtins — that matches the previous behavior. */
    return lamo_builtin_is_lang(name);
}

const char* token_type_name(LamoTokenType type) {
    switch (type) {
        case TOKEN_LET: return "let";
        case TOKEN_FN: return "fn";
        case TOKEN_RETURN: return "return";
        case TOKEN_IF: return "if";
        case TOKEN_ELSE: return "else";
        case TOKEN_WHILE: return "while";
        case TOKEN_FOR: return "for";
        case TOKEN_TRUE: return "true";
        case TOKEN_FALSE: return "false";
        case TOKEN_IMPORT: return "import";
        case TOKEN_BREAK: return "break";
        case TOKEN_CONTINUE: return "continue";
        case TOKEN_AS: return "as";
        case TOKEN_STRUCT: return "struct";
        case TOKEN_IMPL: return "impl";
        case TOKEN_ENUM: return "enum";
        case TOKEN_MATCH: return "match";
        case TOKEN_TRAIT: return "trait";
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_INT: return "INT";
        case TOKEN_FLOAT: return "FLOAT";
        case TOKEN_STRING: return "STRING";
        case TOKEN_EQUALS: return "=";
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_STAR: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_PERCENT: return "%";
        case TOKEN_BANG: return "!";
        case TOKEN_LT: return "<";
        case TOKEN_GT: return ">";
        case TOKEN_LPAREN: return "(";
        case TOKEN_RPAREN: return ")";
        case TOKEN_LBRACE: return "{";
        case TOKEN_RBRACE: return "}";
        case TOKEN_LBRACKET: return "[";
        case TOKEN_RBRACKET: return "]";
        case TOKEN_COMMA: return ",";
        case TOKEN_SEMICOLON: return ";";
        case TOKEN_COLON: return ":";
        case TOKEN_COLON_COLON: return "::";
        case TOKEN_DOT: return ".";
        case TOKEN_EQ_EQ: return "==";
        case TOKEN_BANG_EQ: return "!=";
        case TOKEN_LT_EQ: return "<=";
        case TOKEN_GT_EQ: return ">=";
        case TOKEN_AND_AND: return "&&";
        case TOKEN_OR_OR: return "||";
        case TOKEN_PLUS_EQ: return "+=";
        case TOKEN_MINUS_EQ: return "-=";
        case TOKEN_PLUS_PLUS: return "++";
        case TOKEN_MINUS_MINUS: return "--";
        case TOKEN_ARROW: return "->";
        case TOKEN_FAT_ARROW: return "=>";
        case TOKEN_EOF: return "EOF";
        default: return "UNKNOWN";
    }
}
