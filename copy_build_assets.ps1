# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.4
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
    [switch]$ExcludeOptionalFonts,
    [string]$Sdl3DllPath = ''
)

New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

$copiedSdl = $false
if ($Sdl3DllPath -and (Test-Path $Sdl3DllPath)) {
    Copy-Item $Sdl3DllPath $BuildDir -Force
    $copiedSdl = $true
}

if (-not $copiedSdl) {
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

# Discover toolchain directory for MinGW/UCRT runtime DLLs if imported
$binDir = if ($gccCmd) { Split-Path $gccCmd.Source } elseif (Test-Path "C:\msys64\ucrt64\bin") { "C:\msys64\ucrt64\bin" } else { '' }

if ($binDir) {
    $runtimeDlls = @('libgcc_s_seh-1.dll', 'libwinpthread-1.dll', 'libstdc++-6.dll')
    foreach ($dll in $runtimeDlls) {
        $srcDll = Join-Path $binDir $dll
        $dstDll = Join-Path $BuildDir $dll
        if ((Test-Path $srcDll) -and (-not (Test-Path $dstDll))) {
            $objdumpCmd = Get-Command objdump -ErrorAction SilentlyContinue
            if ($objdumpCmd) {
                $binaries = Get-ChildItem -Path $BuildDir -File | Where-Object { $_.Extension -in '.exe', '.dll' }
                $imported = $false
                foreach ($bin in $binaries) {
                    $imports = & $objdumpCmd.Source -p $bin.FullName 2>$null | Select-String "DLL Name:\s*$([regex]::Escape($dll))"
                    if ($imports) {
                        $imported = $true
                        break
                    }
                }
                if ($imported) {
                    Copy-Item $srcDll $BuildDir -Force
                }
            }
        }
    }
}

# Generate third-party notices bundle and relink materials
$pyCmd = Get-Command python -ErrorAction SilentlyContinue
$noticesScript = if ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot 'tools\package_notices.py'))) {
    Join-Path $PSScriptRoot 'tools\package_notices.py'
} elseif (Test-Path 'tools/package_notices.py') {
    'tools/package_notices.py'
} elseif (Test-Path '../tools/package_notices.py') {
    '../tools/package_notices.py'
} else {
    ''
}

$hasBinaries = Get-ChildItem -Path $BuildDir -File | Where-Object { $_.Extension -in '.exe', '.dll' }
if ($hasBinaries) {
    # Fail closed: a run directory that ships binaries must also ship their license notices.
    if (-not $pyCmd -or -not $noticesScript) {
        throw "PACKAGE_NOTICES_UNAVAILABLE: python or tools/package_notices.py not found; cannot generate third-party notices for $BuildDir"
    }
    & $pyCmd.Source $noticesScript $BuildDir
    if ($LASTEXITCODE -ne 0) {
        throw "PACKAGE_NOTICES_FAILED: tools/package_notices.py exited with status $LASTEXITCODE"
    }
}
