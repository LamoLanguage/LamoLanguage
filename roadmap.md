# Lamo Roadmap

## Goal

Turn Lamo from an educational transpiler prototype into a small but real programming language with:

- a defined type system
- semantic validation
- predictable runtime behavior
- usable tooling
- a stable compilation pipeline

## Authoritative Design Documents

These live under `docs/` and are the source of truth for their respective
areas. The roadmap tracks *what's done* and *what's next*; the design docs
track *what it means*.

| Document | Scope |
|----------|-------|
| [`docs/SPEC.md`](docs/SPEC.md) | The language specification (syntax, types, semantics, runtime, modules). Authoritative for "what the language means". |
| [`docs/TYPE-SYSTEM.md`](docs/TYPE-SYSTEM.md) | Type-system decision record (hybrid inference). |
| [`docs/MEMORY-MODEL.md`](docs/MEMORY-MODEL.md) | Memory model and GC design + rollout plan. |
| [`docs/RFC-generics.md`](docs/RFC-generics.md) | Parametric generics design — implemented in 2.5.0. |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Frontend/backend boundary + the annotated-AST information contract codegen must honor. |
| [`docs/DIRECTORY-LAYOUT.md`](docs/DIRECTORY-LAYOUT.md) | Where everything lives and why. |
| [`docs/STDLIB.md`](docs/STDLIB.md) | Core-vs-stdlib boundary and module organization rules. |
| [`docs/STYLE.md`](docs/STYLE.md) | Canonical `.lamo` style; formatter policy. |

## Release Highlights

**2.5.0** — Generics PRs 1–6 complete: generic functions with inference and
explicit call arguments, typed `array<T>`, constraints (`T: Ord`), `impl<T>`,
typed stdlib collections plus `Option`/`Result` APIs. Also: `print()` driven
by semantic types, the full smoke + golden test harness, and the
architecture/layout/style documentation set.

**2.4.0** — Generic struct declarations (`struct Pair<A, B>`) with type
parameters in field types.

**2.3.0** — Opt-in mark-sweep GC shipped (Steps 2–6 of the memory-model
rollout), `eval`/`run` split formalized, `lampm` scope cut, GCC failure
reporting, always-emit `lamo_exec.c` policy.

**Earlier** — Official standard library (15 modules), structs/methods/arrays/
enums/match, namespaced imports, spec + type-system + memory-model decisions,
CLI refactor into `src/cli/`.

## Phase Status

| Phase | Focus | Status |
|-------|-------|--------|
| 1 | Stabilize the core compiler | ✅ Done |
| 2 | Strengthen semantic analysis | ✅ Done |
| 3 | Define a real type system | ✅ Done — hybrid inference ([TYPE-SYSTEM.md](docs/TYPE-SYSTEM.md)) |
| 4 | Build a minimal runtime | ✅ Done — arena + strings + GC decision |
| 5 | Runtime design (GC rollout) | ✅ Done — mark-sweep GC shipped in 2.3.0 |
| 6 | Language specification | ✅ Done — [SPEC.md](docs/SPEC.md) |
| 7 | Language features & generics | ✅ Done — generics PRs 1–6 shipped (2.5.0) |
| 8 | Standard library | ✅ Done — 15 modules + organization rules |
| 9 | Developer experience | ✅ Done — REPL, scaffolding, hints, color, formatter |
| 10 | Multi-file projects | ✅ Mostly done — visibility decision recorded; some module rules still open |
| 11 | Backend evolution | ✅ Done — annotated-AST contract; VM/LLVM deferred with preconditions |

Per-item detail lives in [`todo.md`](todo.md).

## Current Priorities

The next meaningful work, in suggested order:

1. **Tagged-union enums** — payload-carrying variants (`Some(x) =>` binding).
   Prerequisite for pattern-matched `Option`/`Result` per the generics RFC §10.
2. **Module-boundary type flow** — let imported functions return struct-typed
   values usable for field access/methods in the importing file (today they
   erase to opaque arrays across module boundaries).
3. **Explicit type arguments on module member calls** (`col.f<int>(...)`) —
   plain calls accept them today; member chains do not.
4. **Boolean print form** — decide whether `print(true)` should render
   `true`/`false` instead of `1`/`0`, and spec it before anyone depends on it.
5. **Remaining module rules** (Phase 7/10 leftovers) — explicit export rules,
   package/folder-based modules, duplicate-import behavior, entry-file vs
   library-file expectations, and module loading for `lamo eval`/`lamo repl`.

## Guiding Principles

- Keep the language small and coherent.
- Prefer correctness before adding a lot of syntax.
- Define behavior explicitly before optimizing it.
- Build features in vertical slices: syntax → AST → semantics → backend → tests → docs.
- Replace temporary shortcuts once they start shaping language behavior.

## Definition Of "Real Language"

Lamo becomes a real language when:

- the language behavior is defined by Lamo, not by accidental C behavior
- invalid programs fail early and clearly
- values and types have consistent rules
- multi-file programs have explicit module semantics
- the compiler is tested enough that changes are safe
- documentation describes reality

## Nice-To-Have Later

- debugger integration
- editor support (LSP, VSCode extension)
- optimization passes
- generational / incremental / concurrent GC
- `pub` visibility keyword
- string interpolation

## Final Note

The most important milestone is still not "more syntax". It is the moment
when Lamo has a semantic model strong enough that changing the backend does
not change what the language means.
