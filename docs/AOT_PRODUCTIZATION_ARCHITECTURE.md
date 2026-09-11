# AOT Productization Architecture: End-User Recompilation Without Developer Toolchains

## 1. The Core Problem

In developer environments, static recompilation uses:

~~~text
Game ISO/ELF
  → Python static analyzer (tools/codegen.py)
  → Dynamic C chunks (build/<game>/<game>_recomp_*.c)
  → Host C compiler (GCC 16 / Clang / MSVC) + GNU Make
  → Native executable (build/<game>/<game>.exe)
~~~

For common end users, requiring **Git, Python, MSYS2, GNU Make, or a C compiler** is a critical adoption barrier.
The product vision requires:

~~~text
Nakagawa Program + User's Game ISO → Play
~~~

---

## 2. Evaluation of Architectural Recompilation Options (A through G)

| Option | Architecture Concept | Zero User Prereqs | LLE/Fidelity Preservation | Startup Latency | Distribution Size | Assessment |
| --- | --- | :---: | :---: | :---: | :---: | --- |
| **Option A** | **Bundled Lightweight C Compiler (TCC / MinGW-w64 Clang)** | YES | High (Pure C chunks) | Moderate (10–30s on first run) | Small (+15–30 MB) | **Viable Short-Term Native Route** |
| **Option B** | **Embedded MIPS JIT Engine** | YES | Medium (JIT vs AOT optimization gap) | Instant (<1s) | Minimal (<5 MB) | **Secondary/Fallback Engine** |
| **Option C** | **Build-Time AOT + End-User Asset Binding** | YES | **Maximum (100% genuine AOT)** | **Instant (<1s)** | Moderate (+20–40 MB per title) | **Recommended Primary Architecture** |
| **Option D** | **Pre-Generated Static Stems + Interpreter Floor (#118)** | YES | High (fail-closed interpreter floor) | Instant (<1s) | Minimal (<10 MB) | **Transitional / Development Proving** |
| **Option E** | **Embedded Cranelift / LLVM Backend** | YES | High | Fast (3–8s first run) | Moderate (+35 MB) | **Strategic Long-Term AOT Engine** |
| **Option F** | **Cloud-Based Recompilation Service** | YES | Zero (Severe Privacy/Legal Breach) | Poor (Network transfer of multi-GB ISOs) | Minimal | **REJECTED (Illegal & Hostile to Privacy)** |
| **Option G** | **Clean-Room Standalone Title Releases** | YES | High | Instant | Minimal | **Viable for Specific Public Domain / Homebrew Titles** |

---

## 3. In-Depth Analysis of Viable Options

### Option C: Build-Time AOT Engine + End-User Asset Extraction (Recommended Primary)

In this model, Nakagawa maintainers/packagers compile the verified title executable (or generic recompiled core for supported manifests) into clean-room release packages.

* **How It Works:**
  1. The distributed binary contains the recompiled code structure without proprietary game assets, textures, sounds, or movies.
  2. When the user loads their ISO in `nakagawa_player`, the player validates the disc hash and extracts the required private game assets (`USRDIR/xbdata`, audio files, saves) into the user's local app data directory.
  3. The runtime is launched with `PSP_ISO=<user_iso_path>` and pointers to the extracted assets.
* **Why It Wins:**
  * Zero compilation latency for the user.
  * Zero compiler required on user machine.
  * 100% reproducible optimization (`-O2` runtime, `-O1` recomp chunks).
  * Completely clean legally: no proprietary game bytes are distributed.

### Option A: Bundled Embedded Host Compiler (The "Compile-on-Import" Route)

For titles requiring dynamic or local recompilation from novel ISO revisions:

* A tiny, self-contained C compiler (such as a trimmed LLVM/Clang or TinyCC) is embedded inside Nakagawa's installation directory (`bin/toolchain/`).
* When an un-recompiled ISO is imported, the native core executes code generation and invokes the bundled compiler in the background with truthful progress reporting.
* **Tradeoffs:** Adds 20–40 MB to the installer, but gives total autonomy without installing system-wide SDKs.

### Option E: Embedded Cranelift MIPS-to-Host Code Generator (Long-Term North Star)

* Replace the `MIPS -> C -> Host Machine Code` two-stage pipeline with a direct `MIPS AST -> Cranelift IR -> Host Native Machine Code` in-process compiler.
* **Advantages:**
  * Native execution speed without intermediate C files or external compiler invocations.
  * Sub-second compilation time for entire PSP executables.
  * Cross-architecture support (x86-64, aarch64 for Apple Silicon and Android).

---

## 4. Immediate Architectural Decisions for the ux-investigation Lane

1. **Retain Isolated Child Process Launch Contract:** `NkLaunchSession` handles both pre-compiled executables (Option C) and locally compiled binaries (Option A) identically via environment parameters.
2. **Deterministic Manifest-Driven Metadata:** Title configurations (`assets/titles/*.json`) dictate whether an executable is pre-packaged or locally derived.
3. **Fail-Closed Dispatch Enforcement:** All launch paths enforce `SR_DISPATCH_FATAL=1` to guarantee fidelity and prevent silent emulation hacks.
