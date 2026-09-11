# Early preview release scope

Status: preparation proposal only. This document does not create a tag, GitHub
Release, or release asset.

## Proposed version

`0.1.0-preview.1` is the proposed version. The `0.x` series and the explicit
`preview` prerelease label say that this is an experimental, source-led preview,
not a production compatibility release. The version is a proposal for the
maintainer; it has not been tagged.

## What this preview contains

The preview is a Windows public-source and reproducibility package built from
the sanitized source tree. Its centerpiece is the synthetic production smoke
workload:

- `mingw32-make production-smoke` creates a checked-in synthetic PSP PRX
  fixture, statically translates MIPS to C, compiles native code, and verifies
  and runs the AOT result.
- `mingw32-make production-smoke-gap` runs the same kind of synthetic workload
  with one function deliberately omitted from AOT emission, exercising the
  production dispatch seam.
- `mingw32-make cosim-selftest` compares the source-owned AOT and interpreter
  lanes over the synthetic corpus, including its fail-closed negative cases.
- `mingw32-make platform-ladder` builds and runs the source-owned synthetic
  workload ladder, including relocation, scheduler, filesystem, FPU, title-2,
  and negative cases.
- `mingw32-make selftest sched-selftest hle-thread-selftest
  public-safe-verify` runs the native interpreter, scheduler, HLE/thread, and
  public-safe object gates.

The package may include the resulting `production_smoke.exe` as a synthetic
verification binary. It is not a game executable and is not a player.

The `interface/` tree is included as dashboard source with its checked-in npm
lockfile. It builds as an independent Next.js project and exposes a
browser-local ISO9660/PARAM.SFO inspector. The inspector reads a user-selected
image in the browser tab; it does not upload the image, prepare a game, or
connect a retail execution route.

The package also includes the public documentation selected by the packaging
command, `LICENSE`, `NOTICE.md`, and the reviewed third-party notice files. It
does not include `interface/node_modules`, a `.next` tree, or fetched npm
packages.

## What this preview does not do

This is the limitations section, not a footnote:

- **No preparation pipeline is connected in this build.** The preview cannot
  prepare or run a retail PSP game and provides no gameplay evidence.
- The exact base `a699000` has no `src/player` tree and no `player` Make target;
  `mingw32-make player` therefore fails with “No rule to make target 'player'.”
  No player binary is part of this candidate. Player work on other lanes is not
  imported by this release-preparation branch.
- The synthetic executables prove the named synthetic contracts only. They do
  not prove retail-title compatibility, complete Allegrex interpreter coverage,
  software/Vulkan agreement with a PSP, or physical PSP correctness.
- The dashboard is a local development tool. Its ISO/PARAM.SFO inspector is
  not a game-preparation pipeline and must not be presented as one.
- This package makes no Linux or macOS support claim. A Windows binary is
  evidence for the verified Windows host only.
- No retail executable, ISO, asset, decrypted module, generated retail C,
  save, key, private trace/capture, private route, private path, or hardware
  evidence is shipped.
