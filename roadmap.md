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

**2.10.0** — the roadmap's top two priorities shipped together:
interpreter value-model completion (arrays and structs in `EVAL_VAL_*`
— refcounted shared objects mirroring the backend's pointer semantics,
with literal/index/method/trait-impl/aliasing parity and byte-for-byte
`print` agreement on the eval suite) closed the last `lamo eval`/`lamo
run` divergence, and trait dictionary dispatch (static per call site)
made `s.area()` inside `fn draw<T: Shape>` compile to a resolved call:
generated `LamoDict_<Trait>` structs of function pointers ride as
hidden trailing parameters, call sites pass static instances built from
the impl registry (or forward the enclosing fn's dictionary through
generic→generic calls), and no runtime type tags are needed. En route:
struct alias flow in semantic (`let q = p` keeps the struct identity),
the chained array-len route (`self.items.len` no longer falls back to
`lamo_make_int(0)`), and constraint-aware diagnostics for every
non-dispatchable receiver shape.

**2.9.0** — TRAITS shipped: `trait Name {
fn sig; ... }` declarations and `impl Trait for Type { ... }` blocks
validated for coherence (one impl per trait/struct pair), completeness
(every required method present) and signature compatibility (strict
arity; annotations compared wherever both sides annotate). Trait names
are first-class generic constraints — `fn draw<T: Shape>(s: T)` —
enforced at call sites against the impl registry for functions, structs
and enums (SPEC §3.7/§7.8). Generic trait impls (`impl<T> Printable for
Stack<T>`) satisfy every instantiation. En route: honest compile-time
errors for method calls on bare type-parameter receivers (previously a
silent `lamo_make_int(0)` in the C backend) and struct-literal arguments
now carry their concrete full type into §7.7 binding and constraint
enforcement (closing the same gap for the built-in catalogue).

**2.8.0** — the 2.7.0 Open Follow-Ups ledger closed: eval/REPL enum support
(`EVAL_VAL_ENUM` + an interpreter enum registry — enum declarations,
constructor calls, and `match` now run under `lamo eval`/`lamo repl` with
run parity), match literal patterns (`1 =>`, `"a" =>`, negative numerics,
nested literals — plus the latent invalid-C bug for guarded untagged match
arms it uncovered), match as an expression (`let x = match ...` value
threading through semantic, the C backend, and the interpreter), enum
annotation type-arg invariance at call sites (partially-inferable enums
like `enum E<T, U> { V(T) }` complete against annotations instead of
degrading unchecked), and the full `run_tests.ps1` parity (smoke, golden,
and std sections plus runtime `.stdin` support on a real-exit-code process
runner).

**2.7.0** — the 2.6.0 Open Follow-Ups ledger closed: enum type annotations
(`let o: Option<int>`, params/returns/fields with arg-count validation and
ctor full-type inference), nested payload patterns + `when` guards in
`match` (guarded arms excluded from exhaustiveness), `Enum::Variant`
qualification with cross-enum "later wins" collisions defined as legal,
`pub` step 2 enforced (non-`pub` via alias is a compile error, REPL
included), and the eval-cases section added to `run_tests.ps1`.

**2.6.0** — the entire Open Follow-Ups ledger closed: tagged-union enums
(`Some(x) =>` bindings), module-boundary struct type flow, explicit type
arguments on module member calls, the boolean print form decision
(`true`/`false`), and the module rules set (entry `main()` actually invoked,
contextual `pub` with the §10.6 step-1 warnings, folder-based modules,
formalized duplicate-import behavior, and module loading in eval/REPL).

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
| 10 | Multi-file projects | ✅ Done — module rules closed in 2.6.0; `pub` boundary enforced in 2.7.0 ([SPEC §10](docs/SPEC.md)) |
| 11 | Backend evolution | ✅ Done — annotated-AST contract; VM/LLVM deferred with preconditions |

Per-item detail lives in [`todo.md`](todo.md).

## Current Priorities

The next meaningful work, in suggested order (2.10.0 closed the
value-model and dictionary-dispatch items — see the Release Highlights
above):

1. **Trait dispatch polish** — impl-level trait constraints
   (`impl<T: Shape> ...`), trait objects / existential parameters if a
   use case appears, and dict-argument support on module-aliased calls.
2. **Better formatter** (AST-based pretty-printer), **LSP**, **VSCode
   extension**, **documentation website**.
3. **Windows CI contributor** — `run_tests.ps1` is now section-complete;
   a Windows host in CI would gate the parity the POSIX suite already
   enforces.

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
- string interpolation

## Final Note

The most important milestone is still not "more syntax". It is the moment
when Lamo has a semantic model strong enough that changing the backend does
not change what the language means.
