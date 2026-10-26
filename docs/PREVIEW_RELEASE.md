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
- **Generic raw-ISO to Play coverage is still in the works.** Nakagawa does not decrypt games; it uses decrypted files from your own copy (built-in decryption was removed, [#548](https://github.com/Jstar269/nakagawa-recomp/pull/548), [#550](https://github.com/Jstar269/nakagawa-recomp/pull/550)). An encrypted executable or module without a plain copy fails closed by name, and PGD-protected content remains unavailable within the broader ISO-to-Play work ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). The in-product staging wizard and **BUILD PACKAGE** action are connected ([#487](https://github.com/Jstar269/nakagawa-recomp/pull/487)). On a clean `PATH`, the player offers one consent to download and verify pinned build prerequisites from their declared official hosts ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)); unsupported title profiles and imports remain named boundaries under active work ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).
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
- CPython 3.15.x.
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
mingw32-make --no-print-directory shader-verify
mingw32-make --no-print-directory player
mingw32-make --no-print-directory display-smoke
```

## Recreate the package

The binary staging step passes `-ExcludeOptionalFonts` to `tools/copy_build_assets.ps1`, so the optional repository font directory is not copied into the attested bundle.

The attestation workflow runs `python tools/stage_runtime_dlls.py --target dist/bin` under `set -euo pipefail`; a nonzero exit aborts the job before artifact upload. Packaging copies the verified runtime files byte-for-byte and does not run the stager again.

Complete the exact candidate's local release gates and hosted CI before packaging. The binary bundle must come from the [Trusted provenance attestation workflow](../.github/workflows/provenance-attestation.yml), which runs on `main` only after a maintainer freezes the exact candidate SHA. It checks that the required `candidate_sha` equals the workflow's checked-out commit, builds `nakagawa_player.exe` and `production_smoke.exe`, stages their runtime files and `toolchain.json`, creates SLSA Build L2 attestations for every file under `dist/`, and uploads a workflow artifact. This workflow does not create a tag, release, or release asset.

The public-safe export command filters the source with `assets/public_source_profile.json` and needs the maintainer-controlled publication inputs before the output can be called a release asset. The package retains the source tree under `source/` so the player can resolve its BUILD PACKAGE CLI from the installed layout.

The attested `toolchain.json` records the full source commit, clean-tree status, release-manifest version and SHA-256, and hashes for the compiler, Make, Python, SDL3 link/runtime files, and optional `glslc`. `glslc` is not a standard release-build input because checked-in shader embeddings are used; its absence is acceptable only when checked-in shader verification passes and no shader regeneration is required. This record binds an environment snapshot to the candidate; it does not by itself prove reproducible builds or hosted/private acceptance.

The separate SLSA Build L3 isolated-builder follow-up is linked to [issue #373](https://github.com/Jstar269/nakagawa-recomp/issues/373); it is not part of this Build L2 procedure.

### Download and verify the Build L2 bundle

After the maintainer freezes the approved 40-character `main` SHA and the workflow run succeeds, download that run's `nakagawa-windows-x64-binaries-<SHA>` artifact. Set `$CandidateSha` to the approved full SHA and `$RunId` to the successful workflow run ID:

```powershell
$CandidateSha = '<approved-40-character-main-SHA>'
$RunId = '<successful-workflow-run-id>'
$AttestedBundle = Join-Path $env:TEMP "nakagawa-attested-$CandidateSha"
$ArtifactName = "nakagawa-windows-x64-binaries-$CandidateSha"

if ($CandidateSha -notmatch '^[0-9a-f]{40}$') {
    throw 'Use the maintainer-approved full 40-character candidate SHA.'
}
if (Test-Path -LiteralPath $AttestedBundle) {
    throw "Attested bundle directory already exists: $AttestedBundle"
}
New-Item -ItemType Directory -Path $AttestedBundle | Out-Null
gh run download $RunId --repo Jstar269/nakagawa-recomp --name $ArtifactName --dir $AttestedBundle
if ($LASTEXITCODE -ne 0) { throw 'Could not download the selected workflow artifact.' }

$SignerWorkflow = 'Jstar269/nakagawa-recomp/.github/workflows/provenance-attestation.yml'
$AttestedFiles = Get-ChildItem -LiteralPath $AttestedBundle -Recurse -File
if ($AttestedFiles.Count -eq 0) { throw 'The workflow artifact is empty.' }
foreach ($File in $AttestedFiles) {
    gh attestation verify $File.FullName --repo Jstar269/nakagawa-recomp --signer-workflow $SignerWorkflow --source-ref refs/heads/main --source-digest $CandidateSha
    if ($LASTEXITCODE -ne 0) { throw "Attestation verification failed for $($File.Name)." }
}

$ToolchainIdentity = Join-Path $AttestedBundle 'toolchain.json'
$Toolchain = Get-Content -LiteralPath $ToolchainIdentity -Raw | ConvertFrom-Json
if ($Toolchain.candidate.commit -ne $CandidateSha -or -not $Toolchain.candidate.working_tree_clean) {
    throw 'The attested toolchain record does not identify this clean candidate.'
}
$env:NAKAGAWA_ATTESTED_BUNDLE = $AttestedBundle
$env:NAKAGAWA_CANDIDATE_SHA = $CandidateSha
```

`gh attestation verify <file> -R Jstar269/nakagawa-recomp` is the basic user command. The fuller loop above also checks the signer workflow, `main` source ref, and exact source commit. Run it for the downloaded executable, runtime DLLs, and toolchain record. See the [GitHub CLI verification reference](https://cli.github.com/manual/gh_attestation_verify).

The packaged player binaries and runtime closure are copied byte-for-byte from the verified bundle. The package script below repeats the attestation checks and confirms the bundle's `toolchain.json` matches the frozen source SHA; it does not rebuild or restage those binaries locally. The resulting ZIP is a packaging container, not a subject of this Build L2 workflow: after extracting it, verify each executable and runtime DLL individually with gh attestation verify.

At candidate freeze, the maintainer-selected trust anchor is the detailed `IMPLEMENTATION_PROVENANCE.json` from the external authority repository, never a candidate-committed public ledger. Verify the frozen main commit's admissions, commit and push truthful authority changes, then copy the ledger from that exact authority commit to an external immutable file. Record the full candidate SHA, authority commit SHA, and copied ledger SHA-256 together in #278. Set `NAKAGAWA_TRUSTED_PUBLIC_LEDGER` to that detailed copy and do not edit or regenerate it. Different source bytes require a fresh freeze and binding. The export audit consumes a public `entries` projection; the commands below independently generate and verify that projection from the selected detailed authority instead of trusting the candidate's copy.

```powershell
$Version = '0.0.1'
$CandidateSha = $env:NAKAGAWA_CANDIDATE_SHA
$FrozenCandidateSha = (git rev-parse HEAD).Trim()
if ($CandidateSha -notmatch '^[0-9a-f]{40}$' -or $FrozenCandidateSha -ne $CandidateSha) {
    throw 'The clean source checkout must be the same full SHA as the attested bundle.'
}
$AttestedBundle = $env:NAKAGAWA_ATTESTED_BUNDLE
if (-not $AttestedBundle -or -not (Test-Path -LiteralPath $AttestedBundle -PathType Container)) {
    throw 'Set NAKAGAWA_ATTESTED_BUNDLE to the verified Build L2 artifact directory.'
}
$AttestedBin = Join-Path $AttestedBundle 'bin'
$ToolchainIdentity = Join-Path $AttestedBundle 'toolchain.json'
if (-not (Test-Path -LiteralPath $AttestedBin -PathType Container) -or
    -not (Test-Path -LiteralPath $ToolchainIdentity -PathType Leaf)) {
    throw 'The verified artifact must contain bin/ and toolchain.json.'
}
$Toolchain = Get-Content -LiteralPath $ToolchainIdentity -Raw | ConvertFrom-Json
if ($Toolchain.candidate.commit -ne $CandidateSha -or -not $Toolchain.candidate.working_tree_clean) {
    throw 'The toolchain record does not identify this clean candidate.'
}
$SignerWorkflow = 'Jstar269/nakagawa-recomp/.github/workflows/provenance-attestation.yml'
$AttestedFiles = Get-ChildItem -LiteralPath $AttestedBundle -Recurse -File
if ($AttestedFiles.Count -eq 0) { throw 'The verified bundle is empty.' }
foreach ($File in $AttestedFiles) {
    gh attestation verify $File.FullName --repo Jstar269/nakagawa-recomp --signer-workflow $SignerWorkflow --source-ref refs/heads/main --source-digest $CandidateSha
    if ($LASTEXITCODE -ne 0) { throw "Attestation verification failed for $($File.Name)." }
}
$CandidateShort = $CandidateSha.Substring(0, 12)
$ArtifactRoot = Join-Path $env:TEMP "nakagawa-recomp-$Version-$CandidateShort"
$DetailedLedger = $env:NAKAGAWA_TRUSTED_PUBLIC_LEDGER
$Stage = Join-Path $ArtifactRoot "nakagawa-recomp-$Version-windows-x64"
$Bin = Join-Path $Stage 'bin'
$Source = Join-Path $Stage 'source'
$PackageDocs = Join-Path $Stage 'docs'
$Zip = Join-Path $ArtifactRoot "nakagawa-recomp-$Version-windows-x64.zip"

if (Test-Path -LiteralPath $ArtifactRoot) {
    throw "Candidate output already exists; choose a new artifact directory: $ArtifactRoot"
}
if (-not $DetailedLedger -or -not (Test-Path -LiteralPath $DetailedLedger -PathType Leaf)) {
    throw 'Set NAKAGAWA_TRUSTED_PUBLIC_LEDGER to the frozen external detailed authority ledger copy.'
}
$TrustScratch = Join-Path ([System.IO.Path]::GetTempPath()) ('nk-release-trust-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $TrustScratch | Out-Null
$TrustBaseline = Join-Path $TrustScratch 'authority-baseline.json'
$TrustControls = Join-Path $TrustScratch 'verified-controls'
python tools/provenance_attest_verify.py --repo . --base $FrozenCandidateSha `
    --trusted-ledger $DetailedLedger --emit-authority-baseline $TrustBaseline
if ($LASTEXITCODE -ne 0) { throw 'Frozen authority baseline generation failed.' }
python tools/provenance_attest_verify.py --repo . --base $FrozenCandidateSha `
    --candidate $FrozenCandidateSha --require-immutable-revisions --ephemeral `
    --trusted-ledger $DetailedLedger --trusted-baseline $TrustBaseline --output-dir $TrustControls
if ($LASTEXITCODE -ne 0) { throw 'Frozen candidate trusted attestation failed.' }
$TrustedLedger = Join-Path $TrustControls 'public_provenance_ledger.json'
New-Item -ItemType Directory -Path $ArtifactRoot, $Source -Force | Out-Null
python tools/build_public_export.py --public-safe-profile --export-dir $Source --trusted-ledger $TrustedLedger

New-Item -ItemType Directory -Path $Bin, $PackageDocs -Force | Out-Null
Get-ChildItem -LiteralPath $AttestedBin -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $Bin -Recurse -Force
}
foreach ($Required in @('nakagawa_player.exe', 'production_smoke.exe', 'SDL3.dll', 'SDL3_ttf.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $Bin $Required) -PathType Leaf)) {
        throw "The verified binary bundle is missing $Required."
    }
}
# Use the exact toolchain record shipped and attested with the binary bundle.
Copy-Item -LiteralPath $ToolchainIdentity -Destination (Join-Path $Stage 'toolchain.json')
Copy-Item LICENSE, NOTICE.md (Join-Path $Stage '.')
Copy-Item README.md (Join-Path $Stage '.')
Copy-Item docs\PREVIEW_RELEASE.md, docs\SETUP.md, docs\SMOKE_TEST.md, docs\RELEASE_NOTES_v0.0.1.md $PackageDocs
Copy-Item (Join-Path $Bin 'THIRD_PARTY_NOTICES') $Stage -Recurse
Copy-Item (Join-Path $Bin 'THIRD_PARTY_NOTICES.txt'), (Join-Path $Bin 'SOURCE.txt'), (Join-Path $Bin 'RELINK.md') $Stage

python tools/generate_sbom.py --package-dir $Bin `
    --spdx-out (Join-Path $Stage 'SBOM.spdx.json') `
    --spdx3-out (Join-Path $Stage 'SBOM.spdx3.jsonld') `
    --cyclonedx-out (Join-Path $Stage 'SBOM.cyclonedx.json')
python tools/verify_sbom.py --expected-release-version $Version `
    --observed-toolchain $ToolchainIdentity `
    --spdx (Join-Path $Stage 'SBOM.spdx.json') `
    --spdx3 (Join-Path $Stage 'SBOM.spdx3.jsonld') `
    --cyclonedx (Join-Path $Stage 'SBOM.cyclonedx.json')

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
# The ZIP itself is a container, not an attested subject: publish its SHA-256 next to it so a
# download can be checked before it is extracted.
$ZipHash = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$ZipHash  $(Split-Path -Leaf $Zip)" | Set-Content -LiteralPath "$Zip.sha256" -Encoding utf8NoBOM
Write-Output $Zip, "$Zip.sha256"
```

The package contains the native player and synthetic verification binary under `bin/`, the mechanically resolved SDL3/SDL3_ttf runtime DLL closures and their notices, the public-safe source export under `source/`, setup and smoke instructions, root README/license/notices, `SOURCE.txt`, LGPL relinking material, the third-party notice bundle, three SBOM formats, and `SHA256SUMS.txt`. It does not copy the whole `build/` tree, generated translation units, fixture PRXs, Vulkan SDK files, local databases, or title outputs. `SHA256SUMS.txt` covers every staged file except itself. Attach `nakagawa-recomp-<version>-windows-x64.zip.sha256` to the release next to the ZIP: v0.0.1 binaries are not Authenticode-signed, so the published checksum and the per-file build attestations are how a user checks a download.

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
