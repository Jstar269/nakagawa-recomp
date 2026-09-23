# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Fail-closed path, process, build, and run safety primitives shared by nk_manager.ps1 and tests.

.DESCRIPTION
    These helpers are the ones whose silent failure modes are destructive or
    evidence-corrupting:

      Assert-NkWorkspaceRoot       validate a Nakagawa Recomp workspace root and record it.
      Get-CanonicalPath            canonical full path, resolving reparse points/junctions
                                   where the OS allows (Windows: GetFinalPathNameByHandle).
      Test-PathContained           strict canonical containment (path == root or descendant).
      Test-PathAncestor            strict descendant relationship between two canonical paths.
      Test-SafeComponentName       identifier grammar for names that become path components.
      Remove-SafeDirectory         recursive delete that refuses reparse-point escapes.
      Get-ProcessIdentityRecord    PID + creation time + exe + command line + parent identity.
      Test-ProcessRecordStillValid verdict used before any stale-process kill.
      Get-BuildPidRecords          read/validate the JSON build-PID ledger.
      Invoke-StaleBuildCleanup     identity-verified stale build-tree cleanup (DryRun supported).
      Get-ProcessTreeIds           root PID plus transitive children.
      Get-KnownExitCode            a build result is success only when the exit code is known.
      ConvertTo-SafeTimeoutSeconds strict non-negative integer parse for run deadlines.

    Exact guarantees:

    * Canonical containment resolves existing path components through reparse points
      (symlinks/junctions) on Windows via GetFinalPathNameByHandle, so a junction whose
      final target leaves the approved root is rejected even though the lexical path is
      a descendant. Where the final-path API is unavailable (non-Windows, or a path
      component does not exist yet), the guarantee degrades to lexical GetFullPath
      normalization: the deepest existing ancestor is canonicalized and the remainder
      appended. A non-existent component cannot be a reparse point, so this degradation
      cannot reintroduce a reparse escape in an existing prefix.
    * Test-PathContained returns $false on any resolution uncertainty (fail closed).
    * Process identity: a recorded build root is killed only when PID, creation time,
      normalized executable path and command line all still match. Any missing or
      contradictory field leaves the process untouched. A missed stale process is
      preferred over terminating unrelated user work.

    Dot-source this file (it defines functions and returns nothing).
#>

# ---------------------------------------------------------------------------
# Repository-root identity
# ---------------------------------------------------------------------------

function Assert-NkWorkspaceRoot {
    <#
        Fail closed unless $Root looks like the Nakagawa Recomp repository the manager
        belongs to. Records the root in $script:NkWorkspaceRoot for process-identity
        records. Returns the canonical root.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string[]]$Anchors = @("Makefile", "AGENTS.md", "src/rt/recomp.c", "tools/codegen.py")
    )
    if ([string]::IsNullOrWhiteSpace($Root)) {
        throw "Workspace root must not be empty"
    }
    $full = $null
    try { $full = [IO.Path]::GetFullPath($Root) } catch { throw "Invalid workspace root: $Root" }
    foreach ($anchor in $Anchors) {
        if (-not (Test-Path -LiteralPath (Join-Path $full $anchor))) {
            throw "Not a Nakagawa Recomp workspace root (missing $anchor): $full"
        }
    }
    $script:NkWorkspaceRoot = $full
    return $full
}

# ---------------------------------------------------------------------------
# Canonical paths and containment
# ---------------------------------------------------------------------------

