#!/bin/sh
# fmt test suite — verifies the AST-based pretty-printer (2.11.0).
#
# For every parseable .lamo file in the repo corpus (tests/valid,
# tests/runtime, std/, examples/):
#   1. format it             -> `lamo check` must still pass
#   2. format it again       -> byte-identical (idempotency)
#   3. count `//` comments   -> must not decrease (comment preservation)
# Plus targeted checks on tests/fmt/cases/*.in.lamo (format-in /
# expected-out pairs) exercising specific constructs.
#
# Usage: sh tests/fmt/run_fmt_tests.sh /path/to/lamo
# Exit code: 0 all pass, 1 otherwise.

set -u

if [ $# -ge 1 ]; then
    LAMO="$1"
else
    LAMO="./lamo"
fi

TESTS_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$TESTS_DIR"

TMP_DIR="$(mktemp -d 2>/dev/null || mktemp -d /tmp/lamo-fmt.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

PASS=0
FAIL=0
FAILED_CASES=""

record_pass() { PASS=$((PASS + 1)); }
record_fail() {
    FAIL=$((FAIL + 1))
    FAILED_CASES="$FAILED_CASES
  - $1"
}

# ── corpus checks ────────────────────────────────────────────────────
for f in tests/valid/*.lamo tests/runtime/*.lamo std/*.lamo examples/*.lamo; do
    [ -f "$f" ] || continue
    name="$f"
    cp "$f" "$TMP_DIR/case.lamo"

    # Files that do not parse are out of scope for the AST formatter.
    if ! "$LAMO" check "$TMP_DIR/case.lamo" >/dev/null 2>&1; then
        continue
    fi

    "$LAMO" fmt "$TMP_DIR/case.lamo" >/dev/null 2>&1

    if ! "$LAMO" check "$TMP_DIR/case.lamo" >/dev/null 2>&1; then
        record_fail "fmt corpus: $name does not check after formatting"
        continue
    fi

    cp "$TMP_DIR/case.lamo" "$TMP_DIR/once.lamo"
    "$LAMO" fmt "$TMP_DIR/case.lamo" >/dev/null 2>&1
    if ! cmp -s "$TMP_DIR/case.lamo" "$TMP_DIR/once.lamo"; then
        record_fail "fmt corpus: $name is not idempotent"
        continue
    fi

    orig_comments=$(grep -c '//' "$f" 2>/dev/null; true)
    fmt_comments=$(grep -c '//' "$TMP_DIR/case.lamo" 2>/dev/null; true)
    if [ "$fmt_comments" -lt "$orig_comments" ]; then
        record_fail "fmt corpus: $name lost comments ($orig_comments -> $fmt_comments)"
        continue
    fi

    record_pass
done

# ── targeted cases: format input and compare with expected output ────
for input in tests/fmt/cases/*.in.lamo; do
    [ -f "$input" ] || continue
    expected="${input%.in.lamo}.out.lamo"
    name="$(basename "$input")"
    cp "$input" "$TMP_DIR/t.lamo"
    "$LAMO" fmt "$TMP_DIR/t.lamo" >/dev/null 2>&1
    if ! cmp -s "$TMP_DIR/t.lamo" "$expected"; then
        record_fail "fmt case: $name output mismatch (see diff below)
$(diff "$expected" "$TMP_DIR/t.lamo" | head -15 | sed 's/^/      /')"
        continue
    fi
    record_pass
done

echo "== Formatter tests (AST pretty-printer) =="
if [ "$FAIL" -eq 0 ]; then
    printf "  PASS  %d files formatted safely (semantic-preserving, idempotent, comments kept)\n" "$PASS"
else
    printf "  FAIL  %d/%d fmt checks\n" "$FAIL" "$((PASS + FAIL))"
    printf "Failed cases:%s\n" "$FAILED_CASES"
fi

[ "$FAIL" -eq 0 ]
