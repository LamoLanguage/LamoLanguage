#ifndef LAMO_FMT_FORMATTER_H
#define LAMO_FMT_FORMATTER_H

/*
 * formatter.h — AST-based pretty-printer for `lamo fmt` (2.11.0).
 *
 * The formatter parses the source into the SAME AST the compiler uses,
 * then re-emits normalized source text from that AST:
 *
 *   - 4-space indentation, opening braces on the same line, one
 *     statement per line, explicit semicolons on simple statements.
 *   - Space normalization around operators; minimal parenthesization
 *     re-derived from the parser's precedence table (redundant parens
 *     that the parser recorded as AST_GROUPING_EXPR are dropped when
 *     precedence alone would rebuild them).
 *   - Comments are NOT part of the AST; they are captured from the raw
 *     source by a string-aware pre-scan and re-attached by line
 *     position (own-line comments above the following node, same-line
 *     comments kept trailing). Comment TEXT is always preserved — only
 *     position can shift when a comment sat inside a multi-line
 *     expression.
 *   - Semantics are preserved: numbers round-trip (shortest float
 *     representation that strtod-matches the original bits), string
 *     values are re-escaped exactly per the lexer's escape set, and
 *     `x++`/`x--` (which the parser desugars to `x = x + 1`) are
 *     re-detected and printed compactly.
 *
 * Safety contract: if the file does not parse cleanly, the formatter
 * refuses to touch it (*ok = 0) and the caller falls back to the legacy
 * whitespace-only normalization — `fmt` never breaks a file it cannot
 * fully understand.
 */

#include "../ast/ast.h"

/* Pretty-print `source` (a full Lamo file). Returns a freshly malloc'd
 * NUL-terminated string and sets *ok = 1 on success. On parse failure
 * returns NULL with *ok = 0 (caller should fall back). `file_path` is
 * only used for parser diagnostics. */
char* fmt_format_source(const char* source, const char* file_path, int* ok);

#endif /* LAMO_FMT_FORMATTER_H */
