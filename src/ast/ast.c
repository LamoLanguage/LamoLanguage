#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "ast.h"

// Bug #5 fix: o parser seta esse "current file path" antes de criar nós da
// AST, e ast_new_node() copia o ponteiro para cada nó. Isso permite que o
// semântico reporte erros com o arquivo de origem correto, mesmo em
// compilações multi-arquivo (programa principal + imports). Single-threaded:
// safe.
//
// O ponteiro NÃO é owned pela AST — ele aponta para o `normalized_path` que
// já vive na CompilationState durante todo o tempo de vida da AST. Não há
// free aqui.
static const char* g_default_file_path = NULL;

void ast_set_default_file_path(const char* path) {
    g_default_file_path = path;
}

const char* ast_get_default_file_path(void) {
    return g_default_file_path;
}

ASTNode* ast_new_node(ASTNodeType type, size_t size, int line, int column) {
    ASTNode* node = (ASTNode*)malloc(size);
    if (!node) {
        perror("Failed to allocate ASTNode");
        exit(EXIT_FAILURE);
    }
    memset(node, 0, size);
    node->type = type;
    node->line = line;
    node->column = column;
    node->next = NULL;
    node->file_path = g_default_file_path;
    node->sema_struct_name = NULL;  /* Phase 2: populated by semantic pass */
    return node;
}

ASTProgram* ast_new_program() {
    ASTProgram* node = (ASTProgram*)ast_new_node(AST_PROGRAM, sizeof(ASTProgram), 0, 0);
    node->declarations = NULL;
    return node;
}

ASTVarDecl* ast_new_var_decl(char* name, ASTNode* initializer, int line, int column) {
    return ast_new_var_decl_typed(name, initializer, NULL, line, column);
}

/* Sprint 3: typed var decl. type_annotation is owned by the AST (it's
 * strdup'd here). NULL means no annotation. */
ASTVarDecl* ast_new_var_decl_typed(char* name, ASTNode* initializer, char* type_annotation, int line, int column) {
    ASTVarDecl* node = (ASTVarDecl*)ast_new_node(AST_VAR_DECL, sizeof(ASTVarDecl), line, column);
    node->name = strdup(name);
    node->initializer = initializer;
    node->type_annotation = type_annotation ? strdup(type_annotation) : NULL;
    return node;
}

ASTFnDecl* ast_new_fn_decl(char* name, char** params, int param_count, ASTNode* body, int line, int column) {
    return ast_new_fn_decl_typed(name, params, NULL, param_count, NULL, body, line, column);
}

/* Sprint 3: typed fn decl. param_types may be NULL (no annotations) or
 * an array of param_count entries, each either NULL or a heap-allocated
 * string. The array is taken ownership of (caller should NOT free it).
 * return_type_annotation is owned by the AST (strdup'd here). */
ASTFnDecl* ast_new_fn_decl_typed(char* name, char** params, char** param_types, int param_count, char* return_type_annotation, ASTNode* body, int line, int column) {
    return ast_new_fn_decl_generic(name, NULL, NULL, 0,
                                   params, param_types, param_count,
                                   return_type_annotation, body, line, column);
}

/* Generics PR 2: full fn constructor. See ast.h for the contract. */
ASTFnDecl* ast_new_fn_decl_generic(char* name, char** type_params, char** constraints, int tp_count,
                                   char** params, char** param_types, int param_count,
                                   char* return_type_annotation, ASTNode* body, int line, int column) {
    ASTFnDecl* node = (ASTFnDecl*)ast_new_node(AST_FN_DECL, sizeof(ASTFnDecl), line, column);
    node->name = strdup(name);
    node->params = params;
    node->param_types = param_types;
    node->param_count = param_count;
    node->return_type_annotation = return_type_annotation ? strdup(return_type_annotation) : NULL;
    node->type_param_count = tp_count;
    if (tp_count > 0 && type_params) {
        node->type_params = malloc(sizeof(char*) * (size_t)tp_count);
        node->type_param_constraints = malloc(sizeof(char*) * (size_t)tp_count);
        if (!node->type_params || !node->type_param_constraints) {
            perror("Failed to allocate generic fn type parameter list");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < tp_count; i++) {
            node->type_params[i] = strdup(type_params[i] ? type_params[i] : "");
            node->type_param_constraints[i] =
                (constraints && constraints[i]) ? strdup(constraints[i]) : NULL;
        }
    } else {
        node->type_params = NULL;
        node->type_param_constraints = NULL;
        node->type_param_count = 0;
    }
    node->body = body;
    return node;
}

