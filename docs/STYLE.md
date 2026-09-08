# Lamo Canonical Code Style

**Status:** Official. This file documents the style that the standard
library, examples, and tests follow. `lamo fmt <file>` normalizes sources
toward it.

## 1. Layout

- Indentation: **4 spaces**, never tabs.
- One statement per line. Semicolons are OPTIONAL (SPEC §3.1); stdlib and
  examples omit them where a newline unambiguously ends the statement. When
  in doubt inside one-liners, keep the semicolon.
- Braces: opening brace on the SAME line (`fn f() {`, `if (c) {`,
  `while (c) {`). Closing brace on its own line. The one-line body form is
  allowed for match arms and trivial guards.
- Blank lines: separate top-level declarations with ONE blank line; inside
  functions only between logical chunks.

## 2. Naming

| Element | Convention | Examples |
|---------|------------|----------|
| variables | lowerCamelCase | `fileCount`, `userName` |
| functions | verbs/subjects in lowerCamelCase | `listPush`, `startsWith`, `resultUnwrapOr` |
| structs / enums | UpperCamelCase | `Player`, `Option` |
| type parameters | SINGLE uppercase letters, role-named when clearer | `T`, `K`/`V` for maps, `E` for errors |
| module alias | short lowercase | `import std.fs as fs` |
| constants | no dedicated form yet — bind with `let` at top level | |

Reserved words can never be names (the parser explains this when you try).

## 3. Type Annotations

Follow the hybrid-inference rules of [TYPE-SYSTEM.md](TYPE-SYSTEM.md) /
SPEC §7.1:

```lamo
let xs: array<int> = []          // annotate when empty or intent-bearing
let n = count + 1                // infer when obvious
fn load(path: string) -> Result<string, string> { ... }   // public API fully annotated
struct Node<T> { value: T, next: int }                    // struct fields ALWAYS annotated
```

Generics spelling notes:

- Prefer `array<T>` over bare `array`; bare `array` triggers a deprecation
  warning (it means `array<any>`).
- Generic call sites usually need NO explicit type arguments (local
  inference): `pick("a", "b")`. Write them when inference cannot decide:
  `pair<int, string>(1, "x")`.
- Constraints use the catalogue names exactly: `T: Ord`, `T: Eq`, `T: Num`,
  `T: Hash`, `T: Show`.

## 4. Control Flow Idiom

- Guard clauses over nesting:
  ```lamo
  if (!ready) { return 0; }
  ```
- `match` arms end without semicolons; keep arms small enough to scan.
- String building in loops uses `+=` on an accumulator variable.

## 5. Errors and Option/Result

Prefer returning `collections.optionSome`/`optionNone` or
`collections.resultOk`/`resultErr` over sentinel values:

```lamo
import std.collections as col;

fn findUser(id: int) -> array {
    if (!exists(id)) { return col.optionNone(); }
    return col.optionSome(load(id));
}
let found = col.optionUnwrapOr(findUser(7), "anon");
print(found);
```

## 6. Comments and Docs

- `//` line comments. A short header comment on every FILE stating purpose;
  every PUBLIC function gets one line describing behavior, and complex ones
  add parameters/returns.
- Mark known simplifications inline instead of TODO-sprawl; link the design
  doc (`RFC §x`, `SPEC §y`) driving the current shape.

## 7. Formatter Policy

`lamo fmt` exists and is the enforcement tool; the canonical rules it
applies are limited to whitespace-level normalization. Bigger normalization
(syntax rewrites) will NOT be added to fmt — style stays human-owned; fmt
stays safe-to-run-on-everything.
