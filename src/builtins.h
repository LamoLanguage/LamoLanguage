#ifndef LAMO_BUILTINS_H
#define LAMO_BUILTINS_H

/*
 * builtins.h - Single source of truth for Lamo builtins.
 *
 * Sprint 2 refactor: previously, the same builtin name lists were
 * duplicated across lexer.c (lexer_is_builtin_name), semantic.c
 * (builtin_function_arity, builtin_function_return_type),
 * codegen.c (is_gui_builtin, is_http_builtin, is_lang_builtin), and
 * lamo_v2.c (lamo_is_gui_builtin_name). Adding a new builtin required
 * editing 5 places; forgetting one caused silent bugs. This single
 * table replaces all of them.
 *
 * To add a new builtin:
 *   1. Add an entry to lamo_builtins[] below.
 *   2. Implement the actual function in lamo_runtime.h.
 *   3. Add the codegen case in generate_lang_builtin_call_expr /
 *      generate_gui_call_expr / generate_http_call_expr in codegen.c.
 *
 * This header is included by lexer.c, semantic.c, codegen.c, and
 * lamo_v2.c. It uses only `inline` static functions and a `static
 * const` array, so it does not introduce any new translation units or
 * link-time symbols.
 */

#include <string.h>
#include <stddef.h>

typedef enum {
    BUILTIN_LANG,
    BUILTIN_GUI,
    BUILTIN_HTTP,
    BUILTIN_STD
} BuiltinCategory;

typedef enum {
    BUILTIN_RET_INT,            /* always returns int (or int-shaped) */
    BUILTIN_RET_STRING,         /* always returns string */
    BUILTIN_RET_BOOL,           /* always returns bool */
    BUILTIN_RET_MIRROR_ARG0     /* return type mirrors args[0]'s type (abs) */
} BuiltinRetPolicy;

typedef struct {
    const char* name;
    int arity;
    BuiltinCategory category;
    BuiltinRetPolicy ret_policy;
} BuiltinInfo;

