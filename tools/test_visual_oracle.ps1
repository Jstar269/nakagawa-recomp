# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Behavioral regression tests for tools/hst_run_support.ps1.
.DESCRIPTION
    Driven by tools/test_visual_oracle.py so it runs inside the standard Python suite.
    These exercise real processes and real directories -- not string matching against the
    manager -- because all three helpers exist to prevent silent, expensive failures:
    a wait that outlives its process, an evidence directory that merges two runs, and a
    truncated capture set that reads as complete.

    Prints one "ok <name>" or "FAIL <name>: <reason>" line per test; exits nonzero on any
    failure.
#>

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "hst_run_support.ps1")

$script:Failures = 0
function Test-Case {
    param([string]$Name, [scriptblock]$Body)
    try {
        & $Body
        Write-Host "ok   $Name"
    } catch {
        Write-Host "FAIL $Name`: $($_.Exception.Message)"
        $script:Failures++
    }
}
function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

# A child process that exits after N seconds, with no console window of its own.
# `ping -n` rather than `timeout`: timeout refuses to run without an interactive console
# and exits 125 immediately, which would make the deadline test vacuously pass.
function Start-SleeperProcess {
    param([int]$Seconds)
    return Start-Process -FilePath "cmd.exe" `
        -ArgumentList @("/c", "ping -n $($Seconds + 1) 127.0.0.1 > nul") `
        -PassThru -WindowStyle Hidden
}

# --- Wait-ProcessOrKill: early exit -------------------------------------------------
# The defect this guards: the runner slept the entire deadline regardless of when the
# process exited, so a 44,000-vblank backstop (4,400s) added ~50 idle minutes to a
# ~25-minute replay that had already self-terminated at its requested vblank.
Test-Case "wait returns when the process exits, not at the deadline" {
    $p = Start-SleeperProcess -Seconds 2
    $r = Wait-ProcessOrKill -Process $p -TimeoutSeconds 120
    Assert-True (-not $r.TimedOut) "should not have timed out"
    Assert-True (-not $r.Killed)   "should not have been killed"
    Assert-True ($r.ElapsedSeconds -lt 30) `
        "returned after $($r.ElapsedSeconds)s; a deadline-sleeping wait would take 120s"
    Assert-True ($r.ElapsedSeconds -ge 1) `
        "returned after $($r.ElapsedSeconds)s, before the child could have exited"
}

