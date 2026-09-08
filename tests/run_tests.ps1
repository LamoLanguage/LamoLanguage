# LamoLanguage test runner (PowerShell, Windows).
#
# Tests are organized under tests/:
#   tests/valid/*.lamo    - programs that must `lamo check` successfully
#   tests/invalid/*.lamo  - programs that must FAIL `lamo check`
#   tests/runtime/*.lamo  - programs that must `lamo run` and produce stdout
#                            matching the sibling .expected file
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

$TestsDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ValidDir   = Join-Path $TestsDir "valid"
$InvalidDir = Join-Path $TestsDir "invalid"
$RuntimeDir = Join-Path $TestsDir "runtime"

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

function Invoke-LamoCheck([string]$file) {
    # Returns $true if lamo exited with code 0.
    $out = & $LamoPath check $file 2>&1
    return ($LASTEXITCODE -eq 0)
}

function Invoke-LamoRun([string]$file, [ref]$stdoutOut, [ref]$stderrOut) {
    # Sprint 1 fix: cap runtime tests at 10 seconds so a runaway Lamo
    # program (e.g. infinite loop) cannot hang the test suite. We use a
    # background job + Wait-Job -Timeout 10; on timeout we kill the job
    # and treat it as a failure.
    $stdout = ""
    $stderr = ""
    $job = Start-Job -ScriptBlock {
        param($lamo, $file)
        & $lamo run $file 2>&1
    } -ArgumentList $LamoPath, $file

    if (Wait-Job $job -Timeout 10) {
        # Job finished within the timeout. Collect output.
        $results = Receive-Job $job
        foreach ($line in $results) {
            if ($line -is [System.Management.Automation.ErrorRecord]) {
                $stderr += $line.Exception.Message + "`n"
            } else {
                $stdout += $line.ToString() + "`n"
            }
        }
        Remove-Job $job -Force
        $stdoutOut.Value = $stdout
        $stderrOut.Value = $stderr
        # If $LASTEXITCODE is set (i.e. the spawned process exited with a code),
        # use it; otherwise treat as success.
        return ($LASTEXITCODE -eq 0 -or $null -eq $LASTEXITCODE)
    } else {
        # Timed out. Kill the job and any child processes it spawned.
        Remove-Job $job -Force
        $stdoutOut.Value = ""
        $stderrOut.Value = "timed out after 10s`n"
        return $false
    }
}

# ---------------------------------------------------------------------------
# 1. Valid cases: must pass `lamo check` with exit 0.
# ---------------------------------------------------------------------------
Write-Host "== Valid programs (must check successfully) =="
if (Test-Path $ValidDir) {
    Get-ChildItem -Path $ValidDir -Filter *.lamo | ForEach-Object {
        $name = $_.Name
        if (Invoke-LamoCheck $_.FullName) {
            Record-Pass
            Write-Host ("  PASS  " + $name)
        } else {
            Record-Fail ("valid/" + $name)
            Write-Host ("  FAIL  " + $name)
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
        if (Invoke-LamoCheck $_.FullName) {
            Record-Fail ("invalid/" + $name + " (accepted but should have been rejected)")
            Write-Host ("  FAIL  " + $name + " (accepted but should have been rejected)")
        } else {
            Record-Pass
            Write-Host ("  PASS  " + $name)
        }
    }
}

# ---------------------------------------------------------------------------
# 3. Runtime cases: must `lamo run` and produce stdout matching .expected.
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
        $stdoutRef = [ref]""
        $stderrRef = [ref]""
        if (Invoke-LamoRun $_.FullName $stdoutRef $stderrRef) {
            $expected = (Get-Content -Raw $expectedFile) -replace "`r`n", "`n"
            $actual = ($stdoutRef.Value -join "`n") -replace "`r`n", "`n"
            # Normalize trailing newline
            if (-not $expected.EndsWith("`n")) { $expected += "`n" }
            if (-not $actual.EndsWith("`n")) { $actual += "`n" }
            if ($expected -eq $actual) {
                Record-Pass
                Write-Host ("  PASS  " + $name)
            } else {
                Record-Fail ("runtime/" + $name + " (stdout mismatch)")
                Write-Host ("  FAIL  " + $name + " (stdout mismatch)")
                Write-Host ("        expected: $expected")
                Write-Host ("        actual:   $actual")
            }
        } else {
            Record-Fail ("runtime/" + $name + " (run failed)")
            Write-Host ("  FAIL  " + $name + " (run failed)")
            Write-Host ("        " + $stderrRef.Value)
        }
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=========================================="
Write-Host ("Total: " + $script:Pass + " passed, " + $script:Fail + " failed")
if ($script:Fail -ne 0) {
    Write-Host "Failed cases:"
    $script:FailedCases | ForEach-Object { Write-Host ("  - " + $_) }
    exit 1
}
exit 0
