# Nakagawa Recomp

> **Independent research/compatibility project.** The repository-level project declaration is GPL-3.0-or-later, while many source files retain GPL-2.0-or-later or upstream-specific terms. Inherited PGF/font and PGD/amctrl questions remain under explicit review. See [NOTICE.md](NOTICE.md) and [docs/PUBLICATION_READINESS.md](docs/PUBLICATION_READINESS.md); do not treat the project declaration as final clearance for every combined configuration.

Nakagawa Recomp is an experimental static recompiler for the PSP release of *Hot Shots Tennis: Get a Grip*. Its name comes from the in-game Nakagawa Tennis Club. It translates a user-supplied decrypted PRX/ELF into C, links it with a native C runtime, and runs the result on Windows through SDL3 and Vulkan.

**Project lineage:** Nakagawa Recomp began as a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project), a GPL-2.0-or-later PSP static-recompiler toolkit, and still contains substantial code inherited from that project. Nakagawa has since substantially extended and modified that codebase. See [NOTICE.md](NOTICE.md) and the [sal063 retention/provenance audit](docs/provenance/SAL063_RETENTION_2026-08-06.md) for detailed attribution, retained-code measurements, and downstream provenance.

The project is not a game download or a general-purpose PSP emulator. It does not include the game, firmware modules, private keys, or private oracle traces. Development requires files from the user's own lawfully obtained copy.

This is an unofficial compatibility/research project. **"Independent" describes its relationship to Sony Interactive Entertainment, Clap Hanz, and the game rights-holders; it does not mean the recompiler codebase is clean-room or independently originated.** Nakagawa is not affiliated with or endorsed by Sony Interactive Entertainment, Clap Hanz, PPSSPP, sal063, or other upstream toolkit authors; names and marks are used only to identify compatibility and source lineage.

## Project status

This repository is under active bring-up and is **not an end-user release**. Accepted development evidence includes reaching the title, main menu, 3D lobby, active tennis gameplay, and returning from a match to the club; see [`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md) for dated acceptance evidence and public GitHub Issues for active tracking.

The recompiler is experimental, and active development focuses on fidelity, timing, HLE completeness, and graphics/audio rendering. Current known open areas include:

- in-match HUD portrait resource path resolution ([`docs/issue-139-face-resource-semantics.md`](docs/issue-139-face-resource-semantics.md));
- sceSasCore and ATRAC audio edge cases ([sceSasCore PR #20](https://github.com/Jstar269/nakagawa-recomp/pull/20), [`src/rt/atrac3p/PROVENANCE.md`](src/rt/atrac3p/PROVENANCE.md));
- full PSMF intro-movie playback integration ([`docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md`](docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md)); and
- formal performance benchmark baselines ([`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md)).

