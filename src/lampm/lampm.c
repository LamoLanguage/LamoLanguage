/*
 * lampm.c — Entry point and per-subcommand handlers for the integrated
 * Lamo package manager.
 *
 * Originally a standalone binary (LamoPacketManager/src/main.c). Now
 * compiled into the main `lamo` executable and reachable through the
 * package-manager subcommands (lamo install, lamo update, lamo list, ...).
 * The single public entry point is lampm_main() declared in lampm.h.
 *
 * Improvement summary (v0.2.0):
 *   - Fixed POSIX build (added _POSIX_C_SOURCE / _DEFAULT_SOURCE so popen,
 *     pclose, S_IFDIR, PATH_MAX are exposed under -std=c99).
 *   - Fixed `2>nul` Windows-ism in git_head_commit; now uses platform null.
 *   - Version pinning: `lamo install owner/repo@ref` (branch/tag/commit).
 *   - Lockfile (lamo.lock) records the exact commit installed per dep, so
 *     `lamo install` (no args) reproduces the same checkout across machines.
 *   - New commands: update, outdated, info, doctor, cache, lock, why.
 *   - Non-GitHub sources: gitlab.com, bitbucket.org, git+https://, git@...:...
 *   - `lamo init` now scaffolds .gitignore (ignoring lamo_modules/ and
 *     lamo.lock optionally), an empty main.lamo, and a friendlier lamo.pkg.
 *   - Global flags: --verbose, --quiet, --no-color, --help/-h, --version/-v.
 *   - Per-command help: `lamo help <command>` or `lamo <command> --help`.
 *   - ANSI color output (auto-disabled on non-TTY or with --no-color).
 *   - Better diagnostics on git/network failures.
 *
 * Refactor (Sprint 5): this file used to be 2400+ lines, mixing string/fs
 * helpers, manifest parsing, lockfile parsing, git operations, and the
 * per-subcommand handlers all in one translation unit. It now contains
 * only:
 *
 *   - lampm_main / lampm_is_subcommand / lampm_configure (public API)
 *   - install_dependency (the shared install logic used by install/update)
 *   - all command_* handlers (init, install, update, remove, list, info,
 *     outdated, doctor, cache, lock, why)
 *   - print_usage / print_command_help
 *
 * Everything else has been split into sibling files (see lampm_internal.h
 * for the full map). The public API in lampm.h is unchanged.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lampm_internal.h"

/* ── Public API ────────────────────────────────────────────────────────── */

void lampm_configure(int verbose, int quiet, int color) {
    if (verbose >= 0) g_pm_verbose = verbose;
    if (quiet >= 0) g_pm_quiet = quiet;
    if (color >= 0) g_pm_color = color;
}

int lampm_is_subcommand(const char* name) {
    if (!name) return 0;
    if (strcmp(name, "init") == 0) return 1;
    if (strcmp(name, "install") == 0) return 1;
    if (strcmp(name, "update") == 0) return 1;
    if (strcmp(name, "remove") == 0) return 1;
    if (strcmp(name, "list") == 0) return 1;
    if (strcmp(name, "info") == 0) return 1;
    if (strcmp(name, "lock") == 0) return 1;
    if (strcmp(name, "cache") == 0) return 1;
    if (strcmp(name, "doctor") == 0) return 1;
    /* 2.3.0 scope reduction: `why` (pure alias of `info`) and `outdated`
     * (fragile network-dependent check) were removed. See lampm.h for
     * the rationale. Users who need `outdated`-style info can run
     * `lamo info <alias>` and compare the local commit against
     * origin/HEAD manually. */
    return 0;
}

/* ── Install ───────────────────────────────────────────────────────────── */

