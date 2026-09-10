# Lamo Standard Library (std/)

This is the official standard library for the LamoLanguage. It is shipped
alongside the `lamo` compiler and is automatically available to every
Lamo program via the `import std.<module>` syntax — no installation
required.

## Philosophy

The standard library follows the same philosophy as the language:

> *As simple as an interpreted language, as fast as a compiled one.*

Concretely, this means:

- **Small surface area.** Each module has a tight, predictable API. We
  prefer a few well-named functions over dozens of slightly-different
  variants.
- **Consistency.** Equivalent operations use the same name everywhere
  (`readText` / `writeText`, `len`, `get`, `has`, `clear`).
- **Performance.** All hot paths are backed by C runtime functions;
  the Lamo wrappers are thin and add no measurable overhead.
- **Cross-platform.** Every module works identically on Linux, macOS,
  and Windows. Platform differences are hidden inside the runtime.
- **Excellent documentation.** Each module ships its own `.md` file
  with examples and a function reference.

## Modules

| Module         | Description                                              |
|----------------|----------------------------------------------------------|
| `std.io`       | Console input/output (`print`, `println`, `readLine`)    |
| `std.fs`       | File system operations (`readText`, `writeText`, `exists`) |
| `std.path`     | Path manipulation (`join`, `parent`, `filename`, `normalize`) |
| `std.string`   | UTF-8 string utilities (`length`, `split`, `trim`, `replace`) |
| `std.math`     | Math functions and constants (`sqrt`, `sin`, `PI`)       |
| `std.random`   | PRNG (`int`, `float`, `bool`, `choice`, `shuffle`)       |
| `std.time`     | Time functions (`now`, `sleep`, `monotonic`)             |
| `std.collections` | List, Stack, Queue, HashMap, HashSet                  |
| `std.process`  | Process operations (`run`, `exec`, `currentPid`)         |
| `std.env`      | Environment variables (`get`, `set`, `remove`)           |
| `std.os`       | OS info (`name`, `arch`, `cpuCount`, `home`)             |
| `std.net`      | HTTP client (`get`, `post`)                              |
| `std.json`     | JSON parser and serializer (`parse`, `stringify`)        |
| `std.testing`  | Testing framework (`begin`, `end`, `assertEqual`, `summary`) |
| `std.debug`    | Debugging utilities (`log`, `dump`, `time`, `timeEnd`)   |

## Usage

Import any module with the dotted syntax:

```lamo
import std.io            // alias `io` (default: last segment)
import std.math as math  // explicit alias
import std.fs as fs

io.println("Hello, world!")
io.println("sqrt(16) = " + math.sqrt(16))

if (fs.exists("config.txt")) {
    io.println(fs.readText("config.txt"))
}
```

The import path `std.<module>` is resolved by the loader in this order:

1. `$LAMO_STD_DIR/<module>.lamo` (environment override)
2. `<bindir>/std/<module>.lamo` (shipped alongside the compiler binary)
3. `<bindir>/../std/<module>.lamo` (development layout)
4. `<bindir>/../share/lamo/std/<module>.lamo` (system install)
5. `./std/<module>.lamo` (current working directory)
6. `<importing_file_dir>/std/<module>.lamo` (local override)

The first match wins. This lets users override individual stdlib modules
by placing files in `./std/` next to their program.

## Layout

```
std/
├── io.lamo
├── fs.lamo
├── path.lamo
├── string.lamo
├── math.lamo
├── random.lamo
├── time.lamo
├── collections.lamo
├── process.lamo
├── env.lamo
├── os.lamo
├── net.lamo
├── json.lamo
├── testing.lamo
├── debug.lamo
├── examples/         # runnable example programs for each module
└── tests/            # test programs (used by `lamo test`)
```

## Implementation Notes

The standard library is a mix of:

1. **Pure-Lamo modules** — implemented entirely in `.lamo` source,
   using only existing language primitives. These are easy to read,
   modify, and learn from. Examples: `std.math`, `std.collections`,
   `std.testing`, `std.debug`, `std.json`, `std.path`.

2. **C-backed modules** — wrap a small set of C runtime functions
   (`fs_*`, `env_*`, `os_*`, `time_*`, `process_*`, `random_*`,
   `net_http_*`, `math_*`, `str_*`) exposed as builtins. The C
   implementations live in `src/codegen/lamo_runtime.h` under
   `#ifdef LAMO_NEEDS_STD_RUNTIME`. The `.lamo` wrappers add the
   consistent naming and documentation layer.

Each C-backed builtin uses a `__lamo_std_<module>_<fn>` prefix that
cannot collide with user-defined functions. The `.lamo` wrappers expose
them through the namespaced import API (e.g. `math.sqrt`, `fs.readText`).

## Adding a New Module

1. Create `std/<module>.lamo` with the public API (functions and any
   module-level constants).
2. If you need OS access, add the C implementation to the STD section
   of `src/codegen/lamo_runtime.h`, register the builtin in
   `src/builtins.h` (category `BUILTIN_STD`), add a codegen case in
   `src/codegen/codegen.c::generate_std_builtin_call_expr`, and
   re-run `python3 scripts/embed_runtime.py` to regenerate the
   embedded runtime data.
3. Write `std/examples/<module>_demo.lamo` and run it to verify.
4. Add the module to the table above.

## Quality Bar

Before adding any function, answer internally:

- Does it really belong in the standard library?
- Does it solve a common problem?
- Is there a simpler way?
- Does it follow the style of the other APIs?
- Would a beginner understand it from the name alone?

The standard library should stay small, elegant, consistent, and
pleasant to use.
