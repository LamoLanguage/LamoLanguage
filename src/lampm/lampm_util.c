/*
 * lampm_util.c — String / filesystem helpers and global option state for
 * the integrated Lamo package manager. See lampm_internal.h for the
 * overall design.
 *
 * Refactor (Sprint 5): these helpers used to be `static` inside the
 * 2400-line lampm.c. They are now external so that the manifest,
 * lockfile, git, and command modules can all call them without each
 * holding a private copy.
 *
 * The global option state (g_pm_verbose / g_pm_quiet / g_pm_color) is
 * defined here; lampm.c::lampm_configure() writes to it through the
 * extern decls in lampm_internal.h.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lampm_internal.h"

/* ── Global options ────────────────────────────────────────────────────── */

/* Default color = -1 (auto-detect; lampm_main will force to 0 if not a
 * TTY, and the truthiness check in col() treats any non-zero value as
 * "color on"). */
int g_pm_verbose = 0;
int g_pm_quiet = 0;
int g_pm_color = -1;

/* ── ANSI color helpers ─────────────────────────────────────────────────── */

const char* col(const char* code) {
    return g_pm_color ? code : "";
}

void info_msg(const char* fmt, ...) {
    va_list ap;
    if (g_pm_quiet) return;
    va_start(ap, fmt);
    fputs(col(COL_CYAN), stdout);
    vfprintf(stdout, fmt, ap);
    fputs(col(COL_RESET), stdout);
    fputc('\n', stdout);
    va_end(ap);
}

void success_msg(const char* fmt, ...) {
    va_list ap;
    if (g_pm_quiet) return;
    va_start(ap, fmt);
    fputs(col(COL_GREEN), stdout);
    vfprintf(stdout, fmt, ap);
    fputs(col(COL_RESET), stdout);
    fputc('\n', stdout);
    va_end(ap);
}

void verbose_msg(const char* fmt, ...) {
    va_list ap;
    if (!g_pm_verbose) return;
    va_start(ap, fmt);
    fputs(col(COL_DIM), stdout);
    vfprintf(stdout, fmt, ap);
    fputs(col(COL_RESET), stdout);
    fputc('\n', stdout);
    va_end(ap);
}

void error_msg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs(col(COL_RED), stderr);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputs(col(COL_RESET), stderr);
    fputc('\n', stderr);
    va_end(ap);
}

/* ── String / path helpers ──────────────────────────────────────────────── */

char* lampm_duplicate_string(const char* value) {
    size_t length;
    char* copy;

    if (!value) {
        return NULL;
    }

    length = strlen(value);
    copy = malloc(length + 1);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, value, length + 1);
    return copy;
}

char* trim_whitespace(char* value) {
    char* end;

    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
        value++;
    }

    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }

    *end = '\0';
    return value;
}

char* strip_optional_quotes(char* value) {
    size_t length = strlen(value);

    if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
        value[length - 1] = '\0';
        return value + 1;
    }

    return value;
}

int split_key_value(char* line, char** key, char** value) {
    char* separator = strchr(line, '=');

    if (!separator) {
        return 0;
    }

    *separator = '\0';
    *key = trim_whitespace(line);
    *value = strip_optional_quotes(trim_whitespace(separator + 1));
    return 1;
}

int file_exists(const char* path) {
    return ACCESS(path, 0) == 0;
}

int directory_exists(const char* path) {
    struct stat info;

    if (stat(path, &info) != 0) {
        return 0;
    }

    return (info.st_mode & S_IFDIR) != 0;
}

int ensure_directory(const char* path) {
    if (directory_exists(path)) {
        return 1;
    }

    if (MKDIR(path) == 0) {
        return 1;
    }

    error_msg("failed to create directory %s: %s", path, strerror(errno));
    return 0;
}

char* join_path(const char* base, const char* leaf) {
    size_t base_length = strlen(base);
    size_t leaf_length = strlen(leaf);
    int needs_separator = base_length > 0 && base[base_length - 1] != '/' && base[base_length - 1] != '\\';
    char* joined = malloc(base_length + (size_t)needs_separator + leaf_length + 1);

    if (!joined) {
        return NULL;
    }

    memcpy(joined, base, base_length);
    if (needs_separator) {
        joined[base_length++] = PATH_SEP;
    }
    memcpy(joined + base_length, leaf, leaf_length);
    joined[base_length + leaf_length] = '\0';
    return joined;
}

char* current_directory_name(void) {
    char buffer[4096];
    char* separator;

#ifdef _WIN32
    if (!_getcwd(buffer, sizeof(buffer))) {
#else
    if (!getcwd(buffer, sizeof(buffer))) {
#endif
        return lampm_duplicate_string("lamo-project");
    }

    separator = strrchr(buffer, '\\');
    if (!separator) {
        separator = strrchr(buffer, '/');
    }

    if (!separator || !separator[1]) {
        return lampm_duplicate_string(buffer);
    }

    return lampm_duplicate_string(separator + 1);
}

/* ── Directory removal (cross-platform) ─────────────────────────────────── */

#ifdef _WIN32
int remove_directory_recursive(const char* path) {
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    char pattern[4096];

    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        return RemoveDirectoryA(path) != 0;
    }

    do {
        char* child_path;

        if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) {
            continue;
        }

        child_path = join_path(path, entry.cFileName);
        if (!child_path) {
            FindClose(handle);
            return 0;
        }

        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            SetFileAttributesA(child_path, FILE_ATTRIBUTE_NORMAL);
            if (!remove_directory_recursive(child_path)) {
                free(child_path);
                FindClose(handle);
                return 0;
            }
        } else {
            SetFileAttributesA(child_path, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileA(child_path)) {
                free(child_path);
                FindClose(handle);
                return 0;
            }
        }

        free(child_path);
    } while (FindNextFileA(handle, &entry));

    FindClose(handle);
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryA(path) != 0;
}
#else
int remove_directory_recursive(const char* path) {
    DIR* directory = opendir(path);
    struct dirent* entry;

    if (!directory) {
        return rmdir(path) == 0;
    }

    while ((entry = readdir(directory)) != NULL) {
        char* child_path;
        struct stat info;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        child_path = join_path(path, entry->d_name);
        if (!child_path) {
            closedir(directory);
            return 0;
        }

        if (stat(child_path, &info) != 0) {
            free(child_path);
            closedir(directory);
            return 0;
        }

        if (S_ISDIR(info.st_mode)) {
            if (!remove_directory_recursive(child_path)) {
                free(child_path);
                closedir(directory);
                return 0;
            }
        } else if (unlink(child_path) != 0) {
            free(child_path);
            closedir(directory);
            return 0;
        }

        free(child_path);
    }

    closedir(directory);
    return rmdir(path) == 0;
}
#endif
