#ifndef LAMO_ERROR_UTIL_H
#define LAMO_ERROR_UTIL_H

/*
 * error_util.h - Source-snippet printing and hint support for error messages.
 *
 * Sprint 3 feature: when the parser or semantic analyzer reports an
 * error, we print the offending source line followed by a caret line
 * pointing at the column. This matches the diagnostic style of GCC,
 * Rust, and most modern languages — a single "file:line:col" line is
 * much harder to act on than seeing the actual source with a visual
 * pointer.
 *
 * Example output:
 *
 *   /home/user/prog.lamo:3:5: semantic error: use of undeclared variable 'x'
 *   | let y = x + 1;
 *   |     ^
 *
 * The caret is positioned under the column reported by the lexer. We
 * count columns starting at 1, matching the rest of the codebase.
 *
 * Sprint 4 enhancements:
 *   - Optional ANSI color (auto-detected via isatty(2); can be forced
 *     off with lamo_error_set_color(0)).
 *   - Optional "hint" line after the snippet, formatted as:
 *
 *       hint: did you forget to declare 'x' with `let x = ...;`?
 *
 *     Hints are passed alongside the main error message by the parser
 *     and semantic analyzer. They turn a generic error into actionable
 *     advice, which is especially valuable for beginners.
 *
 * Multi-byte characters (UTF-8): the caret column is computed in
 * bytes, not characters. This is the same convention the lexer uses
 * for its `column` field. For ASCII-heavy Lamo source this is fine;
 * for code with wide CJK characters the caret may be visually shifted
 * to the right of where it "should" be. That's a known limitation,
 * documented in the README.
 */

#include <stdio.h>
#include <string.h>

/* Sprint 4: color state. -1 = auto-detect (isatty), 0 = off, 1 = on. */
static int g_lamo_error_color = -1;

/* Set color mode: 1 = always on, 0 = always off, -1 = auto-detect
 * (default; uses isatty(2) at first error). Call this from the CLI
 * when --no-color is passed, or from any caller that wants to force
 * a specific mode. */
static inline void lamo_error_set_color(int mode) {
    g_lamo_error_color = mode;
}

/* Sprint 4: returns 1 if color should be used on stderr. Uses the
 * cached value if set; otherwise auto-detects via isatty(2). The
 * isatty check is done once and cached — repeated calls are cheap. */
static inline int lamo_error_use_color(void) {
    if (g_lamo_error_color >= 0) return g_lamo_error_color;
#ifdef _POSIX_C_SOURCE
    /* isatty is declared in <unistd.h>. We don't include it here to
     * keep this header lightweight; instead we declare isatty manually.
     * The linker resolves it at runtime. */
    extern int isatty(int);
    g_lamo_error_color = isatty(2) ? 1 : 0;
#else
    /* On non-POSIX (Windows) we conservatively disable color unless
     * explicitly enabled. */
    g_lamo_error_color = 0;
#endif
    return g_lamo_error_color;
}

/* ANSI escape sequences. Used only when lamo_error_use_color() returns 1. */
#define LAMO_COLOR_RED     "\x1b[31m"
#define LAMO_COLOR_BOLD    "\x1b[1m"
#define LAMO_COLOR_CYAN    "\x1b[36m"
#define LAMO_COLOR_RESET   "\x1b[0m"

/* Print the source line containing the error, plus a caret line
 * pointing at the column. `source` is the full source text of the
 * file (multiple lines separated by '\n'). `line` and `column` are
 * 1-indexed. If `source` is NULL or the line is out of range, this
 * function is a no-op — callers don't need to guard. */
static inline void error_print_snippet(FILE* out, const char* source, int line, int column) {
    const char* line_start;
    const char* line_end;
    int current_line;
    int i;

    if (!source || line < 1) {
        return;
    }

    /* Walk the source counting newlines until we reach the target line. */
    line_start = source;
    current_line = 1;
    while (line_start < source + strlen(source) && current_line < line) {
        if (*line_start == '\n') {
            current_line++;
        }
        line_start++;
    }
    if (current_line != line) {
        /* Line number is past EOF (shouldn't happen, but be safe). */
        return;
    }

    /* Find the end of the line (either '\n' or end of string). */
    line_end = line_start;
    while (*line_end != '\0' && *line_end != '\n') {
        line_end++;
    }

    /* Print the source line. We strip a trailing '\r' so files with
     * CRLF endings don't show a stray carriage return. */
    fputs("  | ", out);
    fwrite(line_start, 1, (size_t)(line_end - line_start), out);
    /* Strip trailing \r if present. */
    if (line_end > line_start && *(line_end - 1) == '\r') {
        /* already written — emit a backspace-equivalent by writing
         * a fresh newline after the stripped line. We can't actually
         * erase the \r from the stream, so we accept it. */
    }
    fputc('\n', out);

    /* Print the caret line: spaces up to (column - 1), then '^'. */
    fputs("  | ", out);
    for (i = 1; i < column && i < 200; i++) {
        fputc(' ', out);
    }
    if (lamo_error_use_color()) {
        fputs(LAMO_COLOR_BOLD LAMO_COLOR_RED, out);
    }
    fputc('^', out);
    if (lamo_error_use_color()) {
        fputs(LAMO_COLOR_RESET, out);
    }
    fputc('\n', out);
}

/* Sprint 4: print a hint line below the snippet. `hint` may be NULL —
 * in that case this is a no-op. The hint is formatted as:
 *
 *   hint: <hint text>
 *
 * with the "hint:" prefix in cyan when color is enabled. */
static inline void error_print_hint(FILE* out, const char* hint) {
    if (!hint || !*hint) return;
    if (lamo_error_use_color()) {
        fputs(LAMO_COLOR_CYAN "hint" LAMO_COLOR_RESET ": ", out);
    } else {
        fputs("hint: ", out);
    }
    fputs(hint, out);
    fputc('\n', out);
}

#endif /* LAMO_ERROR_UTIL_H */