int install_dependency(const Manifest* manifest, const Dependency* dependency,
                       Lockfile* lock) {
    char* install_dir;
    char* clone_url;
    char command[4096];
    int success;
    char* commit = NULL;
    const LockEntry* locked = NULL;

    if (!ensure_directory(manifest->packages_dir)) {
        return 0;
    }

    install_dir = join_path(manifest->packages_dir, dependency->alias);
    clone_url = repo_clone_url(dependency->repo);
    if (!install_dir || !clone_url) {
        free(install_dir);
        free(clone_url);
        return 0;
    }

    if (lock) {
        locked = lockfile_find(lock, dependency->alias);
    }

    if (directory_exists(install_dir)) {
        /* Already installed. If we have a pinned ref or a locked commit, check
         * it out. Otherwise pull. */
        if (dependency->ref) {
            info_msg("Checking out %s@%s", dependency->alias, dependency->ref);
            snprintf(command, sizeof(command), "git -C \"%s\" fetch origin 2>%s",
                     install_dir, NULL_DEVICE);
            if (!run_command(command)) {
                free(install_dir);
                free(clone_url);
                return 0;
            }
            if (!git_checkout(install_dir, dependency->ref)) {
                free(install_dir);
                free(clone_url);
                return 0;
            }
        } else if (locked && locked->commit && strcmp(locked->ref, "HEAD") != 0) {
            /* Reproduce the locked commit. */
            info_msg("Restoring %s @ %s", dependency->alias, locked->commit);
            snprintf(command, sizeof(command), "git -C \"%s\" fetch origin 2>%s",
                     install_dir, NULL_DEVICE);
            if (!run_command(command)) {
                free(install_dir);
                free(clone_url);
                return 0;
            }
            if (!git_checkout(install_dir, locked->commit)) {
                free(install_dir);
                free(clone_url);
                return 0;
            }
        } else {
            info_msg("Updating %s from %s", dependency->alias, dependency->repo);
            snprintf(command, sizeof(command), "git -C \"%s\" pull --ff-only 2>%s",
                     install_dir, NULL_DEVICE);
            if (!run_command(command)) {
                free(install_dir);
                free(clone_url);
                return 0;
            }
        }
    } else {
        info_msg("Installing %s from %s%s%s",
                 dependency->alias,
                 dependency->repo,
                 dependency->ref ? "@" : "",
                 dependency->ref ? dependency->ref : "");
        snprintf(command, sizeof(command), "git clone --depth 50 \"%s\" \"%s\" 2>%s",
                 clone_url, install_dir, NULL_DEVICE);
        if (!run_command(command)) {
            error_msg("clone failed for %s — check that the repository exists and is public",
                      dependency->repo);
            free(install_dir);
            free(clone_url);
            return 0;
        }
        if (dependency->ref) {
            if (!git_checkout(install_dir, dependency->ref)) {
                free(install_dir);
                free(clone_url);
                return 0;
            }
        } else if (locked && locked->commit) {
            /* Try to check out the locked commit (best-effort: depth-50 may
             * not include it; if it fails, we just stay at HEAD). */
            verbose_msg("Attempting to restore locked commit %s for %s",
                        locked->commit, dependency->alias);
            if (git_checkout(install_dir, locked->commit)) {
                /* Fine. */
            } else {
                verbose_msg("could not check out %s (probably outside depth-50 clone); staying at HEAD",
                            locked->commit);
            }
        }
    }

    commit = git_head_commit(install_dir);
    if (commit) {
        verbose_msg("%s @ %s", dependency->alias, commit);
        if (lock) {
            lockfile_set(lock, dependency->alias, dependency->repo,
                         dependency->ref ? dependency->ref : "HEAD", commit);
        }
    } else if (lock) {
        lockfile_set(lock, dependency->alias, dependency->repo,
                     dependency->ref ? dependency->ref : "HEAD", NULL);
    }

    success = 1;
    free(install_dir);
    free(clone_url);
    free(commit);
    return success;
}

/* ── Commands ──────────────────────────────────────────────────────────── */

int command_init(int argc, char** argv) {
    Manifest manifest;
    const char* project_name = NULL;
    char* gitignore_path = NULL;
    char* main_path = NULL;
    FILE* gi = NULL;
    FILE* mainf = NULL;
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            lampm_print_command_help(argv[0], "init");
            return 0;
        }
        if (argv[i][0] == '-') {
            error_msg("unknown option: %s", argv[i]);
            return 1;
        }
        if (!project_name) {
            project_name = argv[i];
        } else {
            error_msg("unexpected extra argument: %s", argv[i]);
            return 1;
        }
    }

    if (file_exists(MANIFEST_FILE)) {
        error_msg("%s already exists in this directory", MANIFEST_FILE);
        return 1;
    }

    manifest_init(&manifest);
    manifest.name = project_name ? lampm_duplicate_string(project_name) : current_directory_name();
    manifest.version = lampm_duplicate_string("0.1.0");
    manifest.packages_dir = lampm_duplicate_string(DEFAULT_PACKAGES_DIR);

    if (!manifest.name || !manifest.packages_dir) {
        manifest_free(&manifest);
        return 1;
    }

    if (!ensure_directory(manifest.packages_dir)) {
        manifest_free(&manifest);
        return 1;
    }

    if (!manifest_write(MANIFEST_FILE, &manifest)) {
        manifest_free(&manifest);
        return 1;
    }

    /* Write a .gitignore if one doesn't already exist. */
    gitignore_path = lampm_duplicate_string(".gitignore");
    if (!file_exists(gitignore_path)) {
        gi = fopen(gitignore_path, "w");
        if (gi) {
            fprintf(gi, "# Lamo project\n");
            fprintf(gi, "%s/\n", manifest.packages_dir);
            fprintf(gi, "%s\n", LOCKFILE);
            fprintf(gi, "# Uncomment the next line to also ignore generated binaries:\n");
            fprintf(gi, "# lamo_exec*\n");
            fclose(gi);
            success_msg("wrote .gitignore");
        }
    }

    /* Scaffold a minimal main.lamo if the directory looks empty. */
    main_path = lampm_duplicate_string("main.lamo");
    if (!file_exists(main_path)) {
        mainf = fopen(main_path, "w");
        if (mainf) {
            fprintf(mainf, "// %s — entry point\n", manifest.name);
            fprintf(mainf, "fn main() {\n");
            fprintf(mainf, "    print(\"Hello from Lamo!\");\n");
            fprintf(mainf, "    return 0;\n");
            fprintf(mainf, "}\n");
            fprintf(mainf, "\n");
            fprintf(mainf, "main();\n");
            fclose(mainf);
            success_msg("wrote main.lamo");
        }
    }

    success_msg("Created %s for project %s%s%s",
                MANIFEST_FILE,
                col(COL_BOLD),
                manifest.name,
                col(COL_RESET));
    info_msg("Packages will be installed into %s/", manifest.packages_dir);
    info_msg("Next: `lampm install owner/repo` to add a dependency.");

    free(gitignore_path);
    free(main_path);
    manifest_free(&manifest);
    return 0;
}

