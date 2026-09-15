# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Canonical, title-agnostic recompiler tool for static rebuilds, environment
    profile execution, and verification.
.DESCRIPTION
    Authoritative orchestration layer for Nakagawa Recomp (nk). Wraps compilation
    tasks, interactive environment controls, visual regression oracle, and deep
    trace parsing. All title bindings and targets derive dynamically from validated
    title manifests.
#>

Param(
    [Parameter(Mandatory=$false)]
    [ValidateSet("BuildFull", "BuildFast", "Run", "Inspect", "Clean", "Test", "Verify", "DiffFunc", "FindSymbol", "Fuzz", "VisualOracle")]
    [string]$Action,

    # VisualOracle: deterministic route replay for a visual regression oracle.
    # -Route <pad file>, -ExitAtVblank <N> (stop once the guest delivers N vblanks),
    # -SnapEvery <N> / -SnapAfter <N> (capture only around the transition of interest),
    # -OracleName <tag> (archive directory under logs/). See docs/DEBUGGING.md.
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

    # Two-window capture: "<a>-<b>,<c>-<d>". A visual comparison needs both ends of a
    # transition from the SAME run.
    [Parameter(Mandatory=$false)]
    [ValidatePattern('^\s*$|^\d+-\d+(,\d+-\d+)*$')]
    [string]$SnapWindows,

    # Hold the guest's save state still across replays.
    [Parameter(Mandatory=$false)]
    [string]$SaveBase,

    # OracleName becomes a single path component under logs/ (oracle_<name>).
    [Parameter(Mandatory=$false)]
    [ValidatePattern('^$|^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')]
    [string]$OracleName,

    [Parameter(Mandatory=$false)]
    [switch]$OverwriteOracle,

    # Run deadline in seconds; 0 = indefinite (interactive).
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
    [string]$DiffTarget,          # e.g. f_00010738 - function to diff against oracle

    [Parameter(Mandatory=$false)]
    [string]$DiffOracle,          # path to reference .trace file for DiffFunc

    [Parameter(Mandatory=$false)]
    [int]$DiffStep = 0,           # entry step in the oracle trace for DiffFunc

    [Parameter(Mandatory=$false)]
    [string]$FindName,            # symbol name or hex address for FindSymbol

    [Parameter(Mandatory=$false)]
    [string]$MsysPath = "C:\msys64\ucrt64\bin",

    # Optional explicit make path for hermetic/non-MSYS2 callers.
    [Parameter(Mandatory=$false)]
    [string]$MakeExecutable = "",

    [Parameter(Mandatory=$false)]
    [string]$VulkanSdk = "",

    # Optional compile-profile overrides.
    [Parameter(Mandatory=$false)]
    [ValidateSet("O0", "O1", "O2")]
    [string]$RuntimeOpt,

    [Parameter(Mandatory=$false)]
    [ValidateSet("O0", "O1", "O2")]
    [string]$RecompOpt,

    [Parameter(Mandatory=$false)]
    [ValidateRange(1, 1000000)]
    [int]$FuncsPerChunk = 0,

    # Parallel make jobs for build actions. 0 preserves legacy single-job invocation.
    [Parameter(Mandatory=$false)]
    [ValidateRange(0, 1024)]
    [int]$Jobs = 0,

    # Title configuration manifest. Defaults to safe synthetic fixture when omitted.
    [Parameter(Mandatory=$false)]
    [string]$TitleManifest = "assets/titles/synthetic.json",

    # Optional title name override. Derived from manifest if not provided.
    [Parameter(Mandatory=$false)]
    [string]$GameName = ""
)

# -----------------------------------------------------------------------------
# Safe Environment Configuration & Error Trapping
# -----------------------------------------------------------------------------
$ErrorActionPreference = "Stop"

$script:ManagerExitCode = 0
$script:OriginalLocation = $null

# -Jobs 0 (unset) preserves legacy single-job invocation.
$script:EffectiveJobs = if ($Jobs -gt 0) { $Jobs } else { 1 }

$script:TitleManagerPlan = $null
$script:TitleManagerMakeArgs = $null
$script:TitleManagerSpans = $null
$script:TitleManagerRunEntry = $null
$script:LastBuildInfo = $null
$VulkanDiscovery = Join-Path $PSScriptRoot "tools\vulkan_sdk.ps1"

function Safe-ClearHost {
    try {
        Clear-Host
    } catch {
        Write-Host "`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n`n"
    }
}

