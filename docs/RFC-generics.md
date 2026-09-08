# RFC: Generics for Lamo

**Status:** ✅ **Implemented** — PRs 1–6 shipped (PR 1 in 2.4.0; PRs 2–6 in
2.5.0). PR 5 shipped the `Option`/`Result` API via typed signatures over
erased arrays: struct payloads cannot yet cross module boundaries, and
tagged-union enums remain a separate RFC (see §10 and the
`std/collections.lamo` header notes). Constraints ship with the §6 initial
catalogue (builtins only; structs satisfy `Any`).

| Metadata | |
|----------|-|
| **Depends on** | Type-system decision ([TYPE-SYSTEM.md](TYPE-SYSTEM.md)) — shipped |
| **Author** | Lamo engineering |
| **Discussion** | Open a GitHub issue with the `generics` label |
| **Written against** | Compiler `2.2.0` (motivation §1 reflects that era) |

This RFC proposes a **parametric generics** system for Lamo. The goal is to
unlock real typed collections (`Array<T>`, `Map<K, V>`), optional/error types
(`Option<T>`, `Result<T, E>`), and user-defined generic structs and functions
— without paying for a full Hindley–Milner type checker and without changing
Lamo's "feels like a script" identity.

---

## 1. Motivation

Today (compiler `2.2.0`), Lamo has:

- **Untyped arrays**: `let xs = [1, 2, 3]` is `array`, not `Array<int>`. You
  can write `xs.push("abc")` and the compiler accepts it — the bug only
  surfaces at runtime when you read `xs[3]` expecting an int and get a
  string.
- **No `Option<T>` / `Result<T, E>`**: there's no null, but there's also no
  way to express "this might be absent" or "this might fail" in the type
  system. Code uses sentinel values (`-1`, `""`) or out-of-band error flags.
- **No generic structs**: a `Stack` of `int` and a `Stack` of `Player` have
  to be separate types or be the same untyped `Stack` of `array`.
- **No generic functions**: a `map(fn, xs)` has to be untyped; the compiler
  can't check that `fn`'s parameter type matches `xs`'s element type.

This is the item the user identified as "provavelmente destrava mais coisa
de uma vez" — generics unlock the entire collections story, error handling,
and a meaningful chunk of the stdlib in one go.

## 2. Goals

1. **Typed collections**: `Array<T>`, `Map<K, V>`, `Set<T>` with compile-time
   element type checking.
2. **Option/Result**: `Option<T>` (replaces "use `-1` as a sentinel"), `Result<T, E>`
   (replaces "exit on error").
3. **Generic user types**: `struct Stack<T> { items: Array<T>, top: int }`.
4. **Generic functions**: `fn map<T, U>(xs: Array<T>, f: fn(T) -> U) -> Array<U>`.
5. **Zero runtime cost**: generic types are erased at runtime — `Array<int>`
   and `Array<string>` use the same runtime `LamoArray*` representation.
   Type parameters exist only at compile time.
6. **Local inference**: `let xs = [1, 2, 3]` should infer `Array<int>` without
   an annotation. No need to write `let xs: Array<int> = [1, 2, 3]`.

## 3. Non-goals

- **Full higher-kinded types** (`Functor<T>` etc.). Out of scope; Lamo doesn't
  need them for the collections story.
- **Variance annotations** (`in`/`out`). Lamo generics are **invariant** by
  default, like Rust and Java. Variance is future work if real use cases
  emerge.
- **Generic enums with associated data** (tagged unions). This RFC is about
  parametric generics only. Tagged unions are a separate RFC.
- **Type classes / traits**. Also a separate RFC. Generics here use **where
  clauses** (lightweight) when constraints are needed, not full trait
  dispatch.
- **Specialization**. No overload resolution based on type parameters.

## 4. Proposed syntax

### 4.1 Generic struct declarations

```lamo
struct Stack<T> {
    items: Array<T>,
    top: int
}

struct Map<K, V> {
    keys: Array<K>,
    values: Array<V>
}

struct Pair<A, B> {
    first: A,
    second: B
}
```

