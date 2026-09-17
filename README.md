# Nakagawa Recomp

Nakagawa Recomp is an experimental static recompiler that translates user-supplied decrypted PlayStation Portable (PSP) executables into C, links them with a native C runtime, and runs the resulting binary on Windows via SDL3 and Vulkan. Named after the in-game Nakagawa Tennis Club from its flagship test title, *Hot Shots Tennis: Get a Grip*, the project investigates ahead-of-time (AOT) binary translation, low-level hardware fidelity, and high-performance native execution for PSP software.

## Current status

This repository is an experimental research and compatibility project, **not an end-user release** or a game distribution. It does not include game binaries, copyrighted assets, firmware modules, decryption keys, or private oracle traces.

Public development and automated continuous integration verify the recompiler through source-owned synthetic guests:
- The **differential cosimulation harness** (`mingw32-make cosim-selftest`) verifies semantic parity between AOT-generated code and the fail-closed interpreter floor.
- The **platform ladder** (`mingw32-make platform-ladder`) exercises relocations, scheduler threading, scalar FPU, and filesystem semantics across synthetic workloads.
- The **production smoke fixtures** (`mingw32-make production-smoke`, `mingw32-make display-smoke`) test the complete two-phase build pipeline, display bring-up, and native player launch without proprietary inputs.

