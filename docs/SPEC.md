# Lamo Language Specification

**Version:** 1.2 (matches compiler `2.5.0`)

**Status:** Authoritative. When this document and the compiler disagree, the
**compiler is the bug** (for spec'd behavior) or the **spec is the bug** (for
unspecified behavior). Either way, file an issue.

This document defines Lamo's syntax, static semantics, runtime semantics,
memory model, and standard library contract. It is the single source of truth
for "what the language means" — independent of any specific backend (today the
C transpiler; future interpreters or VMs must conform to this spec, not the
other way around).

> **Companion documents**
> - `docs/TYPE-SYSTEM.md` — Type-system decision record (hybrid inference).
> - `docs/MEMORY-MODEL.md` — Memory model and GC design.
> - `docs/RFC-generics.md` — Generics design (implemented in 2.5.0).
> - `../README.md` — User-facing quickstart.
> - `../todo.md` — Implementation tracking (this spec drives future TODO items).

---

## 1. Lexical Structure

### 1.1 Source format

- Source files use the `.lamo` extension.
- Source encoding is **UTF-8**. The lexer treats bytes 0x80–0xFF as part of
  string literals but does not perform Unicode normalization. Identifiers are
  restricted to ASCII (see §1.5).
- Both LF and CRLF line endings are accepted. The lexer normalizes CRLF to LF
  internally before tokenizing, so line/column numbers are consistent.
- A file MUST end with a newline. The formatter (`lamo fmt`) enforces this.

### 1.2 Comments

- `//` begins a line comment that runs to the end of the line. Block comments
  are not supported.
- Comments are stripped by the lexer and produce no tokens.

```lamo
let x = 5;       // this is a comment
let y = x + 1;   // so is this
```

### 1.3 Whitespace

Whitespace (space, tab, CR, LF) separates tokens but is otherwise insignificant.
The formatter normalizes tabs to 4 spaces, but the lexer accepts either.

### 1.4 Literals

| Literal kind        | Syntax                                          | Type      |
|---------------------|-------------------------------------------------|-----------|
| Integer (decimal)   | `42`, `0`, `-5` (unary minus on literal)        | `int`     |
| Integer (hex)       | `0xFF`, `0x1A2B`                                | `int`     |
| Integer (binary)    | `0b1010`, `0b0001`                              | `int`     |
| Integer (underscore)| `1_000_000`, `0xFF_FF`                          | `int`     |
| Float               | `3.14`, `0.5`, `2.0`                            | `float`   |
| String              | `"abc"`, `"with\nescapes"`                      | `string`  |
| Boolean             | `true`, `false`                                 | `bool`    |

`int` is a 64-bit signed integer (C `long long` on most platforms). Float is
IEEE 754 double-precision (C `double`).

#### String escapes

Inside a `"..."` literal, the following escapes are recognized:

| Escape  | Meaning                          |
|---------|----------------------------------|
| `\n`    | newline (0x0A)                   |
| `\t`    | horizontal tab (0x09)            |
| `\r`    | carriage return (0x0D)           |
| `\\`    | backslash                        |
| `\"`    | double quote                     |
| `\0`    | null byte (0x00)                 |
| `\xNN`  | byte with hex value NN           |

Any other `\X` is a **lexical error**. Raw newlines inside a string literal
are also a lexical error — use `\n` instead.

### 1.5 Identifiers

An identifier is `[A-Za-z_][A-Za-z0-9_]*`. Identifiers are case-sensitive.
`foo` and `Foo` are distinct.

### 1.6 Keywords

The following identifiers are reserved and cannot be used as variable or
function names:

```
let fn if else while for return break continue
import as struct impl enum match true false
int float bool string void array
```

`pub`, `match`, `self`, and `std` are contextually reserved: `self` is only
meaningful inside an `impl` block; `std` is a path prefix in `import std.X`
(see §10).

### 1.7 Operators and punctuation

```
+  -  *  /  %         arithmetic
+= -= *= /= %=        compound assignment
=                     assignment
== != < <= > >=        comparison
&& || !               logical
++ --                 increment / decrement
(  )  [  ]  {  }       grouping / array / block
,  ;  :  ::  ->  .     punctuation (`::` = variant qualification, §3.5)
```

`;` is optional at the end of a statement (see §3.1).

---

## 2. Grammar (informal)

This section gives an informal EBNF-ish grammar. Where the grammar is
ambiguous, the prose in §3–§9 wins. The parser is recursive descent and uses
the precedence in §6.2.

