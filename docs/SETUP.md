# Build and development setup

Nakagawa Recomp's core build is Windows-only. The dashboard is a separate optional web project.

## Supported development baseline

The supported and tested core development environment is:

- Windows 11 x64. Older or unsupported Windows versions may work, but receive no compatibility guarantee.
- PowerShell 7.6+ (`pwsh`). Windows PowerShell 5.1 is not supported.
- CPython 3.14.x (`>=3.14,<3.15`), with `python` resolving to that feature line.
- Current MSYS2 UCRT64 GCC/G++, GNU Make, SDL3, and Vulkan loader packages.
- A current Vulkan SDK and Vulkan-capable GPU.

The environment doctor is the executable form of this contract:

```powershell
python tools/hst_doctor.py --scope build
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

`glslc` from the Vulkan SDK is only needed when regenerating the checked-in shader headers. LLD, Clang, CMake, Ninja, and Node.js are not required for the core build.

### Runtime DLLs (SDL3.dll & vulkan-1.dll)

Since `hst.exe` is a native 64-bit Windows application, it relies on two dynamic libraries at runtime: `SDL3.dll` and `vulkan-1.dll`. Because binary DLLs are ignored by this repository's `.gitignore` to keep the Git history clean, you must locate or acquire them manually.

#### 1. SDL3.dll

To ensure that your runtime binary matches the compiled headers and libraries exactly:

- **The Recommended Way (from MSYS2):** Since you installed `mingw-w64-ucrt-x86_64-sdl3` in Step 1, the matching 64-bit DLL is already on your system under your MSYS2 directory. Copy it from:
  `C:\msys64\ucrt64\bin\SDL3.dll` (assuming default MSYS2 install path)
  and place it at the **root of the repository**. The build script (`copy_build_assets.ps1`) will automatically detect it and copy it to the build output folder (`build/hst/`) alongside `hst.exe` when linking.
- **Alternative (Official Releases):** You can download official precompiled Windows binaries from the [SDL3 GitHub Releases page](https://github.com/libsdl-org/SDL/releases). Make sure to choose the `SDL3-3.x.x-win32-x64.zip` release (or the equivalent 64-bit developer archive) and copy the `SDL3.dll` out of it.

#### 2. vulkan-1.dll

This is the Vulkan loader library:

- **System Loader:** This file is usually installed system-wide in `C:\Windows\System32\vulkan-1.dll` by your graphics card driver (NVIDIA/AMD/Intel). In most cases, Windows resolves it automatically from your system directory, so you do not need a copy in the repository root.
- **SDK Copy:** If Windows fails to resolve the system loader, or if you want a fully self-contained build folder, copy the loader from your Vulkan SDK directory:
  `C:\VulkanSDK\<version>\Bin\vulkan-1.dll`
  and place it at the **root of the repository**.

#### 3. Verifying Correctness & Compatibility

To ensure your runtime DLLs are compatible and up to date:

- **64-bit (x64) Architecture Check:** Both DLLs must be **64-bit**. If you copy a 32-bit (x86) version of either DLL by mistake, the application will crash immediately on startup with the error code `0xc000007b`.
- **Version/Metadata Verification:**
  To check details, right-click the DLL file in Windows Explorer, select **Properties**, and navigate to the **Details** tab:
  - For `SDL3.dll`: Verify that **Product version** is `3.0.0` or newer.
  - For `vulkan-1.dll`: Verify that the **Copyright** mentions `Khronos Group` and the version matches or exceeds your Vulkan SDK version (e.g. `1.4.x`).

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
`game.iso` links remain a legacy fallback, not a requirement. A source
`PSP/GAME/UCUS98701/EBOOT.PBP` and `DOCUMENT.DAT` are not read by the current
manager, build, or runtime once the canonical inputs above exist; retain them
outside the working set only if you want a private source archive.

Missing decrypted PRXs prevent late-import registration; missing extracted XB
data breaks plain-file asset lookups. `SYSDIR/EBOOT.BIN` supplies the PSP
header/BSS metadata while `place_game_here/EBOOT.elf` remains the flat
translation input. Do not replace the flat input mechanically until the
repacker preserves its current memory layout.

For automated or pre-loaded runs that require installed game data:

- Utility savedata uses the hierarchical
  `memstick/PSP/SAVEDATA/<game><save>/` tree.
- Ordinary guest `sceIoOpen("ms0:...")` calls still use the legacy flat
  `fs/` mapping. Therefore the current PGD route expects
  `fs/ms0__PSP_SAVEDATA_UCUS98701GAMEDATA_GAMEDATA.BDL`.
- The approximately 413 MB `GAMEDATA.BDL` is not needed to compile and is not
  required for a clean first-run path, but current pre-loaded/menu routes that
  read the installed cache need it. If the same file also exists under
  `memstick/`, a hard link can avoid storing the bytes twice. Both paths remain
  private and Git-ignored.

Unifying generic `ms0:` I/O with the savedata storage root is still portability
work; do not remove `fs/` until that runtime change is implemented and the
current menu route is revalidated.

To regenerate the extracted asset tree, fetch libxb locally and run the extractor:

```powershell
git clone https://github.com/kiwi515/libxb.git third_party/libxb
git -C third_party/libxb checkout --detach ce6df78e5ca99241dd2bbbd68ca485e34003d760
python tools/extract_xb.py place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata --output place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted -v
```

The detached commit is the audited libxb 0.2.0 source snapshot. Upstream has no
release/tag, so do not leave this optional checkout tracking `main`; record the
commit above (and verify the 0.2.0 sdist hash in
[`docs/ISSUE196_DIRECT_XB.md`](ISSUE196_DIRECT_XB.md)) when reproducing an
extraction.

`third_party/` and `place_game_here/` are local-only and ignored by Git. If you use `tools/validate_assets.py`, its optional `tools/reference_hashes.json` reference file is also local-only; it is not required by the normal build.

`pspdecrypt` may be used as an optional, user-supplied GPLv3 extraction/decryption helper; it is
not shipped by this repository. The currently available build validates/decrypts the main EBOOT,
but rejects this title's three encrypted `~SCE` library modules, so it does **not** yet make the
workflow ISO-only.

### Dump the required PRXs with PPSSPP

PPSSPP can decrypt and dump the game-supplied PRXs as it loads them. Use only modules produced
from your own legally obtained copy of the game:

1. In a current desktop PPSSPP build, open **Settings > Tools > Developer Tools**. Select the
   **Dump files** tab and enable **PRX**. This is separate from **Dump Decrypted Eboot**.
2. Start Hot Shots Tennis from the same ISO/PBP supplied to this project. Run it at least through
   the title/menu so the game loads its font and PSMF modules. If one of the files below is still
   absent, exercise the title-screen movie path and check the dump folder again.
3. In PPSSPP, use **Settings > System > Show Memory Stick folder**, then open
   `PSP/SYSTEM/DUMP/`. PPSSPP prefixes dumps with the disc ID, so the US build produces files named
   like `UCUS98701_libfont.prx`.
4. Create `place_game_here/EXTRACTED/decrypted/`, copy these three dumps into it, and remove the
   `UCUS98701_` prefix so the final paths are:

   ```text
   place_game_here/EXTRACTED/decrypted/libfont.prx
   place_game_here/EXTRACTED/decrypted/scePsmf_library.prx
   place_game_here/EXTRACTED/decrypted/scePsmfP_library.prx
   ```

5. A valid decrypted module begins with the ELF magic bytes `7F 45 4C 46`; a file beginning with `~SCE` or `~PSP` is still encrypted and will not work as this runtime input.

PPSSPP's current implementation exposes separate EBOOT and PRX dump switches and writes enabled
dumps beneath the emulated Memory Stick's `PSP/SYSTEM/DUMP` directory. It dumps a PRX only when
the game actually loads that module, and it does not overwrite an existing dump. Delete an old
dump first if you need PPSSPP to regenerate it. Preserve the known-good local files once created,
and never copy game or firmware material into Git history.

## 3. Build

From the repository root:

```powershell
.\hst_manager.ps1 -Action BuildFull
```

This runs the complete pipeline and compilation. Generated C is split into a dynamic number of
translation units based on the discovered function count and `FUNCS_PER_CHUNK`, then compiled with
intentionally conservative flags to avoid excessive compiler memory use.

For runtime-only changes:

```powershell
.\hst_manager.ps1 -Action BuildFast
```

For a direct Make build:

```bash
mingw32-make GAME_NAME=hst GAME_ELF=place_game_here/EBOOT.elf GAME_BASE=0 GAME_ENTRY=0 all
```

Direct Make does not perform SDK discovery; export `VULKAN_SDK` or pass it as a Make variable when
using this form. HST requires both address values to be zero. The Makefile's generic defaults are
intentionally not HST defaults.

## 4. Run and test

```powershell
.\hst_manager.ps1 -Action Test
.\hst_manager.ps1 -Action Run
.\hst_manager.ps1 -Action Run -SoftwareRender
.\hst_manager.ps1 -Action Run -NoGui -Duration 30
```

For normal Vulkan runs, an explicit runtime profile can isolate the intended task:

```powershell
.\hst_manager.ps1 -Action Run -Profile Performance # log-free visual/audio smoke test
.\hst_manager.ps1 -Action Run -Profile Benchmark   # 1 Hz telemetry + logs/perf.csv
.\hst_manager.ps1 -Action Run -Profile Benchmark -GuestProfile # plus guest-PC hotspot summary
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

