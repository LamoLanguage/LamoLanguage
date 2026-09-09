#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../builtins.h"
#include "../error_util.h"
#include "semantic.h"
#include "lexer.h"
#include "../modules.h"

// ---------------------------------------------------------------------------
// Type model used by the semantic analyzer.
//
// Lamo is dynamically typed at runtime, but the semantic pass does a
// best-effort compile-time check on operators that would always crash at
// runtime (e.g. `"abc" * 3`). The inferred type is attached to each Symbol
// and propagated through expressions.
//
// LAMO_TYPE_UNKNOWN is used for "could not infer" (e.g. function return type
// before it is analyzed, or after a previous error). Operations involving
// UNKNOWN are not flagged so we don't cascade errors.
// ---------------------------------------------------------------------------
typedef enum {
    LAMO_TYPE_UNKNOWN,
    LAMO_TYPE_INT,
    LAMO_TYPE_FLOAT,
    LAMO_TYPE_STRING,
    LAMO_TYPE_BOOL,
    /* Phase 2: composite types. ARRAY is the dynamic array type from
     * Sprint 3; STRUCT is a user-defined struct. The struct's name is
     * stored separately on the Symbol (struct_name field) since multiple
     * distinct struct types exist. */
    LAMO_TYPE_ARRAY,
    LAMO_TYPE_STRUCT,
    /* Generics PR 2: VOID is produced by functions annotated `-> void`.
     * Per SPEC §6.3/§7.5, using a void value in a boolean context (if /
     * while / for conditions, && || ! operands) is a COMPILE-TIME error;
     * semantic_check_truthy_operand() enforces exactly that. */
    LAMO_TYPE_VOID,
    /* 2.6.0: a value of a tagged-union enum (SPEC §3.5). The concrete
     * enum's name rides on the AST node via sema_enum_name / symbol
     * struct_name, mirroring how structs are tracked. */
    LAMO_TYPE_ENUM
} LamoType;

typedef enum {
    SYMBOL_VAR,
    SYMBOL_FN
} SymbolKind;

typedef struct Symbol {
    char* name;
    SymbolKind kind;
    int arity;
    LamoType type;          // for SYMBOL_VAR: the variable's inferred type.
                            // for SYMBOL_FN: the inferred return type (UNKNOWN if not yet known).
    /* Sprint 2 fix: store the source location of the original declaration so
     * the duplicate-declaration error message can point the user at the
     * previous site, not just the new one. */
    int line;
    int column;
    /* Sprint 4: also store the originating file path so cross-file duplicate
     * declarations include the file name, not just line:col. Pointer is not
     * owned — it points into the AST node's file_path string which lives as
     * long as the ASTProgram. */
    const char* file_path;
    /* Phase 2: when type == LAMO_TYPE_STRUCT, this is the struct's type
     * name (e.g. "Player"). Borrowed pointer into the matching
     * ASTStructDecl->name — NOT owned. NULL for non-struct symbols. */
    const char* struct_name;
    /* ── Phase 3 item "track declarations by scope level" ────────────────
     * scope_level records the nesting depth at which this symbol was
     * declared: 0 = global/top-level, 1 = function body, 2+ = nested
     * blocks (if/while/for bodies and bare blocks). The Scope that owns
     * the symbol knows its own level too (see Scope below), so duplicate
     * detection and future visibility rules can reason about shadowing
     * across levels without re-walking scopes. */
    int scope_level;
    /* ── Generics PR 2: full annotated-signature storage ────────────────
     * All strings are NORMALIZED (semantic_normalize_type()) borrowed
     * pointers into the intern table — NOT individually freed.
     *
     * full_type   — variable's normalized annotation ("int",
     *               "array<int>", "pair<int,string>"); NULL when the let
     *               had no annotation or inference produced only the
     *               legacy LamoType enum value.
     * param_full  — array of arity entries; entry i is the normalized
     *               annotation of parameter i or NULL (unannotated).
     * ret_full    — normalized return annotation or NULL.
     * tp_names    — for generic fns: type parameter names ("T", ...
     *               aligned with constraints). tp_count==0 => non-generic.
     * tp_constraints — per-parameter constraint name from the PR 6
     *               catalogue ("Ord","Eq","Hash","Show","Num") or NULL. */
    const char* full_type;
    const char** param_full;
    const char* ret_full;
    const char** tp_names;
    const char** tp_constraints;
    int tp_count;
    struct Symbol* next;
} Symbol;

typedef struct Scope {
    Symbol* symbols;
    struct Scope* parent;
    /* Phase 3 item "track declarations by scope level": depth of this
     * scope in the nesting chain. The global scope created by
     * semantic_analyze_full has level 0; every scope_push adds 1. */
    int level;
} Scope;

/* Sprint 3: source lookup callback so semantic_error_at can print the
 * offending source line + caret. The compile_sources() caller registers
 * a function that maps file_path -> source text; in single-file builds
 * we just use the one source we have. */
typedef const char* (*SourceLookupFn)(const char* path, void* user_data);

typedef struct {
    const char* file_path;
    // Bug #5 fix: file_path do nó sendo visitado no momento. Setado em
    // semantic_visit_statement() com base no node->file_path da AST. Usado
    // por semantic_error_at() para reportar erros no arquivo correto.
    const char* last_node_path;
    Scope* current_scope;
    int inside_function;
    // Próximo passo 5: break/continue só são válidos dentro de while/for.
    // Incrementado ao entrar num loop, decrementado ao sair.
    int inside_loop;
    int errors;
    /* Sprint 3: source lookup for error snippets. May be NULL — in that
     * case semantic_error_at just omits the snippet. */
    SourceLookupFn source_lookup;
    void* source_lookup_user_data;
    /* Sprint 4: module-resolution callbacks. May be NULL — in that case
     * AST_MEMBER_CALL nodes always error with "module resolution not
     * available". When set (by lamo_v2.c via semantic_analyze_full),
     * they resolve `alias.member(args)` against the module registry. */
    LamoModuleResolveFn module_resolve;
    LamoModuleArityFn module_arity;
    void* module_user_data;
    /* 2.6.0 (FU5): direct registry handle for the §10.6 export-marker
     * warnings. May be NULL (then no pub warnings are emitted). Not
     * owned — lives in CompilationState. */
    struct LamoModuleRegistry* module_registry;
    /* Return-type tracking: set when entering a function body so that
     * AST_RETURN_STMT can validate the returned expression against the
     * declared (or inferred) return type. LAMO_TYPE_UNKNOWN means either
     * we are not inside a function or the return type could not be
     * determined (no annotation, no inferrable body). */
    LamoType current_fn_return_type;
    const char* current_fn_name;   /* for error messages; may be NULL */
    /* Phase 2: struct/enum/method registries.
     *
     * struct_defs: linked list of all AST_STRUCT_DECL nodes seen at top
     *   level. Used to look up field indices for AST_STRUCT_LITERAL,
     *   AST_PROP_EXPR (field access), AST_MEMBER_CALL (method call),
     *   and AST_PLACE_ASSIGN_STMT (field assignment).
     *
     * enum_defs: linked list of all AST_ENUM_DECL nodes. Used to resolve
     *   match-arm patterns and to register variant names as int constants.
     *
     * impl_defs: linked list of all AST_IMPL_DECL nodes. Used to look up
     *   methods by struct name + method name.
     *
     * trait_defs: 2.9.0 — walks the SAME declaration list, filtering
     *   AST_TRAIT_DECL nodes. Backs find_trait_def (constraint
     *   validation + impl completeness checks). */
    ASTNode* struct_defs;
    ASTNode* enum_defs;
    ASTNode* impl_defs;
    ASTNode* trait_defs;
    /* Phase 2: when visiting an impl block, this is set to the struct
     * name so that `self` references inside method bodies can be
     * resolved. NULL outside of impl method bodies. */
    const char* current_impl_struct;
    /* Generics PR 2 §4.4: type parameters inherited from an enclosing
     * generic impl (`impl<T> Stack<T>`). Method signatures may use these
     * like their own fn type parameters. Borrowed into intern table;
     * NULL/0 outside impl blocks or for non-generic impls. */
    const char* const* impl_tp_names;
    int impl_tp_count;
    /* Generics PR 2 §4.3/§5.2: type parameters of the function CURRENTLY
     * being visited (`fn id<T>` while walking its body/literals). Set in
     * the AST_FN_DECL case; NULL/0 elsewhere. Struct literals like
     * `Option<T> { ... }` consult this so payloads can be parameterized
     * over the enclosing function's parameters.
     * 2.10.0: cur_fn_tp_constraints mirrors the names 1:1 (borrowed from
     * the fn decl's type_param_constraints) so trait-dictionary dispatch
     * can resolve the constraint of a receiver's type parameter. */
    const char* const* cur_fn_tp_names;
    int cur_fn_tp_count;
    const char* const* cur_fn_tp_constraints;
} SemanticContext;

static void semantic_visit_statement(SemanticContext* ctx, ASTNode* node);
static LamoType semantic_infer_expression(SemanticContext* ctx, ASTNode* node);
/* 2.8.0 (FU4): shared match validation (statement + expression forms);
 * returns the LUB of the arm body types. */
static LamoType semantic_validate_match(SemanticContext* ctx, ASTMatchStmt* ms,
                                        ASTNode* node);
/* Phase 2 struct registry lookup — referenced by the PR 6 constraint
 * catalogue defined further below (ast.h is already included via
 * semantic.h, so the types are complete here). */
static ASTStructDecl* find_struct_def(SemanticContext* ctx, const char* name);
static void semantic_error_at(SemanticContext* ctx, int line, int column, const char* message);
/* 2.9.0 traits: trait registry lookups + static trait-constraint
 * satisfaction. Defined near the other registry helpers; forward-declared
 * here because the constraint-catalogue block below runs earlier in this
 * file. */
static ASTTraitDecl* find_trait_def(SemanticContext* ctx, const char* name);
static ASTImplDecl* find_trait_impl_before(SemanticContext* ctx, const char* trait_name,
                                           const char* struct_name, const ASTNode* before);
static int lamo_type_satisfies_trait(SemanticContext* ctx, const char* normalized,
                                     const char* trait_name);
/* 2.10.0 trait dictionary dispatch: build the hidden-dictionary stamp
 * for a call to a trait-constrained generic fn. Defined with the other
 * trait helpers; forward-declared for the call-site block. */
static void stamp_trait_dicts(SemanticContext* ctx, ASTNode* call_node,
                              const char* fn_name,
                              const char* const* tp_names, int tp_count,
                              const char* const* tp_constraints,
                              const char* const* bound_values);
/* 2.9.0: find_enum_def is defined further down (with the other registry
 * helpers) but the trait-satisfaction helper above already calls it. */
static ASTEnumDecl* find_enum_def(SemanticContext* ctx, const char* name);
/* Generics PR 2/3: annotation resolvers — defined after the machinery
 * block that uses them; declared here. */
static LamoType annotation_to_type_with_ctx(SemanticContext* ctx, const char* annotation);
static LamoType annotation_resolve_full(SemanticContext* ctx, const char* annotation, const char** out_norm);
/* Sprint 4: forward declaration so semantic_error_at can delegate to
 * the hinted variant below. */
static void semantic_error_at_hint(SemanticContext* ctx, int line, int column,
                                    const char* message, const char* hint);
/* Sprint 2 refactor: arity lookup is now a single call into builtins.h's
 * lamo_builtin_arity(). The forward declaration is kept so the call sites
 * below don't need to be renamed. */
static int builtin_function_arity(const char* name);
static LamoType builtin_function_return_type(const char* name, ASTNode** args, int arg_count);
static int semantic_validate_builtin_call(SemanticContext* ctx, const char* name, ASTNode** args, int arg_count, int line, int column);
/* 2.6.0 (FU5) step 1 → 2.7.0 (pub step 2) ENFORCED: §10.6 non-pub
 * module members are a compile error when reached through an alias —
 * defined near the bottom of this file; called from the AST_MEMBER_CALL
 * / AST_PROP_EXPR paths. */
static void semantic_error_module_export(SemanticContext* ctx,
                                         const char* alias, const char* member,
                                         int line, int column);

static const char* type_name(LamoType type) {
    switch (type) {
        case LAMO_TYPE_INT:    return "int";
        case LAMO_TYPE_FLOAT:  return "float";
        case LAMO_TYPE_STRING: return "string";
        case LAMO_TYPE_BOOL:   return "bool";
        case LAMO_TYPE_ARRAY:  return "array";
        case LAMO_TYPE_STRUCT: return "struct";
        case LAMO_TYPE_VOID:   return "void";
        case LAMO_TYPE_ENUM:   return "enum";
        case LAMO_TYPE_UNKNOWN: return "unknown";
    }
    return "unknown";
}

/* ════════════════════════════════════════════════════════════════════
 * Generics PR 2 / PR 3 / PR 6: normalized full-type machinery.
 *
 * The parser now delivers annotations like "array<int>" or
 * "Pair<int, array<string>>". To CHECK them (PR 2 call sites) and BIND
 * type parameters (T := int), we need three primitives:
 *
 *   semantic_normalize_type(raw) — canonical compact spelling
 *       - strips ALL whitespace around '<' ',' '>'
 *       - lowercases the head only when it spells a builtin
 *         ("Array" -> "array"); user struct names stay case-sensitive
 *   ann_equal(a, b)              — structural equality of two
 *                                  ALREADY-normalized types
 *   ann_subst(ann, map)          — replaces free type parameters with
 *                                  their bound concrete types,
 *                                  recursively, returning malloc'd text
 *   lamo_intern_type(str)        — stable borrowed pointer for storage
 *                                  on Symbols/AST nodes
 *
 * Everything else in the compiler keeps working unchanged: the legacy
 * LamoType enum path is untouched; this layer only ADDS checks when
 * annotations are present.
 * ════════════════════════════════════════════════════════════════════ */

/* Intern table: grows-only, process-lifetime. Strings stored here are
 * never freed — deliberate (the compiler is short-lived and this keeps
 * every borrowed pointer valid from semantic pass through codegen). */
static char** g_lamo_intern = NULL;
static int g_lamo_intern_count = 0;
static int g_lamo_intern_cap = 0;

static const char* lamo_intern_type(const char* s) {
    if (!s) return NULL;
    for (int i = 0; i < g_lamo_intern_count; i++) {
        if (strcmp(g_lamo_intern[i], s) == 0) return g_lamo_intern[i];
    }
    if (g_lamo_intern_count == g_lamo_intern_cap) {
        g_lamo_intern_cap = g_lamo_intern_cap ? g_lamo_intern_cap * 2 : 64;
        char** grown = realloc(g_lamo_intern, sizeof(char*) * (size_t)g_lamo_intern_cap);
        if (!grown) {
            perror("Failed to grow type intern table");
            exit(EXIT_FAILURE);
        }
        g_lamo_intern = grown;
    }
    char* copy = strdup(s);
    if (!copy) {
        perror("Failed to intern type string");
        exit(EXIT_FAILURE);
    }
    g_lamo_intern[g_lamo_intern_count++] = copy;
    return copy;
}

/* Is `head` a builtin type name in any accepted spelling? */
static int is_builtin_type_head(const char* head) {
    static const char* kBuiltins[] = {
        "int", "float", "string", "bool", "array", "void", NULL
    };
    for (int i = 0; kBuiltins[i]; i++) {
        if (strcmp(head, kBuiltins[i]) == 0) return 1;
        /* Accepted alternate spellings (RFC §7.1: Array<T> etc.). */
        char capbuf[32];
        snprintf(capbuf, sizeof(capbuf), "%c%s",
                 (char)(kBuiltins[i][0] - 32), kBuiltins[i] + 1);
        if (strcmp(head, capbuf) == 0) return 1;
    }
    return 0;
}

/* Normalize an annotation into a malloc'd compact string. */
static char* semantic_normalize_type(const char* raw) {
    if (!raw) return NULL;
    size_t n = strlen(raw);
    char* out = malloc(n + 1);
    if (!out) {
        perror("Failed to allocate normalized type buffer");
        exit(EXIT_FAILURE);
    }
    size_t o = 0;
    /* Extract head identifier. */
    size_t h = 0;
    while (h < n && raw[h] != '<') h++;
    /* Head word without spaces (defensive — parser output has none). */
    size_t head_end = h;
    while (head_end > 0 && (raw[head_end-1] == ' ' || raw[head_end-1] == '\t')) head_end--;
    int builtin_head = 0;
    {
        char headbuf[64];
        size_t len = head_end < sizeof(headbuf)-1 ? head_end : sizeof(headbuf)-1;
        memcpy(headbuf, raw, len);
        headbuf[len] = '\0';
        builtin_head = is_builtin_type_head(headbuf);
    }
    for (size_t i = 0; i < head_end; i++) {
        char c = raw[i];
        if (builtin_head && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[o++] = c;
    }
    /* Copy the remainder verbatim except whitespace. */
    for (size_t i = h; i < n; i++) {
        char c = raw[i];
        if (c == ' ' || c == '\t') continue;
        if (builtin_head && o < h && c >= 'A' && c <= 'Z') { /* unreachable guard */ }
        out[o++] = c;
    }
    out[o] = '\0';
    return out;
}

/* Base head of a normalized annotation ("array" of "array<int>"). */
static void ann_head(const char* ann, char* out, size_t out_size) {
    if (!ann) { out[0] = '\0'; return; }
    size_t i = 0;
    while (ann[i] && ann[i] != '<' && i + 1 < out_size) {
        out[i] = ann[i];
        i++;
    }
    out[i] = '\0';
}

/* Structural equality over normalized strings. Because normalization is
 * canonical (no spaces, builtin heads lowercase), plain strcmp suffices.
 * Kept as a named function so future canonicalizations (e.g. u8 vs byte)
 * have one place to change. */
static int ann_equal(const char* a, const char* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

/* Map for ann_subst: parallel arrays of names + normalized values. */
typedef struct {
    const char** names;     /* tp name e.g. "T"           */
    const char** values;    /* bound normalized type      */
    int count;
} AnnSubstMap;

/* Substitute type parameters per map, writing compact result into sb.
 * `pos` walks the annotation string; recursion handles nesting. Both
 * leave pos past what was consumed. Returns 1 on success. */
static int ann_subst_into(const char** pos, const AnnSubstMap* map, char* out, size_t out_size, size_t* olen);

static int ann_subst_ident(const char** pos, const AnnSubstMap* map, char* out, size_t out_size, size_t* olen) {
    const char* p = *pos;
    size_t o = *olen;
    size_t start = 0;
    while (p[start] && p[start] != '<' && p[start] != '>' &&
           p[start] != ',' ) start++;
    if (start == 0) return 0;
    char word[128];
    size_t w = start < sizeof(word)-1 ? start : sizeof(word)-1;
    memcpy(word, p, w);
    word[w] = '\0';
    /* Replace with binding when the word IS a type parameter. */
    const char* replacement = NULL;
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->names[i], word) == 0) { replacement = map->values[i]; break; }
    }
    const char* emit = replacement ? replacement : word;
    size_t elen = strlen(emit);
    if (o + elen + 1 > out_size) return 0;
    memcpy(out + o, emit, elen + 1);
    *olen = o + elen;
    *pos = p + start;
    return 1;
}

static int ann_subst_into(const char** pos, const AnnSubstMap* map, char* out, size_t out_size, size_t* olen) {
    const char* p = *pos;
    if (*p == '<') {
        /* Nesting opens right after an ident; recurse element-wise. */
        size_t o = *olen;
        if (o + 2 > out_size) return 0;
        out[o++] = '<';
        *olen = o;
        p++;  /* consume '<' */
        while (*p && *p != '>') {
            if (!ann_subst_into(&p, map, out, out_size, olen)) return 0;
            p = *p == ',' ? p : p;  /* no-op; handled below */
            if (*p == ',') {
                if (*olen + 2 > out_size) return 0;
                out[(*olen)++] = ',';
                out[*olen] = '\0';
                p++;
            }
        }
        if (*p != '>') return 0;
        if (*olen + 2 > out_size) return 0;
        out[(*olen)++] = '>';
        out[*olen] = '\0';
        *pos = p + 1;  /* consume '>' */
        return 1;
    }
    return ann_subst_ident(pos, map, out, out_size, olen);
}

/* Top-level subst wrapper. Returns malloc'd normalized substituted string,
 * or NULL on malformed input (which should not happen post-parse). */
static char* ann_subst(const char* ann, const AnnSubstMap* map) {
    if (!ann) return NULL;
    size_t cap = strlen(ann) * 4 + 64;
    char* buf = malloc(cap);
    if (!buf) {
        perror("Failed to allocate substitution buffer");
        exit(EXIT_FAILURE);
    }
    size_t len = 0;
    buf[0] = '\0';
    const char* pos = ann;
    while (*pos) {
        if (!ann_subst_into(&pos, map, buf, cap, &len)) {
            free(buf);
            return NULL;
        }
    }
    return buf;
}

/* ── PR 6: constraint catalogue ────────────────────────────────────── */

typedef enum {
    LAMO_CON_ANY,    /* every type                        */
    LAMO_CON_EQ,     /* == != support                     */
    LAMO_CON_ORD,    /* < <= > >= == !=                   */
    LAMO_CON_NUM,    /* + - * /                           */
    LAMO_CON_HASH,   /* hashable keys                     */
    LAMO_CON_SHOW    /* printable via print()/to_string   */
} LamoConstraintKind;

/* Map catalogue name -> kind. Returns -1 for unknown constraints. */
static int lamo_constraint_kind(const char* name) {
    if (!name) return -1;
    if (strcmp(name, "Any") == 0)  return LAMO_CON_ANY;
    if (strcmp(name, "Eq") == 0)   return LAMO_CON_EQ;
    if (strcmp(name, "Ord") == 0)  return LAMO_CON_ORD;
    if (strcmp(name, "Num") == 0)  return LAMO_CON_NUM;
    if (strcmp(name, "Hash") == 0) return LAMO_CON_HASH;
    if (strcmp(name, "Show") == 0) return LAMO_CON_SHOW;
    return -1;
}

static const char* lamo_constraint_name(int kind) {
    switch (kind) {
        case LAMO_CON_ANY:  return "Any";
        case LAMO_CON_EQ:   return "Eq";
        case LAMO_CON_ORD:  return "Ord";
        case LAMO_CON_NUM:  return "Num";
        case LAMO_CON_HASH: return "Hash";
        case LAMO_CON_SHOW: return "Show";
    }
    return "?";
}

/* RFC §6 initial implementation: constraints checked against BUILTIN
 * types only. User structs satisfy Any alone (documented limitation).
 * `normalized` must already be normalized; only its HEAD is inspected. */
static int lamo_type_satisfies_constraint(SemanticContext* ctx, const char* normalized, int kind) {
    if (kind == LAMO_CON_ANY) return 1;
    char head[64];
    ann_head(normalized, head, sizeof(head));
    int is_num = strcmp(head, "int") == 0 || strcmp(head, "float") == 0;
    int is_str = strcmp(head, "string") == 0;
    int is_bool = strcmp(head, "bool") == 0;
    switch (kind) {
        case LAMO_CON_EQ:
        case LAMO_CON_ORD:
        case LAMO_CON_HASH:
            return is_num || is_str || is_bool;
        case LAMO_CON_NUM:
            return is_num;
        case LAMO_CON_SHOW:
            return is_num || is_str || is_bool || strcmp(head, "array") == 0 ||
                   find_struct_def(ctx, head) != NULL ||
                   strcmp(head, "") == 0 /* unknown head: defer */;
        default:
            return 0;
    }
}

/* 2.9.0 traits: STATIC satisfaction of a user-declared trait constraint.
 * A concrete type satisfies trait `T` when the type is a declared struct
 * AND an `impl T for <struct>` exists (find_trait_impl). Builtins, arrays
 * and enums never satisfy user traits in 2.9.0 — impl blocks are
 * struct-only (documented limitation; the built-in catalogue covers
 * builtins). An UNKNOWN head defers (return 1) so error cascades don't
 * multiply — unknown-constraint names are reported at declaration. */
static int lamo_type_satisfies_trait(SemanticContext* ctx, const char* normalized,
                                     const char* trait_name) {
    if (!normalized || !trait_name) return 0;
    char head[64];
    ann_head(normalized, head, sizeof(head));
    if (strcmp(head, "") == 0) return 1;              /* unknown: defer */
    if (is_builtin_type_head(head)) return 0;         /* builtins: catalogue only */
    if (strcmp(head, "array") == 0) return 0;         /* arrays: no impls */
    if (find_enum_def(ctx, head)) return 0;           /* enums: no impls */
    if (!find_struct_def(ctx, head)) return 0;        /* not a declared struct */
    return find_trait_impl_before(ctx, trait_name, head, NULL) != NULL;
}

/* 2.10.0 trait dictionary dispatch: FORWARDING satisfaction. `val` may
 * be a type parameter of the ENCLOSING generic fn (`draw(x)` inside
 * `fn wrapper<T: Shape>(x: T)`). The concrete type argument is unknown
 * here, but the enclosing fn's own call sites already enforce `T:
 * Shape` — so an identical constraint on the enclosing parameter
 * transitively guarantees satisfaction. Only fn-level (top-level fn)
 * parameters forward: codegen emits hidden dictionary parameters for
 * generic fns, and methods cannot declare trait-constrained type
 * parameters. */
