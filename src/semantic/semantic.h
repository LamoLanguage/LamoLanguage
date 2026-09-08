#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "ast.h"

/* Type of the source-lookup callback used by semantic_analyze to print
 * source snippets under error messages. Given a file_path, returns the
 * full source text of that file (NULL if not known). The returned
 * pointer must remain valid for the duration of semantic_analyze(). */
typedef const char* (*LamoSourceLookupFn)(const char* file_path, void* user_data);

/* Sprint 4: module-resolution callback. Given an alias name (e.g. "math"
 * in `import "math.lamo" as math;`) and a member name (e.g. "sqrt" in
 * `math.sqrt(x)`), returns the prefixed function name to call
 * (e.g. "lamo_mod_math__sqrt"), or NULL if the alias is not a registered
 * module or the member is not in it. The returned pointer must remain
 * valid for the duration of semantic_analyze().
 *
 * Used to resolve AST_MEMBER_CALL nodes against the module registry
 * kept in CompilationState. */
typedef const char* (*LamoModuleResolveFn)(const char* alias, const char* member,
                                            void* user_data);

/* Also lookup the arity of a module member function so the semantic
 * pass can validate call arity the same way it does for regular calls.
 * Returns -1 if the alias/member is not found. */
typedef int (*LamoModuleArityFn)(const char* alias, const char* member,
                                  void* user_data);

/* Basic entry point: runs the semantic pass with no source-lookup
 * callback. Error messages will be emitted without a source snippet. */
int semantic_analyze(ASTProgram* program, const char* file_path);

/* Sprint 3 entry point: same as semantic_analyze, but registers a
 * source-lookup callback so that error messages include the offending
 * source line + a caret pointing at the column. */
int semantic_analyze_with_source_lookup(ASTProgram* program, const char* file_path,
                                        LamoSourceLookupFn lookup, void* user_data);

/* Sprint 4 entry point: also registers module-resolution callbacks so
 * that AST_MEMBER_CALL nodes (`math.sqrt(x)` style) can be validated.
 * The source-lookup callback may be NULL (snippet omitted on errors);
 * the module callbacks may also be NULL (member calls always error with
 * "module resolution not available in this context"). */
int semantic_analyze_full(ASTProgram* program, const char* file_path,
                          LamoSourceLookupFn src_lookup, void* src_user_data,
                          LamoModuleResolveFn mod_resolve,
                          LamoModuleArityFn mod_arity,
                          void* mod_user_data);

#endif
