# Architecture Overview

High-level map of the PSP static recompiler: what each major area does, how data flows, and where
to start when behavior diverges. The live source, Makefile, and tests are authoritative when this
overview and implementation disagree.

## Pipeline

```text
Private decrypted PSP ELF/PRXs
            │
            ▼
     ┌─────────────┐
     │  prxload.py │  rebase/relocate, build flat guest image
     └──────┬──────┘
            │
            ├───────────────┐
            ▼               ▼
     ┌─────────────┐  ┌─────────────┐
     │ imports.py  │  │ analyze.py  │
     │ NID mapping │  │ functions   │
     └──────┬──────┘  └──────┬──────┘
            │                │
            └───────┬────────┘
                    ▼
             ┌─────────────┐
             │ codegen.py  │  MIPS → generated C
             └──────┬──────┘
                    ▼
             ┌─────────────┐
             │ GNU Make    │  generated C + native runtime
             └──────┬──────┘
                    ▼
             ┌─────────────┐
             │ <game>.exe  │
             └─────────────┘
```

The repository does not contain the retail game executable, ISO, decrypted game PRXs, or private
oracle traces. Those remain local inputs.

## Directory Layout

```text
NakagawaRecomp/
├── tools/              # Python offline compilation, verification, and audit tooling
├── src/
│   ├── rt/             # C native runtime
│   │   ├── gpu_sdl3vk/ # SDL3 + Vulkan backend
│   │   └── ...
│   └── ref/            # C++ reference interpreter / differential-test support
├── assets/vfpu/        # Pinned VFPU lookup-table assets with provenance
├── font/               # Pinned replacement PGF fonts with provenance
├── build/              # Generated build output (Git-ignored)
├── docs/               # Maintained documentation and dated investigation records
├── Makefile            # Build driver
├── hst_manager.ps1     # HST-specific build/run/inspection orchestration
└── README.md           # Project entry point
```

## `tools/` — Offline Compilation

These tools run on the development host before the native compiler.

| Script | Input | Output | Purpose |
| --- | --- | --- | --- |
| `prxload.py` | decrypted ELF/PRX | `*_image.bin` | Rebase modules, apply supported relocations, emit flat guest image |
| `imports.py` | ELF + base | `*_imports.toml` | Resolve PSP import NIDs for HLE/codegen |
| `analyze.py` | ELF | in-memory function map | Discover function boundaries/control-flow metadata |
| `codegen.py` | ELF + analysis/import data | `*_recomp.c`, `*_recomp_N.c`, headers/reports | Translate guest MIPS functions into C |

### Codegen internals

`codegen.py` is the translation core.

- **Function discovery:** consumes `analyze.py` results.
- **Instruction translation:** emits C operating on the shared `CpuState` ABI.
- **Narrow compatibility translations:** address-specific/custom behavior is allowed only when
  evidence justifies it and should be tracked as semantic debt rather than treated as a general
  translation rule.
- **Chunking:** generated functions are split into `<game>_recomp_0.c` through
  `<game>_recomp_N.c`. The count is computed from the discovered functions and
  `FUNCS_PER_CHUNK`; it is not fixed to eight HST chunks.

### Verification tools

| Script | Purpose |
| --- | --- |
| `codegen_gate.py` | Generate/compile translated code using `$CC` (falling back to `gcc`) and compare the pre-HLE execution trace with a supplied oracle |
| `funcdiff_cmp.py` | Compare developer-supplied per-function traces |
| `ppmdiff.py` | Compare framebuffer snapshots, commonly software-vs-Vulkan A/B output |
| `gen_microtest.py` / `microtest_gate.py` | Build targeted instruction/function verification cases when the required external inputs are available |
| `verify_gates.py` | Orchestrate optional codegen/microtest gates and report missing oracle inputs as blocked |

In-repository A/B agreement is evidence of local consistency; it is not by itself an external
proof of PSP correctness.

## `src/rt/` — Native Runtime

The runtime executes generated guest functions and implements the host side of PSP services.

### Core

