#ifndef LAMO_CLI_OPTIONS_H
#define LAMO_CLI_OPTIONS_H

/*
 * cli_options.h — Global CLI option state for the `lamo` driver.
 *
 * Refactor (Sprint 5): the global option variables (g_verbose, g_quiet,
 * g_no_color) and the LamoCommand / exit-code enums used to live as
 * static decls at the top of the 2500-line lamo_v2.c. They are now in
 * their own translation unit so that the CLI dispatch (lamo_v2.c),
 * subcommand handlers (commands.c), compile pipeline (compile.c), and
 * help text (help.c) can all read the same option state without each
 * file redeclaring it.
 *
 * Access is through accessor functions (cli_verbose / cli_quiet /
 * cli_no_color) rather than extern variables so that future changes to
 * the storage (e.g. thread-locals) don't ripple through every caller.
 * Setters are used only by main() in lamo_v2.c.
 *
 * The VERSION macro is also re-exported through cli_version() so that
 * help.c and commands.c don't need to know the literal version string.
 */

typedef enum {
    COMMAND_RUN,
    COMMAND_BUILD,
    COMMAND_CHECK,
    COMMAND_EVAL,
    COMMAND_NEW,
    COMMAND_CLEAN,
    COMMAND_REPL
} LamoCommand;

enum {
    EXIT_SUCCESS_CODE = 0,
    EXIT_COMPILE_ERROR = 1,
    EXIT_BACKEND_ERROR = 2
};

/* Lamo compiler version. Matches the VERSION macro that used to live
 * at the top of lamo_v2.c. */
const char* cli_version(void);

/* Read current option state. */
int cli_verbose(void);
int cli_quiet(void);
int cli_no_color(void);

/* Set option state. Used only from main() in lamo_v2.c. */
void cli_set_verbose(int v);
void cli_set_quiet(int v);
void cli_set_no_color(int v);

#endif /* LAMO_CLI_OPTIONS_H */
