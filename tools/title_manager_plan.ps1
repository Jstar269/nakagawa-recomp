# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
#requires -Version 7.6

<#
    Strict PowerShell adapter for the versioned title_codegen_plan.py manager plan.

    The Python validator owns public-manifest semantics. These helpers validate the
    bounded machine contract, check the protected-contract digest, and bind the plan's
    title semantics to local private paths. They never evaluate shell text, write a
    manifest, or silently fall back to HST defaults.
#>

$script:TitleManagerPlanVersion = 1

function Get-TitlePlanPropertyNames {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return @() }
    return @($Value.PSObject.Properties | ForEach-Object { $_.Name })
}

function Assert-TitlePlanObject {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Allowed,
        [Parameter(Mandatory = $true)][string[]]$Required
    )
    if ($null -eq $Value -or $Value -is [System.Array] -or $Value -is [string]) {
        throw "$Path must be an object"
    }
    $names = @(Get-TitlePlanPropertyNames $Value)
    $unknown = @($names | Where-Object { $_ -notin $Allowed } | Sort-Object -Unique)
    if ($unknown.Count -gt 0) {
        throw "$Path contains unknown field(s): $($unknown -join ', ')"
    }
    $missing = @($Required | Where-Object { $_ -notin $names })
    if ($missing.Count -gt 0) {
        throw "$Path is missing required field(s): $($missing -join ', ')"
    }
    return $Value
}

function Assert-TitlePlanString {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$AllowEmpty
    )
    if ($null -eq $Value -or $Value -isnot [string]) { throw "$Path must be a string" }
    if (-not $AllowEmpty -and $Value.Length -eq 0) { throw "$Path must not be empty" }
    if ($Value.IndexOfAny([char[]]"`r`n`t") -ge 0) { throw "$Path must not contain control characters" }
    foreach ($char in $Value.ToCharArray()) {
        if ([int][char]$char -lt 0x20) { throw "$Path must not contain control characters" }
    }
    return $Value
}

function Assert-TitlePlanInteger {
    param([AllowNull()][object]$Value, [Parameter(Mandatory = $true)][string]$Path)
    if ($null -eq $Value -or $Value -is [bool] -or $Value -is [string]) {
        throw "$Path must be an integer"
    }
    $typeName = $Value.GetType().FullName
    if ($typeName -notin @(
            "System.Byte", "System.SByte", "System.Int16", "System.UInt16",
            "System.Int32", "System.UInt32", "System.Int64", "System.UInt64"
        )) {
        throw "$Path must be an integer"
    }
    return [int64]$Value
}

function Assert-TitlePlanBoolean {
    param([AllowNull()][object]$Value, [Parameter(Mandatory = $true)][string]$Path)
    if ($Value -isnot [bool]) { throw "$Path must be a boolean" }
    return [bool]$Value
}

