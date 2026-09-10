/*
 * formatter.c — AST-based pretty-printer backing `lamo fmt` (2.11.0).
 *
 * Design (see formatter.h for the contract):
 *
 *   1. Parse the file with the real parser (parse_program_v2). Any
 *      syntax error => *ok = 0, caller falls back to whitespace-only
 *      normalization. This keeps `fmt` safe on files the compiler
 *      itself cannot read.
 *
 *   2. Pre-scan the raw source for comments (string-aware — line
 *      comments and block comment spans inside string literals are
 *      ignored). The AST has
 *      no comment nodes, so comments are re-attached by LINE POSITION
 *      during emission: an unprinted comment whose start line is below
 *      the next node's anchor line is emitted as an own-line comment
 *      right above it; a comment starting on a node's anchor line is
 *      kept as a trailing comment on that physical line. Comment text
 *      is never altered or dropped — only position can shift (when a
 *      comment sat inside a multi-line expression).
 *
 *   3. Pre-scan the token stream with the real lexer to record the
 *      source line of every `{`/`}` pair, in open order. The printer
 *      emits braces in exactly the same order, so each emitted brace
 *      consumes the next pair; the matched close line drives the
 *      "comments before `}`" flush.
 *
 *   4. Emit from the AST: 4-space indents, one statement per line,
 *      explicit semicolons, spaces around binary operators, minimal
 *      parenthesization re-derived from the parser's precedence table
 *      (PREC_OR..PREC_POSTFIX mirror parse_expression/parse_and/
 *      parse_equality/parse_comparison/parse_term/parse_factor/
 *      parse_unary/parse_postfix), shortest-round-trip float
 *      printing, and lexer-mirror string re-escaping.
 */

#include "formatter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../lexer/lexer.h"
#include "../parser/parser.h"

/* ────────────────────────────────────────────────────────────────────
 * Grow-only output buffer
 * ──────────────────────────────────────────────────────────────────── */

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf* b) {
    b->cap = 4096;
    b->len = 0;
    b->data = malloc(b->cap);
    if (!b->data) {
        perror("fmt: out of memory");
        exit(EXIT_FAILURE);
    }
    b->data[0] = '\0';
}

static void buf_reserve(Buf* b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    while (b->cap < need) b->cap *= 2;
    char* grown = realloc(b->data, b->cap);
    if (!grown) {
        perror("fmt: out of memory");
        exit(EXIT_FAILURE);
    }
    b->data = grown;
}

static void buf_append(Buf* b, const char* s) {
    if (!s) return;
    size_t n = strlen(s);
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n + 1);
    b->len += n;
}

static void buf_append_len(Buf* b, const char* s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_char(Buf* b, char c) {
    buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void buf_indent(Buf* b, int level) {
    for (int i = 0; i < level * 4; i++) buf_char(b, ' ');
}

/* ────────────────────────────────────────────────────────────────────
 * Comment capture (string-aware raw scan)
 * ──────────────────────────────────────────────────────────────────── */

typedef struct {
    int start_line;
    int start_col;
    int end_line;      /* line where the comment ends */
    int is_block;      /* 1 = slash-star, 0 = line comment */
    char* text;        /* full text incl. delimiters; may embed \n */
    int used;          /* printed flag */
} Comment;

typedef struct {
    Comment* items;
    int count;
    int cap;
} CommentList;

static void comments_push(CommentList* list, Comment c) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 16;
        Comment* grown = realloc(list->items, sizeof(Comment) * (size_t)list->cap);
        if (!grown) {
            perror("fmt: out of memory");
            exit(EXIT_FAILURE);
        }
        list->items = grown;
    }
    list->items[list->count++] = c;
}

/* Walk the source capturing every comment span, skipping string
 * literals so `"http://x"` never yields a phantom comment. Mirrors the
 * lexer's string rules (escapes, no real newlines inside strings). */
static void scan_comments(const char* src, CommentList* out) {
    size_t i = 0;
    int line = 1;
    int col = 1;
    while (src[i] != '\0') {
        char c = src[i];
        if (c == '"') {
            /* Skip a string literal. */
            i++;
            col++;
            while (src[i] != '\0' && src[i] != '"' && src[i] != '\n') {
                if (src[i] == '\\' && src[i + 1] != '\0') {
                    i++;
                    col++;
                }
                i++;
                col++;
            }
            if (src[i] == '"') {
                i++;
                col++;
            }
            continue;
        }
        if (c == '/' && src[i + 1] == '/') {
            Comment cm;
            size_t start = i;
            cm.start_line = line;
            cm.start_col = col;
            cm.is_block = 0;
            while (src[i] != '\0' && src[i] != '\n' && src[i] != '\r') i++;
            cm.end_line = line;
            cm.text = malloc((size_t)(i - start) + 1);
            if (!cm.text) {
                perror("fmt: out of memory");
                exit(EXIT_FAILURE);
            }
            memcpy(cm.text, src + start, (size_t)(i - start));
            cm.text[i - start] = '\0';
            cm.used = 0;
            comments_push(out, cm);
            col += (int)(i - start);
            continue;
        }
        if (c == '/' && src[i + 1] == '*') {
            Comment cm;
            size_t start = i;
            cm.start_line = line;
            cm.start_col = col;
            cm.is_block = 1;
            i += 2;
            col += 2;
            while (src[i] != '\0' && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') {
                    line++;
                    col = 1;
                } else {
                    col++;
                }
                i++;
            }
            if (src[i] != '\0') {
                i += 2;  /* consume '*' and '/' */
                col += 2;
            }
            cm.end_line = line;
            cm.text = malloc((size_t)(i - start) + 1);
            if (!cm.text) {
                perror("fmt: out of memory");
                exit(EXIT_FAILURE);
            }
            memcpy(cm.text, src + start, (size_t)(i - start));
            cm.text[i - start] = '\0';
            cm.used = 0;
            comments_push(out, cm);
            continue;
        }
        if (c == '\n') {
            line++;
            col = 1;
            i++;
            continue;
        }
        i++;
        col++;
    }
}

