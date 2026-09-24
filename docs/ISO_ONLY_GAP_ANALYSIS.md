# Gap Analysis: Achieving Authentic Execution from "Program + ISO"

> **Status: CURRENT — maintained LLE gap analysis.** This document distinguishes
> existing bounded ISO helpers from the absent retail-preparation path. Its proposed
> cryptography, middleware, VFS, and firmware work is not implemented merely because
> it appears in a resolution column. The current player performs bounded ISO/PARAM.SFO
> inspection plus staged XB extraction for supported wizard inputs; it does not
> decrypt retail containers or serve guest reads from archives at runtime.

## 1. Architectural Correction & Superseded Recommendations

> [!IMPORTANT]
> **SUPERSEDED ARCHITECTURAL NOTE:**
> An earlier iteration of this document proposed wholesale High-Level Emulation (HLE) bridges for `libfont.prx`, `scePsmf_library.prx`, and `scePsmfP_library.prx`, as well as replacing the Sony PGF font engine with `stb_truetype.h`.
>
> **Those recommendations are formally superseded and rejected as permanent architectural solutions.**
>
> Under the project's guiding doctrine:
> $$\text{CORRECTNESS / FIDELITY} > \text{ARCHITECTURAL CLEANLINESS} > \text{GENERALITY} > \text{SHORT-TERM EASE}$$
> Nakagawa Recomp prioritizes **genuine guest execution** over convenience. Bypassing authentic guest middleware, hardcoding memory flags, and substituting TrueType rasterization introduces subtle visual defects, UI text clipping, and architectural fragmentation.

---

## 2. Updated Gap Analysis Matrix: True LLE Resolution Path

The matrix is a decision and gap record. In the current public source, ISO/PARAM.SFO
inspection and bounded file extraction exist; encrypted retail preparation, module
decryption, complete archive mounting, and title acceptance remain open.

| Preparation Surface | Current Manual Requirement | Technical Root Cause | True Low-Level (LLE) Resolution Path | Status |
| :--- | :--- | :--- | :--- | :--- |
| **Main Executable** | Decrypted flat MIPS ELF (`place_game_here/EBOOT.elf`) | `EBOOT.BIN` is encrypted with Kirk tag `0x08000000` | Lawful decryption boundary: an open maintainer legal decision ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)); no design is adopted. Public builds accept a plain executable (an unencrypted `BOOT.BIN` on disc is selected automatically) | **OPEN — MAINTAINER LEGAL DECISION** |
| **Encrypted PRXs** | Decrypted `libfont.prx`, `scePsmf_library.prx`, `scePsmfP_library.prx` | Modules are encrypted `~PSP`/`~SCE` containers; `module_start` previously hung on `WaitSema` | Same decryption boundary as the main executable ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)); fix kernel semaphore/scheduler contracts to execute original `module_start` | **REQUIRES_NEW_IMPLEMENTATION** (Kernel synchronization) |
| **PSP System Fonts** | Dumped firmware PGFs (`jpn0.pgf`, `ltn0.pgf`) from `flash0:/font/` | Sony PGF format has proprietary metrics; fonts reside in firmware, not on UMD | Honest prerequisite: require user firmware dump for authentic rendering; optional synthetic font provider for developer convenience | **HONEST_PREREQUISITE_REQUIRED** |
| **Video Middleware** | Host-HLE `scePsmfPlayer*` driving it over a bounded PSMF producer (see `docs/LLE_FIDELITY_ARCHITECTURE.md` §3.4) | The host-HLE player works, but the genuine `psmf.prx` / `libpsmfplayer.prx` middleware is not executing yet | Execute original guest `psmf.prx` & `libpsmfplayer.prx`; bridge only the lowest hardware codec boundary (`sceMpeg`) to host decoders | **PARTIAL — CODEC BRIDGE AND PRODUCER LANDED; GUEST MIDDLEWARE EXECUTION OPEN** |
| **Game Assets** | Unpacked `xbdata_extracted/` (~56k loose files) | Host filesystem previously expected flat directories | Transparent in-engine block VFS reading `.xb` archive sectors directly, preserving all PSP I/O semantics | **TARGET — NOT CONNECTED** |
| **Title Manifest** | Private title-specific manifest | Excluded from public tree due to guest addresses | Generic title-catalog parameterization without hardcoded patches | **GENERIC SUPPORT EXISTS; TITLE ACCEPTANCE NOT RUN** |

---

## 3. Deep Technical Analysis of LLE Gaps

The subsections below describe proposed implementation paths and known blockers. They
are not evidence that the paths have landed in the current player.

### 3.1 Retail EBOOT and PRX Cryptography