function Assert-TitleManagerPlan {
    param([Parameter(Mandatory = $true)][AllowNull()][object]$Plan)

    Assert-TitlePlanObject $Plan '$' @(
        'plan_version', 'plan_kind', 'protected_digest', 'title_manifest_id', 'title_kind',
        'game_name', 'game_base', 'game_entry', 'codegen_profile', 'bss_metadata_source',
        'disc', 'extra_executable_spans', 'required_guest_modules', 'optional_guest_modules',
        'private_binding_requirements', 'environment', 'make'
    ) @(
        'plan_version', 'plan_kind', 'protected_digest', 'title_manifest_id', 'title_kind',
        'game_name', 'game_base', 'game_entry', 'codegen_profile', 'bss_metadata_source',
        'disc', 'extra_executable_spans', 'required_guest_modules', 'optional_guest_modules',
        'private_binding_requirements', 'environment', 'make'
    ) | Out-Null

    if ((Assert-TitlePlanInteger $Plan.plan_version '$.plan_version') -ne $script:TitleManagerPlanVersion) {
        throw "unsupported manager-plan version: $($Plan.plan_version)"
    }
    foreach ($field in @('plan_kind', 'protected_digest', 'title_manifest_id', 'title_kind', 'game_name', 'codegen_profile', 'bss_metadata_source')) {
        Assert-TitlePlanString $Plan.$field "`$.$field" | Out-Null
    }
    [void](Assert-TitlePlanInteger $Plan.game_base '$.game_base')
    [void](Assert-TitlePlanInteger $Plan.game_entry '$.game_entry')

    $disc = $Plan.disc
    if ($null -ne $disc) {
        Assert-TitlePlanObject $disc '$.disc' @('id', 'region', 'revision_policy', 'compatible_revisions') @('id', 'region', 'revision_policy') | Out-Null
        foreach ($field in @('id', 'region', 'revision_policy')) { Assert-TitlePlanString $disc.$field "`$.disc.$field" | Out-Null }
        if ($disc.PSObject.Properties.Name -contains 'compatible_revisions') {
            if ($disc.compatible_revisions -isnot [System.Array]) { throw '$.disc.compatible_revisions must be an array' }
            foreach ($revision in @($disc.compatible_revisions)) { Assert-TitlePlanString $revision '$.disc.compatible_revisions[]' | Out-Null }
        }
    }

    foreach ($field in @('extra_executable_spans', 'required_guest_modules', 'optional_guest_modules')) {
        if ($Plan.$field -isnot [System.Array]) { throw "`$.$field must be an array" }
    }
    $spans = @($Plan.extra_executable_spans)
    for ($index = 0; $index -lt $spans.Count; $index++) {
        $span = $spans[$index]
        Assert-TitlePlanObject $span "`$.extra_executable_spans[$index]" @('start', 'end') @('start', 'end') | Out-Null
        [void](Assert-TitlePlanInteger $span.start "`$.extra_executable_spans[$index].start")
        [void](Assert-TitlePlanInteger $span.end "`$.extra_executable_spans[$index].end")
    }
    foreach ($field in @('required_guest_modules', 'optional_guest_modules')) {
        $fieldModules = @($Plan.$field)
        for ($index = 0; $index -lt $fieldModules.Count; $index++) {
            $module = $fieldModules[$index]
            Assert-TitlePlanObject $module "`$.$field[$index]" @('name', 'load_address') @('name', 'load_address') | Out-Null
            Assert-TitlePlanString $module.name "`$.$field[$index].name" | Out-Null
            [void](Assert-TitlePlanInteger $module.load_address "`$.$field[$index].load_address")
        }
    }

    $bindings = Assert-TitlePlanObject $Plan.private_binding_requirements '$.private_binding_requirements' @('game_elf', 'module_dir', 'psp_header') @('game_elf', 'module_dir', 'psp_header')
    foreach ($field in @('game_elf', 'module_dir', 'psp_header')) { [void](Assert-TitlePlanBoolean $bindings.$field "`$.private_binding_requirements.$field") }

    $environment = Assert-TitlePlanObject $Plan.environment '$.environment' @('GAME_BASE', 'GAME_ENTRY', 'HST_EXTRA_SPANS') @('GAME_BASE', 'GAME_ENTRY', 'HST_EXTRA_SPANS')
    foreach ($field in @('GAME_BASE', 'GAME_ENTRY', 'HST_EXTRA_SPANS')) { Assert-TitlePlanString $environment.$field "`$.environment.$field" -AllowEmpty | Out-Null }

    $make = Assert-TitlePlanObject $Plan.make '$.make' @('game_name', 'game_base', 'game_entry', 'codegen_profile_arg', 'build_dir', 'funcs_per_chunk') @('game_name', 'game_base', 'game_entry', 'codegen_profile_arg', 'build_dir', 'funcs_per_chunk')
    foreach ($field in @('game_name', 'game_base', 'game_entry', 'codegen_profile_arg', 'build_dir')) { Assert-TitlePlanString $make.$field "`$.make.$field" -AllowEmpty | Out-Null }
    $funcs = Assert-TitlePlanInteger $make.funcs_per_chunk '$.make.funcs_per_chunk'
    if ($funcs -lt 1 -or $funcs -gt 100000) { throw '$.make.funcs_per_chunk is outside the supported range' }
    return $Plan
}