The public source boundary deliberately makes no claim of retail title playability. Active development focuses on HLE completeness, timing, scheduler edge cases, and graphics/audio fidelity. Active defect tracking is maintained on [GitHub Issues](https://github.com/Jstar269/nakagawa-recomp/issues); see [`ISSUES.md`](ISSUES.md) for the project status dashboard.

## Quick start

### Prerequisites
- Windows 11 x64 with PowerShell 7.6+ (`pwsh`)
- CPython 3.14.x
- MSYS2 UCRT64 toolchain (GCC/G++, GNU Make, SDL3, Vulkan headers & loader)
- A current Vulkan SDK and a Vulkan-capable GPU

Install the MSYS2 packages from a UCRT64 terminal:
```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader
```

### Build public synthetic smoke routes
Verify the toolchain and pipeline without proprietary game inputs:
```powershell
.\nk_manager.ps1 -Action Test                 # Python tooling suite and synthetic doctor
mingw32-make production-smoke                 # Two-phase pipeline smoke test
mingw32-make platform-ladder                  # Multi-workload synthetic platform ladder
mingw32-make cosim-selftest                   # Differential AOT vs. interpreter cosimulation
```

### Local game inputs
To build with a lawfully obtained copy of a game, place private inputs into the Git-ignored `place_game_here/` layout:
```text
place_game_here/
├── EBOOT.elf                     # Decrypted game executable
├── ISO/<game>.iso                # Lawfully obtained game ISO
└── EXTRACTED/
    ├── decrypted/                # Decrypted PRXs (libfont.prx, scePsmf_library.prx, scePsmfP_library.prx)
    └── PSP_GAME/
        ├── SYSDIR/EBOOT.BIN      # PSP header/BSS metadata
        └── USRDIR/xbdata_extracted/ # Extracted game data (via python tools/extract_xb.py)
```

Building a retail title requires a local title manifest (for HST: `assets/titles/hst-ucus98701.json`, which is intentionally never checked in and publication-excluded):
```powershell
.\nk_manager.ps1 -Action BuildFull -TitleManifest assets/titles/hst-ucus98701.json -GameName hst
.\nk_manager.ps1 -Action Run -TitleManifest assets/titles/hst-ucus98701.json -GameName hst
```

See [`docs/SETUP.md`](docs/SETUP.md) for authoritative toolchain details, input layout requirements, and troubleshooting.

## How it works

The build system operates in two phases:
1. **Offline translation:** Host-side Python tools analyze the decrypted ELF/PRX (`tools/prxload.py`, `tools/analyze.py`), resolve import NIDs to native HLE functions (`tools/imports.py`), and translate MIPS machine code into chunks of portable C (`tools/codegen.py`).
2. **Native compilation:** GNU Make and GCC compile the generated C translation units alongside the native runtime (`src/rt/`), which provides guest memory management, cooperative thread scheduling, HLE syscall emulation, an AOT-gap interpreter fallback floor, audio decoding, and an SDL3 + Vulkan hardware renderer (`src/rt/gpu_sdl3vk/`).

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for subsystem structure, execution models, and data-flow diagrams.

## Project direction

Nakagawa Recomp's long-term goal is to evolve into a definitive, multi-title PSP recompilation and preservation platform built from original code wherever possible.

While initial development began by adapting upstream open-source toolkits, the architectural direction systematically replaces convenience shortcuts and high-level approximations with maximal sensible low-level emulation (LLE), precise hardware-measured semantics, and original implementations. Core initiatives include:
- Driving title-specific HLE overrides toward zero in generic runtime code
- Prioritizing guest execution fidelity over host reimplementations
- Expanding data-driven multi-title manifest planning
- Delivering a standalone native player interface

See [`docs/LLE_FIDELITY_ARCHITECTURE.md`](docs/LLE_FIDELITY_ARCHITECTURE.md) and [`docs/PROJECT_MODEL.md`](docs/PROJECT_MODEL.md) for the project's engineering principles.

## Legal and lineage summary

- **Project license declaration:** The repository-level project declaration is **GPL-3.0-or-later** ([LICENSE](LICENSE)). Many source files and inherited components retain GPL-2.0-or-later or upstream-specific terms; this declaration does not establish that every combined distribution configuration is legally cleared.
- **Lineage and upstreams:** The project began as a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project) (GPL-2.0-or-later) and retains substantial modified code from that lineage. Portions of the HLE, GE, and VFPU subsystems adapt or derive from [PPSSPP](https://github.com/hrydgard/ppsspp) (GPL-2.0-or-later). See [CREDITS.md](CREDITS.md), [NOTICE.md](NOTICE.md), and [assets/public_provenance_ledger.json](assets/public_provenance_ledger.json).
- **"Independent" disclaimer:** Nakagawa Recomp is an independent research and compatibility project. "Independent" describes its relationship to Sony Interactive Entertainment, Clap Hanz, and game rights-holders; it does not mean the recompiler codebase is clean-room or independently originated. Nakagawa Recomp is not affiliated with, authorized by, or endorsed by Sony Interactive Entertainment, Clap Hanz, PPSSPP, sal063, or other upstream authors.
- **No proprietary content:** This repository does not distribute game executables, assets, firmware modules, decryption keys, or private oracle traces. Users must supply their own lawfully obtained game files.
- **Publication controls:** `Jstar269/nakagawa-recomp` is the active sanitized public source repository. Its history begins with the sanitized restoration lineage; former development history is not ordinary `main` ancestry and must not be reconnected. Publication gates, source profiles ([`assets/public_source_profile.json`](assets/public_source_profile.json)), and provenance audits are engineering controls, not legal clearance.

## Contributing

Contributions are welcome. Before submitting changes, review:
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — contribution workflow, coding standards, and verification expectations
- [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) — community standards and pledge
- [`SECURITY.md`](SECURITY.md) — vulnerability reporting and security scope
- [`docs/DCO_POLICY.md`](docs/DCO_POLICY.md) — Developer Certificate of Origin (DCO 1.1) sign-off policy (`git commit -s`)
- [`docs/AI_USAGE.md`](docs/AI_USAGE.md) — boundaries and disclosures for AI-assisted development
- [`CREDITS.md`](CREDITS.md) — upstream attribution and project lineage

## Dedication

Nakagawa Recomp is part of **Project Blitzen**, a broader long-term effort dedicated to Blitzen, my German Shepherd and companion for 13½ years. The best dog you could ever have.

Blitzen's loyalty, strength, and constant presence are remembered through the patience, care, and persistence behind this work.

The name **Nakagawa Recomp** remains the technical identity of this repository. **Project Blitzen** is intended as an umbrella name for this project and possible future PSP recompilation, decompilation, compatibility, and preservation-research work.

This dedication is personal. It does not imply affiliation with any other project, organization, product, or prior use of the name “Project Blitzen.”
