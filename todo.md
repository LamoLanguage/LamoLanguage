# Lamo TODO

This file tracks implementation work per phase. Sprint 2.5.0 completed every
outstanding item below; each completed row carries a short evidence note
pointing at the code, docs, or tests that fulfill it. Genuinely new work
discovered along the way is listed under **Open Follow-Ups** and should be
picked up before (or alongside) any larger new feature.

## Status Snapshot

| Area | Status |
|------|--------|
| Foundation (docs, layout, contributor notes) | ✅ Complete |
| Phase 1 — Stabilize the compiler | ✅ Complete |
| Phase 2 — Testing infrastructure | ✅ Complete |
| Phase 3 — Semantic analysis | ✅ Complete |
| Phase 4 — Type system | ✅ Complete |
| Phase 5 — Runtime design (GC rollout) | ✅ Complete (2.3.0) |
| Phase 6 — Language specification | ✅ Complete |
| Phase 7 — Language features & generics | ✅ Complete (2.5.0) |
| Phase 8 — Standard library | ✅ Complete |
| Phase 9 — Developer experience | ✅ Complete |
| Phase 10 — Multi-file projects | ✅ Complete (module rules → Open Follow-Ups) |
| Phase 11 — Backend evolution | ✅ Complete (VM/LLVM deferred by design) |
| Cross-cutting work | ✅ Ongoing policy, currently honored |

## Open Follow-Ups

Honest ledger of work discovered but **not** completed in 2.5.0:

- [ ] **Tagged-union enums** — payload-carrying variants + `Some(x) =>`
      binding. Prerequisite for pattern-matched `Option`/`Result` per
      [RFC-generics](docs/RFC-generics.md) §10; PR 5 ships a function-shaped
      API in the meantime.
- [ ] **Module-boundary type flow** — let imported functions return
      struct-typed values usable for field access/methods in the importing
      file (today they erase to opaque arrays when declared locally inside
      modules; PR 5 works around it with array payloads). Design needed in
      `modules.c`/`semantic.c`.
- [ ] **Explicit type arguments on module member calls** (`col.f<int>(...)`)
      — plain calls accept them today; member chains do not. Parser probe
      exists; needs `ASTMemberCall` plumbing.
- [ ] **Boolean print form** — should `print(true)` render `%true`/`%false`
      instead of `1`/`0`? Decide and spec it before anyone depends on it.
- [ ] **Module rules carried over from Phases 7/10** — explicit export rules,
      package/folder-based modules, duplicate-import behavior, entry-file vs
      library-file expectations, and module loading for `lamo eval`/`lamo repl`.

## Completed Work

### Foundation

| Item | Evidence |
|------|----------|
| Align `README.md` with the current implementation | Rewritten alongside 2.5.0 docs |
| Remove outdated references to old file names / structure | `docs/DIRECTORY-LAYOUT.md` is the map |
| Define a directory strategy for compiler stages | `docs/DIRECTORY-LAYOUT.md` — pipeline order, `src/` responsibilities, runtime embedding, tests taxonomy |
| Contributor note for building/running the compiler | `CLAUDE.md` + `Makefile` |

### Phase 1 — Stabilize The Current Compiler

| Item | Evidence |
|------|----------|
| Remove silent token skipping; explicit syntax errors | parser rewrite |
| Audit parser branches for missing validation / edge cases | parser audit |
| Clearer expected/actual token output in parser errors | `error_util.h` |
| Source-location coverage on all frontend errors | `file:line:col` everywhere |
| Move `import` parsing into the real frontend grammar | `AST_IMPORT` node |
| Error recovery (synchronize-and-continue) — many errors per pass | parser |
| Standardized exit codes (success / compile failure / backend failure) | SPEC §12.7 |
| Improved CLI usage text and error messages | `src/cli/help.c` |
| GCC invocation failures surfaced clearly | `compile.c` (2.3.0) |
| Generated C emission policy decided | **Always** emit `lamo_exec.c` (2.3.0) |
| Windows default build produces `lamo.exe`; `make clean` fixed | Makefile |

### Phase 2 — Testing Infrastructure

