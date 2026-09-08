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

Token lexer_next_token(Lexer* l) {
    skip_whitespace(l);

    Token t;
    t.line = l->line;
    t.column = l->column;
    t.value = NULL;

    char c = peek(l);
    if (c == '\0') {
        t.type = TOKEN_EOF;
        t.value = strdup("EOF");
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

        if (strcmp(t.value, "let") == 0) t.type = TOKEN_LET;
        else if (strcmp(t.value, "fn") == 0) t.type = TOKEN_FN;
        else if (strcmp(t.value, "return") == 0) t.type = TOKEN_RETURN;
        else if (strcmp(t.value, "if") == 0) t.type = TOKEN_IF;
        else if (strcmp(t.value, "else") == 0) t.type = TOKEN_ELSE;
        else if (strcmp(t.value, "while") == 0) t.type = TOKEN_WHILE;
        else if (strcmp(t.value, "for") == 0) t.type = TOKEN_FOR;
        else if (strcmp(t.value, "true") == 0) t.type = TOKEN_TRUE;
        else if (strcmp(t.value, "false") == 0) t.type = TOKEN_FALSE;
        else if (strcmp(t.value, "import") == 0) t.type = TOKEN_IMPORT;
        else if (strcmp(t.value, "break") == 0) t.type = TOKEN_BREAK;
        else if (strcmp(t.value, "continue") == 0) t.type = TOKEN_CONTINUE;
        else if (strcmp(t.value, "as") == 0) t.type = TOKEN_AS;
        else if (strcmp(t.value, "struct") == 0) t.type = TOKEN_STRUCT;
        else if (strcmp(t.value, "impl") == 0) t.type = TOKEN_IMPL;
        else if (strcmp(t.value, "enum") == 0) t.type = TOKEN_ENUM;
        else if (strcmp(t.value, "match") == 0) t.type = TOKEN_MATCH;
        // print, input, isnumber, isstring, exit, abs são identificadores comuns:
        // resolvidos como builtins na tabela de símbolos e no codegen.
        // self também é um identificador comum — o parser/seântico tratam disso
        // implicitamente dentro de métodos `impl Type { fn ... self ... }`.
        else t.type = TOKEN_IDENTIFIER;

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
            t.value = strdup("");
            return t;
        }
        decode_string_escapes(l->source + start, raw_len, decoded);
        if (peek(l) == '"') advance(l);
        t.type = TOKEN_STRING;
        t.value = decoded;
        return t;
    }

    advance(l);
    switch (c) {
        case '(': t.type = TOKEN_LPAREN; t.value = strdup("("); break;
        case ')': t.type = TOKEN_RPAREN; t.value = strdup(")"); break;
        case '{': t.type = TOKEN_LBRACE; t.value = strdup("{"); break;
        case '}': t.type = TOKEN_RBRACE; t.value = strdup("}"); break;
        case '[': t.type = TOKEN_LBRACKET; t.value = strdup("["); break;
        case ']': t.type = TOKEN_RBRACKET; t.value = strdup("]"); break;
        case ',': t.type = TOKEN_COMMA; t.value = strdup(","); break;
        case ';': t.type = TOKEN_SEMICOLON; t.value = strdup(";"); break;
        case ':': t.type = TOKEN_COLON; t.value = strdup(":"); break;
        case '.': t.type = TOKEN_DOT; t.value = strdup("."); break;
        case '+':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_PLUS_EQ; t.value = strdup("+="); }
            else if (peek(l) == '+') { advance(l); t.type = TOKEN_PLUS_PLUS; t.value = strdup("++"); }
            else { t.type = TOKEN_PLUS; t.value = strdup("+"); }
            break;
        case '-':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_MINUS_EQ; t.value = strdup("-="); }
            else if (peek(l) == '-') { advance(l); t.type = TOKEN_MINUS_MINUS; t.value = strdup("--"); }
            else if (peek(l) == '>') { advance(l); t.type = TOKEN_ARROW; t.value = strdup("->"); }
            else { t.type = TOKEN_MINUS; t.value = strdup("-"); }
            break;
        case '*': t.type = TOKEN_STAR; t.value = strdup("*"); break;
        case '/': t.type = TOKEN_SLASH; t.value = strdup("/"); break;
        case '%': t.type = TOKEN_PERCENT; t.value = strdup("%"); break;
        case '=':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_EQ_EQ; t.value = strdup("=="); }
            else if (peek(l) == '>') { advance(l); t.type = TOKEN_FAT_ARROW; t.value = strdup("=>"); }
            else { t.type = TOKEN_EQUALS; t.value = strdup("="); }
            break;
        case '!':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_BANG_EQ; t.value = strdup("!="); }
            else { t.type = TOKEN_BANG; t.value = strdup("!"); }
            break;
        case '<':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_LT_EQ; t.value = strdup("<="); }
            else { t.type = TOKEN_LT; t.value = strdup("<"); }
            break;
        case '>':
            if (peek(l) == '=') { advance(l); t.type = TOKEN_GT_EQ; t.value = strdup(">="); }
            else { t.type = TOKEN_GT; t.value = strdup(">"); }
            break;
        case '&':
            if (peek(l) == '&') { advance(l); t.type = TOKEN_AND_AND; t.value = strdup("&&"); }
            else { t.type = TOKEN_UNKNOWN; t.value = strdup("&"); }
            break;
        case '|':
            if (peek(l) == '|') { advance(l); t.type = TOKEN_OR_OR; t.value = strdup("||"); }
            else { t.type = TOKEN_UNKNOWN; t.value = strdup("|"); }
            break;
        default:
            t.type = TOKEN_UNKNOWN;
            t.value = malloc(2);
            t.value[0] = c;
            t.value[1] = '\0';
            break;
    }
    return t;
}

void token_free(Token t) {
    free(t.value);
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