| File | Purpose |
| --- | --- |
| `recomp.h` / `recomp.c` | Shared `CpuState` ABI, guest-memory access helpers, dispatch support, instrumentation |
| `sched.c` | Cooperative PSP-thread scheduler and wait/lifecycle behavior |
| `sr_coro.c` / `sr_coro.h` | Host coroutine abstraction: Windows fibers on Windows and the POSIX/ucontext path where supported |
| `driver.c` | Runtime entry point, image setup, tracing/termination plumbing |
| `debug.c` / `debug.h` | Centralized debug categories and memory-watch support |
| `perf.c` / `perf.h` | Runtime performance counters/telemetry |

### HLE

| File | Purpose |
| --- | --- |
| `hle.c` | PSP syscall/NID dispatch and a large portion of kernel/user HLE behavior |
| `hle_thread_selftest.c` | Game-input-free Windows harness that executes selected production HLE handlers through registered NIDs against a synthetic scheduler world |
| `audio.c` | PSP audio services through SDL3 host audio |
| `iso.c` / `iso.h` | UMD/ISO filesystem access |
| `pgd.c` | PGD/installed-data handling |
| `mpeg.c` | MPEG/SAS/Atrac-related behavior derived in part from PPSSPP lineage |
| `savedata.c` | Utility savedata mapped to host storage |
| `pgf.c` / `pgf.h` | PGF parsing/rasterization; licensing/provenance is tracked separately as a publication blocker |
| `h264_mf.c` | Windows Media Foundation video-decode integration |
| `h264_null.c` | Host-neutral/null video-decoder path used by portability/test builds |
| `osk_win.c` | Win32 on-screen keyboard integration |
| `gui.c` | Host GUI/input integration and fallback presentation plumbing |

Unknown/unregistered PSP operations are not intentionally converted into fabricated success in a
scheduled game run. Missing behavior should fail visibly so the HLE gap remains observable.

### GE / graphics

| File | Purpose |
| --- | --- |
| `ge.c` | Software GE rasterizer with PPSSPP-derived behavior; dedicated `-O2` build rule |
| `ge_shared.h` | Constants/data shared by renderer paths |
| `gpu_sdl3vk/sdl3vk.c` / `.h` | SDL3/Vulkan initialization, host window/input/presentation |
| `gpu_sdl3vk/ge_gpu.c` / `.h` | Vulkan GE command processing |
| `gpu_sdl3vk/shaders/` | GLSL sources and checked-in embedded shader data |

`SR_GPU_GE=1` selects the Vulkan GE path; `SR_GPU_GE=0` selects the software comparison path.
Neither path should be described as an external PSP oracle.

### VFPU

| File | Purpose |
| --- | --- |
| `vfpu_interp.c` | Single-instruction VFPU interpreter using the pinned lookup tables in `assets/vfpu/` |
| `vfpu_fuzz.c` | Differential harness for translated VFPU behavior versus runtime/reference behavior |

## `src/ref/` — Reference Interpreter

The separate C++ interpreter provides an independent execution implementation for verification
work. It is not linked into the normal game executable.

| File | Purpose |
| --- | --- |
| `cpu.h` | C++ mirror of the guest CPU state needed by the interpreter |
| `interp.cpp` / `interp.h` | Instruction-level reference execution |
| `run_elf.cpp` | Reference runner used by verification workflows |
| `selftest.cpp` | Reference-interpreter/runtime self-tests |

Trace format: `tools/TRACE_FORMAT.md`.

## `CpuState` ABI

`src/rt/recomp.h` defines the load-bearing state shared with generated code. The current structure
contains:

- `r[32]` — MIPS general-purpose registers;
- `hi`, `lo` — integer multiply/divide state;
- `pc` — current guest PC at maintained boundaries;
- `f[32]` / `fi[32]` — FPU register view;
- `fcr31`, `fpcond` — FPU control/condition state;
- `v[128]` / `vi[128]` — physical VFPU register file;
- `vfpuCtrl[16]` — VFPU control/prefix/condition state;
- `status` — modeled COP0 status state;
- `next_pc`, `in_delay_slot` — branch/delay-slot bookkeeping.

There is **no separate `lr` member**. MIPS `$ra` is general register `r[31]`; similarly `$sp` is
`r[29]` and `$gp` is `r[28]`.