int command_install(int argc, char** argv) {
    Manifest manifest;
    Lockfile lock;
    int status = 0;
    int write_lock = 0;
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            lampm_print_command_help(argv[0], "install");
            return 0;
        }
        if (argv[i][0] == '-') {
            error_msg("unknown option: %s", argv[i]);
            return 1;
        }
        break;
    }

    if (!file_exists(MANIFEST_FILE)) {
        error_msg("missing %s. Run `%s init` first.", MANIFEST_FILE, argv[0]);
        return 1;
    }

    if (!manifest_load(MANIFEST_FILE, &manifest)) {
        return 1;
    }

    lockfile_init(&lock);
    (void)lockfile_load(LOCKFILE, &lock);

    if (argc >= 3) {
        char* repo = NULL;
        char* ref = NULL;
        char* alias = NULL;

        if (!parse_repo_spec(argv[2], &repo, &ref)) {
            manifest_free(&manifest);
            lockfile_free(&lock);
            return 1;
        }

        alias = argc >= 4 ? lampm_duplicate_string(argv[3]) : default_alias_from_repo(repo);
        if (!alias) {
            free(repo);
            free(ref);
            manifest_free(&manifest);
            lockfile_free(&lock);
            return 1;
        }

        if (!manifest_add_or_update_dependency(&manifest, alias, repo, ref)) {
            error_msg("failed to update manifest dependencies");
            free(alias);
            free(repo);
            free(ref);
            manifest_free(&manifest);
            lockfile_free(&lock);
            return 1;
        }

        if (!manifest_write(MANIFEST_FILE, &manifest)) {
            free(alias);
            free(repo);
            free(ref);
            manifest_free(&manifest);
            lockfile_free(&lock);
            return 1;
        }

        {
            int index = manifest_find_dependency(&manifest, alias);
            if (index < 0 || !install_dependency(&manifest, &manifest.dependencies[index], &lock)) {
                status = 1;
            }
            write_lock = 1;
        }

        free(alias);
        free(repo);
        free(ref);
    } else {
        size_t j;

        if (manifest.dependency_count == 0) {
            info_msg("No dependencies recorded in %s", MANIFEST_FILE);
            manifest_free(&manifest);
            lockfile_free(&lock);
            return 0;
        }

        for (j = 0; j < manifest.dependency_count; j++) {
            if (!install_dependency(&manifest, &manifest.dependencies[j], &lock)) {
                status = 1;
                break;
            }
        }
        write_lock = 1;
    }

    if (write_lock && !lockfile_write(LOCKFILE, &lock)) {
        /* Lockfile write failure is non-fatal. */
        verbose_msg("could not write %s", LOCKFILE);
    }

    if (status == 0) {
        success_msg("Install complete");
    }

    manifest_free(&manifest);
    lockfile_free(&lock);
    return status;
}

int command_update(int argc, char** argv) {
    /* update [alias] — pull latest HEAD for one dep or all deps, ignoring
     * any locked commit. Updates the lockfile afterwards. */
    Manifest manifest;
    Lockfile lock;
    int status = 0;
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            lampm_print_command_help(argv[0], "update");
            return 0;
        }
        if (argv[i][0] == '-') {
            error_msg("unknown option: %s", argv[i]);
            return 1;
        }
        break;
    }

    if (!file_exists(MANIFEST_FILE)) {
        error_msg("missing %s. Run `%s init` first.", MANIFEST_FILE, argv[0]);
        return 1;
    }

    if (!manifest_load(MANIFEST_FILE, &manifest)) {
        return 1;
    }

    lockfile_init(&lock);

    if (argc >= 3) {
        int index = manifest_find_dependency(&manifest, argv[2]);
        if (index < 0) {
            error_msg("dependency `%s` not found in %s", argv[2], MANIFEST_FILE);
            manifest_free(&manifest);
            return 1;
        }
        /* Force update: pretend there's no pinned ref so install_dependency
         * pulls instead of checking out. We do this by temporarily clearing
         * the ref. */
        {
            char* saved_ref = manifest.dependencies[index].ref;
            manifest.dependencies[index].ref = NULL;
            if (!install_dependency(&manifest, &manifest.dependencies[index], &lock)) {
                status = 1;
            }
            manifest.dependencies[index].ref = saved_ref;
        }
    } else {
        size_t j;
        if (manifest.dependency_count == 0) {
            info_msg("No dependencies to update.");
            manifest_free(&manifest);
            return 0;
        }
        for (j = 0; j < manifest.dependency_count; j++) {
            char* saved_ref = manifest.dependencies[j].ref;
            manifest.dependencies[j].ref = NULL;
            if (!install_dependency(&manifest, &manifest.dependencies[j], &lock)) {
                status = 1;
            }
            manifest.dependencies[j].ref = saved_ref;
        }
    }

    if (!lockfile_write(LOCKFILE, &lock)) {
        verbose_msg("could not write %s", LOCKFILE);
    }

    if (status == 0) {
        success_msg("Update complete");
    }

    manifest_free(&manifest);
    lockfile_free(&lock);
    return status;
}

int command_remove(int argc, char** argv) {
    Manifest manifest;
    char* install_dir;
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            lampm_print_command_help(argv[0], "remove");
            return 0;
        }
        if (argv[i][0] == '-') {
            error_msg("unknown option: %s", argv[i]);
            return 1;
        }
        break;
    }

    if (argc < 3) {
        error_msg("missing dependency alias");
        return 1;
    }

    if (!file_exists(MANIFEST_FILE)) {
        error_msg("missing %s. Run `%s init` first.", MANIFEST_FILE, argv[0]);
        return 1;
    }

    if (!manifest_load(MANIFEST_FILE, &manifest)) {
        return 1;
    }

    install_dir = join_path(manifest.packages_dir, argv[2]);
    if (!install_dir) {
        manifest_free(&manifest);
        return 1;
    }

    if (!manifest_remove_dependency(&manifest, argv[2])) {
        error_msg("dependency `%s` was not found in %s", argv[2], MANIFEST_FILE);
        free(install_dir);
        manifest_free(&manifest);
        return 1;
    }

    if (!manifest_write(MANIFEST_FILE, &manifest)) {
        free(install_dir);
        manifest_free(&manifest);
        return 1;
    }

    if (directory_exists(install_dir) && !remove_directory_recursive(install_dir)) {
        error_msg("failed to remove installed package directory %s", install_dir);
        free(install_dir);
        manifest_free(&manifest);
        return 1;
    }

    success_msg("Removed %s", argv[2]);
    free(install_dir);
    manifest_free(&manifest);
    return 0;
}

