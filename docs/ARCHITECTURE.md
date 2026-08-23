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

## PSP core, title profile, and backend boundary

Wave 1 introduces a narrow, versioned contract without changing the HST production path:

```text
validated ProgramImage (tools/prxload.py)
        -> canonical CFG / ownership observation (tools/analyze.py)
        -> psp-core-v1 semantic capabilities
        -> title profile (boot/resources/explicit HLE declarations/input labels)
        -> host backend contracts and optional enhancements
```

The core contract owns Allegrex/VFPU semantics, guest memory, scheduling,
interrupts/callbacks, generic HLE, GE/display/audio semantics, I/O capability
interfaces, backend contracts, and evidence schemas. A title profile may select
boot policy, public resource locators, input labels, and explicit HLE capability
dispositions. Unknown capabilities fail closed; a profile cannot add an implicit
PSP-semantic replacement, and enhancements are disabled by default in the public
profile-zero contract. HST remains an existing manager/build profile and is not
switched to the new adapter wholesale in this wave.

### ProgramImage v1 and CFG ownership observation v1

**Scope: offline and test-only.** Neither `ProgramImage` nor `CanonicalCfgState` is
reachable from the production pipeline. Nothing in `tools/codegen.py`,
`tools/imports.py`, the `Makefile`, or `hst_manager.ps1` imports or constructs
either type; their only consumers are `tools/prxload.py`, `tools/analyze.py`, and
their unit tests. That is a checkable property, not an intention, and it is the
reason this wave makes no production-wiring claim.

**Precondition for any later production wiring.** Before either type may replace a
production path, it must first be shown *equivalent* to the path it replaces on
real inputs -- not merely self-consistent. `cfg_compatibility_findings()` exists
for exactly this: it reports differences against a legacy entry set and
deliberately does not pick a winner. Wiring either type in without that
equivalence evidence would convert an observation tool into an unverified
reimplementation of the analysis the pipeline already depends on.

`tools.prxload.load_program_image()` is the read-only Wave-1 adapter. It validates the
ELF32 envelope, checked load/file/guest spans, permissions, zero-fill extents, entry,
imports/exports, module metadata, and relocation records before allocating a flat image.
The immutable object carries source name/size/SHA-256, fixed-width guest spans, and
`bytes` payloads; `canonical_program_image_json()` serializes metadata deterministically
and omits raw payload bytes. It deliberately does not apply relocations or replace the
legacy `Prx` loader, so the current HST path remains authoritative while synthetic tests
compare both representations.

`tools.analyze.canonical_cfg_report()` emits schema version 1 as an observation-only
ownership report. Its instruction rows carry address, raw word, opcode identity,
delay-slot attachment, branch-likely annulment, owners and reasons. Edge rows distinguish
direct branches/jumps, calls, tail transfers, fallthrough, delay slots, and unresolved
computed transfers. The report also records interior entries, continuations, ownership
conflicts, jump-table candidates, data spans, padding, unowned executable words, partial/unreadable executable
spans, and explicitly unmapped entry candidates. `verify_canonical_cfg_report()` checks coverage and
structural consistency; `cfg_compatibility_findings()` reports differences from a legacy
entry set without silently selecting a winner. `canonical_cfg_json()` is stable for
fixtures and build-cache comparisons. Neither report is an optimizing IR or a production
HST switch.

`assets/titles/synthetic.json` carries the source-owned `psp-core-v1` /
`profile-zero-v1` contract and its acceptance scaffold. Its source program and
build path point to `fixtures/pspdev_phase5`; the acceptance cases deliberately
separate `SOURCE_SHAPE`, `PRODUCTION_DISPATCH`, and `PRODUCTION_HELPER` evidence.
The scaffold is not a claim that the complete profile-zero route is runnable yet.
Generated output from a source-owned profile may be public in principle, but retail
or private-input-derived AOT remains local, ignored, and outside publication.

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

### Source-owned production smoke

`mingw32-make production-smoke` generates a deterministic PSP-shaped ELF/PRX fixture under
`build/production-smoke/`, then enters the ordinary two-phase `all` target with `PUBLIC_SAFE=1`.
The fixture is a recipe in [`fixtures/production_smoke/`](../fixtures/production_smoke/); the PRX,
`~PSP` header, relocated image, generated C, objects, link map, executable, and run logs remain
ignored build outputs.

