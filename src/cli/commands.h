#ifndef LAMO_CLI_COMMANDS_H
#define LAMO_CLI_COMMANDS_H

/*
 * commands.h — Per-subcommand handlers for the `lamo` CLI.
 *
 * Refactor (Sprint 5): these handlers (command_new, command_clean,
 * command_repl, command_test, command_fmt) used to be `static` inside
 * lamo_v2.c. They are now external so that main (still in lamo_v2.c)
 * can dispatch to them while keeping the per-command implementations
 * out of the dispatch file.
 *
 * Each handler receives the full original argv (argv[0] = program name,
 * argv[1] = subcommand) and returns an exit code (0 = success). The
 * shared EXIT_*_CODE constants are in cli_options.h.
 */

#include "cli_options.h"

int command_new(int argc, char** argv);
int command_clean(int argc, char** argv);
int command_repl(int argc, char** argv);
int command_test(int argc, char** argv);
int command_fmt(int argc, char** argv);

#endif /* LAMO_CLI_COMMANDS_H */