| Item | Evidence |
|------|----------|
| `tests/` structure for valid/invalid programs + runner | `tests/`, `make test` |
| Parser smoke tests | `tests/smoke/` — 16 cases with `.expect_err` / `.expect_ok` contracts |
| End-to-end tests (compile + run sample programs) | `tests/runtime/` |
| Golden tests for generated C | `tests/golden/` diffs the user-code section of `lamo_exec.c` against committed snapshots |
| Tests for expected compiler diagnostics | smoke corpus asserts real message shapes |
| Duplicate-import regression coverage | `tests/smoke/import_same_file_twice.*` (writing it exposed & fixed an uninitialized-field crash in optimized builds) |

### Phase 3 — Semantic Analysis

| Item | Evidence |
|------|----------|
| Semantic pass between parsing and codegen; symbol table | `src/semantic/semantic.c` |
| Global / function / block scopes; declarations tracked by scope level | `Scope.level` (global = 0, nested +1) on every `Symbol` |
| Undeclared variables, duplicate declarations, duplicate functions detected | `tests/invalid/duplicate_fn.lamo` |
| Function call arity validated; assignment targets validated | dedicated hinted errors for assignment-to-function/builtin |
| `return` outside functions rejected; reserved words rejected with hints | grammar-level rejection + human hints |
| Semantic analysis across file boundaries; import-time duplicate rules | re-import dedupes + warns; different-alias warns, first alias wins; cross-file duplicates hard-error — SPEC §10.5 |
| Errors carry line/column and stop codegen | `error_util.h` |
| Multi-file errors show the originating file | regression-pinned: `tests/smoke/import_reports_broken_file` |
| Tests for every semantic error category | constraint violation, void-in-condition/logical/bang, return-in-void-fn, param mismatch, unknown constraint, unknown nested field type, wrong/incomplete generic annotations, assignment targets, reserved words, import paths |

### Phase 4 — Type System

| Item | Evidence |
|------|----------|
| `Type` representation in the compiler; hybrid inference decision | [TYPE-SYSTEM.md](docs/TYPE-SYSTEM.md); full recursive `type_ann` support |
| Inferred/declared types attached to AST nodes | normalized full types interned on nodes (`sema_full_type`) |
| Arithmetic / comparison / logical / unary operators validated by type | SPEC §6; `&&`/`||`/`!` reject void operands (SPEC §6.3, §7.10) |
| Function parameter and return types validated | call sites check annotated params vs concrete args (numeric widening kept, generics invariant per RFC §5.4) |
| Equality semantics and truthiness defined | SPEC §6.3 / §7.5 |
| Backend no longer hardcodes `int` everywhere; `print()` uses semantic types | struct args render `Player { 10, arthur }` via `lamo_print_struct_named` |
| `input()` split into `input_int` / `input_str` | builtins |
| Builtin type predicates reflect real types; top-level `let`s emitted as C globals with initializers | codegen |

### Phase 5 — Runtime Design (GC Rollout)

All items shipped in **2.3.0**; the full design and step-by-step rollout live
in [MEMORY-MODEL.md](docs/MEMORY-MODEL.md):

- GC skeleton in `lamo_runtime.h` (`LamoGcHeader`, mark-sweep, root stack).
- Codegen emits root push/pop per scope (`LAMO_GC_PUSH_ROOT` / `LAMO_GC_POP_ROOTS_N`).
- GC wired into the HTTP loop (every 100 requests) and GUI loop (every 1000 frames).
- `examples/http_server.lamo` re-promoted to "official".
- Tests: `tests/runtime/gc_basic.lamo`, `tests/runtime/gc_cycle.lamo` (cycle reclamation).
- New builtins: `gc_collect()`, `gc_set_threshold(N)`, `gc_heap_size()`, `gc_heap_count()`.

### Phase 6 — Language Specification

| Item | Evidence |
|------|----------|
| Authoritative spec written | [SPEC.md](docs/SPEC.md) — lexical structure, grammar, declarations, statements, expressions, operators, builtins, type rules, scopes, modules, platform builtins, runtime behavior |
| Generics grammar + semantics specced | SPEC §7.7–7.9 |
| Import duplicate rule + visibility decision + project layout | SPEC §10.5, §10.6 |
| Assignment-as-expression decision | **Stays a statement** (bool-vs-value ambiguity under erasure; matches all examples) — SPEC note + ARCHITECTURE review note |
| Hoisting decision | **Functions ARE hoisted within a file** — the global pre-pass registers all top-level functions before statement visits; verified + documented |

