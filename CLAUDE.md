# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Authoritative Design Documents

Before changing language behavior, read the relevant doc under `docs/`:

- `docs/SPEC.md` — the language specification. Authoritative for "what the
  language means". If the compiler disagrees with the spec on spec'd
  behavior, the compiler is the bug.
- `docs/TYPE-SYSTEM.md` — the type-system decision (hybrid inference).
  Read before touching anything in `src/semantic/` or adding type annotations.
- `docs/MEMORY-MODEL.md` — memory model + GC design + rollout plan. Read
  before touching `lamo_runtime.h` or any allocation path.
- `docs/RFC-generics.md` — the parametric generics design (implemented as of
  2.5.0). Read before any work on typed collections, `Option`/`Result`, or
  generic functions.

The roadmap (`roadmap.md`) tracks *what's done* and *what's next*. The spec
tracks *what it means*. The TODO (`todo.md`) tracks implementation items per
phase.

## Build Commands

```bash
make          # Build the compiler (produces the 'lamo' executable)
make clean    # Remove build artifacts
make test     # Run the regression suite (valid / invalid / runtime tests)
```

The build automatically re-runs `scripts/embed_runtime.py` whenever
`src/codegen/lamo_runtime.h` changes, regenerating
`src/codegen/lamo_runtime_data.c` (the runtime source as an embedded C
string). Python is required for this; if absent, the build proceeds with the
stale generated file (which is committed to the repo).

## Running

```bash
./lamo run   <file.lamo>            # Compile and run a Lamo source file
./lamo build <file.lamo> -o demo    # Compile to a binary, do not run
./lamo check <file.lamo>            # Parse + semantic-check only
./lamo test                         # Run the test suite (tests/run_tests.sh)
./lamo fmt   <file.lamo>            # Normalize source formatting in place
./lamo help                         # Show usage
./lamo version                      # Print version
```

The compiler transpiles Lamo to C (`lamo_exec.c`), compiles it with GCC, and
executes the result.

### Namespaced Imports

`import "math.lamo" as math;` exposes the imported file's top-level
declarations under the `math` alias. Call them with `math.sqrt(25)`.
The loader renames declarations to `lamo_mod_<alias>__<name>` and registers
them in `src/modules.c`'s `LamoModuleRegistry`. The semantic pass and codegen
consult this registry to resolve `AST_MEMBER_CALL` nodes. Legacy
`import "math.lamo";` (without `as`) still merges into the global namespace
as before. `import std.<module>` resolves against the `std/` standard library
using the search order documented in `docs/SPEC.md` §10.4.

## Architecture

Lamo is a transpiled language that compiles to C. The pipeline is:

**Source (.lamo) → Lexer → Parser → AST → Semantic → Codegen → C Code → GCC → Execution**

The frontend/backend boundary and the annotation contract between them are
formalized in `docs/ARCHITECTURE.md`.

### Core Components

- **src/lexer/lexer.c** — Tokenizes source into tokens (keywords, identifiers,
  literals, operators).
- **src/parser/parser.c** — Recursive descent parser producing an AST. Handles
  expressions with operator precedence. Records syntax errors in
  `file:line:col` format (matching semantic.c) and recovers via
  synchronize-and-continue so multiple syntax errors are reported in one pass.
- **src/ast/ast.c** — AST node definitions and constructors for all language
  constructs. Each node carries a `file_path` pointer so multi-file builds can
  report errors against the right source file.
- **src/semantic/semantic.c** — Scope-resolved type inference + operator
  checks. Walks the AST, defines variables/functions per scope, flags type
  errors at compile time (e.g. `"abc" * 3` is rejected), binds generic type
  parameters, and uses `node->file_path` for multi-file diagnostics.
- **src/codegen/codegen.c** — Transpiles AST to C code. Manages indentation
  and forward declarations. Decisions that differ by type are driven by
  semantic annotations on the AST (`sema_struct_name`, `sema_full_type`),
  never re-derived inside codegen. The runtime library is **not** inlined as
  fprintf calls — it lives in `src/codegen/lamo_runtime.h` and is emitted as
  a pre-built string constant (`lamo_runtime_source`) at the top of every
  generated .c file.
- **src/codegen/lamo_runtime.h** — The actual Lamo runtime (value type,
  arithmetic ops, string arena, mark-sweep GC, GUI runtime, HTTP runtime,
  std builtins). All functions are marked `LAMO_UNUSED` so programs that
  don't use every op don't trigger `-Wunused-function`.
- **src/codegen/lamo_runtime_data.{c,h}** — Auto-generated embedded-string
  version of `lamo_runtime.h`. Regenerate with `python3 scripts/embed_runtime.py`.
