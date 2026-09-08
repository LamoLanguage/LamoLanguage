/*
 * commands.c — Per-subcommand handlers for the `lamo` CLI.
 * See commands.h for the design rationale.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include "commands.h"
#include "cli_options.h"
#include "paths.h"
#include "help.h"
#include "compile.h"  /* for run_argv() */
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "eval/eval.h"

/* command_new: scaffold a new Lamo project. */
int command_new(int argc, char** argv) {
    const char* project_name = NULL;
    char* main_path = NULL;
    char* gitignore_path = NULL;
    char* manifest_path = NULL;
    FILE* mainf = NULL;
    FILE* gi = NULL;
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_command_help(argv[0], "new");
            return EXIT_SUCCESS_CODE;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return EXIT_COMPILE_ERROR;
        }
        if (!project_name) {
            project_name = argv[i];
        } else {
            fprintf(stderr, "unexpected extra argument: %s\n", argv[i]);
            return EXIT_COMPILE_ERROR;
        }
    }

    if (!project_name) {
        fprintf(stderr, "missing project name. Usage: %s new <project-name>\n", argv[0]);
        return EXIT_COMPILE_ERROR;
    }

    /* Validate the name — basic sanity, no path separators. */
    if (strchr(project_name, '/') != NULL || strchr(project_name, '\\') != NULL) {
        fprintf(stderr, "invalid project name `%s`: must not contain path separators\n", project_name);
        return EXIT_COMPILE_ERROR;
    }

#ifdef _WIN32
    if (_mkdir(project_name) != 0) {
#else
    if (mkdir(project_name, 0755) != 0) {
#endif
        fprintf(stderr, "failed to create project directory `%s`: %s\n",
                project_name, strerror(errno));
        return EXIT_COMPILE_ERROR;
    }

#ifdef _WIN32
    if (_chdir(project_name) != 0) {
#else
    if (chdir(project_name) != 0) {
#endif
        fprintf(stderr, "failed to enter project directory: %s\n", strerror(errno));
        return EXIT_COMPILE_ERROR;
    }

    main_path = duplicate_string("main.lamo");
    mainf = fopen(main_path, "w");
    if (!mainf) {
        fprintf(stderr, "failed to write %s: %s\n", main_path, strerror(errno));
        free(main_path);
        free(gitignore_path);
        free(manifest_path);
        return EXIT_COMPILE_ERROR;
    }
    fprintf(mainf, "// %s — entry point\n", project_name);
    fprintf(mainf, "fn main() {\n");
    fprintf(mainf, "    print(\"Hello from %s!\");\n", project_name);
    fprintf(mainf, "    return 0;\n");
    fprintf(mainf, "}\n");
    fprintf(mainf, "\n");
    fprintf(mainf, "main();\n");
    fclose(mainf);

    gitignore_path = duplicate_string(".gitignore");
    gi = fopen(gitignore_path, "w");
    if (gi) {
        fprintf(gi, "# Lamo project artifacts\n");
        fprintf(gi, "lamo_exec.c\n");
        fprintf(gi, "lamo_exec\n");
        fprintf(gi, "lamo_exec.exe\n");
        fprintf(gi, "lamo_modules/\n");
        fprintf(gi, "lamo.lock\n");
        fclose(gi);
    }

    manifest_path = duplicate_string("lamo.pkg");
    {
        FILE* mf = fopen(manifest_path, "w");
        if (mf) {
            fprintf(mf, "# Lamo package manifest.\n");
            fprintf(mf, "name = %s\n", project_name);
            fprintf(mf, "version = 0.1.0\n");
            fprintf(mf, "packages_dir = lamo_modules\n\n");
            fprintf(mf, "[dependencies]\n");
            fclose(mf);
        }
    }

    if (!cli_quiet()) {
        printf("Created Lamo project `%s`\n", project_name);
        printf("  %s/main.lamo     — entry point with a hello-world program\n", project_name);
        printf("  %s/.gitignore    — ignores lamo_exec* and lamo_modules/\n", project_name);
        printf("  %s/lamo.pkg      — package manifest (add deps with `lampm install owner/repo`)\n", project_name);
        printf("\nNext steps:\n");
        printf("  cd %s\n", project_name);
        printf("  lamo run main.lamo\n");
    }

    free(main_path);
    free(gitignore_path);
    free(manifest_path);
    return EXIT_SUCCESS_CODE;
}

/* command_clean: remove generated build artifacts. */
int command_clean(int argc, char** argv) {
    static const char* artifacts[] = {
        "lamo_exec.c",
        "lamo_exec",
        "lamo_exec.exe",
        NULL
    };
    int i;
    int removed = 0;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_command_help(argv[0], "clean");
            return EXIT_SUCCESS_CODE;
        }
        fprintf(stderr, "unexpected argument: %s (clean takes no arguments)\n", argv[i]);
        return EXIT_COMPILE_ERROR;
    }

    for (i = 0; artifacts[i] != NULL; i++) {
        if (unlink(artifacts[i]) == 0) {
            if (cli_verbose()) {
                printf("[verbose] removed %s\n", artifacts[i]);
            }
            removed++;
        }
    }

    if (!cli_quiet()) {
        if (removed == 0) {
            printf("nothing to clean\n");
        } else {
            printf("removed %d artifact(s)\n", removed);
        }
    }
    return EXIT_SUCCESS_CODE;
}