### Phase 7 — Language Features & Generics

| Item | Evidence |
|------|----------|
| Typed variable declarations, typed fn signatures, `break`, `continue` | shipped pre-2.4 |
| Arrays, structs, methods, enums, match | Phase 2 language-expansion sprint |
| Maps/dictionaries decision | stdlib (`std/collections`) |
| Generics RFC written | [RFC-generics.md](docs/RFC-generics.md) — status header updated to SHIPPED 2.5.0 |
| **Generics PR 1** — generic struct declarations + type parameters in field types | 2.4.0 |
| **Generics PR 2** — generic functions + type inference at call sites | `fn id<T>(x: T) -> T`; local inference binds `T` from concrete args; explicit `f<int>(...)` supported for plain calls (scanner-gated so comparisons never misparse); runtime unchanged (erasure) |
| **Generics PR 3** — `array<T>` typed-array syntax + deprecation warning for bare `array` | recursive annotation parsing; element LUB inference (RFC §5.1); bare-array warning is stderr-only, never fails builds |
| **Generics PR 4** — typed `Map<K,V>` and `Set<T>` in stdlib | `std/collections` typedList/typedMap/typedSet APIs: signatures give compile-time checking while representation stays erased arrays; `Eq`-constrained set demo; `std/tests/test_collections_typed.lamo` |
| **Generics PR 5** (adjusted scope) — `Option<T>` / `Result<T,E>` | type-checked factories/accessors in `std.collections` over erased arrays; struct payloads across module boundaries await the module-type-flow fix; match payload syntax waits on the tagged-enum RFC |
| **Generics PR 6** — constraint syntax (`T: Ord`, ...) | parsed on struct AND fn AND impl type parameter lists; catalogue `Any/Eq/Ord/Num/Hash/Show`; unknown names and violated constraints are compile errors (`err_constraint_violation`) |

### Phase 8 — Standard Library

| Item | Evidence |
|------|----------|
| Core-vs-stdlib boundary defined | [STDLIB.md](docs/STDLIB.md) §8.1 test-for-inclusion rule |
| I/O expansion beyond `print`/`input` | satisfied by `std.io` (`println`/`eprint`/`read_line`/`write`); core stays minimal by design |
| Numeric helpers | `std.math` shipped on stable float semantics; growth purely additive |
| String helpers | `std.string` complete per STDLIB §8.4 |
| Module organization plan | STDLIB §8.5: four-artifact rule (implementation + `.md` + example + tests), naming, no cross-module deps |

### Phase 9 — Developer Experience

| Item | Evidence |
|------|----------|
| `lamo repl`, `lamo new <name>`, `lamo clean`, per-command help | `src/cli/commands.c` |
| `--verbose` / `--quiet` flags + `LAMO_VERBOSE` / `LAMO_QUIET` env vars | CLI |
| `LAMO_CC` env var overrides the C compiler | CLI |
| Backend failure messages name the C compiler and exit code | `compile.c` |
| File path, line, column in all compiler errors | end-to-end audit 2.5.0; smoke corpus pins message shapes |
| Source snippet with caret marker | `error_util.h` for parser + semantic; runtime-side errors stay snippet-less by design |
| Error hints under snippets | missing initializers, undeclared variables/functions, missing `let` on assignment |
| ANSI color diagnostics (auto TTY detect; `--no-color` / `LAMO_NO_COLOR=1`) | `error_util.h` |
| Canonical `.lamo` code style defined | [STYLE.md](docs/STYLE.md) |
| Formatter policy decided | `lamo fmt` stays whitespace-level normalization only, never syntax rewriting (STYLE §7) |
| `lamo test` runs the suite without `make test` | CLI |
| Quieter success output unless verbose | CLI |

### Phase 10 — Multi-File Projects

| Item | Evidence |
|------|----------|
| Module/import syntax designed in the frontend | namespaced imports |
| Symbol visibility rules decided | SPEC §10.6: namespace = privacy boundary today; `pub` later narrows mechanically; two-step rollout written |
| Multiple `.lamo` files per build; semantic analysis across files | loader + semantic |
| Project/package layout conventions | SPEC §10.6: `lamo.pkg` scaffolding, artifacts never committed |
| Import cycles detected and reported | loader, with cycle stack |
| File ownership attached to AST nodes (not a merged-program label) | multi-file diagnostic tests regression-prove it |

