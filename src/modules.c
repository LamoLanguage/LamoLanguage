/*
 * modules.c - Implementation of the module registry (see modules.h).
 *
 * Sprint 4: backs the namespaced import feature. Lives in src/ so it
 * can be linked into the main `lamo` binary without adding a new
 * subdirectory. The registry is small and flat — we don't expect more
 * than a handful of modules per program, so a linear search is fine.
 */

#define _POSIX_C_SOURCE 200809L
#include "modules.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void lamo_modules_init(LamoModuleRegistry* reg) {
    reg->entries = NULL;
    reg->count = 0;
    reg->capacity = 0;
}

void lamo_modules_free(LamoModuleRegistry* reg) {
    int i, j;
    if (!reg) return;
    for (i = 0; i < reg->count; i++) {
        free(reg->entries[i].alias);
        for (j = 0; j < reg->entries[i].member_count; j++) {
            /* original_name points into the AST (the file_path-style
             * string the loader set); it's NOT owned by us. prefixed_name
             * was strdup'd in lamo_modules_add_member — free it. */
            free((void*)reg->entries[i].members[j].prefixed_name);
        }
        free(reg->entries[i].members);
    }
    free(reg->entries);
    reg->entries = NULL;
    reg->count = 0;
    reg->capacity = 0;
}

int lamo_modules_register_alias(LamoModuleRegistry* reg, const char* alias) {
    LamoModuleEntry* resized;
    int i;

    if (!reg || !alias) return 0;

    /* Reject duplicates. */
    for (i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].alias, alias) == 0) {
            return 0;
        }
    }

    if (reg->count == reg->capacity) {
        int new_cap = reg->capacity > 0 ? reg->capacity * 2 : 4;
        resized = realloc(reg->entries, sizeof(LamoModuleEntry) * (size_t)new_cap);
        if (!resized) return 0;
        reg->entries = resized;
        reg->capacity = new_cap;
    }

    reg->entries[reg->count].alias = strdup(alias);
    if (!reg->entries[reg->count].alias) return 0;
    reg->entries[reg->count].members = NULL;
    reg->entries[reg->count].member_count = 0;
    reg->entries[reg->count].member_capacity = 0;
    reg->count++;
    return 1;
}

int lamo_modules_add_member(LamoModuleRegistry* reg, const char* alias,
                             const char* original_name, const char* prefixed_name,
                             int arity) {
    LamoModuleEntry* entry = NULL;
    LamoModuleMember* resized;
    int i;

    if (!reg || !alias || !original_name || !prefixed_name) return 0;

    for (i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].alias, alias) == 0) {
            entry = &reg->entries[i];
            break;
        }
    }
    if (!entry) return 0;

    if (entry->member_count == entry->member_capacity) {
        int new_cap = entry->member_capacity > 0 ? entry->member_capacity * 2 : 8;
        resized = realloc(entry->members, sizeof(LamoModuleMember) * (size_t)new_cap);
        if (!resized) return 0;
        entry->members = resized;
        entry->member_capacity = new_cap;
    }

    /* original_name points into the AST (the loader set ASTFnDecl.name
     * to the prefixed name; the original name was the source string
     * before renaming). We store the pointer as-is, trusting the AST
     * stays alive for the duration of the compile. The prefixed_name
     * is also stored as-is — the loader passes a freshly allocated
     * string, which we'll free in lamo_modules_free(). */
    entry->members[entry->member_count].original_name = original_name;
    entry->members[entry->member_count].prefixed_name = strdup(prefixed_name);
    if (!entry->members[entry->member_count].prefixed_name) return 0;
    entry->members[entry->member_count].arity = arity;
    entry->member_count++;
    return 1;
}

const LamoModuleEntry* lamo_modules_lookup_alias(const LamoModuleRegistry* reg, const char* alias) {
    int i;
    if (!reg || !alias) return NULL;
    for (i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].alias, alias) == 0) {
            return &reg->entries[i];
        }
    }
    return NULL;
}

const char* lamo_modules_resolve_member(const LamoModuleRegistry* reg,
                                         const char* alias, const char* member_name) {
    const LamoModuleEntry* entry;
    int i;
    entry = lamo_modules_lookup_alias(reg, alias);
    if (!entry) return NULL;
    for (i = 0; i < entry->member_count; i++) {
        if (strcmp(entry->members[i].original_name, member_name) == 0) {
            return entry->members[i].prefixed_name;
        }
    }
    return NULL;
}

int lamo_modules_resolve_arity(const LamoModuleRegistry* reg,
                                const char* alias, const char* member_name) {
    const LamoModuleEntry* entry;
    int i;
    entry = lamo_modules_lookup_alias(reg, alias);
    if (!entry) return -1;
    for (i = 0; i < entry->member_count; i++) {
        if (strcmp(entry->members[i].original_name, member_name) == 0) {
            return entry->members[i].arity;
        }
    }
    return -1;
}

char* lamo_module_prefix(const char* alias) {
    /* "lamo_mod_" + alias + "__" + NUL */
    size_t alias_len;
    char* out;
    if (!alias) return NULL;
    alias_len = strlen(alias);
    out = malloc(alias_len + 8 + 2 + 1);
    if (!out) return NULL;
    memcpy(out, "lamo_mod_", 9);
    memcpy(out + 9, alias, alias_len);
    memcpy(out + 9 + alias_len, "__", 2);
    out[9 + alias_len + 2] = '\0';
    return out;
}