# --- Wait-ProcessOrKill: deadline still enforced ------------------------------------
# Returning early must not cost the safety property: a hung run still dies at the
# deadline rather than holding the machine forever.
Test-Case "wait kills and reports a timeout at the deadline" {
    $p = Start-SleeperProcess -Seconds 90
    $r = Wait-ProcessOrKill -Process $p -TimeoutSeconds 2
    Assert-True ($r.TimedOut) "should have reported a timeout"
    Assert-True ($p.HasExited) "the process should have been killed"
    Assert-True ($r.ElapsedSeconds -lt 30) `
        "took $($r.ElapsedSeconds)s to enforce a 2s deadline"
}

# Redirection is not incidental here: it is the configuration the real runner uses, and
# Start-Process -PassThru with redirected streams returns a Process whose ExitCode reads
# back EMPTY unless the native handle was cached first. An earlier version of this test
# omitted the redirection and passed while the real oracle recorded no exit code at all.
Test-Case "wait reports the child's exit code (with redirected streams)" {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) "hst_exitcode_$PID"
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    try {
        $p = Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", "exit 3") `
                 -PassThru -WindowStyle Hidden `
                 -RedirectStandardOutput (Join-Path $tmp "out.txt") `
                 -RedirectStandardError (Join-Path $tmp "err.txt")
        $r = Wait-ProcessOrKill -Process $p -TimeoutSeconds 60
        Assert-True (-not $r.TimedOut) "should not have timed out"
        Assert-True ($null -ne $r.ExitCode) "exit code came back empty despite the handle cache"
        Assert-True ($r.ExitCode -eq 3) "expected exit code 3, got $($r.ExitCode)"
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case "wait reports exit code 0 for a clean exit" {
    $p = Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", "exit 0") `
             -PassThru -WindowStyle Hidden
    $r = Wait-ProcessOrKill -Process $p -TimeoutSeconds 60
    Assert-True ($null -ne $r.ExitCode) "clean exit must report 0, not an empty code"
    Assert-True ($r.ExitCode -eq 0) "expected exit code 0, got $($r.ExitCode)"
}

# --- Reset-OracleArchive: stale-output rejection ------------------------------------
# Snapshots are numbered per run, so a shorter second run into the same directory leaves
# the first run's tail in place and the mixed set still looks like one complete capture.
$tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) "hst_oracle_test_$PID"
New-Item -ItemType Directory -Path $tmpRoot -Force | Out-Null
try {
    Test-Case "archive reset creates a missing directory" {
        $d = Join-Path $tmpRoot "fresh"
        Assert-True (Reset-OracleArchive -Path $d -AllowedRoot $tmpRoot) "should have accepted a new name"
        Assert-True (Test-Path -LiteralPath $d) "directory was not created"
    }

    Test-Case "archive reset accepts an existing but empty directory" {
        $d = Join-Path $tmpRoot "empty"
        New-Item -ItemType Directory -Path $d -Force | Out-Null
        Assert-True (Reset-OracleArchive -Path $d -AllowedRoot $tmpRoot) "an empty directory holds no evidence to protect"
    }

    Test-Case "archive reset REJECTS a non-empty directory by default" {
        $d = Join-Path $tmpRoot "stale"
        New-Item -ItemType Directory -Path $d -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $d "snap_9.ppm") -Value "old evidence"
        $ok = Reset-OracleArchive -Path $d -AllowedRoot $tmpRoot
        Assert-True (-not $ok) "reused an oracle name that already held captures"
        Assert-True (Test-Path -LiteralPath (Join-Path $d "snap_9.ppm")) `
            "a rejected reset must not delete the existing evidence"
    }

    Test-Case "archive reset clears stale captures only with -Overwrite" {
        $d = Join-Path $tmpRoot "overwrite"
        New-Item -ItemType Directory -Path $d -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $d "snap_9.ppm") -Value "old evidence"
        Assert-True (Reset-OracleArchive -Path $d -AllowedRoot $tmpRoot -Overwrite) "-Overwrite should have been accepted"
        Assert-True (Test-Path -LiteralPath $d) "directory should still exist"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $d "snap_9.ppm"))) `
            "stale capture survived an -Overwrite reset"
    }
    # --- Sync-SaveBase: hold guest save state still across replays --------------------
    # Two replays of one route diverged because the first wrote a save that cleared a
    # first-time tutorial popup; the second hit a different screen sequence and ended
    # somewhere else entirely. Inputs being deterministic is not enough.
    Test-Case "save base captures the live save on first use" {
        $root = Join-Path $tmpRoot "ms1/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "run0"
        $base = Join-Path $tmpRoot "base1"
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($null -ne $r) "should have succeeded"
        Assert-True ($r.Action -eq "captured") "first use must capture, got $($r.Action)"
        Assert-True ((Get-Content -LiteralPath (Join-Path $base "UCUS98701/DATA0.BIN")) -eq "run0") `
            "baseline content not captured"
    }

    Test-Case "save base restores the baseline over a mutated save" {
        $root = Join-Path $tmpRoot "ms2/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "pristine"
        $base = Join-Path $tmpRoot "base2"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        # ...the run mutates the save, exactly as the give-up path's real save does.
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "mutated-by-run"
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/EXTRA.BIN") -Value "stray"
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($r.Action -eq "restored") "second use must restore, got $($r.Action)"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN")) -eq "pristine") `
            "save was not rolled back to the baseline"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $root "UCUS98701/EXTRA.BIN"))) `
            "a file the run added survived the restore"
    }

    Test-Case "save base never touches the GAMEDATA install" {
        # ~400 MB of installed game data, not save state. Removing it would trigger a
        # reinstall that changes the route's timing completely.
        $root = Join-Path $tmpRoot "ms3/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701GAMEDATA") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "save"
        Set-Content -LiteralPath (Join-Path $root "UCUS98701GAMEDATA/BIG.BIN") -Value "install"
        $base = Join-Path $tmpRoot "base3"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $base "UCUS98701GAMEDATA"))) `
            "GAMEDATA must not be copied into the baseline"
        # The restore's own verdict has to be asserted. Discarding it hid the defect this
        # test was written to cover: post-swap verification compared the WHOLE live root
        # against a manifest that deliberately excludes GAMEDATA, so every GAMEDATA file
        # counted as an unexplained extra and the restore always failed -- while GAMEDATA
        # itself did survive, so the surviving-install assertion still passed.
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($null -ne $r) "restore must not fail merely because a GAMEDATA install exists"
        Assert-True ($r.Action -eq "restored") "second use must restore, got $($r.Action)"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701GAMEDATA/BIG.BIN")) -eq "install") `
            "GAMEDATA must survive a restore untouched"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN")) -eq "save") `
            "the managed save must be restored alongside an untouched install"
    }

    Test-Case "a completed restore leaves no swap orphan behind" {
        # An orphan .hst_savebase_* sibling is the signal for an interrupted swap, and the
        # next restore fails closed on it. A restore that ran to completion must therefore
        # leave none, or one run poisons every later run of the same baseline.
        $root = Join-Path $tmpRoot "ms4/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701GAMEDATA") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "save"
        Set-Content -LiteralPath (Join-Path $root "UCUS98701GAMEDATA/BIG.BIN") -Value "install"
        $base = Join-Path $tmpRoot "base4"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        $parent = [IO.Path]::GetDirectoryName((Get-CanonicalPath -Path $root))
        $orphans = @(Get-ChildItem -LiteralPath $parent -Directory -Force |
            Where-Object { $_.Name -like ".hst_savebase_*" })
        Assert-True ($orphans.Count -eq 0) `
            "restore left $($orphans.Count) swap orphan(s), which fails every later restore closed"
        # ...and a third run must still be able to restore, not trip the orphan guard.
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($null -ne $r -and $r.Action -eq "restored") `
            "a repeated restore must keep working across runs"
    }
} finally {
    Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
}