* **The Blocker:** Physical UMDs and PSN packages store executables in `~SCE` and `~PSP` encrypted containers. `tools/codegen.py` disassembles standard MIPS ELF sections and fails closed on encrypted headers.
* **Status:** Whether Nakagawa offers any decryption capability, and in what form, is an open
  maintainer legal decision ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)). This document proposes no decryption or
  key-handling design. The public repository never contains keys, and public builds accept only
  plain (unencrypted) executables and modules.

---

### 3.2 Executing Original PRX Middleware (`libfont`, `scePsmf`)

* **Why Wholesale HLE Was Rejected:**
  * In `af5c4f4`, `src/rt/hle.c` intercepted `libfont.prx` and `psmf.prx`, skipped their `module_start` entry points, and wrote `1u` into a hardcoded guest memory address (`sr_title_config_libfont_ready_flag_addr`).
  * This was done because executing `module_start` hung on an unconditional `WaitSema`.
  * Replacing the modules with host HLE bypassed genuine guest state machines, introduced timing discrepancies, and required ongoing title-specific maintenance.
* **The LLE Solution:**
  1. **Identify the Kernel Root Cause:** The hang in `f_32200000` (`libfont` `module_start`) occurred because the semaphore was initialized with a count of 0, and the expected signaling thread was either delayed or unmapped.
  2. **Harden the Scheduler & Semaphores:** Audit `src/rt/sched.c` to ensure exact PSP kernel semantics for `sceKernelCreateSema`, `sceKernelWaitSema`, and thread context switching.
  3. **Execute Guest Code:** Allow `libfont.prx`, `scePsmf_library.prx`, and `scePsmfP_library.prx` to execute their genuine `module_start` routines.
  4. **Purge Title Pokes:** Remove `sr_title_config_libfont_ready_flag_addr` and all title-specific flag writes from `src/rt/hle.c`.

---

### 3.3 Font Subsystem: PGF Fidelity vs. TrueType Approximation

* **Why TrueType (`stb_truetype`) Substitution Was Rejected:**
  * TrueType font metrics (advance width, ascent, descent, kerning pairs) differ from Sony's proprietary PGF rasterizer.
  * In fixed-width game menus, character selection screens, and HUD elements, TTF fonts cause text overflow, improper line wrapping, and clipping.
  * PGF supports customized subpixel baseline alignment and embedded shadow offsets that standard TTF engines do not emulate accurately.
* **The Authentic PGF Path:**
  * The game calls `libfont.prx` to load PGF fonts.
  * The system fonts (`jpn0.pgf`, `ltn0.pgf`) reside in PSP firmware `flash0:/font/`, not on the game disc.
  * **Honest Prerequisite Policy:** We state truthfully that 100% authentic typography requires a user-supplied firmware font dump.
  * Nakagawa may offer a clearly labeled, non-authentic fallback font provider for developers without firmware files, but it will never be masqueraded as an authentic LLE replacement.

---

### 3.4 PSMF Video Subsystem: Downward Dependency Boundary

* **Preserving Original Middleware:** `scePsmf_library.prx` and `scePsmfP_library.prx` contain the authentic demuxing, timestamp synchronization, and ring-buffer management logic.
* **The Proper Hardware Boundary:**
  * Guest PSMF code delegates low-level bitstream decoding to `sceMpeg` / `sceVideocodec`.
  * On real hardware, this is processed by the Media Engine (ME).
  * In Nakagawa, the HLE boundary is established at `sceMpeg`:
    * Guest PRXs demux the streams and manage buffers.
    * Nakagawa takes the elementary AVC/H.264 video NAL units from `sceMpeg` and dispatches them to host hardware decoders (Vulkan Video / Media Foundation).
    * Audio NAL units (ATRAC3plus) are decoded via standalone host audio routines and written back into guest memory buffers.

---

### 3.5 XB Archive & Filesystem Invariants

* **Preserving PSP-Visible Filesystem Invariants:**
  * Direct XB access is a future target for a transparent block driver; the current
    public ISO helper is `src/core/nk_iso.c` and does not provide the complete guest VFS.
  * The guest program must observe identical file paths, file sizes, seek offsets, partial reads, and error codes (`SCE_ERROR_ERRNO_FILE_NOT_FOUND`).
  * No game-specific path aliases or asset redirects may leak into generic guest execution.

---

## 4. Summary: The Authentic "Program + ISO" Reality

$$\text{Ideal Target:} \quad \text{Nakagawa} + \text{Game ISO} \implies \text{Play}$$
$$\text{Authentic Reality:} \quad \text{Nakagawa} + \text{Game ISO} + \text{User-supplied fonts} + \text{Plain executables} \implies \text{Play}$$

The project can continue automating everything possible (ISO inspection,
transparent VFS mounting, and dynamic module loading) without compromising
fidelity or cutting architectural corners. At this snapshot, only the bounded ISO
inspection/file-helper slice is present; the other items remain proposed work.
