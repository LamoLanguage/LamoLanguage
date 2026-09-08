/*
 * lampm_manifest.c — lamo.pkg manifest load / write / mutate.
 * See lampm_internal.h for the overall design.
 *
 * Refactor (Sprint 5): these functions used to be `static` inside the
 * 2400-line lampm.c. They are now external so that the per-command
 * handlers (in lampm.c) and install_dependency can call them.
 *
 * Manifest file format:
 *
 *     # Comment
 *     name = my-project
 *     version = 0.1.0
 *     packages_dir = lamo_modules
 *
 *     [dependencies]
 *     alias = owner/repo
 *     alias2 = owner/repo@v1.0.0
 *
 * Unknown keys are silently ignored (forward-compatible).
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lampm_internal.h"

void manifest_init(Manifest* manifest) {
    memset(manifest, 0, sizeof(*manifest));
}

void manifest_free(Manifest* manifest) {
    size_t i;

    free(manifest->name);
    free(manifest->version);
    free(manifest->packages_dir);

    for (i = 0; i < manifest->dependency_count; i++) {
        free(manifest->dependencies[i].alias);
        free(manifest->dependencies[i].repo);
        free(manifest->dependencies[i].ref);
    }

    free(manifest->dependencies);
    manifest_init(manifest);
}

int ensure_manifest_dependency_capacity(Manifest* manifest, size_t required_count) {
    Dependency* resized;
    size_t new_capacity;

    if (required_count <= manifest->dependency_capacity) {
        return 1;
    }

    new_capacity = manifest->dependency_capacity > 0 ? manifest->dependency_capacity * 2 : 4;
    while (new_capacity < required_count) {
        new_capacity *= 2;
    }

    resized = realloc(manifest->dependencies, sizeof(Dependency) * new_capacity);
    if (!resized) {
        return 0;
    }

    manifest->dependencies = resized;
    manifest->dependency_capacity = new_capacity;
    return 1;
}

int manifest_find_dependency(const Manifest* manifest, const char* alias) {
    size_t i;

    for (i = 0; i < manifest->dependency_count; i++) {
        if (strcmp(manifest->dependencies[i].alias, alias) == 0) {
            return (int)i;
        }
    }

    return -1;
}

int manifest_add_or_update_dependency(Manifest* manifest, const char* alias,
                                      const char* repo, const char* ref) {
    int index = manifest_find_dependency(manifest, alias);
    char* repo_copy;
    char* ref_copy;

    if (index >= 0) {
        repo_copy = lampm_duplicate_string(repo);
        ref_copy = ref ? lampm_duplicate_string(ref) : NULL;
        if (!repo_copy || (ref && !ref_copy)) {
            free(repo_copy);
            free(ref_copy);
            return 0;
        }

        free(manifest->dependencies[index].repo);
        free(manifest->dependencies[index].ref);
        manifest->dependencies[index].repo = repo_copy;
        manifest->dependencies[index].ref = ref_copy;
        return 1;
    }

    if (!ensure_manifest_dependency_capacity(manifest, manifest->dependency_count + 1)) {
        return 0;
    }

    {
        Dependency* d = &manifest->dependencies[manifest->dependency_count];
        d->alias = lampm_duplicate_string(alias);
        d->repo = lampm_duplicate_string(repo);
        d->ref = ref ? lampm_duplicate_string(ref) : NULL;
        if (!d->alias || !d->repo || (ref && !d->ref)) {
            free(d->alias);
            free(d->repo);
            free(d->ref);
            d->alias = NULL;
            d->repo = NULL;
            d->ref = NULL;
            return 0;
        }
    }

    manifest->dependency_count++;
    return 1;
}

int manifest_remove_dependency(Manifest* manifest, const char* alias) {
    int index = manifest_find_dependency(manifest, alias);
    size_t i;

    if (index < 0) {
        return 0;
    }

    free(manifest->dependencies[index].alias);
    free(manifest->dependencies[index].repo);
    free(manifest->dependencies[index].ref);

    for (i = (size_t)index; i + 1 < manifest->dependency_count; i++) {
        manifest->dependencies[i] = manifest->dependencies[i + 1];
    }

    manifest->dependency_count--;
    return 1;
}

/* Manifest line syntax: alias = repo[@ref]
 * The @ref part is split off here so callers see a clean (repo, ref) pair. */
