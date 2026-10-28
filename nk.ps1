# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.4

<#
.SYNOPSIS
    Entry point for checking, building, running and cleaning a Nakagawa Recomp workspace.
.DESCRIPTION
    Runs one action, chosen with -Action (the first positional argument). Most actions run the
    workspace doctor (tools\nk_doctor.py) before and after the step they wrap, stop at the first
    failure and exit 1. The build, run and verify steps are carried out by the expert console,
    nk_manager.ps1, which this script calls with the matching manager action.

    Actions:
      Doctor   (default) Run the workspace doctor for -Scope and print its report.
      Build    Doctor (build), incremental build (manager BuildFast), Doctor (products).
      Rebuild  Doctor (build), full build (manager BuildFull), Doctor (products).
      Play     Doctor (inputs, build), incremental build, Doctor (products, run), then start the
               game (manager Run).
      Verify   Doctor (repo), then the manager's Verify action.
      Clean    Run tools\nk_clean.py to remove build output; -Yes skips its confirmation.
      Manager  Open the interactive expert/developer console (nk_manager.ps1).
.PARAMETER Action
    Doctor, Build, Rebuild, Play, Verify, Clean or Manager. Defaults to Doctor.
.PARAMETER Scope
    What the Doctor action checks: repo, inputs, build, products, run or all (default).
.PARAMETER Json
    Print the Doctor report as JSON (Doctor action only).
.PARAMETER Strict
    Treat doctor warnings as failures.
.PARAMETER Yes
    Pass --yes to the Clean action so it actually deletes what it lists (the default only previews).
.PARAMETER MsysPath
    MSYS2 UCRT64 bin folder. Defaults to $env:MSYS_PATH, or C:\msys64\ucrt64\bin.
.PARAMETER VulkanSdk
    Vulkan SDK folder, passed to the doctor and the manager when set.
.PARAMETER TitleManifest
    Title manifest to use, passed to the doctor and the manager when set. Without one the
    public synthetic manifest is used.
.PARAMETER GameName
    Game name to select, passed to the doctor and the manager when set.
.EXAMPLE
    .\nk.ps1 Doctor -Scope build -Strict
.EXAMPLE
    .\nk.ps1 Build -TitleManifest C:\path\to\manifest.json -GameName game
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("Doctor", "Build", "Rebuild", "Play", "Verify", "Clean", "Manager")]
    [string]$Action = "Doctor",

    [ValidateSet("repo", "inputs", "build", "products", "run", "all")]
    [string]$Scope = "all",

    [switch]$Json,
    [switch]$Strict,
    [switch]$Yes,

    [string]$MsysPath = $(if ($env:MSYS_PATH) { $env:MSYS_PATH } else { "C:\msys64\ucrt64\bin" }),
    [string]$VulkanSdk = "",
    [string]$TitleManifest = "",
    [string]$GameName = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = $PSScriptRoot
$Manager = Join-Path $RepoRoot "nk_manager.ps1"
$Doctor = Join-Path $RepoRoot "tools\nk_doctor.py"
$OriginalLocation = Get-Location

function Invoke-WorkspaceDoctor {
    param(
        [ValidateSet("repo", "inputs", "build", "products", "run", "all")]
        [string]$DoctorScope,
        [switch]$AsJson,
        [switch]$WarningsFail
    )

    if (-not (Test-Path -LiteralPath $Doctor)) {
        throw "Missing workspace doctor: $Doctor"
    }

    $arguments = @(
        $Doctor,
        "--root", $RepoRoot,
        "--scope", $DoctorScope,
        "--msys-path", $MsysPath
    )
    if ($VulkanSdk) { $arguments += @("--vulkan-sdk", $VulkanSdk) }
    if ($TitleManifest) { $arguments += @("--title-manifest", $TitleManifest) }
    if ($GameName) { $arguments += @("--game-name", $GameName) }
    if ($AsJson) { $arguments += "--json" }
    if ($WarningsFail) { $arguments += "--strict" }

    & python @arguments | Out-Host
    return ($LASTEXITCODE -eq 0)
}

