# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.4

<#
.SYNOPSIS
    Deprecated backward-compatibility forwarding wrapper for nk_manager.ps1.
.DESCRIPTION
    Forwards all arguments to nk_manager.ps1. Emits a deprecation warning.
    When -TitleManifest is omitted, defaults to assets/titles/hst-ucus98701.json
    if that file exists, preserving legacy Hot Shots Tennis build behavior.
    See -Action help for all supported operations; omit -Action for canonical usage.
.PARAMETER Action
    BuildFull - Clean and rebuild the selected title through the full pipeline.
    BuildFast - Incrementally build the selected title without cleaning.
    Run - Launch the selected title with the chosen runtime profile.
    Inspect - Locate a generated C function using -InspectFunc.
    Clean - Stop tracked build processes and clear local tracking logs.
    Test - Run the Makefile selftest target (C++ reference-runtime selftest), not the Python suite.
    Verify - Run Python tests, native selftests, and import/publication audits.
    DiffFunc - Compare a function against a reference trace using -DiffTarget and -DiffOracle.
    FindSymbol - Search the optional symbol reference using -FindName.
    Fuzz - Run the Makefile vfpu_fuzz target.
    VisualOracle - Replay a route and archive visual regression captures.
#>

Param(
    [Parameter(Mandatory=$false)]
    [ValidateSet("BuildFull", "BuildFast", "Run", "Inspect", "Clean", "Test", "Verify", "DiffFunc", "FindSymbol", "Fuzz", "VisualOracle")]
    [string]$Action,

    [Parameter(Mandatory=$false)]
    [string]$Route,

    [Parameter(Mandatory=$false)]
    [ValidateRange(0, 2000000000)]
    [int]$ExitAtVblank = 0,

    [Parameter(Mandatory=$false)]
    [ValidateRange(0, 2000000000)]
    [int]$SnapEvery = 0,

    [Parameter(Mandatory=$false)]
    [ValidateRange(0, 2000000000)]
    [int]$SnapAfter = 0,

    [Parameter(Mandatory=$false)]
    [ValidatePattern('^\s*$|^\d+-\d+(,\d+-\d+)*$')]
    [string]$SnapWindows,

    [Parameter(Mandatory=$false)]
    [string]$SaveBase,

    [Parameter(Mandatory=$false)]
    [ValidatePattern('^$|^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')]
    [string]$OracleName,

    [Parameter(Mandatory=$false)]
    [switch]$OverwriteOracle,

    [Parameter(Mandatory=$false)]
    [ValidateRange(0, 2000000000)]
    [int]$Duration = 0,

    [Parameter(Mandatory=$false)]
    [switch]$NoGui,

    [Parameter(Mandatory=$false)]
    [switch]$SoftwareRender,

    [Parameter(Mandatory=$false)]
    [ValidateSet("Standard", "Performance", "Benchmark", "Diagnostics", "Software")]
    [string]$Profile = "Standard",

    [Parameter(Mandatory=$false)]
    [switch]$GuestProfile,

    [Parameter(Mandatory=$false)]
    [ValidateRange(0, 100000000)]
    [int]$GuestProfilePeriod = 3600,

    [Parameter(Mandatory=$false)]
    [string]$InspectFunc,

    [Parameter(Mandatory=$false)]
    [string]$DiffTarget,

    [Parameter(Mandatory=$false)]
    [string]$DiffOracle,

    [Parameter(Mandatory=$false)]
    [int]$DiffStep = 0,

    [Parameter(Mandatory=$false)]
    [string]$FindName,

    [Parameter(Mandatory=$false)]
    [string]$MsysPath = "C:\msys64\ucrt64\bin",

    [Parameter(Mandatory=$false)]
    [string]$MakeExecutable = "",

    [Parameter(Mandatory=$false)]
    [string]$VulkanSdk = "",

    [Parameter(Mandatory=$false)]
    [ValidateSet("O0", "O1", "O2")]
    [string]$RuntimeOpt,

    [Parameter(Mandatory=$false)]
    [ValidateSet("O0", "O1", "O2")]
    [string]$RecompOpt,

    [Parameter(Mandatory=$false)]
    [ValidateRange(1, 1000000)]
    [int]$FuncsPerChunk = 0,

    [Parameter(Mandatory=$false)]
    [ValidateRange(0, 1024)]
    [int]$Jobs = 0,

    [Parameter(Mandatory=$false)]
    [string]$TitleManifest = "",

    [Parameter(Mandatory=$false)]
    [string]$GameName = ""
)

Write-Warning "hst_manager.ps1 is deprecated and will be retired in Phase 3/4. Delegating to nk_manager.ps1..."

$RepoRoot = $PSScriptRoot
$NkManager = Join-Path $RepoRoot "nk_manager.ps1"
if (-not (Test-Path -LiteralPath $NkManager)) {
    Write-Error "Could not find canonical manager: $NkManager"
    exit 1
}

# Construct forwarded parameter table
$forwardArgs = @{}
foreach ($paramName in $PSBoundParameters.Keys) {
    $forwardArgs[$paramName] = $PSBoundParameters[$paramName]
}

# If TitleManifest was not supplied, default to the HST manifest if available.
# Issue #196 Phase 4: the wrapper DECLARES the legacy build name explicitly
# (-GameName hst). nk_manager no longer mints built-in names from manifest-id
# prefixes, so this declaration is what keeps the legacy default route building
# the 'hst' target (Makefile's GAME_NAME=hst compatibility block) without any
# title coupling in the generic manager.
$HstManifest = Join-Path $RepoRoot "assets\titles\hst-ucus98701.json"
$hstManifestSelected = $false
if (-not $forwardArgs.ContainsKey('TitleManifest') -or -not $forwardArgs['TitleManifest']) {
    if (Test-Path -LiteralPath $HstManifest) {
        $forwardArgs['TitleManifest'] = "assets/titles/hst-ucus98701.json"
        $hstManifestSelected = $true
    }
}

# An explicitly selected HST manifest is also eligible for the legacy name;
# an explicitly selected non-HST manifest must keep its own name or derivation.
if (-not $hstManifestSelected -and $forwardArgs.ContainsKey('TitleManifest') -and $forwardArgs['TitleManifest']) {
    try {
        $requestedManifest = [IO.Path]::GetFullPath((Join-Path $RepoRoot ([string]$forwardArgs['TitleManifest'])))
        $hstManifestSelected = $requestedManifest -ieq ([IO.Path]::GetFullPath($HstManifest))
    } catch {
        $hstManifestSelected = $false
    }
}

# Issue #196 Phase 4: declare the legacy build name explicitly. The generic
# manager derives game_name only from -GameName, the manifest's own game_name
# declaration, or the manifest id — it never mints "hst" from an id prefix.
if ($hstManifestSelected -and (-not $forwardArgs.ContainsKey('GameName') -or -not $forwardArgs['GameName'])) {
    $forwardArgs['GameName'] = "hst"
}

& $NkManager @forwardArgs
exit $LASTEXITCODE