- **src/eval/eval.c** — Tree-walking AST interpreter. Powers `lamo eval` and
  `lamo repl`. Independent of the C codegen backend and intentionally
  module-less (see SPEC §10.7).
- **src/lampm/** — Integrated package manager. Compiled into the main `lamo`
  executable; reachable through `lamo install`, `lamo update`, `lamo list`,
  etc. The single public entry point is `lampm_main()` (see
  `src/lampm/lampm.h`), which `lamo_v2.c::main()` dispatches to when the
  subcommand is one of the package-manager ones (init, install, update,
  remove, list, info, lock, cache, doctor). The implementation is split
  across:
  - `lampm.c` — entry point + all `command_*` handlers + `install_dependency`.
  - `lampm_internal.h` — shared types (`Manifest`, `Lockfile`, `Dependency`,
    `LockEntry`) + helper function decls used across the split files.
  - `lampm_util.c` — string/fs helpers + output helpers (`info_msg`,
    `success_msg`, `error_msg`, etc.) + the global option vars.
  - `lampm_manifest.c` — `lamo.pkg` parsing/writing.
  - `lampm_lockfile.c` — `lamo.lock` parsing/writing.
  - `lampm_git.c` — git operations + repo-spec parsing.
- **src/modules.c** + **src/modules.h** — Module registry backing the
  namespaced-import feature. The loader (in `src/cli/import_resolver.c`)
  renames top-level declarations of an aliased import to
  `lamo_mod_<alias>__<name>` and registers them here; the semantic pass and
  codegen look them up via `lamo_modules_resolve_member()` to resolve
  `alias.fn(args)` calls.
- **src/cli/** — CLI layer split out of `lamo_v2.c` for maintainability:
  - `cli_options.{h,c}` — global flags (`g_verbose`, `g_quiet`,
    `g_no_color`), `LamoCommand` enum, `VERSION`.
  - `paths.{h,c}` — path utilities, `resolve_import_path` (with the
    `std/` resolution logic), `read_file`, `lamo_cc`, `executable_suffix`.
  - `import_resolver.{h,c}` — `CompilationState`, the recursive loader
    (`load_program_recursive_from`), import cycle detection, and module
    declaration renaming (`rename_module_declarations`).
  - `commands.{h,c}` — `command_new`, `command_clean`, `command_repl`,
    `command_test`, `command_fmt`.
  - `compile.{h,c}` — `compile_sources` + helpers (GUI detection,
    source-lookup and module-resolution callbacks).
  - `help.{h,c}` — `print_usage`, `print_command_help`.
- **src/lamo_v2.c** — Thin CLI entry point (~380 lines): env-var parsing,
  global-flag parsing, subcommand dispatch. Delegates to `src/cli/*` for
  everything else, and to `src/lampm/lampm.h::lampm_main()` for
  package-manager subcommands.
- **src/builtins.h** — Single source of truth for every builtin (name, arity,
  category, return policy). Adding a builtin starts here.
- **src/error_util.h** — Shared error formatting: `file:line:col`, caret
  snippets, hints, color. Both parser and semantic diagnostics render
  through it.

### Language Features

- Variables: `let x = 10;`
- Functions: `fn name(params) { ... return expr; }` — declarations are
  hoisted within a file (the global pre-pass registers all top-level
  functions before statement visits).
- Control flow: `if/else`, `while`, `for` with `break` and `continue`
- Truthiness: **Python-like**, not strict-bool. `if (5)` and `if ("abc")` are
  valid. `if (0)`, `if (0.0)`, `if ("")` are false. Formally defined in
  SPEC §6.3 / §7.5; `void` in a boolean context is a compile error.
- Built-ins: `print`, `input`, `input_int`, `input_str`, `isnumber`,
  `isstring`, `exit`, `abs` — treated as ordinary identifiers by the lexer
  and resolved as builtins in the semantic pass and codegen (they are
  shadowable by user functions).
- Operators: arithmetic, comparison, logical, assignment (including `+=`, `-=`,
  `++`, `--`)
- Structs & methods: `struct Name { field: type, ... }` +
  `Name { field: value, ... }` literals; `impl Type { fn method(args) { self.field ... } }`
  with implicit `self`.
- Arrays: `[1, 2, 3]`, `arr[i]` (negative indices allowed), `arr[i] = value`,
  `arr.push(x)`, `arr.pop()`, `arr.len()` — plus the `len`/`push`/`pop`
  builtin forms.
- Enums & match: `enum Name { Variant, ... }` (variants are global int
  constants); `match scrutinee { Pattern => body, _ => default }` with
  exhaustiveness checks. 2.6.0: TAGGED-UNION enums — variants may carry
  payloads (`enum Option<T> { Some(T), None }`), constructed by call syntax
  and destructured in match arms (`Some(x) => ...`); tagged values are a
  distinct runtime kind (`LAMO_VALUE_ENUM`), always truthy, compared
  tag-then-payload-wise, printed as `Some(42)`. Untagged enums keep the
  legacy int representation. 2.7.0: NESTED payload patterns
  (`Some(Pair(a, b))`), `when` guards (`Some(x) when x > 0 => ...`,
  guarded arms don't count toward exhaustiveness), ENUM TYPE ANNOTATIONS
  (`let o: Option<int> = ...`, params/returns/for-lets/fields — arg-count
  and leaf validated; ctor calls infer their concrete full type feeding
  call-site binding), and `Enum::Variant` QUALIFICATION with legal
  cross-enum "later wins" collisions + shadow warning (variant globals
  are deduped in generated C; untagged match arms compare the variant
  index).
- Export markers (2.6.0): contextual `pub` on top-level declarations
  (`pub fn ...`). 2.7.0 (§10.6 step 2): non-`pub` members accessed through
  a module alias are a compile ERROR — all shipped `std/` modules mark
  their members `pub`; the REPL enforces the same boundary at lookup
  time.
- Generics (2.5.0): generic structs `struct Pair<A, B>`, generic functions
  `fn id<T>(x: T) -> T` with call-site inference, `impl<T> Stack<T>`, typed
  arrays `array<T>` (bare `array` is a deprecated alias for `array<any>`),
  and the constraint catalogue `Any | Eq | Ord | Num | Hash | Show`.
- GUI builtins (Windows-native, X11 on Linux/Mac, no-op elsewhere):
  `gui_open`, `gui_should_close`, `gui_begin_frame`, `gui_draw_rect`,
  `gui_draw_text`, `gui_end_frame`, `gui_close`
- HTTP server builtins: `http_route`, `http_serve`, `http_serve_once`
- Namespaced imports: `import "..." as alias;` exposes the imported file's
  top-level declarations under `alias`. Member access uses `alias.fn(args)`
  syntax. `import std.<module>` is the standard-library form.
- Optional semicolons: both `let x = 5;` and `let x = 5` are accepted.

### Diagnostics

Errors print `file:line:col: <kind> error: <message>`, followed by the
source line with a caret pointing at the column, followed by an optional
`hint: <advice>` line. The `<kind>` label is colored red+bold when stderr
is a TTY; disable with `--no-color` or `LAMO_NO_COLOR=1`. Use
`parser_error_with_hint(p, msg, hint)` (parser) and
`semantic_error_at_hint(ctx, line, col, msg, hint)` (semantic) to attach
a hint to a new error site.

### Code Style

- C99 standard with POSIX extensions
- Error handling via `fprintf(stderr)` and `exit(1)` for fatal errors
- Memory management: manual `malloc`/`free`; AST nodes freed after code generation
- All runtime functions marked `LAMO_UNUSED` to avoid -Wunused-function on
  programs that don't use every builtin
- `user_name1()` returns a pointer into a 4-entry ring buffer; safe for up to
  4 simultaneous uses in the same C expression. Use `user_name()` with an
  explicit buffer for cases needing more.

### Standard Library

The `std/` directory ships 15 Lamo modules (io, fs, path, string, math,
random, time, collections, process, env, os, net, json, testing, debug)
plus `examples/` and `tests/` subdirectories. Modules are imported via
`import std.<module>` (dotted syntax), which the parser synthesizes into
the path `std/<module>.lamo` with `<module>` as the default alias.

The loader (`resolve_import_path`) resolves `std/...` paths against a list
of candidate directories (env override, bindir, system install, cwd, local
file dir). The first match wins.

C-backed builtins power the OS-touching modules (fs, env, os, time,
process, net, random, plus math/string for performance). They are
registered in `src/builtins.h` under the `BUILTIN_STD` category with
the `__lamo_std_<module>_<fn>` naming convention. The codegen dispatch
lives in `generate_std_builtin_call_expr` in `src/codegen/codegen.c`.
The C implementations live in `src/codegen/lamo_runtime.h` under
`#ifdef LAMO_NEEDS_STD_RUNTIME` (set by `emit_runtime()` when the
program uses any STD builtin).

To add a new C-backed builtin:
1. Add the implementation to the STD section of `lamo_runtime.h`.
2. Run `python3 scripts/embed_runtime.py` to regenerate
   `lamo_runtime_data.c`.
3. Register the builtin in `src/builtins.h` (category `BUILTIN_STD`).
4. Add the codegen case in `generate_std_builtin_call_expr` in
   `src/codegen/codegen.c`.
5. Add a thin Lamo wrapper in `std/<module>.lamo`.
6. Write a test in `std/tests/test_<module>.lamo`.

Pure-Lamo modules (math constants, collections, testing, debug, json,
path helpers) don't need any compiler changes — just create the `.lamo`
file.