ASTBlock* ast_new_block(ASTNode* statements, int line, int column) {
    ASTBlock* node = (ASTBlock*)ast_new_node(AST_BLOCK, sizeof(ASTBlock), line, column);
    node->statements = statements;
    return node;
}

ASTIfStmt* ast_new_if_stmt(ASTNode* condition, ASTNode* then_branch, ASTNode* else_branch, int line, int column) {
    ASTIfStmt* node = (ASTIfStmt*)ast_new_node(AST_IF_STMT, sizeof(ASTIfStmt), line, column);
    node->condition = condition;
    node->then_branch = then_branch;
    node->else_branch = else_branch;
    return node;
}

ASTWhileStmt* ast_new_while_stmt(ASTNode* condition, ASTNode* body, int line, int column) {
    ASTWhileStmt* node = (ASTWhileStmt*)ast_new_node(AST_WHILE_STMT, sizeof(ASTWhileStmt), line, column);
    node->condition = condition;
    node->body = body;
    return node;
}

ASTForStmt* ast_new_for_stmt(ASTNode* initializer, ASTNode* condition, ASTNode* increment, ASTNode* body, int line, int column) {
    ASTForStmt* node = (ASTForStmt*)ast_new_node(AST_FOR_STMT, sizeof(ASTForStmt), line, column);
    node->initializer = initializer;
    node->condition = condition;
    node->increment = increment;
    node->body = body;
    return node;
}

ASTReturnStmt* ast_new_return_stmt(ASTNode* expression, int line, int column) {
    ASTReturnStmt* node = (ASTReturnStmt*)ast_new_node(AST_RETURN_STMT, sizeof(ASTReturnStmt), line, column);
    node->expression = expression;
    return node;
}

ASTNode* ast_new_break_stmt(int line, int column) {
    return ast_new_node(AST_BREAK_STMT, sizeof(ASTNode), line, column);
}

ASTNode* ast_new_continue_stmt(int line, int column) {
    return ast_new_node(AST_CONTINUE_STMT, sizeof(ASTNode), line, column);
}

ASTAssignStmt* ast_new_assign_stmt(char* name, ASTNode* value, LamoTokenType op_type, int line, int column) {
    ASTAssignStmt* node = (ASTAssignStmt*)ast_new_node(AST_ASSIGN_STMT, sizeof(ASTAssignStmt), line, column);
    node->name = strdup(name);
    node->value = value;
    node->op_type = op_type;
    return node;
}

ASTCallStmt* ast_new_call_stmt(char* name, ASTNode** args, int arg_count, int line, int column) {
    return ast_new_call_stmt_typed(name, NULL, 0, args, arg_count, line, column);
}

/* Generics PR 2: statement-call ctor with optional explicit type args.
 * Strings in type_args are strdup'd; caller retains input ownership. */
ASTCallStmt* ast_new_call_stmt_typed(char* name, char** type_args, int type_arg_count, ASTNode** args, int arg_count, int line, int column) {
    ASTCallStmt* node = (ASTCallStmt*)ast_new_node(AST_CALL_STMT, sizeof(ASTCallStmt), line, column);
    node->name = strdup(name);
    node->type_arg_count = 0;
    node->type_args = NULL;
    if (type_arg_count > 0 && type_args) {
        node->type_args = malloc(sizeof(char*) * (size_t)type_arg_count);
        if (!node->type_args) {
            perror("Failed to allocate call type argument list");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < type_arg_count; i++) {
            node->type_args[i] = strdup(type_args[i] ? type_args[i] : "");
        }
        node->type_arg_count = type_arg_count;
    }
    node->args = args;
    node->arg_count = arg_count;
    return node;
}

ASTBinaryExpr* ast_new_binary_expr(ASTNode* left, LamoTokenType operator, ASTNode* right, int line, int column) {
    ASTBinaryExpr* node = (ASTBinaryExpr*)ast_new_node(AST_BINARY_EXPR, sizeof(ASTBinaryExpr), line, column);
    node->left = left;
    node->operator = operator;
    node->right = right;
    return node;
}

