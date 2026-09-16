# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Deprecated forwarding wrapper. Dot-source tools/nk_safety.ps1 instead.
.DESCRIPTION
    hst_safety.ps1 is a deprecated forwarding wrapper for nk_safety.ps1 (issue #196
    Phase 3). Dot-sourcing this file dot-sources nk_safety.ps1 and then exports the
    legacy HST-prefixed function names and variable as aliases/aliases so callers that
    have not yet migrated continue to work during the deprecation window.

    The Assert-HstWorkspaceRoot alias is declared by nk_safety.ps1 itself; this file
    is kept only for callers that explicitly dot-source hst_safety.ps1 by path.

    This file will be removed in the Phase 5 rename sweep; update any scripts or
    documentation that reference it (#196).
#>

Write-Warning "hst_safety.ps1 is deprecated (issue #196). Dot-source tools/nk_safety.ps1 instead."
$_NkSafetyPath = Join-Path $PSScriptRoot "nk_safety.ps1"
if (-not (Test-Path -LiteralPath $_NkSafetyPath)) {
    throw "Cannot forward to nk_safety.ps1: file not found at $_NkSafetyPath"
}
. $_NkSafetyPath
Remove-Variable _NkSafetyPath
