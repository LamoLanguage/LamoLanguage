#ifndef AST_H
#define AST_H

#include "lexer.h"
#include <stddef.h>

// Enumeração dos tipos de nós da AST
typedef enum {
    AST_PROGRAM,
    AST_VAR_DECL,
    AST_FN_DECL,
    AST_BLOCK,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_FOR_STMT,
    AST_RETURN_STMT,
    AST_BREAK_STMT,         // break;  (somente válido dentro de while/for)
    AST_CONTINUE_STMT,      // continue; (somente válido dentro de while/for)
    AST_ASSIGN_STMT,
    AST_CALL_STMT,
    AST_BINARY_EXPR,
    AST_UNARY_EXPR,
    AST_INT_LITERAL,
    AST_FLOAT_LITERAL,
    AST_STRING_LITERAL,
    AST_BOOL_LITERAL,
    AST_IDENTIFIER,
    AST_CALL_EXPR,
    AST_GROUPING_EXPR,
    AST_IMPORT,             // import "path";  (carregado por lamo_v2.c)
    /* Sprint 3: arrays. AST_ARRAY_LITERAL is `[1, 2, 3]`; AST_INDEX_EXPR
     * is `arr[i]`; AST_PROP_EXPR is `arr.len` (the only supported
     * property today, but the node is generic so we can add .push()
     * etc. later without changing the AST shape). */
    AST_ARRAY_LITERAL,
    AST_INDEX_EXPR,
    AST_PROP_EXPR,
    /* Sprint 4: module member call — `module.member(args)`. `object` is
     * the module alias (always AST_IDENTIFIER today), `member_name` is
     * the function name being called inside the module, `args` /
     * `arg_count` are the call arguments. Resolved by the semantic pass
     * against the module registry kept in CompilationState; codegen emits
     * a call to lamo_mod_<alias>__<member_name>. Lives in both statement
     * and expression positions. */
    AST_MEMBER_CALL,
    /* Phase 2: structs / methods / enums / match.
     *
     * AST_STRUCT_DECL    - `struct Name { field: type, ... }`
     * AST_IMPL_DECL      - `impl Type { fn method(...) {...} ... }`
     * AST_ENUM_DECL      - `enum Name { Variant, ... }`
     * AST_MATCH_STMT     - `match expr { Pat => body, ... }`
     * AST_STRUCT_LITERAL - `Name { field: value, ... }`
     *
     * Field access (no parens) reuses AST_PROP_EXPR so the existing
     * `arr.len` shape keeps working. Method calls with parens reuse
     * AST_MEMBER_CALL so the existing `module.fn(args)` shape keeps
     * working - the semantic pass distinguishes module calls from
     * value method calls by looking at the object's inferred type. */
    AST_STRUCT_DECL,
    AST_IMPL_DECL,
    AST_ENUM_DECL,
    AST_MATCH_STMT,
    AST_STRUCT_LITERAL,
    /* Phase 2: place-assignment statement - `arr[i] = value;` and
     * `obj.field = value;`. `target` is an AST_INDEX_EXPR or AST_PROP_EXPR
     * representing the lvalue, `value` is the RHS, `op_type` is `=`, `+=`,
     * or `-=` (the latter two desugar to read-modify-write at codegen
     * time). This avoids adding two separate node types for index-assign
     * and field-assign since they share the same shape. */
    AST_PLACE_ASSIGN_STMT
} ASTNodeType;

