#ifndef LAMO_CLI_PATHS_H
#define LAMO_CLI_PATHS_H

/*
 * paths.h — File-system and path-manipulation helpers for the `lamo` CLI.
 *
 * Refactor (Sprint 5): these helpers used to be `static` inside
 * lamo_v2.c. They are now external so that the import resolver
 * (import_resolver.c), subcommand handlers (commands.c), and compile
 * pipeline (compile.c) can all call them without each file owning a
 * private copy.
 *
 * Platform notes: resolve_import_path uses GetModuleFileNameA on
 * Windows, _NSGetExecutablePath on macOS, and readlink("/proc/self/exe")
 * on Linux to locate the directory of the running `lamo` binary so that
 * `import "std/..."` can find the standard library shipped alongside
 * the binary. The platform-specific code lives in paths.c.
 */

/* Heap-allocate a copy of `value`. Returns NULL on allocation failure
 * or if value is NULL. Caller frees. */
char* duplicate_string(const char* value);

/* Returns 1 if `path` is absolute (POSIX leading '/', Windows drive
 * letter or leading '\\'/'.'). Returns 0 otherwise (also for NULL/empty). */
int path_is_absolute(const char* path);

/* Resolve `path` to an absolute, canonical path. Returns a freshly
 * malloc'd string (caller frees) or NULL on failure. On POSIX uses
 * realpath(); on Windows uses _fullpath(). */
char* normalize_path(const char* path);

/* Return a heap-allocated string holding the directory portion of
 * `path` (everything up to but not including the last path separator).
 * Returns "." if `path` has no separator. Caller frees. */
char* path_directory(const char* path);

/* Join `directory` and `file_name` with a single platform-appropriate
 * separator in between. If `file_name` is already absolute, returns a
 * copy of it. Caller frees. */
char* path_join(const char* directory, const char* file_name);

/* Resolve an `import "..."` path relative to the importing file, with
 * special handling for std/ imports (looked up against $LAMO_STD_DIR,
 * the bindir, and a few fallbacks — see implementation). Returns a
 * malloc'd normalized path or NULL on failure. Caller frees. */
char* resolve_import_path(const char* importing_file, const char* import_path);

/* Read the entire file at `path` into a freshly malloc'd NUL-terminated
 * buffer. Returns NULL on open/read failure. Caller frees. */
char* read_file(const char* path);

/* Platform executable suffix: ".exe" on Windows, "" elsewhere. */
const char* executable_suffix(void);

/* Return the C compiler to use for `run`/`build`. Honors LAMO_CC env
 * var; defaults to "gcc". The returned pointer may point at env space
 * and is only valid until the next getenv() call. */
const char* lamo_cc(void);

#endif /* LAMO_CLI_PATHS_H */
