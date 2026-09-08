#ifndef LAMO_LAMPM_INTERNAL_H
#define LAMO_LAMPM_INTERNAL_H

/*
 * lampm_internal.h — Shared internal header for the integrated Lamo
 * package manager.
 *
 * Refactor (Sprint 5): the integrated lampm used to be a single 2400+
 * line translation unit (src/lampm/lampm.c) mixing string/fs helpers,
 * manifest parsing, lockfile parsing, git operations, and the
 * per-subcommand handlers. It is now split into:
 *
 *   - lampm_util.c       : string/fs helpers + global option state
 *   - lampm_manifest.c   : lamo.pkg manifest load/write
 *   - lampm_lockfile.c   : lamo.lock lockfile load/write
 *   - lampm_git.c        : git clone / checkout / commit-query helpers
 *   - lampm.c (slim)     : lampm_main, lampm_configure,
 *                          lampm_is_subcommand, install_dependency,
 *                          all per-command_* handlers, print_usage,
 *                          print_command_help
 *
 * This header is the single point of contact between those files: it
 * defines the Manifest / Dependency / Lockfile / LockEntry structs,
 * extern-declares the global option vars (g_pm_*) defined in
 * lampm_util.c, and forward-declares every helper that's used across
 * the new module boundaries.
 *
 * Each new .c file MUST `#define _DEFAULT_SOURCE` and
 * `#define _POSIX_C_SOURCE 200809L` at the very top BEFORE including
 * this header, so the system headers below expose popen / pclose /
 * S_IFDIR / PATH_MAX etc. under -std=c99. Matching the original lampm.c.
 *
 * The public API in lampm.h is unchanged.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define popen _popen
#define pclose _pclose
#define ACCESS _access
#define MKDIR(path) _mkdir(path)
#define PATH_SEP '\\'
#define IS_PATH_TTY(fd) _isatty(fd)
#define NULL_DEVICE "NUL"
#else
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#define ACCESS access
#define MKDIR(path) mkdir(path, 0755)
#define PATH_SEP '/'
#define IS_PATH_TTY(fd) isatty(fd)
#define NULL_DEVICE "/dev/null"
#endif

#include "lampm.h"

#define MANIFEST_FILE "lamo.pkg"
#define LOCKFILE "lamo.lock"
#define DEFAULT_PACKAGES_DIR "lamo_modules"

/* ── ANSI color helpers ─────────────────────────────────────────────────── */

#define COL_RESET   "\x1b[0m"
#define COL_RED     "\x1b[31m"
#define COL_GREEN   "\x1b[32m"
#define COL_YELLOW  "\x1b[33m"
#define COL_BLUE    "\x1b[34m"
#define COL_CYAN    "\x1b[36m"
#define COL_BOLD    "\x1b[1m"
#define COL_DIM     "\x1b[2m"

/* ── Data types ────────────────────────────────────────────────────────── */

typedef struct {
    char* alias;
    char* repo;     /* e.g. "owner/repo" or full URL, no @ref */
    char* ref;      /* optional: branch/tag/commit (NULL = HEAD) */
} Dependency;

typedef struct {
    char* name;
    char* version;       /* project version (optional, informational) */
    char* packages_dir;
    Dependency* dependencies;
    size_t dependency_count;
    size_t dependency_capacity;
} Manifest;

typedef struct {
    char* alias;
    char* repo;
    char* ref;       /* the ref recorded in the lockfile (could be HEAD or pin) */
    char* commit;    /* the actual commit hash installed */
} LockEntry;

typedef struct {
    LockEntry* entries;
    size_t count;
    size_t capacity;
} Lockfile;

/* ── Global option state (defined in lampm_util.c) ──────────────────────── */

extern int g_pm_verbose;
extern int g_pm_quiet;
extern int g_pm_color;

/* ── String / path helpers (lampm_util.c) ───────────────────────────────── */

/* Note: named lampm_duplicate_string (not duplicate_string) to avoid a
 * link-time collision with src/cli/paths.c's duplicate_string, which is
 * a separate (functionally identical) helper that lives in the compiler
 * driver. Both used to be `static` in their respective .c files; making
 * them external exposed the collision. Renaming keeps the two modules
 * independent. */
char* lampm_duplicate_string(const char* value);
char* trim_whitespace(char* value);
char* strip_optional_quotes(char* value);
int split_key_value(char* line, char** key, char** value);
int file_exists(const char* path);
int directory_exists(const char* path);
int ensure_directory(const char* path);
int remove_directory_recursive(const char* path);
char* join_path(const char* base, const char* leaf);
char* current_directory_name(void);
const char* col(const char* code);
void info_msg(const char* fmt, ...);
void success_msg(const char* fmt, ...);
void verbose_msg(const char* fmt, ...);
void error_msg(const char* fmt, ...);

/* ── Manifest (lampm_manifest.c) ────────────────────────────────────────── */

void manifest_init(Manifest* manifest);
void manifest_free(Manifest* manifest);
int ensure_manifest_dependency_capacity(Manifest* manifest, size_t required_count);
int manifest_find_dependency(const Manifest* manifest, const char* alias);
int manifest_add_or_update_dependency(Manifest* manifest, const char* alias,
                                      const char* repo, const char* ref);
int manifest_remove_dependency(Manifest* manifest, const char* alias);
int manifest_parse_dependency_value(char* value, char** out_repo, char** out_ref);
int manifest_load(const char* path, Manifest* manifest);
int manifest_write(const char* path, const Manifest* manifest);

/* ── Lockfile (lampm_lockfile.c) ────────────────────────────────────────── */

void lockfile_init(Lockfile* lock);
void lockfile_free(Lockfile* lock);
int lockfile_ensure_capacity(Lockfile* lock, size_t required);
const LockEntry* lockfile_find(const Lockfile* lock, const char* alias);
int lockfile_set(Lockfile* lock, const char* alias, const char* repo,
                 const char* ref, const char* commit);
int lockfile_load(const char* path, Lockfile* lock);
int lockfile_write(const char* path, const Lockfile* lock);

/* ── Git helpers (lampm_git.c) ──────────────────────────────────────────── */

int parse_repo_spec(const char* raw, char** out_repo, char** out_ref);
char* repo_clone_url(const char* repo);
char* default_alias_from_repo(const char* repo);
int run_command(const char* command);
char* capture_command_output(const char* command);
char* git_head_commit(const char* directory);
int git_checkout(const char* directory, const char* ref);

/* ── Entry point + commands + install (lampm.c) ─────────────────────────── */

int install_dependency(const Manifest* manifest, const Dependency* dependency,
                       Lockfile* lock);
int command_init(int argc, char** argv);
int command_install(int argc, char** argv);
int command_update(int argc, char** argv);
int command_remove(int argc, char** argv);
int command_list(int argc, char** argv);
int command_info(int argc, char** argv);
int command_doctor(int argc, char** argv);
int command_cache(int argc, char** argv);
int command_lock(int argc, char** argv);
/* 2.3.0 scope reduction: command_outdated() and command_why() were
 * removed. command_why is kept as a stub that returns an error
 * pointing users at `info` (in case any external code links against
 * it), but lampm_main no longer dispatches to either. */
/* Note: named lampm_print_usage / lampm_print_command_help (not
 * print_usage / print_command_help) to avoid a link-time collision with
 * src/cli/help.c's print_usage / print_command_help, which is a
 * separate function (the lamo compiler help, not the lampm help).
 * Both used to be `static` in their respective .c files. */
void lampm_print_usage(const char* program_name);
void lampm_print_command_help(const char* program_name, const char* command);

#endif /* LAMO_LAMPM_INTERNAL_H */
