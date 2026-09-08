/*
 * lamo_v2.c — Entry point and CLI dispatch for the `lamo` compiler.
 *
 * Refactor (Sprint 5): this file used to be 2500+ lines, mixing CLI
 * option parsing, import resolution, subcommand handlers, and the
 * compile pipeline all in one translation unit. It now contains only:
 *
 *   - main():            env-var parsing, global flag parsing, subcommand
 *                        dispatch (calling into commands.c, compile.c,
 *                        or lampm).
 *   - run_argv():        cross-platform spawn helper (fork+execvp on
 *                        POSIX, CreateProcessA on Windows). Used by
 *                        compile_sources() in compile.c and by
 *                        command_test() in commands.c.
 *
 * Everything else has been split out into focused modules under src/cli/:
 *
 *   - cli_options.{c,h}: global option state + VERSION + LamoCommand enum
 *   - paths.{c,h}:       file/path helpers (path_join, resolve_import_path,
 *                        read_file, lamo_cc, executable_suffix, ...)
 *   - import_resolver.{c,h}: CompilationState + recursive import loader
 *                        + module-alias renaming
 *   - commands.{c,h}:    per-subcommand handlers (new, clean, repl, test, fmt)
 *   - compile.{c,h}:     compile_sources pipeline + GUI detection +
 *                        semantic/codegen callbacks
 *   - help.{c,h}:        print_usage + print_command_help
 *
 * The public API of lampm (lampm.h) is unchanged.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* Sprint 1 fix (Windows): the Lamo TokenType (in lexer.h) used to collide
 * with the Windows SDK TokenType (an enumerator in winnt.h's
 * _TOKEN_INFORMATION_CLASS enum, exposed via <windows.h>). We renamed
 * ours to LamoTokenType to avoid the collision, so the include order
 * no longer matters. We still define WIN32_LEAN_AND_MEAN and NOGDI to
 * keep the <windows.h> surface small (smaller object files, faster
 * compiles). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#else
#include <unistd.h>      /* execvp, fork (via sys/wait.h's deps) */
#include <sys/wait.h>    /* WIFEXITED, WEXITSTATUS, WIFSIGNALED, WTERMSIG */
#include <errno.h>
#endif

#include "cli/cli_options.h"
#include "cli/paths.h"
#include "cli/help.h"
#include "cli/commands.h"
#include "cli/compile.h"
#include "error_util.h"
#include "lampm/lampm.h"