static const BuiltinInfo lamo_builtins[] = {
    /* Language builtins — shadowable by user functions, available everywhere. */
    {"print",      1, BUILTIN_LANG, BUILTIN_RET_INT},
    {"input",      1, BUILTIN_LANG, BUILTIN_RET_INT},
    {"input_int",  1, BUILTIN_LANG, BUILTIN_RET_INT},
    {"input_str",  1, BUILTIN_LANG, BUILTIN_RET_STRING},
    {"isnumber",   1, BUILTIN_LANG, BUILTIN_RET_BOOL},
    {"isstring",   1, BUILTIN_LANG, BUILTIN_RET_BOOL},
    {"isarray",    1, BUILTIN_LANG, BUILTIN_RET_BOOL},
    {"exit",       1, BUILTIN_LANG, BUILTIN_RET_INT},
    {"abs",        1, BUILTIN_LANG, BUILTIN_RET_MIRROR_ARG0},
    /* Sprint 3: array builtins. len(arr) returns the element count as int.
     * push(arr, x) appends x and returns 0. pop(arr) removes and returns
     * the last element. */
    {"len",        1, BUILTIN_LANG, BUILTIN_RET_INT},
    {"push",       2, BUILTIN_LANG, BUILTIN_RET_INT},
    {"pop",        1, BUILTIN_LANG, BUILTIN_RET_INT},  /* actually returns the popped value's type, but we report INT conservatively */

    /* GC builtins (BUILTIN_LANG, shadowable). Opt-in mark-sweep GC;
     * see docs/MEMORY-MODEL.md. Programs that never call these see
     * zero GC overhead (the per-allocation header is the only cost). */
    {"gc_collect",       0, BUILTIN_LANG, BUILTIN_RET_INT},     /* run a full mark-sweep; returns live count */
    {"gc_set_threshold", 1, BUILTIN_LANG, BUILTIN_RET_INT},     /* set auto-trigger threshold in bytes; 0 disables */
    {"gc_heap_size",     0, BUILTIN_LANG, BUILTIN_RET_INT},     /* live bytes after the last collect */
    {"gc_heap_count",    0, BUILTIN_LANG, BUILTIN_RET_INT},     /* live allocation count after the last collect */

    /* ==================================================================== */
    /* Standard library builtins (BUILTIN_STD)                              */
    /* These power the std.* modules under std/. They're emitted only when  */
    /* the program actually uses them (gated by LAMO_NEEDS_STD_RUNTIME).    */
    /* Names use a __lamo_std_<module>_<fn> prefix to avoid collisions with  */
    /* user-defined functions; the std/<module>.lamo wrappers expose them    */
    /* through the namespaced import API (e.g. math.sqrt, fs.readText).      */
    /* ==================================================================== */

    /* std.math */
    {"__lamo_std_math_sqrt",    1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_math_pow",     2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_math_sin",     1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_math_cos",     1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_math_tan",     1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_math_floor",   1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_math_ceil",    1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_math_round",   1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_math_min",     2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_math_max",     2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_math_clamp",   3, BUILTIN_STD, BUILTIN_RET_INT},

    /* std.string */
    {"__lamo_std_str_length",      1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_str_upper",       1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_str_lower",       1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_str_starts_with", 2, BUILTIN_STD, BUILTIN_RET_BOOL},
    {"__lamo_std_str_ends_with",   2, BUILTIN_STD, BUILTIN_RET_BOOL},
    {"__lamo_std_str_contains",    2, BUILTIN_STD, BUILTIN_RET_BOOL},
    {"__lamo_std_str_index_of",    2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_str_trim",        1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_str_substring",   3, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_str_replace",     3, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_str_split",       2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_str_char_at",     2, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_str_repeat",      2, BUILTIN_STD, BUILTIN_RET_STRING},

    /* std.path */
    {"__lamo_std_path_join",      2, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_path_parent",    1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_path_filename",  1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_path_extension", 1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_path_absolute",  1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_path_normalize", 1, BUILTIN_STD, BUILTIN_RET_STRING},

    /* std.fs */
    {"__lamo_std_fs_exists",     1, BUILTIN_STD, BUILTIN_RET_BOOL},
    {"__lamo_std_fs_is_file",    1, BUILTIN_STD, BUILTIN_RET_BOOL},
    {"__lamo_std_fs_is_dir",     1, BUILTIN_STD, BUILTIN_RET_BOOL},
    {"__lamo_std_fs_read_text",  1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_fs_write_text", 2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_fs_append_text",2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_fs_delete",     1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_fs_create_dir", 1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_fs_remove_dir", 1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_fs_copy",       2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_fs_move",       2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_fs_list_files", 1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_fs_size",       1, BUILTIN_STD, BUILTIN_RET_INT},

    /* std.env */
    {"__lamo_std_env_get",    1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_env_set",    2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_env_remove", 1, BUILTIN_STD, BUILTIN_RET_INT},

    /* std.os */
    {"__lamo_std_os_name",      0, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_os_arch",      0, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_os_cpu_count", 0, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_os_home",      0, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_os_temp_dir",  0, BUILTIN_STD, BUILTIN_RET_STRING},

    /* std.time */
    {"__lamo_std_time_now",      0, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_time_timestamp",0, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_time_sleep",    1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_time_monotonic",0, BUILTIN_STD, BUILTIN_RET_INT},

    /* std.process */
    {"__lamo_std_process_pid",  0, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_process_run",  1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_process_exec", 1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_process_exit", 1, BUILTIN_STD, BUILTIN_RET_INT},

    /* std.random */
    {"__lamo_std_random_seed",   1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_random_int",    2, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_random_float",  0, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_random_bool",   0, BUILTIN_STD, BUILTIN_RET_BOOL},
    {"__lamo_std_random_choice", 1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_random_shuffle",1, BUILTIN_STD, BUILTIN_RET_INT},

    /* std.io (extra I/O builtins; print/input/exit already in LANG) */
    {"__lamo_std_io_println",  1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_io_eprint",   1, BUILTIN_STD, BUILTIN_RET_INT},
    {"__lamo_std_io_read_line",0, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_io_write",    1, BUILTIN_STD, BUILTIN_RET_INT},

    /* std.net (HTTP client) */
    {"__lamo_std_net_http_get",  1, BUILTIN_STD, BUILTIN_RET_STRING},
    {"__lamo_std_net_http_post", 2, BUILTIN_STD, BUILTIN_RET_STRING},
    /* GUI builtins — Windows GDI / X11, gated behind LAMO_NEEDS_GUI_RUNTIME. */
    {"gui_open",          3, BUILTIN_GUI, BUILTIN_RET_INT},
    {"gui_should_close",  0, BUILTIN_GUI, BUILTIN_RET_INT},
    {"gui_begin_frame",   3, BUILTIN_GUI, BUILTIN_RET_INT},
    {"gui_draw_rect",     7, BUILTIN_GUI, BUILTIN_RET_INT},
    {"gui_draw_text",     6, BUILTIN_GUI, BUILTIN_RET_INT},
    {"gui_end_frame",     0, BUILTIN_GUI, BUILTIN_RET_INT},
    {"gui_close",         0, BUILTIN_GUI, BUILTIN_RET_INT},
    /* HTTP builtins — link with -lws2_32 on Windows, plain sockets on POSIX. */
    {"http_route",      2, BUILTIN_HTTP, BUILTIN_RET_INT},
    {"http_serve",      1, BUILTIN_HTTP, BUILTIN_RET_INT},
    {"http_serve_once", 1, BUILTIN_HTTP, BUILTIN_RET_INT},
};
#define LAMO_BUILTINS_COUNT (sizeof(lamo_builtins) / sizeof(lamo_builtins[0]))

/* Lookup by name. Returns NULL if not found. */
static inline const BuiltinInfo* lamo_builtin_lookup(const char* name) {
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < LAMO_BUILTINS_COUNT; i++) {
        if (strcmp(lamo_builtins[i].name, name) == 0) {
            return &lamo_builtins[i];
        }
    }
    return NULL;
}

/* Convenience predicates — replace the old per-file is_*_builtin() helpers. */
static inline int lamo_builtin_is_lang(const char* name) {
    const BuiltinInfo* b = lamo_builtin_lookup(name);
    return b != NULL && b->category == BUILTIN_LANG;
}
static inline int lamo_builtin_is_gui(const char* name) {
    const BuiltinInfo* b = lamo_builtin_lookup(name);
    return b != NULL && b->category == BUILTIN_GUI;
}
static inline int lamo_builtin_is_http(const char* name) {
    const BuiltinInfo* b = lamo_builtin_lookup(name);
    return b != NULL && b->category == BUILTIN_HTTP;
}
static inline int lamo_builtin_is_std(const char* name) {
    const BuiltinInfo* b = lamo_builtin_lookup(name);
    return b != NULL && b->category == BUILTIN_STD;
}

/* Returns arity (-1 if not a builtin). Replaces semantic.c's
 * builtin_function_arity(). */
static inline int lamo_builtin_arity(const char* name) {
    const BuiltinInfo* b = lamo_builtin_lookup(name);
    return b ? b->arity : -1;
}

#endif /* LAMO_BUILTINS_H */
