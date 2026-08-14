# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Run/oracle support helpers shared by hst_manager.ps1 and its regression tests.
.DESCRIPTION
    These helpers were extracted from hst_manager.ps1 so they can be exercised
    without a game build. They are the parts of a visual-oracle run whose failure
    modes are silent and expensive:

      Wait-ProcessOrKill    a bounded wait that returns when the process exits, instead
                            of sleeping the whole deadline.
      Reset-OracleArchive   refuse to write a new evidence set on top of an old one,
                            and refuse to clear anything outside the approved logs root.
      Sync-SaveBase         transactional save-state hold-still with manifest identity,
                            containment and rollback.
      Get-OracleVerdict     decide whether a finished run's captures may be trusted.

    Dot-source this file; it defines functions and returns nothing.
#>

. (Join-Path $PSScriptRoot "hst_safety.ps1")

# Wait-ProcessOrKill: bounded, process-aware wait.
#
# The original runner did `Start-Sleep -Seconds $RunDuration` unconditionally, so a run
# that self-terminated at its requested vblank still held the shell for the entire
# backstop. With a backstop sized for a 44,000-vblank route that is over an hour of dead
# wall-clock after a ~25 minute replay, which made a generous backstop actively costly and
# pushed the caller toward guessing the deadline tightly -- the exact guess that silently
# truncates routes.
#
# Returns a result object rather than writing to the pipeline implicitly, so callers can
# record the real exit code and elapsed time in an evidence manifest.
function Wait-ProcessOrKill {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        # Deadline in seconds. 0 or less means "wait indefinitely" (interactive runs).
        [int]$TimeoutSeconds = 0
    )

    # Touching .Handle caches the native process handle on the object. Without this, a
    # process launched by Start-Process -PassThru WITH redirected streams reports HasExited
    # correctly but returns an EMPTY ExitCode -- so a crashed run would be recorded with no
    # exit code and sail through the completeness verdict, which only rejects a nonzero one.
    # Observed on this exact configuration; the regression test below redirects for that reason.
    try { $null = $Process.Handle } catch { }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $killed = $false

    if ($TimeoutSeconds -gt 0) {
        # Milliseconds are an int in the BCL overload; clamp rather than overflow.
        $ms = [double]$TimeoutSeconds * 1000.0
        if ($ms -gt [int]::MaxValue) { $ms = [double][int]::MaxValue }
        if (-not $Process.WaitForExit([int]$ms)) {
            $timedOut = $true
            try {
                $Process | Stop-Process -Force -ErrorAction Stop
                $killed = $true
            } catch {
                # Already gone between the timeout and the kill: not a failure.
            }
            # Give the kill a moment to be reflected in HasExited/ExitCode.
            [void]$Process.WaitForExit(5000)
        }
    } else {
        $Process.WaitForExit()
    }

    # The parameterless overload also drains any redirected streams. It returns at once
    # when the process is already gone, so this is free on the normal path.
    try { $Process.WaitForExit() } catch { }
    $sw.Stop()

    $exitCode = $null
    try { if ($Process.HasExited) { $exitCode = $Process.ExitCode } } catch { }

    return [pscustomobject]@{
        TimedOut       = $timedOut
        Killed         = $killed
        ExitCode       = $exitCode
        ElapsedSeconds = [math]::Round($sw.Elapsed.TotalSeconds, 2)
    }
}