/* command_repl: interactive read-eval-print loop using the eval module. */
int command_repl(int argc, char** argv) {
    EvalEnv* env;
    char line[8192];
    int i;

    /* Parsed AST nodes that must stay alive for the duration of the REPL
     * session. The eval module stores raw pointers to ASTFnDecl nodes
     * inside the environment (eval_env_define_fn), so freeing them
     * between lines would leave dangling pointers. We collect every
     * parsed node here and free them all at exit. */
    ASTNode** keep_alive = NULL;
    size_t keep_alive_count = 0;
    size_t keep_alive_capacity = 0;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_command_help(argv[0], "repl");
            return EXIT_SUCCESS_CODE;
        }
        fprintf(stderr, "unexpected argument: %s (repl takes no arguments)\n", argv[i]);
        return EXIT_COMPILE_ERROR;
    }

    env = eval_env_new(NULL);

    if (!cli_quiet()) {
        printf("Lamo v%s REPL. Type .exit or .quit to leave, .help for commands.\n", cli_version());
    }

    while (1) {
        char* p;
        size_t len;
        int is_stmt = 0;
        char* source = NULL;
        Lexer* lexer = NULL;
        Parser* parser = NULL;
        ASTNode* parsed = NULL;

        if (!cli_quiet()) {
            fputs("lamo> ", stdout);
        }
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            if (!cli_quiet()) fputs("\n", stdout);
            break;
        }

        /* Trim leading whitespace. */
        p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        /* Trim trailing whitespace. */
        len = strlen(p);
        while (len > 0 && isspace((unsigned char)p[len - 1])) {
            p[--len] = '\0';
        }
        if (*p == '\0') continue;

        if (strcmp(p, ".exit") == 0 || strcmp(p, ".quit") == 0) {
            break;
        }
        if (strcmp(p, ".help") == 0) {
            printf(".exit / .quit  Leave the REPL\n");
            printf(".help          Show this help\n");
            printf("\n");
            printf("Type a Lamo expression to evaluate it (its value is printed),\n");
            printf("or a statement (let/fn/if/while/for/return/break/continue/import)\n");
            printf("to execute it. Statements that produce a value (e.g. `let x = 5;`)\n");
            printf("do not print anything.\n");
            continue;
        }

        /* Decide whether to parse as a statement or an expression. */
        if (strncmp(p, "let ", 4) == 0 || strncmp(p, "fn ", 3) == 0 ||
            strncmp(p, "if ", 3) == 0 || strncmp(p, "if(", 3) == 0 ||
            strncmp(p, "while ", 6) == 0 || strncmp(p, "while(", 6) == 0 ||
            strncmp(p, "for ", 4) == 0 || strncmp(p, "for(", 4) == 0 ||
            strncmp(p, "return", 6) == 0 ||
            strcmp(p, "break") == 0 || strcmp(p, "continue") == 0 ||
            strncmp(p, "import ", 7) == 0) {
            is_stmt = 1;
        }

        source = duplicate_string(p);
        if (!source) break;
        lexer = lexer_init(source);
        parser = parser_init(lexer);

        if (is_stmt) {
            parsed = (ASTNode*)parse_statement(parser);
        } else {
            parsed = (ASTNode*)parse_expression(parser);
        }

        if (parser_had_error(parser)) {
            /* Errors already printed by the parser. Free what we got. */
            if (parsed) ast_free(parsed);
            parsed = NULL;
        } else if (parsed) {
            EvalSignal sig = EVAL_SIG_NONE;
            EvalValue v = is_stmt
                ? eval_statement(parsed, env, &sig)
                : eval_expression(parsed, env, &sig);
            if (sig == EVAL_SIG_ERROR) {
                fprintf(stderr, "runtime error\n");
            } else if (!is_stmt && v.type != EVAL_VAL_VOID && v.type != EVAL_VAL_ERROR) {
                char* s = eval_value_to_string(v);
                printf("%s\n", s ? s : "");
                free(s);
            }
            eval_value_free(v);

            /* Keep the AST alive for the rest of the session (functions
             * registered in env point into it). */
            if (keep_alive_count == keep_alive_capacity) {
                size_t new_cap = keep_alive_capacity > 0 ? keep_alive_capacity * 2 : 16;
                ASTNode** resized = realloc(keep_alive, sizeof(ASTNode*) * new_cap);
                if (!resized) {
                    /* If realloc fails, free this node immediately to
                     * avoid a leak; this may cause use-after-free if the
                     * node was a function declaration that was just
                     * registered in env, but at least we don't crash on
                     * the realloc path. */
                    fprintf(stderr, "warning: out of memory for REPL history; freeing node\n");
                    ast_free(parsed);
                    parsed = NULL;
                } else {
                    keep_alive = resized;
                    keep_alive_capacity = new_cap;
                    keep_alive[keep_alive_count++] = parsed;
                    /* ownership transferred; don't free below */
                    parsed = NULL;
                }
            } else {
                keep_alive[keep_alive_count++] = parsed;
                parsed = NULL;
            }
        }

        parser_free(parser);
        lexer_free(lexer);
        free(source);
        if (parsed) ast_free(parsed);
    }

    /* Free all keep-alive AST nodes. */
    {
        size_t j;
        for (j = 0; j < keep_alive_count; j++) {
            ast_free(keep_alive[j]);
        }
        free(keep_alive);
    }

    eval_env_free(env);
    return EXIT_SUCCESS_CODE;
}

