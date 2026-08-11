# Nakagawa Recomp

> **Independent research/compatibility project.** The repository-level project declaration is GPL-3.0-or-later, while many source files retain GPL-2.0-or-later or upstream-specific terms. Inherited PGF/font and PGD/amctrl questions remain under explicit review. See [NOTICE.md](NOTICE.md) and [docs/PUBLICATION_READINESS.md](docs/PUBLICATION_READINESS.md); do not treat the project declaration as final clearance for every combined configuration.

Nakagawa Recomp is an experimental static recompiler for the PSP release of *Hot Shots Tennis: Get a Grip*. Its name comes from the in-game Nakagawa Tennis Club. It translates a user-supplied decrypted PRX/ELF into C, links it with a native C runtime, and runs the result on Windows through SDL3 and Vulkan.

**Project lineage:** Nakagawa Recomp began as a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project), a GPL-2.0-or-later PSP static-recompiler toolkit, and still contains substantial code inherited from that project. Nakagawa has since substantially extended and modified that codebase. See [NOTICE.md](NOTICE.md) and the [sal063 retention/provenance audit](docs/provenance/SAL063_RETENTION_2026-08-06.md) for detailed attribution, retained-code measurements, and downstream provenance.

The project is not a game download or a general-purpose PSP emulator. It does not include the game, firmware modules, private keys, or private oracle traces. Development requires files from the user's own lawfully obtained copy.

This is an unofficial compatibility/research project. **"Independent" describes its relationship to Sony Interactive Entertainment, Clap Hanz, and the game rights-holders; it does not mean the recompiler codebase is clean-room or independently originated.** Nakagawa is not affiliated with or endorsed by Sony Interactive Entertainment, Clap Hanz, PPSSPP, sal063, or other upstream toolkit authors; names and marks are used only to identify compatibility and source lineage.

## Project status

This repository is under active bring-up and is **not an end-user release**. Development builds reach the title, main menu, 3D lobby, and active tennis gameplay, and return from a match to the club. As of 2026-07-25 a deterministic scripted-input route reproduces that path on current `main` — through the coin toss into a rally with points scored, then pause, give up, savedata dialogs, and back to the club — without the dispatch/heap/stall failures that blocked earlier builds.

Two earlier visual-corruption claims were retired by controlled pixel evidence: the Options screen and club interior after returning from Exhibition render correctly on the accepted route, and #29 is closed as not reproducible. Current known defects are narrower:

