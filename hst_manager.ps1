# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    Consolidated recompiler tool for static rebuilds, environment profile execution,
    and log monitoring.
.DESCRIPTION
    Wraps compilation tasks, interactive environment controls, and deep trace parsing.
    Maintains window persistence on startup and runtime exceptions.
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
    # transition from the SAME run, because the club backdrop varies with host wall-clock
    # time and an old capture is therefore not a valid reference. Captures are named by
    # vblank (snap_v<n>.ppm) so neither window can overwrite the other.
    [Parameter(Mandatory=$false)]
    [ValidatePattern('^\s*$|^\d+-\d+(,\d+-\d+)*$')]
    [string]$SnapWindows,

    # Hold the guest's save state still across replays. The game writes a real save, so
    # without this run N+1 starts from whatever run N left behind -- two replays of one
    # route have been observed to diverge on a first-time tutorial popup. First use captures
    # the current save as the baseline; later runs restore it. Never touches *GAMEDATA.
    [Parameter(Mandatory=$false)]
    [string]$SaveBase,

    # OracleName becomes a single path component under logs/ (oracle_<name>). It is an
    # identifier, not a path: separators, "..", rooted and drive-relative forms are rejected
    # at binding; when derived from -Route it is sanitized to the same grammar.
    [Parameter(Mandatory=$false)]
    [ValidatePattern('^$|^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')]
    [string]$OracleName,

    # An oracle archive holds exactly one run. Reusing a name is rejected rather than
    # merged, because snapshots are numbered per run and a shorter second run would leave
    # the first one's tail behind in a set that still looks complete.
    [Parameter(Mandatory=$false)]
    [switch]$OverwriteOracle,

    # Run deadline in seconds; 0 = indefinite (interactive). Negative or overflowing
    # values must never silently become an indefinite wait, so the range is explicit.
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

    # Optional explicit make path for hermetic/non-MSYS2 callers.  The normal
    # Windows resolver remains the default; tests and supported POSIX hosts can
    # supply a concrete GNU Make executable without relying on a .exe suffix.
    [Parameter(Mandatory=$false)]
    [string]$MakeExecutable = "",

    [Parameter(Mandatory=$false)]
    [string]$VulkanSdk = "",

    # Optional compile-profile overrides. The Makefile defaults both native runtime
    # and generated game code to O0; native O2 is verified but not the default.
    [Parameter(Mandatory=$false)]
    [ValidateSet("O0", "O1", "O2")]
    [string]$RuntimeOpt,

    [Parameter(Mandatory=$false)]
    [ValidateSet("O0", "O1", "O2")]
    [string]$RecompOpt,

    [Parameter(Mandatory=$false)]
    [ValidateRange(1, 1000000)]
    [int]$FuncsPerChunk = 0,

    # Opt-in public title configuration. The legacy HST path remains the default.
    [Parameter(Mandatory=$false)]
    [string]$TitleManifest = ""
)

# -----------------------------------------------------------------------------
# Safe Environment Configuration & Error Trapping
# -----------------------------------------------------------------------------
$ErrorActionPreference = "Stop"

# Action-mode termination code. Set inside the try/catch, applied with a single `exit`
# AFTER the finally block has restored the caller's location, so `exit` never skips
# cleanup the way a mid-body `exit 1` would.
$script:ManagerExitCode = 0
$script:OriginalLocation = $null

