/*
 * help.c — Usage and per-command help text for the `lamo` CLI.
 * See help.h for the design rationale.
 */

#include <stdio.h>
#include <string.h>

#include "help.h"
#include "cli_options.h"
#include "lampm/lampm.h"

void print_usage(const char* prog) {
    printf("Lamo v%s (with integrated package manager v%s)\n\n", cli_version(), LAMPM_VERSION);
    printf("Usage:\n");
    printf("  %s run   <file.lamo> [more-files.lamo ...] [-o output]\n", prog);
    printf("  %s build <file.lamo> [more-files.lamo ...] [-o output]\n", prog);
    printf("  %s check <file.lamo> [more-files.lamo ...]\n", prog);
    printf("  %s eval  <file.lamo>   (interpret without compiling — instant feedback)\n", prog);
    printf("  %s repl                (interactive read-eval-print loop)\n", prog);
    printf("  %s new   <project-name>\n", prog);
    printf("  %s clean               (remove generated lamo_exec* artifacts)\n", prog);
    printf("  %s test                (run the test suite under tests/)\n", prog);
    printf("  %s fmt   <file.lamo>   (normalize source formatting in place)\n", prog);
    printf("\n");
    printf("Package manager subcommands (formerly `lampm`):\n");
    printf("  %s init [project-name]              Create a new lamo.pkg (and scaffold)\n", prog);
    printf("  %s install [owner/repo@ref] [alias] Install a dependency (or all)\n", prog);
    printf("  %s update [alias]                   Pull latest HEAD for one or all deps\n", prog);
    printf("  %s remove <alias>                   Remove a dependency and its install dir\n", prog);
    printf("  %s list                             List dependencies and their state\n", prog);
    printf("  %s info <alias>                     Show details about a dependency\n", prog);
    printf("  %s outdated                         Check which deps are behind remote HEAD\n", prog);
    printf("  %s why <alias>                      Alias for `info`\n", prog);
    printf("  %s lock                             Refresh the lockfile from installed deps\n", prog);
    printf("  %s cache <clean|list>               Manage the local packages directory\n", prog);
    printf("  %s doctor                           Verify your environment is set up\n", prog);
    printf("\n");
    printf("  %s help [command]\n", prog);
    printf("  %s version [--verbose]\n", prog);
    printf("\n");
    printf("Global options (apply to most commands):\n");
    printf("  --verbose   Show extra progress information (also: LAMO_VERBOSE=1)\n");
    printf("  --quiet     Suppress success messages (also: LAMO_QUIET=1)\n");
    printf("  --no-color  Disable ANSI color output in compiler errors and package manager\n");
    printf("\n");
    printf("Environment variables:\n");
    printf("  LAMO_CC       C compiler to use for `run`/`build` (default: gcc)\n");
    printf("  LAMO_VERBOSE  Same as --verbose\n");
    printf("  LAMO_QUIET    Same as --quiet\n");
    printf("  LAMO_NO_COLOR Same as --no-color\n");
}