- #143's formatter fix is merged and both reported UI surfaces have exact-main visual evidence; the issue was closed as completed on 2026-08-04 ([#143](https://github.com/Jstar269/nakagawa-recomp/issues/143));
- #142's display-latch correction is merged and the original exact-main dense replay did not reproduce the absence; the issue was closed as completed on 2026-08-04 ([#142](https://github.com/Jstar269/nakagawa-recomp/issues/142));
- in-match HUD portraits are empty because the guest constructs a mismatched face-resource path ([#139](https://github.com/Jstar269/nakagawa-recomp/issues/139));
- background music and intro-movie output remain incomplete ([#32](https://github.com/Jstar269/nakagawa-recomp/issues/32), [#31](https://github.com/Jstar269/nakagawa-recomp/issues/31)); and
- **current gameplay has not yet received two formal reproducible #33 Benchmark baselines**. Historical/title-route measurements exist, but they are not substitutes for the current gameplay route ([#33](https://github.com/Jstar269/nakagawa-recomp/issues/33)).

**GitHub Issues are the canonical source of truth for actionable defects and acceptance criteria.** [`ISSUES.md`](ISSUES.md) is the concise dashboard; [`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md) preserves dated/resolved evidence. Historical screenshots and milestones are not guarantees for later revisions.

Hosted GitHub Actions execution is active again. The latest full successful hosted run was run `30733971304`, a manual validation of the post-#234 integration candidate. It passed the classifier, hygiene/security, Markdown, native/translation, dashboard, main-smoke, Python, Windows, and aggregate gates. That run is evidence for its recorded head, not an automatic claim about every later commit; Dependabot PRs remain draft and are not substitute CI evidence.

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

`python tools/extract_xb.py` can regenerate `xbdata_extracted/`; it requires a local checkout of [libxb](https://github.com/kiwi515/libxb) under `third_party/libxb/` at the audited commit `ce6df78e5ca99241dd2bbbd68ca485e34003d760` (the 0.2.0 source snapshot). That optional dependency remains local-only; direct-archive containment and runtime semantics are investigated in [docs/ISSUE196_DIRECT_XB.md](docs/ISSUE196_DIRECT_XB.md) and tracked under [#15](https://github.com/Jstar269/nakagawa-recomp/issues/15), [#149](https://github.com/Jstar269/nakagawa-recomp/issues/149), and [#196](https://github.com/Jstar269/nakagawa-recomp/issues/196).

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
> Compiler/codegen profile invalidation, shader-source/embed freshness, and transitive-header dependency tracking are enforced by content-addressed manifests, deterministic shader verification, and `-MMD -MP` metadata (#146, #147, #150). Performance/profile experiments should still explicitly rebuild all affected runtime objects, and high-confidence verification after broad build-system changes should use a true clean/known-complete rebuild.

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
| `font/` | Replacement PGF fonts; exact per-font redistribution chains remain pre-publication work |
| `interface/` | Separate local-only Next.js dashboard/prototype; not part of `hst.exe` |
| `docs/` | Architecture, setup, debugging, porting, governance, verification, and legal/provenance engineering records |
| `build/` | Fully generated local output; ignored by Git |

Generated `build/<game>/<game>_recomp_*.c` files must never be edited. Change generator/runtime source and regenerate them. The number of generated translation units is controlled by `FUNCS_PER_CHUNK`; it is dynamic and not a fixed HST chunk count.

## Verification

```powershell
.\hst_manager.ps1 -Action Test
python -m unittest discover -s tools -p "test_*.py" -v
python tools/publish_audit.py --tracked-only --worktree
```

`--worktree` audits the bytes on disk. Without it the audit reads staged Git blobs, which is
what the pre-commit hook wants but means an unstaged edit goes unexamined.

The checked-in GitHub Actions workflow defines path-gated public/synthetic Python, lint, native-object, reference-interpreter, translation, renderer-comparison, and dashboard gates without proprietary game inputs. [`docs/CI.md`](docs/CI.md) documents the applicability matrix and the stable `CI required` aggregate; manual run `30733971304` is the latest full hosted validation, while exact-head status must be checked separately for each later revision.

The full `make verify` path additionally requires external oracle traces and a microtest module and intentionally reports blocked when they are absent. See [`docs/STATIC_VERIFY.md`](docs/STATIC_VERIFY.md).

## Documentation and work tracking

Start at [`docs/README.md`](docs/README.md).

- **GitHub Wiki:** [Nakagawa Recomp project manual](https://github.com/Jstar269/nakagawa-recomp/wiki).
- **Broader PSP research/reference:** [recomp.jaycast.net](https://recomp.jaycast.net/) covers generalized PSP recompilation and hardware research.
- **GitHub Issues:** canonical actionable work items and acceptance criteria.
- [`docs/NEXT_SESSION.md`](docs/NEXT_SESSION.md): current machine-capable handoff and evidence discipline.
- [`ISSUES.md`](ISSUES.md): concise current-status dashboard.
- [`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md): dated resolved evidence and superseded hypotheses.
- [`AGENTS.md`](AGENTS.md): repository working rules for human and AI-assisted development.

## Legal and provenance

The repository-level project declaration is **GPL-3.0-or-later**, as reflected by [LICENSE](LICENSE), `assets/release_manifest.json`, and the dashboard package metadata. Many source files and inherited components retain GPL-2.0-or-later or other upstream-specific terms; that does **not** establish that every possible combined public distribution is cleared. The PGF implementation's PPSSPP/JPCSP/intraFont chain is tracked in [#98](https://github.com/Jstar269/nakagawa-recomp/issues/98), replacement-font rights/notices in [#99](https://github.com/Jstar269/nakagawa-recomp/issues/99), full-history/privacy sanitation in [#102](https://github.com/Jstar269/nakagawa-recomp/issues/102), and PGD/amctrl distribution posture in [#104](https://github.com/Jstar269/nakagawa-recomp/issues/104). See [NOTICE.md](NOTICE.md) and [docs/PUBLICATION_READINESS.md](docs/PUBLICATION_READINESS.md).

This repository is the sanitized public source repository (`public-safe-v1`); private historical/development material remains outside it. Only public-safe source and content cross this publication boundary. Proprietary game content, generated retail output, private traces/captures, keys, saves, and private historical Git material remain excluded. [NOTICE.md](NOTICE.md), [docs/PUBLICATION_READINESS.md](docs/PUBLICATION_READINESS.md), and [docs/PUBLIC_SOURCE_PROFILE.md](docs/PUBLIC_SOURCE_PROFILE.md) are the detailed authorities; unresolved PGF/PGD and other provenance or legal questions remain open.

This is an independent compatibility/research project. Product and game names are used only to identify compatibility; no affiliation or endorsement is claimed.

## Contributing and security

Read [CONTRIBUTING.md](CONTRIBUTING.md), [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md), and [SECURITY.md](SECURITY.md) before submitting changes. The repository is not yet accepting a public release-security posture; arbitrary PSP/game inputs should be treated as untrusted until the parser/span hardening campaign is complete.
