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

void eval_value_free(EvalValue v) {
    if (v.type == EVAL_VAL_STRING && v.as.s) {
        free(v.as.s);
    }
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
        case EVAL_VAL_VOID:
            return strdup("");
        case EVAL_VAL_ERROR:
            return strdup("<error>");
    }
    return strdup("");
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
                /* Return a deep copy of strings so caller can free safely. */
                if (b->value.type == EVAL_VAL_STRING) {
                    *out = eval_string(b->value.as.s);
                } else {
                    *out = b->value;
                }
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
                if (value.type == EVAL_VAL_STRING) {
                    b->value = eval_string(value.as.s);
                } else {
                    b->value = value;
                }
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
                int truthy = (right.type == EVAL_VAL_BOOL) ? right.as.b
                           : (right.type == EVAL_VAL_INT)  ? (right.as.i != 0)
                           : (right.type == EVAL_VAL_FLOAT)? (right.as.f != 0.0)
                           : (right.type == EVAL_VAL_STRING)? (right.as.s && right.as.s[0] != '\0')
                           : 0;
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
            /* Equality for bools and strings. */
            if (bin->operator == TOKEN_EQ_EQ || bin->operator == TOKEN_BANG_EQ) {
                int eq = 0;
                if (left.type == EVAL_VAL_BOOL && right.type == EVAL_VAL_BOOL)
                    eq = (left.as.b == right.as.b);
                else if (left.type == EVAL_VAL_STRING && right.type == EVAL_VAL_STRING)
                    eq = (strcmp(left.as.s, right.as.s) == 0);
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
            RUNTIME_ERROR("index operation not supported on this type");
            eval_value_free(obj); eval_value_free(index);
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }

        case AST_PROP_EXPR: {
            ASTPropExpr* pe = (ASTPropExpr*)node;
            /* Sprint 4: if object is an identifier, this might be a
             * module-qualified access (`math.PI`) from an aliased import.
             * The eval/REPL path does not load modules through the
             * CompilationState pipeline, so it has no module registry
             * and cannot resolve these. Emit a clear error instead of
             * the confusing "undefined variable 'math'" that would
             * otherwise surface from eval_expression trying to look up
             * the alias as a regular variable. */
            if (pe->object && pe->object->type == AST_IDENTIFIER) {
                RUNTIME_ERROR("module-qualified access `%s.%s` is not supported in eval/REPL mode (use `lamo run` instead)",
                              ((ASTIdentifier*)pe->object)->name, pe->prop_name);
                *sig = EVAL_SIG_ERROR;
                return eval_error();
            }
            EvalValue obj = eval_expression(pe->object, env, sig);
            if (*sig != EVAL_SIG_NONE) return obj;
            if (obj.type == EVAL_VAL_STRING && strcmp(pe->prop_name, "len") == 0) {
                long long len = (long long)strlen(obj.as.s);
                eval_value_free(obj);
                return eval_int(len);
            }
            RUNTIME_ERROR("unknown property '%s'", pe->prop_name);
            eval_value_free(obj);
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }

        case AST_MEMBER_CALL: {
            /* Sprint 4: `module.member(args)` — not supported in
             * eval/REPL because there's no module registry. Emit a
             * clear error pointing the user at `lamo run`. */
            ASTMemberCall* mc = (ASTMemberCall*)node;
            const char* alias = (mc->object && mc->object->type == AST_IDENTIFIER)
                ? ((ASTIdentifier*)mc->object)->name : "<expr>";
            RUNTIME_ERROR("module member call `%s.%s(...)` is not supported in eval/REPL mode (use `lamo run` instead)",
                          alias, mc->member_name);
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }

        case AST_ARRAY_LITERAL: {
            /* Arrays are not natively representable in EvalValue yet.
             * Return a void placeholder so programs compile without crashing.
             * TODO: add EVAL_VAL_ARRAY with a dynamic EvalValue[]. */
            return eval_void();
        }

        default:
            return eval_void();
    }
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
                (argv[0].type == EVAL_VAL_STRING) ? (argv[0].as.s && argv[0].as.s[0]) : 0;
        return eval_bool(b);
    }

    if (strcmp(name, "len") == 0 && argc == 1) {
        if (argv[0].type == EVAL_VAL_STRING)
            return eval_int((long long)strlen(argv[0].as.s));
        return eval_int(0);
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

    /* push/pop/append for arrays — no-op for now */
    if (strcmp(name, "push") == 0 || strcmp(name, "pop") == 0 ||
        strcmp(name, "append") == 0 || strcmp(name, "array") == 0) {
        return eval_void();
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
        EvalEnv* frame = eval_env_new(closure ? closure : env);
        for (int i = 0; i < fn->param_count; i++) {
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

    /* Builtin. */
    EvalValue result = eval_builtin(name, argv, argc, sig);
    for (int i = 0; i < argc; i++) eval_value_free(argv[i]);
    free(argv);
    (void)line;
    return result;
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
            EvalValue result = eval_call(cs->name, cs->args, cs->arg_count, env, sig, node->line);
            eval_value_free(result);
            return eval_void();
        }

        case AST_MEMBER_CALL: {
            /* Sprint 4: same as the expression case — emit a clear
             * error rather than trying to look up the alias as a
             * variable. */
            ASTMemberCall* mc = (ASTMemberCall*)node;
            const char* alias = (mc->object && mc->object->type == AST_IDENTIFIER)
                ? ((ASTIdentifier*)mc->object)->name : "<expr>";
            RUNTIME_ERROR("module member call `%s.%s(...)` is not supported in eval/REPL mode (use `lamo run` instead)",
                          alias, mc->member_name);
            *sig = EVAL_SIG_ERROR;
            return eval_error();
        }

        case AST_IF_STMT: {
            ASTIfStmt* is = (ASTIfStmt*)node;
            EvalValue cond = eval_expression(is->condition, env, sig);
            if (*sig != EVAL_SIG_NONE) return cond;
            int truthy = (cond.type == EVAL_VAL_BOOL)   ? cond.as.b :
                         (cond.type == EVAL_VAL_INT)    ? (cond.as.i != 0) :
                         (cond.type == EVAL_VAL_FLOAT)  ? (cond.as.f != 0.0) :
                         (cond.type == EVAL_VAL_STRING) ? (cond.as.s && cond.as.s[0]) : 0;
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
                int truthy = (cond.type == EVAL_VAL_BOOL) ? cond.as.b :
                             (cond.type == EVAL_VAL_INT)  ? (cond.as.i != 0) :
                             (cond.type == EVAL_VAL_FLOAT)? (cond.as.f != 0.0) : 0;
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
                int truthy = (cond.type == EVAL_VAL_BOOL) ? cond.as.b :
                             (cond.type == EVAL_VAL_INT)  ? (cond.as.i != 0) :
                             (cond.type == EVAL_VAL_FLOAT)? (cond.as.f != 0.0) : 0;
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

        default:
            /* Try as expression-statement (e.g. function call as expression). */
            return eval_expression(node, env, sig);
    }
}

/* ── Program entry point ──────────────────────────────────────────────── */

int eval_program(ASTProgram* program, EvalEnv* env) {
    /* Pre-register all top-level functions so forward calls work. */
    for (ASTNode* n = program->declarations; n; n = n->next) {
        if (n->type == AST_FN_DECL) {
            ASTFnDecl* fn = (ASTFnDecl*)n;
            eval_env_define_fn(env, fn->name, fn, env);
        }
    }

    /* Execute top-level statements in order. */
    for (ASTNode* n = program->declarations; n; n = n->next) {
        if (n->type == AST_FN_DECL) continue; /* already registered */
        EvalSignal sig = EVAL_SIG_NONE;
        EvalValue result = eval_statement(n, env, &sig);
        eval_value_free(result);
        if (sig == EVAL_SIG_ERROR) return 0;
        if (sig == EVAL_SIG_RETURN) break; /* top-level return exits program */
    }
    return 1;
}
