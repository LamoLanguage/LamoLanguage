/*
 * import_resolver.c — Recursive import loader for the `lamo` compiler.
 * See import_resolver.h for the design rationale.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "import_resolver.h"
#include "paths.h"

int ensure_state_capacity(CompilationState* state, int required_count) {
    int new_capacity;
    char** resized_paths;
    char** resized_sources;
    Lexer** resized_lexers;
    Parser** resized_parsers;
    FileLoadState* resized_load_states;
    int i;

    if (required_count <= state->capacity) {
        return 1;
    }

    new_capacity = state->capacity > 0 ? state->capacity * 2 : 4;
    while (new_capacity < required_count) {
        new_capacity *= 2;
    }

    resized_paths = calloc((size_t)new_capacity, sizeof(char*));
    resized_sources = calloc((size_t)new_capacity, sizeof(char*));
    resized_lexers = calloc((size_t)new_capacity, sizeof(Lexer*));
    resized_parsers = calloc((size_t)new_capacity, sizeof(Parser*));
    resized_load_states = calloc((size_t)new_capacity, sizeof(FileLoadState));
    if (!resized_paths || !resized_sources || !resized_lexers || !resized_parsers || !resized_load_states) {
        free(resized_paths);
        free(resized_sources);
        free(resized_lexers);
        free(resized_parsers);
        free(resized_load_states);
        return 0;
    }

    for (i = 0; i < state->count; i++) {
        resized_paths[i] = state->file_paths[i];
        resized_sources[i] = state->sources[i];
        resized_lexers[i] = state->lexers[i];
        resized_parsers[i] = state->parsers[i];
        resized_load_states[i] = state->load_states[i];
    }

    free(state->file_paths);
    free(state->sources);
    free(state->lexers);
    free(state->parsers);
    free(state->load_states);

    state->file_paths = resized_paths;
    state->sources = resized_sources;
    state->lexers = resized_lexers;
    state->parsers = resized_parsers;
    state->load_states = resized_load_states;
    state->capacity = new_capacity;

    return 1;
}

int ensure_load_stack_capacity(CompilationState* state, int required_depth) {
    int new_capacity;
    int* resized_stack;

    if (required_depth <= state->load_stack_capacity) {
        return 1;
    }

    new_capacity = state->load_stack_capacity > 0 ? state->load_stack_capacity * 2 : 8;
    while (new_capacity < required_depth) {
        new_capacity *= 2;
    }

    resized_stack = realloc(state->load_stack, sizeof(int) * (size_t)new_capacity);
    if (!resized_stack) {
        return 0;
    }

    state->load_stack = resized_stack;
    state->load_stack_capacity = new_capacity;
    return 1;
}

int push_loading_file(CompilationState* state, int file_index) {
    if (!ensure_load_stack_capacity(state, state->load_depth + 1)) {
        return 0;
    }

    state->load_stack[state->load_depth++] = file_index;
    return 1;
}

void pop_loading_file(CompilationState* state) {
    if (state->load_depth > 0) {
        state->load_depth--;
    }
}

void report_import_cycle(const CompilationState* state, int repeated_index) {
    int cycle_start = -1;
    int i;

    for (i = 0; i < state->load_depth; i++) {
        if (state->load_stack[i] == repeated_index) {
            cycle_start = i;
            break;
        }
    }

    if (cycle_start < 0) {
        fprintf(stderr, "import cycle detected involving %s\n", state->file_paths[repeated_index]);
        return;
    }

    fprintf(stderr, "import cycle detected: ");
    for (i = cycle_start; i < state->load_depth; i++) {
        fprintf(stderr, "%s -> ", state->file_paths[state->load_stack[i]]);
    }
    fprintf(stderr, "%s\n", state->file_paths[repeated_index]);
}

/* Like report_import_cycle but prefixes the message with the file:line:col
 * of the import statement that closed the cycle, matching the format used
 * by parser and semantic errors so the user can jump to it directly. */