int command_list(int argc, char** argv) {
    Manifest manifest;
    Lockfile lock;
    size_t i;

    for (i = 2; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            lampm_print_command_help(argv[0], "list");
            return 0;
        }
    }

    if (!file_exists(MANIFEST_FILE)) {
        error_msg("missing %s. Run `%s init` first.", MANIFEST_FILE, argv[0]);
        return 1;
    }

    if (!manifest_load(MANIFEST_FILE, &manifest)) {
        return 1;
    }

    lockfile_init(&lock);
    (void)lockfile_load(LOCKFILE, &lock);

    printf("%sProject:%s %s\n", col(COL_BOLD), col(COL_RESET), manifest.name);
    if (manifest.version) {
        printf("%sVersion:%s  %s\n", col(COL_BOLD), col(COL_RESET), manifest.version);
    }
    printf("%sModules dir:%s %s\n", col(COL_BOLD), col(COL_RESET), manifest.packages_dir);

    if (manifest.dependency_count == 0) {
        printf("%sDependencies:%s none\n", col(COL_BOLD), col(COL_RESET));
        manifest_free(&manifest);
        lockfile_free(&lock);
        return 0;
    }

    printf("%sDependencies:%s %zu\n", col(COL_BOLD), col(COL_RESET), manifest.dependency_count);
    for (i = 0; i < manifest.dependency_count; i++) {
        char* install_dir = join_path(manifest.packages_dir, manifest.dependencies[i].alias);
        char* commit = NULL;
        const LockEntry* locked = lockfile_find(&lock, manifest.dependencies[i].alias);

        if (install_dir && directory_exists(install_dir)) {
            commit = git_head_commit(install_dir);
        }

        printf("  %s%s%s -> %s%s%s%s",
               col(COL_CYAN), manifest.dependencies[i].alias, col(COL_RESET),
               manifest.dependencies[i].repo,
               manifest.dependencies[i].ref ? "@" : "",
               manifest.dependencies[i].ref ? manifest.dependencies[i].ref : "",
               col(COL_DIM));

        if (commit) {
            printf(" (installed @ %s)", commit);
            if (locked && locked->commit && strcmp(locked->commit, commit) != 0) {
                printf(" %s[locked: %s]%s", col(COL_YELLOW), locked->commit, col(COL_RESET));
            }
        } else if (install_dir && directory_exists(install_dir)) {
            printf(" (installed)");
        } else {
            printf(" %s(not installed)%s", col(COL_YELLOW), col(COL_RESET));
        }
        printf("%s\n", col(COL_RESET));

        free(commit);
        free(install_dir);
    }

    manifest_free(&manifest);
    lockfile_free(&lock);
    return 0;
}

int command_info(int argc, char** argv) {
    Manifest manifest;
    Lockfile lock;
    int index;
    char* install_dir;
    char* commit = NULL;
    char* branch = NULL;
    char* remote = NULL;
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            lampm_print_command_help(argv[0], "info");
            return 0;
        }
        if (argv[i][0] == '-') {
            error_msg("unknown option: %s", argv[i]);
            return 1;
        }
        break;
    }

    if (argc < 3) {
        error_msg("missing dependency alias");
        return 1;
    }

    if (!file_exists(MANIFEST_FILE)) {
        error_msg("missing %s.", MANIFEST_FILE);
        return 1;
    }

    if (!manifest_load(MANIFEST_FILE, &manifest)) {
        return 1;
    }

    index = manifest_find_dependency(&manifest, argv[2]);
    if (index < 0) {
        error_msg("dependency `%s` not found in %s", argv[2], MANIFEST_FILE);
        manifest_free(&manifest);
        return 1;
    }

    lockfile_init(&lock);
    (void)lockfile_load(LOCKFILE, &lock);

    install_dir = join_path(manifest.packages_dir, argv[2]);

    printf("%sDependency:%s %s\n", col(COL_BOLD), col(COL_RESET), argv[2]);
    printf("%sRepository:%s %s\n", col(COL_BOLD), col(COL_RESET),
           manifest.dependencies[index].repo);
    printf("%sPinned ref:%s %s\n", col(COL_BOLD), col(COL_RESET),
           manifest.dependencies[index].ref ? manifest.dependencies[index].ref : "(HEAD)");
    printf("%sInstall path:%s %s\n", col(COL_BOLD), col(COL_RESET),
           install_dir ? install_dir : "?");

    if (install_dir && directory_exists(install_dir)) {
        commit = git_head_commit(install_dir);
        {
            char command[4096];
            char* out;
            snprintf(command, sizeof(command), "git -C \"%s\" rev-parse --abbrev-ref HEAD 2>%s",
                     install_dir, NULL_DEVICE);
            out = capture_command_output(command);
            if (out) {
                char* trimmed = trim_whitespace(out);
                if (*trimmed) {
                    branch = lampm_duplicate_string(trimmed);
                }
                free(out);
            }
        }
        {
            char command[4096];
            char* out;
            snprintf(command, sizeof(command), "git -C \"%s\" config --get remote.origin.url 2>%s",
                     install_dir, NULL_DEVICE);
            out = capture_command_output(command);
            if (out) {
                char* trimmed = trim_whitespace(out);
                if (*trimmed) {
                    remote = lampm_duplicate_string(trimmed);
                }
                free(out);
            }
        }

        printf("%sBranch:%s %s\n", col(COL_BOLD), col(COL_RESET),
               branch ? branch : "(detached)");
        printf("%sCommit:%s %s\n", col(COL_BOLD), col(COL_RESET),
               commit ? commit : "(unknown)");
        printf("%sRemote URL:%s %s\n", col(COL_BOLD), col(COL_RESET),
               remote ? remote : "(unknown)");
    } else {
        printf("%sStatus:%s %snot installed%s\n",
               col(COL_BOLD), col(COL_RESET),
               col(COL_YELLOW), col(COL_RESET));
    }

    {
        const LockEntry* locked = lockfile_find(&lock, argv[2]);
        if (locked) {
            printf("%sLocked commit:%s %s\n", col(COL_BOLD), col(COL_RESET),
                   locked->commit ? locked->commit : "(none)");
        }
    }

    free(install_dir);
    free(commit);
    free(branch);
    free(remote);
    manifest_free(&manifest);
    lockfile_free(&lock);
    return 0;
}

