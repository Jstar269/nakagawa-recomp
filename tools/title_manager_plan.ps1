# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
#requires -Version 7.6

<#
    Strict PowerShell adapter for the versioned title_codegen_plan.py manager plan.

    The Python validator owns public-manifest semantics. These helpers validate the
    bounded machine contract, check that its build-facing projections are derived from
    its own semantics, re-check the protected-contract digest against the manifest on
    disk, and bind the plan to local private paths. They never evaluate shell text,
    write a manifest, or fall back to a built-in title default.

    ARCHITECTURAL SEPARATION (this file):
      GENERIC TITLE CONTRACT (host-portable, Windows-neutral):
        - title identifier / title_manifest_id, title_kind, game_name (portable)
        - executable base/entry, codegen_profile, bss_metadata_source
        - extra_executable_spans (one optional span, rendered for analyzer)
        - required/optional guest modules, private_binding_requirements
        - run_entry (portable guest address, validated as 0 or 0x........)
        - environment projections GAME_BASE/GAME_ENTRY/TITLE_EXTRA_SPANS (and legacy
          HST_EXTRA_SPANS alias) and make projections (game_name, base, entry,
          codegen_profile_arg, build_dir, funcs_per_chunk)
        All generic validation (Assert-TitleManagerPlan, Assert-TitlePlanDerivation)
        is title-neutral and host-portable: no UCUS98701, no HST addresses, no .exe
        semantics, no C:\ or Win32 paths, no MSYS2. The plan JSON itself is portable;
        forward-slash rendering is enforced by the Python planner.

      HST PROFILE / ADAPTER (Windows HST compatibility, isolated):
        - Get-HstManifestMakeArgs pins the checked-in HST retail manifest
          (hst-ucus98701-v1, UCUS98701, exact-disc-id, 0-base, hst profile,
          psp-header, 0x00303194-0x00306e24 span, three required modules).
        This is the ONLY place that names HST constants. Generic code never
        compares title_manifest_id to hst-... nor inherits HST modules/spans.
        See the function header for the exact pin set.
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
        'private_binding_requirements', 'run_entry', 'environment', 'make'
    ) @(
        'plan_version', 'plan_kind', 'protected_digest', 'title_manifest_id', 'title_kind',
        'game_name', 'game_base', 'game_entry', 'codegen_profile', 'bss_metadata_source',
        'disc', 'extra_executable_spans', 'required_guest_modules', 'optional_guest_modules',
        'private_binding_requirements', 'run_entry', 'environment', 'make'
    ) | Out-Null

    if ((Assert-TitlePlanInteger $Plan.plan_version '$.plan_version') -ne $script:TitleManagerPlanVersion) {
        throw "unsupported manager-plan version: $($Plan.plan_version)"
    }
    foreach ($field in @('plan_kind', 'protected_digest', 'title_manifest_id', 'title_kind', 'game_name', 'codegen_profile', 'bss_metadata_source')) {
        Assert-TitlePlanString $Plan.$field "`$.$field" | Out-Null
    }
    if ($Plan.protected_digest -cnotmatch '^[0-9a-f]{64}$') {
        throw '$.protected_digest must be a lowercase 64-character SHA-256 hex digest'
    }
    [void](Assert-TitlePlanInteger $Plan.game_base '$.game_base')
    [void](Assert-TitlePlanInteger $Plan.game_entry '$.game_entry')
    # The address a run starts at, rendered the same way Make renders an address. It is
    # not game_entry: a title whose real entry is not compiled names a fallback entry in
    # its runtime bindings, and that is what a run must start at.
    Assert-TitlePlanString $Plan.run_entry '$.run_entry' | Out-Null
    if ($Plan.run_entry -cnotmatch '^(0|0x[0-9a-f]{8})$') {
        throw '$.run_entry must be 0 or a lowercase 0x-prefixed 8-digit guest address'
    }

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

    # GENERIC: environment projections are title-neutral (GAME_BASE/GAME_ENTRY + extra spans).
    # TITLE_EXTRA_SPANS is the host-portable generic key. HST_EXTRA_SPANS is the historic
    # legacy alias that lives ONLY in the explicit HST compatibility layer (Makefile
    # GAME_NAME=hst block and Get-HstManifestMakeArgs); generic plans emit only
    # TITLE_EXTRA_SPANS, and the HST adapter synthesizes the legacy key for legacy Make
    # consumers. Presence of HST_EXTRA_SPANS in a generic plan is allowed for backward
    # compat but must agree with TITLE_EXTRA_SPANS.
    $envAllowed = @('GAME_BASE', 'GAME_ENTRY', 'TITLE_EXTRA_SPANS', 'HST_EXTRA_SPANS')
    $environment = Assert-TitlePlanObject $Plan.environment '$.environment' $envAllowed @('GAME_BASE', 'GAME_ENTRY', 'TITLE_EXTRA_SPANS')
    foreach ($field in @('GAME_BASE', 'GAME_ENTRY', 'TITLE_EXTRA_SPANS')) { Assert-TitlePlanString $environment.$field "`$.environment.$field" -AllowEmpty | Out-Null }
    if ($environment.PSObject.Properties.Name -contains 'HST_EXTRA_SPANS') {
        Assert-TitlePlanString $environment.HST_EXTRA_SPANS '$.environment.HST_EXTRA_SPANS' -AllowEmpty | Out-Null
        if ($environment.HST_EXTRA_SPANS -ne $environment.TITLE_EXTRA_SPANS) {
            throw 'plan environment TITLE_EXTRA_SPANS must agree with HST_EXTRA_SPANS'
        }
    }

    $make = Assert-TitlePlanObject $Plan.make '$.make' @('game_name', 'game_base', 'game_entry', 'codegen_profile_arg', 'build_dir', 'funcs_per_chunk') @('game_name', 'game_base', 'game_entry', 'codegen_profile_arg', 'build_dir', 'funcs_per_chunk')
    foreach ($field in @('game_name', 'game_base', 'game_entry', 'codegen_profile_arg', 'build_dir')) { Assert-TitlePlanString $make.$field "`$.make.$field" -AllowEmpty | Out-Null }
    $funcs = Assert-TitlePlanInteger $make.funcs_per_chunk '$.make.funcs_per_chunk'
    if ($funcs -lt 1 -or $funcs -gt 100000) { throw '$.make.funcs_per_chunk is outside the supported range' }
    return $Plan
}