// Estrutura base para todos os nós da AST
typedef struct ASTNode {
    ASTNodeType type;
    int line;
    int column;
    struct ASTNode* next;
    // Bug #5 fix: path do arquivo de origem deste nó. Setado pelo parser no
    // momento de criação (parser_init_with_file). O semântico usa isso para
    // reportar erros com o arquivo correto, mesmo em compilações multi-arquivo
    // (programa principal + imports). Pode ser NULL quando o nó é sintético
    // (ex.: nó criado pelo codegen sem passar pelo parser).
    const char* file_path;
    /* Phase 2: optional struct-type annotation populated by the semantic
     * pass and consumed by codegen. When non-NULL, this expression's
     * inferred type is a struct with this name (borrowed pointer into the
     * matching ASTStructDecl->name — NOT owned, NOT freed by ast_free).
     * Used to resolve field access (AST_PROP_EXPR), method calls
     * (AST_MEMBER_CALL on a struct value), and struct literals. */
    const char* sema_struct_name;
    /* Generics PR 2: optional NORMALIZED full type annotation populated by
     * the semantic pass and consumed by codegen/backend alignment work.
     * Examples: "int", "array<int>", "pair<int,string>". Borrowed pointer
     * into the semantic intern table — NOT owned, NOT freed by ast_free,
     * valid until process exit (the compiler is short-lived). NULL when
     * unknown/uneeded. Unlike sema_struct_name this can carry ANY type
     * including nested generics; see semantic.c::lamo_intern_type(). */
    const char* sema_full_type;
} ASTNode;

typedef struct {
    ASTNode base;
    char* name;
    struct ASTNode* initializer;
    /* Sprint 3: optional type annotation (e.g. `let x: int = 5;`).
     * NULL when no annotation was given. The string is owned by the
     * AST and freed in ast_free(). Possible values: "int", "float",
     * "string", "bool". The semantic pass validates that the
     * initializer's inferred type matches; the codegen ignores it
     * (all Lamo values are LamoValue at runtime). */
    char* type_annotation;
} ASTVarDecl;

typedef struct {
    ASTNode base;
    char* name;
    char** params;
    int param_count;
    /* Sprint 3: optional per-parameter type annotations. NULL when
     * no annotations were given. params_types[i] is the annotation
     * for params[i], or NULL if that parameter had no annotation.
     * The array is owned by the AST and freed in ast_free(). */
    char** param_types;
    /* Sprint 3: optional return-type annotation (e.g. `fn f() -> int`).
     * NULL when no annotation was given. */
    char* return_type_annotation;
    /* Generics PR 2: optional type parameter list `fn id<T>(x: T) -> T`.
     * type_params[i] / param_constraints[i] are strdup'd owned strings
     * (constraints may be NULL). When type_param_count == 0 the function
     * is non-generic (the legacy case). The strings are freed in
     * ast_free(). */
    char** type_params;
    char** type_param_constraints;
    int type_param_count;
    struct ASTNode* body;
} ASTFnDecl;

typedef struct {
    ASTNode base;
    struct ASTNode* statements;
} ASTBlock;

typedef struct {
    ASTNode base;
    struct ASTNode* condition;
    struct ASTNode* then_branch;
    struct ASTNode* else_branch;
} ASTIfStmt;

typedef struct {
    ASTNode base;
    struct ASTNode* condition;
    struct ASTNode* body;
} ASTWhileStmt;

typedef struct {
    ASTNode base;
    struct ASTNode* initializer;
    struct ASTNode* condition;
    struct ASTNode* increment;
    struct ASTNode* body;
} ASTForStmt;

typedef struct {
    ASTNode base;
    struct ASTNode* expression;
} ASTReturnStmt;

typedef struct {
    ASTNode base;
    char* name;
    struct ASTNode* value;
    LamoTokenType op_type;
} ASTAssignStmt;

typedef struct {
    ASTNode base;
    char* name;
    /* Generics PR 2: same explicit-type-argument support as ASTCallExpr,
     * for statement-position calls like `swap<int, int>(a, b);`. */
    char** type_args;
    int type_arg_count;
    struct ASTNode** args;
    int arg_count;
} ASTCallStmt;

typedef struct {
    ASTNode base;
    struct ASTNode* left;
    LamoTokenType operator;
    struct ASTNode* right;
} ASTBinaryExpr;

typedef struct {
    ASTNode base;
    LamoTokenType operator;
    struct ASTNode* right;
} ASTUnaryExpr;

typedef struct {
    ASTNode base;
    long long value;
} ASTIntLiteral;

