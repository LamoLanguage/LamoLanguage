# Lamo Language for Visual Studio Code

Syntax highlighting and rich language-server support for the
[Lamo programming language](https://github.com/LamoLanguage/LamoLanguage).

## Features

- **Syntax highlighting** — TextMate grammar covering keywords, types,
  numbers (decimal / hex / binary), strings with escapes, comments and calls.
- **Diagnostics** — parse, scope, type and arity errors as you type, powered
  by the `lamo-lsp` language server.
- **Completions** — context-sensitive suggestions: keywords, types, snippets,
  builtins, local variables, workspace symbols and `std.` module members.
- **Hover information** — signatures and doc comments for local symbols,
  builtins and standard-library functions.
- **Go to definition** — jumps into your own code, or into the embedded
  standard library source (opened as a read-only `lamo-std://` document).
- **Document symbols** — outline view for functions, structs, enums, traits
  and top-level variables.
- **Signature help** — parameter hints triggered on `(` and `,`.
- **Document highlights** — all occurrences of the symbol under the cursor.

## Requirements

Everything works out of the box: the extension bundles a prebuilt
`lamo-lsp` server binary. To use a custom server build instead, set:

```json
{ "lamo.server.path": "/path/to/lamo-lsp" }
```

## Extension Settings

| Setting | Description |
| ------- | ----------- |
| `lamo.server.path` | Path to the `lamo-lsp` executable (empty = bundled server). |
| `lamo.trace.server` | Trace communication between VS Code and the server. |

## Known Issues

- The bundled server binary targets Linux x64; on other platforms build
  `lamo-lsp` from source and point `lamo.server.path` at it.

## Release Notes

### 0.1.0

Initial release: TextMate grammar, language configuration, and the full
language-server feature set (diagnostics, completion, hover, definition,
symbols, signature help, highlights).