# Reset-OracleArchive: guarantee an oracle directory holds exactly one run's output.
#
# Snapshot files are numbered per run (snap_0.ppm, snap_1.ppm, ...), so a shorter second
# run into the same directory leaves the tail of the first one in place and the mixed set
# still looks complete. Reject by default -- an accidentally reused name should be loud,
# not silently merged -- and clear only when the caller says so explicitly.
#
# The archive path must be contained in $AllowedRoot (the logs root in the manager); an
# archive name that traverses out of it is rejected before anything is created or
# deleted. The recursive clear also refuses reparse-point escapes (junction/symlink
# descendants whose final target leaves the approved root).
#
# Returns $true when the directory is ready to receive a run, $false when it was rejected.
function Reset-OracleArchive {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [switch]$Overwrite,
        [string]$AllowedRoot = $null
    )

    if ([string]::IsNullOrWhiteSpace($AllowedRoot)) {
        $AllowedRoot = [System.IO.Path]::GetFullPath((Get-Location).Path)
    }
    if (-not (Test-PathContained -Path $Path -Root $AllowedRoot)) {
        Write-Host "[!] Oracle archive path is outside the approved logs root: $Path" -ForegroundColor Red
        Write-Host "    Oracle names are single path components under the logs root." -ForegroundColor Red
        return $false
    }

    if (Test-Path -LiteralPath $Path) {
        $existing = @(Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue)
        if ($existing.Count -gt 0) {
            if (-not $Overwrite) {
                Write-Host "[!] Oracle archive already exists and is not empty: $Path" -ForegroundColor Red
                Write-Host "    It holds $($existing.Count) file(s) from an earlier run. Choose a new" -ForegroundColor Red
                Write-Host "    -OracleName, or pass -OverwriteOracle to discard that evidence set." -ForegroundColor Red
                return $false
            }
            Write-Host "[*] -OverwriteOracle: discarding $($existing.Count) file(s) in $Path" -ForegroundColor Yellow
            try {
                # [void]: Remove-SafeDirectory returns $true and must not pollute the
                # caller's output stream (which is this function's own return value).
                [void](Remove-SafeDirectory -Path $Path -Root $AllowedRoot)
            } catch {
                Write-Host "[!] Refusing to clear oracle archive: $($_.Exception.Message)" -ForegroundColor Red
                return $false
            }
        }
    }
    try {
        New-Item -ItemType Directory -Path $Path -Force -ErrorAction Stop | Out-Null
    } catch {
        Write-Host "[!] Could not create oracle archive directory: $($_.Exception.Message)" -ForegroundColor Red
        return $false
    }
    return $true
}

# ---------------------------------------------------------------------------
# Save-base manifest helpers (privacy-conscious; lives inside the ignored baseline dir)
# ---------------------------------------------------------------------------

function Get-SaveBaseManifestPath {
    param([Parameter(Mandatory = $true)][string]$Root)
    return (Join-Path $Root ".hst_savebase_manifest.json")
}

function New-SaveBaseInventory {
    <#
        Relative inventory of a baseline directory: rel path, size and SHA-256 per file.
        Never includes the manifest itself.
    #>
    param([Parameter(Mandatory = $true)][string]$Root)
    $entries = @()
    foreach ($f in @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force -ErrorAction Stop)) {
        if ($f.FullName -eq (Get-SaveBaseManifestPath $Root)) { continue }
        $hash = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256 -ErrorAction Stop).Hash
        $entries += [pscustomobject]@{
            rel    = [System.IO.Path]::GetRelativePath($Root, $f.FullName)
            size   = $f.Length
            sha256 = $hash
        }
    }
    return $entries
}

function Write-SaveBaseManifest {
    <#
        Record a baseline's identity without publishing save contents or sensitive
        absolute paths: creation time, a non-secret source hash, the relative file
        inventory with hashes, and optional route/build context.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][object[]]$Inventory,
        [Parameter(Mandatory = $true)][string]$SaveRootCanon,
        [string]$RouteContext = "",
        [string]$BuildContext = ""
    )
    $sourceHash = ""
    try {
        $sourceHash = (Get-FileHash -InputStream ([IO.MemoryStream]::new(
            [Text.Encoding]::UTF8.GetBytes($SaveRootCanon))) -Algorithm SHA256).Hash
    } catch { }
    $manifest = [ordered]@{
        format          = "hst-savebase-manifest/v1"
        created_utc     = ([DateTime]::UtcNow).ToString("o")
        managed_by      = "hst_manager.ps1"
        source_identity = [ordered]@{
            save_root_name    = [IO.Path]::GetFileName($SaveRootCanon)
            source_root_sha256 = $sourceHash
        }
        file_count      = $Inventory.Count
        files           = $Inventory
        route_context   = $RouteContext
        build_context   = $BuildContext
    }
    $manifest | ConvertTo-Json -Depth 6 |
        Out-File -FilePath (Get-SaveBaseManifestPath $Root) -Encoding utf8 -ErrorAction Stop
}