function Invoke-FinalPathFromHandle {
    <#
        Windows-only: resolve the final path of an existing file/directory through every
        reparse point using GetFinalPathNameByHandle. Returns $null on any failure (caller
        falls back to lexical normalization and must treat that as a weaker guarantee).
    #>
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
            [System.Runtime.InteropServices.OSPlatform]::Windows)) {
        return $null
    }
    try {
        if ($null -eq ("NkCanonical" -as [type])) {
            Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NkCanonical {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern IntPtr CreateFileW(string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern uint GetFinalPathNameByHandleW(IntPtr hFile, StringBuilder lpszFilePath, uint cchFilePath, uint dwFlags);
    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr hObject);
    public static string FinalPath(string path) {
        IntPtr h = CreateFileW(path, 0, 1 | 2 | 4, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
        if (h == IntPtr.Zero || h.ToInt64() == -1) { return null; }
        try {
            var sb = new StringBuilder(32768);
            uint r = GetFinalPathNameByHandleW(h, sb, (uint)sb.Capacity, 0);
            if (r == 0 || r >= sb.Capacity) { return null; }
            string p = sb.ToString();
            if (p.StartsWith(@"\\?\UNC\", StringComparison.Ordinal)) { p = @"\\" + p.Substring(8); }
            else if (p.StartsWith(@"\\?\", StringComparison.Ordinal)) { p = p.Substring(4); }
            else if (p.StartsWith(@"\??\", StringComparison.Ordinal)) { p = p.Substring(4); }
            return p;
        } finally { CloseHandle(h); }
    }
}
"@ -ErrorAction Stop
        }
        return [NkCanonical]::FinalPath($Path)
    } catch {
        return $null
    }
}

function Get-CanonicalPath {
    <#
        Canonical absolute path for containment decisions. Resolves reparse points for
        existing paths on Windows; for non-existent paths canonicalizes the deepest
        existing ancestor and appends the remainder.
    #>
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $full) {
        $final = Invoke-FinalPathFromHandle -Path $full
        if ($final) { return $final }
        return $full
    }
    $cur = $full
    $tail = New-Object System.Collections.Generic.Stack[string]
    while ($true) {
        if (Test-Path -LiteralPath $cur) { break }
        $name = [System.IO.Path]::GetFileName($cur)
        if ([string]::IsNullOrEmpty($name)) { break }
        $tail.Push($name)
        $parent = [System.IO.Path]::GetDirectoryName($cur)
        if ($parent -eq $cur) { break }
        $cur = $parent
    }
    $base = if (Test-Path -LiteralPath $cur) {
        $final = Invoke-FinalPathFromHandle -Path $cur
        if ($final) { $final } else { [System.IO.Path]::GetFullPath($cur) }
    } else {
        [System.IO.Path]::GetFullPath($cur)
    }
    while ($tail.Count -gt 0) { $base = [System.IO.Path]::Combine($base, $tail.Pop()) }
    return [System.IO.Path]::GetFullPath($base)
}

function Get-PathComparison {
    # Windows paths are case-insensitive; POSIX paths are case-sensitive.
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
            [System.Runtime.InteropServices.OSPlatform]::Windows)) {
        return [System.StringComparison]::OrdinalIgnoreCase
    }
    return [System.StringComparison]::Ordinal
}

function Test-PathContained {
    <#
        True when $Path is $Root itself or a strict descendant of $Root, using canonical
        paths. Any resolution uncertainty is a fail-closed $false.
    #>
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )
    if ([string]::IsNullOrWhiteSpace($Path) -or [string]::IsNullOrWhiteSpace($Root)) { return $false }
    try {
        $canonicalPath = Get-CanonicalPath -Path $Path
        $canonicalRoot = Get-CanonicalPath -Path $Root
        if ([string]::IsNullOrWhiteSpace($canonicalPath) -or [string]::IsNullOrWhiteSpace($canonicalRoot)) {
            return $false
        }
        $cmp = Get-PathComparison
        if ($canonicalPath.Equals($canonicalRoot, $cmp)) { return $true }
        $rootWithSep = $canonicalRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        return $canonicalPath.StartsWith($rootWithSep, $cmp)
    } catch {
        return $false
    }
}

function Test-PathAncestor {
    <#
        True when $Descendant is a strict descendant of $Ancestor (equal paths are not a
        descendant relationship; callers check distinctness separately).
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Ancestor,
        [Parameter(Mandatory = $true)][string]$Descendant
    )
    try {
        $a = Get-CanonicalPath -Path $Ancestor
        $d = Get-CanonicalPath -Path $Descendant
        if ([string]::IsNullOrWhiteSpace($a) -or [string]::IsNullOrWhiteSpace($d)) { return $false }
        $cmp = Get-PathComparison
        if ($a.Equals($d, $cmp)) { return $false }
        $rootWithSep = $a.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        return $d.StartsWith($rootWithSep, $cmp)
    } catch {
        return $false
    }
}

function Test-SafeComponentName {
    <#
        Identifier grammar for a name that becomes a single path component: at most 64
        chars, starts alphanumeric, then alphanumeric/._/-, and never a path separator or
        a leading/trailing dot. This keeps derived names like "deep_return_run1" legal
        while rejecting separators, "..", rooted and drive-relative forms outright.
    #>
    param([Parameter(Mandatory = $true)][string]$Name)
    if ([string]::IsNullOrWhiteSpace($Name)) { return $false }
    if ($Name.Length -gt 64) { return $false }
    if ($Name -match "[\\/]") { return $false }
    if ($Name -match "[\x00-\x1f]") { return $false }
    if ($Name -match "^[A-Za-z0-9][A-Za-z0-9._-]*$" -and
        -not $Name.StartsWith(".") -and -not $Name.EndsWith(".") -and
        -not $Name.Contains("..")) {
        return $true
    }
    return $false
}

function Remove-SafeDirectory {
    <#
        The ONLY recursive-delete primitive the manager uses. Refuses (throws) when the
        target is not contained in $Root or when any reparse point in the tree resolves
        to a final path outside $Root. Uses -LiteralPath and -ErrorAction Stop throughout.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )
    if (-not (Test-Path -LiteralPath $Path)) { return $true }
    $canonicalTarget = Get-CanonicalPath -Path $Path
    if (-not (Test-PathContained -Path $canonicalTarget -Root $Root)) {
        throw "refusing recursive delete of '$Path': canonical target '$canonicalTarget' is outside allowed root '$Root'"
    }
    # The target itself may be a reparse point; resolve and re-check it.
    $rootItem = Get-Item -LiteralPath $canonicalTarget -Force -ErrorAction Stop
    if ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        $final = Get-CanonicalPath -Path $canonicalTarget
        if (-not (Test-PathContained -Path $final -Root $Root)) {
            throw "refusing recursive delete of '$Path': reparse target '$final' escapes allowed root '$Root'"
        }
    }
    # Every reparse descendant must stay inside the root as well.
    $reparse = @(Get-ChildItem -LiteralPath $canonicalTarget -Recurse -Force -ErrorAction Stop |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
    foreach ($r in $reparse) {
        $final = Get-CanonicalPath -Path $r.FullName
        if (-not (Test-PathContained -Path $final -Root $Root)) {
            throw "refusing recursive delete of '$Path': reparse descendant '$($r.FullName)' escapes to '$final'"
        }
    }
    Remove-Item -LiteralPath $canonicalTarget -Recurse -Force -ErrorAction Stop
    return $true
}