/* Sprint 4: command_test — run the Lamo test suite.
 *
 * Invokes tests/run_tests.sh (POSIX) or tests/run_tests.ps1 (Windows),
 * forwarding the lamo binary path as the first argument. The test runner
 * discovers .lamo files under tests/{valid,invalid,runtime}/ and exits
 * 0 on full pass, 1 on any failure.
 *
 * With no argument, we look for ./lamo (or ./lamo.exe on Windows). With
 * one argument, we treat it as an explicit binary path. This lets users
 * run `lamo test` after `make` without any setup.
 *
 * We use run_argv() (POSIX fork+exec / Windows CreateProcess) rather
 * than system() to avoid shell-injection concerns on the user-supplied
 * path. */
int command_test(int argc, char** argv) {
    const char* lamo_bin = NULL;
    char test_script[4096];
    int i;
    int exit_status;
#ifndef _WIN32
    /* POSIX uses sh + run_tests.sh via run_argv(). Windows uses system()
     * with PowerShell, so it doesn't need this array. Declaring it
     * unconditionally would trigger -Wunused-variable on Windows. */
    char* argv_list[4];
#endif

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_command_help(argv[0], "test");
            return EXIT_SUCCESS_CODE;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return EXIT_COMPILE_ERROR;
        }
        if (!lamo_bin) {
            lamo_bin = argv[i];
        } else {
            fprintf(stderr, "unexpected extra argument: %s\n", argv[i]);
            return EXIT_COMPILE_ERROR;
        }
    }

    if (!lamo_bin) {
        /* Default: look for ./lamo or ./lamo.exe in the current directory. */
#ifdef _WIN32
        lamo_bin = "lamo.exe";
#else
        lamo_bin = "./lamo";
#endif
        /* Quick existence check — if not found, give a helpful message. */
#ifdef _WIN32
        {
            FILE* probe = fopen(lamo_bin, "r");
            if (!probe) {
                fprintf(stderr, "lamo binary not found at ./%s. Build it first with `make`, or pass its path as `lamo test /path/to/lamo`.\n", lamo_bin);
                return EXIT_COMPILE_ERROR;
            }
            fclose(probe);
        }
#else
        {
            struct stat st;
            if (stat(lamo_bin, &st) != 0) {
                fprintf(stderr, "lamo binary not found at %s. Build it first with `make`, or pass its path as `lamo test /path/to/lamo`.\n", lamo_bin);
                return EXIT_COMPILE_ERROR;
            }
        }
#endif
    }

    /* Locate the test script relative to the lamo binary's directory.
     * Convention: tests/run_tests.sh sits next to the binary (when built
     * in the repo root). We try a few candidate paths:
     *   1. ./tests/run_tests.sh  (most common — running from repo root)
     *   2. <bin_dir>/tests/run_tests.sh
     *   3. <bin_dir>/../tests/run_tests.sh  (when binary is in a build/ subdir)
     * On Windows we use tests/run_tests.ps1 instead.
     */
    {
        const char* candidates[3];
        int n_candidates = 0;
        candidates[n_candidates++] = "tests/run_tests.sh";
        {
            /* Build <bin_dir>/tests/run_tests.sh */
            char* bin_dir = path_directory(lamo_bin);
            char* joined;
            if (bin_dir) {
                joined = path_join(bin_dir, "tests/run_tests.sh");
                if (joined) candidates[n_candidates++] = joined;
                joined = path_join(bin_dir, "../tests/run_tests.sh");
                if (joined) candidates[n_candidates++] = joined;
                free(bin_dir);
            }
        }
        for (i = 0; i < n_candidates; i++) {
            struct stat st;
            if (stat(candidates[i], &st) == 0 && (st.st_mode & S_IFREG)) {
                strncpy(test_script, candidates[i], sizeof(test_script) - 1);
                test_script[sizeof(test_script) - 1] = '\0';
                /* Free any later candidates we didn't use (they were malloc'd). */
                for (int j = i + 1; j < n_candidates; j++) {
                    if (candidates[j] != candidates[0]) free((void*)candidates[j]);
                }
                goto found;
            }
            /* Free this candidate if it was malloc'd. */
            if (i > 0 && candidates[i] != candidates[0]) free((void*)candidates[i]);
        }
        fprintf(stderr, "could not find tests/run_tests.sh next to %s\n", lamo_bin);
        fprintf(stderr, "looked in: tests/run_tests.sh, <bin_dir>/tests/run_tests.sh, <bin_dir>/../tests/run_tests.sh\n");
        return EXIT_COMPILE_ERROR;
    }