function Read-SaveBaseManifest {
    <#
        Returns the parsed manifest, or $null when the baseline is unmanaged (no manifest)
        or the manifest is not the expected format. A baseline without a valid manifest is
        "some directory someone pointed at" and is never restored destructively.
    #>
    param([Parameter(Mandatory = $true)][string]$Root)
    $mf = Get-SaveBaseManifestPath $Root
    if (-not (Test-Path -LiteralPath $mf -PathType Leaf)) { return $null }
    try {
        $m = Get-Content -LiteralPath $mf -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($m.format -ne "hst-savebase-manifest/v1" -or $null -eq $m.file_count) { return $null }
        return $m
    } catch {
        return $null
    }
}

# The one place the managed/unmanaged split is decided. Capture, stage-copy and the swap
# all skip the *GAMEDATA* install; verification has to use the same rule or it reads the
# install as unexplained extra content.
function Test-SaveBaseUnmanagedName {
    param([Parameter(Mandatory = $true)][string]$Name)
    return ($Name -like "*GAMEDATA*")
}

function Compare-SaveBaseInventory {
    <#
        Compare a directory's actual content against a manifest. Returns the list of
        differing relative paths (missing, extra, size or hash mismatch).

        -IncludesUnmanaged marks a root that legitimately holds directories the baseline
        never captures (the live save root always sits beside the ~400 MB *GAMEDATA*
        install). Their files are not extras: the manifest was written to exclude them,
        so counting them as differences fails every restore on a real installed title.
        Baseline and stage roots are compared without it, so they stay strictly verified.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][object]$Manifest,
        [switch]$IncludesUnmanaged
    )
    $actual = @{}
    foreach ($e in (New-SaveBaseInventory -Root $Root)) {
        if ($IncludesUnmanaged) {
            $top = ($e.rel -split '[\\/]')[0]
            if (Test-SaveBaseUnmanagedName -Name $top) { continue }
        }
        $actual[$e.rel] = "$($e.size)|$($e.sha256)"
    }
    $diff = @()
    $manifestRels = New-Object System.Collections.Generic.HashSet[string]
    foreach ($e in @($Manifest.files)) {
        $rel = [string]$e.rel
        [void]$manifestRels.Add($rel)
        if (-not $actual.ContainsKey($rel) -or $actual[$rel] -ne "$($e.size)|$($e.sha256)") {
            $diff += $rel
        }
    }
    foreach ($key in $actual.Keys) {
        if (-not $manifestRels.Contains($key)) { $diff += $key }
    }
    return $diff
}

