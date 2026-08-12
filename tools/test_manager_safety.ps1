# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Hermetic behavioral tests for the #183 manager safety primitives.
.DESCRIPTION
    Driven by tools/test_manager_safety.py inside the standard Python suite. Uses
    temporary directories, junctions and mock process identities only -- never real user
    data and never real process termination.

    Covers: canonical containment, OracleName-style component grammar, reparse/junction
    escape refusal, oracle archive reset containment, transactional save-base restore
    (overlap, empty/unmanaged/tampered baselines, failpoints, rollback, GAMEDATA),
    PID-reuse/identity-verdict logic, build-PID ledger handling, unknown-exit build
    truth, timeout parsing, and manager invocation from an unrelated CWD.

    Prints one "ok <name>" or "FAIL <name>: <reason>" line per test; exits nonzero on
    any failure.
#>

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "hst_safety.ps1")
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
function Assert-Throws {
    param([scriptblock]$Body, [string]$Message)
    try {
        & $Body | Out-Null
        throw "expected an exception: $Message"
    } catch {
        if ($_.Exception.Message -like "expected an exception*") { throw }
    }
}

$tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("hst_safety_test_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmpRoot -Force | Out-Null
$onWindows = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
    [System.Runtime.InteropServices.OSPlatform]::Windows)

