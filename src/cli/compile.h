#ifndef LAMO_CLI_COMPILE_H
#define LAMO_CLI_COMPILE_H

/*
 * compile.h — The compile pipeline (run / build / check / eval) for
 * the `lamo` CLI.
 *
 * Refactor (Sprint 5): compile_sources and its helpers (program_uses_gui,
 * the semantic-analyzer callbacks, the GUI-builtin walker) used to be
 * `static` inside lamo_v2.c. They are now external so that main (in
 * lamo_v2.c) can dispatch to compile_sources without owning the whole
 * pipeline implementation.
 *
 * run_argv (the cross-platform spawn helper used by compile_sources and
 * command_test) still lives in lamo_v2.c; it is forward-declared here.
 */

#include "cli_options.h"
#include "import_resolver.h"
#include "ast.h"

/* Walk an AST subtree and return 1 if any call targets a GUI builtin
 * (per builtins.h::lamo_builtin_is_gui). Used to decide whether to
 * link -lX11 on POSIX. */
int lamo_program_uses_gui_recursive(ASTNode* node);

/* Top-level wrapper around lamo_program_uses_gui_recursive that accepts
 * an ASTProgram*. */
int program_uses_gui(ASTProgram* program);

/* Source-lookup callback handed to semantic_analyze_full. Given a file
 * path, returns the source text loaded for it (stored in
 * CompilationState.sources[]), or NULL if the path is not known. */
const char* lamo_source_lookup(const char* file_path, void* user_data);

/* Module-resolution callback: given an alias and a member name,
 * returns the prefixed function name to call (e.g.
 * "lamo_mod_math__sqrt"), or NULL if the alias/member is not
 * registered. */
const char* lamo_module_resolve_cb(const char* alias, const char* member, void* user_data);

/* Module-arity callback. Returns the param count of a module member
 * function, or -1 for non-function members / unknown alias/member. */
int lamo_module_arity_cb(const char* alias, const char* member, void* user_data);

/* Drive the full compile pipeline for a set of input files:
 *   - parse + import-resolve each input
 *   - run the semantic pass
 *   - for `check`: stop after semantic
 *   - for `eval`: run the tree-walking interpreter
 *   - for `run`/`build`: generate C, invoke LAMO_CC, and (for `run`)
 *     execute the resulting binary
 *
 * Returns one of the EXIT_*_CODE constants from cli_options.h.
 */
int compile_sources(const char** input_files, int input_file_count,
                    LamoCommand command, const char* output_path);

/* run_argv: execute a program with the given argv without going through
 * a shell. Implemented in lamo_v2.c (the only file that needs fork/exec
 * directly outside of compile_sources / command_test). argv MUST be
 * NULL-terminated; argv[0] is the program name. Returns the child's
 * exit status (0-255) on success, -1 on spawn failure. */
int run_argv(char* const argv[]);

#endif /* LAMO_CLI_COMPILE_H */