- Type parameters are listed in `<...>` after the struct name.
- Multiple parameters allowed: `Map<K, V>`, `Result<T, E>`.
- Parameters can be used as field types.
- **Constraint syntax** (optional): `struct SortedMap<K: Ord, V: Hash> { ... }`.
  The `:` after a type parameter introduces a constraint — `K: Ord` means `K`
  must support `<` `>` `<=` `>=` `==`. See §6 for the constraint catalogue.

### 4.2 Generic enum declarations (when tagged enums ship)

Not part of this RFC. Tagged unions are a separate proposal.

### 4.3 Generic function declarations

```lamo
fn map<T, U>(xs: Array<T>, f: fn(T) -> U) -> Array<U> {
    let result: Array<U> = []
    for (let i = 0; i < xs.len(); i++) {
        result.push(f(xs[i]))
    }
    return result
}

fn first<T>(xs: Array<T>) -> Option<T> {
    if (xs.len() == 0) { return None }
    return Some(xs[0])
}

fn unwrap_or<T>(opt: Option<T>, default: T) -> T {
    match opt {
        Some(x) => return x,
        None => return default
    }
}
```

- Type parameters are listed in `<...>` after the function name.
- Parameters can be used in argument types, return types, and inside the body.
- **All generic functions require full annotations** on params and return type
  (per the type-system decision: hybrid model, mandatory on `fn` API surfaces).
- Function type syntax: `fn(T) -> U` for a function taking `T` and returning
  `U`. First-class functions are not in scope today; the `fn(...)` syntax here
  is a **type annotation only**, used in generic signatures to express
  higher-order operations like `map`. Real first-class function values are a
  separate RFC.

### 4.4 Generic method declarations

```lamo
impl<T> Stack<T> {
    fn push(self, x: T) -> void {
        self.items.push(x)
        self.top += 1
    }
    fn pop(self) -> Option<T> {
        if (self.top == 0) { return None }
        self.top -= 1
        return Some(self.items[self.top])
    }
}
```

- `impl<T> Stack<T> { ... }` — the `<T>` after `impl` declares the type
  parameter, and `Stack<T>` references it.
- Methods can use `T` in their signatures.
- Inside the body, `self` is `Stack<T>` (inferred; no need to annotate).

### 4.5 Generic call sites

```lamo
let s: Stack<int> = Stack { items: [], top: 0 }
s.push(1)
s.push(2)
let top: Option<int> = s.pop()        // Some(2)

let nums: Array<int> = [1, 2, 3]
let doubled: Array<int> = map(nums, fn(x) { return x * 2 })
//                                  ^^^ first-class fn expression (RFC: future)
```

- At call sites, type arguments are usually inferred. Write
  `map(nums, f)` — the compiler infers `T = int, U = int` from the argument
  types.
- Explicit type arguments are allowed when inference can't decide:
  `map<int, string>(nums, int_to_string)`.
- **Turbofish** (`map::<int, string>(...)`) is NOT supported — Lamo uses the
  Java/Scala `<...>` style for consistency with struct literals.

### 4.6 Type aliases (optional, low priority)

```lamo
type IntStack = Stack<int>
type StringMap = Map<string, string>
```

Not strictly required for the collections story, but cheap to add and helps
readability. Marked as "nice to have" — defer if implementation cost is high.

## 5. Inference rules

### 5.1 Array literals

`[e1, e2, e3]` infers `Array<T>` where `T` is the **least upper bound** of the
element types:

- `[1, 2, 3]` → `Array<int>`
- `[1.0, 2.5]` → `Array<float>`
- `[1, 2.5]` → `Array<float>` (int promotes to float; same rule as `1 + 2.5`)
- `["a", "b"]` → `Array<string>`
- `[1, "a"]` → **type error** (no common supertype)

Empty array `[]` is `Array<?>` (unknown element type). It must be assigned to
a typed target or used in a context that fixes the type:

```lamo
let xs: Array<int> = []      // OK, fixes T=int
let ys = []                  // ERROR: cannot infer element type
let zs = []: Array<int>      // OK, alternative syntax (ascription)
```

### 5.2 Struct literals

`Stack { items: [], top: 0 }` infers `Stack<?>` from the field values. If the
target type is annotated (`let s: Stack<int> = Stack { ... }`), the empty
array inherits `T=int`.

### 5.3 Generic function calls

