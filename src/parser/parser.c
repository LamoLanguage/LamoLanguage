#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "parser.h"
#include "../error_util.h"

struct Parser {
    Lexer* lexer;
    Token current;
    int error_count;
    int panic_mode;   // se 1, suprime próximos erros até sincronizar
    const char* file_path;  // Bug #4 fix: path exibido nas mensagens de erro
    /* Phase 2: when non-zero, parse_primary does NOT treat `IDENTIFIER {`
     * as a struct literal. Used by the match parser so `match c { ... }`
     * doesn't get misparsed as `match (c { ... })`. The flag is set
     * around the scrutinee expression and cleared afterwards. */
    int no_struct_literal;
};

Parser* parser_init(Lexer* lexer) {
    return parser_init_with_file(lexer, NULL);
}

Parser* parser_init_with_file(Lexer* lexer, const char* file_path) {
    Parser* p = malloc(sizeof(Parser));
    if (!p) {
        perror("Failed to allocate Parser");
        exit(EXIT_FAILURE);
    }
    p->lexer = lexer;
    p->current = lexer_next_token(lexer);
    p->error_count = 0;
    p->panic_mode = 0;
    p->file_path = file_path;
    p->no_struct_literal = 0;  /* Phase 2: struct literal parsing enabled by default */
    // Bug #5 fix: todos os nós da AST criados a partir deste momento até o
    // fim deste parse vão receber file_path como origem. O caller é dono do
    // string e deve mantê-lo vivo enquanto a AST existir (lamo_v2.c usa
    // normalized_path que vive na CompilationState até o fim).
    ast_set_default_file_path(file_path);
    return p;
}

void parser_free(Parser* p) {
    if (!p) return;
    token_free(p->current);
    free(p);
}

const char* parser_file_path(const Parser* p) {
    return p ? p->file_path : NULL;
}

static void advance_p(Parser* p) {
    token_free(p->current);
    p->current = lexer_next_token(p->lexer);
}

// Sincroniza o parser após um erro: descarta tokens até encontrar um ponto
// estável (statement boundary como ; ou }, ou início de statement como let, fn,
// if, while, for, return, import). Técnica padrão de "synchronize-and-continue"
// (Crafting Interpreters, cap. 8).
static void parser_synchronize(Parser* p) {
    p->panic_mode = 0;

    while (p->current.type != TOKEN_EOF) {
        if (p->current.type == TOKEN_SEMICOLON) {
            advance_p(p);
            return;
        }
        switch (p->current.type) {
            case TOKEN_LET:
            case TOKEN_FN:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_FOR:
            case TOKEN_RETURN:
            case TOKEN_IMPORT:
            case TOKEN_BREAK:
            case TOKEN_CONTINUE:
            case TOKEN_AS:  /* Sprint 4: treat `as` as a sync point too */
            /* Phase 2: struct / impl / enum / match also start new top-level
             * or block-level statements, so the synchronizer should stop at
             * them after an error. */
            case TOKEN_STRUCT:
            case TOKEN_IMPL:
            case TOKEN_ENUM:
            case TOKEN_MATCH:
                return;
            default:
                advance_p(p);
                break;
        }
    }
}

void parser_error(Parser* p, const char* msg) {
    /* Delegate to the hinted variant with no hint. */
    parser_error_with_hint(p, msg, NULL);
}

/* Sprint 4: parser_error with an optional hint. The hint is printed
 * below the source snippet, formatted as "hint: <text>". Pass NULL
 * when no hint is appropriate. */
void parser_error_with_hint(Parser* p, const char* msg, const char* hint) {
    const char* label;
    if (p->panic_mode) {
        return; // já estamos em recuperação, ignora erros derivados
    }
    p->panic_mode = 1;
    p->error_count++;
    // Bug #4 fix: inclui o path do arquivo na mensagem de erro, igual ao
    // semantic.c. Caso o parser tenha sido criado sem file_path (caso legado
    // de parser_init()), usa "<input>".
    label = p->file_path ? p->file_path : "<input>";
    /* Sprint 4: color the "error" label red+bold when stderr is a TTY. */
    if (lamo_error_use_color()) {
        fprintf(stderr, "\n%s:%d:%d: %s[Syntax Error]%s %s\n",
                label, p->current.line, p->current.column,
                LAMO_COLOR_BOLD LAMO_COLOR_RED, LAMO_COLOR_RESET, msg);
    } else {
        fprintf(stderr, "\n%s:%d:%d: [Syntax Error] %s\n",
                label, p->current.line, p->current.column, msg);
    }
    fprintf(stderr, "  found token: %s (%s)\n",
            token_type_name(p->current.type),
            p->current.value ? p->current.value : "<null>");
    /* Sprint 3 fix: print the source line + a caret pointing at the
     * column. The lexer owns the source string, so we ask it for the
     * raw pointer. If the parser was constructed without a lexer
     * (defensive), no snippet is printed. */
    if (p->lexer) {
        const char* source = NULL;
        /* lexer_source() is not in the public API but the Lexer struct
         * is defined in lexer.h with a `source` field, so we can read
         * it directly. We go through the struct because there's no
         * getter. */
        source = ((Lexer*)p->lexer)->source;
        error_print_snippet(stderr, source, p->current.line, p->current.column);
    }
    /* Sprint 4: print the hint below the snippet, if any. */
    error_print_hint(stderr, hint);
    // NÃO chama exit(1) — deixa o caller tentar sincronizar e continuar.
}

int parser_had_error(const Parser* p) {
    return p->error_count > 0;
}

int parser_error_count(const Parser* p) {
    return p->error_count;
}

static void eat_p(Parser* p, LamoTokenType type) {
    if (p->current.type == type) {
        advance_p(p);
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "expected %s, got %s",
                 token_type_name(type), token_type_name(p->current.type));
        parser_error(p, buf);
    }
}

/* expect_p: like eat_p but emits a human-readable message instead of the
 * raw token type name. Use this at call sites where the context makes the
 * error unambiguous (e.g. "missing ';' after statement" is clearer than
 * "expected TOKEN_SEMICOLON, got TOKEN_IDENTIFIER"). */
static void expect_p(Parser* p, LamoTokenType type, const char* human_msg) {
    if (p->current.type == type) {
        advance_p(p);
    } else {
        parser_error(p, human_msg);
    }
}

/* Phase 2: consume an optional semicolon. The spec shows method bodies
 * and top-level programs without semicolons; we accept both styles.
 * If the next token is `;`, consume it. Otherwise, leave it alone (the
 * next statement-parsing iteration will handle it). This is safe because
 * Lamo's grammar is unambiguous without `;` in practice — every
 * statement starts with a keyword (let, fn, if, while, for, return,
 * break, continue, import, struct, impl, enum, match) or an identifier
 * followed by `=`, `(`, `.`, `[`, `++`, `--`, none of which can extend
 * the previous expression. */
static void optional_semicolon(Parser* p) {
    if (p->current.type == TOKEN_SEMICOLON) {
        advance_p(p);
    }
}

ASTNode* parse_expression(Parser* p);
static ASTNode* parse_primary(Parser* p);

/* Sprint 3: parse_postfix — handles `expr[index]` and `expr.prop` after
 * a primary expression. We treat both as left-associative postfix
 * operators, so `a[0][1].len` parses as `((a[0])[1]).len`.
 *
 * The implementation is a loop that keeps wrapping the current node
 * in an index or prop expr as long as we see `[` or `.`. */
static ASTNode* parse_postfix(Parser* p);
static ASTNode* parser_recover(Parser* p);

static ASTNode* parse_postfix(Parser* p) {
    ASTNode* node = parse_primary(p);
    while (1) {
        if (p->current.type == TOKEN_LBRACKET) {
            int line = p->current.line;
            int column = p->current.column;
            advance_p(p);  /* consume '[' */
            ASTNode* index = parse_expression(p);
            eat_p(p, TOKEN_RBRACKET);
            node = (ASTNode*)ast_new_index_expr(node, index, line, column);
        } else if (p->current.type == TOKEN_DOT) {
            int line = p->current.line;
            int column = p->current.column;
            advance_p(p);  /* consume '.' */
            if (p->current.type != TOKEN_IDENTIFIER) {
                parser_error(p, "expected property name after '.'");
                return node;  /* return what we have so far; caller may recover */
            }
            char* prop_name = strdup(p->current.value);
            int prop_line = p->current.line;
            int prop_column = p->current.column;
            advance_p(p);
            /* Phase 2: if the next token is '(', this is a method call
             * (expr.method(args)) - parse the argument list and build an
             * AST_MEMBER_CALL. Otherwise it's a property access
             * (expr.prop) - build an AST_PROP_EXPR. The semantic pass
             * dispatches AST_MEMBER_CALL based on the object's inferred
             * type: module alias / array / struct. */
            if (p->current.type == TOKEN_LPAREN) {
                advance_p(p);  /* consume '(' */
                ASTNode** args = NULL;
                int arg_count = 0;
                while (p->current.type != TOKEN_RPAREN && p->current.type != TOKEN_EOF) {
                    ASTNode* arg = parse_expression(p);
                    if (arg) {
                        ASTNode** resized = realloc(args, sizeof(ASTNode*) * (size_t)(arg_count + 1));
                        if (!resized) {
                            parser_error(p, "out of memory while growing argument list");
                            ast_free(arg);
                            for (int i = 0; i < arg_count; i++) ast_free(args[i]);
                            free(args);
                            free(prop_name);
                            ast_free(node);
                            return parser_recover(p);
                        }
                        args = resized;
                        args[arg_count++] = arg;
                    }
                    if (p->current.type == TOKEN_COMMA) advance_p(p);
                }
                expect_p(p, TOKEN_RPAREN, "missing ')' after method call arguments");
                node = (ASTNode*)ast_new_member_call(node, prop_name, args, arg_count, prop_line, prop_column);
                free(prop_name);
            } else {
                node = (ASTNode*)ast_new_prop_expr(node, prop_name, line, column);
                free(prop_name);
            }
        } else {
            break;
        }
    }
    return node;
}

// Tenta sincronizar e retornar NULL. Usado quando um erro impede de construir
// um nó válido: o caller recebe NULL e o parser continua a partir do próximo
// statement estável.
static ASTNode* parser_recover(Parser* p) {
    parser_synchronize(p);
    return NULL;
}

// Converte um literal inteiro (decimal, hex, binário) para long long.
// Trata "0x..." com strtoll base 16, "0b..." manualmente, e decimal com strtoll.
static long long parse_int_literal(const char* text) {
    if (!text) return 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return strtoll(text, NULL, 16);
    }
    if (text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        // pula "0b"
        const char* digits = text + 2;
        long long v = 0;
        while (*digits) {
            if (*digits == '0' || *digits == '1') {
                v = (v << 1) | (*digits - '0');
            }
            digits++;
        }
        return v;
    }
    return strtoll(text, NULL, 10);
}