ASTUnaryExpr* ast_new_unary_expr(LamoTokenType operator, ASTNode* right, int line, int column) {
    ASTUnaryExpr* node = (ASTUnaryExpr*)ast_new_node(AST_UNARY_EXPR, sizeof(ASTUnaryExpr), line, column);
    node->operator = operator;
    node->right = right;
    return node;
}

ASTIntLiteral* ast_new_int_literal(long long value, int line, int column) {
    ASTIntLiteral* node = (ASTIntLiteral*)ast_new_node(AST_INT_LITERAL, sizeof(ASTIntLiteral), line, column);
    node->value = value;
    return node;
}

ASTFloatLiteral* ast_new_float_literal(double value, int line, int column) {
    ASTFloatLiteral* node = (ASTFloatLiteral*)ast_new_node(AST_FLOAT_LITERAL, sizeof(ASTFloatLiteral), line, column);
    node->value = value;
    return node;
}

ASTStringLiteral* ast_new_string_literal(char* value, int line, int column) {
    ASTStringLiteral* node = (ASTStringLiteral*)ast_new_node(AST_STRING_LITERAL, sizeof(ASTStringLiteral), line, column);
    node->value = strdup(value);
    return node;
}

ASTBoolLiteral* ast_new_bool_literal(int value, int line, int column) {
    ASTBoolLiteral* node = (ASTBoolLiteral*)ast_new_node(AST_BOOL_LITERAL, sizeof(ASTBoolLiteral), line, column);
    node->value = value;
    return node;
}

ASTIdentifier* ast_new_identifier(char* name, int line, int column) {
    ASTIdentifier* node = (ASTIdentifier*)ast_new_node(AST_IDENTIFIER, sizeof(ASTIdentifier), line, column);
    node->name = strdup(name);
    return node;
}

ASTCallExpr* ast_new_call_expr(char* name, ASTNode** args, int arg_count, int line, int column) {
    return ast_new_call_expr_typed(name, NULL, 0, args, arg_count, line, column);
}

/* Generics PR 2: expression-call ctor with optional explicit type args.
 * Strings in type_args are strdup'd; caller retains input ownership. */
ASTCallExpr* ast_new_call_expr_typed(char* name, char** type_args, int type_arg_count, ASTNode** args, int arg_count, int line, int column) {
    ASTCallExpr* node = (ASTCallExpr*)ast_new_node(AST_CALL_EXPR, sizeof(ASTCallExpr), line, column);
    node->name = strdup(name);
    node->type_arg_count = 0;
    node->type_args = NULL;
    if (type_arg_count > 0 && type_args) {
        node->type_args = malloc(sizeof(char*) * (size_t)type_arg_count);
        if (!node->type_args) {
            perror("Failed to allocate call type argument list");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < type_arg_count; i++) {
            node->type_args[i] = strdup(type_args[i] ? type_args[i] : "");
        }
        node->type_arg_count = type_arg_count;
    }
    node->args = args;
    node->arg_count = arg_count;
    return node;
}

ASTGroupingExpr* ast_new_grouping_expr(ASTNode* expression, int line, int column) {
    ASTGroupingExpr* node = (ASTGroupingExpr*)ast_new_node(AST_GROUPING_EXPR, sizeof(ASTGroupingExpr), line, column);
    node->expression = expression;
    return node;
}

ASTImport* ast_new_import_decl(char* path, int line, int column) {
    return ast_new_import_decl_aliased(path, NULL, line, column);
}

/* Sprint 4: aliased import constructor. The legacy ast_new_import_decl()
 * above is now a thin wrapper. `alias` may be NULL (legacy global-merge
 * behavior). The alias string is strdup'd so the caller may free its
 * copy immediately. */
ASTImport* ast_new_import_decl_aliased(char* path, char* alias, int line, int column) {
    ASTImport* node = (ASTImport*)ast_new_node(AST_IMPORT, sizeof(ASTImport), line, column);
    node->path = strdup(path);
    node->alias = alias ? strdup(alias) : NULL;
    return node;
}

/* Sprint 3: array literal. Takes ownership of the elements array (caller
 * must NOT free it). element_count is the number of valid entries in
 * elements. */
ASTArrayLiteral* ast_new_array_literal(ASTNode** elements, int element_count, int line, int column) {
    ASTArrayLiteral* node = (ASTArrayLiteral*)ast_new_node(AST_ARRAY_LITERAL, sizeof(ASTArrayLiteral), line, column);
    node->elements = elements;
    node->element_count = element_count;
    return node;
}

