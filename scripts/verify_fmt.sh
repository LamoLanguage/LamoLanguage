#!/bin/sh
# Comprehensive fmt verification (development harness, not part of make test).
# For every parseable .lamo file in the repo:
#   1. original must check-pass
#   2. fmt it -> must still check-pass (semantic preservation)
#   3. fmt again -> byte-identical (idempotency)
#   4. comment count must not decrease (comment preservation)
#   5. runtime files: run output must match .expected byte-for-byte
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"
LAMO=./lamo
TMP=$(mktemp -d)
PASS=0; FAIL=0; SKIP=0; FAILED=""

verify_file() {
    f="$1"
    cp "$f" "$TMP/a.lamo"
    # 1. original must check
    if ! $LAMO check "$TMP/a.lamo" >/dev/null 2>&1; then
        SKIP=$((SKIP+1)); return
    fi
    # 2. format
    if ! $LAMO fmt "$TMP/a.lamo" >/dev/null 2>&1; then
        FAIL=$((FAIL+1)); FAILED="$FAILED\n  fmt-fail: $f"; return
    fi
    if ! $LAMO check "$TMP/a.lamo" >/dev/null 2>&1; then
        FAIL=$((FAIL+1)); FAILED="$FAILED\n  check-after-fmt: $f"; return
    fi
    # 3. idempotency
    cp "$TMP/a.lamo" "$TMP/b.lamo"
    $LAMO fmt "$TMP/a.lamo" >/dev/null 2>&1
    if ! cmp -s "$TMP/a.lamo" "$TMP/b.lamo"; then
        FAIL=$((FAIL+1)); FAILED="$FAILED\n  not-idempotent: $f"; return
    fi
    # 4. comments preserved (grep -c prints 0 with exit 1 when none)
    orig_c=$(grep -c '//' "$f" 2>/dev/null; true)
    fmt_c=$(grep -c '//' "$TMP/a.lamo" 2>/dev/null; true)
    if [ "$fmt_c" -lt "$orig_c" ]; then
        FAIL=$((FAIL+1)); FAILED="$FAILED\n  comment-loss: $f ($orig_c -> $fmt_c)"; return
    fi
    PASS=$((PASS+1))
}

run_output() {
    f="$1"
    exp="${f%.lamo}.expected"
    [ -f "$exp" ] || return 0
    cp "$f" "$TMP/r.lamo"
    $LAMO check "$TMP/r.lamo" >/dev/null 2>&1 || return 0
    $LAMO fmt "$TMP/r.lamo" >/dev/null 2>&1
    # Golden-suite programs terminate by design; timeout guards anyway.
    # stdin: use the sibling .stdin file when present, else /dev/null
    # (tests exercising input() must not block).
    stdin_file="${f%.lamo}.stdin"
    [ -e "$stdin_file" ] || stdin_file=/dev/null
    if ! timeout 30 $LAMO run "$TMP/r.lamo" > "$TMP/out.txt" 2>/dev/null <"$stdin_file"; then
        FAIL=$((FAIL+1)); FAILED="$FAILED\n  run-after-fmt: $f"; return
    fi
    if ! cmp -s "$TMP/out.txt" "$exp"; then
        FAIL=$((FAIL+1)); FAILED="$FAILED\n  output-changed: $f"; return
    fi
}

for f in tests/valid/*.lamo tests/runtime/*.lamo std/*.lamo examples/*.lamo; do
    [ -f "$f" ] || continue
    verify_file "$f"
done

# Runtime golden outputs: only the terminating suite under tests/runtime.
for f in tests/runtime/*.lamo; do
    [ -f "$f" ] || continue
    run_output "$f"
done

rm -rf "$TMP"
echo "fmt-verify: pass=$PASS fail=$FAIL skip=$SKIP"
[ -n "$FAILED" ] && printf "$FAILED\n"
[ "$FAIL" -eq 0 ]
