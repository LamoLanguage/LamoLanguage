/*
 * cli_options.c — Implementation of the global CLI option state.
 * See cli_options.h for the design rationale.
 */

#include "cli_options.h"

#define VERSION "2.5.0"

/* Global CLI options. Set in main() based on argv / env vars.
 * Sprint 5 refactor: these used to be `static` inside lamo_v2.c; they
 * are now file-scope here and reached through accessor functions so
 * that commands.c / compile.c / help.c can read them without each
 * holding their own copy. */
static int g_verbose = 0;       /* LAMO_VERBOSE=1 or --verbose */
static int g_quiet = 0;         /* LAMO_QUIET=1 or --quiet */
/* --no-color / LAMO_NO_COLOR=1. Also auto-disabled when stderr is not
 * a TTY (handled in error_util.h). Affects compiler errors and (eventually)
 * all colored output. */
static int g_no_color = 0;

const char* cli_version(void) {
    return VERSION;
}

int cli_verbose(void) { return g_verbose; }
int cli_quiet(void)   { return g_quiet; }
int cli_no_color(void) { return g_no_color; }

void cli_set_verbose(int v) { g_verbose = v; }
void cli_set_quiet(int v)   { g_quiet = v; }
void cli_set_no_color(int v) { g_no_color = v; }
