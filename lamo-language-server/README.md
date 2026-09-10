# lamo-lsp — Language Server for the Lamo Programming Language

A language server for [Lamo](https://github.com/LamoLanguage/LamoLanguage)
written in Rust with [tower-lsp](https://crates.io/crates/tower-lsp), built on
a fully self-contained front end: its own lexer, parser and semantic analyzer.
No compiler binary is required — the entire standard library is embedded in
the executable, so the server works out of the box on any machine.

## Features

| Feature | Method | Notes |
| ------- | ------ | ----- |
| Diagnostics | `textDocument/publishDiagnostics` | parse errors, scope errors, type mismatches, arity errors, visibility/deprecation hints |
| Completion | `textDocument/completion` | context-sensitive: `std.` members, struct fields/methods, array methods, keywords, snippets, builtins, locals, workspace symbols, enum variants, module aliases |
| Hover | `textDocument/hover` | signatures + doc comments for locals, functions, builtins and std members |
| Go to definition | `textDocument/definition` | local/workspace symbols and embedded stdlib (synthetic `lamo-std://std/*.lamo` URIs) |
| Document symbols | `textDocument/documentSymbol` | nested outline: structs with fields, methods, enums with variants |
| Signature help | `textDocument/signatureHelp` | triggered on `(` and `,` with active-parameter tracking |
| Document highlight | `textDocument/documentHighlight` | all references of the symbol under the cursor (read/write aware) |
| Stdlib source | `lamo/stdContent` *(custom)* | serves embedded std sources so editors can open real stdlib code |

Incremental text sync (`TextDocumentSyncKind::INCREMENTAL`) keeps edits cheap.

## Build

Requires a recent stable Rust toolchain.

```sh
cargo build --release
# binary: target/release/lamo-lsp
```

## Run

The server communicates over stdio and is normally started by an editor
client — you rarely run it by hand:

```sh
lamo-lsp
```

### VS Code / VSCodium

Install the packaged extension:

```sh
code --install-extension editors/vscode/lamo-language-0.1.0.vsix
```

The extension bundles the server binary, registers the `lamo` language,
installs the TextMate grammar and wires a virtual-document provider for
`lamo-std://` so that go-to-definition lands in real standard-library source.
Set `lamo.server.path` to point at a custom server build if desired.

### Neovim (nvim-lspconfig style)

```lua
vim.lsp.start({
  name = 'lamo-lsp',
  cmd = { '/path/to/lamo-lsp' },
  filetypes = { 'lamo' },
})
```

### Helix

```toml
# languages.toml
[[language]]
name = "lamo"
scope = "source.lamo"
file-types = ["lamo"]
language-servers = ["lamo-lsp"]

[language-server.lamo-lsp]
command = "/path/to/lamo-lsp"
```

## The embedded standard library

The official repository's `std/*.lamo` sources and their `std/*.md` API
references are compiled into the binary (`src/stdlib.rs`, `include_str!`).
Signatures shown by completion/hover come from parsing the real stdlib with
the same parser used for user code — the documentation can never drift from
the code. The custom `lamo/stdContent` request returns the source text for a
module URI such as `lamo-std://std/math.lamo`.

User projects may still override std modules by shipping a local `std/`
directory next to the importing file (SPEC §10.4.6); the workspace resolver
checks local overrides and open documents before falling back to the
embedded modules.

## Architecture

```
src/
├── lexer.rs        UTF-8 lexer: spans, comments, escapes, hex/binary/floats
├── parser.rs       Recursive-descent parser with error recovery and
│                   speculative turbofish parsing (`pick<int>(...)`)
├── ast.rs          Span-carrying AST (top-level statements included)
├── types.rs        Lightweight type model (LType) with numeric widening,
│                   string-coercion and type-parameter handling
├── builtins.rs     Core builtin table (Lang/Gui/Http/std-runtime)
├── stdlib.rs       Embedded standard library + signature extraction
├── analyzer.rs     Scopes, symbol index, use-record, diagnostics,
│                   import resolution across workspace + std
├── workspace.rs    Open-document store, disk cache, cycle detection
├── line_index.rs   UTF-8 <-> UTF-16 position mapping (LSP positions)
├── server.rs       tower-lsp Backend (LSP surface + custom request)
└── features/       completion, goto, highlight, hover, resolve,
                    signature, symbols
```

### Design principles

- **Never report a diagnostic without confidence.** When a type or symbol
  cannot be inferred (e.g. unannotated functions), the corresponding checks
  are skipped rather than guessed.
- **Implementation beats documentation.** Grammar and semantics follow the
  official C implementation (verified against its test suite): top-level
  statements are valid, type names are not keywords, `string + int` coerces
  at runtime, `bool == int` is legal, and turbofish calls must be probed
  before parsing them as generics.

## Testing

```sh
cargo test                      # unit + LSP protocol integration tests
python3 scripts/lsp_smoke_test.py   # end-to-end stdio framing smoke test
cargo run --example validate_repo   # analyzer vs. all real .lamo files
```

- **62 unit tests** cover the lexer, parser, type model, builtin table,
  stdlib signatures and the analyzer (scope errors, arity, redeclaration,
  visibility, deprecation).
- **12 integration tests** (`tests/lsp_protocol.rs`) drive the real
  `LspService` through the JSON-RPC surface: initialize handshake, diagnostics
  notifications, and every feature method, including the custom
  `lamo/stdContent`.
- **The smoke test** spawns the release binary and speaks raw `Content-Length`
  framed stdio, exactly like an editor client.
- **The repository validator** runs the analyzer over every `.lamo` file in
  the official repository (235 files) and asserts zero false positives on
  valid code — intentional-error fixtures are excluded and do produce
  diagnostics.

## Packaging the extension

```sh
cd editors/vscode
npm install
npx @vscode/vsce package
```

See `editors/vscode/README.md` for the extension's user-facing documentation.
