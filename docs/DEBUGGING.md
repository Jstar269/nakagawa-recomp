# Debugging Guide

Maintained guide to the runtime's primary debug categories, commonly used environment variables,
and supported diagnostic workflows. It is not an exhaustive inventory of every specialized
`SR_*` switch: the implementing source and `nk_manager.ps1` are authoritative for diagnostic
switches that are added for a focused investigation and have not yet been promoted into this guide.

## Quick Start

```powershell
# Enable all debug categories
$env:SR_DEBUG = "0xFF"
.\build\hst\hst.exe --image build\hst\hst_image.bin 0 0 none none --gui

# Enable only memory and HLE tracing
$env:SR_DEBUG = "0x03"
.\build\hst\hst.exe --image build\hst\hst_image.bin 0 0 none none --gui
```

Prefer the profiles in `nk_manager.ps1`, which clear stale diagnostics before launching. Most
legacy Boolean `SR_*` switches are presence-based: assigning the string `"0"` can still enable
them. Disable such a switch with `Remove-Item Env:NAME -ErrorAction SilentlyContinue` (or
`$env:NAME = $null` in `pwsh`), not `NAME=0`. Numeric settings such as
`SR_FBSNAP=<N>` and the `SR_DEBUG` bitmask are exceptions.

For performance investigations, add `-GuestProfile` to any manager `Run` profile. It enables the
existing low-noise guest-PC profiler independently of the verbose scheduler/HLE diagnostics and
dumps its call/block summary periodically and when the runtime exits. The manager's default period
is 3,600 vblanks (about one minute at the intended cadence); tune it with
`-GuestProfilePeriod`, or pass `0` for exit-only capture. Combine it with `-Profile Benchmark` to
correlate guest hot paths with `logs/perf.csv`; compare the unprofiled Benchmark run first because
guest instrumentation and profile output have measurable overhead.

## SR_PERF subsystem attribution

`SR_PERF` is an opt-in aggregate profiler. It records one-second stderr/CSV intervals and writes a
machine-readable `nakagawa-perf-v1` summary at process shutdown. Set an explicit JSON path when
using a direct runtime invocation:

```powershell
$env:SR_PERF = "1"
$env:SR_PERF_JSON = "build/perf/baseline.json"
$env:SR_PERF_CSV = "build/perf/baseline.csv"
& .\build\hst\hst.exe --image build\hst\hst_image.bin 0 0 none none
```

`SR_PERF_CSV` derives a sibling `.json` path when `SR_PERF_JSON` is unset. The manager's
`Benchmark` profile sets both paths and copies both artifacts. With `SR_PERF` unset or set to `0`,
clock reads, aggregation, interval output, summary writing, and generated AOT per-instruction hooks
are disabled. Runtime seams use one predicted `sr_perf_enabled` branch; `PERF_AOT_INSTRUCTIONS=0`
removes the generated per-instruction hook entirely. The benchmark command measures the remaining
runtime branch cost rather than assuming a zero-cost binary.

The summary fields mean:

- `guest.ns` is inclusive guest execution time. `guest.aot_ns` and `guest.interpreter_ns` are
  nested tier intervals and must not be added to `guest.ns` or to scheduler time. AOT instruction
  count is `null` unless generated chunks were built with `PERF_AOT_INSTRUCTIONS=1`; a compiled
  hook reports zero for a run that executed no AOT instruction rather than claiming an unobserved
  value.
- `aot_to_interpreter_count` and `interpreter_to_aot_count` count completed tier hand-offs. Each
  direction retains a deterministic, bounded top-16 table keyed by guest PC and reason. The table
  is an aggregate heavy-hitter sample, not a per-transition log; the total counts remain exact.