/* ════════════════════════════════════════════════════════════════════
 * Generics PR 2 / PR 3 / PR 6: full type-annotation parsing.
 *
 * Previously every annotation site (let / param / return / field) parsed
 * a single IDENTIFIER, which made `array<int>`, `Pair<int, string>` and
 * `fn id<T>(x: T) -> T` unparsable. This block adds a recursive
 * annotation grammar shared by ALL annotation positions:
 *
 *     TYPE := IDENT [ '<' TYPE (',' TYPE)* '>' ]
 *
 * The produced string is a COMPACT form (no spaces), e.g.
 * "array<int>" or "pair<int,array<string>>". Case is preserved here;
 * normalization (Array<T> -> array<T>) happens in the semantic pass so
 * the parser stays a pure syntax layer.
 *
 * Because these functions can be invoked speculatively, they never
 * mutate global state besides the token stream itself; the probe helper
 * below saves/restores lexer position exactly like the existing generic
 * struct literal lookahead introduced in Generics PR 1.
 * ════════════════════════════════════════════════════════════════════ */

/* Tiny grow-only string buffer used while assembling nested annotations. */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} TypeStrBuf;

static void tsb_init(TypeStrBuf* sb) {
    sb->cap = 64;
    sb->len = 0;
    sb->data = malloc(sb->cap);
    if (!sb->data) {
        perror("Failed to allocate type annotation buffer");
        exit(EXIT_FAILURE);
    }
    sb->data[0] = '\0';
}

static void tsb_append(TypeStrBuf* sb, const char* text) {
    size_t need;
    if (!text) return;
    need = sb->len + strlen(text) + 1;
    if (need > sb->cap) {
        while (sb->cap < need) sb->cap *= 2;
        char* grown = realloc(sb->data, sb->cap);
        if (!grown) {
            perror("Failed to grow type annotation buffer");
            exit(EXIT_FAILURE);
        }
        sb->data = grown;
    }
    memcpy(sb->data + sb->len, text, strlen(text) + 1);
    sb->len += strlen(text);
}

static void tsb_append_char(TypeStrBuf* sb, char c) {
    char tmp[2];
    tmp[0] = c;
    tmp[1] = '\0';
    tsb_append(sb, tmp);
}

/* Recursive TYPE parser as described above. On success returns a
 * malloc'd compact string owned by the caller. On failure emits ONE
 * parser error and returns NULL (the caller cleans up and recovers). */
static char* parse_type_str(Parser* p) {
    TypeStrBuf sb;

    if (p->current.type != TOKEN_IDENTIFIER) {
        parser_error(p, "expected a type name");
        return NULL;
    }

    tsb_init(&sb);
    tsb_append(&sb, p->current.value);
    advance_p(p);

    if (p->current.type == TOKEN_LT) {
        advance_p(p);  /* consume '<' */
        tsb_append_char(&sb, '<');
        while (p->current.type != TOKEN_EOF) {
            char* inner = parse_type_str(p);
            if (!inner) {
                free(sb.data);
                return NULL;
            }
            tsb_append(&sb, inner);
            free(inner);
            if (p->current.type == TOKEN_COMMA) {
                advance_p(p);
                tsb_append_char(&sb, ',');
                continue;
            }
            break;
        }
        if (p->current.type != TOKEN_GT) {
            parser_error(p, "expected '>' to close the type argument list");
            free(sb.data);
            return NULL;
        }
        advance_p(p);  /* consume '>' */
        tsb_append_char(&sb, '>');
    }

    return sb.data;
}

/* Speculative scanner for `< IDENT ... > FOLLOW`. Consumes NOTHING on
 * balance — state is fully restored whether or not the pattern matches.
 *
 * Token alphabet inside the angles: IDENTIFIER, '<', ',', '>' only, with
 * nesting tracked by depth. This accepts nested generics like
 * `array<pair<int,string>>` while rejecting arithmetic such as
 * `a < b + c > (d)`.
 *
 * `follow` is the single token type required immediately after the
 * closing '>' (TOKEN_LBRACE for generic struct literals — PR 1 rule,
 * TOKEN_LPAREN for explicit call-site type arguments — PR 2). */
static int probe_angle_type_list(Parser* p, LamoTokenType follow) {
    /* Save lexer + current-token state (same scheme as PR 1). */
    int saved_pos = p->lexer->pos;
    int saved_line = p->lexer->line;
    int saved_column = p->lexer->column;
    Token saved_current;
    saved_current.type = p->current.type;
    saved_current.value = p->current.value ? strdup(p->current.value) : NULL;
    saved_current.line = p->current.line;
    saved_current.column = p->current.column;

    int result = 0;
    int depth = 0;
    /* Two-slot state machine:
     *   ident_allowed: next IDENTIFIER is legal here
     *   comma_allowed: next ',' is legal here
     * '<' requires !ident_allowed (opens nesting), '>' requires
     * !ident_allowed (closes one level), everything else aborts the
     * probe. This accepts exactly TYPE-list token shapes such as
     * `int`, `array<int>`, `pair<int,array<string>>, bool>`. */
    int ident_allowed = 1;
    int comma_allowed = 0;
    int saw_any = 0;

    advance_p(p);  /* consume '<' */
    depth = 1;
    while (p->current.type != TOKEN_EOF) {
        if (p->current.type == TOKEN_IDENTIFIER && ident_allowed) {
            ident_allowed = 0;
            comma_allowed = 1;
            saw_any = 1;
            advance_p(p);
        } else if (p->current.type == TOKEN_LT && !ident_allowed) {
            depth++;
            ident_allowed = 1;
            comma_allowed = 0;
            advance_p(p);
        } else if (p->current.type == TOKEN_GT && !ident_allowed) {
            advance_p(p);
            depth--;
            if (depth == 0) break;
            /* Closed a NESTED level: an identifier may follow (next
             * element of this level) and a comma may follow too. */
            ident_allowed = 1;
            comma_allowed = 1;
        } else if (p->current.type == TOKEN_COMMA && comma_allowed) {
            ident_allowed = 1;
            comma_allowed = 0;
            advance_p(p);
        } else {
            goto done_probe;
        }
    }

    if (depth == 0 && saw_any && p->current.type == follow) {
        result = 1;
    }

done_probe:
    /* Restore. Free whatever token we ended on, then reinstate the
     * saved one and rewind the lexer. */
    token_free(p->current);
    p->current = saved_current;
    p->lexer->pos = saved_pos;
    p->lexer->line = saved_line;
    p->lexer->column = saved_column;
    return result;
}

/* PR 2: lookahead confirming `name<...>(` at a call site so explicit
 * type arguments never swallow `x < y` comparisons. */
static int parser_peek_is_generic_call(Parser* p) {
    if (p->current.type != TOKEN_LT) return 0;
    return probe_angle_type_list(p, TOKEN_LPAREN);
}

/* Shared argument-list parser for every call form (plain / member /
 * statement). Consumes '(', the comma-separated expressions and ')'.
 * Returns 1 on success (*out_args may legitimately be NULL when the
 * list is empty — *out_count is then 0), 0 after emitting an error
 * (caller should recover). */
static int parse_paren_args(Parser* p, ASTNode*** out_args, int* out_count) {
    ASTNode** args = NULL;
    int arg_count = 0;

    *out_args = NULL;
    *out_count = 0;
    if (p->current.type != TOKEN_LPAREN) {
        parser_error(p, "expected '(' to start the argument list");
        return 0;
    }
    advance_p(p);
    while (p->current.type != TOKEN_RPAREN && p->current.type != TOKEN_EOF) {
        ASTNode* arg = parse_expression(p);
        if (!arg) {
            free(args);
            return 0;
        }
        {
            ASTNode** resized = realloc(args, sizeof(ASTNode*) * (size_t)(arg_count + 1));
            if (!resized) {
                parser_error(p, "out of memory while growing argument list");
                ast_free(arg);
                free(args);
                return 0;
            }
            args = resized;
        }
        args[arg_count++] = arg;
        if (p->current.type == TOKEN_COMMA) advance_p(p);
    }
    expect_p(p, TOKEN_RPAREN, "missing ')' - did you forget to close the argument list?");
    *out_args = args;
    *out_count = arg_count;
    return p->panic_mode == 0;
}

/* Committed consumption of a `<TYPE, TYPE, ...>` list using the real
 * recursive parser. Caller has already confirmed/decided this IS an
 * angle list (struct literal args, fn-call explicit args, impl echo).
 * Returns malloc'd array of malloc'd strings; *out_count receives the
 * count. On error emits and returns NULL (caller recovers). */
static char** parse_angle_type_args_real(Parser* p, int* out_count) {
    char** items = NULL;
    int count = 0;

    *out_count = 0;
    if (p->current.type != TOKEN_LT) return NULL;
    advance_p(p);  /* consume '<' */
    while (p->current.type != TOKEN_EOF) {
        char* t = parse_type_str(p);
        if (!t) {
            for (int i = 0; i < count; i++) free(items[i]);
            free(items);
            return NULL;
        }
        char** resized = realloc(items, sizeof(char*) * (size_t)(count + 1));
        if (!resized) {
            parser_error(p, "out of memory while growing type argument list");
            free(t);
            for (int i = 0; i < count; i++) free(items[i]);
            free(items);
            return NULL;
        }
        items = resized;
        items[count++] = t;
        if (p->current.type == TOKEN_COMMA) {
            advance_p(p);
            continue;
        }
        break;
    }
    if (p->current.type != TOKEN_GT) {
        parser_error(p, "expected '>' to close the type argument list");
        for (int i = 0; i < count; i++) free(items[i]);
        free(items);
        return NULL;
    }
    advance_p(p);  /* consume '>' */
    *out_count = count;
    return items;
}

/* PR 6 constraint catalogue note: type parameter lists now accept an
 * optional constraint per parameter — `T`, `T: Ord`. Grammar:
 *     PARAMS := '<' IDENT [':' IDENT] (',' IDENT [':' IDENT])* '>'
 * Constraint names are validated in the semantic pass against the
 * catalogue (Ord/Eq/Hash/Show/Num/Any) where better errors live.
 * Assumes the current token IS '<'. Returns 1 on success (arrays are
 * malloc'd, each entry strdup'd, constraints may be NULL), 0 after
 * emitting an error. */