int main(int argc, char** argv) {
    LamoCommand command = COMMAND_RUN;
    const char** input_files = NULL;
    int input_file_count = 0;
    const char* output_path = NULL;
    int arg_index = 1;
    int i;

    /* Honor LAMO_VERBOSE / LAMO_QUIET env vars. */
    if (getenv("LAMO_VERBOSE") && getenv("LAMO_VERBOSE")[0] != '\0' &&
        strcmp(getenv("LAMO_VERBOSE"), "0") != 0) {
        cli_set_verbose(1);
    }
    if (getenv("LAMO_QUIET") && getenv("LAMO_QUIET")[0] != '\0' &&
        strcmp(getenv("LAMO_QUIET"), "0") != 0) {
        cli_set_quiet(1);
    }
    /* Sprint 4: LAMO_NO_COLOR env var. */
    if (getenv("LAMO_NO_COLOR") && getenv("LAMO_NO_COLOR")[0] != '\0' &&
        strcmp(getenv("LAMO_NO_COLOR"), "0") != 0) {
        cli_set_no_color(1);
        lamo_error_set_color(0);
    }

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_COMPILE_ERROR;
    }

    /* Consume leading global flags before the subcommand. */
    while (arg_index < argc && argv[arg_index][0] == '-' &&
           strcmp(argv[arg_index], "--") != 0) {
        if (strcmp(argv[arg_index], "--verbose") == 0) {
            cli_set_verbose(1);
            arg_index++;
        } else if (strcmp(argv[arg_index], "--quiet") == 0) {
            cli_set_quiet(1);
            arg_index++;
        } else if (strcmp(argv[arg_index], "--no-color") == 0) {
            /* Sprint 4: disable ANSI color in compiler errors. */
            cli_set_no_color(1);
            lamo_error_set_color(0);
            arg_index++;
        } else if (strcmp(argv[arg_index], "--help") == 0 ||
                   strcmp(argv[arg_index], "-h") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS_CODE;
        } else if (strcmp(argv[arg_index], "--version") == 0 ||
                   strcmp(argv[arg_index], "-v") == 0) {
            printf("lamo %s\n", cli_version());
            if (cli_verbose()) printf("C compiler: %s\n", lamo_cc());
            return EXIT_SUCCESS_CODE;
        } else {
            fprintf(stderr, "unknown global option: %s\n", argv[arg_index]);
            return EXIT_COMPILE_ERROR;
        }
    }

    if (arg_index >= argc) {
        print_usage(argv[0]);
        return EXIT_COMPILE_ERROR;
    }

    /* Shift argv so the rest of main() can assume the subcommand is at argv[1].
     * We don't actually move memory — just adjust an offset and recompute.
     * Simpler approach: from now on, treat argv[arg_index] as the command. */
    {
        const char* subcommand = argv[arg_index];
        int sub_argc = argc - arg_index + 1;
        char** sub_argv = argv + arg_index - 1;

        /* Reuse the rest of main() by replacing argv[1] semantics.
         * Easier path: just rewrite argv[1] to be the subcommand and adjust
         * argv[] pointers. Since we can't easily do that, we proceed with
         * the original argv but skip past the flags by referencing argv[arg_index]. */

        if (strcmp(subcommand, "help") == 0) {
            if (argc >= arg_index + 2) {
                print_command_help(argv[0], argv[arg_index + 1]);
            } else {
                print_usage(argv[0]);
            }
            return EXIT_SUCCESS_CODE;
        }

        if (strcmp(subcommand, "version") == 0) {
            int show_verbose_version = 0;
            for (i = arg_index + 1; i < argc; i++) {
                if (strcmp(argv[i], "--verbose") == 0) {
                    show_verbose_version = 1;
                }
            }
            printf("lamo %s\n", cli_version());
            if (show_verbose_version || cli_verbose()) {
                printf("lampm (integrated) %s\n", LAMPM_VERSION);
                printf("C compiler: %s\n", lamo_cc());
            }
            return EXIT_SUCCESS_CODE;
        }

        /* Subcommand help: `lamo <command> --help` */
        for (i = arg_index + 1; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                print_command_help(argv[0], subcommand);
                return EXIT_SUCCESS_CODE;
            }
        }

        /* Global flags can also appear after the subcommand. */
        for (i = arg_index + 1; i < argc; i++) {
            if (strcmp(argv[i], "--verbose") == 0) {
                cli_set_verbose(1);
            } else if (strcmp(argv[i], "--quiet") == 0) {
                cli_set_quiet(1);
            } else if (strcmp(argv[i], "--no-color") == 0) {
                /* Sprint 4: same effect as leading --no-color. */
                cli_set_no_color(1);
                lamo_error_set_color(0);
            }
        }

        /* Package-manager subcommands (init, install, update, remove, list,
         * info, outdated, why, lock, cache, doctor) are handled by the
         * integrated lampm module. We hand off the full argv starting at
         * sub_argv (which has the subcommand at index 1) so lampm_main
         * sees the same shape it would as a standalone binary. */
        if (lampm_is_subcommand(subcommand)) {
            /* Configure lampm with the global flags parsed so far. Pass
             * -1 for color so lampm's own TTY detection runs. */
            lampm_configure(cli_verbose(), cli_quiet(), -1);
            return lampm_main(sub_argc, sub_argv);
        }

        if (strcmp(subcommand, "run") == 0) {
            command = COMMAND_RUN;
        } else if (strcmp(subcommand, "build") == 0) {
            command = COMMAND_BUILD;
        } else if (strcmp(subcommand, "check") == 0) {
            command = COMMAND_CHECK;
        } else if (strcmp(subcommand, "eval") == 0) {
            command = COMMAND_EVAL;
        } else if (strcmp(subcommand, "repl") == 0) {
            return command_repl(sub_argc, sub_argv);
        } else if (strcmp(subcommand, "new") == 0) {
            return command_new(sub_argc, sub_argv);
        } else if (strcmp(subcommand, "clean") == 0) {
            return command_clean(sub_argc, sub_argv);
        } else if (strcmp(subcommand, "test") == 0) {
            /* Sprint 4: run the test suite. */
            return command_test(sub_argc, sub_argv);
        } else if (strcmp(subcommand, "fmt") == 0) {
            /* Sprint 4: format source files. */
            return command_fmt(sub_argc, sub_argv);
        } else {
            fprintf(stderr, "unknown command: %s\n\n", subcommand);
            print_usage(argv[0]);
            return EXIT_COMPILE_ERROR;
        }

        /* Now parse the remaining args (files + -o). */
        arg_index++;
    }

    /* Skip global flags interleaved with files. */
    while (arg_index < argc) {
        if (strcmp(argv[arg_index], "--verbose") == 0 ||
            strcmp(argv[arg_index], "--quiet") == 0) {
            arg_index++;
            continue;
        }
        if (strcmp(argv[arg_index], "-o") == 0) {
            arg_index++;
            if (arg_index >= argc) {
                fprintf(stderr, "missing output path after -o\n");
                free(input_files);
                return EXIT_COMPILE_ERROR;
            }
            output_path = argv[arg_index++];
        } else {
            const char** resized_input_files = realloc(input_files, sizeof(char*) * (size_t)(input_file_count + 1));
            if (!resized_input_files) {
                fprintf(stderr, "failed to allocate input file list\n");
                free(input_files);
                return EXIT_COMPILE_ERROR;
            }
            input_files = resized_input_files;
            input_files[input_file_count++] = argv[arg_index++];
        }
    }

    if (input_file_count == 0) {
        fprintf(stderr, "no input files provided\n");
        free(input_files);
        return EXIT_COMPILE_ERROR;
    }

    if (command == COMMAND_CHECK && output_path) {
        fprintf(stderr, "`-o` is not supported with `check`\n");
        free(input_files);
        return EXIT_COMPILE_ERROR;
    }

    {
        int exit_code = compile_sources(input_files, input_file_count, command, output_path);
        free(input_files);
        return exit_code;
    }
}