function Invoke-ManagerAction {
    param([Parameter(Mandatory = $true)][string]$ManagerAction)

    if (-not (Test-Path -LiteralPath $Manager)) {
        throw "Missing NK manager: $Manager"
    }
    $arguments = @("-Action", $ManagerAction, "-MsysPath", $MsysPath)
    if ($VulkanSdk) { $arguments += @("-VulkanSdk", $VulkanSdk) }
    if ($TitleManifest) { $arguments += @("-TitleManifest", $TitleManifest) }
    if ($GameName) { $arguments += @("-GameName", $GameName) }
    $LASTEXITCODE = 0
    & $Manager @arguments | Out-Host
    $exitCode = [int]$LASTEXITCODE
    return ($exitCode -eq 0)
}

try {
    Set-Location -LiteralPath $RepoRoot

    switch ($Action) {
        "Doctor" {
            if (-not (Invoke-WorkspaceDoctor -DoctorScope $Scope -AsJson:$Json -WarningsFail:$Strict)) {
                exit 1
            }
        }
        "Build" {
            if (-not (Invoke-WorkspaceDoctor -DoctorScope "build" -WarningsFail:$Strict)) { exit 1 }
            if (-not (Invoke-ManagerAction -ManagerAction "BuildFast")) { exit 1 }
            if (-not (Invoke-WorkspaceDoctor -DoctorScope "products" -WarningsFail:$Strict)) { exit 1 }
        }
        "Rebuild" {
            if (-not (Invoke-WorkspaceDoctor -DoctorScope "build" -WarningsFail:$Strict)) { exit 1 }
            if (-not (Invoke-ManagerAction -ManagerAction "BuildFull")) { exit 1 }
            if (-not (Invoke-WorkspaceDoctor -DoctorScope "products" -WarningsFail:$Strict)) { exit 1 }
        }
        "Play" {
            # Validate every source/private input needed to prepare the build, then build.
            if (-not (Invoke-WorkspaceDoctor -DoctorScope "inputs" -WarningsFail:$Strict)) { exit 1 }
            if (-not (Invoke-WorkspaceDoctor -DoctorScope "build" -WarningsFail:$Strict)) { exit 1 }
            if (-not (Invoke-ManagerAction -ManagerAction "BuildFast")) { exit 1 }
            if (-not (Invoke-WorkspaceDoctor -DoctorScope "products" -WarningsFail:$Strict)) { exit 1 }

            # Re-check the actual runtime closure after the build before launching.
            if (-not (Invoke-WorkspaceDoctor -DoctorScope "run" -WarningsFail:$Strict)) { exit 1 }
            if (-not (Invoke-ManagerAction -ManagerAction "Run")) { exit 1 }
        }
        "Verify" {
            if (-not (Invoke-WorkspaceDoctor -DoctorScope "repo" -WarningsFail:$Strict)) { exit 1 }
            if (-not (Invoke-ManagerAction -ManagerAction "Verify")) { exit 1 }
        }
        "Clean" {
            if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot "tools\nk_clean.py"))) {
                throw "Missing workspace cleaner: tools\nk_clean.py"
            }
            $arguments = @("tools/nk_clean.py")
            if ($Yes) { $arguments += "--yes" }
            & python @arguments | Out-Host
            if ($LASTEXITCODE -ne 0) { exit 1 }
        }
        "Manager" {
            if (-not (Test-Path -LiteralPath $Manager)) {
                throw "Missing NK manager: $Manager"
            }
            $arguments = @("-MsysPath", $MsysPath)
            if ($VulkanSdk) { $arguments += @("-VulkanSdk", $VulkanSdk) }
            if ($TitleManifest) { $arguments += @("-TitleManifest", $TitleManifest) }
            if ($GameName) { $arguments += @("-GameName", $GameName) }
            & $Manager @arguments
        }
    }
} catch {
    Write-Error $_
    exit 1
} finally {
    Set-Location -LiteralPath $OriginalLocation
}