static int parse_type_param_list(Parser* p, char*** out_names, char*** out_cons, int* out_count) {
    char** names = NULL;
    char** cons = NULL;
    int count = 0;

    *out_names = NULL;
    *out_cons = NULL;
    *out_count = 0;
    if (p->current.type != TOKEN_LT) return 1;  /* absent: trivially ok */

    advance_p(p);  /* consume '<' */
    while (p->current.type != TOKEN_EOF) {
        if (p->current.type != TOKEN_IDENTIFIER) {
            parser_error(p, "expected type parameter name after '<'");
            goto fail;
        }
        char** rn = realloc(names, sizeof(char*) * (size_t)(count + 1));
        if (!rn) { parser_error(p, "out of memory while growing type parameter list"); goto fail; }
        names = rn;
        names[count] = strdup(p->current.value);
        {
            char** rc = realloc(cons, sizeof(char*) * (size_t)(count + 1));
            if (!rc) { parser_error(p, "out of memory while growing type parameter list"); goto fail; }
            cons = rc;
            cons[count] = NULL;
        }
        advance_p(p);
        /* Optional `: ConstraintName`. */
        if (p->current.type == TOKEN_COLON) {
            advance_p(p);
            if (p->current.type != TOKEN_IDENTIFIER) {
                parser_error(p, "expected constraint name after ':' in type parameter");
                goto fail;
            }
            cons[count] = strdup(p->current.value);
            advance_p(p);
        }
        count++;
        if (p->current.type == TOKEN_COMMA) {
            advance_p(p);
            continue;
        }
        break;
    }
    if (p->current.type != TOKEN_GT) {
        parser_error(p, "expected '>' to close the type parameter list");
        goto fail;
    }
    advance_p(p);  /* consume '>' */
    *out_names = names;
    *out_cons = cons;
    *out_count = count;
    return 1;

fail:
    for (int i = 0; i < count; i++) { free(names ? names[i] : NULL); free(cons ? cons[i] : NULL); }
    free(names);
    free(cons);
    return 0;
}

/* Friendlier hints when a RESERVED WORD appears where an identifier is
 * expected (Phase 3 semantic validation item: names vs reserved words —
 * the grammar already rejects them; this just explains WHY). Keywords
 * keep their lexeme in token value, so we can quote it. */
static void parser_reserved_word_hint(char* buf, size_t buf_size, Parser* p) {
    buf[0] = '\0';
    if (p->current.type == TOKEN_IDENTIFIER || p->current.type == TOKEN_EOF) {
        return;
    }
    if (p->current.value && p->current.value[0] &&
        (strncmp(p->current.value, "TOKEN_", 6) != 0)) {
        snprintf(buf, buf_size,
                 "'%s' is a reserved word in Lamo and cannot be used as a name",
                 p->current.value);
    }
}

/* Generics PR 1: lookahead helper — REWRITTEN in Generics PR 2 to share
 * the speculative scanner (probe_angle_type_list) that also powers
 * explicit call-site type arguments. The acceptance rule is unchanged:
 * `Foo<...> {` is a generic struct literal; anything else is a
 * comparison and the caller falls through to a plain identifier.
 *
 * Does NOT consume any tokens. */
static int parser_peek_is_generic_struct_literal(Parser* p) {
    if (p->current.type != TOKEN_LT) return 0;
    return probe_angle_type_list(p, TOKEN_LBRACE);
}

static ASTNode* parse_primary(Parser* p) {
    if (p->current.type == TOKEN_INT) {
        long long val = parse_int_literal(p->current.value);
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        return (ASTNode*)ast_new_int_literal(val, line, column);
    }
    else if (p->current.type == TOKEN_FLOAT) {
        double val = strtod(p->current.value, NULL);
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        return (ASTNode*)ast_new_float_literal(val, line, column);
    }
    else if (p->current.type == TOKEN_STRING) {
        int line = p->current.line;
        int column = p->current.column;
        ASTNode* node = (ASTNode*)ast_new_string_literal(p->current.value, line, column);
        advance_p(p);
        return node;
    }
    else if (p->current.type == TOKEN_TRUE) {
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        return (ASTNode*)ast_new_bool_literal(1, line, column);
    }
    else if (p->current.type == TOKEN_FALSE) {
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        return (ASTNode*)ast_new_bool_literal(0, line, column);
    }
    else if (p->current.type == TOKEN_IDENTIFIER) {
        char* name = strdup(p->current.value);
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);

        /* Generics PR 1 (rewritten in PR 2): optional type argument list
         * for struct literals, parsed when lookahead confirms
         * `Foo<...> {`. The lookahead is necessary because `Foo < bar`
         * is also valid (a comparison expression). Full NESTED type
         * arguments are now accepted — `Foo<array<int>> { ... }` — via
         * the shared committed angle-list parser.
         *
         * We also check !p->no_struct_literal so match-scrutinee parsing
         * doesn't get confused by comparison scrutinees. */
        char** type_args = NULL;
        int type_arg_count = 0;
        if (p->current.type == TOKEN_LT && !p->no_struct_literal && parser_peek_is_generic_struct_literal(p)) {
            type_args = parse_angle_type_args_real(p, &type_arg_count);
            if (!type_args) {
                free(name);
                return parser_recover(p);
            }
            /* After '>', the next token MUST be '{' — the lookahead already
             * confirmed this. If it isn't, something is wrong; emit an error. */
            if (p->current.type != TOKEN_LBRACE) {
                parser_error(p, "expected '{' after generic struct literal type arguments");
                for (int i = 0; i < type_arg_count; i++) free(type_args[i]);
                free(type_args); free(name);
                return parser_recover(p);
            }
        }

        /* Sprint 4: `module.member(args)` in expression position. We
         * recognize this BEFORE the regular call-expression branch so the
         * `.` doesn't fall through to parse_postfix (which would wrap it
         * as a property expression and lose the call). The resulting
         * AST_MEMBER_CALL is resolved by the semantic pass against the
         * module registry. */
        if (p->current.type == TOKEN_DOT) {
            int obj_line = p->current.line;
            int obj_column = p->current.column;
            advance_p(p);  /* consume '.' */
            if (p->current.type != TOKEN_IDENTIFIER) {
                parser_error(p, "expected member name after '.' in module call expression");
                free(name);
                return parser_recover(p);
            }
            char* member_name = strdup(p->current.value);
            eat_p(p, TOKEN_IDENTIFIER);
            /* Only build a member CALL when followed by '('. Otherwise
             * fall back to a property expression (AST_PROP_EXPR) so the
             * existing `.len` and future `.prop` forms keep working. The
             * conversion is done by building the prop_expr here and letting
             * parse_postfix continue wrapping if there are more dots. */
        if (p->current.type == TOKEN_LPAREN) {
            int arg_count = 0;
            ASTNode** args = NULL;
            if (!parse_paren_args(p, &args, &arg_count)) {
                free(name); free(member_name); return parser_recover(p);
            }
            ASTNode* obj = (ASTNode*)ast_new_identifier(name, obj_line, obj_column);
            ASTNode* node = (ASTNode*)ast_new_member_call(obj, member_name, args, arg_count, line, column);
            free(name);
            free(member_name);
            return node;
        } else {
                /* `module.member` without a call — build a prop_expr so
                 * parse_postfix can keep wrapping further `.x.y.z` chains.
                 * The semantic pass resolves whether `object` is a module
                 * alias or a regular value (e.g. array `.len`). */
                ASTNode* obj = (ASTNode*)ast_new_identifier(name, obj_line, obj_column);
                ASTNode* node = (ASTNode*)ast_new_prop_expr(obj, member_name, line, column);
                free(name);
                free(member_name);
                return node;
            }
        }

        if (p->current.type == TOKEN_LPAREN) {
            int arg_count = 0;
            ASTNode** args = NULL;
            if (!parse_paren_args(p, &args, &arg_count)) { free(name); return parser_recover(p); }
            ASTNode* node = (ASTNode*)ast_new_call_expr(name, args, arg_count, line, column);
            free(name);
            return node;
        }

        /* Generics PR 2: explicit type arguments at a call site —
         * `f<int>(5)` or `map<int, string>(xs, f)`. Only taken when the
         * speculative scanner confirms the full `< ... > (` shape, so
         * ordinary comparisons like `x < y` can never misparse here
         * (same disambiguation strategy as PR 1's struct literals).
         * Turbofish (`f::<int>`) is deliberately not supported — RFC §4.5. */
        if (p->current.type == TOKEN_LT && parser_peek_is_generic_call(p)) {
            int ta_count = 0;
            char** tas = parse_angle_type_args_real(p, &ta_count);
            if (!tas) { free(name); return parser_recover(p); }
            int arg_count = 0;
            ASTNode** args = NULL;
            if (!parse_paren_args(p, &args, &arg_count)) {
                for (int i = 0; i < ta_count; i++) free(tas[i]);
                free(tas); free(name);
                return parser_recover(p);
            }
            ASTNode* node = (ASTNode*)ast_new_call_expr_typed(name, tas, ta_count,
                                                              args, arg_count, line, column);
            for (int i = 0; i < ta_count; i++) free(tas[i]);
            free(tas);
            free(name);
            return node;
        } else if (p->current.type == TOKEN_LBRACE && !p->no_struct_literal) {
            /* Phase 2: struct literal — `Name { field: value, ... }`.
             * We treat `IDENTIFIER {` in expression position as a struct
             * literal. The semantic pass rejects it if `Name` is not a
             * declared struct type. Field order does NOT need to match
             * the struct declaration order — we look up each field name
             * at compile time.
             *
             * Why this is safe: Lamo has no block expressions, so a `{`
             * after an identifier in expression position is unambiguously
             * a struct literal. Statements like `if (cond) { ... }` work
             * because the `(cond)` is parenthesized — after parse_primary
             * returns the cond, the next token is `)` (not `{`), so this
             * branch doesn't fire. */
            advance_p(p);  /* consume '{' */
            char** field_names = NULL;
            ASTNode** field_values = NULL;
            int field_count = 0;
            while (p->current.type != TOKEN_RBRACE && p->current.type != TOKEN_EOF) {
                if (p->current.type != TOKEN_IDENTIFIER) {
                    parser_error(p, "expected field name in struct literal");
                    for (int i = 0; i < field_count; i++) { free(field_names[i]); ast_free(field_values[i]); }
                    free(field_names);
                    free(field_values);
                    free(name);
                    return parser_recover(p);
                }
                char* fname = strdup(p->current.value);
                advance_p(p);
                expect_p(p, TOKEN_COLON, "expected ':' after field name in struct literal");
                ASTNode* fval = parse_expression(p);
                /* Grow arrays. Realloc one at a time and assign back
                 * immediately, so each variable always holds a valid
                 * pointer. This avoids -Wuse-after-free: after a
                 * successful realloc, the old pointer may be invalid. */
                {
                    char** fn_resized = realloc(field_names, sizeof(char*) * (size_t)(field_count + 1));
                    if (!fn_resized) {
                        parser_error(p, "out of memory while growing struct literal field list");
                        free(fname);
                        ast_free(fval);
                        for (int i = 0; i < field_count; i++) { free(field_names[i]); ast_free(field_values[i]); }
                        free(field_names);
                        free(field_values);
                        free(name);
                        return parser_recover(p);
                    }
                    field_names = fn_resized;
                }
                {
                    ASTNode** fv_resized = realloc(field_values, sizeof(ASTNode*) * (size_t)(field_count + 1));
                    if (!fv_resized) {
                        parser_error(p, "out of memory while growing struct literal field list");
                        free(fname);
                        ast_free(fval);
                        for (int i = 0; i < field_count; i++) { free(field_names[i]); ast_free(field_values[i]); }
                        free(field_names);
                        free(field_values);
                        free(name);
                        return parser_recover(p);
                    }
                    field_values = fv_resized;
                }
                field_names[field_count] = fname;
                field_values[field_count] = fval;
                field_count++;
                if (p->current.type == TOKEN_COMMA) advance_p(p);
                else if (p->current.type == TOKEN_SEMICOLON) advance_p(p);
            }
            expect_p(p, TOKEN_RBRACE, "missing '}' at end of struct literal");
            ASTNode* node = (ASTNode*)ast_new_struct_literal(name, field_names, field_values, field_count, type_args, type_arg_count, line, column);
            /* Free field_names array contents (the AST strdup'd them) and
             * the field_values array (the AST took ownership of the elements).
             * Free the arrays themselves (the AST made its own copies).
             * Generics PR 1: also free type_args contents (the AST strdup'd them). */
            for (int i = 0; i < field_count; i++) free(field_names[i]);
            free(field_names);
            free(field_values);
            for (int i = 0; i < type_arg_count; i++) free(type_args[i]);
            free(type_args);
            free(name);
            return node;
        } else {
            /* If we somehow parsed type_args but the next token is not '{',
             * we're in an inconsistent state (the lookahead should have
             * prevented this). Free type_args and fall through to identifier. */
            if (type_arg_count > 0) {
                for (int i = 0; i < type_arg_count; i++) free(type_args[i]);
                free(type_args);
                type_args = NULL;
                type_arg_count = 0;
            }
            ASTNode* node = (ASTNode*)ast_new_identifier(name, line, column);
            free(name);
            return node;
        }
    }
    else if (p->current.type == TOKEN_LPAREN) {
        int line = p->current.line;
        int column = p->current.column;
        eat_p(p, TOKEN_LPAREN);
        ASTNode* expr = parse_expression(p);
        eat_p(p, TOKEN_RPAREN);
        return (ASTNode*)ast_new_grouping_expr(expr, line, column);
    }
    else if (p->current.type == TOKEN_LBRACKET) {
        /* Sprint 3: array literal — `[expr, expr, ...]`. The empty
         * array `[]` is allowed and produces a zero-length array. */
        int line = p->current.line;
        int column = p->current.column;
        ASTNode** elements = NULL;
        int element_count = 0;
        eat_p(p, TOKEN_LBRACKET);
        while (p->current.type != TOKEN_RBRACKET && p->current.type != TOKEN_EOF) {
            ASTNode* elem = parse_expression(p);
            if (elem) {
                ASTNode** resized = realloc(elements, sizeof(ASTNode*) * (size_t)(element_count + 1));
                if (!resized) {
                    parser_error(p, "out of memory while growing array literal");
                    ast_free(elem);
                    for (int i = 0; i < element_count; i++) ast_free(elements[i]);
                    free(elements);
                    return parser_recover(p);
                }
                elements = resized;
                elements[element_count++] = elem;
            }
            if (p->current.type == TOKEN_COMMA) advance_p(p);
        }
        eat_p(p, TOKEN_RBRACKET);
        return (ASTNode*)ast_new_array_literal(elements, element_count, line, column);
    }
    else {
        char buf[256];
        snprintf(buf, sizeof(buf), "invalid expression, found %s",
                 token_type_name(p->current.type));
        parser_error(p, buf);
        return NULL;
    }
}