void report_import_cycle_at(const CompilationState* state, int repeated_index,
                            const char* importing_file, int line, int column) {
    int cycle_start = -1;
    int i;

    for (i = 0; i < state->load_depth; i++) {
        if (state->load_stack[i] == repeated_index) {
            cycle_start = i;
            break;
        }
    }

    fprintf(stderr, "%s:%d:%d: import cycle: ", importing_file, line, column);

    if (cycle_start < 0) {
        fprintf(stderr, "%s already imported\n", state->file_paths[repeated_index]);
        return;
    }

    for (i = cycle_start; i < state->load_depth; i++) {
        fprintf(stderr, "%s -> ", state->file_paths[state->load_stack[i]]);
    }
    fprintf(stderr, "%s\n", state->file_paths[repeated_index]);
}

int find_loaded_file(const CompilationState* state, const char* normalized_path) {
    int i;

    for (i = 0; i < state->count; i++) {
        if (strcmp(state->file_paths[i], normalized_path) == 0) {
            return i;
        }
    }

    return -1;
}

int reserve_file_slot(CompilationState* state, char* normalized_path) {
    int index;

    if (!ensure_state_capacity(state, state->count + 1)) {
        return -1;
    }

    index = state->count++;
    state->file_paths[index] = normalized_path;
    state->sources[index] = NULL;
    state->lexers[index] = NULL;
    state->parsers[index] = NULL;
    state->load_states[index] = FILE_NOT_LOADED;
    return index;
}

void free_compilation_state(CompilationState* state) {
    int i;

    for (i = 0; i < state->count; i++) {
        parser_free(state->parsers[i]);
        lexer_free(state->lexers[i]);
        free(state->sources[i]);
        free(state->file_paths[i]);
    }

    free(state->parsers);
    free(state->lexers);
    free(state->sources);
    free(state->file_paths);
    free(state->load_states);
    free(state->load_stack);
    /* Sprint 4: free module registry. */
    lamo_modules_free(&state->modules);
}

/* Sprint 4: rename every top-level declaration in `program` by prefixing
 * its name with `lamo_mod_<alias>__`. This is what makes namespaced
 * imports actually namespace — without it, `import "math.lamo" as math;`
 * would still merge math.lamo's `sqrt` into the global `sqrt` slot,
 * defeating the whole point of the alias.
 *
 * We rename in-place on the AST (the ASTFnDecl.name / ASTVarDecl.name
 * strings are freed and replaced). We also register each member in the
 * module registry so semantic + codegen can resolve `alias.member(args)`
 * calls.
 *
 * After renaming the top-level decls, we ALSO walk each function body
 * and rewrite references to the module's own members (recursive calls,
 * references to module globals, etc.) to use the prefixed names. Without
 * this, `fn fib(n) { return fib(n-1); }` would have its outer name
 * renamed to `lamo_mod_math__fib` but the inner call would still say
 * `fib`, which the semantic pass would reject as undeclared.
 *
 * `program` is the parsed program from the imported file; we walk its
 * top-level declarations list (program->declarations). Nested function
 * declarations are NOT renamed — only top-level decls are exposed
 * through the module namespace, matching the import semantics of every
 * other namespaced language. */

/* Helper: rewrite a single name string in-place if it matches one of
 * the module's original names. Returns 1 if rewritten, 0 otherwise.
 * `names` is a flat array of (original, prefixed) pairs, `count` is
 * the number of pairs. */
int rewrite_name_if_member(char** name_slot,
                            const LamoModuleEntry* entry) {
    int i;
    if (!name_slot || !*name_slot || !entry) return 0;
    for (i = 0; i < entry->member_count; i++) {
        if (strcmp(*name_slot, entry->members[i].original_name) == 0) {
            free(*name_slot);
            *name_slot = strdup(entry->members[i].prefixed_name);
            return 1;
        }
    }
    return 0;
}