function Assert-TitlePlanDerivation {
    <#
        GENERIC: Check that the plan's build-facing projections agree with the plan's
        own title semantics. These are *relations*, not pinned constants: the manager
        re-derives each build-facing value from the semantic field it comes from, so
        nothing here re-encodes any title's values, yet a planner that mis-projected a
        manifest still fails closed before Make runs. No HST constant appears here;
        the span check is against the manifest's own extra_executable_spans, and an
        empty set renders as "" rather than an inherited HST span.
    #>
    param([Parameter(Mandatory = $true)][object]$Plan)

    $expectedHexBase = '0x{0:x8}' -f [uint64]$Plan.game_base
    $expectedHexEntry = '0x{0:x8}' -f [uint64]$Plan.game_entry
    if ($Plan.environment.GAME_BASE -ne $expectedHexBase -or $Plan.environment.GAME_ENTRY -ne $expectedHexEntry) {
        throw 'plan analyzer environment does not match the plan executable base/entry'
    }
    # Make renders address 0 as bare "0" and any other address in hex; the manager
    # accepts only that projection of the plan's own numbers.
    $expectedMakeBase = if ($Plan.game_base -eq 0) { '0' } else { $expectedHexBase }
    $expectedMakeEntry = if ($Plan.game_entry -eq 0) { '0' } else { $expectedHexEntry }
    if ($Plan.make.game_base -ne $expectedMakeBase -or $Plan.make.game_entry -ne $expectedMakeEntry) {
        throw 'plan Make base/entry does not match the plan executable base/entry'
    }
    $expectedProfileArg = if ($Plan.codegen_profile -eq 'none') { '' } else { "--profile=$($Plan.codegen_profile)" }
    if ($Plan.make.codegen_profile_arg -ne $expectedProfileArg) {
        throw 'plan Make codegen profile argument does not match the plan codegen profile'
    }
    if ($Plan.make.game_name -ne $Plan.game_name) {
        throw 'plan Make game name does not match the plan game name'
    }
    # The analyzer span is the manifest's extra executable span, rendered for the
    # analyzer seam. Zero spans means the empty string -- never an inherited default.
    # The generic key is TITLE_EXTRA_SPANS; HST_EXTRA_SPANS is checked only when
    # present (HST compatibility).
    $spans = @($Plan.extra_executable_spans)
    $expectedSpanText = ''
    if ($spans.Count -eq 1) {
        $expectedSpanText = ('0x{0:x8},0x{1:x8}' -f [uint64]$spans[0].start, [uint64]$spans[0].end)
    } elseif ($spans.Count -gt 1) {
        throw 'the current analyzer seam accepts at most one explicit extra executable span'
    }
    if ($Plan.environment.TITLE_EXTRA_SPANS -ne $expectedSpanText) {
        throw 'plan analyzer span environment does not match the plan extra executable spans'
    }
    if ($Plan.environment.PSObject.Properties.Name -contains 'HST_EXTRA_SPANS') {
        if ($Plan.environment.HST_EXTRA_SPANS -ne $expectedSpanText) {
            throw 'plan analyzer span environment does not match the plan extra executable spans (HST_EXTRA_SPANS)'
        }
        if ($Plan.environment.HST_EXTRA_SPANS -ne $Plan.environment.TITLE_EXTRA_SPANS) {
            throw 'plan environment TITLE_EXTRA_SPANS must agree with HST_EXTRA_SPANS'
        }
    }
    return $Plan
}