# The protected-contract digest of the checked-in HST manifest
# (assets/titles/hst-ucus98701.json), computed by
# tools/title_codegen_plan.py::compute_protected_digest() over the canonical
# validated manifest excluding the free-text notes field. It is the single
# opaque constant the manager adapter compares against instead of re-encoding
# every protected value (base/entry, spans, modules, profile, disc, ...).
# Regenerate with:
#   python tools/title_codegen_plan.py --print-protected-digest \
#       assets/titles/hst-ucus98701.json
$script:HstProtectedContractDigest = '286369bb6de64a21209c38579a27da22ff8eb87215cfd1a0e879264e0d6d446a'

function Get-TitleManifestMakeArgs {
    <#
        Bind a validated manager plan to the HST manager's make invocation.

        The title semantics (base/entry, profile, BSS policy, disc, spans, modules,
        analyzer environment) are consumed from the plan, never re-encoded here. The
        protected-contract digest plus the supported-identity checks fail closed when
        the manifest is anything other than the checked-in HST contract; operational
        inputs (build dir, funcs-per-chunk, private binding paths) are validated for
        consistency with the plan. Legacy HST defaults are never silently selected.
    #>
    param(
        [Parameter(Mandatory = $true)][object]$Plan,
        [Parameter(Mandatory = $true)][string]$GameElfForMake,
        [Parameter(Mandatory = $true)][string]$ModuleDirForMake,
        [Parameter(Mandatory = $true)][string]$PspHeaderForMake,
        [Parameter(Mandatory = $true)][string]$VulkanSdkForMake,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][int]$FuncsPerChunk
    )
    Assert-TitleManagerPlan $Plan | Out-Null
    # The manager is the HST orchestration layer; it supports exactly the checked-in
    # HST retail manifest, identified by its plan kind, manifest id, and game name.
    if ($Plan.plan_kind -ne 'title-manager-build' -or $Plan.title_manifest_id -ne 'hst-ucus98701-v1' -or $Plan.title_kind -ne 'retail' -or $Plan.game_name -ne 'hst') {
        throw 'the HST manager accepts only the checked-in HST retail manifest'
    }
    if ($Plan.protected_digest -ne $script:HstProtectedContractDigest) {
        throw 'HST manager plan protected_digest does not match the checked-in HST title contract'
    }
    if (@($Plan.optional_guest_modules).Count -ne 0) { throw 'HST manager does not support optional guest modules in this slice' }
    if (-not $Plan.private_binding_requirements.game_elf -or -not $Plan.private_binding_requirements.module_dir -or -not $Plan.private_binding_requirements.psp_header) {
        throw 'HST manifest omitted a required private binding'
    }
    # Operational consistency: the plan was built with the same build dir and chunk
    # size the manager is about to use, and the manager's own identity matches.
    if ($Plan.make.game_name -ne 'hst' -or $Plan.make.build_dir -ne ($BuildDir -replace '\\', '/') -or $Plan.make.funcs_per_chunk -ne $FuncsPerChunk) {
        throw 'HST manager plan Make mapping conflicts with the operational inputs'
    }
    foreach ($binding in @($GameElfForMake, $ModuleDirForMake, $PspHeaderForMake, $VulkanSdkForMake)) {
        Assert-TitlePlanString $binding 'private binding' | Out-Null
    }
    $gameElf = $GameElfForMake -replace '\\', '/'
    $moduleDir = $ModuleDirForMake -replace '\\', '/'
    $pspHeader = $PspHeaderForMake -replace '\\', '/'
    if ($moduleDir.Contains('@')) { throw 'module directory must not contain @' }
    $modules = @($Plan.required_guest_modules)
    $extra = @($modules | ForEach-Object { "$moduleDir/$($_.name)@0x$('{0:x8}' -f [uint64]$_.load_address)" }) -join ' '
    $args = @(
        "GAME_NAME=$($Plan.make.game_name)",
        "GAME_ELF=$gameElf",
        "GAME_BASE=$($Plan.make.game_base)",
        "GAME_ENTRY=$($Plan.make.game_entry)",
        "VULKAN_SDK=$($VulkanSdkForMake -replace '\\', '/')",
        "BUILD_DIR=$($Plan.make.build_dir)",
        "CODEGEN_PROFILE_ARG=$($Plan.make.codegen_profile_arg)",
        "GAME_EXTRA_ELFS=`"$extra`"",
        "GAME_PSP_HEADER=$pspHeader",
        "FUNCS_PER_CHUNK=$($Plan.make.funcs_per_chunk)"
    )
    return [pscustomobject]@{ MakeArgs = $args; Environment = $Plan.environment }
}

function Push-TitleAnalyzerEnvironment {
    <#
        Apply the plan's analyzer span and return the exact prior state so it can be
        unwound. Windows cannot hold a defined-but-empty environment variable, so
        "absent" and "empty" are the same observable state and both unwind to removal.
    #>
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)
    $state = [pscustomobject]@{
        Existed = [bool](Test-Path -LiteralPath 'Env:HST_EXTRA_SPANS')
        Value   = $env:HST_EXTRA_SPANS
    }
    $env:HST_EXTRA_SPANS = $Value
    return $state
}

function Pop-TitleAnalyzerEnvironment {
    <# Restore the caller's exact prior value, or remove the variable if it had none. #>
    param([Parameter(Mandatory = $true)][object]$State)
    if ($State.Existed) {
        $env:HST_EXTRA_SPANS = $State.Value
    } else {
        Remove-Item -LiteralPath 'Env:HST_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-TitleManagerPlan {
    param(
        [Parameter(Mandatory = $true)][string]$PlannerScript,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$GameName,
        [Parameter(Mandatory = $true)][string]$GameElf,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$ModuleDir,
        [Parameter(Mandatory = $true)][string]$PspHeader,
        [Parameter(Mandatory = $true)][int]$FuncsPerChunk,
        [string]$PythonCommand = 'python'
    )
    $plannerArgs = @(
        $PlannerScript,
        $ManifestPath,
        '--manager-plan',
        "--game-name=$GameName",
        "--game-elf=$GameElf",
        "--build-dir=$BuildDir",
        "--module-dir=$ModuleDir",
        "--psp-header=$PspHeader",
        "--funcs-per-chunk=$FuncsPerChunk"
    )
    $stderrPath = Join-Path ([IO.Path]::GetTempPath()) ("hst-title-plan-" + [guid]::NewGuid().ToString('N') + '.err')
    try {
        $output = @(& $PythonCommand @plannerArgs 2> $stderrPath)
        $exitCode = [int]$LASTEXITCODE
        if ($exitCode -ne 0) {
            $detail = if (Test-Path -LiteralPath $stderrPath) { (Get-Content -LiteralPath $stderrPath -Raw).Trim() } else { '' }
            if ($detail) { throw "title manager planner failed with exit code ${exitCode}: $detail" }
            throw "title manager planner failed with exit code $exitCode"
        }
        $rendered = ($output -join "`n").Trim()
        if (-not $rendered) { throw 'title manager planner returned no JSON' }
        try { $plan = $rendered | ConvertFrom-Json } catch { throw "invalid planner JSON: $($_.Exception.Message)" }
        Assert-TitleManagerPlan $plan | Out-Null
        return $plan
    } finally {
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
    }
}