/* 2.3.0 scope reduction: command_outdated() and command_why() were
 * removed. See lampm.h for the rationale. Users who need
 * outdated-style info can run `lamo info <alias>` and compare the
 * local commit against origin/HEAD manually. */

int command_doctor(int argc, char** argv) {
    /* doctor: verify environment is set up correctly. */
    int problems = 0;
    int j;

    for (j = 2; j < argc; j++) {
        if (strcmp(argv[j], "--help") == 0 || strcmp(argv[j], "-h") == 0) {
            lampm_print_command_help(argv[0], "doctor");
            return 0;
        }
    }

    printf("Lamo Packet Manager v%s — environment check\n\n", LAMPM_VERSION);

    /* git installed? */
    {
        char command[256];
        snprintf(command, sizeof(command), "git --version 2>%s", NULL_DEVICE);
        {
            char* out = capture_command_output(command);
            if (out) {
                char* trimmed = trim_whitespace(out);
                printf("  [+] git: %s\n", trimmed);
                free(out);
            } else {
                printf("  %s[-] git: not found on PATH%s\n", col(COL_RED), col(COL_RESET));
                printf("      lampm needs git to clone dependencies.\n");
                problems++;
            }
        }
    }

    /* manifest present? */
    if (file_exists(MANIFEST_FILE)) {
        Manifest m;
        if (manifest_load(MANIFEST_FILE, &m)) {
            printf("  [+] manifest: %s (%zu dependencies)\n",
                   MANIFEST_FILE, m.dependency_count);
            if (!m.name || strcmp(m.name, "lamo-project") == 0) {
                printf("  %s[!] manifest name is still the default — set it in %s%s\n",
                       col(COL_YELLOW), MANIFEST_FILE, col(COL_RESET));
            }
            manifest_free(&m);
        } else {
            printf("  %s[-] manifest: %s exists but failed to parse%s\n",
                   col(COL_RED), MANIFEST_FILE, col(COL_RESET));
            problems++;
        }
    } else {
        printf("  %s[-] manifest: %s not found (run `%s init`)%s\n",
               col(COL_YELLOW), MANIFEST_FILE, argv[0], col(COL_RESET));
        problems++;
    }

    /* lockfile present? */
    if (file_exists(LOCKFILE)) {
        printf("  [+] lockfile: %s\n", LOCKFILE);
    } else {
        printf("  %s[!] lockfile: %s not found (run `%s install` to generate)%s\n",
               col(COL_YELLOW), LOCKFILE, argv[0], col(COL_RESET));
    }

    /* packages_dir present? */
    {
        Manifest m;
        const char* pkgs_dir = DEFAULT_PACKAGES_DIR;
        if (file_exists(MANIFEST_FILE) && manifest_load(MANIFEST_FILE, &m)) {
            pkgs_dir = m.packages_dir;
            if (directory_exists(pkgs_dir)) {
                printf("  [+] packages dir: %s/\n", pkgs_dir);
            } else {
                printf("  %s[!] packages dir: %s/ missing (run `%s install`)%s\n",
                       col(COL_YELLOW), pkgs_dir, argv[0], col(COL_RESET));
            }
            manifest_free(&m);
        } else {
            printf("  %s[!] packages dir: %s/ (default; manifest not loaded)%s\n",
                   col(COL_YELLOW), pkgs_dir, col(COL_RESET));
        }
    }

    printf("\n");
    if (problems == 0) {
        success_msg("No critical problems found.");
        return 0;
    } else {
        error_msg("%d problem(s) found.", problems);
        return 1;
    }
}