/* Recursive walker: rewrite references to the module's members inside
 * an AST subtree. Visits calls, identifiers, assignments, and all
 * structural nodes. */
void rewrite_member_refs_recursive(ASTNode* node, const LamoModuleEntry* entry) {
    if (!node) return;
    for (ASTNode* cur = node; cur; cur = cur->next) {
        switch (cur->type) {
            case AST_CALL_STMT: {
                ASTCallStmt* cs = (ASTCallStmt*)cur;
                rewrite_name_if_member(&cs->name, entry);
                for (int i = 0; i < cs->arg_count; i++) {
                    rewrite_member_refs_recursive(cs->args[i], entry);
                }
                break;
            }
            case AST_CALL_EXPR: {
                ASTCallExpr* ce = (ASTCallExpr*)cur;
                rewrite_name_if_member(&ce->name, entry);
                for (int i = 0; i < ce->arg_count; i++) {
                    rewrite_member_refs_recursive(ce->args[i], entry);
                }
                break;
            }
            case AST_IDENTIFIER: {
                ASTIdentifier* id = (ASTIdentifier*)cur;
                rewrite_name_if_member(&id->name, entry);
                break;
            }
            case AST_ASSIGN_STMT: {
                ASTAssignStmt* as = (ASTAssignStmt*)cur;
                rewrite_name_if_member(&as->name, entry);
                rewrite_member_refs_recursive(as->value, entry);
                break;
            }
            case AST_VAR_DECL: {
                ASTVarDecl* vd = (ASTVarDecl*)cur;
                rewrite_member_refs_recursive(vd->initializer, entry);
                break;
            }
            case AST_FN_DECL: {
                ASTFnDecl* fn = (ASTFnDecl*)cur;
                /* Don't rewrite the function's own name here — that was
                 * already done by the outer loop in rename_module_declarations.
                 * But DO rewrite references to OTHER module members in the
                 * body. Also rewrite parameter names if they shadow module
                 * members — but that's actually a semantic error (you
                 * can't have a param named the same as a module member),
                 * so we leave them alone and let the semantic pass complain.
                 *
                 * Also: do NOT descend into nested fn declarations
                 * (they have their own scope). We only rewrite refs in
                 * the body of THIS function, treating nested fns as
                 * opaque. The semantic pass will visit them separately.
                 * Actually, the nested fn's body would also need
                 * rewriting if it references the module's members. So
                 * we DO descend, but skip the nested fn's NAME (which
                 * would have been a top-level decl if it were one).
                 * Since nested fns aren't renamed, their NAME doesn't
                 * match any module member anyway. So just descend. */
                rewrite_member_refs_recursive(fn->body, entry);
                break;
            }
            case AST_BLOCK: {
                rewrite_member_refs_recursive(((ASTBlock*)cur)->statements, entry);
                break;
            }
            case AST_IF_STMT: {
                ASTIfStmt* is = (ASTIfStmt*)cur;
                rewrite_member_refs_recursive(is->condition, entry);
                rewrite_member_refs_recursive(is->then_branch, entry);
                rewrite_member_refs_recursive(is->else_branch, entry);
                break;
            }
            case AST_WHILE_STMT: {
                ASTWhileStmt* ws = (ASTWhileStmt*)cur;
                rewrite_member_refs_recursive(ws->condition, entry);
                rewrite_member_refs_recursive(ws->body, entry);
                break;
            }
            case AST_FOR_STMT: {
                ASTForStmt* fs = (ASTForStmt*)cur;
                rewrite_member_refs_recursive(fs->initializer, entry);
                rewrite_member_refs_recursive(fs->condition, entry);
                rewrite_member_refs_recursive(fs->increment, entry);
                rewrite_member_refs_recursive(fs->body, entry);
                break;
            }
            case AST_RETURN_STMT: {
                rewrite_member_refs_recursive(((ASTReturnStmt*)cur)->expression, entry);
                break;
            }
            case AST_BINARY_EXPR: {
                ASTBinaryExpr* be = (ASTBinaryExpr*)cur;
                rewrite_member_refs_recursive(be->left, entry);
                rewrite_member_refs_recursive(be->right, entry);
                break;
            }
            case AST_UNARY_EXPR: {
                rewrite_member_refs_recursive(((ASTUnaryExpr*)cur)->right, entry);
                break;
            }
            case AST_GROUPING_EXPR: {
                rewrite_member_refs_recursive(((ASTGroupingExpr*)cur)->expression, entry);
                break;
            }
            case AST_ARRAY_LITERAL: {
                ASTArrayLiteral* arr = (ASTArrayLiteral*)cur;
                for (int i = 0; i < arr->element_count; i++) {
                    rewrite_member_refs_recursive(arr->elements[i], entry);
                }
                break;
            }
            case AST_INDEX_EXPR: {
                ASTIndexExpr* ie = (ASTIndexExpr*)cur;
                rewrite_member_refs_recursive(ie->array, entry);
                rewrite_member_refs_recursive(ie->index, entry);
                break;
            }
            case AST_PROP_EXPR: {
                rewrite_member_refs_recursive(((ASTPropExpr*)cur)->object, entry);
                break;
            }
            case AST_MEMBER_CALL: {
                ASTMemberCall* mc = (ASTMemberCall*)cur;
                rewrite_member_refs_recursive(mc->object, entry);
                for (int i = 0; i < mc->arg_count; i++) {
                    rewrite_member_refs_recursive(mc->args[i], entry);
                }
                break;
            }
            /* Phase 3 (stdlib): handle AST_PLACE_ASSIGN_STMT (e.g.
             * `arr[i] = value` and `obj.field = value`) so that references
             * to module members inside these assignment targets and values
             * are correctly rewritten. Without this, a function body that
             * does `_timer_labels[j] = _timer_labels[j + 1]` would not
             * have its `_timer_labels` references renamed, causing
             * "undeclared variable" errors at semantic time. */
            case AST_PLACE_ASSIGN_STMT: {
                ASTPlaceAssignStmt* pa = (ASTPlaceAssignStmt*)cur;
                rewrite_member_refs_recursive(pa->target, entry);
                rewrite_member_refs_recursive(pa->value, entry);
                break;
            }
            /* Leaves: AST_INT_LITERAL, AST_FLOAT_LITERAL, AST_STRING_LITERAL,
             * AST_BOOL_LITERAL, AST_IMPORT, AST_BREAK_STMT, AST_CONTINUE_STMT.
             * Nothing to rewrite. */
            default:
                break;
        }
    }
}

