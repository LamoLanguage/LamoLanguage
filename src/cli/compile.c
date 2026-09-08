/*
 * compile.c — The compile pipeline (run / build / check / eval) for the
 * `lamo` CLI. See compile.h for the design rationale.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include "compile.h"
#include "cli_options.h"
#include "paths.h"
#include "import_resolver.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "semantic.h"
#include "builtins.h"
#include "eval/eval.h"

/* Forward declarations: defined in src/codegen/codegen.c. We forward-
 * declare instead of #include-ing codegen.h to avoid pulling codegen's
 * transitive deps (lamo_runtime.h, lamo_runtime_data.h, math.h, ...)
 * into this file, matching the existing pattern. */
void generate_c_code(ASTNode* node, FILE* out);
void codegen_set_module_registry(LamoModuleRegistry* reg);

// Verifica se a AST usa GUI builtins (para linkar -lX11 no Linux).
// Sprint 2 refactor: the name check now delegates to the shared table in
// builtins.h (lamo_builtin_is_gui), so adding a new GUI builtin only
// requires editing builtins.h — this code path picks it up automatically.
//
// Bug #8 fix (preserved): we compare against the exact builtin list rather
// than using strncmp(name, "gui_", 4) == 0, which would have incorrectly
// linked -lX11 for any user-defined function starting with "gui_".
int lamo_program_uses_gui_recursive(ASTNode* node) {
    if (!node) return 0;
    switch (node->type) {
        case AST_PROGRAM: {
            for (ASTNode* c = ((ASTProgram*)node)->declarations; c; c = c->next) {
                if (lamo_program_uses_gui_recursive(c)) return 1;
            }
            return 0;
        }
        case AST_VAR_DECL:
            return lamo_program_uses_gui_recursive(((ASTVarDecl*)node)->initializer);
        case AST_FN_DECL:
            return lamo_program_uses_gui_recursive(((ASTFnDecl*)node)->body);
        case AST_BLOCK: {
            for (ASTNode* c = ((ASTBlock*)node)->statements; c; c = c->next) {
                if (lamo_program_uses_gui_recursive(c)) return 1;
            }
            return 0;
        }
        case AST_IF_STMT: {
            ASTIfStmt* n = (ASTIfStmt*)node;
            return lamo_program_uses_gui_recursive(n->condition) ||
                   lamo_program_uses_gui_recursive(n->then_branch) ||
                   lamo_program_uses_gui_recursive(n->else_branch);
        }
        case AST_WHILE_STMT: {
            ASTWhileStmt* n = (ASTWhileStmt*)node;
            return lamo_program_uses_gui_recursive(n->condition) ||
                   lamo_program_uses_gui_recursive(n->body);
        }
        case AST_FOR_STMT: {
            ASTForStmt* n = (ASTForStmt*)node;
            return lamo_program_uses_gui_recursive(n->initializer) ||
                   lamo_program_uses_gui_recursive(n->condition) ||
                   lamo_program_uses_gui_recursive(n->increment) ||
                   lamo_program_uses_gui_recursive(n->body);
        }
        case AST_RETURN_STMT:
            return lamo_program_uses_gui_recursive(((ASTReturnStmt*)node)->expression);
        case AST_ASSIGN_STMT:
            return lamo_program_uses_gui_recursive(((ASTAssignStmt*)node)->value);
        case AST_CALL_STMT: {
            ASTCallStmt* n = (ASTCallStmt*)node;
            if (lamo_builtin_is_gui(n->name)) return 1;
            for (int i = 0; i < n->arg_count; i++) {
                if (lamo_program_uses_gui_recursive(n->args[i])) return 1;
            }
            return 0;
        }
        case AST_CALL_EXPR: {
            ASTCallExpr* n = (ASTCallExpr*)node;
            if (lamo_builtin_is_gui(n->name)) return 1;
            for (int i = 0; i < n->arg_count; i++) {
                if (lamo_program_uses_gui_recursive(n->args[i])) return 1;
            }
            return 0;
        }
        case AST_BINARY_EXPR: {
            ASTBinaryExpr* n = (ASTBinaryExpr*)node;
            return lamo_program_uses_gui_recursive(n->left) ||
                   lamo_program_uses_gui_recursive(n->right);
        }
        case AST_UNARY_EXPR:
            return lamo_program_uses_gui_recursive(((ASTUnaryExpr*)node)->right);
        case AST_GROUPING_EXPR:
            return lamo_program_uses_gui_recursive(((ASTGroupingExpr*)node)->expression);
        case AST_ARRAY_LITERAL: {
            /* Sprint 3: walk array literal elements for GUI builtin usage. */
            ASTArrayLiteral* arr = (ASTArrayLiteral*)node;
            int i;
            for (i = 0; i < arr->element_count; i++) {
                if (lamo_program_uses_gui_recursive(arr->elements[i])) return 1;
            }
            return 0;
        }
        case AST_INDEX_EXPR: {
            ASTIndexExpr* idx = (ASTIndexExpr*)node;
            return lamo_program_uses_gui_recursive(idx->array) ||
                   lamo_program_uses_gui_recursive(idx->index);
        }
        case AST_PROP_EXPR:
            return lamo_program_uses_gui_recursive(((ASTPropExpr*)node)->object);
        default:
            return 0;
    }
}