**Public GitHub Issues are canonical for active defects and acceptance criteria where a curated issue exists.** [`ISSUES.md`](ISSUES.md) provides the concise status map across public issues and reference evidence; [`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md) preserves dated milestones and historical evidence. Hosted GitHub Actions workflows define automated verification gates on public commits.

## Requirements

- Windows 11 x64
- PowerShell 7.6+ (`pwsh`)
- MSYS2 UCRT64 packages: GCC/G++, GNU Make, SDL3, and the Vulkan loader
- Python 3.14.x
- A current Vulkan SDK (the manager prefers `-VulkanSdk`, then `VULKAN_SDK`, then the newest valid `C:\VulkanSDK\<version>` installation)
- A Vulkan-capable GPU for the default renderer

The authoritative development baseline, discovery rules, and doctor checks are maintained in
[docs/SETUP.md](docs/SETUP.md). Use `pwsh` for PowerShell entrypoints; Windows PowerShell 5.1 is
not a supported host.

Install the MSYS2 packages from a UCRT64 terminal:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader
```

For exact input layout, dependency setup, and troubleshooting, read [docs/SETUP.md](docs/SETUP.md).

## Required local game files

These paths are intentionally ignored by Git:

```text
place_game_here/                 # canonical private runtime/build input
├── EBOOT.elf                    # decrypted flat build input
├── ISO/<your lawfully obtained game>.iso
└── EXTRACTED/
    ├── decrypted/
    │   ├── libfont.prx
    │   ├── scePsmf_library.prx
    │   └── scePsmfP_library.prx
    └── PSP_GAME/
        ├── SYSDIR/EBOOT.BIN     # PSP header/BSS metadata
        └── USRDIR/xbdata_extracted/
```

The manager resolves this layout directly. Legacy root links named `eboot.elf` and `game.iso` still work but are optional. A source `EBOOT.PBP` and `DOCUMENT.DAT` may be retained as private archival inputs, but neither is read by the current manager/build/runtime once the layout above exists.

`python tools/extract_xb.py` can regenerate `xbdata_extracted/`; it requires a local checkout of [libxb](https://github.com/kiwi515/libxb) under `third_party/libxb/` at the audited commit `ce6df78e5ca99241dd2bbbd68ca485e34003d760` (the 0.2.0 source snapshot). That optional dependency remains local-only; direct-archive containment and runtime semantics are investigated in [`docs/ISSUE196_DIRECT_XB.md`](docs/ISSUE196_DIRECT_XB.md), [`assets/release_manifest.json`](assets/release_manifest.json), and PR [#15](https://github.com/Jstar269/nakagawa-recomp/pull/15).

The private `place_game_here/` layout is Git-ignored. A complete ISO-only bootstrap is not automated yet: the runtime still needs the three decrypted PRXs and the plain extracted XB tree. Do not publish files from this folder.

## Build and run

Use the HST manager from the repository root. It supplies HST's required `GAME_BASE=0 GAME_ENTRY=0` values and canonical private-input paths.

```powershell
.\hst_manager.ps1 -Action BuildFull  # pipeline + compile
.\hst_manager.ps1 -Action BuildFast  # incremental/runtime-focused developer build
.\hst_manager.ps1 -Action Test       # configured project test route
.\hst_manager.ps1 -Action Run        # launch with the GUI
```

> [!CAUTION]
> Compiler/codegen profile invalidation, shader-source/embed freshness, and transitive-header dependency tracking are enforced by content-addressed manifests, deterministic shader verification, and `-MMD -MP` metadata (see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)). Performance/profile experiments should still explicitly rebuild all affected runtime objects, and high-confidence verification after broad build-system changes should use a true clean/known-complete rebuild.

Build duration depends heavily on host CPU, storage, compiler version, and whether generated chunks already exist; historical timings are not a build contract.

Equivalent direct Make invocation for the canonical HST ELF path:

```bash
mingw32-make GAME_NAME=hst GAME_ELF=place_game_here/EBOOT.elf GAME_BASE=0 GAME_ENTRY=0 all
```

Direct Make invocations must export `VULKAN_SDK` (or pass it on the command line); the manager is
the canonical path that discovers and validates the current SDK automatically.

The Makefile selects PATH-resolved MSYS2 UCRT64 `gcc` when `CC` is otherwise only GNU Make's built-in `cc` default. Environment and command-line overrides remain supported:

```bash
mingw32-make --no-print-directory compiler-info
mingw32-make CC=clang --no-print-directory compiler-info
```

Do not combine the Makefile's two-phase `all` target into a single dependency line. Generated chunk discovery happens in the second Make process.

## Repository map

| Path | Purpose |
| --- | --- |
| `tools/` | Offline ELF analysis, code generation, extraction, verification, and publication-audit tools |
| `src/rt/` | Native C runtime, HLE, scheduler, audio/video, filesystem, and renderers |
| `src/ref/` | C++ reference interpreter used by selftests and differential gates |
| `assets/vfpu/` | Pinned PPSSPP-derived VFPU lookup tables with upstream provenance |
| `font/` | Replacement-font provenance/review material; `public-safe-v1` excludes the unresolved PGF/font payloads |
| `interface/` | Separate local-only Next.js dashboard/prototype; not part of `hst.exe` |
| `docs/` | Architecture, setup, debugging, porting, governance, verification, and legal/provenance engineering records |
| `build/` | Fully generated local output; ignored by Git |

Generated `build/<game>/<game>_recomp_*.c` files must never be edited. Change generator/runtime source and regenerate them. The number of generated translation units is controlled by `FUNCS_PER_CHUNK`; it is dynamic and not a fixed HST chunk count.

## Verification

```powershell
.\hst_manager.ps1 -Action Test
python -m unittest discover -s tools -p "test_*.py" -v
python tools/publish_audit.py --tracked-only --worktree --public-scope
```

`--worktree` audits the bytes on disk. Without it the audit reads staged Git blobs, which is
what the pre-commit hook wants but means an unstaged edit goes unexamined. `--public-scope` applies
the established `public-safe-v1` exclusions used by this public repository.

The checked-in GitHub Actions workflow defines path-gated public/synthetic Python, lint, native-object, reference-interpreter, translation, renderer-comparison, and dashboard gates without proprietary game inputs. [`docs/CI.md`](docs/CI.md) documents the applicability matrix and the stable `CI required` aggregate status.

The full `make verify` path additionally requires external oracle traces and a microtest module and intentionally reports blocked when they are absent. See [`docs/STATIC_VERIFY.md`](docs/STATIC_VERIFY.md).

## Documentation and work tracking

Start at [`docs/README.md`](docs/README.md).

- **GitHub Wiki:** [Nakagawa Recomp project manual](https://github.com/Jstar269/nakagawa-recomp/wiki).
- **Broader PSP research/reference:** [recomp.jaycast.net](https://recomp.jaycast.net/) covers generalized PSP recompilation and hardware research.
- **GitHub Issues:** canonical actionable work items and acceptance criteria where curated public issues exist.
- [`docs/NEXT_SESSION.md`](docs/NEXT_SESSION.md): current machine-capable handoff and evidence discipline.
- [`ISSUES.md`](ISSUES.md): concise current-status dashboard.
- [`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md): dated resolved evidence and superseded hypotheses.
- [`AGENTS.md`](AGENTS.md): repository working rules for human and AI-assisted development.

## Legal and provenance

The repository-level project declaration is **GPL-3.0-or-later**, as reflected by [LICENSE](LICENSE), `assets/release_manifest.json`, and the dashboard package metadata. Many source files and inherited components retain GPL-2.0-or-later or other upstream-specific terms; that does **not** establish that every possible combined public distribution is cleared. The PGF implementation's PPSSPP/JPCSP/intraFont chain is documented in [docs/PGF_LICENSE_REVIEW_PACKET.md](docs/PGF_LICENSE_REVIEW_PACKET.md), replacement-font rights/notices in [THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt](THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt), full-history/privacy sanitation in [docs/KEY_HISTORY_SCRUB.md](docs/KEY_HISTORY_SCRUB.md), and PGD/amctrl distribution posture in [docs/PGD_AMCTRL_REVIEW_PACKET.md](docs/PGD_AMCTRL_REVIEW_PACKET.md). See [NOTICE.md](NOTICE.md) and [docs/PUBLICATION_READINESS.md](docs/PUBLICATION_READINESS.md).

This repository is the sanitized public source repository (`public-safe-v1`); private historical/development material remains outside it. Only public-safe source and content cross this publication boundary. Proprietary game content, generated retail output, private traces/captures, keys, saves, and private historical Git material remain excluded. [NOTICE.md](NOTICE.md), [docs/PUBLICATION_READINESS.md](docs/PUBLICATION_READINESS.md), and [docs/PUBLIC_SOURCE_PROFILE.md](docs/PUBLIC_SOURCE_PROFILE.md) are the detailed authorities; unresolved PGF/PGD and other provenance or legal questions remain open.

This is an independent compatibility/research project. Product and game names are used only to identify compatibility; no affiliation or endorsement is claimed.

## Contributing and security

Read [CONTRIBUTING.md](CONTRIBUTING.md), [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md), and [SECURITY.md](SECURITY.md) before submitting changes. The repository does not yet claim a release-grade security posture; arbitrary PSP/game inputs should be treated as untrusted until the parser/span hardening campaign is complete.
