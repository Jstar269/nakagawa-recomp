# Gap Analysis: Achieving Authentic Execution from "Program + ISO"

> **Status: CURRENT — maintained LLE gap analysis.** This document distinguishes
> existing bounded ISO helpers from the remaining retail-preparation gaps. Its proposed
> cryptography, middleware, and firmware work is not implemented merely because
> it appears in a resolution column. The current player performs bounded ISO/PARAM.SFO
> inspection, staged XB extraction, and package build/launch for supported inputs. It
> accepts user-supplied decrypted executables and uses the built-in boundary for
> supported encrypted executable and PRX containers with the user's own key file (#295).
> The runtime now also serves guest reads from the
> read-only archive-backed VFS (#298) when `SR_DATAROOT` holds `.xb` archives.

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
inspection, bounded file extraction, and the read-only archive-backed VFS exist.
Supported executable and PRX decryption is implemented; guest middleware and title
acceptance remain partial or open.

| Preparation Surface | Current Manual Requirement | Technical Root Cause | True Low-Level (LLE) Resolution Path | Status |
| :--- | :--- | :--- | :--- | :--- |
| **Main Executable** | A supported encrypted disc executable plus a local key file, a user-supplied decrypted MIPS ELF, or the automatically selected plain `BOOT.BIN` fallback | Supported encrypted containers must be unwrapped and decrypted before the analyzer can consume MIPS ELF32; missing key entries fail closed | The built-in boundary from [#295](https://github.com/Jstar269/nakagawa-recomp/issues/295) handles supported containers without shipping key material; plain and user-decrypted inputs remain accepted | **DECRYPTION BOUNDARY LANDED; TITLE EXECUTION AND ACCEPTANCE REMAIN TITLE-SPECIFIC** |
| **Encrypted PRXs** | Supported encrypted modules selected by the player or required by the title manifest, plus a local key file; user-decrypted required modules are also accepted | Decryption makes selected module bytes available for analysis, but guest translation/startup and middleware support remain separate boundaries | The same built-in boundary ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)) handles supported PRX containers; genuine guest middleware execution remains incomplete | **DECRYPTION BOUNDARY LANDED FOR SUPPORTED CONTAINERS; GUEST MIDDLEWARE EXECUTION PARTIAL** |
| **PSP System Fonts** | Dumped firmware PGFs (`jpn0.pgf`, `ltn0.pgf`) from `flash0:/font/` | Sony PGF format has proprietary metrics; fonts reside in firmware, not on UMD | Honest prerequisite: require user firmware dump for authentic rendering; optional synthetic font provider for developer convenience | **HONEST_PREREQUISITE_REQUIRED** |
| **Video Middleware** | Host-HLE `scePsmfPlayer*` driving it over a bounded PSMF producer (see `docs/LLE_FIDELITY_ARCHITECTURE.md` §3.4) | The host-HLE player works, but the genuine `psmf.prx` / `libpsmfplayer.prx` middleware is not executing yet | Execute original guest `psmf.prx` & `libpsmfplayer.prx`; bridge only the lowest hardware codec boundary (`sceMpeg`) to host decoders | **PARTIAL — CODEC BRIDGE AND PRODUCER LANDED; GUEST MIDDLEWARE EXECUTION OPEN** |
| **Game Assets** | Loose `xbdata_extracted/` (~56k files) or the `.xb` archives | Host filesystem previously expected flat directories | Transparent in-engine block VFS reading `.xb` archive sectors directly, preserving all PSP I/O semantics | **LANDED — READ-ONLY ARCHIVE-BACKED VFS (#298); LOOSE LAYOUT STILL ACCEPTED** |
| **Title Manifest** | Private title-specific manifest | Excluded from public tree due to guest addresses | Generic title-catalog parameterization without hardcoded patches | **GENERIC SUPPORT EXISTS; TITLE ACCEPTANCE NOT RUN** |

---

## 3. Deep Technical Analysis of LLE Gaps

The subsections below describe proposed implementation paths and known blockers. They
are not evidence that the paths have landed in the current player.

### 3.1 Retail EBOOT and PRX Cryptography

* **Input boundary:** `tools/codegen.py` consumes supported MIPS ELF inputs and fails closed on encrypted headers. The player and CLI provide the upstream boundary: issue [#295](https://github.com/Jstar269/nakagawa-recomp/issues/295) added support for encrypted container forms documented in [`docs/SETUP.md`](SETUP.md#built-in-decryption-boundary-issue-295), using a key file supplied locally by the user.
* **Status:** The boundary handles supported encrypted executables and PRX modules; no keys are shipped, and decrypted output stays in user data. Missing key entries and unsupported container forms fail closed. This does not establish support for every PSP encryption form, PGD-protected game data, guest middleware startup, or any specific title's compatibility.

---

### 3.2 Executing Original PRX Middleware (`libfont`, `scePsmf`)

* **Why Wholesale HLE Was Rejected:**
  * In `af5c4f4`, `src/rt/hle.c` intercepted `libfont.prx` and `psmf.prx`, skipped their `module_start` entry points, and wrote `1u` into a title-configured guest word through `sr_title_config_libfont_ready_flag_addr`.
  * This was done because executing `module_start` hung on an unconditional `WaitSema`.
  * Replacing the modules with host HLE bypassed genuine guest state machines, introduced timing discrepancies, and required ongoing title-specific maintenance.
* **Current boundary:** The `libfont.prx` skip is removed for translated entries: `sceKernelStartModule` runs guest startup and its exports become available after startup. When startup is unavailable (an untranslated entry, no recorded entry, or the `SR_REAL_MODULE_START=0` kill switch), the runtime reports `LIBFONT_STARTUP_UNAVAILABLE`, returns `SCE_KERNEL_ERROR_NOTIMP`, and leaves guest readiness unchanged. Legacy manifests using the retired ready-word field fail with `LIBFONT_READY_FLAG_RETIRED`. Real-title startup dependencies still need private-route validation under [#299](https://github.com/Jstar269/nakagawa-recomp/issues/299). PSMF startup remains a separate bypass.

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
  * Direct XB access now has a read-only route: the archive-backed VFS serves
    validated `.xb` members directly (#298) while the loose-content route stays
    available; the current public ISO helper is `src/core/nk_iso.c` and does not
    provide the complete guest VFS.
  * The guest program must observe identical file paths, file sizes, seek offsets, partial reads, and error codes (`SCE_ERROR_ERRNO_FILE_NOT_FOUND`).
  * No game-specific path aliases or asset redirects may leak into generic guest execution.

---

## 4. Summary: The Authentic "Program + ISO" Reality

$$\text{Ideal Target:} \quad \text{Nakagawa} + \text{Game ISO} \implies \text{Play}$$
$$\text{Bounded Route:} \quad \text{Nakagawa} + \text{Game ISO} + \text{supported local key material when needed} \implies \text{title-specific analysis and launch attempt}$$

Authentic PGF typography separately requires user-supplied firmware fonts.

The project can continue automating everything possible (ISO inspection,
transparent VFS mounting, and dynamic module loading) without compromising
fidelity or cutting architectural corners. At this snapshot, the bounded ISO
inspection/file-helper slice and the read-only archive-backed VFS (#298) are
present, and supported executable/PRX decryption is implemented. Guest PRX
middleware execution and title acceptance remain partial/open work.