static int enclosing_tp_satisfies_trait(SemanticContext* ctx, const char* val,
                                        const char* trait_name) {
    if (!val || !trait_name) return 0;
    if (ctx->current_impl_struct) return 0;            /* method bodies: no forwarding */
    if (!ctx->cur_fn_tp_constraints) return 0;
    for (int t = 0; t < ctx->cur_fn_tp_count; t++) {
        const char* tpn = ctx->cur_fn_tp_names[t];
        const char* con = ctx->cur_fn_tp_constraints[t];
        if (tpn && con && strcmp(val, tpn) == 0 &&
            lamo_constraint_kind(con) < 0 && strcmp(con, trait_name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 2.10.0 trait dictionary dispatch: stamp the hidden dictionary
 * arguments for a call to a trait-constrained generic fn. One entry per
 * trait-constrained type parameter, in the callee's type-parameter
 * order (matching the codegen's hidden-parameter emission order). The
 * target is the concrete struct head of the bound value, or the
 * enclosing fn's type-parameter name when the value IS one (forwarding
 * the enclosing fn's own dictionary). A trait-constrained parameter
 * that is still UNBOUND at the call site is an error: the dictionary
 * must be known statically. */
static void stamp_trait_dicts(SemanticContext* ctx, ASTNode* call_node,
                              const char* fn_name,
                              const char* const* tp_names, int tp_count,
                              const char* const* tp_constraints,
                              const char* const* bound_values) {
    if (!call_node || tp_count <= 0) return;
    int dict_count = 0;
    for (int t = 0; t < tp_count; t++) {
        const char* con = tp_constraints ? tp_constraints[t] : NULL;
        if (!con || lamo_constraint_kind(con) >= 0) continue;
        if (!find_trait_def(ctx, con)) continue;
        if (!bound_values || !bound_values[t]) {
            char message[300];
            snprintf(message, sizeof(message),
                     "type parameter '%s' of '%s' could not be inferred at this call site (required to pass the '%s' trait dictionary)",
                     tp_names[t], fn_name ? fn_name : "?", con);
            semantic_error_at(ctx, call_node->line, call_node->column, message);
            return;
        }
        dict_count++;
    }
    if (dict_count == 0) return;
    SemaTraitDictList* list = ast_new_trait_dict_list(dict_count);
    int d = 0;
    for (int t = 0; t < tp_count; t++) {
        const char* con = tp_constraints ? tp_constraints[t] : NULL;
        if (!con || lamo_constraint_kind(con) >= 0) continue;
        if (!find_trait_def(ctx, con)) continue;
        const char* val = bound_values[t];
        const char* target = NULL;
        int forwards = 0;
        int is_enclosing_tp = 0;
        if (!ctx->current_impl_struct && ctx->cur_fn_tp_names) {
            for (int f = 0; f < ctx->cur_fn_tp_count; f++) {
                if (strcmp(val, ctx->cur_fn_tp_names[f]) == 0) { is_enclosing_tp = 1; break; }
            }
        }
        if (is_enclosing_tp) {
            target = val;                       /* forward enclosing dict */
            forwards = 1;
        } else {
            char head[64];
            ann_head(val, head, sizeof(head));
            target = lamo_intern_type(head);    /* bare struct name */
        }
        list->items[d].trait_name = con;
        list->items[d].target = target;
        list->items[d].forwards = forwards;
        d++;
    }
    call_node->sema_trait_dicts = list;
}

static int is_numeric_type(LamoType type) {
    return type == LAMO_TYPE_INT || type == LAMO_TYPE_FLOAT;
}

/* Generics PR 3 / migration §9: warning channel. Warnings go to stderr,
 * NEVER fail compilation and never appear on stdout — regression tests
 * that compare stdout stay unaffected. Format mirrors the error style:
 *     <file>:<line>:<col>: warning: <message> */
static void semantic_warn_at(SemanticContext* ctx, int line, int column, const char* message) {
    const char* label = ctx->last_node_path ? ctx->last_node_path :
                        (ctx->file_path ? ctx->file_path : "<input>");
    if (lamo_error_use_color()) {
        fprintf(stderr, "%s:%d:%d: %swarning:%s %s\n",
                label, line, column,
                LAMO_COLOR_BOLD, LAMO_COLOR_RESET, message);
    } else {
        fprintf(stderr, "%s:%d:%d: warning: %s\n", label, line, column, message);
    }
}

/* ── Truthiness-domain check (SPEC §6.3 / §7.5) ──────────────────────
 * All Lamo types have defined truthiness EXCEPT void. Conditions
 * (if/while/for) and &&/||/! operands reject VOID values at compile
 * time per the spec's explicit rule. UNKNOWN defers silently so error
 * cascades don't multiply. */
static void semantic_check_truthy_operand(SemanticContext* ctx, LamoType t,
                                          const char* where,
                                          int line, int column) {
    if (t == LAMO_TYPE_VOID) {
        char message[256];
        snprintf(message, sizeof(message),
                 "void value used in boolean context (%s); a function declared '-> void' returns nothing",
                 where);
        semantic_error_at(ctx, line, column, message);
    }
}

static Scope* scope_push(Scope* parent) {
    Scope* scope = malloc(sizeof(Scope));
    if (!scope) {
        perror("Failed to allocate semantic scope");
        exit(EXIT_FAILURE);
    }

    scope->symbols = NULL;
    scope->parent = parent;
    /* Phase 3 "track declarations by scope level": depth = parent+1,
     * global scope (NULL parent) stays at 0. */
    scope->level = parent ? parent->level + 1 : 0;
    return scope;
}

static void scope_free(Scope* scope) {
    Symbol* symbol = scope->symbols;
    while (symbol) {
        Symbol* next = symbol->next;
        free(symbol->name);
        free(symbol);
        symbol = next;
    }
    free(scope);
}

static void semantic_error_at(SemanticContext* ctx, int line, int column, const char* message) {
    /* Delegate to the hinted variant with no hint. */
    semantic_error_at_hint(ctx, line, column, message, NULL);
}

/* Sprint 4: semantic_error_at with an optional hint. The hint is
 * printed below the source snippet as "hint: <text>". Pass NULL when
 * no hint is appropriate. The hint should give actionable advice —
 * e.g. "did you mean to declare 'x' with `let x = ...;`?". */
static void semantic_error_at_hint(SemanticContext* ctx, int line, int column,
                                    const char* message, const char* hint) {
    // Bug #5 fix: usa o file_path do nó específico que disparou o erro,
    // não o label global do contexto. Em compilações multi-arquivo (programa
    // principal + imports), isso significa que o erro aponta para o arquivo
    // onde o problema realmente está, não para "<multiple inputs>".
    //
    // O caller passa o line/column do nó; aqui nós não recebemos o ponteiro
    // do nó diretamente, mas o ctx->last_node_path é setado por
    // semantic_visit_statement antes de chamar esta função para nós
    // específicos. Se last_node_path for NULL (caso legado), cai no
    // file_path do contexto.
    const char* label = ctx->last_node_path ? ctx->last_node_path :
                        (ctx->file_path ? ctx->file_path : "<input>");
    /* Sprint 4: color the "semantic error" label red+bold on TTY. */
    if (lamo_error_use_color()) {
        fprintf(stderr, "%s:%d:%d: %ssemantic error:%s %s\n",
                label, line, column,
                LAMO_COLOR_BOLD LAMO_COLOR_RED, LAMO_COLOR_RESET, message);
    } else {
        fprintf(stderr, "%s:%d:%d: semantic error: %s\n",
                label, line, column, message);
    }
    /* Sprint 3: print the source line + caret. We ask the registered
     * source-lookup callback for the source text of the current file;
     * if no callback is registered (e.g. semantic_analyze was called
     * directly without going through compile_sources), we just skip
     * the snippet. */
    if (ctx->source_lookup) {
        const char* source = ctx->source_lookup(label, ctx->source_lookup_user_data);
        if (source) {
            error_print_snippet(stderr, source, line, column);
        }
    }
    /* Sprint 4: print the hint below the snippet, if any. */
    error_print_hint(stderr, hint);
    ctx->errors++;
}

static Symbol* scope_find_in_current(Scope* scope, const char* name) {
    for (Symbol* symbol = scope->symbols; symbol; symbol = symbol->next) {
        if (strcmp(symbol->name, name) == 0) {
            return symbol;
        }
    }
    return NULL;
}

static Symbol* scope_find(Scope* scope, const char* name) {
    for (Scope* current = scope; current; current = current->parent) {
        Symbol* symbol = scope_find_in_current(current, name);
        if (symbol) {
            return symbol;
        }
    }
    return NULL;
}

static void scope_define(SemanticContext* ctx, Scope* scope, const char* name, SymbolKind kind, int arity, LamoType type, int line, int column, const char* file_path) {
    Symbol* existing = scope_find_in_current(scope, name);
    if (existing) {
        /* Sprint 2 fix: report the kind of the previously-declared symbol
         * and its source location, so the user can find the original
         * declaration without grepping.
         * Sprint 4: include the file name in cross-file conflicts. */
        char message[512];
        const char* prev_kind_str = existing->kind == SYMBOL_FN ? "function" : "variable";
        const char* new_kind_str = kind == SYMBOL_FN ? "function" : "variable";
        const char* prev_file = existing->file_path ? existing->file_path : "<unknown>";
        const char* cur_file  = file_path           ? file_path           : "<unknown>";
        int cross_file = (prev_file != cur_file) && (strcmp(prev_file, cur_file) != 0);
        if (cross_file) {
            snprintf(message, sizeof(message),
                     "duplicate declaration of '%s' as %s "
                     "(previously declared as %s in %s at %d:%d)",
                     name, new_kind_str, prev_kind_str, prev_file,
                     existing->line, existing->column);
        } else {
            snprintf(message, sizeof(message),
                     "duplicate declaration of '%s' as %s "
                     "(previously declared as %s at %d:%d)",
                     name, new_kind_str, prev_kind_str,
                     existing->line, existing->column);
        }
        semantic_error_at(ctx, line, column, message);
        return;
    }

    Symbol* symbol = malloc(sizeof(Symbol));
    if (!symbol) {
        perror("Failed to allocate semantic symbol");
        exit(EXIT_FAILURE);
    }

    symbol->name = strdup(name);
    symbol->kind = kind;
    symbol->arity = arity;
    symbol->type = type;
    symbol->line = line;
    symbol->column = column;
    symbol->file_path = file_path;
    symbol->struct_name = NULL;  /* Phase 2: set by callers via scope_define_struct */
    /* Phase 3: record which nesting level owns this declaration. */
    symbol->scope_level = scope->level;
    /* Generics PR 2: annotated-signature fields start empty; they are
     * populated right after registration by the fn/var visiting paths. */
    symbol->full_type = NULL;
    symbol->param_full = NULL;
    symbol->ret_full = NULL;
    symbol->tp_names = NULL;
    symbol->tp_constraints = NULL;
    symbol->tp_count = 0;
    symbol->next = scope->symbols;
    scope->symbols = symbol;
}

/* Phase 2: define a variable with a known struct type. The struct_name
 * is borrowed from the ASTStructDecl->name (NOT owned). */
static void scope_define_struct_var(SemanticContext* ctx, Scope* scope, const char* name, const char* struct_name, int line, int column, const char* file_path) {
    scope_define(ctx, scope, name, SYMBOL_VAR, 0, LAMO_TYPE_STRUCT, line, column, file_path);
    Symbol* sym = scope_find_in_current(scope, name);
    if (sym) sym->struct_name = struct_name;
}

/* ─── Phase 2: struct / enum / method lookup helpers ───────────────── */

/* Find a struct definition by name. Returns NULL if not found. */
static ASTStructDecl* find_struct_def(SemanticContext* ctx, const char* name) {
    ASTNode* cur;
    if (!name) return NULL;
    for (cur = ctx->struct_defs; cur; cur = cur->next) {
        if (cur->type == AST_STRUCT_DECL) {
            ASTStructDecl* sd = (ASTStructDecl*)cur;
            if (sd->name && strcmp(sd->name, name) == 0) return sd;
        }
    }
    return NULL;
}

/* Find the index of a field in a struct. Returns -1 if not found. */
static int struct_field_index(ASTStructDecl* sd, const char* field_name) {
    int i;
    if (!sd || !field_name) return -1;
    for (i = 0; i < sd->field_count; i++) {
        if (sd->field_names[i] && strcmp(sd->field_names[i], field_name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Find an enum definition by name. Returns NULL if not found. */
static ASTEnumDecl* find_enum_def(SemanticContext* ctx, const char* name) {
    ASTNode* cur;
    if (!name) return NULL;
    for (cur = ctx->enum_defs; cur; cur = cur->next) {
        if (cur->type == AST_ENUM_DECL) {
            ASTEnumDecl* ed = (ASTEnumDecl*)cur;
            if (ed->name && strcmp(ed->name, name) == 0) return ed;
        }
    }
    return NULL;
}

/* Find an enum variant by name across all registered enums. Returns the
 * variant's index (>= 0) via *out_index, and the enum's name via the
 * return value (borrowed pointer). Returns NULL if not found.
 *
 * 2.7.0 (FU4): iterates in REVERSE declaration order — when two enums
 * declare the same variant name, the LATER enum wins (SPEC §3.5). Bare
 * references to the shadowed variant stay reachable through
 * `Enum::Variant` qualification. */
static const char* find_enum_variant_any(SemanticContext* ctx, const char* variant_name, int* out_index) {
    ASTNode* cur;
    if (!variant_name) return NULL;
    /* Later wins: keep the LAST matching enum in declaration order. */
    ASTEnumDecl* best = NULL;
    int best_index = -1;
    for (cur = ctx->enum_defs; cur; cur = cur->next) {
        if (cur->type == AST_ENUM_DECL) {
            ASTEnumDecl* ed = (ASTEnumDecl*)cur;
            int i;
            for (i = 0; i < ed->variant_count; i++) {
                if (ed->variants[i] && strcmp(ed->variants[i], variant_name) == 0) {
                    best = ed;
                    best_index = i;
                    break;
                }
            }
        }
    }
    if (best) {
        if (out_index) *out_index = best_index;
        return best->name;
    }
    return NULL;
}

/* ── 2.6.0: tagged-union enum helpers (SPEC §3.5) ──────────────────── */

/* Number of payloads carried by variant `index` of `ed` (0 for unit
 * variants and for legacy untagged enums, which have no payload list). */
static int enum_variant_payload_count(const ASTEnumDecl* ed, int index) {
    if (!ed || !ed->variant_payload_counts || index < 0 || index >= ed->variant_count) {
        return 0;
    }
    return ed->variant_payload_counts[index];
}

/* An enum is a "tagged union" when any variant carries a payload.
 * Untagged enums keep the legacy plain-int runtime representation. */
static int enum_decl_is_tagged(const ASTEnumDecl* ed) {
    int i;
    if (!ed || !ed->variant_payload_counts) return 0;
    for (i = 0; i < ed->variant_count; i++) {
        if (ed->variant_payload_counts[i] > 0) return 1;
    }
    return 0;
}

/* 2.7.0 (FU4): resolve a QUALIFIED variant — `Enum::Variant`. Returns
 * the owning ASTEnumDecl and the variant index via *out_index, or NULL
 * when either the enum or the variant is unknown. Bypasses the bare
 * "later wins" lookup entirely, so shadowed variants stay reachable. */
static ASTEnumDecl* find_variant_qualified(SemanticContext* ctx, const char* enum_name,
                                           const char* variant_name, int* out_index) {
    ASTEnumDecl* ed = find_enum_def(ctx, enum_name);
    if (!ed) return NULL;
    for (int i = 0; i < ed->variant_count; i++) {
        if (ed->variants[i] && strcmp(ed->variants[i], variant_name) == 0) {
            if (out_index) *out_index = i;
            return ed;
        }
    }
    return NULL;
}

/* 2.7.0 (FU4): split a compound "Enum::Variant" name into its two
 * parts. Returns 1 when the name is qualified (parts written through
 * the out params), 0 when the name has no "::". */
static int split_qualified_variant_name(const char* name, char* out_enum, size_t enum_size,
                                        char* out_variant, size_t variant_size) {
    const char* sep = strstr(name, "::");
    if (!sep) return 0;
    size_t elen = (size_t)(sep - name);
    if (elen >= enum_size) elen = enum_size - 1;
    memcpy(out_enum, name, elen);
    out_enum[elen] = '\0';
    snprintf(out_variant, variant_size, "%s", sep + 2);
    return 1;
}

/* Find a method on a struct by name. Returns the AST_FN_DECL node, or
 * NULL if not found. Searches all impl blocks for the given struct. */
static ASTFnDecl* find_method(SemanticContext* ctx, const char* struct_name, const char* method_name) {
    ASTNode* cur;
    if (!struct_name || !method_name) return NULL;
    for (cur = ctx->impl_defs; cur; cur = cur->next) {
        if (cur->type == AST_IMPL_DECL) {
            ASTImplDecl* id = (ASTImplDecl*)cur;
            if (id->struct_name && strcmp(id->struct_name, struct_name) == 0) {
                for (ASTNode* m = id->methods; m; m = m->next) {
                    if (m->type == AST_FN_DECL) {
                        ASTFnDecl* fn = (ASTFnDecl*)m;
                        if (fn->name && strcmp(fn->name, method_name) == 0) {
                            return fn;
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

/* Find an impl block for a struct by name. Returns NULL if not found.
 * Generics PR 2 §4.4: used by typed method dispatch to resolve the
 * impl's type parameters against the receiver's instantiation. */
static ASTImplDecl* find_impl_decl(SemanticContext* ctx, const char* struct_name) {
    ASTNode* cur;
    if (!struct_name) return NULL;
    for (cur = ctx->impl_defs; cur; cur = cur->next) {
        if (cur->type == AST_IMPL_DECL) {
            ASTImplDecl* id = (ASTImplDecl*)cur;
            if (id->struct_name && strcmp(id->struct_name, struct_name) == 0) return id;
        }
    }
    return NULL;
}

/* ── 2.9.0 traits: registry helpers ────────────────────────────────── */

/* Find a trait declaration by name. Walks the same top-level list as
 * find_struct_def / find_impl_decl, filtering AST_TRAIT_DECL nodes, so
 * traits are order-independent (a trait declared after its use site
 * still resolves — same hoisting semantics as structs). */
static ASTTraitDecl* find_trait_def(SemanticContext* ctx, const char* name) {
    ASTNode* cur;
    if (!name) return NULL;
    for (cur = ctx->trait_defs; cur; cur = cur->next) {
        if (cur->type == AST_TRAIT_DECL) {
            ASTTraitDecl* td = (ASTTraitDecl*)cur;
            if (td->name && strcmp(td->name, name) == 0) return td;
        }
    }
    return NULL;
}

/* Find an EXISTING trait impl — `impl <trait_name> for <struct_name>` —
 * walking only nodes BEFORE `before` in the declaration list. The
 * before-bound makes the duplicate-impl check in the visit pass safe:
 * when validating the impl node itself, earlier impls are the only ones
 * that can conflict. Pass before = NULL to scan the whole list (used by
 * constraint satisfaction, which must see every impl regardless of
 * position). */
static ASTImplDecl* find_trait_impl_before(SemanticContext* ctx, const char* trait_name,
                                           const char* struct_name, const ASTNode* before) {
    ASTNode* cur;
    if (!trait_name || !struct_name) return NULL;
    for (cur = ctx->impl_defs; cur && cur != before; cur = cur->next) {
        if (cur->type == AST_IMPL_DECL) {
            ASTImplDecl* id = (ASTImplDecl*)cur;
            if (id->trait_name && id->struct_name &&
                strcmp(id->trait_name, trait_name) == 0 &&
                strcmp(id->struct_name, struct_name) == 0) {
                return id;
            }
        }
    }
    return NULL;
}

/* Generics PR 2: recursive annotation-tree validator for struct field
 * types. Every leaf identifier must be a builtin, a declared struct or
 * one of `sd`'s own type parameters; nesting is walked recursively.
 * Returns 1 when the whole tree resolves. */
static int lamo_validate_annotation_tree_cursor(SemanticContext* ctx, const ASTStructDecl* sd,
                                                const char* s, const char** end);
static int lamo_validate_annotation_tree(SemanticContext* ctx, const ASTStructDecl* sd, const char* ann) {
    if (!ann) return 0;
    char* norm = semantic_normalize_type(ann);
    const char* p = norm;
        int ok = lamo_validate_annotation_tree_cursor(ctx, sd, p, &p);
        /* Consume the end-pointer BEFORE freeing norm — dereferencing a
         * freed buffer is undefined behavior (this bit us once). */
        int fully_consumed = (*p == '\0');
            free(norm);
    return ok && fully_consumed;
}
static int lamo_validate_annotation_tree_cursor(SemanticContext* ctx, const ASTStructDecl* sd,
                                                const char* s, const char** end) {
    size_t i = 0;
    while (s[i] && s[i] != '<' && s[i] != '>') i++;
    if (i == 0) return 0;
    char head[64];
    size_t n = i < sizeof(head)-1 ? i : sizeof(head)-1;
    memcpy(head, s, n);
    head[n] = '\0';

    /* Leaf validity. */
    int leaf_ok = strcmp(head,"int")==0 || strcmp(head,"float")==0 ||
                  strcmp(head,"bool")==0 || strcmp(head,"string")==0 ||
                  strcmp(head,"array")==0 || strcmp(head,"void")==0;
    if (!leaf_ok && find_struct_def(ctx, head)) leaf_ok = 1;
    /* 2.7.0 (FU1): declared enums are valid annotation leaves —
     * `array<Option<int>>`, struct fields of enum type, and enum payload
     * types all resolve through here. Enum type-argument COUNTS are
     * validated separately at the declaration sites that care. */
    if (!leaf_ok && find_enum_def(ctx, head)) leaf_ok = 1;
    if (!leaf_ok && sd) {
        for (int j = 0; j < sd->type_param_count; j++) {
            if (strcmp(head, sd->type_params[j]) == 0) { leaf_ok = 1; break; }
        }
    }
    if (!leaf_ok && sd == NULL) {
        /* Called without a declaring struct (expression contexts): a leaf
         * may be any in-scope type parameter (impl-level or enclosing
         * fn-level) or a declared enum/builtin handled by callers. */
        int i;
        for (i = 0; i < ctx->impl_tp_count; i++) {
            if (strcmp(head, ctx->impl_tp_names[i]) == 0) { leaf_ok = 1; break; }
        }
        if (!leaf_ok) {
            for (i = 0; i < ctx->cur_fn_tp_count; i++) {
                if (strcmp(head, ctx->cur_fn_tp_names[i]) == 0) { leaf_ok = 1; break; }
            }
        }
    }
    if (!leaf_ok) return 0;
    s += i;

    if (*s != '<') { *end = s; return 1; }
    /* Nested list — walk element-wise recursively. */
    s++;
    while (*s && *s != '>') {
        if (!lamo_validate_annotation_tree_cursor(ctx, sd, s, &s)) return 0;
        if (*s == ',') s++;
        else break;
    }
    if (*s != '>') return 0;
    *end = s + 1;
    return 1;
}

static void semantic_visit_block(SemanticContext* ctx, ASTBlock* block) {
    Scope* parent = ctx->current_scope;
    ctx->current_scope = scope_push(parent);

    for (ASTNode* statement = block->statements; statement; statement = statement->next) {
        semantic_visit_statement(ctx, statement);
    }

    Scope* finished = ctx->current_scope;
    ctx->current_scope = parent;
    scope_free(finished);
}

/* ── Generics PR 2: compile-time type binding at call sites ────────── */

#define LAMO_MAX_BIND_ARGS 32

/* Generics PR 2 §4.4: find the (first) impl block declared for a struct. */
static ASTImplDecl* find_impl_decl(SemanticContext* ctx, const char* struct_name);
/* Defined later in this file; needed by typed method dispatch below. */
static const char* arg_concrete_full_type(SemanticContext* ctx, ASTNode* node);
static int ann_bind_pattern(const char* pattern, const char* concrete, AnnSubstMap* map);

/* Split top-level type arguments of a normalized instantiation
 * ("stack<int>" -> head="stack", args=["int"]). *out_args receives a
 * malloc'd array of interned strings (free the ARRAY only); returns the
 * count, or -1 when the annotation carries no angle list. */
static int ann_split_top_args(const char* norm, char* out_head, size_t head_size,
                              const char*** out_args) {
    *out_args = NULL;
    *out_head = '\0';
    const char* lt = strchr(norm, '<');
    if (!lt) {
        snprintf(out_head, head_size, "%s", norm);
        return -1;
    }
    size_t hlen = (size_t)(lt - norm);
    if (hlen >= head_size) hlen = head_size - 1;
    memcpy(out_head, norm, hlen);
    *(out_head + hlen) = '\0';

    /* Walk the angle list tracking depth, slicing between commas. */
    const char** items = NULL;
    int count = 0;
    const char* p = lt + 1;
    const char* start = p;
    int depth = 1;
    while (*p) {
        if (*p == '<') depth++;
        else if (*p == '>') { depth--; if (depth == 0) break; }
        else if (*p == ',' && depth == 1) {
            char slice[128];
            size_t len = (size_t)(p - start);
            len = len < sizeof(slice) - 1 ? len : sizeof(slice) - 1;
            memcpy(slice, start, len);
            slice[len] = '\0';
            const char** grown = realloc(items, sizeof(const char*) * (size_t)(count + 1));
            if (!grown) { free(items); return -1; }
            items = grown;
            items[count++] = lamo_intern_type(slice);
            start = p + 1;
        }
        p++;
    }
    if (depth != 0 || p == start) { free(items); return -1; }  /* empty <> */
    char slice[128];
    size_t len = (size_t)(p - start);
    len = len < sizeof(slice) - 1 ? len : sizeof(slice) - 1;
    memcpy(slice, start, len);
    slice[len] = '\0';
    const char** grown = realloc(items, sizeof(const char*) * (size_t)(count + 1));
    if (!grown) { free(items); return -1; }
    items = grown;
    items[count++] = lamo_intern_type(slice);
    *out_args = items;
    return count;
}

/* Typed struct-method dispatch (RFC §4.4 + §5): given a receiver whose
 * variable carries an instantiated full type ("stack<int>") and the
 * matched method's annotated signature, validate argument compatibility
 * and optionally annotate the call with the substituted return type.
 * Everything degrades gracefully to the legacy arity-only behavior when
 * instantiations or annotations are missing. */
static void check_struct_method_call(SemanticContext* ctx, ASTMemberCall* mc,
                                     const char* obj_struct_name,
                                     ASTNode* call_node, int line, int column) {
    ASTFnDecl* method = find_method(ctx, obj_struct_name, mc->member_name);
    if (!method) return;  /* caller already reported unknown method */

    /* Receiver instantiation (may be NULL for unannotated vars). */
    Symbol* recv_sym = NULL;
    if (mc->object->type == AST_IDENTIFIER) {
        recv_sym = scope_find(ctx->current_scope, ((ASTIdentifier*)mc->object)->name);
    }
    const char* recv_full = recv_sym && recv_sym->kind == SYMBOL_VAR ? recv_sym->full_type : NULL;

    ASTImplDecl* impl = find_impl_decl(ctx, obj_struct_name);
    int tp_count = impl ? impl->type_param_count : 0;
    if (tp_count == 0 || !recv_full) return;   /* nothing generic to do */

    char head[64];
    const char** rargs = NULL;
    int nargs = ann_split_top_args(recv_full, head, sizeof(head), &rargs);
    if (nargs <= 0) { free(rargs); return; }
    if (head[0] == '\0' || strcmp(head, obj_struct_name) != 0 || nargs != tp_count) {
        free(rargs);
        return;
    }

    AnnSubstMap map;
    map.names = malloc(sizeof(const char*) * (size_t)tp_count);
    map.values = malloc(sizeof(const char*) * (size_t)tp_count);
    map.count = tp_count;
    if (!map.names || !map.values) {
        perror("Failed to allocate method binding map");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < tp_count; i++) {
        map.names[i] = lamo_intern_type(impl->type_params[i]);
        map.values[i] = rargs[i];
    }

    /* Validate arguments against the method's parameter annotations. */
    int limit = mc->arg_count < LAMO_MAX_BIND_ARGS ? mc->arg_count : LAMO_MAX_BIND_ARGS;
    for (int i = 0; i < limit; i++) {
        const char* raw_pat = method->param_types ? method->param_types[i] : NULL;
        if (!raw_pat) continue;
        char* pat_norm = semantic_normalize_type(raw_pat);
        const char* pat_interned = lamo_intern_type(pat_norm);
        free(pat_norm);
        const char* actual = arg_concrete_full_type(ctx, mc->args[i]);
        if (!actual) continue;
        int r = ann_bind_pattern(pat_interned, actual, &map);
        if (r == 0) {
            char message[300];
            snprintf(message, sizeof(message),
                     "argument %d to method '%s.%s': expected type '%s', got '%s'",
                     i + 1, obj_struct_name, mc->member_name, pat_interned, actual);
            semantic_error_at(ctx, line, column, message);
        }
    }

    /* Constraint satisfaction of the receiver instantiation. */
    for (int t = 0; t < tp_count; t++) {
        /* Note: impl-level constraints are not parsed onto the impl today
         * (PR 6 grammar covers fn/struct lists); placeholder keeps the
         * door open without changing behavior. */
        (void)t;
    }

    /* Annotate the substituted return type when annotated. */
    if (method->return_type_annotation && call_node) {
        char* ret_norm = semantic_normalize_type(method->return_type_annotation);
        char* substituted = ann_subst(ret_norm, &map);
        free(ret_norm);
        if (substituted) {
            call_node->sema_full_type = lamo_intern_type(substituted);
            free(substituted);
        }
    }

    free(map.names);
    free(map.values);
    free(rargs);
}

/* Best-effort concrete full type of an already-visited argument node.
 * Returns a normalized interned string, or NULL when unknown. Covers
 * the cases that matter for real call sites: literals, identifiers,
 * array literals of inferable elements and nested calls whose result
 * type was annotated on the node by an earlier inference round. */
static const char* arg_concrete_full_type(SemanticContext* ctx, ASTNode* node) {
    if (!node) return NULL;
    switch (node->type) {
        case AST_INT_LITERAL:    return lamo_intern_type("int");
        case AST_FLOAT_LITERAL:  return lamo_intern_type("float");
        case AST_STRING_LITERAL: return lamo_intern_type("string");
        case AST_BOOL_LITERAL:   return lamo_intern_type("bool");
        case AST_IDENTIFIER: {
            Symbol* s = scope_find(ctx->current_scope,
                                   ((ASTIdentifier*)node)->name);
            if (s && s->kind == SYMBOL_VAR) {
                if (s->full_type) return s->full_type;
                if (s->struct_name) return lamo_intern_type(s->struct_name);
                /* Fall back to the legacy enum inference. */
                switch (s->type) {
                    case LAMO_TYPE_INT:    return lamo_intern_type("int");
                    case LAMO_TYPE_FLOAT:  return lamo_intern_type("float");
                    case LAMO_TYPE_STRING: return lamo_intern_type("string");
                    case LAMO_TYPE_BOOL:   return lamo_intern_type("bool");
                    default:
                        /* 2.8.0 (FU5): enum-typed symbols carry their
                         * concrete full type on the NODE (bare unit
                         * variants stamp the degraded bare enum name). */
                        return node->sema_full_type;
                }
            }
            return NULL;
        }
        case AST_ARRAY_LITERAL: {
            /* PR 3 §5.1: [a,b,c] infers array<T> via least-upper-bound
             * over element concrete types; heterogeneous arrays stay
             * plain "array" (the PR 3 warning handles them at decl). */
            ASTArrayLiteral* arr = (ASTArrayLiteral*)node;
            const char* lub = NULL;
            int conflict = 0;
            for (int i = 0; i < arr->element_count; i++) {
                const char* t = arg_concrete_full_type(ctx, arr->elements[i]);
                if (!t) { lub = NULL; break; }
                char head[32];
                ann_head(t, head, sizeof(head));
                int is_num_t = strcmp(head, "int") == 0 || strcmp(head, "float") == 0;
                if (!lub) { lub = t; continue; }
                if (ann_equal(lub, t)) continue;
                char prev_head[32];
                ann_head(lub, prev_head, sizeof(prev_head));
                int both_num = (strcmp(prev_head,"int")==0 || strcmp(prev_head,"float")==0) && is_num_t;
                if (both_num) { lub = lamo_intern_type("float"); continue; }
                conflict = 1;
                lub = NULL;
                break;
            }
            if (conflict || !lub) return lamo_intern_type("array");
            char buf[160];
            snprintf(buf, sizeof(buf), "array<%s>", lub);
            return lamo_intern_type(buf);
        }
        case AST_UNARY_EXPR: {
            /* 2.7.0 (FU1): `-5` and `!b` carry the operand's concrete
             * type — negative enum payload args (`Some(-5)`) used to
             * lose their full type and break enum-annotation matching. */
            ASTUnaryExpr* un = (ASTUnaryExpr*)node;
            if (un->operator == TOKEN_MINUS) return arg_concrete_full_type(ctx, un->right);
            return node->sema_full_type;
        }
        case AST_GROUPING_EXPR: {
            ASTGroupingExpr* gr = (ASTGroupingExpr*)node;
            return arg_concrete_full_type(ctx, gr->expression);
        }
        case AST_STRUCT_LITERAL: {
            /* 2.9.0: a struct literal argument carries its struct's full
             * type — bare name ("Plain") or instantiation ("Stack<int>")
             * when explicit type args are present. Before this case the
             * literal had no concrete type yet at signature-check time,
             * so §7.7 binding AND constraint enforcement (catalogue and
             * traits alike) silently skipped those call sites. */
            ASTStructLiteral* sl = (ASTStructLiteral*)node;
            if (sl->type_arg_count > 0 && sl->type_args) {
                char buf[224];
                size_t blen = 0;
                int w = snprintf(buf, sizeof(buf), "%s<", sl->struct_name ? sl->struct_name : "");
                if (w < 0 || (size_t)w >= sizeof(buf)) return lamo_intern_type(sl->struct_name ? sl->struct_name : "");
                blen = (size_t)w;
                for (int i = 0; i < sl->type_arg_count && blen < sizeof(buf) - 1; i++) {
                    char* norm = sl->type_args[i] ? semantic_normalize_type(sl->type_args[i]) : NULL;
                    if (i > 0 && blen < sizeof(buf) - 1) buf[blen++] = ',';
                    if (norm) {
                        size_t nlen = strlen(norm);
                        size_t room = sizeof(buf) - 1 - blen;
                        size_t take = nlen < room ? nlen : room;
                        memcpy(buf + blen, norm, take);
                        blen += take;
                        free(norm);
                    }
                }
                if (blen < sizeof(buf) - 1) buf[blen++] = '>';
                buf[blen] = '\0';
                return lamo_intern_type(buf);
            }
            return lamo_intern_type(sl->struct_name ? sl->struct_name : "");
        }
        default:
            return node->sema_full_type;  /* set by earlier rounds */
    }
}

/* Numeric-compat rule carried over from `let` validation: top-level
 * int ↔ float interplay is allowed (widening); everything else must be
 * structurally equal (RFC §5.4: generics are invariant). */
static int call_types_compatible(const char* expected_full, LamoType expected_base,
                                 const char* actual_full, LamoType actual_base) {
    if (expected_base == LAMO_TYPE_UNKNOWN || actual_base == LAMO_TYPE_UNKNOWN) return 1; /* defer */
    if (actual_base == LAMO_TYPE_VOID || expected_base == LAMO_TYPE_VOID) {
        return actual_base == expected_base;
    }
    int numeric_pair = (expected_base == LAMO_TYPE_INT || expected_base == LAMO_TYPE_FLOAT) &&
                       (actual_base == LAMO_TYPE_INT || actual_base == LAMO_TYPE_FLOAT);
    if (numeric_pair) return 1;
    if (expected_full && actual_full) {
        if (ann_equal(expected_full, actual_full)) return 1;
        /* 2.8.0 (FU5): a DEGRADED enum full type (bare "E" — only
         * produced by partially-inferable ctor inference or bare
         * unit-variant values, never by a validated annotation, which
         * always carries its type args) defers against its own enum's
         * annotated form "E<...>". "Annotated enum params bind like
         * generic signatures": unknown type args cannot violate
         * invariance, so the check defers instead of failing. */
        if (strchr(actual_full, '<') == NULL && strchr(expected_full, '<') != NULL) {
            char ah[64];
            ann_head(expected_full, ah, sizeof(ah));
            if (strcmp(ah, actual_full) == 0) return 1;
        }
        return 0;
    }
    /* No strings to compare — bases decide when non-degenerate. */
    if (!actual_full && !expected_full) return expected_base == actual_base ||
                                                    expected_base == LAMO_TYPE_STRUCT ||
                                                    actual_base == LAMO_TYPE_ARRAY;
    return 0;
}

/* Bind pattern leaves against a concrete type. Returns:
 *   1 ok/consistent, 0 conflict, -1 malformed (skip silently).
 *
 * Works by SIMULTANEOUS recursive descent over the (normalized) pattern
 * and concrete annotations:
 *   pattern  array<T>        concrete  array<int>   ⇒ binds T := int
 *   pattern  Map<K,V>        concrete  Map<int,int> ⇒ K:=int, V:=int,
 *                                               conflict if K were string
 *   pattern  T               concrete  anything     ⇒ direct binding
 * Head identifiers must match exactly unless the pattern head IS a type
 * parameter name (RFC §5.4 invariant generics: no implicit widening). */
static int bind_read_ident(const char** p, char* out, size_t out_size) {
    const char* s = *p;
    size_t i = 0;
    while (s[i] && s[i] != '<' && s[i] != '>' && s[i] != ',') i++;
    if (i == 0) return 0;
    size_t n = i < out_size - 1 ? i : out_size - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    *p = s + i;
    return 1;
}

/* Capture a balanced '<...>' subtree starting AT '<'. Advances *p past
 * the closing '>'. Returns malloc'd slice including the angles. */
static char* bind_capture_subtree(const char** p) {
    const char* s = *p;
    if (*s != '<') return NULL;
    int depth = 0;
    const char* start = s;
    while (*s) {
        if (*s == '<') depth++;
        else if (*s == '>') { depth--; if (depth == 0) break; }
        s++;
    }
    if (depth != 0) return NULL;
    size_t len = (size_t)(s - start + 1);
    char* out = malloc(len + 1);
    if (!out) {
        perror("Failed to allocate binding buffer");
        exit(EXIT_FAILURE);
    }
    memcpy(out, start, len);
    out[len] = '\0';
    *p = s + 1;
    return out;
}

static int bind_walk(const char** pp, const char** cp, AnnSubstMap* map);

/* Pairwise-compare two argument lists inside matched '<' contexts.
 * Both cursors sit right after their '<'. */
static int bind_walk_list(const char** pp, const char** cp, AnnSubstMap* map) {
    while (**pp && **pp != '>') {
        if (!bind_walk(pp, cp, map)) return 0;
        if (**pp == ',') {
            if (**cp != ',') return 0;
            (*pp)++; (*cp)++;
            continue;
        }
        break;
    }
    /* Counts must agree: either both hit '>' now or the pattern list has
     * a fixed arity mismatch against concrete. */
    if ((**pp == '>') != (**cp == '>')) return 0;
    return 1;
}

static int bind_walk(const char** pp, const char** cp, AnnSubstMap* map) {
    char pid[128], cid[128];
    if (!bind_read_ident(pp, pid, sizeof(pid))) return 0;
    if (!bind_read_ident(cp, cid, sizeof(cid))) return 0;

    /* Is the pattern head one of our type parameters? */
    const char* bound_value = NULL;
    int idx = -1;
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->names[i], pid) == 0) { idx = i; break; }
    }
    if (idx >= 0) {
        /* The CONCRETE side may itself carry a nested subtree. */
        char* csub = NULL;
        size_t clen = strlen(cid);
        if (**cp == '<') {
            csub = bind_capture_subtree(cp);
            if (!csub) return 0;
            char* joined = malloc(clen + strlen(csub) + 1);
            if (!joined) {
                perror("Failed to allocate binding buffer");
                exit(EXIT_FAILURE);
            }
            strcpy(joined, cid);
            strcat(joined, csub);
            free(csub);
            cid[0] = '\0';
            strncat(cid, joined, sizeof(cid) - 1);
            free(joined);
        }
        bound_value = lamo_intern_type(cid);
        if (!map->values[idx]) {
            map->values[idx] = bound_value;
            return 1;
        }
        return ann_equal(map->values[idx], bound_value) ? 1 : 0;
    }

    /* Plain identifier: heads must match. Then continue structurally. */
    if (strcmp(pid, cid) != 0) return 0;

    int p_angle = (**pp == '<');
    int c_angle = (**cp == '<');
    if (p_angle != c_angle) return 0;
    if (p_angle) {
        (*pp)++; (*cp)++;   /* consume both '<' */
        if (!bind_walk_list(pp, cp, map)) return 0;
        if (**pp != '>' || **cp != '>') return 0;
        (*pp)++; (*cp)++;
    }
    return 1;
}

static int ann_bind_pattern(const char* pattern, const char* concrete, AnnSubstMap* map) {
    if (!pattern || !concrete) return -1;
    const char* pp = pattern;
    const char* cp = concrete;
    int r = bind_walk(&pp, &cp, map);
    /* Full consumption required on both sides. */
    if (r && (*pp || *cp)) return 0;
    return r;
}

/* ── 2.8.0 (FU5): enum annotation type-arg invariance at call sites ─────
 * A constructor call whose enum is only PARTIALLY inferable from its
 * payloads (`enum E<T, U> { V(T) }`) degrades to the BARE enum name as
 * its sema_full_type (the 2.7.0 FU1 behavior). Against an expected
 * annotation `E<int, string>` that bare name loses information: the
 * let-annotation check compares heads only (type args unchecked), and
 * call-site binding fails the angle-list agreement test.
 *
 * This helper completes the degraded type AGAINST the expected enum
 * annotation: type params are re-bound from the ctor payloads (the
 * §7.7 machinery), the remaining params are filled from the
 * annotation's own type arguments, and invariance is enforced — a
 * param bound by a payload must EQUAL the annotation's argument
 * (Rust-style: `E<string, ...> = V(42)` is an error because the
 * payload binds T=int).
 *
 * Returns the completed interned full type (and stamps it on the ctor
 * call node so downstream arg_concrete_full_type sees the concrete
 * type), or NULL when completion does not apply / is impossible.
 * Mismatch errors are reported here. */
static const char* enum_complete_degraded_full_type(SemanticContext* ctx,
                                                    ASTNode* init,
                                                    const char* annotation,
                                                    int line, int column) {
    if (!init || !annotation) return NULL;
    /* Applies only to DEGRADED ctor calls: stamped by the ctor branch,
     * full type present but without type args. */
    if (!init->sema_enum_name || init->sema_variant_index < 0) return NULL;
    if (!init->sema_full_type || strchr(init->sema_full_type, '<') != NULL) return NULL;

    /* The annotation must head the same enum and carry a type-arg list. */
    char ahead[64];
    ann_head(annotation, ahead, sizeof(ahead));
    if (strcmp(ahead, init->sema_enum_name) != 0) return NULL;
    const char** ann_args = NULL;
    char ann_head_buf[64];
    int ann_argc = ann_split_top_args(semantic_normalize_type(annotation),
                                      ann_head_buf, sizeof(ann_head_buf), &ann_args);
    if (ann_argc <= 0) return NULL;  /* no args / malformed: nothing to fill */

    ASTEnumDecl* ed = find_enum_def(ctx, init->sema_enum_name);
    if (!ed || ed->type_param_count == 0 || ed->type_param_count != ann_argc) {
        free(ann_args);
        return NULL;
    }
    int vidx = init->sema_variant_index;
    int pcount = enum_variant_payload_count(ed, vidx);
    if (pcount <= 0 || !ed->variant_payloads || !ed->variant_payloads[vidx]) {
        free(ann_args);
        return NULL;
    }

    /* Re-bind the enum's type params from the ctor payloads — the same
     * loop the ctor branch runs (semantic_visit_call_full FU1 block). */
    ASTCallExpr* ctor = (ASTCallExpr*)init;
    AnnSubstMap emap;
    emap.count = ed->type_param_count;
    emap.names = (const char**)ed->type_params;
    emap.values = calloc((size_t)ed->type_param_count, sizeof(char*));
    if (!emap.values) {
        free(ann_args);
        return NULL;
    }
    for (int i = 0; i < pcount; i++) {
        const char* ann = ed->variant_payloads[vidx][i];
        const char* arg_full = (i < ctor->arg_count)
            ? arg_concrete_full_type(ctx, ctor->args[i]) : NULL;
        if (!ann || !arg_full) continue;
        ann_bind_pattern(ann, arg_full, &emap);
    }

    /* Fill the unbound params from the annotation and enforce
     * invariance against the payload-bound ones. */
    int complete = 1;
    for (int t = 0; t < ed->type_param_count && complete; t++) {
        const char* ann_arg = ann_args[t];  /* interned */
        if (emap.values[t]) {
            if (ann_arg && !ann_equal(emap.values[t], ann_arg)) {
                char message[400];
                snprintf(message, sizeof(message),
                         "enum '%s' type parameter '%s' is bound to '%s' by the constructor payload, "
                         "but annotation '%s' says '%s'",
                         ed->name, ed->type_params[t], emap.values[t],
                         annotation, ann_arg);
                semantic_error_at(ctx, line, column, message);
                complete = 0;
            }
        } else if (ann_arg) {
            emap.values[t] = ann_arg;
        } else {
            /* Param in neither payloads nor annotation: cannot complete
             * (annotation was validated for arg count, so defensive). */
            complete = 0;
        }
    }

    const char* result = NULL;
    if (complete) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s", ed->name);
        size_t blen = strlen(buf);
        for (int t = 0; t < ed->type_param_count && blen < sizeof(buf); t++) {
            int n = snprintf(buf + blen, sizeof(buf) - blen, "%s%s",
                             t == 0 ? "<" : ",", emap.values[t]);
            if (n < 0 || (size_t)n >= sizeof(buf) - blen) { blen = sizeof(buf); break; }
            blen += (size_t)n;
        }
        if (blen < sizeof(buf) - 1) {
            buf[blen++] = '>';
            buf[blen] = '\0';
            result = lamo_intern_type(buf);
            init->sema_full_type = result;  /* upgrade the degraded stamp */
        }
    }
    free((void*)emap.values);
    free(ann_args);
    return result;
}

// Visit a call site: validates arity, parameter/argument types (SPEC
// §7.3 "Function call"), binds generic type parameters with local
// inference + optional explicit `<...>` arguments (RFC §4.5/§5.3),
// enforces PR 6 constraints, and returns the inferred return type. The
// args themselves are visited (and their types inferred) as part of the
// validation.
static LamoType semantic_visit_call_full(SemanticContext* ctx, const char* name,
                                         char** explicit_type_args, int explicit_type_arg_count,
                                         ASTNode** args, int arg_count,
                                         int line, int column,
                                         ASTNode* call_node_for_annotation) {
    Symbol* symbol = scope_find(ctx->current_scope, name);
    int builtin_arity = builtin_function_arity(name);
    LamoType return_type = LAMO_TYPE_UNKNOWN;

    /* Infer every argument's legacy enum type first (order preserved:
     * errors report innermost-first as before). */
    LamoType arg_types[LAMO_MAX_BIND_ARGS];
    const char* arg_full[LAMO_MAX_BIND_ARGS] = {0};
    int checkable = arg_count <= LAMO_MAX_BIND_ARGS;
    for (int i = 0; i < arg_count; i++) {
        arg_types[i & (LAMO_MAX_BIND_ARGS - 1)] = semantic_infer_expression(ctx, args[i]);
        if (checkable) arg_full[i] = arg_concrete_full_type(ctx, args[i]);
    }

    /* ── 2.6.0: enum variant constructor — `Some(42)` (SPEC §3.5) ─────
     * Variant constructors take precedence over function lookup: a
     * payload variant is not a value, only a constructor. Arity is
     * checked against the variant's payload count, and concrete
     * payload annotations get a light type check (numeric widening
     * kept, matching §7.3). The call node is annotated so codegen
     * emits lamo_make_enum instead of a call.
     *
     * 2.7.0 (FU4): the name may be QUALIFIED — `Enum::Variant(args)`.
     * Qualified lookup bypasses the bare "later wins" rule entirely
     * (that is its purpose); the compound name resolves to an exact
     * enum + variant pair or errors. */
    {
        ASTEnumDecl* ved = NULL;
        int vidx = -1;
        char q_enum[128];
        char q_variant[128];
        if (split_qualified_variant_name(name, q_enum, sizeof(q_enum),
                                         q_variant, sizeof(q_variant))) {
            ved = find_variant_qualified(ctx, q_enum, q_variant, &vidx);
            if (!ved) {
                if (find_enum_def(ctx, q_enum)) {
                    char message[512];
                    snprintf(message, sizeof(message),
                             "enum '%s' has no variant '%s'", q_enum, q_variant);
                    semantic_error_at(ctx, line, column, message);
                } else {
                    char message[512];
                    snprintf(message, sizeof(message),
                             "unknown enum '%s' in qualified variant '%s'", q_enum, name);
                    semantic_error_at(ctx, line, column, message);
                }
                return LAMO_TYPE_ENUM;  /* degrade: don't cascade into "unknown function" */
            }
        } else {
            const char* venum = find_enum_variant_any(ctx, name, &vidx);
            if (venum) ved = find_enum_def(ctx, venum);
        }
        if (ved) {
            int pcount = enum_variant_payload_count(ved, vidx);
            if (pcount > 0) {
                const char* display = name;  /* compound or bare, for messages */
                if (arg_count != pcount) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "variant '%s' expects %d payload argument(s), got %d",
                             display, pcount, arg_count);
                    semantic_error_at(ctx, line, column, message);
                } else if (ved->variant_payloads) {
                    /* Light concrete-payload check. Type-parameter
                     * payloads (e.g. `Some(T)`) were erased at runtime
                     * and unchecked before 2.7.0 — they now drive the
                     * enum's concrete full type below (FU1). */
                    for (int i = 0; i < pcount; i++) {
                        const char* ann = ved->variant_payloads[vidx][i];
                        if (!ann || !arg_full[i]) continue;
                        if (ved->type_param_count > 0) {
                            int is_tp = 0;
                            for (int t = 0; t < ved->type_param_count; t++) {
                                if (strcmp(ann, ved->type_params[t]) == 0) { is_tp = 1; break; }
                            }
                            if (is_tp) continue;
                        }
                        if (strchr(ann, '<') != NULL) continue;  /* nested generic: erased */
                        LamoType want = LAMO_TYPE_UNKNOWN;
                        if      (strcmp(ann, "int") == 0)    want = LAMO_TYPE_INT;
                        else if (strcmp(ann, "float") == 0)  want = LAMO_TYPE_FLOAT;
                        else if (strcmp(ann, "string") == 0) want = LAMO_TYPE_STRING;
                        else if (strcmp(ann, "bool") == 0)   want = LAMO_TYPE_BOOL;
                        else continue;  /* struct payload: erased representation */
                        LamoType got = arg_types[i];
                        int numeric_widen = (want == LAMO_TYPE_INT && got == LAMO_TYPE_FLOAT);
                        if (got != LAMO_TYPE_UNKNOWN && got != want && !numeric_widen) {
                            char message[512];
                            snprintf(message, sizeof(message),
                                     "payload %d of variant '%s': expected '%s', got '%s'",
                                     i + 1, display, ann, type_name(got));
                            semantic_error_at(ctx, line, column, message);
                        }
                    }
                }
                if (call_node_for_annotation) {
                    call_node_for_annotation->sema_enum_name = ved->name;
                    call_node_for_annotation->sema_variant_index = vidx;
                    /* 2.7.0 (FU1): compute the enum's concrete full type
                     * by binding the enum's type parameters against the
                     * concrete payload args — `Some(5)` gets full type
                     * "Option<int>". Used by let-annotation matching and
                     * generic call-site binding. Only stamped when EVERY
                     * type parameter is bound from the payloads; a
                     * partially-inferable enum (payloads don't mention
                     * all params) degrades to the bare enum name. */
                    if (ved->type_param_count > 0 && arg_count == pcount && ved->variant_payloads) {
                        AnnSubstMap emap;
                        emap.count = ved->type_param_count;
                        emap.names = (const char**)ved->type_params;
                        emap.values = calloc((size_t)ved->type_param_count, sizeof(char*));
                        if (emap.values) {
                            int bound_all = 1;
                            for (int i = 0; i < pcount && bound_all; i++) {
                                const char* ann = ved->variant_payloads[vidx][i];
                                if (!ann || !arg_full[i]) continue;
                                /* Bind through this payload annotation
                                 * ("T" := "int", "array<T>" := "array<int>"). */
                                ann_bind_pattern(ann, arg_full[i], &emap);
                            }
                            for (int t = 0; t < ved->type_param_count; t++) {
                                if (!emap.values[t]) { bound_all = 0; break; }
                            }
                            if (bound_all) {
                                char buf[160];
                                snprintf(buf, sizeof(buf), "%s", ved->name);
                                size_t blen = strlen(buf);
                                for (int t = 0; t < ved->type_param_count && blen < sizeof(buf); t++) {
                                    int n = snprintf(buf + blen, sizeof(buf) - blen,
                                                     "%s%s", t == 0 ? "<" : ",",
                                                     emap.values[t]);
                                    if (n < 0 || (size_t)n >= sizeof(buf) - blen) { blen = sizeof(buf); break; }
                                    blen += (size_t)n;
                                }
                                if (blen < sizeof(buf) - 1) {
                                    buf[blen++] = '>';
                                    buf[blen] = '\0';
                                    call_node_for_annotation->sema_full_type =
                                        lamo_intern_type(buf);
                                }
                            } else {
                                call_node_for_annotation->sema_full_type =
                                    lamo_intern_type(ved->name);
                            }
                            free((void*)emap.values);
                        }
                    } else if (ved->type_param_count == 0) {
                        /* Non-generic enum ctor: the bare name IS the full type. */
                        call_node_for_annotation->sema_full_type = lamo_intern_type(ved->name);
                    }
                }
                return LAMO_TYPE_ENUM;
            }
        }
    }

    if (symbol && symbol->kind == SYMBOL_FN) {
        if (symbol->arity != arg_count) {
            char message[256];
            snprintf(message, sizeof(message), "function '%s' expects %d argument(s), got %d",
                     name, symbol->arity, arg_count);
            semantic_error_at(ctx, line, column, message);
        }

        /* ── SPEC §7.3 / Generics PR 2 + PR 6 checks ─────────────────── */
        /* 2.10.0: the `symbol->param_full ||` clause lets ZERO-parameter
         * generic fns enter — a trait-constrained parameter still needs
         * its dictionary stamped (or the uninferred-param error) even
         * when the call passes no arguments. Safe: the arg loop below
         * only dereferences param_full when arg_count > 0, which implies
         * param_count > 0 and therefore param_full != NULL. */
        if (checkable && symbol->arity == arg_count &&
            (symbol->param_full || symbol->tp_count > 0)) {
            AnnSubstMap map;
            map.names = symbol->tp_names;
            map.values = malloc(sizeof(const char*) * (size_t)(symbol->tp_count > 0 ? symbol->tp_count : 1));
            map.count = symbol->tp_count;
            for (int i = 0; i < symbol->tp_count; i++) {
                /* Explicit type arguments fill positions first (RFC §4.5). */
                if (explicit_type_args && i < explicit_type_arg_count && explicit_type_args[i]) {
                    char* n = semantic_normalize_type(explicit_type_args[i]);
                    map.values[i] = lamo_intern_type(n);
                    free(n);
                } else {
                    map.values[i] = NULL;
                }
            }

            int generic_fn = symbol->tp_count > 0;

            for (int i = 0; i < arg_count; i++) {
                const char* expected = symbol->param_full[i];
                if (!expected) continue;
                /* 2.8.0 (FU5): a DEGRADED ctor arg (bare enum name from
                 * a partially-inferable enum like `enum E<T, U> { V(T) }`)
                 * completes against the parameter's enum annotation —
                 * unbound params fill from the annotation, payload-bound
                 * params enforce invariance — so §7.7 binding and the
                 * compat check see the full concrete type. */
                if (arg_full[i] && strchr(arg_full[i], '<') == NULL &&
                    args[i] && args[i]->sema_enum_name) {
                    const char* completed = enum_complete_degraded_full_type(
                        ctx, args[i], expected, line, column);
                    if (completed) arg_full[i] = completed;
                }
                LamoType expected_base = annotation_to_type_with_ctx(ctx, expected);

                /* Type-parameter leaves: "T" or "array<T>" etc. */
                int referenced_tp = 0;
                for (int t = 0; t < symbol->tp_count; t++) {
                    if (strstr(expected, symbol->tp_names[t])) { referenced_tp = 1; break; }
                }

                if (generic_fn && referenced_tp && arg_full[i]) {
                    int r = ann_bind_pattern(expected, arg_full[i], &map);
                    if (r == 0) {
                        char message[400];
                        char disp[128];
                        snprintf(disp, sizeof(disp), "%s", expected);
                        AnnSubstMap cur = map;
                        char* shown = ann_subst(expected, &cur);
                        snprintf(message, sizeof(message),
                                 "argument %d to '%s': expected type '%s', got '%s'%s",
                                 i + 1, name,
                                 shown ? shown : disp,
                                 arg_full[i],
                                 shown ? "" : "");
                        if (shown) free(shown);
                        semantic_error_at(ctx, line, column, message);
                    }
                } else if (!referenced_tp || !generic_fn) {
                    /* Concrete parameter annotation → direct compat. */
                    const char* exp_norm = NULL;
                    annotation_resolve_full(ctx, expected, &exp_norm);
                    if (!call_types_compatible(exp_norm, expected_base,
                                               arg_full[i], arg_types[i])) {
                        char message[300];
                        snprintf(message, sizeof(message),
                                 "argument %d to '%s': expected type '%s', got '%s'",
                                 i + 1, name,
                                 exp_norm ? exp_norm : type_name(expected_base),
                                 arg_full[i] ? arg_full[i] : type_name(arg_types[i]));
                        semantic_error_at(ctx, line, column, message);
                    }
                }
                /* Generic-but-unbound here: leave for the map pass below. */
            }

            /* Constraint enforcement (PR 6 §6 + 2.9.0 traits): each BOUND
             * parameter must satisfy its constraint — catalogue kinds via
             * the built-in rules, user-declared traits via impl lookup
             * (lamo_type_satisfies_trait). Still-unbound parameters
             * cannot be checked here. */
            if (generic_fn) {
                for (int t = 0; t < symbol->tp_count; t++) {
                    const char* val = map.values[t];
                    const char* con = symbol->tp_constraints[t];
                    if (!val || !con) continue;
                    int ckind = lamo_constraint_kind(con);
                    if (ckind == LAMO_CON_ANY) continue;   /* unconstrained */
                    if (ckind > 0) {
                        if (!lamo_type_satisfies_constraint(ctx, val, ckind)) {
                            char message[300];
                            snprintf(message, sizeof(message),
                                     "type argument '%s' for parameter '%s' does not satisfy constraint '%s' (%s)",
                                     val, symbol->tp_names[t], lamo_constraint_name(ckind),
                                     ckind == LAMO_CON_NUM ? "requires int or float"
                                     : ckind == LAMO_CON_SHOW ? "requires a printable builtin or declared struct"
                                     : "requires a builtin with the required operations");
                            semantic_error_at(ctx, line, column, message);
                        }
                    } else if (find_trait_def(ctx, con)) {
                        /* 2.10.0: a value that is the ENCLOSING fn's own
                         * type parameter forwards transitively — the
                         * enclosing fn's call sites enforce its identical
                         * constraint (dictionary dispatch). */
                        if (!lamo_type_satisfies_trait(ctx, val, con) &&
                            !enclosing_tp_satisfies_trait(ctx, val, con)) {
                            char message[400];
                            char hint[260];
                            char val_head[64];
                            ann_head(val, val_head, sizeof(val_head));
                            snprintf(message, sizeof(message),
                                     "type argument '%s' for parameter '%s' does not satisfy trait '%s'",
                                     val, symbol->tp_names[t], con);
                            if (is_builtin_type_head(val_head) || strcmp(val_head, "array") == 0) {
                                snprintf(hint, sizeof(hint),
                                         "user traits are implemented by structs only — builtins satisfy just the built-in catalogue (Any, Eq, Ord, Num, Hash, Show)");
                            } else {
                                snprintf(hint, sizeof(hint),
                                         "write `impl %s for %s { ... }` (a trait is satisfied by a struct with a trait impl)",
                                         con, val);
                            }
                            semantic_error_at_hint(ctx, line, column, message, hint);
                        }
                    }
                    /* else: unknown constraint name — already reported at
                     * the declaration site; don't double-report. */
                }
                /* 2.10.0 trait dictionary dispatch: stamp the hidden
                 * dictionary arguments (one per trait-constrained type
                 * parameter) so codegen appends them at this call site. */
                stamp_trait_dicts(ctx, call_node_for_annotation, name,
                                  symbol->tp_names, symbol->tp_count,
                                  symbol->tp_constraints, map.values);
            }

            /* Return-type computation with substitution. */
            if (symbol->ret_full) {
                char* substituted = ann_subst(symbol->ret_full, &map);
                if (substituted) {
                    const char* stored = lamo_intern_type(substituted);
                    free(substituted);
                    if (call_node_for_annotation) {
                        call_node_for_annotation->sema_full_type = stored;
                    }
                    char head[64];
                    ann_head(stored, head, sizeof(head));
                    /* Legacy consumers read the enum too. */
                    if      (strcmp(head, "int") == 0)    return_type = LAMO_TYPE_INT;
                    else if (strcmp(head, "float") == 0)  return_type = LAMO_TYPE_FLOAT;
                    else if (strcmp(head, "string") == 0) return_type = LAMO_TYPE_STRING;
                    else if (strcmp(head, "bool") == 0)   return_type = LAMO_TYPE_BOOL;
                    else if (strcmp(head, "array") == 0)  return_type = LAMO_TYPE_ARRAY;
                    else if (find_struct_def(ctx, head))  return_type = LAMO_TYPE_STRUCT;
                    else if (strcmp(head, "void") == 0)   return_type = LAMO_TYPE_VOID;
                    else                                  return_type = symbol->type;
                } else {
                    return_type = symbol->type;
                }
            } else {
                return_type = symbol->type;
            }

            free(map.values);
        } else {
            return_type = symbol->type;
        }
    } else if (builtin_arity >= 0) {
        if (builtin_arity != arg_count) {
            char message[256];
            snprintf(message, sizeof(message), "builtin '%s' expects %d argument(s), got %d",
                     name, builtin_arity, arg_count);
            semantic_error_at(ctx, line, column, message);
        } else {
            semantic_validate_builtin_call(ctx, name, args, arg_count, line, column);
        }
        // Compute return type from builtin signature (with arg-dependent types where relevant).
        return_type = builtin_function_return_type(name, args, arg_count);

        /* Backend-alignment item "make print() use semantic type
         * information": annotate the printed expression with its known
         * full type so codegen can specialize output (named struct
         * printing — see generate_lang_builtin_call_expr). */
        if (strcmp(name, "print") == 0 && arg_count == 1 && args[0]) {
            const char* ft = arg_concrete_full_type(ctx, args[0]);
            if (ft) {
                args[0]->sema_full_type = ft;
                char head[64];
                ann_head(ft, head, sizeof(head));
                if (find_struct_def(ctx, head)) {
                    args[0]->sema_struct_name = lamo_intern_type(head);
                }
            }
        }
    } else {
        char message[256];
        char hint[256];
        snprintf(message, sizeof(message), "call to undeclared function '%s'", name);
        snprintf(hint, sizeof(hint),
                 "did you forget to define '%s' (with `fn %s(...) { ... }`) or import it (with `import \"...\" as ...;`)?",
                 name, name);
        semantic_error_at_hint(ctx, line, column, message, hint);
    }

    return return_type;
}

/* Legacy wrapper removed in Generics PR 2: all call paths now go through
 * semantic_visit_call_full (statements/expressions pass their optional
 * explicit type arguments; NULL when absent). */

// Sprint 2 refactor: the per-builtin arity and return-type logic now lives
// in src/builtins.h as a single shared table. The two wrappers below are
// thin adapters that preserve the old call-site names while delegating to
// the table. Adding a new builtin only requires editing builtins.h (plus
// the codegen site that emits the call).
static int builtin_function_arity(const char* name) {
    return lamo_builtin_arity(name);
}

// Return type for each builtin. Reads the BuiltinRetPolicy from the table;
// for BUILTIN_RET_MIRROR_ARG0 (abs), inspects args[0] to guess int vs float.
static LamoType builtin_function_return_type(const char* name, ASTNode** args, int arg_count) {
    const BuiltinInfo* info = lamo_builtin_lookup(name);
    if (!info) {
        return LAMO_TYPE_UNKNOWN;
    }
    switch (info->ret_policy) {
        case BUILTIN_RET_INT:
            return LAMO_TYPE_INT;
        case BUILTIN_RET_STRING:
            return LAMO_TYPE_STRING;
        case BUILTIN_RET_BOOL:
            return LAMO_TYPE_BOOL;
        case BUILTIN_RET_MIRROR_ARG0:
            /* abs(): if the argument is a float literal, the result is float;
             * otherwise we conservatively report int. Real type inference of
             * the argument expression happens via semantic_infer_expression
             * when the args are visited, so by the time we get here, simple
             * cases are already represented in the AST node types. */
            if (arg_count >= 1 && args[0] && args[0]->type == AST_FLOAT_LITERAL) {
                return LAMO_TYPE_FLOAT;
            }
            return LAMO_TYPE_INT;
    }
    return LAMO_TYPE_UNKNOWN;
}

static int semantic_validate_builtin_call(SemanticContext* ctx, const char* name, ASTNode** args, int arg_count, int line, int column) {
    (void)arg_count;

    if (strcmp(name, "gui_open") == 0 && args[2]->type != AST_STRING_LITERAL) {
        semantic_error_at(ctx, line, column, "gui_open(width, height, title) requires a string literal title");
        return 0;
    }

    if (strcmp(name, "gui_draw_text") == 0 && args[0]->type != AST_STRING_LITERAL) {
        semantic_error_at(ctx, line, column, "gui_draw_text(text, x, y, r, g, b) requires a string literal text");
        return 0;
    }

    return 1;
}

// Reports a type-mismatch error if `actual` is incompatible with `expected`,
// accounting for LAMO_TYPE_UNKNOWN (no error) and numeric compatibility
// (int and float are interchangeable in numeric contexts).
static void semantic_check_numeric_operand(SemanticContext* ctx, const char* op_name,
                                            LamoType left, LamoType right,
                                            int line, int column) {
    if (left == LAMO_TYPE_UNKNOWN || right == LAMO_TYPE_UNKNOWN) {
        return; // don't cascade errors when a previous expression failed to infer
    }
    if (left == LAMO_TYPE_STRING) {
        char message[256];
        snprintf(message, sizeof(message),
                 "operator '%s' does not support string operand (got %s, %s)",
                 op_name, type_name(left), type_name(right));
        semantic_error_at(ctx, line, column, message);
        return;
    }
    if (right == LAMO_TYPE_STRING) {
        char message[256];
        snprintf(message, sizeof(message),
                 "operator '%s' does not support string operand (got %s, %s)",
                 op_name, type_name(left), type_name(right));
        semantic_error_at(ctx, line, column, message);
        return;
    }
}

/* Sprint 3: map a type-annotation string ("int", "float", "string", "bool")
 * to the internal LamoType enum. Returns LAMO_TYPE_UNKNOWN for unknown
 * names so the caller can emit a single clear error.
 *
 * Phase 2: also recognizes user-defined struct names. Since structs are
 * registered during the first pre-pass, we can resolve struct-name
 * annotations here. We pass the SemanticContext so we can look up the
 * struct registry. */
static LamoType annotation_to_type_with_ctx(SemanticContext* ctx, const char* annotation) {
    if (!annotation) return LAMO_TYPE_UNKNOWN;
    if (strcmp(annotation, "int") == 0) return LAMO_TYPE_INT;
    if (strcmp(annotation, "float") == 0) return LAMO_TYPE_FLOAT;
    if (strcmp(annotation, "string") == 0) return LAMO_TYPE_STRING;
    if (strcmp(annotation, "bool") == 0) return LAMO_TYPE_BOOL;
    if (strcmp(annotation, "array") == 0 || strcmp(annotation, "Array") == 0) return LAMO_TYPE_ARRAY;
    if (strcmp(annotation, "void") == 0 || strcmp(annotation, "Void") == 0) return LAMO_TYPE_VOID;
    /* Phase 2: struct-name annotation. Generics PR 2/3: generic
     * instantiations like "Pair<int, string>" resolve via their head.
     * 2.7.0 (FU1): enum-name annotations resolve the same way. */
    {
        char head[64];
        ann_head(annotation, head, sizeof(head));
        if (strchr(annotation, '<') != NULL) {
            if (find_struct_def(ctx, head)) return LAMO_TYPE_STRUCT;
            if (ctx && find_enum_def(ctx, head)) return LAMO_TYPE_ENUM;
            return LAMO_TYPE_UNKNOWN;
        }
        if (ctx && find_struct_def(ctx, annotation)) return LAMO_TYPE_STRUCT;
        if (ctx && find_enum_def(ctx, annotation)) return LAMO_TYPE_ENUM;
    }
    return LAMO_TYPE_UNKNOWN;
}

/* Generics PR 2/3: full-resolution variant. Maps a RAW parser annotation
 * to (base kind, normalized interned string). Returns UNKNOWN for
 * garbage; *out_norm receives the canonical form (borrowed, never
 * freed) even when the head is unknown so error paths can quote it. */
static LamoType annotation_resolve_full(SemanticContext* ctx, const char* annotation, const char** out_norm) {
    if (out_norm) *out_norm = NULL;
    if (!annotation) return LAMO_TYPE_UNKNOWN;
    char* norm = semantic_normalize_type(annotation);
    char head[64];
    ann_head(norm, head, sizeof(head));
    const char* stored = lamo_intern_type(norm);
    free(norm);
    if (out_norm) *out_norm = stored;

    if (strcmp(head, "int") == 0) return LAMO_TYPE_INT;
    if (strcmp(head, "float") == 0) return LAMO_TYPE_FLOAT;
    if (strcmp(head, "string") == 0) return LAMO_TYPE_STRING;
    if (strcmp(head, "bool") == 0) return LAMO_TYPE_BOOL;
    if (strcmp(head, "array") == 0) return LAMO_TYPE_ARRAY;
    if (strcmp(head, "void") == 0) return LAMO_TYPE_VOID;
    if (ctx && find_struct_def(ctx, head)) return LAMO_TYPE_STRUCT;
    /* 2.7.0 (FU1): enum annotations — `let o: Option<int> = Some(5);`,
     * enum-typed parameters and returns (SPEC §3.5). Only the head is
     * inspected here (matching the struct behavior); type-argument
     * counts and leaf validity are checked by
     * validate_enum_annotation() at the sites that report errors. */
    if (ctx && find_enum_def(ctx, head)) return LAMO_TYPE_ENUM;
    /* A declared TYPE PARAMETER of an enclosing generic fn is legal in
     * signature position; callers bind it at call sites. Report ARRAY? No
     * — report UNKNOWN-with-string and let binding logic decide. The
     * distinction matters so we surface real typos as errors there. */
    return LAMO_TYPE_UNKNOWN;
}

/* 2.7.0 (FU1): validate an ENUM-typed annotation — `Option<int>`.
 * Checks that the enum exists (caller guarantees it), that the type-
 * argument count matches the declaration for generic enums (bare
 * `Option` for a generic enum is an error; args on a non-generic enum
 * are an error), and that every argument resolves leaf-wise. Emits a
 * semantic error and returns 0 on failure, 1 when OK. */
static int validate_enum_annotation(SemanticContext* ctx, const char* annotation,
                                    int line, int column) {
    char* norm = semantic_normalize_type(annotation);
    char head[64];
    ann_head(norm, head, sizeof(head));
    ASTEnumDecl* ed = find_enum_def(ctx, head);
    if (!ed) { free(norm); return 1; }  /* not an enum head: other checks handle it */

    const char** args = NULL;
    int arg_count = ann_split_top_args(norm, head, sizeof(head), &args);
    free(norm);

    if (ed->type_param_count == 0) {
        if (arg_count >= 0) {
            char message[256];
            snprintf(message, sizeof(message),
                     "enum '%s' is not generic; remove the type arguments from annotation '%s'",
                     ed->name, annotation);
            semantic_error_at(ctx, line, column, message);
            if (args) free((void*)args);
            return 0;
        }
        return 1;
    }

    /* Generic enum: the annotation must carry exactly type_param_count
     * arguments (RFC §5.4-style invariance, mirroring struct literals). */
    if (arg_count < 0 || arg_count != ed->type_param_count) {
        char message[256];
        snprintf(message, sizeof(message),
                 "enum '%s' expects %d type argument(s) in annotation '%s', got %d",
                 ed->name, ed->type_param_count, annotation,
                 arg_count < 0 ? 0 : arg_count);
        semantic_error_at(ctx, line, column, message);
        if (args) free((void*)args);
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < arg_count; i++) {
        if (!lamo_validate_annotation_tree(ctx, NULL, args[i])) {
            char message[256];
            snprintf(message, sizeof(message),
                     "enum annotation '%s' has unknown type argument %d ('%s')",
                     annotation, i + 1, args[i]);
            semantic_error_at(ctx, line, column, message);
            ok = 0;
        }
    }
    if (args) free((void*)args);
    return ok;
}

/* Legacy wrapper that doesn't take a context — kept for compatibility with
 * any callers that don't have a SemanticContext. Loses struct-name resolution.
 * Currently unused (the with_ctx variant is the one actually called), but
 * kept as a public-ish API for future use. The __attribute__((unused)) silences
 * -Wunused-function without removing the function. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static LamoType annotation_to_type(const char* annotation) {
    return annotation_to_type_with_ctx(NULL, annotation);
}

/* Infer the return type of a function body by scanning all direct
 * AST_RETURN_STMT nodes (without descending into nested function
 * declarations). Used when there is no return-type annotation to give
 * the call site a more useful type than UNKNOWN.
 *
 * Algorithm: collect every return-expression type; if all agree on a
 * single type, that becomes the inferred type. If they conflict, or if
 * there are no return statements, returns LAMO_TYPE_UNKNOWN.
 *
 * Note: this is a shallow scan — it does NOT call the full
 * semantic_infer_expression (which would require a valid context with
 * the function's parameter scope). Instead it uses a lightweight
 * literal-only inference that handles the common cases (returning a
 * literal, a variable declared as a known type, or a builtin call).
 * For complex expressions the result is UNKNOWN, which is safe. */
static LamoType infer_fn_return_type_from_body(ASTNode* node, int depth) {
    if (!node || depth > 32) return LAMO_TYPE_UNKNOWN;

    if (node->type == AST_RETURN_STMT) {
        ASTReturnStmt* ret = (ASTReturnStmt*)node;
        if (!ret->expression) return LAMO_TYPE_UNKNOWN; /* bare return */
        switch (ret->expression->type) {
            case AST_INT_LITERAL:    return LAMO_TYPE_INT;
            case AST_FLOAT_LITERAL:  return LAMO_TYPE_FLOAT;
            case AST_STRING_LITERAL: return LAMO_TYPE_STRING;
            case AST_BOOL_LITERAL:   return LAMO_TYPE_BOOL;
            default:                 return LAMO_TYPE_UNKNOWN;
        }
    }

    /* Don't descend into nested fn declarations. */
    if (node->type == AST_FN_DECL) return LAMO_TYPE_UNKNOWN;

    LamoType found = LAMO_TYPE_UNKNOWN;

    /* Walk children based on node type. */
    ASTNode* children[8];
    int n = 0;
    switch (node->type) {
        case AST_BLOCK: {
            ASTNode* s = ((ASTBlock*)node)->statements;
            while (s && n < 8) { children[n++] = s; s = s->next; }
            break;
        }
        case AST_IF_STMT: {
            ASTIfStmt* is = (ASTIfStmt*)node;
            children[n++] = is->then_branch;
            if (is->else_branch) children[n++] = is->else_branch;
            break;
        }
        case AST_WHILE_STMT:
            children[n++] = ((ASTWhileStmt*)node)->body; break;
        case AST_FOR_STMT:
            children[n++] = ((ASTForStmt*)node)->body; break;
        default: break;
    }

    for (int i = 0; i < n; i++) {
        LamoType t = infer_fn_return_type_from_body(children[i], depth + 1);
        if (t == LAMO_TYPE_UNKNOWN) continue;
        if (found == LAMO_TYPE_UNKNOWN) { found = t; continue; }
        if (found != t) return LAMO_TYPE_UNKNOWN; /* conflict */
    }

    /* Also walk the ->next siblings at this level (statement lists). */
    if (node->next) {
        LamoType t = infer_fn_return_type_from_body(node->next, depth);
        if (t != LAMO_TYPE_UNKNOWN) {
            if (found == LAMO_TYPE_UNKNOWN) found = t;
            else if (found != t) return LAMO_TYPE_UNKNOWN;
        }
    }

    return found;
}

/* ── 2.7.0 (FU2/FU4): match pattern helpers ────────────────────────── */

#define LAMO_MAX_PATTERN_BINDINGS 64

/* Collect the BINDING leaves of a pattern tree in extraction order
 * (depth-first, left-to-right — the same order codegen pulls payloads).
 * Silently stops at `cap` bindings; the semantic arm-walk reports
 * nothing here because codegen mirrors the same cap. */
static void lamo_pattern_collect_bindings(LamoPattern* pat, LamoPattern** out,
                                          int cap, int* out_count) {
    if (!pat) return;
    if (pat->kind == LAMO_PATTERN_BINDING) {
        if (*out_count < cap) out[(*out_count)++] = pat;
        return;
    }
    if (pat->kind == LAMO_PATTERN_CTOR) {
        for (int i = 0; i < pat->child_count; i++) {
            lamo_pattern_collect_bindings(pat->children[i], out, cap, out_count);
        }
    }
}

/* Resolve a NESTED constructor pattern (`Pair(a, b)` inside
 * `Some(Pair(a, b))`): find its enum + variant (bare names use the
 * later-wins lookup; `Enum::Variant` resolves exactly), stamp the
 * sema fields codegen needs, validate payload arity, and recurse.
 * Emits semantic errors; returns the resolved ASTEnumDecl or NULL. */
static ASTEnumDecl* lamo_sema_resolve_nested_pattern(SemanticContext* ctx, LamoPattern* pat) {
    if (!pat || pat->kind != LAMO_PATTERN_CTOR) return NULL;
    ASTEnumDecl* ed = NULL;
    int vidx = -1;
    char q_enum[128];
    char q_variant[128];
    if (split_qualified_variant_name(pat->name, q_enum, sizeof(q_enum),
                                     q_variant, sizeof(q_variant))) {
        ed = find_variant_qualified(ctx, q_enum, q_variant, &vidx);
        if (!ed) {
            char message[512];
            if (find_enum_def(ctx, q_enum)) {
                snprintf(message, sizeof(message),
                         "enum '%s' has no variant '%s' (in nested pattern '%s')",
                         q_enum, q_variant, pat->name);
            } else {
                snprintf(message, sizeof(message),
                         "unknown enum '%s' in nested pattern '%s'", q_enum, pat->name);
            }
            semantic_error_at(ctx, pat->line, pat->column, message);
            return NULL;
        }
    } else {
        const char* ename = find_enum_variant_any(ctx, pat->name, &vidx);
        if (!ename) {
            char message[256];
            snprintf(message, sizeof(message),
                     "nested pattern '%s' is not a known enum variant",
                     pat->name);
            semantic_error_at(ctx, pat->line, pat->column, message);
            return NULL;
        }
        ed = find_enum_def(ctx, ename);
        if (!ed) return NULL;
    }
    pat->sema_enum_name = ed->name;
    pat->sema_variant_index = vidx;
    int pcount = enum_variant_payload_count(ed, vidx);
    if (pcount != pat->child_count) {
        char message[256];
        snprintf(message, sizeof(message),
                 "nested variant '%s' carries %d payload(s), but the pattern binds %d",
                 pat->name, pcount, pat->child_count);
        semantic_error_at(ctx, pat->line, pat->column, message);
        return ed;  /* stamped anyway so codegen stays consistent */
    }
    for (int c = 0; c < pat->child_count; c++) {
        LamoPattern* child = pat->children[c];
        if (child && child->kind == LAMO_PATTERN_CTOR) {
            lamo_sema_resolve_nested_pattern(ctx, child);
        }
    }
    return ed;
}

static void semantic_visit_statement(SemanticContext* ctx, ASTNode* node) {
    if (!node) {
        return;
    }

    // Bug #5 fix: lembra de qual arquivo veio este nó para que
    // semantic_error_at() possa reportar a origem correta.
    if (node->file_path) {
        ctx->last_node_path = node->file_path;
    }

    switch (node->type) {
        case AST_VAR_DECL: {
            ASTVarDecl* var_decl = (ASTVarDecl*)node;
            LamoType init_type = semantic_infer_expression(ctx, var_decl->initializer);
            const char* inferred_struct_name = NULL;
            /* If the initializer is a struct literal, infer the struct name
             * from the literal itself. This lets `let p = Player {...};`
             * work without requiring a `: Player` annotation. */
            if (var_decl->initializer && var_decl->initializer->type == AST_STRUCT_LITERAL) {
                ASTStructLiteral* sl = (ASTStructLiteral*)var_decl->initializer;
                if (find_struct_def(ctx, sl->struct_name)) {
                    inferred_struct_name = sl->struct_name;
                    init_type = LAMO_TYPE_STRUCT;
                }
            }
            /* If the initializer is an array literal, infer LAMO_TYPE_ARRAY. */
            if (var_decl->initializer && var_decl->initializer->type == AST_ARRAY_LITERAL) {
                init_type = LAMO_TYPE_ARRAY;
            }
            /* 2.6.0 FU2 (module-boundary type flow): calls whose resolved
             * return type names a declared struct — module member calls
             * (`let p = lib.make_point(1, 2)`) and plain calls — carry
             * the substituted type on the initializer's sema_full_type.
             * Propagate the struct so the variable is struct-typed and
             * field access / method calls work in the importing file
             * (SPEC §5.5, §10.2). Covers both an UNKNOWN-typed call that
             * is upgraded to struct and a call that already returned
             * LAMO_TYPE_STRUCT without carrying a name (member calls). */
            if (!inferred_struct_name && var_decl->initializer &&
                (var_decl->initializer->sema_full_type ||
                 var_decl->initializer->sema_struct_name)) {
                char head[64];
                if (var_decl->initializer->sema_struct_name) {
                    snprintf(head, sizeof(head), "%s", var_decl->initializer->sema_struct_name);
                } else {
                    ann_head(var_decl->initializer->sema_full_type, head, sizeof(head));
                }
                if (find_struct_def(ctx, head)) {
                    if (init_type == LAMO_TYPE_UNKNOWN) init_type = LAMO_TYPE_STRUCT;
                    if (init_type == LAMO_TYPE_STRUCT) {
                        inferred_struct_name = lamo_intern_type(head);
                    }
                }
            }
            /* 2.10.0 (FU-vmc): alias flow — `let q = p` inherits the
             * source variable's struct identity so field access and
             * method calls (inherent AND trait impls) resolve through
             * the alias. Before this, aliasing a struct value silently
             * produced a nameless-struct variable and every subsequent
             * member call failed with "cannot call method ... on value
             * of type 'struct'" — on BOTH backends (same semantic pass). */
            if (!inferred_struct_name && var_decl->initializer &&
                var_decl->initializer->type == AST_IDENTIFIER) {
                Symbol* src = scope_find(ctx->current_scope,
                                         ((ASTIdentifier*)var_decl->initializer)->name);
                if (src && src->kind == SYMBOL_VAR) {
                    const char* src_head = NULL;
                    if (src->struct_name) {
                        src_head = src->struct_name;
                    } else if (src->full_type) {
                        char ah[64];
                        ann_head(src->full_type, ah, sizeof(ah));
                        if (find_struct_def(ctx, ah)) src_head = lamo_intern_type(ah);
                    }
                    if (src_head && find_struct_def(ctx, src_head)) {
                        if (init_type == LAMO_TYPE_UNKNOWN) init_type = LAMO_TYPE_STRUCT;
                        if (init_type == LAMO_TYPE_STRUCT) {
                            inferred_struct_name = src_head;
                        }
                    }
                }
            }
            /* Sprint 3: validate type annotation if present. The check is
             * strict: int != float (annotated int with float initializer
             * is an error), and string/bool are entirely separate. The
             * one relaxation: UNKNOWN initializer type (e.g. from a
             * previous error) is accepted to avoid cascading errors. */
            const char* annotated_struct_name = NULL;
            if (var_decl->type_annotation) {
                /* Generics PR 2/3: full resolver so nested annotations
                 * ("array<int>", "Pair<int,string>") pass the legacy
                 * strict check instead of being rejected as unknown. */
                LamoType annotated = annotation_resolve_full(ctx, var_decl->type_annotation, NULL);
                if (annotated == LAMO_TYPE_UNKNOWN) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "unknown type annotation '%s' (expected int, float, string, bool, array<T>, void, a struct name, or an enum name like Option<int>)",
                             var_decl->type_annotation);
                    semantic_error_at(ctx, node->line, node->column, message);
                } else if (annotated == LAMO_TYPE_STRUCT) {
                    /* The annotation is a struct name; remember it so we
                     * can define the variable with the struct type. */
                    annotated_struct_name = var_decl->type_annotation;
                    /* If the initializer is also a struct literal, validate
                     * the struct names match. Generics PR 2: annotated
                     * instantiations (`Pair<int,string>`) match literals of
                     * the same BASE struct (`Pair`). */
                    if (inferred_struct_name &&
                        strcmp(inferred_struct_name, annotated_struct_name) != 0) {
                        char ahead[64];
                        ann_head(annotated_struct_name, ahead, sizeof(ahead));
                        if (strcmp(inferred_struct_name, ahead) != 0) {
                            char message[256];
                            snprintf(message, sizeof(message),
                                     "type annotation '%s' does not match struct literal '%s'",
                                     annotated_struct_name, inferred_struct_name);
                            semantic_error_at(ctx, node->line, node->column, message);
                        }
                    }
                    init_type = LAMO_TYPE_STRUCT;
                } else if (annotated == LAMO_TYPE_ENUM) {
                    /* 2.7.0 (FU1): enum-typed annotation — `let o:
                     * Option<int> = Some(5);`. Validates the type-argument
                     * count/leaves, then matches the annotation's enum
                     * head against the initializer's enum (constructor
                     * calls and qualified references carry their enum in
                     * sema_enum_name; identifiers fall back to the head
                     * of their concrete full type). */
                    validate_enum_annotation(ctx, var_decl->type_annotation,
                                             node->line, node->column);
                    const char* init_enum_name = NULL;
                    if (var_decl->initializer &&
                        var_decl->initializer->sema_enum_name) {
                        init_enum_name = var_decl->initializer->sema_enum_name;
                    } else if (init_type == LAMO_TYPE_ENUM) {
                        const char* ft = arg_concrete_full_type(ctx, var_decl->initializer);
                        if (ft) {
                            char fhead[64];
                            ann_head(ft, fhead, sizeof(fhead));
                            if (find_enum_def(ctx, fhead)) init_enum_name = lamo_intern_type(fhead);
                        }
                    }
                    if (init_enum_name) {
                        char ahead[64];
                        ann_head(var_decl->type_annotation, ahead, sizeof(ahead));
                        if (strcmp(init_enum_name, ahead) != 0) {
                            char message[256];
                            snprintf(message, sizeof(message),
                                     "type annotation '%s' does not match enum '%s' value",
                                     var_decl->type_annotation, init_enum_name);
                            semantic_error_at(ctx, node->line, node->column, message);
                        }
                    }
                    /* 2.8.0 (FU5): a degraded ctor initializer completes
                     * against the annotation — unbound type params fill
                     * from the annotation's arguments and invariance is
                     * enforced (payload-bound params must equal the
                     * annotation's). `let e: E<int, string> = V(42);`
                     * yields a fully concrete E<int, string> instead of
                     * leaving the type args unchecked. */
                    if (var_decl->initializer &&
                        var_decl->initializer->sema_enum_name) {
                        enum_complete_degraded_full_type(
                            ctx, var_decl->initializer, var_decl->type_annotation,
                            node->line, node->column);
                    }
                    if (init_type != LAMO_TYPE_UNKNOWN && init_type != LAMO_TYPE_ENUM) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "type annotation '%s' does not match inferred type '%s'",
                                 var_decl->type_annotation, type_name(init_type));
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                    init_type = LAMO_TYPE_ENUM;
                } else if (init_type != LAMO_TYPE_UNKNOWN && init_type != annotated) {
                    /* Allow int initializer for float annotation (numeric
                     * widening) and float initializer for int annotation
                     * (will be truncated at runtime, but is a common
                     * pattern). The strict-check version would reject
                     * both; we err on the side of permissiveness here. */
                    int numeric_compat = (annotated == LAMO_TYPE_INT && init_type == LAMO_TYPE_FLOAT) ||
                                         (annotated == LAMO_TYPE_FLOAT && init_type == LAMO_TYPE_INT);
                    if (!numeric_compat) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "type annotation '%s' does not match inferred type '%s'",
                                 var_decl->type_annotation, type_name(init_type));
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                    init_type = annotated;
                } else {
                    init_type = annotated;
                }
            }
            /* Generics PR 2/3: resolve the annotation through the FULL
             * resolver so nested generics work, remember the normalized
             * type on the symbol (consumed by call-site binding and the
             * Array<T> element checks), and warn on BARE `array` per RFC
             * §9 ("the old array keyword remains as an alias for
             * Array<Any> for backwards compatibility"). */
            const char* var_full_norm = NULL;
            if (var_decl->type_annotation) {
                LamoType ann_kind = annotation_resolve_full(ctx, var_decl->type_annotation,
                                                            &var_full_norm);
                if (ann_kind != LAMO_TYPE_UNKNOWN && var_full_norm &&
                    strcmp(var_full_norm, "array") == 0) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "bare 'array' annotation is deprecated; prefer 'array<T>' (e.g. array<int>) - bare means array<any>");
                    semantic_warn_at(ctx, node->line, node->column, message);
                }
            }

            /* Phase 2: if the variable has a struct type (either from
             * annotation or inferred from a struct literal), define it
             * with the struct name so field access can be validated. */
            if (init_type == LAMO_TYPE_STRUCT) {
                const char* sn = annotated_struct_name ? annotated_struct_name : inferred_struct_name;
                if (sn) {
                    scope_define_struct_var(ctx, ctx->current_scope, var_decl->name, sn, node->line, node->column, node->file_path);
                } else {
                    scope_define(ctx, ctx->current_scope, var_decl->name, SYMBOL_VAR, 0, init_type, node->line, node->column, node->file_path);
                }
            } else {
                scope_define(ctx, ctx->current_scope, var_decl->name, SYMBOL_VAR, 0, init_type, node->line, node->column, node->file_path);
            }
            /* Attach normalized full type + PR3 literal inference. */
            {
                Symbol* sym = scope_find_in_current(ctx->current_scope, var_decl->name);
                if (sym) {
                    if (var_full_norm) {
                        sym->full_type = var_full_norm;
                        /* Generic instantiations keep their concrete head
                         * as struct_name so method dispatch still finds
                         * impls declared under the bare name. */
                        if (init_type == LAMO_TYPE_STRUCT) {
                            char head[64];
                            ann_head(var_full_norm, head, sizeof(head));
                            sym->struct_name = lamo_intern_type(head);
                            node->sema_struct_name = sym->struct_name;
                        }
                    } else if (!var_decl->type_annotation) {
                        /* No annotation: infer from literals. Array
                         * literals get element-typed via least upper
                         * bound (PR 3 §5.1); heterogeneous arrays fall
                         * back to plain "array" WITHOUT a warning here —
                         * the migration warning only fires on explicit
                         * annotations to avoid noise in existing code. */
                        const char* inferred = arg_concrete_full_type(ctx, var_decl->initializer);
                        if (inferred && strcmp(inferred, "array") != 0) {
                            sym->full_type = inferred;
                            node->sema_full_type = inferred;
                        }
                    }
                }
            }
            break;
        }
        case AST_FN_DECL: {
            ASTFnDecl* fn_decl = (ASTFnDecl*)node;
            Scope* parent = ctx->current_scope;
            int previous_inside_function = ctx->inside_function;
            LamoType previous_fn_return_type = ctx->current_fn_return_type;
            const char* previous_fn_name = ctx->current_fn_name;

            /* Expose this fn's own type parameters to literal/annotation
             * validation while its body is walked. 2.10.0: constraints
             * ride along for trait-dictionary dispatch. */
            const char* const* saved_fn_tps = ctx->cur_fn_tp_names;
            int saved_fn_tp_count = ctx->cur_fn_tp_count;
            const char* const* saved_fn_tp_cons = ctx->cur_fn_tp_constraints;
            const char** own_tp_interned = NULL;
            if (fn_decl->type_param_count > 0) {
                own_tp_interned = malloc(sizeof(const char*) * (size_t)fn_decl->type_param_count);
                if (!own_tp_interned) {
                    perror("Failed to allocate fn tp name array");
                    exit(EXIT_FAILURE);
                }
                for (int i2 = 0; i2 < fn_decl->type_param_count; i2++) {
                    own_tp_interned[i2] = lamo_intern_type(fn_decl->type_params[i2]);
                }
                ctx->cur_fn_tp_names = own_tp_interned;
                ctx->cur_fn_tp_count = fn_decl->type_param_count;
                ctx->cur_fn_tp_constraints = (const char* const*)fn_decl->type_param_constraints;
            }

            ctx->current_scope = scope_push(parent);
            ctx->inside_function = 1;

            for (int i = 0; i < fn_decl->param_count; i++) {
                /* Sprint 3: if the parameter has a type annotation, use
                 * it as the inferred type. Phase 2 added struct names.
                 * Generics PR 2: bare/nested references to this fn's OWN
                 * type parameters ("T", "array<T>") are legal here and
                 * stay UNKNOWN at declaration time — bindings happen at
                 * call sites (RFC §5.3). */
                LamoType param_type = LAMO_TYPE_UNKNOWN;
                const char* param_struct_name = NULL;
                if (fn_decl->param_types && fn_decl->param_types[i]) {
                    const char* raw_ann = fn_decl->param_types[i];
                    int is_own_tp = 0;
                    {
                        int scope_tp_count = fn_decl->type_param_count + ctx->impl_tp_count;
                        for (int t = 0; t < scope_tp_count; t++) {
                            const char* tp_name = t < fn_decl->type_param_count
                                ? fn_decl->type_params[t]
                                : ctx->impl_tp_names[t - fn_decl->type_param_count];
                            if (strcmp(raw_ann, tp_name) == 0 || strstr(raw_ann, tp_name)) {
                                is_own_tp = 1;
                                break;
                            }
                        }
                    }
                    if (!is_own_tp) {
                        param_type = annotation_to_type_with_ctx(ctx, raw_ann);
                    }
                    if (param_type == LAMO_TYPE_UNKNOWN && !is_own_tp) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "unknown type annotation '%s' on parameter '%s' (expected int, float, string, bool, array<T>, void, a struct name, or a declared type parameter)",
                                 raw_ann, fn_decl->params[i]);
                        semantic_error_at(ctx, node->line, node->column, message);
                    } else if (param_type == LAMO_TYPE_STRUCT) {
                        param_struct_name = raw_ann;
                    }
                }
                if (param_type == LAMO_TYPE_STRUCT && param_struct_name) {
                    scope_define_struct_var(ctx, ctx->current_scope, fn_decl->params[i], param_struct_name, node->line, node->column, node->file_path);
                } else {
                    scope_define(ctx, ctx->current_scope, fn_decl->params[i], SYMBOL_VAR, 0, param_type, node->line, node->column, node->file_path);
                }
                /* 2.9.0 traits: attach the normalized annotation to the
                 * param's symbol even when the head is one of the fn's
                 * own type parameters ("T", "array<T>"). Method-call
                 * resolution reads this to detect (and honestly reject)
                 * method calls on type-parameter values — under the
                 * erasure backend they previously fell into a silent
                 * lamo_make_int(0) fallback. */
                if (fn_decl->param_types && fn_decl->param_types[i]) {
                    Symbol* p_sym = scope_find_in_current(ctx->current_scope, fn_decl->params[i]);
                    if (p_sym && !p_sym->full_type) {
                        char* p_norm = semantic_normalize_type(fn_decl->param_types[i]);
                        p_sym->full_type = lamo_intern_type(p_norm);
                        free(p_norm);
                    }
                }
            }

            /* Phase 2: if we're inside an impl block, define `self` as a
             * struct-typed variable so the method body can reference it.
             * Methods in Lamo don't declare `self` as a parameter (it's
             * implicit), so we add it to the local scope here. The codegen
             * emits `self` as the first parameter of the underlying C function. */
            if (ctx->current_impl_struct) {
                scope_define_struct_var(ctx, ctx->current_scope, "self", ctx->current_impl_struct, node->line, node->column, node->file_path);
            }

            /* Look up the symbol we registered for this function so we can
             * update its return type after body inference (if no annotation). */
            Symbol* fn_symbol = scope_find(parent, fn_decl->name);

            /* Set context so AST_RETURN_STMT can validate against us. */
            ctx->current_fn_return_type = fn_symbol ? fn_symbol->type : LAMO_TYPE_UNKNOWN;
            ctx->current_fn_name = fn_decl->name;

            /* If no annotation yet, do a shallow pre-scan of the body to
             * infer a return type before visiting (so call sites that appear
             * later in the same file can benefit from it). Also detect when
             * multiple return statements within the body yield incompatible
             * types — that is a semantic error even without an annotation. */
            if (ctx->current_fn_return_type == LAMO_TYPE_UNKNOWN && fn_decl->body) {
                LamoType inferred = infer_fn_return_type_from_body(fn_decl->body, 0);
                if (inferred != LAMO_TYPE_UNKNOWN) {
                    ctx->current_fn_return_type = inferred;
                    if (fn_symbol) fn_symbol->type = inferred;
                } else {
                    /* infer returned UNKNOWN — either no returns, or a conflict.
                     * Re-scan to detect the conflict so we can emit a good error.
                     * We look for any two concrete-typed return statements that
                     * disagree. Use a simple two-pass: collect first type, then
                     * find one that differs. */
                    LamoType first_ret_type = LAMO_TYPE_UNKNOWN;
                    /* Walk the body looking for literal-typed return stmts. */
                    ASTNode* scan = fn_decl->body;
                    /* For a block, look at its statements directly. */
                    if (scan && scan->type == AST_BLOCK) {
                        scan = ((ASTBlock*)scan)->statements;
                    }
                    /* Find the first typed return. */
                    for (ASTNode* s = scan; s; s = s->next) {
                        if (s->type == AST_RETURN_STMT) {
                            ASTReturnStmt* rs = (ASTReturnStmt*)s;
                            if (rs->expression) {
                                LamoType t = LAMO_TYPE_UNKNOWN;
                                switch (rs->expression->type) {
                                    case AST_INT_LITERAL:    t = LAMO_TYPE_INT;    break;
                                    case AST_FLOAT_LITERAL:  t = LAMO_TYPE_FLOAT;  break;
                                    case AST_STRING_LITERAL: t = LAMO_TYPE_STRING; break;
                                    case AST_BOOL_LITERAL:   t = LAMO_TYPE_BOOL;   break;
                                    default: break;
                                }
                                if (t != LAMO_TYPE_UNKNOWN) {
                                    if (first_ret_type == LAMO_TYPE_UNKNOWN) {
                                        first_ret_type = t;
                                    } else if (first_ret_type != t) {
                                        int nc = (first_ret_type == LAMO_TYPE_INT && t == LAMO_TYPE_FLOAT) ||
                                                 (first_ret_type == LAMO_TYPE_FLOAT && t == LAMO_TYPE_INT);
                                        if (!nc) {
                                            char message[256];
                                            snprintf(message, sizeof(message),
                                                "function '%s' has inconsistent return types: %s and %s",
                                                fn_decl->name, type_name(first_ret_type), type_name(t));
                                            semantic_error_at(ctx, s->line, s->column, message);
                                        }
                                    }
                                }
                            }
                        } else if (s->type == AST_IF_STMT) {
                            /* Descend one level into if/else to catch the common pattern. */
                            ASTIfStmt* is = (ASTIfStmt*)s;
                            ASTNode* branches[2] = { is->then_branch, is->else_branch };
                            for (int b = 0; b < 2; b++) {
                                ASTNode* br = branches[b];
                                if (!br) continue;
                                ASTNode* brs = br->type == AST_BLOCK
                                    ? ((ASTBlock*)br)->statements : br;
                                for (ASTNode* bs = brs; bs; bs = bs->next) {
                                    if (bs->type != AST_RETURN_STMT) continue;
                                    ASTReturnStmt* rs = (ASTReturnStmt*)bs;
                                    if (!rs->expression) continue;
                                    LamoType t = LAMO_TYPE_UNKNOWN;
                                    switch (rs->expression->type) {
                                        case AST_INT_LITERAL:    t = LAMO_TYPE_INT;    break;
                                        case AST_FLOAT_LITERAL:  t = LAMO_TYPE_FLOAT;  break;
                                        case AST_STRING_LITERAL: t = LAMO_TYPE_STRING; break;
                                        case AST_BOOL_LITERAL:   t = LAMO_TYPE_BOOL;   break;
                                        default: break;
                                    }
                                    if (t != LAMO_TYPE_UNKNOWN) {
                                        if (first_ret_type == LAMO_TYPE_UNKNOWN) {
                                            first_ret_type = t;
                                        } else if (first_ret_type != t) {
                                            int nc = (first_ret_type == LAMO_TYPE_INT && t == LAMO_TYPE_FLOAT) ||
                                                     (first_ret_type == LAMO_TYPE_FLOAT && t == LAMO_TYPE_INT);
                                            if (!nc) {
                                                char message[256];
                                                snprintf(message, sizeof(message),
                                                    "function '%s' has inconsistent return types: %s and %s",
                                                    fn_decl->name, type_name(first_ret_type), type_name(t));
                                                semantic_error_at(ctx, bs->line, bs->column, message);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            semantic_visit_statement(ctx, fn_decl->body);

            /* Restore enclosing fn type-parameter scope (PR 2). */
            ctx->cur_fn_tp_names = saved_fn_tps;
            ctx->cur_fn_tp_count = saved_fn_tp_count;
            ctx->cur_fn_tp_constraints = saved_fn_tp_cons;
            free(own_tp_interned);

            Scope* finished = ctx->current_scope;
            ctx->current_scope = parent;
            ctx->inside_function = previous_inside_function;
            ctx->current_fn_return_type = previous_fn_return_type;
            ctx->current_fn_name = previous_fn_name;
            scope_free(finished);
            break;
        }
        case AST_BLOCK:
            semantic_visit_block(ctx, (ASTBlock*)node);
            break;
        case AST_IF_STMT: {
            ASTIfStmt* if_stmt = (ASTIfStmt*)node;
            LamoType cond_type = semantic_infer_expression(ctx, if_stmt->condition);
            semantic_check_truthy_operand(ctx, cond_type, "if condition", node->line, node->column);
            semantic_visit_statement(ctx, if_stmt->then_branch);
            semantic_visit_statement(ctx, if_stmt->else_branch);
            break;
        }
        case AST_WHILE_STMT: {
            ASTWhileStmt* while_stmt = (ASTWhileStmt*)node;
            LamoType cond_type = semantic_infer_expression(ctx, while_stmt->condition);
            semantic_check_truthy_operand(ctx, cond_type, "while condition", node->line, node->column);
            int prev_in_loop = ctx->inside_loop;
            ctx->inside_loop = 1;
            semantic_visit_statement(ctx, while_stmt->body);
            ctx->inside_loop = prev_in_loop;
            break;
        }
        case AST_FOR_STMT: {
            ASTForStmt* for_stmt = (ASTForStmt*)node;
            Scope* parent = ctx->current_scope;
            ctx->current_scope = scope_push(parent);

            semantic_visit_statement(ctx, for_stmt->initializer);
            {
                LamoType cond_type = semantic_infer_expression(ctx, for_stmt->condition);
                semantic_check_truthy_operand(ctx, cond_type, "for condition", node->line, node->column);
            }
            semantic_visit_statement(ctx, for_stmt->increment);
            int prev_in_loop = ctx->inside_loop;
            ctx->inside_loop = 1;
            semantic_visit_statement(ctx, for_stmt->body);
            ctx->inside_loop = prev_in_loop;

            Scope* finished = ctx->current_scope;
            ctx->current_scope = parent;
            scope_free(finished);
            break;
        }
        case AST_RETURN_STMT: {
            ASTReturnStmt* return_stmt = (ASTReturnStmt*)node;
            if (!ctx->inside_function) {
                semantic_error_at(ctx, node->line, node->column, "return is only valid inside a function");
            }
            if (return_stmt->expression) {
                LamoType ret_expr_type = semantic_infer_expression(ctx, return_stmt->expression);
                /* Validate against the declared/inferred return type. Skip
                 * when either side is UNKNOWN to avoid cascading errors. */
                if (ctx->current_fn_return_type != LAMO_TYPE_UNKNOWN
                    && ret_expr_type != LAMO_TYPE_UNKNOWN
                    && ret_expr_type != ctx->current_fn_return_type) {
                    /* Allow numeric widening (int ↔ float). */
                    int numeric_compat =
                        (ctx->current_fn_return_type == LAMO_TYPE_FLOAT && ret_expr_type == LAMO_TYPE_INT) ||
                        (ctx->current_fn_return_type == LAMO_TYPE_INT   && ret_expr_type == LAMO_TYPE_FLOAT);
                    if (!numeric_compat) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "function '%s' declared to return %s but this return yields %s",
                                 ctx->current_fn_name ? ctx->current_fn_name : "<unknown>",
                                 type_name(ctx->current_fn_return_type),
                                 type_name(ret_expr_type));
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                }
            }
            break;
        }
        case AST_BREAK_STMT:
            // Próximo passo 5: break é inválido fora de loops. `if` não conta
            // como loop — só while e for incrementam ctx->inside_loop.
            if (!ctx->inside_loop) {
                semantic_error_at(ctx, node->line, node->column,
                                  "break is only valid inside a while or for loop");
            }
            break;
        case AST_CONTINUE_STMT:
            if (!ctx->inside_loop) {
                semantic_error_at(ctx, node->line, node->column,
                                  "continue is only valid inside a while or for loop");
            }
            break;
        case AST_ASSIGN_STMT: {
            ASTAssignStmt* assign_stmt = (ASTAssignStmt*)node;
            Symbol* symbol = scope_find(ctx->current_scope, assign_stmt->name);
            if (!symbol || symbol->kind != SYMBOL_VAR) {
                char message[256];
                char hint[256];
                if (symbol && symbol->kind == SYMBOL_FN) {
                    /* Phase 3 item "validate that assignment targets are
                     * valid identifiers": naming a function on the left
                     * of '=' is a category error, not "undeclared". */
                    snprintf(message, sizeof(message),
                             "cannot assign to function '%s' (functions are immutable)", assign_stmt->name);
                    snprintf(hint, sizeof(hint),
                             "did you mean to CALL it ('%s(...)') or declare a variable with `let %s = ...;`?",
                             assign_stmt->name, assign_stmt->name);
                    semantic_error_at_hint(ctx, node->line, node->column, message, hint);
                } else if (!symbol && lamo_builtin_lookup(assign_stmt->name)) {
                    snprintf(message, sizeof(message),
                             "cannot assign to builtin '%s'", assign_stmt->name);
                    snprintf(hint, sizeof(hint),
                             "builtins are immutable; declare your own variable with `let %s = ...;` to shadow the builtin name",
                             assign_stmt->name);
                    semantic_error_at_hint(ctx, node->line, node->column, message, hint);
                } else {
                    snprintf(message, sizeof(message), "assignment to undeclared variable '%s'", assign_stmt->name ? assign_stmt->name : "<null>");
                    snprintf(hint, sizeof(hint),
                             "did you mean `let %s = ...;`? Lamo requires variables to be declared before assignment.",
                             assign_stmt->name ? assign_stmt->name : "<null>");
                    semantic_error_at_hint(ctx, node->line, node->column, message, hint);
                }
            }
            LamoType value_type = semantic_infer_expression(ctx, assign_stmt->value);
            // Update the variable's inferred type to the new value. Lamo is
            // dynamically typed, so reassignment with a different type is
            // allowed — but we still want to flag incompatibilities in
            // compound assignment (+=, -=).
            if (symbol) {
                if (assign_stmt->op_type == TOKEN_PLUS_EQ) {
                    // += : if symbol is string, value can be anything (concat).
                    // If symbol is numeric, value must be numeric.
                    if (is_numeric_type(symbol->type) && value_type == LAMO_TYPE_STRING) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "cannot use '+=' to add string to numeric variable '%s' (got %s += %s)",
                                 assign_stmt->name, type_name(symbol->type), type_name(value_type));
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                } else if (assign_stmt->op_type == TOKEN_MINUS_EQ) {
                    // -= : both must be numeric.
                    semantic_check_numeric_operand(ctx, "-=",
                                                   symbol->type, value_type,
                                                   node->line, node->column);
                }
                symbol->type = value_type;
            }
            break;
        }
        case AST_CALL_STMT: {
            ASTCallStmt* call_stmt = (ASTCallStmt*)node;
            /* 2.7.0 (FU4): pass the node so enum-variant constructor
             * statements (`Some(5);`) get annotated — codegen needs the
             * sema_enum_name/sema_variant_index stamps to emit
             * lamo_make_enum instead of a bogus call to the variant
             * global. */
            semantic_visit_call_full(ctx, call_stmt->name,
                                     call_stmt->type_args, call_stmt->type_arg_count,
                                     call_stmt->args, call_stmt->arg_count,
                                     node->line, node->column, node);
            break;
        }
        case AST_MEMBER_CALL: {
            /* Sprint 4: `module.member(args);` statement. Resolve the
             * alias against the module registry, validate the member
             * exists, and validate call arity. The args are visited for
             * type errors just like a regular call.
             *
             * Phase 2: AST_MEMBER_CALL is also used for value method
             * calls like `arr.push(x)` and `player.damage(10)`. The
             * semantic_infer_expression function dispatches based on the
             * object's inferred type. */
            ASTMemberCall* mc = (ASTMemberCall*)node;
            semantic_infer_expression(ctx, (ASTNode*)mc);  /* reuse the expression-path logic */
            break;
        }
        case AST_IMPORT:
            // import é resolvido pelo loader antes da análise semântica; nada a
            // validar aqui além da estrutura.
            break;
        /* ─── Phase 2: struct / impl / enum / match / place-assign ────── */
        case AST_STRUCT_DECL: {
            /* Already registered during the pre-pass; nothing to visit
             * for non-generic structs. For generic structs (Generics PR
             * 1), we validate:
             *   - Type parameter names are unique within the declaration.
             *   - Each field type is a builtin, a declared struct name,
             *     one of this struct's type parameters — GENERICS PR 2:
             *     recursively for NESTED annotations like `array<T>` or
             *     `Pair<int, array<T>>`, so containers of parameters are
             *     legal but typos still fail fast. */
            ASTStructDecl* sd = (ASTStructDecl*)node;
            /* Check type parameter uniqueness. */
            for (int i = 0; i < sd->type_param_count; i++) {
                for (int j = i + 1; j < sd->type_param_count; j++) {
                    if (strcmp(sd->type_params[i], sd->type_params[j]) == 0) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "duplicate type parameter '%s' in struct '%s'",
                                 sd->type_params[i], sd->name);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                }
                const char* con = sd->type_param_constraints
                                      ? sd->type_param_constraints[i] : NULL;
                if (con && lamo_constraint_kind(con) < 0 && !find_trait_def(ctx, con)) {
                    char message[320];
                    snprintf(message, sizeof(message),
                             "unknown constraint '%s' on type parameter '%s' of struct '%s' (catalogue: Any, Eq, Ord, Num, Hash, Show — or a declared trait)",
                             con, sd->type_params[i], sd->name);
                    semantic_error_at(ctx, node->line, node->column, message);
                }
            }
            /* Validate field types (nested-aware since PR 2). */
            for (int i = 0; i < sd->field_count; i++) {
                const char* ft = sd->field_types[i];
                if (!ft) continue;
                if (!lamo_validate_annotation_tree(ctx, sd, ft)) {
                    char message[320];
                    snprintf(message, sizeof(message),
                             "struct '%s' field '%s' has unknown type '%s' (expected builtins, declared structs, type parameters, or generic nests of those)",
                             sd->name, sd->field_names[i], ft);
                    semantic_error_at(ctx, node->line, node->column, message);
                }
            }
            break;
        }
        case AST_TRAIT_DECL: {
            /* 2.9.0 traits: a trait is a compile-time CONTRACT — no code
             * is generated and no symbol is added to the variable
             * namespace (trait names resolve through find_trait_def, not
             * the scope chain). Validation here:
             *   - duplicate method names within the trait are errors;
             *   - every parameter / return annotation must resolve
             *     (builtins, declared structs/enums — traits are
             *     non-generic in 2.9.0, so type parameters are NOT
             *     accepted; the parser already rejects `<T>` on the
             *     signature itself). */
            ASTTraitDecl* td = (ASTTraitDecl*)node;
            for (ASTNode* m = td->methods; m; m = m->next) {
                if (m->type != AST_FN_DECL) continue;
                ASTFnDecl* sig = (ASTFnDecl*)m;
                for (ASTNode* prev = td->methods; prev && prev != m; prev = prev->next) {
                    if (prev->type == AST_FN_DECL &&
                        strcmp(((ASTFnDecl*)prev)->name, sig->name) == 0) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "duplicate method '%s' in trait '%s'",
                                 sig->name, td->name);
                        semantic_error_at(ctx, m->line, m->column, message);
                    }
                }
                for (int i = 0; i < sig->param_count; i++) {
                    const char* raw = sig->param_types ? sig->param_types[i] : NULL;
                    if (!raw) continue;
                    const char* norm = NULL;
                    LamoType k = annotation_resolve_full(ctx, raw, &norm);
                    if (k == LAMO_TYPE_UNKNOWN) {
                        char message[320];
                        snprintf(message, sizeof(message),
                                 "unknown type annotation '%s' on parameter '%s' of trait method '%s' (trait signatures use builtins and declared struct/enum names)",
                                 raw, sig->params[i], sig->name);
                        semantic_error_at(ctx, m->line, m->column, message);
                    }
                }
                if (sig->return_type_annotation) {
                    const char* norm = NULL;
                    LamoType k = annotation_resolve_full(ctx, sig->return_type_annotation, &norm);
                    if (k == LAMO_TYPE_UNKNOWN) {
                        char message[320];
                        snprintf(message, sizeof(message),
                                 "unknown return type annotation '%s' on trait method '%s' (trait signatures use builtins and declared struct/enum names)",
                                 sig->return_type_annotation, sig->name);
                        semantic_error_at(ctx, m->line, m->column, message);
                    }
                }
            }
            break;
        }
        case AST_IMPL_DECL: {
            ASTImplDecl* id = (ASTImplDecl*)node;
            /* Validate the struct exists. */
            if (!find_struct_def(ctx, id->struct_name)) {
                char message[256];
                if (id->trait_name) {
                    /* 2.9.0: name the trait in the diagnostic so the
                     * reader knows WHICH impl block is broken. */
                    snprintf(message, sizeof(message),
                             "impl %s for unknown struct '%s' (declare it with `struct %s { ... }` first)",
                             id->trait_name, id->struct_name, id->struct_name);
                } else {
                    snprintf(message, sizeof(message),
                             "impl for unknown struct '%s' (declare it with `struct %s { ... }` first)",
                             id->struct_name, id->struct_name);
                }
                semantic_error_at(ctx, node->line, node->column, message);
                break;
            }
            /* ── 2.9.0 traits: contract validation ─────────────────────
             * For `impl Trait for Type` blocks we check, in order:
             *   1. the trait is declared (find_trait_def);
             *   2. no earlier impl of the SAME trait for the SAME struct
             *      exists (one impl per pair — "coherence");
             *   3. COMPLETENESS: every required method of the trait is
             *      present among the struct's methods;
             *   4. SIGNATURES: arity matches exactly; annotations are
             *      compared (normalized) whenever BOTH sides annotate.
             * A missing/unknown trait is reported but the block then
             * falls through to the shared inherent-impl flow below, so
             * its methods still resolve and errors don't cascade. */
            if (id->trait_name) {
                ASTTraitDecl* td = find_trait_def(ctx, id->trait_name);
                if (!td) {
                    char message[256];
                    char hint[160];
                    snprintf(message, sizeof(message),
                             "impl for unknown trait '%s' (no `trait %s { ... }` declaration exists)",
                             id->trait_name, id->trait_name);
                    snprintf(hint, sizeof(hint),
                             "declare the trait first, or drop `for %s` to write an inherent impl",
                             id->struct_name);
                    semantic_error_at_hint(ctx, node->line, node->column, message, hint);
                } else {
                    /* 2. Coherence: one impl per (trait, struct) pair. */
                    if (find_trait_impl_before(ctx, id->trait_name, id->struct_name, node)) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "duplicate impl of trait '%s' for struct '%s' (a type may implement a trait only once)",
                                 id->trait_name, id->struct_name);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                    /* 3 + 4. Completeness + signature compatibility. */
                    for (ASTNode* sm = td->methods; sm; sm = sm->next) {
                        if (sm->type != AST_FN_DECL) continue;
                        ASTFnDecl* sig = (ASTFnDecl*)sm;
                        ASTFnDecl* impl_fn = find_method(ctx, id->struct_name, sig->name);
                        if (!impl_fn) {
                            char message[320];
                            char hint[160];
                            snprintf(message, sizeof(message),
                                     "impl of trait '%s' for struct '%s' is missing required method '%s'",
                                     id->trait_name, id->struct_name, sig->name);
                            snprintf(hint, sizeof(hint),
                                     "add `fn %s(...) { ... }` to this impl block",
                                     sig->name);
                            semantic_error_at_hint(ctx, node->line, node->column, message, hint);
                            continue;
                        }
                        if (sig->param_count != impl_fn->param_count) {
                            char message[320];
                            snprintf(message, sizeof(message),
                                     "method '%s' of impl %s for %s takes %d parameter(s) but the trait declares %d",
                                     sig->name, id->trait_name, id->struct_name,
                                     impl_fn->param_count, sig->param_count);
                            semantic_error_at(ctx, node->line, node->column, message);
                            continue;
                        }
                        /* Per-parameter annotations: compare only when
                         * both sides annotate (Lamo keeps annotations
                         * optional; a missing annotation can't conflict). */
                        for (int pi = 0; pi < sig->param_count; pi++) {
                            const char* sig_ann = sig->param_types ? sig->param_types[pi] : NULL;
                            const char* imp_ann = impl_fn->param_types ? impl_fn->param_types[pi] : NULL;
                            if (!sig_ann || !imp_ann) continue;
                            char* sig_norm = semantic_normalize_type(sig_ann);
                            char* imp_norm = semantic_normalize_type(imp_ann);
                            if (strcmp(sig_norm, imp_norm) != 0) {
                                char message[400];
                                snprintf(message, sizeof(message),
                                         "parameter %d of '%s' in impl %s for %s has type '%s' but the trait declares '%s'",
                                         pi + 1, sig->name, id->trait_name, id->struct_name,
                                         imp_norm, sig_norm);
                                semantic_error_at(ctx, node->line, node->column, message);
                            }
                            free(sig_norm);
                            free(imp_norm);
                        }
                        /* Return annotations: same both-sides rule. */
                        if (sig->return_type_annotation && impl_fn->return_type_annotation) {
                            char* sig_norm = semantic_normalize_type(sig->return_type_annotation);
                            char* imp_norm = semantic_normalize_type(impl_fn->return_type_annotation);
                            if (strcmp(sig_norm, imp_norm) != 0) {
                                char message[400];
                                snprintf(message, sizeof(message),
                                         "method '%s' of impl %s for %s returns '%s' but the trait declares '%s'",
                                         sig->name, id->trait_name, id->struct_name,
                                         imp_norm, sig_norm);
                                semantic_error_at(ctx, node->line, node->column, message);
                            }
                            free(sig_norm);
                            free(imp_norm);
                        }
                    }
                }
            }
            /* RFC §4.4: generic impl validation.
             *   - the echo `Stack<T>` must name exactly this impl's own
             *     parameters, in order;
             *   - constraints referenced here must exist in the catalogue. */
            if (id->type_arg_count > 0 || id->type_param_count > 0) {
                if (id->type_arg_count != id->type_param_count) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "impl for '%s' declares %d type parameter(s) but echoes %d type argument(s)",
                             id->struct_name, id->type_param_count, id->type_arg_count);
                    semantic_error_at(ctx, node->line, node->column, message);
                } else {
                    for (int i = 0; i < id->type_arg_count; i++) {
                        if (strcmp(id->type_args[i], id->type_params[i]) != 0) {
                            char message[256];
                            snprintf(message, sizeof(message),
                                     "impl echo '%s<%s>' does not match declared parameter '%s' (position %d)",
                                     id->struct_name, id->type_args[i], id->type_params[i], i + 1);
                            semantic_error_at(ctx, node->line, node->column, message);
                        }
                    }
                }
            }
            /* Set the current impl struct so method bodies can use `self`.
             * Mark each method's AST node with sema_struct_name so codegen
             * knows to (a) emit it with the mangled name `lamo_method_<Type>__<name>`
             * and (b) prepend `self` as the first parameter. We do NOT
             * mangle fn->name in-place — that would break find_method,
             * which looks up methods by their original name. */
            const char* prev_impl = ctx->current_impl_struct;
            ctx->current_impl_struct = id->struct_name;
            {
                /* Inherit impl type parameters so method signatures can
                 * use them (RFC §4.4). */
                const char* const* saved_tp_names = ctx->impl_tp_names;
                int saved_tp_count = ctx->impl_tp_count;
                const char** tp_interned = NULL;
                if (id->type_param_count > 0) {
                    tp_interned = malloc(sizeof(const char*) * (size_t)id->type_param_count);
                    if (!tp_interned) {
                        perror("Failed to allocate impl tp name array");
                        exit(EXIT_FAILURE);
                    }
                    for (int i = 0; i < id->type_param_count; i++) {
                        tp_interned[i] = lamo_intern_type(id->type_params[i]);
                    }
                    ctx->impl_tp_names = tp_interned;
                    ctx->impl_tp_count = id->type_param_count;
                }
                for (ASTNode* m = id->methods; m; m = m->next) {
                    if (m->type == AST_FN_DECL) {
                        /* 2.10.0: trait-constrained type parameters ride
                         * the dictionary-dispatch ABI, whose hidden
                         * parameters codegen emits for STANDALONE fns
                         * only. Impl methods keep the erasure ABI (self
                         * + declared params), so a trait constraint here
                         * would promise a dictionary nobody passes. */
                        ASTFnDecl* md = (ASTFnDecl*)m;
                        if (md->type_param_constraints) {
                            for (int t = 0; t < md->type_param_count; t++) {
                                const char* con = md->type_param_constraints[t];
                                if (con && lamo_constraint_kind(con) < 0 &&
                                    find_trait_def(ctx, con)) {
                                    char message[340];
                                    snprintf(message, sizeof(message),
                                             "trait-constrained type parameter '%s: %s' on method '%s' is not supported (method dispatch keeps the erased ABI)",
                                             md->type_params[t], con, md->name);
                                    char hint[280];
                                    snprintf(hint, sizeof(hint),
                                             "declare a standalone generic fn (`fn %s_for<T: %s>(r: %s, ...)`) or move the generic behavior to a free function",
                                             md->name, con, id->struct_name ? id->struct_name : "T");
                                    semantic_error_at_hint(ctx, m->line, m->column, message, hint);
                                }
                            }
                        }
                        m->sema_struct_name = id->struct_name;
                        semantic_visit_statement(ctx, m);
                    }
                }
                ctx->impl_tp_names = saved_tp_names;
                ctx->impl_tp_count = saved_tp_count;
                free(tp_interned);
            }
            ctx->current_impl_struct = prev_impl;
            break;
        }
        case AST_ENUM_DECL: {
            /* Already registered during the pre-pass. Validate variant
             * names are unique within the enum.
             *
             * 2.6.0: for tagged-union enums we also validate
             *   - type parameter uniqueness + constraint catalogue (same
             *     rules as generic structs, PR 6),
             *   - each payload annotation: builtins, declared structs,
             *     the enum's own type parameters, or nested generics of
             *     those (same rule as struct fields). */
            ASTEnumDecl* ed = (ASTEnumDecl*)node;
            int i;
            for (i = 0; i < ed->variant_count; i++) {
                int j;
                for (j = i + 1; j < ed->variant_count; j++) {
                    if (strcmp(ed->variants[i], ed->variants[j]) == 0) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "duplicate variant '%s' in enum '%s'",
                                 ed->variants[i], ed->name);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                }
            }
            /* 2.6.0: type parameter checks (mirrors struct logic). */
            for (i = 0; i < ed->type_param_count; i++) {
                int j;
                for (j = i + 1; j < ed->type_param_count; j++) {
                    if (strcmp(ed->type_params[i], ed->type_params[j]) == 0) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "duplicate type parameter '%s' in enum '%s'",
                                 ed->type_params[i], ed->name);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                }
                {
                    const char* con = ed->type_param_constraints
                                          ? ed->type_param_constraints[i] : NULL;
                    if (con && lamo_constraint_kind(con) < 0 && !find_trait_def(ctx, con)) {
                        char message[320];
                        snprintf(message, sizeof(message),
                                 "unknown constraint '%s' on type parameter '%s' of enum '%s' (catalogue: Any, Eq, Ord, Num, Hash, Show — or a declared trait)",
                                 con, ed->type_params[i], ed->name);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                }
            }
            /* 2.6.0: payload annotation validation. The shared annotation
             * validator takes a struct-shaped view; a stack shim with the
             * enum's name + type parameters gives payloads exactly the
             * same validation rules as struct fields. */
            if (ed->variant_payloads) {
                for (i = 0; i < ed->variant_count; i++) {
                    int pc = enum_variant_payload_count(ed, i);
                    int j;
                    for (j = 0; j < pc; j++) {
                        const char* ann = ed->variant_payloads[i][j];
                        ASTStructDecl shim;
                        if (!ann) continue;
                        memset(&shim, 0, sizeof(shim));
                        shim.name = ed->name;
                        shim.type_params = ed->type_params;
                        shim.type_param_constraints = ed->type_param_constraints;
                        shim.type_param_count = ed->type_param_count;
                        if (!lamo_validate_annotation_tree(ctx, &shim, ann)) {
                            char message[320];
                            snprintf(message, sizeof(message),
                                     "enum '%s' variant '%s' payload %d has unknown type '%s' (expected builtins, declared structs, this enum's type parameters, or generic nests of those)",
                                     ed->name, ed->variants[i], j + 1, ann);
                            semantic_error_at(ctx, node->line, node->column, message);
                        }
                    }
                }
            }
            break;
        }
        case AST_MATCH_STMT: {
            ASTMatchStmt* ms = (ASTMatchStmt*)node;
            LamoType scrut_type = semantic_infer_expression(ctx, ms->scrutinee);
            /* Validate each arm's pattern tree. Top-level patterns are:
             *   - "_" (wildcard) - always matches
             *   - a variant constructor (optionally qualified
             *     `Enum::Variant`) with a recursive payload sub-pattern
             *     list — 2.7.0 (FU2) supports nested destructuring like
             *     `Some(Pair(a, b))` and (FU4) qualification.
             * Binding leaves are collected in extraction order and
             * defined in a fresh scope covering the guard + body; the
             * `when` guard is visited in that scope so it can read the
             * bindings. Exhaustiveness counts only UNGUARDED arms
             * (Rust-style: a guard can fail, so it does not cover its
             * variant). */
            int has_wildcard = 0;
            int total_variants = -1;
            const char* scrut_enum_name = NULL;
            ASTEnumDecl* matched_enum = NULL;  /* enum decl being matched */

            for (int i = 0; i < ms->arm_count; i++) {
                LamoPattern* pat = ms->patterns[i];
                if (!pat) continue;
                if (pat->kind == LAMO_PATTERN_WILDCARD) {
                    has_wildcard = 1;
                } else if (pat->kind == LAMO_PATTERN_CTOR) {
                    ASTEnumDecl* arm_enum = NULL;
                    int vidx = -1;
                    char q_enum[128];
                    char q_variant[128];
                    if (split_qualified_variant_name(pat->name, q_enum, sizeof(q_enum),
                                                     q_variant, sizeof(q_variant))) {
                        arm_enum = find_variant_qualified(ctx, q_enum, q_variant, &vidx);
                        if (!arm_enum) {
                            char message[512];
                            if (find_enum_def(ctx, q_enum)) {
                                snprintf(message, sizeof(message),
                                         "enum '%s' has no variant '%s' (match pattern '%s')",
                                         q_enum, q_variant, pat->name);
                            } else {
                                snprintf(message, sizeof(message),
                                         "unknown enum '%s' in match pattern '%s'",
                                         q_enum, pat->name);
                            }
                            semantic_error_at(ctx, pat->line, pat->column, message);
                        }
                    } else {
                        const char* ename = find_enum_variant_any(ctx, pat->name, &vidx);
                        if (!ename) {
                            char message[256];
                            snprintf(message, sizeof(message),
                                     "match pattern '%s' is not a known enum variant (declare an `enum { ... }` first, or use '_' for wildcard)",
                                     pat->name);
                            semantic_error_at(ctx, pat->line, pat->column, message);
                        } else {
                            arm_enum = find_enum_def(ctx, ename);
                        }
                    }
                    if (arm_enum) {
                        /* Track the enum we're matching against. */
                        if (scrut_enum_name == NULL) {
                            scrut_enum_name = arm_enum->name;
                            matched_enum = arm_enum;
                            total_variants = arm_enum->variant_count;
                        } else if (strcmp(scrut_enum_name, arm_enum->name) != 0) {
                            char message[256];
                            snprintf(message, sizeof(message),
                                     "match arm pattern '%s' belongs to enum '%s', but earlier arms matched enum '%s'",
                                     pat->name, arm_enum->name, scrut_enum_name);
                            semantic_error_at(ctx, pat->line, pat->column, message);
                        }
                        /* Stamp for codegen (tag check + payload pulls). */
                        pat->sema_enum_name = arm_enum->name;
                        pat->sema_variant_index = vidx;
                        /* Top-level arity: payload count vs child count. */
                        int pcount = enum_variant_payload_count(arm_enum, vidx);
                        if (pcount == 0 && pat->child_count > 0) {
                            char message[256];
                            snprintf(message, sizeof(message),
                                     "variant '%s' carries no payloads; remove the binding list from the pattern",
                                     pat->name);
                            semantic_error_at(ctx, pat->line, pat->column, message);
                        } else if (pcount > 0 && pat->child_count == 0) {
                            char message[256];
                            snprintf(message, sizeof(message),
                                     "variant '%s' carries %d payload(s); bind them: %s(a) => ...",
                                     pat->name, pcount, pat->name);
                            semantic_error_at(ctx, pat->line, pat->column, message);
                        } else if (pcount > 0 && pat->child_count != pcount) {
                            char message[256];
                            snprintf(message, sizeof(message),
                                     "variant '%s' carries %d payload(s), but the pattern binds %d",
                                     pat->name, pcount, pat->child_count);
                            semantic_error_at(ctx, pat->line, pat->column, message);
                        }
                        /* NESTED payload sub-patterns (FU2): each ctor
                         * child must itself resolve to a known variant
                         * with matching arity; errors are emitted inside.
                         * Nested payload values are erased LamoValues —
                         * the payload's enum type is not statically
                         * known, so any declared variant is accepted and
                         * the tag check happens at runtime. */
                        for (int c = 0; c < pat->child_count; c++) {
                            LamoPattern* child = pat->children[c];
                            if (child && child->kind == LAMO_PATTERN_CTOR) {
                                lamo_sema_resolve_nested_pattern(ctx, child);
                            }
                        }
                    }
                } else if (pat->kind == LAMO_PATTERN_LITERAL) {
                    /* 2.8.0 (FU3): literal pattern — `1 => ...`,
                     * `"a" => ...`, `true => ...`. Type-checked against
                     * the scrutinee when the scrutinee's type is a
                     * known builtin (int→float widening kept, matching
                     * §7.3). Enum scrutinees defer to runtime semantics
                     * (legacy untagged enums ARE ints at runtime), and
                     * literal arms bind nothing and never count toward
                     * enum exhaustiveness. */
                    LamoType lit_type = semantic_infer_expression(ctx, pat->literal);
                    if ((scrut_type == LAMO_TYPE_INT || scrut_type == LAMO_TYPE_FLOAT ||
                         scrut_type == LAMO_TYPE_STRING || scrut_type == LAMO_TYPE_BOOL) &&
                        lit_type != LAMO_TYPE_UNKNOWN) {
                        /* Numeric pairs coerce both ways at runtime
                         * (lamo_equal compares via float when either side
                         * is float), so int↔float literal patterns are
                         * accepted against numeric scrutinees; everything
                         * else must match exactly. */
                        int numeric_pair = (scrut_type == LAMO_TYPE_INT || scrut_type == LAMO_TYPE_FLOAT) &&
                                           (lit_type == LAMO_TYPE_INT || lit_type == LAMO_TYPE_FLOAT);
                        int ok = numeric_pair ||
                                 (scrut_type == LAMO_TYPE_STRING && lit_type == LAMO_TYPE_STRING) ||
                                 (scrut_type == LAMO_TYPE_BOOL   && lit_type == LAMO_TYPE_BOOL);
                        if (!ok) {
                            char message[256];
                            snprintf(message, sizeof(message),
                                     "literal pattern of type '%s' cannot match scrutinee of type '%s'",
                                     type_name(lit_type), type_name(scrut_type));
                            semantic_error_at(ctx, pat->line, pat->column, message);
                        }
                    }
                }
            }
            /* Visit guards and bodies. Binding leaves are defined in a
             * fresh per-arm scope in EXTRACTION order (depth-first,
             * left-to-right — the same order codegen pulls payloads),
             * so `Some(Pair(a, b))` binds `a` then `b`. */
            for (int i = 0; i < ms->arm_count; i++) {
                LamoPattern* pat = ms->patterns[i];
                if (!ms->bodies[i] && !ms->guards[i]) continue;
                LamoPattern* leaf_bindings[LAMO_MAX_PATTERN_BINDINGS];
                int nb = 0;
                if (pat) lamo_pattern_collect_bindings(pat, leaf_bindings,
                                                       LAMO_MAX_PATTERN_BINDINGS, &nb);
                Scope* parent = ctx->current_scope;
                if (nb > 0) {
                    ctx->current_scope = scope_push(parent);
                    for (int b = 0; b < nb; b++) {
                        scope_define(ctx, ctx->current_scope, leaf_bindings[b]->name,
                                     SYMBOL_VAR, 0, LAMO_TYPE_UNKNOWN,
                                     leaf_bindings[b]->line, leaf_bindings[b]->column,
                                     node->file_path);
                    }
                }
                /* 2.7.0 (FU2): the `when` guard sees the bindings and
                 * must itself be a truthy-compatible expression (void is
                 * rejected, matching if/while conditions). */
                if (ms->guards[i]) {
                    LamoType gt = semantic_infer_expression(ctx, ms->guards[i]);
                    semantic_check_truthy_operand(ctx, gt, "match guard",
                                                  ms->guards[i]->line, ms->guards[i]->column);
                }
                if (ms->bodies[i]) {
                    semantic_visit_statement(ctx, ms->bodies[i]);
                }
                if (nb > 0) {
                    ctx->current_scope = parent;
                }
            }
            /* 2.6.0: annotate the match so codegen uses the tag-compare
             * desugar when the matched enum is a tagged union. */
            if (matched_enum && enum_decl_is_tagged(matched_enum)) {
                ms->sema_enum_name = matched_enum->name;
            }
            /* Exhaustiveness: with no wildcard arm, every variant must
             * be covered by at least one UNGUARDED arm — a guarded arm
             * can fail its condition and fall through (SPEC §4.6). */
            if (!has_wildcard && total_variants > 0) {
                int unique = 0;
                for (int i = 0; i < ms->arm_count; i++) {
                    LamoPattern* pat = ms->patterns[i];
                    if (!pat || pat->kind != LAMO_PATTERN_CTOR) continue;
                    if (ms->guards[i]) continue;  /* guarded arms don't cover */
                    const char* bare = pat->name;
                    const char* sep = strstr(pat->name, "::");
                    if (sep) bare = sep + 2;
                    int dup = 0;
                    for (int j = 0; j < i; j++) {
                        LamoPattern* prev = ms->patterns[j];
                        if (!prev || prev->kind != LAMO_PATTERN_CTOR) continue;
                        if (ms->guards[j]) continue;
                        const char* prev_bare = prev->name;
                        const char* prev_sep = strstr(prev->name, "::");
                        if (prev_sep) prev_bare = prev_sep + 2;
                        if (strcmp(bare, prev_bare) == 0) { dup = 1; break; }
                    }
                    if (!dup) unique++;
                }
                if (unique < total_variants) {
                    /* Kept as a compile error for 2.7.0 (the pre-2.7.0
                     * behavior was already fatal despite the "warning"
                     * wording); the message keeps its historical shape. */
                    char message[256];
                    snprintf(message, sizeof(message),
                             "warning: match on enum '%s' is not exhaustive (%d of %d variants covered; add a '_' arm or cover the rest)",
                             scrut_enum_name, unique, total_variants);
                    semantic_error_at(ctx, node->line, node->column, message);
                }
            }
            (void)scrut_type;
            break;
        }
        case AST_PLACE_ASSIGN_STMT: {
            ASTPlaceAssignStmt* pa = (ASTPlaceAssignStmt*)node;
            /* Validate the target is AST_INDEX_EXPR or AST_PROP_EXPR. */
            if (!pa->target) break;
            if (pa->target->type == AST_INDEX_EXPR) {
                ASTIndexExpr* ie = (ASTIndexExpr*)pa->target;
                /* The object should be array-typed. We infer its type to
                 * validate, but the codegen will emit lamo_array_set. */
                semantic_infer_expression(ctx, ie->array);
                semantic_infer_expression(ctx, ie->index);
                LamoType value_type = semantic_infer_expression(ctx, pa->value);
                (void)value_type;
            } else if (pa->target->type == AST_PROP_EXPR) {
                ASTPropExpr* pe = (ASTPropExpr*)pa->target;
                /* The object should be struct-typed; the field name must
                 * exist. semantic_infer_expression on a AST_PROP_EXPR
                 * already does this validation. */
                semantic_infer_expression(ctx, (ASTNode*)pe);
                LamoType value_type = semantic_infer_expression(ctx, pa->value);
                (void)value_type;
            } else {
                semantic_error_at(ctx, node->line, node->column,
                                  "invalid assignment target (expected arr[i] or obj.field)");
            }
            break;
        }
        default:
            break;
    }
}
/* ── 2.8.0 (FU4): match as an expression ────────────────────────────────
 * Validation itself lives in the AST_MATCH_STMT case of
 * semantic_visit_statement (unchanged since 2.7.0 — exhaustiveness,
 * pattern resolution, guards, bindings all run exactly once through
 * it). This wrapper runs that pass, then computes the VALUE type for
 * the expression form (`let x = match ... { ... }`) as the least upper
 * bound of the arm body types (SPEC §13).
 * Statement-shaped bodies contribute no value — they are skipped here
 * so their declarations are never re-visited. Expression bodies are
 * pure to re-infer (inference defines nothing in any scope), so the
 * extra pass is safe; a program with errors may report duplicates, but
 * it fails either way.
 * When every typed arm is an enum constructor of the SAME enum head,
 * the head is stamped on the match node (e.g. "Option") so the
 * let-annotation head check and call-site degraded-enum acceptance
 * (FU5) treat the whole match as that enum's type. */
static LamoType semantic_validate_match(SemanticContext* ctx, ASTMatchStmt* ms,
                                        ASTNode* node) {
    semantic_visit_statement(ctx, node);
    LamoType lub = LAMO_TYPE_UNKNOWN;
    int lub_mismatch = 0;
    const char* shared_head = NULL;
    int same_head = 1;
    for (int i = 0; i < ms->arm_count; i++) {
        ASTNode* body = ms->bodies[i];
        if (!body) continue;
        switch (body->type) {
            case AST_BLOCK: case AST_VAR_DECL: case AST_FN_DECL:
            case AST_ENUM_DECL: case AST_IF_STMT: case AST_WHILE_STMT:
            case AST_FOR_STMT: case AST_RETURN_STMT: case AST_BREAK_STMT:
            case AST_CONTINUE_STMT: case AST_ASSIGN_STMT: case AST_CALL_STMT:
            case AST_IMPORT: case AST_PLACE_ASSIGN_STMT: case AST_IMPL_DECL:
            case AST_STRUCT_DECL: case AST_TRAIT_DECL:
                continue;  /* statement body: no value contribution */
            default:
                break;
        }
        /* Recreate the arm's binding scope (the statement pass popped
         * its own): payload bindings must be visible while inferring
         * the body's value type — `Some(n) => n * 2`. */
        LamoPattern* leaf_bindings[LAMO_MAX_PATTERN_BINDINGS];
        int nb = 0;
        if (ms->patterns[i]) {
            lamo_pattern_collect_bindings(ms->patterns[i], leaf_bindings,
                                          LAMO_MAX_PATTERN_BINDINGS, &nb);
        }
        Scope* parent = ctx->current_scope;
        if (nb > 0) {
            ctx->current_scope = scope_push(parent);
            for (int b = 0; b < nb; b++) {
                scope_define(ctx, ctx->current_scope, leaf_bindings[b]->name,
                             SYMBOL_VAR, 0, LAMO_TYPE_UNKNOWN,
                             leaf_bindings[b]->line, leaf_bindings[b]->column,
                             node->file_path);
            }
        }
        LamoType bt = semantic_infer_expression(ctx, body);
        if (nb > 0) {
            ctx->current_scope = parent;
        }
        if (bt == LAMO_TYPE_UNKNOWN || bt == LAMO_TYPE_VOID) continue;
        if (bt == LAMO_TYPE_ENUM) {
            const char* ft = body->sema_full_type;
            if (!ft) {
                same_head = 0;
            } else {
                char h[64];
                ann_head(ft, h, sizeof(h));
                if (!shared_head) shared_head = lamo_intern_type(h);
                else if (strcmp(shared_head, h) != 0) same_head = 0;
            }
        }
        if (lub_mismatch) continue;
        if (lub == LAMO_TYPE_UNKNOWN) { lub = bt; continue; }
        if (lub == bt) continue;
        int numeric_pair = (lub == LAMO_TYPE_INT || lub == LAMO_TYPE_FLOAT) &&
                           (bt == LAMO_TYPE_INT || bt == LAMO_TYPE_FLOAT);
        if (numeric_pair) { lub = LAMO_TYPE_FLOAT; continue; }
        lub_mismatch = 1;
        lub = LAMO_TYPE_UNKNOWN;
    }
    if (lub == LAMO_TYPE_ENUM && same_head && shared_head) {
        node->sema_full_type = shared_head;
    }
    return lub;
}

// Infer the compile-time type of an expression and run any operator-level
// checks. Visits sub-expressions recursively. Returns LAMO_TYPE_UNKNOWN when
// the type cannot be inferred (e.g. due to a prior error).
static LamoType semantic_infer_expression(SemanticContext* ctx, ASTNode* node) {
    if (!node) {
        return LAMO_TYPE_UNKNOWN;
    }

    // Bug #5 fix: atualiza o path do nó atual. Expression nodes também podem
    // disparar erros (ex.: "use of undeclared variable"), e precisamos do
    // arquivo certo.
    if (node->file_path) {
        ctx->last_node_path = node->file_path;
    }

    switch (node->type) {
        case AST_INT_LITERAL:
            return LAMO_TYPE_INT;
        case AST_FLOAT_LITERAL:
            return LAMO_TYPE_FLOAT;
        case AST_STRING_LITERAL:
            return LAMO_TYPE_STRING;
        case AST_BOOL_LITERAL:
            return LAMO_TYPE_BOOL;
        case AST_IDENTIFIER: {
            ASTIdentifier* identifier = (ASTIdentifier*)node;
            Symbol* symbol = scope_find(ctx->current_scope, identifier->name);
            if (!symbol || symbol->kind != SYMBOL_VAR) {
                // Permite usar builtins como valores? Por enquanto não — apenas
                // como calls. Variáveis precisam estar declaradas.
                char message[256];
                char hint[256];
                snprintf(message, sizeof(message), "use of undeclared variable '%s'", identifier->name);
                snprintf(hint, sizeof(hint),
                         "did you forget to declare it with `let %s = ...;`?",
                         identifier->name);
                semantic_error_at_hint(ctx, node->line, node->column, message, hint);
                return LAMO_TYPE_UNKNOWN;
            }
            /* 2.6.0: a payload variant is not a first-class value — using
             * it bare (e.g. `let x = Some;`) is a compile error with a
             * hint pointing at constructor syntax (SPEC §3.5). */
            if (symbol->type == LAMO_TYPE_ENUM) {
                int vidx = -1;
                const char* venum = find_enum_variant_any(ctx, identifier->name, &vidx);
                if (venum) {
                    ASTEnumDecl* ved = find_enum_def(ctx, venum);
                    if (ved && enum_variant_payload_count(ved, vidx) > 0) {
                        char message[256];
                        char hint[256];
                        snprintf(message, sizeof(message),
                                 "variant '%s' carries a payload and cannot be used as a value",
                                 identifier->name);
                        snprintf(hint, sizeof(hint),
                                 "construct it with %s(...) and match with %s(x) => ...",
                                 identifier->name, identifier->name);
                        semantic_error_at_hint(ctx, node->line, node->column, message, hint);
                        return LAMO_TYPE_UNKNOWN;
                    }
                    /* 2.8.0 (FU5): a bare unit-variant value degrades to
                     * the BARE enum name as its concrete full type (e.g.
                     * `None` → "Option") — the same degraded shape the
                     * ctor branch produces for partially-inferable
                     * enums. Call sites expecting `Option<int>` accept
                     * it via the degraded-enum deferral in
                     * call_types_compatible. */
                    node->sema_full_type = lamo_intern_type(venum);
                }
            }
            return symbol->type;
        }
        case AST_VARIANT_REF: {
            /* 2.7.0 (FU4): qualified variant value — `Enum::Variant`.
             * Resolved exactly (no "later wins"), stamped for codegen.
             * Payload variants cannot be values, same as the bare form. */
            ASTVariantRef* vr = (ASTVariantRef*)node;
            int vidx = -1;
            ASTEnumDecl* ved = find_variant_qualified(ctx, vr->enum_name, vr->variant_name, &vidx);
            if (!ved) {
                char message[512];
                if (find_enum_def(ctx, vr->enum_name)) {
                    snprintf(message, sizeof(message),
                             "enum '%s' has no variant '%s'", vr->enum_name, vr->variant_name);
                } else {
                    snprintf(message, sizeof(message),
                             "unknown enum '%s' in qualified variant '%s::%s'",
                             vr->enum_name, vr->enum_name, vr->variant_name);
                }
                semantic_error_at(ctx, node->line, node->column, message);
                return LAMO_TYPE_UNKNOWN;
            }
            if (enum_variant_payload_count(ved, vidx) > 0) {
                char message[256];
                char hint[256];
                snprintf(message, sizeof(message),
                         "variant '%s::%s' carries a payload and cannot be used as a value",
                         vr->enum_name, vr->variant_name);
                snprintf(hint, sizeof(hint),
                         "construct it with %s::%s(...) and match with %s::%s(x) => ...",
                         vr->enum_name, vr->variant_name, vr->enum_name, vr->variant_name);
                semantic_error_at_hint(ctx, node->line, node->column, message, hint);
                return LAMO_TYPE_UNKNOWN;
            }
            node->sema_enum_name = ved->name;
            node->sema_variant_index = vidx;
            /* 2.8.0 (FU5): degraded bare-enum full type, matching the
             * bare unit-variant identifier path (call-site deferral). */
            node->sema_full_type = lamo_intern_type(ved->name);
            return enum_decl_is_tagged(ved) ? LAMO_TYPE_ENUM : LAMO_TYPE_INT;
        }
        case AST_BINARY_EXPR: {
            ASTBinaryExpr* expr = (ASTBinaryExpr*)node;
            LamoType left = semantic_infer_expression(ctx, expr->left);
            LamoType right = semantic_infer_expression(ctx, expr->right);

            switch (expr->operator) {
                case TOKEN_PLUS:
                    // + aceita string (concat) com qualquer coisa, ou numérico.
                    if (left == LAMO_TYPE_STRING || right == LAMO_TYPE_STRING) {
                        return LAMO_TYPE_STRING;
                    }
                    if (left == LAMO_TYPE_UNKNOWN || right == LAMO_TYPE_UNKNOWN) {
                        return LAMO_TYPE_UNKNOWN;
                    }
                    if (left == LAMO_TYPE_FLOAT || right == LAMO_TYPE_FLOAT) {
                        return LAMO_TYPE_FLOAT;
                    }
                    return LAMO_TYPE_INT;
                case TOKEN_MINUS:
                case TOKEN_STAR:
                case TOKEN_SLASH:
                case TOKEN_PERCENT: {
                    const char* op_name =
                        expr->operator == TOKEN_MINUS ? "-" :
                        expr->operator == TOKEN_STAR  ? "*" :
                        expr->operator == TOKEN_SLASH ? "/" : "%";
                    semantic_check_numeric_operand(ctx, op_name, left, right, node->line, node->column);
                    if (left == LAMO_TYPE_UNKNOWN || right == LAMO_TYPE_UNKNOWN) {
                        return LAMO_TYPE_UNKNOWN;
                    }
                    if (left == LAMO_TYPE_FLOAT || right == LAMO_TYPE_FLOAT) {
                        return LAMO_TYPE_FLOAT;
                    }
                    return LAMO_TYPE_INT;
                }
                case TOKEN_LT:
                case TOKEN_GT:
                case TOKEN_LT_EQ:
                case TOKEN_GT_EQ: {
                    const char* op_name =
                        expr->operator == TOKEN_LT    ? "<" :
                        expr->operator == TOKEN_GT    ? ">" :
                        expr->operator == TOKEN_LT_EQ ? "<=" : ">=";
                    semantic_check_numeric_operand(ctx, op_name, left, right, node->line, node->column);
                    return LAMO_TYPE_BOOL;
                }
                case TOKEN_EQ_EQ:
                case TOKEN_BANG_EQ:
                    // Equality accepts any pair; runtime handles mixed types by returning false.
                    return LAMO_TYPE_BOOL;
                case TOKEN_AND_AND:
                case TOKEN_OR_OR: {
                    /* Phase 4 item "validate logical operators by type",
                     * per SPEC §6.3/§7.5: both operands accept any type
                     * WITH defined truthiness; void values are a compile
                     * error. The result is bool built from truthiness
                     * (matches the shipped runtime lamo_and/lamo_or). */
                    const char* op_name =
                        expr->operator == TOKEN_AND_AND ? "&&" : "||";
                    char where[64];
                    snprintf(where, sizeof(where), "'%s' operand", op_name);
                    semantic_check_truthy_operand(ctx, left, where, node->line, node->column);
                    semantic_check_truthy_operand(ctx, right, where, node->line, node->column);
                    return LAMO_TYPE_BOOL;
                }
                default:
                    return LAMO_TYPE_UNKNOWN;
            }
        }
        case AST_UNARY_EXPR: {
            ASTUnaryExpr* expr = (ASTUnaryExpr*)node;
            LamoType right = semantic_infer_expression(ctx, expr->right);
            if (expr->operator == TOKEN_MINUS) {
                if (right == LAMO_TYPE_STRING) {
                    semantic_error_at(ctx, node->line, node->column,
                                      "unary '-' does not support string operand");
                }
                return right; // -int -> int, -float -> float
            }
            if (expr->operator == TOKEN_BANG) {
                semantic_check_truthy_operand(ctx, right, "'!' operand", node->line, node->column);
                return LAMO_TYPE_BOOL;
            }
            return LAMO_TYPE_UNKNOWN;
        }
        case AST_CALL_EXPR: {
            ASTCallExpr* call_expr = (ASTCallExpr*)node;
            return semantic_visit_call_full(ctx, call_expr->name,
                                            call_expr->type_args, call_expr->type_arg_count,
                                            call_expr->args, call_expr->arg_count,
                                            node->line, node->column, node);
        }
        case AST_MEMBER_CALL: {
            /* Sprint 4: `module.member(args)` in expression position.
             * Resolve through the module registry; if found, treat like
             * a regular function call (validate arity, visit args). The
             * return type is LAMO_TYPE_UNKNOWN — the module member's
             * body is the prefixed function, and we don't have a cheap
             * way to look up its inferred return type from here. The
             * type-inference downstream will simply treat the result as
             * unknown, which is safe (no cascading errors).
             *
             * Phase 2: AST_MEMBER_CALL is now also used for value method
             * calls — `arr.push(x)`, `arr.len()`, `player.damage(10)`.
             * The dispatch:
             *   - If the object is an identifier that matches a registered
             *     module alias, it's a module call (existing behavior).
             *   - Else if the object is an identifier that resolves to an
             *     array-typed variable, it's an array method call.
             *   - Else if the object is an identifier that resolves to a
             *     struct-typed variable, it's a struct method call.
             *   - Else: error. */
            ASTMemberCall* mc = (ASTMemberCall*)node;
            const char* alias = NULL;
            int resolved_module_call = 0;
            if (!mc->object) {
                semantic_error_at(ctx, node->line, node->column,
                                  "member call missing object expression");
                return LAMO_TYPE_UNKNOWN;
            }
            /* If the object is an identifier, try module-alias resolution
             * first (Sprint 4 behavior). 2.6.0 FU2: resolution no longer
             * returns early — it defers to the FULL signature-binding
             * path below so imported functions keep their return types
             * (including struct results) across module boundaries. */
            if (mc->object->type == AST_IDENTIFIER) {
                alias = ((ASTIdentifier*)mc->object)->name;
                if (ctx->module_resolve && ctx->module_resolve(alias, mc->member_name, ctx->module_user_data)) {
                    resolved_module_call = 1;
                    /* Registry arity check (fast path for all members). */
                    if (ctx->module_arity) {
                        int expected_arity = ctx->module_arity(alias, mc->member_name, ctx->module_user_data);
                        if (expected_arity >= 0 && expected_arity != mc->arg_count) {
                            char message[256];
                            snprintf(message, sizeof(message),
                                     "module member `%s.%s` expects %d argument(s), got %d",
                                     alias, mc->member_name, expected_arity, mc->arg_count);
                            semantic_error_at(ctx, node->line, node->column, message);
                        }
                    }
                    /* 2.6.0 (FU5): §10.6 step-1 export warning. */
                    semantic_error_module_export(ctx, alias, mc->member_name,
                                                node->line, node->column);
                }
            }
            if (resolved_module_call) {
                /* Generics PR 2 §5.3: when this member call targets an
                 * IMPORTED function, its renamed declaration still lives in
                 * the global scope — look it up and run the SAME signature
                 * binding/validation as plain calls, so Option<T>-style
                 * factories keep their payload types across boundaries.
                 * 2.6.0 FU2: when the substituted return type names a
                 * declared struct, the RESULT is struct-typed (SPEC §5.5). */
                if (ctx->module_resolve) {
                    const char* prefixed = ctx->module_resolve(alias, mc->member_name, ctx->module_user_data);
                    if (prefixed) {
                        Symbol* fsym = scope_find(ctx->current_scope, prefixed);
                        if (fsym && fsym->kind == SYMBOL_FN &&
                            fsym->arity == mc->arg_count && fsym->param_full &&
                            mc->arg_count <= LAMO_MAX_BIND_ARGS) {
                            AnnSubstMap tmap;
                            tmap.names = fsym->tp_names;
                            tmap.values = malloc(sizeof(const char*) * (size_t)(fsym->tp_count > 0 ? fsym->tp_count : 1));
                            tmap.count = fsym->tp_count;
                            /* 2.6.0 (FU3): explicit type arguments fill
                             * positions first (RFC §4.5) — `col.pick<int>(3, 4)`
                             * now validates exactly like the plain-call form
                             * `pick<int>(3, 4)`. */
                            if (mc->type_arg_count > 0 && mc->type_arg_count != fsym->tp_count) {
                                char message[300];
                                snprintf(message, sizeof(message),
                                         "module member '%s.%s' expects %d type argument(s), got %d",
                                         alias, mc->member_name, fsym->tp_count, mc->type_arg_count);
                                semantic_error_at(ctx, node->line, node->column, message);
                            }
                            for (int i = 0; i < fsym->tp_count; i++) {
                                if (mc->type_args && i < mc->type_arg_count && mc->type_args[i]) {
                                    char* n = semantic_normalize_type(mc->type_args[i]);
                                    tmap.values[i] = lamo_intern_type(n);
                                    free(n);
                                } else {
                                    tmap.values[i] = NULL;
                                }
                            }
                            const char* argf[LAMO_MAX_BIND_ARGS] = {0};
                            for (int i = 0; i < mc->arg_count; i++) {
                                semantic_infer_expression(ctx, mc->args[i]);
                                argf[i] = arg_concrete_full_type(ctx, mc->args[i]);
                            }
                            int mismatch = 0;
                            for (int i = 0; i < mc->arg_count; i++) {
                                const char* pat = fsym->param_full[i];
                                if (!pat || !argf[i]) continue;
                                int r = ann_bind_pattern(pat, argf[i], &tmap);
                                if (r == 0) {
                                    mismatch = 1;
                                    char message[300];
                                    snprintf(message, sizeof(message),
                                             "argument %d to '%s.%s': expected type '%s', got '%s'",
                                             i + 1, alias, mc->member_name, pat, argf[i]);
                                    semantic_error_at(ctx, node->line, node->column, message);
                                }
                            }
                            (void)mismatch;
                            if (fsym->ret_full) {
                                char* sub = ann_subst(fsym->ret_full, &tmap);
                                if (sub) {
                                    node->sema_full_type = lamo_intern_type(sub);
                                    /* ── 2.6.0 FU2: module-boundary type flow ──
                                     * When the substituted return type names a
                                     * declared struct, the call RESULT is
                                     * struct-typed: annotate the node so field
                                     * access (`p.x`) and method calls
                                     * (`p.norm()`) work in the importing file
                                     * exactly like local returns (SPEC §5.5,
                                     * §10.2). Previously the result erased to
                                     * an opaque unknown/array-ish value and
                                     * field access silently produced 0. */
                                    {
                                        char head[64];
                                        ann_head(sub, head, sizeof(head));
                                        if (find_struct_def(ctx, head)) {
                                            node->sema_struct_name = lamo_intern_type(head);
                                            free(sub);
                                            free(tmap.values);
                                            return LAMO_TYPE_STRUCT;
                                        }
                                    }
                                    free(sub);
                                }
                            }
                            free(tmap.values);
                            return LAMO_TYPE_UNKNOWN;
                        }
                    }
                }
                /* Member is a module global variable, or the renamed fn
                 * declaration has no signature info — visit args and
                 * stay type-unknown (legacy behavior). */
                for (int i = 0; i < mc->arg_count; i++) {
                    semantic_infer_expression(ctx, mc->args[i]);
                }
                return LAMO_TYPE_UNKNOWN;
            }
            /* Not a module alias; fall through to value-method-call. */
            /* Phase 2: value method call. Infer the object's type. */
            LamoType obj_type = semantic_infer_expression(ctx, mc->object);
            const char* obj_struct_name = NULL;
            if (mc->object->type == AST_IDENTIFIER) {
                Symbol* sym = scope_find(ctx->current_scope, ((ASTIdentifier*)mc->object)->name);
                if (sym && sym->kind == SYMBOL_VAR) {
                    obj_struct_name = sym->struct_name;
                }
            }
            /* 2.6.0 FU2: chains on module-returned structs —
             * `lib.make_point(1, 2).norm()`. The inner module call is not
             * an identifier, but its (now propagated) full type still
             * identifies the struct. Mirrors the AST_PROP_EXPR fallback
             * (Generics PR 2 §5.3). */
            if (!obj_struct_name && mc->object->sema_full_type) {
                char fh[64];
                ann_head(mc->object->sema_full_type, fh, sizeof(fh));
                if (find_struct_def(ctx, fh)) {
                    obj_struct_name = lamo_intern_type(fh);
                    obj_type = LAMO_TYPE_STRUCT;
                }
            }
            /* 2.10.0 traits: DICTIONARY DISPATCH for method calls on
             * TYPE-PARAMETER receivers (`s.area()` where `s: T` and
             * `T: Shape` of the enclosing generic fn). Generics compile
             * ONCE under erasure, so the receiver has no concrete struct
             * name to dispatch through (RFC §12.4 keeps dispatch static;
             * 2.9.0 rejected these calls outright). Instead, when the
             * type parameter's FN-LEVEL constraint is a declared trait,
             * the call resolves through the trait's signature: arity and
             * argument compatibility are checked HERE, the node is
             * stamped (sema_trait_name + sema_tp_receiver) so codegen
             * emits `_dict_T->method(...)` through the hidden dictionary
             * parameter, and the trait's return annotation flows into
             * the call's full type. Catalogue constraints (Ord, Eq, ...)
             * and unconstrained parameters keep the honest diagnostics —
             * they carry no methods. */
            {
                const char* recv_full = NULL;
                if (mc->object->type == AST_IDENTIFIER) {
                    Symbol* rsym = scope_find(ctx->current_scope, ((ASTIdentifier*)mc->object)->name);
                    if (rsym && rsym->kind == SYMBOL_VAR) recv_full = rsym->full_type;
                }
                if (!recv_full) recv_full = mc->object->sema_full_type;
                if (recv_full) {
                    int recv_tp_idx = -1;
                    int scope_tp_count = ctx->cur_fn_tp_count + ctx->impl_tp_count;
                    for (int t = 0; t < scope_tp_count; t++) {
                        const char* tpn = t < ctx->cur_fn_tp_count
                            ? ctx->cur_fn_tp_names[t]
                            : ctx->impl_tp_names[t - ctx->cur_fn_tp_count];
                        if (tpn && strcmp(recv_full, tpn) == 0) { recv_tp_idx = t; break; }
                    }
                    if (recv_tp_idx >= 0) {
                        /* Visit args first so concrete argument types
                         * exist for the compatibility check below. */
                        for (int i = 0; i < mc->arg_count; i++) {
                            semantic_infer_expression(ctx, mc->args[i]);
                        }
                        /* Dispatch rides FN-LEVEL constraints only:
                         * codegen adds hidden dictionary parameters to
                         * generic fns, and impl type parameters carry no
                         * constraints in the current grammar. */
                        const char* con = recv_tp_idx < ctx->cur_fn_tp_count
                            ? (ctx->cur_fn_tp_constraints
                               ? ctx->cur_fn_tp_constraints[recv_tp_idx] : NULL)
                            : NULL;
                        ASTTraitDecl* td = NULL;
                        if (con && lamo_constraint_kind(con) < 0) {
                            td = find_trait_def(ctx, con);
                        }
                        if (!td) {
                            /* Non-dispatchable receiver: keep the honest
                             * diagnostics, with constraint-aware wording. */
                            char message[320];
                            char hint[240];
                            if (con && lamo_constraint_kind(con) > 0) {
                                snprintf(message, sizeof(message),
                                         "cannot call method '%s' on a value of constrained type parameter '%s' (built-in constraint '%s' carries no methods)",
                                         mc->member_name, recv_full, con);
                                snprintf(hint, sizeof(hint),
                                         "built-in constraints catalogue structural properties (Eq, Ord, Num, ...), not methods; declare a trait with the method and constrain '%s' with it",
                                         recv_full);
                            } else if (con) {
                                /* Unknown constraint name — already a
                                 * declaration-site error; don't double-
                                 * report the name, keep the generic note. */
                                snprintf(message, sizeof(message),
                                         "cannot call method '%s' on a type-parameter value ('%s' is a generic parameter here)",
                                         mc->member_name, recv_full);
                                snprintf(hint, sizeof(hint),
                                         "constrain '%s' with a declared trait to dispatch through it",
                                         recv_full);
                            } else {
                                snprintf(message, sizeof(message),
                                         "cannot call method '%s' on a type-parameter value ('%s' is an unconstrained generic parameter; generics are erased at compile time)",
                                         mc->member_name, recv_full);
                                snprintf(hint, sizeof(hint),
                                         "constrain '%s' with a declared trait (e.g. `fn f<%s: Shape>(...)`) to dispatch through the trait's methods",
                                         recv_full, recv_full);
                            }
                            semantic_error_at_hint(ctx, node->line, node->column, message, hint);
                            return LAMO_TYPE_UNKNOWN;
                        }
                        /* Find the method signature in the trait. */
                        ASTFnDecl* sig = NULL;
                        for (ASTNode* m = td->methods; m; m = m->next) {
                            if (m->type == AST_FN_DECL &&
                                strcmp(((ASTFnDecl*)m)->name, mc->member_name) == 0) {
                                sig = (ASTFnDecl*)m;
                                break;
                            }
                        }
                        if (!sig) {
                            char message[320];
                            char hint[240];
                            snprintf(message, sizeof(message),
                                     "trait '%s' (constraint of type parameter '%s') has no method '%s'",
                                     td->name, recv_full, mc->member_name);
                            snprintf(hint, sizeof(hint),
                                     "declare `fn %s` in the trait, or call a method the trait provides",
                                     mc->member_name);
                            semantic_error_at_hint(ctx, node->line, node->column, message, hint);
                            return LAMO_TYPE_UNKNOWN;
                        }
                        if (sig->param_count != mc->arg_count) {
                            char message[320];
                            snprintf(message, sizeof(message),
                                     "method '%s.%s' (trait '%s') expects %d argument(s), got %d",
                                     recv_full, mc->member_name, td->name,
                                     sig->param_count, mc->arg_count);
                            semantic_error_at(ctx, node->line, node->column, message);
                            return LAMO_TYPE_UNKNOWN;
                        }
                        /* Argument compatibility where BOTH sides
                         * annotate: the trait signature's annotation
                         * against the argument's concrete full type.
                         * Arguments that are themselves type parameters
                         * are unresolvable here and skipped. */
                        for (int i = 0; i < mc->arg_count; i++) {
                            const char* raw_ann = sig->param_types ? sig->param_types[i] : NULL;
                            if (!raw_ann) continue;
                            const char* actual = arg_concrete_full_type(ctx, mc->args[i]);
                            if (!actual) continue;
                            int arg_is_tp = 0;
                            for (int t = 0; t < ctx->cur_fn_tp_count; t++) {
                                if (strcmp(actual, ctx->cur_fn_tp_names[t]) == 0) { arg_is_tp = 1; break; }
                            }
                            if (arg_is_tp) continue;
                            char* ann_norm = semantic_normalize_type(raw_ann);
                            const char* ann_interned = lamo_intern_type(ann_norm);
                            LamoType ann_base = annotation_to_type_with_ctx(ctx, ann_norm);
                            LamoType arg_base = semantic_infer_expression(ctx, mc->args[i]);
                            if (!call_types_compatible(ann_interned, ann_base,
                                                       actual, arg_base)) {
                                char message[320];
                                snprintf(message, sizeof(message),
                                         "argument %d to '%s.%s' (trait '%s'): expected type '%s', got '%s'",
                                         i + 1, recv_full, mc->member_name,
                                         td->name, ann_interned, actual);
                                semantic_error_at(ctx, node->line, node->column, message);
                            }
                            free(ann_norm);
                        }
                        /* Stamp the dispatch so codegen routes through
                         * the hidden dictionary parameter. */
                        node->sema_trait_name = td->name;
                        node->sema_tp_receiver = ctx->cur_fn_tp_names[recv_tp_idx];
                        /* Trait return annotation flows into the call's
                         * full type (traits are non-generic, so there is
                         * nothing to substitute). */
                        if (sig->return_type_annotation) {
                            char* ret_norm = semantic_normalize_type(sig->return_type_annotation);
                            node->sema_full_type = lamo_intern_type(ret_norm);
                            LamoType ret_base = annotation_to_type_with_ctx(ctx, ret_norm);
                            free(ret_norm);
                            return ret_base;
                        }
                        return LAMO_TYPE_UNKNOWN;
                    }
                }
            }
            if (obj_type == LAMO_TYPE_ARRAY || (obj_type == LAMO_TYPE_UNKNOWN && !obj_struct_name)) {
                /* Array method call: .push, .pop, .len. */
                if (strcmp(mc->member_name, "push") == 0) {
                    if (mc->arg_count != 1) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "array method `push` expects 1 argument, got %d", mc->arg_count);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                } else if (strcmp(mc->member_name, "pop") == 0) {
                    if (mc->arg_count != 0) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "array method `pop` expects 0 arguments, got %d", mc->arg_count);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                } else if (strcmp(mc->member_name, "len") == 0) {
                    if (mc->arg_count != 0) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "array method `len` expects 0 arguments, got %d", mc->arg_count);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                } else {
                    /* Phase 3 (stdlib): when the object's type is UNKNOWN
                     * (e.g. returned by a module call we can't statically
                     * resolve), don't error on unknown methods — the actual
                     * struct method dispatch happens at runtime via the
                     * codegen. Only error if we KNOW it's an array. */
                    if (obj_type == LAMO_TYPE_ARRAY) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "arrays have no method '%s' (valid: push, pop, len)",
                                 mc->member_name);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                    /* For UNKNOWN objects, accept and let runtime/codegen
                     * handle it. Mark the node so codegen knows to attempt
                     * struct method dispatch. */
                }
                /* Visit args. */
                for (int i = 0; i < mc->arg_count; i++) {
                    semantic_infer_expression(ctx, mc->args[i]);
                }
                /* Mark the resolution outcome so CODEGEN picks the array-
                 * builtin route even when the receiver chain LOOKS like a
                 * struct field access (self.items.push on generic impls). */
                node->sema_full_type = lamo_intern_type("array");
                node->sema_struct_name = NULL;
                /* Return type: push/pop return int (or the popped value's
                 * type for pop, but we conservatively say UNKNOWN); len
                 * returns int. */
                if (strcmp(mc->member_name, "len") == 0) { return LAMO_TYPE_INT; }
                return LAMO_TYPE_UNKNOWN;
            }
            if (obj_type == LAMO_TYPE_STRUCT && obj_struct_name) {
                /* Struct method call. */
                ASTFnDecl* method = find_method(ctx, obj_struct_name, mc->member_name);
                if (!method) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "struct '%s' has no method '%s'",
                             obj_struct_name, mc->member_name);
                    semantic_error_at(ctx, node->line, node->column, message);
                } else {
                    /* Validate arity: method's param_count + 1 (for self)
                     * should equal arg_count + 1 = the actual number of
                     * values we'll pass (self + args). So args should
                     * equal method->param_count. */
                    if (method->param_count != mc->arg_count) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "method '%s.%s' expects %d argument(s), got %d",
                                 obj_struct_name, mc->member_name, method->param_count, mc->arg_count);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                }
                /* Annotate the AST node so codegen knows the struct type. */
                node->sema_struct_name = obj_struct_name;
                /* Also annotate the object identifier for codegen. */
                mc->object->sema_struct_name = obj_struct_name;
                for (int i = 0; i < mc->arg_count; i++) {
                    semantic_infer_expression(ctx, mc->args[i]);
                }
                /* Generics PR 2 §4.4: full typed dispatch when the
                 * receiver carries an instantiation (annotated vars).
                 * Runs AFTER arg inference so concrete types exist. */
                check_struct_method_call(ctx, mc, obj_struct_name, node,
                                         node->line, node->column);
                return LAMO_TYPE_UNKNOWN;  /* method return type unknown */
            }
            /* Object is not array, not struct, not module. */
            {
                char message[256];
                snprintf(message, sizeof(message),
                         "cannot call method '%s' on value of type '%s' (only arrays and structs have methods)",
                         mc->member_name, type_name(obj_type));
                semantic_error_at(ctx, node->line, node->column, message);
                for (int i = 0; i < mc->arg_count; i++) {
                    semantic_infer_expression(ctx, mc->args[i]);
                }
                return LAMO_TYPE_UNKNOWN;
            }
        }
        case AST_GROUPING_EXPR:
            return semantic_infer_expression(ctx, ((ASTGroupingExpr*)node)->expression);
        /* ─── Phase 2: composite expression types ───────────────────── */
        case AST_ARRAY_LITERAL: {
            ASTArrayLiteral* arr = (ASTArrayLiteral*)node;
            for (int i = 0; i < arr->element_count; i++) {
                semantic_infer_expression(ctx, arr->elements[i]);
            }
            return LAMO_TYPE_ARRAY;
        }
        case AST_INDEX_EXPR: {
            ASTIndexExpr* ie = (ASTIndexExpr*)node;
            LamoType arr_type = semantic_infer_expression(ctx, ie->array);
            semantic_infer_expression(ctx, ie->index);
            /* If the array is a struct, indexing doesn't make sense. */
            if (arr_type == LAMO_TYPE_STRUCT) {
                semantic_error_at(ctx, node->line, node->column,
                                  "cannot index into a struct value (use .field access instead)");
            }
            /* Indexing an array returns the element type, which we can't
             * know statically (arrays are heterogeneous). Return UNKNOWN. */
            return LAMO_TYPE_UNKNOWN;
        }
        case AST_PROP_EXPR: {
            ASTPropExpr* pe = (ASTPropExpr*)node;
            /* Phase 3 (stdlib): if the object is a registered module alias
             * and the property is a registered member (variable), accept
             * the access. This is what makes `math.PI` work after
             * `import std.math as math`. The codegen will emit a reference
             * to the prefixed global `lamo_mod_<alias>__<member>`. */
            if (pe->object && pe->object->type == AST_IDENTIFIER) {
                const char* alias = ((ASTIdentifier*)pe->object)->name;
                if (ctx->module_resolve && ctx->module_resolve(alias, pe->prop_name, ctx->module_user_data)) {
                    /* 2.6.0 (FU5): §10.6 step-1 export warning. */
                    semantic_error_module_export(ctx, alias, pe->prop_name,
                                                node->line, node->column);
                    /* It's a module variable. Mark the node so codegen can
                     * find the alias without re-doing the lookup. We store
                     * the alias on sema_struct_name (a string ptr field
                     * already on every AST node) as a side channel — the
                     * codegen will recognize this convention for prop_expr
                     * nodes only. */
                    node->sema_struct_name = alias;
                    return LAMO_TYPE_UNKNOWN;
                }
            }
            LamoType obj_type = semantic_infer_expression(ctx, pe->object);
            const char* obj_struct_name = NULL;
            if (pe->object->type == AST_IDENTIFIER) {
                Symbol* sym = scope_find(ctx->current_scope, ((ASTIdentifier*)pe->object)->name);
                if (sym && sym->kind == SYMBOL_VAR) {
                    obj_struct_name = sym->struct_name;
                }
            }
            /* Case 1: array.len (existing behavior). */
            if (obj_type == LAMO_TYPE_ARRAY || (obj_type == LAMO_TYPE_UNKNOWN && !obj_struct_name)) {
                if (strcmp(pe->prop_name, "len") == 0) {
                    /* 2.10.0 (FU-vmc): route marker for codegen — the
                     * same `sema_full_type == "array"` convention member
                     * calls already use. Chained receivers (`self.items.len`,
                     * `b.items.len`) carry a struct tag on the object from
                     * the inner field access; without this stamp codegen
                     * takes the struct-field route and emits the defensive
                     * lamo_make_int(0) fallback. */
                    node->sema_full_type = lamo_intern_type("array");
                    return LAMO_TYPE_INT;
                }
                /* Unknown property on an array/unknown-typed value. If the
                 * object is unknown, don't error (could be a module alias
                 * that's checked elsewhere). If it's an array, error. */
                if (obj_type == LAMO_TYPE_ARRAY) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "arrays have no property '%s' (did you mean .len?)",
                             pe->prop_name);
                    semantic_error_at(ctx, node->line, node->column, message);
                }
                return LAMO_TYPE_UNKNOWN;
            }
            /* Case 2: struct field access. Generics PR 2: when direct
             * symbol typing is unavailable (module fn result etc.), the
             * substituted full type stashed by the caller machinery
             * (sema_full_type) still identifies the struct. */
            if (!obj_struct_name && pe->object->sema_full_type) {
                char fh[64];
                ann_head(pe->object->sema_full_type, fh, sizeof(fh));
                if (find_struct_def(ctx, fh)) {
                    obj_type = LAMO_TYPE_STRUCT;
                    obj_struct_name = lamo_intern_type(fh);
                    node->sema_full_type = pe->object->sema_full_type;
                }
            }
            if (obj_type == LAMO_TYPE_STRUCT && obj_struct_name) {
                ASTStructDecl* sd = find_struct_def(ctx, obj_struct_name);
                int idx = struct_field_index(sd, pe->prop_name);
                if (idx < 0) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "struct '%s' has no field '%s'",
                             obj_struct_name, pe->prop_name);
                    semantic_error_at(ctx, node->line, node->column, message);
                }
                /* Annotate the AST node so codegen knows the struct type
                 * and can look up the field index. */
                node->sema_struct_name = obj_struct_name;
                pe->object->sema_struct_name = obj_struct_name;
                /* Field type is UNKNOWN (we don't track per-field types
                 * yet). Return UNKNOWN. */
                return LAMO_TYPE_UNKNOWN;
            }
            /* Object is a string, int, etc. - no properties. */
            {
                char message[256];
                snprintf(message, sizeof(message),
                         "value of type '%s' has no property '%s'",
                         type_name(obj_type), pe->prop_name);
                semantic_error_at(ctx, node->line, node->column, message);
                return LAMO_TYPE_UNKNOWN;
            }
        }
        case AST_STRUCT_LITERAL: {
            ASTStructLiteral* sl = (ASTStructLiteral*)node;
            ASTStructDecl* sd = find_struct_def(ctx, sl->struct_name);
            if (!sd) {
                char message[256];
                snprintf(message, sizeof(message),
                         "unknown struct type '%s' (declare it with `struct %s { ... }` first)",
                         sl->struct_name, sl->struct_name);
                semantic_error_at(ctx, node->line, node->column, message);
                /* Still visit field values for cascading errors. */
                for (int i = 0; i < sl->field_count; i++) {
                    semantic_infer_expression(ctx, sl->field_values[i]);
                }
                return LAMO_TYPE_UNKNOWN;
            }
            /* Generics PR 1: validate type argument count.
             *   - If the struct is generic, type_arg_count must equal type_param_count.
             *   - If the struct is non-generic, type_arg_count must be 0.
             * We also validate that each type arg is a known type (builtin
             * or declared struct). Type parameters as type args (i.e.,
             * using one generic struct inside another generic struct's
             * field) is future work — for PR 1 we only support concrete
             * instantiations. */
            if (sd->type_param_count > 0) {
                if (sl->type_arg_count != sd->type_param_count) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "struct '%s' expects %d type argument(s) <%s>, but %d were provided",
                             sl->struct_name, sd->type_param_count,
                             sd->type_param_count > 0 ? sd->type_params[0] : "",
                             sl->type_arg_count);
                    semantic_error_at(ctx, node->line, node->column, message);
                }
            } else {
                if (sl->type_arg_count > 0) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "struct '%s' is not generic; remove the '<...>' type arguments",
                             sl->struct_name);
                    semantic_error_at(ctx, node->line, node->column, message);
                }
            }
            /* Validate each type arg is a known type (builtin, struct,
             * or an in-scope TYPE PARAMETER — Generics PR 2 §5.2 makes
             * `Option<T> { ... }` legal inside generic fn bodies). */
            for (int i = 0; i < sl->type_arg_count; i++) {
                const char* ta = sl->type_args[i];
                if (!ta) continue;
                if (strcmp(ta, "int") == 0 || strcmp(ta, "float") == 0 ||
                    strcmp(ta, "bool") == 0 || strcmp(ta, "string") == 0 ||
                    strcmp(ta, "array") == 0) {
                    continue;
                }
                {
                    char ta_head[64];
                    ann_head(ta, ta_head, sizeof(ta_head));
                    int nested_ok = 0;
                    if (strchr(ta, '<') != NULL && lamo_validate_annotation_tree(ctx, NULL, ta)) {
                        nested_ok = 1;
                    }
                    int tp_ok = 0;
                    for (int t = 0; t < ctx->cur_fn_tp_count; t++) {
                        if (strcmp(ta_head, ctx->cur_fn_tp_names[t]) == 0) { tp_ok = 1; break; }
                    }
                    if (!tp_ok) {
                        for (int t = 0; t < ctx->impl_tp_count; t++) {
                            if (strcmp(ta_head, ctx->impl_tp_names[t]) == 0) { tp_ok = 1; break; }
                        }
                    }
                    if (nested_ok || tp_ok || find_struct_def(ctx, ta_head)) continue;
                }
                {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "type argument '%s' in struct literal '%s' is not a known type (expected a builtin, a declared struct, or an in-scope type parameter)",
                             ta, sl->struct_name);
                    semantic_error_at(ctx, node->line, node->column, message);
                }
            }
            /* Validate each field name exists in the struct. */
            for (int i = 0; i < sl->field_count; i++) {
                int idx = struct_field_index(sd, sl->field_names[i]);
                if (idx < 0) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "struct '%s' has no field '%s'",
                             sl->struct_name, sl->field_names[i]);
                    semantic_error_at(ctx, node->line, node->column, message);
                }
                semantic_infer_expression(ctx, sl->field_values[i]);
            }
            /* Check that all struct fields are covered (warning, not error). */
            if (sl->field_count < sd->field_count) {
                /* Find a missing field and report it. */
                for (int i = 0; i < sd->field_count; i++) {
                    int found = 0;
                    for (int j = 0; j < sl->field_count; j++) {
                        if (strcmp(sd->field_names[i], sl->field_names[j]) == 0) {
                            found = 1; break;
                        }
                    }
                    if (!found) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "warning: struct '%s' field '%s' not set in literal (defaults to 0)",
                                 sl->struct_name, sd->field_names[i]);
                        semantic_error_at(ctx, node->line, node->column, message);
                    }
                }
            }
            /* Annotate the AST node so codegen knows the struct type. */
            node->sema_struct_name = sl->struct_name;
            return LAMO_TYPE_STRUCT;
        }
        case AST_MATCH_STMT:
            /* 2.8.0 (FU4): match in EXPRESSION position — `let x = match
             * ... { ... }`, `return match ...`, `print(match ...)`.
             * Validation is shared with the statement form
             * (semantic_validate_match routes through
             * semantic_visit_statement); the expression's value is the
             * least upper bound of the arm body types. */
            return semantic_validate_match(ctx, (ASTMatchStmt*)node, node);
        default:
            // Recurse into statement-shaped nodes that can appear inside
            // expressions via legacy AST types we still keep for compat.
            semantic_visit_statement(ctx, node);
            return LAMO_TYPE_UNKNOWN;
    }
}