static void comments_free(CommentList* list) {
    for (int i = 0; i < list->count; i++) free(list->items[i].text);
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

/* ────────────────────────────────────────────────────────────────────
 * Brace map — source line of every { } pair, in open order.
 * The printer emits braces in the same order, so close-brace emission
 * consumes the next pair to learn the original close line (used for
 * the comments-before-'}' flush).
 * ──────────────────────────────────────────────────────────────────── */

typedef struct {
    int open_line;
    int close_line;
} BracePair;

typedef struct {
    BracePair* pairs;
    int count;
    int cap;
} BraceMap;

/* qsort comparator: order pairs by OPEN line. The printer consumes one
 * pair per emitted '{' in emission order, which mirrors the source's
 * open order (nested pairs must therefore be ordered by their open
 * position, not by when their '}' appeared). */
static int brace_pair_cmp(const void* a, const void* b) {
    const BracePair* pa = (const BracePair*)a;
    const BracePair* pb = (const BracePair*)b;
    return pa->open_line - pb->open_line;
}

static void brace_map_scan(const char* source, BraceMap* bm) {
    bm->pairs = NULL;
    bm->count = 0;
    bm->cap = 0;

    char* copy = strdup(source ? source : "");
    if (!copy) return;
    Lexer* lex = lexer_init(copy);
    if (!lex) {
        free(copy);
        return;
    }

    int* open_stack = NULL;
    int stack_cap = 0;
    int stack_len = 0;

    while (1) {
        Token t = lexer_next_token(lex);
        if (t.type == TOKEN_LBRACE) {
            if (stack_len == stack_cap) {
                stack_cap = stack_cap ? stack_cap * 2 : 32;
                int* grown = realloc(open_stack, sizeof(int) * (size_t)stack_cap);
                if (!grown) break;
                open_stack = grown;
            }
            open_stack[stack_len++] = t.line;
        } else if (t.type == TOKEN_RBRACE) {
            if (stack_len > 0) {
                int open_line = open_stack[--stack_len];
                if (bm->count == bm->cap) {
                    bm->cap = bm->cap ? bm->cap * 2 : 32;
                    BracePair* grown = realloc(bm->pairs, sizeof(BracePair) * (size_t)bm->cap);
                    if (!grown) break;
                    bm->pairs = grown;
                }
                bm->pairs[bm->count].open_line = open_line;
                bm->pairs[bm->count].close_line = t.line;
                bm->count++;
            }
        } else if (t.type == TOKEN_EOF) {
            token_free(t);
            break;
        }
        token_free(t);
    }
    free(open_stack);
    lexer_free(lex);
    /* NB: lexer_init does not take ownership of `copy`; the Lexer only
     * stores the pointer. Freeing here is safe after lexer_free. */
    free(copy);

    /* Open-order normalization (see comparator comment). */
    if (bm->count > 1) {
        qsort(bm->pairs, (size_t)bm->count, sizeof(BracePair), brace_pair_cmp);
    }
}

static void brace_map_free(BraceMap* bm) {
    free(bm->pairs);
    bm->pairs = NULL;
    bm->count = bm->cap = 0;
}

/* ────────────────────────────────────────────────────────────────────
 * Printer context
 * ──────────────────────────────────────────────────────────────────── */

typedef struct {
    Buf out;
    CommentList comments;
    int next_comment;
    BraceMap braces;
    int next_brace;
    int indent;          /* current indentation level for nested match */
} Fmt;

/* ── lexer-mirror literals ────────────────────────────────────────── */

/* Re-escape a decoded string value exactly per decode_string_escapes:
 * \n \t \r \\ \" \' \0 \xHH, unknown escapes kept as backslash+char.
 * We escape backslash, quote, control bytes (< 0x20, 0x7F). Bytes
 * >= 0x80 (UTF-8) pass through verbatim. */
static void emit_escaped_string(Fmt* f, const char* value) {
    buf_char(&f->out, '"');
    for (const char* p = value; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '\n': buf_append(&f->out, "\\n"); break;
            case '\t': buf_append(&f->out, "\\t"); break;
            case '\r': buf_append(&f->out, "\\r"); break;
            case '\\': buf_append(&f->out, "\\\\"); break;
            case '"':  buf_append(&f->out, "\\\""); break;
            case '\0': buf_append(&f->out, "\\0"); break;  /* defensive */
            default:
                if (c < 0x20 || c == 0x7F) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\x%02X", (unsigned)c);
                    buf_append(&f->out, tmp);
                } else {
                    buf_char(&f->out, (char)c);
                }
                break;
        }
    }
    buf_char(&f->out, '"');
}

/* Shortest decimal representation that strtod-matches the original
 * double. Guarantees `fmt` output re-parses to the same value; the
 * ".0" suffix keeps float-ness visible (%g alone would print 2.0 as
 * "2", which lexes as an INT and changes inference). */
static void emit_float(Fmt* f, double value) {
    char tmp[64];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(tmp, sizeof(tmp), "%.*g", prec, value);
        if (strtod(tmp, NULL) == value) break;
    }
    if (!strchr(tmp, '.') && !strchr(tmp, 'e') && !strchr(tmp, 'E') &&
        !strchr(tmp, 'n') /* nan/inf defensive */) {
        size_t n = strlen(tmp);
        tmp[n] = '.';
        tmp[n + 1] = '0';
        tmp[n + 2] = '\0';
    }
    buf_append(&f->out, tmp);
}

/* ── comment flushing ─────────────────────────────────────────────── */

/* Emit one comment as its own line(s) at `indent`. */
static void emit_comment_line(Fmt* f, const Comment* cm, int indent) {
    if (!cm->is_block || !strchr(cm->text, '\n')) {
        buf_indent(&f->out, indent);
        buf_append(&f->out, cm->text);
        buf_char(&f->out, '\n');
        return;
    }
    /* Multi-line block comment: first line at current indent, interior
     * lines verbatim (they carry their original inner alignment). */
    const char* p = cm->text;
    while (p) {
        const char* nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        buf_indent(&f->out, indent);
        buf_append_len(&f->out, p, n);
        buf_char(&f->out, '\n');
        p = nl ? nl + 1 : NULL;
    }
}

/* Emit every unprinted comment that starts strictly before `anchor`
 * line, at `indent`. Called before each node anchored at `anchor`. */
static void flush_leading(Fmt* f, int anchor, int indent) {
    while (f->next_comment < f->comments.count &&
           f->comments.items[f->next_comment].start_line < anchor) {
        emit_comment_line(f, &f->comments.items[f->next_comment], indent);
        f->comments.items[f->next_comment].used = 1;
        f->next_comment++;
    }
}