int rename_module_declarations(ASTProgram* program, const char* alias,
                               LamoModuleRegistry* reg) {
    char* prefix = lamo_module_prefix(alias);
    ASTNode* cur;
    const LamoModuleEntry* entry;
    if (!prefix) return 0;

    /* Phase 1: rename top-level decls and register them in the module. */
    for (cur = program->declarations; cur; cur = cur->next) {
        char* new_name;
        const char* original_name;
        int arity = -1;
        if (cur->type == AST_FN_DECL) {
            ASTFnDecl* fn = (ASTFnDecl*)cur;
            original_name = fn->name;
            arity = fn->param_count;
            new_name = malloc(strlen(prefix) + strlen(original_name) + 1);
            if (!new_name) { free(prefix); return 0; }
            sprintf(new_name, "%s%s", prefix, original_name);
            {
                char* orig_copy = strdup(original_name);
                if (!orig_copy) { free(new_name); free(prefix); return 0; }
                if (!lamo_modules_add_member(reg, alias, orig_copy, new_name, arity)) {
                    free(orig_copy);
                    free(new_name);
                    free(prefix);
                    return 0;
                }
            }
            free(fn->name);
            fn->name = new_name;
        } else if (cur->type == AST_VAR_DECL) {
            ASTVarDecl* vd = (ASTVarDecl*)cur;
            original_name = vd->name;
            arity = -1;  /* non-function member */
            new_name = malloc(strlen(prefix) + strlen(original_name) + 1);
            if (!new_name) { free(prefix); return 0; }
            sprintf(new_name, "%s%s", prefix, original_name);
            {
                char* orig_copy = strdup(original_name);
                if (!orig_copy) { free(new_name); free(prefix); return 0; }
                if (!lamo_modules_add_member(reg, alias, orig_copy, new_name, arity)) {
                    free(orig_copy);
                    free(new_name);
                    free(prefix);
                    return 0;
                }
            }
            free(vd->name);
            vd->name = new_name;
        }
        /* Skip AST_IMPORT and other node types — we don't expose them
         * through the module namespace. */
    }

    /* Phase 2: now that all members are registered, look up the entry
     * and walk each top-level decl's body to rewrite references to the
     * module's own members (recursive calls, references to module
     * globals, etc.). Without this, `fn fib(n) { return fib(n-1); }`
     * would have its outer name renamed but the inner call would still
     * say `fib`, causing a semantic error. */
    entry = lamo_modules_lookup_alias(reg, alias);
    if (entry) {
        for (cur = program->declarations; cur; cur = cur->next) {
            if (cur->type == AST_FN_DECL) {
                ASTFnDecl* fn = (ASTFnDecl*)cur;
                rewrite_member_refs_recursive(fn->body, entry);
            } else if (cur->type == AST_VAR_DECL) {
                ASTVarDecl* vd = (ASTVarDecl*)cur;
                rewrite_member_refs_recursive(vd->initializer, entry);
            }
            /* Top-level call statements, assignments, etc. also need
             * rewriting if they reference module members. */
            if (cur->type == AST_CALL_STMT || cur->type == AST_ASSIGN_STMT ||
                cur->type == AST_RETURN_STMT || cur->type == AST_IF_STMT ||
                cur->type == AST_WHILE_STMT || cur->type == AST_FOR_STMT) {
                rewrite_member_refs_recursive(cur, entry);
            }
        }
    }

    free(prefix);
    return 1;
}

