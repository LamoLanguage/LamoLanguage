#ifndef LAMO_EVAL_H
#define LAMO_EVAL_H

/*
 * eval.h — AST interpreter for Lamo
 *
 * Provides `lamo eval <file>` and the upcoming REPL loop. Runs entirely
 * inside the compiler process — no C code is generated, no GCC is invoked.
 *
 * Design notes:
 *   - EvalValue is independent of the runtime's LamoValue; that type exists
 *     to be emitted as source text, this one is used directly in C.
 *   - Control flow (return, break, continue) is signalled via EvalSignal so
 *     the eval functions never call setjmp/longjmp.
 *   - All memory is owned by EvalEnv frames or by EvalValue strings (strdup'd
 *     from the AST or produced by operations). Callers must call
 *     eval_value_free() on values they receive that are no longer needed.
 */

#include "ast/ast.h"

/* ── Value ─────────────────────────────────────────────────────────────── */

typedef enum {
    EVAL_VAL_INT,
    EVAL_VAL_FLOAT,
    EVAL_VAL_STRING,
    EVAL_VAL_BOOL,
    EVAL_VAL_VOID,    /* result of a statement or a bare return; */
    EVAL_VAL_ERROR    /* propagated runtime error sentinel */
} EvalValueType;

typedef struct {
    EvalValueType type;
    union {
        long long  i;   /* EVAL_VAL_INT  */
        double     f;   /* EVAL_VAL_FLOAT */
        char*      s;   /* EVAL_VAL_STRING (heap-allocated, owned) */
        int        b;   /* EVAL_VAL_BOOL  */
    } as;
} EvalValue;

/* Convenience constructors */
EvalValue eval_int(long long i);
EvalValue eval_float(double f);
EvalValue eval_string(const char* s);   /* strdup's s */
EvalValue eval_string_take(char* s);    /* takes ownership */
EvalValue eval_bool(int b);
EvalValue eval_void(void);
EvalValue eval_error(void);

/* Free any heap resources held by a value (only strings). Safe to call on
 * any EvalValue including VOID, INT, etc. */
void eval_value_free(EvalValue v);

/* ── Signal ─────────────────────────────────────────────────────────────── */

/* Returned alongside an EvalValue to communicate control-flow intent. */
typedef enum {
    EVAL_SIG_NONE,      /* normal execution */
    EVAL_SIG_RETURN,    /* fn returned; value is the return value */
    EVAL_SIG_BREAK,
    EVAL_SIG_CONTINUE,
    EVAL_SIG_ERROR      /* runtime error; execution should unwind */
} EvalSignal;

/* ── Environment ────────────────────────────────────────────────────────── */

typedef struct EvalEnv EvalEnv;

EvalEnv* eval_env_new(EvalEnv* parent);
void eval_env_free(EvalEnv* env);

/* Returns 1 on success, 0 if the name is already defined in this frame. */
int eval_env_define(EvalEnv* env, const char* name, EvalValue value);

/* Returns 1 if found (and sets *out), 0 if not found. */
int eval_env_get(EvalEnv* env, const char* name, EvalValue* out);

/* Updates the binding in the nearest frame that owns it.
 * Returns 1 on success, 0 if name is not defined anywhere in the chain. */
int eval_env_set(EvalEnv* env, const char* name, EvalValue value);

/* ── Entry points ───────────────────────────────────────────────────────── */

/*
 * eval_program: interpret an ASTProgram in env.
 * Returns 0 on runtime error, 1 on success.
 * The program's functions are registered into env before execution so
 * forward calls within the same file work.
 */
int eval_program(ASTProgram* program, EvalEnv* env);

/*
 * eval_expression / eval_statement: lower-level entry points used by the
 * REPL and tests. Both write the signal into *sig.
 */
EvalValue eval_expression(ASTNode* node, EvalEnv* env, EvalSignal* sig);
EvalValue eval_statement(ASTNode* node, EvalEnv* env, EvalSignal* sig);

/* Convert an EvalValue to a display string (caller frees). */
char* eval_value_to_string(EvalValue v);

#endif /* LAMO_EVAL_H */
