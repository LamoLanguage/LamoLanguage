#include "codegen.h"
#include "lamo_runtime_data.h"
#include "../builtins.h"
#include "../modules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>   /* fmod() — used by constant folding for float % */

static int indent_level = 0;

/* GC Step 3: scope tracking for root push/pop.
 *
 * The codegen emits LAMO_GC_PUSH_ROOT(&v) for every LamoValue parameter
 * and every LamoValue local declared with `let`. To balance the pushes
 * with pops, we maintain a compile-time stack of "roots pushed in this
 * scope". On scope exit (block close `}` or function return), we emit
 * LAMO_GC_POP_ROOTS_N(count) for the appropriate count.
 *
 * The stack is bounded by 64 — that's the maximum nesting depth of
 * blocks within a single function. Real programs rarely exceed ~10.
 *
 * On `return`, we pop ALL active scopes (sum of all entries on the
 * stack), because every root pushed in the current function must be
 * balanced before the function returns. */
#define LAMO_GC_SCOPE_MAX 64
static int lamo_gc_scope_stack[LAMO_GC_SCOPE_MAX];
static int lamo_gc_scope_top = 0;

static void lamo_gc_scope_enter(void) {
    if (lamo_gc_scope_top < LAMO_GC_SCOPE_MAX) {
        lamo_gc_scope_stack[lamo_gc_scope_top++] = 0;
    }
}
static void lamo_gc_scope_push_root(void) {
    if (lamo_gc_scope_top > 0) {
        lamo_gc_scope_stack[lamo_gc_scope_top - 1]++;
    }
}
static int lamo_gc_scope_exit(void) {
    if (lamo_gc_scope_top > 0) {
        return lamo_gc_scope_stack[--lamo_gc_scope_top];
    }
    return 0;
}
static int lamo_gc_scope_total_roots(void) {
    int total = 0;
    int i;
    for (i = 0; i < lamo_gc_scope_top; i++) total += lamo_gc_scope_stack[i];
    return total;
}
/* Reset the scope stack — called at the start of each function (and at
 * the start of main()) so leftover state from a previous function doesn't
 * leak into the new one. */
static void lamo_gc_scope_reset(void) {
    lamo_gc_scope_top = 0;
}

/* Sprint 4: module registry pointer. Set via codegen_set_module_registry()
 * before generate_c_code() is called. May be NULL — in that case,
 * AST_MEMBER_CALL nodes emit a defensive `lamo_make_int(0)` (the
 * semantic pass should have already rejected them, so this is just a
 * safety net). */
static LamoModuleRegistry* g_module_registry = NULL;

/* Phase 2: pointer to the program's top-level declarations list. Set at
 * the start of generate_c_code() so generate_prop_expr_code and
 * generate_struct_literal_code can walk it to find struct definitions
 * and look up field indices. NULL outside of generate_c_code(). */
static ASTNode* g_program_decls = NULL;

/* 2.6.0 (FU5): the entry file's normalized path, set via
 * codegen_set_entry_file(). Used to decide whether `fn main()` in the
 * aggregate program belongs to the ENTRY file (and must be called from
 * the C entry point) or to an imported library file (renamed or merged
 * — never invoked). NULL disables the call entirely (REPL codegen). */
static const char* g_entry_file = NULL;

void codegen_set_module_registry(LamoModuleRegistry* reg) {
    g_module_registry = reg;
}

/* 2.6.0 (FU5): record the entry file so the C entry point calls the
 * entry file's fn main() (SPEC §12.1). The path must be the normalized
 * path the loader assigned to the entry AST's nodes. Pass NULL to
 * disable (no fn main call in the generated main). */
void codegen_set_entry_file(const char* entry_path) {
    g_entry_file = entry_path;
}

/* Find the entry file's user main (name == "main", zero params, defined
 * in the entry file). Returns the decl or NULL. */
static ASTFnDecl* find_entry_main(ASTNode* declarations) {
    ASTNode* cur;
    if (!g_entry_file) return NULL;
    for (cur = declarations; cur; cur = cur->next) {
        if (cur->type == AST_FN_DECL) {
            ASTFnDecl* fn = (ASTFnDecl*)cur;
            if (fn->name && strcmp(fn->name, "main") == 0 &&
                fn->param_count == 0) {
                if (fn->base.file_path && strcmp(fn->base.file_path, g_entry_file) == 0) {
                    return fn;
                }
            }
        }
    }
    return NULL;
}

/* 2.6.0 (FU5): back-compat guard. Pre-2.6 programs had to call main()
 * explicitly as a top-level statement (the implicit call was missing).
 * When such an explicit top-level `main(...)` call statement exists,
 * the implicit call is suppressed so the function runs exactly once —
 * old programs keep their exact behavior, new programs (no explicit
 * call) get the SPEC §12.1 semantics. */
static int program_has_explicit_main_call(ASTNode* declarations) {
    ASTNode* cur;
    for (cur = declarations; cur; cur = cur->next) {
        if (cur->type == AST_CALL_STMT) {
            ASTCallStmt* cs = (ASTCallStmt*)cur;
            if (cs->name && strcmp(cs->name, "main") == 0) return 1;
        }
    }
    return 0;
}

static void print_indent(FILE* out) {
    int i;
    for (i = 0; i < indent_level; i++) {
        fprintf(out, "    ");
    }
}

// Prefixo adicionado a todos os identificadores declarados pelo usuário para
// evitar colisões com nomes da libc (abs, exit, index, ...).
// Builtins da linguagem (print, input, ...) e builtins GUI/HTTP continuam
// sendo detectados pelo nome original.
#define LAMO_USER_PREFIX "lamo_u_"

// Bug #3 fix: user_name() no longer returns a pointer into a static buffer.
// Instead, callers pass their own buffer (256 bytes is enough for any
// identifier; the language has no arbitrary-length identifier limits today).
// This makes it safe to use multiple user_name() calls in the same fprintf,
// e.g. fprintf(out, "%s, %s", user_name(a, bufa), user_name(b, bufb)).
/* LAMO_USER_NAME_MAX must be large enough for the longest user symbol:
 * the prefix (7 chars) + the longest identifier we might prefix. 512
 * gives plenty of headroom and silences -Wformat-truncation. */
#define LAMO_USER_NAME_MAX 512

#if defined(__GNUC__) || defined(__clang__)
#define LAMO_CGEN_UNUSED __attribute__((unused))
#else
#define LAMO_CGEN_UNUSED
#endif

// Explicit-buffer form. Kept for future use cases where a caller wants full
// control over buffer lifetime (e.g. embedding multiple user_name() results
// in the same fprintf). Currently unused — all callers use user_name1().
static LAMO_CGEN_UNUSED const char* user_name(const char* name, char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "%s%s", LAMO_USER_PREFIX, name);
    return buffer;
}

// Convenience wrapper that uses a fresh stack buffer for each call. Use this
// when you only need ONE user_name() per expression. If you need two (e.g.
// for "lamo_add(a, b)"), use user_name() with two explicit buffers.
static const char* user_name1(const char* name) {
    // Each call returns a pointer into its own static buffer. We keep a small
    // ring of 4 buffers so that up to 4 user_name1() calls in the same
    // expression don't collide. This is a pragmatic compromise: the previous
    // code had a single static buffer (Bug #3), which broke for cases like
    // `user_name(a) + ", " + user_name(a)`. The ring is not thread-safe but
    // the compiler is single-threaded.
    static char ring[4][LAMO_USER_NAME_MAX];
    static int idx = 0;
    char* buffer = ring[idx];
    idx = (idx + 1) % 4;
    snprintf(buffer, LAMO_USER_NAME_MAX, "%s%s", LAMO_USER_PREFIX, name);
    return buffer;
}

/* 2.7.0 (FU4): qualified constructor calls reuse AST_CALL_EXPR /
 * AST_CALL_STMT with the compound name "Enum::Variant"; the runtime
 * variant literal is only the variant part. */
static const char* lamo_variant_short_name(const char* name) {
    const char* sep = strstr(name, "::");
    return sep ? sep + 2 : name;
}

/* 2.7.0 (FU4): is the named enum a tagged union (payload variants)?
 * Used by the AST_VARIANT_REF emitter to pick lamo_make_enum vs
 * lamo_make_int. Mirrors semantic.c's enum_decl_is_tagged. */
static int lamo_codegen_enum_is_tagged(const char* enum_name) {
    for (ASTNode* cur = g_program_decls; cur; cur = cur->next) {
        if (cur->type == AST_ENUM_DECL) {
            ASTEnumDecl* cand = (ASTEnumDecl*)cur;
            if (cand->name && strcmp(cand->name, enum_name) == 0) {
                if (cand->variant_payload_counts) {
                    for (int v = 0; v < cand->variant_count; v++) {
                        if (cand->variant_payload_counts[v] > 0) return 1;
                    }
                }
                return 0;
            }
        }
    }
    return 0;
}

static void generate_statement_code(ASTNode* node, FILE* out);
static void generate_expression_code(ASTNode* node, FILE* out);
static void generate_call_arguments(ASTNode** args, int arg_count, FILE* out);
/* Sprint 2 refactor: is_gui_builtin / is_http_builtin / is_lang_builtin are
 * now inline functions in builtins.h, so we just use lamo_builtin_is_*()
 * directly. The forward declarations below are kept for the code paths that
 * still call them by the old names. */
#define is_gui_builtin(name)    lamo_builtin_is_gui(name)
#define is_http_builtin(name)   lamo_builtin_is_http(name)
#define is_lang_builtin(name)   lamo_builtin_is_lang(name)
#define is_std_builtin(name)    lamo_builtin_is_std(name)
static void generate_lang_builtin_call_expr(const char* name, ASTNode** args, int arg_count, FILE* out);
static void generate_gui_call_expr(const char* name, ASTNode** args, int arg_count, FILE* out);
static void generate_http_call_expr(const char* name, ASTNode** args, int arg_count, FILE* out);
static void generate_std_builtin_call_expr(const char* name, ASTNode** args, int arg_count, FILE* out);
static void emit_runtime(FILE* out, int needs_gui, int needs_http, int needs_std, int feat_flags);
static int ast_uses_gui(ASTNode* node);
static int ast_uses_http(ASTNode* node);
static int ast_uses_std(ASTNode* node);



static void generate_call_arguments(ASTNode** args, int arg_count, FILE* out) {
    int i;
    for (i = 0; i < arg_count; i++) {
        if (i > 0) {
            fprintf(out, ", ");
        }
        generate_expression_code(args[i], out);
    }
}

/* Sprint 2 refactor: is_gui_builtin / is_http_builtin / is_lang_builtin
 * were inlined into lamo_builtin_is_*() in builtins.h. The macros above
 * rewire the old call sites to the new shared table. */