int command_cache(int argc, char** argv) {
    /* cache clean: remove the packages_dir.
     * cache list: list contents of packages_dir. */
    Manifest manifest;
    int j;

    for (j = 2; j < argc; j++) {
        if (strcmp(argv[j], "--help") == 0 || strcmp(argv[j], "-h") == 0) {
            lampm_print_command_help(argv[0], "cache");
            return 0;
        }
        if (argv[j][0] == '-') {
            error_msg("unknown option: %s", argv[j]);
            return 1;
        }
        break;
    }

    if (argc < 3) {
        lampm_print_command_help(argv[0], "cache");
        return 1;
    }

    if (!file_exists(MANIFEST_FILE)) {
        error_msg("missing %s.", MANIFEST_FILE);
        return 1;
    }

    if (!manifest_load(MANIFEST_FILE, &manifest)) {
        return 1;
    }

    if (strcmp(argv[2], "clean") == 0 || strcmp(argv[2], "purge") == 0) {
        if (directory_exists(manifest.packages_dir)) {
            if (remove_directory_recursive(manifest.packages_dir)) {
                success_msg("Removed %s/", manifest.packages_dir);
                /* Recreate empty so subsequent installs don't fail. */
                ensure_directory(manifest.packages_dir);
            } else {
                error_msg("failed to remove %s/", manifest.packages_dir);
                manifest_free(&manifest);
                return 1;
            }
        } else {
            info_msg("%s/ does not exist.", manifest.packages_dir);
        }
    } else if (strcmp(argv[2], "list") == 0) {
        if (!directory_exists(manifest.packages_dir)) {
            info_msg("%s/ does not exist.", manifest.packages_dir);
            manifest_free(&manifest);
            return 0;
        }
#ifdef _WIN32
        {
            WIN32_FIND_DATAA entry;
            HANDLE handle;
            char pattern[4096];
            snprintf(pattern, sizeof(pattern), "%s\\*", manifest.packages_dir);
            handle = FindFirstFileA(pattern, &entry);
            if (handle == INVALID_HANDLE_VALUE) {
                info_msg("%s/ is empty.", manifest.packages_dir);
                manifest_free(&manifest);
                return 0;
            }
            do {
                if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) continue;
                printf("  %s\n", entry.cFileName);
            } while (FindNextFileA(handle, &entry));
            FindClose(handle);
        }
#else
        {
            DIR* dir = opendir(manifest.packages_dir);
            struct dirent* entry;
            if (!dir) {
                info_msg("%s/ is empty.", manifest.packages_dir);
                manifest_free(&manifest);
                return 0;
            }
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
                printf("  %s\n", entry->d_name);
            }
            closedir(dir);
        }
#endif
    } else {
        error_msg("unknown cache subcommand: %s (expected: clean | purge | list)", argv[2]);
        manifest_free(&manifest);
        return 1;
    }

    manifest_free(&manifest);
    return 0;
}

int command_lock(int argc, char** argv) {
    /* lock: refresh the lockfile from the currently-installed deps. */
    Manifest manifest;
    Lockfile lock;
    size_t i;
    int j;

    for (j = 2; j < argc; j++) {
        if (strcmp(argv[j], "--help") == 0 || strcmp(argv[j], "-h") == 0) {
            lampm_print_command_help(argv[0], "lock");
            return 0;
        }
    }

    if (!file_exists(MANIFEST_FILE)) {
        error_msg("missing %s.", MANIFEST_FILE);
        return 1;
    }

    if (!manifest_load(MANIFEST_FILE, &manifest)) {
        return 1;
    }

    lockfile_init(&lock);

    for (i = 0; i < manifest.dependency_count; i++) {
        char* install_dir = join_path(manifest.packages_dir, manifest.dependencies[i].alias);
        char* commit = NULL;
        if (install_dir && directory_exists(install_dir)) {
            commit = git_head_commit(install_dir);
        }
        lockfile_set(&lock, manifest.dependencies[i].alias,
                     manifest.dependencies[i].repo,
                     manifest.dependencies[i].ref ? manifest.dependencies[i].ref : "HEAD",
                     commit);
        free(install_dir);
        free(commit);
    }

    if (!lockfile_write(LOCKFILE, &lock)) {
        manifest_free(&manifest);
        lockfile_free(&lock);
        return 1;
    }

    success_msg("Wrote %s (%zu entries)", LOCKFILE, lock.count);
    manifest_free(&manifest);
    lockfile_free(&lock);
    return 0;
}

int command_why(int argc, char** argv) {
    /* 2.3.0 scope reduction: `why` was a pure alias for `info` with no
     * unique behavior. Removed to keep the command surface focused.
     * Kept as a stub returning an error so any external caller linking
     * against this translation unit doesn't break, but
     * `lampm_is_subcommand("why")` returns 0 and `lampm_main` no longer
     * dispatches here. Marked LAMO_UNUSED so -Wall doesn't warn. */
    (void)argc;
    (void)argv;
    error_msg("`why` was removed in 2.3.0 — use `info <alias>` instead");
    return 1;
}

#if 0
/* Disabled: command_outdated was removed in 2.3.0. Kept here as
 * reference in case we want to reintroduce it (perhaps behind a
 * --network flag) once the threading story is worked out. */
int command_outdated(int argc, char** argv) {
    /* ... original body ... */
}
#endif

/* ── Help ──────────────────────────────────────────────────────────────── */