typedef struct {
    ASTNode base;
    double value;
} ASTFloatLiteral;

typedef struct {
    ASTNode base;
    char* value;
} ASTStringLiteral;

typedef struct {
    ASTNode base;
    int value;
} ASTBoolLiteral;

typedef struct {
    ASTNode base;
    char* name;
} ASTIdentifier;

typedef struct {
    ASTNode base;
    char* name;
    /* Generics PR 2 / RFC §4.5: optional explicit type arguments at the
     * call site — `f<int>(5)` or `map<int, string>(xs, f)`. Strings are
     * strdup'd and freed in ast_free(). type_arg_count == 0 for the
     * common inferred case. Filled by the parser only when lookahead
     * confirms `< ... > (` so `a < b` comparisons never misparse. */
    char** type_args;
    int type_arg_count;
    struct ASTNode** args;
    int arg_count;
} ASTCallExpr;

typedef struct {
    ASTNode base;
    struct ASTNode* expression;
} ASTGroupingExpr;

typedef struct {
    ASTNode base;
    char* path;            // caminho bruto exatamente como apareceu entre aspas
    /* Sprint 4: optional module alias. When non-NULL, the imported file's
     * top-level declarations are renamed to `lamo_mod_<alias>__<name>` and
     * accessed through `<alias>.<name>` syntax (AST_MEMBER_CALL). When NULL,
     * the legacy behavior applies — everything is merged into the global
     * namespace. */
    char* alias;
} ASTImport;

/* Sprint 3: array literal — `[expr, expr, ...]`. elements is owned by
 * the AST (heap-allocated array of ASTNode*). */
typedef struct {
    ASTNode base;
    struct ASTNode** elements;
    int element_count;
} ASTArrayLiteral;

/* Sprint 3: index expression — `array[index]`. Both `array` and `index`
 * are owned by the AST. */
typedef struct {
    ASTNode base;
    struct ASTNode* array;
    struct ASTNode* index;
} ASTIndexExpr;

/* Sprint 3: property access — `obj.prop_name`. Currently prop_name is
 * restricted to "len" by the semantic pass, but the AST is generic. */
typedef struct {
    ASTNode base;
    struct ASTNode* object;
    char* prop_name;
} ASTPropExpr;

/* Sprint 4: module member call — `module.member(args)`. `object` is
 * conventionally an AST_IDENTIFIER naming a module alias declared by an
 * earlier `import "..." as alias;`. `member_name` is the function inside
 * the module. `args` / `arg_count` mirror ASTCallStmt.
 *
 * The same node type is used in both statement and expression positions;
 * codegen and the interpreter dispatch on context. This avoids splitting
 * the node into AST_MEMBER_CALL_STMT / AST_MEMBER_CALL_EXPR the way the
 * legacy AST_CALL_STMT / AST_CALL_EXPR pair was split — a lesson learned
 * the hard way from the call-stmt/call-expr duplication. */
typedef struct {
    ASTNode base;
    struct ASTNode* object;       /* typically AST_IDENTIFIER(name = alias) */
    char* member_name;            /* e.g. "sqrt" in `math.sqrt(x)` */
    struct ASTNode** args;
    int arg_count;
} ASTMemberCall;

/* Phase 2: struct declaration.
 *   struct Player { name: string; hp: int; level: int; }
 * field_names[i] / field_types[i] are owned by the AST (strdup'd).
 * field_types[i] may be NULL when the field has no annotation.
 *
 * Generics PR 1: type_params / type_param_count carry the optional
 * `<T, K, V>` parameter list after the struct name. When type_param_count
 * == 0 the struct is non-generic (the legacy case). Type parameters can
 * be used as field types; the semantic pass validates that any non-builtin,
 * non-struct field type is one of these names. */
typedef struct {
    ASTNode base;
    char* name;
    char** field_names;
    char** field_types;
    int field_count;
    char** type_params;
    /* PR 6: optional per-parameter constraint names ("Ord", "Eq", ...
     * or NULL when unconstrained). Aligned 1:1 with type_params; both
     * arrays are owned and freed in ast_free(). */
    char** type_param_constraints;
    int type_param_count;
} ASTStructDecl;

