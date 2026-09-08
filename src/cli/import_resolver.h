#ifndef LAMO_CLI_IMPORT_RESOLVER_H
#define LAMO_CLI_IMPORT_RESOLVER_H

/*
 * import_resolver.h — Recursive import loader for the `lamo` compiler.
 *
 * Refactor (Sprint 5): the loader (CompilationState struct, FileLoadState
 * enum, all the load_* / cycle-detection / module-rename helpers) used
 * to be `static` inside lamo_v2.c. They are now external so that
 * compile.c can drive the pipeline while import_resolver.c owns the
 * bookkeeping.
 *
 * Responsibilities:
 *   - Track every file loaded during a single compile (dedup by
 *     normalized absolute path) so each file is parsed only once.
 *   - Detect import cycles (file A imports B which imports A) and
 *     report them with a file:line:col pointing at the import that
 *     closed the cycle.
 *   - For `import "..." as alias;` statements, rename every top-level
 *     declaration in the imported file to `lamo_mod_<alias>__<name>`
 *     and register them in the module registry so semantic + codegen
 *     can resolve `alias.member(args)` calls.
 */

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "modules.h"

typedef enum {
    FILE_NOT_LOADED,
    FILE_LOADING,
    FILE_LOADED
} FileLoadState;

typedef struct {
    char** file_paths;
    char** sources;
    Lexer** lexers;
    Parser** parsers;
    FileLoadState* load_states;
    int* load_stack;
    int count;
    int capacity;
    int load_depth;
    int load_stack_capacity;
    /* Sprint 4: module registry. Tracks each `import "..." as alias;`
     * statement so the semantic pass and codegen can resolve
     * `alias.member(args)` calls. Lives for the duration of the
     * compilation and is freed in free_compilation_state(). */
    LamoModuleRegistry modules;
} CompilationState;

/* Grow the per-file arrays so they can hold at least `required_count`
 * entries. Returns 1 on success, 0 on allocation failure. */
int ensure_state_capacity(CompilationState* state, int required_count);

/* Grow the import-stack array so it can hold at least `required_depth`
 * entries. Returns 1 on success, 0 on allocation failure. */
int ensure_load_stack_capacity(CompilationState* state, int required_depth);

/* Push a file index onto the loading stack. Returns 1 on success. */
int push_loading_file(CompilationState* state, int file_index);

/* Pop the top of the loading stack. No-op if the stack is empty. */
void pop_loading_file(CompilationState* state);

/* Report an import cycle ending at `repeated_index`. The cycle path is
 * reconstructed from the current loading stack. */
void report_import_cycle(const CompilationState* state, int repeated_index);

/* Like report_import_cycle, but prefix the message with the file:line:col
 * of the import statement that closed the cycle, matching the format
 * used by parser and semantic errors. */
void report_import_cycle_at(const CompilationState* state, int repeated_index,
                            const char* importing_file, int line, int column);

/* Linear search for a previously-loaded file by normalized path.
 * Returns its index, or -1 if not found. */
int find_loaded_file(const CompilationState* state, const char* normalized_path);

/* Reserve a fresh slot in the state arrays and store `normalized_path`
 * (ownership transfers to the state). Returns the new slot index, or
 * -1 on allocation failure. */
int reserve_file_slot(CompilationState* state, char* normalized_path);

/* Free everything owned by the state (parsers, lexers, sources, paths,
 * module registry). Does not free the state struct itself. */
void free_compilation_state(CompilationState* state);

/* Walk an AST subtree looking for AST_IMPORT nodes and load each one
 * recursively via load_program_recursive_from(). `importing_file` is
 * used for error messages. Returns 1 on success, 0 on failure. */
int load_imports_from_ast(CompilationState* state, ASTProgram* aggregate_program,
                          ASTNode* node, const char* importing_file);

/* Rewrite `*name_slot` in place if it matches one of the module's
 * original member names. Returns 1 if rewritten, 0 otherwise. */
int rewrite_name_if_member(char** name_slot, const LamoModuleEntry* entry);

/* Recursive walker: rewrite references to the module's members inside
 * an AST subtree. Visits calls, identifiers, assignments, and all
 * structural nodes. */
void rewrite_member_refs_recursive(ASTNode* node, const LamoModuleEntry* entry);

/* Rename every top-level declaration in `program` by prefixing its
 * name with `lamo_mod_<alias>__` and register each one in `reg`.
 * After renaming top-level decls, also rewrites references to those
 * members inside each function body so recursive calls / global refs
 * still resolve. Returns 1 on success, 0 on allocation failure. */
int rename_module_declarations(ASTProgram* program, const char* alias,
                               LamoModuleRegistry* reg);

/* Load `path` (a top-level input file). Normalizes the path, dedups
 * against already-loaded files, parses, recurses into imports, and
 * merges the parsed program into `aggregate_program`. Returns 1 on
 * success, 0 on failure (error message already printed). */
int load_program_recursive(CompilationState* state, ASTProgram* aggregate_program,
                           const char* path);

/* Like load_program_recursive, but called from an import site — carries
 * the importing file / line / column for cycle-error messages, and the
 * optional alias. When `alias` is non-NULL, the loaded file's top-level
 * decls are renamed to `lamo_mod_<alias>__<name>` before merging. */
int load_program_recursive_from(CompilationState* state, ASTProgram* aggregate_program,
                                const char* path, const char* imported_from,
                                int import_line, int import_column,
                                const char* alias);

#endif /* LAMO_CLI_IMPORT_RESOLVER_H */