try {
    # ---------------------------------------------------------------------
    # Repository-root identity (#183). Anchored to the manager script root.
    # ---------------------------------------------------------------------
    $script:OriginalLocation = (Get-Location).Path
    $SafetySupport = Join-Path $PSScriptRoot "tools\hst_safety.ps1"
    if (-not (Test-Path -LiteralPath $SafetySupport -PathType Leaf)) {
        throw "Missing required helper: $SafetySupport"
    }
    . $SafetySupport
    $script:RepoRoot = Assert-HstWorkspaceRoot -Root $PSScriptRoot
    Set-Location -LiteralPath $script:RepoRoot

    if (-not (Test-Path -LiteralPath $VulkanDiscovery -PathType Leaf)) {
        throw "Missing Vulkan SDK discovery helper: $VulkanDiscovery"
    }
    . $VulkanDiscovery

    # Ensure title manifest defaults to synthetic if empty
    if ([string]::IsNullOrWhiteSpace($TitleManifest)) {
        $TitleManifest = "assets/titles/synthetic.json"
    }

    $TitlePlanSupport = Join-Path $PSScriptRoot "tools\title_manager_plan.ps1"
    $hasManifest = (Test-Path -LiteralPath $TitleManifest -PathType Leaf)
    $hasTitlePlan = (Test-Path -LiteralPath $TitlePlanSupport -PathType Leaf)

    # Initialize MSYS path elements
    if ($env:Path -notlike "*$MsysPath*") {
        $env:Path = "$MsysPath$([IO.Path]::PathSeparator)" + $env:Path
    }

    $VulkanSdk = Resolve-VulkanSdk -ExplicitPath $VulkanSdk -EnvironmentPath $env:VULKAN_SDK
    Write-Host "Using Vulkan SDK: $VulkanSdk" -ForegroundColor DarkGray
    $VulkanSdkForMake = $VulkanSdk -replace "\\", "/"

    if (-not $hasManifest -or -not $hasTitlePlan) {
        if ($Action -eq "Clean") {
            $script:ActiveGameName = if ($GameName) { $GameName } else { "recomp" }
            $script:ManifestId = "none"
            $script:IsRetail = $false
            $BuildDirForMake = "build/$script:ActiveGameName"
            $BuildDir = "build\$script:ActiveGameName"
            $isWin = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)
            $ExecutableName = if ($isWin) { "$($script:ActiveGameName).exe" } else { $script:ActiveGameName }
            $ExePath = Join-Path $BuildDir $ExecutableName
            $ImagePath = Join-Path $BuildDir "$($script:ActiveGameName)_image.bin"
        } else {
            if (-not $hasManifest) {
                throw "Title manifest was not found: $TitleManifest"
            }
            throw "Missing title manager planning helper: $TitlePlanSupport"
        }
    } else {
        . $TitlePlanSupport

        # Read manifest metadata
        $manifestRaw = Get-Content -LiteralPath $TitleManifest -Raw -Encoding utf8
        $manifestJson = $manifestRaw | ConvertFrom-Json
        $script:ManifestId = $manifestJson.id
        $script:IsRetail = ($manifestJson.kind -eq "retail")

        # Determine GameName (issue #196 Phase 4). Order: explicit -GameName,
        # then the manifest's own game_name declaration, then a portable
        # derivation from the manifest id. No manifest-id prefix ever mints a
        # built-in title name: the manager cannot know what a title wants to be
        # called, and a prefix rule is title coupling in generic tooling. The
        # deprecated hst_manager.ps1 wrapper declares -GameName hst explicitly
        # for the legacy default route.
        if ([string]::IsNullOrWhiteSpace($GameName)) {
            $manifestProps = @($manifestJson.PSObject.Properties.Name)
            if (($manifestProps -contains 'game_name') -and -not [string]::IsNullOrWhiteSpace($manifestJson.game_name)) {
                $GameName = [string]$manifestJson.game_name
            } elseif ($manifestJson.id) {
                $GameName = ($manifestJson.id -replace '-v\d+$', '') -replace '[^a-zA-Z0-9_.-]', '_'
            } else {
                $GameName = "recomp"
            }
        }
        if ($GameName -notmatch '^[a-z0-9][a-z0-9._-]*$') {
            throw "GameName '$GameName' is not a portable build identifier (must match ^[a-z0-9][a-z0-9._-]*$; same contract as the codegen planner)"
        }
        $script:ActiveGameName = $GameName

        # Title input layout (issue #196 Phase 4): a manifest DECLARES where its
        # private inputs live; generic code paths never assume a layout. The
        # legacy input layout is consulted only for retail manifests (the HST
        # adapter compatibility surface) and only where the manifest did not
        # declare the location.
        $script:LegacyInputLayout = ($manifestJson.kind -eq "retail")
        $script:TitleDataRoot = $null
        $script:TitleModuleDir = $null
        $script:TitlePspHeader = $null
        $script:TitleDiscImage = $null
        $fsProps = @()
        if ($manifestJson.filesystem) { $fsProps = @($manifestJson.filesystem.PSObject.Properties.Name) }
        foreach ($decl in @(
            @{ Key = 'data_root'; Var = 'TitleDataRoot' },
            @{ Key = 'module_dir'; Var = 'TitleModuleDir' },
            @{ Key = 'psp_header'; Var = 'TitlePspHeader' },
            @{ Key = 'disc_image'; Var = 'TitleDiscImage' }
        )) {
            if (($fsProps -contains $decl.Key) -and -not [string]::IsNullOrWhiteSpace($manifestJson.filesystem.($decl.Key))) {
                $value = [string]$manifestJson.filesystem.($decl.Key)
                if ($value -notmatch '^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$') {
                    throw "Title manifest filesystem.$($decl.Key) must be a relative path with forward slashes: $value"
                }
                Set-Variable -Name "script:$($decl.Var)" -Value $value
            }
        }

        $BuildDirForMake = "build/$GameName"
        $BuildDir = "build\$GameName"
        $isWin = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)
        $ExecutableName = if ($isWin) { "$GameName.exe" } else { $GameName }
        $ExePath = Join-Path $BuildDir $ExecutableName
        $ImagePath = Join-Path $BuildDir "$($GameName)_image.bin"

        # Private input discovery (issue #196 Phase 4): manifest declarations
        # first, then (retail/legacy-layout only) the legacy layout. Generic and
        # synthetic titles get no implicit input location.
        $GameElfPath = $null
        if ($script:IsRetail) {
            foreach ($candidate in @("place_game_here\EBOOT.elf", "eboot.elf")) {
                if (Test-Path -LiteralPath $candidate) { $GameElfPath = $candidate; break }
            }
        } else {
            $candidates = @()
            if ($manifestJson.executable -and $manifestJson.executable.path) {
                $candidates += $manifestJson.executable.path
            }
            $candidates += @(
                "build\fixtures\$GameName.elf",
                "fixtures\$GameName.elf",
                "eboot.elf"
            )
            foreach ($candidate in $candidates) {
                if (Test-Path -LiteralPath $candidate) { $GameElfPath = $candidate; break }
            }
        }

        $GameIsoPath = $null
        if ($script:TitleDiscImage) {
            if (Test-Path -LiteralPath $script:TitleDiscImage -PathType Leaf) { $GameIsoPath = $script:TitleDiscImage }
        } elseif (Test-Path -LiteralPath "game.iso") {
            $GameIsoPath = "game.iso"
        } elseif ($script:LegacyInputLayout) {
            $isoCandidates = @(Get-ChildItem -LiteralPath "place_game_here\ISO" -File -Filter "*.iso" -ErrorAction SilentlyContinue)
            if ($isoCandidates.Count -eq 1) {
                $GameIsoPath = $isoCandidates[0].FullName
            }
        }

        $GameElfForMake = if ($GameElfPath) { $GameElfPath -replace "\\", "/" } else { "eboot.elf" }
        $ModuleDirPath = $null
        if ($script:TitleModuleDir) {
            $ModuleDirPath = $script:TitleModuleDir
        } elseif ($script:LegacyInputLayout -and (Test-Path -LiteralPath "place_game_here\EXTRACTED\decrypted" -PathType Container)) {
            $ModuleDirPath = "place_game_here\EXTRACTED\decrypted"
        }
        $ModuleDirForMake = if ($ModuleDirPath) { $ModuleDirPath -replace "\\", "/" } else { $null }
        $PspHeaderPath = $null
        if ($script:TitlePspHeader) {
            $PspHeaderPath = $script:TitlePspHeader
        } elseif ($script:LegacyInputLayout -and (Test-Path -LiteralPath "place_game_here\EXTRACTED\PSP_GAME\SYSDIR\EBOOT.BIN" -PathType Leaf)) {
            $PspHeaderPath = "place_game_here\EXTRACTED\PSP_GAME\SYSDIR\EBOOT.BIN"
        }
        $PspHeaderForMake = if ($PspHeaderPath) { $PspHeaderPath -replace "\\", "/" } else { $null }

        # Title planning via title_codegen_plan.py
        $plannerScript = Join-Path $PSScriptRoot "tools\title_codegen_plan.py"
        $effectiveFuncsPerChunk = if ($FuncsPerChunk -gt 0) { $FuncsPerChunk } else { 2000 }
        $needsPspHeader = ($manifestJson.executable -and $manifestJson.executable.bss_metadata_source -eq "psp-header")
        $needsModuleDir = $false
        if ($manifestJson.modules) {
            foreach ($mod in @($manifestJson.modules)) {
                if ($mod.role -eq "guest-prx") {
                    $needsModuleDir = $true
                    break
                }
            }
        }

        $plannerArgs = @(
            $plannerScript,
            $TitleManifest,
            '--manager-plan',
            "--game-name=$GameName",
            "--game-elf=$GameElfForMake",
            "--build-dir=$BuildDirForMake",
            "--funcs-per-chunk=$effectiveFuncsPerChunk"
        )
        if ($needsModuleDir -and $ModuleDirForMake) { $plannerArgs += "--module-dir=$ModuleDirForMake" }
        if ($needsPspHeader -and $PspHeaderForMake) { $plannerArgs += "--psp-header=$PspHeaderForMake" }

        $stderrPath = Join-Path ([IO.Path]::GetTempPath()) ("nk-title-plan-" + [guid]::NewGuid().ToString('N') + '.err')
        try {
            $output = @(& python @plannerArgs 2> $stderrPath)
            $planExit = [int]$LASTEXITCODE
            if ($planExit -ne 0) {
                $detail = if (Test-Path -LiteralPath $stderrPath) { (Get-Content -LiteralPath $stderrPath -Raw).Trim() } else { '' }
                throw "title manager planner failed with exit code ${planExit}: $detail"
            }
            $script:TitleManagerPlan = ($output -join "`n").Trim() | ConvertFrom-Json
            Assert-TitleManagerPlan $script:TitleManagerPlan | Out-Null
            Assert-TitlePlanDerivation $script:TitleManagerPlan | Out-Null
        } finally {
            Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
        }

        # Bind Make arguments: delegate to HST adapter for retail HST profile, or use generic contract
        if ($script:TitleManagerPlan.title_kind -eq 'retail' -and $script:TitleManagerPlan.codegen_profile -eq 'hst') {
            if (-not $ModuleDirForMake -or -not $PspHeaderForMake) {
                throw "HST adapter requires module_dir/psp_header input locations (declare them in the title manifest's filesystem block, or provide the legacy retail input layout)"
            }
            $boundPlan = Get-HstManifestMakeArgs `
                -Plan $script:TitleManagerPlan `
                -GameElfForMake $GameElfForMake `
                -ModuleDirForMake $ModuleDirForMake `
                -PspHeaderForMake $PspHeaderForMake `
                -VulkanSdkForMake $VulkanSdkForMake `
                -BuildDir $BuildDirForMake `
                -FuncsPerChunk $effectiveFuncsPerChunk `
                -TitleManifestForMake $TitleManifest
            $script:TitleManagerMakeArgs = @($boundPlan.MakeArgs)
            $script:TitleManagerSpans = $boundPlan.Environment.HST_EXTRA_SPANS
            $script:TitleManagerRunEntry = $boundPlan.RunEntry
        } else {
            $modules = @($script:TitleManagerPlan.required_guest_modules)
            $extra = ""
            if ($modules.Count -gt 0) {
                $extra = @($modules | ForEach-Object { "$ModuleDirForMake/$($_.name)@0x$('{0:x8}' -f [uint64]$_.load_address)" }) -join ' '
            }
            $mArgs = @(
                "GAME_NAME=$($script:TitleManagerPlan.make.game_name)",
                "GAME_ELF=$GameElfForMake",
                "GAME_BASE=$($script:TitleManagerPlan.make.game_base)",
                "GAME_ENTRY=$($script:TitleManagerPlan.make.game_entry)",
                "VULKAN_SDK=$VulkanSdkForMake",
                "BUILD_DIR=$($script:TitleManagerPlan.make.build_dir)",
                "CODEGEN_PROFILE_ARG=$($script:TitleManagerPlan.make.codegen_profile_arg)",
                "FUNCS_PER_CHUNK=$($script:TitleManagerPlan.make.funcs_per_chunk)",
                "TITLE_MANIFEST=$($TitleManifest -replace '\\', '/')"
            )
            if ($extra) { $mArgs += "GAME_EXTRA_ELFS=`"$extra`"" }
            if ($needsPspHeader) { $mArgs += "GAME_PSP_HEADER=$PspHeaderForMake" }
            $script:TitleManagerMakeArgs = $mArgs
            $script:TitleManagerSpans = $script:TitleManagerPlan.environment.TITLE_EXTRA_SPANS
            $script:TitleManagerRunEntry = $script:TitleManagerPlan.run_entry
        }
        Write-Host "Using title manifest: $($script:TitleManagerPlan.title_manifest_id) (target: $GameName)" -ForegroundColor DarkGray
    }

    function Get-NkRunEntry {
        if ($script:TitleManagerRunEntry) { return $script:TitleManagerRunEntry }
        throw "No run entry point resolved by the active title manifest."
    }

    # Backward compatibility alias
    function Get-HstRunEntry { return Get-NkRunEntry }

    function Get-NkMakeBaseArgs {
        $args = @($script:TitleManagerMakeArgs)
        if ($RuntimeOpt) { $args += "RUNTIME_OPT=-$RuntimeOpt" }
        if ($RecompOpt) { $args += "RECOMP_OPT=-$RecompOpt" }
        return $args
    }

    # Backward compatibility alias
    function Get-HstMakeBaseArgs { return Get-NkMakeBaseArgs }

    # Log directory
    $LogDir = Join-Path $script:RepoRoot "logs"
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
    $script:LogDir = $LogDir

    $script:BuildPidFile = Join-Path $LogDir ".build_pids"
    $script:BuildToolNames = @(
        "make", "mingw32-make", "gcc", "g++", "c++", "cc1", "cc1plus",
        "collect2", "as", "ld", "ld.lld", "lld", "lld-link", "cpp", "windres", "ar"
    )

    $RunSupport = Join-Path $PSScriptRoot "tools\hst_run_support.ps1"
    if (-not (Test-Path -LiteralPath $RunSupport)) {
        throw "Missing required helper: $RunSupport"
    }
    . $RunSupport

    function Register-BuildProcess {
        param([int]$ProcessId)
        if ($ProcessId -le 0) { return }
        $rec = Get-ProcessIdentityRecord -Id $ProcessId
        if ($null -eq $rec) {
            Write-Host "[!] Could not capture identity for build process $ProcessId; not recording it." -ForegroundColor Yellow
            return
        }
        Add-Content -LiteralPath $script:BuildPidFile -Value ($rec | ConvertTo-Json -Compress) -Encoding utf8 -ErrorAction SilentlyContinue
    }

    function Unregister-BuildProcess {
        param([int]$ProcessId)
        if (-not (Test-Path -LiteralPath $script:BuildPidFile)) { return }
        $parsed = Get-BuildPidRecords -Path $script:BuildPidFile
        $kept = @($parsed.Records | Where-Object { [int]$_.pid -ne $ProcessId })
        if ($kept.Count -gt 0) {
            $kept | ForEach-Object { $_ | ConvertTo-Json -Compress } |
                Set-Content -LiteralPath $script:BuildPidFile -Encoding utf8 -ErrorAction SilentlyContinue
        } else {
            Remove-Item -LiteralPath $script:BuildPidFile -Force -ErrorAction SilentlyContinue
        }
    }

    function Stop-WorkspaceTarget {
        $exeFull = $null
        try {
            $targetPath = Join-Path $script:RepoRoot $ExePath
            $rp = Resolve-Path -LiteralPath $targetPath -ErrorAction SilentlyContinue
            if ($rp) { $exeFull = [IO.Path]::GetFullPath($rp.Path) }
        } catch { }
        if (-not $exeFull) { return }
        Get-Process -Name $script:ActiveGameName -ErrorAction SilentlyContinue | Where-Object {
            $ppath = $null
            try { $ppath = [IO.Path]::GetFullPath($_.Path) } catch { $ppath = $null }
            $ppath -and ($ppath -ieq $exeFull)
        } | Stop-Process -Force -ErrorAction SilentlyContinue
    }

    # Backward compatibility alias
    function Stop-WorkspaceHst { Stop-WorkspaceTarget }

    function Stop-BuildProcesses {
        Write-Host "Clearing this workspace's stale build/runtime processes..." -ForegroundColor Yellow
        $cleanup = Invoke-StaleBuildCleanup -PidFile $script:BuildPidFile `
            -WorkspaceRoot $script:RepoRoot -BuildToolNames $script:BuildToolNames
        if ($cleanup.Malformed.Count -gt 0) {
            Write-Host "[!] Discarding $($cleanup.Malformed.Count) legacy/unverifiable build-PID record(s); nothing was killed on their account." -ForegroundColor Yellow
        }
        Stop-WorkspaceTarget
        Start-Sleep -Milliseconds 250
    }

    function Find-MakeExecutable {
        if ($MakeExecutable) {
            $explicit = Get-Command -Name $MakeExecutable -CommandType Application -ErrorAction SilentlyContinue
            if ($explicit) { return $explicit.Source }
            if (Test-Path -LiteralPath $MakeExecutable -PathType Leaf) {
                return (Resolve-Path -LiteralPath $MakeExecutable).Path
            }
            return $null
        }
        foreach ($exe in @("mingw32-make.exe", "make.exe")) {
            $cmd = Get-Command $exe -ErrorAction SilentlyContinue
            if ($cmd) { return $cmd.Source }
        }
        return $null
    }

    function Start-ScopedMake {
        param([Parameter(Mandatory=$true)][hashtable]$StartProcess)
        $startParams = $StartProcess.Clone()
        if (-not [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
                [System.Runtime.InteropServices.OSPlatform]::Windows)) {
            $startParams.Remove("WindowStyle")
        }
        if (-not $TitleManifest) { return (Start-Process @startParams) }
        Assert-TitleManifestDigest `
            -Plan $script:TitleManagerPlan `
            -PlannerScript (Join-Path $PSScriptRoot "tools\title_codegen_plan.py") `
            -ManifestPath $TitleManifest | Out-Null
        $state = Push-TitleAnalyzerEnvironment -Value $script:TitleManagerSpans
        try {
            return (Start-Process @startParams)
        } finally {
            Pop-TitleAnalyzerEnvironment -State $state
        }
    }

    function Assert-TitleManagerPrivateBindings {
        param([switch]$Runtime)
        $missing = @()
        $req = $script:TitleManagerPlan.private_binding_requirements
        if ($req.game_elf -and (-not $GameElfPath -or -not (Test-Path -LiteralPath $GameElfPath -PathType Leaf))) {
            $missing += "executable ELF ($GameElfForMake)"
        }
        if ($req.module_dir) {
            if (-not (Test-Path -LiteralPath $ModuleDirPath -PathType Container)) {
                $missing += $ModuleDirPath
            } else {
                foreach ($mod in @($script:TitleManagerPlan.required_guest_modules)) {
                    $mPath = Join-Path $ModuleDirPath $mod.name
                    if (-not (Test-Path -LiteralPath $mPath -PathType Leaf)) { $missing += $mPath }
                }
            }
        }
        if ($req.psp_header -and -not (Test-Path -LiteralPath $PspHeaderPath -PathType Leaf)) {
            $missing += $PspHeaderPath
        }
        if ($Runtime -and $script:IsRetail) {
            if (-not $GameIsoPath) {
                $missing += "a disc image (declare filesystem.disc_image in the title manifest, or provide game.iso)"
            }
            $dataRoot = $null
            if ($script:TitleDataRoot) {
                $dataRoot = $script:TitleDataRoot
            } elseif ($script:LegacyInputLayout) {
                $dataRoot = "place_game_here\EXTRACTED\PSP_GAME\USRDIR\xbdata_extracted"
            }
            if (-not $dataRoot) {
                $missing += "filesystem.data_root declaration in the title manifest (required for retail runtime)"
            } elseif (-not (Test-Path -LiteralPath $dataRoot -PathType Container)) {
                $missing += $dataRoot
            }
        }
        if ($missing.Count -gt 0) {
            throw "Manifest mode is missing required private binding(s): $($missing -join ', ')"
        }
    }

    function Copy-RequiredAssets {
        if (Test-Path "SDL3.dll") {
            Copy-Item "SDL3.dll" (Join-Path $BuildDir "SDL3.dll") -Force -ErrorAction SilentlyContinue
            Write-Host "SDL3.dll verified next to binary." -ForegroundColor Gray
        }
        if (Test-Path "font") {
            Copy-Item "font" (Join-Path $BuildDir "font") -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host "Fonts verified next to binary." -ForegroundColor Gray
        }
    }

    function Write-BuildError {
        param(
            [string]$Message,
            [string]$LogFile,
            [int]$TailLines = 15
        )
        Write-Host "`n[!] ERROR: $Message" -ForegroundColor Red
        if ($LogFile -and (Test-Path $LogFile)) {
            Write-Host "--- Tail of $LogFile ---" -ForegroundColor Gray
            Get-Content $LogFile -Tail $TailLines | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
            Write-Host "------------------------" -ForegroundColor Gray
        }
    }

    function Find-Function {
        param([string]$FuncName)
        if (-not (Test-Path $BuildDir)) {
            Write-Warning "No compilation target directory exists to scan: $BuildDir"
            return
        }
        $targetChunkPattern = "$($script:ActiveGameName)_recomp_*.c"
        $files = Get-ChildItem (Join-Path $BuildDir $targetChunkPattern) -ErrorAction SilentlyContinue
        $singleChunk = Join-Path $BuildDir "$($script:ActiveGameName)_recomp.c"
        if ($files.Count -eq 0 -and (Test-Path $singleChunk)) {
            $files = Get-Item $singleChunk
        }

        foreach ($file in $files) {
            $match = Select-String -LiteralPath $file.FullName -Pattern "^void $FuncName\b" | Select-Object -First 1
            if ($match) {
                Write-Host "Found definition of $FuncName in $($file.Name) at line $($match.LineNumber):" -ForegroundColor Green
                $lines = Get-Content -LiteralPath $file.FullName
                $start = [Math]::Max(1, $match.LineNumber - 2)
                $end = [Math]::Min($lines.Count, $match.LineNumber + 5)
                for ($i = $start; $i -le $end; $i++) {
                    $prefix = if ($i -eq $match.LineNumber) { "=>" } else { "  " }
                    Write-Host ("{0} {1,5}: {2}" -f $prefix, $i, $lines[$i-1])
                }
                return
            }
        }
        Write-Warning "Function '$FuncName' not found in recompiled source chunks."
    }

    function Find-Symbol {
        param([string]$Query)
        $csvCandidates = @(
            "docs\opengrip_ref\functions.csv",
            "OpenGrip_For_Inspiration\functions.csv"
        )
        $csvPath = $null
        foreach ($c in $csvCandidates) {
            if (Test-Path $c) { $csvPath = $c; break }
        }
        if (-not $csvPath) {
            Write-Warning "functions.csv not found. See docs/SYMBOL_REFERENCE.md for setup."
            return
        }
        $results = Select-String -LiteralPath $csvPath -Pattern $Query -ErrorAction SilentlyContinue
        if ($results) {
            Write-Host "Symbol matches in ${csvPath}:" -ForegroundColor Green
            $results | Select-Object -First 20 | ForEach-Object {
                Write-Host "  $($_.Line)" -ForegroundColor White
            }
        } else {
            Write-Warning "No symbol matching '$Query' in functions.csv."
        }
    }

    function Invoke-Selftest {
        $makeExe = Find-MakeExecutable
        if (-not $makeExe) {
            Write-BuildError -Message "Could not find make executable."
            return $false
        }
        $args = @(Get-NkMakeBaseArgs) + @("selftest", "--no-print-directory")
        Write-Host "Building and running selftest..." -ForegroundColor Cyan
        $proc = Start-Process -FilePath $makeExe -ArgumentList $args -PassThru -NoNewWindow -Wait
        if ($proc.ExitCode -ne 0) {
            Write-Host "[FAIL] selftest exited with code $($proc.ExitCode)." -ForegroundColor Red
            return $false
        }
        Write-Host "[PASS] selftest OK." -ForegroundColor Green
        return $true
    }

    function Invoke-VerifySuite {
        $makeExe = Find-MakeExecutable
        if (-not $makeExe) {
            Write-BuildError -Message "Could not find make executable."
            return $false
        }
        $failed = @()
        $gateStatus = @{}
        $makeBaseArgs = @(Get-NkMakeBaseArgs)

        Write-Host "`n[1/15] Python unit suite (tools/test_*.py)..." -ForegroundColor Cyan
        & python -m unittest discover -s tools -p "test_*.py" | Out-Host
        if ($LASTEXITCODE -ne 0) { $failed += "python-unittest"; $gateStatus["python-unittest"] = "FAIL" } else { $gateStatus["python-unittest"] = "PASS" }

        Write-Host "`n[2/15] Scheduler/callback selftest (src/rt/sched_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("sched-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -ne 0) { $failed += "sched-selftest"; $gateStatus["sched-selftest"] = "FAIL" } else { $gateStatus["sched-selftest"] = "PASS" }

        Write-Host "`n[3/15] Profiler hash-table selftest (src/rt/profiler_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("profiler-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $MakeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "profiler-selftest"; $gateStatus["profiler-selftest"] = "FAIL" } else { $gateStatus["profiler-selftest"] = "PASS" }

        Write-Host "`n[4/15] Guest-heap allocator selftest (src/rt/heap_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("heap-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "heap-selftest"; $gateStatus["heap-selftest"] = "FAIL" } else { $gateStatus["heap-selftest"] = "PASS" }

        Write-Host "`n[5/15] Asset index selftest (src/rt/asset_index_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("asset-index-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "asset-index-selftest"; $gateStatus["asset-index-selftest"] = "FAIL" } else { $gateStatus["asset-index-selftest"] = "PASS" }

        Write-Host "`n[6/15] HLE thread/wait selftest (src/rt/hle_thread_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("hle-thread-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "hle-thread-selftest"; $gateStatus["hle-thread-selftest"] = "FAIL" } else { $gateStatus["hle-thread-selftest"] = "PASS" }

        Write-Host "`n[7/15] FPU conversion round-trip selftest (src/rt/fp_convert_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("fp-convert-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "fp-convert-selftest"; $gateStatus["fp-convert-selftest"] = "FAIL" } else { $gateStatus["fp-convert-selftest"] = "PASS" }

        Write-Host "`n[8/15] VFPU sin/cos lookup table selftest (src/rt/vfpu_tables_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("vfpu-tables-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "vfpu-tables-selftest"; $gateStatus["vfpu-tables-selftest"] = "FAIL" } else { $gateStatus["vfpu-tables-selftest"] = "PASS" }

        Write-Host "`n[9/15] Watchpoints and file I/O selftest (src/rt/watchpoints_file_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("watchpoints-file-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "watchpoints-file-selftest"; $gateStatus["watchpoints-file-selftest"] = "FAIL" } else { $gateStatus["watchpoints-file-selftest"] = "PASS" }

        Write-Host "`n[10/15] VFPU interpreter selftest (src/rt/vfpu_interp_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("vfpu-interp-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "vfpu-interp-selftest"; $gateStatus["vfpu-interp-selftest"] = "FAIL" } else { $gateStatus["vfpu-interp-selftest"] = "PASS" }

        Write-Host "`n[11/15] Reference selftest (src/ref/selftest.cpp)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("ref-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "ref-selftest"; $gateStatus["ref-selftest"] = "FAIL" } else { $gateStatus["ref-selftest"] = "PASS" }

        Write-Host "`n[12/15] Import audit gate (tools/import_audit.py --gate)..." -ForegroundColor Cyan
        & python tools/import_audit.py --gate | Out-Host
        if ($LASTEXITCODE -ne 0) { $failed += "import-audit-gate"; $gateStatus["import-audit-gate"] = "FAIL" } else { $gateStatus["import-audit-gate"] = "PASS" }

        Write-Host "`n[13/15] Publication audit index leg (tools/publish_audit.py --public-scope)..." -ForegroundColor Cyan
        & python tools/publish_audit.py --public-scope --provenance-self-consistency | Out-Host
        if ($LASTEXITCODE -ne 0) { $failed += "publish-audit-index"; $gateStatus["publish-audit-index"] = "FAIL" } else { $gateStatus["publish-audit-index"] = "PASS" }

        Write-Host "`n[14/15] Publication audit worktree leg (tools/publish_audit.py --worktree)..." -ForegroundColor Cyan
        & python tools/publish_audit.py --public-scope --worktree --provenance-self-consistency | Out-Host
        if ($LASTEXITCODE -ne 0) { $failed += "publish-audit-worktree"; $gateStatus["publish-audit-worktree"] = "FAIL" } else { $gateStatus["publish-audit-worktree"] = "PASS" }

        Write-Host "`n[15/15] GPU coherence/capture gates..." -ForegroundColor Cyan
        $gpuSkipReason = $null
        if ($null -eq (Get-Command glslc -ErrorAction SilentlyContinue) -and -not (Test-Path (Join-Path $VulkanSdk "bin\glslc.exe"))) {
            $gpuSkipReason = "glslc not found"
        }
        if ($gpuSkipReason) {
            Write-Host "[SKIP] GPU selftests skipped: $gpuSkipReason." -ForegroundColor Yellow
            $gateStatus["gpu-coherence-selftest"] = "SKIP"
            $gateStatus["gpu-capture-selftest"] = "SKIP"
        } else {
            $a = $makeBaseArgs + @("gpu-coherence-selftest", "--no-print-directory")
            $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
            if ($p.ExitCode -ne 0) { $failed += "gpu-coherence-selftest"; $gateStatus["gpu-coherence-selftest"] = "FAIL" } else { $gateStatus["gpu-coherence-selftest"] = "PASS" }

            $a = $makeBaseArgs + @("gpu-capture-selftest", "--no-print-directory")
            $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
            if ($p.ExitCode -ne 0) { $failed += "gpu-capture-selftest"; $gateStatus["gpu-capture-selftest"] = "FAIL" } else { $gateStatus["gpu-capture-selftest"] = "PASS" }
        }

        $allGates = @(
            "python-unittest", "sched-selftest", "profiler-selftest", "heap-selftest",
            "asset-index-selftest", "hle-thread-selftest", "fp-convert-selftest",
            "vfpu-tables-selftest", "watchpoints-file-selftest", "vfpu-interp-selftest",
            "ref-selftest", "import-audit-gate", "publish-audit-index", "publish-audit-worktree",
            "gpu-coherence-selftest", "gpu-capture-selftest"
        )
        $summaryParts = @($allGates | ForEach-Object {
            $st = if ($gateStatus.ContainsKey($_)) { $gateStatus[$_] } else { "NOT_RUN" }
            "$_=$st"
        })
        $summaryParts += "make-verify=NOT_RUN(private PPSSPP oracle traces absent)"
        $summaryParts += "atrac3p-title-accept=NOT_RUN(private title stream absent)"
        $summaryParts += "visual-oracle=NOT_RUN(private title route required)"

        $aggregate = if ($failed.Count -eq 0) { "PASS" } else { "FAIL" }
        Write-Host ("VERIFY_SUMMARY aggregate={0} {1}" -f $aggregate, ($summaryParts -join " ")) -ForegroundColor $(if ($aggregate -eq "PASS") { "Green" } else { "Red" })

        if ($failed.Count -gt 0) {
            Write-Host "`n[FAIL] Verify suite encountered $($failed.Count) failure(s): $($failed -join ', ')" -ForegroundColor Red
            return $false
        }
        Write-Host "`n[PASS] All verification gates passed." -ForegroundColor Green
        return $true
    }

    function Invoke-VisualOracle {
        param(
            [string]$Route,
            [int]$ExitAtVblank = 0,
            [int]$SnapEvery = 0,
            [int]$SnapAfter = 0,
            [string]$SnapWindows,
            [string]$SaveBase,
            [string]$OracleName,
            [switch]$OverwriteOracle,
            [string]$RunProfile = "Standard"
        )
        Assert-TitleManagerPrivateBindings -Runtime
        $exePath = $ExePath
        if (-not (Test-Path $exePath)) {
            Write-Host "[!] $exePath not found - run BuildFast or BuildFull first." -ForegroundColor Red
            return $false
        }
        if (-not $Route -or -not (Test-Path $Route)) {
            Write-Host "[!] A route file (-Route <path>) is required for VisualOracle." -ForegroundColor Red
            return $false
        }
        if (-not $OracleName) {
            $OracleName = ([System.IO.Path]::GetFileNameWithoutExtension($Route)) -replace '^route_', ''
        }
        if (-not (Test-SafeComponentName -Name $OracleName -Label "OracleName")) {
            return $false
        }
        $outDir = Join-Path $script:LogDir "oracle_$OracleName"
        if (-not (Reset-OracleArchive -Path $outDir -AllowedRoot $script:LogDir -Overwrite:$OverwriteOracle)) { return $false }
        try {
            @(Get-ChildItem -LiteralPath $script:RepoRoot -Filter "snap_*.ppm" -File -ErrorAction Stop) |
                Remove-Item -Force -ErrorAction Stop
        } catch {
            Write-Host "[!] Could not clear stale snapshots before the run: $($_.Exception.Message)" -ForegroundColor Red
            return $false
        }

        $env:SR_PADSCRIPT     = (Resolve-Path $Route).Path
        $env:SR_NOINPUT       = "1"
        $env:SR_EXIT_AT_VBLANK = "$ExitAtVblank"
        if ($SnapEvery -gt 0) { $env:SR_FBSNAP = "$SnapEvery" } else { $env:SR_FBSNAP = $null }
        if ($SnapAfter -gt 0) { $env:SR_FBSNAP_AFTER = "$SnapAfter" } else { $env:SR_FBSNAP_AFTER = $null }
        if ($SnapWindows) { $env:SR_FBSNAP_WINDOWS = $SnapWindows } else { $env:SR_FBSNAP_WINDOWS = $null }

        $exeHash   = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
        $routeHash = (Get-FileHash -LiteralPath $Route  -Algorithm SHA256).Hash
        $gitHead   = (& git rev-parse HEAD 2>$null)
        if ($LASTEXITCODE -ne 0 -or -not $gitHead) { $gitHead = "unknown" }
        $gitDirty  = [bool](& git status --porcelain 2>$null)

        $saveSync = $null
        if ($SaveBase) {
            $saveSync = Sync-SaveBase -BasePath $SaveBase `
                -SaveRoot (Join-Path $script:RepoRoot "memstick\PSP\SAVEDATA") `
                -ApprovedRoot $script:RepoRoot `
                -RouteContext $OracleName -BuildContext $exeHash
            if (-not $saveSync) { return $false }
        }

        Write-Host "VisualOracle: route=$Route exit_at_vblank=$ExitAtVblank snap_every=$SnapEvery snap_after=$SnapAfter snap_windows=$SnapWindows profile=$RunProfile" -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $backstop = [int][math]::Max(180, [math]::Ceiling($ExitAtVblank / 10.0))
        Run-NkEngine -Profile $RunProfile -RunDuration $backstop
        $sw.Stop()
        $runResult = $script:LastRunResult

        $errLog = Join-Path $script:LogDir "stderr_run.log"
        $vblanks = 0
        $reachedExit = $false
        if (Test-Path $errLog) {
            $m = Select-String -Path $errLog -Pattern 'phase=exit_at_vblank vblanks=(\d+)' | Select-Object -Last 1
            if ($m) { $vblanks = [int]$m.Matches[0].Groups[1].Value; $reachedExit = $true }
            Copy-Item $errLog (Join-Path $outDir "stderr.log") -Force
        }
        $routeOutcome = Read-RouteOutcome -StderrPath (Join-Path $outDir "stderr.log")
        $captures = @(Get-ChildItem (Join-Path (Get-Location) "snap_*.ppm") -ErrorAction SilentlyContinue)
        $captures | ForEach-Object { Copy-Item $_.FullName (Join-Path $outDir $_.Name) -Force }
        if ($RunProfile -eq "Benchmark") {
            $perfCsv = Join-Path $script:LogDir "perf.csv"
            if (Test-Path $perfCsv) { Copy-Item $perfCsv (Join-Path $outDir "perf.csv") -Force }
        }

        $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)
        $rate = if ($secs -gt 0 -and $vblanks -gt 0) { [math]::Round($vblanks / $secs, 1) } else { 0 }
        $verdict = Get-OracleVerdict -ReachedExit $reachedExit -TimedOut $runResult.TimedOut `
                       -ExitCode $runResult.ExitCode -CaptureCount $captures.Count `
                       -RequestedVblank $ExitAtVblank -ObservedVblank $vblanks `
                       -RouteKind $routeOutcome.Kind -RouteFailReason $routeOutcome.FailReason

        $manifest = [ordered]@{
            oracle_name          = $OracleName
            complete             = $verdict.Complete
            incomplete_reasons   = @($verdict.Reasons)
            git_head             = $gitHead
            git_worktree_dirty   = $gitDirty
            exe_path             = $exePath
            exe_sha256           = $exeHash
            route_path           = (Resolve-Path $Route).Path
            route_sha256         = $routeHash
            route_kind           = $routeOutcome.Kind
            route_checkpoints    = @($routeOutcome.Checkpoints)
            route_fail_reason    = $routeOutcome.FailReason
            run_profile          = $RunProfile
            requested_vblank     = $ExitAtVblank
            observed_vblank      = $vblanks
            snap_every           = $SnapEvery
            snap_after           = $SnapAfter
            snap_windows         = $SnapWindows
            save_base            = $SaveBase
            save_base_action     = if ($saveSync) { $saveSync.Action } else { "not-isolated" }
            backstop_seconds     = $backstop
            reached_exit         = $reachedExit
            timed_out            = $runResult.TimedOut
            exit_code            = $runResult.ExitCode
            capture_count        = $captures.Count
            wall_seconds         = $secs
            guest_vblanks_per_s  = $rate
        }
        $manifest | ConvertTo-Json -Depth 4 |
            Out-File -FilePath (Join-Path $outDir "oracle_manifest.json") -Encoding utf8

        $summary = "VisualOracle result: name=$OracleName wall_s=$secs vblanks=$vblanks " +
                   "guest_vblanks_per_s=$rate captures=$($captures.Count) exit_code=$($runResult.ExitCode) " +
                   "timed_out=$($runResult.TimedOut) reached_exit=$reachedExit route=$($routeOutcome.Kind) " +
                   "checkpoints=$($routeOutcome.Checkpoints.Count) complete=$($verdict.Complete)"
        Write-Host $summary -ForegroundColor $(if ($verdict.Complete) { "Green" } else { "Yellow" })
        $summary | Out-File -FilePath (Join-Path $outDir "oracle_summary.txt") -Encoding utf8

        if (-not $verdict.Complete) {
            Write-Host "[!] Incomplete run - its captures are NOT admissible evidence:" -ForegroundColor Yellow
            $verdict.Reasons | ForEach-Object { Write-Host "      - $_" -ForegroundColor Yellow }
            return $false
        }
        Write-Host "[PASS] $($captures.Count) captures archived to $outDir" -ForegroundColor Green
        return $true
    }

    function Invoke-DiffFunc {
        param(
            [string]$Target,
            [string]$Oracle,
            [int]$Step = 0
        )
        if (-not (Test-Path $ExePath)) {
            Write-Host "[!] $ExePath not found - run BuildFast first." -ForegroundColor Red
            return
        }
        if (-not (Test-Path $Oracle)) {
            Write-Host "[!] Oracle trace not found: $Oracle" -ForegroundColor Red
            return
        }
        $addr = $Target -replace '^f_',''
        $outTrace = "diff_${addr}.trace"
        Write-Host "DiffFunc: target=0x$addr oracle=$Oracle step=$Step" -ForegroundColor Cyan
        $imagePath = $ImagePath
        $args = @("--image", $imagePath, "0", (Get-NkRunEntry), $outTrace, "none",
                  "--diff-func=0x$addr", "--diff-oracle=$Oracle", "--diff-step=$Step")
        $proc = Start-Process -FilePath $ExePath -ArgumentList $args -PassThru -NoNewWindow -Wait `
            -RedirectStandardError "$LogDir/difffunc_err.log"
        Write-Host "DiffFunc finished (exit $($proc.ExitCode)). Output: $outTrace" -ForegroundColor $(if ($proc.ExitCode -eq 0) { "Green" } else { "Yellow" })
        if (Test-Path "$LogDir/difffunc_err.log") {
            Get-Content "$LogDir/difffunc_err.log" -Tail 10 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        }
        if (Test-Path $outTrace) {
            $cmpTool = Join-Path $script:RepoRoot "tools\funcdiff_cmp.py"
            if (Test-Path $cmpTool) {
                Write-Host "Comparing with oracle via funcdiff_cmp.py..." -ForegroundColor Cyan
                python $cmpTool $outTrace $Oracle 2>&1 | Select-Object -First 20 |
                    ForEach-Object { Write-Host "  $_" }
            }
        }
    }

    function Invoke-NkBuild {
        param(
            [Parameter(Mandatory=$true)]
            [ValidateSet("Fast", "Full")]
            [string]$Mode
        )

        Assert-TitleManagerPrivateBindings
        if ($script:IsRetail) {
            $missingBuildInputs = @()
            if (-not $GameElfPath -or -not (Test-Path -LiteralPath $GameElfPath)) {
                $missingBuildInputs += "executable ELF ($GameElfForMake)"
            }
            if ($missingBuildInputs.Count -gt 0) {
                Write-BuildError -Message "Missing required private build inputs: $($missingBuildInputs -join ', ')"
                return $false
            }
        }
        Stop-BuildProcesses

        if (-not (Test-Path $BuildDir)) {
            New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
        }

        $baseArgs = @(Get-NkMakeBaseArgs)
        $makeExe = Find-MakeExecutable
        if (-not $makeExe) {
            Write-BuildError -Message "Could not resolve make tools in active PATH environment."
            return $false
        }

        switch ($Mode) {
            "Fast" {
                Write-Host "Running dependency/profile-aware incremental build for $script:ActiveGameName..." -ForegroundColor Green
                $args = $baseArgs + @("--no-print-directory", "-j$($script:EffectiveJobs)", "all")
                $proc = Start-ScopedMake -StartProcess @{
                    FilePath = $makeExe; ArgumentList = $args
                    PassThru = $true; NoNewWindow = $true; Wait = $true
                }
                $exitInfo = Get-KnownExitCode -Process $proc
                if (-not $exitInfo.Known) {
                    Write-BuildError -Message "Make finished with an UNKNOWN exit status; the build result cannot be verified as success."
                    return $false
                }
                if ($exitInfo.ExitCode -ne 0) {
                    Write-BuildError -Message "Make compilation exited with an error status."
                    return $false
                }
                $script:LastBuildInfo = $exitInfo
            }
            "Full" {
                Write-Host "Removing the target build directory for full rebuild: $BuildDir..." -ForegroundColor Green
                $cleanArgs = $baseArgs + @("--no-print-directory", "clean")
                $cleanProc = Start-Process -FilePath $makeExe -ArgumentList $cleanArgs -PassThru -NoNewWindow -Wait
                if ($cleanProc.ExitCode -ne 0) {
                    Write-BuildError -Message "Could not clean the target build directory."
                    return $false
                }

                $args = $baseArgs + @("--no-print-directory", "-j$($script:EffectiveJobs)", "all")
                $outLog = "$LogDir/build_out_recomp.log"
                $errLog = "$LogDir/build_err_recomp.log"
                Remove-Item $outLog, $errLog -ErrorAction SilentlyContinue

                Write-Host "Monitoring background build metrics (Press Ctrl+C to stop)..." -ForegroundColor Yellow
                $proc = Start-ScopedMake -StartProcess @{
                    FilePath = $makeExe; ArgumentList = $args
                    RedirectStandardOutput = $outLog; RedirectStandardError = $errLog
                    PassThru = $true; WindowStyle = 'Hidden'
                }
                Register-BuildProcess $proc.Id

                while (-not $proc.HasExited) {
                    $outSize = if (Test-Path $outLog) { (Get-Item $outLog).Length } else { 0 }
                    $errSize = if (Test-Path $errLog) { (Get-Item $errLog).Length } else { 0 }
                    $recompTarget = Join-Path $BuildDir "$($script:ActiveGameName)_recomp.o"
                    $recompSize = if (Test-Path $recompTarget) { (Get-Item $recompTarget).Length } else { 0 }
                    $exeState = if (Test-Path $ExePath) { "Ready" } else { "Building" }

                    Write-Host ("[{0}] stdout: {1}b | stderr: {2}b | recomp.o: {3}b | status: {4}" -f `
                        (Get-Date -Format "HH:mm:ss"), $outSize, $errSize, $recompSize, $exeState) -ForegroundColor Gray
                    Start-Sleep -Seconds 15
                }

                $makeResult = Wait-ProcessOrKill -Process $proc -TimeoutSeconds 0
                Unregister-BuildProcess $proc.Id
                $makeExitCode = $makeResult.ExitCode
                if ($null -eq $makeExitCode) {
                    Write-BuildError -Message "Make finished but its exit status is UNKNOWN (null exit code); the build result cannot be verified as success."
                    return $false
                }
                Write-Host "Make finished processing with code: $makeExitCode" -ForegroundColor Green
                if ($makeExitCode -ne 0) {
                    Write-BuildError -Message "Full recompile failed." -LogFile $errLog -TailLines 20
                    return $false
                }
                if (-not (Test-Path -LiteralPath $ExePath)) {
                    Write-BuildError -Message "Full rebuild reported success but $ExePath was not produced."
                    return $false
                }
                $script:LastBuildInfo = @{ Known = $true; ExitCode = $makeExitCode }
            }
        }

        # Refresh Clang compilation database if compiledb is available
        $compiledbExe = Get-Command compiledb -ErrorAction SilentlyContinue
        if ($compiledbExe) {
            Write-Host "[compiledb] refreshing compile_commands.json..." -ForegroundColor DarkGray
            try {
                $dryRunLog = Join-Path $env:TEMP "nk_compiledb_dryrun.log"
                $dryRunArgs = @("-Bnwk") + $baseArgs + @("all", "selftest")
                & $makeExe @dryRunArgs 2>$null > $dryRunLog
                & compiledb -p $dryRunLog -o compile_commands.json 2>&1 | Out-Null
                if (Test-Path compile_commands.json) {
                    try {
                        python -c "import json; d=json.load(open('compile_commands.json')); [e.update({'directory': e['directory'].replace(chr(92),'/'), 'file': e.get('file','').replace(chr(92),'/')}) for e in d]; json.dump(d, open('compile_commands.json','w'), indent=2)" 2>$null
                    } catch { }
                }
            } catch { }
        }

        Copy-RequiredAssets
        if ($null -ne $script:LastBuildInfo -and $script:LastBuildInfo.Known -and $script:LastBuildInfo.ExitCode -eq 0) {
            Write-NkBuildManifest -Mode $Mode -ExitCode $script:LastBuildInfo.ExitCode
        }
        return $true
    }

    # Backward compatibility alias
    function Invoke-HstBuild {
        param([string]$Mode)
        return Invoke-NkBuild -Mode $Mode
    }

    function Write-NkBuildManifest {
        param([string]$Mode, [int]$ExitCode)
        try {
            $gitHead = (& git rev-parse HEAD 2>$null)
            if ($LASTEXITCODE -ne 0 -or -not $gitHead) { $gitHead = "unknown" }
            $manifest = [ordered]@{
                format     = "nk-build-manifest/v1"
                game_name  = $script:ActiveGameName
                mode       = $Mode
                exit_code  = $ExitCode
                exe_path   = $ExePath
                exe_sha256 = if (Test-Path -LiteralPath $ExePath) { (Get-FileHash -LiteralPath $ExePath -Algorithm SHA256).Hash } else { $null }
                git_head   = $gitHead
                built_utc  = ([DateTime]::UtcNow).ToString("o")
            }
            $manifest | ConvertTo-Json -Depth 4 |
                Out-File -FilePath (Join-Path $script:LogDir "build_manifest.json") -Encoding utf8 -ErrorAction SilentlyContinue
        } catch { }
    }

    function Write-HstBuildManifest {
        param([string]$Mode, [int]$ExitCode)
        Write-NkBuildManifest -Mode $Mode -ExitCode $ExitCode
    }

    function Run-NkEngine {
        param(
            [string]$Profile = "Standard",
            [int]$RunDuration = 0,
            [switch]$NoGui
        )

        Assert-TitleManagerPrivateBindings -Runtime

        if (-not (Test-Path $ExePath)) {
            Write-Host "[!] Error: Executable target is missing: $ExePath. Execute build pipeline first." -ForegroundColor Red
            return
        }

        if (-not (Test-Path $ImagePath)) {
            Write-Host "[!] Error: Game image target is missing: $ImagePath. Execute build pipeline first." -ForegroundColor Red
            return
        }

        if ($script:IsRetail) {
            if (-not $GameIsoPath -or -not (Test-Path -LiteralPath $GameIsoPath)) {
                Write-Host "[!] Cannot run game: no disc image found (declare filesystem.disc_image in the title manifest, or provide game.iso)." -ForegroundColor Red
                return
            }
            $effectiveDataRoot = $null
            if ($script:TitleDataRoot) {
                $effectiveDataRoot = $script:TitleDataRoot
            } elseif ($script:LegacyInputLayout) {
                $effectiveDataRoot = "place_game_here\EXTRACTED\PSP_GAME\USRDIR\xbdata_extracted"
            }
            if (-not $effectiveDataRoot) {
                Write-Host "[!] Cannot run game: the title manifest does not declare filesystem.data_root and no legacy layout is present." -ForegroundColor Red
                return
            }
            if (-not (Test-Path -LiteralPath $effectiveDataRoot -PathType Container)) {
                Write-Host "[!] Cannot run game: Extracted asset tree was not found at $effectiveDataRoot." -ForegroundColor Red
                return
            }
        }

        Stop-WorkspaceTarget
        Start-Sleep -Milliseconds 150

        Remove-Item "$LogDir/stdout_run.log", "$LogDir/stderr_run.log" -ErrorAction SilentlyContinue

        if ($GameIsoPath) { $env:PSP_ISO = $GameIsoPath }
        $env:PSP_VFPU_TABLES = "assets/vfpu"

        $env:SR_QUIET = $null
        $env:SR_PERF = $null
        $env:SR_PERF_CSV = $null
        $env:SR_PROFILE = $null
        $env:SR_PROFILE_DUMP_VBLANKS = $null

        switch ($Profile) {
            "Standard" {
                $env:SR_GPU_GE = "1"
                $env:SR_ALLOC_MAX = "04000000"
                $env:SR_WATCHDOG_EXIT = $null
                $env:SR_THLOG = $null
                $env:SR_BLOCKLOG = $null
                $env:SR_SYSLOG = $null
                $env:SR_HLELOG = $null
                $env:SR_WAKELOG = $null
                $env:SR_GEDUMP = $null
                $env:SR_IOLOG = $null
                $env:SR_POSTUMD = $null
                $env:SR_EXITSNAP = $null
                $env:SR_DEBUG = $null
                $env:SR_PROFILE = $null
                $env:SR_ARGLOG = $null
            }
            "Performance" {
                $env:SR_GPU_GE = "1"
                $env:SR_ALLOC_MAX = "04000000"
                $env:SR_QUIET = "1"
                $env:SR_THLOG = $null
                $env:SR_BLOCKLOG = $null
                $env:SR_SYSLOG = $null
                $env:SR_HLELOG = $null
                $env:SR_WAKELOG = $null
                $env:SR_GEDUMP = $null
                $env:SR_IOLOG = $null
                $env:SR_POSTUMD = $null
                $env:SR_EXITSNAP = $null
                $env:SR_WATCHDOG_EXIT = $null
                $env:SR_DEBUG = $null
                $env:SR_PROFILE = $null
                $env:SR_ARGLOG = $null
            }
            "Benchmark" {
                $env:SR_GPU_GE = "1"
                $env:SR_ALLOC_MAX = "04000000"
                $env:SR_PERF = "1"
                $env:SR_PERF_CSV = "$LogDir/perf.csv"
                Remove-Item $env:SR_PERF_CSV -ErrorAction SilentlyContinue
                $env:SR_THLOG = $null
                $env:SR_BLOCKLOG = $null
                $env:SR_SYSLOG = $null
                $env:SR_HLELOG = $null
                $env:SR_WAKELOG = $null
                $env:SR_GEDUMP = $null
                $env:SR_IOLOG = $null
                $env:SR_POSTUMD = $null
                $env:SR_EXITSNAP = $null
                $env:SR_WATCHDOG_EXIT = $null
                $env:SR_DEBUG = $null
                $env:SR_PROFILE = $null
                $env:SR_ARGLOG = $null
            }
            "Diagnostics" {
                $env:SR_GPU_GE = "1"
                $env:SR_ALLOC_MAX = "04000000"
                $env:SR_THLOG = "1"
                $env:SR_BLOCKLOG = "1"
                $env:SR_SYSLOG = "1"
                $env:SR_WAKELOG = "1"
                $env:SR_GEDUMP = "1"
                $env:SR_HLELOG = "1"
                $env:SR_IOLOG = "1"
                $env:SR_POSTUMD = "1"
                $env:SR_EXITSNAP = "1"
                $env:SR_WATCHDOG_EXIT = $null
            }
            "Software" {
                $env:SR_GPU_GE = "0"
                $env:SR_THLOG = "1"
                $env:SR_HLELOG = "1"
                $env:SR_WAKELOG = $null
                $env:SR_GEDUMP = $null
                $env:SR_IOLOG = $null
                $env:SR_POSTUMD = $null
                $env:SR_EXITSNAP = $null
            }
        }

        if ($GuestProfile) {
            $env:SR_PROFILE = "1"
            $env:SR_PROFILE_DUMP_VBLANKS = if ($GuestProfilePeriod -gt 0) { $GuestProfilePeriod.ToString() } else { $null }
        }

        if ($env:SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS -eq "1") {
            $env:SR_DISPATCH_FATAL = $null
            Write-Host "[!] SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS=1: dispatch misses will NOT be fatal." -ForegroundColor Yellow
        } else {
            $env:SR_DISPATCH_FATAL = "1"
        }

        if ($NoGui) {
            $env:SR_WATCHDOG_EXIT = $null
        }

        $entryAddr = Get-NkRunEntry
        $imagePath = $ImagePath
        $args = @("--image", $imagePath, "0", (Get-NkRunEntry), "none", "none")
        if (-not $NoGui) {
            $args += "--gui"
        } else {
            $args += "--sched"
        }

        Write-Host "=== NAKAGAWA RECOMP RUNTIME LAUNCH ===" -ForegroundColor Green
        Write-Host "  Binary    : $ExePath" -ForegroundColor Gray
        Write-Host "  Profile   : $Profile" -ForegroundColor Gray
        Write-Host "  Target    : $script:ActiveGameName" -ForegroundColor Gray
        Write-Host "  Entry     : $entryAddr" -ForegroundColor Gray
        Write-Host "======================================" -ForegroundColor Green

        Write-Host "Spawning host runtime executable..." -ForegroundColor Cyan
        $proc = Start-Process -FilePath $ExePath -ArgumentList $args `
            -RedirectStandardOutput "$LogDir/stdout_run.log" `
            -RedirectStandardError "$LogDir/stderr_run.log" `
            -PassThru -WindowStyle Hidden

        if ($RunDuration -gt 0) {
            Write-Host "Runtime deadline: $RunDuration seconds." -ForegroundColor Cyan
        } else {
            Write-Host "Engine active. Close window or terminate process to complete." -ForegroundColor Cyan
        }
        $script:LastRunResult = Wait-ProcessOrKill -Process $proc -TimeoutSeconds $RunDuration
        if ($script:LastRunResult.TimedOut) {
            Write-Host "Target engine process terminated by controller timeout after $($script:LastRunResult.ElapsedSeconds)s." -ForegroundColor Yellow
        } else {
            Write-Host "Game program terminated after $($script:LastRunResult.ElapsedSeconds)s. Code: $($script:LastRunResult.ExitCode)" -ForegroundColor Green
        }

        Analyze-RunLogs "$LogDir/stderr_run.log"
    }

    # Backward compatibility alias
    function Run-HstEngine {
        param([string]$Profile = "Standard", [int]$RunDuration = 0, [switch]$NoGui)
        Run-NkEngine -Profile $Profile -RunDuration $RunDuration -NoGui:$NoGui
    }

    function Analyze-RunLogs {
        param([string]$LogPath)
        if (-not (Test-Path $LogPath)) { return }

        Write-Host "`n=================== OUTPUT TRACE PARSE ===================" -ForegroundColor Yellow
        $errors = Select-String -Path $LogPath -Pattern "error|failed|fail|FATAL|HOST CRASH|exception 0x" -ErrorAction SilentlyContinue
        if ($errors) {
            Write-Host "[!] Execution faults found in telemetry log:" -ForegroundColor Red
            $errors | Select-Object -First 8 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red }
        } else {
            Write-Host "[+] Telemetry stream reports no generic failures." -ForegroundColor Green
        }
        Write-Host "========================================================`n" -ForegroundColor Yellow
    }

    if ($Action) {
        switch ($Action) {
            "BuildFull" { if (-not (Invoke-NkBuild -Mode "Full")) { $script:ManagerExitCode = 1; break } }
            "BuildFast" { if (-not (Invoke-NkBuild -Mode "Fast")) { $script:ManagerExitCode = 1; break } }
            "Fuzz" {
                Assert-TitleManagerPrivateBindings
                $makeExe = Find-MakeExecutable
                if (-not $makeExe) {
                    Write-BuildError -Message "Could not resolve make tools in active PATH environment."
                    $script:ManagerExitCode = 1; break
                }
                Write-Host "Triggering VFPU fuzzing..." -ForegroundColor Cyan
                $args = @(Get-NkMakeBaseArgs) + @("vfpu_fuzz", "--no-print-directory")
                $proc = Start-ScopedMake -StartProcess @{
                    FilePath = $makeExe; ArgumentList = $args
                    PassThru = $true; NoNewWindow = $true; Wait = $true
                }
                $exitInfo = Get-KnownExitCode -Process $proc
                if (-not $exitInfo.Known -or $exitInfo.ExitCode -ne 0) {
                    Write-Host "[FAIL] vfpu_fuzz exited with unknown/failed status." -ForegroundColor Red
                    $script:ManagerExitCode = 1; break
                } else {
                    Write-Host "[PASS] vfpu_fuzz OK." -ForegroundColor Green
                }
            }
            "Run" {
                $profile = if ($SoftwareRender) { "Software" } else { $Profile }
                $script:LastRunResult = $null
                Run-NkEngine -Profile $profile -RunDuration $Duration -NoGui:$NoGui
                if ($null -eq $script:LastRunResult) { $script:ManagerExitCode = 1; break }
                if (-not $script:LastRunResult.TimedOut -and $script:LastRunResult.ExitCode -ne 0) { $script:ManagerExitCode = 1; break }
            }
            "Inspect" {
                if ($InspectFunc) { Find-Function -FuncName $InspectFunc }
                else { Write-Host "[!] Error: Target function name is required (e.g., -InspectFunc f_002b7ca0)." -ForegroundColor Red }
            }
            "Clean" {
                Stop-BuildProcesses
                Remove-Item "$LogDir/build_out_recomp.log", "$LogDir/build_err_recomp.log", "$LogDir/recomp_err.log", "$LogDir/obj_err.log", "$LogDir/link_err.log", "$LogDir/stdout_run.log", "$LogDir/stderr_run.log" -ErrorAction SilentlyContinue
                Write-Host "Local tracking files cleared." -ForegroundColor Green
            }
            "Test" {
                if (-not (Invoke-Selftest)) { $script:ManagerExitCode = 1; break }
            }
            "Verify" {
                if (-not (Invoke-VerifySuite)) { $script:ManagerExitCode = 1; break }
            }
            "VisualOracle" {
                if (-not (Invoke-VisualOracle -Route $Route -ExitAtVblank $ExitAtVblank `
                          -SnapEvery $SnapEvery -SnapAfter $SnapAfter -SnapWindows $SnapWindows `
                          -SaveBase $SaveBase -OracleName $OracleName `
                          -OverwriteOracle:$OverwriteOracle -RunProfile $Profile)) { $script:ManagerExitCode = 1; break }
            }
            "DiffFunc" {
                if (-not $DiffTarget) {
                    Write-Host "[!] -DiffTarget required (e.g. -DiffTarget f_00010738)" -ForegroundColor Red
                } elseif (-not $DiffOracle) {
                    Write-Host "[!] -DiffOracle required (path to reference trace)" -ForegroundColor Red
                } else {
                    Invoke-DiffFunc -Target $DiffTarget -Oracle $DiffOracle -Step $DiffStep
                }
            }
            "FindSymbol" {
                $q = if ($FindName) { $FindName } elseif ($InspectFunc) { $InspectFunc } else { "" }
                if ($q) { Find-Symbol -Query $q }
                else { Write-Host "[!] -FindName required (e.g. -FindName Camera_Update)" -ForegroundColor Red }
            }
        }
    } else {
        Safe-ClearHost
        Write-Host "=========================================================" -ForegroundColor Green
        Write-Host "             Nakagawa Recomp CLI Manager              " -ForegroundColor Green
        Write-Host "=========================================================" -ForegroundColor Green
        Write-Host "Target Title: $script:ActiveGameName (Manifest: $script:ManifestId)" -ForegroundColor Cyan
        Write-Host "Run with -Action <BuildFast|BuildFull|Run|Verify|Test|Inspect|Clean>" -ForegroundColor Gray
        Write-Host "========================================================="
        [int]$durVal = 0
        $tSelection = "0"
        $parsed = ConvertTo-SafeTimeoutSeconds -Text $tSelection
        if ($null -ne $parsed) { $durVal = [int]$parsed }
    }
} catch {
    Write-Host "`n[FATAL SCRIPT ERROR] Execution halted abruptly." -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host "Script location: $($_.InvocationInfo.ScriptLineNumber)" -ForegroundColor Red
    $script:ManagerExitCode = 1
} finally {
    if ($script:OriginalLocation) {
        Set-Location -LiteralPath $script:OriginalLocation -ErrorAction SilentlyContinue
    }
}
if ($Action -and $script:ManagerExitCode -ne 0) {
    exit $script:ManagerExitCode
}