/* Append trailing comments that START on `anchor` line to the current
 * (still open) output line, then close the line with '\n'. Multi-line
 * block comments never trail (they flush as leading comments later).
 * anchor <= 0 just closes the line. */
static void end_line(Fmt* f, int anchor) {
    if (anchor > 0) {
        while (f->next_comment < f->comments.count &&
               f->comments.items[f->next_comment].start_line == anchor &&
               !f->comments.items[f->next_comment].used) {
            Comment* cm = &f->comments.items[f->next_comment];
            if (cm->is_block && strchr(cm->text, '\n')) break;
            buf_append(&f->out, "  ");
            buf_append(&f->out, cm->text);
            cm->used = 1;
            f->next_comment++;
        }
    }
    buf_char(&f->out, '\n');
}

/* EOF / program-end flush: anything left unprinted goes out at top
 * level so no comment is ever lost. */
static void flush_remaining(Fmt* f) {
    while (f->next_comment < f->comments.count) {
        if (!f->comments.items[f->next_comment].used) {
            emit_comment_line(f, &f->comments.items[f->next_comment], f->indent);
        }
        f->comments.items[f->next_comment].used = 1;
        f->next_comment++;
    }
}

/* ── brace helpers ────────────────────────────────────────────────── */

/* Append '{' and consume the next brace pair (source order). */
static int fmt_open_brace(Fmt* f) {
    int idx = f->next_brace;
    if (idx < f->braces.count) f->next_brace++;
    buf_char(&f->out, '{');
    return idx;
}

/* Flush comments that live between the last emitted node and the
 * original '}' line, then append '}'. In own-line mode the brace is
 * prefixed with `brace_indent` indentation; in inline mode (empty
 * bodies, struct literals, `{ }`) it follows the current text
 * directly. */
static void fmt_close_brace_ex(Fmt* f, int idx, int brace_indent, int inline_mode) {
    if (idx >= 0 && idx < f->braces.count) {
        int close_line = f->braces.pairs[idx].close_line;
        while (f->next_comment < f->comments.count &&
               f->comments.items[f->next_comment].start_line < close_line &&
               !f->comments.items[f->next_comment].used) {
            /* Own-line closes: comments flush at the INNER level (one
             * deeper than the brace itself), matching where they were
             * written. Inline closes take the caller's level. */
            int comment_indent = inline_mode ? brace_indent : brace_indent + 1;
            emit_comment_line(f, &f->comments.items[f->next_comment], comment_indent);
            f->comments.items[f->next_comment].used = 1;
            f->next_comment++;
        }
    }
    if (!inline_mode) buf_indent(&f->out, brace_indent);
    buf_char(&f->out, '}');
}

static void fmt_close_brace(Fmt* f, int idx, int brace_indent) {
    fmt_close_brace_ex(f, idx, brace_indent, 0);
}

static void fmt_close_brace_inline(Fmt* f, int idx, int comment_indent) {
    fmt_close_brace_ex(f, idx, comment_indent, 1);
}

/* ────────────────────────────────────────────────────────────────────
 * Expressions — precedence-aware emission with minimal parentheses
 * ──────────────────────────────────────────────────────────────────── */

/* Levels mirror the parser's recursive-descent chain exactly:
 * parse_expression(||) → parse_and(&&) → parse_equality(== !=) →
 * parse_comparison(< > <= >=) → parse_term(+ -) → parse_factor(* / %)
 * → parse_unary(! -) → parse_postfix(call/index/prop) → parse_primary.
 * All binary operators are left-associative. */
enum {
    PREC_NONE = 0,
    PREC_OR = 1,
    PREC_AND = 2,
    PREC_EQ = 3,
    PREC_CMP = 4,
    PREC_ADD = 5,
    PREC_MUL = 6,
    PREC_UNARY = 7,
    PREC_POSTFIX = 8,
    PREC_PRIMARY = 9
};

static int binary_prec(LamoTokenType op) {
    switch (op) {
        case TOKEN_OR_OR:    return PREC_OR;
        case TOKEN_AND_AND:  return PREC_AND;
        case TOKEN_EQ_EQ: case TOKEN_BANG_EQ: return PREC_EQ;
        case TOKEN_LT: case TOKEN_GT: case TOKEN_LT_EQ: case TOKEN_GT_EQ:
            return PREC_CMP;
        case TOKEN_PLUS: case TOKEN_MINUS: return PREC_ADD;
        case TOKEN_STAR: case TOKEN_SLASH: case TOKEN_PERCENT: return PREC_MUL;
        default: return PREC_PRIMARY;
    }
}

static const char* binary_spelling(LamoTokenType op) {
    switch (op) {
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_STAR: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_PERCENT: return "%";
        case TOKEN_EQ_EQ: return "==";
        case TOKEN_BANG_EQ: return "!=";
        case TOKEN_LT: return "<";
        case TOKEN_GT: return ">";
        case TOKEN_LT_EQ: return "<=";
        case TOKEN_GT_EQ: return ">=";
        case TOKEN_AND_AND: return "&&";
        case TOKEN_OR_OR: return "||";
        default: return "?";
    }
}

static const char* assign_spelling(LamoTokenType op) {
    switch (op) {
        case TOKEN_EQUALS: return "=";
        case TOKEN_PLUS_EQ: return "+=";
        case TOKEN_MINUS_EQ: return "-=";
        default: return "=";
    }
}

static int expr_prec(ASTNode* n) {
    if (!n) return PREC_PRIMARY;
    switch (n->type) {
        case AST_BINARY_EXPR:
            return binary_prec(((ASTBinaryExpr*)n)->operator);
        case AST_UNARY_EXPR:
            return PREC_UNARY;
        case AST_CALL_EXPR: case AST_MEMBER_CALL:
        case AST_INDEX_EXPR: case AST_PROP_EXPR:
            return PREC_POSTFIX;
        case AST_GROUPING_EXPR:
            return expr_prec(((ASTGroupingExpr*)n)->expression);
        default:
            return PREC_PRIMARY;
    }
}