```
program       := top_decl*
top_decl      := import_decl
              | struct_decl
              | enum_decl
              | impl_decl
              | trait_decl
              | fn_decl
              | let_decl

import_decl   := 'import' ( string_lit [ 'as' IDENT ] | IDENT [ 'as' IDENT ] ) ';'?
struct_decl   := 'struct' IDENT type_params? '{' field_list '}'
type_params   := '<' tp (',' tp)* '>'                      // PR6: optional constraint per parameter
tp            := IDENT [':' IDENT]                          // e.g. T, K: Ord, V: Hash — 2.9.0: also a declared trait
field_list    := field ( (',' | ';' | NEWLINE) field )*
field         := IDENT ':' type_ann
enum_decl     := 'enum' IDENT '{' variant_list '}'
variant_list  := IDENT ( (',' | NEWLINE) IDENT )*
impl_decl     := 'impl' type_params? ( IDENT ['for' IDENT type_args?]
                                     | IDENT type_args? ) '{' fn_decl* '}'
                // 2.9.0: `impl Shape for Circle { ... }` (trait impl) OR
                // the inherent form `impl Circle { ... }` / `impl<T> Stack<T> { ... }`
trait_decl    := 'trait' IDENT '{' trait_sig* '}'           // 2.9.0 (§3.7)
trait_sig     := 'fn' IDENT '(' param_list? ')' ('->' type_ann)? ';'?
fn_decl       := 'fn' IDENT type_params? '(' param_list? ')' ('->' type_ann)? block
param_list    := param (',' param)*
param         := IDENT (':' type_ann)?
let_decl      := 'let' IDENT (':' type_ann)? '=' expr
type_ann      := ('int' | 'float' | 'bool' | 'string' | 'void') | array_ann | IDENT [ type_args ]
                 // Generics PR2/PR3: full recursive annotations. `array<T>` and nested
                 // instantiations (`Pair<int, array<string>>`) are legal at EVERY annotation
                 // position. Bare `array` is a deprecated alias for array<any> (warning).

block         := '{' statement* '}'
statement     := let_decl
              | return_stmt
              | break_stmt
              | continue_stmt
              | if_stmt
              | while_stmt
              | for_stmt
              | match_stmt
              | assign_stmt
              | call_stmt
              | expr ';?'

return_stmt   := 'return' expr? ';'?
break_stmt    := 'break' ';'?
continue_stmt := 'continue' ';?'
if_stmt       := 'if' '(' expr ')' block ('else' (if_stmt | block))?
while_stmt    := 'while' '(' expr ')' block
for_stmt      := 'for' '(' for_init? ';' expr? ';' for_update? ')' block
for_init      := let_decl | assign_stmt
for_update    := assign_stmt
match_stmt    := 'match' expr '{' match_arm (','? match_arm)* ','? '}'
match_arm     := (IDENT | '_') '=>' (expr | block)

assign_stmt   := lvalue ('=' | '+=' | '-=') expr
              | lvalue '++'
              | lvalue '--'
lvalue        := IDENT
              | IDENT '[' expr ']'
              | IDENT '.' IDENT
              | expr '.' IDENT          // for `obj.field = x`

call_stmt     := expr '(' arg_list? ')' ';?'

expr          := logic_or
logic_or      := logic_and ('||' logic_and)*
logic_and     := equality ('&&' equality)*
equality      := comparison (('==' | '!=') comparison)*
comparison    := add (('<' | '<=' | '>' | '>=') add)*
add           := mul (('+' | '-') mul)*
mul           := unary (('*' | '/' | '%') unary)*
unary         := ('-' | '!') unary | postfix
postfix       := primary ('.' IDENT | '[' expr ']' | '(' arg_list? ')')*
primary       := INT_LIT | FLOAT_LIT | STRING_LIT | 'true' | 'false'
              | IDENT | '(' expr ')' | array_lit | struct_lit
array_lit     := '[' (expr (',' expr)*)? ']'
struct_lit    := IDENT type_args? '{' field_init (',' field_init)* '}'
type_args     := '<' type_ann (',' type_ann)* '>'      // PR2: arguments may be nested types or
                                                        // in-scope type parameters

// Generics PR 2 §4.5: explicit call-site type arguments (`pick<int>(1, 2)`).
// Accepted only when the scanner confirms the full `< ... > (` shape so that
// comparisons can never misparse. Turbofish `f::<int>` is NOT supported.
field_init    := IDENT ':' expr
```

---

## 3. Top-Level Declarations

### 3.1 Semicolon rules

A statement may be terminated by an optional `;`. Both forms are accepted:

```lamo
let x = 5
let y = 6;
```

This is intentional: REPL one-liners feel lighter without `;`, while
multi-statement lines in real files stay readable with `;`. The parser treats
a newline as a soft terminator — if the next token would extend the current
statement (e.g. a binary operator), parsing continues across the newline.

### 3.2 `let` declarations

`let name = expr` declares a variable in the current scope. The variable's
type is inferred from `expr` (see §7.1). An optional annotation `let name: T = expr`
asserts that the inferred type is `T` — if it isn't, the compiler emits a type
error. The annotation does **not** cause a coercion; it only checks.

`let` is also valid at the top level. Top-level `let`s become global mutable
variables (not constants). Their initializers run in `main()` before user
code, in declaration order. This means user functions that reference a global
see the initialized value, not zero.

### 3.3 `fn` declarations

```
fn name(param_list) -> ReturnType { body }
```

- Parameters may have type annotations: `fn add(a: int, b: int) -> int`.
- The return type annotation `-> T` is optional. If present, `return` statements
  inside the body are checked against it (see §7.3). If absent, the return type
  is inferred from the first `return` (best-effort; mixed `return 5` and
  `return "x"` in the same function is a type error).
- A function with no `return` and no return-type annotation has return type
  `void`.
- Functions **are hoisted within a file**: the semantic pass runs a global
  pre-pass that registers all top-level functions before visiting statements,
  so a function may be called before its declaration appears in source order.
- Functions may be recursive (self-reference is allowed within the body).
- Functions may not be nested. There are no closures.

### 3.4 `struct` declarations

```
struct Player {
    name: string,
    hp: int,
    level: int
}
```

- Fields are separated by `,`, `;`, or newlines (any mix).
- Every field MUST have a type annotation. The annotation may be `int`,
  `float`, `bool`, `string`, `array`, `void`, or another struct name.
- Field types are annotations only at runtime — Lamo is dynamically typed at
  the value level (see §7.4) — but the compiler uses them for static field
  validation. Accessing a non-declared field is a compile-time error.
- Structs may not contain themselves by value (no direct recursion). A struct
  may contain an `array` of its own type.

#### 3.4.1 Generic struct declarations (Generics PR 1)

```
struct Box<T> {
    value: T,
    label: string
}

struct Pair<A, B> {
    first: A,
    second: B
}
```

- A struct may declare type parameters in `<...>` immediately after its name.
  Multiple parameters are comma-separated: `Pair<A, B>`, `Map<K, V>`.
- Type parameter names must be unique within the declaration
  (`struct Dup<T, T>` is a compile-time error).
- Field types may reference any declared type parameter (`value: T`,
  `first: A`, `second: B`). Field types that are neither builtins, declared
  structs, nor one of the declared type parameters are a compile-time error
  (catches typos like `struct Pair<A, B> { x: C }`).
- The type parameter list is optional. Omitting it produces a regular
  non-generic struct — fully backwards-compatible with pre-2.4.0 syntax.

#### 3.4.2 Generic struct literals (Generics PR 1)

```
let bi = Box<int> { value: 42, label: "answer" }
let p  = Pair<int, string> { first: 1, second: "one" }
```

- A generic struct is instantiated by listing concrete type arguments in
  `<...>` between the struct name and the `{` of the literal.
- The number of type arguments must match the number of type parameters
  declared on the struct (`Pair<int>` is a compile-time error — `Pair`
  expects 2 type arguments).
- Type arguments must be known types: builtins (`int`, `float`, `bool`,
  `string`, `array`) or declared struct names. Using an unknown identifier
  as a type argument is a compile-time error.
- Non-generic structs MUST NOT receive type arguments
  (`Plain<int> { ... }` is a compile-time error if `Plain` has no type
  parameters).
- The runtime representation of a generic struct is the same regardless of
  type arguments — every field is a tagged `LamoValue`. Type arguments
  exist purely for compile-time type checking; they are erased at runtime.
  This is the "monomorphization with shared layout" strategy from the
  generics RFC §8 — different instantiations of the same generic struct
  share the same C layout for PR 1. Layout-per-instantiation specialization
  (e.g. packed `Array<int>`) will land with PR 3.

#### 3.4.3 Disambiguation: `<` as type args vs comparison

In expression position, `Foo < bar` is ambiguous between a generic struct
literal `Foo<...>` and a comparison expression. Lamo resolves this by
lookahead: if the tokens after `Foo` match the pattern
`< IDENT (, IDENT)* > {`, the construct is parsed as a generic struct
literal; otherwise `<` is treated as a comparison operator. This means
`if (a < b) { ... }` continues to work, but `Foo<int> { ... }` is also
recognized as a struct literal. The `match` scrutinee is exempt — `match
c < X > { ... }` is always parsed as a comparison scrutinee (the `{`
belongs to `match`), not as a generic struct literal.

### 3.5 `enum` declarations

**Simple (untagged) enums:**

```
enum Color {
    Red,
    Green,
    Blue
}
```

- Each variant becomes a top-level `int` constant (`Red = 0`, `Green = 1`, …).
- Variants are accessible by bare name, or with the `Enum::Variant`
  qualifier (2.7.0) — see below.

**Tagged-union enums (2.6.0):** variants may carry payloads:

```
enum Option<T> {
    Some(T),
    None
}

enum Result<T, E> {
    Ok(T),
    Err(E)
}

enum Shape {
    Circle(int),
    Rect(int, int),
    Point
}
```

Grammar: a variant is `IDENT [ '(' TYPE (',' TYPE)* ')' ]`. Payload types
use the same annotation grammar as struct fields — builtins, declared
structs, the enum's own type parameters (§3.4.1 rules apply: parameters
are declared with `<T, ...>` after the enum name, constraints use the
§7.8 catalogue), or nested generics of those.

Semantics:

- An enum is a **tagged union** when at least one variant carries a
  payload. A tagged enum's values are a distinct runtime kind (tag +
  payloads + variant name); they are NOT ints and never mix with the
  untagged int representation. Untagged enums keep the exact §3.5 int
  behavior — existing programs are unaffected.
- **Construction:** a payload variant used as a call (`Some(42)`,
  `Rect(3, 4)`) constructs a value; the argument count must equal the
  variant's payload count, and concrete (non-type-parameter) payload
  annotations are checked against the arguments with the same numeric
  widening as §7.3. Type-parameter payloads (`Some(x)` where the payload
  is `T`) are erased at runtime, like all generics (RFC §8).
- **Unit variants inside a tagged enum** (`None`, `Point`) are values of
  the tagged kind, usable by bare name.
- A payload variant used WITHOUT a call is a compile error — a payload
  variant is a constructor, not a value:
  `variant 'Some' carries a payload and cannot be used as a value`.
- **Equality** (`==` / `!=`): two tagged values are equal when their
  variant tags match AND their payloads compare equal element-wise.
  Comparing values of different enums (or an enum against a non-enum) is
  simply false, never an error.
- **Truthiness:** tagged values are always truthy (like structs). Match
  on the variant, not on truthiness.
- **Printing:** tagged values render `Some(42)`, `None`, `Err("oops")` —
  including inside arrays and structs.

**Variant qualification (2.7.0):**

- `Enum::Variant` names a variant exactly — `Option::Some(5)`,
  `Option::None` (as a value), `Shape::Circle(r) => ...` (as a pattern).
- Qualified lookups bypass bare-name resolution entirely; they are the
  disambiguation tool for cross-enum collisions.