try {
    # --- Canonical containment ------------------------------------------------------
    Test-Case "containment accepts descendants and the root itself" {
        $root = Join-Path $tmpRoot "cont/logs"
        New-Item -ItemType Directory -Path (Join-Path $root "oracle_x") -Force | Out-Null
        Assert-True (Test-PathContained -Path (Join-Path $root "oracle_x") -Root $root) "descendant rejected"
        Assert-True (Test-PathContained -Path $root -Root $root) "root itself rejected"
        Assert-True (Test-PathContained -Path (Join-Path $root "not-yet-created") -Root $root) "non-existent descendant rejected"
    }

    Test-Case "containment rejects siblings and traversal" {
        $root = Join-Path $tmpRoot "cont2/logs"
        New-Item -ItemType Directory -Path (Join-Path $tmpRoot "cont2") -Force | Out-Null
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $tmpRoot "outside") -Force | Out-Null
        Assert-True (-not (Test-PathContained -Path (Join-Path $tmpRoot "outside") -Root $root)) "sibling accepted"
        Assert-True (-not (Test-PathContained -Path (Join-Path $tmpRoot "cont2") -Root $root)) "parent accepted"
        Assert-True (-not (Test-PathContained -Path (Join-Path $root "..\..\outside") -Root $root)) "traversal accepted"
        Assert-True (-not (Test-PathContained -Path "" -Root $root)) "empty path accepted"
    }

    # --- OracleName component grammar ------------------------------------------------
    Test-Case "component grammar accepts identifiers and rejects path forms" {
        foreach ($ok in @("run1", "Run_1", "a-b.c", "deep_return_run1", "x")) {
            Assert-True (Test-SafeComponentName -Name $ok) "valid name '$ok' rejected"
        }
        foreach ($bad in @("a/b", "a\b", "..", ".", ".hidden", "a..b", "C:/x", "C:\x", "C:x", "a b", "a`t")) {
            Assert-True (-not (Test-SafeComponentName -Name $bad)) "invalid name '$bad' accepted"
        }
        Assert-True (-not (Test-SafeComponentName -Name ("y" * 65))) "overlong name accepted"
    }

    # --- Reparse/junction escape -----------------------------------------------------
    if ($onWindows) {
        Test-Case "junction escape is refused and the outside target survives" {
            $root = Join-Path $tmpRoot "junc/logs"
            New-Item -ItemType Directory -Path $root -Force | Out-Null
            $outside = Join-Path $tmpRoot "junc/outside"
            New-Item -ItemType Directory -Path $outside -Force | Out-Null
            Set-Content -LiteralPath (Join-Path $outside "marker.txt") -Value "x"
            $junction = Join-Path $root "evil"
            New-Item -ItemType Junction -Path $junction -Target $outside -Force | Out-Null
            Assert-True (-not (Test-PathContained -Path $junction -Root $root)) "junction accepted as contained"
            Assert-Throws { Remove-SafeDirectory -Path $junction -Root $root } "recursive delete through junction"
            Assert-True (Test-Path -LiteralPath (Join-Path $outside "marker.txt")) "outside marker destroyed"
        }

        Test-Case "a reparse descendant inside a tree blocks the whole recursive delete" {
            $root = Join-Path $tmpRoot "junc2/logs"
            New-Item -ItemType Directory -Path (Join-Path $root "oracle_x") -Force | Out-Null
            $outside = Join-Path $tmpRoot "junc2/outside"
            New-Item -ItemType Directory -Path $outside -Force | Out-Null
            Set-Content -LiteralPath (Join-Path $outside "marker.txt") -Value "x"
            New-Item -ItemType Junction -Path (Join-Path $root "oracle_x/sub") -Target $outside -Force | Out-Null
            Set-Content -LiteralPath (Join-Path $root "oracle_x/snap_0.ppm") -Value "evidence"
            Assert-Throws { Remove-SafeDirectory -Path (Join-Path $root "oracle_x") -Root $root } "delete with escaping descendant"
            Assert-True (Test-Path -LiteralPath (Join-Path $root "oracle_x/snap_0.ppm")) "evidence deleted despite refusal"
            Assert-True (Test-Path -LiteralPath (Join-Path $outside "marker.txt")) "outside marker destroyed"
        }
    } else {
        Test-Case "symlink escape is refused and the outside target survives (POSIX)" {
            $root = Join-Path $tmpRoot "junc3/logs"
            New-Item -ItemType Directory -Path $root -Force | Out-Null
            $outside = Join-Path $tmpRoot "junc3/outside"
            New-Item -ItemType Directory -Path $outside -Force | Out-Null
            Set-Content -LiteralPath (Join-Path $outside "marker.txt") -Value "x"
            $link = Join-Path $root "evil"
            New-Item -ItemType SymbolicLink -Path $link -Target $outside -Force | Out-Null
            Assert-True (-not (Test-PathContained -Path $link -Root $root)) "symlink accepted as contained"
            Assert-Throws { Remove-SafeDirectory -Path $link -Root $root } "recursive delete through symlink"
            Assert-True (Test-Path -LiteralPath (Join-Path $outside "marker.txt")) "outside marker destroyed"
        }
    }

    # --- Reset-OracleArchive containment --------------------------------------------
    Test-Case "archive reset rejects a traversal path outright" {
        $root = Join-Path $tmpRoot "arch/logs"
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        Assert-True (-not (Reset-OracleArchive -Path (Join-Path $root "..\outside_arch") -AllowedRoot $root)) "traversal accepted"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $tmpRoot "outside_arch"))) "traversal dir was created"
    }

    Test-Case "archive reset overwrite refuses a reparse-escape archive" {
        $root = Join-Path $tmpRoot "arch2/logs"
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $outside = Join-Path $tmpRoot "arch2/outside"
        New-Item -ItemType Directory -Path $outside -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $outside "marker.txt") -Value "x"
        if ($isWindows) {
            New-Item -ItemType Junction -Path (Join-Path $root "oracle_evil") -Target $outside -Force | Out-Null
        } else {
            New-Item -ItemType SymbolicLink -Path (Join-Path $root "oracle_evil") -Target $outside -Force | Out-Null
        }
        $ok = Reset-OracleArchive -Path (Join-Path $root "oracle_evil") -AllowedRoot $root -Overwrite
        Assert-True (-not $ok) "overwrite through a reparse point accepted"
        Assert-True (Test-Path -LiteralPath (Join-Path $outside "marker.txt")) "outside marker destroyed"
    }

    # --- Sync-SaveBase: capture/restore with manifest --------------------------------
    Test-Case "save base captures the live save with a manifest" {
        $root = Join-Path $tmpRoot "sb1/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "run0"
        $base = Join-Path $tmpRoot "sb1/base"
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($null -ne $r) "capture failed"
        Assert-True ($r.Action -eq "captured") "expected capture, got $($r.Action)"
        Assert-True ((Get-Content -LiteralPath (Join-Path $base "UCUS98701/DATA0.BIN")) -eq "run0") "content not captured"
        $mf = Join-Path $base ".hst_savebase_manifest.json"
        Assert-True (Test-Path -LiteralPath $mf) "no baseline manifest written"
        $m = Get-Content -LiteralPath $mf -Raw | ConvertFrom-Json
        Assert-True ($m.format -eq "hst-savebase-manifest/v1") "unexpected manifest format"
        Assert-True ($m.file_count -eq 1) "manifest file count wrong"
    }

    Test-Case "save base restores the baseline over a mutated save" {
        $root = Join-Path $tmpRoot "sb2/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "pristine"
        $base = Join-Path $tmpRoot "sb2/base"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "mutated-by-run"
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/EXTRA.BIN") -Value "stray"
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($null -ne $r) "restore failed"
        Assert-True ($r.Action -eq "restored") "expected restore, got $($r.Action)"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN")) -eq "pristine") "save not rolled back"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $root "UCUS98701/EXTRA.BIN"))) "run-added file survived"
        $leftovers = @(Get-ChildItem -LiteralPath (Join-Path $tmpRoot "sb2/PSP") -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like ".hst_savebase_*" })
        Assert-True ($leftovers.Count -eq 0) "stage/rollback leftovers survived: $($leftovers.Name -join ',')"
    }

    Test-Case "save base never touches the GAMEDATA install" {
        $root = Join-Path $tmpRoot "sb3/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701GAMEDATA") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "save"
        Set-Content -LiteralPath (Join-Path $root "UCUS98701GAMEDATA/BIG.BIN") -Value "install"
        $base = Join-Path $tmpRoot "sb3/base"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $base "UCUS98701GAMEDATA"))) "GAMEDATA copied into baseline"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701GAMEDATA/BIG.BIN")) -eq "install") "GAMEDATA lost in restore"
    }

    # --- Sync-SaveBase: containment and overlap rejection ----------------------------
    Test-Case "save base rejects a baseline outside the approved root" {
        $root = Join-Path $tmpRoot "sb4/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $outside = Join-Path ([IO.Path]::GetTempPath()) ("hst_outside_base_" + [guid]::NewGuid().ToString("N"))
        $r = Sync-SaveBase -BasePath $outside -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($null -eq $r) "outside baseline accepted"
        Assert-True (-not (Test-Path -LiteralPath $outside)) "outside baseline directory created"
    }

    Test-Case "save base rejects baseline == live root" {
        $root = Join-Path $tmpRoot "sb5/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        Assert-True ($null -eq (Sync-SaveBase -BasePath $root -SaveRoot $root -ApprovedRoot $tmpRoot)) "identical roots accepted"
    }

    Test-Case "save base rejects overlap in both directions" {
        $root = Join-Path $tmpRoot "sb6/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $liveChild = Join-Path $root "UCUS98701"
        New-Item -ItemType Directory -Path $liveChild -Force | Out-Null
        # live is a descendant of baseline
        Assert-True ($null -eq (Sync-SaveBase -BasePath $root -SaveRoot $liveChild -ApprovedRoot $tmpRoot)) "live-inside-baseline accepted"
        # baseline is a descendant of live
        $baseChild = Join-Path $root "base"
        Assert-True ($null -eq (Sync-SaveBase -BasePath $baseChild -SaveRoot $root -ApprovedRoot $tmpRoot)) "baseline-inside-live accepted"
    }

    Test-Case "save base refuses an empty baseline" {
        $root = Join-Path $tmpRoot "sb7/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "live"
        $base = Join-Path $tmpRoot "sb7/base"
        New-Item -ItemType Directory -Path $base -Force | Out-Null
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($null -eq $r) "empty baseline accepted for restore"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN")) -eq "live") "live save was destroyed"
    }

    Test-Case "save base refuses an unmanaged (manifest-less) baseline" {
        $root = Join-Path $tmpRoot "sb8/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "live"
        $base = Join-Path $tmpRoot "sb8/base"
        New-Item -ItemType Directory -Path (Join-Path $base "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $base "UCUS98701/DATA0.BIN") -Value "arbitrary"
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($null -eq $r) "unmanaged directory accepted as baseline"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN")) -eq "live") "live save was destroyed"
    }

    Test-Case "save base refuses a tampered baseline (manifest mismatch)" {
        $root = Join-Path $tmpRoot "sb9/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "live"
        $base = Join-Path $tmpRoot "sb9/base"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        # Tamper with the baseline content after capture.
        Set-Content -LiteralPath (Join-Path $base "UCUS98701/DATA0.BIN") -Value "tampered"
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($null -eq $r) "tampered baseline accepted"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN")) -eq "live") "live save was destroyed"
    }

    # --- Sync-SaveBase failpoints (documented test seam) ------------------------------
    Test-Case "failed staged copy aborts with the live save untouched" {
        $root = Join-Path $tmpRoot "sb10/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "live"
        $base = Join-Path $tmpRoot "sb10/base"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot -Failpoint "stage-copy"
        Assert-True ($null -eq $r) "stage-copy failure not reported"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN")) -eq "live") "live save was destroyed"
    }

    Test-Case "failed swap rolls the live save back" {
        $root = Join-Path $tmpRoot "sb11/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "live"
        $base = Join-Path $tmpRoot "sb11/base"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot -Failpoint "swap-rename"
        Assert-True ($null -eq $r) "swap failure not reported"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN")) -eq "live") "live save lost after rollback"
    }

    Test-Case "post-swap verification failure rolls the live save back" {
        $root = Join-Path $tmpRoot "sb12/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "live"
        $base = Join-Path $tmpRoot "sb12/base"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot -Failpoint "verify"
        Assert-True ($null -eq $r) "verify failure not reported"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN")) -eq "live") "live save lost after rollback"
    }

    Test-Case "orphaned swap dir from an interrupted restore fails closed" {
        # Simulate a hard kill inside the swap window: an orphaned rollback sibling that
        # a caught-exception path would have cleaned up. The next restore must refuse to
        # run rather than treat the possibly-partial live root as a completed state.
        $root = Join-Path $tmpRoot "sb13/PSP/SAVEDATA"
        New-Item -ItemType Directory -Path (Join-Path $root "UCUS98701") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN") -Value "live"
        $base = Join-Path $tmpRoot "sb13/base"
        [void](Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot)
        # Interrupted restore leaves its rollback shelter behind.
        New-Item -ItemType Directory -Path (Join-Path $tmpRoot "sb13/PSP/.hst_savebase_rollback_deadbeef") -Force | Out-Null
        $r = Sync-SaveBase -BasePath $base -SaveRoot $root -ApprovedRoot $tmpRoot
        Assert-True ($null -eq $r) "restore proceeded despite an orphaned swap directory"
        Assert-True ((Get-Content -LiteralPath (Join-Path $root "UCUS98701/DATA0.BIN")) -eq "live") "live save was destroyed"
    }

    # --- Process identity verdicts (mocked probes, no real processes) ----------------
    $nowTicks = [DateTime]::UtcNow.Ticks
    $recordFull = [pscustomobject]@{
        pid = 4242; creation_ticks = $nowTicks
        exe = "C:/tools/make.exe"; cmd = "make all"
    }
    $recordNoCmd = [pscustomobject]@{ pid = 4243; creation_ticks = $nowTicks; exe = "C:/tools/make.exe"; cmd = $null }
    $recordNoExe = [pscustomobject]@{ pid = 4244; creation_ticks = $nowTicks; exe = $null; cmd = "make all" }
    $recordNoCreation = [pscustomobject]@{ pid = 4245; creation_ticks = $null; exe = "C:/tools/make.exe"; cmd = "make all" }

    $probeFull = { param($Id) [pscustomobject]@{ pid = $Id; creation_ticks = $nowTicks; exe = "C:/tools/make.exe"; cmd = "make all" } }
    $probeGone = { param($Id) $null }
    $probeOtherProcess = { param($Id) [pscustomobject]@{ pid = $Id; creation_ticks = $nowTicks + 1000000; exe = "C:/tools/make.exe"; cmd = "make all" } }
    $probeOtherExe = { param($Id) [pscustomobject]@{ pid = $Id; creation_ticks = $nowTicks; exe = "C:/other/make.exe"; cmd = "make all" } }
    $probeOtherCmd = { param($Id) [pscustomobject]@{ pid = $Id; creation_ticks = $nowTicks; exe = "C:/tools/make.exe"; cmd = "make clean" } }

    Test-Case "process identity: full match is the only valid verdict" {
        Assert-True ((Test-ProcessRecordStillValid -Record $recordFull -Probe $probeFull) -eq "valid") "full match not valid"
    }

    Test-Case "process identity: PID reuse (different creation time) is a mismatch" {
        Assert-True ((Test-ProcessRecordStillValid -Record $recordFull -Probe $probeOtherProcess) -eq "mismatch") "reused PID accepted"
    }

    Test-Case "process identity: same exe name at a different path is a mismatch" {
        Assert-True ((Test-ProcessRecordStillValid -Record $recordFull -Probe $probeOtherExe) -eq "mismatch") "same-name/different-path accepted"
    }

    Test-Case "process identity: same exe, different command line is a mismatch" {
        Assert-True ((Test-ProcessRecordStillValid -Record $recordFull -Probe $probeOtherCmd) -eq "mismatch") "different workspace accepted"
    }

    Test-Case "process identity: missing fields fail safe as unknown" {
        Assert-True ((Test-ProcessRecordStillValid -Record $recordNoCmd -Probe $probeFull) -eq "unknown") "missing cmd accepted"
        Assert-True ((Test-ProcessRecordStillValid -Record $recordNoExe -Probe $probeFull) -eq "unknown") "missing exe accepted"
        Assert-True ((Test-ProcessRecordStillValid -Record $recordFull -Probe { param($Id) [pscustomobject]@{ pid = $Id; creation_ticks = $nowTicks; exe = $null; cmd = $null } }) -eq "unknown") "inaccessible fields accepted"
        Assert-True ((Test-ProcessRecordStillValid -Record $recordNoCreation -Probe $probeFull) -eq "unknown") "missing creation accepted"
    }

    Test-Case "process identity: already-exited PID is gone" {
        Assert-True ((Test-ProcessRecordStillValid -Record $recordFull -Probe $probeGone) -eq "gone") "exited PID not gone"
    }

    # --- Build-PID ledger handling ----------------------------------------------------
    Test-Case "stale cleanup drops gone records and kills nothing" {
        $pidFile = Join-Path $tmpRoot "pids1.jsonl"
        Set-Content -LiteralPath $pidFile -Value (($recordFull | ConvertTo-Json -Compress) + "`n") -Encoding utf8
        $result = Invoke-StaleBuildCleanup -PidFile $pidFile -Probe $probeGone -DryRun
        Assert-True ($result.Killed.Count -eq 0) "killed a gone record"
        Assert-True (-not (Test-Path -LiteralPath $pidFile)) "gone record not dropped from ledger"
    }

    Test-Case "stale cleanup keeps unverifiable records and kills nothing" {
        $pidFile = Join-Path $tmpRoot "pids2.jsonl"
        Set-Content -LiteralPath $pidFile -Value (($recordNoCmd | ConvertTo-Json -Compress) + "`n") -Encoding utf8
        $result = Invoke-StaleBuildCleanup -PidFile $pidFile -Probe $probeFull -DryRun
        Assert-True ($result.Killed.Count -eq 0) "killed an unverifiable record"
        Assert-True (Test-Path -LiteralPath $pidFile) "unverifiable record was dropped"
        Assert-True ($result.Kept.Count -eq 1) "kept count wrong"
    }

    Test-Case "stale cleanup drops reused-PID records and kills nothing" {
        $pidFile = Join-Path $tmpRoot "pids3.jsonl"
        Set-Content -LiteralPath $pidFile -Value (($recordFull | ConvertTo-Json -Compress) + "`n") -Encoding utf8
        $result = Invoke-StaleBuildCleanup -PidFile $pidFile -Probe $probeOtherProcess -DryRun
        Assert-True ($result.Killed.Count -eq 0) "killed a reused PID"
        Assert-True (-not (Test-Path -LiteralPath $pidFile)) "reused-PID record not dropped"
    }

    Test-Case "legacy bare-PID lines are discarded, never killed" {
        $pidFile = Join-Path $tmpRoot "pids4.jsonl"
        Set-Content -LiteralPath $pidFile -Value ("12345`n") -Encoding utf8
        $result = Invoke-StaleBuildCleanup -PidFile $pidFile -Probe $probeFull -DryRun
        Assert-True ($result.Malformed.Count -eq 1) "legacy line not flagged"
        Assert-True ($result.Killed.Count -eq 0) "killed on a legacy PID"
        Assert-True (-not (Test-Path -LiteralPath $pidFile)) "legacy record not removed"
    }

    Test-Case "missing pid file is a no-op" {
        $result = Invoke-StaleBuildCleanup -PidFile (Join-Path $tmpRoot "no-such-pids") -DryRun
        Assert-True ($result.Killed.Count -eq 0 -and $result.Kept.Count -eq 0) "missing file was not a no-op"
    }

    # --- Build truth: unknown exit is not success -------------------------------------
    Test-Case "known zero exit is success; nonzero and unknown are not" {
        # Start-Process -PassThru returns before the child exits; wait first so the exit
        # code is actually readable -- exactly the manager's post-build state.
        $p0 = Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", "exit 0") -PassThru -WindowStyle Hidden
        [void]$p0.WaitForExit()
        $info0 = Get-KnownExitCode -Process $p0
        Assert-True ($info0.Known -and $info0.ExitCode -eq 0) "clean exit not known-zero"
        $p3 = Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", "exit 3") -PassThru -WindowStyle Hidden
        [void]$p3.WaitForExit()
        $info3 = Get-KnownExitCode -Process $p3
        Assert-True ($info3.Known -and $info3.ExitCode -eq 3) "exit 3 not captured"
        # A process object whose exit code reads back null must be reported unknown.
        $unknown = [pscustomobject]@{ HasExited = $true; ExitCode = $null }
        $infoU = Get-KnownExitCode -Process $unknown
        Assert-True (-not $infoU.Known -and $null -eq $infoU.ExitCode) "null exit treated as known"
    }

    # --- Timeout parsing ----------------------------------------------------------------
    Test-Case "timeout parse accepts non-negative integers and rejects the rest" {
        Assert-True ((ConvertTo-SafeTimeoutSeconds -Text "0") -eq 0) "0 rejected"
        Assert-True ((ConvertTo-SafeTimeoutSeconds -Text "120") -eq 120) "120 rejected"
        Assert-True ((ConvertTo-SafeTimeoutSeconds -Text " 30 ") -eq 30) "padded 30 rejected"
        foreach ($bad in @("-5", "abc", "", "   ", "99999999999999", "1.5", "0x10")) {
            Assert-True ($null -eq (ConvertTo-SafeTimeoutSeconds -Text $bad)) "invalid '$bad' accepted"
        }
    }

    # --- Repository-root identity --------------------------------------------------------
    Test-Case "workspace root validation fails closed on missing anchors" {
        $fake = Join-Path $tmpRoot "ws1"
        New-Item -ItemType Directory -Path $fake -Force | Out-Null
        Assert-Throws { Assert-HstWorkspaceRoot -Root $fake } "anchor-less dir accepted"
        Set-Content -LiteralPath (Join-Path $fake "Makefile") -Value ""
        Set-Content -LiteralPath (Join-Path $fake "AGENTS.md") -Value ""
        New-Item -ItemType Directory -Path (Join-Path $fake "src/rt") -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $fake "tools") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $fake "src/rt/recomp.c") -Value ""
        Set-Content -LiteralPath (Join-Path $fake "tools/codegen.py") -Value ""
        $resolved = Assert-HstWorkspaceRoot -Root $fake
        Assert-True ($resolved -eq [IO.Path]::GetFullPath($fake)) "root not canonicalized"
        # A sibling path must resolve the same regardless of caller CWD.
        $prev = Get-Location
        try {
            Set-Location -LiteralPath $tmpRoot
            Assert-True ((Assert-HstWorkspaceRoot -Root $fake) -eq [IO.Path]::GetFullPath($fake)) "root differs by CWD"
        } finally {
            Set-Location -LiteralPath $prev.Path
        }
    }
} finally {
    Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
}