void lampm_print_usage(const char* program_name) {
    printf("Lamo Packet Manager v%s (integrated into `lamo`)\n\n", LAMPM_VERSION);
    printf("%sUsage:%s\n", col(COL_BOLD), col(COL_RESET));
    printf("  %s %s<command>%s [options] [args]\n\n",
           program_name, col(COL_BOLD), col(COL_RESET));
    printf("%sCommands:%s\n", col(COL_BOLD), col(COL_RESET));
    printf("  %sinit%s [project-name]        Create a new lamo.pkg in the current directory\n",
           col(COL_CYAN), col(COL_RESET));
    printf("  %sinstall%s [owner/repo@ref]   Install a dependency (or all if no arg)\n",
           col(COL_CYAN), col(COL_RESET));
    printf("  %supdate%s [alias]              Pull latest HEAD for one or all deps\n",
           col(COL_CYAN), col(COL_RESET));
    printf("  %sremove%s <alias>              Remove a dependency and its install dir\n",
           col(COL_CYAN), col(COL_RESET));
    printf("  %slist%s                        List dependencies and their state\n",
           col(COL_CYAN), col(COL_RESET));
    printf("  %sinfo%s <alias>                Show details about a dependency\n",
           col(COL_CYAN), col(COL_RESET));
    printf("  %slock%s                        Refresh the lockfile from installed deps\n",
           col(COL_CYAN), col(COL_RESET));
    printf("  %scache%s <clean|list>          Manage the local packages directory\n",
           col(COL_CYAN), col(COL_RESET));
    printf("  %sdoctor%s                      Verify your environment is set up\n",
           col(COL_CYAN), col(COL_RESET));
    printf("  %shelp%s [command]              Show help (per-command if given)\n",
           col(COL_CYAN), col(COL_RESET));
    printf("  %sversion%s                     Print version and exit\n\n",
           col(COL_CYAN), col(COL_RESET));
    printf("%sGlobal options:%s\n", col(COL_BOLD), col(COL_RESET));
    printf("  %s--verbose%s     Show extra progress information\n",
           col(COL_YELLOW), col(COL_RESET));
    printf("  %s--quiet%s       Suppress informational output (errors only)\n",
           col(COL_YELLOW), col(COL_RESET));
    printf("  %s--no-color%s    Disable ANSI color output\n",
           col(COL_YELLOW), col(COL_RESET));
    printf("  %s--help, -h%s    Show this help (or per-command help)\n",
           col(COL_YELLOW), col(COL_RESET));
    printf("  %s--version, -v%s Print version\n\n",
           col(COL_YELLOW), col(COL_RESET));
    printf("Manifest: %s   Lockfile: %s   Modules: %s/\n",
           MANIFEST_FILE, LOCKFILE, DEFAULT_PACKAGES_DIR);
}

void lampm_print_command_help(const char* program_name, const char* command) {
    if (strcmp(command, "init") == 0) {
        printf("%sinit%s — Create a new Lamo project in the current directory.\n\n",
               col(COL_CYAN), col(COL_RESET));
        printf("Usage: %s init [project-name]\n\n", program_name);
        printf("Creates %s, %s/, .gitignore, and a minimal main.lamo\n",
               MANIFEST_FILE, DEFAULT_PACKAGES_DIR);
        printf("if they don't already exist. The project name defaults to the\n");
        printf("current directory name.\n");
    } else if (strcmp(command, "install") == 0) {
        printf("%sinstall%s — Install dependencies from GitHub/GitLab/etc.\n\n",
               col(COL_CYAN), col(COL_RESET));
        printf("Usage:\n");
        printf("  %s install <owner/repo[@ref]> [alias]\n", program_name);
        printf("  %s install                (install everything in lamo.pkg)\n\n", program_name);
        printf("Repository spec forms:\n");
        printf("  owner/repo                GitHub shorthand (HEAD)\n");
        printf("  owner/repo@v1.0.0         GitHub, pinned to tag/branch/commit\n");
        printf("  github.com/owner/repo\n");
        printf("  https://github.com/owner/repo[.git]\n");
        printf("  https://gitlab.com/owner/repo[.git]\n");
        printf("  git+https://example.com/foo/bar.git\n");
        printf("  git@github.com:owner/repo.git\n\n");
        printf("If no alias is given, the repo name (with `lamo-` prefix stripped)\n");
        printf("is used. After install, %s is updated to record the exact\n", LOCKFILE);
        printf("commit that was checked out, so the next `lampm install` reproduces it.\n");
    } else if (strcmp(command, "update") == 0) {
        printf("%supdate%s — Pull the latest HEAD for installed deps.\n\n",
               col(COL_CYAN), col(COL_RESET));
        printf("Usage:\n");
        printf("  %s update [alias]\n\n", program_name);
        printf("Without an alias, updates all deps. Ignores pinned refs and the\n");
        printf("lockfile, then refreshes %s with the new commit hashes.\n", LOCKFILE);
    } else if (strcmp(command, "remove") == 0) {
        printf("%sremove%s — Remove a dependency.\n\n", col(COL_CYAN), col(COL_RESET));
        printf("Usage: %s remove <alias>\n\n", program_name);
        printf("Removes the entry from %s and deletes the install dir.\n", MANIFEST_FILE);
    } else if (strcmp(command, "list") == 0) {
        printf("%slist%s — List dependencies and their state.\n\n",
               col(COL_CYAN), col(COL_RESET));
        printf("Usage: %s list\n\n", program_name);
        printf("Shows each dependency's alias, repo, pinned ref (if any), and\n");
        printf("installed commit. Differences between installed and locked\n");
        printf("commit are highlighted.\n");
    } else if (strcmp(command, "info") == 0) {
        printf("%sinfo%s — Show details about one dependency.\n\n",
               col(COL_CYAN), col(COL_RESET));
        printf("Usage: %s info <alias>\n\n", program_name);
        printf("Shows repo, pinned ref, install path, current branch, commit,\n");
        printf("and remote URL (if installed).\n");
    } else if (strcmp(command, "outdated") == 0 || strcmp(command, "why") == 0) {
        /* 2.3.0 scope reduction: these subcommands were removed. */
        printf("%s%s%s was removed in 2.3.0.\n",
               col(COL_YELLOW), command, col(COL_RESET));
        printf("Use `info <alias>` instead. See `lamo help` for the full list.\n");
    } else if (strcmp(command, "doctor") == 0) {
        printf("%sdoctor%s — Verify your environment is set up.\n\n",
               col(COL_CYAN), col(COL_RESET));
        printf("Usage: %s doctor\n\n", program_name);
        printf("Checks git is installed, %s exists and parses, the modules\n",
               MANIFEST_FILE);
        printf("dir is present, etc.\n");
    } else if (strcmp(command, "cache") == 0) {
        printf("%scache%s — Manage the local packages directory.\n\n",
               col(COL_CYAN), col(COL_RESET));
        printf("Usage:\n");
        printf("  %s cache clean   (alias: purge)  Delete the modules dir\n", program_name);
        printf("  %s cache list                     List installed package dirs\n", program_name);
    } else if (strcmp(command, "lock") == 0) {
        printf("%slock%s — Refresh the lockfile from installed deps.\n\n",
               col(COL_CYAN), col(COL_RESET));
        printf("Usage: %s lock\n\n", program_name);
        printf("Walks each dependency in %s, reads the currently-checked-out\n",
               MANIFEST_FILE);
        printf("commit, and writes %s.\n", LOCKFILE);
    } else if (strcmp(command, "help") == 0) {
        printf("%shelp%s — Show help.\n\n", col(COL_CYAN), col(COL_RESET));
        printf("Usage: %s help [command]\n\n", program_name);
        printf("Without an argument, shows general help. With a command name,\n");
        printf("shows detailed help for that command.\n");
    } else if (strcmp(command, "version") == 0) {
        printf("%sversion%s — Print lampm version and exit.\n",
               col(COL_CYAN), col(COL_RESET));
    } else {
        error_msg("no help available for `%s`", command);
    }
}

