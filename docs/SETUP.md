# Build and development setup

Windows 11 x64 remains the supported desktop baseline. Linux also has a bounded,
headless showcase route for the runtime. For an ISO/player workflow, start with
[`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md). For development, see
[`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) and
[`LINUX_DEVELOPMENT.md`](LINUX_DEVELOPMENT.md). The native player is the repository's only user interface.

## Linux headless showcase

Linux can build and run the two source-owned showcase packages through the runtime.
The route needs GCC, GNU Make, Python 3.14, CMake, Ninja, `libvulkan-dev`, SDL3 3.4.16,
and PSPDEV v20260601 installed at `/usr/local/pspdev`. CI builds SDL3 from its pinned
3.4.16 release commit and verifies the PSPDEV archive against
[`pspdev.lock.json`](../assets/upstream/pspdev.lock.json).

Run the showcase from the repository root:

```bash
make CC=gcc showcase-linux
```

The smoke selects SDL's dummy video and audio drivers and runs without a desktop or
GPU. This confirms the Linux runtime builds and runs the showcase fixtures; general
consumer ISO compatibility and interactive Linux presentation remain in the works
([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306)).

## Supported development baseline

The supported and tested core development environment is:

- Windows 11 x64. Older or unsupported Windows versions may work, but receive no compatibility guarantee.
- PowerShell 7.4+ (`pwsh`). Windows PowerShell 5.1 is not supported.
- CPython 3.14.x (`>=3.14,<3.15`), with `python` resolving to that feature line.
- Current MSYS2 UCRT64 GCC/G++, GNU Make, SDL3, and Vulkan loader packages. The release
  manifest's `toolchain_policy` records only the floors the code actually needs: a C11 compiler
  (GCC 4.9+), GNU Make 4.0+ (the title link uses `$(file ...)`), SDL3 3.x, a Vulkan SDK for API 1.1+, and Python 3.14. The rolling
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
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-sdl3-ttf mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader
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

### Runtime DLLs (SDL3.dll, SDL3_ttf.dll & vulkan-1.dll)

The native player (`nakagawa_player.exe`) requires `SDL3.dll` and the host Vulkan loader (`vulkan-1.dll`). It also loads `SDL3_ttf.dll` at run time for the readable UI font: `mingw32-make player` stages `SDL3_ttf.dll` and its full runtime dependency closure beside `build\nakagawa_player.exe` (`tools/stage_runtime_dlls.py`, with `SDL3_TTF_DLL` as the override in the same pattern as `SDL3_DLL`), so the same TTF font path is used whether the player is started from Explorer, a plain `cmd.exe` with no MSYS2 on `PATH`, a built title package, or the release layout. In a release archive, the matching `SDL3.dll`, `SDL3_ttf.dll` and its closure are already beside `bin/nakagawa_player.exe`; leave those files in `bin/`. In a source checkout, `tools/copy_build_assets.ps1` copies SDL3 and any imported MinGW runtime DLLs beside the built game executable and generates their third-party notices; the player target stages its own typography runtime the same way. Without `SDL3_ttf.dll` the player still runs, but falls back to the 8x8 SDL bitmap font and logs the failing step once to stderr — the fallback is never silent — and the workspace doctor reports it as `RUNTIME_SDL3_TTF`.

#### 1. SDL3.dll

For a source build, install the MSYS2 UCRT64 SDL3 package described above. Do not copy the DLL into the repository root; the build asset script resolves the toolchain copy and stages it beside the player. For a release package, use the SDL3 DLL already present in `bin/`.

#### 1a. SDL3_ttf.dll (UI typography)

Install the MSYS2 UCRT64 `sdl3-ttf` package (with the `sdl3` package above). `mingw32-make player` then stages `SDL3_ttf.dll` and the mechanically resolved dependency closure (FreeType, HarfBuzz, Graphite2, libpng, zlib, bzip2, Brotli, GLib, libintl, libiconv, PCRE2 and the MinGW runtimes) beside `build\nakagawa_player.exe`, together with their licence texts in `THIRD_PARTY_NOTICES/`. Set `SDL3_TTF_DLL` to an explicit `SDL3_ttf.dll` file to override the toolchain copy, exactly like `SDL3_DLL`. `NK_UI_NO_TTF=1` still forces the bitmap fallback, for example on CI machines without the library.

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
[`ISSUE196_DIRECT_XB.md`](archive/ISSUE196_DIRECT_XB.md); no optional checkout is required.

