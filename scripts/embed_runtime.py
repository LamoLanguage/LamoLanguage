#!/usr/bin/env python3
"""
embed_runtime.py - Embed src/codegen/lamo_runtime.h as a C string constant.

Generates src/codegen/lamo_runtime_data.c with the runtime source as a single
escaped string literal. The lamo compiler links this in and emits it verbatim
at the top of every generated .c program. This lets us maintain the runtime as
a real .h file (with syntax highlighting, testability, normal diff history)
while still producing self-contained generated C programs.

Usage:
    python3 scripts/embed_runtime.py

Reads:
    src/codegen/lamo_runtime.h
Writes:
    src/codegen/lamo_runtime_data.c
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INPUT = os.path.join(ROOT, "src", "codegen", "lamo_runtime.h")
OUTPUT = os.path.join(ROOT, "src", "codegen", "lamo_runtime_data.c")


def c_escape(text: str) -> str:
    """Escape a string as a C string literal body (without surrounding quotes)."""
    out = []
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif 0x20 <= ord(ch) < 0x7F:
            out.append(ch)
        else:
            # Use octal escape (\NNN) instead of \xNN because \x consumes as many
            # hex digits as it can, which would swallow the opening quote of the
            # next C string if it happens to be a hex char (e.g. \x22" — the C
            # preprocessor reads \x22" as \x22" → \x22 followed by ", but if the
            # next char is a-f or 0-9 it gets eaten). Octal stops after 3 digits.
            out.append(f"\\{ord(ch):03o}")
    return "".join(out)


def main() -> int:
    if not os.path.isfile(INPUT):
        print(f"error: {INPUT} not found", file=sys.stderr)
        return 1

    with open(INPUT, "r", encoding="utf-8") as f:
        source = f.read()

    # Strip the leading /* ... */ comment block so the embedded string is smaller
    # and the runtime appears cleaner at the top of generated .c files. The
    # documentation lives in the source .h file; generated code doesn't need it.
    if source.startswith("/*"):
        end = source.find("*/")
        if end != -1:
            # Skip the closing */ and any leading blank lines after it.
            after = source[end + 2:]
            after = after.lstrip("\n")
            source = after

    escaped = c_escape(source)

    # Split the string into ~120-char chunks for readability of the generated
    # .c file (and to avoid pathological line lengths for editors/diff tools).
    # We must never split inside an escape sequence (\n, \", \\NNN, ...). Walk
    # forward and break at safe boundaries only.
    chunk_size = 120
    lines = []
    i = 0
    n = len(escaped)
    while i < n:
        end = min(i + chunk_size, n)
        # If we're stopping in the middle of an escape sequence (char at end-1
        # is a backslash, or we're inside an octal \NNN), back up to a safe
        # boundary.
        while end > i and end < n:
            # Find the last backslash at or before end-1.
            last_bs = escaped.rfind("\\", i, end)
            if last_bs == -1:
                break
            # How many chars after the backslash are part of this escape?
            # Octal: \ + up to 3 octal digits. Other escapes (\n, \", \\, \t,
            # \r) are exactly 2 chars total.
            j = last_bs + 1
            if j < n and escaped[j] in "01234567":
                # Octal escape: up to 3 octal digits.
                k = j
                while k < n and k < j + 3 and escaped[k] in "01234567":
                    k += 1
                escape_end = k
            else:
                escape_end = j + 1
            if escape_end > end:
                # Splitting here would break this escape; back up to before
                # the backslash and try again.
                end = last_bs
            else:
                break
        if end <= i:
            # Pathological: couldn't find a safe boundary in chunk_size chars.
            # Just take chunk_size chars; the C compiler will tell us if we
            # broke something.
            end = min(i + chunk_size, n)
        chunk = escaped[i:end]
        lines.append(f'    "{chunk}"')
        i = end

    body = "\n".join(lines)

    output = (
        "/*\n"
        " * lamo_runtime_data.c - AUTO-GENERATED. Do not edit by hand.\n"
        " *\n"
        " * Regenerate with: python3 scripts/embed_runtime.py\n"
        " *\n"
        " * Contains the Lamo runtime (from lamo_runtime.h) as an escaped C\n"
        " * string. The codegen emits this verbatim at the top of every\n"
        " * generated .c program, replacing ~600 lines of fprintf() calls.\n"
        " */\n"
        "\n"
        '#include "lamo_runtime_data.h"\n'
        "\n"
        "const char lamo_runtime_source[] =\n"
        f"{body}\n"
        ";\n"
    )

    with open(OUTPUT, "w", encoding="utf-8") as f:
        f.write(output)

    print(f"embedded {len(source)} bytes of runtime -> {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