# Sync-SaveBase: make every oracle run start from byte-identical guest save state.
#
# The game writes a real save ("Finished saving data." during the give-up path), so run N+1
# starts from whatever run N left behind. Observed directly: two replays of the same route
# diverged because the second one hit a first-time tutorial popup the first had cleared, and
# ended in a match instead of at the club. A route replay is only deterministic in its
# INPUTS -- without this the guest state underneath it is not held still, and two runs are
# not two samples of the same thing.
#
# Deliberately a snapshot-and-restore, not a wipe: deleting the save would put the game in a
# "no save data" state no existing route was authored against. First call captures whatever
# is currently there as the baseline; later calls restore that exact baseline.
#
# Safety contract (#183):
#   * baseline and live root must be distinct canonical directories inside $ApprovedRoot,
#     and neither may contain the other;
#   * a captured baseline is bound to a manifest (creation time, source identity, relative
#     inventory and hashes); a baseline without a valid manifest is never restored;
#   * restore preflights the baseline, stages a verified copy beside the live root, swaps
#     live directories into a rollback and staged directories into place, verifies the
#     result, and only then drops the rollback; any failure rolls back and reports, never
#     silently continues;
#   * *GAMEDATA is never touched (that is the ~400 MB game install, not save state).
#
# -Failpoint is a documented test seam that forces a specific failure to prove rollback.
#
# Returns $null on failure, otherwise an object describing what was done.
function Sync-SaveBase {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [string]$SaveRoot = "memstick/PSP/SAVEDATA",
        [string]$ApprovedRoot = $null,
        [string]$RouteContext = "",
        [string]$BuildContext = "",
        [string]$Failpoint = ""
    )
    try {
        if ([string]::IsNullOrWhiteSpace($ApprovedRoot)) {
            $ApprovedRoot = [System.IO.Path]::GetFullPath((Get-Location).Path)
        }
        $baseCanon = Get-CanonicalPath -Path $BasePath
        $saveCanon = Get-CanonicalPath -Path $SaveRoot
        $rootCanon = Get-CanonicalPath -Path $ApprovedRoot

        # Containment and distinctness first: refuse destructive work on anything that is
        # not an explicitly approved directory pair.
        if (-not (Test-PathContained -Path $baseCanon -Root $rootCanon)) {
            throw "save baseline is outside the approved root: $BasePath"
        }
        if (-not (Test-PathContained -Path $saveCanon -Root $rootCanon)) {
            throw "save root is outside the approved root: $SaveRoot"
        }
        if ($baseCanon -eq $saveCanon) {
            throw "save baseline and live save root must be distinct directories: $baseCanon"
        }
        if (Test-PathAncestor -Ancestor $baseCanon -Descendant $saveCanon) {
            throw "the live save root must not live inside the baseline: $SaveRoot"
        }
        if (Test-PathAncestor -Ancestor $saveCanon -Descendant $baseCanon) {
            throw "the baseline must not live inside the live save root: $BasePath"
        }
        if (-not (Test-Path -LiteralPath $saveCanon -PathType Container)) {
            throw "save root not found: $SaveRoot"
        }

        $live = @(Get-ChildItem -LiteralPath $saveCanon -Directory -Force -ErrorAction Stop |
            Where-Object { -not (Test-SaveBaseUnmanagedName -Name $_.Name) })

        if (-not (Test-Path -LiteralPath $baseCanon -PathType Container)) {
            # ---- CAPTURE: bind the live save set to a manifest. ----
            New-Item -ItemType Directory -Path $baseCanon -Force -ErrorAction Stop | Out-Null
            foreach ($d in $live) {
                Copy-Item -LiteralPath $d.FullName -Destination $baseCanon -Recurse -Force -ErrorAction Stop
            }
            $inventory = @(New-SaveBaseInventory -Root $baseCanon)
            Write-SaveBaseManifest -Root $baseCanon -Inventory $inventory `
                -SaveRootCanon $saveCanon -RouteContext $RouteContext -BuildContext $BuildContext
            if ($inventory.Count -eq 0) {
                Write-Host "[!] Save baseline is EMPTY (no save directories captured); it cannot be used to restore." -ForegroundColor Yellow
            }
            Write-Host "[*] Save baseline captured ($($inventory.Count) file(s)) -> $BasePath" -ForegroundColor Cyan
            return [pscustomobject]@{ Action = "captured"; BasePath = $BasePath; Files = $inventory.Count }
        }

        # ---- RESTORE: preflight the baseline before any destructive step. ----
        $manifest = Read-SaveBaseManifest -Root $baseCanon
        if ($null -eq $manifest) {
            throw "baseline has no hst manager manifest; refusing to restore an unmanaged directory: $BasePath"
        }
        $baselineFiles = @(Get-ChildItem -LiteralPath $baseCanon -Recurse -File -Force -ErrorAction Stop |
            Where-Object { $_.FullName -ne (Get-SaveBaseManifestPath $baseCanon) })
        if ($baselineFiles.Count -eq 0) {
            throw "baseline is empty; refusing to destroy the live save set"
        }
        if ($baselineFiles.Count -ne [int]$manifest.file_count) {
            throw "baseline inventory mismatch: manifest records $($manifest.file_count) files, found $($baselineFiles.Count)"
        }
        $baselineDiff = @(Compare-SaveBaseInventory -Root $baseCanon -Manifest $manifest)
        if ($baselineDiff.Count -gt 0) {
            throw "baseline does not match its own manifest ($($baselineDiff.Count) difference(s)); refusing to restore"
        }

        # Stage and rollback are same-volume siblings of the live root (both inside the
        # approved root), so the swap is renames, not cross-volume copies.
        $parentDir = [IO.Path]::GetDirectoryName($saveCanon)
        # A leftover stage/rollback sibling means an earlier restore was interrupted
        # (hard kill, not a caught exception) inside its swap window: the live root may
        # be incomplete and the old data is stranded in the orphan. Fail closed instead
        # of treating the partial state as a completed restore.
        $orphans = @(Get-ChildItem -LiteralPath $parentDir -Directory -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like ".hst_savebase_stage_*" -or $_.Name -like ".hst_savebase_rollback_*" })
        if ($orphans.Count -gt 0) {
            $orphanList = ($orphans | ForEach-Object { $_.FullName }) -join ", "
            throw "an earlier save restore did not complete (orphan swap dirs present: $orphanList); resolve or remove them before restoring again"
        }
        $stage = Join-Path $parentDir (".hst_savebase_stage_" + [guid]::NewGuid().ToString("N"))
        $rollback = Join-Path $parentDir (".hst_savebase_rollback_" + [guid]::NewGuid().ToString("N"))
        try {
            New-Item -ItemType Directory -Path $stage -Force -ErrorAction Stop | Out-Null
            foreach ($d in @(Get-ChildItem -LiteralPath $baseCanon -Directory -Force -ErrorAction Stop)) {
                if (Test-SaveBaseUnmanagedName -Name $d.Name) { continue }
                Copy-Item -LiteralPath $d.FullName -Destination $stage -Recurse -Force -ErrorAction Stop
            }
            if ($Failpoint -eq "stage-copy") { throw "injected stage-copy failure (test seam)" }
            $stageDiff = @(Compare-SaveBaseInventory -Root $stage -Manifest $manifest)
            if ($stageDiff.Count -gt 0) {
                throw "staged restore does not match the baseline manifest ($($stageDiff.Count) difference(s))"
            }

            # Move the live save dirs into the rollback shelter, then the staged dirs into
            # place. Same-volume moves; GAMEDATA never moves.
            New-Item -ItemType Directory -Path $rollback -Force -ErrorAction Stop | Out-Null
            $movedLive = @()
            $movedStage = @()
            try {
                foreach ($d in $live) {
                    Move-Item -LiteralPath $d.FullName -Destination $rollback -ErrorAction Stop
                    $movedLive += $d.FullName
                }
                if ($Failpoint -eq "swap-rename") { throw "injected swap-rename failure (test seam)" }
                foreach ($d in @(Get-ChildItem -LiteralPath $stage -Directory -Force -ErrorAction Stop)) {
                    Move-Item -LiteralPath $d.FullName -Destination $saveCanon -ErrorAction Stop
                    $movedStage += $d.FullName
                }
            } catch {
                # Undo the partial swap before rethrowing: nothing may be left in limbo.
                # Stage-moved children are removed FIRST so a same-named live dir can then
                # be moved back into the now-vacant slot. [void]: these helpers return
                # $true and must not pollute Sync-SaveBase's own return value.
                foreach ($m in $movedStage) {
                    $name = [IO.Path]::GetFileName($m)
                    [void](Remove-SafeDirectory -Path (Join-Path $saveCanon $name) -Root $rootCanon)
                }
                foreach ($m in $movedLive) {
                    $name = [IO.Path]::GetFileName($m)
                    $target = Join-Path $saveCanon $name
                    if (-not (Test-Path -LiteralPath $target)) {
                        Move-Item -LiteralPath (Join-Path $rollback $name) -Destination $saveCanon -ErrorAction Stop
                    }
                }
                throw
            }

            # Verify the live root against the manifest before the rollback may be dropped.
            if ($Failpoint -eq "verify") { throw "injected post-swap verification failure (test seam)" }
            $liveDiff = @(Compare-SaveBaseInventory -Root $saveCanon -Manifest $manifest -IncludesUnmanaged)
            if ($liveDiff.Count -gt 0) {
                throw "post-swap verification failed ($($liveDiff.Count) difference(s)); live save left in rollback"
            }
            # Both the empty stage and the rollback shelter are dropped only after the
            # live root verified against the manifest.
            [void](Remove-SafeDirectory -Path $stage -Root $rootCanon)
            [void](Remove-SafeDirectory -Path $rollback -Root $rootCanon)
            Write-Host "[*] Save state restored from baseline ($($manifest.file_count) file(s)): $BasePath" -ForegroundColor Cyan
            return [pscustomobject]@{ Action = "restored"; BasePath = $BasePath; Files = $manifest.file_count }
        } catch {
            # Rollback: discard the partial stage and move every sheltered live dir back.
            if (Test-Path -LiteralPath $stage) { [void](Remove-SafeDirectory -Path $stage -Root $rootCanon) }
            if (Test-Path -LiteralPath $rollback -PathType Container) {
                foreach ($d in @(Get-ChildItem -LiteralPath $rollback -Directory -Force -ErrorAction SilentlyContinue)) {
                    $target = Join-Path $saveCanon $d.Name
                    if (-not (Test-Path -LiteralPath $target)) {
                        Move-Item -LiteralPath $d.FullName -Destination $saveCanon -ErrorAction SilentlyContinue
                    }
                }
            }
            throw
        }
    } catch {
        Write-Host "[!] Save sync failed: $($_.Exception.Message)" -ForegroundColor Red
        return $null
    }
}

# Get-OracleVerdict: decide whether a completed run's captures are admissible evidence.
#
# A run that was killed at its backstop, exited nonzero, never reported reaching its
# requested vblank, or produced no captures has an output directory that looks exactly
# like a good run's -- just shorter. Earlier in this investigation a truncated capture set
# was read as a complete one and cost two full replays, so the verdict is computed here
# and recorded, rather than left to whoever opens the directory.
function Get-OracleVerdict {
    param(
        [bool]$ReachedExit,
        [bool]$TimedOut,
        [Nullable[int]]$ExitCode,
        [int]$CaptureCount,
        [int]$RequestedVblank,
        [int]$ObservedVblank
    )

    $reasons = @()
    if ($TimedOut) {
        $reasons += "killed at the backstop deadline instead of exiting on its own"
    }
    if (-not $ReachedExit) {
        $reasons += "no exit_at_vblank record in stderr (route did not reach vblank $RequestedVblank)"
    }
    if ($null -eq $ExitCode) {
        # Not pedantry: an unreadable exit code is exactly what the Start-Process handle
        # quirk produces, and treating "unknown" as "fine" would let a crashed run be
        # archived as admissible evidence.
        $reasons += "could not determine the process exit code"
    } elseif ($ExitCode -ne 0) {
        $reasons += "process exit code $ExitCode"
    }
    if ($ObservedVblank -lt $RequestedVblank) {
        $reasons += "observed $ObservedVblank vblanks, requested $RequestedVblank"
    }
    if ($CaptureCount -le 0) {
        $reasons += "no framebuffer captures were produced"
    }

    return [pscustomobject]@{
        Complete = ($reasons.Count -eq 0)
        Reasons  = $reasons
    }
}