# --- Full-process CWD hermeticity -------------------------------------------------
# Run the REAL manager from an unrelated CWD against a copy of itself in a fake
# workspace; it must anchor to the fake workspace (its own script location) and leave
# the caller's CWD untouched.
$cwdTestRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("hst_cwd_test_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $cwdTestRoot -Force | Out-Null
try {
    $fakeRepo = Join-Path $cwdTestRoot "repo"
    $fakeTools = Join-Path $fakeRepo "tools"
    $unrelated = Join-Path $cwdTestRoot "unrelated"
    New-Item -ItemType Directory -Path $fakeTools -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $unrelated "logs") -Force | Out-Null
    $managerSrc = Join-Path $PSScriptRoot ".."
    Copy-Item -LiteralPath (Join-Path $managerSrc "hst_manager.ps1") -Destination $fakeRepo -Force
    foreach ($helper in @("hst_safety.ps1", "hst_run_support.ps1", "vulkan_sdk.ps1")) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $helper) -Destination $fakeTools -Force
    }
    foreach ($anchor in @("Makefile", "AGENTS.md")) {
        Set-Content -LiteralPath (Join-Path $fakeRepo $anchor) -Value ""
    }
    New-Item -ItemType Directory -Path (Join-Path $fakeRepo "src/rt") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $fakeRepo "src/rt/recomp.c") -Value ""
    Set-Content -LiteralPath (Join-Path $fakeRepo "tools/codegen.py") -Value ""
    $fakeSdk = Join-Path $fakeRepo "fake-sdk"
    New-Item -ItemType Directory -Path (Join-Path $fakeSdk "Include/vulkan") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $fakeSdk "Lib") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $fakeSdk "Include/vulkan/vulkan.h") -Value ""
    Set-Content -LiteralPath (Join-Path $fakeSdk "Lib/vulkan-1.lib") -Value ""

    # Pre-existing "logs" in both the fake repo (target of -Action Clean) and the
    # unrelated CWD (must never be touched).
    New-Item -ItemType Directory -Path (Join-Path $fakeRepo "logs") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $fakeRepo "logs/stdout_run.log") -Value "repo-log"
    Set-Content -LiteralPath (Join-Path $unrelated "logs/stdout_run.log") -Value "cwd-log"
    Set-Content -LiteralPath (Join-Path $unrelated "stdout_run.log") -Value "cwd-marker"

    Test-Case "manager invoked from an unrelated CWD anchors to its own workspace" {
        $pwshExe = (Get-Process -Id $PID).Path
        $prev = Get-Location
        try {
            Set-Location -LiteralPath $unrelated
            & $pwshExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $fakeRepo "hst_manager.ps1") `
                -Action Clean -VulkanSdk $fakeSdk | Out-Null
            $exitCode = [int]$LASTEXITCODE
            Assert-True ($exitCode -eq 0) "manager exited $exitCode"
            Assert-True (-not (Test-Path -LiteralPath (Join-Path $fakeRepo "logs/stdout_run.log"))) "repo log was not cleaned"
            Assert-True (Test-Path -LiteralPath (Join-Path $unrelated "logs/stdout_run.log")) "unrelated CWD log was deleted"
            Assert-True (Test-Path -LiteralPath (Join-Path $unrelated "stdout_run.log")) "unrelated CWD marker was deleted"
        } finally {
            Set-Location -LiteralPath $prev.Path
        }
    }

    Test-Case "manager fails closed when copied into an anchor-less tree" {
        $bare = Join-Path $cwdTestRoot "bare"
        New-Item -ItemType Directory -Path (Join-Path $bare "tools") -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $managerSrc "hst_manager.ps1") -Destination $bare -Force
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot "hst_safety.ps1") -Destination (Join-Path $bare "tools") -Force
        $pwshExe = (Get-Process -Id $PID).Path
        $out = & $pwshExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $bare "hst_manager.ps1") `
            -Action Clean 2>&1 | Out-String
        $exitCode = [int]$LASTEXITCODE
        Assert-True ($exitCode -ne 0) "anchor-less manager exited 0"
        Assert-True ($out -match "workspace root") "anchor error not reported"
    }
} finally {
    Remove-Item -LiteralPath $cwdTestRoot -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:Failures -gt 0) {
    Write-Host "$($script:Failures) failure(s)"
    exit 1
}
Write-Host "all manager safety tests passed"
exit 0