function Assert-TitleManifestDigest {
    <#
        Fail closed when the manifest on disk no longer matches the plan that was built
        from it. The plan is produced once at manager start-up but Make may run minutes
        later, so the digest is recomputed from the manifest file immediately before the
        spawn: a manifest edited in between is rejected rather than silently half-applied.

        The digest is recomputed by the planner itself, so there is exactly one
        implementation of the protected-contract serialization.
    #>
    param(
        [Parameter(Mandatory = $true)][object]$Plan,
        [Parameter(Mandatory = $true)][string]$PlannerScript,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [string]$PythonCommand = 'python'
    )
    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
        throw "title manifest is no longer readable: $ManifestPath"
    }
    $stderrPath = Join-Path ([IO.Path]::GetTempPath()) ("hst-title-digest-" + [guid]::NewGuid().ToString('N') + '.err')
    try {
        $output = @(& $PythonCommand @($PlannerScript, $ManifestPath, '--print-protected-digest') 2> $stderrPath)
        $exitCode = [int]$LASTEXITCODE
        if ($exitCode -ne 0) {
            $detail = if (Test-Path -LiteralPath $stderrPath) { (Get-Content -LiteralPath $stderrPath -Raw).Trim() } else { '' }
            throw "protected-digest recomputation failed with exit code ${exitCode}: $detail"
        }
        $digest = ($output -join '').Trim()
        if ($digest -cnotmatch '^[0-9a-f]{64}$') {
            throw 'protected-digest recomputation did not return a SHA-256 digest'
        }
        if ($digest -ne $Plan.protected_digest) {
            throw "title manifest changed after planning: protected digest $digest does not match the validated plan"
        }
    } finally {
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
    }
    return $Plan
}