void print_command_help(const char* prog, const char* command) {
    /* Delegate package-manager subcommands to the lampm help. */
    if (lampm_is_subcommand(command)) {
        char* fake_argv[3];
        fake_argv[0] = (char*)prog;
        fake_argv[1] = (char*)"help";
        fake_argv[2] = (char*)command;
        lampm_main(3, fake_argv);
        return;
    }

    if (strcmp(command, "run") == 0) {
        printf("run — Compile and execute a Lamo program.\n\n");
        printf("Usage: %s run <file.lamo> [more-files.lamo ...] [-o output]\n\n", prog);
        printf("Transpiles to C, invokes the C compiler (LAMO_CC, default gcc), then\n");
        printf("executes the resulting binary. With -o, the binary is saved under that\n");
        printf("name (plus a platform .exe suffix on Windows); otherwise it's named\n");
        printf("lamo_exec and removed after execution.\n");
    } else if (strcmp(command, "build") == 0) {
        printf("build — Compile a Lamo program to a binary, without running it.\n\n");
        printf("Usage: %s build <file.lamo> [more-files.lamo ...] [-o output]\n\n", prog);
        printf("Like `run`, but stops after the binary is built.\n");
    } else if (strcmp(command, "check") == 0) {
        printf("check — Parse and semantic-check a Lamo program without codegen.\n\n");
        printf("Usage: %s check <file.lamo> [more-files.lamo ...]\n\n", prog);
        printf("Fastest way to validate syntax and types. -o is rejected.\n");
    } else if (strcmp(command, "eval") == 0) {
        printf("eval — Interpret a Lamo program directly (no C compiler).\n\n");
        printf("Usage: %s eval <file.lamo>\n\n", prog);
        printf("Uses the built-in tree-walking interpreter. Best for quick feedback\n");
        printf("or environments without a C compiler. Not all runtime features\n");
        printf("(GUI, HTTP) are available in eval mode.\n");
    } else if (strcmp(command, "repl") == 0) {
        printf("repl — Interactive read-eval-print loop.\n\n");
        printf("Usage: %s repl\n\n", prog);
        printf("Reads one line at a time. Lines starting with `let`, `fn`, `if`,\n");
        printf("`while`, `for`, `return`, `break`, `continue`, or `import` are\n");
        printf("parsed as statements; everything else is parsed as an expression\n");
        printf("and its value is printed. Type .exit or .quit to leave.\n");
    } else if (strcmp(command, "new") == 0) {
        printf("new — Scaffold a new Lamo project.\n\n");
        printf("Usage: %s new <project-name>\n\n", prog);
        printf("Creates a directory <project-name> with a starter main.lamo,\n");
        printf("a .gitignore, and a lamo.pkg manifest ready for `lamo install`.\n");
    } else if (strcmp(command, "clean") == 0) {
        printf("clean — Remove generated build artifacts.\n\n");
        printf("Usage: %s clean\n\n", prog);
        printf("Deletes lamo_exec.c, lamo_exec, lamo_exec.exe (the intermediate C\n");
        printf("source and compiled binary). Source files are untouched.\n");
    } else if (strcmp(command, "test") == 0) {
        printf("test — Run the Lamo test suite.\n\n");
        printf("Usage: %s test [test-binary-path]\n\n", prog);
        printf("Invokes tests/run_tests.sh (POSIX) or tests/run_tests.ps1 (Windows),\n");
        printf("discovering .lamo files under tests/valid/ (must check successfully),\n");
        printf("tests/invalid/ (must fail check), and tests/runtime/ (must run and\n");
        printf("match a sibling .expected file). With no argument, uses ./lamo.\n");
        printf("Exits 0 on full pass, 1 if any test fails.\n");
    } else if (strcmp(command, "fmt") == 0) {
        printf("fmt — Normalize source formatting in place.\n\n");
        printf("Usage: %s fmt <file.lamo> [more.lamo ...]\n\n", prog);
        printf("Rewrites each file in place with:\n");
        printf("  - LF line endings (CRLF converted)\n");
        printf("  - trailing whitespace stripped from each line\n");
        printf("  - file ends with exactly one newline\n");
        printf("  - tabs converted to 4 spaces (matches the codegen output style)\n");
        printf("Does NOT reflow expressions or reindent blocks yet — a full AST-based\n");
        printf("pretty-printer is future work. Safe to run on any .lamo file.\n");
        printf("Use --check to print a diff and exit non-zero without modifying files\n");
        printf("(useful in CI / pre-commit hooks).\n");
    } else if (strcmp(command, "help") == 0) {
        printf("help — Show help.\n\n");
        printf("Usage: %s help [command]\n\n", prog);
        printf("Without an argument, shows general help. With a command name,\n");
        printf("shows detailed help for that command. Package-manager subcommands\n");
        printf("(init, install, update, remove, list, info, outdated, why, lock,\n");
        printf("cache, doctor) delegate to the integrated lampm help.\n");
    } else if (strcmp(command, "version") == 0) {
        printf("version — Print the Lamo compiler version and exit.\n\n");
        printf("Usage: %s version [--verbose]\n\n", prog);
        printf("With --verbose, also prints the integrated lampm version and the\n");
        printf("C compiler that will be used.\n");
    } else {
        fprintf(stderr, "no help available for `%s`\n", command);
    }
}
