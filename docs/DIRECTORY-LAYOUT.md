# Lamo Directory Layout Strategy

**Status:** Official. This document records WHERE compiler stages, runtime
code, tests, and tooling live — and WHY — so new work has an unambiguous
home.

## Top Level

```
lamo                  # compiler binary (built; not committed)
Makefile              # one-command build + `make test`
scripts/              # build-time generators (not shipped with the binary)
src/                  # the compiler itself (C99)
std/                  # standard library modules (.lamo) + docs (.md) + tests
tests/                # regression corpus for the COMPILER
examples/             # end-user example programs
docs/                 # authoritative design documents & this file
```

## src/ — Compiler Stages, in Pipeline Order

The compile pipeline is:

```
source text ─▶ lexer ─▶ parser ─▶ AST ─▶ import resolver ─▶ semantic pass ─▶ codegen ─▶ lamo_exec.c ─▶ C compiler
                                                    │                        │
                                                 modules.c              lamo_runtime.h (embedded)
```

| Path | Stage / responsibility |
|------|------------------------|
| `src/lamo_v2.c` | `main()` + subcommand dispatch only (kept thin by design) |
| `src/cli/` | CLI layer: option parsing, per-subcommand handlers, the compile pipeline driver (`compile.c`), import resolution (`import_resolver.c`), help text |
| `src/lexer/` | Lexing: source → tokens |
| `src/parser/` | Parsing: tokens → AST (including all generics syntax) |
| `src/ast/` | AST node definitions, constructors, and `ast_free` |
| `src/semantic/` | The semantic pass: symbol tables, scoping, type checking, generics binding, constraint catalogue |
| `src/codegen/` | C backend: AST → C (`codegen.c`), embedded runtime string data — and the runtime itself lives in one header, see below |
| `src/eval/` | Tree-walking interpreter backing `lamo eval` / `lamo repl` (fast-feedback path; deliberately NOT module-capable — SPEC §10.7) |
| `src/lampm/` | Integrated package manager (`lamo install`, …); linked into the same binary |
| `src/modules.c/.h` | Module registry used by namespaced imports |
| `src/builtins.h` | Single source of truth for every builtin (name, arity, category, return policy). Adding a builtin starts here. |
| `src/error_util.h` | Shared error formatting: `file:line:col`, caret snippets, hints, color |

Rules of thumb that keep the split honest:

1. **Frontend vs backend boundary** — everything up to and including
   `src/semantic/` is the frontend; `src/codegen/` is the backend. The
   backend must derive its decisions from semantic annotations on AST nodes
   (e.g. `sema_struct_name`, `sema_full_type`) rather than re-deriving facts.
   See [ARCHITECTURE.md](ARCHITECTURE.md).
2. **One builtin = three edits maximum** — a `builtins.h` entry, a runtime
   function in `lamo_runtime.h`, and a codegen case. Anything more is a bug
   in the layout.
3. **Diagnostics always render through `error_util.h`.**

## Runtime

`src/codegen/lamo_runtime.h` is a real, compilable header maintained like any
other source file. `scripts/embed_runtime.py` converts it into
`src/codegen/lamo_runtime_data.c`, which is committed to the repo so builds
do not require Python. `make embed-runtime` regenerates it whenever the
header changes; `make` does this automatically.

## Tests

```
tests/
├── valid/       # programs that MUST pass `lamo check`
├── invalid/     # programs that MUST fail `lamo check`
├── runtime/     # programs run via `lamo run`, stdout compared to .expected
│   └── import_dup/  # multi-file fixtures may live next to their test
├── smoke/       # parser/diagnostic smoke cases:
│                 #   NAME.expect_err pins required stderr substrings on failure
│                 #   NAME.expect_ok pins warnings while requiring success
│                 #   no directive file => must succeed silently
├── golden/      # generated-C snapshot diffs (NAME.lamo + NAME.c.expected)
└── fixtures/    # helper files imported by tests above
```

Standard library module tests live beside the stdlib they cover:
`std/tests/*.lamo`, executed as part of `make test`. Each module pairs its
implementation (`std/<name>.lamo`) with documentation (`std/<name>.md`) and a
demonstration (`std/examples/<name>_demo.lamo`).

## Adding a New Feature, Path-Wise

- **New syntax** → `src/parser/` (+ AST node if needed) → `src/semantic/`
  checks → SPEC section → both a `valid` and, where relevant, an `invalid`
  test, plus a `smoke` case pinning at least one real diagnostic message.
- **New statement/expression lowering only** → `src/codegen/` + a `golden`
  snapshot so future refactors can't silently change output shape.
- **New standard library surface** → `std/<module>.lamo` + `.md` docs +
  demo + entries in `std/tests`.
