# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Simple, fail-closed entry point for normal Nakagawa Recomp setup and use.
.DESCRIPTION
    Keeps the existing hst_manager.ps1 as the expert/developer console while exposing a
    smaller surface for diagnostics, incremental/full builds, verification, and play.
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("Doctor", "Build", "Rebuild", "Play", "Verify", "Manager")]
    [string]$Action = "Doctor",

    [ValidateSet("repo", "inputs", "build", "products", "run", "all")]
    [string]$Scope = "all",

    [switch]$Json,
    [switch]$Strict,

    [string]$MsysPath = "C:\msys64\ucrt64\bin",
    [string]$VulkanSdk = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = $PSScriptRoot
$Manager = Join-Path $RepoRoot "hst_manager.ps1"
$Doctor = Join-Path $RepoRoot "tools\hst_doctor.py"
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
    if ($AsJson) { $arguments += "--json" }
    if ($WarningsFail) { $arguments += "--strict" }

    & python @arguments | Out-Host
    return ($LASTEXITCODE -eq 0)
}

function Invoke-ManagerAction {
    param([Parameter(Mandatory = $true)][string]$ManagerAction)

    if (-not (Test-Path -LiteralPath $Manager)) {
        throw "Missing HST manager: $Manager"
    }
    $arguments = @("-Action", $ManagerAction, "-MsysPath", $MsysPath)
    if ($VulkanSdk) { $arguments += @("-VulkanSdk", $VulkanSdk) }
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
        "Manager" {
            if (-not (Test-Path -LiteralPath $Manager)) {
                throw "Missing HST manager: $Manager"
            }
            $arguments = @("-MsysPath", $MsysPath)
            if ($VulkanSdk) { $arguments += @("-VulkanSdk", $VulkanSdk) }
            & $Manager @arguments
        }
    }
} catch {
    Write-Error $_
    exit 1
} finally {
    Set-Location -LiteralPath $OriginalLocation
}
