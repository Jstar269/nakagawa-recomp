# Release candidate scope & qualification guide

Status: CURRENT — maintained release candidate specification. Governed by release qualification authority [issue #278](https://github.com/Jstar269/nakagawa-recomp/issues/278). This document does not create a Git tag, GitHub Release, or release asset (AGENTS.md § 3).

> [!IMPORTANT]
> **SUPERSEDED VERSION PROPOSAL & HISTORICAL CANDIDATE CONTEXT:**
> An earlier draft proposed `0.1.0-preview.1` based on older source snapshots (such as base `a699000` and `b3f87be`). That proposal is formally superseded. The project roadmap has converged on **`v0.0.1`** as the first formal experimental platform release, with qualification criteria defined in [issue #278](https://github.com/Jstar269/nakagawa-recomp/issues/278). Historical preview exercises proved basic packaging feasibility for earlier trees, but they do not represent the current candidate.

## Target release version

`v0.0.1` is the targeted release version. It represents the first formal experimental public platform release from an exact, reproducible, publication-clean integrated commit on `main`.

Per `AGENTS.md` § 3, agents must not create, move, push, or delete Git tags; create, edit, delete, publish, or unpublish GitHub Releases; upload release assets; or change a published version without explicit maintainer authorization in the current turn. This document serves as the maintainer's candidate specification and reproduction procedure.

## What this release contains

The release is a Windows public-source and reproducibility package built from the sanitized source tree:

- **Synthetic production smoke:** `mingw32-make production-smoke` generates a synthetic PSP PRX fixture from checked-in generator source into `build/`, statically translates MIPS to C, compiles native code, and verifies and runs the AOT result.
- **Fail-closed dispatch floor:** `mingw32-make production-smoke-gap` runs the same synthetic workload with one function deliberately omitted from AOT emission, exercising the production dispatch seam.
- **Differential cosimulation:** `mingw32-make cosim-selftest` compares the source-owned AOT and interpreter lanes over the synthetic corpus, including its fail-closed negative cases.
- **Platform ladder:** `mingw32-make platform-ladder` builds and runs the source-owned synthetic workload ladder, including relocation, scheduler, filesystem, FPU, title-2, and negative cases.
- **Core runtime selftests:** `mingw32-make selftest sched-selftest hle-thread-selftest public-safe-verify` runs the native interpreter, scheduler, HLE/thread, and public-safe object gates.
- **Native player application:** `mingw32-make player` builds `build/nakagawa_player.exe` from `src/player/`. The standalone SDL3 desktop application provides direct C ISO9660 PVD reading, `PARAM.SFO` metadata parsing (`DISC_ID`, `TITLE`), asset census staging, gamepad navigation, and structured error views.
- **Display smoke & player launch:** `mingw32-make display-smoke` and `mingw32-make display-smoke-player` prove the two-phase pipeline, loader, NID imports, vblank delivery, display latch, and child runtime launch via the native player without proprietary inputs.
- **Media subsystem:** PSMF MPEG-PS demuxing and H.264 video decoding pipelines for in-game cutscenes.
- **User interface:** The native player is the only UI in the public repository.

The package includes both `nakagawa_player.exe` (native player desktop application) and `production_smoke.exe` (synthetic verification binary) under `bin/`.

The package also includes the public documentation selected by the packaging command, `LICENSE`, `NOTICE.md`, and the reviewed third-party notice files. It excludes generated build intermediates and title inputs.

## What this release does not do

This is the limitations section, not a footnote:

- **Not an end-user "ISO -> click Play" product release.** This release is an experimental platform milestone, not a consumer emulator. A seamless user workflow is tracked separately under the [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308) roadmap.
- **No one-click retail preparation in this build.** The release cannot auto-decrypt a retail executable, and a package build still needs the developer toolchain (Python, GNU Make, a C compiler). A commercial game therefore needs a plain executable supplied by the user (decryption is out of scope, [#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)); after that, the in-product staging wizard and the library **BUILD PACKAGE** action are connected ([#487](https://github.com/Jstar269/nakagawa-recomp/pull/487)), while zero-toolchain end-user recompilation routes (#296) remain under active development.
- **Audio output requires a connected audio device.** Public builds drive one SDL3 audio stream per sceAudio channel ([#301](https://github.com/Jstar269/nakagawa-recomp/issues/301), [#411](https://github.com/Jstar269/nakagawa-recomp/pull/411)); with no audio device the game runs silently after one message.
- **Windows 11 x64 only.** Verified on Windows 11 x64 only. It makes no Linux ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306)), macOS ([#329](https://github.com/Jstar269/nakagawa-recomp/issues/329)), or Android ([#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)) support claim.
- **Synthetic executables prove synthetic contracts only.** They do not prove commercial-title compatibility, complete Allegrex interpreter coverage, software/Vulkan agreement with a PSP, or physical PSP correctness.
- **No proprietary content.** No retail executable, ISO, asset, decrypted module, generated retail C, save, key, private trace/capture, private route, private path, or firmware font is shipped.
- **Local passes are not hosted-CI or publication authority.** The final publication requires maintainer-controlled provenance refresh, green hosted CI, and the gates described in [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md).

### Historical candidate notes

- The dated preview candidate at base `a699000` had no `src/player/` tree and no `player` Make target.
- The older `b3f87be` packaging exercise proved toolchain reproducibility for an earlier snapshot, but predated PR #277 (PSMF MPEG-2 flag corrections and media work) and #293 (public export immutability).
- These historical exercises remain recorded as packaging evidence, but they do not define the scope of `v0.0.1`.

## Supported host contract

The verified native host contract is the Windows contract in [`SETUP.md`](SETUP.md):

- Windows 11 x64.
- PowerShell 7.4 or newer; Windows PowerShell 5.1 is not supported.
- CPython 3.14.x.
- MSYS2 UCRT64 GCC/G++, GNU Make, SDL3, and Vulkan loader packages.
- Explicit SDL3 dependency discovery ([#331](https://github.com/Jstar269/nakagawa-recomp/issues/331)).
- A current Vulkan SDK and Vulkan-capable GPU.

## Reproduce from a clean checkout

The following is the source-owned Windows sequence. It uses no retail input:

```powershell
git clone https://github.com/Jstar269/nakagawa-recomp.git nakagawa-recomp-release
Set-Location .\nakagawa-recomp-release
python -m pip install .

mingw32-make --no-print-directory production-smoke
mingw32-make --no-print-directory production-smoke-gap
mingw32-make --no-print-directory cosim-selftest
mingw32-make --no-print-directory platform-ladder
mingw32-make --no-print-directory selftest
mingw32-make --no-print-directory sched-selftest
mingw32-make --no-print-directory hle-thread-selftest
mingw32-make --no-print-directory public-safe-verify
mingw32-make --no-print-directory player
mingw32-make --no-print-directory display-smoke
```

## Recreate the package

Run the following after the native smoke and player build, from a clean candidate checkout on the exact integrated release commit. The public-safe export command filters the source with `assets/public_source_profile.json`; it must pass the maintainer-controlled publication inputs before the result can be called a release asset. The package retains the source tree under `source/`, so the player can resolve its BUILD PACKAGE CLI from the installed layout.

```powershell
$Version = '0.0.1'
$CandidateSha = (git rev-parse --short=12 HEAD).Trim()
$ArtifactRoot = Join-Path $env:TEMP "nakagawa-recomp-$Version-$CandidateSha"
$TrustedLedger = $env:NAKAGAWA_TRUSTED_PUBLIC_LEDGER
$Stage = Join-Path $ArtifactRoot "nakagawa-recomp-$Version-windows-x64"
$ToolchainIdentity = Join-Path $ArtifactRoot 'toolchain.json'
$Bin = Join-Path $Stage 'bin'
$Source = Join-Path $Stage 'source'
$PackageDocs = Join-Path $Stage 'docs'
$Zip = Join-Path $ArtifactRoot "nakagawa-recomp-$Version-windows-x64.zip"

if (Test-Path -LiteralPath $ArtifactRoot) {
    throw "Candidate output already exists; choose a new artifact directory: $ArtifactRoot"
}
if (-not $TrustedLedger -or -not (Test-Path -LiteralPath $TrustedLedger -PathType Leaf)) {
    throw 'Set NAKAGAWA_TRUSTED_PUBLIC_LEDGER to the maintainer-supplied trusted public ledger copy.'
}
New-Item -ItemType Directory -Path $ArtifactRoot, $Source -Force | Out-Null
python tools/record_toolchain.py --output $ToolchainIdentity
python tools/build_public_export.py --public-safe-profile --export-dir $Source --trusted-ledger $TrustedLedger

New-Item -ItemType Directory -Path $Bin, $PackageDocs -Force | Out-Null
Copy-Item build\nakagawa_player.exe, build\production-smoke\production_smoke.exe $Bin
& .\copy_build_assets.ps1 -BuildDir $Bin
if ($LASTEXITCODE -ne 0) { throw "copy_build_assets.ps1 failed with exit code $LASTEXITCODE" }
Copy-Item LICENSE, NOTICE.md (Join-Path $Stage '.')
Copy-Item README.md (Join-Path $Stage '.')
Copy-Item docs\PREVIEW_RELEASE.md, docs\SETUP.md, docs\SMOKE_TEST.md, docs\RELEASE_NOTES_v0.0.1.md $PackageDocs
Copy-Item (Join-Path $Bin 'THIRD_PARTY_NOTICES') $Stage -Recurse
Copy-Item (Join-Path $Bin 'THIRD_PARTY_NOTICES.txt'), (Join-Path $Bin 'SOURCE.txt'), (Join-Path $Bin 'RELINK.md') $Stage

python tools/generate_sbom.py --package-dir $Bin `
    --spdx-out (Join-Path $Stage 'SBOM.spdx.json') `
    --spdx3-out (Join-Path $Stage 'SBOM.spdx3.jsonld') `
    --cyclonedx-out (Join-Path $Stage 'SBOM.cyclonedx.json')
python tools/verify_sbom.py --observed-toolchain $ToolchainIdentity --spdx (Join-Path $Stage 'SBOM.spdx.json')

Get-ChildItem -LiteralPath $Stage -Recurse -File -Force |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName |
    Get-FileHash -Algorithm SHA256 |
    ForEach-Object {
        $relative = $_.Path.Substring($Stage.Length + 1).Replace('\', '/')
        "$($_.Hash.ToLowerInvariant())  $relative"
    } |
    Set-Content -LiteralPath (Join-Path $Stage 'SHA256SUMS.txt') -Encoding utf8NoBOM

# ZipFile includes hidden source-export files such as .github and .editorconfig.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $Stage, $Zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
Write-Output $Zip
```

The package contains the native player and synthetic verification binary under `bin/`, only the host runtime DLLs found by `copy_build_assets.ps1`, the public-safe source export under `source/`, setup and smoke instructions, root README/license/notices, `SOURCE.txt`, LGPL relinking material, the third-party notice bundle, three SBOM formats, and `SHA256SUMS.txt`. It does not copy the whole `build/` tree, generated translation units, fixture PRXs, Vulkan SDK files, local databases, or title outputs. `SHA256SUMS.txt` covers every staged file except itself.

Before a maintainer treats the zip as publishable, inspect it with:

```powershell
$files = Get-ChildItem -LiteralPath $Stage -Recurse -File
$forbidden = @($files | Where-Object {
    $_.Extension -match '^\.(iso|prx|elf|eboot|sav|key|pgf|pgd)$'
})
if ($forbidden.Count -ne 0) {
    $forbidden | ForEach-Object { Write-Error $_.FullName }
    throw 'forbidden package file found'
}
$privateMatches = @(
    rg -a -l -i '[A-Z]:[\\/][^\\r\\n]*[\\/](private|\\.codex)([\\/]|$)' $Stage 2>$null
)
if ($privateMatches.Count -ne 0) {
    $privateMatches | ForEach-Object { Write-Error $_ }
    throw 'private path found in package'
}
```

The expected result is no output from the extension filter and no private-path match. An empty scan is evidence about this package only; it does not clear the repository history or the publication provenance gate.

Additionally, both publication-audit legs must pass against the exact exported bytes to ensure export immutability (#293):

```powershell
python tools/publish_audit.py --candidate-root $Source --candidate-tree --public-scope --provenance-ledger $TrustedLedger
python tools/publish_audit.py --candidate-root $Source --candidate-tree --public-scope --provenance-self-consistency
```

## Attribution boundary

`NOTICE.md`, `THIRD_PARTY_LICENSES/`, `assets/release_manifest.json`, `assets/upstream/`, and the vendored ATRAC3+ license record attribution and third-party license data for the `v0.0.1` candidate. The packaged native binaries also require the generated `THIRD_PARTY_NOTICES` bundle and `RELINK.md` described above.