# ---------------------------------------------------------------------------
# Process identity (PID reuse is not identity)
# ---------------------------------------------------------------------------

function Get-ProcessIdentityRecord {
    <#
        Capture the strongest practical identity for a live process: PID, creation time
        (UTC ticks), normalized executable path, command line and parent PID. Returns
        $null when the process no longer exists. Fields that cannot be read are $null.
    #>
    param([Parameter(Mandatory = $true)][int]$Id)
    try {
        $p = Get-Process -Id $Id -ErrorAction SilentlyContinue
        if ($null -eq $p) { return $null }
        $rec = [ordered]@{ pid = $Id }
        try { $rec.creation_ticks = $p.StartTime.ToUniversalTime().Ticks } catch { $rec.creation_ticks = $null }
        try {
            if (-not [string]::IsNullOrWhiteSpace($p.Path)) {
                $rec.exe = [IO.Path]::GetFullPath($p.Path)
            } else {
                $rec.exe = $null
            }
        } catch { $rec.exe = $null }
        $rec.parent = $null
        $rec.cmd = $null
        try {
            $wmi = Get-CimInstance Win32_Process -Filter "ProcessId = $Id" -ErrorAction Stop
            $rec.cmd = $wmi.CommandLine
            $rec.parent = [int]$wmi.ParentProcessId
        } catch {
            # Command line is unavailable; the process identity is incomplete.
        }
        $rec.root = $script:NkWorkspaceRoot
        $rec.captured_utc = ([DateTime]::UtcNow).ToString("o")
        return [pscustomobject]$rec
    } catch {
        return $null
    }
}