Changing this layout requires coordinated updates to every consumer and explicit ABI/offset
verification.

## Build System

### Important Make variables

| Variable | Default | Purpose |
| --- | --- | --- |
| `GAME_NAME` | `mygame` | Build/output identifier |
| `GAME_ELF` | `eboot.elf` | Decrypted ELF path; HST normally supplies its canonical private path through the manager |
| `GAME_BASE` | `0x08804000` | Generic rebased-ELF load base; HST requires `0` |
| `GAME_ENTRY` | `0x08804000` | Generic entry; HST requires `0` and runtime initialization resolves the actual entry |
| `VULKAN_SDK` | empty for direct Make; manager discovers explicit > environment > newest valid `C:/VulkanSDK/<version>` | SDK path used for Vulkan headers/import libraries; the manager validates capability before invoking Make |
| `GAME_EXTRA_ELFS` | empty generically; HST-specific inside its Make block | Additional decrypted modules |

### Two-phase build

The `all` target intentionally invokes Make twice:

1. `pipeline` generates/rebases the image, imports, and generated C;
2. a second `compile` invocation reparses the Makefile after generated chunk files exist.

`CHUNK_OBJS` is based on `$(wildcard ...)` at parse time, so collapsing the process into a single
`all: pipeline compile` dependency pass can omit generated chunks on a clean build.

### Compile flags

The live Makefile currently uses:

- **Generated translation units:** `-O0 -w -fno-var-tracking -ftrack-macro-expansion=0`.
- **General runtime objects:** `$(CFLAGS)`, whose repository default begins with `-O0` plus
  `-fno-strict-aliasing`, include paths, feature defines, and warnings.
- **`ge.c`:** a dedicated `-O2 -fno-math-errno` compile rule for software-rasterizer speed.
- **Portable-core objects:** a separate host-neutral `PORTABLE_CORE_CFLAGS` set, currently `-O0`.

The opt-in guest-PC profiler uses an explicit occupied bit, so guest PC `0x00000000` is a valid
profile key for zero-based images. Its bounded 64-probe lookup reports `lookup_drops` in every
profile dump; a nonzero value means the hotspot ranking is incomplete and must not be treated as
authoritative. `make profiler-selftest` covers the zero-PC and saturated-probe cases without game
inputs.

`RUNTIME_OPT=-O2` selects the separately verified native optimization experiment without changing
generated game code. It is not the default because the current input replay does not hold the live
scene constant across differently paced builds.
Runtime, generated-code, and codegen profile changes have separate content-addressed invalidation
stamps. C objects emit `-MMD -MP` dependency files so transitive headers participate in freshness.
See [`PERFORMANCE.md`](PERFORMANCE.md) for the local O0/O2 and chunk-size measurements.

## Runtime Execution Model

### Dispatch

Generated functions use the shared `CpuState`, direct translated calls where emitted, and runtime
dispatch support for computed/dynamic transfers. When tracing is enabled, maintained boundaries
can be compared with reference/oracle traces.

### HLE boundary

At a PSP import/HLE boundary:

1. the runtime identifies the NID/registered handler;
2. the host implementation executes;
3. the PSP-visible result is returned through `$v0` (`r[2]`);
4. scheduler/HLE-specific state transitions occur according to that operation's semantics.

Do not infer PSP correctness merely because a handler returns zero or because a route advances.
Behavioral side effects, waits, wakeups, callbacks, outputs, and error values are part of the ABI.

### Scheduler / coroutines

`sched.c` models PSP threads cooperatively. Host execution context is provided by `sr_coro` rather
than being intrinsically tied to one platform API. Windows uses fibers; the repository also keeps
a POSIX/ucontext compile path for host-neutral verification/portability work.

Scheduler correctness includes priority selection, lifecycle, waits/timeouts, callback-aware waits,
and wakeup semantics—not only context switching.

## Environment Variables

The runtime has many diagnostic and behavior switches. This table is intentionally a selected
architecture-level subset; `docs/DEBUGGING.md`, `hst_manager.ps1`, and the implementing source are
the maintained references for exact behavior.