- Local passes are not hosted-CI, DCO, legal, hardware, or maintainer-release
  evidence. The final publication still requires the maintainer-controlled
  provenance refresh and the hosted gates described in
  [`docs/PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md).

## Supported host contract

The verified native host contract is the Windows contract in
[`docs/SETUP.md`](SETUP.md):

- Windows 11 x64.
- PowerShell 7.6 or newer; Windows PowerShell 5.1 is not supported.
- CPython 3.14.x.
- MSYS2 UCRT64 GCC/G++, GNU Make, SDL3, and Vulkan loader packages.

> **Reproducibility caveat, verified by a clean-clone run.** On at least one
> Windows host the build succeeded *without* the documented MSYS2 SDL3
> package: the Vulkan SDK ships an SDL3 subset, and because the Makefile adds
> `-I$(VULKAN_SDK)/Include`, `-lSDL3` was satisfied by
> `C:/VulkanSDK/<version>/Lib/SDL3.lib` instead. This was proven from the
> linker map, not inferred. The build therefore appears to work on machines
> that never installed the documented dependency, and would break on a machine
> with MSYS2 SDL3 but no Vulkan SDK. Do not treat a successful build as proof
> that the documented toolchain contract was satisfied.

- A current Vulkan SDK and Vulkan-capable GPU.

The optional dashboard additionally requires Node.js 24.18.1 or newer and npm
11.17.0 or newer. Keep it bound to `127.0.0.1`; it is not an untrusted-network
service. These requirements describe the verified host contract, not a claim
that every listed component is bundled in the preview archive.

## Reproduce from a clean checkout

The following is the source-owned Windows sequence. It uses no retail input:

```powershell
git clone https://github.com/Jstar269/nakagawa-recomp.git nakagawa-recomp-preview
Set-Location .\nakagawa-recomp-preview
git fetch origin
git switch --detach a699000
python -m pip install .

mingw32-make --no-print-directory production-smoke
mingw32-make --no-print-directory production-smoke-gap
mingw32-make --no-print-directory cosim-selftest
mingw32-make --no-print-directory platform-ladder
mingw32-make --no-print-directory selftest
mingw32-make --no-print-directory sched-selftest
mingw32-make --no-print-directory hle-thread-selftest
mingw32-make --no-print-directory public-safe-verify
```

The dashboard checks are separate from the native build:

```powershell
npm ci --prefix interface
npm --prefix interface run test
npm --prefix interface run lint
npm --prefix interface run typecheck
npm --prefix interface run build
```

For a local dashboard smoke, use an ephemeral port rather than a shared fixed
debug port:

```powershell
npm --prefix interface run dev -- --hostname 127.0.0.1 -p 0
```

## Recreate the package

Run the following after the native smoke build, from a clean candidate
checkout. The public-safe export command filters the source with
`assets/public_source_profile.json`; it must pass the maintainer-controlled
publication inputs before the result can be called a release asset.

```powershell
$Version = '0.1.0-preview.1'
$ArtifactRoot = Join-Path $env:TEMP "nakagawa-recomp-$Version"
$Stage = Join-Path $ArtifactRoot "nakagawa-recomp-$Version-windows-x64"
$Source = Join-Path $Stage 'source'
$Zip = Join-Path $ArtifactRoot "nakagawa-recomp-$Version-windows-x64.zip"

if (Test-Path -LiteralPath $ArtifactRoot) {
    Remove-Item -LiteralPath $ArtifactRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $Source -Force | Out-Null
python tools/build_public_export.py --public-safe-profile --export-dir $Source

New-Item -ItemType Directory -Path (Join-Path $Stage 'bin') -Force | Out-Null
Copy-Item build\production-smoke\production_smoke.exe (Join-Path $Stage 'bin')
Copy-Item LICENSE, NOTICE.md (Join-Path $Stage '.')
Copy-Item docs\PREVIEW_RELEASE.md, docs\PREVIEW_RELEASE_NOTES.md (Join-Path $Stage 'docs')

Get-ChildItem -LiteralPath $Stage -Recurse -File |
    Sort-Object FullName |
    Get-FileHash -Algorithm SHA256 |
    ForEach-Object {
        $relative = $_.Path.Substring($Stage.Length + 1).Replace('\', '/')
        "$($_.Hash.ToLowerInvariant())  $relative"
    } |
    Set-Content -LiteralPath (Join-Path $Stage 'SHA256SUMS.txt') -Encoding utf8NoBOM

Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $Zip -Force
Write-Output $Zip
```

The package deliberately copies one synthetic executable only. It does not
copy the whole `build/` tree, generated translation units, fixture PRXs, SDL or
Vulkan DLLs, local databases, or dashboard installation output.

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

The expected result is no output from the extension filter and no private-path
match. An empty scan is evidence about this package only; it does not clear
the repository history or the publication provenance gate.

## Attribution boundary

`NOTICE.md`, `THIRD_PARTY_LICENSES/`, `assets/release_manifest.json`,
`assets/upstream/`, the vendored ATRAC3+ license, and
`interface/package-lock.json` are the attribution and dependency inventory for
this preview. The dashboard's fetched dependency graph is not bundled: the
package contains its source and lockfile, not `node_modules` or a standalone
Next.js runtime. The lockfile carries the exact versions, integrity values, and
license metadata for the dashboard graph; `THIRD_PARTY_LICENSES/SHADCN_UI.txt`
covers the copied shadcn/ui primitives. A future package that bundles fetched
npm code must add the corresponding package notices before publication.