static void emit_type_args(Fmt* f, char** type_args, int count) {
    if (count <= 0 || !type_args) return;
    buf_char(&f->out, '<');
    for (int i = 0; i < count; i++) {
        if (i) buf_append(&f->out, ", ");
        buf_append(&f->out, type_args[i]);
    }
    buf_char(&f->out, '>');
}

/* Forward declarations (expressions, statements and patterns are
 * mutually recursive through match). */
static void emit_expr(Fmt* f, ASTNode* node, int parent_prec, int is_right);
static void emit_stmt_core(Fmt* f, ASTNode* node, int indent, int no_semi);
static void emit_stmt(Fmt* f, ASTNode* node, int indent, int no_semi);
static void emit_pattern(Fmt* f, LamoPattern* pat);
static void emit_block_body(Fmt* f, ASTBlock* block, int indent, int header_anchor);

static void emit_expr_args(Fmt* f, ASTNode** args, int count) {
    buf_char(&f->out, '(');
    for (int i = 0; i < count; i++) {
        if (i) buf_append(&f->out, ", ");
        emit_expr(f, args[i], PREC_NONE, 0);
    }
    buf_char(&f->out, ')');
}

/* Emit `Name<...> { field: value, ... }` inline (struct literal). */
static void emit_struct_literal(Fmt* f, ASTStructLiteral* s) {
    buf_append(&f->out, s->struct_name);
    emit_type_args(f, s->type_args, s->type_arg_count);
    buf_char(&f->out, ' ');
    int idx = fmt_open_brace(f);
    if (s->field_count == 0) {
        buf_char(&f->out, ' ');
        fmt_close_brace_inline(f, idx, f->indent);
        return;
    }
    for (int i = 0; i < s->field_count; i++) {
        if (i) buf_append(&f->out, ", ");
        buf_append(&f->out, s->field_names[i]);
        buf_append(&f->out, ": ");
        emit_expr(f, s->field_values[i], PREC_NONE, 0);
    }
    fmt_close_brace_inline(f, idx, f->indent);
}

/* match used as an expression or statement: identical emission, arms
 * indented one level under the current statement indent. */
static void emit_match(Fmt* f, ASTMatchStmt* m) {
    buf_append(&f->out, "match ");
    emit_expr(f, m->scrutinee, PREC_NONE, 0);
    buf_char(&f->out, ' ');
    int idx = fmt_open_brace(f);
    if (m->arm_count == 0) {
        buf_char(&f->out, ' ');
        fmt_close_brace_inline(f, idx, f->indent);
        return;
    }
    end_line(f, m->base.line);
    int saved_indent = f->indent;
    for (int i = 0; i < m->arm_count; i++) {
        LamoPattern* pat = m->patterns[i];
        if (pat) flush_leading(f, pat->line, saved_indent + 1);
        buf_indent(&f->out, saved_indent + 1);
        emit_pattern(f, pat);
        if (m->guards[i]) {
            buf_append(&f->out, " when ");
            emit_expr(f, m->guards[i], PREC_NONE, 0);
        }
        buf_append(&f->out, " => ");
        ASTNode* body = m->bodies[i];
        /* Arm-body statements live one level deeper: nested match
         * expressions picked up through emit_expr must base there. */
        f->indent = saved_indent + 1;
        if (body && body->type == AST_BLOCK) {
            buf_char(&f->out, ' ');
            int bidx = fmt_open_brace(f);
            ASTBlock* blk = (ASTBlock*)body;
            if (!blk->statements) {
                buf_char(&f->out, ' ');
                fmt_close_brace_inline(f, bidx, saved_indent + 1);
                buf_char(&f->out, ',');
                end_line(f, 0);
            } else {
                end_line(f, pat ? pat->line : 0);
                for (ASTNode* s = blk->statements; s; s = s->next) {
                    emit_stmt(f, s, saved_indent + 2, 0);
                }
                fmt_close_brace(f, bidx, saved_indent + 1);
                buf_char(&f->out, ',');
                end_line(f, 0);
            }
        } else if (body) {
            int body_is_simple = (body->type != AST_MATCH_STMT);
            emit_stmt_core(f, body, saved_indent + 1, 1);
            if (body_is_simple) {
                buf_char(&f->out, ',');
                end_line(f, pat ? pat->line : 0);
            } else {
                /* match as arm body already closed its own line */
                buf_char(&f->out, ',');
                end_line(f, 0);
            }
        } else {
            buf_char(&f->out, ',');
            end_line(f, 0);
        }
        f->indent = saved_indent;
    }
    /* Closing brace on its own line (fmt_close_brace emits the indent);
     * final line left unterminated — the caller (statement wrapper,
     * enclosing expression, or match-arm loop) appends ';' / ',' /
     * the newline. */
    fmt_close_brace(f, idx, saved_indent);
}

static void emit_pattern(Fmt* f, LamoPattern* pat) {
    if (!pat) {
        buf_char(&f->out, '_');
        return;
    }
    switch (pat->kind) {
        case LAMO_PATTERN_WILDCARD:
            buf_char(&f->out, '_');
            break;
        case LAMO_PATTERN_BINDING:
            buf_append(&f->out, pat->name ? pat->name : "_");
            break;
        case LAMO_PATTERN_LITERAL:
            if (pat->literal) emit_expr(f, pat->literal, PREC_NONE, 0);
            else buf_char(&f->out, '_');
            break;
        case LAMO_PATTERN_CTOR:
        default:
            buf_append(&f->out, pat->name ? pat->name : "_");
            if (pat->children && pat->child_count > 0) {
                buf_char(&f->out, '(');
                for (int i = 0; i < pat->child_count; i++) {
                    if (i) buf_append(&f->out, ", ");
                    emit_pattern(f, pat->children[i]);
                }
                buf_char(&f->out, ')');
            }
            break;
    }
}