/* Phase 2: impl block.
 *   impl Player { fn damage(amount: int) { self.hp -= amount; } ... }
 * struct_name is owned (strdup'd). methods is a linked list of AST_FN_DECL
 * nodes (linked via ->next); we take ownership. */
typedef struct {
    ASTNode base;
    char* struct_name;
    struct ASTNode* methods;
    /* RFC §4.4: generic impls declare their own type parameters and
     * echo them on the struct name:
     *   impl<T> Stack<T> { fn push(self, x: T) { ... } }
     * type_params[i] / type_args[i] are strdup'd owned strings (freed in
     * ast_free()). When both counts are 0 the impl is non-generic
     * (legacy). Validation of the echo (type_arg_count ==
     * type_param_count, names match) happens in the semantic pass with
     * better messages than the parser could produce. */
    char** type_params;
    int type_param_count;
    char** type_args;
    int type_arg_count;
} ASTImplDecl;

/* Phase 2: enum declaration.
 *   enum Color { Red; Green; Blue; }
 * variants[i] is owned (strdup'd). At runtime each variant is an int
 * constant equal to its index. */
typedef struct {
    ASTNode base;
    char* name;
    char** variants;
    int variant_count;
} ASTEnumDecl;

/* Phase 2: match statement.
 *   match color { Red => print("red"); _ => print("other"); }
 * patterns[i] is the variant name (or "_" for wildcard), strdup'd.
 * pattern_is_wildcard[i] is 1 for "_", 0 otherwise.
 * bodies[i] is a statement node (owned). */
typedef struct {
    ASTNode base;
    struct ASTNode* scrutinee;
    char** patterns;
    int* pattern_is_wildcard;
    struct ASTNode** bodies;
    int arm_count;
} ASTMatchStmt;

/* Phase 2: struct literal.
 *   Player { name: "Arthur"; hp: 100; level: 1; }
 * struct_name is owned (strdup'd). field_names[i] / field_values[i]
 * are owned; the AST frees them.
 *
 * Generics PR 1: type_args / type_arg_count carry the optional
 * `<int, string>` argument list after the struct name. When the named
 * struct is generic, the semantic pass validates that type_arg_count
 * matches the declaration's type_param_count. The runtime layout is
 * the same regardless of type args (all fields are LamoValue), so the
 * codegen ignores type_args — they exist purely for compile-time type
 * checking. */
typedef struct {
    ASTNode base;
    char* struct_name;
    char** field_names;
    struct ASTNode** field_values;
    int field_count;
    char** type_args;
    int type_arg_count;
} ASTStructLiteral;

/* Phase 2: place-assignment statement - `arr[i] = value;` or
 * `obj.field = value;`. `target` is an AST_INDEX_EXPR or AST_PROP_EXPR
 * representing the lvalue. `value` is the RHS. `op_type` is `=`, `+=`,
 * or `-=`. For `+=`/`-=`, codegen desugars to read-modify-write using
 * the appropriate getter/setter. */
typedef struct {
    ASTNode base;
    struct ASTNode* target;   /* AST_INDEX_EXPR or AST_PROP_EXPR */
    struct ASTNode* value;
    LamoTokenType op_type;
} ASTPlaceAssignStmt;

typedef struct {
    ASTNode base;
    struct ASTNode* declarations;
} ASTProgram;