int program_uses_gui(ASTProgram* program) {
    return lamo_program_uses_gui_recursive((ASTNode*)program);
}

/* Sprint 3: source-lookup callback for the semantic analyzer.
 *
 * Given a file path, returns the source text that was loaded for it
 * (stored in CompilationState.sources[]). Returns NULL if the path is
 * not known to the compilation state — in that case the semantic
 * analyzer just omits the source snippet for that error.
 *
 * This is what threads the source text from the loader (import_resolver.c)
 * into the semantic analyzer (semantic.c) without leaking the
 * CompilationState type into semantic.h. */
const char* lamo_source_lookup(const char* file_path, void* user_data) {
    CompilationState* state = (CompilationState*)user_data;
    int i;
    if (!state || !file_path) {
        return NULL;
    }
    for (i = 0; i < state->count; i++) {
        if (state->file_paths[i] && strcmp(state->file_paths[i], file_path) == 0) {
            return state->sources[i];
        }
    }
    return NULL;
}

/* Sprint 4: module-resolution callback for the semantic analyzer.
 * Delegates to the registry stored in CompilationState. Given an alias
 * and a member name, returns the prefixed function name (e.g.
 * "lamo_mod_math__sqrt") that the codegen will emit a call to. */
const char* lamo_module_resolve_cb(const char* alias, const char* member, void* user_data) {
    CompilationState* state = (CompilationState*)user_data;
    if (!state || !alias || !member) return NULL;
    return lamo_modules_resolve_member(&state->modules, alias, member);
}

/* Sprint 4: module-arity callback. Delegates to the registry's stored
 * arity (captured at rename time from the ASTFnDecl's param_count).
 * Returns -1 for non-function members or unknown alias/member — the
 * semantic pass treats -1 as "skip arity check". */
int lamo_module_arity_cb(const char* alias, const char* member, void* user_data) {
    CompilationState* state = (CompilationState*)user_data;
    if (!state || !alias || !member) return -1;
    return lamo_modules_resolve_arity(&state->modules, alias, member);
}