/* run_argv: execute a program with the given argv without going through a
 * shell. This eliminates the entire class of shell-injection bugs that
 * system() would expose on user-controlled paths (e.g. the -o argument).
 *
 * POSIX: fork() + execvp(), wait for child, return its exit status
 *        (0-255). If the child was killed by a signal, return 128 + signum.
 * Windows: CreateProcessA() with the first argv element as the program
 *        path. Returns the child's exit code, or -1 on failure to spawn.
 *
 * argv MUST be NULL-terminated. argv[0] is the program name (sent to
 * execvp() as the file to search PATH for; on Windows it is the full
 * command line token). */
int run_argv(char* const argv[]) {
    if (!argv || !argv[0]) {
        return -1;
    }

#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 0;
    char command_line[8192];
    size_t pos = 0;
    int i;

    /* Build a quoted command line. We don't shell-interpret it (we use
     * CreateProcess, not cmd.exe), but Windows still parses the command
     * line string itself, so we must quote each argument. */
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    for (i = 0; argv[i] != NULL; i++) {
        const char* arg = argv[i];
        size_t arg_len = strlen(arg);
        /* Each arg gets surrounded by quotes; inner quotes are doubled.
         * This follows the standard CommandLineToArgvW rules. */
        if (pos + arg_len * 2 + 4 >= sizeof(command_line)) {
            fprintf(stderr, "internal error: command line too long\n");
            return -1;
        }
        if (i > 0) command_line[pos++] = ' ';
        command_line[pos++] = '"';
        for (size_t j = 0; j < arg_len; j++) {
            char c = arg[j];
            if (c == '"') {
                command_line[pos++] = '"';
                command_line[pos++] = '"';
            } else {
                command_line[pos++] = c;
            }
        }
        command_line[pos++] = '"';
    }
    command_line[pos] = '\0';

    if (!CreateProcessA(NULL, command_line, NULL, NULL, TRUE, 0,
                        NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        fprintf(stderr, "failed to spawn '%s' (error %lu)\n", argv[0], (unsigned long)err);
        return -1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        exit_code = (DWORD)-1;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
#else
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* Child: execvp() searches PATH. */
        execvp(argv[0], argv);
        /* If we got here, execvp() failed. */
        fprintf(stderr, "exec '%s' failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    /* Parent: wait for the child to finish. */
    for (;;) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, 0);
        if (waited < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
            return -1;
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
        /* Should not happen, but be safe. */
        return -1;
    }
#endif
}