For `map<T, U>(xs, f)`:
1. Look at `xs`'s type. If it's `Array<X>`, candidate `T = X`.
2. Look at `f`'s type. If it's `fn(X) -> Y` (or compatible), confirm `T = X` and
   candidate `U = Y`.
3. If both agree, the call's type is `Array<Y>`.
4. If they conflict, type error.
5. If inference can't decide (e.g., `f` is a polymorphic literal), require
   explicit type arguments: `map<int, string>(xs, f)`.

### 5.4 Subtyping

There is **no subtyping** between generic instantiations. `Array<int>` is not
a subtype of `Array<float>` even though `int` promotes to `float`. This
matches Java's generics (invariant) and avoids the variance rabbit hole.

If you need a `float` array from an `int` array, map it:
```lamo
let ints: Array<int> = [1, 2, 3]
let floats: Array<float> = map(ints, fn(x) { return x + 0.0 })
```

## 6. Constraint catalogue

Constraints are lightweight — we don't introduce a full trait/typeclass
system. Instead, a small fixed set of named constraints covers the common
needs:

| Constraint  | Required operations                  | Example use                       |
|-------------|--------------------------------------|-----------------------------------|
| `Ord`       | `< <= > >= == !=`                    | `SortedMap<K: Ord, V>`            |
| `Eq`        | `== !=`                              | `Set<T: Eq>`                      |
| `Hash`      | `std.hash.hash(x)`                   | `HashMap<K: Hash, V>`             |
| `Show`      | `std.fmt.to_string(x)`               | `fn debug<T: Show>(x: T)`         |
| `Num`       | `+ - * /`                            | `fn sum<T: Num>(xs: Array<T>) -> T` |
| `Any`       | (no constraint; default)             | `fn identity<T>(x: T) -> T`       |

A constraint is just a name for a set of operations the type must support.
If a generic function uses `<` on a `T`, the constraint is `T: Ord`. The
compiler checks that the concrete type passed in supports those operations
(based on its kind: `int` supports `Ord`, `string` supports `Ord`, structs
don't unless they have an `impl Ord for Struct` block — which is a future
trait-like extension, not part of this RFC).

For the initial implementation, constraints are checked against **builtin
types only**: `int`, `float`, `bool`, `string` satisfy `Ord` and `Eq`; `int`
and `float` satisfy `Num`; all types satisfy `Show` via `to_string` if defined
or a default `"@<typename>"` placeholder; all types satisfy `Any`. User
structs satisfy `Any` only. This is enough to ship the collections story.

## 7. Built-in generic types (ship on day 1)

These are part of the standard library but require generics support in the
compiler:

### 7.1 `Array<T>`

The existing dynamic array. After generics, `array` (the bare type) is
deprecated; `Array<T>` is the new spelling. The old `array` keyword remains
as an alias for `Array<Any>` for backwards compatibility.

### 7.2 `Map<K, V>` and `Set<T>`

New types, backed by the existing `std.collections.HashMap` and
`std.collections.HashSet` (which today are untyped). The std implementations
move to typed generics; the untyped versions stay as `Map<Any, Any>` for
back-compat.

### 7.3 `Option<T>`

```lamo
enum Option<T> {
    Some(T),
    None
}
```

(Tagged-union enums SHIPPED in 2.6.0 — the §10 dependency is resolved;
language-level `enum Option<T> { Some(T), None }` with `Some(x) =>`
match bindings is now real, per SPEC §3.5/§4.6.) Provides:

- `Some(x)` constructor.
- `None` literal.
- `unwrap()` — panics if `None`.
- `unwrap_or(default)` — returns `default` if `None`.
- `is_some()` / `is_none()` — bool queries.
- `match` support: `match opt { Some(x) => ..., None => ... }`.

### 7.4 `Result<T, E>`

```lamo
enum Result<T, E> {
    Ok(T),
    Err(E)
}
```

Same shape as `Option`. Replaces today's "exit on error" pattern for
functions that can fail recoverably (file I/O, parsing, etc.). The stdlib
modules (`std.fs`, `std.json`, ...) get new `Result`-returning variants
alongside the existing panicking ones.

## 8. Codegen impact (high level — full plan in implementation PR)