## 5. Optional developer quality tools

The repository includes shared pre-commit/pre-push checks for text/structured-file hygiene,
baseline Ruff correctness, large files, secret detection, and the publication audit:

```powershell
python -m pip install pre-commit
pre-commit install
pre-commit install --hook-type pre-push
pre-commit run --all-files
```

These hooks install their own pinned Ruff and Gitleaks environments. C formatting is defined by
`.clang-format` but is not currently an automatic pre-commit hook. A mypy configuration remains in
`pyproject.toml`, but mypy is **not** a shared gate while the pre-existing Python typing baseline is
being corrected under GitHub issue #105. Do not describe a known-failing type check as a required
contributor hook. These tools are not core runtime dependencies.

### PSP analysis tools (optional, never runtime dependencies)

- [PSPSDK](https://github.com/pspdev/pspsdk) is the preferred open source reference for PSP
  headers, NID names, documented error constants, PRX structure, and PBP utilities. Keep a local
  checkout outside the repository or fetch a pinned revision in a reproducible tooling step.
- [PSPLink USB](https://github.com/pspdev/psplinkusb) is useful only when a development-capable
  physical PSP is available to collect clean-room behavior traces. It is not required to build
  or run the recompiler.
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra) can independently inspect MIPS
  control flow and shared entries when the Python analyzer is ambiguous. Export only scripts,
  address notes, or other redistributable metadata—never a project database containing game
  bytes.

Do not add any of these large checkouts to this repository, and do not make the eventual player
download them. They are development/oracle aids; the release path should remain the recompiler,
its redistributable host dependencies, and user-supplied game input.

## 6. Optional dashboard

The Next.js 16 dashboard is independent of the native build. Use the checked-in npm lockfile:

```powershell
cd interface
npm ci
npm run dev
```

The development server listens on `127.0.0.1:3000`. Keep it local: the dashboard can launch
native tooling and is not designed for an untrusted network. Any unavailable build or download
feature must report that honestly rather than creating a placeholder artifact; see
[interface/README.md](../interface/README.md).

## Troubleshooting

- **Missing Vulkan headers:** pass the correct `-VulkanSdk` path or `VULKAN_SDK=...` Make variable.
- **`SDL3.dll` missing:** ensure the UCRT64 SDL3 `bin` directory is on `PATH`, or place a compatible `SDL3.dll` at the repository root so the manager copies it beside `hst.exe`.
- **No late PRX exports / asset lookups fail:** restore the required `place_game_here/EXTRACTED/` layout.
- **Clean build omits chunks:** use the unchanged two-process `all` target; do not rewrite it as `all: pipeline compile`.
- **Watchdog fires:** `SR_WATCHDOG_EXIT` counts vblanks since the last newly
  presented frame, not seconds or frame count. If the game waits for
  profile/save creation or cannot open `GAMEDATA.BDL`, place the save fixture at
  `fs/ms0__PSP_SAVEDATA_UCUS98701GAMEDATA_GAMEDATA.BDL`. Compare the resulting
  log with the current symptoms in [`ISSUES.md`](../ISSUES.md).