ASTNode* ast_new_node(ASTNodeType type, size_t size, int line, int column);
// Bug #5 fix: setters/getters para o "file path default" aplicado a cada nó
// criado pelo parser. O parser chama ast_set_default_file_path() antes de
// começar a parsear um arquivo; ast_new_node() copia o valor para cada nó.
// O ponteiro NÃO é owned pela AST — o caller precisa manter a string viva
// enquanto a AST existir.
void ast_set_default_file_path(const char* path);
const char* ast_get_default_file_path(void);
ASTProgram* ast_new_program();
ASTVarDecl* ast_new_var_decl(char* name, ASTNode* initializer, int line, int column);
ASTVarDecl* ast_new_var_decl_typed(char* name, ASTNode* initializer, char* type_annotation, int line, int column);
ASTFnDecl* ast_new_fn_decl(char* name, char** params, int param_count, ASTNode* body, int line, int column);
ASTFnDecl* ast_new_fn_decl_typed(char* name, char** params, char** param_types, int param_count, char* return_type_annotation, ASTNode* body, int line, int column);
/* Generics PR 2: full constructor with an optional type parameter list and
 * per-parameter constraints. type_params may be NULL when tp_count == 0.
 * Constraints may be NULL entries (unconstrained parameters). All strings
 * are strdup'd here; caller retains input ownership. */
ASTFnDecl* ast_new_fn_decl_generic(char* name, char** type_params, char** constraints, int tp_count,
                                   char** params, char** param_types, int param_count,
                                   char* return_type_annotation, ASTNode* body, int line, int column);
/* Generics PR 2: call-site constructors carrying optional explicit type
 * arguments (`f<int>(x)`). Legacy ctors delegate with count 0. The arrays
 * are copied (strdup each entry); caller retains input ownership. */
ASTCallExpr* ast_new_call_expr_typed(char* name, char** type_args, int type_arg_count, ASTNode** args, int arg_count, int line, int column);
ASTCallStmt* ast_new_call_stmt_typed(char* name, char** type_args, int type_arg_count, ASTNode** args, int arg_count, int line, int column);
ASTBlock* ast_new_block(ASTNode* statements, int line, int column);
ASTIfStmt* ast_new_if_stmt(ASTNode* condition, ASTNode* then_branch, ASTNode* else_branch, int line, int column);
ASTWhileStmt* ast_new_while_stmt(ASTNode* condition, ASTNode* body, int line, int column);
ASTForStmt* ast_new_for_stmt(ASTNode* initializer, ASTNode* condition, ASTNode* increment, ASTNode* body, int line, int column);
ASTReturnStmt* ast_new_return_stmt(ASTNode* expression, int line, int column);
// break; e continue; — sem campos além da base ASTNode. Os construtores
// retornam ASTNode* direto porque não há struct derivada.
ASTNode* ast_new_break_stmt(int line, int column);
ASTNode* ast_new_continue_stmt(int line, int column);
ASTAssignStmt* ast_new_assign_stmt(char* name, ASTNode* value, LamoTokenType op_type, int line, int column);
ASTCallStmt* ast_new_call_stmt(char* name, ASTNode** args, int arg_count, int line, int column);
ASTBinaryExpr* ast_new_binary_expr(ASTNode* left, LamoTokenType operator, ASTNode* right, int line, int column);
ASTUnaryExpr* ast_new_unary_expr(LamoTokenType operator, ASTNode* right, int line, int column);
ASTIntLiteral* ast_new_int_literal(long long value, int line, int column);
ASTFloatLiteral* ast_new_float_literal(double value, int line, int column);
ASTStringLiteral* ast_new_string_literal(char* value, int line, int column);
ASTBoolLiteral* ast_new_bool_literal(int value, int line, int column);
ASTIdentifier* ast_new_identifier(char* name, int line, int column);
ASTCallExpr* ast_new_call_expr(char* name, ASTNode** args, int arg_count, int line, int column);
ASTGroupingExpr* ast_new_grouping_expr(ASTNode* expression, int line, int column);
ASTImport* ast_new_import_decl(char* path, int line, int column);
/* Sprint 4: variant of ast_new_import_decl that also takes a module alias.
 * `alias` may be NULL — equivalent to the legacy constructor. The alias
 * string is owned by the AST after this call (strdup'd). */