function Test-ProcessRecordStillValid {
    <#
        Verdict used before ANY stale-process kill:
          "valid"    every available identity field still matches; killing is allowed.
          "gone"     the PID no longer exists; nothing to kill.
          "mismatch" the PID is now a different process (reuse) or a field contradicts.
          "unknown"  identity fields are missing; fail safe and leave the process alive.
        Tests inject a -Probe scriptblock (param($Id) -> identity object or $null) so no
        real process is touched.
    #>
    param(
        [Parameter(Mandatory = $true)][object]$Record,
        [scriptblock]$Probe = $null
    )
    $current = if ($Probe) { & $Probe -Id ([int]$Record.pid) } else { Get-ProcessIdentityRecord -Id ([int]$Record.pid) }
    if ($null -eq $current) { return "gone" }
    if ($null -eq $Record.creation_ticks -or $null -eq $current.creation_ticks) { return "unknown" }
    try {
        $delta = [math]::Abs([int64]$current.creation_ticks - [int64]$Record.creation_ticks)
        if ($delta -gt 0) { return "mismatch" }
    } catch { return "unknown" }
    if ([string]::IsNullOrWhiteSpace([string]$Record.exe) -or [string]::IsNullOrWhiteSpace([string]$current.exe)) {
        return "unknown"
    }
    $cmp = Get-PathComparison
    if (-not [string]::Equals(
            ([string]$Record.exe).TrimEnd('\', '/'),
            ([string]$current.exe).TrimEnd('\', '/'),
            $cmp)) {
        return "mismatch"
    }
    if ($null -eq $Record.cmd -or $null -eq $current.cmd) { return "unknown" }
    if ([string]$Record.cmd -ne [string]$current.cmd) { return "mismatch" }
    return "valid"
}

function Get-BuildPidRecords {
    <#
        Read the JSON-lines build-PID ledger. Returns @{ Records = ...; Malformed = ... }.
        Lines that are not valid identity records (e.g. the legacy bare-number format)
        are reported as malformed and are NEVER trusted for a kill.
    #>
    param([Parameter(Mandatory = $true)][string]$Path)
    $records = New-Object System.Collections.Generic.List[object]
    $malformed = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $Path)) {
        return @{ Records = @(); Malformed = @() }
    }
    foreach ($line in @(Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue)) {
        $t = ($line).Trim()
        if (-not $t) { continue }
        try {
            $rec = $t | ConvertFrom-Json
            [int]$recPid = 0
            $pidOk = $null -ne $rec.pid -and [int]::TryParse([string]$rec.pid, [ref]$recPid)
            if ($pidOk -and $recPid -gt 0 -and $null -ne $rec.creation_ticks -and -not [string]::IsNullOrWhiteSpace([string]$rec.exe)) {
                $records.Add($rec)
            } else {
                $malformed.Add($t)
            }
        } catch {
            $malformed.Add($t)
        }
    }
    # ToArray(): a generic List can fail to unroll inside a hashtable/array context on
    # some hosts; plain arrays are unambiguous for callers and tests.
    return @{ Records = $records.ToArray(); Malformed = $malformed.ToArray() }
}

function Get-ProcessTreeIds {
    <#
        $RootPid plus every transitive child from the Win32_Process parent/child graph.
        Where Get-CimInstance is unavailable (POSIX hosts) only the root PID is returned.
    #>
    param([int]$RootPid)
    $ids = New-Object System.Collections.Generic.List[int]
    if ($RootPid -le 0) { return $ids }
    $cim = Get-Command Get-CimInstance -ErrorAction SilentlyContinue
    $all = if ($cim) {
        @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
            Select-Object ProcessId, ParentProcessId)
    } else {
        @()
    }
    $queue = New-Object System.Collections.Generic.Queue[int]
    $ids.Add($RootPid)
    $queue.Enqueue($RootPid)
    while ($queue.Count -gt 0) {
        $cur = $queue.Dequeue()
        foreach ($p in $all) {
            if (([int]$p.ParentProcessId -eq $cur) -and (-not $ids.Contains([int]$p.ProcessId))) {
                $ids.Add([int]$p.ProcessId)
                $queue.Enqueue([int]$p.ProcessId)
            }
        }
    }
    return $ids
}