/* ── lampm_main ────────────────────────────────────────────────────────── */

int lampm_main(int argc, char** argv) {
    int arg_start = 1;
    int i;

    /* Auto-disable color if stdout/stderr isn't a TTY. The caller (lamo_v2.c
     * main) may have already configured g_pm_* via lampm_configure(); we
     * only force color off here when output isn't a TTY and the user didn't
     * explicitly pass --no-color. */
    if (!IS_PATH_TTY(fileno(stdout)) || !IS_PATH_TTY(fileno(stderr))) {
        g_pm_color = 0;
    }

    if (argc < 2) {
        lampm_print_usage(argv[0]);
        return 1;
    }

    /* Skip leading global flags that the caller may not have stripped. */
    while (arg_start < argc && argv[arg_start][0] == '-' &&
           strcmp(argv[arg_start], "--") != 0) {
        if (strcmp(argv[arg_start], "--verbose") == 0) {
            g_pm_verbose = 1;
        } else if (strcmp(argv[arg_start], "--quiet") == 0) {
            g_pm_quiet = 1;
        } else if (strcmp(argv[arg_start], "--no-color") == 0) {
            g_pm_color = 0;
        } else if (strcmp(argv[arg_start], "--help") == 0 ||
                   strcmp(argv[arg_start], "-h") == 0) {
            lampm_print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[arg_start], "--version") == 0 ||
                   strcmp(argv[arg_start], "-v") == 0) {
            printf("lampm %s\n", LAMPM_VERSION);
            return 0;
        } else {
            error_msg("unknown global option: %s", argv[arg_start]);
            return 1;
        }
        arg_start++;
    }

    if (arg_start >= argc) {
        lampm_print_usage(argv[0]);
        return 1;
    }

    /* Also accept trailing --verbose / --quiet / --no-color after the command. */
    for (i = arg_start + 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            g_pm_verbose = 1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            g_pm_quiet = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            g_pm_color = 0;
        }
    }

    if (strcmp(argv[arg_start], "help") == 0) {
        if (arg_start + 1 < argc) {
            lampm_print_command_help(argv[0], argv[arg_start + 1]);
        } else {
            lampm_print_usage(argv[0]);
        }
        return 0;
    }

    if (strcmp(argv[arg_start], "version") == 0) {
        printf("lampm %s\n", LAMPM_VERSION);
        return 0;
    }

    if (strcmp(argv[arg_start], "init") == 0) {
        return command_init(argc, argv);
    }

    if (strcmp(argv[arg_start], "install") == 0) {
        return command_install(argc, argv);
    }

    if (strcmp(argv[arg_start], "update") == 0) {
        return command_update(argc, argv);
    }

    if (strcmp(argv[arg_start], "remove") == 0) {
        return command_remove(argc, argv);
    }

    if (strcmp(argv[arg_start], "list") == 0) {
        return command_list(argc, argv);
    }

    if (strcmp(argv[arg_start], "info") == 0) {
        return command_info(argc, argv);
    }

    /* 2.3.0 scope reduction: `outdated` and `why` are no longer
     * dispatchable. If a user runs `lamo outdated` or `lamo why`,
     * they fall through to the "unknown subcommand" error below,
     * which suggests `lamo help` for the list of available commands. */

    if (strcmp(argv[arg_start], "doctor") == 0) {
        return command_doctor(argc, argv);
    }

    if (strcmp(argv[arg_start], "cache") == 0) {
        return command_cache(argc, argv);
    }

    if (strcmp(argv[arg_start], "lock") == 0) {
        return command_lock(argc, argv);
    }

    error_msg("unknown lampm subcommand: %s", argv[arg_start]);
    fprintf(stderr, "\n");
    lampm_print_usage(argv[0]);
    return 1;
}
