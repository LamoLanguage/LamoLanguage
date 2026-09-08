/*
 * paths.c — File-system and path-manipulation helpers for the `lamo` CLI.
 * See paths.h for the design rationale.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
/* Windows: keep the include surface small (faster compiles, smaller .o). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <direct.h>
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#include "paths.h"

char* duplicate_string(const char* value) {
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

int path_is_absolute(const char* path) {
    if (!path || !path[0]) {
        return 0;
    }

#ifdef _WIN32
    if ((isalpha((unsigned char)path[0]) && path[1] == ':') || path[0] == '\\' || path[0] == '/') {
        return 1;
    }
#else
    if (path[0] == '/') {
        return 1;
    }
#endif

    return 0;
}

char* normalize_path(const char* path) {
#ifdef _WIN32
    char buffer[4096];

    if (!_fullpath(buffer, path, sizeof(buffer))) {
        return NULL;
    }

    return duplicate_string(buffer);
#else
    return realpath(path, NULL);
#endif
}

char* path_directory(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    const char* separator = slash;
    size_t length;
    char* directory;

    if (backslash && (!separator || backslash > separator)) {
        separator = backslash;
    }

    if (!separator) {
        return duplicate_string(".");
    }

    length = (size_t)(separator - path);
    if (length == 0) {
        length = 1;
    }

    directory = malloc(length + 1);
    if (!directory) {
        return NULL;
    }

    memcpy(directory, path, length);
    directory[length] = '\0';
    return directory;
}

char* path_join(const char* directory, const char* file_name) {
    size_t directory_length;
    size_t file_length;
    int needs_separator;
    char* joined;

    if (path_is_absolute(file_name)) {
        return duplicate_string(file_name);
    }

    directory_length = strlen(directory);
    file_length = strlen(file_name);
    needs_separator = directory_length > 0 &&
        directory[directory_length - 1] != '/' &&
        directory[directory_length - 1] != '\\';

    joined = malloc(directory_length + (size_t)needs_separator + file_length + 1);
    if (!joined) {
        return NULL;
    }

    memcpy(joined, directory, directory_length);
    if (needs_separator) {
#ifdef _WIN32
        joined[directory_length++] = '\\';
#else
        joined[directory_length++] = '/';
#endif
    }
    memcpy(joined + directory_length, file_name, file_length);
    joined[directory_length + file_length] = '\0';
    return joined;
}

char* resolve_import_path(const char* importing_file, const char* import_path) {
    char* directory = path_directory(importing_file);
    char* joined;
    char* normalized;

    /* Phase 3 (stdlib): if the import path starts with "std/" (case-sensitive),
     * we treat it as a standard-library import. We look in several candidate
     * locations, in order:
     *   1. $LAMO_STD_DIR/<import_path>      (env override, dev/CI use)
     *   2. <bindir>/std/<import_path>       (shipped alongside the binary)
     *   3. <bindir>/../std/<import_path>    (development layout)
     *   4. <bindir>/../share/lamo/std/<import_path>  (system install)
     *   5. ./std/<import_path>              (current working directory)
     *   6. <importing_file_dir>/std/<import_path>  (local std/ override)
     * The first existing file wins. If none exist, we fall back to the
     * default relative-path resolution below so the user gets a clear
     * "file not found" error from the loader. */
    if (import_path && strncmp(import_path, "std/", 4) == 0) {
        const char* env_std = getenv("LAMO_STD_DIR");
        /* Compute bindir (directory of the currently-running executable). */
        char bindir[4096];
#ifdef _WIN32
        DWORD n = GetModuleFileNameA(NULL, bindir, sizeof(bindir));
        if (n > 0 && n < sizeof(bindir)) {
            char* slash = strrchr(bindir, '\\');
            if (!slash) slash = strrchr(bindir, '/');
            if (slash) *slash = '\0';
            else { bindir[0] = '.'; bindir[1] = '\0'; }
        } else {
            strcpy(bindir, ".");
        }
#elif defined(__APPLE__)
        char pathbuf[4096];
        uint32_t bufsize = sizeof(pathbuf);
        if (_NSGetExecutablePath(pathbuf, &bufsize) == 0) {
            char* slash = strrchr(pathbuf, '/');
            if (slash) {
                size_t dir_len = (size_t)(slash - pathbuf);
                if (dir_len >= sizeof(bindir)) dir_len = sizeof(bindir) - 1;
                memcpy(bindir, pathbuf, dir_len);
                bindir[dir_len] = '\0';
            } else {
                strcpy(bindir, ".");
            }
        } else {
            strcpy(bindir, ".");
        }
#else
        char exe_path[4096];
        ssize_t link_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (link_len > 0) {
            exe_path[link_len] = '\0';
            char* slash = strrchr(exe_path, '/');
            if (slash) {
                size_t dir_len = (size_t)(slash - exe_path);
                if (dir_len >= sizeof(bindir)) dir_len = sizeof(bindir) - 1;
                memcpy(bindir, exe_path, dir_len);
                bindir[dir_len] = '\0';
            } else {
                strcpy(bindir, ".");
            }
        } else {
            strcpy(bindir, ".");
        }
#endif

        /* List of candidate directories. */
        const char* candidates[8];
        int n_candidates = 0;
        if (env_std && *env_std) candidates[n_candidates++] = env_std;
        {
            static char c1[4200];
            snprintf(c1, sizeof(c1), "%s/std", bindir);
            candidates[n_candidates++] = c1;
        }
        {
            static char c2[4200];
            snprintf(c2, sizeof(c2), "%s/../std", bindir);
            candidates[n_candidates++] = c2;
        }
        {
            static char c3[4200];
            snprintf(c3, sizeof(c3), "%s/../share/lamo/std", bindir);
            candidates[n_candidates++] = c3;
        }
        candidates[n_candidates++] = "./std";
        if (directory && *directory) {
            static char c5[4200];
            snprintf(c5, sizeof(c5), "%s/std", directory);
            candidates[n_candidates++] = c5;
        }

        {
            int i;
            for (i = 0; i < n_candidates; i++) {
                char full[4200];
                FILE* probe;
                snprintf(full, sizeof(full), "%s/%s", candidates[i], import_path + 4);
                probe = fopen(full, "rb");
                if (probe) {
                    fclose(probe);
                    return normalize_path(full);
                }
            }
        }
        /* If nothing found, fall through to default resolution so the
         * user gets a clear error pointing at the import statement. */
    }

    if (!directory) {
        return NULL;
    }

    joined = path_join(directory, import_path);
    free(directory);
    if (!joined) {
        return NULL;
    }

    normalized = normalize_path(joined);
    free(joined);
    return normalized;
}

char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    long size;
    char* content;
    size_t bytes_read;

    if (!file) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);

    content = malloc((size_t)size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }

    bytes_read = fread(content, 1, (size_t)size, file);
    content[bytes_read] = '\0';
    fclose(file);
    return content;
}

const char* executable_suffix(void) {
#ifdef _WIN32
    return ".exe";
#else
    return "";
#endif
}

/* lamo_cc: return the C compiler to use for `run`/`build`. Honors the
 * LAMO_CC environment variable (which can be set to "clang", "gcc-12",
 * "/usr/local/bin/cc", etc.). Returns "gcc" by default. */
const char* lamo_cc(void) {
    const char* env = getenv("LAMO_CC");
    if (env && env[0] != '\0') {
        return env;
    }
    return "gcc";
}
