# LamoLanguage test runner (PowerShell, Windows).
#
# Mirrors tests/run_tests.sh section for section:
#   tests/valid/*.lamo    - programs that must `lamo check` successfully
#   tests/invalid/*.lamo  - programs that must FAIL `lamo check`
#   tests/smoke/*.lamo    - parser/semantic contracts via sibling
#                           .expect_err / .expect_ok directive files
#                           (each non-empty directive line is a required
#                           stderr substring; no directive = must compile
#                           with EMPTY stderr)
#   tests/golden/*.lamo   - `lamo build` output must match the sibling
#                           .c.expected snapshot (the embedded runtime
#                           block is filtered out of the comparison)
#   tests/runtime/*.lamo  - programs that must `lamo run` and produce
#                           stdout matching the sibling .expected file
#                           (optional sibling .stdin feeds the process)
#   tests/eval/*.lamo     - must `lamo eval` and match .expected
#                           (SPEC §10.7 interpreter parity)
#   std/tests/*.lamo      - self-testing stdlib modules; must exit 0 AND
#                           print "0 failed"
#
# 2.8.0 (FU2): smoke, golden, and std sections plus runtime .stdin
# support complete the Windows runner; the run/eval invocations moved
# onto System.Diagnostics.Process so the REAL exit code propagates
# (Start-Job swallowed it) and stdin redirection is possible.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tests/run_tests.ps1
#   powershell -ExecutionPolicy Bypass -File tests/run_tests.ps1 -LamoPath .\lamo.exe

param(
    [string]$LamoPath = ""
)

$ErrorActionPreference = "Continue"

# ---------------------------------------------------------------------------
# Resolve the Lamo binary.
# ---------------------------------------------------------------------------
if ($LamoPath -eq "") {
    if (Test-Path ".\lamo.exe") {
        $LamoPath = ".\lamo.exe"
    } elseif (Test-Path ".\lamo") {
        $LamoPath = ".\lamo"
    } else {
        Write-Error "error: lamo binary not found. Build it first with 'make' or pass -LamoPath."
        exit 2
    }
}

# Absolutize: the golden section builds inside temp working directories,
# and the resolved path must survive the CWD change (run_tests.sh does
# the same before cd-ing into subshells).
$LamoPath = [System.IO.Path]::GetFullPath(($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LamoPath)))
$script:LamoAbs = $LamoPath

$TestsDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$ValidDir    = Join-Path $TestsDir "valid"
$InvalidDir  = Join-Path $TestsDir "invalid"
$SmokeDir    = Join-Path $TestsDir "smoke"
$GoldenDir   = Join-Path $TestsDir "golden"
$RuntimeDir  = Join-Path $TestsDir "runtime"
$EvalDir     = Join-Path $TestsDir "eval"
$StdTestsDir = Join-Path (Split-Path -Parent $TestsDir) "std\tests"

