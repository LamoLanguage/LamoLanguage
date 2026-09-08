# Type System Decision Record

**Status:** Decided (2026-07) · **Decision:** Lamo uses **hybrid type
inference**.

This document records the decision and the alternatives that were
considered, so future contributors don't re-litigate it without seeing the
original trade-offs. The spec ([SPEC.md](SPEC.md) §7) is the authoritative
description; this document is the *why*.

---

## 1. Context

At the time of the decision (compiler `2.2.0`), Lamo had:

- A `LamoType` enum with `UNKNOWN | INT | FLOAT | STRING | BOOL | ARRAY | STRUCT`.
- A best-effort compile-time type inference pass in `src/semantic/semantic.c`
  that walks expressions, infers types, and rejects obvious type errors
  (`"abc" * 3` is rejected at compile time, not at runtime).
- Optional type annotations on `let` (`let x: int = 5`) and `fn`
  (`fn add(a: int, b: int) -> int`). When present, the annotation adds a
  compile-time check; when absent, inference fills in.
- Mandatory type annotations on struct fields (`struct Player { hp: int }`).
  A field without a type annotation is a syntax error.
- No `pub` keyword yet (every top-level declaration is exported by default).
- Dynamic typing at the value level: a `let x = 5` followed by `x = "abc"`
  compiles (with a warning when detected).

This is the **de facto** model. The question was: should we make it official,
or pick a different model before shipping more syntax on top?

## 2. Decision

**Adopt the current hybrid model as the official type system.** Concretely:

| Construct            | Annotation policy                                           |
|----------------------|-------------------------------------------------------------|
| `let x = expr`       | Optional. If present, validates inferred type.              |
| `let x: T = expr`    | Same as above; `T` is checked, not coerced.                 |
| `fn f(a, b)`         | Optional on params and return. Best-effort inference.       |
| `fn f(a: T) -> U`    | If any annotation is present, all `return`s are checked.    |
| `struct S { f: T }`  | **Mandatory** on every field.                               |
| `enum E { V }`       | No annotations (variants are int constants).                |
| `impl S { fn m() }`  | Same policy as `fn`.                                         |
| Public API (future)  | When `pub` ships, **mandatory** full annotations on `pub fn`. |

The model is **stable**: no behavior changes from `2.2.0`. This document
formalizes what's already there.

## 3. Alternatives considered

### 3.1 Option A — Full inference everywhere (Hindley–Milner–style)

Every `let` and `fn` infers. Annotations are forbidden or ignored.

**Pros:**
- Maximally lightweight: `let x = 5` with no ceremony.
- One less syntactic category to teach.

**Cons:**
- **Cross-file calls become unreadable.** With `fn foo(a, b)`, the caller has
  to read `foo`'s body to know what types it expects. For a transpiled
  language aiming at "small but real", this kills the "read the API, not the
  implementation" workflow.
- **Errors get worse.** Type errors surface at the call site, not the
  declaration, making the message harder to act on.
- **Refactoring is scary.** Changing `fn foo(a, b)` to take a different type
  silently breaks every caller; with annotations, the compiler tells you
  exactly which signatures changed.
- **Doesn't match the language's stated identity.** Lamo's pitch is "as simple
  as an interpreted language, as fast as a compiled one." Full inference
  serves the first half but undermines the second (compiled languages need
  explicit interfaces for separate compilation).

**Verdict:** rejected for cross-file ergonomics.

### 3.2 Option B — Mandatory annotations everywhere (Java/Go-style)

Every `let`, every `fn` parameter, every return type must be annotated.

**Pros:**
- Maximum explicitness; no surprises.
- Great for IDE support and separate compilation.
- Matches the "small but real" identity.

**Cons:**
- **Kills the lightweight feel.** `let x: int = 5` is verbose for what is
  obviously an `int` from the literal. Compare to TypeScript / modern Python,
  where you only annotate where it matters.
- **Doesn't fit Lamo's dynamic heritage.** Lamo started as "scripts that
  transpile to C"; forcing annotations everywhere would make simple scripts
  noisy.
- **Doesn't add much over Option C** for the cases that matter (function
  signatures across files). Local `let` is rarely the source of type bugs;
  function signatures are.

**Verdict:** rejected for verbosity; the cost is paid for limited gain over
Option C.

### 3.3 Option C — Hybrid (chosen)

Local inference for `let`; optional annotations on `fn`; mandatory on struct
fields; mandatory on future `pub fn`.

**Pros:**
- Lightweight local code: `let x = 5`, `let name = "Arthur"`.
- Explicit at API boundaries: `fn add(a: int, b: int) -> int` is the
  recommended style for any function called from another file.
