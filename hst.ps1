# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Deprecated forwarding wrapper. Use nk.ps1 instead.
.DESCRIPTION
    hst.ps1 is a deprecated forwarding wrapper for nk.ps1 (issue #196 Phase 3).
    All arguments are forwarded verbatim. This file will be removed in the Phase 5
    rename sweep; update any scripts or documentation that reference it.
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
    [string]$VulkanSdk = "",
    [string]$TitleManifest = ""
)

$ErrorActionPreference = "Stop"
Write-Warning "hst.ps1 is deprecated (issue #196). Use nk.ps1 instead."
$NkScript = Join-Path $PSScriptRoot "nk.ps1"
if (-not (Test-Path -LiteralPath $NkScript)) {
    throw "Cannot forward to nk.ps1: file not found at $NkScript"
}
$forwardArgs = @{ Action = $Action; Scope = $Scope; Json = $Json; Strict = $Strict; MsysPath = $MsysPath }
if ($VulkanSdk) { $forwardArgs["VulkanSdk"] = $VulkanSdk }
if ($TitleManifest) { $forwardArgs["TitleManifest"] = $TitleManifest }
& $NkScript @forwardArgs
exit $LASTEXITCODE