ASTIndexExpr* ast_new_index_expr(ASTNode* array, ASTNode* index, int line, int column) {
    ASTIndexExpr* node = (ASTIndexExpr*)ast_new_node(AST_INDEX_EXPR, sizeof(ASTIndexExpr), line, column);
    node->array = array;
    node->index = index;
    return node;
}

ASTPropExpr* ast_new_prop_expr(ASTNode* object, char* prop_name, int line, int column) {
    ASTPropExpr* node = (ASTPropExpr*)ast_new_node(AST_PROP_EXPR, sizeof(ASTPropExpr), line, column);
    node->object = object;
    node->prop_name = strdup(prop_name);
    return node;
}

/* Sprint 4: module member call constructor. `object` is owned by the AST
 * after this call. `member_name` is strdup'd. `args` array is owned but
 * its elements are NOT — they remain owned by their respective constructors
 * (which is the same convention as ASTCallStmt / ASTCallExpr). */
ASTMemberCall* ast_new_member_call(ASTNode* object, char* member_name, ASTNode** args, int arg_count, int line, int column) {
    ASTMemberCall* node = (ASTMemberCall*)ast_new_node(AST_MEMBER_CALL, sizeof(ASTMemberCall), line, column);
    node->object = object;
    node->member_name = strdup(member_name);
    node->args = args;
    node->arg_count = arg_count;
    return node;
}

/* ─── Phase 2: structs / methods / enums / match ──────────────────── */

ASTNode* ast_new_struct_decl(char* name, char** field_names, char** field_types, int field_count,
                             char** type_params, char** type_param_constraints, int type_param_count,
                             int line, int column) {
    ASTStructDecl* node = (ASTStructDecl*)ast_new_node(AST_STRUCT_DECL, sizeof(ASTStructDecl), line, column);
    int i;
    node->name = strdup(name);
    node->field_count = field_count;
    node->field_names = field_count > 0 ? malloc(sizeof(char*) * (size_t)field_count) : NULL;
    node->field_types = field_count > 0 ? malloc(sizeof(char*) * (size_t)field_count) : NULL;
    for (i = 0; i < field_count; i++) {
        node->field_names[i] = strdup(field_names[i] ? field_names[i] : "");
        node->field_types[i] = field_types && field_types[i] ? strdup(field_types[i]) : NULL;
    }
    /* Generics PR 1: copy type parameter names. PR 6: plus constraints. */
    node->type_param_count = type_param_count;
    node->type_params = type_param_count > 0 ? malloc(sizeof(char*) * (size_t)type_param_count) : NULL;
    node->type_param_constraints = type_param_count > 0 ? malloc(sizeof(char*) * (size_t)type_param_count) : NULL;
    for (i = 0; i < type_param_count; i++) {
        node->type_params[i] = strdup(type_params[i] ? type_params[i] : "");
        node->type_param_constraints[i] =
            (type_param_constraints && type_param_constraints[i]) ? strdup(type_param_constraints[i]) : NULL;
    }
    return (ASTNode*)node;
}

ASTNode* ast_new_impl_decl(char* struct_name, ASTNode* methods, int line, int column) {
    return ast_new_impl_decl_generic(struct_name, NULL, 0, NULL, 0, methods, line, column);
}

/* RFC §4.4: generic impl constructor. See ast.h for the contract. */
ASTNode* ast_new_impl_decl_generic(char* struct_name,
                                   char** type_params, int type_param_count,
                                   char** type_args, int type_arg_count,
                                   ASTNode* methods, int line, int column) {
    ASTImplDecl* node = (ASTImplDecl*)ast_new_node(AST_IMPL_DECL, sizeof(ASTImplDecl), line, column);
    node->struct_name = strdup(struct_name);
    node->methods = methods;
    node->type_params = NULL;
    node->type_param_count = 0;
    node->type_args = NULL;
    node->type_arg_count = 0;
    if (type_param_count > 0 && type_params) {
        node->type_params = malloc(sizeof(char*) * (size_t)type_param_count);
        if (!node->type_params) {
            perror("Failed to allocate impl type parameter list");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < type_param_count; i++) {
            node->type_params[i] = strdup(type_params[i] ? type_params[i] : "");
        }
        node->type_param_count = type_param_count;
    }
    if (type_arg_count > 0 && type_args) {
        node->type_args = malloc(sizeof(char*) * (size_t)type_arg_count);
        if (!node->type_args) {
            perror("Failed to allocate impl type argument list");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < type_arg_count; i++) {
            node->type_args[i] = strdup(type_args[i] ? type_args[i] : "");
        }
        node->type_arg_count = type_arg_count;
    }
    return (ASTNode*)node;
}