- A qualified PAYLOAD variant still cannot be used as a value without a
  call (`Option::Some` alone is an error, with a hint pointing at
  `Option::Some(...)`).
- In expressions, qualified constructor calls reuse the regular call
  forms; qualified unit variants are first-class values.

**Variant-name collisions (2.7.0):** the §3.5 "later wins" wart is now
the DEFINED behavior and is legal:

- Two enums may declare the same variant name. The LATER enum's variant
  wins for bare references, and the compiler emits a one-time warning:
  `variant 'Item' of enum 'Second' shadows variant 'Item' of enum 'First'; bare 'Item' now refers to the later declaration (disambiguate with Second::Item or First::Item)`.
- Either variant remains reachable through `Enum::Variant` qualification.
- A bare variant that resolves to a payload variant still cannot be used
  as a value (the §3.5 payload rule applies to whatever the bare name
  resolves to).

**Enum type annotations (2.7.0):** `let`, parameters, returns, for-lets,
struct fields, and enum payloads all accept enum-typed annotations:

```
let a: Option<int> = Some(42);
let b: Shape = Circle(3);

fn unwrap_or(o: Option<int>, d: int) -> Option<int> { ... }
```

- The annotation resolver accepts any declared enum as an annotation
  head; enum payloads and struct fields may reference enums anywhere a
  type is allowed (including nests like `array<Option<int>>`).
- A generic enum requires exactly `type_param_count` type arguments:
  bare `Option` (where `enum Option<T>`) and `Option<int, string>` are
  compile errors, and every argument must itself resolve (builtins,
  structs, enums, in-scope type parameters). A non-generic enum rejects
  type arguments.
- The annotation's enum must match the initializer's enum: `let o:
  Option<int> = Err("x")` errors with `type annotation 'Option<int>'
  does not match enum 'Result' value`.
- Constructor calls carry their concrete full type (`Some(42)` has type
  `Option<int>`, inferred by binding the enum's type parameters against
  the payload arguments), so annotated parameters/returns participate in
  the §7.7 call-site binding machinery: `fn f(o: Option<int>)` called
  with `f(Some(5))` binds and checks like any generic signature.
- The runtime representation is unchanged (erasure) — annotations are
  purely compile-time.

### 3.6 `impl` blocks

```
impl Player {
    fn damage(amount: int) {
        self.hp -= amount
    }
}
```

- `impl Type { ... }` attaches methods to a previously-declared struct `Type`.
- Inside an `impl` method body, `self` refers to the receiver. It is implicit;
  do NOT declare it as a parameter.
- Methods are called as `obj.method(args)`. Method arity is validated at
  compile time.