static void generate_lang_builtin_call_expr(const char* name, ASTNode** args, int arg_count, FILE* out) {
    (void)arg_count;

    if (strcmp(name, "print") == 0) {
        /* Backend-alignment: when the semantic pass knows the argument
         * is a struct (sema_struct_name set), emit the NAMED printer so
         * output reads `Player { 10, x }` instead of `{ 10, x }` — the
         * runtime cannot know type names, the compiler does. All other
         * cases fall through to the legacy printer verbatim. */
        if (args[0]->sema_struct_name) {
            fprintf(out, "(lamo_print_struct_named(");
            generate_expression_code(args[0], out);
            fprintf(out, ", \"%s\"), lamo_make_int(0))", args[0]->sema_struct_name);
        } else {
            fprintf(out, "(lamo_print_value(");
            generate_expression_code(args[0], out);
            fprintf(out, "), lamo_make_int(0))");
        }
        return;
    }
    if (strcmp(name, "input") == 0) {
        fprintf(out, "lamo_input_value(");
        generate_expression_code(args[0], out);
        fprintf(out, ")");
        return;
    }
    if (strcmp(name, "input_int") == 0) {
        fprintf(out, "lamo_input_int_value(");
        generate_expression_code(args[0], out);
        fprintf(out, ")");
        return;
    }
    if (strcmp(name, "input_str") == 0) {
        fprintf(out, "lamo_input_str_value(");
        generate_expression_code(args[0], out);
        fprintf(out, ")");
        return;
    }
    if (strcmp(name, "isnumber") == 0) {
        fprintf(out, "lamo_isnumber_value(");
        generate_expression_code(args[0], out);
        fprintf(out, ")");
        return;
    }
    if (strcmp(name, "isstring") == 0) {
        fprintf(out, "lamo_isstring_value(");
        generate_expression_code(args[0], out);
        fprintf(out, ")");
        return;
    }
    if (strcmp(name, "isarray") == 0) {
        fprintf(out, "lamo_isarray_value(");
        generate_expression_code(args[0], out);
        fprintf(out, ")");
        return;
    }
    if (strcmp(name, "exit") == 0) {
        fprintf(out, "(exit(lamo_as_int(");
        generate_expression_code(args[0], out);
        fprintf(out, ")), lamo_make_int(0))");
        return;
    }
    if (strcmp(name, "abs") == 0) {
        fprintf(out, "lamo_abs_value(");
        generate_expression_code(args[0], out);
        fprintf(out, ")");
        return;
    }
    /* Sprint 3: array builtins. */
    if (strcmp(name, "len") == 0) {
        fprintf(out, "lamo_array_len(");
        generate_expression_code(args[0], out);
        fprintf(out, ")");
        return;
    }
    if (strcmp(name, "push") == 0) {
        fprintf(out, "lamo_array_push(");
        generate_expression_code(args[0], out);
        fprintf(out, ", ");
        generate_expression_code(args[1], out);
        fprintf(out, ")");
        return;
    }
    if (strcmp(name, "pop") == 0) {
        fprintf(out, "lamo_array_pop(");
        generate_expression_code(args[0], out);
        fprintf(out, ")");
        return;
    }
    /* GC builtins — opt-in mark-sweep (see docs/MEMORY-MODEL.md). */
    if (strcmp(name, "gc_collect") == 0) {
        fprintf(out, "lamo_make_int(lamo_gc_collect_count())");
        return;
    }
    if (strcmp(name, "gc_set_threshold") == 0) {
        fprintf(out, "(lamo_gc_set_threshold((size_t)lamo_as_int(");
        generate_expression_code(args[0], out);
        fprintf(out, ")), lamo_make_int(0))");
        return;
    }
    if (strcmp(name, "gc_heap_size") == 0) {
        fprintf(out, "lamo_make_int(lamo_gc_heap_size())");
        return;
    }
    if (strcmp(name, "gc_heap_count") == 0) {
        fprintf(out, "lamo_make_int(lamo_gc_heap_count())");
        return;
    }
    fprintf(out, "lamo_make_int(0)");
}



static void generate_gui_call_expr(const char* name, ASTNode** args, int arg_count, FILE* out) {
    if (strcmp(name, "gui_open") == 0 && arg_count == 3) {
        fprintf(out, "lamo_make_int(lamo_gui_open(lamo_as_int(");
        generate_expression_code(args[0], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[1], out);
        fprintf(out, "), lamo_as_cstring(");
        generate_expression_code(args[2], out);
        fprintf(out, ")))");
        return;
    }
    if (strcmp(name, "gui_should_close") == 0 && arg_count == 0) {
        fprintf(out, "lamo_make_int(lamo_gui_should_close())");
        return;
    }
    if (strcmp(name, "gui_begin_frame") == 0 && arg_count == 3) {
        fprintf(out, "(lamo_gui_begin_frame(lamo_as_int(");
        generate_expression_code(args[0], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[1], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[2], out);
        fprintf(out, ")), lamo_make_int(0))");
        return;
    }
    if (strcmp(name, "gui_draw_rect") == 0 && arg_count == 7) {
        fprintf(out, "(lamo_gui_draw_rect(lamo_as_int(");
        generate_expression_code(args[0], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[1], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[2], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[3], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[4], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[5], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[6], out);
        fprintf(out, ")), lamo_make_int(0))");
        return;
    }
    if (strcmp(name, "gui_draw_text") == 0 && arg_count == 6) {
        fprintf(out, "(lamo_gui_draw_text(lamo_as_cstring(");
        generate_expression_code(args[0], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[1], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[2], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[3], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[4], out);
        fprintf(out, "), lamo_as_int(");
        generate_expression_code(args[5], out);
        fprintf(out, ")), lamo_make_int(0))");
        return;
    }
    if (strcmp(name, "gui_end_frame") == 0 && arg_count == 0) {
        fprintf(out, "(lamo_gui_end_frame(), lamo_make_int(0))");
        return;
    }
    if (strcmp(name, "gui_close") == 0 && arg_count == 0) {
        fprintf(out, "(lamo_gui_close(), lamo_make_int(0))");
        return;
    }
    fprintf(out, "lamo_make_int(0)");
}



static void generate_http_call_expr(const char* name, ASTNode** args, int arg_count, FILE* out) {
    if (strcmp(name, "http_route") == 0 && arg_count == 2) {
        fprintf(out, "(lamo_http_add_route(lamo_as_cstring(");
        generate_expression_code(args[0], out);
        fprintf(out, "), lamo_as_cstring(");
        generate_expression_code(args[1], out);
        fprintf(out, ")), lamo_make_int(0))");
        return;
    }
    if (strcmp(name, "http_serve") == 0 && arg_count == 1) {
        fprintf(out, "lamo_make_int(lamo_http_run_server(lamo_as_int(");
        generate_expression_code(args[0], out);
        fprintf(out, "), 0))");
        return;
    }
    if (strcmp(name, "http_serve_once") == 0 && arg_count == 1) {
        fprintf(out, "lamo_make_int(lamo_http_run_server(lamo_as_int(");
        generate_expression_code(args[0], out);
        fprintf(out, "), 1))");
        return;
    }
    fprintf(out, "lamo_make_int(0)");
}



/* Standard-library builtins (BUILTIN_STD). These are prefixed with
 * __lamo_std_ so they cannot collide with user-defined functions or
 * with the legacy LANG builtins. The std/<module>.lamo wrappers expose
 * them through the namespaced import API (math.sqrt, fs.readText, ...).
 *
 * Each case maps the builtin name to its runtime function and emits
 * the appropriate value-type coercions (lamo_as_int / lamo_as_float /
 * lamo_as_cstring) for arguments. The runtime functions return LamoValue
 * directly, so most cases are one-liners. */
