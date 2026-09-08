#ifndef LAMO_MODULES_H
#define LAMO_MODULES_H

/*
 * modules.h - Module registry for Lamo's namespaced import system.
 *
 * Sprint 4 feature: `import "math.lamo" as math;` exposes the imported
 * file's top-level declarations under a namespace. The loader renames
 * every top-level function/global in the imported file from `<name>` to
 * `lamo_mod_<alias>__<name>` before merging into the aggregate program,
 * and records the original name in this registry. The semantic pass and
 * codegen then consult the registry to resolve `math.sqrt(args)` style
 * member calls (AST_MEMBER_CALL) — they look up the alias, find the
 * original member name, and emit a call to the prefixed symbol.
 *
 * The registry is a flat array of {alias, members[]} entries. There is
 * no nesting (no `import a.b.c`) — Lamo's import path is a file path,
 * not a hierarchical module path, so one level of aliasing is enough.
 *
 * Lifetime: the registry is owned by CompilationState and lives for the
 * duration of a single compile. It is NOT thread-safe (the compiler is
 * single-threaded).
 */

#include <stddef.h>

typedef struct {
    const char* original_name;   /* e.g. "sqrt" — NOT owned by the registry */
    const char* prefixed_name;   /* e.g. "lamo_mod__math__sqrt" — owned */
    /* Sprint 4: arity for function members (-1 for non-function members
     * like global variables). Used by the semantic pass to validate
     * `module.member(args)` call arity the same way it validates regular
     * function calls. */
    int arity;
} LamoModuleMember;

typedef struct {
    char* alias;                 /* e.g. "math" — owned by the registry */
    LamoModuleMember* members;   /* dynamic array, owned */
    int member_count;
    int member_capacity;
} LamoModuleEntry;

typedef struct LamoModuleRegistry {
    LamoModuleEntry* entries;
    int count;
    int capacity;
} LamoModuleRegistry;

/* Initialize an empty registry (call once per compilation). */
void lamo_modules_init(LamoModuleRegistry* reg);

/* Free all memory owned by the registry (call at end of compilation). */
void lamo_modules_free(LamoModuleRegistry* reg);

/* Register a new module alias. Returns 1 on success, 0 on allocation
 * failure or duplicate alias (duplicates are an error — two `import`
 * statements with the same alias would be ambiguous). The alias string
 * is strdup'd. */
int lamo_modules_register_alias(LamoModuleRegistry* reg, const char* alias);

/* Add a member to a previously-registered alias. Both names are stored
 * as strdup'd copies (the caller may free its copies immediately).
 * `prefixed_name` is what the loader renamed the declaration to in the
 * AST. `arity` is the function's parameter count, or -1 for non-function
 * members (global variables). Returns 1 on success, 0 on allocation
 * failure or unknown alias. */
int lamo_modules_add_member(LamoModuleRegistry* reg, const char* alias,
                             const char* original_name, const char* prefixed_name,
                             int arity);

/* Look up an alias. Returns NULL if not found. */
const LamoModuleEntry* lamo_modules_lookup_alias(const LamoModuleRegistry* reg, const char* alias);

/* Look up a member within an alias. Returns the prefixed name to call,
 * or NULL if the alias is unknown OR the member is not registered under
 * that alias. The returned pointer is owned by the registry and lives
 * until lamo_modules_free() is called. */
const char* lamo_modules_resolve_member(const LamoModuleRegistry* reg,
                                         const char* alias, const char* member_name);

/* Sprint 4: look up a member's arity. Returns the arity (>= 0) for
 * function members, -1 for non-function members, or -1 if the alias or
 * member is not found. Used by the semantic pass to validate call arity. */
int lamo_modules_resolve_arity(const LamoModuleRegistry* reg,
                                const char* alias, const char* member_name);

/* Build the canonical prefix for a module alias: "lamo_mod_<alias>__".
 * Returns a freshly malloc'd string the caller must free. Returns NULL
 * on allocation failure. Used by the loader when renaming top-level
 * declarations of an aliased import. */
char* lamo_module_prefix(const char* alias);

#endif /* LAMO_MODULES_H */
