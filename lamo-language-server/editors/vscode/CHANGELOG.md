# Change Log

All notable changes to the Lamo Language extension are documented here.

## [0.1.0] - 2026-09-10

### Added

- TextMate grammar for the Lamo language (`source.lamo`).
- Language configuration: comments, brackets, auto-closing pairs, folding.
- Language-server client with incremental document sync.
- Diagnostics: parse errors, scope errors, type mismatches, arity errors.
- Context-sensitive completions (keywords, snippets, builtins, locals,
  workspace symbols, std module members).
- Hover signatures and documentation.
- Go to definition, including the embedded standard library (`lamo-std://`).
- Document symbols, signature help, document highlights.
- Custom request `lamo/stdContent` serving embedded std sources.