// Caminha a AST já parseada procurando nós AST_IMPORT e carrega cada
// dependência recursivamente. Substitui o antigo pré-processamento textual de
// imports — agora respeita comentários e strings porque o lexer cuidou disso.
int load_imports_from_ast(CompilationState* state, ASTProgram* aggregate_program, ASTNode* node, const char* importing_file) {
    if (!node) {
        return 1;
    }

    for (ASTNode* current = node; current; current = current->next) {
        if (current->type == AST_IMPORT) {
            ASTImport* imp = (ASTImport*)current;
            char* resolved = resolve_import_path(importing_file, imp->path);
            if (!resolved) {
                fprintf(stderr, "%s:%d:%d: failed to resolve import \"%s\"\n",
                        importing_file, current->line, current->column, imp->path);
                return 0;
            }
            /* Sprint 4: pass the alias through to the loader so it knows
             * whether to rename the imported declarations (aliased import)
             * or merge them as-is (legacy global-merge import). */
            if (!load_program_recursive_from(state, aggregate_program, resolved,
                                              importing_file, current->line, current->column,
                                              imp->alias)) {
                free(resolved);
                return 0;
            }
            free(resolved);
        }
    }
    return 1;
}

int load_program_recursive(CompilationState* state, ASTProgram* aggregate_program, const char* path) {
    char* normalized_path = normalize_path(path);
    char* raw_source;
    ASTProgram* parsed_program;
    int slot;
    int existing_index;
    int success = 0;

    if (!normalized_path) {
        fprintf(stderr, "failed to resolve %s: %s\n", path, strerror(errno));
        return 0;
    }

    existing_index = find_loaded_file(state, normalized_path);
    if (existing_index >= 0) {
        if (state->load_states[existing_index] == FILE_LOADING) {
            report_import_cycle(state, existing_index);
            free(normalized_path);
            return 0;
        }

        free(normalized_path);
        return 1;
    }

    slot = reserve_file_slot(state, normalized_path);
    if (slot < 0) {
        fprintf(stderr, "failed to allocate compiler state for imported files\n");
        free(normalized_path);
        return 0;
    }

    state->load_states[slot] = FILE_LOADING;
    if (!push_loading_file(state, slot)) {
        fprintf(stderr, "failed to track import stack for %s\n", normalized_path);
        state->load_states[slot] = FILE_NOT_LOADED;
        return 0;
    }

    raw_source = read_file(normalized_path);
    if (!raw_source) {
        fprintf(stderr, "failed to read %s: %s\n", normalized_path, strerror(errno));
        goto cleanup;
    }

    // Agora o source vai direto pro lexer — sem strip_imports_and_load_dependencies.
    // Imports viram tokens TOKEN_IMPORT e nós AST_IMPORT, carregados depois.
    state->sources[slot] = raw_source;
    state->lexers[slot] = lexer_init(raw_source);
    // Bug #4 fix: passar o path do arquivo pro parser para que erros sintáticos
    // incluam a origem. Bug #5 fix: o parser também seta o default file path
    // para que todos os nós da AST carreguem essa origem, permitindo ao
    // semântico reportar erros multi-arquivo corretamente.
    state->parsers[slot] = parser_init_with_file(state->lexers[slot], normalized_path);
    parsed_program = parse_program_v2(state->parsers[slot]);

    if (parser_had_error(state->parsers[slot])) {
        fprintf(stderr, "%s: %d syntax error(s)\n",
                normalized_path,
                parser_error_count(state->parsers[slot]));
        // Mesmo com erros, tenta carregar imports dos pedaços que parsearam
        // para dar mais diagnóstico ao usuário. Mas falha a compilação.
        load_imports_from_ast(state, aggregate_program, parsed_program->declarations, normalized_path);
        ast_free((ASTNode*)parsed_program);
        goto cleanup;
    }

    // Carrega dependências antes de mesclar — preserva ordem de imports.
    if (!load_imports_from_ast(state, aggregate_program, parsed_program->declarations, normalized_path)) {
        ast_free((ASTNode*)parsed_program);
        goto cleanup;
    }

    ast_program_append(aggregate_program, parsed_program);
    state->load_states[slot] = FILE_LOADED;
    success = 1;

cleanup:
    pop_loading_file(state);
    if (!success && slot >= 0) {
        state->load_states[slot] = FILE_NOT_LOADED;
    }
    return success;
}