| Variable | Values | Purpose |
| --- | --- | --- |
| `SR_GPU_GE` | `0` / `1` | Software vs Vulkan GE path |
| `SR_GPU_LOG` | present/unset | GPU diagnostics where implemented |
| `SR_VIDEO` | e.g. `gdi` | Select host fallback video path |
| `SR_FBSNAP` | positive integer | Rotating PPM snapshot interval |
| `SR_HLELOG` | present/unset | HLE dispatch diagnostics |
| `SR_SYSLOG` | present/unset | System-call diagnostics |
| `SR_THLOG` | present/unset | Scheduler/thread diagnostics |
| `SR_BLOCKLOG` | present/unset | Blocking/basic diagnostic output where consumed |
| `SR_IOLOG` | present/unset | Filesystem/I/O diagnostics |
| `SR_AUDIOLOG` | present/unset | Bounded audio diagnostics |
| `SR_MSGLOG` | present/unset | Bounded message-pipe diagnostics |
| `SR_PLTLOG` | present/unset | PLT/import-resolution diagnostics |
| `PSP_VFPU_TABLES` | path | Override VFPU lookup-table directory |
| `PSP_ISO` | path | Private ISO path where a route consumes it |
| `SR_FSDIR` | path | Host filesystem mapping root |

Many legacy Boolean diagnostics are enabled by **presence**, so setting them to the literal string
`"0"` may still enable them. Remove/unset such variables to disable them. Value-parsed switches
such as `SR_GPU_GE`, numeric settings such as `SR_FBSNAP`, and the `SR_DEBUG` bitmask are separate
cases. The manager profiles clear stale variables before launching and are safer than accumulating
manual environment state.

## Debug Framework

`src/rt/debug.h` / `debug.c` provide the central `SR_DEBUG` bitmask categories:

| Bit | Hex | Category | Description |
| --- | --- | --- | --- |
| 0 | `0x01` | `SR_DBG_MEM` | Memory access/watch diagnostics |
| 1 | `0x02` | `SR_DBG_HLE` | HLE diagnostics |
| 2 | `0x04` | `SR_DBG_SCHED` | Scheduler/thread diagnostics |
| 3 | `0x08` | `SR_DBG_GE` | GE/graphics diagnostics |
| 4 | `0x10` | `SR_DBG_INPUT` | Input diagnostics |
| 5 | `0x20` | `SR_DBG_FS` | Filesystem/I/O diagnostics |
| 6 | `0x40` | `SR_DBG_VIDEO` | Display/framebuffer/vblank diagnostics |
| 7 | `0x80` | `SR_DBG_MISC` | Miscellaneous subsystem diagnostics |

Example:

```powershell
$env:SR_DEBUG = "0x03"  # memory + HLE
```

The memory-watch and crash-reporting facilities are described in `docs/DEBUGGING.md`.

## Where to Look When Something Breaks

| Symptom | Start here |
| --- | --- |
| Pipeline/codegen failure | `tools/prxload.py`, `tools/imports.py`, `tools/analyze.py`, `tools/codegen.py` and the first failing command |
| Native compile/link failure | Makefile output, exact compiler/linker command, UCRT64/SDL3/Vulkan SDK availability |
| Unknown NID | `src/rt/hle.c`, import manifest/audit tools, `SR_HLELOG` diagnostics |
| Translation mismatch | `tools/codegen_gate.py`, `tools/funcdiff_cmp.py`, reference interpreter |
| Renderer mismatch | software/Vulkan A/B snapshots plus `tools/ppmdiff.py`; use an external oracle before claiming PSP correctness |
| VFPU mismatch | `src/rt/vfpu_interp.c`, generated VFPU tests, pinned tables, reference behavior |
| Scheduler deadlock/wait issue | `src/rt/sched.c`, callback/wait tests, `SR_THLOG`/targeted diagnostics |
| Repeated/infinite guest execution | identify owning guest thread and PC first; repair the missing semantic/control-flow cause rather than adding a loop cap |
| No presented frame | inspect GE/presentation diagnostics and the first upstream failure that prevents valid draw/present work |
