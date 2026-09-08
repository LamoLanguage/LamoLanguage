#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "../modules.h"
#include <stdio.h>

// Função principal para gerar código C a partir da AST
void generate_c_code(ASTNode* node, FILE* out);

/* Sprint 4: register the module registry so the codegen can resolve
 * `module.member(args)` calls. Pass NULL to disable module resolution
 * (e.g. when generating code outside of a full compile pipeline, like
 * from the REPL). The registry pointer is NOT owned — the caller must
 * keep it alive for the duration of generate_c_code(). */
void codegen_set_module_registry(LamoModuleRegistry* reg);

/* 2.6.0 (FU5): record the entry file's normalized path so the generated
 * C entry point calls the entry file's `fn main()` (SPEC §12.1). Pass
 * NULL to disable the call (e.g. codegen outside a full compile). The
 * pointer is NOT owned — the caller must keep it alive through
 * generate_c_code(). */
void codegen_set_entry_file(const char* entry_path);

#endif // CODEGEN_H