/* Variant called when loading an import that originated from a specific AST
 * node — carries the import site location so cycle errors point at the
 * exact `import "..."` statement that closed the cycle.
 *
 * Sprint 4: also carries the optional alias. When non-NULL, the imported
 * file's top-level declarations are renamed to `lamo_mod_<alias>__<name>`
 * before being merged into the aggregate program, and each one is
 * registered in state->modules so the semantic pass and codegen can
 * resolve `alias.member(args)` calls. */
int load_program_recursive_from(CompilationState* state, ASTProgram* aggregate_program,
                                const char* path, const char* imported_from,
                                int import_line, int import_column,
                                const char* alias) {
    char* normalized_path = normalize_path(path);
    char* raw_source;
    ASTProgram* parsed_program;
    int slot;
    int existing_index;
    int success = 0;

    if (!normalized_path) {
        fprintf(stderr, "%s:%d:%d: failed to resolve import: %s\n",
                imported_from, import_line, import_column, strerror(errno));
        return 0;
    }

    existing_index = find_loaded_file(state, normalized_path);
    if (existing_index >= 0) {
        if (state->load_states[existing_index] == FILE_LOADING) {
            report_import_cycle_at(state, existing_index,
                                   imported_from, import_line, import_column);
            free(normalized_path);
            return 0;
        }
        /* Sprint 4: the same file is being imported again (not a cycle,
         * just a re-import). If the new import has an alias, we still
         * need to register the alias → members mapping in the module
         * registry. The declarations were already merged on the first
         * load — but if the first load was aliased and the second isn't
         * (or vice versa), or the aliases differ, we honor the FIRST
         * load's alias (the file's declarations are already renamed).
         * This is a documented limitation: importing the same file twice
         * with different aliases is not supported.
         *
         * Import-time duplicate rule (Phase 10 / SPEC §10): a bare
         * re-import is a harmless no-op — dedupe is by normalized path —
         * but we WARN so accidental double imports are visible. */
        fprintf(stderr, "%s:%d:%d: warning: file \"%s\" already imported; duplicate import ignored%s\n",
                imported_from, import_line, import_column, path,
                alias ? " (re-import with a different alias is not supported)" : "");
        free(normalized_path);
        return 1;
    }

    slot = reserve_file_slot(state, normalized_path);
    if (slot < 0) {
        fprintf(stderr, "failed to allocate compiler state for imported files\n");
        free(normalized_path);
        return 0;
    }

    state->load_states[slot] = FILE_LOADING;
    if (!push_loading_file(state, slot)) {
        fprintf(stderr, "failed to track import stack for %s\n", normalized_path);
        state->load_states[slot] = FILE_NOT_LOADED;
        return 0;
    }

    raw_source = read_file(normalized_path);
    if (!raw_source) {
        fprintf(stderr, "%s:%d:%d: cannot open imported file \"%s\": %s\n",
                imported_from, import_line, import_column, path, strerror(errno));
        goto cleanup;
    }

    state->sources[slot] = raw_source;
    state->lexers[slot] = lexer_init(raw_source);
    state->parsers[slot] = parser_init_with_file(state->lexers[slot], normalized_path);
    parsed_program = parse_program_v2(state->parsers[slot]);

    if (parser_had_error(state->parsers[slot])) {
        fprintf(stderr, "%s: %d syntax error(s)\n",
                normalized_path,
                parser_error_count(state->parsers[slot]));
        load_imports_from_ast(state, aggregate_program, parsed_program->declarations, normalized_path);
        ast_free((ASTNode*)parsed_program);
        goto cleanup;
    }

    if (!load_imports_from_ast(state, aggregate_program, parsed_program->declarations, normalized_path)) {
        ast_free((ASTNode*)parsed_program);
        goto cleanup;
    }

    /* Sprint 4: if this import had an alias, rename the parsed file's
     * top-level declarations before merging. This must happen AFTER
     * load_imports_from_ast so that any imports inside the module file
     * are resolved normally (their symbols merge into the global
     * namespace, NOT the module namespace — same as Python's `from x
     * import *` inside a module). */
    if (alias) {
        if (!lamo_modules_register_alias(&state->modules, alias)) {
            fprintf(stderr, "%s:%d:%d: failed to register module alias `%s` "
                    "(duplicate alias or out of memory)\n",
                    imported_from, import_line, import_column, alias);
            ast_free((ASTNode*)parsed_program);
            goto cleanup;
        }
        if (!rename_module_declarations(parsed_program, alias, &state->modules)) {
            fprintf(stderr, "%s:%d:%d: failed to rename declarations for module `%s`\n",
                    imported_from, import_line, import_column, alias);
            ast_free((ASTNode*)parsed_program);
            goto cleanup;
        }
    }

    ast_program_append(aggregate_program, parsed_program);
    state->load_states[slot] = FILE_LOADED;
    success = 1;

cleanup:
    pop_loading_file(state);
    if (!success && slot >= 0) {
        state->load_states[slot] = FILE_NOT_LOADED;
    }
    return success;
}