ASTNode* ast_new_enum_decl(char* name, char** variants, int variant_count, int line, int column) {
    ASTEnumDecl* node = (ASTEnumDecl*)ast_new_node(AST_ENUM_DECL, sizeof(ASTEnumDecl), line, column);
    int i;
    node->name = strdup(name);
    node->variant_count = variant_count;
    node->variants = variant_count > 0 ? malloc(sizeof(char*) * (size_t)variant_count) : NULL;
    for (i = 0; i < variant_count; i++) {
        node->variants[i] = strdup(variants[i] ? variants[i] : "");
    }
    return (ASTNode*)node;
}

ASTNode* ast_new_match_stmt(ASTNode* scrutinee, char** patterns, int* pattern_is_wildcard, ASTNode** bodies, int arm_count, int line, int column) {
    ASTMatchStmt* node = (ASTMatchStmt*)ast_new_node(AST_MATCH_STMT, sizeof(ASTMatchStmt), line, column);
    int i;
    node->scrutinee = scrutinee;
    node->arm_count = arm_count;
    node->patterns = arm_count > 0 ? malloc(sizeof(char*) * (size_t)arm_count) : NULL;
    node->pattern_is_wildcard = arm_count > 0 ? malloc(sizeof(int) * (size_t)arm_count) : NULL;
    node->bodies = arm_count > 0 ? malloc(sizeof(ASTNode*) * (size_t)arm_count) : NULL;
    for (i = 0; i < arm_count; i++) {
        node->patterns[i] = strdup(patterns[i] ? patterns[i] : "_");
        node->pattern_is_wildcard[i] = pattern_is_wildcard ? pattern_is_wildcard[i] : 0;
        node->bodies[i] = bodies[i];
    }
    return (ASTNode*)node;
}

ASTNode* ast_new_struct_literal(char* struct_name, char** field_names, ASTNode** field_values, int field_count, char** type_args, int type_arg_count, int line, int column) {
    ASTStructLiteral* node = (ASTStructLiteral*)ast_new_node(AST_STRUCT_LITERAL, sizeof(ASTStructLiteral), line, column);
    int i;
    node->struct_name = strdup(struct_name);
    node->field_count = field_count;
    node->field_names = field_count > 0 ? malloc(sizeof(char*) * (size_t)field_count) : NULL;
    node->field_values = field_count > 0 ? malloc(sizeof(ASTNode*) * (size_t)field_count) : NULL;
    for (i = 0; i < field_count; i++) {
        node->field_names[i] = strdup(field_names[i] ? field_names[i] : "");
        node->field_values[i] = field_values[i];
    }
    /* Generics PR 1: copy type argument names. */
    node->type_arg_count = type_arg_count;
    node->type_args = type_arg_count > 0 ? malloc(sizeof(char*) * (size_t)type_arg_count) : NULL;
    for (i = 0; i < type_arg_count; i++) {
        node->type_args[i] = strdup(type_args[i] ? type_args[i] : "");
    }
    return (ASTNode*)node;
}

ASTNode* ast_new_place_assign_stmt(ASTNode* target, ASTNode* value, LamoTokenType op_type, int line, int column) {
    ASTPlaceAssignStmt* node = (ASTPlaceAssignStmt*)ast_new_node(AST_PLACE_ASSIGN_STMT, sizeof(ASTPlaceAssignStmt), line, column);
    node->target = target;
    node->value = value;
    node->op_type = op_type;
    return (ASTNode*)node;
}

void ast_program_append(ASTProgram* destination, ASTProgram* source) {
    ASTNode* tail;

    if (!destination || !source || !source->declarations) {
        if (source) {
            source->declarations = NULL;
            free(source);
        }
        return;
    }

    if (!destination->declarations) {
        destination->declarations = source->declarations;
        source->declarations = NULL;
        free(source);
        return;
    }

    tail = destination->declarations;
    while (tail->next) {
        tail = tail->next;
    }

    tail->next = source->declarations;
    source->declarations = NULL;
    free(source);
}