int compile_sources(const char** input_files, int input_file_count, LamoCommand command, const char* output_path) {
    ASTProgram* program_ast = ast_new_program();
    CompilationState state;
    const char* semantic_label = NULL;
    int exit_code = EXIT_SUCCESS_CODE;
    int i;

    memset(&state, 0, sizeof(state));
    /* Sprint 4: initialize the module registry. Tracks aliased imports
     * (`import "..." as alias;`) so semantic + codegen can resolve
     * `alias.member(args)` calls. */
    lamo_modules_init(&state.modules);

    // Bug #5 fix: o label aqui é só fallback. O semântico agora usa
    // node->file_path de cada nó da AST (setado pelo parser) para reportar
    // erros no arquivo correto. Em compilações multi-arquivo (programa
    // principal + imports), isso significa que o erro aponta para o arquivo
    // onde o problema realmente está. O label abaixo só aparece em nós
    // sintéticos ou se algum caminho não foi setado.
    semantic_label = input_file_count == 1 ? input_files[0] : "<multiple inputs>";

    for (i = 0; i < input_file_count; i++) {
        if (!load_program_recursive(&state, program_ast, input_files[i])) {
            exit_code = EXIT_COMPILE_ERROR;
            goto cleanup;
        }
    }

    if (!semantic_analyze_full(program_ast, semantic_label,
                                lamo_source_lookup, &state,
                                lamo_module_resolve_cb,
                                lamo_module_arity_cb,
                                &state)) {
        exit_code = EXIT_COMPILE_ERROR;
        goto cleanup;
    }

    if (command == COMMAND_CHECK) {
        if (!cli_quiet()) {
            if (input_file_count == 1) {
                printf("check passed: %s\n", input_files[0]);
            } else {
                printf("check passed: %d file(s)\n", input_file_count);
            }
        }
        goto cleanup;
    }

    if (command == COMMAND_EVAL) {
        EvalEnv* env = eval_env_new(NULL);
        int ok = eval_program(program_ast, env);
        eval_env_free(env);
        if (!ok) exit_code = EXIT_BACKEND_ERROR;
        goto cleanup;
    }

    {
        /* Quick win (todo.md Phase 1): .c emission policy.
         *
         * Decision: ALWAYS emit lamo_exec.c to the current directory,
         * for both `run` and `build` modes. Rationale:
         *
         *   1. Debuggability: if compilation fails, the user can inspect
         *      the generated C to understand what the transpiler produced.
         *      This is essential for a transpiled language — the .c IS
         *      the intermediate representation.
         *   2. Transparency: Lamo's value proposition is "as fast as C,
         *      because it IS C". Hiding the .c would undermine that.
         *   3. Consistency: same behavior in run and build modes means
         *      users don't have to remember two policies.
         *
         * The cost (one extra file in the working directory) is minor
         * and documented. Users who want a clean workflow can:
         *   - `lamo check` to validate without generating .c
         *   - `lamo build -o out/bin && rm lamo_exec.c` for clean builds
         *   - add `lamo_exec.c` to .gitignore (lamo init does this)
         *
         * Future: a `--emit-c=no` flag could be added if the noise
         * becomes a real complaint. Not worth the complexity today. */
        const char* c_output_path = "lamo_exec.c";
        const char* binary_output_path = output_path ? output_path : "lamo_exec";
        FILE* out = fopen(c_output_path, "w");
        int needs_gui;
        int exit_status;
        char binary_with_suffix[1024];
        const char* cc = lamo_cc();

        if (!out) {
            fprintf(stderr, "failed to open %s for writing: %s\n", c_output_path, strerror(errno));
            exit_code = EXIT_COMPILE_ERROR;
            goto cleanup;
        }

        /* Sprint 4: register the module registry with the codegen so
         * AST_MEMBER_CALL nodes can resolve `alias.member(args)` to the
         * prefixed function name. The pointer is borrowed — the
         * registry lives in CompilationState which outlives this call. */
        codegen_set_module_registry(&state.modules);
        generate_c_code((ASTNode*)program_ast, out);
        /* Clear the registry pointer to avoid dangling references on
         * subsequent compile_sources() calls (defensive — the static
         * would be reused otherwise). */
        codegen_set_module_registry(NULL);
        fclose(out);

        if (cli_verbose()) {
            printf("[verbose] wrote C source: %s\n", c_output_path);
        }

        needs_gui = program_uses_gui(program_ast);
        if (cli_verbose() && needs_gui) {
            printf("[verbose] program uses GUI builtins; will link X11/GDI\n");
        }

        /* Build argv for the C compiler. Using execvp/CreateProcess instead of
         * system() means user-controlled paths (which only the compiler itself
         * supplies here, but the binary name is also derived from -o output_path
         * that the user can pass) are passed verbatim to the OS — no shell
         * metacharacters can ever be interpreted. */
        snprintf(binary_with_suffix, sizeof(binary_with_suffix),
                 "%s%s", binary_output_path, executable_suffix());

        {
            char c_output_local[256];
            snprintf(c_output_local, sizeof(c_output_local), "%s", c_output_path);
            if (cli_verbose()) {
                printf("[verbose] invoking C compiler: %s -Wall -Wextra -std=c99 -o %s %s\n",
                       cc, binary_with_suffix, c_output_local);
            }
#ifdef _WIN32
            (void)needs_gui;
            /* On Windows the GUI runtime is always linked (GDI32 is always
             * present), so we don't gate on needs_gui. */
            char* argv[] = {
                (char*)cc, "-Wall", "-Wextra", "-std=c99",
                "-o", binary_with_suffix, c_output_local,
                "-lgdi32", "-luser32", "-lws2_32", "-lm",
                NULL
            };
            exit_status = run_argv(argv);
#else
            if (needs_gui) {
                char* argv[] = {
                    (char*)cc, "-Wall", "-Wextra", "-std=c99",
                    "-o", binary_with_suffix, c_output_local,
                    "-lX11", "-lm",
                    NULL
                };
                exit_status = run_argv(argv);
            } else {
                char* argv[] = {
                    (char*)cc, "-Wall", "-Wextra", "-std=c99",
                    "-o", binary_with_suffix, c_output_local,
                    "-lm",
                    NULL
                };
                exit_status = run_argv(argv);
            }
#endif
        }

        if (exit_status != 0) {
            /* Quick win (todo.md Phase 1): surface GCC failures clearly.
             * The user sees GCC's stderr directly (we use execvp, so the
             * child inherits stderr), but we add a clear header pointing
             * at the generated C source and the LAMO_CC override. This
             * turns "lamo_exec.c:42: error: ..." into:
             *
             *   lamo: backend compilation failed (gcc exit code 1)
             *   lamo: generated C source: lamo_exec.c (inspect for details)
             *   lamo: if the C compiler is wrong, set LAMO_CC=/path/to/cc
             *   lamo: rerun with --verbose to see the full cc invocation
             *
             * followed by GCC's own error output. */
            fprintf(stderr, "lamo: backend compilation failed (%s returned exit code %d)\n",
                    cc, exit_status);
            fprintf(stderr, "lamo: generated C source: %s (inspect for details)\n",
                    c_output_path);
            fprintf(stderr, "lamo: if the C compiler is wrong, set LAMO_CC=/path/to/cc (current: %s)\n",
                    cc);
            fprintf(stderr, "lamo: rerun with --verbose to see the full cc invocation\n");
            exit_code = EXIT_BACKEND_ERROR;
            goto cleanup;
        }

        if (command == COMMAND_BUILD) {
            if (!cli_quiet()) {
                printf("build succeeded: %s\n", binary_with_suffix);
            }
            goto cleanup;
        }

        /* Execute the compiled binary. Again, run via argv rather than
         * system() so that user-supplied -o paths cannot inject shell
         * metacharacters. On POSIX, prefix with "./" because PATH lookup
         * would not find a file in the current directory. */
        {
#ifdef _WIN32
            char* argv[] = { binary_with_suffix, NULL };
#else
            char dot_slash[2048];
            snprintf(dot_slash, sizeof(dot_slash), "./%s", binary_with_suffix);
            char* argv[] = { dot_slash, NULL };
#endif
            exit_status = run_argv(argv);
        }
        if (exit_status != 0) {
            fprintf(stderr, "program exited with a non-zero status: %s\n",
                    binary_with_suffix);
            exit_code = EXIT_BACKEND_ERROR;
            goto cleanup;
        }
    }

cleanup:
    ast_free((ASTNode*)program_ast);
    free_compilation_state(&state);
    return exit_code;
}
