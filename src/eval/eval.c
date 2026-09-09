#include "eval.h"
#include "ast/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Value helpers ────────────────────────────────────────────────────── */

EvalValue eval_int(long long i)      { EvalValue v; v.type = EVAL_VAL_INT;    v.as.i = i; return v; }
EvalValue eval_float(double f)       { EvalValue v; v.type = EVAL_VAL_FLOAT;  v.as.f = f; return v; }
EvalValue eval_bool(int b)           { EvalValue v; v.type = EVAL_VAL_BOOL;   v.as.b = b; return v; }
EvalValue eval_void(void)            { EvalValue v; v.type = EVAL_VAL_VOID;   v.as.i = 0; return v; }
EvalValue eval_error(void)           { EvalValue v; v.type = EVAL_VAL_ERROR;  v.as.i = 0; return v; }

EvalValue eval_string(const char* s) {
    EvalValue v;
    v.type = EVAL_VAL_STRING;
    v.as.s = s ? strdup(s) : strdup("");
    return v;
}

EvalValue eval_string_take(char* s) {
    EvalValue v;
    v.type = EVAL_VAL_STRING;
    v.as.s = s;
    return v;
}

/* 2.8.0 (FU1): tagged-union enum value. Takes ownership of the payload
 * array (the caller stops using it) and strdup's the variant name so
 * callers may pass ring-buffer / literal strings. */
EvalValue eval_enum(long long tag, const char* variant_name,
                    EvalValue* payloads, int payload_count) {
    EvalValue v;
    v.type = EVAL_VAL_ENUM;
    v.as.e.tag = tag;
    v.as.e.variant_name = strdup(variant_name ? variant_name : "enum");
    v.as.e.payloads = payloads;
    v.as.e.payload_count = payloads ? payload_count : 0;
    return v;
}

/* ── 2.10.0 (FU-vmc): refcounted heap objects ─────────────────────── */

EvalValue eval_array_take(EvalArrayObj* obj) {
    EvalValue v;
    v.type = EVAL_VAL_ARRAY;
    v.as.arr = obj;
    return v;
}

EvalValue eval_struct_take(EvalStructObj* obj) {
    EvalValue v;
    v.type = EVAL_VAL_STRUCT;
    v.as.strct = obj;
    return v;
}

void eval_array_obj_unref(EvalArrayObj* obj) {
    if (!obj) return;
    if (--obj->refcount > 0) return;
    if (obj->items) {
        for (int i = 0; i < obj->count; i++) eval_value_free(obj->items[i]);
        free(obj->items);
    }
    free(obj);
}

void eval_struct_obj_unref(EvalStructObj* obj) {
    if (!obj) return;
    if (--obj->refcount > 0) return;
    if (obj->struct_name) free(obj->struct_name);
    if (obj->field_names) {
        for (int i = 0; i < obj->field_count; i++) {
            if (obj->field_names[i]) free(obj->field_names[i]);
        }
        free(obj->field_names);
    }
    if (obj->fields) {
        for (int i = 0; i < obj->field_count; i++) eval_value_free(obj->fields[i]);
        free(obj->fields);
    }
    free(obj);
}

void eval_value_free(EvalValue v) {
    if (v.type == EVAL_VAL_STRING && v.as.s) {
        free(v.as.s);
    } else if (v.type == EVAL_VAL_ENUM) {
        if (v.as.e.variant_name) free(v.as.e.variant_name);
        if (v.as.e.payloads) {
            for (int i = 0; i < v.as.e.payload_count; i++) {
                eval_value_free(v.as.e.payloads[i]);
            }
            free(v.as.e.payloads);
        }
    } else if (v.type == EVAL_VAL_ARRAY) {
        eval_array_obj_unref(v.as.arr);
    } else if (v.type == EVAL_VAL_STRUCT) {
        eval_struct_obj_unref(v.as.strct);
    }
}

/* 2.8.0 (FU1): deep copy. Used wherever a stored value escapes to a
 * caller that will free it independently (env lookups, match-binding
 * payloads out of the scrutinee). */
static EvalValue eval_value_clone(EvalValue v) {
    if (v.type == EVAL_VAL_STRING) return eval_string(v.as.s);
    if (v.type == EVAL_VAL_ARRAY) {
        /* Reference semantics (C-backend parity): the clone SHARES the
         * heap object, exactly like the pointer copy `let b = a;`
         * compiles to in the transpiled backend. */
        if (v.as.arr) v.as.arr->refcount++;
        return v;
    }
    if (v.type == EVAL_VAL_STRUCT) {
        if (v.as.strct) v.as.strct->refcount++;
        return v;
    }
    if (v.type == EVAL_VAL_ENUM) {
        EvalValue* payloads = NULL;
        if (v.as.e.payloads) {
            payloads = malloc(sizeof(EvalValue) * (size_t)(v.as.e.payload_count > 0 ? v.as.e.payload_count : 1));
            if (!payloads) { perror("eval_value_clone"); exit(1); }
            for (int i = 0; i < v.as.e.payload_count; i++) {
                payloads[i] = eval_value_clone(v.as.e.payloads[i]);
            }
        }
        return eval_enum(v.as.e.tag, v.as.e.variant_name, payloads,
                         v.as.e.payload_count);
    }
    return v;  /* int/float/bool/void/error are plain values */
}

char* eval_value_to_string(EvalValue v) {
    char buf[64];
    switch (v.type) {
        case EVAL_VAL_INT:
            snprintf(buf, sizeof(buf), "%lld", v.as.i);
            return strdup(buf);
        case EVAL_VAL_FLOAT:
            /* Match runtime: trim trailing zeros unless integral. */
            snprintf(buf, sizeof(buf), "%g", v.as.f);
            return strdup(buf);
        case EVAL_VAL_BOOL:
            return strdup(v.as.b ? "true" : "false");
        case EVAL_VAL_STRING:
            return strdup(v.as.s ? v.as.s : "");
        case EVAL_VAL_ENUM: {
            /* 2.8.0 (FU1): render like the C runtime — `Some(42)` /
             * `None` (lamo_value_to_owned_string, lamo_runtime.h).
             * Payloads render recursively. */
            size_t cap = 64, len = 0;
            char* out = malloc(cap);
            if (!out) return strdup("");
            out[0] = '\0';
            #define EVAL_APPEND_STR(s) do { \
                size_t addlen = strlen(s); \
                if (len + addlen + 1 > cap) { \
                    while (len + addlen + 1 > cap) cap *= 2; \
                    char* grown = realloc(out, cap); \
                    if (!grown) { free(out); return strdup(""); } \
                    out = grown; \
                } \
                memcpy(out + len, s, addlen); \
                len += addlen; \
                out[len] = '\0'; \
            } while (0)
            EVAL_APPEND_STR(v.as.e.variant_name ? v.as.e.variant_name : "enum");
            if (v.as.e.payloads && v.as.e.payload_count > 0) {
                EVAL_APPEND_STR("(");
                for (int i = 0; i < v.as.e.payload_count; i++) {
                    if (i > 0) EVAL_APPEND_STR(", ");
                    char* elem = eval_value_to_string(v.as.e.payloads[i]);
                    EVAL_APPEND_STR(elem ? elem : "");
                    free(elem);
                }
                EVAL_APPEND_STR(")");
            }
            #undef EVAL_APPEND_STR
            return out;
        }
        case EVAL_VAL_ARRAY: {
            /* Backend parity: Python-like [a, b, c] form, elements via
             * their own string representation (lamo_print_value). */
            size_t cap = 64, len = 0;
            char* out = malloc(cap);
            if (!out) return strdup("");
            out[0] = '\0';
            #define EVAL_APPEND_STR(s) do { \
                size_t addlen = strlen(s); \
                if (len + addlen + 1 > cap) { \
                    while (len + addlen + 1 > cap) cap *= 2; \
                    char* grown = realloc(out, cap); \
                    if (!grown) { free(out); return strdup(""); } \
                    out = grown; \
                } \
                memcpy(out + len, s, addlen); \
                len += addlen; \
                out[len] = '\0'; \
            } while (0)
            EVAL_APPEND_STR("[");
            if (v.as.arr) {
                for (int i = 0; i < v.as.arr->count; i++) {
                    if (i > 0) EVAL_APPEND_STR(", ");
                    char* elem = eval_value_to_string(v.as.arr->items[i]);
                    EVAL_APPEND_STR(elem ? elem : "");
                    free(elem);
                }
            }
            EVAL_APPEND_STR("]");
            #undef EVAL_APPEND_STR
            return out;
        }
        case EVAL_VAL_STRUCT: {
            /* Backend parity: `Name { v0, v1 }` (lamo_print_struct_named
             * — positional fields, name from the semantic pass; the
             * interpreter carries the name on the value itself). */
            size_t cap = 96, len = 0;
            char* out = malloc(cap);
            if (!out) return strdup("");
            out[0] = '\0';
            #define EVAL_APPEND_STR(s) do { \
                size_t addlen = strlen(s); \
                if (len + addlen + 1 > cap) { \
                    while (len + addlen + 1 > cap) cap *= 2; \
                    char* grown = realloc(out, cap); \
                    if (!grown) { free(out); return strdup(""); } \
                    out = grown; \
                } \
                memcpy(out + len, s, addlen); \
                len += addlen; \
                out[len] = '\0'; \
            } while (0)
            EVAL_APPEND_STR(v.as.strct && v.as.strct->struct_name
                            ? v.as.strct->struct_name : "struct");
            EVAL_APPEND_STR(" { ");
            if (v.as.strct) {
                for (int i = 0; i < v.as.strct->field_count; i++) {
                    if (i > 0) EVAL_APPEND_STR(", ");
                    char* elem = eval_value_to_string(v.as.strct->fields[i]);
                    EVAL_APPEND_STR(elem ? elem : "");
                    free(elem);
                }
            }
            EVAL_APPEND_STR(" }");
            #undef EVAL_APPEND_STR
            return out;
        }
        case EVAL_VAL_VOID:
            return strdup("");
        case EVAL_VAL_ERROR:
            return strdup("<error>");
    }
    return strdup("");
}

/* 2.8.0 (FU1): structural equality, mirroring the C runtime's
 * lamo_equal (lamo_runtime.h) shape for shape-for-shape parity:
 * enums compare tag-then-payload-wise, strings strcmp, numerics
 * compare via float coercion when either side is float and via int
 * coercion otherwise (bools included) — mixed kinds simply are not
 * equal (no error), matching the backend. */
static int eval_values_equal(EvalValue l, EvalValue r) {
    /* 2.10.0 (FU-vmc): arrays and structs compare by IDENTITY (same
     * heap object), mirroring lamo_equal in the C runtime — deep
     * equality would be expensive and surprising for mutable values. */
    if (l.type == EVAL_VAL_ARRAY || l.type == EVAL_VAL_STRUCT ||
        r.type == EVAL_VAL_ARRAY || r.type == EVAL_VAL_STRUCT) {
        if (l.type != r.type) return 0;
        if (l.type == EVAL_VAL_ARRAY)
            return l.as.arr == r.as.arr;
        return l.as.strct == r.as.strct;
    }
    if (l.type == EVAL_VAL_ENUM || r.type == EVAL_VAL_ENUM) {
        if (l.type != r.type) return 0;
        if (l.as.e.tag != r.as.e.tag) return 0;
        if (!l.as.e.payloads || !r.as.e.payloads)
            return l.as.e.payloads == r.as.e.payloads;
        if (l.as.e.payload_count != r.as.e.payload_count) return 0;
        for (int i = 0; i < l.as.e.payload_count; i++) {
            if (!eval_values_equal(l.as.e.payloads[i], r.as.e.payloads[i])) return 0;
        }
        return 1;
    }
    if (l.type == EVAL_VAL_STRING || r.type == EVAL_VAL_STRING) {
        if (l.type != EVAL_VAL_STRING || r.type != EVAL_VAL_STRING) return 0;
        return strcmp(l.as.s ? l.as.s : "", r.as.s ? r.as.s : "") == 0;
    }
    if (l.type == EVAL_VAL_FLOAT || r.type == EVAL_VAL_FLOAT) {
        double lf = (l.type == EVAL_VAL_FLOAT) ? l.as.f :
                    (l.type == EVAL_VAL_INT)   ? (double)l.as.i :
                    (l.type == EVAL_VAL_BOOL)  ? (double)l.as.b : 0.0;
        double rf = (r.type == EVAL_VAL_FLOAT) ? r.as.f :
                    (r.type == EVAL_VAL_INT)   ? (double)r.as.i :
                    (r.type == EVAL_VAL_BOOL)  ? (double)r.as.b : 0.0;
        return lf == rf;
    }
    if (l.type == EVAL_VAL_INT || l.type == EVAL_VAL_BOOL) {
        if (r.type != EVAL_VAL_INT && r.type != EVAL_VAL_BOOL) return 0;
        long long li = (l.type == EVAL_VAL_INT) ? l.as.i : (long long)l.as.b;
        long long ri = (r.type == EVAL_VAL_INT) ? r.as.i : (long long)r.as.b;
        return li == ri;
    }
    return 0;
}