function Get-HstManifestMakeArgs {
    <#
        HST PROFILE / ADAPTER: Bind a validated manager plan to the HST manager's Make invocation.

        Every build-facing value (base/entry, profile argument, module list, analyzer
        span, build dir, chunk size) is taken from the plan; the manager keeps no copy
        of the title contract and never falls back to a legacy HST default. The pins
        below are *identity* checks -- this manager orchestrates exactly one title, so
        it refuses any other plan before touching a private path.

        This is the ONLY generic-title-truth boundary that names HST constants:
          HST disc identity / region / exact-disc-id
          game_base==0 / game_entry==0 / HST profile / psp-header
          HST span (3158420-3173924) / HST module addresses etc.
        Generic helpers (Assert-TitleManagerPlan, Assert-TitlePlanDerivation,
        Assert-TitleManifestDigest) never name these values.
    #>
    param(
        [Parameter(Mandatory = $true)][object]$Plan,
        [Parameter(Mandatory = $true)][string]$GameElfForMake,
        [Parameter(Mandatory = $true)][string]$ModuleDirForMake,
        [Parameter(Mandatory = $true)][string]$PspHeaderForMake,
        [Parameter(Mandatory = $true)][string]$VulkanSdkForMake,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][int]$FuncsPerChunk,
        [Parameter(Mandatory = $true)][string]$TitleManifestForMake
    )
    Assert-TitleManagerPlan $Plan | Out-Null
    # Every build-facing value below is consumed from the plan. The manager re-derives
    # nothing of its own: it only checks that the plan's projections agree with the
    # plan's semantics, then pins the one title this manager orchestrates.
    Assert-TitlePlanDerivation $Plan | Out-Null
    if ($Plan.plan_kind -ne 'title-manager-build' -or $Plan.title_manifest_id -ne 'hst-ucus98701-v1' -or $Plan.title_kind -ne 'retail' -or $Plan.game_name -ne 'hst') {
        throw 'the HST manager accepts only the checked-in HST retail manifest'
    }
    if ($Plan.game_base -ne 0 -or $Plan.game_entry -ne 0 -or $Plan.codegen_profile -ne 'hst' -or $Plan.bss_metadata_source -ne 'psp-header') {
        throw 'HST manifest protected executable semantics are incompatible'
    }
    $disc = $Plan.disc
    if ($null -eq $disc -or $disc.id -ne 'UCUS98701' -or $disc.region -ne 'NA' -or $disc.revision_policy -ne 'exact-disc-id' -or ($disc.PSObject.Properties.Name -contains 'compatible_revisions')) {
        throw 'HST manifest protected disc identity/revision policy is incompatible'
    }
    # Supported-identity pin for the one title this manager orchestrates. The span's
    # projection into the analyzer environment is checked by Assert-TitlePlanDerivation
    # against this same field, so the value is stated once here and nowhere else.
    $expectedSpans = @(@{ start = 3158420; end = 3173924 })
    $actualSpans = @($Plan.extra_executable_spans)
    if ($actualSpans.Count -ne 1 -or $actualSpans[0].start -ne $expectedSpans[0].start -or $actualSpans[0].end -ne $expectedSpans[0].end) {
        throw 'HST manifest protected extra executable span is incompatible'
    }
    $expectedModules = @(
        @{ name = 'libfont.prx'; load_address = 840957952 },
        @{ name = 'scePsmf_library.prx'; load_address = 841482240 },
        @{ name = 'scePsmfP_library.prx'; load_address = 841975912 }
    )
    $modules = @($Plan.required_guest_modules)
    if ($modules.Count -ne $expectedModules.Count) { throw 'HST manifest required guest modules are incomplete' }
    for ($index = 0; $index -lt $expectedModules.Count; $index++) {
        if ($modules[$index].name -ne $expectedModules[$index].name -or $modules[$index].load_address -ne $expectedModules[$index].load_address) {
            throw 'HST manifest required guest module name/load address conflicts with the HST contract'
        }
    }
    if (@($Plan.optional_guest_modules).Count -ne 0) { throw 'HST manager does not support optional guest modules in this slice' }
    if (-not $Plan.private_binding_requirements.game_elf -or -not $Plan.private_binding_requirements.module_dir -or -not $Plan.private_binding_requirements.psp_header) {
        throw 'HST manifest omitted a required private binding'
    }
    # Operational consistency only: the plan was built for the same build directory and
    # chunk size the manager is about to use. The Make base/entry/profile projections are
    # already tied to the plan's semantics by Assert-TitlePlanDerivation, so they are not
    # re-encoded here.
    if ($Plan.make.build_dir -ne ($BuildDir -replace '\\', '/') -or $Plan.make.funcs_per_chunk -ne $FuncsPerChunk) {
        throw 'HST manager plan Make mapping conflicts with the operational inputs'
    }
    foreach ($binding in @($GameElfForMake, $ModuleDirForMake, $PspHeaderForMake, $VulkanSdkForMake, $TitleManifestForMake)) {
        Assert-TitlePlanString $binding 'private binding' | Out-Null
    }
    $gameElf = $GameElfForMake -replace '\\', '/'
    $moduleDir = $ModuleDirForMake -replace '\\', '/'
    $pspHeader = $PspHeaderForMake -replace '\\', '/'
    if ($moduleDir.Contains('@')) { throw 'module directory must not contain @' }
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
        "FUNCS_PER_CHUNK=$($Plan.make.funcs_per_chunk)",
        # The same validated manifest also supplies the compiled runtime's title
        # bindings. Without it the runtime objects build generically, so this is the
        # only path by which a title's addresses reach src/rt.
        "TITLE_MANIFEST=$($TitleManifestForMake -replace '\\', '/')"
    )
    # RunEntry travels with the Make args because it comes from the same validated
    # plan: it is the guest address a run of THIS title starts at, so the manager
    # never needs a copy of it.
    # HST compatibility: synthesize the legacy HST_EXTRA_SPANS alias from the generic
    # TITLE_EXTRA_SPANS so legacy Make (which still reads HST_EXTRA_SPANS) and the
    # analyzer get the same value. Generic plans emit only TITLE_EXTRA_SPANS.
    $envForHst = [pscustomobject]@{
        GAME_BASE = $Plan.environment.GAME_BASE
        GAME_ENTRY = $Plan.environment.GAME_ENTRY
        TITLE_EXTRA_SPANS = $Plan.environment.TITLE_EXTRA_SPANS
        HST_EXTRA_SPANS = $Plan.environment.TITLE_EXTRA_SPANS
    }
    return [pscustomobject]@{ MakeArgs = $args; Environment = $envForHst; RunEntry = $Plan.run_entry }
}