- Methods can be chained: `p.damage(10).heal(5)` works if `damage` returns
  the receiver (today, methods that don't explicitly `return` return `void`).
- A method may be defined before or after the struct it operates on; the
  semantic pass runs in two phases (collect struct/enum/impl definitions,
  then visit function bodies).

### 3.7 `trait` declarations (2.9.0)

```
trait Shape {
    fn area() -> float;
    fn name() -> string;
}

impl Shape for Circle {
    fn area() -> float { return 3.14159 * self.r * self.r }
    fn name() -> string { return "circle" }
}
```

- `trait Name { ... }` declares a NAMED SET OF REQUIRED METHOD SIGNATURES —
  a compile-time contract. Traits generate no code and add nothing to the
  runtime; they exist so the compiler can check two things:
  1. **impl completeness** — every `impl Name for Type` block implements
     the whole contract (see below);
  2. **first-class generic constraints** — `T: Name` is valid wherever the
     built-in catalogue constraints (§7.8) are, checking call sites
     statically ("first-class constraints beyond the catalogue").
- Trait bodies contain ONLY method signatures: `fn name(params) [-> Type];`.
  A body (`{ ... }`) inside a trait is a syntax error, with a diagnostic
  pointing at `impl Trait for Type`. Parameter and return annotations use
  the same grammar as functions (§3.3) but may NOT reference type
  parameters — traits are non-generic contracts in 2.9.0 (genericity lives
  on the impl/struct side). Duplicate method names within a trait are
  compile errors.
- `impl Trait for Type { ... }` attaches methods to a declared struct AND
  asserts the trait contract. Validation (all compile errors):
  - the trait and the struct must be declared (order-independent — traits
    hoist like structs, so the impl may appear before the trait);
  - a struct may implement a given trait at most once ("coherence");
    duplicate `impl Trait for Type` pairs are errors;
  - every trait method must be present among the struct's methods, with
    the SAME ARITY (strict) and matching annotations wherever BOTH the
    trait and the impl annotate a parameter or the return type (missing
    annotations never conflict — Lamo keeps annotations optional);
  - extra methods beyond the trait's requirements are allowed and behave
    as ordinary inherent methods.
- Trait-impl methods are ordinary methods at runtime: they emit as
  `lamo_method_<Type>__<name>` and resolve through the same machinery as
  inherent methods (§3.6). `self` works identically. A struct's method is
  callable whether or not the caller mentions the trait — the trait is a
  checked contract, not a namespace.
- Generic trait impls are supported with the RFC §4.4 echo form:
  `impl<T> Printable for Stack<T> { ... }`. The echo must name the impl's
  own parameters exactly (same validation as generic inherent impls). A
  generic impl satisfies the trait for EVERY instantiation (`Stack<int>`
  and `Stack<string>` both satisfy `Printable`).
- Traits may be marked `pub trait` in module files (§10.6 marker accepted;
  like structs/enums, trait NAMES are not module-registry members).
- **Static dispatch only.** Calling a method on a value whose type is a
  bare type parameter (`s.area()` where `s: T`) is a compile error:
  generics are erased at the backend and there is no vtable/mono
  machinery (RFC-generics §12.4). Trait constraints check CALL SITES;
  they do not enable dynamic dispatch. Constraint propagation through
  generic-to-generic calls is likewise not checked for traits OR for the
  built-in catalogue (§7.7 limitation, mirrored exactly).

### 3.8 `import` declarations

See §10.

---

## 4. Statements

### 4.1 `if` / `else`

```lamo
if (cond) { ... }
if (cond) { ... } else { ... }
if (cond) { ... } else if (other) { ... } else { ... }
```

The condition uses **Python-like truthiness** (see §7.5). The body is a block.
`else if` is parsed as `else { if (...) { ... } }`.

### 4.2 `while`

```lamo
while (cond) { ... }
```

Repeats the body while `cond` is truthy. `break` exits the loop; `continue`
jumps to the next iteration. Both are compile-time errors outside any loop.

### 4.3 `for`

```lamo
for (let i = 0; i < 10; i++) { ... }
```

C-style for. The init clause is a `let` or an assignment; the condition is an
expression; the update is an assignment (including `++` / `--`). Any of the
three may be omitted: `for (;;) { ... }` is an infinite loop.

There is no `for-in` / iterator form today. That is future work (§13).

### 4.4 `return`

```lamo
return expr
return          // bare return; function returns void
```

`return` outside a function is a compile-time error (top-level `return` was
rejected explicitly — the language does not have "script semantics" for it).
The expression's type is checked against the function's declared return type
when one is declared.

### 4.5 `break` and `continue`

Valid only inside `while` or `for`. Otherwise a semantic error.

### 4.6 `match`

```lamo
match color {
    Red => print("red"),
    Green => print("green"),
    _ => print("other")
}
```

- Patterns are enum variant names, variant names with payload bindings
  (2.6.0: `Some(x) => ...`), qualified variants (2.7.0:
  `Enum::Variant(x) => ...`), or `_` (wildcard).
- The first matching arm wins.
- Arm bodies can be a single expression or a `{ }` block.
- **Payload bindings (2.6.0):** when the matched enum is a tagged union
  (§3.5), a pattern may bind the variant's payloads to names —
  `Some(x) => ...` or `Rect(w, h) => ...`. The binding list must match
  the variant's payload count exactly; binding names are scoped to the
  arm body and may shadow outer variables. A payload variant pattern
  WITHOUT bindings is an error, as is a binding list on a unit variant
  or on the `_` wildcard. On the generated-code side the scrutinee is
  evaluated exactly once.
- **Nested payload patterns (2.7.0):** a payload slot may itself hold a
  constructor pattern — `Some(Pair(a, b))` destructures a payload that
  is another tagged value, to any depth (`W(Some(Shape::Square(s))))`.
  Inside a payload list, a bare identifier is a BINDING; a unit-variant
  payload must be matched with the qualified form (`Some(Option::None)`)
  or with `_`, because a bare `None` in a nested position would be a
  binding named `None`, not the variant. Nested constructor arities are
  validated like top-level ones, and a failed nested tag check falls
  through to the NEXT arm (nested patterns cannot be expressed with an
  else-if chain; the generated code uses a per-match done-flag).
- **`when` guards (2.7.0):** an arm may be gated with an expression —
  `Some(x) when x > 0 => ...`. `when` is contextual (a variant may be
  named `when`; only `when <expr> =>` starts a guard). Guards run after
  the pattern matches, can read the arm's bindings, and must be
  truthy-compatible (§7.5 — a `void` guard is a compile error). An arm
  whose guard fails falls through to the next arm.
- **Exhaustiveness (2.7.0):** with no `_` arm, every variant must be
  covered by at least one UNGUARDED arm — a guarded arm can fail its
  condition, so it does not count as covering its variant (the
  non-exhaustive diagnostic keeps its historical `warning:` wording but
  remains fatal, as in 2.6.0).
- **Literal patterns (2.8.0):** an arm pattern may also be a literal —
  `1 => ...`, `-2 => ...`, `1.5 => ...`, `"a" => ...`, `true`/`false` —
  at any pattern depth, including inside a payload list
  (`Some(Some(3)) => ...`, `Rect(10, _) => ...`). A literal pattern
  compares the scrutinee (or the pulled payload) with the same
  structural equality as `==` (§7.5): numeric pairs coerce in both
  directions (`match 5 { 5.0 => ... }` matches), strings and bools
  compare exactly, enum values compare tag-then-payload-wise. Literal
  arms bind nothing, never count toward variant exhaustiveness (like
  guarded arms), and are statically type-checked against the scrutinee
  when its type is a known builtin (`"a" =>` against an `int`
  scrutinee is a compile error). A literal followed by `(` is a
  syntax error — literals cannot bind payloads.
- **Match as an expression (2.8.0):** `match` may also appear in any
  expression position — `let x = match c { 1 => 10, _ => 20 };`,
  `return match ...`, `print(match ...)`, `1 + (match ...)`. The
  expression's value is the matched arm's body value, and the
  expression's type is the least upper bound of the arm body types
  (numeric arms widen to `float`; heterogeneous arms defer to
  `unknown`). When every typed arm is a constructor of the same enum,
  the match carries that enum as its type head for annotation checks
  (§3.5). Expression-form arms take an EXPRESSION, not a block — Lamo
  has no block expressions (use the statement form for `{ }` bodies).
  A non-exhaustive match that fails at runtime (only possible with
  literal arms or guards, since exhaustiveness is checked statically
  otherwise) yields the default value `0` in both backends — there is
  no runtime "unmatched" error.
- Untagged `match` supports variant equality (collision-safe: arms
  compare the variant's INDEX, not the shadowable bare-variant
  global) and literal patterns on the same else-if desugar.

### 4.7 Assignment

```
lvalue = expr
lvalue += expr
lvalue -= expr
lvalue *= expr
lvalue /= expr
lvalue %= expr
lvalue ++
lvalue --
```

An `lvalue` is one of: a bare identifier, `arr[i]`, or `obj.field`. Compound
assignment desugars to read-then-write. `++` / `--` are sugar for `+= 1` /
`-= 1` (postfix semantics only; the value of the expression is the **new**
value, not the old).

Assignment is a **statement**, not an expression. `let x = (y = 5)` is a
syntax error.

---

## 5. Expressions

### 5.1 Literals

See §1.4.

### 5.2 Identifiers

A bare identifier resolves through the scope chain (§7.6). If the identifier
is a builtin (`print`, `input`, `isnumber`, …), it resolves to the builtin
function. User-defined functions shadow builtins.

### 5.3 Function calls

```
foo(arg1, arg2, ...)
```

The callee must be a function name (no first-class functions today). Argument
count is validated at compile time. Argument types are checked when the
function has annotations.

### 5.4 Method calls

```
obj.method(args)
```

`obj` must have a struct type. The method must be declared in some `impl Type`
block. Arity is validated.

### 5.5 Module member calls

```
alias.member(args)
```

`alias` must be a module alias introduced by `import "..." as alias;` or
`import std.X as alias`. `member` must be a top-level function in that module.
Arity is validated.

**Return-type flow (2.6.0):** when the member's declared return type names a
struct, the call result is struct-typed in the importing file — field access
(`p.x`), method calls (`p.norm()`), field assignment (`p.x = v`), and method
chaining on the result all work exactly as they do for locally produced
structs. Before 2.6.0 such results erased to an opaque value and field access
silently produced `0`. The struct itself must be visible to the importer; a
module that wants its struct-typed results to be usable should either declare
the struct before returning it (the declaration merges/imports with the file,
so this is automatic) — see §10.2.

**Explicit type arguments (2.6.0):** generic module members accept explicit
type arguments at the call site, in every position — `col.pick<int>(3, 4)`
parses and validates exactly like the plain-call form `pick<int>(3, 4)`
(RFC §4.5). Explicit arguments override call-site inference; the count must
match the member's type-parameter count. Type arguments are erased at
runtime.

### 5.6 Indexing

```
arr[i]
```

`arr` must be an array. `i` may be negative (counts from the end: `arr[-1]`
is the last element). Out-of-bounds access is a runtime error.

### 5.7 Field access

```
obj.field
```

`obj` must be a struct. `field` must be a declared field. The result type is
the field's declared type.

For arrays, `arr.len` is the only supported property today (it is a method
call, not a field — `arr.len()`).

---

## 6. Operators

### 6.1 Operator semantics

| Operator | Int       | Float     | String             | Bool     |
|----------|-----------|-----------|--------------------|----------|
| `+`      | add       | add       | concat             | error    |
| `-`      | sub       | sub       | error              | error    |
| `*`      | mul       | mul       | error              | error    |
| `/`      | int div   | float div | error              | error    |
| `%`      | int mod   | float mod | error              | error    |
| `==`     | equality  | equality  | value equality     | equality |
| `!=`     | negation  | negation  | negation           | negation |
| `< <= > >=` | compare  | compare   | compare (lex)      | error    |
| `&&`     | error     | error     | error              | logical and (truthiness) |
| `\|\|`   | error     | error     | error              | logical or (truthiness)  |
| `!`      | error     | error     | error              | logical not            |
| unary `-`| negate    | negate    | error              | error    |

**Mixed numeric operands**: `int + float` is allowed; the result is `float`.
This is the only implicit conversion in the language. String concatenation
`"count: " + 42` is a **type error** — use string interpolation or explicit
conversion (today: `"count: " + to_string(42)` via std.string).

### 6.2 Precedence (lowest to highest)

```
||                          (left-assoc)
&&                          (left-assoc)
== !=                       (left-assoc)
< <= > >=                   (left-assoc)
+ -                         (left-assoc)
* / %                       (left-assoc)
unary - !                   (prefix)
postfix . [] ()             (postfix, left-assoc)
primary                     (atoms)
```

All binary operators are left-associative. There is no exponentiation operator
today (use `std.math.pow`).

### 6.3 Truthiness

Lamo uses Python-like truthiness, NOT strict-bool. The following table defines
what is considered "true" in `if`/`while`/`for` conditions and `&&`/`||`/`!`
operands:

| Type    | Truthy when                          |
|---------|--------------------------------------|
| `int`   | non-zero                             |
| `float` | non-zero                             |
| `bool`  | the value itself                     |
| `string`| non-empty                            |
| `array` | non-empty                            |
| struct  | always (TODO: define; today always)  |
| `void`  | compile-time error in boolean context |

This is a **deliberate language decision**, not an accident. If you want
strict bool, compare explicitly: `if (n > 0)`, `if (s != "")`.

---

## 7. Type System

### 7.1 Decision: hybrid inference

**Lamo uses a hybrid type system: local inference for `let`, optional
annotations on `fn`, mandatory annotations on struct fields.**

The full rationale is in `docs/TYPE-SYSTEM.md`. The short version:

1. **`let x = expr`** — type is inferred from `expr`. The annotation
   `let x: T = expr` is allowed and adds a compile-time check, but is never
   required.
2. **`fn name(params) -> T`** — parameter and return-type annotations are
   optional. When present, the compiler validates them strictly. When absent,
   the compiler does best-effort inference from `return` statements.
3. **`struct Name { field: T }`** — field annotations are **mandatory**. A
   struct without field types is a syntax error.
4. **Public API functions** — when `pub` is added (future), public functions
   MUST have full annotations on all parameters and the return type. Private
   functions may omit them. For now (no `pub` yet), the recommendation is:
   top-level functions that are called from other files SHOULD have full
   annotations; helpers local to a file may omit them.

This model was chosen over the alternatives for these reasons:

- **Full inference everywhere** (Option A) was rejected because it makes
  cross-file calls unreadable: with no annotations on `fn foo(a, b)`, the
  caller has no way to know what types `foo` expects without reading its body.
  For a transpiled language aiming at "small but real", this is too costly.
- **Mandatory annotations everywhere** (Option B) was rejected because it
  kills the "as simple as an interpreted language" promise: `let x = 5` is
  nicer than `let x: int = 5`, and we already have the type from the literal.
- **Hybrid** (Option C, chosen) keeps the lightweight feel for local code
  while making API boundaries explicit. It mirrors what TypeScript, modern
  Python (with PEP 484), and Scala do.

### 7.2 Built-in types

| Type    | Values                                  | C representation   |
|---------|-----------------------------------------|--------------------|
| `int`   | 64-bit signed integer                   | `long long`        |
| `float` | IEEE 754 double                         | `double`           |
| `bool`  | `true` / `false`                        | `int` (0 or 1)     |
| `string`| immutable UTF-8 byte sequence           | `char*` (NUL-term) |
| `void`  | unit type (no value)                    | `void`             |
| `array` | dynamic, heterogeneous                  | `LamoArray*`       |

User-defined types: `struct Name` (composite), `enum Name` (tagged int —
future tagged unions are §13).

### 7.3 Type checking rules

- **Arithmetic**: `int + int = int`, `float + float = float`, `int + float = float`
  (int is promoted). `string + string = string` (concat). Any other operand
  combination is a compile-time error.
- **Comparison**: same-type comparisons return `bool`. Mixed `int < float` is
  allowed (returns `bool`). `string < string` is lexicographic. Comparing
  different-type values (`int == string`) is a compile-time error.
- **Logical**: `&&`, `||`, `!` operate on truthiness. The result type is the
  type of the operand(s), not `bool` — this matches Python and JavaScript
  semantics (`5 || 0` returns `5`, not `true`). If you need `bool`, compare
  explicitly.
- **Assignment**: the RHS type must be compatible with the LHS's known type
  (declared for `let`, inferred from prior assignment for plain `x = ...`).
  Incompatible assignment is a compile-time error.
- **Function return**: when a function has a declared return type, every
  `return expr` must have a type compatible with it. When the return type is
  omitted, the compiler infers it from the first `return`; subsequent `return`s
  must match.
- **Function call**: argument types are checked against parameter annotations
  when present. Arity is always checked.

### 7.4 Static vs. dynamic typing

Lamo is **statically checked at the operator level** but **dynamically typed
at the value level**. Concretely:

- The compiler rejects `"abc" * 3` at compile time (operator type check).
- The compiler does NOT reject:

  ```lamo
  let x = 5
  if (cond) { x = "abc" }   // legal at compile time, but the variable's
                            // inferred type is now "int or string"
  ```

  This is a known limitation. A future "gradual typing" pass (§13) may tighten
  this. For now, the rule is: **don't rebind a variable to a different type**.
  The compiler emits a warning (not an error) when it can detect this.

- Struct field types are checked at field access time (the compiler knows the
  field's declared type), but field writes are not strictly type-checked
  (writing `p.hp = "abc"` compiles, but reading `p.hp` later will produce
  wrong runtime behavior). Future work: tighten field-write checks.

### 7.5 Truthiness (formal)

A value `v` is truthy iff:

- `v` is `int` and `v != 0`
- `v` is `float` and `v != 0.0`
- `v` is `bool` and `v == true`
- `v` is `string` and `strlen(v) > 0`
- `v` is `array` and `v.len() > 0`
- `v` is a struct (always truthy today; future: define per-type)

Using `void` in a boolean context is a compile-time error.

### 7.6 Scopes

Lamo has four scope kinds:

1. **Global** — top-level `let`, `fn`, `struct`, `enum`, `impl` declarations.
2. **Function** — parameters and the function body.
3. **Block** — `{ ... }` inside a function or method body.
4. **Module** — names introduced by `import "..." as alias;` (§10).

Variable shadowing is allowed at any scope boundary. Re-declaration in the
**same** scope is a compile-time error (with a hint pointing at the previous
declaration).

Closures are not supported: a function inside another function would capture
the enclosing scope, but Lamo functions are top-level only. This is a
deliberate simplification.

---

### 7.7 Generic functions and type parameters (Generics PR 2)

Functions may declare type parameters between name and parameter list:

```lamo
fn id<T>(x: T) -> T { return x; }
fn map2<A, B>(xs: array<A>, b: B) -> int { return xs.len(); }
```

- **Full annotations are mandatory on generic functions** (RFC §4.3):
  every parameter and the return type must be annotated. A generic function
  with missing annotations is a compile error.
- Type parameters are visible in parameter types, return types, struct
  literal arguments (`Option<T> { ... }`) and anywhere a `type_ann` is read.
- Generic functions must be annotated, but non-generic functions keep the
  legacy rules (annotations optional-but-checked).
- Methods inside `impl<T> Name<T> { ... }` use the impl's parameters like
  their own (§4.4). The echo `<T>` on the struct name must match the
  declared parameter list exactly.
- Runtime representation is UNCHANGED for any instantiation — generics are
  erased; `array<int>` and `array<string>` lower to the same code.

### 7.8 Constraint catalogue (Generics PR 6) + user traits (2.9.0)

A constraint restricts which concrete types may stand in for a type
parameter (`fn sum<T: Num>(...)`). The built-in catalogue is fixed:

| Constraint | Satisfied by |
|------------|--------------|
| `Any`      | everything (default) |
| `Eq`       | int, float, bool, string |
| `Ord`      | int, float, bool, string |
| `Hash`     | int, float, bool, string |
| `Num`      | int, float |
| `Show`     | builtins + user structs |

**2.9.0: any declared trait (§3.7) is also a valid constraint name** —
`fn draw<T: Shape>(s: T)`. A concrete type argument satisfies a trait
constraint when it is a declared struct with an `impl Trait for Type`
block (generic impls satisfy every instantiation). Built-in types,
arrays and enums never satisfy user traits — they satisfy only the
catalogue above.

Unknown constraint names (neither catalogue nor a declared trait) and
violated constraints at call sites are compile errors. Constraints apply
identically to struct and enum type parameter lists
(`struct SortedMap<K: Ord, V>`).

### 7.9 Typed arrays and deprecation of bare `array` (PR 3)

`let xs: array<int> = [1,2]` fixes the element type compile-time:

- element-type inference follows RFC §5.1 least-upper-bound:
  `[1, 2]` → array<int>, `[1, 2.5]` → array<float>;
- heterogeneous literals still RUN (erasure) but typed boundaries reject them;
- the spelling `Array<int>` is accepted and normalized to `array<int>`;
- BARE `array` remains valid as an alias of `array<any>` and emits a
  deprecation warning encouraging migration (warning never fails builds).

### 7.10 Logical operators & void (validation completion)

Per §6.3 truthiness, every value type participates in boolean contexts.
EXCEPTION: values produced by `-> void` functions are rejected by
if/while/for conditions and by `&&`/`||`/`!` operands with a dedicated
compile error ("void value used in boolean context...").

---

## 8. Built-in Functions

These are treated as ordinary identifiers and may be shadowed by user
functions. The semantic pass resolves them via a builtin table.

| Name            | Arity | Signature                                  | Returns  | Notes |
|-----------------|-------|--------------------------------------------|----------|-------|
| `print`         | 1     | `print(x: any) -> void`                    | `void`   | prints `x` followed by newline; booleans render `true`/`false` (§8.1) |
| `input`         | 0     | `input() -> int`                           | `int`    | reads a line, parses as int |
| `input_int`     | 0     | `input_int() -> int`                       | `int`    | alias for `input` |
| `input_str`     | 0     | `input_str() -> string`                    | `string` | reads a line as string |
| `isnumber`      | 1     | `isnumber(x: any) -> bool`                 | `bool`   | true if `x` is int or float |
| `isstring`      | 1     | `isstring(x: any) -> bool`                 | `bool`   | true if `x` is string |
| `isarray`       | 1     | `isarray(x: any) -> bool`                  | `bool`   | true if `x` is array |
| `len`           | 1     | `len(arr: array) -> int`                   | `int`    | same as `arr.len()` |
| `push`          | 2     | `push(arr: array, x: any) -> void`         | `void`   | same as `arr.push(x)` |
| `pop`           | 1     | `pop(arr: array) -> any`                   | any      | same as `arr.pop()` |
| `abs`           | 1     | `abs(x: int\|float) -> int\|float`         | mirror   | absolute value, mirrors arg type |
| `exit`          | 1     | `exit(code: int) -> void`                  | `void`   | terminates with exit code |

Platform-specific builtins (GUI, HTTP) are documented in §11.

### 8.1 Boolean print form (decision, 2.6.0)

`print(b)` where `b` is a boolean renders **`true`** or **`false`**, not
`1`/`0`. This is now specified and locked so downstream tooling can depend
on it. Rationale and scope:

- The language has first-class `true`/`false` literals; echoing a bool back
  as `1`/`0` translated the value into a different type's spelling.
- Before 2.6.0 the renderer was internally inconsistent: the same boolean
  printed `1` at the top level but `true` inside an array (`[true, false]`)
  or a struct field. The decision removes the special case rather than
  spreading it.
- The tree-walking interpreter (`eval`/`repl`) always rendered `true`/`false`;
  the C backend now matches, so the two execution paths agree (§10.7).
- Converting a boolean to its numeric form stays explicit: `int(b)` yields
  `1`/`0`. Truthiness rules (§6.3) are unchanged — `if (b)` behaves the same
  as before, and `if (0)`/`if (1)` remain valid for ints.

---

## 9. Arrays

### 9.1 Literals and indexing

```lamo
let xs = [1, 2, 3]
let mixed = [1, "two", true]   // heterogeneous — legal but discouraged
print(xs[0])                   // 1
print(xs[-1])                  // 3 (negative index from end)
xs[0] = 99                     // index assignment
xs[0] += 1                     // read-modify-write on index
```

Arrays are dynamic and, without an annotation, heterogeneous. Since 2.5.0,
the element type can be pinned with a typed annotation (`let xs: array<int> =
[1, 2, 3]`, §7.9): element types are then checked at compile time, and a bare
`array` annotation is a deprecated alias for `array<any>` that emits a
warning.

### 9.2 Methods

```
arr.push(x)    // append
arr.pop()      // remove and return last
arr.len()      // length
```

These are equivalent to the `push(arr, x)`, `pop(arr)`, `len(arr)` builtins.

### 9.3 Memory

Arrays are heap-allocated and managed by the runtime. Since compiler
2.3.0, Lamo ships an **opt-in mark-sweep garbage collector** (see
`docs/MEMORY-MODEL.md` for the full design). An array's memory is freed
when:

- the program exits (via `atexit` cleanup — always), OR
- a `gc_collect()` run determines the array is no longer reachable from
  any root (only when the user opts into GC via `gc_collect()` or
  `gc_set_threshold(N > 0)`).

Programs that never call `gc_*` see the same behavior as 2.2.0:
allocations accumulate until exit. Programs that opt in get periodic
reclamation during the run, which is what makes long-running processes
(HTTP servers, GUI event loops) viable.

The HTTP server loop and the GUI event loop both call `gc_collect()`
automatically on a fixed schedule (every 100 HTTP requests, every 1000
GUI frames). Programs can also call `gc_collect()` manually at natural
boundaries, or set an auto-trigger threshold with
`gc_set_threshold(N_bytes)`.

---

## 10. Modules and Imports

Lamo has three import forms. All three are language-level constructs (parsed
by the parser, not preprocessed by the CLI).

### 10.1 Legacy global merge

```lamo
import "math.lamo";
```

Loads `math.lamo` (resolved relative to the importing file's directory) and
merges every top-level declaration into the global namespace. This is the
original behavior and is kept for backwards compatibility. Use it for quick
scripts; for anything structured, prefer the namespaced form.

### 10.2 Namespaced string-path import

```lamo
import "math.lamo" as math;
math.sqrt(25)        // 5 (well, in this example, 625 — see README)
```

Loads `math.lamo` and exposes its top-level functions and globals under the
`math` alias. Calls go through `alias.member(args)` syntax. The loader renames
the imported declarations to `lamo_mod_<alias>__<name>` internally to avoid
collisions with the importing file's own declarations.

Struct and enum declarations inside the module are NOT renamed — they keep
their bare names, which is what makes §5.5's return-type flow possible: a
module function can return a struct it declares, and the importing file sees
the same struct definition (first import wins on duplicate struct names,
per §10.5's duplicate rules).

### 10.3 Bare-identifier import

```lamo
import math          // sugar for: import "math.lamo" as math
import math as m     // sugar for: import "math.lamo" as m
```

Resolves `math` to `math.lamo` in the importing file's directory. Otherwise
identical to §10.2.

**Folder-based modules (2.6.0):** when the direct file does not exist, the
loader falls back to a folder with a `mod.lamo` entry point:

```
myproject/
    main.lamo          // import utils
    utils/
        mod.lamo       // <- resolved when utils.lamo does not exist
        helpers.lamo   // imported by mod.lamo, merged into the global scope
```

`import "utils"` (string form) and `import utils` (bare form) both try
`utils.lamo` first, then `utils/mod.lamo`. The fallback never changes
resolution when the primary path exists, so existing projects are
unaffected. Declaration of the module alias is unchanged (`import utils`
exposes the folder's members under the `utils` alias).

### 10.4 Standard library imports

```lamo
import std.io
import std.math as math
```

Resolves `std.io` to `std/io.lamo`, searched in this order (first match wins):

1. `$LAMO_STD_DIR/io.lamo` (env override, dev/CI use)
2. `<bindir>/std/io.lamo` (shipped alongside the compiler binary)
3. `<bindir>/../std/io.lamo` (development layout)
4. `<bindir>/../share/lamo/std/io.lamo` (system install)
5. `./std/io.lamo` (current working directory)
6. `<importing_file_dir>/std/io.lamo` (local override)

This lets users override individual stdlib modules by placing files in `./std/`
next to their program.

### 10.5 Resolution and cycles

- Imports are resolved **lazily** during compilation: when the parser sees an
  `import` AST node, the loader resolves the path, reads the file, parses it,
  and recursively loads its imports.
- **Import cycles** are detected and reported as a compile-time error, with a
  stack showing the cycle: `a.lamo -> b.lamo -> a.lamo`.
- **Import-time duplicate rule (Phase 10, now enforced + tested):** the same
  file imported twice (via different paths that normalize to the same
  absolute path) is loaded ONCE. The second import is a no-op at load time
  AND emits a `warning: file "..." already imported; duplicate import
  ignored` diagnostic so accidental double imports are visible. Re-importing
  with a DIFFERENT alias warns and does not re-register the alias: first
  alias wins (documented limitation). Both behaviors are regression-tested
  (`tests/smoke/import_same_file_twice.*`).
- Duplicate top-level symbol names across files produce a compile-time error
  with the file path of the previous declaration.

**Decision table (2.6.0, formalized):**

| Situation | Behavior |
|-----------|----------|
| same file, same alias (or no alias) re-imported | no-op + `duplicate import ignored` warning |
| same file, different alias re-imported | no-op + warning; first alias wins |
| different files, same alias | **compile error** (`failed to register module alias`) — two namespaces under one name are ambiguous |
| same file imported through different relative paths | deduped by normalized absolute path — one load, one warning |
| import cycle (a → b → a) | compile error with the cycle stack |
| duplicate top-level symbol across merged files | compile error naming both declaration sites |

### 10.6 Visibility

**Decision (Phase 10):** every top-level declaration of a module is public
THROUGH ITS NAMESPACE; unaliased (legacy) merges place everything into the
global namespace where normal duplicate-declaration errors apply. Rationale
recorded here so the upgrade path stays mechanical:

1. aliased imports are already the de-facto privacy boundary (nothing outside
   the module can reach it without the alias),
2. adding `pub` later only narrows what a namespace exposes — no existing
   program changes meaning,
3. two-step plan when introduced: warn on non-`pub` declarations reachable
   via aliases for one release, then enforce.

**Step 1 LANDED (2.6.0):** the `pub` export marker exists. It is a CONTEXTUAL
keyword (never reserved — `let pub = 5` still parses) accepted at top level
in front of `fn`, `let`, `struct`, `impl`, and `enum`:

```lamo
pub fn visible(x: int) -> int { return x }
fn helper(x: int) -> int { return x }   // implicitly private in a future release
```

Semantics in 2.6.0:

- `pub` declarations and non-`pub` declarations BOTH export through their
  module's namespace. No program changes meaning (step 1 of the plan).
- Reaching a NON-`pub` member through an alias (`lib.helper()`, or reading
  `lib.CONST`) compiles and runs, but emits a one-time-per-member warning:
  `member 'helper' of module 'lib' is not marked 'pub'; it will become
  private in a future release — add 'pub' to export it explicitly`.
- Members marked `pub` never warn. The warning is recorded per member in
  the module registry, so it fires at the FIRST use, not on every line.
- Step 2 (future release): non-`pub` members become unresolvable through
  aliases; the warning becomes an error. The migration is then strictly
  mechanical: add `pub` where the warning points.

**Project/package layout conventions (Phase 10 decision):** a Lamo project
is any directory containing `lamo.pkg`; `lamo new` scaffolds
`main.lamo` + `lamo.pkg` + `.gitignore` (which includes `lamo_exec.c`).
Local packages live under `src/<name>.lamo` or nested directories resolved
relative to the importing file (§10.3's folder-module form applies);
published dependency layout and lockfile semantics live with lampm
(`src/lampm/`). Generator artifacts (`lamo_exec.c`, binaries) are never
committed.

### 10.7 Execution modes: `run` vs `eval`/`repl`

Lamo has two distinct execution paths. Since 2.6.0 they share the same
module-loading semantics (see the table below); what differs is the
backend and the performance/feedback tradeoff.

| Mode        | Backend              | Module imports (`import "..." as alias;`) | Speed     | Use case |
|-------------|----------------------|-------------------------------------------|-----------|----------|
| `lamo run`  | Transpiles to C, GCC | Full `LamoModuleRegistry` support         | Native    | Programs that use namespaced imports (`math.sqrt(x)`, `fs.readText(path)`, …) and want maximum performance. |
| `lamo build`| Same as `run`, but stops after producing the binary | Full support | Native | Producing a distributable binary. |
| `lamo check`| Frontend only (lexer + parser + semantic) | Resolves imports for type/arity checking | Fast | CI / pre-commit validation. |
| `lamo eval` | Built-in tree-walking interpreter (`src/eval/eval.c`) | **Loaded through the same loader as `run`** (2.6.0) — member calls and qualified globals resolve | Slower (no GCC, no optimization) | Quick evaluation, debugging small snippets without the C-compile step. |
| `lamo repl` | Same interpreter as `eval`, interactive | **Import statements load modules** (2.6.0) — type `import std.io` at the prompt | Slower | Interactive development. |

**Decision (revised 2.6.0):** `eval`/`repl` and `run` remain distinct
paths with distinct purposes (the interpreter trades peak performance for
fast feedback — no GCC invocation), but they no longer diverge on module
loading, which had become the single biggest workflow gap between them:

- `lamo eval file.lamo` already ran the full recursive loader (path
  resolution, cycle detection, duplicate-import rules) before executing;
  the interpreter now RESOLVES `alias.member(args)` calls and
  `alias.CONST` reads by routing them to the loader-renamed declarations
  (`lamo_mod_<alias>__<name>`) that the loaded program already defines.
- `lamo repl` accepts `import` statements at the prompt. Each import runs
  the same loader pass, registers the module under its alias, and makes
  its members callable for the rest of the session.
- Generic type arguments on module calls are erased in eval, exactly as
  in the C backend.
- **2.8.0:** eval/REPL evaluate enum declarations, constructor calls,
  and `match` (tagged and untagged), with value parity verified by the
  eval suite — `EVAL_VAL_ENUM` mirrors the C runtime's tagged-union
  representation, and the interpreter keeps an enum registry so the
  REPL (which runs no semantic pass) resolves variants exactly like
  the compiler.
- What eval/repl still do NOT support: the struct/array value model
  (the last interpreter limitation). Programs using those still need
  `lamo run`.

The old `"module member 'math.sqrt' is not available in eval/repl mode"
error no longer exists. If a snippet uses constructs the interpreter
does not model (structs, arrays), eval says so at the runtime-error site
as before.

**Decision (revised 2.6.0):** `eval`/`repl` and `run` are distinct paths
with distinct purposes — fast feedback vs native performance — and now
share module-loading semantics (see the table above). The pre-2.6 limitation
(`"module member 'math.sqrt' is not available in eval/repl mode"`) was
removed by routing interpreter member calls through the same renamed
declarations the loader produces; no registry was duplicated and no GCC
step was added.

**Migrating from `eval` to `run`:** if a snippet uses constructs the
interpreter does not model (structs, arrays), save it to a `.lamo`
file and run it with `lamo run file.lamo`. The language semantics are
otherwise identical between the two paths — the interpreter implements
the same value model (including enums and match since 2.8.0), truthiness
rules, and runtime errors as the transpiler.

---

## 11. Platform-Specific Builtins

### 11.1 GUI (Windows-native, X11 on Linux/macOS)

| Builtin            | Signature                                              |
|--------------------|--------------------------------------------------------|
| `gui_open`         | `gui_open(w: int, h: int, title: string) -> void`      |
| `gui_should_close` | `gui_should_close() -> int`                            |
| `gui_begin_frame`  | `gui_begin_frame(r, g, b: int) -> void`                |
| `gui_draw_rect`    | `gui_draw_rect(x, y, w, h, r, g, b: int) -> void`      |
| `gui_draw_text`    | `gui_draw_text(text: string, x, y, r, g, b: int) -> void` |
| `gui_end_frame`    | `gui_end_frame() -> void`                              |
| `gui_close`        | `gui_close() -> void`                                  |

On platforms without a GUI backend, these compile to no-op stubs that emit a
runtime warning the first time they are called.

### 11.2 HTTP server

| Builtin          | Signature                                              |
|------------------|--------------------------------------------------------|
| `http_route`     | `http_route(path: string, response: string) -> void`   |
| `http_serve`     | `http_serve(port: int) -> void`                        |
| `http_serve_once`| `http_serve_once(port: int) -> int`                    |

The HTTP server loop calls `gc_collect()` every 100 requests so the
arena doesn't grow without bound during long-running `http_serve`. The
route table itself (registered via `http_route`) is allocated outside
the GC heap (via plain `malloc`), so routes stay alive across
collections — only per-request garbage (string buffers built while
parsing/handling) is swept.

Programs that want finer control can call `gc_collect()` manually at
natural boundaries (end of request, after a batch of work) or set an
auto-trigger threshold via `gc_set_threshold(N_bytes)` — when the
runtime has allocated `N` bytes since the last collection, the next
allocation triggers a `gc_collect()`. See `docs/MEMORY-MODEL.md` for
the full GC design.

**Concurrency:** the server is single-threaded, one request at a time.
Multi-threaded HTTP is future work (depends on the broader threading
story for Lamo).

---

## 12. Runtime Behavior

### 12.1 Entry point

The entry point is the implicit `main()` synthesized by the codegen. It:

1. Initializes the string arena, the GC heap list, and the GC root stack.
2. Registers every global `LamoValue` (top-level `let`s) as a GC root.
3. Runs all top-level `let` initializers in declaration order.
4. Calls the ENTRY file's `fn main()` if defined; otherwise (and then)
   runs top-level statements in order. Precisely: top-level statements
   run first, and the entry file's zero-argument `fn main()` is invoked
   after them — unless the program already contains an explicit
   top-level `main()` call statement, in which case the implicit call is
   suppressed (pre-2.6 programs relied on explicit calls; they keep
   their exact behavior).
5. Pops all GC roots and cleans up the string arena via `atexit`.

There is no required `fn main()`. Top-level statements run directly.
Defining `fn main()` is optional — when the entry file defines a
zero-argument `fn main()`, it is called as described above.

**Entry-file vs library-file expectation (2.6.0):** only the ENTRY file's
`fn main()` is invoked. A `fn main` in an IMPORTED file never runs:
aliased imports rename it (`lamo_mod_<alias>__main`), and unaliased
merges trigger a loader warning — `imported file "..." defines 'fn
main'; it will not run — only the entry file's main() is called`. A
`fn main` with parameters defined in the entry file is ignored by the
entry-point machinery (and flagged by normal type checks if it is
never called manually).

### 12.2 Integer overflow

`int` arithmetic wraps around on overflow (C `long long` semantics). There is
no overflow check today. Future: an `--overflow-check` flag may add trapping
semantics.

### 12.3 Division by zero

Integer division by zero terminates the program with a runtime error message
to stderr and exit code 134 (SIGABRT-style). Float division by zero produces
`inf` or `nan` (IEEE 754).

### 12.4 Array out-of-bounds

Accessing `arr[i]` with `i` outside `[-len, len-1]` terminates the program
with a runtime error and exit code 1.

### 12.5 Null pointers

Lamo does not have `null`. Uninitialized struct fields default to `0` (for
int/float/bool) or `""` (for string) or `[]` (for array). A field that should
be "absent" should be modeled explicitly — today with an `int` flag, or with
the `Option<T>` API shipped in `std.collections` (2.5.0; see
`docs/RFC-generics.md` §7.3 for the design and its current scope).

### 12.6 Error reporting

Runtime errors print `lamo: <kind> error: <message>` to stderr and exit 1.
`<kind>` is one of: `arithmetic`, `index`, `cast`, `internal`. There is no
exception system; runtime errors are fatal.

### 12.7 Exit codes

| Code | Meaning                |
|------|------------------------|
| 0    | success                |
| 1    | compile error OR runtime error |
| 2    | backend (C compile) failure |
| 134  | abort (e.g. assertion) |

---

## 13. Future Work (out of spec scope)

These items are tracked in `todo.md` and `roadmap.md` but are **NOT** part of
this spec. When they ship, this spec will be updated.

- **First-class functions / closures** — `let f = fn(x) { ... }`.
- **Iterator protocol** — `for x in arr { ... }`.
- **Pattern matching beyond variants** — OR-patterns, range patterns,
  binding patterns at the top level. Literal patterns, nested payload
  destructuring, `when` guards, `Enum::Variant` qualification, and
  match as an expression **shipped in 2.7.0/2.8.0** (§3.5, §4.6).
- **Exception / error-handling mechanism** — likely `Result<T, E>` based, not
  throw/catch.
- **Forward declarations** for mutual recursion.
- **String interpolation** — `"hello \(name)"` or `` `hello ${name}` ``.
- **`for-in` loops** — `for x in arr { ... }`.
- **Gradual typing pass** — strict mode where rebinding to a different type
  is an error.
- **Future GC work** — generational collection, incremental marking,
  concurrent collection (all depend on the threading story). The opt-in
  mark-sweep GC itself **shipped in 2.3.0**; see `docs/MEMORY-MODEL.md`.

---

## 14. Changelog

- **v1.0** — first authoritative spec. Captured the behavior of compiler
  `2.2.0`. Future changes go through a formal revision process (PR + spec
  bump).
- **v1.1** (compiler 2.3.0) — opt-in mark-sweep GC shipped
  (`docs/MEMORY-MODEL.md` Steps 2–6). New builtins: `gc_collect()`,
  `gc_set_threshold(N)`, `gc_heap_size()`, `gc_heap_count()`. HTTP server
  re-promoted from "preview" to "official". `eval`/`repl` vs `run`
  divergence formalized in §10.7.
- **v1.2** (compiler 2.5.0) — generics shipped (PRs 1–6 of
  `docs/RFC-generics.md`): generic structs (§3.4), generic functions and
  call-site inference (§7.7), the constraint catalogue (§7.8), typed arrays
  with deprecation of bare `array` (§7.9), and void-in-boolean-context
  validation (§7.10). Function hoisting within a file formalized in §3.3.
  `Option<T>`/`Result<T,E>` APIs shipped in `std.collections`.
- **v1.3** (compiler 2.6.0) — tagged-union enums (§3.5): payload-carrying
  variants, generic enum parameters, constructor-call validation, match
  payload bindings (§4.6), tagged equality and rendering. Boolean print
  form decided and specced (§8.1). Module-boundary struct type flow
  (§5.5/§10.2). Explicit type arguments on module member calls (§5.5).
  Module rules: entry-file `main()` semantics (§12.1), contextual `pub`
  export markers with the §10.6 two-step rollout, folder-based modules
  (§10.3), duplicate-import decision table (§10.5), and module loading in
  `eval`/`repl` (§10.7).
- **v1.4** (compiler 2.7.0) — the 2.6.0 Open Follow-Ups ledger closed:
  enum type annotations (§3.5 — `let`, params, returns, for-lets, fields,
  payloads; arg-count + leaf validation; ctor full-type inference feeding
  §7.7 binding); nested payload patterns and `when` guards in `match`
  (§4.6, with Rust-style exhaustiveness treatment of guarded arms);
  `Enum::Variant` qualification (§3.5) with cross-enum "later wins"
  collisions defined as legal plus a shadowing warning; `pub` step 2
  enforced (§10.6 — non-`pub` via alias is a compile error, REPL
  included); `run_tests.ps1` gained the eval-cases section (§10.7 parity
  with `tests/run_tests.sh`). Statement-position constructor calls fixed
  to emit real tagged values.
- **v1.5** (compiler 2.8.0) — the 2.7.0 Open Follow-Ups ledger closed:
  eval/REPL enum support (§10.7 — `EVAL_VAL_ENUM`, interpreter enum
  registry, `match` evaluation with run parity); literal patterns in
  `match` (§4.6 — int/float/string/bool at any depth, bidirectional
  numeric coercion, static scrutinee type checks); match as an
  expression (§4.6 — value threading with arm-body LUB typing);
  enum annotation type-arg invariance at call sites (§3.5/§7.7 —
  partially-inferable enums complete against annotations, payload-bound
  params enforce invariance, degraded bare-enum types defer); full
  `run_tests.ps1` parity (smoke/golden/std sections + runtime `.stdin`).
  Also fixed en route: a latent 2.7.0 bug generating invalid C for
  guarded untagged `match` arms, feature-detection blindness for
  expressions inside `match`/struct literals, and the spurious
  "expected 'Option<int>', got 'enum'" rejection of bare unit variants.
- **v1.6** (compiler 2.9.0) — TRAITS (§3.7): `trait Name { fn sig; ... }`
  declarations, `impl Trait for Type { ... }` blocks validated for
  coherence (one impl per trait/struct pair), completeness (every required
  method present) and signature compatibility (strict arity; annotations
  compared wherever both sides annotate); generic trait impls via the
  RFC §4.4 echo form (`impl<T> Printable for Stack<T>`) satisfying every
  instantiation; trait names accepted as first-class generic constraints
  on function/struct/enum type parameters (§7.8 — "constraints beyond the
  catalogue"), enforced at call sites against the impl registry;
  `pub trait` module marker accepted; honest compile-time diagnostics for
  method calls on bare type-parameter receivers (previously a silent
  `lamo_make_int(0)` fallback in the C backend); struct-literal arguments
  now carry their concrete full type into §7.7 binding and constraint
  enforcement (also closes the same gap for the built-in catalogue);
  §3.7/§3.8 renumbering (import moved to §3.8).