- Matches what TypeScript, modern Python (PEP 484), and Scala do — a
  well-understood model that real-world teams have validated.
- **Zero migration cost**: this is what `2.2.0` already does. Adopting it as
  official means no compiler changes.

**Cons:**
- Two styles in the same codebase (`let x = 5` vs `fn f(a: int) -> int`) can
  feel inconsistent. Mitigated by documenting the convention.
- Best-effort inference on unannotated `fn` is genuinely weaker than
  full-blown Hindley–Milner. A function `fn f(a, b) { return a + b }` infers
  the return type only by looking at the body, and won't propagate type info
  backward from call sites. This is acceptable for a transpiled language; the
  alternative (full HM) would be a much bigger investment.
- "Optional annotations" is a foot-gun if users think the annotation does
  more than it does. The annotation **checks** but does not **coerce**:
  `let x: float = 5` does NOT produce `5.0`; it produces a compile-time error
  (the literal is `int`). This needs to be clearly documented (it is, in
  SPEC.md §7.1 and §7.3).

**Verdict:** chosen.

### 3.4 Option D — Mandatory on `fn`, optional on `let` (the "ML with inference" model)

A subset of Option C where the rule is: every `fn` MUST be fully annotated,
every `let` infers.

**Pros:**
- Stricter than C; eliminates the "best-effort return-type inference" wart.
- Forces API documentation.

**Cons:**
- **Breaks existing code.** `tests/valid/type_annotations.lamo` has
  `fn greet(name: string, n)` with no annotation on `n` and no return type —
  that compiles today. Option D would reject it.
- Forbidding unannotated `fn` is too strict for a language that wants to feel
  script-y in the small. REPL one-liners like `fn double(n) { return n * 2 }`
  should work.
- Could be revisited as a "strict mode" later (`--strict` flag), but the
  default should stay lenient.

**Verdict:** rejected as default; could be a future `--strict` lint.

## 4. Implications

### 4.1 For the spec

SPEC.md §7 now states the hybrid model as official. No prose changes needed
in `README.md` (it already describes optional annotations correctly).

### 4.2 For the compiler

No code changes. The current behavior in `src/semantic/semantic.c` already
matches the decision:
- `let x = expr` infers.
- `let x: T = expr` validates against `T`.
- `fn f(a, b) { ... }` infers return type from `return` statements.
- `fn f(a: T) -> U { ... }` validates parameters and `return`s.
- `struct S { f: T }` requires field types (parser enforces).

### 4.3 For future work

- When `pub` ships: `pub fn` MUST have full annotations. Private `fn` keeps
  the lenient rule.
- Generics (shipped 2.5.0, see [RFC-generics.md](RFC-generics.md)): the same
  hybrid policy applies — `let xs = [1, 2, 3]` infers `array<int>`, but
  generic function signatures require full annotations
  (`fn map<T, U>(xs: array<T>, ...) -> array<U>`).
- When strict mode ships (`--strict`): turn Option D into an opt-in lint.

### 4.4 For tests

`tests/valid/type_annotations.lamo` continues to pass as-is — its mixed
annotations are explicitly allowed by the spec.

`tests/invalid/type_annotation_mismatch.lamo` and
`tests/invalid/type_annotation_unknown.lamo` continue to fail as expected.

## 5. Reconsideration Triggers

This decision should be revisited if any of the following happens:

- **Generics ship and reveal that hybrid inference can't express important
  patterns.** (E.g., if return-type inference can't propagate through generic
  calls, we may need to tighten the rule on `fn`.)
- **A real `pub` system lands and reveals that "private fn lenient, pub fn
  strict" is too inconsistent.** (Then we either make all `fn` strict or make
  `pub fn` lenient — but the spec needs to update either way.)
- **A formal type-checker pass (separate from semantic analysis) is added.**
  At that point the cost of full HM may be worth paying, and Option A becomes
  viable.
- **Significant real-world codebases exist** and the lack of strict typing is
  causing repeated bugs. (This is the strongest signal — we'd add `--strict`
  mode rather than change the default.)

Until then, the hybrid model is **stable**.

## 6. References

- [SPEC.md](SPEC.md) §7 — the authoritative type-system description.
- `src/semantic/semantic.c` — the implementation (matches the decision).
- `tests/valid/type_annotations.lamo` — positive test for optional annotations.
- `tests/invalid/type_annotation_{mismatch,unknown}.lamo` — negative tests.
- TypeScript's structural typing + JSDoc annotations (inspiration for the
  "annotation checks but doesn't coerce" rule).
- PEP 484 (Python type hints) — inspiration for the "optional annotations,
  gradually typed" model.
