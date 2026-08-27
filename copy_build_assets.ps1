# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6
#
# copy_build_assets.ps1 — post-link asset copy for the `compile` Makefile target.
#
# Pulled out of the Makefile's inline PowerShell one-liner because nested
# quoting inside that string broke whenever `make` was invoked from a non-cmd.exe shell (Git
# Bash / MSYS2 sh) — the shell's own quote-stripping mangled the embedded PowerShell before
# the PowerShell host ever saw it, producing "Missing condition in if statement after 'if ('".
# Invoked via `-File` instead of `-Command` so argument passing is plain argv, not a
# string embedded inside another shell's command line — no nested-quoting surface at all.
#
# Copies SDL3.dll and font/ into the build output directory, checking both the current
# directory and its parent (Makefile is normally invoked from the repo root, but this
# supports being run one level down too, matching the original one-liner's behavior).

param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [switch]$ExcludeOptionalFonts
)

New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

if (Test-Path 'SDL3.dll') {
    Copy-Item 'SDL3.dll' $BuildDir -Force
} elseif (Test-Path '../SDL3.dll') {
    Copy-Item '../SDL3.dll' $BuildDir -Force
} else {
    $gccCmd = Get-Command gcc -ErrorAction SilentlyContinue
    if ($gccCmd) {
        $binDir = Split-Path $gccCmd.Source
        $toolchainSdl = Join-Path $binDir 'SDL3.dll'
        if (Test-Path $toolchainSdl) {
            Copy-Item $toolchainSdl $BuildDir -Force
        }
    }
}

if (-not $ExcludeOptionalFonts) {
    $fontSrc = if (Test-Path 'font') { 'font' } elseif (Test-Path '../font') { '../font' } else { '' }
    if ($fontSrc) {
        $fontDst = Join-Path $BuildDir 'font'
        if (Test-Path $fontDst) {
            Remove-Item $fontDst -Recurse -Force
        }
        Copy-Item $fontSrc $fontDst -Recurse -Force
    }
}