function Invoke-StaleBuildCleanup {
    <#
        Identity-verified stale build cleanup for one ledger file. Kills only recorded
        roots whose identity still fully matches (and their compiler children); leaves
        anything unverifiable untouched. Rewrites the ledger with the kept records.
        -DryRun reports what would be killed without killing.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$PidFile,
        [string]$WorkspaceRoot = $null,
        [string[]]$BuildToolNames = $null,
        [scriptblock]$Probe = $null,
        [switch]$DryRun
    )
    if ($null -eq $BuildToolNames) {
        $BuildToolNames = @(
            "make", "mingw32-make", "gcc", "g++", "c++", "cc1", "cc1plus",
            "collect2", "as", "ld", "ld.lld", "lld", "lld-link", "cpp", "windres", "ar"
        )
    }
    $killed = New-Object System.Collections.Generic.List[int]
    $kept = New-Object System.Collections.Generic.List[object]
    if (-not (Test-Path -LiteralPath $PidFile)) {
        return @{ Killed = @(); Kept = @(); Malformed = @() }
    }
    $parsed = Get-BuildPidRecords -Path $PidFile
    foreach ($rec in $parsed.Records) {
        $verdict = Test-ProcessRecordStillValid -Record $rec -Probe $Probe
        switch ($verdict) {
            "valid" {
                if ($DryRun) {
                    Write-Host "[dry-run] would reap verified build tree root $($rec.pid)" -ForegroundColor Yellow
                } else {
                    foreach ($id in (Get-ProcessTreeIds -RootPid ([int]$rec.pid))) {
                        $tp = Get-Process -Id $id -ErrorAction SilentlyContinue
                        if ($tp -and ($BuildToolNames -contains $tp.Name)) {
                            $tp | Stop-Process -Force -ErrorAction SilentlyContinue
                        }
                    }
                }
                $killed.Add([int]$rec.pid)
            }
            "gone" {
                # PID no longer exists; the record is stale history.
            }
            "mismatch" {
                Write-Host "[!] Build record pid $($rec.pid) is now a different process; leaving it alive and dropping the stale record." -ForegroundColor Yellow
            }
            default {
                Write-Host "[!] Cannot verify build record pid $($rec.pid) (missing identity fields); leaving the process alive and keeping the record." -ForegroundColor Yellow
                $kept.Add($rec)
            }
        }
    }
    if ($kept.Count -gt 0) {
        $kept | ForEach-Object { $_ | ConvertTo-Json -Compress } |
            Set-Content -LiteralPath $PidFile -Encoding utf8 -ErrorAction Stop
    } else {
        Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
    }
    return @{ Killed = $killed.ToArray(); Kept = $kept.ToArray(); Malformed = @($parsed.Malformed) }
}

# ---------------------------------------------------------------------------
# Build truth: unknown exit status is not success
# ---------------------------------------------------------------------------

function Get-KnownExitCode {
    <#
        Read a process's exit code in a way that treats "cannot read it" as unknown.
        Touching .Handle caches the native handle so Start-Process -PassThru children with
        redirected streams yield a real exit code; if it is still unreadable the result is
        Known = $false, which callers must treat as failure/unknown, never success.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()][object]$Process
    )
    if ($null -eq $Process) { return @{ Known = $false; ExitCode = $null } }
    try { $null = $Process.Handle } catch { }
    try {
        if ($Process.HasExited) {
            $code = $Process.ExitCode
            return @{ Known = ($null -ne $code); ExitCode = $code }
        }
    } catch { }
    return @{ Known = $false; ExitCode = $null }
}

# ---------------------------------------------------------------------------
# Numeric/time input validation
# ---------------------------------------------------------------------------

function ConvertTo-SafeTimeoutSeconds {
    <#
        Strict non-negative integer parse for run deadlines. Returns $null for blank,
        negative, non-numeric or overflowing input so a caller can reject or re-prompt
        instead of silently waiting indefinitely.
    #>
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    [int]$value = 0
    if (-not [int]::TryParse($Text, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$value)) {
        return $null
    }
    if ($value -lt 0) { return $null }
    return $value
}

function Wait-ProcessOrKill {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [int]$TimeoutSeconds = 0
    )

    try { $null = $Process.Handle } catch { }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $killed = $false

    if ($TimeoutSeconds -gt 0) {
        $ms = [double]$TimeoutSeconds * 1000.0
        if ($ms -gt [int]::MaxValue) { $ms = [double][int]::MaxValue }
        if (-not $Process.WaitForExit([int]$ms)) {
            $timedOut = $true
            try {
                $Process | Stop-Process -Force -ErrorAction Stop
                $killed = $true
            } catch {
            }
            [void]$Process.WaitForExit(5000)
        }
    } else {
        $Process.WaitForExit()
    }

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

function Get-SaveBaseManifestPath {
    param([Parameter(Mandatory = $true)][string]$Root)
    return (Join-Path $Root ".nk_savebase_manifest.json")
}

function New-SaveBaseInventory {
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
        format          = "nk-savebase-manifest/v1"
        created_utc     = ([DateTime]::UtcNow).ToString("o")
        managed_by      = "nk_manager.ps1"
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
    param([Parameter(Mandatory = $true)][string]$Root)
    $mf = Get-SaveBaseManifestPath $Root
    if (-not (Test-Path -LiteralPath $mf -PathType Leaf)) { return $null }
    try {
        $m = Get-Content -LiteralPath $mf -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($m.format -ne "nk-savebase-manifest/v1" -or $null -eq $m.file_count) { return $null }
        return $m
    } catch {
        return $null
    }
}