function Push-TitleAnalyzerEnvironment {
    <#
        Apply the plan's analyzer span and return the exact prior state so it can be
        unwound. Windows cannot hold a defined-but-empty environment variable, so
        "absent" and "empty" are the same observable state and both unwind to removal.
        Setting to "" removes the variable (Test-Path returns False).

        GENERIC: scopes only TITLE_EXTRA_SPANS. HST-specific callers that still need
        the legacy HST_EXTRA_SPANS must use Push-HstAnalyzerEnvironment, which scopes
        both variables together. Generic titles never set HST_EXTRA_SPANS.
    #>
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)
    $state = [pscustomobject]@{
        TitleExisted = [bool](Test-Path -LiteralPath 'Env:TITLE_EXTRA_SPANS')
        TitleValue   = $env:TITLE_EXTRA_SPANS
        # Backward compat for callers that accessed .Existed/.Value
        Existed = [bool](Test-Path -LiteralPath 'Env:TITLE_EXTRA_SPANS')
        Value   = $env:TITLE_EXTRA_SPANS
    }
    if ([string]::IsNullOrEmpty($Value)) {
        Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue
    } else {
        $env:TITLE_EXTRA_SPANS = $Value
    }
    return $state
}

function Pop-TitleAnalyzerEnvironment {
    <# Restore the caller's exact prior TITLE_EXTRA_SPANS, or remove if it was absent/empty. #>
    param([Parameter(Mandatory = $true)][object]$State)
    $titleExisted = if ($State.PSObject.Properties.Name -contains 'TitleExisted') { $State.TitleExisted } elseif ($State.PSObject.Properties.Name -contains 'Existed') { $State.Existed } else { $false }
    $titleValue   = if ($State.PSObject.Properties.Name -contains 'TitleValue')   { $State.TitleValue }   elseif ($State.PSObject.Properties.Name -contains 'Value')   { $State.Value }   else { $null }
    # Windows env: empty and absent are the same (setting to "" removes). Restore empty as Remove-Item.
    if ($titleExisted -and -not [string]::IsNullOrEmpty($titleValue)) {
        $env:TITLE_EXTRA_SPANS = $titleValue
    } elseif ($titleExisted -and [string]::IsNullOrEmpty($titleValue)) {
        # Previously existed but value was empty (which PowerShell stores as absent). Ensure removed.
        Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue
    } else {
        Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue
    }
}

