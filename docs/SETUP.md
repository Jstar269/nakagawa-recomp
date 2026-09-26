# Build and development setup

The supported and tested core build is Windows 11 x64; the host-neutral object
gate is a portability probe, not Linux support. For an ISO/player workflow, start with
[`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md). For development, see
[`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) and
[`LINUX_DEVELOPMENT.md`](LINUX_DEVELOPMENT.md). The native player is the repository's only user interface.

## Supported development baseline

The supported and tested core development environment is:

- Windows 11 x64. Older or unsupported Windows versions may work, but receive no compatibility guarantee.
- PowerShell 7.4+ (`pwsh`). Windows PowerShell 5.1 is not supported.
- CPython 3.14.x (`>=3.14,<3.15`), with `python` resolving to that feature line.
- Current MSYS2 UCRT64 GCC/G++, GNU Make, SDL3, and Vulkan loader packages. The release
  manifest's `toolchain_policy` records only the floors the code actually needs: a C11 compiler
  (GCC 4.9+), GNU Make 3.81+, SDL3 3.x, a Vulkan SDK for API 1.1+, and Python 3.14. The rolling
  MSYS2 packages are not pinned; `tools/record_toolchain.py` records the versions a release
  candidate was really built with (#367).
- A current Vulkan SDK and Vulkan-capable GPU.

PowerShell 7.4 is the current floor and the oldest line Microsoft still supports;
[issue #337](https://github.com/Jstar269/nakagawa-recomp/issues/337) records the
source inventory and cross-version test evidence. Scripts treat empty and absent
environment values alike so the supported 7.4/7.5/7.6 lines behave consistently.

Microsoft ends support for both 7.4 and 7.5 on 2026-11-10. After that date the floor
moves to 7.6 (LTS, supported until 2028-11-14).

The environment doctor is the executable form of this contract:

```powershell
python tools/nk_doctor.py --scope build
```

For the Vulkan SDK, discovery is explicit and fail-closed: `-VulkanSdk` wins first, then
`VULKAN_SDK`, then the newest numerically named usable installation under `C:\VulkanSDK`. A usable
installation contains the Vulkan headers and loader import library required by the build. Do not
copy a patch version from another machine into setup instructions.

## 1. Install the core toolchain

Install [MSYS2](https://www.msys2.org/), open its **UCRT64** terminal, and run:

```bash
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader
```

Also install:

- CPython 3.14.x on `PATH`.
- The [Vulkan SDK](https://vulkan.lunarg.com/sdk/home). The manager follows the discovery order above; pass `-VulkanSdk "C:\path\to\sdk"` or set `VULKAN_SDK` when an explicit location is needed.
- Git, to fetch optional third-party source.

From the repository root, install the declared Python tooling dependencies once:

```powershell
python -m pip install .
```

This installs the declared tooling dependencies, including `compiledb`.

`glslc` from the Vulkan SDK is only needed when regenerating the checked-in shader headers. LLD, Clang, CMake, Ninja, and Node.js are not required for the Windows core build (a staged CMake target for portability work is tracked separately in [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md)).

### Deliberate path conventions

Absolute paths in first-party tooling are limited to platform defaults, never a
developer's machine layout. The conventions in force are:

- `C:\msys64\ucrt64\bin` — the MSYS2 UCRT64 install root used above. Tools accept
  an explicit location first (`-MsysPath`, `MSYS_PATH`, or a `gcc` already on
  `PATH`) and otherwise assume this default install.
- `C:\VulkanSDK` — the default Vulkan SDK root, after `-VulkanSdk` and `VULKAN_SDK`.
- `C:\Windows` — the Windows system directory, used only when `WINDIR` is unset
  (system font discovery).

Any other absolute path must come from the repository root, an environment
variable, or an explicit flag. `tools/test_workspace_paths.py` fails the tracked
tree when a user-profile path or a workspace root appears in a first-party file.

### Runtime DLLs (SDL3.dll & vulkan-1.dll)

The native player (`nakagawa_player.exe`) requires `SDL3.dll` and the host Vulkan loader (`vulkan-1.dll`). In a release archive, the matching `SDL3.dll` is already beside `bin/nakagawa_player.exe`; leave both files in `bin/`. In a source checkout, `copy_build_assets.ps1` copies SDL3 and any imported MinGW runtime DLLs beside the built executable and generates their third-party notices.

#### 1. SDL3.dll

For a source build, install the MSYS2 UCRT64 SDL3 package described above. Do not copy the DLL into the repository root; the build asset script resolves the toolchain copy and stages it beside the player. For a release package, use the SDL3 DLL already present in `bin/`.

#### 2. vulkan-1.dll

This is the host Vulkan loader, normally installed by the graphics driver. The release package records it as host-resolved and does not redistribute it by default.

Windows normally resolves it from the installed NVIDIA, AMD, or Intel graphics driver. The Vulkan SDK's loader is useful for development diagnosis, but it is not copied into the release package; install or repair a Vulkan-capable graphics driver if the loader is missing.

#### 3. Verifying Correctness & Compatibility

To ensure your runtime DLLs are compatible and up to date:

- **64-bit (x64) Architecture Check:** The player and bundled SDL3 DLL are 64-bit. A mismatched DLL can prevent Windows from starting the player.
- **Version/Metadata Verification:**
  To check details, right-click the DLL file in Windows Explorer, select **Properties**, and navigate to the **Details** tab:
  - For `SDL3.dll`: The package generator records the version in the SBOM and notices.
  - For `vulkan-1.dll`: The driver supplies the loader; match it to the installed graphics driver rather than copying a different SDK build beside the player.

Confirm the commands visible to PowerShell 7:

```powershell
python --version
pwsh --version
mingw32-make --version
gcc --version
g++ --version
```

Run `glslc --version` only when regenerating shader headers; it is not a core build prerequisite when
the checked-in generated shader includes are current.

## 2. Supply local game inputs

The repository intentionally excludes all game content. Keep private inputs in
the Git-ignored `place_game_here/` folder. The canonical runtime/build layout
is:

```text
place_game_here/
├── EBOOT.elf
├── ISO/<game>.iso
└── EXTRACTED/
    ├── decrypted/
    │   ├── libfont.prx
    │   ├── scePsmf_library.prx
    │   └── scePsmfP_library.prx
    └── PSP_GAME/
        ├── SYSDIR/EBOOT.BIN
        └── USRDIR/xbdata_extracted/
```

The manager resolves these paths directly. Root-level `eboot.elf` and
`game.iso` links remain a legacy fallback, not a requirement. Any source PBP,
document, or other retail container stays outside the working set; it is not
needed once the canonical local inputs exist.

Missing decrypted PRXs prevent late-import registration; missing extracted XB
data breaks plain-file asset lookups. `SYSDIR/EBOOT.BIN` supplies the PSP
header/BSS metadata while `place_game_here/EBOOT.elf` remains the flat
translation input. Do not replace the flat input mechanically until the
repacker preserves its current memory layout.

For automated or pre-loaded runs that require installed game data:

- Utility savedata uses the hierarchical
  `memstick/PSP/SAVEDATA/<game><save>/` tree.
- Ordinary guest `sceIoOpen("ms0:...")` calls and utility savedata share one
  canonical host Memory Stick root (`SR_MEMSTICK`, default `memstick/`) and
  one path resolver. Title-specific cached assets and save identities are
  private inputs; this public guide deliberately does not enumerate them.
- Legacy hierarchical trees under `fs/` are no longer listed or auto-migrated;
  a read-open miss may import a legacy flat `fs/` file once into the unified
  root, but write/create never creates under `fs/`.

The unified Memory Stick root is the production contract; `fs/` remains only
as a read-only legacy-import source until existing menu routes are
revalidated against the unified tree.

To regenerate the extracted asset tree, run the extractor. It has no
third-party dependency:

```powershell
python tools/extract_xb.py place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata --output place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted -v
```

Extraction is entirely repository-owned: `tools/xb_probe.py` parses the archive
and decodes every member — including the nested `DEFLATE → LZS` layer — under
the budgets declared at the top of `tools/extract_xb.py`, and that same module
normalizes each member name once and writes it itself. The name that is
validated is the name that is written, on every host. An archive containing an
escaping, ambiguous, or colliding member name, one that breaches a decode
budget, or one the reader cannot parse at all is refused rather than extracted;
each archive is built in a staging directory and promoted only after the whole
of it succeeds. The extractor refuses to reuse a non-empty destination or
replace an existing generated file unless `--overwrite` is passed, `--workers`
defaults to a small cap rather than the CPU count because each worker holds a
whole archive in memory, and the number of in-flight worker tasks is bounded
independently of how many archives were found.

[libxb](https://github.com/kiwi515/libxb) is not used by the build, runtime, or
extractor. Its formerly audited `0.2.0` snapshot and the measured comparison boundary
are retained as historical evidence in
[`ISSUE196_DIRECT_XB.md`](ISSUE196_DIRECT_XB.md); no optional checkout is required.

`third_party/` and `place_game_here/` are local-only and ignored by Git. If you use `tools/validate_assets.py`, its optional `tools/reference_hashes.json` reference file is also local-only; it is not required by the normal build.

### Plain module inputs

Some titles load additional modules at runtime. The runtime accepts only plain (unencrypted)
ELF/PRX files; this repository ships no keys or key material. The [built-in decryption boundary](#built-in-decryption-boundary-issue-295) below can unwrap a lawfully obtained disc's executable and required modules when you supply your own local key file. For what the player needs
and where your own unencrypted files go, see [`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md). When you already have plain modules from your own
lawfully obtained copy, place them at the paths the local manifest expects, for example:

```text
place_game_here/EXTRACTED/decrypted/libfont.prx
place_game_here/EXTRACTED/decrypted/scePsmf_library.prx
place_game_here/EXTRACTED/decrypted/scePsmfP_library.prx
```

A valid plain module begins with the ELF magic bytes `7F 45 4C 46`; a file beginning with `~SCE`
or `~PSP` is an encrypted container and is rejected. Never copy game or firmware material into
Git history.

### Per-title decrypted input folder (standalone player and CLI)

For the standalone player (`nakagawa_player.exe`) and `tools/nk_cli.py`:
When an imported ISO contains an encrypted executable (`EBOOT.BIN`), preflight checks in both the player and `nk_cli inspect` direct the user to supply their own decrypted modules in a per-title folder under the per-user data directory:

```text
<user data>/titles/<DISC_ID>/decrypted/
├── EBOOT.elf
└── <module>.prx
```

On Windows, the default per-user data directory is `%LOCALAPPDATA%\Nakagawa\data` (resolving to `<user data>/titles/<DISC_ID>/decrypted/`). When a valid plain MIPS ELF32 `EBOOT.elf` is placed in this folder, the player and CLI select it automatically for analysis ([#428](https://github.com/Jstar269/nakagawa-recomp/pull/428)). If an experimental profile was created while the disc executable was still encrypted (binding no executable), supplying `EBOOT.elf` in this folder is automatically used by `nk_cli build-package` without requiring re-import.

Decrypted guest modules in `<user data>/titles/<DISC_ID>/decrypted/` may be named either after their file name on the disc (for example `psmf.prx`) or after their manifest module name (for example `scePsmf_library.prx`). When a module is encrypted, invalid, or missing, error messages display both names (for example `psmf.prx (scePsmf_library.prx)`). The project ships no keys; the built-in boundary below fills this folder from your own key file when one is present ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)).

### Built-in decryption boundary (issue #295)

The standalone player and `nk_cli` contain a built-in decryption boundary for a lawfully supplied disc image. It understands the container forms the pipeline meets — `~PSP` executables and PRXs (including `~SCE` outer wrappers and PBP `DATA.PSP` entries) plus the gzip-compressed payloads they can carry — and unwraps them into plain MIPS ELF32 images for the analyzer. The algorithms and container formats live in this repository; the keys do not.

**The boundary never contains key material.** You supply a local-only key file at:

```text
<user data>/keys/psp-keyfile.json
```

or at the path named by the `NAKAGAWA_PSP_KEY_FILE` environment variable. The file is JSON carrying `"format": "nakagawa-psp-keystore-1"` and an `entries` object; each entry is named by the thing it unlocks (for example a `prx.tag.0x........` recipe object, `kirk.cmd1.key`, or `kirk.keyvault.<slot>`) with hexadecimal values only. The KeyStore validates the shape, names, and value lengths before anything is decrypted.

How the boundary behaves:

- With a valid key file, `nk_cli inspect`, `build-package`, `bringup`, and the player's compatibility preflight decrypt the disc executable automatically and continue to the analyzer. Decrypted bytes are written only under the private per-user data directory (`titles/<DISC_ID>/decrypted/` and `cache/decrypted/`) — never next to the ISO, and never into this repository.
- Without a key file, or when an entry is missing, the boundary fails closed and names the exact entry the container needs (for example `MISSING_KEY_ENTRY prx.tag.0x........` and "this executable needs key entry ..."). The existing guidance to supply decrypted modules at `<user data>/titles/<DISC_ID>/decrypted/` remains, so that route keeps working; a user-supplied `EBOOT.elf` takes precedence over automatic decryption.
- The key file is never uploaded, never packaged, and never committed. The publication audit rejects key-file paths and KeyStore content outright, and neither the tests nor any release contain key material — the tests generate clearly fake per-run keys only.

### User title manifests (`<user data>/manifests`)

For titles requiring a title manifest overlay, place the manifest JSON files in:

```text
<user data>/manifests/
└── <manifest_name>.json
```

On start-up, `nakagawa_player.exe` automatically scans `<user data>/manifests` and loads every `*.json` file as an overlay in alphabetical filename order (via `nk_title_manifest_load_overlay_dir`, capped at `NK_MANIFEST_MAX_OVERLAYS` = 8). Any malformed or skipped files are reported to stderr.

Similarly, `python tools/nk_cli.py build-package` searches `assets/titles` first and then `<user data>/manifests`, resolving title manifests identically to the native player so user-configured titles build without command-line overlay arguments.

### System fonts

Authentic in-game typography requires PSP system fonts in Sony's PlayStation Glyph Format (`.pgf`), dumped from the user's own physical PSP console firmware (`flash0:/font/`). Proprietary firmware fonts cannot legally be bundled or distributed by the project.

> [!NOTE]
> Do not confuse system fonts with `libfont.prx`. `libfont.prx` (located under `place_game_here/EXTRACTED/decrypted/libfont.prx` or `<user data>/titles/<DISC_ID>/decrypted/libfont.prx`) is a game-supplied guest middleware PRX executable module implementing the `sceFont` API; it does not contain the actual font glyph outlines. Typography requires the separate `.pgf` font files.

#### What the user supplies

From your own lawfully owned PSP console or firmware dump, supply genuine PGF font files:

- `jpn0.pgf`: Japanese and baseline font required by the runtime font manager.
- Optional Latin and regional fonts: `kr0.pgf` and `ltn0.pgf` through `ltn15.pgf`.

Fonts must come only from your own device; the project provides no download links or third-party repositories.

#### Importing fonts

Import dumped firmware fonts using `tools/nk_cli.py`:

```powershell
python tools/nk_cli.py fonts import <folder>
```

The `<folder>` argument can point directly to the directory containing your `.pgf` files, or to a dump directory containing `font/`, `FONT/`, `flash0/font/`, or `flash0/FONT/` subdirectories.

Available options:

- `--user-data-root <path>`: Override the target per-user data directory (default: `%LOCALAPPDATA%\Nakagawa\data` on Windows, `~/Library/Application Support/NakagawaRecomp/data` on macOS, `$XDG_DATA_HOME/nakagawa-recomp` or `~/.local/share/nakagawa-recomp` on Linux).
- `--json`: Emit a machine-readable JSON report of the imported files, sizes, and SHA-256 digests.

The import command scans the source directory, performs structural validation on every PGF candidate (verifying header size, little-endian offsets, the `PGF0` magic signature at `header_offset + 4`, non-negative revision and version fields, and `first_glyph <= last_glyph` index order), and stages validated fonts into the versioned cache directory:

```text
<user data>/fonts/v1/
├── manifest.json
├── jpn0.pgf
└── ltn0.pgf
```

Along with the `.pgf` files, the command writes a validated `manifest.json` recording `schema_version: 1`, an ISO 8601 UTC `import_time`, and each file's size and SHA-256 hash. Re-import is idempotent. If any font file in the source folder fails validation, the import fails closed immediately, leaving the previous cache intact.

#### Verification and player status

To verify that system fonts are correctly installed:

1. **CLI verification:** Run `python tools/nk_cli.py inspect <iso>` (or add `--root <user_data>`). Under the compatibility preflight checklist, verify the `SYSTEM_FONTS` check:

   ```text
   OK: User-supplied PSP system font jpn0.pgf is available.
   ```

2. **Player preflight checklist:** In `build/nakagawa_player.exe`, select your game card. The preflight checklist reports:
   - `OK` with `"User-supplied PSP system font jpn0.pgf is available."` when the cache is valid and contains `jpn0.pgf`.
   - `MISSING` with `"PSP font jpn0.pgf missing; run fonts import <folder> (#300)."` when no font cache or fallback font is detected.
   - `INVALID` with a specific diagnostic (e.g. `"PSP font cache manifest is unreadable or malformed..."` or checksum mismatch) when the cache or any declared font file is corrupted.
3. **Player setup wizard:** During first-time launch setup, the **System & Open-Source Typography** step displays typography settings, noting that open-source defaults (SIL Open Font License) are used for desktop UI while optional user-owned PSP fonts (`jpn0.pgf`) are loaded from the font cache for in-game rendering.

#### In the works

- **#300 (Automated in-player font provisioning):** Providing a direct in-app font import wizard in the player so command-line execution is not required.
- **#313 (Clean-room open-font to PGF converter):** Designing and implementing an independent, deterministic converter from permissively licensed open fonts (SIL OFL) to PSP PGF format, providing an authentic-proportioned public font route that eliminates the proprietary firmware dependency for public builds.
- **#299 (Guest libfont module startup):** Executing genuine guest `libfont.prx` startup routines rather than relying on host flag injection.

## 3. Build

The public checkout's end-to-end display build and player launch is:

```powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
mingw32-make --no-print-directory display-smoke-player
```

This generates the source-owned display fixture, runs the full two-phase pipeline,
verifies the presented framebuffer, builds the native player, and exercises its Play
route without private input. Use `mingw32-make --no-print-directory display-smoke` for
the headless build/verification without opening a window. Generated C is split into a
dynamic number of translation units based on the discovered function count and
`FUNCS_PER_CHUNK`.

For a validated local title manifest, use the manager so every title-derived value
comes from that manifest:

```powershell
.\nk_manager.ps1 -Action BuildFull -TitleManifest C:\path\to\manifest.json -GameName game
.\nk_manager.ps1 -Action BuildFast -TitleManifest C:\path\to\manifest.json -GameName game
```

If `-GameName` is omitted, the manager uses the manifest's `game_name` or a portable
ID fallback; an explicit value overrides that selection. Direct Make is title-neutral
but does not run the manager planner; it must receive the validated
`TITLE_MANIFEST` and every title-derived Make value explicitly. Direct Make also does
not discover the Vulkan SDK, so set `VULKAN_SDK` or pass it as a Make variable.

To build a generated v1 runtime package for an imported disc in the player library:

```powershell
python tools/nk_cli.py build-package <disc_id>
```

This extracts the plaintext executable (or automatically uses `EBOOT.elf` and guest PRXs from `<user data>/titles/<DISC_ID>/decrypted/`), runs the build pipeline, stages runtime assets, and promotes the package to `<user data>/packages/<DISC_ID>/`. The native player (`build/nakagawa_player.exe`) discovers, validates, and launches packages from that directory ([#297](https://github.com/Jstar269/nakagawa-recomp/issues/297)). An explicit `--module-dir <directory>` can be passed when modules are located elsewhere. When a manifest reads BSS metadata from the disc's `~PSP` executable header (`bss_metadata_source: "psp-header"`), `build-package` extracts it directly from the disc image into `cache/packages/<DISC_ID>/selected.psp` and verifies the `~PSP` magic, so `--psp-header` is only needed if the file on the disc lacks that header.

The built package ships its required guest modules inside `<package>/modules/` (copied under their manifest names), and the launcher sets `SR_MODULE_DIR` to `<package>/modules` so launches are self-contained without external dependencies. A run begins at the title's resolved run entry (`run_entry`: a declared `runtime_bindings.fallback_entry`, else `executable.entry`), ensuring that titles requiring a fallback entry point start there across both the native player and CLI launchers. Re-adding an already imported disc in the library preserves its completed staging and extraction state (`player_merge_readded_game`) when the disc ID, version, and image size match. In a checkout without the private PGF/PGD runtime backends, `build-package` builds with the public backends automatically; PGD-protected game data is then unavailable at run time ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295) in the works). Only an explicit private-backend build (`PUBLIC_SAFE=0`) refuses in such a checkout, with a message naming [#297](https://github.com/Jstar269/nakagawa-recomp/issues/297).

Package work is content-addressed below `<user data>/cache/packages/<DISC_ID>/`; the cache key covers executable bytes, analyzer/codegen semantics, codegen options, generated-code/runtime ABI epochs, compiler identity/target, and native flags. An unchanged key reuses the published package, an ABI-compatible runtime or compiler change recompiles native objects while retaining generated C, and any other semantic change regenerates AOT. The builder writes and verifies `completion-manifest.json` before an atomic directory promotion; interrupted or corrupt entries are refused by the player and launcher. The full contract and rebuild-component names are in [`RUNTIME_PACKAGING_ARCHITECTURE.md`](RUNTIME_PACKAGING_ARCHITECTURE.md#6-private-content-addressed-cache-contract-316), tracked by [#316](https://github.com/Jstar269/nakagawa-recomp/issues/316).

To inspect a private package cache with Doctor, pass the user-data root and disc ID:

```powershell
python tools/nk_doctor.py --scope products `
  --user-data-root "$env:LOCALAPPDATA\Nakagawa\data" `
  --disc-id <disc_id>
```

Doctor reports a missing, incomplete, or stale cache entry and names the changed key component when a current key is available. Cache cleanup is bounded per title; set `NK_AOT_CACHE_MAX_ENTRIES` to a positive limit when the default of eight is unsuitable.

Alternatively, to build a local AOT package directly from a validated title manifest and plaintext executable ELF, use
the planner's package action. It runs the same two-phase Make pipeline and writes the executable,
generated objects, `package.json`, and `build-report.json` under one dedicated untracked directory:

```powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
python tools/title_codegen_plan.py assets/titles/my-title.json `
  --package `
  --game-elf place_game_here/EBOOT.elf `
  --output-dir build/my-title
```

For manifests with guest PRXs, add `--module-dir <directory>`; every required manifest module must
exist there, and optional modules are included only when named with
`--include-optional-module <manifest-name>`. Add `--psp-header <path>` when the manifest selects
`bss_metadata_source: "psp-header"`. `--public-safe` is available for synthetic fixture builds.
The output directory must be untracked and dedicated. The package schema and explicit unsupported
semantic-boundary records are defined in
[`RUNTIME_PACKAGING_ARCHITECTURE.md`](RUNTIME_PACKAGING_ARCHITECTURE.md#5-local-aot-package-contract-v1).
Inputs whose paths contain spaces or shell-sensitive characters (common under a Windows profile
directory) are copied into `<output-dir>/staged-inputs/` so the Make recipes can use them. When the
output directory contains spaces, the route builds in a Make-safe workspace—resolved from
`NK_BUILD_ROOT` if set, or the parent directory's 8.3 short name on Windows—and atomically promotes
the finished package to the destination. If neither fallback is available, or if the path contains
characters that Make recipes cannot quote, the route stops with the named `PACKAGE_UNSUPPORTED_PATH`
boundary tracked by #296.

`assets/titles/hst-ucus98701.json` is the local HST title manifest (intentionally never checked in; publication-excluded with a `.gitignore` accident guard): it carries HST's guest-address runtime bindings, and a `GAME_NAME=hst` build refuses to compile without it rather than silently producing a runtime with every title binding disabled. Its contents are not published; see [`assets/titles/README.md`](../assets/titles/README.md).

The local manifest must also declare its build name (`"game_name": "hst"`). Generic launch
resolution (issue #366) derives every runtime and image path from validated title identity only:
an HST session reaches `build/hst/hst[.exe]` because HST's own manifest says so, never because a
launcher defaults to it. A manifest without `game_name` fails closed with a missing-runtime error
instead of inheriting another title's build.

The requirement is enforced incrementally, not only on a clean tree: the generated title-config
header is keyed to a content-addressed identity of the effective configuration, so dropping
`TITLE_MANIFEST` from a previously bound build directory refuses rather than reusing the header
that build left behind.

## 4. Run and test

Run `pwsh -NoProfile -File nk_manager.ps1` without an action for usage (exit code 0).
The canonical manager accepts these actions:

| Action | Description |
| --- | --- |
| `BuildFull` | Clean and rebuild the selected title through the full pipeline. |
| `BuildFast` | Incrementally build the selected title without cleaning. |
| `Run` | Launch the selected title with the chosen runtime profile. |
| `Inspect` | Locate a generated C function using `-InspectFunc`. |
| `Clean` | Stop tracked build processes and clear local tracking logs. |
| `Test` | Run the Makefile `selftest` target (C++ reference-runtime selftest), not the Python suite. |
| `Verify` | Run Python tests, native selftests, and import/publication audits. |
| `DiffFunc` | Compare a function against a reference trace using `-DiffTarget` and `-DiffOracle`. |
| `FindSymbol` | Search the optional symbol reference using `-FindName`. |
| `Fuzz` | Run the Makefile `vfpu_fuzz` target. |
| `VisualOracle` | Replay a route and archive visual regression captures. |

```powershell
.\nk_manager.ps1 -Action Test
mingw32-make --no-print-directory display-smoke-gui
.\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run
.\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run -SoftwareRender
.\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run -NoGui -Duration 30
```

The manager examples require a local validated manifest and its declared inputs. For
normal Vulkan runs, an explicit profile can isolate the intended task:

```powershell
.\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run -Profile Performance # log-free smoke test
.\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run -Profile Benchmark   # 1 Hz telemetry + logs/perf.csv
.\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run -Profile Benchmark -GuestProfile # plus guest-PC hotspot summary
```

`Performance` redirects the runtime's stdout and stderr to the null device; it is not a
debugging mode. `Benchmark` retains bounded startup messages and records actual presented
FPS, vblank rate, CPU/host time, GPU-wait time, present time, idle/scheduler time, and
submit/wait counts. `-GuestProfile` additionally enables the generated-PC call/block profiler and
dumps its summary at exit; use it as a second run because the instrumentation itself adds overhead.
The manager also takes `-GuestProfilePeriod N` (default 3,600 vblanks) to control bounded periodic
captures for duration-limited runs; `0` disables periodic dumps.

Host presentation is capped at 30 FPS by default (`SR_FPS_CAP=30`). The scheduler and
PSP vblank continue at ~59.94 Hz, and scenes below 30 FPS are not delayed. Set
`SR_FPS_CAP=0` only for uncapped diagnostics or A/B measurement.

Runtime logs are written under `logs/`. Use `SR_DEBUG=0xFF` for all categories or consult [DEBUGGING.md](DEBUGGING.md) for targeted logging.

Keyboard controls mirror a PSP layout: `X` = Cross/confirm, `Z` = Circle/back, `A` = Square,
`S` = Triangle, `Q`/`W` = L/R, Enter = Start, Shift = Select, and arrow keys = D-pad.
SDL3 gamepads use the south/east/west/north face buttons as Cross/Circle/Square/Triangle.
Short presses are latched until one PSP controller sample consumes them, so normal taps work
even while a frame is slow.

The full `make verify` command needs external oracle data that is not in the repository. Its blocked result is expected when `CODEGEN_ORACLE`, `MICROTEST_MODULE`, or `MICROTEST_ORACLE` is absent.

### Player BUILD PACKAGE prerequisites

**BUILD PACKAGE** in the native player re-runs this toolchain from inside the app, so
it needs all of the following:

- `tools/nk_cli.py` reachable. The player searches, in order: the `NK_INSTALL_ROOT`
  environment variable (a folder containing `tools/`), `<player exe>/tools`,
  `<player exe>/../tools`, `<player exe>/../source/tools` (the v0.0.1 release
  layout), the working directory, and its parent. Running the player from the
  repository root or from `build/` finds it automatically.
- `python`, `gcc`, and `mingw32-make` on `PATH` — the UCRT64 toolchain installed in
  section 1. The spawned build inherits the player's `PATH`, so a tool your shell
  cannot see is a tool the build cannot see either.
- SDL3 headers/import library and Vulkan headers/loader import library from the
  UCRT64 package set. The Makefile can use the MSYS2 `libvulkan-1.dll.a` import
  archive; a separate Vulkan SDK is not needed for this consumer build.
- `powershell.exe`, which ships with Windows 11. The asset-copy step uses the
  built-in Windows PowerShell 5.1; PowerShell 7 remains the development baseline
  for the other repository scripts.

Missing pieces are named on the error card instead of failing deep inside the build:
`CLI_NOT_FOUND` lists every searched location and the `NK_INSTALL_ROOT` fix,
`BUILD_TOOLCHAIN_MISSING` names the missing tool (`python`, `gcc`, or
`mingw32-make`), and `PYTHON_NOT_FOUND` names the interpreter.

The pinned candidate download set is recorded in
[`assets/prereq_manifest.json`](../assets/prereq_manifest.json): CPython 3.14.7
and 25 MSYS2 UCRT64 packages (GCC/binutils, make, SDL3, Vulkan headers/loader,
and their runtime dependencies), totaling 89,547,599 bytes. Package hashes and
sizes come from the signed MSYS2 repository database; the Python hash is from
python.org's release page. `tools/requirements-lock.txt` contains developer and
build-generation tools; the consumer `build-package` path needs no third-party
Python packages, and `glslc` is only used by opt-in shader regeneration.

The player does not yet bootstrap the Python runtime or show the prerequisite
consent/progress flow. If a build prerequisite is missing, the card names that
boundary and points to automatic build-prerequisite installation, in the works
([#324](https://github.com/Jstar269/nakagawa-recomp/issues/324)); it does not
download anything automatically today.

The player's UI typography loads `SDL3_ttf.dll` from beside the executable first,
then from `PATH`; without it the built-in readable debug font is used. Placing
`SDL3_ttf.dll` next to `nakagawa_player.exe` is enough — no rebuild required.

### Build lifecycle and cleanup targets

The build system provides scoped and explicit cleanup targets:

- `mingw32-make clean`: removes all build outputs for the current target (`build/$(GAME_NAME)`).
- `mingw32-make clean-fixtures`: removes temporary smoke test, cosimulation, and oracle build artifacts (`build/production-smoke`, `build/cosim`, etc.).
- `mingw32-make tidy` (or `distclean`): removes intermediate object files (`.o`, `.d`, profile manifests) and ephemeral build logs while preserving linked binaries (`.exe`, `.pdb`) for debugging.
- `mingw32-make clean-all`: comprehensively removes all subdirectories under `build/` and ephemeral build logs under `logs/`.

These targets strictly operate within `build/` and transient log paths, never deleting protected directories (`place_game_here/`, `memstick/`, `keys/`, `oracle/`, `assets/`, `fixtures/`, `docs/`, `src/`, `tools/`).

## 5. Optional developer quality tools

The repository includes shared pre-commit/pre-push checks for text/structured-file hygiene,
baseline Ruff correctness, large files, secret detection, and the publication audit:

```powershell
python -m pip install pre-commit
python -m pre_commit install
python -m pre_commit install --hook-type pre-push
python -m pre_commit run --all-files
```

Invoked as `python -m pre_commit` rather than the bare `pre-commit` console script: pip
installs that script into a user `Scripts/` directory that is frequently absent from `PATH` on
Windows, so the bare form fails with "command not found" immediately after a successful install.
The module form works regardless of `PATH`.

These hooks install their own pinned Ruff and Betterleaks environments. C formatting is defined by
`.clang-format` but is not currently an automatic pre-commit hook. A mypy configuration remains in
`pyproject.toml`, but mypy is **not** a shared gate while the pre-existing Python typing baseline is being corrected. Do not describe a known-failing type check as a required
contributor hook. These tools are not core runtime dependencies.

### PSP analysis tools (optional, never runtime dependencies)

- [PSPSDK](https://github.com/pspdev/pspsdk) is the preferred open source reference for PSP
  headers, NID names, documented error constants, PRX structure, and PBP utilities. Keep a local
  checkout outside the repository or fetch a pinned revision in a reproducible tooling step.
- [PSPLink USB](https://github.com/pspdev/psplinkusb) is useful only when a development-capable
  physical PSP is available to collect independent behavior traces. It is not required to build
  or run the recompiler.
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra) can independently inspect MIPS
  control flow and shared entries when the Python analyzer is ambiguous. Export only scripts,
  address notes, or other redistributable metadata—never a project database containing game
  bytes.

Do not add any of these large checkouts to this repository, and do not make the eventual player
download them. They are development/oracle aids; the release path should remain the recompiler,
its redistributable host dependencies, and user-supplied game input.

## Troubleshooting

- **Preflight diagnostics:** run `.\nk.ps1 Doctor -TitleManifest C:\path\to\manifest.json -GameName game` (or `python tools/nk_doctor.py --title-manifest C:\path\to\manifest.json --game-name game`) to validate the toolchain, build dependencies, and the selected title's local inputs. Without a title selection, Doctor uses the public synthetic manifest.
- **Missing Vulkan headers:** pass the correct `-VulkanSdk` path or `VULKAN_SDK=...` Make variable.
- **`SDL3.dll` missing:** for the release package, keep `bin/SDL3.dll` beside `bin/nakagawa_player.exe`; for a source build, install the MSYS2 UCRT64 SDL3 package so `copy_build_assets.ps1` stages it beside `build/nakagawa_player.exe`.
- **`PUBLIC_SAFE=1` active:** when building in a public tree where the private backends are absent, the runtime compiles with `PUBLIC_SAFE=1`. This mode links the public replacements — `iso_public.c` for ISO9660 lookups driven by `PSP_ISO`, `pgf_public.c` for fonts, and the SDL3 audio backend — plus `pgd_unavailable.c`. Disc routes keep working; PGD-protected data is refused in this mode, and the runtime still refuses encrypted `~PSP` executables because decryption happens earlier, in the player/CLI boundary that requires your own key file ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)).
- **Missing ISO or missing extracted assets:** `place_game_here/ISO/<game>.iso` must be present, and `place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted` (or configured `SR_DATAROOT`) must be populated. `SR_DATAROOT` may instead hold the read-only `<archive>.xb` archives; the runtime mounts those directly ([#298](https://github.com/Jstar269/nakagawa-recomp/issues/298)).
- **No late PRX exports / asset lookups fail:** restore the required `place_game_here/EXTRACTED/` layout (decrypted `libfont.prx`, `scePsmf_library.prx`, `scePsmfP_library.prx`).
- **PSP font missing or text not rendering:** Run `python tools/nk_cli.py fonts import <folder>` pointing to your dumped PSP firmware fonts. Verify that `<user data>/fonts/v1/manifest.json` and `jpn0.pgf` exist. See [System fonts](#system-fonts).
- **Clean build omits chunks:** use the unchanged two-process `all` target; do not rewrite it as `all: pipeline compile`.
- **Watchdog fires:** `SR_WATCHDOG_EXIT` counts vblanks since the last newly
  presented frame, not seconds or frame count. The no-frame watchdog is a
  NO-NEW-FLIP observation, not a hang verdict: a legitimately static scene
  (e.g. a save-confirmation modal waiting for input) also stops presenting.
  Classify a firing with the facts the watchdog prints -- `WATCHDOG_DISPLAY`
  outcome counters, the thread wait-state dump, and the `WATCHDOG_MPEG` /
  `WATCHDOG_PSMF` activity -- rather than from the threshold alone. If a title
  waits for a profile/save or cannot open a title-specific cache, inspect the
  local diagnostic log. Do not copy that input or its path into Git.