static void generate_std_builtin_call_expr(const char* name, ASTNode** args, int arg_count, FILE* out) {
    (void)arg_count;

    /* ----- std.math ----- */
    if (strcmp(name, "__lamo_std_math_sqrt") == 0) {
        fprintf(out, "lamo_math_sqrt("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_math_pow") == 0) {
        fprintf(out, "lamo_math_pow("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_math_sin") == 0) {
        fprintf(out, "lamo_math_sin("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_math_cos") == 0) {
        fprintf(out, "lamo_math_cos("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_math_tan") == 0) {
        fprintf(out, "lamo_math_tan("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_math_floor") == 0) {
        fprintf(out, "lamo_math_floor("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_math_ceil") == 0) {
        fprintf(out, "lamo_math_ceil("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_math_round") == 0) {
        fprintf(out, "lamo_math_round("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_math_min") == 0) {
        fprintf(out, "lamo_math_min("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_math_max") == 0) {
        fprintf(out, "lamo_math_max("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_math_clamp") == 0) {
        fprintf(out, "lamo_math_clamp("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out);
        fprintf(out, ", "); generate_expression_code(args[2], out); fprintf(out, ")"); return;
    }

    /* ----- std.string ----- */
    if (strcmp(name, "__lamo_std_str_length") == 0) {
        fprintf(out, "lamo_str_length("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_upper") == 0) {
        fprintf(out, "lamo_str_upper("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_lower") == 0) {
        fprintf(out, "lamo_str_lower("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_starts_with") == 0) {
        fprintf(out, "lamo_str_starts_with("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_ends_with") == 0) {
        fprintf(out, "lamo_str_ends_with("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_contains") == 0) {
        fprintf(out, "lamo_str_contains("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_index_of") == 0) {
        fprintf(out, "lamo_str_index_of("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_trim") == 0) {
        fprintf(out, "lamo_str_trim("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_substring") == 0) {
        fprintf(out, "lamo_str_substring("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out);
        fprintf(out, ", "); generate_expression_code(args[2], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_replace") == 0) {
        fprintf(out, "lamo_str_replace("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out);
        fprintf(out, ", "); generate_expression_code(args[2], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_split") == 0) {
        fprintf(out, "lamo_str_split("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_char_at") == 0) {
        fprintf(out, "lamo_str_char_at("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_str_repeat") == 0) {
        fprintf(out, "lamo_str_repeat("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }

    /* ----- std.path ----- */
    if (strcmp(name, "__lamo_std_path_join") == 0) {
        fprintf(out, "lamo_path_join("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_path_parent") == 0) {
        fprintf(out, "lamo_path_parent("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_path_filename") == 0) {
        fprintf(out, "lamo_path_filename("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_path_extension") == 0) {
        fprintf(out, "lamo_path_extension("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_path_absolute") == 0) {
        fprintf(out, "lamo_path_absolute("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_path_normalize") == 0) {
        fprintf(out, "lamo_path_normalize("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }

    /* ----- std.fs ----- */
    if (strcmp(name, "__lamo_std_fs_exists") == 0) {
        fprintf(out, "lamo_fs_exists("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_is_file") == 0) {
        fprintf(out, "lamo_fs_is_file("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_is_dir") == 0) {
        fprintf(out, "lamo_fs_is_dir("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_read_text") == 0) {
        fprintf(out, "lamo_fs_read_text("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_write_text") == 0) {
        fprintf(out, "lamo_fs_write_text("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_append_text") == 0) {
        fprintf(out, "lamo_fs_append_text("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_delete") == 0) {
        fprintf(out, "lamo_fs_delete("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_create_dir") == 0) {
        fprintf(out, "lamo_fs_create_dir("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_remove_dir") == 0) {
        fprintf(out, "lamo_fs_remove_dir("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_copy") == 0) {
        fprintf(out, "lamo_fs_copy("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_move") == 0) {
        fprintf(out, "lamo_fs_move("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_list_files") == 0) {
        fprintf(out, "lamo_fs_list_files("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_fs_size") == 0) {
        fprintf(out, "lamo_fs_size("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }

    /* ----- std.env ----- */
    if (strcmp(name, "__lamo_std_env_get") == 0) {
        fprintf(out, "lamo_env_get("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_env_set") == 0) {
        fprintf(out, "lamo_env_set("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_env_remove") == 0) {
        fprintf(out, "lamo_env_remove("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }

    /* ----- std.os ----- */
    if (strcmp(name, "__lamo_std_os_name") == 0)      { fprintf(out, "lamo_os_name()"); return; }
    if (strcmp(name, "__lamo_std_os_arch") == 0)      { fprintf(out, "lamo_os_arch()"); return; }
    if (strcmp(name, "__lamo_std_os_cpu_count") == 0) { fprintf(out, "lamo_os_cpu_count()"); return; }
    if (strcmp(name, "__lamo_std_os_home") == 0)      { fprintf(out, "lamo_os_home()"); return; }
    if (strcmp(name, "__lamo_std_os_temp_dir") == 0)  { fprintf(out, "lamo_os_temp_dir()"); return; }

    /* ----- std.time ----- */
    if (strcmp(name, "__lamo_std_time_now") == 0)       { fprintf(out, "lamo_time_now()"); return; }
    if (strcmp(name, "__lamo_std_time_timestamp") == 0){ fprintf(out, "lamo_time_timestamp()"); return; }
    if (strcmp(name, "__lamo_std_time_sleep") == 0) {
        fprintf(out, "lamo_time_sleep("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_time_monotonic") == 0){ fprintf(out, "lamo_time_monotonic()"); return; }

    /* ----- std.process ----- */
    if (strcmp(name, "__lamo_std_process_pid") == 0)  { fprintf(out, "lamo_process_pid()"); return; }
    if (strcmp(name, "__lamo_std_process_run") == 0) {
        fprintf(out, "lamo_process_run("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_process_exec") == 0) {
        fprintf(out, "lamo_process_exec("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_process_exit") == 0) {
        fprintf(out, "(exit(lamo_as_int("); generate_expression_code(args[0], out);
        fprintf(out, ")), lamo_make_int(0))"); return;
    }

    /* ----- std.random ----- */
    if (strcmp(name, "__lamo_std_random_seed") == 0) {
        fprintf(out, "lamo_random_seed("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_random_int") == 0) {
        fprintf(out, "lamo_random_int("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_random_float") == 0) { fprintf(out, "lamo_random_float()"); return; }
    if (strcmp(name, "__lamo_std_random_bool") == 0)  { fprintf(out, "lamo_random_bool()"); return; }
    if (strcmp(name, "__lamo_std_random_choice") == 0) {
        fprintf(out, "lamo_random_choice("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_random_shuffle") == 0) {
        fprintf(out, "lamo_random_shuffle("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }

    /* ----- std.io ----- */
    if (strcmp(name, "__lamo_std_io_println") == 0) {
        fprintf(out, "lamo_io_println("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_io_eprint") == 0) {
        fprintf(out, "lamo_io_eprint("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_io_read_line") == 0) { fprintf(out, "lamo_io_read_line()"); return; }
    if (strcmp(name, "__lamo_std_io_write") == 0) {
        fprintf(out, "lamo_io_write("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }

    /* ----- std.net ----- */
    if (strcmp(name, "__lamo_std_net_http_get") == 0) {
        fprintf(out, "lamo_net_http_get("); generate_expression_code(args[0], out); fprintf(out, ")"); return;
    }
    if (strcmp(name, "__lamo_std_net_http_post") == 0) {
        fprintf(out, "lamo_net_http_post("); generate_expression_code(args[0], out);
        fprintf(out, ", "); generate_expression_code(args[1], out); fprintf(out, ")"); return;
    }

    fprintf(out, "lamo_make_int(0)");
}


// Emits the entire Lamo runtime (value + GUI + HTTP) by writing the pre-built
// string literal lamo_runtime_source (see lamo_runtime_data.c). The value
// runtime is always emitted; GUI and HTTP runtimes are gated behind #define
// LAMO_NEEDS_GUI_RUNTIME / LAMO_NEEDS_HTTP_RUNTIME, which we set here based
// on whether the program uses those builtins. This replaces ~600 lines of
// fprintf() calls in the old emit_value_runtime / emit_gui_runtime /
// emit_http_runtime functions (nit #9 fix).
#define FEAT_STRINGS (1 << 0)
#define FEAT_ARRAYS  (1 << 1)
#define FEAT_FLOATS  (1 << 2)

static void emit_runtime(FILE* out, int needs_gui, int needs_http, int needs_std, int feat_flags) {
    fputs("#define LAMO_NEEDS_VALUE_RUNTIME 1\n", out);
    /* Fine-grained feature flags: let GCC dead-strip unused subsections
     * even at -O0. A program that never touches strings, arrays, or floats
     * sees a measurably smaller generated .c and faster GCC invocation. */
    if (feat_flags & FEAT_STRINGS) fputs("#define LAMO_NEEDS_STRING_OPS 1\n", out);
    if (feat_flags & FEAT_ARRAYS)  fputs("#define LAMO_NEEDS_ARRAY_OPS 1\n",  out);
    if (feat_flags & FEAT_FLOATS)  fputs("#define LAMO_NEEDS_FLOAT_OPS 1\n",  out);
    if (needs_gui) {
        fputs("#define LAMO_NEEDS_GUI_RUNTIME 1\n", out);
    }
    if (needs_http) {
        fputs("#define LAMO_NEEDS_HTTP_RUNTIME 1\n", out);
    }
    if (needs_std) {
        fputs("#define LAMO_NEEDS_STD_RUNTIME 1\n", out);
    }
    fputs(lamo_runtime_source, out);
    fputs("\n#undef LAMO_NEEDS_VALUE_RUNTIME\n", out);
    if (feat_flags & FEAT_STRINGS) fputs("#undef LAMO_NEEDS_STRING_OPS\n", out);
    if (feat_flags & FEAT_ARRAYS)  fputs("#undef LAMO_NEEDS_ARRAY_OPS\n",  out);
    if (feat_flags & FEAT_FLOATS)  fputs("#undef LAMO_NEEDS_FLOAT_OPS\n",  out);
    if (needs_gui) {
        fputs("#undef LAMO_NEEDS_GUI_RUNTIME\n", out);
    }
    if (needs_http) {
        fputs("#undef LAMO_NEEDS_HTTP_RUNTIME\n", out);
    }
    if (needs_std) {
        fputs("#undef LAMO_NEEDS_STD_RUNTIME\n", out);
    }
    fputs("\n", out);
}

/* Sprint 2 refactor: ast_uses_gui() and ast_uses_http() were ~95% copy-paste
 * of the same AST walk. Now we have a single recursive walker that takes a
 * predicate ("does this call name match the builtin family we're looking
 * for?") and returns 1 if any call in the AST matches. The two old entry
 * points become one-line wrappers around ast_uses_builtin().
 *
 * The predicate is a function pointer rather than a category enum so that
 * future call sites (e.g. "does this AST call any shadowable builtin?")
 * can use the same walker without extending the table. */
static int ast_uses_builtin(ASTNode* node, int (*predicate)(const char*)) {
    int i;

    if (!node) {
        return 0;
    }

    switch (node->type) {
        case AST_PROGRAM: {
            ASTNode* current = ((ASTProgram*)node)->declarations;
            while (current) {
                if (ast_uses_builtin(current, predicate)) {
                    return 1;
                }
                current = current->next;
            }
            return 0;
        }
        case AST_VAR_DECL:
            return ast_uses_builtin(((ASTVarDecl*)node)->initializer, predicate);
        case AST_FN_DECL:
            return ast_uses_builtin(((ASTFnDecl*)node)->body, predicate);
        case AST_BLOCK: {
            ASTNode* current = ((ASTBlock*)node)->statements;
            while (current) {
                if (ast_uses_builtin(current, predicate)) {
                    return 1;
                }
                current = current->next;
            }
            return 0;
        }
        case AST_IF_STMT: {
            ASTIfStmt* if_stmt = (ASTIfStmt*)node;
            return ast_uses_builtin(if_stmt->condition, predicate) ||
                   ast_uses_builtin(if_stmt->then_branch, predicate) ||
                   ast_uses_builtin(if_stmt->else_branch, predicate);
        }
        case AST_WHILE_STMT: {
            ASTWhileStmt* while_stmt = (ASTWhileStmt*)node;
            return ast_uses_builtin(while_stmt->condition, predicate) ||
                   ast_uses_builtin(while_stmt->body, predicate);
        }
        case AST_FOR_STMT: {
            ASTForStmt* for_stmt = (ASTForStmt*)node;
            return ast_uses_builtin(for_stmt->initializer, predicate) ||
                   ast_uses_builtin(for_stmt->condition, predicate) ||
                   ast_uses_builtin(for_stmt->increment, predicate) ||
                   ast_uses_builtin(for_stmt->body, predicate);
        }
        case AST_RETURN_STMT:
            return ast_uses_builtin(((ASTReturnStmt*)node)->expression, predicate);
        case AST_ASSIGN_STMT:
            return ast_uses_builtin(((ASTAssignStmt*)node)->value, predicate);
        case AST_CALL_STMT: {
            ASTCallStmt* call_stmt = (ASTCallStmt*)node;
            if (predicate(call_stmt->name)) {
                return 1;
            }
            for (i = 0; i < call_stmt->arg_count; i++) {
                if (ast_uses_builtin(call_stmt->args[i], predicate)) {
                    return 1;
                }
            }
            return 0;
        }
        case AST_CALL_EXPR: {
            ASTCallExpr* call_expr = (ASTCallExpr*)node;
            if (predicate(call_expr->name)) {
                return 1;
            }
            for (i = 0; i < call_expr->arg_count; i++) {
                if (ast_uses_builtin(call_expr->args[i], predicate)) {
                    return 1;
                }
            }
            return 0;
        }
        case AST_BINARY_EXPR: {
            ASTBinaryExpr* expr = (ASTBinaryExpr*)node;
            return ast_uses_builtin(expr->left, predicate) ||
                   ast_uses_builtin(expr->right, predicate);
        }
        case AST_UNARY_EXPR:
            return ast_uses_builtin(((ASTUnaryExpr*)node)->right, predicate);
        case AST_GROUPING_EXPR:
            return ast_uses_builtin(((ASTGroupingExpr*)node)->expression, predicate);
        case AST_ARRAY_LITERAL: {
            /* Sprint 3: array literal — walk each element expression. */
            ASTArrayLiteral* arr = (ASTArrayLiteral*)node;
            int i;
            for (i = 0; i < arr->element_count; i++) {
                if (ast_uses_builtin(arr->elements[i], predicate)) {
                    return 1;
                }
            }
            return 0;
        }
        case AST_INDEX_EXPR: {
            ASTIndexExpr* idx = (ASTIndexExpr*)node;
            return ast_uses_builtin(idx->array, predicate) ||
                   ast_uses_builtin(idx->index, predicate);
        }
        case AST_PROP_EXPR:
            return ast_uses_builtin(((ASTPropExpr*)node)->object, predicate);
        case AST_INT_LITERAL:
        case AST_FLOAT_LITERAL:
        case AST_STRING_LITERAL:
        case AST_BOOL_LITERAL:
        case AST_IDENTIFIER:
        case AST_IMPORT:
            return 0;
        default:
            return 0;
    }
}

static int ast_uses_gui(ASTNode* node) {
    return ast_uses_builtin(node, lamo_builtin_is_gui);
}

static int ast_uses_http(ASTNode* node) {
    return ast_uses_builtin(node, lamo_builtin_is_http);
}

static int ast_uses_std(ASTNode* node) {
    return ast_uses_builtin(node, lamo_builtin_is_std);
}

/* ── Feature detection: string, array, float ops ─────────────────────────
 * These walk the AST and return 1 if the program uses the corresponding
 * feature. Used to emit fine-grained #define flags before the runtime so
 * GCC can dead-strip the unused sections even at -O0.
 *
 * "String ops" means: any string literal, any explicit string concatenation
 * (+ on strings), or a call to str()/input()/len().
 * "Array ops" means: any array literal, push/pop/append/array() calls.
 * "Float ops" means: any float literal, or float() cast. */

static int is_string_builtin(const char* name) {
    return strcmp(name, "str") == 0 ||
           strcmp(name, "input") == 0 ||
           strcmp(name, "len") == 0;
}
static int is_array_builtin(const char* name) {
    return strcmp(name, "push") == 0 ||
           strcmp(name, "pop") == 0 ||
           strcmp(name, "append") == 0 ||
           strcmp(name, "array") == 0;
}
static int is_float_builtin(const char* name) {
    return strcmp(name, "float") == 0 ||
           strcmp(name, "sqrt") == 0 ||
           strcmp(name, "pow") == 0 ||
           strcmp(name, "floor") == 0 ||
           strcmp(name, "ceil") == 0 ||
           strcmp(name, "abs") == 0;
}

/* Single combined walk that detects all three features in one pass.
 * Sets bits in *flags: bit 0 = strings, bit 1 = arrays, bit 2 = floats. */

static void ast_detect_features(ASTNode* node, int* flags) {
    int i;
    if (!node || *flags == (FEAT_STRINGS | FEAT_ARRAYS | FEAT_FLOATS)) return;

    switch (node->type) {
        case AST_STRING_LITERAL:
            *flags |= FEAT_STRINGS;
            return;
        case AST_FLOAT_LITERAL:
            *flags |= FEAT_FLOATS;
            return;
        case AST_ARRAY_LITERAL: {
            ASTArrayLiteral* arr = (ASTArrayLiteral*)node;
            *flags |= FEAT_ARRAYS;
            for (i = 0; i < arr->element_count; i++)
                ast_detect_features(arr->elements[i], flags);
            return;
        }
        case AST_PROP_EXPR:
            /* .len property implies string or array usage. */
            *flags |= FEAT_STRINGS;
            ast_detect_features(((ASTPropExpr*)node)->object, flags);
            return;
        case AST_INDEX_EXPR: {
            ASTIndexExpr* ie = (ASTIndexExpr*)node;
            /* Index on a string is a string op; on array is array op.
             * We can't know which without type info, so set both. */
            *flags |= FEAT_STRINGS | FEAT_ARRAYS;
            ast_detect_features(ie->array, flags);
            ast_detect_features(ie->index, flags);
            return;
        }
        case AST_CALL_STMT: {
            ASTCallStmt* cs = (ASTCallStmt*)node;
            if (is_string_builtin(cs->name)) *flags |= FEAT_STRINGS;
            if (is_array_builtin(cs->name))  *flags |= FEAT_ARRAYS;
            if (is_float_builtin(cs->name))  *flags |= FEAT_FLOATS;
            for (i = 0; i < cs->arg_count; i++)
                ast_detect_features(cs->args[i], flags);
            return;
        }
        case AST_CALL_EXPR: {
            ASTCallExpr* ce = (ASTCallExpr*)node;
            if (is_string_builtin(ce->name)) *flags |= FEAT_STRINGS;
            if (is_array_builtin(ce->name))  *flags |= FEAT_ARRAYS;
            if (is_float_builtin(ce->name))  *flags |= FEAT_FLOATS;
            for (i = 0; i < ce->arg_count; i++)
                ast_detect_features(ce->args[i], flags);
            return;
        }
        case AST_BINARY_EXPR: {
            ASTBinaryExpr* be = (ASTBinaryExpr*)node;
            /* % with floats at runtime calls lamo_mod which uses fmod — float op. */
            if (be->operator == TOKEN_PERCENT) *flags |= FEAT_FLOATS;
            ast_detect_features(be->left, flags);
            ast_detect_features(be->right, flags);
            return;
        }
        /* Recurse through all structural nodes. */
        case AST_PROGRAM: {
            for (ASTNode* c = ((ASTProgram*)node)->declarations; c; c = c->next)
                ast_detect_features(c, flags);
            return;
        }
        case AST_VAR_DECL:
            ast_detect_features(((ASTVarDecl*)node)->initializer, flags); return;
        case AST_FN_DECL:
            ast_detect_features(((ASTFnDecl*)node)->body, flags); return;
        case AST_BLOCK: {
            for (ASTNode* s = ((ASTBlock*)node)->statements; s; s = s->next)
                ast_detect_features(s, flags);
            return;
        }
        case AST_IF_STMT: {
            ASTIfStmt* is = (ASTIfStmt*)node;
            ast_detect_features(is->condition, flags);
            ast_detect_features(is->then_branch, flags);
            ast_detect_features(is->else_branch, flags);
            return;
        }
        case AST_WHILE_STMT: {
            ASTWhileStmt* ws = (ASTWhileStmt*)node;
            ast_detect_features(ws->condition, flags);
            ast_detect_features(ws->body, flags);
            return;
        }
        case AST_FOR_STMT: {
            ASTForStmt* fs = (ASTForStmt*)node;
            ast_detect_features(fs->initializer, flags);
            ast_detect_features(fs->condition, flags);
            ast_detect_features(fs->increment, flags);
            ast_detect_features(fs->body, flags);
            return;
        }
        case AST_RETURN_STMT:
            ast_detect_features(((ASTReturnStmt*)node)->expression, flags); return;
        case AST_ASSIGN_STMT:
            ast_detect_features(((ASTAssignStmt*)node)->value, flags); return;
        case AST_UNARY_EXPR:
            ast_detect_features(((ASTUnaryExpr*)node)->right, flags); return;
        case AST_GROUPING_EXPR:
            ast_detect_features(((ASTGroupingExpr*)node)->expression, flags); return;
        default:
            return;
    }
}

void generate_c_code(ASTNode* node, FILE* out) {
    ASTNode* current;
    int needs_gui_runtime;
    int needs_http_runtime;
    int needs_std_runtime;
    int feat_flags = 0;

    if (!node) {
        return;
    }

    /* Phase 2: store the program's declarations list so generate_prop_expr_code
     * can walk it to find struct definitions and look up field indices. */
    g_program_decls = ((ASTProgram*)node)->declarations;

    fprintf(out, "// Generated by Lamo v2 (via AST)\n");
    /* Phase 3 (stdlib): emit POSIX feature-test macros so the STD runtime
     * can use setenv, clock_gettime, nanosleep, popen, etc. without
     * -Wimplicit-function-declaration errors under -std=c99. These
     * macros must appear before any #include. */
    fprintf(out, "#define _DEFAULT_SOURCE 1\n");
    fprintf(out, "#define _POSIX_C_SOURCE 200809L\n");
    fprintf(out, "#define _BSD_SOURCE 1\n");
    fprintf(out, "#define _GNU_SOURCE 1\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <string.h>\n\n");
    needs_gui_runtime = ast_uses_gui(node);
    needs_http_runtime = ast_uses_http(node);
    needs_std_runtime = ast_uses_std(node);
    ast_detect_features(node, &feat_flags);
    emit_runtime(out, needs_gui_runtime, needs_http_runtime, needs_std_runtime, feat_flags);

    // 1. Forward declarations de funções definidas pelo usuário.
    //    Phase 2: also forward-declare methods (from impl blocks). Methods
    //    are stored on AST_IMPL_DECL nodes; their sema_struct_name field
    //    is set by the semantic pass. Methods take `self` as the first
    //    parameter (added implicitly by codegen) and are emitted with
    //    the mangled name `lamo_method_<Type>__<name>`.
    /* Helper macro-like: emit a forward declaration for one fn. */
    #define EMIT_FN_FORWARD(fn_node) do { \
        ASTFnDecl* fn_decl = (ASTFnDecl*)(fn_node); \
        int _i; \
        int _is_method = ((fn_node)->sema_struct_name != NULL); \
        char _mangled[256]; \
        const char* _emit_name; \
        if (_is_method) { \
            snprintf(_mangled, sizeof(_mangled), "lamo_method_%s__%s", (fn_node)->sema_struct_name, fn_decl->name); \
            _emit_name = _mangled; \
        } else { _emit_name = fn_decl->name; } \
        fprintf(out, "LamoValue %s(", user_name1(_emit_name)); \
        int _pstart = 0; \
        if (_is_method) { fprintf(out, "LamoValue %s", user_name1("self")); _pstart = 1; } \
        for (_i = 0; _i < fn_decl->param_count; _i++) { \
            if (_i > 0 || _pstart) fprintf(out, ", "); \
            fprintf(out, "LamoValue %s", user_name1(fn_decl->params[_i])); \
        } \
        if (fn_decl->param_count == 0 && !_is_method) fprintf(out, "void"); \
        fprintf(out, ");\n"); \
    } while (0)

    current = ((ASTProgram*)node)->declarations;
    while (current) {
        if (current->type == AST_FN_DECL) {
            EMIT_FN_FORWARD(current);
        } else if (current->type == AST_IMPL_DECL) {
            ASTImplDecl* id = (ASTImplDecl*)current;
            for (ASTNode* m = id->methods; m; m = m->next) {
                if (m->type == AST_FN_DECL) EMIT_FN_FORWARD(m);
            }
        }
        current = current->next;
    }
    #undef EMIT_FN_FORWARD
    fprintf(out, "\n");

    // 2. Declarações de variáveis globais no escopo de arquivo.
    //    Inicializadores não-constantes são emitidos dentro de main().
    //    Phase 2: also emit globals for enum variants. Each variant
    //    becomes a `static LamoValue` initialized to lamo_make_int(index).
    current = ((ASTProgram*)node)->declarations;
    while (current) {
        if (current->type == AST_VAR_DECL) {
            ASTVarDecl* var_decl = (ASTVarDecl*)current;
            fprintf(out, "static LamoValue %s;\n", user_name1(var_decl->name));
        }
        current = current->next;
    }
    /* Phase 2: emit enum variant globals. */
    char** variant_global_names = NULL;
    int variant_global_count = 0;
    int variant_global_cap = 0;
    current = ((ASTProgram*)node)->declarations;
    while (current) {
        if (current->type == AST_ENUM_DECL) {
            ASTEnumDecl* ed = (ASTEnumDecl*)current;
            for (int i = 0; i < ed->variant_count; i++) {
                /* 2.7.0 (FU4): cross-enum variant-name collisions are
                 * legal ("later wins"), so two enums may declare the
                 * same variant name. Declare each global ONCE — the
                 * initialization loop below still writes per enum in
                 * declaration order, so the LAST enum's value wins,
                 * matching the semantic pass's symbol retargeting. */
                const char* vn = user_name1(ed->variants[i]);
                int seen = 0;
                for (int d = 0; d < variant_global_count; d++) {
                    if (strcmp(variant_global_names[d], ed->variants[i]) == 0) { seen = 1; break; }
                }
                if (seen) continue;
                if (variant_global_count == variant_global_cap) {
                    int nc = variant_global_cap > 0 ? variant_global_cap * 2 : 32;
                    char** nn = realloc(variant_global_names, sizeof(char*) * (size_t)nc);
                    if (!nn) break;
                    variant_global_names = nn;
                    variant_global_cap = nc;
                }
                variant_global_names[variant_global_count] = strdup(ed->variants[i]);
                variant_global_count++;
                fprintf(out, "static LamoValue %s;\n", vn);
            }
        }
        current = current->next;
    }
    fprintf(out, "\n");

    // 3. Corpos das funções definidas pelo usuário.
    //    Phase 2: also emit method bodies. Methods are inside AST_IMPL_DECL
    //    nodes; we walk the methods list and emit each as a regular
    //    function (the method's name was mangled by semantic).
    current = ((ASTProgram*)node)->declarations;
    while (current) {
        if (current->type == AST_FN_DECL) {
            generate_statement_code(current, out);
            fprintf(out, "\n");
        } else if (current->type == AST_IMPL_DECL) {
            ASTImplDecl* id = (ASTImplDecl*)current;
            for (ASTNode* m = id->methods; m; m = m->next) {
                if (m->type == AST_FN_DECL) {
                    generate_statement_code(m, out);
                    fprintf(out, "\n");
                }
            }
        }
        current = current->next;
    }

    // 4. main(): executa os statements top-level. Variáveis globais (let x = ...)
    //    viram assignments para as globais declaradas acima, e outras funções
    //    chamadas pelo nome podem referenciar essas globais porque estão em
    //    escopo de arquivo.
    fprintf(out, "int main(void) {\n");
    indent_level++;

    // Registra limpeza da arena de strings no encerramento do programa.
    print_indent(out);
    fprintf(out, "atexit(lamo_arena_free_all);\n");

    /* Phase 2: initialize enum variant globals. Each variant gets its
     * integer index as the value.
     * 2.6.0: variants of TAGGED-union enums materialize as tagged values
     * (lamo_make_enum) instead of plain ints — unit variants carry no
     * payloads, payload variants are only produced by constructor calls
     * and their globals exist solely so bare unit-variant identifiers
     * (e.g. `None`) resolve. */
    current = ((ASTProgram*)node)->declarations;
    while (current) {
        if (current->type == AST_ENUM_DECL) {
            ASTEnumDecl* ed = (ASTEnumDecl*)current;
            int tagged = 0;
            for (int v = 0; v < ed->variant_count; v++) {
                if (ed->variant_payload_counts && ed->variant_payload_counts[v] > 0) { tagged = 1; break; }
            }
            for (int i = 0; i < ed->variant_count; i++) {
                print_indent(out);
                if (tagged) {
                    fprintf(out, "%s = lamo_make_enum(%d, \"%s\", (LamoArray*)0);\n",
                            user_name1(ed->variants[i]), i, ed->variants[i]);
                } else {
                    fprintf(out, "%s = lamo_make_int(%d);\n", user_name1(ed->variants[i]), i);
                }
            }
        }
        current = current->next;
    }

    /* GC Step 3: open main()'s top-level scope and register every global
     * LamoValue as a root. Globals persist for the program's lifetime,
     * so they stay on the root stack until main() returns. Enum variant
     * globals hold only ints (lamo_make_int(i)) and don't need to be
     * roots — they never reference GC-tracked allocations. */
    lamo_gc_scope_reset();
    lamo_gc_scope_enter();
    current = ((ASTProgram*)node)->declarations;
    while (current) {
        if (current->type == AST_VAR_DECL) {
            ASTVarDecl* var_decl = (ASTVarDecl*)current;
            print_indent(out);
            fprintf(out, "LAMO_GC_PUSH_ROOT(&%s);\n", user_name1(var_decl->name));
            lamo_gc_scope_push_root();
        }
        current = current->next;
    }

    current = ((ASTProgram*)node)->declarations;
    while (current) {
        if (current->type == AST_VAR_DECL) {
            // Inicializa a global correspondente no início do main.
            ASTVarDecl* var_decl = (ASTVarDecl*)current;
            print_indent(out);
            fprintf(out, "%s = ", user_name1(var_decl->name));
            generate_expression_code(var_decl->initializer, out);
            fprintf(out, ";\n");
        } else if (current->type != AST_FN_DECL && current->type != AST_IMPORT &&
                   current->type != AST_STRUCT_DECL && current->type != AST_IMPL_DECL &&
                   current->type != AST_ENUM_DECL) {
            generate_statement_code(current, out);
        }
        current = current->next;
    }

    /* 2.6.0 (FU5): SPEC §12.1 — "Calls user fn main() if defined".
     * The entry file's zero-argument fn main() runs AFTER top-level
     * initialization and statements. Before this fix the generated
     * lamo_u_main was emitted but never invoked, so `lamo run` on the
     * README's hello world printed nothing. main() defined in an
     * IMPORTED file is never called (it is renamed for aliased imports;
     * the loader warns for unaliased merges). Suppressed when the
     * program already calls main() explicitly at the top level
     * (pre-2.6 workaround — see program_has_explicit_main_call). */
    if (find_entry_main(((ASTProgram*)node)->declarations) &&
        !program_has_explicit_main_call(((ASTProgram*)node)->declarations)) {
        print_indent(out);
        fprintf(out, "lamo_u_main();\n");
    }

    /* GC Step 3: pop all roots (globals + any nested locals from top-level
     * if/while/for blocks) before main() returns. */
    {
        int gc_total = lamo_gc_scope_total_roots();
        if (gc_total > 0) {
            print_indent(out);
            fprintf(out, "LAMO_GC_POP_ROOTS_N(%d);\n", gc_total);
        }
        lamo_gc_scope_exit();
    }

    indent_level--;
    fprintf(out, "    return 0;\n}\n");
}

/* Phase 2: generate code for a member call (`obj.method(args)`).
 * Dispatches based on the call kind:
 *   - Module call: object is an identifier matching a registered module alias.
 *     Emit `lamo_mod_<alias>__<member>(args)`.
 *   - Array method: member is push/pop/len. Emit the corresponding runtime call.
 *   - Struct method: object has sema_struct_name set. Emit
 *     `lamo_method_<Type>__<method>(obj, args)` (self is the first arg).
 * Used by both statement and expression positions. */
static void generate_member_call_code(ASTMemberCall* mc, FILE* out) {
    /* Try module call first. */
    if (g_module_registry && mc->object && mc->object->type == AST_IDENTIFIER) {
        const char* alias = ((ASTIdentifier*)mc->object)->name;
        const char* prefixed = lamo_modules_resolve_member(g_module_registry, alias, mc->member_name);
        if (prefixed) {
            fprintf(out, "%s(", user_name1(prefixed));
            generate_call_arguments(mc->args, mc->arg_count, out);
            fprintf(out, ")");
            return;
        }
    }
    /* Try struct method call. The semantic pass annotated the node (or the
     * object identifier) with the struct type.
     *
     * Generics PR 2 correction: when semantic analysis resolved this as an
     * ARRAY-BUILTIN method (it stamps sema_full_type = "array" on the
     * node), the object tag must be IGNORED — `self.items.push(x)` inside
     * `impl<T> Box<T>` carries a Box tag on the field chain but resolves
     * to lamo_array_push, not a Box method. */
    int sema_says_array = (((ASTNode*)mc)->sema_full_type != NULL &&
                           strcmp(((ASTNode*)mc)->sema_full_type, "array") == 0);
    const char* struct_name = mc->object ? mc->object->sema_struct_name : NULL;
    if (!struct_name) struct_name = ((ASTNode*)mc)->sema_struct_name;
    if (sema_says_array) struct_name = NULL;
    if (struct_name) {
        /* Emit `lamo_method_<Type>__<method>(self, args)`. The method's
         * mangled name was set by the semantic pass on the AST_FN_DECL,
         * but we don't have the AST_FN_DECL here — we reconstruct the
         * mangled name from struct_name + member_name. */
        char mangled[256];
        snprintf(mangled, sizeof(mangled), "lamo_method_%s__%s", struct_name, mc->member_name);
        fprintf(out, "%s(", user_name1(mangled));
        /* First arg is self (the object). */
        generate_expression_code(mc->object, out);
        for (int i = 0; i < mc->arg_count; i++) {
            fprintf(out, ", ");
            generate_expression_code(mc->args[i], out);
        }
        fprintf(out, ")");
        return;
    }
    /* Try array method call. */
    if (strcmp(mc->member_name, "push") == 0) {
        fprintf(out, "lamo_array_push(");
        generate_expression_code(mc->object, out);
        fprintf(out, ", ");
        generate_call_arguments(mc->args, mc->arg_count, out);
        fprintf(out, ")");
        return;
    }
    if (strcmp(mc->member_name, "pop") == 0) {
        fprintf(out, "lamo_array_pop(");
        generate_expression_code(mc->object, out);
        fprintf(out, ")");
        return;
    }
    if (strcmp(mc->member_name, "len") == 0) {
        fprintf(out, "lamo_array_len(");
        generate_expression_code(mc->object, out);
        fprintf(out, ")");
        return;
    }
    /* Defensive fallback. */
    fprintf(out, "lamo_make_int(0)");
}

/* Phase 2: generate code for a property access (`obj.prop`).
 * Dispatches based on the property name and the object's type:
 *   - If prop is "len" and object is not struct-annotated, emit lamo_array_len.
 *   - If the object is struct-annotated, look up the field index and emit
 *     lamo_struct_get(obj, field_index).
 * Used by both statement and expression positions. The g_program_decls
 * global (set by generate_c_code) is walked to find the struct definition. */
static void generate_prop_expr_code(ASTPropExpr* pe, FILE* out) {
    /* Phase 3 (stdlib): module variable access (e.g. `math.PI`).
     * The semantic pass marked these by storing the alias name on
     * sema_struct_name. We resolve the alias+prop through the module
     * registry to get the prefixed global name. */
    if (g_module_registry && pe->object && pe->object->type == AST_IDENTIFIER) {
        const char* alias = ((ASTIdentifier*)pe->object)->name;
        const char* prefixed = lamo_modules_resolve_member(g_module_registry, alias, pe->prop_name);
        if (prefixed) {
            /* The loader renamed the imported file's top-level variable
             * declarations to `lamo_mod_<alias>__<name>` (matching the
             * function renaming). user_name1() adds the user_ prefix
             * consistent with regular global variable references. */
            fprintf(out, "%s", user_name1(prefixed));
            return;
        }
    }
    /* Struct field access? */
    const char* struct_name = pe->object ? pe->object->sema_struct_name : NULL;
    if (!struct_name) struct_name = ((ASTNode*)pe)->sema_struct_name;
    if (struct_name) {
        /* Find the struct definition and look up the field index. */
        int field_index = -1;
        for (ASTNode* cur = g_program_decls; cur; cur = cur->next) {
            if (cur->type == AST_STRUCT_DECL) {
                ASTStructDecl* sd = (ASTStructDecl*)cur;
                if (sd->name && strcmp(sd->name, struct_name) == 0) {
                    for (int i = 0; i < sd->field_count; i++) {
                        if (sd->field_names[i] && strcmp(sd->field_names[i], pe->prop_name) == 0) {
                            field_index = i;
                            break;
                        }
                    }
                    break;
                }
            }
        }
        if (field_index >= 0) {
            fprintf(out, "lamo_struct_get(");
            generate_expression_code(pe->object, out);
            fprintf(out, ", %d)", field_index);
            return;
        }
        /* Field not found — semantic should have caught this. */
        fprintf(out, "lamo_make_int(0)");
        return;
    }
    /* Array .len property. */
    if (strcmp(pe->prop_name, "len") == 0) {
        fprintf(out, "lamo_array_len(");
        generate_expression_code(pe->object, out);
        fprintf(out, ")");
        return;
    }
    /* Defensive fallback. */
    fprintf(out, "lamo_make_int(0)");
}

static void generate_assignment_code(const char* name, ASTNode* value, LamoTokenType op_type, FILE* out) {
    // Bug #3 fix: each user_name1() call uses a different slot in a 4-entry
    // ring buffer, so multiple calls in the same statement (e.g. the += case
    // below, which references `name` twice) cannot clobber each other.
    fprintf(out, "%s = ", user_name1(name));
    if (op_type == TOKEN_PLUS_EQ) {
        fprintf(out, "lamo_add(%s, ", user_name1(name));
        generate_expression_code(value, out);
        fprintf(out, ")");
    } else if (op_type == TOKEN_MINUS_EQ) {
        fprintf(out, "lamo_sub(%s, ", user_name1(name));
        generate_expression_code(value, out);
        fprintf(out, ")");
    } else {
        generate_expression_code(value, out);
    }
}

static void generate_statement_code(ASTNode* node, FILE* out) {
    if (!node) {
        return;
    }

    if (node->type != AST_BLOCK) {
        print_indent(out);
    }

    switch (node->type) {
        case AST_VAR_DECL: {
            // Variáveis locais dentro de blocos (não top-level). As top-level
            // são tratadas diretamente em generate_c_code como globais.
            ASTVarDecl* var_decl = (ASTVarDecl*)node;
            fprintf(out, "LamoValue %s = ", user_name1(var_decl->name));
            generate_expression_code(var_decl->initializer, out);
            fprintf(out, ";\n");
            /* GC Step 3: register the local as a root so a gc_collect()
             * inside any called function doesn't free values still
             * referenced by this local. The matching pop happens at
             * scope exit (AST_BLOCK close or function return). */
            print_indent(out);
            fprintf(out, "LAMO_GC_PUSH_ROOT(&%s);\n", user_name1(var_decl->name));
            lamo_gc_scope_push_root();
            break;
        }
        case AST_FN_DECL: {
            ASTFnDecl* fn_decl = (ASTFnDecl*)node;
            int i;
            int is_method = (node->sema_struct_name != NULL);
            char mangled[256];
            const char* emit_name;
            if (is_method) {
                snprintf(mangled, sizeof(mangled), "lamo_method_%s__%s", node->sema_struct_name, fn_decl->name);
                emit_name = mangled;
            } else {
                emit_name = fn_decl->name;
            }
            fprintf(out, "LamoValue %s(", user_name1(emit_name));
            int param_start = 0;
            if (is_method) {
                fprintf(out, "LamoValue %s", user_name1("self"));
                param_start = 1;
            }
            for (i = 0; i < fn_decl->param_count; i++) {
                if (i > 0 || param_start) {
                    fprintf(out, ", ");
                }
                fprintf(out, "LamoValue %s", user_name1(fn_decl->params[i]));
            }
            if (fn_decl->param_count == 0 && !is_method) {
                fprintf(out, "void");
            }
            fprintf(out, ") ");
            /* Phase 2: emit the body with an implicit `return lamo_make_int(0);`
             * at the end so the C compiler doesn't warn about control
             * reaching the end of a non-void function. We unwrap the
             * body block (which is always AST_BLOCK for functions) so we
             * can append the return inside the function's braces.
             *
             * GC Step 3: at function entry, open a root scope and push
             * every parameter (and `self` for methods) as a root. At
             * function exit (the implicit return below, plus any user
             * `return` statements via AST_RETURN_STMT), pop all roots
             * pushed in this function. Locals declared inside the body
             * get pushed by AST_VAR_DECL above and counted in the same
             * scope; inner blocks (AST_BLOCK) get their own sub-scope
             * so their locals are popped at block exit. */
            lamo_gc_scope_reset();
            lamo_gc_scope_enter();
            if (fn_decl->body && fn_decl->body->type == AST_BLOCK) {
                ASTBlock* block = (ASTBlock*)fn_decl->body;
                fprintf(out, "{\n");
                indent_level++;
                /* Push params (and self) as roots. */
                if (is_method) {
                    print_indent(out);
                    fprintf(out, "LAMO_GC_PUSH_ROOT(&%s);\n", user_name1("self"));
                    lamo_gc_scope_push_root();
                }
                for (i = 0; i < fn_decl->param_count; i++) {
                    print_indent(out);
                    fprintf(out, "LAMO_GC_PUSH_ROOT(&%s);\n", user_name1(fn_decl->params[i]));
                    lamo_gc_scope_push_root();
                }
                for (ASTNode* s = block->statements; s; s = s->next) {
                    generate_statement_code(s, out);
                }
                /* GC: pop all roots before the implicit return. */
                {
                    int gc_total = lamo_gc_scope_total_roots();
                    if (gc_total > 0) {
                        print_indent(out);
                        fprintf(out, "LAMO_GC_POP_ROOTS_N(%d);\n", gc_total);
                    }
                }
                print_indent(out);
                fprintf(out, "return lamo_make_int(0);\n");
                indent_level--;
                print_indent(out);
                fprintf(out, "}\n");
            } else {
                /* Defensive: body is not a block (shouldn't happen). */
                fprintf(out, "{\n");
                indent_level++;
                if (fn_decl->body) generate_statement_code(fn_decl->body, out);
                {
                    int gc_total = lamo_gc_scope_total_roots();
                    if (gc_total > 0) {
                        print_indent(out);
                        fprintf(out, "LAMO_GC_POP_ROOTS_N(%d);\n", gc_total);
                    }
                }
                print_indent(out);
                fprintf(out, "return lamo_make_int(0);\n");
                indent_level--;
                print_indent(out);
                fprintf(out, "}\n");
            }
            lamo_gc_scope_exit();
            break;
        }
        case AST_BLOCK: {
            ASTBlock* block = (ASTBlock*)node;
            ASTNode* current = block->statements;
            fprintf(out, "{\n");
            indent_level++;
            /* GC Step 3: open a new root scope for this block. Locals
             * declared inside (via AST_VAR_DECL) are pushed and counted
             * here; they're popped when the block closes. */
            lamo_gc_scope_enter();
            while (current) {
                generate_statement_code(current, out);
                current = current->next;
            }
            /* GC: pop roots pushed for locals declared in this block. */
            {
                int gc_n = lamo_gc_scope_exit();
                if (gc_n > 0) {
                    print_indent(out);
                    fprintf(out, "LAMO_GC_POP_ROOTS_N(%d);\n", gc_n);
                }
            }
            indent_level--;
            print_indent(out);
            fprintf(out, "}\n");
            break;
        }
        case AST_IF_STMT: {
            ASTIfStmt* if_stmt = (ASTIfStmt*)node;
            fprintf(out, "if (lamo_is_truthy(");
            generate_expression_code(if_stmt->condition, out);
            fprintf(out, ")) ");
            generate_statement_code(if_stmt->then_branch, out);
            if (if_stmt->else_branch) {
                print_indent(out);
                fprintf(out, "else ");
                generate_statement_code(if_stmt->else_branch, out);
            }
            break;
        }
        case AST_WHILE_STMT: {
            ASTWhileStmt* while_stmt = (ASTWhileStmt*)node;
            fprintf(out, "while (lamo_is_truthy(");
            generate_expression_code(while_stmt->condition, out);
            fprintf(out, ")) ");
            generate_statement_code(while_stmt->body, out);
            break;
        }
        case AST_FOR_STMT: {
            /* GC Step 3: for-loops with a `let` initializer declare a
             * LamoValue that must be a root for the duration of the body.
             * We wrap the body in an explicit block (always — even if the
             * AST has a single-statement body) so we can push/pop the
             * loop variable as a root inside the block scope. */
            ASTForStmt* for_stmt = (ASTForStmt*)node;
            int has_let_init = (for_stmt->initializer &&
                                for_stmt->initializer->type == AST_VAR_DECL);
            const char* loop_var_name = has_let_init
                ? ((ASTVarDecl*)for_stmt->initializer)->name
                : NULL;
            fprintf(out, "for (");
            if (for_stmt->initializer) {
                if (has_let_init) {
                    ASTVarDecl* var_decl = (ASTVarDecl*)for_stmt->initializer;
                    fprintf(out, "LamoValue %s = ", user_name1(var_decl->name));
                    generate_expression_code(var_decl->initializer, out);
                } else if (for_stmt->initializer->type == AST_ASSIGN_STMT) {
                    ASTAssignStmt* assign_stmt = (ASTAssignStmt*)for_stmt->initializer;
                    generate_assignment_code(assign_stmt->name, assign_stmt->value, assign_stmt->op_type, out);
                }
            }
            fprintf(out, "; ");
            if (for_stmt->condition) {
                fprintf(out, "lamo_is_truthy(");
                generate_expression_code(for_stmt->condition, out);
                fprintf(out, ")");
            }
            fprintf(out, "; ");
            if (for_stmt->increment) {
                ASTAssignStmt* assign_stmt = (ASTAssignStmt*)for_stmt->increment;
                generate_assignment_code(assign_stmt->name, assign_stmt->value, assign_stmt->op_type, out);
            }
            /* Open the body block explicitly. This wraps even
             * single-statement bodies in braces, which is a minor
             * codegen change but lets us push the loop variable as a
             * root inside the block scope. */
            fprintf(out, ") {\n");
            indent_level++;
            lamo_gc_scope_enter();
            if (has_let_init && loop_var_name) {
                print_indent(out);
                fprintf(out, "LAMO_GC_PUSH_ROOT(&%s);\n", user_name1(loop_var_name));
                lamo_gc_scope_push_root();
            }
            /* Emit the body. If it's an AST_BLOCK, iterate its statements
             * directly (we've already opened the block above); otherwise
             * emit the single statement. */
            if (for_stmt->body) {
                if (for_stmt->body->type == AST_BLOCK) {
                    for (ASTNode* s = ((ASTBlock*)for_stmt->body)->statements; s; s = s->next) {
                        generate_statement_code(s, out);
                    }
                } else {
                    generate_statement_code(for_stmt->body, out);
                }
            }
            /* GC: pop roots pushed in this iteration's block scope (the
             * loop variable plus any locals declared in the body). */
            {
                int gc_n = lamo_gc_scope_exit();
                if (gc_n > 0) {
                    print_indent(out);
                    fprintf(out, "LAMO_GC_POP_ROOTS_N(%d);\n", gc_n);
                }
            }
            indent_level--;
            print_indent(out);
            fprintf(out, "}\n");
            break;
        }
        case AST_RETURN_STMT: {
            ASTReturnStmt* return_stmt = (ASTReturnStmt*)node;
            /* GC Step 3: pop all roots pushed in the current function
             * (params + locals + any nested-block locals still live at
             * this return point) before returning. The pop count is the
             * sum of all active scopes — see lamo_gc_scope_total_roots().
             *
             * We wrap pop+return in a block `{ ... }` so the pair acts
             * as a single statement when used as the body of an if/else
             * or match arm. Without the braces, `if (cond) POP; return X;`
             * would parse as `if (cond) POP;` then `return X;` — the
             * `return` would be unconditional and the subsequent `else`
             * would be a syntax error. */
            int gc_total = lamo_gc_scope_total_roots();
            if (gc_total > 0) {
                fprintf(out, "{ LAMO_GC_POP_ROOTS_N(%d); ", gc_total);
            }
            if (return_stmt->expression) {
                fprintf(out, "return ");
                generate_expression_code(return_stmt->expression, out);
                fprintf(out, ";");
            } else {
                fprintf(out, "return lamo_make_int(0);");
            }
            if (gc_total > 0) {
                fprintf(out, " }\n");
            } else {
                fprintf(out, "\n");
            }
            break;
        }
        case AST_BREAK_STMT:
            // break/continue em Lamo mapeiam 1:1 para break/continue em C, mas
            // só são válidos dentro de while/for (checado pelo semântico).
            fprintf(out, "break;\n");
            break;
        case AST_CONTINUE_STMT:
            fprintf(out, "continue;\n");
            break;
        case AST_ASSIGN_STMT: {
            ASTAssignStmt* assign_stmt = (ASTAssignStmt*)node;
            generate_assignment_code(assign_stmt->name, assign_stmt->value, assign_stmt->op_type, out);
            fprintf(out, ";\n");
            break;
        }
        case AST_CALL_STMT: {
            ASTCallStmt* call_stmt = (ASTCallStmt*)node;
            /* 2.7.0 (FU4): enum-variant constructor statement —
             * `Some(5);` / `Enum::Variant(5);`. The semantic pass now
             * annotates statement-position calls, so emit the same
             * statement-expression the expression path uses (previously
             * this emitted a bogus call to the variant global and broke
             * the GCC backend). */
            if (call_stmt->base.sema_enum_name) {
                int payload_count = call_stmt->arg_count;
                fprintf(out, "({ LamoArray* _lamo_enum_pl = lamo_enum_payloads_alloc(%d); ",
                        payload_count > 0 ? payload_count : 0);
                for (int pi = 0; pi < payload_count; pi++) {
                    fprintf(out, "_lamo_enum_pl->items[%d] = ", pi);
                    generate_expression_code(call_stmt->args[pi], out);
                    fprintf(out, "; ");
                }
                fprintf(out, "lamo_make_enum(%d, \"%s\", _lamo_enum_pl); })",
                        call_stmt->base.sema_variant_index,
                        lamo_variant_short_name(call_stmt->name));
                fprintf(out, ";\n");
                break;
            }
            if (is_lang_builtin(call_stmt->name)) {
                generate_lang_builtin_call_expr(call_stmt->name, call_stmt->args, call_stmt->arg_count, out);
            } else if (is_gui_builtin(call_stmt->name)) {
                generate_gui_call_expr(call_stmt->name, call_stmt->args, call_stmt->arg_count, out);
            } else if (is_http_builtin(call_stmt->name)) {
                generate_http_call_expr(call_stmt->name, call_stmt->args, call_stmt->arg_count, out);
            } else if (is_std_builtin(call_stmt->name)) {
                generate_std_builtin_call_expr(call_stmt->name, call_stmt->args, call_stmt->arg_count, out);
            } else {
                fprintf(out, "%s(", user_name1(call_stmt->name));
                generate_call_arguments(call_stmt->args, call_stmt->arg_count, out);
                fprintf(out, ")");
            }
            fprintf(out, ";\n");
            break;
        }
        case AST_MEMBER_CALL: {
            /* Sprint 4 + Phase 2: `module.member(args);`, `arr.push(x);`,
             * or `obj.method(args);`. The dispatch is centralized in
             * generate_member_call_code. */
            ASTMemberCall* mc = (ASTMemberCall*)node;
            generate_member_call_code(mc, out);
            fprintf(out, ";\n");
            break;
        }
        case AST_IMPORT:
            // import é resolvido antes do codegen; não emite nada aqui.
            break;
        /* ─── Phase 2: struct / impl / enum / match / place-assign ────── */
        case AST_STRUCT_DECL:
            /* No code to emit — struct types exist only at compile time.
             * The runtime representation is a LamoArray (one slot per
             * field), allocated by lamo_struct_alloc in AST_STRUCT_LITERAL. */
            break;
        case AST_IMPL_DECL:
            /* Methods are emitted in step 3 of generate_c_code (the
             * function-bodies pass). Nothing to do here when the impl
             * block appears in statement position (which only happens
             * at top level, where generate_c_code already handles it). */
            break;
        case AST_ENUM_DECL:
            /* Enum variant globals are emitted in step 2 of generate_c_code.
             * Nothing to do here. */
            break;
        case AST_MATCH_STMT: {
            /* Phase 2: match desugars to a chain of guarded blocks.
             *
             * 2.6.0: tagged unions (sema_enum_name set) use tag compares
             * against a single-evaluation scrutinee temp.
             *
             * 2.7.0 (FU2/FU4): arms are (pattern tree, guard, body)
             * triples. Nested payload patterns (`Some(Pair(a, b))`) need
             * a MID-ARM failure to fall through to LATER arms, which an
             * else-if chain cannot express — so each arm is emitted as
             * an `if (!_lamo_match_done) { ... }` block with a shared
             * done-flag: nested tag checks just skip to the next arm.
             * `when` guards gate the body with lamo_is_truthy. Binding
             * leaves are pulled positionally per level with
             * lamo_enum_payload; wildcards pull nothing. */
            ASTMatchStmt* ms = (ASTMatchStmt*)node;
            if (ms->sema_enum_name) {
                ASTEnumDecl* ed = NULL;
                for (ASTNode* cur = g_program_decls; cur; cur = cur->next) {
                    if (cur->type == AST_ENUM_DECL) {
                        ASTEnumDecl* cand = (ASTEnumDecl*)cur;
                        if (cand->name && strcmp(cand->name, ms->sema_enum_name) == 0) { ed = cand; break; }
                    }
                }
                if (ed) {
                    print_indent(out);
                    fprintf(out, "{\n");
                    indent_level++;
                    print_indent(out);
                    fprintf(out, "LamoValue _lamo_match_scrut = ");
                    generate_expression_code(ms->scrutinee, out);
                    fprintf(out, ";\n");
                    print_indent(out);
                    fprintf(out, "int _lamo_match_done = 0;\n");
                    /* Unique temp counter for nested payload pulls. */
                    int pat_temp_id = 0;
                    for (int i = 0; i < ms->arm_count; i++) {
                        LamoPattern* pat = ms->patterns[i];
                        ASTNode* guard = ms->guards[i];
                        if (!pat) continue;
                        print_indent(out);
                        fprintf(out, "if (!_lamo_match_done) {\n");
                        indent_level++;
                        if (pat->kind == LAMO_PATTERN_WILDCARD) {
                            /* Catch-all arm; a guard on `_` still gates it. */
                            print_indent(out);
                            if (guard) {
                                fprintf(out, "if (lamo_is_truthy(");
                                generate_expression_code(guard, out);
                                fprintf(out, ")) {\n");
                                indent_level++;
                                if (ms->bodies[i]) generate_statement_code(ms->bodies[i], out);
                                print_indent(out);
                                fprintf(out, "_lamo_match_done = 1;\n");
                                indent_level--;
                                print_indent(out);
                                fprintf(out, "}\n");
                            } else {
                                fprintf(out, "_lamo_match_done = 1;\n");
                                if (ms->bodies[i]) generate_statement_code(ms->bodies[i], out);
                            }
                        } else {
                            /* Constructor arm. Recursive emission over the
                             * pattern tree: value_expr owns the current
                             * C expression (caller frees). */
                            char cur_expr[64];
                            snprintf(cur_expr, sizeof(cur_expr), "_lamo_match_scrut");
                            /* Depth-first walk emitting tag checks and
                             * payload binding lines. We emit the checks
                             * as nested `if (...) {` blocks and close
                             * them after the body. */
                            print_indent(out);
                            fprintf(out, "if (lamo_enum_tag_is(%s, %d)) {\n",
                                    cur_expr, pat->sema_variant_index);
                            indent_level++;
                            /* Emit nested extraction recursively. */
                            /* Stack of open `if` blocks to close later. */
                            int open_blocks = 1;  /* the tag-if above */
                            /* Worklist via recursion would re-emit; use an
                             * explicit recursion helper below. */
                            /* ---- helper-emitted payload chain ---- */
                            /* We recurse manually with a small stack of
                             * (pattern, expr-name, payload-index). */
                            LamoPattern* stack_pat[64];
                            char stack_expr[64][64];
                            int stack_idx[64];
                            int sp = 0;
                            /* Push children of the root ctor. */
                            for (int c = pat->child_count - 1; c >= 0; c--) {
                                stack_pat[sp] = pat->children[c];
                                snprintf(stack_expr[sp], sizeof(stack_expr[sp]), "%s", cur_expr);
                                stack_idx[sp] = c;
                                sp++;
                            }
                            while (sp > 0) {
                                sp--;
                                LamoPattern* cp = stack_pat[sp];
                                int cidx = stack_idx[sp];
                                const char* pexpr = stack_expr[sp];
                                if (!cp) continue;
                                if (cp->kind == LAMO_PATTERN_BINDING) {
                                    print_indent(out);
                                    fprintf(out, "LamoValue %s = lamo_enum_payload(%s, %d);\n",
                                            user_name1(cp->name), pexpr, cidx);
                                    print_indent(out);
                                    fprintf(out, "(void)%s;\n", user_name1(cp->name));
                                } else if (cp->kind == LAMO_PATTERN_CTOR) {
                                    char tmp[48];
                                    snprintf(tmp, sizeof(tmp), "_lamo_pat_%d", pat_temp_id++);
                                    print_indent(out);
                                    fprintf(out, "LamoValue %s = lamo_enum_payload(%s, %d);\n",
                                            tmp, pexpr, cidx);
                                    print_indent(out);
                                    fprintf(out, "if (lamo_enum_tag_is(%s, %d)) {\n",
                                            tmp, cp->sema_variant_index);
                                    indent_level++;
                                    open_blocks++;
                                    for (int c = cp->child_count - 1; c >= 0; c--) {
                                        stack_pat[sp] = cp->children[c];
                                        snprintf(stack_expr[sp], sizeof(stack_expr[sp]), "%s", tmp);
                                        stack_idx[sp] = c;
                                        sp++;
                                    }
                                }
                                /* wildcards bind nothing and check nothing */
                            }
                            /* Guard + body inside the innermost block. */
                            print_indent(out);
                            if (guard) {
                                fprintf(out, "if (lamo_is_truthy(");
                                generate_expression_code(guard, out);
                                fprintf(out, ")) {\n");
                                indent_level++;
                                open_blocks++;
                            }
                            if (ms->bodies[i]) generate_statement_code(ms->bodies[i], out);
                            print_indent(out);
                            fprintf(out, "_lamo_match_done = 1;\n");
                            /* Close all open blocks for this arm. */
                            while (open_blocks > 0) {
                                indent_level--;
                                print_indent(out);
                                fprintf(out, "}\n");
                                open_blocks--;
                            }
                        }
                        indent_level--;
                        print_indent(out);
                        fprintf(out, "}\n");
                    }
                    indent_level--;
                    print_indent(out);
                    fprintf(out, "}\n");
                    break;
                }
                /* Enum decl not found (shouldn't happen): fall through to
                 * the legacy desugar below. */
            }
            {
            /* Legacy untagged-enum path (plain int constants). Guards
             * chain with && so a failed guard falls through to the next
             * arm (the else-if chain gives exactly that). */
            int has_emitted = 0;
            for (int i = 0; i < ms->arm_count; i++) {
                LamoPattern* pat = ms->patterns[i];
                if (!pat) continue;
                if (pat->kind == LAMO_PATTERN_WILDCARD) {
                    /* Trailing else (guard on `_` folds into the else-if). */
                    print_indent(out);
                    if (ms->guards[i]) {
                        if (has_emitted) fprintf(out, "else ");
                        fprintf(out, "if (lamo_is_truthy(");
                        generate_expression_code(ms->guards[i], out);
                        fprintf(out, ")) ");
                    } else {
                        fprintf(out, "else ");
                    }
                    if (ms->bodies[i]) {
                        generate_statement_code(ms->bodies[i], out);
                    } else {
                        fprintf(out, "{ }\n");
                    }
                    has_emitted = 1;
                } else {
                    /* `if (scrut == pattern [&& guard]) body` (or `else if`).
                     * 2.7.0 (FU4): compare against the variant's INDEX
                     * (patterns carry sema_variant_index) instead of the
                     * bare-variant global — with legal cross-enum name
                     * collisions the global may hold a DIFFERENT enum's
                     * variant value, and qualified patterns
                     * (`First::Item`) must compare against First's index
                     * specifically. Falls back to the global for
                     * unstamped patterns (legacy defensive path). */
                    print_indent(out);
                    if (has_emitted) fprintf(out, "else ");
                    fprintf(out, "if (lamo_is_truthy(lamo_equal(");
                    generate_expression_code(ms->scrutinee, out);
                    if (pat->sema_variant_index >= 0) {
                        fprintf(out, ", lamo_make_int(%d))))", pat->sema_variant_index);
                    } else {
                        fprintf(out, ", %s)))", user_name1(lamo_variant_short_name(pat->name)));
                    }
                    if (ms->guards[i]) {
                        fprintf(out, " && lamo_is_truthy(");
                        generate_expression_code(ms->guards[i], out);
                        fprintf(out, ")");
                    }
                    fprintf(out, " ");
                    if (ms->bodies[i]) {
                        generate_statement_code(ms->bodies[i], out);
                    } else {
                        fprintf(out, "{ }\n");
                    }
                    has_emitted = 1;
                }
            }
            }
            break;
        }
        case AST_PLACE_ASSIGN_STMT: {
            /* Phase 2: `arr[i] = value;` or `obj.field = value;`.
             * For `=`, emit a direct setter call. For `+=`/`-=`, emit
             * a read-modify-write: setter(obj, idx, lamo_add(getter(obj, idx), value)). */
            ASTPlaceAssignStmt* pa = (ASTPlaceAssignStmt*)node;
            if (pa->target->type == AST_INDEX_EXPR) {
                ASTIndexExpr* ie = (ASTIndexExpr*)pa->target;
                if (pa->op_type == TOKEN_EQUALS) {
                    fprintf(out, "lamo_array_set(");
                    generate_expression_code(ie->array, out);
                    fprintf(out, ", lamo_as_int(");
                    generate_expression_code(ie->index, out);
                    fprintf(out, "), ");
                    generate_expression_code(pa->value, out);
                    fprintf(out, ");\n");
                } else if (pa->op_type == TOKEN_PLUS_EQ) {
                    fprintf(out, "lamo_array_set(");
                    generate_expression_code(ie->array, out);
                    fprintf(out, ", lamo_as_int(");
                    generate_expression_code(ie->index, out);
                    fprintf(out, "), lamo_add(lamo_array_get(");
                    generate_expression_code(ie->array, out);
                    fprintf(out, ", lamo_as_int(");
                    generate_expression_code(ie->index, out);
                    fprintf(out, ")), ");
                    generate_expression_code(pa->value, out);
                    fprintf(out, "));\n");
                } else if (pa->op_type == TOKEN_MINUS_EQ) {
                    fprintf(out, "lamo_array_set(");
                    generate_expression_code(ie->array, out);
                    fprintf(out, ", lamo_as_int(");
                    generate_expression_code(ie->index, out);
                    fprintf(out, "), lamo_sub(lamo_array_get(");
                    generate_expression_code(ie->array, out);
                    fprintf(out, ", lamo_as_int(");
                    generate_expression_code(ie->index, out);
                    fprintf(out, ")), ");
                    generate_expression_code(pa->value, out);
                    fprintf(out, "));\n");
                }
            } else if (pa->target->type == AST_PROP_EXPR) {
                ASTPropExpr* pe = (ASTPropExpr*)pa->target;
                /* Look up the field index using the struct name from sema. */
                const char* struct_name = pe->object ? pe->object->sema_struct_name : NULL;
                if (!struct_name) struct_name = ((ASTNode*)pe)->sema_struct_name;
                int field_index = -1;
                if (struct_name) {
                    for (ASTNode* cur = g_program_decls; cur; cur = cur->next) {
                        if (cur->type == AST_STRUCT_DECL) {
                            ASTStructDecl* sd = (ASTStructDecl*)cur;
                            if (sd->name && strcmp(sd->name, struct_name) == 0) {
                                for (int i = 0; i < sd->field_count; i++) {
                                    if (sd->field_names[i] && strcmp(sd->field_names[i], pe->prop_name) == 0) {
                                        field_index = i;
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
                if (field_index < 0) field_index = 0;  /* defensive */
                if (pa->op_type == TOKEN_EQUALS) {
                    fprintf(out, "lamo_struct_set(");
                    generate_expression_code(pe->object, out);
                    fprintf(out, ", %d, ", field_index);
                    generate_expression_code(pa->value, out);
                    fprintf(out, ");\n");
                } else if (pa->op_type == TOKEN_PLUS_EQ) {
                    fprintf(out, "lamo_struct_set(");
                    generate_expression_code(pe->object, out);
                    fprintf(out, ", %d, lamo_add(lamo_struct_get(", field_index);
                    generate_expression_code(pe->object, out);
                    fprintf(out, ", %d), ", field_index);
                    generate_expression_code(pa->value, out);
                    fprintf(out, "));\n");
                } else if (pa->op_type == TOKEN_MINUS_EQ) {
                    fprintf(out, "lamo_struct_set(");
                    generate_expression_code(pe->object, out);
                    fprintf(out, ", %d, lamo_sub(lamo_struct_get(", field_index);
                    generate_expression_code(pe->object, out);
                    fprintf(out, ", %d), ", field_index);
                    generate_expression_code(pa->value, out);
                    fprintf(out, "));\n");
                }
            }
            break;
        }
        default:
            break;
    }
}

static void generate_binary_expr(ASTBinaryExpr* expr, FILE* out) {
    /* Sprint 3: constant folding for arithmetic on literal operands.
     *
     * If both sides are numeric literals, we evaluate the expression at
     * compile time and emit a single literal instead of a runtime call.
     * This is a small but real performance win for code that does
     * arithmetic on constants (e.g. `let buf_size = 1024 * 4;`), and it
     * also shrinks the generated C.
     *
     * We deliberately fold only when BOTH operands are literals — this
     * avoids needing a full constant-propagation pass. Foldable cases:
     *   int + int   -> int
     *   int + float -> float
     *   float + int -> float
     *   float + float -> float
     * Same for -, *, /, %.
     *
     * Division/modulo by zero is a runtime error in Lamo; we don't fold
     * those (let the runtime emit the proper error message). */
    if (expr->left->type == AST_INT_LITERAL || expr->left->type == AST_FLOAT_LITERAL) {
        if (expr->right->type == AST_INT_LITERAL || expr->right->type == AST_FLOAT_LITERAL) {
            int left_is_float  = expr->left->type  == AST_FLOAT_LITERAL;
            int right_is_float = expr->right->type == AST_FLOAT_LITERAL;
            long long li = left_is_float  ? 0 : ((ASTIntLiteral*)expr->left)->value;
            double     lf = left_is_float  ? ((ASTFloatLiteral*)expr->left)->value  : 0.0;
            long long ri = right_is_float ? 0 : ((ASTIntLiteral*)expr->right)->value;
            double     rf = right_is_float ? ((ASTFloatLiteral*)expr->right)->value : 0.0;

            switch (expr->operator) {
                case TOKEN_PLUS:
                    if (left_is_float || right_is_float) {
                        fprintf(out, "lamo_make_float(%#.17g)", (double)(left_is_float ? lf : (double)li) + (right_is_float ? rf : (double)ri));
                    } else {
                        fprintf(out, "lamo_make_int(%lldLL)", li + ri);
                    }
                    return;
                case TOKEN_MINUS:
                    if (left_is_float || right_is_float) {
                        fprintf(out, "lamo_make_float(%#.17g)", (double)(left_is_float ? lf : (double)li) - (right_is_float ? rf : (double)ri));
                    } else {
                        fprintf(out, "lamo_make_int(%lldLL)", li - ri);
                    }
                    return;
                case TOKEN_STAR:
                    if (left_is_float || right_is_float) {
                        fprintf(out, "lamo_make_float(%#.17g)", (double)(left_is_float ? lf : (double)li) * (right_is_float ? rf : (double)ri));
                    } else {
                        fprintf(out, "lamo_make_int(%lldLL)", li * ri);
                    }
                    return;
                case TOKEN_SLASH:
                    /* Don't fold if the divisor is zero — let the runtime
                     * emit its "division by zero" error. */
                    if ((right_is_float && rf != 0.0) || (!right_is_float && ri != 0)) {
                        if (left_is_float || right_is_float) {
                            fprintf(out, "lamo_make_float(%#.17g)", (left_is_float ? lf : (double)li) / (right_is_float ? rf : (double)ri));
                        } else {
                            fprintf(out, "lamo_make_int(%lldLL)", li / ri);
                        }
                        return;
                    }
                    break;  /* fall through to runtime call */
                case TOKEN_PERCENT:
                    if ((right_is_float && rf != 0.0) || (!right_is_float && ri != 0)) {
                        if (left_is_float || right_is_float) {
                            fprintf(out, "lamo_make_float(%#.17g)", fmod(left_is_float ? lf : (double)li, right_is_float ? rf : (double)ri));
                        } else {
                            fprintf(out, "lamo_make_int(%lldLL)", li % ri);
                        }
                        return;
                    }
                    break;  /* fall through to runtime call */
                default:
                    /* Comparison/logical operators: not folded here.
                     * They could be, but the win is smaller and the
                     * boolean semantics need careful handling. */
                    break;
            }
        }
    }

    switch (expr->operator) {
        case TOKEN_PLUS:      fprintf(out, "lamo_add("); break;
        case TOKEN_MINUS:     fprintf(out, "lamo_sub("); break;
        case TOKEN_STAR:      fprintf(out, "lamo_mul("); break;
        case TOKEN_SLASH:     fprintf(out, "lamo_div("); break;
        case TOKEN_PERCENT:   fprintf(out, "lamo_mod("); break;
        case TOKEN_LT:        fprintf(out, "lamo_less("); break;
        case TOKEN_GT:        fprintf(out, "lamo_greater("); break;
        case TOKEN_LT_EQ:     fprintf(out, "lamo_less_equal("); break;
        case TOKEN_GT_EQ:     fprintf(out, "lamo_greater_equal("); break;
        case TOKEN_EQ_EQ:     fprintf(out, "lamo_equal("); break;
        case TOKEN_BANG_EQ:   fprintf(out, "lamo_not_equal("); break;
        case TOKEN_AND_AND:   fprintf(out, "lamo_and("); break;
        case TOKEN_OR_OR:     fprintf(out, "lamo_or("); break;
        default:
            fprintf(out, "lamo_make_int(0)");
            return;
    }

    generate_expression_code(expr->left, out);
    fprintf(out, ", ");
    generate_expression_code(expr->right, out);
    fprintf(out, ")");
}

// Emite um literal de string como constante C, escapando corretamente.
static void emit_c_string_literal(const char* s, FILE* out) {
    fputc('"', out);
    while (*s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (c >= 0x20 && c < 0x7f) {
                    fputc((char)c, out);
                } else {
                    fprintf(out, "\\x%02x", c);
                }
                break;
        }
        s++;
    }
    fputc('"', out);
}

static void generate_expression_code(ASTNode* node, FILE* out) {
    if (!node) {
        fprintf(out, "lamo_make_int(0)");
        return;
    }

    switch (node->type) {
        case AST_INT_LITERAL:
            fprintf(out, "lamo_make_int(%lldLL)", ((ASTIntLiteral*)node)->value);
            break;
        case AST_FLOAT_LITERAL:
            fprintf(out, "lamo_make_float(%#.17g)", ((ASTFloatLiteral*)node)->value);
            break;
        case AST_STRING_LITERAL:
            fputs("lamo_make_string(", out);
            emit_c_string_literal(((ASTStringLiteral*)node)->value, out);
            fputc(')', out);
            break;
        case AST_BOOL_LITERAL:
            fprintf(out, "lamo_make_bool(%d)", ((ASTBoolLiteral*)node)->value);
            break;
        case AST_IDENTIFIER:
            fprintf(out, "%s", user_name1(((ASTIdentifier*)node)->name));
            break;
        case AST_BINARY_EXPR:
            generate_binary_expr((ASTBinaryExpr*)node, out);
            break;
        case AST_UNARY_EXPR: {
            ASTUnaryExpr* expr = (ASTUnaryExpr*)node;
            if (expr->operator == TOKEN_MINUS) {
                fprintf(out, "lamo_negate(");
                generate_expression_code(expr->right, out);
                fprintf(out, ")");
            } else if (expr->operator == TOKEN_BANG) {
                fprintf(out, "lamo_not(");
                generate_expression_code(expr->right, out);
                fprintf(out, ")");
            } else {
                fprintf(out, "lamo_make_int(0)");
            }
            break;
        }
        case AST_VARIANT_REF: {
            /* 2.7.0 (FU4): qualified unit-variant value — `Enum::Variant`.
             * Emit the value directly so it does not depend on the
             * (shadowable) bare-variant globals: tagged enums get
             * lamo_make_enum with a NULL payload array, legacy untagged
             * enums keep the plain int constant. */
            ASTVariantRef* vr = (ASTVariantRef*)node;
            if (lamo_codegen_enum_is_tagged(vr->enum_name)) {
                fprintf(out, "lamo_make_enum(%d, \"%s\", (LamoArray*)0)",
                        node->sema_variant_index, vr->variant_name);
            } else {
                fprintf(out, "lamo_make_int(%dLL)", node->sema_variant_index);
            }
            break;
        }
        case AST_CALL_EXPR: {
            ASTCallExpr* call_expr = (ASTCallExpr*)node;
            /* 2.6.0: enum variant constructor — `Some(42)` (SPEC §3.5).
             * The semantic pass annotated the node (sema_enum_name +
             * sema_variant_index); build the payload array and the tagged
             * value with a GCC statement expression so the constructor
             * works in any expression position. Checked FIRST so variant
             * names resolve as constructors before user functions. */
            if (call_expr->base.sema_enum_name) {
                int payload_count = call_expr->arg_count;
                fprintf(out, "({ LamoArray* _lamo_enum_pl = lamo_enum_payloads_alloc(%d); ",
                        payload_count > 0 ? payload_count : 0);
                for (int pi = 0; pi < payload_count; pi++) {
                    fprintf(out, "_lamo_enum_pl->items[%d] = ", pi);
                    generate_expression_code(call_expr->args[pi], out);
                    fprintf(out, "; ");
                }
                fprintf(out, "lamo_make_enum(%d, \"%s\", _lamo_enum_pl); })",
                        call_expr->base.sema_variant_index,
                        /* 2.7.0 (FU4): qualified names print only the variant. */
                        lamo_variant_short_name(call_expr->name));
            } else if (is_lang_builtin(call_expr->name)) {
                generate_lang_builtin_call_expr(call_expr->name, call_expr->args, call_expr->arg_count, out);
            } else if (is_gui_builtin(call_expr->name)) {
                generate_gui_call_expr(call_expr->name, call_expr->args, call_expr->arg_count, out);
            } else if (is_http_builtin(call_expr->name)) {
                generate_http_call_expr(call_expr->name, call_expr->args, call_expr->arg_count, out);
            } else if (is_std_builtin(call_expr->name)) {
                generate_std_builtin_call_expr(call_expr->name, call_expr->args, call_expr->arg_count, out);
            } else {
                fprintf(out, "%s(", user_name1(call_expr->name));
                generate_call_arguments(call_expr->args, call_expr->arg_count, out);
                fprintf(out, ")");
            }
            break;
        }
        case AST_MEMBER_CALL: {
            /* Sprint 4 + Phase 2: dispatch centralized in generate_member_call_code. */
            ASTMemberCall* mc = (ASTMemberCall*)node;
            generate_member_call_code(mc, out);
            break;
        }
        case AST_GROUPING_EXPR:
            fprintf(out, "(");
            generate_expression_code(((ASTGroupingExpr*)node)->expression, out);
            fprintf(out, ")");
            break;
        case AST_ARRAY_LITERAL: {
            /* Sprint 3: array literal. We emit a sequence of LamoValue
             * initializers inside a compound literal, then call
             * lamo_array_from_values. The compound literal has its
             * address taken, so it lives on the stack for the duration
             * of the call. */
            ASTArrayLiteral* arr = (ASTArrayLiteral*)node;
            int i;
            if (arr->element_count == 0) {
                fprintf(out, "lamo_array_from_values((LamoValue*)0, 0)");
            } else {
                fprintf(out, "lamo_array_from_values((LamoValue[]){");
                for (i = 0; i < arr->element_count; i++) {
                    if (i > 0) fprintf(out, ", ");
                    generate_expression_code(arr->elements[i], out);
                }
                fprintf(out, "}, %d)", arr->element_count);
            }
            break;
        }
        case AST_INDEX_EXPR: {
            /* Sprint 3: array index access. We coerce the index to int
             * (Lamo indexing is int-only for now) and call lamo_array_get. */
            ASTIndexExpr* idx = (ASTIndexExpr*)node;
            fprintf(out, "lamo_array_get(");
            generate_expression_code(idx->array, out);
            fprintf(out, ", lamo_as_int(");
            generate_expression_code(idx->index, out);
            fprintf(out, "))");
            break;
        }
        case AST_PROP_EXPR: {
            /* Phase 2: dispatch centralized in generate_prop_expr_code.
             * Handles both array .len and struct field access. */
            ASTPropExpr* prop = (ASTPropExpr*)node;
            generate_prop_expr_code(prop, out);
            break;
        }
        case AST_STRUCT_LITERAL: {
            /* Phase 2: struct literal — `Name { field: value, ... }`.
             * Emit lamo_struct_alloc(field_count) followed by a series
             * of lamo_struct_set calls, one per provided field. The
             * whole expression evaluates to the constructed struct.
             *
             * We use a GCC statement expression `({ ...; result; })` so
             * the literal can appear in any expression context. This is
             * a GNU extension but is widely supported (gcc, clang, tcc).
             * The -std=c99 flag we pass to gcc still accepts statement
             * expressions as an extension (only -pedantic-errors rejects
             * them). */
            ASTStructLiteral* sl = (ASTStructLiteral*)node;
            /* Find the struct definition to get the total field count. */
            int total_fields = sl->field_count;
            for (ASTNode* cur = g_program_decls; cur; cur = cur->next) {
                if (cur->type == AST_STRUCT_DECL) {
                    ASTStructDecl* sd = (ASTStructDecl*)cur;
                    if (sd->name && strcmp(sd->name, sl->struct_name) == 0) {
                        total_fields = sd->field_count;
                        break;
                    }
                }
            }
            fprintf(out, "({ LamoValue _lamo_struct_tmp = lamo_struct_alloc(%d); ", total_fields);
            for (int i = 0; i < sl->field_count; i++) {
                /* Find the field index. */
                int field_index = -1;
                for (ASTNode* cur = g_program_decls; cur; cur = cur->next) {
                    if (cur->type == AST_STRUCT_DECL) {
                        ASTStructDecl* sd = (ASTStructDecl*)cur;
                        if (sd->name && strcmp(sd->name, sl->struct_name) == 0) {
                            for (int j = 0; j < sd->field_count; j++) {
                                if (sd->field_names[j] && strcmp(sd->field_names[j], sl->field_names[i]) == 0) {
                                    field_index = j;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
                if (field_index < 0) field_index = i;  /* defensive */
                fprintf(out, "lamo_struct_set(_lamo_struct_tmp, %d, ", field_index);
                generate_expression_code(sl->field_values[i], out);
                fprintf(out, "); ");
            }
            fprintf(out, "_lamo_struct_tmp; })");
            break;
        }
        default:
            fprintf(out, "lamo_make_int(0)");
            break;
    }
}