/* Sprint 2 refactor: the `next` list traversal is now iterative. The
 * previous version recursed on `next` at the tail of the function:
 *
 *     free(node);
 *     if (next) ast_free(next);
 *
 * That consumed O(N) stack frames for a list of N sibling nodes — fine
 * for small programs, but a Lamo file with tens of thousands of
 * top-level statements would blow the C stack. The recursive descent
 * into CHILDREN (initializer, body, args, left/right, ...) is left
 * alone because that depth is bounded by expression nesting, not by
 * the number of statements in the program.
 *
 * The new shape is a loop that walks `next` siblings while recursing
 * into children exactly as before. */
void ast_free(ASTNode* node) {
    while (node) {
        ASTNode* next = node->next;

        switch (node->type) {
            case AST_PROGRAM:
                ast_free(((ASTProgram*)node)->declarations);
                break;
            case AST_VAR_DECL:
                free(((ASTVarDecl*)node)->name);
                free(((ASTVarDecl*)node)->type_annotation);
                ast_free(((ASTVarDecl*)node)->initializer);
                break;
            case AST_FN_DECL:
                free(((ASTFnDecl*)node)->name);
                for (int i = 0; i < ((ASTFnDecl*)node)->param_count; i++) {
                    free(((ASTFnDecl*)node)->params[i]);
                }
                free(((ASTFnDecl*)node)->params);
                /* Sprint 3: free param type annotations. param_types may
                 * be NULL (no annotations on any parameter). */
                if (((ASTFnDecl*)node)->param_types) {
                    for (int i = 0; i < ((ASTFnDecl*)node)->param_count; i++) {
                        free(((ASTFnDecl*)node)->param_types[i]);
                    }
                    free(((ASTFnDecl*)node)->param_types);
                }
                free(((ASTFnDecl*)node)->return_type_annotation);
                /* Generics PR 2: free the generic type parameter list. */
                for (int i = 0; i < ((ASTFnDecl*)node)->type_param_count; i++) {
                    free(((ASTFnDecl*)node)->type_params[i]);
                    free(((ASTFnDecl*)node)->type_param_constraints[i]);
                }
                free(((ASTFnDecl*)node)->type_params);
                free(((ASTFnDecl*)node)->type_param_constraints);
                ast_free(((ASTFnDecl*)node)->body);
                break;
            case AST_BLOCK:
                ast_free(((ASTBlock*)node)->statements);
                break;
            case AST_IF_STMT:
                ast_free(((ASTIfStmt*)node)->condition);
                ast_free(((ASTIfStmt*)node)->then_branch);
                ast_free(((ASTIfStmt*)node)->else_branch);
                break;
            case AST_WHILE_STMT:
                ast_free(((ASTWhileStmt*)node)->condition);
                ast_free(((ASTWhileStmt*)node)->body);
                break;
            case AST_FOR_STMT:
                ast_free(((ASTForStmt*)node)->initializer);
                ast_free(((ASTForStmt*)node)->condition);
                ast_free(((ASTForStmt*)node)->increment);
                ast_free(((ASTForStmt*)node)->body);
                break;
            case AST_RETURN_STMT:
                ast_free(((ASTReturnStmt*)node)->expression);
                break;
            case AST_BREAK_STMT:
            case AST_CONTINUE_STMT:
                /* leaf nodes — no extra fields to free. */
                break;
            case AST_ASSIGN_STMT:
                free(((ASTAssignStmt*)node)->name);
                ast_free(((ASTAssignStmt*)node)->value);
                break;
            case AST_CALL_STMT:
            case AST_CALL_EXPR:
                free(((ASTCallStmt*)node)->name);
                for (int i = 0; i < ((ASTCallStmt*)node)->arg_count; i++) {
                    ast_free(((ASTCallStmt*)node)->args[i]);
                }
                free(((ASTCallStmt*)node)->args);
                break;
            case AST_BINARY_EXPR:
                ast_free(((ASTBinaryExpr*)node)->left);
                ast_free(((ASTBinaryExpr*)node)->right);
                break;
            case AST_UNARY_EXPR:
                ast_free(((ASTUnaryExpr*)node)->right);
                break;
            case AST_INT_LITERAL:
            case AST_FLOAT_LITERAL:
                break;
            case AST_STRING_LITERAL:
                free(((ASTStringLiteral*)node)->value);
                break;
            case AST_BOOL_LITERAL:
                break;
            case AST_IDENTIFIER:
                free(((ASTIdentifier*)node)->name);
                break;
            case AST_GROUPING_EXPR:
                ast_free(((ASTGroupingExpr*)node)->expression);
                break;
            case AST_IMPORT:
                free(((ASTImport*)node)->path);
                /* Sprint 4: free optional module alias. */
                free(((ASTImport*)node)->alias);
                break;
            case AST_ARRAY_LITERAL: {
                /* Sprint 3: array literal. Free each element expression,
                 * then the elements array itself. */
                ASTArrayLiteral* arr = (ASTArrayLiteral*)node;
                int i;
                for (i = 0; i < arr->element_count; i++) {
                    ast_free(arr->elements[i]);
                }
                free(arr->elements);
                break;
            }
            case AST_INDEX_EXPR: {
                ASTIndexExpr* idx = (ASTIndexExpr*)node;
                ast_free(idx->array);
                ast_free(idx->index);
                break;
            }
            case AST_PROP_EXPR: {
                ASTPropExpr* prop = (ASTPropExpr*)node;
                ast_free(prop->object);
                free(prop->prop_name);
                break;
            }
            case AST_MEMBER_CALL: {
                /* Sprint 4: module member call. Free object, member_name,
                 * each arg expression, then the args array itself. */
                ASTMemberCall* mc = (ASTMemberCall*)node;
                int i;
                ast_free(mc->object);
                free(mc->member_name);
                for (i = 0; i < mc->arg_count; i++) {
                    ast_free(mc->args[i]);
                }
                free(mc->args);
                break;
            }
            case AST_STRUCT_DECL: {
                ASTStructDecl* sd = (ASTStructDecl*)node;
                int i;
                free(sd->name);
                for (i = 0; i < sd->field_count; i++) {
                    free(sd->field_names[i]);
                    free(sd->field_types[i]);  /* may be NULL — free is safe */
                }
                free(sd->field_names);
                free(sd->field_types);
                /* Generics PR 1: free type parameter names. */
                for (i = 0; i < sd->type_param_count; i++) {
                    free(sd->type_params[i]);
                }
                free(sd->type_params);
                if (((ASTStructDecl*)node)->type_param_constraints) {
                    for (i = 0; i < sd->type_param_count; i++) {
                        free(((ASTStructDecl*)node)->type_param_constraints[i]);
                    }
                    free(((ASTStructDecl*)node)->type_param_constraints);
                }
                break;
            }
            case AST_IMPL_DECL: {
                ASTImplDecl* id = (ASTImplDecl*)node;
                free(id->struct_name);
                ast_free(id->methods);
                /* Generics PR 2: free impl type params/args. */
                for (int i = 0; i < id->type_param_count; i++) free(id->type_params[i]);
                free(id->type_params);
                for (int i = 0; i < id->type_arg_count; i++) free(id->type_args[i]);
                free(id->type_args);
                break;
            }
            case AST_ENUM_DECL: {
                ASTEnumDecl* ed = (ASTEnumDecl*)node;
                int i;
                free(ed->name);
                for (i = 0; i < ed->variant_count; i++) {
                    free(ed->variants[i]);
                }
                free(ed->variants);
                break;
            }
            case AST_MATCH_STMT: {
                ASTMatchStmt* ms = (ASTMatchStmt*)node;
                int i;
                ast_free(ms->scrutinee);
                for (i = 0; i < ms->arm_count; i++) {
                    free(ms->patterns[i]);
                    ast_free(ms->bodies[i]);
                }
                free(ms->patterns);
                free(ms->pattern_is_wildcard);
                free(ms->bodies);
                break;
            }
            case AST_STRUCT_LITERAL: {
                ASTStructLiteral* sl = (ASTStructLiteral*)node;
                int i;
                free(sl->struct_name);
                for (i = 0; i < sl->field_count; i++) {
                    free(sl->field_names[i]);
                    ast_free(sl->field_values[i]);
                }
                free(sl->field_names);
                free(sl->field_values);
                /* Generics PR 1: free type argument names. */
                for (i = 0; i < sl->type_arg_count; i++) {
                    free(sl->type_args[i]);
                }
                free(sl->type_args);
                break;
            }
            case AST_PLACE_ASSIGN_STMT: {
                ASTPlaceAssignStmt* pa = (ASTPlaceAssignStmt*)node;
                ast_free(pa->target);
                ast_free(pa->value);
                break;
            }
        }

        free(node);
        node = next;
    }
}
