# Nakagawa Recomp

> **Independent research/compatibility project.** The repository-level project declaration is GPL-3.0-or-later, while many source files retain GPL-2.0-or-later or upstream-specific terms. Inherited PGF/font and PGD/amctrl questions remain under explicit review. See [NOTICE.md](NOTICE.md) and [docs/PUBLICATION_READINESS.md](docs/PUBLICATION_READINESS.md); do not treat the project declaration as final clearance for every combined configuration.

Nakagawa Recomp is an experimental static recompiler for the PSP release of *Hot Shots Tennis: Get a Grip*. Its name comes from the in-game Nakagawa Tennis Club. It translates a user-supplied decrypted PRX/ELF into C, links it with a native C runtime, and runs the result on Windows through SDL3 and Vulkan.

**Project lineage:** Nakagawa Recomp began as a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project), a GPL-2.0-or-later PSP static-recompiler toolkit, and still contains substantial code inherited from that project. Nakagawa has since substantially extended and modified that codebase. See [NOTICE.md](NOTICE.md) for the public attribution boundary and [assets/public_provenance_ledger.json](assets/public_provenance_ledger.json) for the path-hashed public provenance ledger.

`Jstar269/nakagawa-recomp` is the active sanitized public source repository. Its
public history deliberately begins with the sanitized restoration lineage; the
former development history is not ordinary `main` ancestry and must not be
reconnected. Publication gates are engineering and provenance controls, not
legal clearance.

The project is not a game download or a general-purpose PSP emulator. It does not include the game, firmware modules, private keys, or private oracle traces. Development requires files from the user's own lawfully obtained copy.

This is an unofficial compatibility/research project. **"Independent" describes its relationship to Sony Interactive Entertainment, Clap Hanz, and the game rights-holders; it does not mean the recompiler codebase is clean-room or independently originated.** Nakagawa is not affiliated with or endorsed by Sony Interactive Entertainment, Clap Hanz, PPSSPP, sal063, or other upstream toolkit authors; names and marks are used only to identify compatibility and source lineage.

## Project status

This source is an experimental compatibility/research project and **not an end-user release**. The public-source boundary deliberately makes no claim about title playability, private runtime routes, or hardware acceptance. See [docs/PUBLICATION_READINESS.md](docs/PUBLICATION_READINESS.md) for the evidence boundary and public GitHub Issues for curated engineering work.

The recompiler is experimental, and active development focuses on fidelity, timing, HLE completeness, and graphics/audio rendering. Current known open areas include:

- PSP HLE and scheduler edge cases tracked by the public issue tracker;
- source-owned ATRAC3+ decoder/bridge behavior ([`src/rt/atrac3p/PROVENANCE.md`](src/rt/atrac3p/PROVENANCE.md));
- full PSMF integration and other title-specific behavior, which are outside
  this public repository's public-safe acceptance boundary.

**Public GitHub Issues are canonical for active defects and acceptance criteria where a curated public issue exists.** [`ISSUES.md`](ISSUES.md) provides the concise status map across public issues and reference evidence. Hosted GitHub Actions workflows define automated verification gates on public commits.

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
| `font/` | Replacement-font provenance/review material; the public profile excludes unresolved PGF/font payloads |
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
the explicit exclusions in `assets/public_source_profile.json`.

The checked-in GitHub Actions workflow defines path-gated public/synthetic Python, lint, native-object, reference-interpreter, translation, renderer-comparison, and dashboard gates without proprietary game inputs. [`docs/CI.md`](docs/CI.md) documents the applicability matrix and the stable `CI required` aggregate status.

The full `make verify` path additionally requires external oracle traces and a microtest module and intentionally reports blocked when they are absent. See [`docs/STATIC_VERIFY.md`](docs/STATIC_VERIFY.md).

## Documentation and work tracking

Start at [`docs/README.md`](docs/README.md).

- **GitHub Wiki:** [Nakagawa Recomp project manual](https://github.com/Jstar269/nakagawa-recomp/wiki).
- **Broader PSP research/reference:** [recomp.jaycast.net](https://recomp.jaycast.net/) covers generalized PSP recompilation and hardware research.
- **GitHub Issues:** canonical actionable work items and acceptance criteria where curated public issues exist.
- [`ISSUES.md`](ISSUES.md): concise current-status dashboard.

## Legal and provenance

The repository-level project declaration is **GPL-3.0-or-later**, as reflected by [LICENSE](LICENSE), `assets/release_manifest.json`, and the dashboard package metadata. Many source files and inherited components retain GPL-2.0-or-later or other upstream-specific terms; that does **not** establish that every possible combined public distribution is cleared. The explicit source boundary and machine-readable provenance are in [NOTICE.md](NOTICE.md), [assets/public_source_profile.json](assets/public_source_profile.json), and [assets/public_provenance_ledger.json](assets/public_provenance_ledger.json). The public source profile excludes unresolved PGF/font and PGD/amctrl surfaces.

The active public repository can also be used to construct a fresh candidate or
release export under the explicit public-source profile. Such an export must
exclude proprietary game content, generated retail output, private
traces/captures, keys, saves, private routes, private repository metadata, and
unresolved PGF/PGD/audio/ISO components. [NOTICE.md](NOTICE.md),
[docs/PUBLICATION_READINESS.md](docs/PUBLICATION_READINESS.md), and
[docs/PUBLIC_SOURCE_PROFILE.md](docs/PUBLIC_SOURCE_PROFILE.md) describe the
boundary; none is legal clearance.

This is an independent compatibility/research project. Product and game names are used only to identify compatibility; no affiliation or endorsement is claimed.

## Contributing and security

Read [CONTRIBUTING.md](CONTRIBUTING.md), [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md), and [SECURITY.md](SECURITY.md) before submitting changes. The repository does not yet claim a release-grade security posture; arbitrary PSP/game inputs should be treated as untrusted until the parser/span hardening campaign is complete.
