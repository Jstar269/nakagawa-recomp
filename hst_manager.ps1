# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Deprecated backward-compatibility forwarding wrapper for nk_manager.ps1.
.DESCRIPTION
    Forwards all arguments to nk_manager.ps1. Emits a deprecation warning.
    When -TitleManifest is omitted, defaults to assets/titles/hst-ucus98701.json
    if that file exists, preserving legacy Hot Shots Tennis build behavior.
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

# If TitleManifest was not supplied, default to the HST manifest if available
if (-not $forwardArgs.ContainsKey('TitleManifest') -or -not $forwardArgs['TitleManifest']) {
    $HstManifest = Join-Path $RepoRoot "assets\titles\hst-ucus98701.json"
    if (Test-Path -LiteralPath $HstManifest) {
        $forwardArgs['TitleManifest'] = "assets/titles/hst-ucus98701.json"
    }
}

& $NkManager @forwardArgs
exit $LASTEXITCODE
