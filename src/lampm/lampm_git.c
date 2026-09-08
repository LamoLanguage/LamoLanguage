/*
 * lampm_git.c — Git / repository-spec helpers for the integrated Lamo
 * package manager. See lampm_internal.h for the overall design.
 *
 * Refactor (Sprint 5): these functions used to be `static` inside the
 * 2400-line lampm.c. They are now external so that install_dependency
 * and the per-command handlers (in lampm.c) can call them.
 *
 * Implementation notes:
 *   - run_command / capture_command_output go through system() / popen()
 *     so the git CLI must be on PATH. We use NULL_DEVICE ("/dev/null"
 *     on POSIX, "NUL" on Windows) for stderr redirection to avoid the
 *     old `2>nul` Windows-ism that wrote a literal file named "nul" on
 *     Linux when git emitted warnings.
 *   - parse_repo_spec accepts a wide variety of repo specs (owner/repo,
 *     owner/repo@ref, github.com/owner/repo, https://..., git+ssh://,
 *     git@host:path, etc.) and normalizes to a canonical "host/path"
 *     form. repo_clone_url re-expands that to an https URL.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lampm_internal.h"

/* parse_repo_spec: accept any of:
 *   owner/repo
 *   owner/repo@ref
 *   github.com/owner/repo
 *   github.com/owner/repo@ref
 *   https://github.com/owner/repo[.git]
 *   https://gitlab.com/owner/repo[.git]
 *   https://bitbucket.org/owner/repo[.git]
 *   git+https://host/path[.git]
 *   git@host:path[.git]
 *
 * On success, *out_repo holds the canonical repo string (with @ref removed)
 * and *out_ref holds the ref (or NULL). Both are heap-allocated; caller frees.
 */
int parse_repo_spec(const char* raw, char** out_repo, char** out_ref) {
    const char* at;
    const char* ref;
    char* repo;
    size_t repo_len;
    const char* prefix_https = "https://";
    const char* prefix_git_https = "git+https://";
    const char* prefix_git_ssh = "git+ssh://";
    const char* github = "github.com/";
    const char* gitlab = "gitlab.com/";
    const char* bitbucket = "bitbucket.org/";

    if (!raw || !*raw) {
        return 0;
    }

    /* Split off @ref (but careful: git@host:path also has an @, so only treat
     * @ as a ref separator when it appears AFTER a path separator or slash). */
    at = NULL;
    if (strncmp(raw, "git@", 4) == 0) {
        /* git@host:path.git@ref — the second @ is the ref. */
        const char* p = raw + 4;
        while (*p && *p != ':' && *p != '/') p++;
        if (*p) {
            p++;
            while (*p && *p != '@') p++;
            if (*p == '@') at = p;
        }
    } else {
        at = strchr(raw, '@');
    }

    if (at) {
        repo_len = (size_t)(at - raw);
        ref = at + 1;
        if (!*ref) {
            ref = NULL;
        }
    } else {
        repo_len = strlen(raw);
        ref = NULL;
    }

    repo = malloc(repo_len + 1);
    if (!repo) {
        return 0;
    }
    memcpy(repo, raw, repo_len);
    repo[repo_len] = '\0';

    /* Strip a trailing .git (we'll re-add it when constructing the clone URL). */
    if (repo_len > 4 && strcmp(repo + repo_len - 4, ".git") == 0) {
        repo[repo_len - 4] = '\0';
    }

    /* If the user passed `owner/repo` (no scheme, no host), default to GitHub. */
    if (strncmp(repo, prefix_https, strlen(prefix_https)) != 0 &&
        strncmp(repo, prefix_git_https, strlen(prefix_git_https)) != 0 &&
        strncmp(repo, prefix_git_ssh, strlen(prefix_git_ssh)) != 0 &&
        strncmp(repo, "git@", 4) != 0 &&
        strstr(repo, "github.com/") != repo &&
        strstr(repo, "gitlab.com/") != repo &&
        strstr(repo, "bitbucket.org/") != repo) {
        /* No scheme — assume GitHub shorthand owner/repo. */
        if (!strchr(repo, '/')) {
            error_msg("invalid repository spec `%s`: expected owner/repo or a full git URL", raw);
            free(repo);
            return 0;
        }
        /* Already in canonical owner/repo form. */
    } else {
        /* Has a scheme/host — strip down to host/path for storage. */
        char* stripped = repo;
        if (strncmp(stripped, prefix_https, strlen(prefix_https)) == 0) {
            stripped += strlen(prefix_https);
        } else if (strncmp(stripped, prefix_git_https, strlen(prefix_git_https)) == 0) {
            stripped += strlen(prefix_git_https);
        } else if (strncmp(stripped, prefix_git_ssh, strlen(prefix_git_ssh)) == 0) {
            stripped += strlen(prefix_git_ssh);
        }
        /* git@host:path → host/path */
        if (strncmp(stripped, "git@", 4) == 0) {
            char* colon = strchr(stripped, ':');
            if (colon) {
                memmove(stripped, stripped + 4, (size_t)(colon - stripped) - 4 + 1);
                /* Now stripped looks like host:path, replace : with / */
                stripped[colon - stripped - 4] = '/';
            }
        }

        /* Move the stripped version to a fresh allocation so the caller can
         * always free *out_repo. */
        {
            char* canonical = lampm_duplicate_string(stripped);
            free(repo);
            repo = canonical;
        }
    }

    (void)github; (void)gitlab; (void)bitbucket;

    *out_repo = repo;
    *out_ref = ref ? lampm_duplicate_string(ref) : NULL;
    return 1;
}