/* 2.6.0 (FU5): build the loader's renamed module symbol for a member —
 * `lamo_mod_<alias>__<name>` (see src/cli/import_resolver.c). The
 * interpreter resolves module member calls / qualified globals by this
 * name: the aggregate program (already loaded for eval) defines module
 * functions under exactly these names. Returns a static-ring buffer
 * pointer (4 slots, same convention as the codegen's user_name1). */
static const char* eval_module_prefixed_name(const char* alias, const char* member) {
    static char bufs[4][256];
    static int ring = 0;
    char* out = bufs[ring];
    ring = (ring + 1) & 3;
    snprintf(out, sizeof(bufs[0]), "lamo_mod_%s__%s", alias ? alias : "", member ? member : "");
    return out;
}

/* ── 2.7.0 (pub step 2): REPL-side non-pub enforcement ──────────────
 * `lamo eval <file>` runs the semantic pass first, so §10.6 step 2
 * errors fire there. The interactive REPL, however, loads modules
 * WITHOUT a semantic pass, so it must enforce the pub boundary itself:
 * eval_load_module_program records every non-pub top-level fn/let
 * under its prefixed name and the module-member lookups below reject
 * them with the same message shape as the compiler. */
#define EVAL_MAX_PRIVATE_MEMBERS 512
static char* eval_private_members[EVAL_MAX_PRIVATE_MEMBERS];
static int eval_private_member_count = 0;

static void eval_register_private_member(const char* prefixed_name) {
    if (!prefixed_name) return;
    if (eval_private_member_count >= EVAL_MAX_PRIVATE_MEMBERS) return;
    for (int i = 0; i < eval_private_member_count; i++) {
        if (strcmp(eval_private_members[i], prefixed_name) == 0) return;
    }
    eval_private_members[eval_private_member_count++] = strdup(prefixed_name);
}

static int eval_member_is_private(const char* prefixed_name) {
    if (!prefixed_name) return 0;
    for (int i = 0; i < eval_private_member_count; i++) {
        if (strcmp(eval_private_members[i], prefixed_name) == 0) return 1;
    }
    return 0;
}

static void eval_report_private_member(const char* alias, const char* member,
                                       int line) {
    fprintf(stderr,
            "<repl>:%d:1: semantic error: member '%s' of module '%s' is not marked 'pub' "
            "and cannot be accessed through the module alias (§10.6 step 2, enforced in 2.7.0)\n"
            "hint: add 'pub' to the declaration of '%s' in the module file to export it explicitly\n",
            line, member ? member : "", alias ? alias : "", member ? member : "");
}

/* ── 2.8.0 (FU1): enum registry ─────────────────────────────────────
 * The interpreter needs its own picture of every declared enum to
 * (a) resolve bare/qualified variant references and constructor calls
 * in the REPL, which runs NO semantic pass and therefore has no
 * sema stamps, and (b) know whether an enum is tagged (EVAL_VAL_ENUM)
 * or legacy untagged (plain int). `lamo eval <file>` also registers
 * here — it runs sema, but the stamps only cover call/pattern nodes,
 * while bare identifiers and tagged-ness checks are cheaper through
 * the table. Registration happens whenever an AST_ENUM_DECL executes
 * (program pre-pass, statement flow, and module loads alike); the
 * "later wins" rule for cross-enum variant name collisions (SPEC §3.5)
 * is implemented by searching the table newest-first. */
#define EVAL_MAX_ENUMS 64
#define EVAL_MAX_ENUM_VARIANTS 128
typedef struct {
    char* name;                                    /* owned */
    char* variants[EVAL_MAX_ENUM_VARIANTS];        /* owned strdups */
    int   payload_counts[EVAL_MAX_ENUM_VARIANTS];
    int   variant_count;
    int   tagged;   /* any variant carries payloads (SPEC §3.5) */
} EvalEnumEntry;
static EvalEnumEntry eval_enum_table[EVAL_MAX_ENUMS];
static int eval_enum_table_count = 0;

static void eval_register_enum_decl(ASTEnumDecl* ed) {
    if (!ed || !ed->name) return;
    /* Re-declaration in a REPL session replaces the previous entry. */
    for (int i = 0; i < eval_enum_table_count; i++) {
        if (strcmp(eval_enum_table[i].name, ed->name) == 0) {
            for (int v = 0; v < eval_enum_table[i].variant_count; v++)
                free(eval_enum_table[i].variants[v]);
            free(eval_enum_table[i].name);
            for (int j = i; j < eval_enum_table_count - 1; j++)
                eval_enum_table[j] = eval_enum_table[j + 1];
            eval_enum_table_count--;
            break;
        }
    }
    if (eval_enum_table_count >= EVAL_MAX_ENUMS) return;  /* defensive cap */
    EvalEnumEntry* slot = &eval_enum_table[eval_enum_table_count++];
    memset(slot, 0, sizeof(*slot));
    slot->name = strdup(ed->name);
    slot->variant_count = ed->variant_count > EVAL_MAX_ENUM_VARIANTS
        ? EVAL_MAX_ENUM_VARIANTS : ed->variant_count;
    slot->tagged = 0;
    for (int v = 0; v < slot->variant_count; v++) {
        slot->variants[v] = strdup(ed->variants[v] ? ed->variants[v] : "");
        int pc = (ed->variant_payload_counts && ed->variant_payloads)
            ? ed->variant_payload_counts[v] : 0;
        slot->payload_counts[v] = pc;
        if (pc > 0) slot->tagged = 1;
    }
}

/* Exact enum lookup (qualified access: `Enum::Variant`). */
static EvalEnumEntry* eval_find_enum(const char* name) {
    if (!name) return NULL;
    for (int i = eval_enum_table_count - 1; i >= 0; i--) {
        if (strcmp(eval_enum_table[i].name, name) == 0) return &eval_enum_table[i];
    }
    return NULL;
}

/* Bare "later wins" variant lookup across every registered enum
 * (mirrors the compiler's find_enum_variant_any, semantic.c). Returns
 * the owning enum entry and sets *vidx_out. */
static EvalEnumEntry* eval_find_enum_by_variant(const char* variant, int* vidx_out) {
    if (!variant) return NULL;
    for (int i = eval_enum_table_count - 1; i >= 0; i--) {
        EvalEnumEntry* ee = &eval_enum_table[i];
        for (int v = 0; v < ee->variant_count; v++) {
            if (strcmp(ee->variants[v], variant) == 0) {
                if (vidx_out) *vidx_out = v;
                return ee;
            }
        }
    }
    return NULL;
}

/* 2.8.0 (FU1): strip an optional `Enum::` qualifier — constructor call
 * names may be compound ("Option::Some") and the rendered value must
 * carry only the variant name (codegen parity: lamo_variant_short_name). */
static const char* eval_variant_short_name(const char* name) {
    static char bufs[4][128];
    static int ring = 0;
    char* out = bufs[ring];
    ring = (ring + 1) & 3;
    const char* sep = name ? strstr(name, "::") : NULL;
    snprintf(out, sizeof(bufs[0]), "%s", sep ? sep + 2 : (name ? name : ""));
    return out;
}

/* ── 2.10.0 (FU-vmc): struct + impl registries ─────────────────────
 * The interpreter needs its own picture of every declared struct and
 * impl block to (a) size struct literals with field defaults, (b)
 * resolve field indexes for prop access in the REPL (which runs NO
 * semantic pass and has no sema stamps), and (c) find methods —
 * inherent AND trait impls alike, keyed on the bare struct name —
 * which is what makes trait-impl method calls work in eval/REPL.
 * Registration happens whenever an AST_STRUCT_DECL / AST_IMPL_DECL
 * executes (program pre-pass, statement flow, module loads alike);
 * re-declaration in a REPL session replaces the previous entry and
 * method lookup follows the same "later wins" rule as the compiler. */
#define EVAL_MAX_STRUCTS 128
#define EVAL_MAX_IMPLS 128
#define EVAL_MAX_FIELDS 64

typedef struct {
    char* name;                                    /* owned, bare name */
    char* field_names[EVAL_MAX_FIELDS];            /* owned strdups */
    int   field_count;
} EvalStructEntry;
static EvalStructEntry eval_struct_table[EVAL_MAX_STRUCTS];
static int eval_struct_table_count = 0;

typedef struct {
    char* struct_name;      /* owned, bare name */
    ASTNode* methods;       /* NOT owned — the AST owns the method chain */
} EvalImplEntry;
static EvalImplEntry eval_impl_table[EVAL_MAX_IMPLS];
static int eval_impl_table_count = 0;

/* Strip a trailing `<...>` type-argument list: `Stack<int>` → `Stack`.
 * Returns a pointer into a 4-slot static ring (borrowed). */
static const char* eval_struct_bare_name(const char* name) {
    static char bufs[4][128];
    static int ring = 0;
    char* out = bufs[ring];
    ring = (ring + 1) & 3;
    if (!name) { out[0] = '\0'; return out; }
    const char* lt = strchr(name, '<');
    size_t n = lt ? (size_t)(lt - name) : strlen(name);
    if (n >= 128) n = 127;
    memcpy(out, name, n);
    out[n] = '\0';
    return out;
}

static void eval_register_struct_decl(ASTStructDecl* sd) {
    if (!sd || !sd->name) return;
    const char* bare = eval_struct_bare_name(sd->name);
    /* Re-declaration in a REPL session replaces the previous entry. */
    for (int i = 0; i < eval_struct_table_count; i++) {
        if (strcmp(eval_struct_table[i].name, bare) == 0) {
            for (int f = 0; f < eval_struct_table[i].field_count; f++)
                free(eval_struct_table[i].field_names[f]);
            free(eval_struct_table[i].name);
            for (int j = i; j < eval_struct_table_count - 1; j++)
                eval_struct_table[j] = eval_struct_table[j + 1];
            eval_struct_table_count--;
            break;
        }
    }
    if (eval_struct_table_count >= EVAL_MAX_STRUCTS) return;  /* defensive cap */
    EvalStructEntry* slot = &eval_struct_table[eval_struct_table_count++];
    memset(slot, 0, sizeof(*slot));
    slot->name = strdup(bare);
    slot->field_count = sd->field_count > EVAL_MAX_FIELDS
        ? EVAL_MAX_FIELDS : sd->field_count;
    for (int f = 0; f < slot->field_count; f++) {
        slot->field_names[f] = strdup(sd->field_names[f] ? sd->field_names[f] : "");
    }
}

static void eval_register_impl_decl(ASTImplDecl* id) {
    if (!id || !id->struct_name) return;
    const char* bare = eval_struct_bare_name(id->struct_name);
    /* 2.10.0 (FU-vmc): impls ACCUMULATE like the compiler's find_method
     * — every impl block for a struct contributes its methods, so a
     * trait impl and an inherent impl coexist (and a trait impl may
     * carry extra inherent methods). Lookup searches newest-first, so
     * a re-declared method name resolves to the newest impl ("later
     * wins" per method, compiler parity). */
    if (eval_impl_table_count >= EVAL_MAX_IMPLS) return;  /* defensive cap */
    EvalImplEntry* slot = &eval_impl_table[eval_impl_table_count++];
    slot->struct_name = strdup(bare);
    slot->methods = id->methods;
}

/* Field index by name (declaration order). -1 when unknown. */
static int eval_struct_field_index(const char* struct_name, const char* field) {
    if (!struct_name || !field) return -1;
    for (int i = eval_struct_table_count - 1; i >= 0; i--) {
        if (strcmp(eval_struct_table[i].name, struct_name) == 0) {
            for (int f = 0; f < eval_struct_table[i].field_count; f++) {
                if (strcmp(eval_struct_table[i].field_names[f], field) == 0) return f;
            }
            return -1;
        }
    }
    return -1;
}

