# Nakagawa Recomp `v0.0.1` — proposed release notes

Status: proposed release-note copy. Governed by release qualification authority [issue #278](https://github.com/Jstar269/nakagawa-recomp/issues/278). The maintainer must cut any tag or release manually; this branch does not do that (AGENTS.md § 3).

> [!NOTE]
> An earlier draft proposal used version `0.1.0-preview.1` based on older candidate snapshots. That proposal has been superseded by the `v0.0.1` release plan ([#278](https://github.com/Jstar269/nakagawa-recomp/issues/278)).

## What genuinely works

The public, game-input-free verification suite and native player are the centerpiece:

- **Native player application:** `mingw32-make player` compiles `build/nakagawa_player.exe` using SDL3. It supports drag-and-drop and file-dialog ISO loading, parses ISO9660 PVD and `PARAM.SFO` metadata (`DISC_ID`, `TITLE`) in C, performs asset census staging, and handles gamepad navigation.
- **Display smoke verification:** `mingw32-make display-smoke` and `mingw32-make display-smoke-player` prove the two-phase pipeline, loader, NID imports, vblank delivery, display latch, and native player child runtime launch without proprietary inputs.
- **Production smoke:** `mingw32-make production-smoke` generates a synthetic PSP PRX, statically recompiles it MIPS-to-C-to-native, and verifies and runs the AOT output.
- **Fail-closed dispatch:** `mingw32-make production-smoke-gap` proves the fail-closed dispatch path when a synthetic function is intentionally omitted from AOT emission.
- **Differential cosimulation:** `mingw32-make cosim-selftest` compares AOT and interpreter traces, writes, memory, and architectural state over synthetic cases.
- **Platform ladder:** `mingw32-make platform-ladder` exercises relocations, scheduler threading, scalar FPU, and filesystem semantics across synthetic workloads.
- **Core runtime selftests:** `selftest`, `sched-selftest`, `hle-thread-selftest`, and `public-safe-verify` pass on the verified Windows host.
- **Media subsystem:** PSMF demuxing and H.264 video decoding pipelines process video cutscenes.
- **Developer studio:** The Next.js dashboard (`interface/`) builds, lints, typechecks, and passes source tests as developer diagnostic tooling.

## What this release does not do

This is the limitations section, not a footnote:

- **Not an end-user "ISO -> click Play" product release:** This is an experimental platform release. An end-user product workflow is tracked under [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308).
- **No automated retail decryption or compilation:** Commercial games require manual pre-decryption and staging; in-engine KIRK decryption ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)) and end-user recompilation routes (#296) are in active development.
- **Silent by design:** The public-safe build intentionally lacks a host audio output backend and produces no sound (#301).
- **Windows 11 x64 only:** Verified on Windows 11 x64 only; no Linux ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306)), macOS ([#329](https://github.com/Jstar269/nakagawa-recomp/issues/329)), or Android ([#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)) support is claimed.
- **No proprietary content:** Retail executables, ISOs, assets, decrypted modules, generated retail C, saves, keys, private traces/captures, and firmware PGF fonts are not included.
- **Local verification is not hosted-CI or release authority:** Final release tagging requires hosted CI green, provenance refresh, and maintainer authorization.

### Historical candidate notes

- The dated preview candidate at base `a699000` had no `src/player/` tree and no `player` Make target.
- The older base `b3f87be` packaging exercise predated PR #277 (PSMF MPEG-2 flag corrections and media work) and #293 (public export immutability).

## Reproduce the centerpiece

From a clean Windows checkout with the supported toolchain:

```powershell
mingw32-make --no-print-directory production-smoke
mingw32-make --no-print-directory production-smoke-gap
mingw32-make --no-print-directory player
mingw32-make --no-print-directory display-smoke
```

The commands generate their synthetic inputs beneath the ignored `build/`
tree. They do not require a retail ISO, PRX, save, key, or decrypted module.

## Release package contents

The prepared package shape is:

```text
nakagawa-recomp-0.0.1-windows-x64/
├── bin/
│   ├── nakagawa_player.exe        native player desktop application
│   └── production_smoke.exe       synthetic verification binary
├── source/                        public-safe source export
├── docs/
│   ├── PREVIEW_RELEASE.md         scope and packaging specification
│   └── PREVIEW_RELEASE_NOTES.md   this note
├── LICENSE
├── NOTICE.md
└── SHA256SUMS.txt
```

The exact package command and its fail-closed scan are in [`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md).
