<div align="center">

# ⚡ Lamo Language

**A modern programming language that transpiles to C.**

*As simple as an interpreted language. As fast as a compiled one.*

[![License](https://img.shields.io/github/license/arthurlamonattopro/LamoLanguage?style=for-the-badge)](LICENSE)
[![Stars](https://img.shields.io/github/stars/arthurlamonattopro/LamoLanguage?style=for-the-badge)]()
[![Issues](https://img.shields.io/github/issues/arthurlamonattopro/LamoLanguage?style=for-the-badge)]()

</div>

---

## About

Lamo is an experimental programming language focused on being easy to learn while producing native executables.

Instead of executing code through a virtual machine, **Lamo transpiles your program to C** and then uses **GCC** (or any `CC` you configure) to build a highly optimized native binary. There is no runtime VM, no bytecode, no interpreter in the hot path — just readable source in, fast machine code out.

> **Write simple code. Get native performance.**

---

## Features

- ⚡ **Transpiles to C** — native executables, no VM
- 🧠 **Clean, readable syntax** — optional semicolons, Python-like truthiness
- 🧬 **Generics** — generic functions, structs, `impl<T>`, and constraints (`T: Ord`)
- 🏷 **Tagged-union enums** — `Option<T> { Some(T), None }` with `Some(x) =>` match bindings
- 🏗 **Structs & methods** — `struct` / `impl` blocks with implicit `self`
- 📚 **Enums & `match`** — variant patterns with exhaustiveness warnings
- 📋 **Arrays** — dynamic arrays with negative indexing and a typed `array<T>` mode
- 🔍 **Semantic analysis** — scope tracking, type checking, compile-time errors with hints
- 🎨 **Colorized diagnostics** — `file:line:col`, source snippets, carets, and hints
- 📁 **Module system** — namespaced imports (`import std.io`), `pub` export markers, folder modules
- 📦 **Built-in package manager** — `lamo install`, lockfile, version pinning
- 🌐 **Native HTTP server** — server builtins in the runtime
- 🖥 **Native GUI support** — Win32 and X11 backends
- 🗑 **Opt-in garbage collector** — mark-sweep GC for long-running programs
- 🧪 **Built-in testing** — `lamo test` plus the `std.testing` module
- 🛠 **Code formatter** — `lamo fmt` normalizes whitespace-level style
- 🔁 **REPL & eval** — fast feedback, now with module loading

---

## Hello World

```lamo
fn main() {
    print("Hello, World!")
}
```

Run it:

```bash
lamo run hello.lamo
```

## A Taste of the Language

```lamo
struct Player {
    name: string,
    hp: int
}

impl Player {
    fn damage(amount: int) {
        self.hp -= amount
    }
}

let hero = Player { name: "Arthur", hp: 100 }
hero.damage(25)
print(hero.hp)   // 75
```

### Generics (new in 2.5)

```lamo
fn pick<T>(a: T, b: T) -> T {
    if (1 < 2) { return a }
    return b
}

struct Stack<T> {
    items: array<T>,
    top: int
}

impl<T> Stack<T> {
    fn push(x: T) {
        self.items.push(x)
        self.top += 1
    }
}

let s: Stack<int> = Stack<int> { items: [], top: 0 }
s.push(10)
print(pick("left", "right"))   // "left"
```

---

## Getting Started

### 1. Build from source

Requirements: a C99 compiler (GCC/Clang), `make`, and optionally Python 3 (used to embed the runtime into the binary; a pre-generated copy is committed so builds work without it).

```bash
git clone https://github.com/arthurlamonattopro/LamoLanguage
cd LamoLanguage
make          # build the compiler (produces the `lamo` binary)
make test     # run the full regression suite
```

### 2. Run a program

```bash
./lamo run examples/main.lamo
```

### CLI overview

| Command | Description |
|---------|-------------|
| `lamo run <file.lamo>` | Compile and run a source file |
| `lamo build <file.lamo> -o <name>` | Compile to a binary without running it |
| `lamo check <file.lamo>` | Parse and semantic-check only (great for CI) |
| `lamo eval` / `lamo repl` | Fast-feedback interpreter (no C compile step) |
| `lamo fmt <file.lamo>` | Normalize source formatting in place |
| `lamo test` | Run the test suite |
| `lamo new <project>` | Scaffold a new project (`main.lamo`, `lamo.pkg`, `.gitignore`) |
| `lamo clean` | Remove generated artifacts (`lamo_exec*`) |
| `lamo init` / `install` / `update` / `list` / `info` / ... | Package manager subcommands |

---

## Documentation

Full documentation lives in [`docs/`](docs/) and [`std/`](std/):

| Document | Contents |
|----------|----------|
| [`docs/SPEC.md`](docs/SPEC.md) | The language specification — syntax, types, semantics, runtime, modules |
| [`docs/TYPE-SYSTEM.md`](docs/TYPE-SYSTEM.md) | Type-system decision record (hybrid inference) |
| [`docs/MEMORY-MODEL.md`](docs/MEMORY-MODEL.md) | Memory model and the opt-in mark-sweep GC |
| [`docs/RFC-generics.md`](docs/RFC-generics.md) | Generics design (implemented in 2.5.0) |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Compiler architecture: frontend, backend, information contract |
| [`docs/DIRECTORY-LAYOUT.md`](docs/DIRECTORY-LAYOUT.md) | Where everything lives, and why |
| [`docs/STDLIB.md`](docs/STDLIB.md) | Standard library scope and organization rules |
| [`docs/STYLE.md`](docs/STYLE.md) | Canonical `.lamo` code style |
| [`std/README.md`](std/README.md) | Standard library overview |
| [`std/<module>.md`](std/) | Per-module API reference (io, fs, net, json, collections, …) |

Runnable programs live in [`examples/`](examples/) and [`std/examples/`](std/examples/).

---

## Project Status

The current implementation (v2.7.0) includes:

- ✅ Lexer, parser, AST
- ✅ Semantic analyzer (scopes, types, generics binding, constraints)
- ✅ C backend with embedded runtime
- ✅ Structs, methods, arrays, enums, match
- ✅ Generics (PRs 1–6): generic functions/structs/impls, `array<T>`, constraints
- ✅ Tagged-union enums — payload variants, `Some(x) =>` match bindings,
  nested destructuring (`Some(Pair(a, b))`), `when` guards,
  `Enum::Variant` qualification, and enum type annotations
  (`let o: Option<int>`)
- ✅ Modules & namespaced imports — struct-typed returns across boundaries,
  enforced `pub` visibility (non-`pub` via alias is an error), folder-based
  modules, module loading in eval/REPL
- ✅ Package manager (`lampm` integrated into the `lamo` binary)
- ✅ REPL and eval
- ✅ Formatter
- ✅ Test harness: smoke, golden, runtime, eval, stdlib suites
- ✅ Opt-in mark-sweep GC
- ✅ HTTP server & GUI builtins

## Roadmap

Next up (see [`roadmap.md`](roadmap.md) and [`todo.md`](todo.md) for the full picture):

- [x] Tagged-union enums (payload-carrying variants, `Some(x) =>` binding in `match`)
- [x] Module-boundary type flow (struct-typed return values across imports)
- [x] Explicit type arguments on module member calls
- [x] Boolean print form (`print(true)` renders `true` — decided + specced in SPEC §8.1)
- [x] Module rules: `pub` export markers, folder modules, duplicate-import
      decisions, entry-file `main()` semantics, eval/REPL module loading
- [x] 2.6.0 follow-ups: enum type annotations, nested match patterns +
      `when` guards, `Enum::Variant` qualification, `pub` step 2 enforcement,
      Windows eval-suite parity in `run_tests.ps1`
- [x] 2.7.0 follow-ups: eval/REPL enum support, match literal patterns,
      match as an expression, enum annotation type-arg invariance,
      full `run_tests.ps1` parity (smoke/golden/std + `.stdin`)
- [ ] Traits
- [ ] Better formatter (AST-based pretty-printer)
- [ ] Language Server (LSP)
- [ ] VSCode extension
- [ ] Official documentation website

---

## Philosophy

Lamo follows one simple idea:

> **As simple as an interpreted language, as fast as a compiled one.**

Instead of growing into a complex language with hundreds of features, Lamo aims to stay small, readable, and productive. Every new construct must ship with defined semantics, compiler validation, and tests — syntax never outruns meaning.

## Contributing

Contributions are welcome! If you'd like to improve the compiler, the documentation, or the standard library, feel free to open a Pull Request. Before changing language behavior, please read the relevant document under [`docs/`](docs/) — [`docs/SPEC.md`](docs/SPEC.md) is the source of truth for what the language means, and [`CLAUDE.md`](CLAUDE.md) explains how the codebase is organized.

## License

This project is licensed under the [MIT License](LICENSE).
