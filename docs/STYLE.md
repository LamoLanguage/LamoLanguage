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

`lamo fmt` is the enforcement tool and is **AST-based** (2.11.0): the
file is parsed with the real parser and re-emitted from the AST with the
canonical style of this document. The canonical rules the formatter
applies:

- 4-space indentation; one statement per line; opening brace on the
  header line, closing brace dedented to the block's level.
- Explicit semicolons on simple statements (`let`, assignments,
  `return`, call statements).
- Space normalization around operators (`a + b`, `!flag`, `arr[i]`,
  `f(x, y)`); struct/enum/match/struct-literal bodies get one member
  per line with trailing commas; call/array/param lists do not.
- MINIMAL PARENTHESIZATION: parentheses are re-derived from operator
  precedence. Redundant parens the parser recorded as grouping nodes
  are dropped (`(a) + b` → `a + b`); required parens are kept
  (`(a + b) * c`, `-(a && b)`).
- Literals normalize safely: hex/binary/underscore ints print as
  decimal (same value); floats print in the shortest form that
  round-trips to the exact same `double` (and keep a visible `.0` /
  exponent so float-ness survives).
- `x++` / `x--` are preserved (the parser desugars them to
  `x = x + 1`; the formatter re-detects the pattern).
- Comments are not AST nodes; they are re-attached by source line
  position. Comment TEXT is never altered or dropped — position can
  only shift when a comment sat inside a multi-line expression.
- Blank lines: exactly one between top-level declarations (imports
  cluster with imports, top-level `let` with `let`); function bodies
  are compact.

Safety contract: a file that does not parse cleanly is NEVER rewritten
from the AST — fmt falls back to whitespace-only normalization
(CRLF → LF, tabs → 4 spaces, trailing whitespace stripped, one final
newline) and notes the downgrade on stderr. `fmt` therefore never
breaks a file it cannot fully understand. Syntax rewrites that change
meaning remain out of scope: everything the formatter emits parses back
to an equivalent program (verified per-file by the `tests/fmt` suite).