- `vfpu` counts fallback operations, family classification, fallback time, and failures. Native AOT
  VFPU execution has no separate per-instruction timer yet, so AOT VFPU time is not inferred from
  total guest time; this boundary is in the works (#282).
- `scheduler` is global execution residency: running, runnable, blocked, and idle intervals, plus
  context-switch selections. It is sampled at scheduler boundaries, not a sum of simultaneous
  per-thread lifetimes; a wake that remains preempted is accounted for at the next scheduler
  boundary.
- `ge.cpu_ns` covers the GE command-list CPU boundary. `transform_sample_ns` and `primitive_ns`
  are only the existing sampled GE CPU profiler's deltas; they are not inferred from frame time and
  are in the works for a complete phase breakdown (#282).
- `vulkan.submit_ns`, `wait_ns`, `readback_ns`, and `pipeline_creation_ns` measure separate host
  API regions. Readback and general wait can overlap and are intentionally not additive;
  `readbacks` counts completed CPU readback commits, not every fence poll.
- `textures` counts cache hits/misses and central decode calls. `storage` separates public ISO and
  VFS byte/read/failure counters. `media` counts H.264 and ATRAC decode calls, elapsed time, and
  failures. If the host H.264 backend is unavailable, the runtime reports its named boundary
  (`H.264 backend unavailable; in the works (#283)`) instead of manufacturing a decode result.
  `audio` separates SAS mix time from host output-enqueue time and frames; unavailable public audio
  output remains an explicit product boundary (#301).

For exact AOT instruction counts, rebuild generated chunks with the compile-time profile:

```powershell
mingw32-make GAME_NAME=... GAME_ELF=... PERF_AOT_INSTRUCTIONS=1 production-smoke-gap
```

The default summary reports `aot_instruction_count: null` when that hook was not compiled. The
runtime profiler is not a correctness oracle and does not alter guest-visible results.

The public source-owned benchmark matrix is repeatable without private title data:

```powershell
mingw32-make BUILD_DIR=build/perf-282 perf-benchmark
```

It runs the AOT-gap production smoke, cosimulation, PSMF media, audio, ATRAC bridge, platform-ladder
AOT-gap, platform-ladder scheduler, and platform-ladder VFS workloads, records per-run status and
JSON paths, and
measures disabled/enabled overhead from repeated direct runs of one public production-smoke build
in `build/perf-282/perf-benchmark/benchmark.json`. Exit 77 is reported as `SKIP`, never as a pass.
Compare two summaries without treating wall-clock metadata as a regression:

```powershell
python tools/perf_summary_diff.py before.json after.json --tolerance 0.02
```

The diff tool compares transition PCs by `(pc, reason)`, preserves `null` as not compiled, and
ranks non-zero subsystem costs without summing overlapping intervals. A private HST profile may
add a real-workload route, but it is supplemental and must not replace the public matrix.

## VBLANK delivery ledger (is the guest told about too many vblanks?)

A game runs fast if the runtime hands the guest more VBLANK episodes than the
60000/1001 Hz display source produced, so the pacing question is answered over the
WHOLE run, from the two artifacts a `Benchmark` run already writes:

```powershell
python tools/vblank_ledger.py --perf logs/perf.csv --stderr logs/stderr_run.log
```

```text
VBLANK_CHECK: presenting PASS presenting_s=3454 wall_s=3457.723 source_periods=207256.1 delivered=207248
VBLANK_CHECK: rate PASS ratio=0.99992 band=[0.99500,1.00500] excess_episodes=-16 ... per_second_delivered={59: 617, 60: 2209, 61: 623}
VBLANK_CHECK: identity PASS owed=205096 coalesced=7 delivered=205102 dropped=0 in_flight=1 residual=1 late_owed=84207 late_delivered=84206
VBLANK_CHECK: masked PASS coalesced=7 masked_periods=11 credit=-4 in_flight=1
VBLANK_AUDIT: verdict=PASS checks=4 failures=0
```

- `rate` divides delivered episodes by the source periods that elapsed
  (`presenting wall seconds` x 60000/1001) and fails in BOTH directions, naming the
  excess or deficit in episodes and in Hz. A ratio below 1 with `dropped=0` is
  latency, not loss: `identity`'s `in_flight` residual is what the source still owes.
- `identity` is the runtime's own ledger (`PERF_ATTRIB vblank_owed`):
  `owed + coalesced == delivered + dropped + in_flight`. A mismatch is a bookkeeping
  bug and is reported as one, not absorbed into a rate.
- `masked` compares the episodes credited to masked windows
  (`coalesced`) with the periods that actually elapsed with interrupts clear
  (`PERF_ATTRIB vblank_late ... masked_periods=`). A credit beyond the in-flight
  residual is a masked window counted twice; the hardware probe measured `+1` for a
  window however many periods it covered (`docs/ARCHITECTURE.md`).
- A missing ledger is `SKIP` with its reason, and a run where nothing could be
  judged is `NOT_RUN` with a non-zero exit: an absent artifact is not a pass.

Do not read the run's rate from the per-second `vblank_hz` column. A second holds 59,
60 or 61 episodes, so its ratio is quantised -- 60 episodes in a 1.0005 s interval
reads 59.94 Hz while 61 in the same interval reads 60.94 Hz. Averaging those ratios
over a run, or selecting the best-looking seconds, reports a rate the run never
delivered: one 60-minute idle soak measured `delivered/wall = 0.99996` (207248
episodes over 3457.7 s, `dropped=0`, `in_flight=1`) while the best 30 of its seconds
averaged 61.43 Hz. The 61-episode seconds are real and expected: the runtime
delivers a preserved backlog as a burst when a service point finally arrives, which
`per_second_delivered=` prints so a catch-up is visible as a count rather than as a
faster game.

## Debug Categories (SR_DEBUG bitmask)

The `SR_DEBUG` environment variable accepts a hex bitmask to enable multiple categories at once:

| Bit | Hex | Category | Description |
| ----- | ------ | ---------- | ------------- |
| 0 | 0x01 | `SR_DBG_MEM` | Memory access logging (out-of-range, watches) |
| 1 | 0x02 | `SR_DBG_HLE` | HLE syscall dispatch tracing |
| 2 | 0x04 | `SR_DBG_SCHED` | Thread scheduling events |
| 3 | 0x08 | `SR_DBG_GE` | GE command processing |
| 4 | 0x10 | `SR_DBG_INPUT` | Input state changes |
| 5 | 0x20 | `SR_DBG_FS` | Filesystem / I/O operations |
| 6 | 0x40 | `SR_DBG_VIDEO` | Display, framebuffer, vblank |
| 7 | 0x80 | `SR_DBG_MISC` | Everything else (fonts, callbacks, etc.) |

**Examples:**

- `SR_DEBUG=0xFF` — All categories
- `SR_DEBUG=0x03` — Memory + HLE
- `SR_DEBUG=0x0D` — Memory + Sched + GE
- `SR_DEBUG=0x02` — HLE only

## Legacy Environment Variables

Individual `SR_*` variables still work for backward compatibility. When `SR_DEBUG` is not set, these are checked and mapped to categories:

### Memory & Access (→ SR_DBG_MEM)

| Variable | Description |
| ---------- | ------------- |
| `SR_OORLOG=1` | Log out-of-range memory accesses |
| `SR_BREAKLOG=1` | Log sr_break() calls |

### HLE & Syscalls (→ SR_DBG_HLE)

| Variable | Description |
| ---------- | ------------- |
| `SR_HLELOG=1` | Trace HLE syscall dispatch |
| `SR_NIDLOG=1` | Log NID lookups |

`SR_NIDLOG=1` writes one line per syscall to `nidseq_mine.txt` in the run
directory, NID first: `0x<nid> <uid> <vblank>` (e.g. `0x780f88d1 0x105 123`).
The NID is always the first whitespace-separated token so simple `awk '{print
$1}'` extraction keeps working; consumers that only need the call sequence
should ignore the trailing uid/vblank columns.

### Thread Scheduling (→ SR_DBG_SCHED)

| Variable | Description |
| ---------- | ------------- |
| `SR_THLOG=1` | Trace thread create/start/exit |
| `SR_BLOCKLOG=1` | Log thread blocking events |

### GE & Graphics (→ SR_DBG_GE)

| Variable | Description |
| ---------- | ------------- |
| `SR_GELOG=1` | Log GE command processing |
| `SR_GEWATCH=1` | Interleave GE present with GELIST lines |
| `SR_GEWATCH_AFTER=N` | Start GE watch after frame N |
| `SR_GEMATW=1` | Log GE matrix writes |
| `SR_NO2DZ=1` | Disable 2D Z-buffer |
| `SR_GPU_STATS=1` | Emit bounded Vulkan submission/batch/texture/snapshot counters without per-submit logging |
| `SR_GEDUMP=1` | Per-primitive `GE PRIM` lines (through and transform), bounded to the first ~40 |
| `SR_GE_ENQUEUE_TRACE=1` | Trace GE enqueue/stall-update provenance without enabling the broader GE dump |
| `SR_GE_ENQUEUE_TRACE_WINDOWS=a-b[,c-d]` | Restrict enqueue trace output to up to eight inclusive vblank ranges; malformed ranges fail closed |
| `SR_GESTAT=1` | 60-vblank stat windows: `GESTAT` totals, `GE3D` distinct 3D draw signatures, `ASHADE`/`ACLUT` alpha-test-failure decode. A window closes on the first delivered vblank at or past each multiple of 60 and `f=` names that vblank, which is later than the multiple (61, not 60) when VCOUNT stepped over it on a busy host; read the label with `tools/ge_stat_windows.py` instead of matching `f=60` |
| `SR_RTRACE=1` | Exhaustive render trace: one `TRIDRW` line per 3D draw with the complete GE state (render target `fbp`/`zbp`, texture address/format/`bufw`/swizzle, texture function, blend, alpha test, cull, scissor, viewport), plus per-triangle `TRIDEC` lines |
| `SR_RTRACE_FRAMES=N` | Frames traced per stat window (default 2) |
| `SR_TEXDUMP=1` | Write each distinct sampled texture from transform- or through-mode draws once as `tex_ADDR_fF_WxH.ppm` decoded through the real sampler (swizzle + CLUT), and log its CLUT address/format. First 32 distinct addresses per run |
| `SR_TEXDUMP_AFTER=N` | Defer texture dumping until GE frame `N`, preserving the fixed distinct-texture budget for a late deterministic scene |
| `SR_GE_TRANSITION_TRACE=PATH` | Narrow one-frame-corruption harness (issue #69): one JSONL record per weighted `PRIM` draw with frame/draw ordinal, list id, command address, bone/world/view/proj matrices as both last guest writes and draw-time state, `non_finite` (non-finite values among the four effective matrices, so the record names what the GE drops), decoded `VTYPE`, bases/count/prim, render target, bound texture, and stable `draw_id`. `1` selects `logs/ge_transition_trace.jsonl`. Off by default; zero cost when off. Diff offline with `python tools/ge_transition_diff.py TRACE --frames GOOD:BAD`. A non-finite matrix entry is emitted as JSON `null` (printf's `-nan(ind)` is not JSON) and the diff flags it on its own line as `NON_FINITE in bone_written[0..35]` Also one `{"kind":"late_write"}` record per weighted draw whose vertex or index bytes changed between the draw and the next present, `sceGeDrawSync` wait or completed `sceGeListSync` (the runtime reads those bytes at enqueue, earlier than the console's GE would): `vertex_span`/`index_span`, both hashes, `check` and `check_vblank`; mirrored as a `GE_LATE_WRITE` stderr line. A late write is a candidate cause, not proof. |
| `SR_NAN_TRAP_LIMIT=N` | Report limit for the `SR_NAN_TRAP` build option below (default 20; `0` silences it). Only has an effect in a package built with `make NAN_TRAP=1` |
| `SR_NAN_TRAP_CONTEXT=1` | After each `NAN_TRAP` line, a `NAN_TRAP_CTX` line with `ra`, `v0`, `a0`..`a3`, `t0`, `s0`, `s1` and `f12`, then one line per readable 16-byte span behind `v0` and `a0`..`a3` (four floats), so an origin instruction's inputs in guest memory can be read without editing generated code. Same build requirement as the limit. |

### Locating the instruction that produced a NaN (issue #69)

The `SR_GE_TRANSITION_TRACE` above tells you **that** a draw's bone matrices went NaN. It cannot
tell you **which guest instruction** made them NaN, because it records state, not arithmetic.
`SR_NAN_TRAP` answers that: it is a build-time option that follows every FPU and VFPU result
write — in the generated code and in the AOT-gap interpreter alike — with a check that reports the
first few instructions whose result went non-finite **from operands that were all finite**:

```text
NAN_TRAP pc=0x00001234 op=div.s dst=f7 vbl=1180 in=[0,0] out=[nan]
NAN_TRAP pc=0x088a1f2c op=vdiv.s dst=v13 vbl=1180 in=[0,4.5,0,4.5] out=[nan,1,0,1]
```

It is a diagnostic, never a semantic gate: no result changes on any path, and a NaN that is only
*propagated* (every later instruction in a long chain) is not reported, so the report names the
origin rather than the last echo.

`vbl=` is the guest VBLANK count, the same counter the `SR_GE_TRANSITION_TRACE` records stamp as
their `frame`, so a trap line and the draw that showed its effect sit in one frame without
inference. It is read from the mirror the runtime hands to the GE each vblank, so a report produced
inside a vblank handler carries the frame it is about to be drawn in.

`in=` lists **every operand the instruction consumed**, in operand order, which is what makes a
report readable lane by lane:

| Form | `in=` order |
| --- | --- |
| scalar FPU (`add.s`, `div.s`, …) | its source registers |
| lane forms (`vadd`, `vdot`, `vmin`, `vocp`, …) | first source vector, then second |
| `vmmul` | S rows (`side`×`side`), then T rows (`side`×`side`) |
| `vtfm`/`vhtfm` | matrix lanes (`side`×`side`, row-major), then the vector lanes it multiplies by |

A form that reported only part of its operands would classify a propagation as an origin: a `vtfm`
whose vector lane already carried a NaN used to be reported as the instruction that made it, and
the vector was not in the record at all. With every operand listed, that `vtfm` is silent and the
real origin (the instruction that made the vector lane NaN) is the one that reports.

One run with both diagnostics therefore carries both ends of the corruption: the `NAN_TRAP` line
names the instruction, and the trace record with the same `frame`/`vbl` and a non-zero `non_finite`
names the draw it reached.

Rebuild the package with the trap, play to the corruption, and read the first `NAN_TRAP` lines:

```powershell
# 1. Build with both halves of the option (codegen --nan-trap and -DSR_NAN_TRAP).
#    The Make variable is inherited by the manager's make invocation, and it is
#    carried into the codegen and recompiler profile hashes, so this regenerates
#    instead of reusing untrapped objects.
$env:NAN_TRAP = "1"
.\nk_manager.ps1 -Action BuildFull

# 2. Play by hand to the corrupted model, capturing stderr. From another shell:
Get-Content -Wait logs\nan_trap.txt | Select-String '^NAN_TRAP '

# 3. Raise or lower the report budget if the first 20 are not the interesting ones.
$env:SR_NAN_TRAP_LIMIT = "200"

# 4. Back to an ordinary build afterwards.
Remove-Item Env:NAN_TRAP -ErrorAction SilentlyContinue
.\nk_manager.ps1 -Action BuildFull
```

Driving the player directly works the same way — `NAN_TRAP` is an ordinary Make input:

```powershell
mingw32-make all NAN_TRAP=1          # adds --nan-trap and -DSR_NAN_TRAP together
```

> [!IMPORTANT]
> **`make NAN_TRAP=1` turns on both halves at once, and both are needed.** `--nan-trap` makes
> `tools/codegen.py` emit the checks; `-DSR_NAN_TRAP` makes those checks live. Setting only one
> produces a build that looks correct and reports nothing. With the option off, codegen emits no
> check statement at all, so the generated C is byte-identical to a pre-trap build, and every
> `SR_NAN_TRAP_*` macro expands to `((void)0)` — an untrapped object is byte-identical with and
> without `-DSR_NAN_TRAP` (proved by `tools/test_codegen_nan_trap.py`).

Coverage boundary: every FPU result write and every VFPU *value-producing* lane form is checked —
`add.s`/`sub.s`/`mul.s`/`div.s`/`sqrt.s`/`abs.s`/`mov.s`/`neg.s`, `vadd/vsub/vmul/vdiv`, `vdot`,
`vhdp`, `vcrs`, `vscl`, `vmin`/`vmax`, `vscmp`/`vsge`/`vslt`, `vcmov`/`vcmovt`/`vcmovf`, `vocp`, the VV2Op scalar set and
the transcendental set (`vrcp`/`vrsqrt`/`vsin`/`vcos`/`vexp2`/`vlog2`/`vsqrt`/`vasin`), `vmmul`,
`vtfm`, `vmscl`, `vcrsp`/`vqmul`, and `vrot`. Not checked, by construction: constant broadcasts
(`viim`/`vfim`/`vcst`/`vzero`/`vone`/`vidt`/`vmidt`/`vmzero`/`vmone`, which have no operand to be
non-finite relative to), the integer reinterpretations (`vs2i`/`vi2uc`/`vi2c`/`vi2us`/`vi2s`/
`vf2i*`, which write integer words), `vi2f` (a signed 32-bit integer always widens to a finite
float32), the `lv`/`sv` memory forms (guest data, not a computed result), and the raw per-element
`v[]` row copies of `VFPUMatrix1` (`vmmov`/`vmscl` rows, which move an existing lane).

> [!IMPORTANT]
> **`SR_RTRACE` re-arms only when `SR_GESTAT` is also set.** Its per-window frame budget is reset
> inside the `SR_GESTAT` 60-frame window block in `ge_set_frame()`. With `SR_RTRACE=1` alone you get
> `TRIDRW` lines for the first `SR_RTRACE_FRAMES` transform-mode frames of the **whole run** — which
> during boot means you capture the logo screens and nothing else. Always pair them:
> `SR_GESTAT=1 SR_RTRACE=1 SR_RTRACE_FRAMES=1`.
>
> Note also that `SR_TEXDUMP`'s 32-address budget is per **run**, not per window. On a long route,
> pair it with `SR_TEXDUMP_AFTER` so boot textures cannot consume that budget. Generated texture
> and alpha images are private runtime artifacts and must not be committed.

`SR_GE_ENQUEUE_TRACE` is intended for a narrow renderer-independent submission check. Each record
contains the vblank, operation, thread UID, `$ra`-derived callsite, list/stall/callback values, and
the last HLE and guest callback observed on that thread. A paired result record reports whether the
list was deferred, stalled, completed, or missing, plus the completed list's command signature,
primitive count, backend-independent `through`/`transform` command-vertex-sprite tuples, and write
counters. Pair it with
`SR_GE_ENQUEUE_TRACE_WINDOWS`, for example `8200-8300,34800-35200`, on long deterministic routes.
The diagnostic does not skip guest work or change GE execution.

### Input (→ SR_DBG_INPUT)

| Variable | Description |
| ---------- | ------------- |
| `SR_INLOG=1` | Log input state changes |
| `SR_PAD=HEX` | Override pad state (hex buttons) |
| `SR_PADPERIOD=N` | Automatic pad-pulse period in vblanks (default 240) |
| `SR_PADWIDTH=N` | Automatic pad-pulse width in vblanks (default 4); each pulse is held until the guest has read it (see [Scripted input is delivered in guest time](#scripted-input-is-delivered-in-guest-time)) |
| `SR_PADSTART=N` | Pad override start frame |
| `SR_PADSCRIPT=FILE` | Scripted pad input: a state-qualified route program, or the legacy `frame hexmask width` table |
| `SR_PADSCRIPT_READ_BUDGET=N` | Vblanks a scripted press may wait for the guest to read it before the run fails (default 1800; a press whose width is in guest reads may wait for all of them); not a whole number >= 1 refuses the script |
| `SR_OSK_SCRIPT=FILE` | Scripted on-screen-keyboard answers, one per input field in order (`CANCEL` answers that field as cancelled) |
| `SR_OSK_TEXT=TEXT` | Answer every on-screen-keyboard field with the same text |
| `SR_NOINPUT=1` | Disable the automatic START pulse; live and scripted input still work |
| `SR_ROUTE_LEARN=1` | Print the route signature of every sampled and captured frame (`ROUTE_SIG v=<n> <hex>`) |
| `SR_ROUTE_NO_EXIT=1` | Do not terminate on a route failure (executable regression tests only) |

`SR_PADSCRIPT` is the preferred way to make a visual route repeatable. Prefer a
**route program** (below) for anything that has to be trusted as evidence; the
legacy numeric table is still accepted unchanged for existing routes, and its
parser accepts numeric rows only, so do not put headings in such a file.
To turn a recorded run into a replay:

```powershell
$env:SR_INLOG = "1"
.\nk_manager.ps1 -TitleManifest assets/titles/hst-ucus98701.json -GameName hst -Action Run -Profile Standard
python tools/padscript_from_log.py logs/stderr_run.log `
  --minimum-width 8 --output logs/route.pad
$env:SR_PADSCRIPT = (Resolve-Path logs/route.pad).Path
$env:SR_NOINPUT = "1"
.\nk_manager.ps1 -TitleManifest assets/titles/hst-ucus98701.json -GameName hst -Action Run -Profile Standard
```

The converter expands shorter presses so a replay holds each press for as many
vblanks as a person would. A one-vblank pulse can no longer fall between the
game's controller reads, because scripted input waits for a read (below). Use
`--minimum-width 1` only when an exact-width replay is required.

### Scripted input is delivered in guest time

Every scripted input source uses the same delivery rule (`src/rt/scripted_input.h`): the
legacy `SR_PADSCRIPT` table, the route program's `PRESS`, `DELAY`, `PRESS_UNTIL` and
`PRESS_WHILE`, and the automatic START pulse. A script is a sequence of pressed and released
stretches. Each stretch ends only when both of these hold:

- it has been latched for its authored width in controller samples (one sample per serviced
  vblank). A width written in guest reads ([below](#widths-in-guest-reads)) replaces this
  condition with the number of guest reads it asks for;
- the guest has read the controller (`sceCtrlReadBufferPositive` or
  `sceCtrlPeekBufferPositive`) and been handed a sample of it.

A stretch never starts before its authored vblank. On a host that keeps up, the guest reads
inside every window and the timing is exactly the authored timing. On a host that falls
behind, the runtime services several elapsed periods at once, with no guest code between
them. The script then stretches until the guest has seen each press and each release, and
it never shortens or skips one.

Before this rule, a press whose window one such batch stepped over was never latched. A
press shorter than the guest's frame time on a starved host was never read either. The
headless showcase smoke "missed the scripted Cross input sample" that way.

A press the guest does not read within `SR_PADSCRIPT_READ_BUDGET` vblanks (default 1800)
fails the run with `ROUTE_FAIL` naming the buttons, the step and the vblanks waited. That
covers a legacy row and a `PRESS` step. A release waits without a budget, because a guest
that is not polling cannot be pressed. So does a `PRESS_UNTIL` / `PRESS_WHILE` pulse, whose
step timeout already bounds it. A press whose width is in guest reads
([below](#widths-in-guest-reads)) has the same budget for the whole wait for its reads.
`SR_INLOG=1` adds a `ctrl_read: vcount=<n> guest read scripted <mask>` line when the guest
first reads each scripted state.

### The keyboard in the game window

On the SDL3/Vulkan presenter (the default window) the keyboard a title opens is drawn in the
game window (`src/rt/osk_overlay.c` for the state, `src/rt/osk_overlay_paint.c` for the
drawing), and a person answers it with the pad or the keyboard: the d-pad or arrow keys move,
Cross or Enter types the highlighted key, Circle or Esc cancels, Start or Tab confirms, and
L/R switch letter case. A physical keyboard also types directly, and Backspace deletes. The
field's maximum length and its inputtype (`SceUtilityOskData` at +0x10, PSPSDK layout) limit
what it accepts. While it is open the title's pad reads nothing, and guest time keeps running.

A route answers the in-window keyboard with ordinary `PRESS` steps. The keyboard reads every
controller sample while it is open, as the system keyboard polls the pad on a PSP, so a press
it takes counts as read by the route. `SR_OSK_TEXT` and `SR_OSK_SCRIPT` are not needed for it.
`SR_WINDOW_HIDDEN=1` runs the same presenter with a hidden window, so a headless route keeps the
window's keyboard and its frame capture without a visible window. Under the offscreen presenter
the overlay is not drawn, and the variables below still answer. Where the presenter cannot draw
the keyboard, the native input box is the fallback. If SDL refuses the frame's surface or its
renderer after a request opened, the request is dropped, one stderr line names the SDL error,
and the field falls back to the native input path (the Windows input box) for the rest of the
run.

### Scripted keyboard answers

`SR_PADSCRIPT` decides what a person *presses*; `SR_OSK_SCRIPT` decides what they *type*. The
on-screen keyboard a title opens for a name is otherwise answered by a person in a native
Windows input box, which an automated route cannot reach: it has to find that window and type
into it, and the attempt races the game's own dialog timing. Both variables are off unless set,
and neither changes how a person plays.

The keyboard never stops guest time, scripted or not. On the PSP it is a system overlay: the
title keeps running underneath it, polling `sceUtilityOskGetStatus` once per frame. Every guest
thread here runs on the one scheduler thread, so the input box is shown on a worker thread
(`src/rt/osk_text_entry.c`) and each poll returns at once with `VISIBLE` until the person
presses OK or Cancel; vblanks, frames, audio and the title's other threads carry on meanwhile.
Under the offscreen presenter (`SR_VIDEO=offscreen`, headless bring-up) nobody can answer, so
no window is opened at all: the keyboard stays open while the title keeps running, which is
what a PSP nobody is typing at does, and stderr says so once. A headless route that has to get
past a name entry answers it with one of the variables below.

`SR_OSK_SCRIPT=FILE` holds one answer per keyboard field, in the order the keyboard presents
them. Each line is the text to enter, or the keyword `CANCEL` to answer that field as
cancelled. Blank lines and lines starting with `#` are skipped:

```text
# name entry, then a confirmation the route does not want to accept
ACE
CANCEL
```

`SR_OSK_TEXT=TEXT` is the short form: the same text for every field, `CANCEL` included. The
answer is truncated at the field's own `outtextlimit` exactly as typed text would be, and is
written into the field as UTF-16 whether the script file is ASCII or UTF-8. A byte that is not
valid UTF-8 becomes U+FFFD, so a broken script shows up in the guest's text instead of
vanishing.

While either variable is set **no native input box is ever opened**. A field the script does
not cover is answered `CANCELLED` and the shortfall is named on stderr, because an automated
run that reached an unanswered keyboard would otherwise wait at it for a person who is not
there. `SR_DLGLOG` logs the same `osk: field N ...` line either way, so a scripted run and a
played one are read the same way.

A scripted answer follows the same status sequence as a person's. `sceUtilityOskGetStatus`
returns the common dialog state from the PSPSDK headers: `INIT`, then `VISIBLE` (for as many
frames as the person takes; one poll for a scripted answer), then `QUIT` (whether the text was
confirmed or cancelled; each field's result tells them apart) until the title calls
`sceUtilityOskShutdownStart`, then `FINISHED` once and `NONE`. A keyboard the title shuts down,
or replaces with a new one, before the person answered closes its input box and writes nothing.
`sceUtilityOskUpdate` itself keeps its named no-dialog compatibility result
([#281](https://github.com/Jstar269/nakagawa-recomp/issues/281)); the runtime owns the
progression instead.

### State-qualified route programs (issue #64)

A route written as absolute vblanks is a bet that the guest is on the screen its author saw
when they recorded it. Boot and transition durations vary between otherwise identical
replays, so the bet loses: seven replays of one script from one restored save baseline
reached two different menu depths, and the two divergent runs spent their whole budget in
Story Mode rather than the intended Exhibition match. **Both still reported a complete run**,
because "reached vblank N" was the only thing anything checked. Elapsed vblanks are a budget,
never a proof of state.

A route program makes each input wait for the screen it assumes. `SR_PADSCRIPT` selects it
automatically: a file containing any keyword line is a program, a file of bare numeric rows
keeps the original behaviour exactly, and a file mixing the two is refused.

| Line | Meaning |
| ---- | ------- |
| `SIGGRID <cols> <rows>` | Signature grid, default `12 8`; must precede every `CHECKPOINT` |
| `SAMPLE_EVERY <n>` | Observation cadence in vblanks (default 20) |
| `TOLERANCE <n>` | Default match tolerance, mean absolute channel difference (default 12); must precede every `CHECKPOINT` |
| `CHECKPOINT <NAME> [tol=<n>] <hex>` | A screen signature; repeat `NAME` to record it again (see below) |
| `WAIT <NAME> <timeout>` | Block until `NAME` is observed; fail the run on timeout |
| `EXPECT <NAME>` | Assert `NAME` is on screen now; fail the run if it is not |
| `PRESS <hexmask\|buttons> <width>` | Hold `hexmask` (or the named buttons) for `width` samples and until the guest has read it |
| `PRESS_UNTIL <NAME> <hexmask\|buttons> <width> <period> <timeout>` | Repeat the press (held `width`, released for the rest of `period`, each until read) until `NAME` is observed; fail on timeout |
| `PRESS_WHILE <NAME> <hexmask\|buttons> <width> <period> <timeout>` | Repeat the press the same way while `NAME` is observed; complete when it is not |
| `DELAY <n>` | Release the pad for `n` samples and until the guest has read the release (input cadence *within* one screen) |
| `WAIT_NID <import\|0xNID> <timeout>` | Block until the guest calls that import; fail the run on timeout |
| `PRESS_UNTIL_NID <import\|0xNID> <hexmask\|buttons> <width> <period> <timeout>` | Repeat the press (held `width`, released for the rest of `period`) until the guest calls that import; fail on timeout. Use it where a press may land without changing the screen, such as a message box or a Yes/No that waits on a savedata check, because the import is the event that says the press was taken |
| `WIDTHS VBLANKS\|READS` | The unit the widths of `PRESS`, `DELAY`, `PRESS_UNTIL`, `PRESS_WHILE` and `PRESS_UNTIL_NID` count in, for the whole file (default `VBLANKS`); must precede every step, and may appear once |
| `READS <step>` / `VBLANKS <step>` | The unit of that one `PRESS`, `DELAY`, `PRESS_UNTIL`, `PRESS_WHILE` or `PRESS_UNTIL_NID` line; overrides `WIDTHS` |
| `END` | Route complete |

A mask is either a button name (see [Naming the buttons a route presses](#naming-the-buttons-a-route-presses))
or a hex literal of at most eight digits with an optional `0x` prefix. Anything else refuses
the file at load, naming the line and the token, in `PRESS`, `PRESS_UNTIL`, `PRESS_WHILE` and
the legacy `frame hexmask width` row alike — an unknown name, a name cut short, a stray
character after a hex literal, a bare `0x`, or a value too wide for the mask, none of which may
quietly become a *different* press. That refusal is the point: the parser used to coerce a
token it could not read at all to **zero**, so a route asking for a button the format never
defined pressed nothing while the run carried on — indistinguishable, from outside, from a game
that had frozen. A press that is never delivered must be a load-time error, not a mystery.

`#` starts a comment. A screen is "observed" by a coarse signature of the presented
framebuffer: the frame is split into `cols x rows` cells and each cell contributes its mean
R, G and B; a screen matches when the mean absolute difference from a recorded signature is
within tolerance. Sampling only runs while a `WAIT`, `EXPECT` or `PRESS_UNTIL` is pending,
so a route pays nothing for it while pressing or delaying.

**Record a screen twice when part of it varies.** HST draws its menus over a club backdrop
that is not the same every run, so a whole-frame comparison rejects the right screen: two
recordings of the Main Menu taken from different runs sit 14 apart, well outside any
tolerance that still separates the Main Menu from its submenu. Repeating a `CHECKPOINT`
name records the same screen again, and the bytes the recordings disagree on are dropped
from the comparison — they carry the variation, not the identity. On the two observed
backdrops that leaves about half the frame informative and the Main Menu ~8x closer to
itself than to the submenu. Recordings sharing less than a quarter of the frame are refused
at load: they are not one screen, and a route built on them could not fail.

**`PRESS_UNTIL` is for the boot prefix.** The warning screens and intro movie each need
their own START and there is no way to know in advance how many. As a fixed table, every
extra press is one that lands on whatever comes next when the run is faster than the
recording — which is precisely how a `CROSS` meant for the title screen ended up opening a
menu. `PRESS_UNTIL TITLE_SCREEN START 8 240 15000` stops the moment the title appears.

**Failure is loud and terminal.** A failed `WAIT` or `EXPECT` prints `ROUTE_FAIL:` naming the
step, the vblank and the screen that was actually there, then exits **86**. The manager reads
that narration back into `oracle_manifest.json` (`route_kind`, `route_checkpoints`,
`route_fail_reason`) and a failed or unfinished route makes the run **inadmissible**, so a run
that took a different path through the menus can no longer be archived as though it were the
route it names.

Authoring a checkpoint takes one learning run:

```powershell
$env:SR_ROUTE_LEARN = "1"
.\nk_manager.ps1 -TitleManifest assets/titles/hst-ucus98701.json -GameName hst -Action VisualOracle -Route logs/route_legacy.pad -ExitAtVblank 9500 `
    -SnapEvery 60 -SnapWindows "7800-9200" -SaveBase logs/oracle_savebase -OracleName learn
```

Every captured frame emits its signature at the same vblank
(`ROUTE_SIG v=8220 <hex>` beside `snap_v8220.ppm`), so you convert the frame you actually
looked at — `python tools/ppm2png.py snap_v8220.ppm out.png`, identify the screen, then paste
that vblank's hex into a `CHECKPOINT` line. Signatures are derived from retail frames: they
belong in the private route file beside the rest of the run inputs and must never be
committed.

Two habits keep a program honest. Gate every screen *transition* with `WAIT`, and use `DELAY`
only for input cadence inside one screen — a `DELAY` standing in for a transition is the
fixed-vblank bet again. And put an `EXPECT` after a press whose effect you care about: `WAIT`
proves you arrived, `EXPECT` proves the press did what the route claims.

### Widths in guest reads

A press or a release is normally held for its width in vblanks. On a host that falls behind,
one batch can cover several vblanks before the guest reads the pad again, so a press of four
vblanks may reach the guest in a single read, and a menu that needs three frames of hold to
move one step sees it once. A width given in guest reads does not depend on the host's pace:
the press is held until the guest has read it that many times. `WIDTHS READS` states that for
the whole file:

```text
WIDTHS READS
PRESS DOWN 3          # DOWN until the guest has read it three times
DELAY 2               # released until the guest has read the release twice
PRESS DOWN 3
END
```

A line may name its own unit with a word in front of the step, so one file can mix the two.
Each stretch counts in the unit its line names:

```text
PRESS START 4         # four vblanks, the default for this file
READS PRESS CROSS 2   # two guest reads
VBLANKS DELAY 1       # one vblank
```

Only these rules differ from the vblank widths above:

- A read counts when the guest's controller read (`sceCtrlReadBufferPositive` or
  `sceCtrlPeekBufferPositive`) hands it a sample latched while that stretch was current. A
  read that hands over several samples counts once, and a read that comes before the stretch's
  first sample is latched counts for nothing.
- A release is counted the same way: a `DELAY` in reads waits for that many reads that observe
  the release.
- The release is latched at the next vblank. A guest that reads once per vblank, which is what
  most titles do, gets exactly the count. A guest that reads twice in one vblank may see the
  last press in one of its extra reads.
- `SR_PADSCRIPT_READ_BUDGET` bounds the whole wait for the reads. A read-width press that the
  guest reads fewer times than it asked for fails the run, naming how many reads it got.
- The timeouts of `WAIT`, `EXPECT`, `WAIT_NID`, `PRESS_UNTIL` / `PRESS_WHILE` and
  `PRESS_UNTIL_NID` stay in vblanks, because they are time limits. `PRESS_UNTIL`,
  `PRESS_WHILE` and `PRESS_UNTIL_NID` take their width and period in reads when they say
  `READS`.
- `WIDTHS` must precede every step and may appear only once. `READS` and `VBLANKS` apply to
  `PRESS`, `DELAY`, `PRESS_UNTIL`, `PRESS_WHILE` and `PRESS_UNTIL_NID` only, and anything
  else that names a unit is refused at load, naming the line. No existing route starts a
  line with either word, so an existing route parses and replays exactly as it did.
- The legacy `frame hexmask width` table has no keyword lines, so it stays in vblanks. Write a
  route program to use reads.

### Naming the buttons a route presses

A press mask is a bit in the `SceCtrlData.Buttons` field the guest reads — the same field a
player's keyboard or controller produces. Name the button and the mask is filled in from one
table (`NK_PSP_BTN_*_BIT` in `src/core/nk_input_profile.h`, which both host front-ends also
publish from):

| Name | Bit | Name | Bit | Name | Bit |
| ---- | --- | ---- | --- | ---- | --- |
| `SELECT` | `0x0001` | `L` | `0x0100` | `CIRCLE` | `0x2000` |
| `START` | `0x0008` | `R` | `0x0200` | `CROSS` | `0x4000` |
| `UP` | `0x0010` | `TRIANGLE` | `0x1000` | `SQUARE` | `0x8000` |
| `RIGHT` | `0x0020` | `HOME` | `0x10000` | | |
| `DOWN` | `0x0040` | `HOLD` | `0x20000` | | |
| `LEFT` | `0x0080` | | | | |

```text
PRESS_WHILE TITLE CROSS 20 90 6000
PRESS START+UP 30
```

Names are case-insensitive and join with `+`; a hex mask (`PRESS 4000 20`) still means exactly
what it always did, in the program and in the legacy `frame hexmask width` table. A name that is
not a button is refused at load, which fails the route instead of pressing nothing. Every press
step is narrated at load, so the log names the button rather than leaving a hex mask to be
decoded:

```text
ROUTE: step 1 (PRESS_WHILE) presses CROSS
```

A press that reaches the guest and is ignored still cannot be told from a hang by the guest, so
put an `EXPECT` or a `WAIT` after it: `PRESS_UNTIL` is that wait, and it is the honest form
whenever the press has a consequence.

Count the bits by hand only as a last resort, because a mask that is one bit out cannot fail
visibly: the guest receives a button the screen ignores, the screen never changes, and the run
looks exactly like a game that has frozen. `START` on a title that waits for `CROSS` is that
mistake, and it cost a whole investigation before the log said which button was being pressed.
Every run now narrates it at load —

```text
ROUTE: step 1 (PRESS_WHILE) presses CROSS
```

— so a route that stalls says which button it offered. Note that the boot prefix wants `START`
(warning screens, intro movie) and title screens usually want `CROSS`; one press is not
automatically the right one for the next screen.

### Waiting on what the guest does (`WAIT_NID`)

Every other gated step watches the *screen*, and a screen signature can only be recorded from
a run that is already on that screen. That is a chicken-and-egg problem for any route trying
to reach a screen nobody has a signature for yet: the step that would get there is the one
step that cannot be written. What the guest **calls** has no such problem — a module load, a
savedata status poll, a display-mode change is observable from the first boot and means the
same thing in every title, so the runtime can offer it and the route file names it:

```text
WAIT_NID sceIoOpen 600
WAIT_NID 0x109f50bc 600      # the same import as raw hex
```

The name comes from the runtime's own table (`src/rt/nid_names.h`); the hex form exists so a
route never depends on a name being in it, and a name that resolves to nothing is refused at
load rather than becoming a wait that can never succeed. Three properties matter and are
tested:

- the wait completes on that import and **only** on that import — an unrelated call leaves the
  route waiting;
- it counts calls made **since the step began**, so an event that already happened cannot
  satisfy a later step (two waits for the same import in a row is the shape that shows it);
- a guest that never makes the call **fails the run**, naming the import, the vblank range and
  how many imports it did make — the alternative, a route that waits forever, is
  indistinguishable from a hang.

It costs one store per import, and only while such a step is running, at the single point
(`sr_syscall`) every guest import already passes through. The step needs no framebuffer
observation, so it also does not pay the observer's sampling cost.

### Visual-oracle runs (`-Action VisualOracle`)

A visual regression oracle cares about a handful of frames around one transition, but a plain
`Run` replays the whole route with captures on from vblank 0 and stops on a wall-clock
`-Duration` guess. Guessing low silently truncates the route before its last inputs fire;
guessing high replays a finished scene for minutes. Each capture writes a ~380 KB PPM *and* an
unbounded `build/snapshots` PNG, so a long route spends hundreds of megabytes of I/O on frames
nobody looks at.

```powershell
.\nk_manager.ps1 -TitleManifest assets/titles/hst-ucus98701.json -GameName hst -Action VisualOracle -Route logs/route_X.pad `
    -ExitAtVblank 41400 -SnapEvery 60 -SnapAfter 39000 -OracleName deep_return_run1
```

It reuses whatever `hst.exe` is already built (build once, replay many), stops at a **vblank**
count rather than a wall-clock guess so the stop point is machine-independent, captures only
inside the window of interest, and archives `snap_*.ppm`, `stderr.log`, `oracle_summary.txt` and
`oracle_manifest.json` under `logs/oracle_<name>/`. Add `-Profile Benchmark` to collect
`perf.csv` alongside the captures; it goes through the same runner, so a measured run and a
visual run are the same replay.

**One run per archive.** Snapshots are numbered per run, so a shorter second run into the same
directory leaves the first run's tail behind in a set that still looks complete. Reusing an
`-OracleName` whose directory is non-empty is therefore **rejected**, not merged; pass
`-OverwriteOracle` to discard the old evidence deliberately.

**Every run is adjudicated.** `oracle_manifest.json` records Git HEAD (and whether the worktree
was dirty), the SHA-256 of both `hst.exe` and the route file, every parameter, the process exit
code, capture count, wall time, and the vblanks actually delivered. The run is reported
`complete` only if it reached its requested vblank, exited 0, was not killed at the backstop, and
produced captures; otherwise the reasons are listed and the action fails. A truncated capture set
looks exactly like a complete one on disk — reading one as complete already cost two full replays.

**The backstop is a deadline, not a duration.** The runner returns the instant `hst.exe` exits
and only kills at the deadline, so an over-generous backstop costs nothing.

#### Holding guest save state still (`-SaveBase`)

A route replay is deterministic in its **inputs** only. The game writes a real save (the
give-up path ends in "Finished saving data."), so run N+1 starts from whatever run N left
behind. This is not theoretical: two replays of the identical deep-return route diverged
because the first run's save cleared a first-time tutorial popup — the second run hit that
popup, took a different branch, and ended in a new match instead of at the club.

```powershell
.\nk_manager.ps1 -TitleManifest assets/titles/hst-ucus98701.json -GameName hst -Action VisualOracle ... -SaveBase logs/oracle_savebase
```

First use captures the current save as the baseline; every later run restores it, so all runs
start byte-identical. It is a snapshot-and-restore, **not** a wipe — deleting the save would
put the game in a "no save data" state no existing route was authored against. `*GAMEDATA` is
never touched: that is the ~400 MB install, and removing it would trigger a reinstall that
changes the route's timing completely.

**Safety contract.** `-SaveBase` is a path *inside the repository root* (the manager
anchors every managed path to its own script location, never the caller's CWD). The baseline
and the live `memstick/PSP/SAVEDATA` root must be distinct canonical directories, and neither
may contain the other. On first use the manager writes `.hst_savebase_manifest.json` into the
baseline directory recording a creation timestamp, a non-secret source identity, the relative
file inventory and per-file SHA-256s. A baseline without that manifest — an arbitrary directory
someone pointed at — is **refused** on restore, as are empty or tampered baselines. Restore
preflights the baseline, stages a verified copy beside the live root, swaps the save directories
into a rollback shelter and the staged copies into place, verifies the result against the
manifest, and only then drops the rollback; any failure rolls back and aborts loudly. The
manifest lives inside the ignored baseline directory, so no save contents are ever tracked.

**Interrupted-restore residual.** The swap is two same-volume renames (live → rollback, staged →
live). A hard OS/process termination (not a caught exception) landing between them cannot be made
transactionally atomic, so the live root can be left partially populated with the original data
stranded in an orphan `.hst_savebase_*` directory beside it. The next restore invocation detects
any such orphan and **fails closed** with an explicit message rather than running against a
possibly-partial state; manual recovery (re-inspecting the orphan and either completing or
removing it) may be required. This is an acknowledged residual, not a crash-atomicity guarantee.

`-OracleName` is an identifier, not a path: it must match `[A-Za-z0-9][A-Za-z0-9._-]{0,63}` and
archive directories are created (and cleared) only under the logs root, with reparse-point
escapes refused. The manager itself fails closed when run from outside a validated workspace —
copy it into an unrelated tree and it refuses to start.

Without `-SaveBase`, treat two runs as two different experiments, not two samples of one.

#### Capturing both ends of a transition (`-SnapWindows`)

A "did this screen come back correctly?" comparison needs the *before* and *after* frames from
the **same run**. The club backdrop varies with host wall-clock time, so a capture from an
earlier session is not a valid reference and a difference against it proves nothing.

```powershell
.\nk_manager.ps1 -TitleManifest assets/titles/hst-ucus98701.json -GameName hst -Action VisualOracle -Route logs/route_E_deep_return_20260725.pad `
    -ExitAtVblank 44000 -SnapEvery 60 -SnapWindows '8300-9200,35500-44000' `
    -OracleName deep_return_run1
```

`SR_FBSNAP_WINDOWS=<a>-<b>[,<c>-<d>...]` (up to 8 ranges) captures only inside the listed vblank
ranges. With windows active, files are named by the vblank that produced them
(`frame_v<vcount>.ppm`) rather than rotating through 8 slots — so neither window can overwrite the
other, and every file records when it was taken. Without windows the rotating name is unchanged,
so existing routes and tooling are unaffected. `-SnapEvery` still sets the cadence inside a
window. Like the other controls this is a host-side gate: no guest work is skipped and captured
frames are byte-identical to an ungated run.

`SR_FBSNAP_WINDOWS` is self-sufficient. The full contract:

| `SR_FBSNAP` | `SR_FBSNAP_WINDOWS` | Captured presents |
| --- | --- | --- |
| unset or empty | unset | none |
| unset or empty | set | every present inside the windows (`N` = 1) |
| `N` >= 1 | unset | presents at least `N` vblanks apart |
| `N` >= 1 | set | presents at least `N` vblanks apart, inside the windows |
| `0`, negative or non-numeric | either | none: an explicit `0` still disables FBSNAP |

`SR_FBSNAP_AFTER` applies in every row, and `SR_FBDUMP` still takes the capture slot when it
is set. A window whose text does not parse leaves no window active (reported on stderr as
`FBSNAP_WINDOWS: could not parse`), so it selects nothing by itself.

FBSNAP/FBDUMP capture is **present-truthful** (the old `sdl3vk_capture_swapchain_ppm`
was an invalid acquisition that could read a stale/undefined image and published a PPM under a
`.png` name). The capture is armed *before* the present and published by whichever presenter
shows the frame, through the presenter-neutral capture service (`src/rt/fbcap.{h,c}`): the
SDL3/Vulkan presenter records a readback inside the same command buffer that blits the displayed
frame, while the headless `SR_VIDEO=offscreen` sink and the GDI window publish the converted frame
they accepted. Either way the file is an exact P6 `.ppm` published atomically
(`build/snapshots/frame_<n>.ppm`, or `frame_v<vcount>.ppm` with windows), and the same guest frame
produces the same bytes on every presenter. A frame whose present did not run (its output slot was
skipped) is reported `SKIPPED` and is never published later with newer pixels; with no presenter at
all (no `--gui`) nothing is armed. The legacy guest-VRAM
`snap_*.ppm`/`snap_v*.ppm` files (route evidence via `dump_fb_fmt`) are still written unchanged.
`SR_FBDUMP=<N>` publishes the presented frame as `present_source.ppm` and exits; the exit status is
0 only if a capture was truly published, 1 otherwise (the run must not be claimed as captured when
nothing was written). `gpu-capture-selftest` (Verify step 15) byte-checks both CPU- and GPU-source
captures and asserts zero validation-layer errors under `SR_VULKAN_VALIDATION`;
`fbcap-selftest` (part of `native-core-tests`) byte-checks the presenter-neutral publisher
itself, with no GPU or window.

#### Headless VRAM capture and PNG export

Set `SR_VRAMDUMP=<vblank>[,<vblank>...]` and `SR_VRAMDUMP_DIR=<existing-directory>` to capture at up to eight unique vblank numbers. A request is serviced only when that vblank presents a display frame; an unpresented selection is reported as `NOT_CAPTURED`. Each serviced selection writes `vram_<vblank>.bin` (the complete 2 MiB PSP VRAM image) and `vram_<vblank>.json` (display and GE draw framebuffers, depth buffer, bound texture-level registers, and loaded CLUT metadata/data). This is off by default and host-side. A GPU backend may materialize pending render-target state at the capture boundary before copying it. A texture level outside the captured VRAM image is marked `in_vram: false` and cannot be exported from that snapshot; report it as an unavailable VRAM surface under issue #314.

Decode one surface from a raw image with `nk_cli vram`:

```powershell
nk_cli vram vram_120.bin --addr 0x04000000 --width 480 --height 272 `
    --stride 512 --format 8888 --output display.png
nk_cli vram vram_120.bin --addr 0x04020000 --width 64 --height 64 `
    --stride 64 --format CLUT8 --clut-addr 0x04030000 `
    --clut-format 5650 --output texture.png
nk_cli vram --from-sidecar capture/vram_120.json --out-dir capture/png
```

Supported formats are `5650`, `5551`, `4444`, `8888`, `CLUT4`, `CLUT8`, `CLUT16`, `CLUT32`, `DXT1`, `DXT3`, `DXT5`, and `DEPTH16`. `--swizzled` applies PSP swizzle addressing to non-DXT textures. Sidecar mode exports each named VRAM surface, including the loaded palette and a grayscale depth preview. The CLI compiles the existing GE sampler into a temporary decoder with GCC, so Windows users need UCRT64 GCC on `PATH`.

`mingw32-make --no-print-directory display-smoke-run` checks raw-to-PNG pixels against the display smoke fixture's host-sink PPM and checks that enabling the capture leaves that PPM byte-identical. This headless check uses the existing `SR_FIRST_FRAME_DUMP` host-sink PPM; `SR_FBSNAP` and `SR_FBDUMP` captures are published from the same `SR_VIDEO=offscreen` sink as well. The interactive in-player VRAM panel remains UNBUILT; the headless viewer is the partial capability tracked by [#314](https://github.com/Jstar269/nakagawa-recomp/issues/314).

#### Where `SR_EXIT_AT_VBLANK` actually stops

It is the **last statement of `sr_vblank_tick()`**. At that point vblank *V* is complete in
everything that function owns: the frame counter is advanced, `ge_set_frame(V)` has run,
`sr_ctrl_sample()` has latched *V*'s controller sample (so a pad-script press scheduled *for* V is
delivered before the exit), and the latch assist and no-frame watchdog have run.

It does **not** wait for work outside the tick. Guest threads this vblank resumed run after it
returns, and a frame whose `sceDisplaySetFrameBuf` lands later in vblank *V* is neither presented
nor captured. So:

- schedule a route's last input comfortably before *V* — a few hundred vblanks of settle;
- expect the last useful capture to come from an earlier vblank than *V*.

The controls are host-side only. The guest executes every vblank exactly as it would under
`Run`; pacing is untouched, no guest work is skipped, and captured frames are byte-identical to
the ungated run. **Do not** reach for `SR_NOVBPACE=1` to speed a route up: that is turbo mode, it
jumps over idle delay waits, and it demonstrably changes game speed (it is the behaviour vblank
pacing was added to fix). A faster route is worthless if it is not the same route.

`tools/nk_safety.ps1` holds the helpers whose failure modes are silent (bounded wait,
archive reset, completeness verdict); `tools/test_visual_oracle.py` exercises them against real
processes and directories.

#### Judging a long run

One run of any length is only evidence if something states what "healthy" meant.
`tools/soak_audit.py` takes the three artifacts a run already produces — `logs/perf.csv`
(one row per wall second), `logs/stderr_run.log`, and a process-metrics CSV of
`working_set_kb` / `private_bytes_kb` / `handles` sampled on a timer — and answers one
question per assertion with a number:

```powershell
python tools/soak_audit.py --perf logs/perf.csv --proc proc.csv --stderr logs/stderr_run.log --audio
```

| Check | Fails on |
| --- | --- |
| `presenting` | fewer than `--min-presenting` of the seconds after the first presented one, or a gap longer than `--max-stall-s` |
| `cadence` | vblank Hz below `--min-hz` at the 5th percentile of the last `--cadence-tail-s` presenting seconds |
| `memory_working_set`, `memory_private`, `handles` | growth above `--max-growth-pct` after `--warmup-s` samples, **peak** included so a spike that shrinks back still fails |
| `audio` | dropped frames or failed callback puts; the no-host-audio backend's underruns/overruns above `--max-underruns` |
| `audio_drift` | un-consumed audio above `--max-drift-ms` at any point, the queue ending more than `--max-drift-net-ms` ahead of where it started, or a queue that rose at every single window |
| `route` | a route program that ran without reporting `ROUTE_OK` |
| `fatal` | `FATAL`, `ROUTE_FAIL`, `UNRESOLVED_DISPATCH`, watchdog or access-violation markers |

Output is one `SOAK_CHECK:` line per assertion plus a `SOAK_AUDIT:` verdict line, and the exit
status is non-zero if any assertion failed. Two rules keep it honest: an input it was not
given reports `SKIP` with the reason rather than passing, and a quantity the runtime does not
publish is reported as absent.

**Audio drift** is the one that was unmeasurable until the telemetry published it. The runtime
reads `sr_audio_queued()` — the queue depth the blocking output paces against — in
`src/rt/hle.c`, in the same place it computes the delay from it, and prints one reading per
300 delivered vblanks (about five seconds) behind `SR_AUDIOSTAT`:

```text
AUDIOSTAT_LEAD: vbl=9300 ch=8 outputs=268 queued=8820 lead_ms=200
```

Each host mixer prints the same pair in its own end-of-run or per-window line
(`AUDIOSTAT_WIN` from the mixing backend, `AUDIOSTAT_HOST` from the per-channel one), so a
build has whichever its mixer provides plus the runtime's own:

```text
AUDIOSTAT_WIN: vbl=927 frames=220500 nonzero=89565 duty=40% pushed_total=622848 queued=4410 lead_ms=12
AUDIOSTAT_HOST: state=active driver=wasapi pushed=9841152 ... peak_q=22050 queued=4410 lead_ms=100
```

`lead_ms` is the number that matters over a long run: a queue that only grows is drift, which
is audible as lag building over minutes even while nothing is dropped, and once it reaches the
ring's capacity the push clamps and real guest audio is lost. `queued` is the deepest lead any
channel carried, not the last reading, and `-1` in either field means the backend had no host
queue to be ahead of — absence of a measurement, never a zero. Because a
drift number nobody can parse is the same as no drift number,
`tools/test_soak_audit.py` reads all three format strings out of their C sources and feeds
them to the audit's own patterns, so renaming a field in C fails a test instead of a soak.
`mingw32-make audio-selftest` checks the arithmetic the per-channel mixer publishes it from,
and the HLE selftest's `test_audio_drift_window_reports_the_pacing_value` covers the window the
runtime's own line prints.

The seconds before the guest owns its first frame are the runtime's own index
scan: they are excluded from `presenting` and never counted as a stall.

### Filesystem & I/O (→ SR_DBG_FS)

| Variable | Description |
| ---------- | ------------- |
| `SR_IOLOG=1` | Log file open/read/write operations |
| `SR_STATLOG=1` | Log stat operations |
| `SR_PATHHEX=1` | Log path hex values |
| `SR_UMDDUMP=1` | Dump UMD data |

### Display & Video (→ SR_DBG_VIDEO)

| Variable | Description |
| ---------- | ------------- |
| `SR_VBLOG=1` | Log vblank events |
| `SR_FBSNAP=N` | Every N vblanks: legacy guest-VRAM `snap_<n>.ppm` (route evidence) plus present-truthful P6 `build/snapshots/frame_<n>.ppm` (see below) |
| `SR_FBSNAP_AFTER=V` | Suppress every capture before vblank V (host-side gate only) |
| `SR_FBSNAP_WINDOWS=a-b[,c-d]` | Capture only inside these vblank ranges; names files `frame_v<vcount>.ppm` so windows cannot overwrite each other (legacy `snap_v<vcount>.ppm` still written). Without `SR_FBSNAP` it captures every present inside the windows |
| `SR_EXIT_AT_VBLANK=V` | Terminate cleanly (status 0) at the **end** of vblank V's tick (see above) |
| `SR_FBDUMP=N` | At vcount=N publish the presented frame as `present_source.ppm` and exit; status 0 only if a capture was truly published, else 1 |
| `SR_VRAMDUMP=V[,V...]` | Capture the raw 2 MiB guest VRAM image and GE metadata at up to eight unique presented vblanks; pair with `SR_VRAMDUMP_DIR` |
| `SR_VRAMDUMP_DIR=PATH` | Existing directory for `vram_<vblank>.bin` and `.json` capture files |
| `SR_NOVBPACE=0\|1` | Vblank pacing mode: unset, empty, or "0" = paced (default); "1" = turbo. Any other value fails closed at startup |

For a replayable GE fixture, set `SR_GE_CAPTURE_FRAME=<vblank>` or
`SR_GE_CAPTURE_FRAME=<first>-<last>`. The range form captures the first submitted frame in that
bounded window that meets `SR_GE_CAPTURE_MIN_PRIMS`; it is useful when a deterministic input route
does not submit a display list on exactly the same vblank every run. `SR_GE_CAPTURE_PATH` selects
the private `.ngef` output path. Captures remain private game-derived inputs and must not be
committed.

### Misc (→ SR_DBG_MISC)

| Variable | Description |
| ---------- | ------------- |
| `SR_FONTLOG=1` | Log font operations |
| `SR_MPEGLOG=1` | Log mpeg operations. The PSMF media producer also logs one bounded counter line per reporting interval: bytes read, packs, PES packets, per-track PES and access-unit counts, access units that carried no presentation time, resync bytes, both compressed-queue depths, EOF state and failure offset, the disposition census of both tracks (submitted, decoded, delivered, warm-up-held, eos-drained, rejected — plus audio format rejects `arejf` and mono upmixes `aupm`), decoder-error counts, the pipeline presentation clocks (`vpts`/`apts`), the guest-facing presentation points at the getters (`vts`/`ats`), the A/V separation between those comparable points (`avgap`, `vts-ats`), submitted-minus-delivered (`vlead`, the pipeline distance inside the output ring), displaced ring times (`vlost`), the last `displaypts`, and the drain flags — enough to classify every decoded picture and locate the first boundary that stops a movie without attaching a debugger |
| `SR_DLGLOG=1` | Log dialog operations |
| `SR_CBLOG=1` | Log callback operations |
| `SR_SYSLOG=1` | Log system calls |
| `SR_WAKELOG=1` | Log thread wakeup events |
| `SR_HLE_DIAGNOSTICS=1` | Enable the retained title-scoped HLE diagnostic reads only for a validated `codegen_profile: "hst"` build; generic and public fixture profiles remain inert |
| `SR_REAL_MODULE_START` | `sceKernelStartModule` entry gate, tri-state. `1` runs a translated/real `module_start` for any module; unset (the default) runs it for `libfont.prx` only, so the guest owns its own readiness while `psmf.prx`, `libpsmfplayer.prx` and unknown paths keep the host bypass; `0` is the environmental kill switch and runs no `module_start` for any module, For libfont, disabled or unavailable startup reports `LIBFONT_STARTUP_UNAVAILABLE`, returns `SCE_KERNEL_ERROR_NOTIMP`, and does not write readiness. Any other value is refused with one diagnostic and the default is used. `sceKernelStopModule`/`sceKernelUnloadModule` still require `1` |
| `SR_FONTDIR=ABSOLUTE_PATH` | Override font directory (relative values are rejected; unset uses the executable's sibling `font`) |
| `SR_DATAROOT=ABSOLUTE_PATH` | Override the extracted-XB data root (relative values are rejected; unset uses the executable-anchored HST tree). The executable-anchored root and walked descendants reject reparse points; an explicitly configured root is operator-trusted and may be a junction for a staged long-path fixture. Access-time replacement races inside that trusted root are not a containment boundary. |
| `SR_FSDIR=PATH` | Legacy flat `fs/` source for one-time read-open import into the unified Memory Stick root; relative paths, including `.`/`..`, are resolved against the current directory. Write/create never creates under this root |
| `SR_MEMSTICK=PATH` | Canonical host Memory Stick root shared by ordinary `sceIo*` `ms0:` I/O and savedata (default `memstick/`) |
| `SR_SYSTEM_REGISTRY=PATH` | Overlay file of the virtual PSP system registry: every `sceReg` change a game saves with `sceRegFlushCategory`/`sceRegFlushRegistry`, as schema-versioned JSON replaced atomically (default `registry/system.json` in the per-user data directory). A corrupt file is reported, ignored, and moved to `<file>.corrupt` on the next save |

### Scheduling & Behavior

| Variable | Description |
| ---------- | ------------- |
| `SR_VBLANK_Q_US=N` | Host-clock vblank quantum in microseconds. In the default paced profile it does **not** produce vblanks -- the scheduler's rational 60000/1001 source is the only producer -- and only sets the threshold at which a yield is counted as late vblank *service* (reported as `vbl_late_service_yields` by the spin watchdog). It is an out-of-band vblank source only under `SR_NOVBPACE=1` |
| `SR_WATCHDOG_EXIT=N` | Abort after N vblanks with no new frame; a firing is a NO-NEW-FLIP observation, not a hang verdict -- classify it with the display counters, thread dump, and MPEG/PSMF activity the watchdog prints |
| `SR_NO_RELAUNCH=1` | Disable thread relaunch |
| `SR_NO_THREAD_REUSE=1` | Disable thread reuse |
| `SR_NOAUDIO=1` | Disable audio output |
| `SR_AUDIODUMP=PATH` | Debug only, off by default. Write the device mix (every channel's stream after the master gain, as SDL is about to submit it) to a 16-bit PCM WAV at the device's own rate and channel count. The header is patched once per second of audio data and at exit, and a stats line (`AUDIODUMP: ... frames= silent_frames= clipped_samples= peak=`) is printed at close. It runs on SDL's audio thread and does file I/O there, so it changes host pacing; never compare timing with it on. Audit the file for silence, clipping, gaps and sample rate |
| `SR_POSTUMD=1` | Post-UMD processing |
| `SR_HEAP_BASE=HEX` | Override heap base address |
| `SR_PARTITION_TOP=HEX` | Override partition top |
| `SR_CALLCOUNT=1` | Enable call counting |
| `SR_CBLOG=1` | Log callback create/register/notify/dispatch to stderr |
| `SR_PGD_KEYS=PATH` | Not read by public runtime builds: PGD-protected data is unsupported and the PGD backend is excluded; broader ISO-to-Play support is in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)) |

There is no `SR_HLE_CONTINUE` switch. In a scheduled game run, an unimplemented NID is fatal;
returning zero would turn an unknown operation into phantom success. Register the NID with real
semantics. Non-PLT dispatch misses are never silent and carry no switch: the miss is logged
(`DISPATCH_MISS_NEW`), only analyzer-owned executable spans may execute, and any rejection
terminates the run.

## Interpreting the audio-thread semaphore trace

PSP thread UIDs are allocated at runtime and can change between runs. Identify this worker by its
guest entry address, `0x00082a14`, rather than by a UID such as `0x133` or `0x134`.

The generated guest logic disproves the earlier "WaitSema retries without SignalSema" theory:

- helper `0x00086d9c` waits at `0x00086df0` and unconditionally signals at `0x00086e54`;
- helper `0x00082f14` waits at `0x00082f5c`, scans state, and unconditionally signals at
  `0x00082fbc`; only its `sceSasCore` work is conditional.

Generic `HLE: calling` lines are emitted once per `(uid, nid)`, while the WaitSema handler can
emit a line on each call. A repeated `count=1 need=1` line therefore does not show that no signal
ran between waits; it shows an immediately satisfiable wait. Trace both handler bodies or the
guest PCs before inferring a missing branch.

## Audio-observation sampling traps

Two sampling traps produced a false "voice audio is silent" diagnosis for a short title/logo
voice sting. Both are methodology lessons, not game-specific behavior:

- **Power-of-two-only push sampling can miss short audio windows entirely.** The `AUDIO_PUSH`
  logger under `SR_AUDIOLOG=1` samples the first 16 calls and then only power-of-two call counts.
  A short voice/SE window (tens of vblanks) can fall entirely between sampled calls, so "every
  sampled push was peak 0" does not prove silence. When testing a short audio event, log by
  semantic trigger (keyon) or over a bounded vblank window around the event instead.
- **A VAG voice slot can legitimately begin with a zero block before real ADPCM data.** Dumping
  only the first 16 bytes at keyon (or decoding only the first 28-sample block) can classify a
  live stream as zero/silent. The decoder's second block at source offset `+0x10` is the first
  data block. Under `SR_SASLOG=1`, `SAS_VAG_B0`/`SAS_VAG_B16` capture both blocks per voice
  (bounded, two per voice), and `SAS_MIX_V` shows whether voice position advances across grains.

## CLUT start offset

Do not change the renderer's `((clut_fmt >> 16) & 0x1f) << 4` start calculation based on the old
scan-note hypothesis. PPSSPP's
[`getClutIndexStartPos`](https://github.com/hrydgard/ppsspp/blob/f0baf3ade7bcb6c86f0835962b36eb4e51559d8f/GPU/GPUState.h)
uses the identical expression: the start field selects a 16-byte unit, not an individual
palette entry. Both renderers share this decode path. A palette bug still needs draw-level
evidence, but `<< 4` itself is not one.

## Memory Watch System

The debug framework supports watching specific memory address ranges. When `SR_DBG_MEM` is enabled, any read/write to a watched address is logged.

### Startup Instruction Trace Window

`SR_TRACE_PC=<low>:<high>` limits the instruction trace to the inclusive guest-PC range. The runtime
arms this window during initialization, so it can capture startup code before the first vblank; later
vblank markers place records on the frame timeline. Use a trace-enabled build or the manager's
instruction-trace option to open the underlying instruction trace. Set `SR_TRACE=<path>` to choose
the window output file and `SR_TRACE_LIMIT=<count>` to stop recording after a bounded number of
records.

### Range Watch Without Code Changes

For a temporary range watch without editing the runtime's watch table, set
`SR_WATCH=<address>:<length>`. The range is half-open, `[address, address + length)`, and may be written in decimal or with a
`0x` prefix. Each AOT or interpreter store that overlaps it is reported to stderr with its guest
PC, starting address, value, width in bytes, and most recently delivered vblank:

```powershell
$env:SR_WATCH = '0x08800000:4'
```

Output uses `SR_WATCH: pc=... addr=... val=... width=... vblank=...`. A vblank value of zero
means no vblank has been delivered yet. The watch is inert when unset; the shared store helpers
take only a predicted-false flag check in that case.

### Adding Watches in Code

```c
#include "debug.h"

// At startup, add watches:
sr_add_mem_watch(0x002cf6dc, 0x002cf6e0, "asset_bucket");
sr_add_mem_watch(0x002de908, 0x002deb60, "param_name_bucket");
```

### Programmatic Usage

```c
// Check if an address is watched (returns 1 if logged)
sr_check_mem_watch(addr, val, 1/*write*/, pc);

// Or use the macro for conditional logging
if (SR_DBG(SR_DBG_MEM)) {
    dbg_mem(addr, val, 1/*write*/, pc);
}
```

When a transient buffer's address changes between runs, watch the written value instead:

```powershell
$env:SR_VALUE_WATCH_0 = '0x440b4000,PANEL_X0'
```

`SR_VALUE_WATCH_0` through `SR_VALUE_WATCH_15` accept an unsigned 32-bit value and label.
Matches are reported as `MEM_VALUE_WATCH[...]` with the destination address, value, and guest PC.
Address and value watches share the 16-entry watch table. Value watches are diagnostics only and
do not change guest memory or execution. Configuring a watch explicitly enables its focused log;
`SR_DEBUG=MEM` is not also required.

For a bounded register snapshot at one exact writer, add
`SR_WATCH_CONTEXT_PC=0x<guest-pc>`. `SR_WATCH_CONTEXT_LIMIT` defaults to one and may be set from
1 through 1024. A snapshot is emitted only when an address/value watch matches at that PC; it
includes all GPR/FPR raw values and does not pause or alter guest execution. This is useful when a
dynamic buffer value identifies the final writer but its source operands must be traced further.
For a writer shared by several values, `SR_WATCH_CONTEXT_FPR=<index>,0x<raw-value>` adds an exact
raw FPR-bit condition (for example, `20,0x436e0000` selects `f20 = 238.0f`).

When the value itself is common enough that a value watch would flood a long route, use
`SR_STORE_CONTEXT_PC=0x<guest-pc>` instead. It emits a bounded register snapshot only for a store
originating at that exact generated-code PC, without logging other writes. `SR_STORE_CONTEXT_LIMIT`
defaults to one and accepts 1 through 1024. `SR_STORE_CONTEXT_MEM=<gpr>,<offset>,<words>` optionally
dumps 1 through 32 guest words relative to a GPR; for example, `16,0x24,7` snapshots seven words
starting at `r16 + 0x24`. These switches are observational and do not pause or modify execution.

To correlate a decoded through-sprite with the guest code that builds its dynamic vertices, set
`SR_GE_ARM_RECT=x0,y0,x1,y1`. When GE observes that exact integer rectangle it adds deduplicated
write watches for both source vertex records. Later reuse of those records is reported through
the normal `MEM_WATCH[...]` log with the exact guest writer PC. The shared 16-range watch limit
still applies, and the option never changes vertex data or rendering.

## Flight Recorder (`SR_FLIGHT`)

`SR_FLIGHT=<classes>;<limit>` retains the last `<limit>` (at most 4096) structured events of the
named classes (`hle`, `unsupported`, `sched`, `prx`, `fault`, `fatal`, `media`, `ge`, `present`)
and writes a bundle to `SR_FLIGHT_OUTPUT` (default `flight-recorder.json`). The schema is
`assets/flight_recorder_schema.json`; `tools/flight_diff.py` validates and compares bundles.

`terminal.reason` names what ended the run:

| Reason | Meaning |
| --- | --- |
| `running` | No terminal yet. The bundle is rewritten at each named refusal (at each power of two of the refusal count), so a run killed later reads as still running. |
| `exit` | Normal exit; `arg0` is the exit status. |
| `budget` | `SR_EXIT_AT_VBLANK` was reached; `arg0` is the vblank. |
| `hang` | The no-frame watchdog (`SR_WATCHDOG_EXIT`) aborted; `arg0` is the vblanks without a new frame. |
| `fatal` | A fatal event; `kind` names it. |
| `unsupported-nid` | An NID with no handler ended the run (`kind` 13). |

A named refusal is a call answered with its registered error while the guest keeps running. It is
never the terminal record. It is an `unsupported` event, counted in the bundle's `refusals` block:
`count`, `first_nid` and `first_pc` (the first refusal, in its own fields), and `nids`, the distinct
refused NIDs in first-seen order with a count each (up to 32; further NIDs are counted in
`nids_unlisted`). Schema 1 to 4 bundles froze at their first refusal (`unsupported-nid`, kind 2), so
they cannot show what ran after it. The private triage tooling reports those as frozen at
a refusal; that grouping is not part of this repository.

## Crash Reporter

When the program crashes (access violation, etc.), the crash reporter dumps:

1. **Exception info** — Exception code and host address
2. **Fault address** — Mapped to guest address if in arena
3. **Memory watches** — Shows if fault address is in a watched range
4. **Host registers** — Full x64 register state (RIP, RSP, RAX, etc.)
5. **Guest registers** — Full PSP CpuState (PC, SP, RA, all GPRs, HI/LO, FCR31)

Example output:

```text
=== PSP RECOMPILER CRASH REPORT ===
Exception: 0xc0000005 at host 0x00007ff6abc12345
Fault: READ of host 0x000001a3b5c00000 -> guest 0x0830b000 [WATCHED: asset_bucket]

--- Host Registers ---
RIP=0x00007ff6abc12345  RSP=0x000000f1a3b00000
RAX=0x0000000000000000  RBX=0x000001a3b5c00000
...

--- Guest CpuState ---
PC=0x00222a00  SP(r29)=0x09f00000  RA(r31)=0x00222b00
r4=0x0830b000  r5=0x00000100  r6=0x00000004  r7=0x00000000
...
=== END CRASH REPORT ===
```

## Debug Output Format

All debug output goes to stderr with consistent formatting:

```text
CATEGORY: key=value key=value ...
```

Examples:

```text
MEM_R: addr=0x0830b000 val=0x12345678 pc=0x00222a00
HLE: nid=0x00000001(sceDisplaySetFrameBuf) pc=0x00222b00
SCHED: create_thread uid=0x111 entry=0x00222a00 pri=32 stack=0x2000
GE: list_submit addr=0x04000000 stall=0x00000000
INPUT: buttons=0x00000000
FS: Open(./sce_lbn0x0e0f) -> 0x00000000
VIDEO: present fb=0x04000000 fmt=3 stride=512
```

## In-Game Performance Overlay (HUD)

The Vulkan runtime (`src/rt/gpu_sdl3vk/`) includes an opt-in in-game performance overlay (HUD) that can be toggled at runtime or enabled at process start:

- **Startup Switch**: Launch with `SR_HUD=1` to have the overlay enabled at startup.
- **Runtime Toggle**: Press `F1` while the game window is focused to toggle the overlay on or off.
- **Telemetry Displayed**:
  - **FPS & Frame Time**: Presented frames per second and average millisecond latency per presented frame.
  - **VBlank Rate**: Cadence of the scheduler's VBlank ticks in Hertz.
  - **Audio Status**: Whether the host audio stream is actively producing output (`Active` vs. `Inactive`).
- **Timing and Performance Guarantees**:
  - Reuses the low-overhead counters already tracked by `SR_PERF`; no parallel measurement system or polling thread is introduced.
  - When disabled, overhead is a single branch check (`if (s_hud_enabled && ...)`), executing zero extra Vulkan commands and performing zero extra GPU work.
  - When enabled, host-side drawing renders text onto an SDL3 surface after the guest frame is composed and copies it directly into the swapchain staging buffer, guaranteeing zero impact on guest-visible timing or emulation clocks.
  - Snapshot captures (`SR_FBSNAP` / visual evidence capture) capture the pristine guest framebuffer before the presentation blit, keeping automated tests and snapshots free of HUD artifacts.

## Troubleshooting

### Black screen

1. Set `SR_DEBUG=0x0D` (MEM + SCHED + GE) to trace boot sequence
2. Check for HLE calls that halt: look for `HLE:` lines followed by process exit
3. Verify thread scheduling: look for `SCHED:` lines showing thread creation

### Crash on startup

1. Check crash report for guest PC — this shows where execution stopped
2. Look for `MEM_R` or `MEM_W` lines just before the crash
3. If address is out of range, the game may need additional memory regions

### Performance issues

1. Avoid `SR_DEBUG=0xFF` in production — causes massive stderr output
2. Use specific categories: `SR_DEBUG=0x02` for HLE tracing only
3. Set `SR_WATCHDOG_EXIT=N` to abort after N vblanks without a newly presented frame; the firing is a NO-NEW-FLIP observation, so classify it with the display counters, thread dump, and MPEG/PSMF activity the watchdog prints, not the threshold alone