# Temp workspace for the golden builds (mktemp -d equivalent).
$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("lamo_tests_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $TempRoot -Force | Out-Null

$script:Pass = 0
$script:Fail = 0
$script:FailedCases = New-Object System.Collections.Generic.List[string]

function Record-Pass {
    $script:Pass++
}

function Record-Fail([string]$name) {
    $script:Fail++
    $script:FailedCases.Add($name) | Out-Null
}

function Invoke-LamoProcess {
    # The workhorse behind every section: start the compiler as a real
    # child process, capture stdout/stderr through async reads (so a
    # chatty child cannot deadlock on a full pipe), feed an optional
    # stdin file, and enforce a 10-second cap exactly like run_tests.sh.
    # Returns the REAL exit status ($LASTEXITCODE is unreadable here —
    # the old Start-Job approach lost it).
    param(
        [string[]]$ArgumentList,
        [string]$WorkingDirectory = (Get-Location).Path,
        [string]$StdinFile = "",
        [ref]$stdoutOut,
        [ref]$stderrOut,
        [int]$TimeoutSeconds = 10
    )
    $stdoutOut.Value = ""
    $stderrOut.Value = ""
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $script:LamoAbs
    # Quoted argument join (ProcessStartInfo.ArgumentList is .NET Core
    # only; this keeps Windows PowerShell 5.1 compatibility).
    $quoted = ($ArgumentList | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join " "
    $psi.Arguments = $quoted
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if ($StdinFile -ne "") { $psi.RedirectStandardInput = $true }

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    if (-not $proc.Start()) {
        $stderrOut.Value = "failed to start lamo process`n"
        return $false
    }
    # Begin async reads BEFORE waiting, in case output exceeds the pipe
    # buffer while the child is still running.
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if ($StdinFile -ne "") {
        $writer = $proc.StandardInput
        $writer.Write([System.IO.File]::ReadAllText($StdinFile))
        $writer.Close()
    }
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
        try { $proc.Kill() } catch { }
        $stdoutOut.Value = ""
        $stderrOut.Value = "timed out after ${TimeoutSeconds}s`n"
        return $false
    }
    $stdoutOut.Value = $outTask.GetAwaiter().GetResult()
    $stderrOut.Value = $errTask.GetAwaiter().GetResult()
    return ($proc.ExitCode -eq 0)
}

function Invoke-LamoRun([string]$file, [ref]$stdoutOut, [ref]$stderrOut, [string]$Mode = "run") {
    # 2.7.0 (FU5): $Mode selects the subcommand — "run" for the runtime
    # suite or "eval" for the interpreter suite (SPEC §10.7 parity).
    Invoke-LamoProcess -ArgumentList @($Mode, $file) -stdoutOut $stdoutOut -stderrOut $stderrOut
}

function Invoke-LamoCheck([string]$file, [ref]$stdoutOut, [ref]$stderrOut) {
    # Returns $true if lamo check exited with code 0; stderr surfaces
    # through the ref so valid-section failures print diagnostics like
    # the POSIX runner.
    Invoke-LamoProcess -ArgumentList @("check", $file) -stdoutOut $stdoutOut -stderrOut $stderrOut
}

function Compare-LamoStdout([string]$expectedRaw, [string]$actualRaw) {
    # CRLF-insensitive whole-output equality with a normalized trailing
    # newline (mirrors the `tr -d '\r'` + diff of run_tests.sh).
    $expected = $expectedRaw -replace "`r`n", "`n"
    $actual = $actualRaw -replace "`r`n", "`n"
    if (-not $expected.EndsWith("`n")) { $expected += "`n" }
    if (-not $actual.EndsWith("`n")) { $actual += "`n" }
    return ($expected -eq $actual)
}

function Filter-LamoRuntimeBlock([string]$text) {
    # Golden helper: strip the embedded runtime (identical for every
    # program, version-controlled as src/codegen/lamo_runtime.h) from
    # #ifndef LAMO_RUNTIME_H through #endif /* LAMO_RUNTIME_H */ — the
    # snapshots cover the USER-CODE section only.
    $lines = ($text -replace "`r`n", "`n") -split "`n"
    $out = New-Object System.Collections.Generic.List[string]
    $skip = $false
    foreach ($ln in $lines) {
        $t = $ln.Trim()
        if (-not $skip -and $t -eq "#ifndef LAMO_RUNTIME_H") { $skip = $true; continue }
        if ($skip -and $t -eq "#endif /* LAMO_RUNTIME_H */") { $skip = $false; continue }
        if (-not $skip) { [void]$out.Add($ln) }
    }
    return ($out -join "`n")
}

# ---------------------------------------------------------------------------
# 1. Valid cases: must pass `lamo check` with exit 0.
# ---------------------------------------------------------------------------
Write-Host "== Valid programs (must check successfully) =="
if (Test-Path $ValidDir) {
    Get-ChildItem -Path $ValidDir -Filter *.lamo | ForEach-Object {
        $name = $_.Name
        $stdoutRef = [ref]""; $stderrRef = [ref]""
        if (Invoke-LamoCheck $_.FullName $stdoutRef $stderrRef) {
            Record-Pass
            Write-Host ("  PASS  " + $name)
        } else {
            Record-Fail ("valid/" + $name)
            Write-Host ("  FAIL  " + $name)
            $stderrRef.Value -split "`n" | ForEach-Object { if ($_.Trim() -ne "") { Write-Host ("        | " + $_) } }
        }
    }
}

# ---------------------------------------------------------------------------
# 2. Invalid cases: must FAIL `lamo check` with non-zero exit.
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "== Invalid programs (must fail check) =="
if (Test-Path $InvalidDir) {
    Get-ChildItem -Path $InvalidDir -Filter *.lamo | ForEach-Object {
        $name = $_.Name
        $stdoutRef = [ref]""; $stderrRef = [ref]""
        if (Invoke-LamoCheck $_.FullName $stdoutRef $stderrRef) {
            Record-Fail ("invalid/" + $name + " (accepted but should have been rejected)")
            Write-Host ("  FAIL  " + $name + " (accepted but should have been rejected)")
        } else {
            Record-Pass
            Write-Host ("  PASS  " + $name)
        }
    }
}

# ---------------------------------------------------------------------------
# 2.5 Smoke cases: parser/semantic contracts (2.8.0 FU2).
#   NAME.expect_err -> compile must FAIL; every non-empty line is a
#                      required stderr substring.
#   NAME.expect_ok  -> compile must SUCCEED; every non-empty line is a
#                      required stderr substring (pins warnings).
#   neither         -> must compile with EMPTY stderr.
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "== Smoke cases (diagnostic contracts) =="
if (Test-Path $SmokeDir) {
    Get-ChildItem -Path $SmokeDir -Filter *.lamo | ForEach-Object {
        $name = $_.Name
        $base = [System.IO.Path]::Combine($SmokeDir, [System.IO.Path]::GetFileNameWithoutExtension($name))
        $errFile = $base + ".expect_err"
        $okFile = $base + ".expect_ok"
        $stdoutRef = [ref]""; $stderrRef = [ref]""
        $exitOk = Invoke-LamoProcess -ArgumentList @("check", $_.FullName) -stdoutOut $stdoutRef -stderrOut $stderrRef
        if (Test-Path $errFile) {
            if ($exitOk) {
                Record-Fail ("smoke/" + $name + " (compiled but expected errors)")
                Write-Host ("  FAIL  " + $name + " (compiled but expected errors)")
                return
            }
            $missing = @()
            foreach ($want in (Get-Content $errFile)) {
                if ($want.Trim() -ne "" -and -not $stderrRef.Value.Contains($want)) { $missing += $want }
            }
            if ($missing.Count -eq 0) {
                Record-Pass
                Write-Host ("  PASS  " + $name)
            } else {
                Record-Fail ("smoke/" + $name + " (stderr missing [" + ($missing -join "] [") + "])")
                Write-Host ("  FAIL  " + $name + " (stderr missing [" + ($missing -join "] [") + "])")
            }
        } elseif (Test-Path $okFile) {
            if (-not $exitOk) {
                Record-Fail ("smoke/" + $name + " (should have compiled)")
                Write-Host ("  FAIL  " + $name + " (should have compiled)")
                $stderrRef.Value -split "`n" | ForEach-Object { if ($_.Trim() -ne "") { Write-Host ("        | " + $_) } }
                return
            }
            $missing = @()
            foreach ($want in (Get-Content $okFile)) {
                if ($want.Trim() -ne "" -and -not $stderrRef.Value.Contains($want)) { $missing += $want }
            }
            if ($missing.Count -eq 0) {
                Record-Pass
                Write-Host ("  PASS  " + $name)
            } else {
                Record-Fail ("smoke/" + $name + " (stderr missing [" + ($missing -join "] [") + "])")
                Write-Host ("  FAIL  " + $name + " (stderr missing [" + ($missing -join "] [") + "])")
            }
        } else {
            if (-not $exitOk) {
                Record-Fail ("smoke/" + $name + " (should check cleanly)")
                Write-Host ("  FAIL  " + $name + " (should check cleanly)")
                $stderrRef.Value -split "`n" | ForEach-Object { if ($_.Trim() -ne "") { Write-Host ("        | " + $_) } }
            } elseif ($stderrRef.Value.Trim().Length -ne 0) {
                Record-Fail ("smoke/" + $name + " (expected empty stderr)")
                Write-Host ("  FAIL  " + $name + " (expected empty stderr)")
                Write-Host ("        " + $stderrRef.Value)
            } else {
                Record-Pass
                Write-Host ("  PASS  " + $name)
            }
        }
    }
}

# ---------------------------------------------------------------------------
# 3. Runtime cases: must `lamo run` and produce stdout matching .expected.
#    2.8.0 (FU2): an optional sibling .stdin feeds the child process.
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "== Runtime cases (must run and match expected stdout) =="
if (Test-Path $RuntimeDir) {
    Get-ChildItem -Path $RuntimeDir -Filter *.lamo | ForEach-Object {
        $name = $_.Name
        $expectedFile = [System.IO.Path]::ChangeExtension($_.FullName, ".expected")
        if (-not (Test-Path $expectedFile)) {
            Record-Fail ("runtime/" + $name + " (missing .expected file)")
            Write-Host ("  FAIL  " + $name + " (missing .expected file)")
            return
        }
        $stdinFile = [System.IO.Path]::ChangeExtension($_.FullName, ".stdin")
        $stdoutRef = [ref]""; $stderrRef = [ref]""
        $ok = $false
        if (Test-Path $stdinFile) {
            $ok = Invoke-LamoProcess -ArgumentList @("run", $_.FullName) -StdinFile $stdinFile -stdoutOut $stdoutRef -stderrOut $stderrRef
        } else {
            $ok = Invoke-LamoRun $_.FullName $stdoutRef $stderrRef
        }
        if ($ok) {
            $expected = [string](Get-Content -Raw $expectedFile)
            if (Compare-LamoStdout $expected $stdoutRef.Value) {
                Record-Pass
                Write-Host ("  PASS  " + $name)
            } else {
                Record-Fail ("runtime/" + $name + " (stdout mismatch)")
                Write-Host ("  FAIL  " + $name + " (stdout mismatch)")
                Write-Host ("        expected: $expected")
                Write-Host ("        actual:   " + $stdoutRef.Value)
            }
        } else {
            $why = "run failed"
            if ($stderrRef.Value -like "timed out*") { $why = "timed out after 10s" }
            Record-Fail ("runtime/" + $name + " ($why)")
            Write-Host ("  FAIL  " + $name + " ($why)")
            Write-Host ("        " + $stderrRef.Value)
        }
    }
}

# ---------------------------------------------------------------------------
# 3.5 Eval cases (2.7.0 FU5): must `lamo eval` and produce stdout matching
#     the sibling .expected file. Mirrors the eval section of
#     tests/run_tests.sh (SPEC §10.7 interpreter module parity). Only
#     stdout is diffed; stderr is surfaced when the case fails.
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "== Eval cases (interpreter; must match expected stdout) =="
if (Test-Path $EvalDir) {
    Get-ChildItem -Path $EvalDir -Filter *.lamo | ForEach-Object {
        $name = $_.Name
        $expectedFile = [System.IO.Path]::ChangeExtension($_.FullName, ".expected")
        if (-not (Test-Path $expectedFile)) {
            Record-Fail ("eval/" + $name + " (missing .expected file)")
            Write-Host ("  FAIL  " + $name + " (missing .expected file)")
            return
        }
        $stdoutRef = [ref]""; $stderrRef = [ref]""
        if (Invoke-LamoRun $_.FullName $stdoutRef $stderrRef -Mode "eval") {
            # Coerce with [string] — an EMPTY .expected (e.g. modlib.expected)
            # makes Get-Content -Raw return $null, whose .EndsWith would throw.
            $expected = [string](Get-Content -Raw $expectedFile)
            if (Compare-LamoStdout $expected $stdoutRef.Value) {
                Record-Pass
                Write-Host ("  PASS  " + $name)
            } else {
                Record-Fail ("eval/" + $name + " (stdout mismatch)")
                Write-Host ("  FAIL  " + $name + " (stdout mismatch)")
                Write-Host ("        expected: $expected")
                Write-Host ("        actual:   " + $stdoutRef.Value)
            }
        } else {
            Record-Fail ("eval/" + $name + " (eval failed)")
            Write-Host ("  FAIL  " + $name + " (eval failed)")
            Write-Host ("        " + $stderrRef.Value)
        }
    }
}

# ---------------------------------------------------------------------------
# 3.6 Golden cases (2.8.0 FU2): `lamo build` must succeed and the
#     generated lamo_exec.c must match the sibling .c.expected snapshot.
#     The build runs in a per-case temp directory (so relative-path
#     comments in the source stay stable), and the embedded runtime block
#     is filtered out before the comparison — snapshots cover the
#     USER-CODE section only.
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "== Golden cases (generated C must match snapshot) =="
if (Test-Path $GoldenDir) {
    Get-ChildItem -Path $GoldenDir -Filter *.lamo | ForEach-Object {
        $name = $_.Name
        $expectedFile = [System.IO.Path]::ChangeExtension($_.FullName, ".c.expected")
        if (-not (Test-Path $expectedFile)) {
            Record-Fail ("golden/" + $name + " (missing .c.expected file)")
            Write-Host ("  FAIL  " + $name + " (missing .c.expected file)")
            return
        }
        $work = Join-Path $TempRoot ("golden_" + [System.IO.Path]::GetFileNameWithoutExtension($name))
        New-Item -ItemType Directory -Path $work -Force | Out-Null
        Copy-Item $_.FullName -Destination $work
        $stdoutRef = [ref]""; $stderrRef = [ref]""
        $ok = Invoke-LamoProcess -ArgumentList @("build", $name, "-o", "out_bin") -WorkingDirectory $work -stdoutOut $stdoutRef -stderrOut $stderrRef
        $lamoExec = Join-Path $work "lamo_exec.c"
        if (-not $ok -or -not (Test-Path $lamoExec)) {
            Record-Fail ("golden/" + $name + " (build failed)")
            Write-Host ("  FAIL  " + $name + " (build failed)")
            $stderrRef.Value -split "`n" | ForEach-Object { if ($_.Trim() -ne "") { Write-Host ("        | " + $_) } }
            return
        }
        $genClean = Filter-LamoRuntimeBlock ([System.IO.File]::ReadAllText($lamoExec))
        $expClean = ([string](Get-Content -Raw $expectedFile)) -replace "`r`n", "`n"
        if ($expClean -eq $genClean) {
            Record-Pass
            Write-Host ("  PASS  " + $name)
        } else {
            Record-Fail ("golden/" + $name + " (snapshot mismatch; update .c.expected if intentional)")
            Write-Host ("  FAIL  " + $name + " (snapshot mismatch; update .c.expected if intentional)")
            $diff = Compare-Object -ReferenceObject ($expClean -split "`n") -DifferenceObject ($genClean -split "`n") -SyncWindow 0 |
                Select-Object -First 40
            $diff | ForEach-Object {
                $side = if ($_.SideIndicator -eq "<=") { "expected" } else { "generated" }
                Write-Host ("        | [$side] " + $_.InputObject)
            }
        }
    }
}

# ---------------------------------------------------------------------------
# 4. Std library cases (2.8.0 FU2): the std/tests/*.lamo modules use
#    std.testing internally and print PASS/FAIL lines themselves; they
#    exit non-zero if any test failed. Pass = exit 0 AND "0 failed" in
#    stdout.
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "== Std library cases (self-testing modules; require '0 failed') =="
if (Test-Path $StdTestsDir) {
    Get-ChildItem -Path $StdTestsDir -Filter *.lamo | ForEach-Object {
        $name = $_.Name
        $stdoutRef = [ref]""; $stderrRef = [ref]""
        if (Invoke-LamoProcess -ArgumentList @("run", $_.FullName) -stdoutOut $stdoutRef -stderrOut $stderrRef) {
            if ($stdoutRef.Value -match "0 failed") {
                Record-Pass
                Write-Host ("  PASS  " + $name)
            } else {
                Record-Fail ("std/" + $name + " (failures reported)")
                Write-Host ("  FAIL  " + $name + " (failures reported)")
                $stdoutRef.Value -split "`n" | ForEach-Object { if ($_.Trim() -ne "") { Write-Host ("        | " + $_) } }
            }
        } else {
            Record-Fail ("std/" + $name + " (run failed)")
            Write-Host ("  FAIL  " + $name + " (run failed)")
            $stdoutRef.Value -split "`n" | ForEach-Object { if ($_.Trim() -ne "") { Write-Host ("        | " + $_) } }
            $stderrRef.Value -split "`n" | ForEach-Object { if ($_.Trim() -ne "") { Write-Host ("        | " + $_) } }
        }
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
if (Test-Path $TempRoot) {
    Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
}
Write-Host ""
Write-Host "=========================================="
Write-Host ("Total: " + $script:Pass + " passed, " + $script:Fail + " failed")
if ($script:Fail -ne 0) {
    Write-Host "Failed cases:"
    $script:FailedCases | ForEach-Object { Write-Host ("  - " + $_) }
    exit 1
}
exit 0