# --- Get-OracleVerdict: incomplete-run failure --------------------------------------
# A truncated run's output directory looks exactly like a good one's, just shorter.
# Reading one as complete already cost two full replays in the #29 investigation.
Test-Case "verdict accepts a run that reached its requested vblank" {
    $v = Get-OracleVerdict -ReachedExit $true -TimedOut $false -ExitCode 0 `
             -CaptureCount 18 -RequestedVblank 44000 -ObservedVblank 44000
    Assert-True ($v.Complete) "clean run rejected: $($v.Reasons -join '; ')"
}

Test-Case "verdict rejects a run killed at the backstop" {
    $v = Get-OracleVerdict -ReachedExit $false -TimedOut $true -ExitCode 1 `
             -CaptureCount 18 -RequestedVblank 44000 -ObservedVblank 0
    Assert-True (-not $v.Complete) "a killed run must not be admissible"
    Assert-True (($v.Reasons -join ' ') -match "backstop") "reason should name the backstop"
}

Test-Case "verdict rejects a run that stopped short of its requested vblank" {
    $v = Get-OracleVerdict -ReachedExit $true -TimedOut $false -ExitCode 0 `
             -CaptureCount 18 -RequestedVblank 44000 -ObservedVblank 41000
    Assert-True (-not $v.Complete) "a truncated route must not be admissible"
}

Test-Case "verdict rejects a nonzero exit code" {
    $v = Get-OracleVerdict -ReachedExit $true -TimedOut $false -ExitCode 1 `
             -CaptureCount 18 -RequestedVblank 44000 -ObservedVblank 44000
    Assert-True (-not $v.Complete) "a crashed run must not be admissible"
}

Test-Case "verdict rejects a run that captured nothing" {
    $v = Get-OracleVerdict -ReachedExit $true -TimedOut $false -ExitCode 0 `
             -CaptureCount 0 -RequestedVblank 44000 -ObservedVblank 44000
    Assert-True (-not $v.Complete) "an oracle with no frames proves nothing"
}

Test-Case "verdict rejects an unreadable exit code" {
    $v = Get-OracleVerdict -ReachedExit $true -TimedOut $false -ExitCode $null `
             -CaptureCount 18 -RequestedVblank 44000 -ObservedVblank 44000
    Assert-True (-not $v.Complete) "an unverifiable exit must not be read as success"
}

# --- state-qualified routes (issue #64) ---------------------------------------------
# The defect: seven replays of one pad script from one restored save baseline reached two
# different menu depths, and both the divergent runs reported complete=true. "Reached
# vblank N" is not "reached the intended screen", so the verdict has to read the reached
# state, and a run that did not reach it must be inadmissible rather than merely odd.
Test-Case "verdict rejects a run whose route reached the wrong state" {
    $v = Get-OracleVerdict -ReachedExit $true -TimedOut $false -ExitCode 86 `
             -CaptureCount 18 -RequestedVblank 44000 -ObservedVblank 44000 `
             -RouteKind "failed" -RouteFailReason "line 12: EXPECT EXHIBITION_SETUP at vblank 9600, but the screen is SINGLE_PLAYER_MENU d=2"
    Assert-True (-not $v.Complete) "a route that reached the wrong screen must not be admissible"
    Assert-True (($v.Reasons -join ' ') -match "expected state") "reason should name the reached-state failure"
    Assert-True (($v.Reasons -join ' ') -match "SINGLE_PLAYER_MENU") "reason should carry the runtime's diagnosis"
}

Test-Case "verdict rejects a route program that never completed its steps" {
    $v = Get-OracleVerdict -ReachedExit $true -TimedOut $false -ExitCode 0 `
             -CaptureCount 18 -RequestedVblank 44000 -ObservedVblank 44000 `
             -RouteKind "incomplete"
    Assert-True (-not $v.Complete) "a route that never finished its program is not the route it claims"
}