char* repo_clone_url(const char* repo) {
    /* repo is in canonical form: either "owner/repo" (GitHub shorthand) or
     * "host/path". Construct the HTTPS clone URL. The shorthand needs
     * github.com prepended; the full form already has the host. */
    const char* prefix_https = "https://";
    const char* github_host = "github.com/";
    const char* suffix = ".git";
    size_t length;
    char* clone_url;
    int need_host = 0;

    /* If repo is just "owner/repo" (single slash, no scheme/host), we need
     * to prepend github.com/. Heuristic: if the first segment before the
     * first slash contains a '.', it's a host (e.g. "gitlab.com/..."); else
     * it's an owner name and we default to GitHub. */
    {
        const char* first_slash = strchr(repo, '/');
        if (first_slash) {
            size_t first_seg_len = (size_t)(first_slash - repo);
            int has_dot = 0;
            size_t i;
            for (i = 0; i < first_seg_len; i++) {
                if (repo[i] == '.') {
                    has_dot = 1;
                    break;
                }
            }
            if (!has_dot) {
                need_host = 1;
            }
        }
    }

    length = strlen(prefix_https) +
             (need_host ? strlen(github_host) : 0) +
             strlen(repo) + strlen(suffix);
    clone_url = malloc(length + 1);
    if (!clone_url) {
        return NULL;
    }

    if (need_host) {
        sprintf(clone_url, "%s%s%s%s", prefix_https, github_host, repo, suffix);
    } else {
        sprintf(clone_url, "%s%s%s", prefix_https, repo, suffix);
    }
    return clone_url;
}

char* default_alias_from_repo(const char* repo) {
    /* repo is "owner/repo" or "host/path/.../repo" — take the last component. */
    const char* slash = strrchr(repo, '/');
    const char* name = slash ? slash + 1 : repo;

    /* Strip a lamo- prefix if present, for nicer default aliases. */
    if (strncmp(name, "lamo-", 5) == 0) {
        return lampm_duplicate_string(name + 5);
    }

    return lampm_duplicate_string(name);
}

/* ── Command execution ──────────────────────────────────────────────────── */

int run_command(const char* command) {
    int result = system(command);

    if (result != 0) {
        error_msg("command failed (exit %d): %s", result, command);
        return 0;
    }

    return 1;
}

char* capture_command_output(const char* command) {
    FILE* pipe = popen(command, "r");
    char buffer[256];
    char* output = NULL;
    size_t output_length = 0;

    if (!pipe) {
        return NULL;
    }

    while (fgets(buffer, sizeof(buffer), pipe)) {
        size_t chunk_length = strlen(buffer);
        char* resized = realloc(output, output_length + chunk_length + 1);
        if (!resized) {
            free(output);
            pclose(pipe);
            return NULL;
        }

        output = resized;
        memcpy(output + output_length, buffer, chunk_length);
        output_length += chunk_length;
        output[output_length] = '\0';
    }

    pclose(pipe);

    return output;
}

/* git_head_commit: returns the short SHA of HEAD in `directory`, or NULL. */
char* git_head_commit(const char* directory) {
    char command[4096];
    char* output;
    char* trimmed;

    /* NULL_DEVICE is "/dev/null" on POSIX, "NUL" on Windows. Using it
     * instead of the old `2>nul` Windows-ism fixes a real bug: the previous
     * code wrote a file literally named `nul` on Linux when git emitted
     * warnings to stderr. */
    snprintf(command, sizeof(command),
             "git -C \"%s\" rev-parse --short HEAD 2>%s",
             directory, NULL_DEVICE);
    output = capture_command_output(command);
    if (!output) {
        return NULL;
    }

    trimmed = trim_whitespace(output);
    if (*trimmed == '\0') {
        free(output);
        return NULL;
    }

    {
        char* commit = lampm_duplicate_string(trimmed);
        free(output);
        return commit;
    }
}

/* git_resolve_ref: returns the short SHA of `ref` in `directory`.
 *
 * Currently unused (kept under #if 0 for symmetry with the original
 * lampm.c, which kept it as a documented-but-unused helper for future
 * `install --resolve-ref` functionality). */
#if 0
static char* git_resolve_ref(const char* directory, const char* ref) {
    char command[4096];
    char* output;
    char* trimmed;

    snprintf(command, sizeof(command),
             "git -C \"%s\" rev-parse --short %s 2>%s",
             directory, ref, NULL_DEVICE);
    output = capture_command_output(command);
    if (!output) {
        return NULL;
    }

    trimmed = trim_whitespace(output);
    if (*trimmed == '\0') {
        free(output);
        return NULL;
    }

    {
        char* commit = lampm_duplicate_string(trimmed);
        free(output);
        return commit;
    }
}
#endif

int git_checkout(const char* directory, const char* ref) {
    char command[4096];

    snprintf(command, sizeof(command),
             "git -C \"%s\" checkout %s 2>%s",
             directory, ref, NULL_DEVICE);
    return run_command(command);
}
