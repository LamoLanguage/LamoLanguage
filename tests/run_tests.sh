#!/bin/sh
# LamoLanguage test runner (POSIX sh).
#
# Tests are organized under tests/:
#   tests/valid/*.lamo    - programs that must `lamo check` successfully
#   tests/invalid/*.lamo  - programs that must FAIL `lamo check`
#   tests/runtime/*.lamo  - programs that must `lamo run` and produce stdout
#                            matching the sibling .expected file
#
# Usage:
#   sh tests/run_tests.sh           # auto-detect ./lamo or ./lamo.exe
#   sh tests/run_tests.sh /path/lamo
#
# Exit code: 0 if all tests passed, 1 otherwise.

set -u

# ---------------------------------------------------------------------------
# Resolve the Lamo binary.
# ---------------------------------------------------------------------------
if [ $# -ge 1 ]; then
    LAMO="$1"
else
    if [ -x "./lamo" ]; then
        LAMO="./lamo"
    elif [ -x "./lamo.exe" ]; then
        LAMO="./lamo.exe"
    else
        echo "error: lamo binary not found. Build it first with 'make' or pass its path as arg." >&2
        exit 2
    fi
fi

# Absolutize so subshells (golden tests cd into temp dirs) still find it.
case "$LAMO" in
    /*) ;;
    *)  LAMO="$(cd "$(dirname "$LAMO")" && pwd)/$(basename "$LAMO")" ;;
esac

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
VALID_DIR="$TESTS_DIR/valid"
INVALID_DIR="$TESTS_DIR/invalid"
RUNTIME_DIR="$TESTS_DIR/runtime"
STD_TESTS_DIR="$TESTS_DIR/../std/tests"

TMP_DIR="$(mktemp -d 2>/dev/null || mktemp -d /tmp/lamo.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

PASS=0
FAIL=0
FAILED_CASES=""

record_pass() {
    PASS=$((PASS + 1))
}

record_fail() {
    FAIL=$((FAIL + 1))
    FAILED_CASES="$FAILED_CASES
  - $1"
}

# Sprint 1 fix: prevent runaway runtime tests from hanging the suite.
# Try GNU coreutils `timeout`, then the BusyBox `timeout` (same syntax),
# then `gtimeout` (macOS Homebrew coreutils), then fall back to bare
# execution if none is available (we still run the test, just without
# protection — same as before this fix).
TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi

# Wraps a command with a 10s timeout if a timeout binary is available.
# Usage: run_with_timeout <cmd> <args...>
# Returns the exit status of the wrapped command (or 124 on timeout).
run_with_timeout() {
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" 10 "$@"
    else
        "$@"
    fi
}

# ---------------------------------------------------------------------------
# 1. Valid cases: must pass `lamo check` with exit 0.
# ---------------------------------------------------------------------------
echo "== Valid programs (must check successfully) =="
if [ -d "$VALID_DIR" ]; then
    for src in "$VALID_DIR"/*.lamo; do
        [ -e "$src" ] || continue
        name=$(basename "$src")
        if "$LAMO" check "$src" >"$TMP_DIR/out" 2>"$TMP_DIR/err"; then
            record_pass
            printf "  PASS  %s\n" "$name"
        else
            record_fail "valid/$name"
            printf "  FAIL  %s\n" "$name"
            sed 's/^/        | /' "$TMP_DIR/err" >&2
        fi
    done
fi

# ---------------------------------------------------------------------------
# 2. Invalid cases: must FAIL `lamo check` with non-zero exit.
# ---------------------------------------------------------------------------
echo
echo "== Invalid programs (must fail check) =="
if [ -d "$INVALID_DIR" ]; then
    for src in "$INVALID_DIR"/*.lamo; do
        [ -e "$src" ] || continue
        name=$(basename "$src")
        if "$LAMO" check "$src" >"$TMP_DIR/out" 2>"$TMP_DIR/err"; then
            record_fail "invalid/$name (accepted but should have been rejected)"
            printf "  FAIL  %s (accepted but should have been rejected)\n" "$name"
        else
            record_pass
            printf "  PASS  %s\n" "$name"
        fi
    done
fi


# ---------------------------------------------------------------------------
# 2.5 Smoke cases (Phase 2: parser smoke tests + diagnostics regression).
#
# Layout: tests/smoke/NAME.lamo plus an OPTIONAL directive file:
#   NAME.expect_err  - compile must FAIL; each line is a substring that must
#                      appear in stderr.
#   NAME.expect_ok   - compile must SUCCEED (exit 0); each line in the file
#                      is a substring that must appear in stderr (used to
#                      pin warnings, e.g. deprecation notices).
# Without either file, the case requires success with EMPTY stderr.
# ---------------------------------------------------------------------------
echo
echo "== Smoke / diagnostic cases =="
SMOKE_DIR="$TESTS_DIR/smoke"
if [ -d "$SMOKE_DIR" ]; then
    for src in "$SMOKE_DIR"/*.lamo; do
        [ -e "$src" ] || continue
        name=$(basename "$src")
        base="${src%.lamo}"
        expect_err="$base.expect_err"
        expect_ok="$base.expect_ok"
        if [ -e "$expect_err" ]; then
            if "$LAMO" check "$src" >"$TMP_DIR/out" 2>"$TMP_DIR/err"; then
                record_fail "smoke/$name (accepted but expected failure)"
                printf "  FAIL  %s (expected compile failure)\n" "$name"
            else
                missing=""
                while IFS= read -r want; do
                    [ -z "$want" ] && continue
                    if ! grep -qF -- "$want" "$TMP_DIR/err"; then
                        missing="$missing [$want]"
                    fi
                done < "$expect_err"
                if [ -n "$missing" ]; then
                    record_fail "smoke/$name (stderr missing$missing)"
                    printf "  FAIL  %s (stderr missing%s)\n" "$name" "$missing"
                    sed 's/^/        | /' "$TMP_DIR/err" >&2
                else
                    record_pass
                    printf "  PASS  %s\n" "$name"
                fi
            fi
        elif [ -e "$expect_ok" ]; then
            if "$LAMO" check "$src" >"$TMP_DIR/out" 2>"$TMP_DIR/err"; then
                missing=""
                while IFS= read -r want; do
                    [ -z "$want" ] && continue
                    if ! grep -qF -- "$want" "$TMP_DIR/err"; then
                        missing="$missing [$want]"
                    fi
                done < "$expect_ok"
                if [ -n "$missing" ]; then
                    record_fail "smoke/$name (warning missing$missing)"
                    printf "  FAIL  %s (stderr missing%s)\n" "$name" "$missing"
                    sed 's/^/        | /' "$TMP_DIR/err" >&2
                else
                    record_pass
                    printf "  PASS  %s\n" "$name"
                fi
            else
                record_fail "smoke/$name (rejected, expected success)"
                printf "  FAIL  %s (rejected)\n" "$name"
                sed 's/^/        | /' "$TMP_DIR/err" >&2
            fi
        else
            if "$LAMO" check "$src" >"$TMP_DIR/out" 2>"$TMP_DIR/err" && [ ! -s "$TMP_DIR/err" ]; then
                record_pass
                printf "  PASS  %s\n" "$name"
            else
                record_fail "smoke/$name (must succeed silently)"
                printf "  FAIL  %s\n" "$name"
                sed 's/^/        | /' "$TMP_DIR/err" >&2
            fi
        fi
    done
fi

# ---------------------------------------------------------------------------
# 3.5 Golden C-output tests (Phase 2: snapshot tests for generated code).
#
# Layout: tests/golden/NAME.lamo + NAME.c.expected. Each program is built
# in an isolated temp CWD via `lamo build` and the generated lamo_exec.c is
# diffed against the committed snapshot. Codegen is deterministic (no
# timestamps or absolute paths), which makes plain diff viable. The source
# file is COPIED into the temp dir under its own name so any embedded
# relative-path comments stay stable across machines.
# ---------------------------------------------------------------------------
echo
echo "== Golden generated-C snapshot tests =="
GOLDEN_DIR="$TESTS_DIR/golden"
if [ -d "$GOLDEN_DIR" ]; then
    for src in "$GOLDEN_DIR"/*.lamo; do
        [ -e "$src" ] || continue
        name=$(basename "$src")
        expected_file="${src%.lamo}.c.expected"
        if [ ! -e "$expected_file" ]; then
            record_fail "golden/$name (missing .c.expected)"
            printf "  FAIL  %s (missing .c.expected)\n" "$name"
            continue
        fi
        work="$TMP_DIR/golden_$(basename "$name" .lamo)"
        rm -rf "$work"; mkdir -p "$work"
        cp "$src" "$work/"
        if ( cd "$work" && run_with_timeout "$LAMO" build "$(basename "$src")" -o out_bin >build.log 2>&1 ) && [ -f "$work/lamo_exec.c" ]; then
            # The embedded runtime (~2900 lines) is identical across all
            # programs and is version-controlled directly as
            # src/codegen/lamo_runtime.h; snapshots cover the USER-CODE
            # section only (everything from the trailing #undefs on).
            tr -d '\r' < "$work/lamo_exec.c" | sed -e '/^#ifndef LAMO_RUNTIME_H$/,/^#endif \/\* LAMO_RUNTIME_H \*\//d' > "$work/gen_clean"
            tr -d '\r' < "$expected_file" > "$work/exp_clean"
            if diff -u "$work/exp_clean" "$work/gen_clean" > "$work/diff" 2>&1; then
                record_pass
                printf "  PASS  %s\n" "$name"
            else
                record_fail "golden/$name (generated C drift)"
                printf "  FAIL  %s (snapshot mismatch; update .c.expected if intentional)\n" "$name"
                sed 's/^/        | /' "$work/diff" | head -40 >&2
            fi
        else
            rc=$?
            if [ "$rc" = 124 ]; then
                record_fail "golden/$name (timed out)"
                printf "  FAIL  %s (timed out)\n" "$name"
            else
                record_fail "golden/$name (build failed)"
                printf "  FAIL  %s (build failed)\n" "$name"
                sed 's/^/        | /' "$work/build.log" >&2
            fi
        fi
    done
fi

# ---------------------------------------------------------------------------
# 3. Runtime cases: must `lamo run` and produce stdout matching .expected.
# ---------------------------------------------------------------------------
echo
echo "== Runtime cases (must run and match expected stdout) =="
if [ -d "$RUNTIME_DIR" ]; then
    for src in "$RUNTIME_DIR"/*.lamo; do
        [ -e "$src" ] || continue
        name=$(basename "$src")
        expected_file="${src%.lamo}.expected"
        stdin_file="${src%.lamo}.stdin"
        [ -e "$stdin_file" ] || stdin_file=""
        if [ ! -e "$expected_file" ]; then
            record_fail "runtime/$name (missing .expected file)"
            printf "  FAIL  %s (missing .expected file)\n" "$name"
            continue
        fi
        if run_with_timeout "$LAMO" run "$src" >"$TMP_DIR/actual" 2>"$TMP_DIR/err" <"${stdin_file:-/dev/null}"; then
            # Strip trailing whitespace differences by trimming both files.
            # `lamo run` may emit trailing spaces depending on backend; we
            # compare ignoring them.
            tr -d '\r' < "$TMP_DIR/actual" > "$TMP_DIR/actual_clean"
            tr -d '\r' < "$expected_file" > "$TMP_DIR/expected_clean"
            if diff -u "$TMP_DIR/expected_clean" "$TMP_DIR/actual_clean" >"$TMP_DIR/diff" 2>&1; then
                record_pass
                printf "  PASS  %s\n" "$name"
            else
                record_fail "runtime/$name (stdout mismatch)"
                printf "  FAIL  %s (stdout mismatch)\n" "$name"
                sed 's/^/        | /' "$TMP_DIR/diff" >&2
            fi
        else
            rc=$?
            if [ "$rc" = 124 ]; then
                record_fail "runtime/$name (timed out after 10s)"
                printf "  FAIL  %s (timed out after 10s)\n" "$name"
            else
                record_fail "runtime/$name (run failed)"
                printf "  FAIL  %s (run failed)\n" "$name"
                sed 's/^/        | /' "$TMP_DIR/err" >&2
            fi
        fi
    done
fi

# ---------------------------------------------------------------------------
# 4. Standard library tests: must `lamo run` and exit 0 (the test files
#    use std.testing internally and print PASS/FAIL lines themselves;
#    they exit non-zero if any test failed).
# ---------------------------------------------------------------------------
echo
echo "== Standard library tests (std/tests/*.lamo) =="
if [ -d "$STD_TESTS_DIR" ]; then
    for src in "$STD_TESTS_DIR"/*.lamo; do
        [ -e "$src" ] || continue
        name=$(basename "$src")
        if run_with_timeout "$LAMO" run "$src" >"$TMP_DIR/actual" 2>"$TMP_DIR/err"; then
            # Check that no failures were reported in the output.
            if grep -q "0 failed" "$TMP_DIR/actual"; then
                record_pass
                printf "  PASS  %s\n" "$name"
            else
                record_fail "std/$name (test failures reported)"
                printf "  FAIL  %s (test failures reported)\n" "$name"
                sed 's/^/        | /' "$TMP_DIR/actual" >&2
            fi
        else
            rc=$?
            if [ "$rc" = 124 ]; then
                record_fail "std/$name (timed out after 10s)"
                printf "  FAIL  %s (timed out after 10s)\n" "$name"
            else
                record_fail "std/$name (run failed)"
                printf "  FAIL  %s (run failed)\n" "$name"
                sed 's/^/        | /' "$TMP_DIR/err" >&2
            fi
        fi
    done
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "=========================================="
printf "Total: %d passed, %d failed\n" "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    echo "Failed cases:$FAILED_CASES"
    exit 1
fi
exit 0
