#ifndef LAMO_CLI_HELP_H
#define LAMO_CLI_HELP_H

/*
 * help.h — Usage and per-command help text for the `lamo` CLI.
 *
 * Refactor (Sprint 5): print_usage / print_command_help used to be
 * `static` inside lamo_v2.c. They are now external so that main (in
 * lamo_v2.c) and the per-subcommand handlers (in commands.c) can both
 * call them.
 *
 * print_command_help delegates package-manager subcommands to the
 * integrated lampm help (see lampm.h) and handles the core lamo
 * subcommands (run, build, check, eval, repl, new, clean, test, fmt,
 * help, version) inline.
 */

void print_usage(const char* prog);
void print_command_help(const char* prog, const char* command);

#endif /* LAMO_CLI_HELP_H */
