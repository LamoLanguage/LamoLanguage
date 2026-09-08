#ifndef LAMO_LAMPM_H
#define LAMO_LAMPM_H

/*
 * lampm.h — Package manager commands integrated into the `lamo` CLI.
 *
 * Originally a standalone binary (`lampm`), the package manager is now
 * reachable as a set of subcommands of the main `lamo` executable:
 *
 *     lamo init [project-name]
 *     lamo install [owner/repo@ref] [alias]
 *     lamo update [alias]
 *     lamo remove <alias>
 *     lamo list
 *     lamo info <alias>
 *     lamo lock
 *     lamo cache <clean|list>
 *     lamo doctor
 *
 * Scope reduction (2.3.0): the `why` and `outdated` subcommands were
 * removed. `why` was a pure alias for `info` (no unique behavior — a
 * maintenance cost with no user benefit). `outdated` did a network
 * fetch per dep to compare local HEAD against origin/HEAD; that's
 * fragile (depends on `git fetch` succeeding) and easy to do manually
 * via `lamo info <alias>` + a quick `git log` inspection. Cutting
 * both keeps the command surface focused on what the package manager
 * actually needs to do well (install, update, remove, list, lock).
 *
 * Implementation lives in src/lampm/lampm.c. The function below behaves
 * exactly like a `main()` for those subcommands: it receives the full argv
 * (with argv[1] = the package-manager subcommand) and returns an exit
 * code. The caller in lamo_v2.c is responsible for parsing leading global
 * flags (--verbose / --quiet / --no-color) and for dispatching to this
 * function only when the subcommand is one of the package-manager ones.
 *
 * The implementation in lampm.c reads the same global option variables
 * (g_verbose / g_quiet / g_color) — but to keep lampm.c a self-contained
 * translation unit that doesn't depend on lamo_v2.c symbols, we re-export
 * the option values through lampm_configure() below.
 */

#define LAMPM_VERSION "0.2.0"

/*
 * lampm_configure: set runtime options for the package manager.
 * Call this once from lamo_v2.c::main() before dispatching to
 * lampm_main(). Pass -1 to leave an option unchanged.
 */
void lampm_configure(int verbose, int quiet, int color);

/*
 * lampm_is_subcommand: returns 1 if `name` is one of the package-manager
 * subcommands (init, install, update, remove, list, info, lock, cache,
 * doctor). Returns 0 otherwise.
 */
int lampm_is_subcommand(const char* name);

/*
 * lampm_main: entry point for the package-manager subcommands. Behaves
 * like main(): argv[0] is the program name, argv[1] is the subcommand.
 * Returns an exit code (0 = success).
 */
int lampm_main(int argc, char** argv);

#endif /* LAMO_LAMPM_H */