static ASTNode* parse_unary(Parser* p) {
    if (p->current.type == TOKEN_BANG || p->current.type == TOKEN_MINUS) {
        LamoTokenType op_type = p->current.type;
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        ASTNode* right = parse_unary(p);
        return (ASTNode*)ast_new_unary_expr(op_type, right, line, column);
    } else {
        /* Sprint 3: parse_postfix handles `expr[idx]` and `expr.prop`
         * on top of parse_primary. */
        return parse_postfix(p);
    }
}

static ASTNode* parse_factor(Parser* p) {
    ASTNode* left = parse_unary(p);
    while (p->current.type == TOKEN_STAR || p->current.type == TOKEN_SLASH ||
           p->current.type == TOKEN_PERCENT) {
        LamoTokenType op_type = p->current.type;
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        ASTNode* right = parse_unary(p);
        left = (ASTNode*)ast_new_binary_expr(left, op_type, right, line, column);
    }
    return left;
}

static ASTNode* parse_term(Parser* p) {
    ASTNode* left = parse_factor(p);
    while (p->current.type == TOKEN_PLUS || p->current.type == TOKEN_MINUS) {
        LamoTokenType op_type = p->current.type;
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        ASTNode* right = parse_factor(p);
        left = (ASTNode*)ast_new_binary_expr(left, op_type, right, line, column);
    }
    return left;
}

static ASTNode* parse_comparison(Parser* p) {
    ASTNode* left = parse_term(p);
    while (p->current.type == TOKEN_LT || p->current.type == TOKEN_GT ||
           p->current.type == TOKEN_LT_EQ || p->current.type == TOKEN_GT_EQ) {
        LamoTokenType op_type = p->current.type;
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        ASTNode* right = parse_term(p);
        left = (ASTNode*)ast_new_binary_expr(left, op_type, right, line, column);
    }
    return left;
}

static ASTNode* parse_equality(Parser* p) {
    ASTNode* left = parse_comparison(p);
    while (p->current.type == TOKEN_EQ_EQ || p->current.type == TOKEN_BANG_EQ) {
        LamoTokenType op_type = p->current.type;
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        ASTNode* right = parse_comparison(p);
        left = (ASTNode*)ast_new_binary_expr(left, op_type, right, line, column);
    }
    return left;
}

static ASTNode* parse_and(Parser* p) {
    ASTNode* left = parse_equality(p);
    while (p->current.type == TOKEN_AND_AND) {
        LamoTokenType op_type = p->current.type;
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        ASTNode* right = parse_equality(p);
        left = (ASTNode*)ast_new_binary_expr(left, op_type, right, line, column);
    }
    return left;
}

ASTNode* parse_expression(Parser* p) {
    ASTNode* left = parse_and(p);
    while (p->current.type == TOKEN_OR_OR) {
        LamoTokenType op_type = p->current.type;
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);
        ASTNode* right = parse_and(p);
        left = (ASTNode*)ast_new_binary_expr(left, op_type, right, line, column);
    }
    return left;
}

ASTNode* parse_statement(Parser* p);

static ASTNode* parse_block(Parser* p) {
    int line = p->current.line;
    int column = p->current.column;
    expect_p(p, TOKEN_LBRACE, "expected '{' to open block");
    ASTNode* head = NULL;
    ASTNode* current = NULL;
    while (p->current.type != TOKEN_RBRACE && p->current.type != TOKEN_EOF) {
        ASTNode* stmt = parse_statement(p);
        if (stmt) {
            if (!head) {
                head = stmt;
                current = stmt;
            } else {
                current->next = stmt;
                current = stmt;
            }
        }
        // Se um erro ocorreu dentro do statement, sincroniza dentro do bloco.
        if (p->panic_mode) {
            parser_synchronize(p);
        }
    }
    expect_p(p, TOKEN_RBRACE, "missing '}' — block not closed");
    return (ASTNode*)ast_new_block(head, line, column);
}