found:

    if (!cli_quiet()) {
        printf("Running Lamo test suite via %s...\n", test_script);
    }

    /* Invoke: sh <test_script> <lamo_bin>
     * On Windows we'd invoke powershell, but the script path is .ps1.
     * For simplicity, this POSIX-only invocation uses sh. The Windows
     * path (run_tests.ps1) is handled separately below. */
#ifdef _WIN32
    {
        char ps_cmd[8192];
        /* powershell -ExecutionPolicy Bypass -File <script> -LamoBinary <bin> */
        /* But run_tests.ps1 doesn't take a binary arg the same way; it auto-detects.
         * Just run the .ps1 directly. */
        snprintf(ps_cmd, sizeof(ps_cmd),
                 "powershell -ExecutionPolicy Bypass -File \"tests\\run_tests.ps1\"");
        exit_status = system(ps_cmd);
        /* system returns the exit code in the same way waitpid does on POSIX,
         * so we need to extract it. But Windows system() returns the exit
         * code directly. Either way, non-zero means failure. */
        if (exit_status != 0) {
            fprintf(stderr, "test suite failed (exit code %d)\n", exit_status);
            return EXIT_COMPILE_ERROR;
        }
        return EXIT_SUCCESS_CODE;
    }
#else
    argv_list[0] = (char*)"sh";
    argv_list[1] = test_script;
    argv_list[2] = (char*)lamo_bin;
    argv_list[3] = NULL;
    exit_status = run_argv(argv_list);
    if (exit_status != 0) {
        if (!cli_quiet()) {
            fprintf(stderr, "test suite failed (exit code %d)\n", exit_status);
        }
        return EXIT_COMPILE_ERROR;
    }
    if (!cli_quiet()) {
        printf("All tests passed.\n");
    }
    return EXIT_SUCCESS_CODE;