### Phase 11 — Backend Evolution

| Item | Evidence |
|------|----------|
| C backend kept as short-term primary target | by design |
| Codegen depends on semantic information | formal contract in [ARCHITECTURE.md](docs/ARCHITECTURE.md) (annotation table); named-struct print uses sema names; member-call route decided by sema markers (fixed a `self.items.push` misrouting found by the new suite) |
| Frontend/backend separation | boundary == annotated AST; shared error rendering via `error_util`; golden snapshots pin emitted shape; DIRECTORY-LAYOUT encodes it in paths |
| Interpreter decision | tree-walking `eval`/`repl` exists, intentionally module-less; `run`/`build`/`check` authoritative (SPEC §10.7, ARCHITECTURE §3) |
| VM/LLVM | deferred with three explicit preconditions (ARCHITECTURE §4) |

### Cross-Cutting Work

| Item | Evidence |
|------|----------|
| Tests added with every feature | each item above landed with cases |
| Docs updated as behavior changes | SPEC/RFC/DIRECTORY-LAYOUT/ARCHITECTURE/STDLIB/STYLE updated in the same release |
| Shortcuts that leak C behavior into semantics removed | e.g. print type-name flow |
| Windows / Unix compatibility checked regularly | Makefile stays dual-target; runtime guarded as before (2.5.0 developed/tested on POSIX; Windows gates untouched) |
| No syntax before semantics | standing policy — every syntax change pairs with a SPEC section + validation + tests in the same release |

## Historical Sprint Ledgers

Condensed records of earlier sprints; the per-item detail above supersedes
these lists where they overlap.

**CLI Tooling Pass** — `lamo new/clean/repl/help`; global flags and env vars;
`LAMO_CC` override; backend failure messages; `lampm` merged into the `lamo`
binary (no separate executable); LamoPacketManager v0.2 (version pinning
`@ref`, `lamo.lock` lockfile, non-GitHub sources, per-subcommand help);
fixed the `2>nul` Windows-only redirection bug and the `_POSIX_C_SOURCE`
feature-macro bug that broke `-std=c99` builds.

**Lint Pass** — removed an unused lexer function (leak); replaced
exit-on-first-error with synchronize-and-continue; int literals to
`long long` + `strtoll` with hex/binary/underscore/float support; string
escape decoding (`\n`, `\t`, `\r`, `\\`, `\"`, `\0`, `\xNN`); top-level `let`
emitted as C globals; `import` moved from textual preprocessing to real AST
nodes; X11 GUI backend added on Linux/macOS; builtins resolved via table and
shadowable; all user identifiers prefixed `lamo_u_` in generated C;
`_DEFAULT_SOURCE` feature macro for `realpath` on Linux.

**Engineering Sprint (spec, types, memory, refactor, generics RFC)** —
type-system decision formalized ([TYPE-SYSTEM.md](docs/TYPE-SYSTEM.md));
authoritative spec written ([SPEC.md](docs/SPEC.md)); memory model resolved
with the opt-in GC design and the "official example" policy
([MEMORY-MODEL.md](docs/MEMORY-MODEL.md)); `lamo_v2.c` split 2563 → 379 lines
(-85%) into `src/cli/`, `lampm.c` split 2473 → 1337 lines (-46%) into
`src/lampm/` with zero test failures and zero warnings under `-Wall -Wextra`;
generics RFC drafted with the 6-PR rollout plan
([RFC-generics.md](docs/RFC-generics.md)).

**2.3.0 Sprint (GC ship, eval/run spec, lampm scope cut)** — opt-in
mark-sweep GC shipped (header per allocation, codegen root push/pop, HTTP/GUI
auto-collect hooks, four new `gc_*` builtins); `eval`/`repl` vs `run`
divergence formalized (SPEC §10.7 — the interpreter does not load modules,
with a clear error pointing at `lamo run`); `lampm` scope reduced (cut `why`
and `outdated`; 9 commands remain); GCC failure reporting header; `.c`
emission policy decided (always emit `lamo_exec.c` — it is the IR).
Generics PR 1 was deliberately deferred until the GC landed to avoid
changing the memory model and the codegen shape at the same time.