ASTImport* ast_new_import_decl_aliased(char* path, char* alias, int line, int column);
ASTArrayLiteral* ast_new_array_literal(ASTNode** elements, int element_count, int line, int column);
ASTIndexExpr* ast_new_index_expr(ASTNode* array, ASTNode* index, int line, int column);
ASTPropExpr* ast_new_prop_expr(ASTNode* object, char* prop_name, int line, int column);
/* Sprint 4: module member call constructor. Takes ownership of `args`
 * (the array, not the elements — elements are still owned by their
 * constructors as usual). `member_name` is strdup'd. `object` is owned
 * by the AST and freed in ast_free(). */
ASTMemberCall* ast_new_member_call(ASTNode* object, char* member_name, ASTNode** args, int arg_count, int line, int column);

/* ─── Phase 2: structs / methods / enums / match ──────────────────── */

/* struct Name { field: type, ... }
 * `field_names` and `field_types` are arrays of strings; we strdup each
 * entry. field_types[i] may be NULL when no annotation is given for that
 * field. The caller retains ownership of the input arrays.
 *
 * Generics PR 1: type_params / type_param_count carry the optional
 * `<T, K, V>` parameter list. Pass type_param_count = 0 (and type_params
 * = NULL) for non-generic structs — the legacy case. The strings are
 * strdup'd here.
 * PR 6: type_param_constraints may be NULL (all-unconstrained) or an
 * array of type_param_count entries whose entries may be NULL. */
ASTNode* ast_new_struct_decl(char* name, char** field_names, char** field_types, int field_count,
                             char** type_params, char** type_param_constraints, int type_param_count,
                             int line, int column);

/* impl Type { fn method(...) {...} ... } - `methods` is a linked list of
 * AST_FN_DECL nodes (linked via ->next). The list is taken ownership of;
 * the caller should NOT free it. struct_name is strdup'd.
 * See ast_new_impl_decl_generic for the RFC §4.4 form. */
ASTNode* ast_new_impl_decl(char* struct_name, ASTNode* methods, int line, int column);
/* RFC §4.4: generic impl variant. Pass counts 0 + NULL arrays for the
 * legacy non-generic form. All strings are strdup'd here. */
ASTNode* ast_new_impl_decl_generic(char* struct_name,
                                   char** type_params, int type_param_count,
                                   char** type_args, int type_arg_count,
                                   ASTNode* methods, int line, int column);

/* enum Name { Variant, ... } - `variants` is an array of strings, strdup'd
 * here. The caller retains ownership of the input array. */
ASTNode* ast_new_enum_decl(char* name, char** variants, int variant_count, int line, int column);

/* match expr { Pat => body, ... }
 * `patterns` is an array of strdup'd pattern names ("_" for wildcard).
 * `pattern_is_wildcard` is an array of 0/1 (1 = wildcard "_").
 * `bodies` is an array of ASTNode* (we take ownership). All arrays have
 * arm_count entries. The caller retains ownership of the input arrays
 * (we copy/stread what we need). */
ASTNode* ast_new_match_stmt(ASTNode* scrutinee, char** patterns, int* pattern_is_wildcard, ASTNode** bodies, int arm_count, int line, int column);

/* Struct literal: Name { field: value, ... }
 * `field_names` and `field_values` are arrays of size field_count.
 * field_names[i] is strdup'd. field_values[i] is owned by the AST.
 *
 * Generics PR 1: type_args / type_arg_count carry the optional
 * `<int, string>` argument list. Pass type_arg_count = 0 (and type_args
 * = NULL) for non-generic literals. Strings are strdup'd here. */
ASTNode* ast_new_struct_literal(char* struct_name, char** field_names, ASTNode** field_values, int field_count, char** type_args, int type_arg_count, int line, int column);

/* Place-assignment: `target op= value` where target is AST_INDEX_EXPR
 * or AST_PROP_EXPR. The target and value nodes are owned by the AST. */
ASTNode* ast_new_place_assign_stmt(ASTNode* target, ASTNode* value, LamoTokenType op_type, int line, int column);

void ast_program_append(ASTProgram* destination, ASTProgram* source);

void ast_free(ASTNode* node);

#endif