static void emit_expr(Fmt* f, ASTNode* node, int parent_prec, int is_right) {
    if (!node) return;

    int my = expr_prec(node);
    int paren = (my < parent_prec) || (is_right && my == parent_prec);

    switch (node->type) {
        case AST_GROUPING_EXPR: {
            /* Transparent: precedence alone decides whether the parens
             * survive. `a + (b + c)` keeps them (right child, equal
             * prec); `(a) + b` drops them. */
            emit_expr(f, ((ASTGroupingExpr*)node)->expression,
                      parent_prec, is_right);
            return;
        }
        case AST_INT_LITERAL:
            { char tmp[32]; snprintf(tmp, sizeof(tmp), "%lld",
                ((ASTIntLiteral*)node)->value); buf_append(&f->out, tmp); }
            return;
        case AST_FLOAT_LITERAL:
            emit_float(f, ((ASTFloatLiteral*)node)->value);
            return;
        case AST_STRING_LITERAL:
            emit_escaped_string(f, ((ASTStringLiteral*)node)->value);
            return;
        case AST_BOOL_LITERAL:
            buf_append(&f->out, ((ASTBoolLiteral*)node)->value ? "true" : "false");
            return;
        case AST_IDENTIFIER:
            buf_append(&f->out, ((ASTIdentifier*)node)->name);
            return;
        case AST_VARIANT_REF: {
            ASTVariantRef* v = (ASTVariantRef*)node;
            buf_append(&f->out, v->enum_name);
            buf_append(&f->out, "::");
            buf_append(&f->out, v->variant_name);
            return;
        }
        case AST_ARRAY_LITERAL: {
            ASTArrayLiteral* a = (ASTArrayLiteral*)node;
            buf_char(&f->out, '[');
            for (int i = 0; i < a->element_count; i++) {
                if (i) buf_append(&f->out, ", ");
                emit_expr(f, a->elements[i], PREC_NONE, 0);
            }
            buf_char(&f->out, ']');
            return;
        }
        case AST_STRUCT_LITERAL:
            emit_struct_literal(f, (ASTStructLiteral*)node);
            return;
        case AST_MATCH_STMT:
            if (paren) buf_char(&f->out, '(');
            emit_match(f, (ASTMatchStmt*)node);
            if (paren) buf_char(&f->out, ')');
            return;
        case AST_BINARY_EXPR: {
            ASTBinaryExpr* b = (ASTBinaryExpr*)node;
            int prec = binary_prec(b->operator);
            if (paren) buf_char(&f->out, '(');
            emit_expr(f, b->left, prec, 0);
            buf_char(&f->out, ' ');
            buf_append(&f->out, binary_spelling(b->operator));
            buf_char(&f->out, ' ');
            emit_expr(f, b->right, prec, 1);
            if (paren) buf_char(&f->out, ')');
            return;
        }
        case AST_UNARY_EXPR: {
            ASTUnaryExpr* u = (ASTUnaryExpr*)node;
            if (paren) buf_char(&f->out, '(');
            buf_append(&f->out, u->operator == TOKEN_BANG ? "!" : "-");
            /* `-(-x)` — printing `--x` would lex as decrement. The
             * operand may hide a unary behind grouping parens, so
             * look through AST_GROUPING_EXPR wrappers first. */
            ASTNode* inner = u->right;
            while (inner && inner->type == AST_GROUPING_EXPR) {
                inner = ((ASTGroupingExpr*)inner)->expression;
            }
            if (inner && inner->type == AST_UNARY_EXPR) {
                buf_char(&f->out, '(');
                emit_expr(f, u->right, PREC_NONE, 0);
                buf_char(&f->out, ')');
            } else {
                emit_expr(f, u->right, PREC_UNARY, 0);
            }
            if (paren) buf_char(&f->out, ')');
            return;
        }
        case AST_CALL_EXPR: {
            ASTCallExpr* c = (ASTCallExpr*)node;
            if (paren) buf_char(&f->out, '(');
            buf_append(&f->out, c->name);  /* may be "Enum::Variant" */
            emit_type_args(f, c->type_args, c->type_arg_count);
            emit_expr_args(f, c->args, c->arg_count);
            if (paren) buf_char(&f->out, ')');
            return;
        }
        case AST_MEMBER_CALL: {
            ASTMemberCall* m = (ASTMemberCall*)node;
            if (paren) buf_char(&f->out, '(');
            emit_expr(f, m->object, PREC_POSTFIX, 0);
            buf_char(&f->out, '.');
            buf_append(&f->out, m->member_name);
            emit_type_args(f, m->type_args, m->type_arg_count);
            emit_expr_args(f, m->args, m->arg_count);
            if (paren) buf_char(&f->out, ')');
            return;
        }
        case AST_INDEX_EXPR: {
            ASTIndexExpr* ix = (ASTIndexExpr*)node;
            if (paren) buf_char(&f->out, '(');
            emit_expr(f, ix->array, PREC_POSTFIX, 0);
            buf_char(&f->out, '[');
            emit_expr(f, ix->index, PREC_NONE, 0);
            buf_char(&f->out, ']');
            if (paren) buf_char(&f->out, ')');
            return;
        }
        case AST_PROP_EXPR: {
            ASTPropExpr* p = (ASTPropExpr*)node;
            if (paren) buf_char(&f->out, '(');
            emit_expr(f, p->object, PREC_POSTFIX, 0);
            buf_char(&f->out, '.');
            buf_append(&f->out, p->prop_name);
            if (paren) buf_char(&f->out, ')');
            return;
        }
        default:
            /* Call statements reaching expression context etc. */
            emit_stmt_core(f, node, f->indent, 1);
            return;
    }
}

/* ────────────────────────────────────────────────────────────────────
 * Statements & declarations
 *
 * Contract: emit_stmt_core writes the statement text; the FINAL line it
 * produces is left unterminated so the caller can append ';' / ',' /
 * ' else' / the newline. Simple statements never emit their ';' when
 * no_semi is set (match arms). Compound statements manage their own
 * inner lines (headers end via end_line with the node's anchor so
 * trailing comments stick to the right physical line).
 * ──────────────────────────────────────────────────────────────────── */

/* `{` after a header; statements one level in; comments that live
 * between the last statement and the original '}' flushed by
 * fmt_close_brace; final line unterminated. */
static void emit_block_body(Fmt* f, ASTBlock* block, int indent, int header_anchor) {
    buf_char(&f->out, ' ');
    int idx = fmt_open_brace(f);
    if (!block || !block->statements) {
        buf_char(&f->out, ' ');
        fmt_close_brace_inline(f, idx, indent);
        return;
    }
    end_line(f, header_anchor);
    for (ASTNode* s = block->statements; s; s = s->next) {
        emit_stmt(f, s, indent + 1, 0);
    }
    fmt_close_brace(f, idx, indent);
}