int semantic_analyze_with_source_lookup(ASTProgram* program, const char* file_path,
                                        LamoSourceLookupFn lookup, void* user_data) {
    /* Delegate to the full entry point with NULL module callbacks —
     * keeps the Sprint 3 signature compatible. */
    return semantic_analyze_full(program, file_path, lookup, user_data,
                                  NULL, NULL, NULL, NULL);
}

/* 2.6.0 (FU5) / 2.7.0 (FU2-ledger): §10.6 explicit-export two-step
 * rollout — STEP 2 (ENFORCED as of 2.7.0). Non-`pub` members reached
 * through a module alias are now a compile ERROR: the boundary promised
 * in step 1 ("it will become private in a future release") is real.
 * The migration stays strictly mechanical: add `pub` where the error
 * points. The once-per-member flag is kept so a member used several
 * times reports at its FIRST use only, mirroring the step-1 warning. */
static void semantic_error_module_export(SemanticContext* ctx,
                                         const char* alias, const char* member,
                                         int line, int column) {
    LamoModuleMember* mem;
    if (!ctx->module_registry || !alias || !member) return;
    mem = lamo_modules_find_member(ctx->module_registry, alias, member);
    if (!mem || mem->is_pub || mem->warned_not_pub) return;
    mem->warned_not_pub = 1;  /* one diagnostic per member */
    {
        char message[300];
        char hint[200];
        snprintf(message, sizeof(message),
                 "member '%s' of module '%s' is not marked 'pub' and cannot be accessed through the module alias (§10.6 step 2, enforced in 2.7.0)",
                 member, alias);
        snprintf(hint, sizeof(hint),
                 "add 'pub' to the declaration of '%s' in the module file to export it explicitly",
                 member);
        semantic_error_at_hint(ctx, line, column, message, hint);
    }
}