function Test-SaveBaseUnmanagedName {
    param([Parameter(Mandatory = $true)][string]$Name)
    return ($Name -like "*GAMEDATA*")
}

function Compare-SaveBaseInventory {
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

        $manifest = Read-SaveBaseManifest -Root $baseCanon
        if ($null -eq $manifest) {
            throw "baseline has no nk manager manifest; refusing to restore an unmanaged directory: $BasePath"
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

        $parentDir = [IO.Path]::GetDirectoryName($saveCanon)
        $orphans = @(Get-ChildItem -LiteralPath $parentDir -Directory -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like ".nk_savebase_stage_*" -or $_.Name -like ".nk_savebase_rollback_*" })
        if ($orphans.Count -gt 0) {
            $orphanList = ($orphans | ForEach-Object { $_.FullName }) -join ", "
            throw "an earlier save restore did not complete (orphan swap dirs present: $orphanList); resolve or remove them before restoring again"
        }
        $stage = Join-Path $parentDir (".nk_savebase_stage_" + [guid]::NewGuid().ToString("N"))
        $rollback = Join-Path $parentDir (".nk_savebase_rollback_" + [guid]::NewGuid().ToString("N"))
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

            if ($Failpoint -eq "verify") { throw "injected post-swap verification failure (test seam)" }
            $liveDiff = @(Compare-SaveBaseInventory -Root $saveCanon -Manifest $manifest -IncludesUnmanaged)
            if ($liveDiff.Count -gt 0) {
                throw "post-swap verification failed ($($liveDiff.Count) difference(s)); live save left in rollback"
            }
            [void](Remove-SafeDirectory -Path $stage -Root $rootCanon)
            [void](Remove-SafeDirectory -Path $rollback -Root $rootCanon)
            Write-Host "[*] Save state restored from baseline ($($manifest.file_count) file(s)): $BasePath" -ForegroundColor Cyan
            return [pscustomobject]@{ Action = "restored"; BasePath = $BasePath; Files = $manifest.file_count }
        } catch {
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

function Read-RouteOutcome {
    param([string]$StderrPath)

    $outcome = [pscustomobject]@{
        Kind        = "none"
        Checkpoints = @()
        FailReason  = $null
    }
    if (-not $StderrPath -or -not (Test-Path -LiteralPath $StderrPath)) { return $outcome }

    $reached = [System.Collections.Generic.List[string]]::new()
    $loaded = $false
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path -LiteralPath $StderrPath).Path)) {
        if ($line -match '^ROUTE: program loaded from ') { $loaded = $true; continue }
        if ($line -match '^ROUTE: (?:reached|confirmed) (\S+) at vblank (\d+)') {
            $reached.Add("$($Matches[1])@$($Matches[2])")
            continue
        }
        if ($line -match '^ROUTE_FAIL: (.+)$') {
            $outcome.Kind = "failed"
            $outcome.FailReason = $Matches[1].Trim()
            continue
        }
        if ($line -match '^ROUTE_PARSE: (.+)$' -and -not $outcome.FailReason) {
            $outcome.FailReason = "parse: $($Matches[1].Trim())"
            continue
        }
        if ($line -match '^ROUTE_OK:') { if ($outcome.Kind -ne "failed") { $outcome.Kind = "ok" }; continue }
    }
    $outcome.Checkpoints = $reached.ToArray()
    if ($outcome.Kind -eq "none" -and ($loaded -or $reached.Count -gt 0)) { $outcome.Kind = "incomplete" }
    return $outcome
}

function Get-OracleVerdict {
    param(
        [bool]$ReachedExit,
        [bool]$TimedOut,
        [Nullable[int]]$ExitCode,
        [int]$CaptureCount,
        [int]$RequestedVblank,
        [int]$ObservedVblank,
        [ValidateSet("none", "ok", "incomplete", "failed")]
        [string]$RouteKind = "none",
        [string]$RouteFailReason
    )

    $reasons = @()
    if ($RouteKind -eq "failed") {
        $detail = if ($RouteFailReason) { ": $RouteFailReason" } else { "" }
        $reasons += "the route did not reach its expected state$detail"
    } elseif ($RouteKind -eq "incomplete") {
        $reasons += "the route program never reported ROUTE_OK (its steps did not all complete)"
    }
    if ($TimedOut) {
        $reasons += "killed at the backstop deadline instead of exiting on its own"
    }
    if (-not $ReachedExit) {
        $reasons += "no exit_at_vblank record in stderr (route did not reach vblank $RequestedVblank)"
    }
    if ($null -eq $ExitCode) {
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