/* Method lookup by (bare) struct name + method name. Searches every
 * registered impl NEWEST-FIRST — inherent and trait impls alike, so
 * trait-impl dispatch rides the same table, and a method re-declared
 * in a newer impl wins (compiler parity: find_method accumulation +
 * REPL re-declaration "later wins"). Returns NULL when absent. */
static ASTFnDecl* eval_find_method(const char* struct_name, const char* method) {
    if (!struct_name || !method) return NULL;
    for (int i = eval_impl_table_count - 1; i >= 0; i--) {
        if (strcmp(eval_impl_table[i].struct_name, struct_name) == 0) {
            for (ASTNode* m = eval_impl_table[i].methods; m; m = m->next) {
                if (m->type == AST_FN_DECL &&
                    strcmp(((ASTFnDecl*)m)->name, method) == 0) {
                    return (ASTFnDecl*)m;
                }
            }
        }
    }
    return NULL;
}

/* ── Truthiness (SPEC §6.3 parity with lamo_is_truthy) ────────────── */

static int eval_is_truthy(EvalValue v) {
    switch (v.type) {
        case EVAL_VAL_BOOL:   return v.as.b != 0;
        case EVAL_VAL_INT:    return v.as.i != 0;
        case EVAL_VAL_FLOAT:  return v.as.f != 0.0;
        case EVAL_VAL_STRING: return v.as.s && v.as.s[0] != '\0';
        case EVAL_VAL_ENUM:   return 1;  /* enums always truthy (runtime parity) */
        case EVAL_VAL_ARRAY:  return v.as.arr != NULL && v.as.arr->count > 0;
        case EVAL_VAL_STRUCT: return v.as.strct != NULL;  /* always truthy */
        default:              return 0;
    }
}

/* ── Environment ─────────────────────────────────────────────────────── */

typedef struct EvalBinding {
    char* name;
    EvalValue value;
    struct EvalBinding* next;
} EvalBinding;

/* Functions are stored separately from variables so a fn name doesn't
 * shadow a var and vice-versa (Lamo allows both). */
typedef struct EvalFnBinding {
    char* name;
    ASTFnDecl* decl;
    EvalEnv* closure_env; /* env at definition time (for closures later) */
    struct EvalFnBinding* next;
} EvalFnBinding;

struct EvalEnv {
    EvalBinding*   bindings;
    EvalFnBinding* fns;
    EvalEnv*       parent;
};

EvalEnv* eval_env_new(EvalEnv* parent) {
    EvalEnv* env = malloc(sizeof(EvalEnv));
    if (!env) { perror("eval_env_new"); exit(1); }
    env->bindings = NULL;
    env->fns = NULL;
    env->parent = parent;
    return env;
}

void eval_env_free(EvalEnv* env) {
    EvalBinding* b = env->bindings;
    while (b) {
        EvalBinding* next = b->next;
        free(b->name);
        eval_value_free(b->value);
        free(b);
        b = next;
    }
    EvalFnBinding* f = env->fns;
    while (f) {
        EvalFnBinding* next = f->next;
        free(f->name);
        free(f);
        f = next;
    }
    free(env);
}

int eval_env_define(EvalEnv* env, const char* name, EvalValue value) {
    EvalBinding* b = malloc(sizeof(EvalBinding));
    if (!b) { perror("eval_env_define"); exit(1); }
    b->name  = strdup(name);
    b->value = value;
    b->next  = env->bindings;
    env->bindings = b;
    return 1;
}

static int eval_env_define_fn(EvalEnv* env, const char* name, ASTFnDecl* decl, EvalEnv* closure) {
    EvalFnBinding* f = malloc(sizeof(EvalFnBinding));
    if (!f) { perror("eval_env_define_fn"); exit(1); }
    f->name = strdup(name);
    f->decl = decl;
    f->closure_env = closure;
    f->next = env->fns;
    env->fns = f;
    return 1;
}

static ASTFnDecl* eval_env_find_fn(EvalEnv* env, const char* name, EvalEnv** closure_out) {
    for (EvalEnv* e = env; e; e = e->parent) {
        for (EvalFnBinding* f = e->fns; f; f = f->next) {
            if (strcmp(f->name, name) == 0) {
                if (closure_out) *closure_out = f->closure_env;
                return f->decl;
            }
        }
    }
    return NULL;
}

int eval_env_get(EvalEnv* env, const char* name, EvalValue* out) {
    for (EvalEnv* e = env; e; e = e->parent) {
        for (EvalBinding* b = e->bindings; b; b = b->next) {
            if (strcmp(b->name, name) == 0) {
                /* Return a deep copy of heap-holding values (strings,
                 * enum payloads) so caller can free safely. */
                *out = eval_value_clone(b->value);
                return 1;
            }
        }
    }
    return 0;
}

int eval_env_set(EvalEnv* env, const char* name, EvalValue value) {
    for (EvalEnv* e = env; e; e = e->parent) {
        for (EvalBinding* b = e->bindings; b; b = b->next) {
            if (strcmp(b->name, name) == 0) {
                eval_value_free(b->value);
                /* 2.8.0: the binding OWNS the value — callers hand
                 * ownership over on set (same contract as define). */
                b->value = value;
                return 1;
            }
        }
    }
    return 0;
}

/* ── Runtime helpers ─────────────────────────────────────────────────── */