/* `x++` / `x--` are DESUGARED by the parser into `x = x + 1` /
 * `x = x - 1`; re-detect the pattern so formatting is intent- and
 * idempotence-preserving (printing `x++` re-parses to the same AST). */
static int is_self_incr_decr(ASTAssignStmt* a, LamoTokenType op) {
    if (a->op_type != TOKEN_EQUALS || !a->value ||
        a->value->type != AST_BINARY_EXPR) return 0;
    ASTBinaryExpr* b = (ASTBinaryExpr*)a->value;
    if (b->operator != op || !b->left || b->left->type != AST_IDENTIFIER)
        return 0;
    if (strcmp(((ASTIdentifier*)b->left)->name, a->name) != 0) return 0;
    if (!b->right || b->right->type != AST_INT_LITERAL) return 0;
    return ((ASTIntLiteral*)b->right)->value == 1;
}

static int needs_semi(ASTNode* node) {
    switch (node->type) {
        case AST_VAR_DECL: case AST_ASSIGN_STMT: case AST_PLACE_ASSIGN_STMT:
        case AST_RETURN_STMT: case AST_BREAK_STMT: case AST_CONTINUE_STMT:
        case AST_CALL_STMT: case AST_MEMBER_CALL:
            return 1;
        default:
            return 0;
    }
}

static void emit_fn_decl(Fmt* f, ASTFnDecl* fn, int indent) {
    if (fn->base.is_pub) buf_append(&f->out, "pub ");
    buf_append(&f->out, "fn ");
    buf_append(&f->out, fn->name);

    if (fn->type_param_count > 0 && fn->type_params) {
        buf_char(&f->out, '<');
        for (int i = 0; i < fn->type_param_count; i++) {
            if (i) buf_append(&f->out, ", ");
            buf_append(&f->out, fn->type_params[i]);
            if (fn->type_param_constraints && fn->type_param_constraints[i]) {
                buf_char(&f->out, ':');
                buf_char(&f->out, ' ');
                buf_append(&f->out, fn->type_param_constraints[i]);
            }
        }
        buf_char(&f->out, '>');
    }

    buf_char(&f->out, '(');
    for (int i = 0; i < fn->param_count; i++) {
        if (i) buf_append(&f->out, ", ");
        buf_append(&f->out, fn->params[i]);
        if (fn->param_types && fn->param_types[i]) {
            buf_char(&f->out, ':');
            buf_char(&f->out, ' ');
            buf_append(&f->out, fn->param_types[i]);
        }
    }
    buf_char(&f->out, ')');

    if (fn->return_type_annotation) {
        buf_append(&f->out, " -> ");
        buf_append(&f->out, fn->return_type_annotation);
    }

    if (!fn->body) {
        /* Trait method signature: `fn area() -> float;` */
        buf_append(&f->out, ";");
        return;
    }
    emit_block_body(f, (ASTBlock*)fn->body, indent, fn->base.line);
}

static void emit_struct_decl(Fmt* f, ASTStructDecl* s, int indent) {
    if (s->base.is_pub) buf_append(&f->out, "pub ");
    buf_append(&f->out, "struct ");
    buf_append(&f->out, s->name);
    if (s->type_param_count > 0 && s->type_params) {
        buf_char(&f->out, '<');
        for (int i = 0; i < s->type_param_count; i++) {
            if (i) buf_append(&f->out, ", ");
            buf_append(&f->out, s->type_params[i]);
            if (s->type_param_constraints && s->type_param_constraints[i]) {
                buf_append(&f->out, ": ");
                buf_append(&f->out, s->type_param_constraints[i]);
            }
        }
        buf_char(&f->out, '>');
    }
    buf_char(&f->out, ' ');
    int idx = fmt_open_brace(f);
    if (s->field_count == 0) {
        buf_char(&f->out, ' ');
        fmt_close_brace_inline(f, idx, indent);
        return;
    }
    end_line(f, s->base.line);
    for (int i = 0; i < s->field_count; i++) {
        buf_indent(&f->out, indent + 1);
        buf_append(&f->out, s->field_names[i]);
        if (s->field_types && s->field_types[i]) {
            buf_append(&f->out, ": ");
            buf_append(&f->out, s->field_types[i]);
        }
        buf_char(&f->out, ',');
        buf_char(&f->out, '\n');
    }
    fmt_close_brace(f, idx, indent);
}

static void emit_enum_decl(Fmt* f, ASTEnumDecl* e, int indent) {
    if (e->base.is_pub) buf_append(&f->out, "pub ");
    buf_append(&f->out, "enum ");
    buf_append(&f->out, e->name);
    if (e->type_param_count > 0 && e->type_params) {
        buf_char(&f->out, '<');
        for (int i = 0; i < e->type_param_count; i++) {
            if (i) buf_append(&f->out, ", ");
            buf_append(&f->out, e->type_params[i]);
            if (e->type_param_constraints && e->type_param_constraints[i]) {
                buf_append(&f->out, ": ");
                buf_append(&f->out, e->type_param_constraints[i]);
            }
        }
        buf_char(&f->out, '>');
    }
    buf_char(&f->out, ' ');
    int idx = fmt_open_brace(f);
    if (e->variant_count == 0) {
        buf_char(&f->out, ' ');
        fmt_close_brace_inline(f, idx, indent);
        return;
    }
    end_line(f, e->base.line);
    for (int i = 0; i < e->variant_count; i++) {
        buf_indent(&f->out, indent + 1);
        buf_append(&f->out, e->variants[i]);
        if (e->variant_payloads && e->variant_payloads[i]) {
            buf_char(&f->out, '(');
            for (int j = 0; j < e->variant_payload_counts[i]; j++) {
                if (j) buf_append(&f->out, ", ");
                if (e->variant_payloads[i][j]) {
                    buf_append(&f->out, e->variant_payloads[i][j]);
                }
            }
            buf_char(&f->out, ')');
        }
        buf_char(&f->out, ',');
        buf_char(&f->out, '\n');
    }
    fmt_close_brace(f, idx, indent);
}