/* Sprint 4: full entry point with module-resolution callbacks. The
 * source-lookup and module callbacks may all be NULL; the semantic pass
 * just skips the corresponding features. */
int semantic_analyze_full(ASTProgram* program, const char* file_path,
                          LamoSourceLookupFn src_lookup, void* src_user_data,
                          LamoModuleResolveFn mod_resolve,
                          LamoModuleArityFn mod_arity,
                          void* mod_user_data,
                          struct LamoModuleRegistry* mod_registry) {
    SemanticContext ctx;
    ctx.file_path = file_path;
    ctx.last_node_path = NULL;  // Bug #5 fix: preenchido por node->file_path
    ctx.current_scope = scope_push(NULL);
    ctx.inside_function = 0;
    ctx.inside_loop = 0;        // Próximo passo 5: break/continue tracking
    ctx.errors = 0;
    /* Sprint 3: source lookup for error snippets. May be NULL — in that
     * case semantic_error_at just omits the snippet. */
    ctx.source_lookup = src_lookup;
    ctx.source_lookup_user_data = src_user_data;
    /* Sprint 4: module-resolution callbacks. May be NULL — in that case
     * AST_MEMBER_CALL nodes always error. */
    ctx.module_resolve = mod_resolve;
    ctx.module_arity = mod_arity;
    ctx.module_user_data = mod_user_data;
    ctx.module_registry = mod_registry;
    /* Return-type tracking: UNKNOWN at top level (not inside any function). */
    ctx.current_fn_return_type = LAMO_TYPE_UNKNOWN;
    ctx.current_fn_name = NULL;
    /* Phase 2: initialize struct/enum/impl registries. We point them at
     * program->declarations and walk that list, filtering by node type,
     * in the lookup helpers (find_struct_def, find_enum_def, find_method).
     * This avoids the need for separate linked lists (and the resulting
     * corruption of node->next). */
    ctx.struct_defs = program->declarations;
    ctx.enum_defs = program->declarations;
    ctx.impl_defs = program->declarations;
    ctx.trait_defs = program->declarations;
    ctx.current_impl_struct = NULL;
    /* Generics PR 2: enclosing-impl type parameters (RFC §4.4). MUST be
     * initialized here — every annotated parameter reads this list, and
     * leaving it as stack garbage crashed optimized builds (-O1/-O2). */
    ctx.impl_tp_names = NULL;
    ctx.impl_tp_count = 0;
    ctx.cur_fn_tp_names = NULL;
    ctx.cur_fn_tp_count = 0;

    /* Phase 2: register enum variants as global int constants. Each
     * variant becomes a SYMBOL_VAR with type INT and a known value (its
     * index). The codegen emits these as global LamoValue variables.
     * 2.6.0: variants of TAGGED-union enums register as LAMO_TYPE_ENUM
     * instead — their runtime representation is a tagged LamoValue, not
     * a plain int (SPEC §3.5). Unit variants of tagged enums are still
     * usable as bare identifiers (they are values); payload variants
     * only through constructor calls, enforced at the identifier site.
     *
     * 2.7.0 (FU4): variant-name collisions ACROSS ENUMS are now legal
     * ("later wins", SPEC §3.5) — the symbol is retargeted to the later
     * enum's variant and a one-time shadowing warning points at the
     * `Enum::Variant` qualification that disambiguates. Collisions with
     * USER variables/functions remain hard errors. */
    {
        /* Track registered variant names so an enum-vs-enum collision
         * can be distinguished from an enum-vs-user-declaration one.
         * Parallel arrays: name / owning enum. */
        char** rv_names = NULL;
        const char** rv_enums = NULL;
        int rv_count = 0;
        int rv_cap = 0;
        for (ASTNode* node = program->declarations; node; node = node->next) {
            if (node->type == AST_ENUM_DECL) {
                ASTEnumDecl* ed = (ASTEnumDecl*)node;
                LamoType variant_type = enum_decl_is_tagged(ed) ? LAMO_TYPE_ENUM : LAMO_TYPE_INT;
                for (int i = 0; i < ed->variant_count; i++) {
                    const char* vname = ed->variants[i];
                    Symbol* existing = scope_find_in_current(ctx.current_scope, vname);
                    int prev_is_variant = 0;
                    const char* prev_enum = NULL;
                    for (int r = 0; r < rv_count; r++) {
                        if (strcmp(rv_names[r], vname) == 0) {
                            prev_is_variant = 1;
                            prev_enum = rv_enums[r];
                            break;
                        }
                    }
                    if (existing && prev_is_variant) {
                        /* Later-wins: retarget the symbol to this enum's
                         * variant and warn once per shadowed name. */
                        existing->type = variant_type;
                        char message[320];
                        snprintf(message, sizeof(message),
                                 "variant '%s' of enum '%s' shadows variant '%s' of enum '%s'; bare '%s' now refers to the later declaration (disambiguate with %s::%s or %s::%s)",
                                 vname, ed->name, vname, prev_enum,
                                 vname, ed->name, vname, prev_enum, vname);
                        semantic_warn_at(&ctx, node->line, node->column, message);
                    } else {
                        /* First registration (or collision with a user
                         * declaration — scope_define reports that). */
                        scope_define(&ctx, ctx.current_scope, vname, SYMBOL_VAR, 0, variant_type, node->line, node->column, node->file_path);
                    }
                    /* Record in the shadow-tracking table. */
                    if (rv_count == rv_cap) {
                        int new_cap = rv_cap > 0 ? rv_cap * 2 : 16;
                        char** n_names = realloc(rv_names, sizeof(char*) * (size_t)new_cap);
                        const char** n_enums = realloc(rv_enums, sizeof(const char*) * (size_t)new_cap);
                        if (n_names && n_enums) {
                            rv_names = n_names; rv_enums = n_enums;
                            rv_cap = new_cap;
                        } else {
                            /* OOM: keep compiling without shadow tracking. */
                            if (n_names) rv_names = n_names;
                            if (n_enums) rv_enums = n_enums;
                            break;
                        }
                    }
                    rv_names[rv_count] = strdup(vname);
                    rv_enums[rv_count] = ed->name;
                    rv_count++;
                }
            }
        }
        for (int r = 0; r < rv_count; r++) free(rv_names[r]);
        free(rv_names); free(rv_enums);
    }

    for (ASTNode* node = program->declarations; node; node = node->next) {
        if (node->type == AST_FN_DECL) {
            ASTFnDecl* fn_decl = (ASTFnDecl*)node;
            /* Sprint 3: if the function has a return-type annotation, use
             * it as the inferred return type. Otherwise leave UNKNOWN
             * (the call site will infer from the call context, or stay
             * UNKNOWN if there's no context).
             *
             * Generics PR 2/PR 6 additions:
             *   - type parameter lists are validated: duplicates and
             *     unknown constraint names are hard errors;
             *   - RFC §4.3 rule enforced: a GENERIC fn must annotate
             *     every parameter AND its return type;
             *   - every annotation resolves through the FULL resolver so
             *     nested generics ("array<T>", "Pair<int,string>") work,
             *     with normalized interned strings stored on the symbol
             *     for call-site binding. `void` is now a legal return. */
            LamoType ret_type = LAMO_TYPE_UNKNOWN;

            /* Type-parameter sanity. */
            for (int i = 0; i < fn_decl->type_param_count; i++) {
                for (int j = i + 1; j < fn_decl->type_param_count; j++) {
                    if (strcmp(fn_decl->type_params[i], fn_decl->type_params[j]) == 0) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "duplicate type parameter '%s' in function '%s'",
                                 fn_decl->type_params[i], fn_decl->name);
                        semantic_error_at(&ctx, node->line, node->column, message);
                    }
                }
                const char* con = fn_decl->type_param_constraints
                                      ? fn_decl->type_param_constraints[i] : NULL;
                if (con && lamo_constraint_kind(con) < 0 && !find_trait_def(&ctx, con)) {
                    char message[320];
                    snprintf(message, sizeof(message),
                             "unknown constraint '%s' on type parameter '%s' of function '%s' (catalogue: Any, Eq, Ord, Num, Hash, Show — or a declared trait)",
                             con, fn_decl->type_params[i], fn_decl->name);
                    semantic_error_at(&ctx, node->line, node->column, message);
                }
            }

            int generic_fn = fn_decl->type_param_count > 0;
            if (generic_fn) {
                int missing = !fn_decl->return_type_annotation;
                for (int i = 0; i < fn_decl->param_count; i++) {
                    if (!fn_decl->param_types || !fn_decl->param_types[i]) missing = 1;
                }
                if (missing) {
                    char message[300];
                    snprintf(message, sizeof(message),
                             "generic function '%s' must annotate every parameter and its return type (RFC §4.3)",
                             fn_decl->name);
                    semantic_error_at(&ctx, node->line, node->column, message);
                }
            }

            const char* ret_full_norm = NULL;
            if (fn_decl->return_type_annotation) {
                ret_type = annotation_resolve_full(&ctx, fn_decl->return_type_annotation,
                                                   &ret_full_norm);
                if (ret_type == LAMO_TYPE_UNKNOWN) {
                    /* A declared TYPE PARAMETER is fine in return position. */
                    int is_tp = 0;
                    for (int t = 0; t < fn_decl->type_param_count; t++) {
                        if (strcmp(fn_decl->return_type_annotation, fn_decl->type_params[t]) == 0) { is_tp = 1; break; }
                    }
                    if (!is_tp) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "unknown return type annotation '%s' on function '%s' (expected int, float, string, bool, array<T>, void, a struct name, or a declared type parameter)",
                                 fn_decl->return_type_annotation, fn_decl->name);
                        semantic_error_at(&ctx, node->line, node->column, message);
                        ret_full_norm = NULL;
                    }
                }
            } else {
                ret_full_norm = NULL;
            }

            scope_define(&ctx, ctx.current_scope, fn_decl->name, SYMBOL_FN, fn_decl->param_count, ret_type, node->line, node->column, node->file_path);

            /* Attach the normalized signature to the freshly-defined
             * symbol so call sites can validate without re-resolving. */
            {
                Symbol* sym = scope_find_in_current(ctx.current_scope, fn_decl->name);
                if (sym) {
                    sym->ret_full = ret_full_norm;
                    sym->tp_count = generic_fn ? fn_decl->type_param_count : 0;
                    if (sym->tp_count > 0) {
                        sym->tp_names = malloc(sizeof(const char*) * (size_t)sym->tp_count);
                        sym->tp_constraints = malloc(sizeof(const char*) * (size_t)sym->tp_count);
                        if (!sym->tp_names || !sym->tp_constraints) {
                            perror("Failed to allocate generic signature arrays");
                            exit(EXIT_FAILURE);
                        }
                        for (int i = 0; i < sym->tp_count; i++) {
                            sym->tp_names[i] = lamo_intern_type(fn_decl->type_params[i]);
                            sym->tp_constraints[i] =
                                fn_decl->type_param_constraints && fn_decl->type_param_constraints[i]
                                    ? lamo_intern_type(fn_decl->type_param_constraints[i])
                                    : NULL;
                        }
                    }
                    if (fn_decl->param_count > 0) {
                        sym->param_full = malloc(sizeof(const char*) * (size_t)fn_decl->param_count);
                        if (!sym->param_full) {
                            perror("Failed to allocate param signature array");
                            exit(EXIT_FAILURE);
                        }
                        for (int i = 0; i < fn_decl->param_count; i++) {
                            sym->param_full[i] = NULL;
                            const char* raw = fn_decl->param_types ? fn_decl->param_types[i] : NULL;
                            if (raw) {
                                LamoType pk = LAMO_TYPE_UNKNOWN;
                                const char* norm = NULL;
                                pk = annotation_resolve_full(&ctx, raw, &norm);
                                int is_tp = 0;
                                for (int t = 0; t < sym->tp_count; t++) {
                                    char headcmp[64];
                                    ann_head(norm, headcmp, sizeof(headcmp));
                                    if (strcmp(headcmp, fn_decl->type_params[t]) == 0 ||
                                        strcmp(raw, fn_decl->type_params[t]) == 0) { is_tp = 1; break; }
                                }
                                if (pk == LAMO_TYPE_UNKNOWN && !is_tp) {
                                    char message[256];
                                    snprintf(message, sizeof(message),
                                             "unknown type annotation '%s' on parameter '%s' of function '%s' (expected int, float, string, bool, array<T>, void, a struct name, or a declared type parameter)",
                                             raw, fn_decl->params[i], fn_decl->name);
                                    semantic_error_at(&ctx, node->line, node->column, message);
                                    sym->param_full[i] = NULL;
                                } else {
                                    sym->param_full[i] = norm;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    for (ASTNode* node = program->declarations; node; node = node->next) {
        semantic_visit_statement(&ctx, node);
    }

    scope_free(ctx.current_scope);
    return ctx.errors == 0;
}

int semantic_analyze(ASTProgram* program, const char* file_path) {
    /* Backwards-compatible entry point: no source lookup, so error
     * messages will not include a source snippet. */
    return semantic_analyze_with_source_lookup(program, file_path, NULL, NULL);
}