#define RUNTIME_ERROR(fmt, ...) \
    do { fprintf(stderr, "runtime error: " fmt "\n", ##__VA_ARGS__); } while(0)

/* Promote int to float if one operand is float. */
static void coerce_numeric(EvalValue* a, EvalValue* b) {
    if (a->type == EVAL_VAL_INT && b->type == EVAL_VAL_FLOAT) {
        a->type = EVAL_VAL_FLOAT;
        a->as.f = (double)a->as.i;
    } else if (a->type == EVAL_VAL_FLOAT && b->type == EVAL_VAL_INT) {
        b->type = EVAL_VAL_FLOAT;
        b->as.f = (double)b->as.i;
    }
}

/* ── Forward declarations ─────────────────────────────────────────────── */

static EvalValue eval_block(ASTBlock* block, EvalEnv* env, EvalSignal* sig);
static EvalValue eval_call(const char* name, ASTNode** args, int argc,
                            EvalEnv* env, EvalSignal* sig, int line);

/* 2.10.0 (FU-vmc): call a fn decl with an optional `self` binding in
 * the frame (methods pass the receiver; plain fns pass has_self=0). */
static EvalValue eval_call_fn(ASTFnDecl* fn, EvalValue self_val, int has_self,
                              EvalValue* argv, int argc,
                              EvalEnv* env, EvalSignal* sig, const char* name);

/* 2.8.0 (FU1) — enum + match machinery (defined below). */
static EvalValue eval_enum_ctor_call(long long tag, const char* display_name,
                                     ASTNode** args, int argc,
                                     EvalEnv* env, EvalSignal* sig);
static int eval_pattern_match(LamoPattern* pat, EvalValue scrut,
                              EvalEnv* bind_env, EvalSignal* sig);
static EvalValue eval_match_value(ASTMatchStmt* ms, EvalEnv* env,
                                  EvalSignal* sig, int want_value);

/* ── Expression evaluator ─────────────────────────────────────────────── */

EvalValue eval_expression(ASTNode* node, EvalEnv* env, EvalSignal* sig) {
    if (!node) return eval_void();
    *sig = EVAL_SIG_NONE;

    switch (node->type) {
        case AST_INT_LITERAL:
            return eval_int(((ASTIntLiteral*)node)->value);
        case AST_FLOAT_LITERAL:
            return eval_float(((ASTFloatLiteral*)node)->value);
        case AST_STRING_LITERAL:
            return eval_string(((ASTStringLiteral*)node)->value);
        case AST_BOOL_LITERAL:
            return eval_bool(((ASTBoolLiteral*)node)->value);

        case AST_IDENTIFIER: {
            const char* name = ((ASTIdentifier*)node)->name;
            EvalValue out;
            if (eval_env_get(env, name, &out)) return out;
            /* 2.8.0 (FU1): bare variant value — `None`, `Red`. A
             * variable lookup failed, so resolve through the enum
             * registry: tagged enums materialize as EVAL_VAL_ENUM with
             * no payloads, legacy untagged enums keep the plain int
             * representation (identical to the transpiled backend's
             * deduped variant globals). */
            {
                int vidx = -1;
                EvalEnumEntry* ee = eval_find_enum_by_variant(name, &vidx);
                if (ee) {
                    if (ee->tagged) return eval_enum(vidx, name, NULL, 0);
                    return eval_int(vidx);
                }
            }
            RUNTIME_ERROR("undefined variable '%s'", name);
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }

        case AST_GROUPING_EXPR:
            return eval_expression(((ASTGroupingExpr*)node)->expression, env, sig);

        case AST_UNARY_EXPR: {
            ASTUnaryExpr* u = (ASTUnaryExpr*)node;
            EvalValue right = eval_expression(u->right, env, sig);
            if (*sig == EVAL_SIG_ERROR) return eval_error();
            if (u->operator == TOKEN_MINUS) {
                if (right.type == EVAL_VAL_INT)   return eval_int(-right.as.i);
                if (right.type == EVAL_VAL_FLOAT) return eval_float(-right.as.f);
                RUNTIME_ERROR("unary '-' on non-numeric value");
                *sig = EVAL_SIG_ERROR; return eval_error();
            }
            if (u->operator == TOKEN_BANG) {
                int truthy = eval_is_truthy(right);
                eval_value_free(right);
                return eval_bool(!truthy);
            }
            eval_value_free(right);
            return eval_error();
        }

        case AST_BINARY_EXPR: {
            ASTBinaryExpr* bin = (ASTBinaryExpr*)node;

            /* Short-circuit logical operators first. */
            if (bin->operator == TOKEN_AND_AND) {
                EvalValue left = eval_expression(bin->left, env, sig);
                if (*sig != EVAL_SIG_NONE) return left;
                int truthy = (left.type == EVAL_VAL_BOOL) ? left.as.b : 0;
                eval_value_free(left);
                if (!truthy) return eval_bool(0);
                EvalValue right = eval_expression(bin->right, env, sig);
                int rt = (right.type == EVAL_VAL_BOOL) ? right.as.b : 0;
                eval_value_free(right);
                return eval_bool(rt);
            }
            if (bin->operator == TOKEN_OR_OR) {
                EvalValue left = eval_expression(bin->left, env, sig);
                if (*sig != EVAL_SIG_NONE) return left;
                int truthy = (left.type == EVAL_VAL_BOOL) ? left.as.b : 0;
                eval_value_free(left);
                if (truthy) return eval_bool(1);
                EvalValue right = eval_expression(bin->right, env, sig);
                int rt = (right.type == EVAL_VAL_BOOL) ? right.as.b : 0;
                eval_value_free(right);
                return eval_bool(rt);
            }

            EvalValue left  = eval_expression(bin->left,  env, sig);
            if (*sig != EVAL_SIG_NONE) return left;
            EvalValue right = eval_expression(bin->right, env, sig);
            if (*sig != EVAL_SIG_NONE) { eval_value_free(left); return right; }

            /* String concatenation: + with at least one string operand. */
            if (bin->operator == TOKEN_PLUS &&
                (left.type == EVAL_VAL_STRING || right.type == EVAL_VAL_STRING)) {
                char* ls = eval_value_to_string(left);
                char* rs = eval_value_to_string(right);
                size_t len = strlen(ls) + strlen(rs) + 1;
                char* cat = malloc(len);
                if (cat) { strcpy(cat, ls); strcat(cat, rs); }
                free(ls); free(rs);
                eval_value_free(left); eval_value_free(right);
                return eval_string_take(cat ? cat : strdup(""));
            }

            coerce_numeric(&left, &right);

            /* Arithmetic */
            if (left.type == EVAL_VAL_FLOAT && right.type == EVAL_VAL_FLOAT) {
                double l = left.as.f, r = right.as.f;
                switch (bin->operator) {
                    case TOKEN_PLUS:     return eval_float(l + r);
                    case TOKEN_MINUS:    return eval_float(l - r);
                    case TOKEN_STAR:     return eval_float(l * r);
                    case TOKEN_SLASH:
                        if (r == 0.0) { RUNTIME_ERROR("division by zero"); *sig = EVAL_SIG_ERROR; return eval_error(); }
                        return eval_float(l / r);
                    case TOKEN_PERCENT:  return eval_float(fmod(l, r));
                    case TOKEN_LT:       return eval_bool(l < r);
                    case TOKEN_GT:       return eval_bool(l > r);
                    case TOKEN_LT_EQ:    return eval_bool(l <= r);
                    case TOKEN_GT_EQ:    return eval_bool(l >= r);
                    case TOKEN_EQ_EQ:    return eval_bool(l == r);
                    case TOKEN_BANG_EQ:  return eval_bool(l != r);
                    default: break;
                }
            }
            if (left.type == EVAL_VAL_INT && right.type == EVAL_VAL_INT) {
                long long l = left.as.i, r = right.as.i;
                switch (bin->operator) {
                    case TOKEN_PLUS:     return eval_int(l + r);
                    case TOKEN_MINUS:    return eval_int(l - r);
                    case TOKEN_STAR:     return eval_int(l * r);
                    case TOKEN_SLASH:
                        if (r == 0) { RUNTIME_ERROR("integer division by zero"); *sig = EVAL_SIG_ERROR; return eval_error(); }
                        return eval_int(l / r);
                    case TOKEN_PERCENT:
                        if (r == 0) { RUNTIME_ERROR("integer modulo by zero"); *sig = EVAL_SIG_ERROR; return eval_error(); }
                        return eval_int(l % r);
                    case TOKEN_LT:       return eval_bool(l < r);
                    case TOKEN_GT:       return eval_bool(l > r);
                    case TOKEN_LT_EQ:    return eval_bool(l <= r);
                    case TOKEN_GT_EQ:    return eval_bool(l >= r);
                    case TOKEN_EQ_EQ:    return eval_bool(l == r);
                    case TOKEN_BANG_EQ:  return eval_bool(l != r);
                    default: break;
                }
            }
            /* Equality for bools and strings — and 2.8.0 (FU1): tagged
             * enum values compare structurally (tag first, then
             * payloads), mirroring lamo_equal in the C runtime. Mixed
             * kinds compare as not-equal without erroring. */
            if (bin->operator == TOKEN_EQ_EQ || bin->operator == TOKEN_BANG_EQ) {
                int eq = eval_values_equal(left, right);
                eval_value_free(left); eval_value_free(right);
                return eval_bool(bin->operator == TOKEN_EQ_EQ ? eq : !eq);
            }
            eval_value_free(left); eval_value_free(right);
            RUNTIME_ERROR("unsupported operand types for binary operator");
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }

        case AST_CALL_EXPR: {
            ASTCallExpr* call = (ASTCallExpr*)node;
            /* 2.8.0 (FU1): stamped variant constructor — `Some(42)`,
             * `Option::Some(42)`. `lamo eval <file>` runs the semantic
             * pass, which annotates constructor calls with
             * sema_enum_name + sema_variant_index; intercept before
             * regular function lookup (constructors take precedence,
             * SPEC §3.5). */
            if (call->base.sema_enum_name && call->base.sema_variant_index >= 0) {
                return eval_enum_ctor_call(call->base.sema_variant_index,
                                           call->name, call->args, call->arg_count,
                                           env, sig);
            }
            return eval_call(call->name, call->args, call->arg_count, env, sig, node->line);
        }

        case AST_INDEX_EXPR: {
            ASTIndexExpr* ie = (ASTIndexExpr*)node;
            EvalValue obj   = eval_expression(ie->array, env, sig);
            if (*sig != EVAL_SIG_NONE) return obj;
            EvalValue index = eval_expression(ie->index, env, sig);
            if (*sig != EVAL_SIG_NONE) { eval_value_free(obj); return index; }

            /* String index: return single-char string. */
            if (obj.type == EVAL_VAL_STRING && index.type == EVAL_VAL_INT) {
                long long idx = index.as.i;
                size_t len = strlen(obj.as.s);
                if (idx < 0) idx += (long long)len;
                if (idx < 0 || (size_t)idx >= len) {
                    RUNTIME_ERROR("string index %lld out of range (length %zu)", idx, len);
                    eval_value_free(obj);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                char ch[2] = { obj.as.s[idx], '\0' };
                eval_value_free(obj);
                return eval_string(ch);
            }
            /* 2.10.0 (FU-vmc): array index — negative indexes wrap
             * Python-like; out-of-bounds mirrors the C runtime's message
             * and hard-exit behavior. */
            if (obj.type == EVAL_VAL_ARRAY && index.type == EVAL_VAL_INT) {
                EvalArrayObj* arr = obj.as.arr;
                long long idx = index.as.i;
                long long count = arr ? arr->count : 0;
                if (idx < 0) idx += count;
                if (!arr || idx < 0 || idx >= count) {
                    RUNTIME_ERROR("array index %lld out of bounds (array length %lld)",
                                  idx, count);
                    eval_value_free(obj);
                    eval_value_free(index);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                EvalValue out = eval_value_clone(arr->items[idx]);
                eval_value_free(obj);
                eval_value_free(index);
                return out;
            }
            RUNTIME_ERROR("index operation not supported on this type");
            eval_value_free(obj); eval_value_free(index);
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }

        case AST_PROP_EXPR: {
            ASTPropExpr* pe = (ASTPropExpr*)node;
            EvalValue obj;
            /* 2.6.0 (FU5): module-qualified global access (`math.PI`) —
             * the loader renamed module globals to lamo_mod_<alias>__<name>
             * and the aggregate program defines them in the env. Resolve
             * through the prefixed name before falling back to errors. */
            if (pe->object && pe->object->type == AST_IDENTIFIER) {
                const char* prefixed = eval_module_prefixed_name(
                    ((ASTIdentifier*)pe->object)->name, pe->prop_name);
                /* 2.7.0 (pub step 2): non-pub module globals are private. */
                if (eval_member_is_private(prefixed)) {
                    eval_report_private_member(((ASTIdentifier*)pe->object)->name,
                                               pe->prop_name, node->line);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                if (eval_env_get(env, prefixed, &obj)) {
                    return obj;
                }
            }
            obj = eval_expression(pe->object, env, sig);
            if (*sig != EVAL_SIG_NONE) return obj;
            if (obj.type == EVAL_VAL_STRING && strcmp(pe->prop_name, "len") == 0) {
                long long len = (long long)strlen(obj.as.s);
                eval_value_free(obj);
                return eval_int(len);
            }
            /* 2.10.0 (FU-vmc): array `.len` (backend parity:
             * lamo_array_len). */
            if (obj.type == EVAL_VAL_ARRAY && strcmp(pe->prop_name, "len") == 0) {
                long long len = obj.as.arr ? obj.as.arr->count : 0;
                eval_value_free(obj);
                return eval_int(len);
            }
            /* 2.10.0 (FU-vmc): struct field access. Field names live on
             * the value itself; the registry backs REPL nodes that were
             * built without a semantic pass. */
            if (obj.type == EVAL_VAL_STRUCT && obj.as.strct) {
                EvalStructObj* so = obj.as.strct;
                int fidx = -1;
                for (int f = 0; f < so->field_count; f++) {
                    if (so->field_names[f] &&
                        strcmp(so->field_names[f], pe->prop_name) == 0) { fidx = f; break; }
                }
                if (fidx < 0 && so->struct_name)
                    fidx = eval_struct_field_index(so->struct_name, pe->prop_name);
                if (fidx >= 0 && fidx < so->field_count) {
                    EvalValue out = eval_value_clone(so->fields[fidx]);
                    eval_value_free(obj);
                    return out;
                }
                RUNTIME_ERROR("struct '%s' has no field '%s'",
                              so->struct_name ? so->struct_name : "?", pe->prop_name);
                eval_value_free(obj);
                *sig = EVAL_SIG_ERROR;
                return eval_error();
            }
            if (pe->object && pe->object->type == AST_IDENTIFIER) {
                RUNTIME_ERROR("unknown module member or variable `%s.%s` (import the module first, e.g. `import %s.lamo as %s`)",
                              ((ASTIdentifier*)pe->object)->name, pe->prop_name,
                              ((ASTIdentifier*)pe->object)->name, ((ASTIdentifier*)pe->object)->name);
            } else {
                RUNTIME_ERROR("unknown property '%s'", pe->prop_name);
            }
            eval_value_free(obj);
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }

        case AST_MEMBER_CALL: {
            /* 2.6.0 (FU5): module member calls now WORK in eval/REPL.
             * The loader (used by `lamo eval file.lamo` and by the REPL
             * import loader) renames module functions to
             * `lamo_mod_<alias>__<name>` and defines them in the env, so
             * the call is routed through the prefixed name. Generic type
             * arguments are erased, matching the C backend. */
            ASTMemberCall* mc = (ASTMemberCall*)node;
            if (mc->object && mc->object->type == AST_IDENTIFIER) {
                const char* alias = ((ASTIdentifier*)mc->object)->name;
                const char* prefixed = eval_module_prefixed_name(alias, mc->member_name);
                /* 2.7.0 (pub step 2): non-pub module functions are private. */
                if (eval_member_is_private(prefixed)) {
                    eval_report_private_member(alias, mc->member_name, node->line);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                EvalEnv* closure = NULL;
                if (eval_env_find_fn(env, prefixed, &closure)) {
                    return eval_call(prefixed, mc->args, mc->arg_count, env, sig, node->line);
                }
                /* Fall through to value-method dispatch below (the alias
                 * may also be a plain variable holding a struct/array —
                 * the compiler resolves module aliases first, then value
                 * members). */
            }
            /* 2.10.0 (FU-vmc): value method dispatch. Evaluate the
             * receiver and route by its runtime kind: struct → method
             * table (inherent AND trait impls), array → push/pop/len. */
            {
                EvalValue obj = eval_expression(mc->object, env, sig);
                if (*sig != EVAL_SIG_NONE) return obj;
                if (obj.type == EVAL_VAL_STRUCT && obj.as.strct) {
                    EvalStructObj* so = obj.as.strct;
                    ASTFnDecl* method = eval_find_method(so->struct_name, mc->member_name);
                    if (!method) {
                        RUNTIME_ERROR("struct '%s' has no method '%s'",
                                      so->struct_name ? so->struct_name : "?",
                                      mc->member_name);
                        eval_value_free(obj);
                        *sig = EVAL_SIG_ERROR;
                        return eval_error();
                    }
                    if (method->param_count != mc->arg_count) {
                        RUNTIME_ERROR("method '%s.%s' expects %d argument(s), got %d",
                                      so->struct_name ? so->struct_name : "?",
                                      mc->member_name, method->param_count, mc->arg_count);
                        eval_value_free(obj);
                        *sig = EVAL_SIG_ERROR;
                        return eval_error();
                    }
                    EvalValue* argv = mc->arg_count > 0
                        ? malloc(sizeof(EvalValue) * (size_t)mc->arg_count) : NULL;
                    if (mc->arg_count > 0 && !argv) { perror("eval member call"); exit(1); }
                    for (int i = 0; i < mc->arg_count; i++) {
                        argv[i] = eval_expression(mc->args[i], env, sig);
                        if (*sig != EVAL_SIG_NONE) {
                            for (int j = 0; j < i; j++) eval_value_free(argv[j]);
                            free(argv);
                            eval_value_free(obj);
                            return eval_error();
                        }
                    }
                    /* obj stays owned by this scope; the frame clones
                     * `self` (shared refcount for structs). eval_call_fn
                     * takes ownership of argv (and frees it). */
                    EvalValue result = eval_call_fn(method, obj, 1, argv, mc->arg_count,
                                                    env, sig, mc->member_name);
                    eval_value_free(obj);
                    return result;
                }
                if (obj.type == EVAL_VAL_ARRAY) {
                    EvalArrayObj* arr = obj.as.arr;
                    if (strcmp(mc->member_name, "push") == 0) {
                        if (mc->arg_count != 1) {
                            RUNTIME_ERROR("array method `push` expects 1 argument, got %d",
                                          mc->arg_count);
                            eval_value_free(obj);
                            *sig = EVAL_SIG_ERROR;
                            return eval_error();
                        }
                        EvalValue item = eval_expression(mc->args[0], env, sig);
                        if (*sig != EVAL_SIG_NONE) { eval_value_free(obj); eval_value_free(item); return eval_error(); }
                        if (arr) {
                            arr->items = realloc(arr->items, sizeof(EvalValue) * (size_t)(arr->count + 1));
                            if (!arr->items) { perror("array push"); exit(1); }
                            arr->items[arr->count++] = item;  /* takes ownership */
                        } else {
                            eval_value_free(item);
                        }
                        eval_value_free(obj);
                        return eval_void();
                    }
                    if (strcmp(mc->member_name, "pop") == 0) {
                        if (mc->arg_count != 0) {
                            RUNTIME_ERROR("array method `pop` expects 0 arguments, got %d",
                                          mc->arg_count);
                            eval_value_free(obj);
                            *sig = EVAL_SIG_ERROR;
                            return eval_error();
                        }
                        if (!arr || arr->count == 0) {
                            RUNTIME_ERROR("pop from empty array");
                            eval_value_free(obj);
                            *sig = EVAL_SIG_ERROR;
                            return eval_error();
                        }
                        EvalValue item = arr->items[--arr->count];  /* moves out */
                        eval_value_free(obj);
                        return item;
                    }
                    if (strcmp(mc->member_name, "len") == 0) {
                        long long n = arr ? arr->count : 0;
                        eval_value_free(obj);
                        return eval_int(n);
                    }
                    RUNTIME_ERROR("array has no method '%s' (valid: push, pop, len)",
                                  mc->member_name);
                    eval_value_free(obj);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                eval_value_free(obj);
            }
            {
                const char* alias = (mc->object && mc->object->type == AST_IDENTIFIER)
                    ? ((ASTIdentifier*)mc->object)->name : "<expr>";
                RUNTIME_ERROR("module member call `%s.%s(...)` requires an identifier receiver in eval/REPL mode",
                              alias, mc->member_name);
            }
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }

        case AST_VARIANT_REF: {
            /* 2.8.0 (FU1): qualified unit-variant value —
             * `Option::None`, `Color::Red` (2.7.0 FU4 syntax). The
             * semantic pass stamps the variant index when it runs
             * (`lamo eval`); the registry provides exact qualified
             * lookup in the REPL (no stamps). */
            ASTVariantRef* vr = (ASTVariantRef*)node;
            int idx = -1;
            EvalEnumEntry* ee = NULL;
            /* sema stamps are only trustworthy when the semantic pass
             * actually ran (sema_enum_name non-NULL); fresh REPL nodes
             * carry index 0 from zero-initialization. */
            if (node->sema_enum_name && node->sema_variant_index >= 0) {
                idx = node->sema_variant_index;
                ee = eval_find_enum(vr->enum_name);
            } else {
                ee = eval_find_enum(vr->enum_name);
                if (ee) {
                    for (int v = 0; v < ee->variant_count; v++) {
                        if (vr->variant_name && strcmp(ee->variants[v], vr->variant_name) == 0) {
                            idx = v;
                            break;
                        }
                    }
                }
            }
            if (!ee || idx < 0) {
                RUNTIME_ERROR("unknown enum variant '%s::%s'",
                              vr->enum_name ? vr->enum_name : "?",
                              vr->variant_name ? vr->variant_name : "?");
                *sig = EVAL_SIG_ERROR;
                return eval_error();
            }
            if (ee->tagged) return eval_enum(idx, vr->variant_name, NULL, 0);
            return eval_int(idx);  /* legacy untagged: plain int */
        }

        case AST_ARRAY_LITERAL: {
            /* 2.10.0 (FU-vmc): array literal → refcounted EVAL_VAL_ARRAY.
             * The object takes ownership of the evaluated elements. */
            ASTArrayLiteral* al = (ASTArrayLiteral*)node;
            EvalArrayObj* arr = malloc(sizeof(EvalArrayObj));
            if (!arr) { perror("eval array literal"); exit(1); }
            arr->refcount = 1;
            arr->count = al->element_count;
            arr->items = al->element_count > 0
                ? malloc(sizeof(EvalValue) * (size_t)al->element_count) : NULL;
            if (al->element_count > 0 && !arr->items) { perror("eval array literal"); exit(1); }
            for (int i = 0; i < al->element_count; i++) {
                arr->items[i] = eval_expression(al->elements[i], env, sig);
                if (*sig != EVAL_SIG_NONE) {
                    arr->count = i;  /* only i elements are initialized */
                    EvalValue partial = eval_array_take(arr);
                    eval_value_free(partial);
                    return eval_error();
                }
            }
            return eval_array_take(arr);
        }

        case AST_STRUCT_LITERAL: {
            /* 2.10.0 (FU-vmc): struct literal → refcounted
             * EVAL_VAL_STRUCT. Missing fields default to int 0 (backend
             * parity with lamo_struct_alloc); provided fields are placed
             * by DECLARATION index (codegen parity), so field order in
             * the literal is irrelevant but names must be declared. */
            ASTStructLiteral* sl = (ASTStructLiteral*)node;
            const char* bare = eval_struct_bare_name(sl->struct_name);
            EvalStructEntry* se = NULL;
            for (int i = eval_struct_table_count - 1; i >= 0; i--) {
                if (strcmp(eval_struct_table[i].name, bare) == 0) { se = &eval_struct_table[i]; break; }
            }
            EvalStructObj* so = malloc(sizeof(EvalStructObj));
            if (!so) { perror("eval struct literal"); exit(1); }
            so->refcount = 1;
            so->struct_name = strdup(bare);
            so->field_count = se ? se->field_count : 0;
            so->field_names = malloc(sizeof(char*) * (size_t)(so->field_count > 0 ? so->field_count : 1));
            so->fields = malloc(sizeof(EvalValue) * (size_t)(so->field_count > 0 ? so->field_count : 1));
            if (!so->field_names || !so->fields) { perror("eval struct literal"); exit(1); }
            for (int f = 0; f < so->field_count; f++) {
                so->field_names[f] = strdup(se->field_names[f]);
                so->fields[f] = eval_int(0);  /* default (backend parity) */
            }
            /* Evaluate provided values first (element order parity with
             * the compiler: literal values evaluate in literal order),
             * then place by declared index. */
            EvalValue* provided = sl->field_count > 0
                ? malloc(sizeof(EvalValue) * (size_t)sl->field_count) : NULL;
            if (sl->field_count > 0 && !provided) { perror("eval struct literal"); exit(1); }
            int* slots = sl->field_count > 0
                ? malloc(sizeof(int) * (size_t)sl->field_count) : NULL;
            if (sl->field_count > 0 && !slots) { perror("eval struct literal"); exit(1); }
            for (int i = 0; i < sl->field_count; i++) {
                provided[i] = eval_expression(sl->field_values[i], env, sig);
                if (*sig != EVAL_SIG_NONE) {
                    for (int j = 0; j < i; j++) eval_value_free(provided[j]);
                    free(provided); free(slots);
                    EvalValue partial = eval_struct_take(so);
                    eval_value_free(partial);
                    return eval_error();
                }
                const char* fname = sl->field_names[i] ? sl->field_names[i] : "";
                int fidx = -1;
                for (int f = 0; f < so->field_count; f++) {
                    if (strcmp(so->field_names[f], fname) == 0) { fidx = f; break; }
                }
                if (fidx < 0) {
                    /* Unknown field: the semantic pass rejects this;
                     * REPL defensiveness mirrors struct_get's fallback. */
                    fidx = 0;
                    if (so->field_count == 0) {
                        RUNTIME_ERROR("struct '%s' has no field '%s'", bare, fname);
                        for (int j = 0; j <= i; j++) eval_value_free(provided[j]);
                        free(provided); free(slots);
                        EvalValue partial = eval_struct_take(so);
                        eval_value_free(partial);
                        *sig = EVAL_SIG_ERROR;
                        return eval_error();
                    }
                }
                slots[i] = fidx;
            }
            for (int i = 0; i < sl->field_count; i++) {
                eval_value_free(so->fields[slots[i]]);
                so->fields[slots[i]] = provided[i];  /* moves in */
            }
            free(provided);
            free(slots);
            return eval_struct_take(so);
        }

        case AST_MATCH_STMT: {
            /* 2.8.0 (FU1): match in EXPRESSION position (`let x = match
             * ... { ... }`, 2.8.0 FU3). Threads the matched arm's value
             * out; the statement form goes through eval_statement, which
             * discards it. */
            ASTMatchStmt* ms = (ASTMatchStmt*)node;
            return eval_match_value(ms, env, sig, 1);
        }

        default:
            return eval_void();
    }
}

/* ── 2.8.0 (FU1): enum constructor + match machinery ──────────────────── */

/* Build a tagged enum value from a constructor call's arguments. The
 * tag comes from the semantic stamps (eval mode) or the registry
 * (REPL mode); the rendered name is the bare variant name. Takes
 * ownership of the evaluated payloads (handed to eval_enum). */
static EvalValue eval_enum_ctor_call(long long tag, const char* display_name,
                                     ASTNode** args, int argc,
                                     EvalEnv* env, EvalSignal* sig) {
    EvalValue* payloads = NULL;
    if (argc > 0) {
        payloads = malloc(sizeof(EvalValue) * (size_t)argc);
        if (!payloads) { perror("eval_enum_ctor_call"); exit(1); }
    }
    for (int i = 0; i < argc; i++) {
        payloads[i] = eval_expression(args[i], env, sig);
        if (*sig != EVAL_SIG_NONE) {
            for (int j = 0; j < i; j++) eval_value_free(payloads[j]);
            free(payloads);
            return eval_error();
        }
    }
    return eval_enum(tag, eval_variant_short_name(display_name), payloads, argc);
}

/* Try to match `pat` against `scrut`. Arm bindings are defined in
 * bind_env (a per-arm frame the caller discards on rejection), so a
 * failed arm never leaks bindings. Mirrors the compiler's semantics:
 * tagged enums compare the variant tag then recurse over payloads,
 * legacy untagged enums compare the plain int index (SPEC §3.5/§4.6). */
static int eval_pattern_match(LamoPattern* pat, EvalValue scrut,
                              EvalEnv* bind_env, EvalSignal* sig) {
    if (!pat) return 0;
    switch (pat->kind) {
        case LAMO_PATTERN_WILDCARD:
            return 1;

        case LAMO_PATTERN_LITERAL: {
            /* 2.8.0 (FU3): `1 => ...`, `"a" => ...` — compared with the
             * same structural equality the == operator uses (which
             * mirrors the backend's lamo_equal, including numeric
             * coercion). Literals bind nothing. */
            EvalValue lit = eval_expression(pat->literal, bind_env, sig);
            if (*sig == EVAL_SIG_ERROR) return 0;
            int eq = eval_values_equal(scrut, lit);
            eval_value_free(lit);
            return eq;
        }

        case LAMO_PATTERN_BINDING:
            /* Legal only in a nested position; the clone gives the arm
             * frame its own copy of the payload (the scrutinee keeps
             * ownership of the original). */
            eval_env_define(bind_env, pat->name, eval_value_clone(scrut));
            return 1;

        case LAMO_PATTERN_CTOR: {
            /* Variant resolution: exact stamps when the semantic pass
             * ran (`lamo eval`), otherwise the registry — qualified
             * `Enum::Variant` names resolve exactly, bare names use
             * "later wins" (SPEC §3.5). */
            int idx = -1;
            EvalEnumEntry* ee = NULL;
            if (pat->sema_variant_index >= 0 && pat->sema_enum_name) {
                idx = pat->sema_variant_index;
                ee = eval_find_enum(pat->sema_enum_name);
            } else {
                const char* sep = strstr(pat->name, "::");
                if (sep) {
                    char q_enum[128];
                    size_t elen = (size_t)(sep - pat->name);
                    if (elen >= sizeof(q_enum)) elen = sizeof(q_enum) - 1;
                    memcpy(q_enum, pat->name, elen);
                    q_enum[elen] = '\0';
                    ee = eval_find_enum(q_enum);
                    if (ee) {
                        const char* q_variant = sep + 2;
                        for (int v = 0; v < ee->variant_count; v++) {
                            if (strcmp(ee->variants[v], q_variant) == 0) { idx = v; break; }
                        }
                    }
                } else {
                    ee = eval_find_enum_by_variant(pat->name, &idx);
                }
            }
            if (!ee || idx < 0) {
                /* The semantic pass rejects unknown variants before the
                 * interpreter runs; this is REPL-side defensiveness. */
                RUNTIME_ERROR("match pattern '%s' is not a known enum variant", pat->name);
                *sig = EVAL_SIG_ERROR;
                return 0;
            }

            if (ee->tagged) {
                if (scrut.type != EVAL_VAL_ENUM || scrut.as.e.tag != idx) return 0;
                for (int c = 0; c < pat->child_count; c++) {
                    LamoPattern* child = pat->children[c];
                    if (!child || child->kind == LAMO_PATTERN_WILDCARD) continue;
                    EvalValue* payload = NULL;
                    if (scrut.as.e.payloads && c < scrut.as.e.payload_count)
                        payload = &scrut.as.e.payloads[c];
                    if (!payload) {
                        RUNTIME_ERROR("variant '%s' carries no payload at position %d",
                                      ee->variants[idx], c + 1);
                        *sig = EVAL_SIG_ERROR;
                        return 0;
                    }
                    if (!eval_pattern_match(child, *payload, bind_env, sig)) return 0;
                }
                return 1;
            }
            /* Legacy untagged enum: the scrutinee IS the int index. */
            if (scrut.type != EVAL_VAL_INT || scrut.as.i != idx) return 0;
            if (pat->child_count > 0) {
                RUNTIME_ERROR("variant '%s' carries no payloads", ee->variants[idx]);
                *sig = EVAL_SIG_ERROR;
                return 0;
            }
            return 1;
        }

        default:
            return 0;
    }
}

/* Evaluate a match. want_value=1 (expression position) threads the
 * matched arm's value out; want_value=0 (statement position) discards
 * it. Control-flow signals (return/break/continue) and errors propagate
 * to the caller with their payload intact. Unmatched scrutinees mirror
 * the C backend: no runtime error, default value. */
static EvalValue eval_match_value(ASTMatchStmt* ms, EvalEnv* env,
                                  EvalSignal* sig, int want_value) {
    EvalValue scrut = eval_expression(ms->scrutinee, env, sig);
    if (*sig != EVAL_SIG_NONE) return scrut;

    for (int i = 0; i < ms->arm_count; i++) {
        LamoPattern* pat = ms->patterns[i];
        if (!pat) continue;
        EvalEnv* arm_env = eval_env_new(env);
        int matched = eval_pattern_match(pat, scrut, arm_env, sig);
        if (matched && *sig != EVAL_SIG_ERROR && ms->guards[i]) {
            /* `when` guard, evaluated in the arm scope so it can read
             * the arm's payload bindings (SPEC §4.6). */
            EvalValue g = eval_expression(ms->guards[i], arm_env, sig);
            if (*sig == EVAL_SIG_ERROR) {
                eval_value_free(g);
                eval_env_free(arm_env);
                eval_value_free(scrut);
                return eval_error();
            }
            int truthy = eval_is_truthy(g);
            eval_value_free(g);
            matched = truthy;
        }
        if (matched && *sig != EVAL_SIG_ERROR) {
            /* Arm bodies parse as statements (or expressions in the
             * 2.8.0 FU3 expression form); eval_statement handles both. */
            EvalValue result = ms->bodies[i]
                ? eval_statement(ms->bodies[i], arm_env, sig)
                : eval_void();
            eval_env_free(arm_env);
            eval_value_free(scrut);
            if (!want_value && *sig == EVAL_SIG_NONE) {
                eval_value_free(result);
                return eval_void();
            }
            return result;  /* value / RETURN / BREAK / CONTINUE payload */
        }
        eval_env_free(arm_env);
        if (*sig == EVAL_SIG_ERROR) {
            eval_value_free(scrut);
            return eval_error();
        }
    }
    eval_value_free(scrut);
    if (want_value) return eval_int(0);  /* backend parity: default int */
    return eval_void();
}

/* ── Builtin functions ────────────────────────────────────────────────── */

static EvalValue eval_builtin(const char* name, EvalValue* argv, int argc,
                               EvalSignal* sig) {
    *sig = EVAL_SIG_NONE;

    if (strcmp(name, "print") == 0) {
        for (int i = 0; i < argc; i++) {
            char* s = eval_value_to_string(argv[i]);
            printf("%s", s);
            free(s);
        }
        printf("\n");
        return eval_void();
    }

    if (strcmp(name, "str") == 0 && argc == 1) {
        char* s = eval_value_to_string(argv[0]);
        return eval_string_take(s);
    }

    if (strcmp(name, "int") == 0 && argc == 1) {
        if (argv[0].type == EVAL_VAL_INT)    return eval_int(argv[0].as.i);
        if (argv[0].type == EVAL_VAL_FLOAT)  return eval_int((long long)argv[0].as.f);
        if (argv[0].type == EVAL_VAL_STRING) return eval_int(atoll(argv[0].as.s));
        if (argv[0].type == EVAL_VAL_BOOL)   return eval_int(argv[0].as.b ? 1 : 0);
        return eval_int(0);
    }

    if (strcmp(name, "float") == 0 && argc == 1) {
        if (argv[0].type == EVAL_VAL_FLOAT)  return eval_float(argv[0].as.f);
        if (argv[0].type == EVAL_VAL_INT)    return eval_float((double)argv[0].as.i);
        if (argv[0].type == EVAL_VAL_STRING) return eval_float(atof(argv[0].as.s));
        if (argv[0].type == EVAL_VAL_BOOL)   return eval_float(argv[0].as.b ? 1.0 : 0.0);
        return eval_float(0.0);
    }

    if (strcmp(name, "bool") == 0 && argc == 1) {
        int b = (argv[0].type == EVAL_VAL_BOOL)   ? argv[0].as.b :
                (argv[0].type == EVAL_VAL_INT)    ? (argv[0].as.i != 0) :
                (argv[0].type == EVAL_VAL_FLOAT)  ? (argv[0].as.f != 0.0) :
                (argv[0].type == EVAL_VAL_STRING) ? (argv[0].as.s && argv[0].as.s[0]) :
                (argv[0].type == EVAL_VAL_ENUM)   ? 1 : 0;
        return eval_bool(b);
    }

    if (strcmp(name, "len") == 0 && argc == 1) {
        if (argv[0].type == EVAL_VAL_STRING)
            return eval_int((long long)strlen(argv[0].as.s));
        /* 2.10.0 (FU-vmc): array length (backend parity). */
        if (argv[0].type == EVAL_VAL_ARRAY)
            return eval_int(argv[0].as.arr ? argv[0].as.arr->count : 0);
        return eval_int(0);
    }

    /* 2.10.0 (FU-vmc): type predicates gain isarray (backend parity:
     * lamo_is_array). */
    if (strcmp(name, "isarray") == 0 && argc == 1) {
        return eval_bool(argv[0].type == EVAL_VAL_ARRAY);
    }

    /* 2.10.0 (FU-vmc): global array builtins (SPEC §8: push(arr, x) /
     * pop(arr) are the function forms of arr.push(x) / arr.pop()).
     * `append` is a legacy alias of push. Mutation happens on the
     * SHARED heap object, mirroring the pointer-based backend. */
    if ((strcmp(name, "push") == 0 || strcmp(name, "append") == 0) && argc == 2) {
        if (argv[0].type != EVAL_VAL_ARRAY || !argv[0].as.arr) {
            RUNTIME_ERROR("push expects an array as its first argument");
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }
        EvalArrayObj* arr = argv[0].as.arr;
        arr->items = realloc(arr->items, sizeof(EvalValue) * (size_t)(arr->count + 1));
        if (!arr->items) { perror("push"); exit(1); }
        arr->items[arr->count++] = argv[1];  /* takes ownership */
        return eval_void();
    }
    if (strcmp(name, "pop") == 0 && argc == 1) {
        if (argv[0].type != EVAL_VAL_ARRAY || !argv[0].as.arr) {
            RUNTIME_ERROR("pop expects an array as its first argument");
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }
        EvalArrayObj* arr = argv[0].as.arr;
        if (arr->count == 0) {
            RUNTIME_ERROR("pop from empty array");
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }
        return arr->items[--arr->count];  /* moves out */
    }

    if (strcmp(name, "input") == 0) {
        /* Print optional prompt. */
        if (argc > 0) {
            char* s = eval_value_to_string(argv[0]);
            printf("%s", s);
            free(s);
            fflush(stdout);
        }
        char buf[1024];
        if (!fgets(buf, sizeof(buf), stdin)) return eval_string("");
        /* Strip trailing newline. */
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
        return eval_string(buf);
    }

    if (strcmp(name, "sqrt") == 0 && argc == 1) {
        double v = argv[0].type == EVAL_VAL_INT ? (double)argv[0].as.i : argv[0].as.f;
        return eval_float(sqrt(v));
    }
    if (strcmp(name, "abs") == 0 && argc == 1) {
        if (argv[0].type == EVAL_VAL_INT)   return eval_int(argv[0].as.i < 0 ? -argv[0].as.i : argv[0].as.i);
        if (argv[0].type == EVAL_VAL_FLOAT) return eval_float(fabs(argv[0].as.f));
        return argv[0];
    }
    if (strcmp(name, "floor") == 0 && argc == 1) {
        double v = argv[0].type == EVAL_VAL_INT ? (double)argv[0].as.i : argv[0].as.f;
        return eval_int((long long)floor(v));
    }
    if (strcmp(name, "ceil") == 0 && argc == 1) {
        double v = argv[0].type == EVAL_VAL_INT ? (double)argv[0].as.i : argv[0].as.f;
        return eval_int((long long)ceil(v));
    }
    if (strcmp(name, "pow") == 0 && argc == 2) {
        double a = argv[0].type == EVAL_VAL_INT ? (double)argv[0].as.i : argv[0].as.f;
        double b = argv[1].type == EVAL_VAL_INT ? (double)argv[1].as.i : argv[1].as.f;
        return eval_float(pow(a, b));
    }

    /* GUI and HTTP builtins: no-op in eval mode with a warning. */
    if (strncmp(name, "gui_", 4) == 0 || strncmp(name, "http_", 5) == 0) {
        fprintf(stderr, "note: '%s' is a GUI/HTTP builtin and is not supported in eval mode\n", name);
        return eval_void();
    }

    /* push/pop/append for arrays — handled above when the arity
     * matches; array() with 0 args mirrors the backend's empty array. */
    if (strcmp(name, "array") == 0 && argc == 0) {
        EvalArrayObj* arr = malloc(sizeof(EvalArrayObj));
        if (!arr) { perror("array"); exit(1); }
        arr->refcount = 1;
        arr->items = NULL;
        arr->count = 0;
        return eval_array_take(arr);
    }
    if (strcmp(name, "push") == 0 || strcmp(name, "pop") == 0 ||
        strcmp(name, "append") == 0) {
        RUNTIME_ERROR("%s expects an array argument", name);
        *sig = EVAL_SIG_ERROR;
        return eval_error();
    }

    RUNTIME_ERROR("call to unknown function '%s'", name);
    *sig = EVAL_SIG_ERROR;
    return eval_error();
}

/* ── Function call ────────────────────────────────────────────────────── */

static EvalValue eval_call(const char* name, ASTNode** args, int argc,
                            EvalEnv* env, EvalSignal* sig, int line) {
    /* Evaluate arguments first. */
    EvalValue* argv = argc > 0 ? malloc(sizeof(EvalValue) * (size_t)argc) : NULL;
    for (int i = 0; i < argc; i++) {
        argv[i] = eval_expression(args[i], env, sig);
        if (*sig != EVAL_SIG_NONE) {
            for (int j = 0; j < i; j++) eval_value_free(argv[j]);
            free(argv);
            return eval_error();
        }
    }

    /* 2.8.0 (FU1): variant constructor via the enum registry — the REPL
     * runs no semantic pass, so there are no stamps here. Payload-variant
     * constructors take precedence over functions AND builtins, matching
     * the compiler's resolution order (semantic_visit_call_full only
     * hijacks calls with payloads; `None()` falls through to the
     * function path exactly like the compiler, SPEC §3.5). */
    {
        int vidx = -1;
        EvalEnumEntry* ee = eval_find_enum_by_variant(name, &vidx);
        if (ee && ee->payload_counts[vidx] > 0) {
            int pcount = ee->payload_counts[vidx];
            if (pcount != argc) {
                RUNTIME_ERROR("variant '%s' expects %d payload argument(s), got %d",
                              name, pcount, argc);
                for (int i = 0; i < argc; i++) eval_value_free(argv[i]);
                free(argv);
                *sig = EVAL_SIG_ERROR;
                return eval_error();
            }
            /* argv transfers into eval_enum. */
            return eval_enum(vidx, eval_variant_short_name(name), argv, argc);
        }
    }

    /* Try user-defined function first. */
    EvalEnv* closure = NULL;
    ASTFnDecl* fn = eval_env_find_fn(env, name, &closure);
    if (fn) {
        if (fn->param_count != argc) {
            RUNTIME_ERROR("function '%s' expects %d argument(s), got %d",
                          name, fn->param_count, argc);
            for (int i = 0; i < argc; i++) eval_value_free(argv[i]);
            free(argv);
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }
        return eval_call_fn(fn, eval_void(), 0, argv, argc, closure ? closure : env, sig, name);
    }

    /* Builtin. */
    EvalValue result = eval_builtin(name, argv, argc, sig);
    for (int i = 0; i < argc; i++) eval_value_free(argv[i]);
    free(argv);
    (void)line;
    return result;
}

/* 2.10.0 (FU-vmc): shared call machinery. Takes ownership of argv[]
 * (each entry moves into the new frame). has_self=1 binds `self`
 * first — the receiver is cloned into the frame (structs share the
 * refcounted object, so methods mutate the SAME instance the caller
 * holds, exactly like the compiled backend). */
static EvalValue eval_call_fn(ASTFnDecl* fn, EvalValue self_val, int has_self,
                              EvalValue* argv, int argc,
                              EvalEnv* env, EvalSignal* sig, const char* name) {
    (void)name;
    EvalEnv* frame = eval_env_new(env);
    if (has_self) {
        eval_env_define(frame, "self", eval_value_clone(self_val));
    }
    for (int i = 0; i < argc && i < fn->param_count; i++) {
        eval_env_define(frame, fn->params[i], argv[i]);
        /* argv[i] is now owned by frame — don't double-free. */
    }
    free(argv);

    EvalSignal inner_sig = EVAL_SIG_NONE;
    EvalValue result = eval_statement(fn->body, frame, &inner_sig);
    eval_env_free(frame);

    if (inner_sig == EVAL_SIG_RETURN) {
        *sig = EVAL_SIG_NONE; /* return is handled — caller sees normal value */
        return result;
    }
    if (inner_sig == EVAL_SIG_ERROR) {
        *sig = EVAL_SIG_ERROR;
        eval_value_free(result);
        return eval_error();
    }
    eval_value_free(result);
    return eval_void();
}

/* ── Statement evaluator ──────────────────────────────────────────────── */

static EvalValue eval_block(ASTBlock* block, EvalEnv* env, EvalSignal* sig) {
    EvalEnv* frame = eval_env_new(env);
    EvalValue last = eval_void();

    for (ASTNode* s = block->statements; s; s = s->next) {
        eval_value_free(last);
        last = eval_statement(s, frame, sig);
        if (*sig != EVAL_SIG_NONE) break;
    }

    eval_env_free(frame);
    return last;
}

EvalValue eval_statement(ASTNode* node, EvalEnv* env, EvalSignal* sig) {
    if (!node) { *sig = EVAL_SIG_NONE; return eval_void(); }
    *sig = EVAL_SIG_NONE;

    switch (node->type) {
        case AST_BLOCK:
            return eval_block((ASTBlock*)node, env, sig);

        case AST_VAR_DECL: {
            ASTVarDecl* vd = (ASTVarDecl*)node;
            EvalValue val = eval_expression(vd->initializer, env, sig);
            if (*sig != EVAL_SIG_NONE) return val;
            eval_env_define(env, vd->name, val);
            return eval_void();
        }

        case AST_FN_DECL: {
            ASTFnDecl* fn = (ASTFnDecl*)node;
            eval_env_define_fn(env, fn->name, fn, env);
            return eval_void();
        }

        case AST_ENUM_DECL: {
            /* 2.8.0 (FU1): register the enum in the interpreter's enum
             * table so variant references, constructor calls, and match
             * patterns resolve (SPEC §3.5/§10.7). No runtime effect —
             * variants materialize on use, like the C backend's deduped
             * globals. */
            eval_register_enum_decl((ASTEnumDecl*)node);
            return eval_void();
        }

        case AST_STRUCT_DECL: {
            /* 2.10.0 (FU-vmc): register the struct so literals size
             * correctly, field lookups work in the REPL, and methods
             * have a receiver type to key on. */
            eval_register_struct_decl((ASTStructDecl*)node);
            return eval_void();
        }

        case AST_IMPL_DECL: {
            /* 2.10.0 (FU-vmc): register the impl (inherent OR trait) so
             * method calls dispatch — trait impls included, which is
             * what lets trait-impl methods run in eval/REPL. */
            eval_register_impl_decl((ASTImplDecl*)node);
            return eval_void();
        }

        case AST_PLACE_ASSIGN_STMT: {
            /* 2.10.0 (FU-vmc): `arr[i] = v;` / `obj.field = v;` with
             * =, +=, -= (codegen parity: direct setter or a
             * read-modify-write through lamo_add/lamo_sub). The target
             * expression is evaluated once; the mutation lands on the
             * SHARED heap object so every alias observes it. */
            ASTPlaceAssignStmt* pa = (ASTPlaceAssignStmt*)node;
            int op = pa->op_type;   /* TOKEN_EQUALS / PLUS_EQ / MINUS_EQ */
            if (pa->target->type == AST_INDEX_EXPR) {
                ASTIndexExpr* ie = (ASTIndexExpr*)pa->target;
                EvalValue obj = eval_expression(ie->array, env, sig);
                if (*sig != EVAL_SIG_NONE) return obj;
                EvalValue index = eval_expression(ie->index, env, sig);
                if (*sig != EVAL_SIG_NONE) { eval_value_free(obj); return index; }
                EvalValue val = eval_expression(pa->value, env, sig);
                if (*sig != EVAL_SIG_NONE) {
                    eval_value_free(obj); eval_value_free(index); return val;
                }
                if (obj.type != EVAL_VAL_ARRAY || !obj.as.arr) {
                    RUNTIME_ERROR("indexed assignment target is not an array");
                    eval_value_free(obj); eval_value_free(index); eval_value_free(val);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                if (index.type != EVAL_VAL_INT) {
                    RUNTIME_ERROR("array index must be an int");
                    eval_value_free(obj); eval_value_free(index); eval_value_free(val);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                EvalArrayObj* arr = obj.as.arr;
                long long idx = index.as.i;
                if (idx < 0) idx += arr->count;
                if (idx < 0 || idx >= arr->count) {
                    RUNTIME_ERROR("array index %lld out of bounds (array length %lld)",
                                  idx, (long long)arr->count);
                    eval_value_free(obj); eval_value_free(index); eval_value_free(val);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                eval_value_free(obj); eval_value_free(index);
                if (op == TOKEN_EQUALS) {
                    eval_value_free(arr->items[idx]);
                    arr->items[idx] = val;   /* moves in */
                    return eval_void();
                }
                /* Read-modify-write for += / -=. */
                EvalValue current = arr->items[idx];   /* moves out */
                if (op == TOKEN_PLUS_EQ &&
                    (current.type == EVAL_VAL_STRING || val.type == EVAL_VAL_STRING)) {
                    char* ls = eval_value_to_string(current);
                    char* rs = eval_value_to_string(val);
                    size_t len = strlen(ls) + strlen(rs) + 1;
                    char* cat = malloc(len);
                    if (cat) { strcpy(cat, ls); strcat(cat, rs); }
                    free(ls); free(rs);
                    eval_value_free(current); eval_value_free(val);
                    if (!cat) { *sig = EVAL_SIG_ERROR; return eval_error(); }
                    arr->items[idx] = eval_string_take(cat);
                    return eval_void();
                }
                coerce_numeric(&current, &val);
                if (current.type == EVAL_VAL_INT && val.type == EVAL_VAL_INT) {
                    long long r = op == TOKEN_PLUS_EQ
                        ? current.as.i + val.as.i
                        : current.as.i - val.as.i;
                    eval_value_free(current); eval_value_free(val);
                    arr->items[idx] = eval_int(r);
                    return eval_void();
                }
                if (current.type == EVAL_VAL_FLOAT && val.type == EVAL_VAL_FLOAT) {
                    double r = op == TOKEN_PLUS_EQ
                        ? current.as.f + val.as.f
                        : current.as.f - val.as.f;
                    eval_value_free(current); eval_value_free(val);
                    arr->items[idx] = eval_float(r);
                    return eval_void();
                }
                eval_value_free(current); eval_value_free(val);
                RUNTIME_ERROR("unsupported operand types for compound assignment");
                *sig = EVAL_SIG_ERROR;
                return eval_error();
            }
            if (pa->target->type == AST_PROP_EXPR) {
                ASTPropExpr* pe = (ASTPropExpr*)pa->target;
                EvalValue obj = eval_expression(pe->object, env, sig);
                if (*sig != EVAL_SIG_NONE) return obj;
                EvalValue val = eval_expression(pa->value, env, sig);
                if (*sig != EVAL_SIG_NONE) { eval_value_free(obj); return val; }
                if (obj.type != EVAL_VAL_STRUCT || !obj.as.strct) {
                    RUNTIME_ERROR("field assignment target is not a struct");
                    eval_value_free(obj); eval_value_free(val);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                EvalStructObj* so = obj.as.strct;
                int fidx = -1;
                for (int f = 0; f < so->field_count; f++) {
                    if (so->field_names[f] &&
                        strcmp(so->field_names[f], pe->prop_name) == 0) { fidx = f; break; }
                }
                if (fidx < 0 && so->struct_name)
                    fidx = eval_struct_field_index(so->struct_name, pe->prop_name);
                if (fidx < 0 || fidx >= so->field_count) {
                    RUNTIME_ERROR("struct '%s' has no field '%s'",
                                  so->struct_name ? so->struct_name : "?", pe->prop_name);
                    eval_value_free(obj); eval_value_free(val);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                eval_value_free(obj);
                if (op == TOKEN_EQUALS) {
                    eval_value_free(so->fields[fidx]);
                    so->fields[fidx] = val;   /* moves in */
                    return eval_void();
                }
                EvalValue current = so->fields[fidx];   /* moves out */
                if (op == TOKEN_PLUS_EQ &&
                    (current.type == EVAL_VAL_STRING || val.type == EVAL_VAL_STRING)) {
                    char* ls = eval_value_to_string(current);
                    char* rs = eval_value_to_string(val);
                    size_t len = strlen(ls) + strlen(rs) + 1;
                    char* cat = malloc(len);
                    if (cat) { strcpy(cat, ls); strcat(cat, rs); }
                    free(ls); free(rs);
                    eval_value_free(current); eval_value_free(val);
                    if (!cat) { *sig = EVAL_SIG_ERROR; return eval_error(); }
                    so->fields[fidx] = eval_string_take(cat);
                    return eval_void();
                }
                coerce_numeric(&current, &val);
                if (current.type == EVAL_VAL_INT && val.type == EVAL_VAL_INT) {
                    long long r = op == TOKEN_PLUS_EQ
                        ? current.as.i + val.as.i
                        : current.as.i - val.as.i;
                    eval_value_free(current); eval_value_free(val);
                    so->fields[fidx] = eval_int(r);
                    return eval_void();
                }
                if (current.type == EVAL_VAL_FLOAT && val.type == EVAL_VAL_FLOAT) {
                    double r = op == TOKEN_PLUS_EQ
                        ? current.as.f + val.as.f
                        : current.as.f - val.as.f;
                    eval_value_free(current); eval_value_free(val);
                    so->fields[fidx] = eval_float(r);
                    return eval_void();
                }
                eval_value_free(current); eval_value_free(val);
                RUNTIME_ERROR("unsupported operand types for compound assignment");
                *sig = EVAL_SIG_ERROR;
                return eval_error();
            }
            RUNTIME_ERROR("unsupported assignment target");
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }

        case AST_ASSIGN_STMT: {
            ASTAssignStmt* as = (ASTAssignStmt*)node;
            EvalValue val = eval_expression(as->value, env, sig);
            if (*sig != EVAL_SIG_NONE) return val;

            if (as->op_type == TOKEN_PLUS_EQ || as->op_type == TOKEN_MINUS_EQ) {
                EvalValue current;
                if (!eval_env_get(env, as->name, &current)) {
                    RUNTIME_ERROR("undefined variable '%s'", as->name);
                    eval_value_free(val);
                    *sig = EVAL_SIG_ERROR; return eval_error();
                }
                coerce_numeric(&current, &val);
                EvalValue result;
                if (current.type == EVAL_VAL_INT && val.type == EVAL_VAL_INT) {
                    result = eval_int(as->op_type == TOKEN_PLUS_EQ
                        ? current.as.i + val.as.i
                        : current.as.i - val.as.i);
                } else if (current.type == EVAL_VAL_FLOAT || val.type == EVAL_VAL_FLOAT) {
                    double l = current.type == EVAL_VAL_INT ? (double)current.as.i : current.as.f;
                    double r = val.type == EVAL_VAL_INT ? (double)val.as.i : val.as.f;
                    result = eval_float(as->op_type == TOKEN_PLUS_EQ ? l + r : l - r);
                } else if (current.type == EVAL_VAL_STRING && as->op_type == TOKEN_PLUS_EQ) {
                    char* ls = eval_value_to_string(current);
                    char* rs = eval_value_to_string(val);
                    size_t len = strlen(ls) + strlen(rs) + 1;
                    char* cat = malloc(len);
                    if (cat) { strcpy(cat, ls); strcat(cat, rs); }
                    free(ls); free(rs);
                    eval_value_free(current); eval_value_free(val);
                    if (!eval_env_set(env, as->name, eval_string_take(cat ? cat : strdup(""))))
                        eval_env_define(env, as->name, eval_string(cat ? cat : ""));
                    return eval_void();
                } else {
                    eval_value_free(current); eval_value_free(val);
                    RUNTIME_ERROR("unsupported operand types for compound assignment");
                    *sig = EVAL_SIG_ERROR; return eval_error();
                }
                eval_value_free(current); eval_value_free(val);
                if (!eval_env_set(env, as->name, result))
                    eval_env_define(env, as->name, result);
                return eval_void();
            }

            /* Simple assignment. */
            if (!eval_env_set(env, as->name, val)) {
                /* Not found anywhere up the chain — define in current frame
                 * (matches runtime behaviour of assignment to undeclared). */
                eval_env_define(env, as->name, val);
            }
            return eval_void();
        }

        case AST_CALL_STMT: {
            ASTCallStmt* cs = (ASTCallStmt*)node;
            /* 2.8.0 (FU1): statement-position constructor call —
             * `Some(5);`. Stamped by the semantic pass in eval mode
             * (codegen parity: the 2.7.0 statement-ctor fix). */
            if (cs->base.sema_enum_name && cs->base.sema_variant_index >= 0) {
                EvalValue result = eval_enum_ctor_call(cs->base.sema_variant_index,
                                                       cs->name, cs->args,
                                                       cs->arg_count, env, sig);
                eval_value_free(result);
                return eval_void();
            }
            EvalValue result = eval_call(cs->name, cs->args, cs->arg_count, env, sig, node->line);
            eval_value_free(result);
            return eval_void();
        }

        case AST_MEMBER_CALL: {
            /* 2.6.0 (FU5): statement-position module member calls work in
             * eval/REPL — same prefixed-name routing as the expression
             * case. 2.10.0 (FU-vmc): value receivers (structs/arrays)
             * dispatch to their methods here too, sharing the expression
             * path via eval_expression on the member-call node. */
            ASTMemberCall* mc = (ASTMemberCall*)node;
            if (mc->object && mc->object->type == AST_IDENTIFIER) {
                const char* alias = ((ASTIdentifier*)mc->object)->name;
                const char* prefixed = eval_module_prefixed_name(alias, mc->member_name);
                /* 2.7.0 (pub step 2): non-pub module functions are private. */
                if (eval_member_is_private(prefixed)) {
                    eval_report_private_member(alias, mc->member_name, node->line);
                    *sig = EVAL_SIG_ERROR;
                    return eval_error();
                }
                EvalEnv* closure = NULL;
                if (eval_env_find_fn(env, prefixed, &closure)) {
                    EvalValue result = eval_call(prefixed, mc->args, mc->arg_count, env, sig, node->line);
                    eval_value_free(result);
                    return eval_void();
                }
                /* Not a module member — fall through to value-method
                 * dispatch on the receiver (struct method, array builtin
                 * or a private-member error), mirroring the expression
                 * case below. */
            }
            EvalValue result = eval_expression(node, env, sig);
            eval_value_free(result);
            return eval_void();
        }

        case AST_IF_STMT: {
            ASTIfStmt* is = (ASTIfStmt*)node;
            EvalValue cond = eval_expression(is->condition, env, sig);
            if (*sig != EVAL_SIG_NONE) return cond;
            int truthy = eval_is_truthy(cond);
            eval_value_free(cond);
            if (truthy) return eval_statement(is->then_branch, env, sig);
            if (is->else_branch) return eval_statement(is->else_branch, env, sig);
            return eval_void();
        }

        case AST_WHILE_STMT: {
            ASTWhileStmt* ws = (ASTWhileStmt*)node;
            for (;;) {
                EvalValue cond = eval_expression(ws->condition, env, sig);
                if (*sig != EVAL_SIG_NONE) return cond;
                int truthy = eval_is_truthy(cond);
                eval_value_free(cond);
                if (!truthy) break;

                EvalValue body_val = eval_statement(ws->body, env, sig);
                eval_value_free(body_val);
                if (*sig == EVAL_SIG_BREAK)    { *sig = EVAL_SIG_NONE; break; }
                if (*sig == EVAL_SIG_CONTINUE) { *sig = EVAL_SIG_NONE; continue; }
                if (*sig != EVAL_SIG_NONE)     break;
            }
            return eval_void();
        }

        case AST_FOR_STMT: {
            ASTForStmt* fs = (ASTForStmt*)node;
            EvalEnv* for_frame = eval_env_new(env);
            if (fs->initializer) eval_statement(fs->initializer, for_frame, sig);
            if (*sig != EVAL_SIG_NONE) { eval_env_free(for_frame); return eval_void(); }

            for (;;) {
                if (!fs->condition) break;
                EvalValue cond = eval_expression(fs->condition, for_frame, sig);
                if (*sig != EVAL_SIG_NONE) { eval_value_free(cond); break; }
                int truthy = eval_is_truthy(cond);
                eval_value_free(cond);
                if (!truthy) break;

                EvalValue body_val = eval_statement(fs->body, for_frame, sig);
                eval_value_free(body_val);
                if (*sig == EVAL_SIG_BREAK)    { *sig = EVAL_SIG_NONE; break; }
                if (*sig == EVAL_SIG_CONTINUE) { *sig = EVAL_SIG_NONE; /* fall to increment */ }
                else if (*sig != EVAL_SIG_NONE) break;

                if (fs->increment) {
                    EvalValue inc = eval_statement(fs->increment, for_frame, sig);
                    eval_value_free(inc);
                    if (*sig != EVAL_SIG_NONE) break;
                }
            }
            eval_env_free(for_frame);
            return eval_void();
        }

        case AST_RETURN_STMT: {
            ASTReturnStmt* rs = (ASTReturnStmt*)node;
            EvalValue val = rs->expression
                ? eval_expression(rs->expression, env, sig)
                : eval_void();
            if (*sig == EVAL_SIG_NONE) *sig = EVAL_SIG_RETURN;
            return val;
        }

        case AST_BREAK_STMT:
            *sig = EVAL_SIG_BREAK;
            return eval_void();

        case AST_CONTINUE_STMT:
            *sig = EVAL_SIG_CONTINUE;
            return eval_void();

        case AST_IMPORT:
            /* Imports are resolved before eval_program is called; nothing
             * to do here. */
            return eval_void();

        case AST_MATCH_STMT:
            /* 2.8.0 (FU1): match statements evaluate in the interpreter
             * (was a silent no-op before 2.8.0 — SPEC §10.7 parity with
             * `lamo run`). Statement position discards the arm value. */
            return eval_match_value((ASTMatchStmt*)node, env, sig, 0);

        default:
            /* Try as expression-statement (e.g. function call as expression). */
            return eval_expression(node, env, sig);
    }
}

/* ── Program entry point ──────────────────────────────────────────────── */

int eval_program(ASTProgram* program, EvalEnv* env) {
    /* Pre-register all top-level functions so forward calls work, and
     * all enums so variants/ctors resolve before their declaration line
     * (2.8.0 FU1 — hoisting parity with functions). 2.10.0: structs and
     * impl blocks register the same way — struct literals, field access
     * and method calls (trait impls included) resolve before their
     * declaration line, mirroring the compiler's hoisted registries. */
    for (ASTNode* n = program->declarations; n; n = n->next) {
        if (n->type == AST_FN_DECL) {
            ASTFnDecl* fn = (ASTFnDecl*)n;
            eval_env_define_fn(env, fn->name, fn, env);
        } else if (n->type == AST_ENUM_DECL) {
            eval_register_enum_decl((ASTEnumDecl*)n);
        } else if (n->type == AST_STRUCT_DECL) {
            eval_register_struct_decl((ASTStructDecl*)n);
        } else if (n->type == AST_IMPL_DECL) {
            eval_register_impl_decl((ASTImplDecl*)n);
        }
    }

    /* Execute top-level statements in order. */
    for (ASTNode* n = program->declarations; n; n = n->next) {
        if (n->type == AST_FN_DECL) continue; /* already registered */
        if (n->type == AST_ENUM_DECL) continue; /* already registered (2.8.0) */
        if (n->type == AST_STRUCT_DECL) continue; /* already registered (2.10.0) */
        if (n->type == AST_IMPL_DECL) continue; /* already registered (2.10.0) */
        if (n->type == AST_TRAIT_DECL) continue; /* no runtime effect (2.9.0) */
        EvalSignal sig = EVAL_SIG_NONE;
        EvalValue result = eval_statement(n, env, &sig);
        eval_value_free(result);
        if (sig == EVAL_SIG_ERROR) return 0;
        if (sig == EVAL_SIG_RETURN) break; /* top-level return exits program */
    }
    return 1;
}

/* 2.6.0 (FU5): load an already-loaded + renamed module program into the
 * REPL environment (SPEC §10.7). Functions are registered under their
 * (prefixed) names; top-level lets execute so module globals exist.
 * Imports inside the module were resolved by the caller's loader pass.
 * Returns 0 on a module-level runtime error, 1 on success. */
int eval_load_module_program(ASTProgram* program, EvalEnv* env) {
    /* 2.7.0 (pub step 2): record non-pub top-level members as private
     * so the lookups above reject them. Declaration names are ALREADY
     * prefixed by the loader (lamo_mod_<alias>__<name>). */
    for (ASTNode* n = program->declarations; n; n = n->next) {
        if (n->type == AST_FN_DECL || n->type == AST_VAR_DECL) {
            if (!n->is_pub) {
                const char* renamed = (n->type == AST_FN_DECL)
                    ? ((ASTFnDecl*)n)->name
                    : ((ASTVarDecl*)n)->name;
                eval_register_private_member(renamed);
            }
        }
    }
    for (ASTNode* n = program->declarations; n; n = n->next) {
        if (n->type == AST_FN_DECL) {
            ASTFnDecl* fn = (ASTFnDecl*)n;
            eval_env_define_fn(env, fn->name, fn, env);
        } else if (n->type == AST_STRUCT_DECL) {
            /* 2.10.0 (FU-vmc): module structs + impls register so
             * literals, fields and methods work through module aliases. */
            eval_register_struct_decl((ASTStructDecl*)n);
        } else if (n->type == AST_IMPL_DECL) {
            eval_register_impl_decl((ASTImplDecl*)n);
        }
    }
    for (ASTNode* n = program->declarations; n; n = n->next) {
        if (n->type == AST_FN_DECL) continue; /* registered above */
        if (n->type == AST_IMPORT) continue;  /* resolved by the loader */
        if (n->type == AST_STRUCT_DECL) continue; /* registered above (2.10.0) */
        if (n->type == AST_IMPL_DECL) continue;   /* registered above (2.10.0) */
        if (n->type == AST_TRAIT_DECL) continue;  /* no runtime effect (2.9.0) */
        EvalSignal sig = EVAL_SIG_NONE;
        EvalValue result = eval_statement(n, env, &sig);
        eval_value_free(result);
        if (sig == EVAL_SIG_ERROR) return 0;
    }
    return 1;
}