$script:TitleManagerPlan = $null
$script:TitleManagerMakeArgs = $null
$script:TitleManagerSpans = $null
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
    # Repository-root identity (#183). The manager owns one workspace: the
    # directory its own script lives in, never the caller's CWD. All managed
    # paths below (logs, build outputs, snapshots, oracle archives, memstick/
    # save roots, generated manifests, temp state) derive from this root, and
    # execution fails closed when the script is moved/copied into a tree that
    # does not carry the repository identity anchors.
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

    if ($TitleManifest) {
        $TitlePlanSupport = Join-Path $PSScriptRoot "tools\title_manager_plan.ps1"
        if (-not (Test-Path -LiteralPath $TitlePlanSupport)) {
            throw "Missing title manager planning helper: $TitlePlanSupport"
        }
        . $TitlePlanSupport
        if (-not (Test-Path -LiteralPath $TitleManifest -PathType Leaf)) {
            throw "Title manifest was not found: $TitleManifest"
        }
    }

    # Initialize MSYS path elements
    if ($env:Path -notlike "*$MsysPath*") {
        $env:Path = "$MsysPath$([IO.Path]::PathSeparator)" + $env:Path
    }

    $VulkanSdk = Resolve-VulkanSdk -ExplicitPath $VulkanSdk -EnvironmentPath $env:VULKAN_SDK
    Write-Host "Using Vulkan SDK: $VulkanSdk" -ForegroundColor DarkGray

    # Resolve the canonical private-input layout first. Legacy root links remain
    # supported, but are no longer required when place_game_here is complete.
    $GameElfPath = $null
    foreach ($candidate in @("place_game_here\EBOOT.elf", "eboot.elf")) {
        if (Test-Path -LiteralPath $candidate) { $GameElfPath = $candidate; break }
    }
    $GameIsoPath = $null
    if (Test-Path -LiteralPath "game.iso") {
        $GameIsoPath = "game.iso"
    } else {
        $isoCandidates = @(Get-ChildItem -LiteralPath "place_game_here\ISO" -File -Filter "*.iso" -ErrorAction SilentlyContinue)
        if ($isoCandidates.Count -eq 1) {
            $GameIsoPath = $isoCandidates[0].FullName
        }
    }
    $GameElfForMake = if ($GameElfPath) { $GameElfPath -replace "\\", "/" } else { "eboot.elf" }
    $ModuleDirPath = "place_game_here\EXTRACTED\decrypted"
    $ModuleDirForMake = $ModuleDirPath -replace "\\", "/"
    $PspHeaderPath = "place_game_here\EXTRACTED\PSP_GAME\SYSDIR\EBOOT.BIN"
    $PspHeaderForMake = $PspHeaderPath -replace "\\", "/"
    $BuildDirForMake = "build/hst"
    # Forward-slash form of the (possibly -VulkanSdk-overridden) SDK path for Make.
    # The override must reach Make's compile/link flags, not only glslc (issue #52).
    $VulkanSdkForMake = $VulkanSdk -replace "\\", "/"

    function Get-HstMakeBaseArgs {
        if ($null -ne $script:TitleManagerMakeArgs) {
            $args = @($script:TitleManagerMakeArgs)
            if ($RuntimeOpt) { $args += "RUNTIME_OPT=-$RuntimeOpt" }
            if ($RecompOpt) { $args += "RECOMP_OPT=-$RecompOpt" }
            return $args
        }
        $args = @(
            "GAME_NAME=hst",
            "GAME_ELF=$GameElfForMake",
            "GAME_BASE=0",
            "GAME_ENTRY=0",
            "VULKAN_SDK=$VulkanSdkForMake"
        )
        if ($RuntimeOpt) { $args += "RUNTIME_OPT=-$RuntimeOpt" }
        if ($RecompOpt) { $args += "RECOMP_OPT=-$RecompOpt" }
        if ($FuncsPerChunk -gt 0) { $args += "FUNCS_PER_CHUNK=$FuncsPerChunk" }
        return $args
    }

    if ($TitleManifest) {
        $plannerScript = Join-Path $PSScriptRoot "tools\title_codegen_plan.py"
        $effectiveFuncsPerChunk = if ($FuncsPerChunk -gt 0) { $FuncsPerChunk } else { 2000 }
        $script:TitleManagerPlan = Invoke-TitleManagerPlan `
            -PlannerScript $plannerScript `
            -ManifestPath $TitleManifest `
            -GameName "hst" `
            -GameElf $GameElfForMake `
            -BuildDir $BuildDirForMake `
            -ModuleDir $ModuleDirForMake `
            -PspHeader $PspHeaderForMake `
            -FuncsPerChunk $effectiveFuncsPerChunk
        $boundPlan = Get-HstManifestMakeArgs `
            -Plan $script:TitleManagerPlan `
            -GameElfForMake $GameElfForMake `
            -ModuleDirForMake $ModuleDirForMake `
            -PspHeaderForMake $PspHeaderForMake `
            -VulkanSdkForMake $VulkanSdkForMake `
            -BuildDir $BuildDirForMake `
            -FuncsPerChunk $effectiveFuncsPerChunk
        $script:TitleManagerMakeArgs = @($boundPlan.MakeArgs)
        # The analyzer reads this protected title semantic from its environment. It is
        # deliberately recorded (not exported) here: Start-ScopedMake applies it across the
        # make spawn only, so the manager never leaves the caller's session mutated. The
        # legacy path is untouched and never sees the variable.
        $script:TitleManagerSpans = $boundPlan.Environment.HST_EXTRA_SPANS
        Write-Host "Using opt-in title manifest: $($script:TitleManagerPlan.title_manifest_id)" -ForegroundColor DarkGray
    }

    # Verify workspace prerequisites. The legacy path deliberately retains its historical
    # warning-only discovery and ISO/asset checks. Manifest builds defer to the plan's
    # action-specific private-binding checks below so BuildFull/BuildFast do not require
    # runtime-only ISO/assets.
    $MissingPrereqs = @()
    if (-not $TitleManifest) {
        if (-not $GameIsoPath) {
            $MissingPrereqs += "one ISO at place_game_here/ISO/<game>.iso (or legacy game.iso)"
        }
        if (-not $GameElfPath) {
            $MissingPrereqs += "place_game_here/EBOOT.elf (or legacy eboot.elf)"
        }
        foreach ($runtimeInput in @(
            "place_game_here\EXTRACTED\PSP_GAME\SYSDIR\EBOOT.BIN",
            "place_game_here\EXTRACTED\PSP_GAME\USRDIR\xbdata_extracted",
            "place_game_here\EXTRACTED\decrypted\libfont.prx",
            "place_game_here\EXTRACTED\decrypted\scePsmf_library.prx",
            "place_game_here\EXTRACTED\decrypted\scePsmfP_library.prx"
        )) {
            if (-not (Test-Path -LiteralPath $runtimeInput)) {
                $MissingPrereqs += $runtimeInput
            }
        }
    }

    # Log directory — all generated diagnostic logs go here, anchored to the canonical
    # repository root so an alternate caller CWD can never redirect them.
    $LogDir = Join-Path $script:RepoRoot "logs"
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null

    # Identity records of build/make process trees THIS manager launches. Cleanup
    # terminates only roots whose recorded identity (PID + creation time + executable
    # path + command line) still matches, never a reused PID or a same-named compiler
    # owned by another workspace. A numeric PID alone is not identity.
    $script:BuildPidFile = Join-Path $LogDir ".build_pids"
    $script:BuildToolNames = @(
        "make", "mingw32-make", "gcc", "g++", "c++", "cc1", "cc1plus",
        "collect2", "as", "ld", "ld.lld", "lld", "lld-link", "cpp", "windres", "ar"
    )

    # Helper Functions

    # Run/oracle helpers whose failure modes are silent (a wait that outlives the process,
    # an evidence directory that merges two runs, a truncated capture set that reads as
    # complete). They live in their own file so tools/test_visual_oracle.py can exercise
    # them without a game build; see tools/hst_run_support.ps1.
    $RunSupport = Join-Path $PSScriptRoot "tools\hst_run_support.ps1"
    if (-not (Test-Path -LiteralPath $RunSupport)) {
        throw "Missing required helper: $RunSupport"
    }
    . $RunSupport

    function Register-BuildProcess {
        # Record a launched build/make PID together with its creation time, executable
        # path and command line, so a later cleanup run can prove identity before it
        # terminates anything. A bare PID is never recorded.
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
        # Drop a build record once its build finished, so the ledger never accumulates
        # identities the OS could later attach to unrelated processes.
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

    function Stop-WorkspaceHst {
        # Terminate only an hst.exe launched from THIS workspace's build directory,
        # matched by full canonical executable path, never every process named "hst".
        $exeFull = $null
        try {
            $rp = Resolve-Path -LiteralPath (Join-Path $script:RepoRoot "build\hst\hst.exe") -ErrorAction SilentlyContinue
            if ($rp) { $exeFull = [IO.Path]::GetFullPath($rp.Path) }
        } catch { }
        if (-not $exeFull) { return }
        Get-Process -Name hst -ErrorAction SilentlyContinue | Where-Object {
            $ppath = $null
            try { $ppath = [IO.Path]::GetFullPath($_.Path) } catch { $ppath = $null }
            $ppath -and ($ppath -ieq $exeFull)
        } | Stop-Process -Force -ErrorAction SilentlyContinue
    }

    function Stop-BuildProcesses {
        # Workspace-scoped pre-build cleanup. Kills only recorded build roots whose full
        # identity (PID + creation time + exe + command line) still matches, plus their
        # compiler children. Unverifiable or reused identities are left alive; legacy
        # bare-PID records are discarded, never trusted. hst.exe is matched by exact
        # workspace path only. Nothing is killed machine-wide by image name.
        Write-Host "Clearing this workspace's stale build/runtime processes..." -ForegroundColor Yellow
        $cleanup = Invoke-StaleBuildCleanup -PidFile $script:BuildPidFile `
            -WorkspaceRoot $script:RepoRoot -BuildToolNames $script:BuildToolNames
        if ($cleanup.Malformed.Count -gt 0) {
            Write-Host "[!] Discarding $($cleanup.Malformed.Count) legacy/unverifiable build-PID record(s); nothing was killed on their account." -ForegroundColor Yellow
        }
        Stop-WorkspaceHst
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

    # Spawn make with the manifest's analyzer span scoped to the child process. A child
    # snapshots its environment at creation, so the value is applied only across the spawn
    # itself and unwound in a finally on both the success and the failure path.
    function Start-ScopedMake {
        param([Parameter(Mandatory=$true)][hashtable]$StartProcess)
        # PowerShell Core exposes -WindowStyle only on Windows.  The manager's
        # Linux CI tests use a synthetic make executable, so discard that
        # presentation-only option there while preserving hidden build windows
        # on the supported Windows host.
        $startParams = $StartProcess.Clone()
        if (-not [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
                [System.Runtime.InteropServices.OSPlatform]::Windows)) {
            $startParams.Remove("WindowStyle")
        }
        if (-not $TitleManifest) { return (Start-Process @startParams) }
        # Time-of-check/time-of-use: the plan was validated once at start-up, but Make
        # runs later. Re-derive the manifest's protected digest now and refuse to build
        # when the file on disk no longer matches the plan the arguments came from.
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
        if (-not $TitleManifest) { return }
        $missing = @()
        if ($script:TitleManagerPlan.private_binding_requirements.game_elf -and -not $GameElfPath) {
            $missing += "place_game_here/EBOOT.elf (or legacy eboot.elf)"
        }
        if ($script:TitleManagerPlan.private_binding_requirements.module_dir) {
            if (-not (Test-Path -LiteralPath $ModuleDirPath -PathType Container)) {
                $missing += $ModuleDirPath
            } else {
                foreach ($module in @($script:TitleManagerPlan.required_guest_modules)) {
                    $modulePath = Join-Path $ModuleDirPath $module.name
                    if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
                        $missing += $modulePath
                    }
                }
            }
        }
        if ($script:TitleManagerPlan.private_binding_requirements.psp_header -and
            -not (Test-Path -LiteralPath $PspHeaderPath -PathType Leaf)) {
            $missing += $PspHeaderPath
        }
        if ($Runtime) {
            if (-not $GameIsoPath) {
                $missing += "one ISO at place_game_here/ISO/<game>.iso (or legacy game.iso)"
            }
            $dataRoot = "place_game_here\EXTRACTED\PSP_GAME\USRDIR\xbdata_extracted"
            if (-not (Test-Path -LiteralPath $dataRoot -PathType Container)) {
                $missing += $dataRoot
            }
        }
        if ($missing.Count -gt 0) {
            throw "Manifest mode is missing required private binding(s): $($missing -join ', ')"
        }
    }

    function Copy-RequiredAssets {
        $buildDir = "build\hst"
        if (Test-Path "SDL3.dll") {
            Copy-Item "SDL3.dll" (Join-Path $buildDir "SDL3.dll") -Force -ErrorAction SilentlyContinue
            Write-Host "SDL3.dll verified next to binary." -ForegroundColor Gray
        }
        if (Test-Path "font") {
            Copy-Item "font" (Join-Path $buildDir "font") -Recurse -Force -ErrorAction SilentlyContinue
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
        $recompDir = "build\hst"
        if (-not (Test-Path $recompDir)) {
            Write-Warning "No compilation target directory exists to scan."
            return
        }
        $files = Get-ChildItem (Join-Path $recompDir "hst_recomp_*.c") -ErrorAction SilentlyContinue
        if ($files.Count -eq 0 -and (Test-Path (Join-Path $recompDir "hst_recomp.c"))) {
            $files = Get-Item (Join-Path $recompDir "hst_recomp.c")
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

    # Find-Symbol: looks up a hex address or symbol name in OpenGrip's functions.csv
    # reference (docs/opengrip_ref/functions.csv or OpenGrip_For_Inspiration/functions.csv).
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
            Write-Warning "functions.csv not found. See docs/PARALLEL_WORK.md for setup."
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

    # Invoke-Selftest: builds and runs src/ref/selftest.cpp via make selftest.
    function Invoke-Selftest {
        $makeExe = Find-MakeExecutable
        if (-not $makeExe) {
            Write-BuildError -Message "Could not find make executable."
            return $false
        }
        $args = @(Get-HstMakeBaseArgs) + @("selftest", "--no-print-directory")
        Write-Host "Building and running selftest..." -ForegroundColor Cyan
        $proc = Start-Process -FilePath $makeExe -ArgumentList $args -PassThru -NoNewWindow -Wait
        if ($proc.ExitCode -ne 0) {
            Write-Host "[FAIL] selftest exited with code $($proc.ExitCode)." -ForegroundColor Red
            return $false
        }
        Write-Host "[PASS] selftest OK." -ForegroundColor Green
        return $true
    }

    # Invoke-VerifySuite: the full non-interactive verification set, mirroring the CI gates.
    # Covers what Invoke-Selftest alone does not: the Python unit suite, the white-box
    # scheduler/callback selftest, and the publication/import audits.
    function Invoke-VerifySuite {
        $makeExe = Find-MakeExecutable
        if (-not $makeExe) {
            Write-BuildError -Message "Could not find make executable."
            return $false
        }
        $failed = @()
        # Per-gate machine-readable status. The summary line emitted at the end is the
        # stable contract for tooling: every gate here is PASS/FAIL/SKIP, and the
        # external-oracle/private-input gates that are deliberately outside this suite are
        # listed as NOT_RUN so a consumer can never mistake their absence for a pass.
        $gateStatus = @{}
        $makeBaseArgs = @(Get-HstMakeBaseArgs)

        # Native stdout is part of a function's output stream, which the caller's
        # [void](...) would discard. Out-Host writes straight to the console instead.
        Write-Host "`n[1/16] Python unit suite (tools/test_*.py)..." -ForegroundColor Cyan
        & python -m unittest discover -s tools -p "test_*.py" | Out-Host
        if ($LASTEXITCODE -ne 0) { $failed += "python-unittest"; $gateStatus["python-unittest"] = "FAIL" } else { $gateStatus["python-unittest"] = "PASS" }

        Write-Host "`n[2/16] Scheduler/callback selftest (src/rt/sched_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("sched-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -ne 0) { $failed += "sched-selftest"; $gateStatus["sched-selftest"] = "FAIL" } else { $gateStatus["sched-selftest"] = "PASS" }

        Write-Host "`n[3/16] Profiler hash-table selftest (src/rt/profiler_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("profiler-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $MakeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "profiler-selftest"; $gateStatus["profiler-selftest"] = "FAIL" } else { $gateStatus["profiler-selftest"] = "PASS" }

        # Guest-heap boundary-tag coalescing (#122). The Makefile target landed with the
        # allocator fix but was never reachable from this route, so a coalescing regression
        # would not have failed a local Verify run.
        Write-Host "`n[4/16] Guest-heap allocator selftest (src/rt/heap_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("heap-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { $failed += "heap-selftest"; $gateStatus["heap-selftest"] = "FAIL" } else { $gateStatus["heap-selftest"] = "PASS" }

        Write-Host "`n[5/16] Extracted-asset index selftest (src/rt/asset_index_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("asset-index-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -ne 0) { $failed += "asset-index-selftest"; $gateStatus["asset-index-selftest"] = "FAIL" } else { $gateStatus["asset-index-selftest"] = "PASS" }

        Write-Host "`n[6/16] Production HLE ThreadMan selftest (src/rt/hle_thread_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("hle-thread-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -ne 0) { $failed += "hle-thread-selftest"; $gateStatus["hle-thread-selftest"] = "FAIL" } else { $gateStatus["hle-thread-selftest"] = "PASS" }

        Write-Host "`n[7/16] Portable FPU/VFPU conversion selftest (src/rt/fp_convert_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("fp-convert-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -ne 0) { $failed += "fp-convert-selftest"; $gateStatus["fp-convert-selftest"] = "FAIL" } else { $gateStatus["fp-convert-selftest"] = "PASS" }

        Write-Host "`n[8/16] VFPU table loader selftest (src/rt/vfpu_tables_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("vfpu-tables-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -ne 0) { $failed += "vfpu-tables-selftest"; $gateStatus["vfpu-tables-selftest"] = "FAIL" } else { $gateStatus["vfpu-tables-selftest"] = "PASS" }

        Write-Host "`n[9/16] Watchpoints-file parser selftest (src/rt/watchpoints_file_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("watchpoints-file-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -ne 0) { $failed += "watchpoints-file-selftest"; $gateStatus["watchpoints-file-selftest"] = "FAIL" } else { $gateStatus["watchpoints-file-selftest"] = "PASS" }

        Write-Host "`n[10/16] VFPU interpreter selftest (src/rt/vfpu_interp_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("vfpu-interp-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -ne 0) { $failed += "vfpu-interp-selftest"; $gateStatus["vfpu-interp-selftest"] = "FAIL" } else { $gateStatus["vfpu-interp-selftest"] = "PASS" }

        Write-Host "`n[11/16] VFPU source/destination overlap selftest (src/rt/vfpu_overlap_selftest.c)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("vfpu-overlap-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -ne 0) { $failed += "vfpu-overlap-selftest"; $gateStatus["vfpu-overlap-selftest"] = "FAIL" } else { $gateStatus["vfpu-overlap-selftest"] = "PASS" }

        Write-Host "`n[12/16] Reference interpreter selftest (src/ref/selftest.cpp)..." -ForegroundColor Cyan
        if (-not (Invoke-Selftest)) { $failed += "selftest"; $gateStatus["ref-selftest"] = "FAIL" } else { $gateStatus["ref-selftest"] = "PASS" }

        Write-Host "`n[13/16] Import-coverage and fake-success audit gate..." -ForegroundColor Cyan
        & python tools/import_audit_gate.py | Out-Host
        if ($LASTEXITCODE -ne 0) { $failed += "import-audit-gate"; $gateStatus["import-audit-gate"] = "FAIL" } else { $gateStatus["import-audit-gate"] = "PASS" }

        # Both content sources, because "Verify passed" has to be true of every byte this
        # checkout could publish. The pre-commit hook audits staged blobs and is right to:
        # it runs with unstaged changes stashed, so for it the index IS the tree. Verify has
        # no such guarantee, and auditing staged blobs alone let an unstaged publication
        # finding sit on disk while this suite reported PASS. Auditing worktree bytes alone
        # would just move the blind spot onto staged-but-uncommitted content, so run both.
        # --tracked-only selects the path set in each case; --worktree selects the bytes.
        # --provenance-self-consistency scopes the provenance ledger check to candidate-internal
        # consistency (coverage, resolution, content hashes). This checkout cannot attest against
        # the trusted release evidence: attestation authenticity is only asserted by the release
        # flow, which supplies the externally trusted ledger via publish_audit --provenance-ledger.
        Write-Host "`n[14/16] Publication safety audit (staged blobs)..." -ForegroundColor Cyan
        & python tools/publish_audit.py --tracked-only --provenance-self-consistency | Out-Host
        if ($LASTEXITCODE -ne 0) { $failed += "publish-audit-index"; $gateStatus["publish-audit-index"] = "FAIL" } else { $gateStatus["publish-audit-index"] = "PASS" }

        Write-Host "      ... and the working tree on disk..." -ForegroundColor Cyan
        & python tools/publish_audit.py --tracked-only --worktree --provenance-self-consistency | Out-Host
        if ($LASTEXITCODE -ne 0) { $failed += "publish-audit-worktree"; $gateStatus["publish-audit-worktree"] = "FAIL" } else { $gateStatus["publish-audit-worktree"] = "PASS" }

        Write-Host "`n[15/16] GPU framebuffer coherence selftest (gpu-coherence-selftest)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("gpu-coherence-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -eq 77) {
            Write-Host "[SKIP] gpu-coherence-selftest: Vulkan initialization unavailable" -ForegroundColor Yellow
            $gateStatus["gpu-coherence-selftest"] = "SKIP"
        } elseif ($p.ExitCode -ne 0) {
            $failed += "gpu-coherence-selftest"; $gateStatus["gpu-coherence-selftest"] = "FAIL"
        } else {
            $gateStatus["gpu-coherence-selftest"] = "PASS"
        }

        Write-Host "`n[16/16] GPU present-capture selftest (gpu-capture-selftest)..." -ForegroundColor Cyan
        $a = $makeBaseArgs + @("gpu-capture-selftest", "--no-print-directory")
        $p = Start-Process -FilePath $makeExe -ArgumentList $a -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -eq 77) {
            Write-Host "[SKIP] gpu-capture-selftest: Vulkan or validation layer unavailable" -ForegroundColor Yellow
            $gateStatus["gpu-capture-selftest"] = "SKIP"
        } elseif ($p.ExitCode -ne 0) {
            $failed += "gpu-capture-selftest"; $gateStatus["gpu-capture-selftest"] = "FAIL"
        } else {
            $gateStatus["gpu-capture-selftest"] = "PASS"
        }

        Write-Host ""
        if ($failed.Count -gt 0) {
            Write-Host "[FAIL] Verification failed: $($failed -join ', ')" -ForegroundColor Red
        } else {
            Write-Host "[PASS] All verification suites passed." -ForegroundColor Green
        }
        $summaryOrder = @(
            "python-unittest", "sched-selftest", "profiler-selftest", "heap-selftest",
            "asset-index-selftest", "hle-thread-selftest", "fp-convert-selftest",
            "vfpu-tables-selftest", "watchpoints-file-selftest", "vfpu-interp-selftest",
            "vfpu-overlap-selftest", "ref-selftest", "import-audit-gate",
            "publish-audit-index", "publish-audit-worktree",
            "gpu-coherence-selftest", "gpu-capture-selftest"
        )
        $summaryParts = @(foreach ($name in $summaryOrder) { "$name=$($gateStatus[$name])" })
        # External-oracle/private-input gates are deliberately outside this suite; name them
        # explicitly so a machine consumer cannot mistake their absence for a pass.
        $summaryParts += "make-verify=NOT_RUN(private PPSSPP oracle traces absent)"
        $summaryParts += "atrac3p-title-accept=NOT_RUN(private title stream absent)"
        $summaryParts += "visual-oracle=NOT_RUN(private title route required)"
        $summaryAggregate = if ($failed.Count -gt 0) { "FAIL" } else { "PASS" }
        Write-Host ("VERIFY_SUMMARY aggregate={0} {1}" -f $summaryAggregate, ($summaryParts -join " "))
        if ($failed.Count -gt 0) { return $false }
        return $true
    }

    # Invoke-VisualOracle: one deterministic route replay for a visual regression oracle.
    #
    # A visual-oracle run cares about a handful of frames around one transition, but the plain
    # Run route replays the whole thing with snapshots on from vblank 0 and stops on a wall-clock
    # -Duration guess. Guessing low truncates the route before its last inputs fire; guessing high
    # replays a finished scene for minutes. This wraps the same runtime with three host-side
    # controls and reports the numbers needed to compare runs:
    #
    #   SR_EXIT_AT_VBLANK  stop once the guest has delivered N vblanks (the unit routes are
    #                      written in), so the stop point is machine-independent
    #   SR_FBSNAP_AFTER    suppress captures before the transition window
    #   SR_FBSNAP          capture cadence inside it
    #
    # None of these skip guest work, alter pacing, or change rendering: the guest executes every
    # vblank exactly as it would under Run, and the frames that are captured are byte-identical.
    # It reuses whatever hst.exe is already built -- build once, replay many.
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
        $buildDir = "build\hst"
        $exePath = Join-Path $buildDir "hst.exe"
        if (-not (Test-Path $exePath)) {
            Write-Host "[!] $exePath not found - run BuildFast or BuildFull first." -ForegroundColor Red
            return $false
        }
        if (-not $Route) {
            Write-Host "[!] -Route <pad file> is required." -ForegroundColor Red
            return $false
        }
        if (-not (Test-Path $Route)) {
            Write-Host "[!] Route not found: $Route" -ForegroundColor Red
            return $false
        }
        if ($ExitAtVblank -le 0) {
            Write-Host "[!] -ExitAtVblank <N> is required; it is what makes the run terminate." -ForegroundColor Red
            return $false
        }
        # OracleName is an identifier, not a path. An explicit name must already match the
        # safe component grammar (validated at binding); a name derived from -Route is
        # sanitized to that grammar before it becomes a path component under logs/.
        if (-not $OracleName) {
            $OracleName = [System.IO.Path]::GetFileNameWithoutExtension($Route)
            $sanitized = ($OracleName -replace '[^A-Za-z0-9._-]', '_').TrimStart('.')
            if ($sanitized -match '^[A-Za-z0-9][A-Za-z0-9._-]*$' -and $sanitized.Length -le 64 -and -not $sanitized.Contains('..')) {
                $OracleName = $sanitized
            } else {
                Write-Host "[!] Cannot derive a safe oracle name from route: $Route" -ForegroundColor Red
                return $false
            }
        }
        if (-not (Test-SafeComponentName -Name $OracleName)) {
            Write-Host "[!] Invalid oracle name: '$OracleName' (single alphanumeric component of at most 64 chars)." -ForegroundColor Red
            return $false
        }

        # An oracle directory must hold exactly one run. Snapshots are numbered per run, so
        # a shorter second run leaves the first run's tail behind and the mixed set still
        # looks complete -- silently contaminating the evidence a #29-class comparison rests on.
        # The archive lives under the logs root and Reset-OracleArchive refuses anything
        # outside it (including reparse-point escapes) before deleting a single file.
        $outDir = Join-Path $script:LogDir "oracle_$OracleName"
        if (-not (Reset-OracleArchive -Path $outDir -AllowedRoot $script:LogDir -Overwrite:$OverwriteOracle)) { return $false }
        # Same hazard in the workspace directory the runtime writes captures into: remove
        # only plain snap_*.ppm files under the repo root, and fail closed if that is not
        # possible so a truncated set is never mixed with a new run's captures.
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

        # Identify the exact inputs BEFORE the run, so a manifest can never describe a
        # different build or route than the one that produced the captures.
        $exeHash   = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
        $routeHash = (Get-FileHash -LiteralPath $Route  -Algorithm SHA256).Hash
        $gitHead   = (& git rev-parse HEAD 2>$null)
        if ($LASTEXITCODE -ne 0 -or -not $gitHead) { $gitHead = "unknown" }
        $gitDirty  = [bool](& git status --porcelain 2>$null)

        # A route replay is deterministic in its INPUTS only. The guest's persisted save has
        # to be held still too, or two runs are not two samples of the same thing. The save
        # baseline and live root must both be canonical directories inside the repo root;
        # Sync-SaveBase refuses overlap, restores transactionally with a verified stage and
        # a rollback shelter, and never touches *GAMEDATA.
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
        # The runtime self-terminates at SR_EXIT_AT_VBLANK and Run-HstEngine now returns the
        # moment it does, so this is a pure safety deadline for a hung run -- it costs nothing
        # on a healthy one. ~10 guest vblanks/second is well below anything observed (30-50),
        # so it cannot cut a healthy run short.
        $backstop = [int][math]::Max(180, [math]::Ceiling($ExitAtVblank / 10.0))
        Run-HstEngine -Profile $RunProfile -RunDuration $backstop
        $sw.Stop()
        $runResult = $script:LastRunResult

        $errLog = Join-Path $LogDir "stderr_run.log"
        $vblanks = 0
        $reachedExit = $false
        if (Test-Path $errLog) {
            $m = Select-String -Path $errLog -Pattern 'phase=exit_at_vblank vblanks=(\d+)' | Select-Object -Last 1
            if ($m) { $vblanks = [int]$m.Matches[0].Groups[1].Value; $reachedExit = $true }
            Copy-Item $errLog (Join-Path $outDir "stderr.log") -Force
        }
        $captures = @(Get-ChildItem (Join-Path (Get-Location) "snap_*.ppm") -ErrorAction SilentlyContinue)
        $captures | ForEach-Object { Copy-Item $_.FullName (Join-Path $outDir $_.Name) -Force }
        # A Benchmark run's telemetry belongs with the captures it describes, not in the
        # shared logs/ slot the next run overwrites. Only under Benchmark: any other profile
        # would archive a leftover perf.csv from some earlier run as if it described this one.
        if ($RunProfile -eq "Benchmark") {
            $perfCsv = Join-Path $LogDir "perf.csv"
            if (Test-Path $perfCsv) { Copy-Item $perfCsv (Join-Path $outDir "perf.csv") -Force }
        }

        $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)
        $rate = if ($secs -gt 0 -and $vblanks -gt 0) { [math]::Round($vblanks / $secs, 1) } else { 0 }
        $verdict = Get-OracleVerdict -ReachedExit $reachedExit -TimedOut $runResult.TimedOut `
                       -ExitCode $runResult.ExitCode -CaptureCount $captures.Count `
                       -RequestedVblank $ExitAtVblank -ObservedVblank $vblanks

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
                   "timed_out=$($runResult.TimedOut) reached_exit=$reachedExit complete=$($verdict.Complete)"
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

    # Invoke-DiffFunc: runs hst.exe in --diff-func mode against a reference trace.
    # Requires: a reference oracle trace file and the target function address.
    function Invoke-DiffFunc {
        param(
            [string]$Target,    # e.g. f_00010738 or 0x00010738
            [string]$Oracle,    # path to reference .trace
            [int]$Step = 0      # entry step in oracle trace
        )
        $buildDir = "build\hst"
        $exePath = Join-Path $buildDir "hst.exe"
        $imagePath = Join-Path $buildDir "hst_image.bin"
        if (-not (Test-Path $exePath)) {
            Write-Host "[!] hst.exe not found - run BuildFast first." -ForegroundColor Red
            return
        }
        if (-not (Test-Path $Oracle)) {
            Write-Host "[!] Oracle trace not found: $Oracle" -ForegroundColor Red
            return
        }
        # Normalise address: strip leading f_ prefix if present
        $addr = $Target -replace '^f_',''
        $outTrace = "diff_${addr}.trace"
        Write-Host "DiffFunc: target=0x$addr oracle=$Oracle step=$Step" -ForegroundColor Cyan
        $args = @("--image", $imagePath, "0", "0x0029a060", $outTrace, "none",
                  "--diff-func=0x$addr", "--diff-oracle=$Oracle", "--diff-step=$Step")
        $proc = Start-Process -FilePath $exePath -ArgumentList $args -PassThru -NoNewWindow -Wait `
            -RedirectStandardError "$LogDir/difffunc_err.log"
        Write-Host "DiffFunc finished (exit $($proc.ExitCode)). Output: $outTrace" -ForegroundColor $(if ($proc.ExitCode -eq 0) { "Green" } else { "Yellow" })
        if (Test-Path "$LogDir/difffunc_err.log") {
            Get-Content "$LogDir/difffunc_err.log" -Tail 10 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        }
        if (Test-Path $outTrace) {
            Write-Host "Comparing with oracle via funcdiff_cmp.py..." -ForegroundColor Cyan
            python tools/funcdiff_cmp.py $outTrace $Oracle 2>&1 | Select-Object -First 20 |
                ForEach-Object { Write-Host "  $_" }
        }
    }

    function Invoke-HstBuild {
        param(
            [Parameter(Mandatory=$true)]
            [ValidateSet("Fast", "Full")]
            [string]$Mode
        )

        Assert-TitleManagerPrivateBindings
        Stop-BuildProcesses
        $buildDir = "build\hst"
        if (-not (Test-Path $buildDir)) {
            New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
        }

        $baseArgs = @(Get-HstMakeBaseArgs)

        # Both modes invoke `mingw32-make ... all` (pipeline + compile), not just `compile`,
        # so a clean checkout (no build/hst/hst_recomp*.c yet) generates the MIPS chunks
        # before linking instead of failing with missing chunk objects. `pipeline`'s targets
        # are make-dependency-gated on codegen.py/analyze.py/prxload.py/the ELF, so on a
        # checkout that already has current generated C this is a no-op beyond the `compile`
        # relink Fast/Full always did — it does not defeat Fast mode's "preserve compiled
        # MIPS chunks" intent.
        switch ($Mode) {
            "Fast" {
                $makeExe = Find-MakeExecutable
                if (-not $makeExe) {
                    Write-BuildError -Message "Could not resolve make tools in active PATH environment."
                    return $false
                }

                Write-Host "Running dependency/profile-aware incremental build..." -ForegroundColor Green
                $args = $baseArgs + @("--no-print-directory", "-j1", "all")
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
                $makeExe = Find-MakeExecutable
                if (-not $makeExe) {
                    Write-BuildError -Message "Could not resolve make tools in active PATH environment."
                    return $false
                }

                Write-Host "Removing the complete target build for a true full rebuild..." -ForegroundColor Green
                $cleanArgs = $baseArgs + @("--no-print-directory", "clean")
                $cleanProc = Start-Process -FilePath $makeExe -ArgumentList $cleanArgs -PassThru -NoNewWindow -Wait
                if ($cleanProc.ExitCode -ne 0) {
                    Write-BuildError -Message "Could not clean the HST build directory."
                    return $false
                }

                $args = $baseArgs + @("--no-print-directory", "-j1", "all")
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
                    $recompSize = if (Test-Path (Join-Path $buildDir "hst_recomp.o")) { (Get-Item (Join-Path $buildDir "hst_recomp.o")).Length } else { 0 }
                    $exeState = if (Test-Path (Join-Path $buildDir "hst.exe")) { "Ready" } else { "Building" }

                    Write-Host ("[{0}] stdout: {1}b | stderr: {2}b | recomp.o: {3}b | status: {4}" -f `
                        (Get-Date -Format "HH:mm:ss"), $outSize, $errSize, $recompSize, $exeState) -ForegroundColor Gray
                    Start-Sleep -Seconds 15
                }

                # Finalize through the handle-caching wait so the redirected child's exit
                # code is actually readable. Unknown exit status is failure: a leftover
                # hst.exe does NOT prove a successful link (the target was cleaned before
                # this build, but "unknown" must stay unknown).
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
                # A clean rebuild that reports success must actually have produced the target.
                $fullExe = Join-Path $buildDir "hst.exe"
                if (-not (Test-Path -LiteralPath $fullExe)) {
                    Write-BuildError -Message "Full rebuild reported success but $fullExe was not produced."
                    return $false
                }
                $script:LastBuildInfo = @{ Known = $true; ExitCode = $makeExitCode }
            }
        }

        # Refresh the Clang compilation database (compile_commands.json) so clangd and other
        # clang-based tooling see the current runtime + generated-chunk flags. A `mingw32-make
        # -Bnwk` dry-run reprints every compile command WITHOUT recompiling; the -B (always-make)
        # is required so make prints the chunk/runtime recipes even when their outputs are
        # up-to-date. The generated hst_recomp_*.c chunks already exist from the build above, so
        # the Makefile's $(wildcard) resolves and every current chunk entry is captured. A dry-run on a
        # *clean* tree would miss them, which is why this runs only after a successful build
        # (both Fast and Full converge here). compiledb parses the dry-run log rather than invoking
        # make itself, because it only wraps a literal `make` binary (not `mingw32-make`).
        $compiledbExe = Get-Command compiledb -ErrorAction SilentlyContinue
        if ($compiledbExe) {
            Write-Host "[compiledb] refreshing compile_commands.json (dry-run)..." -ForegroundColor DarkGray
            try {
                $dryRunLog = Join-Path $env:TEMP "hst_compiledb_dryrun.log"
                $dryRunArgs = @("-Bnwk") + $baseArgs + @("all", "selftest")
                & $makeExe @dryRunArgs 2>$null > $dryRunLog
                & compiledb -p $dryRunLog -o compile_commands.json 2>&1 | Out-Null
                if (Test-Path compile_commands.json) {
                    # Normalize Windows backslashes to forward slashes and re-emit valid JSON.
                    # Guarded: a parse failure leaves compiledb's original output untouched.
                    try {
                        python -c "import json; d=json.load(open('compile_commands.json')); [e.update({'directory': e['directory'].replace(chr(92),'/'), 'file': e.get('file','').replace(chr(92),'/')}) for e in d]; json.dump(d, open('compile_commands.json','w'), indent=2)" 2>$null
                    } catch {
                        Write-Host "[compiledb] sanitize skipped (compile_commands.json left as-is)." -ForegroundColor DarkGray
                    }
                }
            } catch {
                Write-Host "[compiledb] generation failed; keeping previous compile_commands.json (if any)." -ForegroundColor DarkGray
            }
        } else {
            Write-Host "[compiledb] not installed - skipping compile_commands.json (run: pip install compiledb)." -ForegroundColor DarkGray
        }

        Copy-RequiredAssets
        # Success is a known-zero exit from the make invocation above, never the existence
        # of a possibly stale binary. Record a fresh exact-build manifest for evidence
        # consumers (executable hash, git head, mode, timestamp).
        if ($null -ne $script:LastBuildInfo -and $script:LastBuildInfo.Known -and $script:LastBuildInfo.ExitCode -eq 0) {
            Write-HstBuildManifest -Mode $Mode -ExitCode $script:LastBuildInfo.ExitCode
        }
        return $true
    }

    function Write-HstBuildManifest {
        <# Record a fresh exact-build manifest after a known-zero make result: executable
           hash, git head, mode and timestamp, so stale binaries can be distinguished from
           current-build artifacts by evidence consumers. #>
        param([string]$Mode, [int]$ExitCode)
        try {
            $exePath = Join-Path (Join-Path $script:RepoRoot "build\hst") "hst.exe"
            $gitHead = (& git rev-parse HEAD 2>$null)
            if ($LASTEXITCODE -ne 0 -or -not $gitHead) { $gitHead = "unknown" }
            $manifest = [ordered]@{
                format     = "hst-build-manifest/v1"
                mode       = $Mode
                exit_code  = $ExitCode
                exe_path   = $exePath
                exe_sha256 = if (Test-Path -LiteralPath $exePath) { (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash } else { $null }
                git_head   = $gitHead
                built_utc  = ([DateTime]::UtcNow).ToString("o")
            }
            $manifest | ConvertTo-Json -Depth 4 |
                Out-File -FilePath (Join-Path $script:LogDir "build_manifest.json") -Encoding utf8 -ErrorAction SilentlyContinue
        } catch { }
    }

    function Run-HstEngine {
        param(
            [string]$Profile = "Standard",
            [int]$RunDuration = 0,
            [switch]$NoGui
        )

        Assert-TitleManagerPrivateBindings -Runtime
        $buildDir = "build\hst"
        $exePath = Join-Path $buildDir "hst.exe"
        $imagePath = Join-Path $buildDir "hst_image.bin"

        if (-not (Test-Path $exePath)) {
            Write-Host "[!] Error: Executable target is missing: $exePath. Execute build pipeline first." -ForegroundColor Red
            return
        }

        Stop-WorkspaceHst
        Start-Sleep -Milliseconds 150

        Remove-Item "$LogDir/stdout_run.log", "$LogDir/stderr_run.log" -ErrorAction SilentlyContinue

        $env:PSP_ISO = if ($GameIsoPath) { $GameIsoPath } else { "game.iso" }
        $env:PSP_VFPU_TABLES = "assets/vfpu"

        # Profiles can be selected repeatedly in the interactive manager and may
        # inherit from the parent shell. Clear mode-specific switches first.
        $env:SR_QUIET = $null
        $env:SR_PERF = $null
        $env:SR_PERF_CSV = $null
        $env:SR_PROFILE = $null
        $env:SR_PROFILE_DUMP_VBLANKS = $null

        switch ($Profile) {
            "Standard" {
                # Quiet, production-like profile: GPU active, per-call trace logging OFF so the
                # framerate is not dragged down by thousands of synchronous stderr writes (SR_HLELOG
                # alone logs every WaitSema — 11k+ lines on the title screen). Use Diagnostics for
                # verbose subsystem/scheduler logging.
                # NOTE: these SR_* flags are presence-based getenv() checks in the runtime, so a
                # value of "0" still ENABLES them — they must be $null (unset) to be OFF.
                $env:SR_GPU_GE = "1"
                $env:SR_ALLOC_MAX = "04000000"
                # Honest startup currently spends more than 20 seconds parsing the retail data
                # before its first frame.  Keep watchdog diagnostics, but do not turn a slow
                # startup into a false process failure; -Duration remains the explicit test cap.
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
                # Visual/audio smoke test: behavior matches Standard, but both C
                # streams are discarded inside the process to avoid terminal/file I/O.
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
                # Measuring has a small cost, so keep it separate from Performance.
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
                # Logging toggles are presence-based; "0" still enables them.
                $env:SR_WAKELOG = $null
                $env:SR_GEDUMP = $null
                $env:SR_IOLOG = $null
                $env:SR_POSTUMD = $null
                $env:SR_EXITSNAP = $null
            }
        }

        # Opt-in guest hot-path profiling is orthogonal to the runtime profile above. Keep it
        # available through the canonical manager route so a Benchmark run can collect both the
        # one-second host/GE telemetry and the generated-PC call/block summary. The profiler is
        # intentionally off by default because timing every dispatched guest function adds
        # measurement overhead and its atexit report can be large.
        if ($GuestProfile) {
            $env:SR_PROFILE = "1"
            $env:SR_PROFILE_DUMP_VBLANKS = if ($GuestProfilePeriod -gt 0) {
                $GuestProfilePeriod.ToString()
            } else {
                $null
            }
        }

        # Correctness gate (2026-07-17): make a silently-permissive dispatch miss
        # impossible to mistake for real progress. A screen reached because an
        # unresolved indirect-call target returned a sentinel zero and execution
        # limped onward is not verified progress -- see ISSUES.md. Every profile
        # above is therefore SR_DISPATCH_FATAL=1 by default; the only way back to
        # the old silent-continue behavior is the explicit, alarmingly-named
        # SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS=1, set by the caller before invoking
        # this script (kept out of the profile switch on purpose, so no profile
        # can accidentally default to permissive again).
        if ($env:SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS -eq "1") {
            $env:SR_DISPATCH_FATAL = $null
            Write-Host "[!] SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS=1: dispatch misses will NOT be fatal. Any screen reached this way is not verified progress." -ForegroundColor Yellow
        } else {
            $env:SR_DISPATCH_FATAL = "1"
        }

        # A headless scheduler run never presents a host frame, so the GUI's
        # no-frame watchdog cannot distinguish healthy loading from a stall.
        # Scheduler/thread diagnostics remain available, and -Duration is the
        # explicit process bound for automated headless runs.
        if ($NoGui) {
            $env:SR_WATCHDOG_EXIT = $null
        }

        $args = @("--image", $imagePath, "0", "0x0029a060", "none", "none")
        if (-not $NoGui) {
            $args += "--gui"
        } else {
            $args += "--sched"
        }

        Write-Host "Spawning host runtime executable..." -ForegroundColor Cyan
        $proc = Start-Process -FilePath $exePath -ArgumentList $args `
            -RedirectStandardOutput "$LogDir/stdout_run.log" `
            -RedirectStandardError "$LogDir/stderr_run.log" `
            -PassThru -WindowStyle Hidden

        # -RunDuration is a DEADLINE, not a duration: the wait returns the moment hst.exe
        # exits (which SR_EXIT_AT_VBLANK makes the normal case) and only kills at the
        # deadline. It used to sleep the full span unconditionally, which turned an
        # over-generous backstop into dead wall-clock on every run and pressured callers
        # into tight guesses -- the guess that silently truncates a route.
        if ($RunDuration -gt 0) {
            Write-Host "Runtime deadline: $RunDuration seconds (returns as soon as the process exits)." -ForegroundColor Cyan
        } else {
            Write-Host "Engine active. Terminate window frame or end process sequence to complete." -ForegroundColor Cyan
        }
        $script:LastRunResult = Wait-ProcessOrKill -Process $proc -TimeoutSeconds $RunDuration
        if ($script:LastRunResult.TimedOut) {
            Write-Host "Target engine process terminated by script controller timeout after $($script:LastRunResult.ElapsedSeconds)s." -ForegroundColor Yellow
        } else {
            Write-Host "Game program terminated organically after $($script:LastRunResult.ElapsedSeconds)s. Code: $($script:LastRunResult.ExitCode)" -ForegroundColor Green
        }

        Analyze-RunLogs "$LogDir/stderr_run.log"
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

        $gpu = Select-String -Path $LogPath -Pattern "sdl3vk|gegpu|GPU GE|GE PRIM|GE FRAMEBUFPTR|GEDUMP|GELIST" -ErrorAction SilentlyContinue
        if ($gpu) {
            Write-Host "[*] Display Engine Logs:" -ForegroundColor Cyan
            $gpu | Select-Object -First 10 | ForEach-Object { Write-Host "    $($_.Line)" }
        }

        $sched = Select-String -Path $LogPath -Pattern "create thread|sched_create_thread|MAIN_THREAD|s_sys|SLEEP|WAKEUP|WaitSema|ge_run_list|sceGeListEnQueue|GEEND" -ErrorAction SilentlyContinue
        if ($sched) {
            Write-Host "[*] Threading and Kernel Scheduler Logs:" -ForegroundColor White
            $sched | Select-Object -First 12 | ForEach-Object { Write-Host "    $($_.Line)" }
        }

        Write-Host "========================================================`n" -ForegroundColor Yellow
    }

    # Warn if prereqs are missing on display but let interactive user read it
    if ($MissingPrereqs.Count -gt 0) {
        Write-Host "=== CONFIGURATION WARNINGS ===" -ForegroundColor Yellow
        Write-Host "The following baseline workspace files were not located in the current directory:" -ForegroundColor Yellow
        foreach ($p in $MissingPrereqs) {
            Write-Host "  - $p" -ForegroundColor Yellow
        }
        Write-Host "You can continue compilation, but runtime components may fail without these files present." -ForegroundColor Yellow
        Write-Host "==============================" -ForegroundColor Yellow
        Start-Sleep -Seconds 2
    }

    # Dispatch Execution Logic. Failures set $script:ManagerExitCode and break out of the
    # switch so the finally block below restores the caller's location BEFORE the single
    # `exit` at the end of the script -- an early `exit 1` would skip that cleanup.
    if ($Action) {
        # Execution of parameterized Action
        switch ($Action) {
            "BuildFull" { if (-not (Invoke-HstBuild -Mode "Full")) { $script:ManagerExitCode = 1; break } }
            "BuildFast" { if (-not (Invoke-HstBuild -Mode "Fast")) { $script:ManagerExitCode = 1; break } }
            "Fuzz" {
                Assert-TitleManagerPrivateBindings
                $makeExe = Find-MakeExecutable
                if (-not $makeExe) {
                    Write-BuildError -Message "Could not resolve make tools in active PATH environment."
                    $script:ManagerExitCode = 1; break
                }
                Write-Host "Triggering VFPU fuzzing..." -ForegroundColor Cyan
                $args = @(Get-HstMakeBaseArgs) + @("vfpu_fuzz", "--no-print-directory")
                # vfpu_fuzz runs `$(MAKE) pipeline`, so it reaches analyze.py and needs the
                # scoped span; the selftest/verify targets do not and stay unscoped.
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
                Run-HstEngine -Profile $profile -RunDuration $Duration -NoGui:$NoGui
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
        # Interactive Option Loop
        while ($true) {
            Safe-ClearHost
            Write-Host "=========================================================" -ForegroundColor Green
            Write-Host "             Nakagawa Recomp CLI Manager              " -ForegroundColor Green
            Write-Host "=========================================================" -ForegroundColor Green
            Write-Host "1) Build Fast (Runtime Changes Only) & Run Game"
            Write-Host "2) Build Full (Slow Chunk Rebuild) & Run Game"
            Write-Host "3) Recompile Runtime Only (Preserves compiled MIPS chunks)"
            Write-Host "4) Run Full Clean Recompile (Slower)"
            Write-Host "5) Run Full Verification Suite (Python tests, selftests, audit gates)"
            Write-Host "6) Run Game Executable (Select Custom Environment Profile)"
            Write-Host "7) Search Recompiled C File Chunks for Specific MIPS Function"
            Write-Host "8) Run Reference Selftest (src/ref/selftest.cpp)"
            Write-Host "9) Lookup Symbol in OpenGrip functions.csv"
            Write-Host "0) Purge Task Threads & Clean Build Tracking Logs"
            Write-Host "Q) Exit"
            Write-Host "========================================================="
            $choice = Read-Host "Select execution index [0-9/Q]"

            switch ($choice) {
                "1" {
                    if (Invoke-HstBuild -Mode "Fast") {
                        Run-HstEngine -Profile "Standard" -RunDuration 0 -NoGui:$false
                    }
                    Read-Host "Press Enter to return to menu..."
                }
                "2" {
                    if (Invoke-HstBuild -Mode "Full") {
                        Run-HstEngine -Profile "Standard" -RunDuration 0 -NoGui:$false
                    }
                    Read-Host "Press Enter to return to menu..."
                }
                "3" {
                    [void](Invoke-HstBuild -Mode "Fast")
                    Read-Host "Press Enter to return to menu..."
                }
                "4" {
                    [void](Invoke-HstBuild -Mode "Full")
                    Read-Host "Press Enter to return to menu..."
                }
                "5" {
                    [void](Invoke-VerifySuite)
                    Read-Host "Press Enter to return to menu..."
                }
                "6" {
                    Write-Host "`nSelect Operational Logging Profile:"
                    Write-Host "1) Standard Mode (Vulkan, bounded compatibility diagnostics)"
                    Write-Host "2) Performance Mode (Vulkan, stdout/stderr discarded)"
                    Write-Host "3) Benchmark Mode (Vulkan, 1 Hz metrics + logs/perf.csv)"
                    Write-Host "4) Diagnostics Mode (Verbose subsystem and scheduler hooks active)"
                    Write-Host "5) Software Fallback Mode (No GPU pipeline, pure reference)"
                    $pSelection = Read-Host "Select choice [1-5]"
                    $prof = "Standard"
                    if ($pSelection -eq "2") { $prof = "Performance" }
                    elseif ($pSelection -eq "3") { $prof = "Benchmark" }
                    elseif ($pSelection -eq "4") { $prof = "Diagnostics" }
                    elseif ($pSelection -eq "5") { $prof = "Software" }

                    # Negative, non-numeric or overflowing input must never silently become
                    # an indefinite wait: re-prompt until a valid non-negative integer arrives.
                    [int]$durVal = 0
                    do {
                        $tSelection = Read-Host "Process termination threshold (seconds, 0 for indefinite)"
                        $parsed = ConvertTo-SafeTimeoutSeconds -Text $tSelection
                        if ($null -eq $parsed) {
                            Write-Host "[!] Invalid timeout '$tSelection' - enter a non-negative integer (0 = indefinite)." -ForegroundColor Red
                        } else {
                            $durVal = [int]$parsed
                        }
                    } while ($null -eq $parsed)

                    Run-HstEngine -Profile $prof -RunDuration $durVal -NoGui:$false
                    Read-Host "Press Enter to return to menu..."
                }
                "7" {
                    $fnName = Read-Host "Enter recompiled function address/name (e.g. f_002b7ca0)"
                    if ($fnName) { Find-Function -FuncName $fnName }
                    Read-Host "`nPress Enter to return to menu..."
                }
                "8" {
                    [void](Invoke-Selftest)
                    Read-Host "Press Enter to return to menu..."
                }
                "9" {
                    $symQuery = Read-Host "Enter symbol name or hex address (e.g. Camera_Update or 47054)"
                    if ($symQuery) { Find-Symbol -Query $symQuery }
                    Read-Host "`nPress Enter to return to menu..."
                }
                "0" {
                    Stop-BuildProcesses
                    Remove-Item "$LogDir/build_out_recomp.log", "$LogDir/build_err_recomp.log", "$LogDir/recomp_err.log", "$LogDir/obj_err.log", "$LogDir/link_err.log", "$LogDir/stdout_run.log", "$LogDir/stderr_run.log" -ErrorAction SilentlyContinue
                    Write-Host "Runtime lock trackers, build logs, and pending threads terminated." -ForegroundColor Green
                    Read-Host "Press Enter to return to menu..."
                }
                { $_ -in @("Q","q") } {
                    Write-Host "Terminating recompiler session."
                    return
                }
                default {
                    Write-Host "Input index value out of range." -ForegroundColor Red
                    Start-Sleep -Seconds 1
                }
            }
        }
    }
} catch {
    Write-Host "`n[FATAL SCRIPT ERROR] Execution halted abruptly." -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host "Script location: $($_.InvocationInfo.ScriptLineNumber)" -ForegroundColor Red
    Write-Host "`nTraceback Information:" -ForegroundColor Red
    Write-Host $_.ScriptStackTrace -ForegroundColor Red
    Write-Host "`n======================================================="
    $script:ManagerExitCode = 1
    if (-not $Action) {
        Read-Host "Execution trapped. Press Enter to exit this screen"
    }
} finally {
    # Restore the caller's location so an alternate caller CWD is never left mutated,
    # on every path (including the interactive loop's return).
    if ($script:OriginalLocation) {
        Set-Location -LiteralPath $script:OriginalLocation -ErrorAction SilentlyContinue
    }
}
if ($Action -and $script:ManagerExitCode -ne 0) {
    exit $script:ManagerExitCode
}
