# Lamo Architecture: Frontend, Backend, and the Information Contract

**Status:** Official. This document records the architectural decisions
behind the current compiler shape — it is the "why" companion to
[DIRECTORY-LAYOUT.md](DIRECTORY-LAYOUT.md). It formalizes three decisions:

1. codegen must be driven by semantic information,
2. frontend and backend are separated along a precise boundary,
3. `eval`/`repl` is intentionally NOT a third full pipeline.

## 1. Pipeline

```
        ┌──────────────────── FRONTEND ────────────────────┐   ┌── BACKEND ──┐
src ─▶ lexer ─▶ parser ─▶ AST ─▶ import resolver ─▶ semantic │   │  codegen    │
        (tokens)      (AST nodes) (modules.c)            │   │             │
                                          analysis pass ──┼─▶ │ lamo_exec.c │─▶ cc
                                       (semantic.c)       │   │ + runtime   │
```

The single artifact crossing the boundary is the **annotated AST**:

| Annotation (on every `ASTNode`) | Producer | Consumer | Meaning |
|---------------------------------|----------|----------|---------|
| `file_path`, `line`, `column` | parser | semantic (errors), diagnostics | origin of each node |
| `sema_struct_name` | semantic | codegen | this expression is a value of that user struct |
| `sema_full_type` | semantic | codegen / future passes | normalized full type, e.g. `array<int>`, `Option<int>` after generic substitution |
| resolution markers such as `sema_full_type == "array"` on member calls | semantic | codegen | which dispatch route was chosen (`lamo_array_push` vs method call) |

### The rule: backend behavior depends on semantic information

Codegen must not re-implement typing or scoping. Where output shape differs by
type, codegen reads what semantic computed:

- `print(x)` renders known structs through `lamo_print_struct_named`, using
  the name only the compiler has (the runtime cannot know it).
- Member calls route to module call / struct method / array builtin strictly
  following the markers above.
- Generic functions compile ONCE under type erasure: substitutions are a
  compile-time checking concept with zero runtime representation.

When a new backend behavior needs new information, extend the annotation set
here first — never re-walk scopes inside codegen.

## 2. Frontend/Backend Separation

The split above is formal: `src/{lexer,parser,ast,semantic}` vs
`src/codegen`. Practical consequences already shipped:

- All error rendering goes through `error_util.h` — identical style from both
  sides.
- The runtime is a plain header consumed verbatim by generated C, so the
  backend's "ABI" to user programs is reviewable as ordinary C
  (`src/codegen/lamo_runtime.h`).
- Golden snapshot tests (`tests/golden/`) pin the emitted shape of the user
  code section, so refactors of either side surface their blast radius in
  review instead of at users' machines.

## 3. eval/repl Decision (SPEC §10.7)

`lamo eval` and `lamo repl` run the tree-walking interpreter in
`src/eval/`. They do NOT load modules and are not a full pipeline
replacement. Rationale: sub-100 ms feedback loops need to skip C compilation
entirely; bringing interpreter semantics to parity would freeze the language
into the intersection of two implementations. `run`/`build`/`check` are
authoritative for meaning; anything they accept that eval rejects must
produce an error naming `lamo run`.

## 4. VM / LLVM (Deferred With Conditions)

Revisit a lower-level backend only when ALL of the following hold:

1. The generics rollout + stdlib typed collections have stabilized for one
   minor release.
2. Real programs show the C-transpile loop (compile-to-C + cc invocation)
   dominating iteration time.
3. A volunteer exists to own an IR long-term.

Until then the C backend remains the sole target by design — "as fast as C
because it IS C".