int manifest_parse_dependency_value(char* value, char** out_repo, char** out_ref) {
    char* at = strchr(value, '@');

    if (at) {
        *at = '\0';
        *out_repo = trim_whitespace(value);
        *out_ref = trim_whitespace(at + 1);
        if (**out_ref == '\0') {
            *out_ref = NULL;
        }
    } else {
        *out_repo = trim_whitespace(value);
        *out_ref = NULL;
    }

    return 1;
}

int manifest_load(const char* path, Manifest* manifest) {
    FILE* file = fopen(path, "r");
    char line[2048];
    int in_dependencies = 0;

    if (!file) {
        error_msg("failed to open %s: %s", path, strerror(errno));
        return 0;
    }

    manifest_init(manifest);

    while (fgets(line, sizeof(line), file)) {
        char* trimmed = trim_whitespace(line);
        char* key;
        char* value;

        if (*trimmed == '\0' || *trimmed == '#') {
            continue;
        }

        if (strcmp(trimmed, "[dependencies]") == 0) {
            in_dependencies = 1;
            continue;
        }

        if (*trimmed == '[') {
            in_dependencies = 0;
            continue;
        }

        if (!split_key_value(trimmed, &key, &value)) {
            error_msg("invalid manifest line: %s", trimmed);
            fclose(file);
            manifest_free(manifest);
            return 0;
        }

        if (in_dependencies) {
            char* repo;
            char* ref;
            if (!manifest_parse_dependency_value(value, &repo, &ref)) {
                error_msg("failed to parse dependency value: %s", value);
                fclose(file);
                manifest_free(manifest);
                return 0;
            }
            if (!manifest_add_or_update_dependency(manifest, key, repo, ref)) {
                error_msg("failed to load dependency from manifest");
                fclose(file);
                manifest_free(manifest);
                return 0;
            }
        } else if (strcmp(key, "name") == 0) {
            free(manifest->name);
            manifest->name = lampm_duplicate_string(value);
        } else if (strcmp(key, "version") == 0) {
            free(manifest->version);
            manifest->version = lampm_duplicate_string(value);
        } else if (strcmp(key, "packages_dir") == 0) {
            free(manifest->packages_dir);
            manifest->packages_dir = lampm_duplicate_string(value);
        }
        /* Unknown keys are silently ignored to allow forward-compatible manifests. */
    }

    fclose(file);

    if (!manifest->name) {
        manifest->name = lampm_duplicate_string("lamo-project");
    }

    if (!manifest->packages_dir) {
        manifest->packages_dir = lampm_duplicate_string(DEFAULT_PACKAGES_DIR);
    }

    if (!manifest->name || !manifest->packages_dir) {
        manifest_free(manifest);
        return 0;
    }

    return 1;
}

int manifest_write(const char* path, const Manifest* manifest) {
    FILE* file = fopen(path, "w");
    size_t i;

    if (!file) {
        error_msg("failed to write %s: %s", path, strerror(errno));
        return 0;
    }

    fprintf(file, "# Lamo package manifest. Edit by hand or use `lampm install/remove`.\n");
    fprintf(file, "name = %s\n", manifest->name ? manifest->name : "lamo-project");
    if (manifest->version) {
        fprintf(file, "version = %s\n", manifest->version);
    }
    fprintf(file, "packages_dir = %s\n\n",
            manifest->packages_dir ? manifest->packages_dir : DEFAULT_PACKAGES_DIR);
    fprintf(file, "[dependencies]\n");

    for (i = 0; i < manifest->dependency_count; i++) {
        if (manifest->dependencies[i].ref) {
            fprintf(file, "%s = %s@%s\n",
                    manifest->dependencies[i].alias,
                    manifest->dependencies[i].repo,
                    manifest->dependencies[i].ref);
        } else {
            fprintf(file, "%s = %s\n",
                    manifest->dependencies[i].alias,
                    manifest->dependencies[i].repo);
        }
    }

    fclose(file);
    return 1;
}