Test-Case "verdict accepts a completed state-qualified route" {
    $v = Get-OracleVerdict -ReachedExit $true -TimedOut $false -ExitCode 0 `
             -CaptureCount 18 -RequestedVblank 44000 -ObservedVblank 44000 -RouteKind "ok"
    Assert-True ($v.Complete) "a completed route rejected: $($v.Reasons -join '; ')"
}

Test-Case "verdict of a legacy pad script is unchanged" {
    $v = Get-OracleVerdict -ReachedExit $true -TimedOut $false -ExitCode 0 `
             -CaptureCount 18 -RequestedVblank 44000 -ObservedVblank 44000 -RouteKind "none"
    Assert-True ($v.Complete) "an unqualified route is unproven, not failed"
}

$routeTmp = Join-Path ([System.IO.Path]::GetTempPath()) ("routeout_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $routeTmp -Force | Out-Null
try {
    Test-Case "route outcome records every reached checkpoint in order" {
        $log = Join-Path $routeTmp "ok.log"
        @(
            "ROUTE: program loaded from C:\r.pad (9 steps, 4 checkpoints, grid 12x8, sample_every=20, tolerance=12)",
            "ROUTE: reached MAIN_MENU at vblank 8220 (step 0, d=3)",
            "ROUTE: reached SINGLE_PLAYER_MENU at vblank 8760 (step 2, d=1)",
            "ROUTE: confirmed EXHIBITION_SETUP at vblank 9640 (step 5, d=2)",
            "ROUTE_OK: 9 steps completed by vblank 44100"
        ) | Set-Content -LiteralPath $log
        $o = Read-RouteOutcome -StderrPath $log
        Assert-True ($o.Kind -eq "ok") "a completed program should read as ok, got $($o.Kind)"
        Assert-True ($o.Checkpoints.Count -eq 3) "expected 3 checkpoints, got $($o.Checkpoints.Count)"
        Assert-True ($o.Checkpoints[0] -eq "MAIN_MENU@8220") "first checkpoint should carry its vblank"
    }

    Test-Case "route outcome reports a failed assertion with its reason" {
        $log = Join-Path $routeTmp "fail.log"
        @(
            "ROUTE: program loaded from C:\r.pad (9 steps, 4 checkpoints, grid 12x8, sample_every=20, tolerance=12)",
            "ROUTE: reached MAIN_MENU at vblank 8220 (step 0, d=3)",
            "ROUTE_FAIL: line 12: EXPECT EXHIBITION_SETUP at vblank 9600, but the screen is SINGLE_PLAYER_MENU d=2 (EXHIBITION_SETUP d=41, match needs d<=12)"
        ) | Set-Content -LiteralPath $log
        $o = Read-RouteOutcome -StderrPath $log
        Assert-True ($o.Kind -eq "failed") "a failed route should read as failed, got $($o.Kind)"
        Assert-True ($o.FailReason -match "SINGLE_PLAYER_MENU") "the reason should be preserved verbatim"
    }

    Test-Case "route outcome distinguishes a program that stopped mid-route" {
        $log = Join-Path $routeTmp "partial.log"
        @(
            "ROUTE: program loaded from C:\r.pad (9 steps, 4 checkpoints, grid 12x8, sample_every=20, tolerance=12)",
            "ROUTE: reached MAIN_MENU at vblank 8220 (step 0, d=3)"
        ) | Set-Content -LiteralPath $log
        $o = Read-RouteOutcome -StderrPath $log
        Assert-True ($o.Kind -eq "incomplete") "a program with no ROUTE_OK is incomplete, got $($o.Kind)"
    }

    Test-Case "route outcome of a legacy pad script is none" {
        $log = Join-Path $routeTmp "legacy.log"
        @("BOOT_EVENT phase=display_flip vcount=180 buffer=0x04000000 stride=512 format=3",
          "BOOT_EVENT phase=exit_at_vblank vblanks=44000 (SR_EXIT_AT_VBLANK=44000)") |
            Set-Content -LiteralPath $log
        $o = Read-RouteOutcome -StderrPath $log
        Assert-True ($o.Kind -eq "none") "a run with no route narration is unqualified, got $($o.Kind)"
        Assert-True ($o.Checkpoints.Count -eq 0) "an unqualified run reaches no recorded checkpoints"
    }

    Test-Case "route outcome reports a refused route file" {
        $log = Join-Path $routeTmp "parse.log"
        @("ROUTE_PARSE: C:\r.pad:7: no CHECKPOINT defines 'EXHIBITION_SETUP'",
          "ROUTE_FAIL: route file 'C:\r.pad' names undefined checkpoints") |
            Set-Content -LiteralPath $log
        $o = Read-RouteOutcome -StderrPath $log
        Assert-True ($o.Kind -eq "failed") "an unusable route file must fail the run, got $($o.Kind)"
    }
} finally {
    Remove-Item -LiteralPath $routeTmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:Failures -gt 0) {
    Write-Host "$($script:Failures) failure(s)"
    exit 1
}
Write-Host "all visual-oracle support tests passed"
exit 0