static void emit_impl_decl(Fmt* f, ASTImplDecl* im, int indent) {
    if (im->base.is_pub) buf_append(&f->out, "pub ");
    buf_append(&f->out, "impl");
    if (im->type_param_count > 0 && im->type_params) {
        buf_char(&f->out, '<');
        for (int i = 0; i < im->type_param_count; i++) {
            if (i) buf_append(&f->out, ", ");
            buf_append(&f->out, im->type_params[i]);
        }
        buf_char(&f->out, '>');
    }
    buf_char(&f->out, ' ');
    if (im->trait_name) {
        buf_append(&f->out, im->trait_name);
        buf_append(&f->out, " for ");
    }
    buf_append(&f->out, im->struct_name);
    if (im->type_arg_count > 0 && im->type_args) {
        buf_char(&f->out, '<');
        for (int i = 0; i < im->type_arg_count; i++) {
            if (i) buf_append(&f->out, ", ");
            buf_append(&f->out, im->type_args[i]);
        }
        buf_char(&f->out, '>');
    }
    buf_char(&f->out, ' ');
    int idx = fmt_open_brace(f);
    if (!im->methods) {
        buf_char(&f->out, ' ');
        fmt_close_brace_inline(f, idx, indent);
        return;
    }
    end_line(f, im->base.line);
    for (ASTNode* m = im->methods; m; m = m->next) {
        emit_stmt(f, m, indent + 1, 0);
    }
    fmt_close_brace(f, idx, indent);
}

static void emit_trait_decl(Fmt* f, ASTTraitDecl* t, int indent) {
    if (t->base.is_pub) buf_append(&f->out, "pub ");
    buf_append(&f->out, "trait ");
    buf_append(&f->out, t->name);
    buf_char(&f->out, ' ');
    int idx = fmt_open_brace(f);
    if (!t->methods) {
        buf_char(&f->out, ' ');
        fmt_close_brace_inline(f, idx, indent);
        return;
    }
    end_line(f, t->base.line);
    for (ASTNode* m = t->methods; m; m = m->next) {
        emit_stmt(f, m, indent + 1, 0);
    }
    fmt_close_brace(f, idx, indent);
}

static void emit_stmt_core(Fmt* f, ASTNode* node, int indent, int no_semi) {
    (void)no_semi;
    switch (node->type) {
        case AST_VAR_DECL: {
            ASTVarDecl* v = (ASTVarDecl*)node;
            if (v->base.is_pub) buf_append(&f->out, "pub ");
            buf_append(&f->out, "let ");
            buf_append(&f->out, v->name);
            if (v->type_annotation) {
                buf_append(&f->out, ": ");
                buf_append(&f->out, v->type_annotation);
            }
            buf_append(&f->out, " = ");
            if (v->initializer) emit_expr(f, v->initializer, PREC_NONE, 0);
            else buf_char(&f->out, '_');
            break;
        }
        case AST_FN_DECL:
            emit_fn_decl(f, (ASTFnDecl*)node, indent);
            break;
        case AST_ASSIGN_STMT: {
            ASTAssignStmt* a = (ASTAssignStmt*)node;
            if (is_self_incr_decr(a, TOKEN_PLUS)) {
                buf_append(&f->out, a->name);
                buf_append(&f->out, "++");
            } else if (is_self_incr_decr(a, TOKEN_MINUS)) {
                buf_append(&f->out, a->name);
                buf_append(&f->out, "--");
            } else {
                buf_append(&f->out, a->name);
                buf_char(&f->out, ' ');
                buf_append(&f->out, assign_spelling(a->op_type));
                buf_char(&f->out, ' ');
                if (a->value) emit_expr(f, a->value, PREC_NONE, 0);
            }
            break;
        }
        case AST_PLACE_ASSIGN_STMT: {
            ASTPlaceAssignStmt* p = (ASTPlaceAssignStmt*)node;
            if (p->target) emit_expr(f, p->target, PREC_NONE, 0);
            buf_char(&f->out, ' ');
            buf_append(&f->out, assign_spelling(p->op_type));
            buf_char(&f->out, ' ');
            if (p->value) emit_expr(f, p->value, PREC_NONE, 0);
            break;
        }
        case AST_IF_STMT: {
            ASTIfStmt* ifs = (ASTIfStmt*)node;
            buf_append(&f->out, "if (");
            if (ifs->condition) emit_expr(f, ifs->condition, PREC_NONE, 0);
            buf_char(&f->out, ')');
            emit_block_body(f, (ASTBlock*)ifs->then_branch, indent, ifs->base.line);
            if (ifs->else_branch) {
                buf_append(&f->out, " else");
                if (ifs->else_branch->type == AST_IF_STMT) {
                    buf_char(&f->out, ' ');
                    emit_stmt_core(f, ifs->else_branch, indent, 1);
                } else {
                    emit_block_body(f, (ASTBlock*)ifs->else_branch, indent, 0);
                }
            }
            break;
        }
        case AST_WHILE_STMT: {
            ASTWhileStmt* w = (ASTWhileStmt*)node;
            buf_append(&f->out, "while (");
            if (w->condition) emit_expr(f, w->condition, PREC_NONE, 0);
            buf_char(&f->out, ')');
            emit_block_body(f, (ASTBlock*)w->body, indent, w->base.line);
            break;
        }
        case AST_FOR_STMT: {
            ASTForStmt* fo = (ASTForStmt*)node;
            buf_append(&f->out, "for (");
            if (fo->initializer) emit_stmt_core(f, fo->initializer, indent, 1);
            buf_char(&f->out, ';');
            if (fo->condition) emit_expr(f, fo->condition, PREC_NONE, 0);
            buf_char(&f->out, ';');
            if (fo->increment) {
                buf_char(&f->out, ' ');
                emit_stmt_core(f, fo->increment, indent, 1);
            }
            buf_char(&f->out, ')');
            emit_block_body(f, (ASTBlock*)fo->body, indent, fo->base.line);
            break;
        }
        case AST_RETURN_STMT: {
            ASTReturnStmt* r = (ASTReturnStmt*)node;
            buf_append(&f->out, "return");
            if (r->expression) {
                buf_char(&f->out, ' ');
                emit_expr(f, r->expression, PREC_NONE, 0);
            }
            break;
        }
        case AST_BREAK_STMT:
            buf_append(&f->out, "break");
            break;
        case AST_CONTINUE_STMT:
            buf_append(&f->out, "continue");
            break;
        case AST_CALL_STMT: {
            ASTCallStmt* c = (ASTCallStmt*)node;
            buf_append(&f->out, c->name);
            emit_type_args(f, c->type_args, c->type_arg_count);
            emit_expr_args(f, c->args, c->arg_count);
            break;
        }
        case AST_MEMBER_CALL:
        case AST_INDEX_EXPR:
        case AST_PROP_EXPR:
        case AST_CALL_EXPR:
        case AST_STRUCT_LITERAL:
            /* Value expressions appearing in statement position (the
             * parser produces these for e.g. method-call statements). */
            emit_expr(f, node, PREC_NONE, 0);
            break;
        case AST_IMPORT: {
            ASTImport* im = (ASTImport*)node;
            buf_append(&f->out, "import ");
            emit_escaped_string(f, im->path);
            if (im->alias) {
                buf_append(&f->out, " as ");
                buf_append(&f->out, im->alias);
            }
            buf_append(&f->out, ";");
            break;
        }
        case AST_STRUCT_DECL:
            emit_struct_decl(f, (ASTStructDecl*)node, indent);
            break;
        case AST_ENUM_DECL:
            emit_enum_decl(f, (ASTEnumDecl*)node, indent);
            break;
        case AST_IMPL_DECL:
            emit_impl_decl(f, (ASTImplDecl*)node, indent);
            break;
        case AST_TRAIT_DECL:
            emit_trait_decl(f, (ASTTraitDecl*)node, indent);
            break;
        case AST_MATCH_STMT:
            emit_match(f, (ASTMatchStmt*)node);
            break;
        case AST_BLOCK: {
            /* Bare block statement (match arm bodies parse here too,
             * but the match printer handles those directly). */
            buf_char(&f->out, ' ');
            int idx = fmt_open_brace(f);
            ASTBlock* b = (ASTBlock*)node;
            if (!b->statements) {
                buf_char(&f->out, ' ');
                fmt_close_brace_inline(f, idx, indent);
                break;
            }
            end_line(f, node->line);
            for (ASTNode* s = b->statements; s; s = s->next) {
                emit_stmt(f, s, indent + 1, 0);
            }
            fmt_close_brace(f, idx, indent);
            break;
        }
        default:
            /* Pure expression nodes in statement-ish positions —
             * notably match-arm EXPRESSION bodies (`1 => 10,`) and any
             * value the parser hands over in statement context. */
            emit_expr(f, node, PREC_NONE, 0);
            break;
    }
}