`third_party/` and `place_game_here/` are local-only and ignored by Git. If you use `tools/validate_assets.py`, its optional `tools/reference_hashes.json` reference file is also local-only; it is not required by the normal build.

### Plain module inputs

Some titles load additional modules at runtime. The runtime accepts only plain (unencrypted)
ELF/PRX files; this repository ships no keys or key material. The [built-in decryption boundary](#built-in-decryption-boundary-issue-295) below can unwrap the executable and required modules of your own copy of the game when you supply your own local key file. For what the player needs
and where your own unencrypted files go, see [`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md). When you already have plain modules from your own
copy of the game, place them at the paths the local manifest expects, for example:

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

On Windows, the normal per-user data directory is `%LOCALAPPDATA%\Nakagawa\data`. The player and the Python tools both use `%LOCALAPPDATA%` first; when it is unset, the player also asks Windows for `FOLDERID_LocalAppData`. Both fall back to `%APPDATA%` and fail with `DATA_DIR_UNAVAILABLE` if neither application-data location resolves. `USERPROFILE` alone is never used as a data root. If the legacy `%USERPROFILE%\Nakagawa\data` exists while the current root does not, the player and Doctor report its exact path; move its contents manually. When a valid plain MIPS ELF32 `EBOOT.elf` is placed in the current folder, the player and CLI select it automatically for analysis ([#428](https://github.com/Jstar269/nakagawa-recomp/pull/428)). If an experimental profile was created while the disc executable was still encrypted (binding no executable), supplying `EBOOT.elf` in this folder is automatically used by `nk_cli build-package` without requiring re-import.

Decrypted guest modules in `<user data>/titles/<DISC_ID>/decrypted/` may be named either after their file name on the disc (for example `psmf.prx`) or after their manifest module name (for example `scePsmf_library.prx`). When a module is encrypted, invalid, or missing, error messages display both names (for example `psmf.prx (scePsmf_library.prx)`). The project ships no keys; the built-in boundary below fills this folder from your own key file when one is present ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)).

### Built-in decryption boundary (issue #295)

The standalone player and `nk_cli` contain a built-in decryption boundary for a disc image you supply. It understands the supported container forms — `~PSP` executables and PRXs (including `~SCE` outer wrappers and PBP `DATA.PSP` entries) plus the gzip-compressed payloads they can carry — and unwraps them into plain MIPS ELF32 images for the analyzer. The player processes its bounded set of eligible module candidates from four `PSP_GAME` directories (at most 32); CLI workflows process the required `guest-prx` modules declared by the title manifest. The decryption engine in `src/core/nk_psp_*` is ported from one GPL upstream, John-K/pspdecrypt (its libkirk code and its PRX decrypter, which itself derives from PPSSPP's PrxDecrypter), with every key removed. Each file's header names its upstream path, commit and notices, and [NOTICE.md](../NOTICE.md) lists the files and licences. The project ships no keys: key material comes only from a key file you supply.

**The boundary never contains key material.** You supply a local-only key file at:

```text
<user data>/keys/psp-keyfile.json
```

or at the path named by the `NAKAGAWA_PSP_KEY_FILE` environment variable. The file is JSON carrying `"format": "nakagawa-psp-keystore-1"` and an `entries` object; each entry is named by the thing it unlocks (for example a `prx.tag.0x........` recipe object, `kirk.cmd1.key`, or `kirk.keyvault.<slot>`) with hexadecimal values only. The KeyStore validates the shape, names, and value lengths before anything is decrypted.

How the boundary behaves:

- With a valid key file, `nk_cli inspect`, `build-package`, `bringup`, and the player's compatibility preflight decrypt the disc executable and the selected or required encrypted PRX modules automatically and continue to the analyzer. The executable lands in `titles/<DISC_ID>/decrypted/EBOOT.elf`; each decrypted module lands in the same folder under its file name on the disc (for example `psmf.prx`), staged to a temporary name and renamed into place so an interrupted run never leaves a half-written module. Decrypted bytes are written only under the private per-user data directory (`titles/<DISC_ID>/decrypted/` and `cache/decrypted/`) — never next to the ISO, and never into this repository.
- A valid user-supplied plain copy always wins: if `EBOOT.elf` or a module is already present in the per-title folder under its disc file name or manifest module name, the boundary never overwrites it and that module is skipped.
- Without a key file, or when an entry is missing, the boundary fails closed and names the exact entry the container needs (for example `MISSING_KEY_ENTRY prx.tag.0x........` and "this executable needs key entry ..."). Each module fails closed on its own: the other modules still decrypt, and the compatibility preflight reports "Guest modules: N of M ready" with the missing entry. The existing guidance to supply decrypted modules at `<user data>/titles/<DISC_ID>/decrypted/` remains, so that route keeps working; a user-supplied `EBOOT.elf` takes precedence over automatic decryption.
- A reported failure belongs to the container type whose shape matched, not to the last type tried: a modified or corrupt container reports `INTEGRITY_CHECK_FAILED` (or `CONTAINER_MALFORMED` for inconsistent fields), `MISSING_KEY_ENTRY` names only an entry the matched type truly needs, and a `~PSP` container whose header matches no supported type fails closed as `CONTAINER_MALFORMED` naming that instead of requesting key material.
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

Authentic in-game typography requires PSP system fonts in Sony's PlayStation Glyph Format (`.pgf`), dumped from the user's own physical PSP console firmware (`flash0:/font/`). The project does not bundle or distribute proprietary firmware fonts.

> [!NOTE]
> Do not confuse system fonts with `libfont.prx`. `libfont.prx` (located under `place_game_here/EXTRACTED/decrypted/libfont.prx` or `<user data>/titles/<DISC_ID>/decrypted/libfont.prx`) is a game-supplied guest middleware PRX executable module implementing the `sceFont` API; it does not contain the actual font glyph outlines. Typography requires the separate `.pgf` font files.

#### What the user supplies

From your own PSP, copy its PGF font files to a folder on a USB drive, a memory stick, or your PC. The import reads every `.pgf` file directly in the folder you choose, whatever its name, and checks each one with the same native reader the player uses. Each accepted file is used for the slot its glyphs cover:

- Japanese: covers kana (U+3042 or U+30A2).
- Korean: covers Hangul (U+AC00 or U+D55C).
- Latin: covers the Latin letters (U+0041 and U+0061) and neither of the above.

A file that covers both kana and Hangul, or neither, names no slot and is skipped. Files over 16 MiB are refused. If two files name the same slot, the import imports neither until you choose one with `--choose <slot>=<file>`; the player asks you to keep one file per slot.

Fonts must come only from your own device. The project provides no download links or third-party repositories. The import makes no network request, uses no key, and does no decryption, and it never uploads anything.

#### Importing fonts

```powershell
python tools/nk_cli.py fonts import <folder>
python tools/nk_cli.py fonts status
python tools/nk_cli.py fonts remove --all
```

Available options:

- `--choose <slot>=<file>`: Pick the file for a slot that several files name (`japanese`, `latin`, or `korean`).
- `--user-data-root <path>`: Override the target per-user data directory (default: `%LOCALAPPDATA%`, then `%APPDATA%`, under `Nakagawa\data` on Windows; `~/Library/Application Support/NakagawaRecomp/data` on macOS; `$XDG_DATA_HOME/nakagawa-recomp` or `~/.local/share/nakagawa-recomp` on other POSIX systems).
- `--json`: Emit a machine-readable report of the outcome for each file, and of each slot's status.

The import copies each chosen file into the per-user cache as that slot's file, and writes `manifest.json` with `schema_version: 2`, an ISO 8601 UTC `import_time`, and for each slot its `slot`, `size`, `sha256`, `source: "user"`, and `reader_version`. The originals are not touched. Re-importing is idempotent, and a slot the new run does not change keeps its entry:

```text
<user data>/fonts/v2/
├── manifest.json
├── nkjpn.pgf   (Japanese slot)
├── nkltn.pgf   (Latin slot)
└── nkkr.pgf    (Korean slot)
```

A slot is served from the first source that checks: your imported font, then the project font at `<project>/font/<slot file>`, then a named refusal, which the game sees as a missing slot. There is no substitution between slots. `fonts remove` deletes only these slot files and their manifest entries. An import from an earlier cache version is not read; run the import again.

> [!NOTE]
> The in-game font path serves the slot files in two ways. The HLE `sceFont` shim reads them directly (#825): a title that uses the HLE font API gets your imported font from the per-user cache first, then the project font, then a named refusal. The read-only `flash0:/font/` device (#808) serves the same files to guest file I/O for titles that load their own font library, but its slot table is still pending the console measurement, so until that lands it lists and opens no slot; such a title does not see the imported font yet. The import, the status command and the player's preflight (#300) list the font as present in both cases.

#### Verification and player status

To verify that system fonts are in place:

1. **CLI:** `python tools/nk_cli.py fonts status` prints one line per slot, such as `Japanese: imported from your PSP.`, `Latin: project font.`, or `Korean: missing. Import a font from your PSP or install the project font.`
2. **Player preflight:** the `SYSTEM_FONTS` check is `OK` when every slot is covered by an imported or a project font. It is `MISSING` when a slot is uncovered, and its message names the slots. It is `INVALID` (issue #313) when the manifest is malformed or a listed file fails its check.
3. **Player setup wizard:** the **Fonts & System** step shows the source of each slot and has **IMPORT FONTS FOLDER** and **REMOVE IMPORTED FONTS** buttons. Import and removal report their outcome on that step.

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

The built package ships its required guest modules inside `<package>/modules/` (copied under their manifest names), and the launcher sets `SR_MODULE_DIR` to `<package>/modules` so launches are self-contained without external dependencies. A run begins at the title's resolved run entry (`run_entry`: a declared `runtime_bindings.fallback_entry`, else `executable.entry`), ensuring that titles requiring a fallback entry point start there across both the native player and CLI launchers. Re-adding an already imported disc in the library preserves its completed staging and extraction state (`player_merge_readded_game`) when the disc ID, version, and image size match. In a checkout without the private PGF/PGD runtime backends, `build-package` builds with the public backends automatically; PGD-protected game data remains unavailable at run time ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). Only an explicit private-backend build (`PUBLIC_SAFE=0`) refuses in such a checkout, with a message naming [#297](https://github.com/Jstar269/nakagawa-recomp/issues/297).

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

When `SR_FPS_CAP` is unset, the runtime caps host presentation to the PSP's
60000/1001 Hz display scanout, so repeated guest framebuffer submissions within one
scanout do no extra GPU/WSI work. The scheduler and PSP vblank continue at that rate,
and scenes below the cap are not delayed. That default applies only to a direct runtime
launch: every launcher exports an explicit cap (the native player and `nk_launch` use
60, the `nk` CLI and `tools/nk_core/launcher.py` use 30). Set `SR_FPS_CAP=0` for
uncapped diagnostics or A/B measurement; a positive value selects that explicit host
presentation cap.

Runtime logs are written under `logs/`. Use `SR_DEBUG=0xFF` for all categories or consult [DEBUGGING.md](DEBUGGING.md) for targeted logging.

Keyboard controls mirror a PSP layout: `X` = Cross/confirm, `Z` = Circle/back, `A` = Square,
`S` = Triangle, `Q`/`W` = L/R, Enter = Start, Shift = Select, and arrow keys = D-pad.
SDL3 gamepads use the south/east/west/north face buttons as Cross/Circle/Square/Triangle.
Short presses are latched until one PSP controller sample consumes them, so normal taps work
even while a frame is slow.

The full `make verify` command needs external trace data that is not in the repository. Missing `CODEGEN_ORACLE`, `MICROTEST_MODULE`, or `MICROTEST_ORACLE` inputs report `NOT_RUN` with a non-zero result. The hardware differential gate is optional: with neither `PSP_HARDWARE_TRACE` nor `LOCAL_COSIM_TRACE` it reports `NOT_RUN` without changing the result, supplying only one fails, and a matching pair reports `STRICT_V2_AGREEMENT` from the traces' own tier metadata, which is not device attestation. Legacy v1 and `PPSSPP_CORROBORATIVE` traces are corroborative only for hardware evidence.

### Public synthetic verification routes

These routes verify the toolchain and the pipeline without any proprietary game
input. Run them from a shell whose `PATH` includes the MSYS2 UCRT64 tools: a
UCRT64 terminal, or PowerShell after `$env:Path = "C:\msys64\ucrt64\bin;$env:Path"`.

```powershell
.\nk_manager.ps1 -Action Test            # selftest gate (make selftest)
mingw32-make player                      # build/nakagawa_player.exe, the native player
mingw32-make production-smoke            # complete two-phase pipeline smoke test
mingw32-make platform-ladder             # relocations, scheduler, scalar FPU, filesystem
mingw32-make cosim-selftest              # differential AOT vs. interpreter cosimulation
mingw32-make showcase showcase-smoke     # build the showcase demos, then run both headlessly
```

- The **differential cosimulation harness** verifies semantic parity between
  AOT-generated code and the fail-closed interpreter floor.
- The **platform ladder** exercises relocations, scheduler threading, scalar FPU,
  and filesystem semantics across synthetic workloads.
- The **production and display smoke fixtures** (`mingw32-make production-smoke`,
  `mingw32-make display-smoke`) test the complete two-phase build pipeline and
  display bring-up without proprietary inputs; `mingw32-make display-smoke-player`
  also launches the fixture through the native player.
- The **source-owned showcase demos** are project-authored PSP programs that
  traverse the whole pipeline into validated packages the player discovers on
  its own ([`SHOWCASE.md`](SHOWCASE.md)).

What hosted CI runs, and what each check proves, is defined in [`CI.md`](CI.md);
the pipeline itself is described in [`ARCHITECTURE.md`](ARCHITECTURE.md).

### Player BUILD PACKAGE prerequisites

**BUILD PACKAGE** in the native player re-runs this toolchain from inside the app, so
it needs all of the following:

- `tools/nk_cli.py` reachable. The player searches, in order: the `NK_INSTALL_ROOT`
  environment variable (a folder containing `tools/`), `<player exe>/tools`,
  `<player exe>/../tools`, `<player exe>/../source/tools` (the v0.0.1 release
  layout), the working directory, and its parent. Running the player from the
  repository root or from `build/` finds it automatically.
- `python`, `gcc`, and `mingw32-make` from either the current environment or the
  player's per-user downloaded build tools. The player changes `PATH` only for
  the build child; it never edits system `PATH` or the registry.
- `powershell.exe` (Windows PowerShell 5.1, built into Windows 10 and 11). The
  asset-copy step and the unpacking of the downloaded Python archive
  (`Expand-Archive`) use it; PowerShell 7 remains the development baseline for
  the other repository scripts.

When a pinned Windows prerequisite is missing, BUILD PACKAGE opens one consent
card showing each component's name, version, source host, download size, and
licence, plus the total. Nothing is downloaded until **DOWNLOAD** is selected;
the answer applies only to that build. If CPython is missing, the native player
first downloads the pinned embeddable archive and checks its HTTPS host, exact
size, and SHA-256 before extracting it. The verified runtime then runs the
Python fetcher for the remaining packages. A progress card shows the current
component and received/total bytes. **CANCEL** removes partial downloads and
staged extraction data. A successful install resumes BUILD PACKAGE
automatically. Named error cards explain offline, redirect, size, hash, and disk
errors and offer a retry.

The pinned candidate download set is recorded in
[`assets/prereq_manifest.json`](../assets/prereq_manifest.json): CPython 3.14.7
and 34 MSYS2 UCRT64 packages (GCC/binutils, make, SDL3 and `SDL3_ttf` with the
readable-font runtime closure — FreeType, HarfBuzz, Graphite2, libpng, bzip2,
Brotli, GLib, and PCRE2 — Vulkan headers/loader, and their runtime dependencies),
totaling 100,665,604 bytes. Package hashes and
sizes come from the signed MSYS2 repository database; the Python hash is from
python.org's release page. The manifest's `source_snapshot_date` dates the MSYS2
package pins only; the locked developer-tool pins carry their own retrieval
date in `assets/pypi_tool_metadata_*.json`. `tools/requirements-lock.txt` contains developer and
build-generation tools; the consumer `build-package` path needs no third-party
Python packages, and `glslc` is only used by opt-in shader regeneration.

Downloaded tools, verified archives, and extracted licence texts live under the
current user's Nakagawa data folder in `prerequisites/`. **Settings → About &
Licenses** lists installed components, versions, and licence identifiers and
opens the notice folder. **Settings → Remove Downloaded Build Tools** removes
that prerequisites folder after confirmation; it does not remove the game
library, ISO files, saves, or built packages. The consent card marks unresolved
`NOASSERTION` licence entries as under review in [#304](https://github.com/Jstar269/nakagawa-recomp/issues/304).

This automatic prerequisite flow currently targets Windows x64 with the
UCRT64 package set. Linux prerequisite installation is in the works
([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306)); the wider
distribution and update contract remains tracked by [#324](https://github.com/Jstar269/nakagawa-recomp/issues/324).

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

Two variables relocate everything these targets touch. `BUILD_ROOT` (default `build`) holds the
default per-title `BUILD_DIR` (`$(BUILD_ROOT)/$(GAME_NAME)`), the shared fixture trees
`clean-fixtures` removes, and the SDL3 discovery cache; `clean-all` empties it. `LOG_DIR` (default
`logs`) holds the ephemeral logs `distclean` and `clean-all` remove; every other log there is kept.
For example, `mingw32-make clean-all BUILD_ROOT=../scratch/build LOG_DIR=../scratch/logs` cleans
only that scratch tree beside the checkout, which is how the Python suite exercises these targets
without touching your real `build/` and `logs/`. An overridden `BUILD_ROOT` must be the checkout's
`build/` tree, a directory beneath it, or a directory outside the checkout; Make refuses the
repository root, an ancestor of it, or any other directory inside the checkout before anything is
created or deleted.

An overridden `BUILD_DIR` (`make clean BUILD_DIR=<x>` deletes `<x>`) gets the same scope rule: it
must be a directory beneath the checkout's `build/` tree or one outside the checkout. `BUILD_DIR=.`,
`BUILD_DIR=src` and the checkout root are refused while Make parses. `clean` and `clean-all` also
refuse a `BUILD_DIR` that is the whole build root, meaning `BUILD_ROOT` or the checkout's `build/` tree
in any spelling (`build`, `build/`, `./build`, an absolute path, backslashes, or another letter case
on Windows), because they delete it wholesale: name one title's directory beneath the root instead, or
run `clean-all` without `BUILD_DIR` to empty `BUILD_ROOT`. The default `BUILD_DIR` derives from
`BUILD_ROOT`, so it needs no separate check. Every Makefile output is derived from `BUILD_ROOT`
(the native test binaries, the player executables, the platform-ladder and display-smoke trees), so
one scratch `BUILD_ROOT` redirects every write a Make run makes.

### Checkout path and `BUILD_DIR`

The repository may be cloned under a path that contains spaces (`C:/path/with spaces/nakagawa`): a
build, `mingw32-make native-core-tests`, `mingw32-make contrib-check` and the Python tooling suite
all work from there, and `tools/test_relocated_clone.py` runs a bounded contributor command set from
a copy of the tracked tree placed under a spaced directory, so the property is exercised rather than
assumed.

Two limits are stated rather than implied, because that check does not reach them:

- **Non-ASCII characters in the checkout path are unverified.** No hosted gate runs from such a path.
  The places measured are the selftest respawn sites named here, and they were measured locally, not
  by a hosted gate.
- **The selftest respawn sites quote their command line only recently.** `src/rt/hle_thread_selftest.c`,
  `src/rt/cpu_lle_selftest.c` and `src/rt/dispatch_isolation_selftest.c` start a second copy of their
  own binary, and they used the CRT's `_spawnl`/`_spawnv`, which joins its arguments without quoting:
  a spaced self path was re-parsed as extra arguments, the mode flag never arrived, and the child ran
  the whole suite instead of the requested mode. The HLE exit-game site was corrected in PR #680 and
  the CPU-LLE and dispatch-isolation sites in PR #681, which also changed all three to decode
  `argv[0]` in the active code page rather than assuming UTF-8, since that is the encoding the CRT
  produced it in. [Issue #667](https://github.com/Jstar269/nakagawa-recomp/issues/667) tracks the
  remaining path handling.

`BUILD_DIR` is a narrower contract. GNU Make splits a target or prerequisite name on whitespace, so a
`BUILD_DIR` containing a space cannot be named by Make at all: the name silently becomes several
targets, which previously let `make clean BUILD_DIR=...` remove the first fragment — a directory the
caller never named — and left a `spaces/` directory tree in the checkout root. The Makefile now
refuses such a `BUILD_DIR` before any recipe runs and names the boundary:

```text
BUILD_DIR 'C:/path/with spaces/build' contains a space, which GNU Make cannot represent
in a target or prerequisite name. Use a relative BUILD_DIR under the repository
root (the default `build/<game>`), or set NK_BUILD_ROOT to a folder without spaces
so tools/title_codegen_plan.py can pick a Make-safe build root (issue #296).
```

The default relative `BUILD_DIR` carries no space, so an ordinary checkout is unaffected. Every route
that accepts an operator-chosen output directory already resolves a Make-safe root through
`NK_BUILD_ROOT` or the 8.3 short name before it reaches Make, or stops with `PACKAGE_UNSUPPORTED_PATH`.

## 5. Optional developer quality tools

The repository includes shared pre-commit/pre-push checks for text/structured-file hygiene,
baseline Ruff correctness, large files, secret detection, and the publication audit:

```powershell
python -m pip install pre-commit
python -m pre_commit install
python -m pre_commit run --all-files
```

`install` sets up both the commit and the push hook (`default_install_hook_types` in
`.pre-commit-config.yaml`). The publication audit runs at both stages on purpose: the push stage
audits the index, not each pushed commit, and a pushed branch is public, so only the commit stage
checks every commit before it can leave the machine.

Invoked as `python -m pre_commit` rather than the bare `pre-commit` console script: pip
installs that script into a user `Scripts/` directory that is frequently absent from `PATH` on
Windows, so the bare form fails with "command not found" immediately after a successful install.
The module form works regardless of `PATH`.

These hooks install their own pinned Ruff and Betterleaks environments. The
Betterleaks hook runs at the commit stage in directory mode over the staged files, which works the
same on Windows and Linux; it is not a pre-push hook, because pre-commit passes no file names at
that stage and directory mode would then scan the whole working tree, including the private input
directories (the pushed history is scanned in hosted CI); its upstream git mode sets `GIT_CONFIG_GLOBAL=NUL`,
which Git for Windows rejects, so that mode scanned nothing on Windows. C formatting is defined by
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

### Optional local symbol reference (never track it)

`nk_manager.ps1 -Action FindSymbol` can search an OpenGrip-style `functions.csv` when a
contributor keeps that reference data locally. This is an optional reverse-engineering aid; it is
not required to build or run Nakagawa Recomp. The manager checks these locations in order:

1. `docs/opengrip_ref/functions.csv`
2. `OpenGrip_For_Inspiration/functions.csv`

Both parent directories are ignored by the repository. **Keep the CSV and any associated OpenGrip
checkout, decompiler export, annotations, or game-derived material untracked.** Place or link an
authorized local `functions.csv` at either supported path: the first is preferable when only the
CSV is needed, the second supports a complete local inspiration/reference checkout. Verify that Git
excludes the selected path before using it:

```bash
git check-ignore -v docs/opengrip_ref/functions.csv
# or
git check-ignore -v OpenGrip_For_Inspiration/functions.csv
```

Then run a lookup from the repository root:

```powershell
.\nk_manager.ps1 -Action FindSymbol -FindName Camera_Update
.\nk_manager.ps1 -Action FindSymbol -FindName 47054
```

The command performs a text search and prints at most 20 matching CSV rows. It does not download,
generate, or validate the reference data.

Use only reference material that you are authorized to possess. Do not copy a third-party
repository, raw decompiler output, proprietary game bytes, private symbols, or local-path-bearing
exports into Git history merely to enable the lookup command. Facts learned from a local symbol
reference may be documented when they are independently supportable and do not reproduce protected
implementation or private game data; keep the local CSV itself and raw reverse-engineering exports
outside the published repository.

## Troubleshooting

- **Preflight diagnostics:** run `.\nk.ps1 Doctor -TitleManifest C:\path\to\manifest.json -GameName game` (or `python tools/nk_doctor.py --title-manifest C:\path\to\manifest.json --game-name game`) to validate the toolchain, build dependencies, and the selected title's local inputs. Without a title selection, Doctor uses the public synthetic manifest.
- **Missing Vulkan headers:** pass the correct `-VulkanSdk` path or `VULKAN_SDK=...` Make variable.
- **`SDL3.dll` missing:** for the release package, keep `bin/SDL3.dll` beside `bin/nakagawa_player.exe`; for a source build, install the MSYS2 UCRT64 SDL3 package so `tools/copy_build_assets.ps1` stages it beside `build/nakagawa_player.exe`.
- **Retro 8x8 bitmap font in the player UI:** the readable-font path needs `SDL3_ttf.dll` and its dependency closure beside `nakagawa_player.exe` (or on `PATH`). Install the MSYS2 UCRT64 `sdl3-ttf` package and re-run `mingw32-make player`, which stages the whole closure and its licence notices; the workspace doctor reports `RUNTIME_SDL3_TTF`/`RUNTIME_SDL3_TTF_CLOSURE`, and the player itself logs the failing step once to stderr when it falls back. `NK_UI_NO_TTF=1` forces the bitmap fallback on purpose.
- **`PUBLIC_SAFE=1` active:** when building in a public tree where the private backends are absent, the runtime compiles with `PUBLIC_SAFE=1`. This mode links the public replacements — `iso_public.c` for ISO9660 lookups driven by `PSP_ISO`, `pgf_public.c` for fonts, and the SDL3 audio backend — plus `pgd_unavailable.c`. Disc routes keep working; PGD-protected data is refused in this mode, and the runtime still refuses encrypted `~PSP` executables because decryption happens earlier, in the player/CLI boundary that requires your own key file ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)).
- **Missing ISO or missing extracted assets:** `place_game_here/ISO/<game>.iso` must be present, and `place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted` (or configured `SR_DATAROOT`) must be populated. `SR_DATAROOT` may instead hold the read-only `<archive>.xb` archives; the runtime mounts those directly ([#298](https://github.com/Jstar269/nakagawa-recomp/issues/298)).
- **No late PRX exports / asset lookups fail:** restore the required `place_game_here/EXTRACTED/` layout (decrypted `libfont.prx`, `scePsmf_library.prx`, `scePsmfP_library.prx`).
- **PSP font missing or text not rendering:** Run `python tools/nk_cli.py fonts status` to see each slot's source, and `python tools/nk_cli.py fonts import <folder>` for any slot that is missing. Your imported fonts are in `<user data>/fonts/v2/` with a `manifest.json`. See [System fonts](#system-fonts).
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