The C backend erases type parameters. The runtime representation of
`Array<T>` for any `T` is the existing `LamoArray*` (which already holds
`LamoValue`s — a tagged union). Type parameters do not appear in generated C.

Concretely:
- `Array<int>` and `Array<string>` both lower to `LamoArray*` in generated C.
- `Stack<T>` (any `T`) lowers to a struct of `{ LamoArray* items; long long top; }`.
- `Option<T>` lowers to a `LamoValue` with type tag `LAMO_VALUE_OPTION` plus
  a payload. (Requires adding `LAMO_VALUE_OPTION` to the runtime enum.)
- Generic functions are **monomorphized** in the codegen: each distinct type
  argument tuple produces a separate C function. `map<int, int>` and
  `map<string, int>` become two C functions: `lamo_user_map__int_int` and
  `lamo_user_map__string_int`.

Monomorphization has a code-size cost but zero runtime cost. For Lamo's scale
(educational, small programs), this is the right trade. If code size becomes
a problem later, a "box everything" erasure strategy (like Java's) is a
fallback — but we start with monomorphization because it gives native-speed
code.

## 9. Migration & back-compat

- **`array` keyword** stays as an alias for `Array<Any>`. Existing code that
  uses `array` keeps compiling.
- **Untyped array literals** (`[1, "a", true]`) become a **warning** under
  the new model — the inferred type is `Array<Any>`, which works at runtime
  but loses the type check. A `--strict` flag (future) turns this into an
  error.
- **Existing stdlib modules** (`std.collections`) gain typed variants. The
  old untyped API stays for one release, then is removed.
- **Existing tests** that use `array` keep passing.
- **No breaking syntax changes.** All new syntax (`<T>`, `Some(x)`, `None`)
  is additive.

## 10. Dependencies & rollout order

This RFC depended on:

1. **Type-system decision** ([TYPE-SYSTEM.md](TYPE-SYSTEM.md)) — shipped.
2. **Tagged-union enums** — `Option<T>` and `Result<T, E>` need enums that
   carry data, not just int constants. This is a separate, smaller RFC. **The
   initial generics rollout shipped WITHOUT struct-backed `Option`/`Result`** —
   `array<T>`, `Map<K, V>`, `Set<T>`, and generic structs/functions shipped
   first; `Option`/`Result` follow as function-shaped APIs until tagged
   enums land. ✅ **RESOLVED (2.6.0):** tagged-union enums shipped as a
   language feature (payload variants, `Some(x) =>` match bindings, generic
   enum parameters) — see SPEC §3.5/§4.6. The `std.collections` function-
   shaped APIs keep working unchanged.

Actual rollout (all complete):

1. **PR 1 — Generic struct declarations + type parameters in field types.**
   ✅ SHIPPED (2.4.0). `struct Pair<A, B> { ... }` and
   `Pair<int, string> { ... }` are accepted and validated; field types
   must be builtins, declared structs, or one of the declared type params.
   The runtime representation is unchanged (all fields are `LamoValue`),
   so different instantiations share the same C layout — true
   layout-per-instantiation monomorphization may land later
   (a packed `array<int>` is the motivating case).
2. **PR 2 — Generic functions + type inference at call sites.**
   ✅ SHIPPED (2.5.0).
3. **PR 3 — `array<T>` typed-array syntax + deprecation warning for bare
   `array`.** ✅ SHIPPED (2.5.0).
4. **PR 4 — `Map<K, V>`, `Set<T>` in stdlib with typed signatures.**
   ✅ SHIPPED (2.5.0).
5. **PR 5 (adjusted scope) — `Option<T>`/`Result<T,E>` APIs in**
   **`std.collections`** as type-checked factories/accessors over erased
   arrays. ✅ SHIPPED (2.5.0). Generic STRUCT definitions of Option/Result
   also parse & check, but struct payloads crossing module boundaries await
   the module-type-flow fix, and match-payload syntax waits on the
   tagged-union enum RFC.
6. **PR 6 — Constraint syntax `T: Ord`.** Catalogue per §6; unknown
   constraint names are errors; satisfaction checked at call sites against
   concrete bindings. ✅ SHIPPED (2.5.0).

Each PR was independently shippable and testable.

## 11. Open questions

- **Variance**: do we need it? My current answer is "no" (invariant by
  default, like Rust). Reconsider if real codebases hit variance-related
  friction.
- **First-class functions** (the `fn(T) -> U` syntax in higher-order
  generics): do we ship the type annotation now and the values later, or
  wait until both are ready? My current answer: ship the annotation now
  (used in `map` etc.) and the values in a separate RFC. The annotation
  without values is still useful because `std.collections` and friends can
  define the API surface; users pass lambdas which the codegen lowers to
  generated C function pointers.
- **Recursive type bounds**: `struct Tree<T> { children: Array<Tree<T>> }` —
  needs the type to refer to itself. Easy to support (just allow it in the
  parser); confirm with tests.
- **Generic type aliases** (`type IntStack = Stack<int>`): low priority,
  defer to a later PR if implementation is non-trivial.
- **Default type arguments** (`struct Foo<T = int>`): not in scope. Java has
  them; Rust doesn't. Skip for now.

## 12. Rejected alternatives

### 12.1 Full HM type inference (Option A from the type-system decision)

Already rejected in `docs/TYPE-SYSTEM.md`. Generics don't change that
decision — generics compose cleanly with hybrid inference (see §5).

### 12.2 Type erasure with boxing (Java-style)

Every generic value becomes a heap-allocated `Any*`. Simpler codegen (no
monomorphization) but pays runtime cost (every int gets boxed). Rejected for
Lamo because the language's pitch is "as fast as compiled" — boxing kills
that for numeric code.

### 12.3 C++ templates

C++ templates are Turing-complete at compile time and produce notoriously
bad error messages. We borrow monomorphization from C++ but NOT the
template-meta-programming model. Generics in Lamo are first-class language
constructs with proper type checking, not a macro system.

### 12.4 Trait objects / dynamic dispatch

`Box<dyn Trait>` style — adds vtables, runtime dispatch, and a trait system.
Way too much for Lamo's scope. Constraints in this RFC are static-only.

## 13. Examples (putting it all together)

### 13.1 A typed stack

```lamo
struct Stack<T> {
    items: Array<T>,
    top: int
}

impl<T> Stack<T> {
    fn push(self, x: T) -> void {
        self.items.push(x)
        self.top += 1
    }
    fn pop(self) -> Option<T> {
        if (self.top == 0) { return None }
        self.top -= 1
        return Some(self.items[self.top])
    }
    fn is_empty(self) -> bool {
        return self.top == 0
    }
}

fn main() {
    let s: Stack<int> = Stack { items: [], top: 0 }
    s.push(1)
    s.push(2)
    s.push(3)
    while (!s.is_empty()) {
        let x: Option<int> = s.pop()
        match x {
            Some(v) => print(v),
            None => print("impossible")
        }
    }
}
```

### 13.2 A generic `map` function

```lamo
fn map<T, U>(xs: Array<T>, f: fn(T) -> U) -> Array<U> {
    let result: Array<U> = []
    for (let i = 0; i < xs.len(); i++) {
        result.push(f(xs[i]))
    }
    return result
}

fn double(x: int) -> int { return x * 2 }

let nums: Array<int> = [1, 2, 3, 4, 5]
let doubled: Array<int> = map(nums, double)
// doubled == [2, 4, 6, 8, 10]
```

### 13.3 Error handling with `Result`

```lamo
import std.fs

fn read_config(path: string) -> Result<string, string> {
    if (!fs.exists(path)) {
        return Err("config file not found: " + path)
    }
    return Ok(fs.readText(path))
}

match read_config("config.txt") {
    Ok(content) => print("loaded: " + content),
    Err(msg) => print("error: " + msg)
}
```

## 14. References

- [TYPE-SYSTEM.md](TYPE-SYSTEM.md) — the hybrid inference decision this
  builds on.
- [SPEC.md](SPEC.md) §7 — the type system (now includes generics, §7.7–7.9).
- [MEMORY-MODEL.md](MEMORY-MODEL.md) — runtime representation generics must
  respect.
- Rust generics (inspiration for monomorphization + invariant by default).
- Java generics (inspiration for type erasure alternative, rejected).
- TypeScript generics (inspiration for inference at call sites).
- Haskell typeclasses (inspiration for constraints — we adopt a much smaller
  version).