ASTNode* parse_statement(Parser* p) {
    if (p->current.type == TOKEN_LET) {
        eat_p(p, TOKEN_LET);
        if (p->current.type != TOKEN_IDENTIFIER) {
            /* Reserved-word friendliness: `let fn = ...` explains itself. */
            char rw_hint[160];
            parser_reserved_word_hint(rw_hint, sizeof(rw_hint), p);
            parser_error_with_hint(p, "expected identifier after let",
                                   rw_hint[0] ? rw_hint : NULL);
            return parser_recover(p);
        }
        char* name = strdup(p->current.value);
        int line = p->current.line;
        int column = p->current.column;
        eat_p(p, TOKEN_IDENTIFIER);
        /* Sprint 3: optional type annotation `: int | float | string | bool`.
         * Generics PR 2/3: annotations are now FULL types via the shared
         * recursive parser (TYPE := IDENT [ '<' TYPE,... '>' ]), so
         * `let xs: array<int> = ...` and `let p: Pair<int, string> = ...`
         * parse here and are validated semantically downstream. */
        char* type_annotation = NULL;
        if (p->current.type == TOKEN_COLON) {
            eat_p(p, TOKEN_COLON);
            type_annotation = parse_type_str(p);
            if (!type_annotation) {
                free(name);
                return parser_recover(p);
            }
        }
        eat_p(p, TOKEN_EQUALS);
        /* Sprint 1 fix: explicit "missing initializer" error for `let x = ;`.
         * Without this, parse_expression() would fall through to parse_primary()
         * and emit a generic "invalid expression, found ;" — which technically
         * reports the error but is unhelpful to a beginner writing Lamo.
         * The explicit message tells them exactly what is wrong.
         * Sprint 4: also emit a hint suggesting the most likely fix. */
        if (p->current.type == TOKEN_SEMICOLON) {
            parser_error_with_hint(p,
                "missing initializer after '=' in let declaration",
                "did you forget a value after '='? e.g. `let x = 0;`");
            free(name);
            free(type_annotation);
            return parser_recover(p);
        }
        ASTNode* initializer = parse_expression(p);
        /* Phase 2: semicolon is optional (spec shows code without `;`). */
        optional_semicolon(p);
        ASTNode* node = (ASTNode*)ast_new_var_decl_typed(name, initializer, type_annotation, line, column);
        free(name);
        free(type_annotation);
        return node;
    }
    else if (p->current.type == TOKEN_FN) {
        eat_p(p, TOKEN_FN);
        if (p->current.type != TOKEN_IDENTIFIER) {
            char rw_hint[160];
            parser_reserved_word_hint(rw_hint, sizeof(rw_hint), p);
            parser_error_with_hint(p, "expected function name after fn",
                                   rw_hint[0] ? rw_hint : NULL);
            return parser_recover(p);
        }
        char* name = strdup(p->current.value);
        int line = p->current.line;
        int column = p->current.column;
        eat_p(p, TOKEN_IDENTIFIER);

        /* Generics PR 2: optional type parameter list `fn id<T, U>(...)`.
         * Unambiguous at this position: after a function name the only
         * legal continuation is '(' (params), so '<' can ONLY start the
         * generic parameter list — no speculative scanning needed.
         * PR 6: each parameter optionally carries a constraint (`T: Ord`). */
        char** fn_type_params = NULL;
        char** fn_type_constraints = NULL;
        int fn_type_param_count = 0;
        if (p->current.type == TOKEN_LT) {
            if (!parse_type_param_list(p, &fn_type_params, &fn_type_constraints,
                                       &fn_type_param_count)) {
                free(name);
                return parser_recover(p);
            }
        }

        expect_p(p, TOKEN_LPAREN, "expected '(' after function name");

        char** params = NULL;
        char** param_types = NULL;  /* Sprint 3: per-param type annotations */
        int param_count = 0;
        int has_any_type_annotation = 0;

        while (p->current.type != TOKEN_RPAREN && p->current.type != TOKEN_EOF) {
            if (p->current.type != TOKEN_IDENTIFIER) {
                char rw_hint[160];
                parser_reserved_word_hint(rw_hint, sizeof(rw_hint), p);
                parser_error_with_hint(p, "expected parameter name",
                                       rw_hint[0] ? rw_hint : NULL);
                break;
            }
            char** resized = realloc(params, sizeof(char*) * (size_t)(param_count + 1));
            if (!resized) {
                parser_error(p, "out of memory while growing parameter list");
                for (int i = 0; i < param_count; i++) free(params[i]);
                free(params);
                if (param_types) {
                    for (int i = 0; i < param_count; i++) free(param_types[i]);
                    free(param_types);
                }
                free(name);
                return parser_recover(p);
            }
            params = resized;
            params[param_count] = strdup(p->current.value);
            eat_p(p, TOKEN_IDENTIFIER);

            /* Sprint 3: optional `: type` per parameter. We allocate the
             * param_types array lazily — only if at least one parameter
             * has an annotation. This keeps the common case (no
             * annotations) allocation-free. */
            if (p->current.type == TOKEN_COLON) {
                if (!param_types) {
                    param_types = calloc((size_t)param_count, sizeof(char*));
                    if (!param_types) {
                        parser_error(p, "out of memory while allocating param_types array");
                        for (int i = 0; i < param_count; i++) free(params[i]);
                        free(params);
                        free(name);
                        return parser_recover(p);
                    }
                }
                /* Grow param_types to match params. */
                {
                    char** pt_resized = realloc(param_types, sizeof(char*) * (size_t)(param_count + 1));
                    if (!pt_resized) {
                        parser_error(p, "out of memory while growing param_types array");
                        for (int i = 0; i < param_count; i++) free(params[i]);
                        free(params);
                        for (int i = 0; i < param_count; i++) free(param_types[i]);
                        free(param_types);
                        free(name);
                        return parser_recover(p);
                    }
                    param_types = pt_resized;
                    param_types[param_count] = NULL;
                }
                eat_p(p, TOKEN_COLON);
                /* Generics PR 2/3: full type annotations, including type
                 * parameters (`x: T`) and nested generics (`xs: array<T>`). */
                param_types[param_count] = parse_type_str(p);
                if (!param_types[param_count]) {
                    for (int i = 0; i <= param_count; i++) free(params[i]);
                    free(params);
                    for (int i = 0; i < param_count; i++) free(param_types[i]);
                    free(param_types);
                    for (int i = 0; i < fn_type_param_count; i++) { free(fn_type_params[i]); free(fn_type_constraints[i]); }
                    free(fn_type_params); free(fn_type_constraints);
                    free(name);
                    return parser_recover(p);
                }
                has_any_type_annotation = 1;
            } else if (param_types) {
                /* Need to grow param_types even when this param has no
                 * annotation, to keep the arrays in sync. */
                char** pt_resized = realloc(param_types, sizeof(char*) * (size_t)(param_count + 1));
                if (!pt_resized) {
                    parser_error(p, "out of memory while growing param_types array");
                    free(name);
                    return parser_recover(p);
                }
                param_types = pt_resized;
                param_types[param_count] = NULL;
            }
            param_count++;
            if (p->current.type == TOKEN_COMMA) advance_p(p);
        }
        expect_p(p, TOKEN_RPAREN, "missing ')' — did you forget to close the parameter list?");
        char* return_type_annotation = NULL;
        if (p->current.type == TOKEN_ARROW) {
            eat_p(p, TOKEN_ARROW);
            /* Generics PR 2: full return-type annotation (`-> T`,
             * `-> array<U>`, `-> Option<int>`). `void` allowed; validated
             * semantically. */
            return_type_annotation = parse_type_str(p);
            if (!return_type_annotation) {
                for (int i = 0; i < param_count; i++) free(params[i]);
                free(params);
                if (param_types) {
                    for (int i = 0; i < param_count; i++) free(param_types[i]);
                    free(param_types);
                }
                for (int i = 0; i < fn_type_param_count; i++) { free(fn_type_params[i]); free(fn_type_constraints[i]); }
                free(fn_type_params); free(fn_type_constraints);
                free(name);
                return parser_recover(p);
            }
            (void)has_any_type_annotation;
        }

        ASTNode* body = parse_block(p);
        ASTNode* node = (ASTNode*)ast_new_fn_decl_generic(name,
                                                          fn_type_params, fn_type_constraints, fn_type_param_count,
                                                          params, param_types, param_count,
                                                          return_type_annotation, body, line, column);
        free(name);
        free(return_type_annotation);
        free(fn_type_params);
        free(fn_type_constraints);
        return node;
    }
    else if (p->current.type == TOKEN_IMPORT) {
        int line = p->current.line;
        int column = p->current.column;
        eat_p(p, TOKEN_IMPORT);
        /* Phase 2: accept either a STRING path (legacy) or an IDENTIFIER
         * (bare module name like `import math`). For the bare form, we
         * synthesize the path `<name>.lamo` and (if no `as` clause is
         * given) use the name itself as the alias. This matches the
         * spec syntax:
         *   import math        // -> import "math.lamo" as math
         *   import math as m   // -> import "math.lamo" as m
         * The loader (lamo_v2.c::resolve_import_path) resolves the path
         * relative to the importing file. */
        char* path = NULL;
        char* alias = NULL;
        if (p->current.type == TOKEN_STRING) {
            path = strdup(p->current.value);
            eat_p(p, TOKEN_STRING);
        } else if (p->current.type == TOKEN_IDENTIFIER) {
            /* Phase 3 (stdlib): accept dotted module names like `import std.io`.
             * We accumulate the dotted path into `<a>/<b>/<c>.lamo` and use
             * the LAST component as the default alias (so `import std.io`
             * becomes `import "std/io.lamo" as io`).
             *
             * This is the syntax the standard library uses to expose itself
             * without colliding with user files: a top-level `std/` directory
             * holds the standard modules, and the loader knows to look there
             * when the path starts with `std/`. */
            char* first = strdup(p->current.value);
            char* last_segment = strdup(p->current.value);
            size_t total_len = strlen(first);
            eat_p(p, TOKEN_IDENTIFIER);
            while (p->current.type == TOKEN_DOT) {
                eat_p(p, TOKEN_DOT);
                if (p->current.type != TOKEN_IDENTIFIER) {
                    parser_error(p, "expected identifier after '.' in import path");
                    free(first); free(last_segment);
                    return parser_recover(p);
                }
                /* Append "/<next>" to first (which accumulates the path). */
                {
                    size_t seg_len = strlen(p->current.value);
                    char* grown = (char*)realloc(first, total_len + 1 + seg_len + 1);
                    if (!grown) { free(first); free(last_segment); parser_error(p, "out of memory in import path"); return parser_recover(p); }
                    first = grown;
                    first[total_len] = '/';
                    memcpy(first + total_len + 1, p->current.value, seg_len + 1);
                    total_len += 1 + seg_len;
                }
                free(last_segment);
                last_segment = strdup(p->current.value);
                eat_p(p, TOKEN_IDENTIFIER);
            }
            /* Synthesize "<path>.lamo". */
            size_t plen = total_len + 6;
            path = malloc(plen);
            snprintf(path, plen, "%s.lamo", first);
            /* Default alias is the last segment. */
            alias = last_segment;
            free(first);
        } else {
            parser_error(p, "expected string path or module name after import");
            return parser_recover(p);
        }
        /* Optional `as IDENTIFIER` clause. Overrides the default alias. */
        if (p->current.type == TOKEN_AS) {
            eat_p(p, TOKEN_AS);
            if (p->current.type != TOKEN_IDENTIFIER) {
                parser_error(p, "expected identifier after 'as' in import");
                free(path);
                free(alias);
                return parser_recover(p);
            }
            free(alias);  /* free the default alias (if any) */
            alias = strdup(p->current.value);
            eat_p(p, TOKEN_IDENTIFIER);
        }
        optional_semicolon(p);  /* Phase 2: `;` optional */
        ASTNode* node = (ASTNode*)ast_new_import_decl_aliased(path, alias, line, column);
        free(path);
        free(alias);
        return node;
    }
    /* ─── Phase 2: struct / impl / enum / match declarations ─────────── */
    else if (p->current.type == TOKEN_STRUCT) {
        int line = p->current.line;
        int column = p->current.column;
        eat_p(p, TOKEN_STRUCT);
        if (p->current.type != TOKEN_IDENTIFIER) {
            parser_error(p, "expected struct name after 'struct'");
            return parser_recover(p);
        }
        char* name = strdup(p->current.value);
        eat_p(p, TOKEN_IDENTIFIER);
        /* Generics PR 1 (rewritten PR 6): optional type parameter list
         * `<T, K: Ord, V>`. We distinguish this from the comparison
         * operator `<` by context: after a struct name, the only valid
         * follow tokens are `{` (start of body) or `<` (start of type
         * params). A `<` here cannot be a comparison because there's no
         * left-hand expression. Constraints (`: Ord`) are parsed here
         * and validated semantically against the catalogue. */
        char** type_params = NULL;
        char** struct_param_constraints = NULL;
        int type_param_count = 0;
        if (p->current.type == TOKEN_LT) {
            if (!parse_type_param_list(p, &type_params, &struct_param_constraints,
                                       &type_param_count)) {
                free(name);
                return parser_recover(p);
            }
        }
        expect_p(p, TOKEN_LBRACE, "expected '{' to open struct body");
        char** field_names = NULL;
        char** field_types = NULL;
        int field_count = 0;
        while (p->current.type != TOKEN_RBRACE && p->current.type != TOKEN_EOF) {
            if (p->current.type != TOKEN_IDENTIFIER) {
                parser_error(p, "expected field name in struct body");
                for (int i = 0; i < field_count; i++) { free(field_names[i]); free(field_types[i]); }
                free(field_names); free(field_types); free(name);
                return parser_recover(p);
            }
            char* fname = strdup(p->current.value);
            eat_p(p, TOKEN_IDENTIFIER);
            expect_p(p, TOKEN_COLON, "expected ':' after field name in struct");
            /* Generics PR 2/3: full field-type annotation. Commas INSIDE
             * '<...>' belong to the type and are consumed by the
             * parse_type_str recursion; only commas at bracket depth 0
             * reach the field-separator handling below. */
            char* ftype = parse_type_str(p);
            if (!ftype) {
                free(fname); free(name);
                for (int i = 0; i < field_count; i++) { free(field_names[i]); free(field_types[i]); }
                free(field_names); free(field_types);
                return parser_recover(p);
            }
            /* Grow both arrays. Realloc one at a time and assign back
             * immediately to avoid -Wuse-after-free. */
            {
                char** fn_r = realloc(field_names, sizeof(char*) * (size_t)(field_count + 1));
                if (!fn_r) {
                    parser_error(p, "out of memory while growing struct field list");
                    free(fname); free(ftype); free(name);
                    for (int i = 0; i < field_count; i++) { free(field_names[i]); free(field_types[i]); }
                    free(field_names); free(field_types);
                    return parser_recover(p);
                }
                field_names = fn_r;
            }
            {
                char** ft_r = realloc(field_types, sizeof(char*) * (size_t)(field_count + 1));
                if (!ft_r) {
                    parser_error(p, "out of memory while growing struct field list");
                    free(fname); free(ftype); free(name);
                    for (int i = 0; i < field_count; i++) { free(field_names[i]); free(field_types[i]); }
                    free(field_names); free(field_types);
                    return parser_recover(p);
                }
                field_types = ft_r;
            }
            field_names[field_count] = fname;
            field_types[field_count] = ftype;
            field_count++;
            /* Fields can be separated by `,` or `;` or just newlines
             * (the spec example shows no separators, just one field per
             * line). We accept all three for flexibility. */
            if (p->current.type == TOKEN_COMMA) advance_p(p);
            else if (p->current.type == TOKEN_SEMICOLON) advance_p(p);
        }
        expect_p(p, TOKEN_RBRACE, "missing '}' at end of struct body");
        /* Optional trailing semicolon (struct decls usually don't have one,
         * but we allow it for symmetry with other declarations). */
        if (p->current.type == TOKEN_SEMICOLON) advance_p(p);
        ASTNode* node = (ASTNode*)ast_new_struct_decl(name, field_names, field_types, field_count,
                                                      type_params, struct_param_constraints,
                                                      type_param_count, line, column);
        for (int i = 0; i < field_count; i++) { free(field_names[i]); free(field_types[i]); }
        free(field_names); free(field_types);
        for (int i = 0; i < type_param_count; i++) { free(type_params[i]); free(struct_param_constraints[i]); }
        free(type_params); free(struct_param_constraints);
        free(name);
        return node;
    }
    else if (p->current.type == TOKEN_IMPL) {
        int line = p->current.line;
        int column = p->current.column;
        eat_p(p, TOKEN_IMPL);

        /* RFC §4.4: optional type parameter list directly after the
         * `impl` keyword — `impl<T> Stack<T> { ... }`. Context makes the
         * '<' unambiguous (an identifier is required next either way). */
        char** impl_type_params = NULL;
        char** impl_param_constraints = NULL;
        int impl_type_param_count = 0;
        if (p->current.type == TOKEN_LT) {
            if (!parse_type_param_list(p, &impl_type_params, &impl_param_constraints,
                                       &impl_type_param_count)) {
                return parser_recover(p);
            }
        }

        if (p->current.type != TOKEN_IDENTIFIER) {
            parser_error(p, "expected struct name after 'impl'");
            for (int i = 0; i < impl_type_param_count; i++) { free(impl_type_params[i]); free(impl_param_constraints[i]); }
            free(impl_type_params); free(impl_param_constraints);
            return parser_recover(p);
        }
        char* struct_name = strdup(p->current.value);
        eat_p(p, TOKEN_IDENTIFIER);

        /* RFC §4.4: optional echo of the type parameters on the struct
         * name — `Stack<T>`. Unambiguous in this position: an impl body
         * must open with '{', so '<' can ONLY start the echo list (same
         * contextual argument as the struct/impl declaration sites).
         * Must reference exactly the declared parameter names, validated
         * semantically with better messages. */
        char** impl_type_args = NULL;
        int impl_type_arg_count = 0;
        if (p->current.type == TOKEN_LT) {
            impl_type_args = parse_angle_type_args_real(p, &impl_type_arg_count);
            if (!impl_type_args) {
                for (int i = 0; i < impl_type_param_count; i++) { free(impl_type_params[i]); free(impl_param_constraints[i]); }
                free(impl_type_params); free(impl_param_constraints);
                free(struct_name);
                return parser_recover(p);
            }
        }

        expect_p(p, TOKEN_LBRACE, "expected '{' to open impl body");
        /* Parse a sequence of `fn ...` declarations as the methods. */
        ASTNode* head = NULL;
        ASTNode* tail = NULL;
        while (p->current.type != TOKEN_RBRACE && p->current.type != TOKEN_EOF) {
            /* Reuse parse_statement's TOKEN_FN handling by calling it
             * directly. parse_statement returns AST_FN_DECL nodes for
             * `fn name(params) { body }`. We chain them via ->next. */
            if (p->current.type != TOKEN_FN) {
                parser_error(p, "expected 'fn' inside impl block");
                parser_synchronize(p);
                /* Try to recover by skipping to next 'fn' or '}'. */
                while (p->current.type != TOKEN_FN && p->current.type != TOKEN_RBRACE && p->current.type != TOKEN_EOF) {
                    advance_p(p);
                }
                continue;
            }
            ASTNode* method = parse_statement(p);
            if (method) {
                if (!head) { head = method; tail = method; }
                else { tail->next = method; tail = method; }
            }
            if (p->panic_mode) parser_synchronize(p);
        }
        expect_p(p, TOKEN_RBRACE, "missing '}' at end of impl body");
        if (p->current.type == TOKEN_SEMICOLON) advance_p(p);
        ASTNode* node = (ASTNode*)ast_new_impl_decl_generic(struct_name,
                                                            impl_type_params, impl_type_param_count,
                                                            impl_type_args, impl_type_arg_count,
                                                            head, line, column);
        for (int i = 0; i < impl_type_param_count; i++) { free(impl_type_params[i]); free(impl_param_constraints[i]); }
        free(impl_type_params); free(impl_param_constraints);
        for (int i = 0; i < impl_type_arg_count; i++) free(impl_type_args[i]);
        free(impl_type_args);
        free(struct_name);
        return node;
    }
    else if (p->current.type == TOKEN_ENUM) {
        int line = p->current.line;
        int column = p->current.column;
        eat_p(p, TOKEN_ENUM);
        if (p->current.type != TOKEN_IDENTIFIER) {
            parser_error(p, "expected enum name after 'enum'");
            return parser_recover(p);
        }
        char* name = strdup(p->current.value);
        eat_p(p, TOKEN_IDENTIFIER);
        expect_p(p, TOKEN_LBRACE, "expected '{' to open enum body");
        char** variants = NULL;
        int variant_count = 0;
        while (p->current.type != TOKEN_RBRACE && p->current.type != TOKEN_EOF) {
            if (p->current.type != TOKEN_IDENTIFIER) {
                parser_error(p, "expected variant name in enum body");
                for (int i = 0; i < variant_count; i++) free(variants[i]);
                free(variants); free(name);
                return parser_recover(p);
            }
            char* vname = strdup(p->current.value);
            eat_p(p, TOKEN_IDENTIFIER);
            {
                char** resized = realloc(variants, sizeof(char*) * (size_t)(variant_count + 1));
                if (!resized) {
                    parser_error(p, "out of memory while growing enum variant list");
                    free(vname);
                    for (int i = 0; i < variant_count; i++) free(variants[i]);
                    free(variants); free(name);
                    return parser_recover(p);
                }
                variants = resized;
                variants[variant_count++] = vname;
            }
            if (p->current.type == TOKEN_COMMA) advance_p(p);
            else if (p->current.type == TOKEN_SEMICOLON) advance_p(p);
        }
        expect_p(p, TOKEN_RBRACE, "missing '}' at end of enum body");
        if (p->current.type == TOKEN_SEMICOLON) advance_p(p);
        ASTNode* node = (ASTNode*)ast_new_enum_decl(name, variants, variant_count, line, column);
        for (int i = 0; i < variant_count; i++) free(variants[i]);
        free(variants); free(name);
        return node;
    }
    else if (p->current.type == TOKEN_MATCH) {
        int line = p->current.line;
        int column = p->current.column;
        eat_p(p, TOKEN_MATCH);
        /* Phase 2: disable struct literal parsing while parsing the
         * scrutinee so `match c { ... }` doesn't get misparsed as
         * `match (c { ... })` (a struct literal swallowing the match
         * body). The flag is cleared after the scrutinee is parsed. */
        p->no_struct_literal = 1;
        ASTNode* scrutinee = parse_expression(p);
        p->no_struct_literal = 0;
        expect_p(p, TOKEN_LBRACE, "expected '{' to open match body");
        char** patterns = NULL;
        int* pattern_is_wildcard = NULL;
        ASTNode** bodies = NULL;
        int arm_count = 0;
        while (p->current.type != TOKEN_RBRACE && p->current.type != TOKEN_EOF) {
            char* pat = NULL;
            int is_wild = 0;
            if (p->current.type == TOKEN_IDENTIFIER) {
                /* "_" is the wildcard pattern (we read it as an identifier
                 * since the lexer doesn't have a special token for it). */
                if (strcmp(p->current.value, "_") == 0) {
                    is_wild = 1;
                    pat = strdup("_");
                } else {
                    pat = strdup(p->current.value);
                }
                eat_p(p, TOKEN_IDENTIFIER);
            } else {
                parser_error(p, "expected pattern (variant name or '_') in match arm");
                free(pat);
                for (int i = 0; i < arm_count; i++) { free(patterns[i]); ast_free(bodies[i]); }
                free(patterns); free(pattern_is_wildcard); free(bodies);
                ast_free(scrutinee);
                return parser_recover(p);
            }
            expect_p(p, TOKEN_FAT_ARROW, "expected '=>' in match arm");
            /* Arm body: parse a single statement. We use parse_statement
             * so the user can write `Red => print("red");` or
             * `Red => { print("red"); print("!"); }`. */
            ASTNode* body = parse_statement(p);
            /* Grow arrays. Realloc one at a time and assign back
             * immediately to avoid -Wuse-after-free. */
            {
                char** p_r = realloc(patterns, sizeof(char*) * (size_t)(arm_count + 1));
                if (!p_r) {
                    parser_error(p, "out of memory while growing match arm list");
                    free(pat); ast_free(body);
                    for (int i = 0; i < arm_count; i++) { free(patterns[i]); ast_free(bodies[i]); }
                    free(patterns); free(pattern_is_wildcard); free(bodies);
                    ast_free(scrutinee);
                    return parser_recover(p);
                }
                patterns = p_r;
            }
            {
                int* w_r = realloc(pattern_is_wildcard, sizeof(int) * (size_t)(arm_count + 1));
                if (!w_r) {
                    parser_error(p, "out of memory while growing match arm list");
                    free(pat); ast_free(body);
                    for (int i = 0; i < arm_count; i++) { free(patterns[i]); ast_free(bodies[i]); }
                    free(patterns); free(pattern_is_wildcard); free(bodies);
                    ast_free(scrutinee);
                    return parser_recover(p);
                }
                pattern_is_wildcard = w_r;
            }
            {
                ASTNode** b_r = realloc(bodies, sizeof(ASTNode*) * (size_t)(arm_count + 1));
                if (!b_r) {
                    parser_error(p, "out of memory while growing match arm list");
                    free(pat); ast_free(body);
                    for (int i = 0; i < arm_count; i++) { free(patterns[i]); ast_free(bodies[i]); }
                    free(patterns); free(pattern_is_wildcard); free(bodies);
                    ast_free(scrutinee);
                    return parser_recover(p);
                }
                bodies = b_r;
            }
            patterns[arm_count] = pat;
            pattern_is_wildcard[arm_count] = is_wild;
            bodies[arm_count] = body;
            arm_count++;
            if (p->current.type == TOKEN_COMMA) advance_p(p);
        }
        expect_p(p, TOKEN_RBRACE, "missing '}' at end of match body");
        if (p->current.type == TOKEN_SEMICOLON) advance_p(p);
        ASTNode* node = (ASTNode*)ast_new_match_stmt(scrutinee, patterns, pattern_is_wildcard, bodies, arm_count, line, column);
        for (int i = 0; i < arm_count; i++) free(patterns[i]);
        free(patterns); free(pattern_is_wildcard); free(bodies);
        return node;
    }
    else if (p->current.type == TOKEN_IDENTIFIER) {
        char* name = strdup(p->current.value);
        int line = p->current.line;
        int column = p->current.column;
        advance_p(p);

        /* Sprint 4: `module.member(args);` — module member call statement.
         * We handle this BEFORE the regular call-statement branch because
         * the `.` would otherwise fall through to the "expected '=', '+='..."
         * error. The object is always an AST_IDENTIFIER naming the module
         * alias; the semantic pass validates that the alias was declared
         * by an earlier `import "..." as alias;`.
         *
         * Phase 2: we also accept `obj.field = value;` here (field
         * assignment). The disambiguation is: after consuming `.IDENTIFIER`,
         * if the next token is `(`, parse as method call; if it's `=`,
         * `+=`, or `-=`, parse as field assignment. */
        if (p->current.type == TOKEN_DOT) {
            int obj_line = p->current.line;
            int obj_column = p->current.column;
            advance_p(p);  /* consume '.' */
            if (p->current.type != TOKEN_IDENTIFIER) {
                parser_error(p, "expected member name after '.' in module call");
                free(name);
                return parser_recover(p);
            }
            char* member_name = strdup(p->current.value);
            eat_p(p, TOKEN_IDENTIFIER);

            /* Chained member statements — `self.items.push(x);`.
             * Previously only ONE `.member` segment was recognized here,
             * so method calls on a field (the bread and butter of generic
             * structs, RFC §13.1) failed to parse. Grammar for the chain:
             *   IDENT (.IDENT)* [( args )]      — final '(' = call.
             * Intermediate segments become AST_PROP_EXPR links; the last
             * segment becomes an AST_MEMBER_CALL whose object is the
             * accumulated chain (same shape as expression-position
             * postfix parsing produces). */
            if (p->current.type == TOKEN_DOT) {
                ASTNode* chain;
                {
                    ASTNode* obj = (ASTNode*)ast_new_identifier(name, obj_line, obj_column);
                    chain = (ASTNode*)ast_new_prop_expr(obj, member_name, line, column);
                }
                free(member_name);
                free(name);
                name = NULL; member_name = NULL;
                while (1) {
                    if (p->current.type != TOKEN_DOT) {
                        parser_error(p, "expected '.' in chained statement");
                        ast_free(chain);
                        return parser_recover(p);
                    }
                    advance_p(p);
                    if (p->current.type != TOKEN_IDENTIFIER) {
                        parser_error(p, "expected member name after '.' in chained statement");
                        ast_free(chain);
                        return parser_recover(p);
                    }
                    char* seg = strdup(p->current.value);
                    int seg_line = p->current.line;
                    int seg_column = p->current.column;
                    advance_p(p);
                    if (p->current.type == TOKEN_LPAREN) {
                        int arg_count = 0;
                        ASTNode** args = NULL;
                        if (!parse_paren_args(p, &args, &arg_count)) {
                            free(seg); ast_free(chain); return parser_recover(p);
                        }
                        optional_semicolon(p);
                        ASTNode* node = (ASTNode*)ast_new_member_call(chain, seg,
                                                                      args, arg_count,
                                                                      seg_line, seg_column);
                        free(seg);
                        return node;
                    }
                    chain = (ASTNode*)ast_new_prop_expr(chain, seg, seg_line, seg_column);
                    free(seg);
                }
            }

            if (p->current.type == TOKEN_LPAREN) {
                /* Method call: `obj.method(args);` */
                int arg_count = 0;
                ASTNode** args = NULL;
                if (!parse_paren_args(p, &args, &arg_count)) {
                    free(name); free(member_name); return parser_recover(p);
                }
                optional_semicolon(p);  /* Phase 2: `;` optional */
                ASTNode* obj = (ASTNode*)ast_new_identifier(name, obj_line, obj_column);
                ASTNode* node = (ASTNode*)ast_new_member_call(obj, member_name, args, arg_count, line, column);
                free(name);
                free(member_name);
                return node;
            } else if (p->current.type == TOKEN_EQUALS || p->current.type == TOKEN_PLUS_EQ ||
                       p->current.type == TOKEN_MINUS_EQ) {
                /* Phase 2: field assignment `obj.field = value;` */
                LamoTokenType op_type = p->current.type;
                advance_p(p);
                ASTNode* value = parse_expression(p);
                /* Semicolon is optional — the spec shows method bodies
                 * without semicolons. We accept both `;` and no-`;` (e.g.
                 * when the next token is `}`). */
                if (p->current.type == TOKEN_SEMICOLON) advance_p(p);
                ASTNode* obj = (ASTNode*)ast_new_identifier(name, obj_line, obj_column);
                ASTNode* target = (ASTNode*)ast_new_prop_expr(obj, member_name, line, column);
                ASTNode* node = (ASTNode*)ast_new_place_assign_stmt(target, value, op_type, line, column);
                free(name);
                free(member_name);
                return node;
            } else {
                parser_error(p, "expected '(' or '=' after '.member' in statement");
                free(name);
                free(member_name);
                return parser_recover(p);
            }
        }

        /* Phase 2: `arr[index] = value;` — index assignment. We detect
         * this when the identifier is followed by `[`. The expression
         * form `arr[index]` (read) is handled by parse_postfix; here we
         * specifically handle the assignment form. After consuming the
         * `[index]`, we look for `=`, `+=`, or `-=`. */
        if (p->current.type == TOKEN_LBRACKET) {
            int idx_line = p->current.line;
            int idx_column = p->current.column;
            advance_p(p);  /* consume '[' */
            ASTNode* index = parse_expression(p);
            expect_p(p, TOKEN_RBRACKET, "missing ']' after index in assignment");
            if (p->current.type == TOKEN_EQUALS || p->current.type == TOKEN_PLUS_EQ ||
                p->current.type == TOKEN_MINUS_EQ) {
                LamoTokenType op_type = p->current.type;
                advance_p(p);
                ASTNode* value = parse_expression(p);
                optional_semicolon(p);  /* Phase 2: `;` optional */
                ASTNode* obj = (ASTNode*)ast_new_identifier(name, idx_line, idx_column);
                ASTNode* target = (ASTNode*)ast_new_index_expr(obj, index, line, column);
                ASTNode* node = (ASTNode*)ast_new_place_assign_stmt(target, value, op_type, line, column);
                free(name);
                return node;
            } else {
                parser_error(p, "expected '=', '+=', or '-=' after 'arr[index]' in statement");
                ast_free(index);
                free(name);
                return parser_recover(p);
            }
        }

        if (p->current.type == TOKEN_LPAREN) {
            int arg_count = 0;
            ASTNode** args = NULL;
            if (!parse_paren_args(p, &args, &arg_count)) { free(name); return parser_recover(p); }
            optional_semicolon(p);  /* Phase 2: `;` optional */
            ASTNode* node = (ASTNode*)ast_new_call_stmt(name, args, arg_count, line, column);
            free(name);
            return node;
        }

        /* Generics PR 2: statement-position explicit type arguments —
         * `swap<int, int>(a, b);`. Same speculative-gate strategy as the
         * expression form above. */
        if (p->current.type == TOKEN_LT && parser_peek_is_generic_call(p)) {
            int ta_count = 0;
            char** tas = parse_angle_type_args_real(p, &ta_count);
            if (!tas) { free(name); return parser_recover(p); }
            int arg_count = 0;
            ASTNode** args = NULL;
            if (!parse_paren_args(p, &args, &arg_count)) {
                for (int i = 0; i < ta_count; i++) free(tas[i]);
                free(tas); free(name);
                return parser_recover(p);
            }
            optional_semicolon(p);
            ASTNode* node = (ASTNode*)ast_new_call_stmt_typed(name, tas, ta_count,
                                                              args, arg_count, line, column);
            for (int i = 0; i < ta_count; i++) free(tas[i]);
            free(tas);
            free(name);
            return node;
        }
        else if (p->current.type == TOKEN_EQUALS || p->current.type == TOKEN_PLUS_EQ ||
                 p->current.type == TOKEN_MINUS_EQ) {
            LamoTokenType op_type = p->current.type;
            advance_p(p);
            ASTNode* value = parse_expression(p);
            optional_semicolon(p);  /* Phase 2: `;` optional */
            ASTNode* node = (ASTNode*)ast_new_assign_stmt(name, value, op_type, line, column);
            free(name);
            return node;
        } else if (p->current.type == TOKEN_PLUS_PLUS) {
            advance_p(p);
            optional_semicolon(p);  /* Phase 2: `;` optional */
            ASTNode* one = (ASTNode*)ast_new_int_literal(1, line, column);
            ASTNode* ident = (ASTNode*)ast_new_identifier(name, line, column);
            ASTNode* expr = (ASTNode*)ast_new_binary_expr(ident, TOKEN_PLUS, one, line, column);
            ASTNode* node = (ASTNode*)ast_new_assign_stmt(name, expr, TOKEN_EQUALS, line, column);
            free(name);
            return node;
        }
        else if (p->current.type == TOKEN_MINUS_MINUS) {
            advance_p(p);
            optional_semicolon(p);  /* Phase 2: `;` optional */
            ASTNode* one = (ASTNode*)ast_new_int_literal(1, line, column);
            ASTNode* ident = (ASTNode*)ast_new_identifier(name, line, column);
            ASTNode* expr = (ASTNode*)ast_new_binary_expr(ident, TOKEN_MINUS, one, line, column);
            ASTNode* node = (ASTNode*)ast_new_assign_stmt(name, expr, TOKEN_EQUALS, line, column);
            free(name);
            return node;
        }
        else {
            parser_error(p, "expected '=', '+=', '-=', '++', '--', or '(' after identifier");
            free(name);
            return parser_recover(p);
        }
    }
    else if (p->current.type == TOKEN_IF) {
        eat_p(p, TOKEN_IF);
        int line = p->current.line;
        int column = p->current.column;
        expect_p(p, TOKEN_LPAREN, "expected '(' after 'if'");
        ASTNode* condition = parse_expression(p);
        expect_p(p, TOKEN_RPAREN, "missing ')' after if condition");
        ASTNode* then_branch = parse_block(p);

        ASTNode* else_branch = NULL;
        if (p->current.type == TOKEN_ELSE) {
            eat_p(p, TOKEN_ELSE);
            if (p->current.type == TOKEN_IF) {
                else_branch = parse_statement(p);
            } else {
                else_branch = parse_block(p);
            }
        }
        return (ASTNode*)ast_new_if_stmt(condition, then_branch, else_branch, line, column);
    }
    else if (p->current.type == TOKEN_WHILE) {
        eat_p(p, TOKEN_WHILE);
        int line = p->current.line;
        int column = p->current.column;
        expect_p(p, TOKEN_LPAREN, "expected '(' after 'while'");
        ASTNode* condition = parse_expression(p);
        expect_p(p, TOKEN_RPAREN, "missing ')' after while condition");
        ASTNode* body = parse_block(p);
        return (ASTNode*)ast_new_while_stmt(condition, body, line, column);
    }
    else if (p->current.type == TOKEN_FOR) {
        eat_p(p, TOKEN_FOR);
        int line = p->current.line;
        int column = p->current.column;
        expect_p(p, TOKEN_LPAREN, "expected '(' after 'for'");

        ASTNode* initializer = NULL;
        if (p->current.type == TOKEN_LET) {
            eat_p(p, TOKEN_LET);
            if (p->current.type != TOKEN_IDENTIFIER) {
                parser_error(p, "expected identifier after let in for");
                return parser_recover(p);
            }
            char* v_name = strdup(p->current.value);
            int v_line = p->current.line;
            int v_column = p->current.column;
            eat_p(p, TOKEN_IDENTIFIER);
            /* Sprint 3: optional `: type` annotation in for-let. */
            char* v_type_annotation = NULL;
            if (p->current.type == TOKEN_COLON) {
                eat_p(p, TOKEN_COLON);
                if (p->current.type != TOKEN_IDENTIFIER) {
                    parser_error(p, "expected type name after ':' in for-let declaration");
                    free(v_name);
                    return parser_recover(p);
                }
                v_type_annotation = strdup(p->current.value);
                eat_p(p, TOKEN_IDENTIFIER);
            }
            eat_p(p, TOKEN_EQUALS);
            /* Sprint 1 fix: same "missing initializer" check as the standalone
             * `let` case — applies to `for (let i = ; ...)` as well.
             * Sprint 4: same hint as the let case. */
            if (p->current.type == TOKEN_SEMICOLON) {
                parser_error_with_hint(p,
                    "missing initializer after '=' in for-let declaration",
                    "did you forget a value after '='? e.g. `for (let i = 0; i < n; i++) { ... }`");
                free(v_name);
                free(v_type_annotation);
                return parser_recover(p);
            }
            ASTNode* init_expr = parse_expression(p);
            initializer = (ASTNode*)ast_new_var_decl_typed(v_name, init_expr, v_type_annotation, v_line, v_column);
            free(v_name);
            free(v_type_annotation);
        } else if (p->current.type == TOKEN_IDENTIFIER) {
            char* v_name = strdup(p->current.value);
            int assign_line = p->current.line;
            int assign_column = p->current.column;
            advance_p(p);
            if (p->current.type == TOKEN_EQUALS || p->current.type == TOKEN_PLUS_EQ || p->current.type == TOKEN_MINUS_EQ) {
                LamoTokenType op_type = p->current.type;
                advance_p(p);
                ASTNode* value = parse_expression(p);
                initializer = (ASTNode*)ast_new_assign_stmt(v_name, value, op_type, assign_line, assign_column);
            } else if (p->current.type == TOKEN_PLUS_PLUS) {
                advance_p(p);
                ASTNode* one = (ASTNode*)ast_new_int_literal(1, assign_line, assign_column);
                ASTNode* ident = (ASTNode*)ast_new_identifier(v_name, assign_line, assign_column);
                ASTNode* expr = (ASTNode*)ast_new_binary_expr(ident, TOKEN_PLUS, one, assign_line, assign_column);
                initializer = (ASTNode*)ast_new_assign_stmt(v_name, expr, TOKEN_EQUALS, assign_line, assign_column);
            } else if (p->current.type == TOKEN_MINUS_MINUS) {
                advance_p(p);
                ASTNode* one = (ASTNode*)ast_new_int_literal(1, assign_line, assign_column);
                ASTNode* ident = (ASTNode*)ast_new_identifier(v_name, assign_line, assign_column);
                ASTNode* expr = (ASTNode*)ast_new_binary_expr(ident, TOKEN_MINUS, one, assign_line, assign_column);
                initializer = (ASTNode*)ast_new_assign_stmt(v_name, expr, TOKEN_EQUALS, assign_line, assign_column);
            }
            free(v_name);
        }
        expect_p(p, TOKEN_SEMICOLON, "missing ';' in for loop — expected 'for (init; condition; increment)'");

        ASTNode* condition = parse_expression(p);
        expect_p(p, TOKEN_SEMICOLON, "missing ';' in for loop — expected 'for (init; condition; increment)'");

        ASTNode* increment = NULL;
        if (p->current.type == TOKEN_IDENTIFIER) {
            char* v_name = strdup(p->current.value);
            int inc_line = p->current.line;
            int inc_column = p->current.column;
            advance_p(p);
            if (p->current.type == TOKEN_PLUS_PLUS) {
                advance_p(p);
                ASTNode* one = (ASTNode*)ast_new_int_literal(1, inc_line, inc_column);
                ASTNode* ident = (ASTNode*)ast_new_identifier(v_name, inc_line, inc_column);
                ASTNode* expr = (ASTNode*)ast_new_binary_expr(ident, TOKEN_PLUS, one, inc_line, inc_column);
                increment = (ASTNode*)ast_new_assign_stmt(v_name, expr, TOKEN_EQUALS, inc_line, inc_column);
            } else if (p->current.type == TOKEN_MINUS_MINUS) {
                advance_p(p);
                ASTNode* one = (ASTNode*)ast_new_int_literal(1, inc_line, inc_column);
                ASTNode* ident = (ASTNode*)ast_new_identifier(v_name, inc_line, inc_column);
                ASTNode* expr = (ASTNode*)ast_new_binary_expr(ident, TOKEN_MINUS, one, inc_line, inc_column);
                increment = (ASTNode*)ast_new_assign_stmt(v_name, expr, TOKEN_EQUALS, inc_line, inc_column);
            } else if (p->current.type == TOKEN_PLUS_EQ || p->current.type == TOKEN_MINUS_EQ || p->current.type == TOKEN_EQUALS) {
                LamoTokenType op_type = p->current.type;
                advance_p(p);
                ASTNode* value = parse_expression(p);
                increment = (ASTNode*)ast_new_assign_stmt(v_name, value, op_type, inc_line, inc_column);
            }
            free(v_name);
        }

        expect_p(p, TOKEN_RPAREN, "missing ')' to close for loop header");
        ASTNode* body = parse_block(p);
        return (ASTNode*)ast_new_for_stmt(initializer, condition, increment, body, line, column);
    }
    else if (p->current.type == TOKEN_RETURN) {
        eat_p(p, TOKEN_RETURN);
        int line = p->current.line;
        int column = p->current.column;
        ASTNode* expression = NULL;
        if (p->current.type != TOKEN_SEMICOLON) {
            expression = parse_expression(p);
        }
        optional_semicolon(p);  /* Phase 2: `;` optional */
        return (ASTNode*)ast_new_return_stmt(expression, line, column);
    }
    else if (p->current.type == TOKEN_LBRACE) {
        /* Phase 2: a bare block `{ ... }` as a statement. This is used
         * by match arm bodies like `Red => { print("red"); print("!"); }`.
         * We delegate to parse_block which handles the braces and the
         * inner statement list. */
        return parse_block(p);
    }
    else if (p->current.type == TOKEN_BREAK) {
        int line = p->current.line;
        int column = p->current.column;
        eat_p(p, TOKEN_BREAK);
        optional_semicolon(p);  /* Phase 2: `;` optional */
        return ast_new_break_stmt(line, column);
    }
    else if (p->current.type == TOKEN_CONTINUE) {
        int line = p->current.line;
        int column = p->current.column;
        eat_p(p, TOKEN_CONTINUE);
        optional_semicolon(p);  /* Phase 2: `;` optional */
        return ast_new_continue_stmt(line, column);
    }
    else if (p->current.type == TOKEN_UNKNOWN) {
        char buf[256];
        snprintf(buf, sizeof(buf), "unexpected token '%s'",
                 p->current.value ? p->current.value : "?");
        parser_error(p, buf);
        return parser_recover(p);
    }
    else if (p->current.type != TOKEN_EOF) {
        char buf[256];
        snprintf(buf, sizeof(buf), "unexpected token %s in statement",
                 token_type_name(p->current.type));
        parser_error(p, buf);
        return parser_recover(p);
    }
    return NULL;
}

ASTProgram* parse_program_v2(Parser* p) {
    ASTProgram* program = ast_new_program();
    ASTNode* head = NULL;
    ASTNode* current = NULL;

    while (p->current.type != TOKEN_EOF) {
        ASTNode* stmt = parse_statement(p);
        if (stmt) {
            if (!head) {
                head = stmt;
                current = stmt;
            } else {
                current->next = stmt;
                current = stmt;
            }
        }
        if (p->panic_mode) {
            parser_synchronize(p);
        }
    }
    program->declarations = head;
    return program;
}