This gate covers two load segments, PSP-header BSS recovery, type-A relocation, import discovery,
entry/helper analysis, multiple generated chunks, the complete public-safe production link, the
real driver and registration table, scheduler startup, real NID dispatch in `hle.c`, and a checked
guest-memory sentinel. It is useful before bringing up another title because it catches generic
pipeline and composition failures without requiring an ISO: dropped production objects, stale or
missing chunks, entry discovery regressions, bad relocations/imports, broken scheduler startup,
guest-to-HLE dispatch failures, and public-safe link drift.

It does **not** establish commercial-title compatibility or legality, PSP timing, rendering or
audio correctness, physical UMD behavior, or title-specific runtime bindings. Those remain separate
private-title, visual/audio, and hardware evidence domains.

### Compile flags

The live Makefile currently uses:

- **Generated translation units:** `-O1 -w -fno-var-tracking -ftrack-macro-expansion=0` by default
  for HST based on measured and qualified acceptance; `-O0` remains the conservative default for
  generic/unqualified titles.
- **General runtime objects:** `$(CFLAGS)`, whose default begins with `-O2` for HST and `-O0` for
  generic titles, plus `-fno-strict-aliasing`, include paths, feature defines, and warnings.
- **`ge.c`:** a dedicated `-O2 -fno-math-errno` compile rule for software-rasterizer speed.
- **Portable-core objects:** a separate host-neutral `PORTABLE_CORE_CFLAGS` set, currently `-O0`.

The opt-in guest-PC profiler uses an explicit occupied bit, so guest PC `0x00000000` is a valid
profile key for zero-based images. Its bounded 64-probe lookup reports `lookup_drops` in every
profile dump; a nonzero value means the hotspot ranking is incomplete and must not be treated as
authoritative. `make profiler-selftest` covers the zero-PC and saturated-probe cases without game
inputs.

HST defaults to `RUNTIME_OPT=-O2` and `RECOMP_OPT=-O1`, while generic/unqualified titles remain
conservative `-O0/-O0`. Explicit overrides (e.g. `RUNTIME_OPT=-O0 RECOMP_OPT=-O0`) remain fully
supported on both direct Make and `hst_manager.ps1`. Generated `-O2` is not being adopted; `-O1`'s
measured build cost is higher but acceptable for HST.
Runtime, generated-code, and codegen profile changes have separate content-addressed invalidation
stamps. C objects emit `-MMD -MP` dependency files so transitive headers participate in freshness.
The [`Makefile`](../Makefile) is the source of truth for the current optimization split and
generated chunk policy.

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

### Clocks

`sched.c` owns one authoritative monotonic microsecond timeline (`s_vtime_us`). Every guest-visible
time value derives from it; reading a time API never advances it. The clock advances only at
scheduler/emulation progression boundaries (`sr_hle_advance_time`, yield/idle steps, vblank source
delivery).

Clock ownership:

- **System time** — `sceKernelGetSystemTime[Low/Wide]` and `sceKernelLibcClock` read `s_vtime_us`
  directly; the libc clock is elapsed guest time, not Unix time.
- **RTC calendar** — `sceRtcGetCurrentTick`/current-clock map the same timeline through a one-time
  epoch offset (`s_rtc_epoch_tick`), sampled from the host wall clock at first RTC use and anchored
  at guest time zero. After that, RTC reads are pure guest-time arithmetic at the same 1 us/us rate
  as system time. Host wall time never enters an ordinary read path.
- **Scheduler waits** — delay deadlines, timeout deadlines, and remaining-time computations all use
  `s_vtime_us` via the shared `sched_vtime_refresh`/`sched_vtime_deadline_after`/
  `sched_block_on_timeout` plumbing. No wait object or interrupt/precedence semantics are duplicated
  in the clock layer.