function Push-HstAnalyzerEnvironment {
    <#
        HST COMPATIBILITY: Apply the HST analyzer span and return prior state for both
        variables. This is the ONLY generic-title-path-adjacent code that touches
        HST_EXTRA_SPANS; generic helpers never set it. HST builds set both variables
        to the same value so legacy Make (HST_EXTRA_SPANS) and future analyzer
        (TITLE_EXTRA_SPANS) see the same span.
    #>
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)
    $state = [pscustomobject]@{
        HstExisted   = [bool](Test-Path -LiteralPath 'Env:HST_EXTRA_SPANS')
        HstValue     = $env:HST_EXTRA_SPANS
        TitleExisted = [bool](Test-Path -LiteralPath 'Env:TITLE_EXTRA_SPANS')
        TitleValue   = $env:TITLE_EXTRA_SPANS
        Existed = [bool](Test-Path -LiteralPath 'Env:HST_EXTRA_SPANS')
        Value   = $env:HST_EXTRA_SPANS
    }
    if ([string]::IsNullOrEmpty($Value)) {
        Remove-Item -LiteralPath 'Env:HST_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue
    } else {
        $env:HST_EXTRA_SPANS = $Value
        $env:TITLE_EXTRA_SPANS = $Value
    }
    return $state
}

function Pop-HstAnalyzerEnvironment {
    <# Restore both HST and generic span variables. #>
    param([Parameter(Mandatory = $true)][object]$State)
    $hstExisted = if ($State.PSObject.Properties.Name -contains 'HstExisted') { $State.HstExisted } elseif ($State.PSObject.Properties.Name -contains 'Existed') { $State.Existed } else { $false }
    $hstValue   = if ($State.PSObject.Properties.Name -contains 'HstValue')   { $State.HstValue }   elseif ($State.PSObject.Properties.Name -contains 'Value')   { $State.Value }   else { $null }
    $titleExisted = if ($State.PSObject.Properties.Name -contains 'TitleExisted') { $State.TitleExisted } else { $hstExisted }
    $titleValue   = if ($State.PSObject.Properties.Name -contains 'TitleValue')   { $State.TitleValue }   else { $hstValue }
    if ($hstExisted -and -not [string]::IsNullOrEmpty($hstValue)) {
        $env:HST_EXTRA_SPANS = $hstValue
    } else {
        Remove-Item -LiteralPath 'Env:HST_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue
    }
    if ($titleExisted -and -not [string]::IsNullOrEmpty($titleValue)) {
        $env:TITLE_EXTRA_SPANS = $titleValue
    } else {
        Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue
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