/* Statements whose core ends with an UNTERMINATED `}` (compound).
 * Their header-line trailing comments are flushed inside the core;
 * the wrapper must close the final line with anchor 0 so comments do
 * not migrate onto the closing brace line. */
static int is_compound_stmt(ASTNode* n) {
    switch (n->type) {
        case AST_FN_DECL: case AST_IF_STMT: case AST_WHILE_STMT:
        case AST_FOR_STMT: case AST_MATCH_STMT: case AST_STRUCT_DECL:
        case AST_ENUM_DECL: case AST_IMPL_DECL: case AST_TRAIT_DECL:
        case AST_BLOCK:
            return 1;
        default:
            return 0;
    }
}

/* Full statement line: leading comments, indent, statement, optional
 * ';', trailing same-line comments, newline. */
static void emit_stmt(Fmt* f, ASTNode* node, int indent, int no_semi) {
    if (!node) return;
    flush_leading(f, node->line, indent);
    buf_indent(&f->out, indent);
    f->indent = indent;
    emit_stmt_core(f, node, indent, no_semi);
    if (!no_semi && needs_semi(node)) buf_char(&f->out, ';');
    if (no_semi) {
        /* Inline context (match arm): the caller terminates the line. */
        return;
    }
    end_line(f, is_compound_stmt(node) ? 0 : node->line);
}

/* ────────────────────────────────────────────────────────────────────
 * Program emission + entry point
 * ──────────────────────────────────────────────────────────────────── */

/* Blank line between top-level items unless both cluster together. */
static int same_top_level_cluster(ASTNode* a, ASTNode* b) {
    if (!a || !b) return 0;
    if (a->type == AST_IMPORT && b->type == AST_IMPORT) return 1;
    if (a->type == AST_VAR_DECL && b->type == AST_VAR_DECL) return 1;
    return 0;
}

char* fmt_format_source(const char* source, const char* file_path, int* ok) {
    *ok = 0;
    if (!source) return NULL;

    /* 1. Parse with the real parser. Any error => refuse the file. */
    char* lex_source = strdup(source);
    if (!lex_source) return NULL;
    Lexer* lexer = lexer_init(lex_source);
    if (!lexer) {
        free(lex_source);
        return NULL;
    }
    Parser* parser = parser_init_with_file(lexer, file_path ? file_path : "<fmt>");
    ASTProgram* program = parse_program_v2(parser);
    if (parser_had_error(parser)) {
        ast_free((ASTNode*)program);
        parser_free(parser);
        lexer_free(lexer);
        free(lex_source);
        return NULL;
    }

    /* 2. Capture comments + brace map from the raw source. */
    Fmt f;
    buf_init(&f.out);
    f.comments.items = NULL;
    f.comments.count = 0;
    f.comments.cap = 0;
    f.next_comment = 0;
    f.indent = 0;
    scan_comments(source, &f.comments);
    brace_map_scan(source, &f.braces);
    f.next_brace = 0;

    /* 3. Emit declarations with top-level blank-line clustering. */
    ASTNode* decl = program ? program->declarations : NULL;
    ASTNode* prev = NULL;
    int first = 1;
    for (; decl; prev = decl, decl = decl->next) {
        if (!first && !same_top_level_cluster(prev, decl)) {
            buf_char(&f.out, '\n');
        }
        emit_stmt(&f, decl, 0, 0);
        first = 0;
    }
    flush_remaining(&f);

    /* 4. Guarantee exactly one trailing newline (empty stays empty). */
    while (f.out.len >= 2 && f.out.data[f.out.len - 1] == '\n' &&
           f.out.data[f.out.len - 2] == '\n') {
        f.out.data[--f.out.len] = '\0';
    }

    /* 5. Cleanup. AST nodes own no comment text; free our scans. */
    brace_map_free(&f.braces);
    comments_free(&f.comments);
    ast_free((ASTNode*)program);
    parser_free(parser);
    lexer_free(lexer);
    free(lex_source);

    *ok = 1;
    return f.out.data;
}