- **Display domain** — VCOUNT/HCOUNT/VBLANK phase are a separate display timeline derived from
  `s_vtime_us` through the rational 60000/1001 Hz model (286 scanlines/frame). Display reads never
  deliver vblanks or move the counters. Guest-visible VCOUNT is modelled as an interrupt-gated
  display-source counter. It advances only when the scheduler observes and latches an elapsed source
  period, so it is not a strictly free-running register and is not a count of serviced episodes:
  - **CPU interrupts enabled.** The scheduler source latch
    (`scheduler_latch_due_events` -> `sr_display_advance_vcount`) advances VCOUNT by the number of
    elapsed display periods even when VBLANK *service* is starved, while the serviced episode
    (`deliver_vblank` -> `sr_vblank_tick`) stays coalesced to one and performs
    framebuffer/interrupt/callback work without re-incrementing VCOUNT.
  - **CPU interrupts masked** (`sceKernelCpuSuspendIntr`). VCOUNT stops, and VBLANK delivery stops
    with it; elapsed periods are consumed into the single coalesced pending bit rather than replayed.
    Resume delivers exactly one episode and credits VCOUNT **exactly one** — never `N`, and nothing
    at all when no period became pending.
  - **Clearing the interrupt bit is itself a display-timeline boundary.** Source periods are
    discovered lazily at scheduler boundaries, so `sched_suspend_interrupts()` consumes everything
    already due *before* clearing the bit. Without that step a period that elapsed with interrupts
    enabled stays undiscovered until some later latch, and the most frequent later latch is the one
    `sched_resume_interrupts()` performs before restoring the bit — which would classify it as masked
    and drop it. This is a discovery-time question, not a residency one: private route measurement
    found every dropped period had a boundary predating the mask that later discovered it, while the
    mask itself was held for a negligible fraction of wall time. Per-title rate figures are run
    evidence and belong with the run that produced them, not here.

  The masked-window behavior is `HARDWARE_MEASURED`. The original #88 probe found system time
  advancing while VCOUNT and VBLANK handler calls stayed frozen, followed by one coalesced delivery
  on resume, but it never sampled VCOUNT immediately after `CpuResumeIntr`. The source-owned
  `display-mask-vcount` probe (PSP-3001 / 6.61-ARK, 12 trials at each of 4 / 16.7 / 30 / 50 ms) took
  that sample and settled it: a mask crossing no source period credits `+0`, and a mask crossing one
  or more credits `+1` — measured across durations from 0.24 to 3.00 display periods, which crossed
  0, 1, 1 and 2 source boundaries respectively. No trial showed an N-period catch-up. Guest-visible
  VCOUNT is therefore a count of *delivered* VBLANKs, and the observed behavior is consistent with a
  single coalesced pending VBLANK delivery. The probe observes the `+0`/`+1` result, not the
  interrupt controller's internal state, so the coalescing is the model that fits the measurement
  rather than a claim about hardware internals.

  **The display source and the delivered counter are different quantities.** The same probe measured
  `sceDisplayGetAccumulatedHcount` running straight through every mask at the full display rate
  (+69 scanlines over 4 ms, +857 over 50 ms, matching 286 lines per period), so the display
  controller never stops — only the interrupt-gated counter the guest reads does. That probe also
  calibrated the device's own period at 16 682 850 ns (59.9418 Hz), which is 0.003% from the
  60000/1001 model the runtime uses; the rate is now measured rather than assumed. The
  enabled/service-starved multi-period behavior remains `CORROBORATIVE_ONLY`, and the checked-in
  production-path regression is `HOST_TESTED`.

  A companion probe, `display-ge-mask`, established that the GE is a separate hardware domain: a
  stall-gated list released *while interrupts were masked* completed 1 MiB of block transfers in
  12/12 trials at the same speed as with interrupts enabled, while its interrupt-context finish
  handler stayed pending until resume (0/12 during, 12/12 immediately after). The CPU interrupt mask
  blocks interrupt delivery; it does not stop GE execution, and a guest polling GE-written memory
  under a mask is supposed to observe progress.

  Controller sample timestamps and audio pacing use the same vblank counter,
  matching the PSP's vblank-unit pad timestamps.
- **libc time/gettimeofday** — seconds/usec since the standard Unix epoch, converted from the RTC
  tick. The PSP timezone is a console setting (`s_psp_timezone_minutes`), not the host process
  timezone. The retained, settable system-profile owner for timezone/daylight does not exist yet:
  `sceRtcGetCurrentClockLocalTime` and UTC/local conversion therefore run on the fixed UTC
  constant, and that single UTC/local-conversion criterion stays blocked on the missing owner.
  The explicit-offset `sceRtcGetCurrentClock` path is complete and independent of it.
- **Media** — the PSMF timestamp model (`mpeg.c`) and H.264 PES timestamps are stream-relative media
  domains and are not wall time.

Host behavior: in paced mode (default) `s_vtime_us` tracks SDL's monotonic clock at scheduler
boundaries — a host stall or sleep advances guest time by the stall, and slow frames are caught up by
skipping missed vblank slots while preserving the rational phase carry. The update is forward-only
(`t > s_vtime_us`), so host wall-clock corrections/rollback cannot move guest time backward or jump
it. In turbo mode (`SR_NOVBPACE=1`) time advances deterministically at scheduler boundaries and is
never host-dependent. SDL monotonic time is used for pacing/profiling only; the one host wall-clock
read is the RTC epoch init.

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
