# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Shared Vulkan SDK validation and discovery for the Windows manager.

.DESCRIPTION
    A usable SDK is identified by the files required by the native build, not
    by a directory name alone. Discovery is deliberately ordered: an explicit
    manager argument, VULKAN_SDK, then the newest numerically named usable
    installation below C:\VulkanSDK.
#>

function Test-UsableVulkanSdk {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $false
    }

    try {
        $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    } catch {
        return $false
    }

    $headers = @(
        (Join-Path $resolved "Include\vulkan\vulkan.h"),
        (Join-Path $resolved "include\vulkan\vulkan.h")
    )
    $libraries = @(
        (Join-Path $resolved "Lib\vulkan-1.lib"),
        (Join-Path $resolved "lib\vulkan-1.lib")
    )

    $header = $headers | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    $library = $libraries | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    return [bool]($header -and $library)
}

function Resolve-VulkanSdk {
    [CmdletBinding()]
    param(
        [AllowEmptyString()]
        [string]$ExplicitPath = "",

        [AllowEmptyString()]
        [string]$EnvironmentPath = "",

        [string]$InstallRoot = "C:\VulkanSDK"
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (Test-UsableVulkanSdk -Path $ExplicitPath) {
            return (Resolve-Path -LiteralPath $ExplicitPath -ErrorAction Stop).Path
        }
        throw "Explicit -VulkanSdk path is not a usable Vulkan SDK: $ExplicitPath. It must contain Include\vulkan\vulkan.h and Lib\vulkan-1.lib."
    }

    $environmentCandidate = if (-not [string]::IsNullOrWhiteSpace($EnvironmentPath)) {
        $EnvironmentPath
    } else {
        $env:VULKAN_SDK
    }
    if (-not [string]::IsNullOrWhiteSpace($environmentCandidate)) {
        if (Test-UsableVulkanSdk -Path $environmentCandidate) {
            return (Resolve-Path -LiteralPath $environmentCandidate -ErrorAction Stop).Path
        }
        throw "VULKAN_SDK points to an unusable Vulkan SDK: $environmentCandidate. Clear it, correct it, or pass -VulkanSdk with a current SDK."
    }

    $versioned = @()
    if (Test-Path -LiteralPath $InstallRoot -PathType Container) {
        foreach ($directory in @(Get-ChildItem -LiteralPath $InstallRoot -Directory -ErrorAction SilentlyContinue)) {
            $parsedVersion = $null
            if ([version]::TryParse($directory.Name, [ref]$parsedVersion)) {
                $versioned += [pscustomobject]@{
                    Path    = $directory.FullName
                    Version = $parsedVersion
                }
            }
        }
    }

    foreach ($candidate in ($versioned | Sort-Object Version -Descending)) {
        if (Test-UsableVulkanSdk -Path $candidate.Path) {
            return (Resolve-Path -LiteralPath $candidate.Path -ErrorAction Stop).Path
        }
    }

    throw "No usable Vulkan SDK found. Pass -VulkanSdk <path> or set VULKAN_SDK to a current SDK containing Include\vulkan\vulkan.h and Lib\vulkan-1.lib."
}