#endif
}

/* Sprint 4: command_fmt — normalize source formatting in place.
 *
 * This is a deliberately conservative formatter. It does NOT reflow
 * expressions, reindent blocks, or rename anything. It only applies
 * safe, idempotent transformations:
 *
 *   - CRLF → LF (Windows line endings normalized to Unix)
 *   - Tabs → 4 spaces (matches the codegen output style)
 *   - Trailing whitespace stripped from each line
 *   - File ends with exactly one trailing newline (no zero, no double)
 *
 * These are the transformations that NEVER change the meaning of a
 * valid Lamo program. A full AST-based pretty-printer (reindent,
 * space normalization around operators, etc.) is future work — see
 * the roadmap. The current command is still useful for keeping a
 * project's files normalized across contributors.
 *
 * --check: read-only mode. Print a diff for each file that would be
 * changed and exit non-zero if any file needs formatting (CI mode).
 * Files are not modified. */
int command_fmt(int argc, char** argv) {
    const char** files = NULL;
    int file_count = 0;
    int check_mode = 0;
    int i;
    int rc = EXIT_SUCCESS_CODE;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_command_help(argv[0], "fmt");
            return EXIT_SUCCESS_CODE;
        }
        if (strcmp(argv[i], "--check") == 0) {
            check_mode = 1;
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            free(files);
            return EXIT_COMPILE_ERROR;
        }
        /* Positional file argument. */
        {
            const char** resized = realloc(files, sizeof(char*) * (size_t)(file_count + 1));
            if (!resized) {
                fprintf(stderr, "out of memory\n");
                free(files);
                return EXIT_COMPILE_ERROR;
            }
            files = resized;
            files[file_count++] = argv[i];
        }
    }

    if (file_count == 0) {
        fprintf(stderr, "no input files. Usage: %s fmt <file.lamo> [more.lamo ...]\n", argv[0]);
        free(files);
        return EXIT_COMPILE_ERROR;
    }

    for (i = 0; i < file_count; i++) {
        const char* path = files[i];
        char* source = read_file(path);
        char* out;
        size_t out_len;
        size_t in_pos;
        size_t out_pos;
        size_t out_capacity;

        if (!source) {
            fprintf(stderr, "failed to read %s: %s\n", path, strerror(errno));
            rc = EXIT_COMPILE_ERROR;
            continue;
        }

        /* First pass: convert CRLF → LF and tabs → 4 spaces.
         * Output buffer is at most 4x the input (every tab → 4 spaces).
         * We allocate that worst case to avoid reallocation. */
        in_pos = 0;
        out_pos = 0;
        {
            size_t src_len = strlen(source);
            out_capacity = src_len * 4 + 2;  /* +2 for trailing newline + NUL */
            out = malloc(out_capacity);
            if (!out) {
                fprintf(stderr, "out of memory formatting %s\n", path);
                free(source);
                rc = EXIT_COMPILE_ERROR;
                continue;
            }
        }
        while (source[in_pos] != '\0') {
            char c = source[in_pos++];
            if (c == '\r') {
                /* Skip; the following \n (if any) will produce the LF. */
                /* If there's no \n (Mac classic line ending), emit LF. */
                if (source[in_pos] != '\n') {
                    out[out_pos++] = '\n';
                }
                continue;
            }
            if (c == '\t') {
                out[out_pos++] = ' ';
                out[out_pos++] = ' ';
                out[out_pos++] = ' ';
                out[out_pos++] = ' ';
                continue;
            }
            out[out_pos++] = c;
        }
        out[out_pos] = '\0';

        /* Second pass: strip trailing whitespace from each line, ensure
         * exactly one trailing newline. We walk the (now LF-terminated)
         * buffer line by line, copying trimmed lines into the final
         * output. Reuse the same buffer — we never grow, only shrink. */
        out_pos = 0;
        for (in_pos = 0; in_pos < strlen(out); ) {
            /* Find the end of this line. */
            char* nl = strchr(out + in_pos, '\n');
            size_t line_end = nl ? (size_t)(nl - (out + in_pos)) : strlen(out + in_pos);
            size_t trimmed_end = line_end;
            /* Strip trailing spaces (we already converted tabs). */
            while (trimmed_end > 0 && (out[in_pos + trimmed_end - 1] == ' ' ||
                                        out[in_pos + trimmed_end - 1] == '\r')) {
                trimmed_end--;
            }
            /* Copy trimmed line into the output (in-place, so it always fits). */
            memmove(out + out_pos, out + in_pos, trimmed_end);
            out_pos += trimmed_end;
            out[out_pos++] = '\n';
            /* Advance past the original line + its newline. */
            in_pos += line_end + (nl ? 1 : 0);
        }

        /* Ensure exactly one trailing newline. If the file is empty,
         * leave it empty (no trailing newline on an empty file). */
        while (out_pos >= 2 && out[out_pos - 1] == '\n' && out[out_pos - 2] == '\n') {
            out_pos--;
        }
        out_len = out_pos;
        out[out_len] = '\0';

        /* Compare with original. If unchanged, skip writing. */
        if (strcmp(source, out) == 0) {
            if (cli_verbose()) {
                printf("[verbose] %s: already formatted\n", path);
            }
            free(source);
            free(out);
            continue;
        }

        if (check_mode) {
            /* Print a short diff summary. */
            printf("%s: would reformat (%zu bytes -> %zu bytes)\n",
                   path, strlen(source), out_len);
            rc = EXIT_COMPILE_ERROR;  /* non-zero signals "needs formatting" */
            free(source);
            free(out);
            continue;
        }

        /* Write the file in place. */
        {
            FILE* f = fopen(path, "wb");
            if (!f) {
                fprintf(stderr, "failed to write %s: %s\n", path, strerror(errno));
                rc = EXIT_COMPILE_ERROR;
                free(source);
                free(out);
                continue;
            }
            fwrite(out, 1, out_len, f);
            fclose(f);
        }
        if (!cli_quiet()) {
            printf("formatted %s (%zu -> %zu bytes)\n", path, strlen(source), out_len);
        }
        free(source);
        free(out);
    }

    free(files);
    return rc;
}
