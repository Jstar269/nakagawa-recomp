# Source-owned PSP oracle probes

This directory contains source-owned PSPDEV/PSPLink fixtures. They are
synthetic and independent of the HST executable, retail assets, firmware
files, keys, and private traces.

The fixture prints the versioned `NAKAGAWA_PSP_META` and
`NAKAGAWA_PSP_TEST` records defined in
[`tools/psp_oracle/protocol.py`](../../tools/psp_oracle/protocol.py). Every
campaign case also writes each line to its own host0 log through one durable
writer (`probe_emit_durable()`: append, then close). The log is named as
`_campaign_host0_log_path` in `tools/psp_oracle/run_psplink.py` expects: the
case id with `-` turned into `_` and a `dma_` prefix turned into `dmac_`, plus
`_log.txt` (for example `host0:/smoke_log.txt`). The campaign runner reads that
log; PSPLink stdout is a secondary copy. Before a
call that could hang or fault the console, a probe also writes a
`NAKAGAWA_PSP_STEP schema=1 case_id=<id> step=<name>` progress marker
(`probe_step()`), appended to the host0 log and closed before the call runs,
so a launch that never returns still names its last step. The parser collects
step markers (`ParsedOutput.steps`, `last_step`) and never counts them as
records. A probe never waits without a bound: GE waits poll the non-blocking
sync peek against a deadline, kernel waits carry timeouts, and exhaustion loops
stop at a documented cap; a bound that expires is recorded as a `TIMEOUT`
outcome with the last observed state. The
default `CASE=smoke` build emits `PSP-SMOKE-001`; kernel sessions build one
case per launch with `CASE=callback-notify-check`, `CASE=wait-cancel`,
`CASE=thread-lifecycle`, `CASE=thread-delete-lifecycle`,
`CASE=thread-delete-followup`, `CASE=thread-delete-explicit`, or
`CASE=thread-delete-boundary`. DMA sessions use `CASE=dma-concurrency`, the
Tier S/B `CASE=dma-invalid-tail-*` cases described below, or the size-matrix
cases described below. Display/interrupt-mask
sessions use `CASE=display-mask-vcount`, `CASE=display-mask-duty`, or
`CASE=display-ge-mask`. Transport sessions use `CASE=transport-write`, which
emits `PSP-TRANSPORT-001`/`host0-write-readback`. Lifecycle sessions use
`CASE=thread-exit-delete` (`PSP-THREAD-EXIT-001`, 12 cells). Allocator
sessions use `CASE=dmac-survey` (`PSP-DMAC-001`/`allocator-survey`). The
thread-free `CASE=dma-size-matrix` control allocates both spans from partition
2 with page-aligned data and guard redzones, separating a true API-wide size
limit from invalid-tail boundary behavior. `CASE=dma-size-matrix-cell`
requires `DMAC_SIZE_REQUEST=<supported-size>` and runs one requested size per
session. The thread-free
`CASE=model-profile` control records the raw `kuKernelGetModel()` PSPSDK/
kubridge ordinal, `sceKernelDevkitVersion()` word, and CPU clock. A host-side
decoder maps the ordinal to a generation and retail family; it never applies
the PSPSDK enum table to the kernel-only `sceKernelGetModel()` original/slim
convention.

Bounded follow-up cases for issues #69, #311, and #303 are `CASE=ge-nan`,
`CASE=audio-query`, and `CASE=dma-cells`; these remain `NOT_RUN`. The
`CASE=delay-zero` records were measured on the accepted 2026-10-01 PSPLink
campaign and are indexed in `docs/HARDWARE_ORACLE.md`.

## Transport write-readback (`CASE=transport-write`)

The PSP writes a fixed 64-byte pattern (byte `i` =
`(0x5A ^ (i * 0x25 + (i >> 3))) & 0xFF`) to the probe-owned disposable path
`host0:/nakagawa_transport_write.bin`, reads it back, and reports FNV-1a,
written/read-back sizes, a self-match flag, and the CPU clock in MHz
(`scePowerGetCpuClockFrequencyInt`). The host independently regenerates the
pattern and compares bytes and SHA-256. Only that one path is touched; the
host removes it after verification. `status=PASS` means the PSP-side
write/read-back matched; file acceptance additionally requires the host-side
byte/SHA comparison.

## Thread exit/delete boundary (`CASE=thread-exit-delete`)

Twelve cells: implicit return / `sceKernelExitThread` /
`sceKernelExitDeleteThread` across `0x77`, `0`, `-17` (`0xffffffef`), and
`0x800201ac`. Each cell records the `WaitThreadEnd` return, the
`ReferThreadStatus` exit status and thread state, and the raw results of a
post-mortem delete and a restart attempt. `ExitDeleteThread` cells
specifically measure whether the UID is gone (delete/restart raw codes) and
what a joiner observes. `result` is the `WaitThreadEnd` return; `PASS` means
the harness completed, not that any outcome matched an expectation. One
thread per cell, sequential, immediate exits; accepted restarts are
waited on and deleted so the launch leaks nothing in-process.

## Allocator boundary survey (`CASE=dmac-survey`)

An earlier invalid-tail probe's compiled-in user-end assumption
(`0x0A000000`) was rejected by the firmware on 64 MiB units (partition 2
allocates there), so that historical shape skipped by design. This case scans
partition-2 fixed-address allocatability upward from the old assumption in
64 KiB steps (32 attempts), freeing every success immediately, and emits the
highest provable base plus the first failure. Thread-free and write-free;
failures are ordinary error codes. It settles no DMA semantics. The current
bounded Tier S/B cells are described below.
Display-wait sessions use `CASE=display-wait-late`,
`CASE=display-wait-priority`, or `CASE=display-vblank-window`. Plain-mutex
sessions use one of the four `CASE=mutex-*` cases described below.

The thread-delete follow-up is a bounded two-control probe for the
second-order `sceKernelWaitThreadEnd` discrepancy: semaphore handshakes prove
both joiners entered and completed the inner wait on a terminate-deleted
target, one returns the error-shaped inner result and the other explicitly
returns `0x77`, and each `SceKernelThreadInfo` state is recorded after the
outer wait. The explicit sibling uses the same synchronization but calls
`sceKernelExitThread(0x800201ac)` and `sceKernelExitThread(0x78)` directly.
The PSP-3001/6.61-ARK controls establish that signed-negative status values
(`0x800201a8`, `0x800201ac`, and ordinary `-17`) normalize to `0x800200d2` on
the measured non-delete exit paths, while ordinary positive values propagate
unchanged. `ExitDeleteThread` is deliberately outside this measurement. The
raw hardware stream remains local until its current-session model/firmware
metadata is confirmed.

The bounded `thread-delete-boundary` case passes
`SCE_KERNEL_ERROR_WAIT_TIMEOUT` (`0x800201a8`) through explicit
`sceKernelExitThread`, then uses the existing ordinary `-17` control. It is
intentionally limited to those two boundary values; it is not an error-code
matrix.

All records contain only scalar arithmetic and API results; pointers and raw
memory are never treated as stable evidence.

## Display-wait cases (issue 70)

Three cases answer what a PSP display wait actually does, because
`docs/PSP_INTR_WAITS_MATRIX.md` recorded the normal-context rows for both NIDs
as `hardware = unknown / WOULD_BLOCK control`: only the error cells had ever been
measured. Each is bounded by both an elapsed-system-time test and an iteration
cap, so a stopped clock yields a finite record rather than a hang, and none of
them touches game content or firmware state.

`CASE=display-wait-late` (`PSP-DISPLAY-002`) phase-aligns to an edge, busy-spins a
controlled fraction of a calibrated period without any voluntary yield, then
times the call under test. Offsets of 2/8, 6/8, 10/8, 14/8 and 20/8 of a period
straddle one and two boundaries, so "several periods elapsed while the caller was
busy" is covered rather than only a narrow late window. A separate in-vblank cell
polls `sceDisplayIsVblank` and calls from inside the interval, which is the only
phase at which the two NIDs can differ. The period is calibrated on the same
device in the same run; nothing assumes 60000/1001.

`CASE=display-wait-priority` (`PSP-DISPLAY-003`) runs identical high-priority work
twice, once alone and once alongside an always-runnable lower-priority peer that
never issues a blocking call. Per iteration the caller samples the peer's counter
before its display wait, after it, and again after a pure-CPU spin, so progress
made while BLOCKED is separated from progress made while merely RUNNABLE. The
control/experiment pair is what distinguishes "a real block hands the CPU over"
from "the syscall behaves differently because another thread exists".

`CASE=display-vblank-window` (`PSP-DISPLAY-004`) aligns to an edge and times
`sceDisplayIsVblank`'s falling transition, recording hcount at entry and exit so
the interval is expressed in the display's own units as well as microseconds.

## Issue 23 DMA cases

`CASE=dma-concurrency` runs three 64-trial API combinations:

- `sceDmacMemcpy` then `sceDmacTryMemcpy`;
- `sceDmacTryMemcpy` then `sceDmacTryMemcpy`;
- `sceDmacTryMemcpy` then `sceDmacMemcpy`.

Each trial starts the first 1 MiB VRAM-to-VRAM call in a priority-`0x10`
thread while the main thread remains at priority `0x20`. This reproduces the
public PSPAutotests scheduling shape. It records whether the first caller had
entered, whether it had returned, whether it remained pending when the second
caller started, and whether the measured call intervals overlapped. The record
also correlates BUSY with the pending/returned snapshot: BUSY after return is
the observable needed to show that the first syscall returned while its DMA
state remained active. Timing or thread state alone is not described as
in-flight DMA proof.

The concurrency record uses `result` for the last second-caller return (or a
setup error) and these scalar outputs:

| Field | Meaning |
| --- | --- |
| `out0` | Completed trial count |
| `out1` / `out2` | First / second API (`0` blocking, `1` try) |
| `out3` / `out4` | First-entered / first-returned snapshots before the second call |
| `out5` | First-entered/not-returned snapshots before the second call |
| `out6` | Second-call start times earlier than the first-call return time |
| `out7` / `out8` | First-call zero / other return counts |
| `out9` / `out10` / `out11` | Second-call BUSY (`0x80000021`) / zero / other counts |
| `out12` / `out13` | BUSY while first pending / after first returned |
| `out14` / `out15` | Minimum / maximum contiguous transferred prefix |
| `out16` | Trials whose contiguous prefix was exactly `0xC000` |
| `out17` | Trials with non-sentinel mutation after the contiguous prefix |
| `out18` / `out19` | First-call minimum / maximum wall time in microseconds |
| `out20` / `out21` | Second-call minimum / maximum wall time in microseconds |
| `out22` | First-caller thread priority |
| `out23` | Last first-caller return |

The sequential `dma-size-matrix` records cover `0xBFFF`, `0xC000`,
`0xC001`, `0xD000`, `0xF000`, `0xFFFF`, `0x10000`, and `0x100000` (1 MiB)
for both APIs. Each API/size cell is repeated three times. Each requested data
span is inside a distinct `sceKernelAllocPartitionMemory` block from user
partition 2. Data begins at a 4 KiB aligned address, with a 4 KiB leading
redzone and at least a 4 KiB trailing redzone in each block. Blocks are freed
after both APIs finish for that size. The 1 MiB cell allocates `0x102000` bytes
per buffer, rather than borrowing the full VRAM range. The record uses `result`
for the last DMAC return and these outputs: `out0` requested bytes, `out1`
maximum contiguous copied prefix, `out2` maximum non-sentinel bytes after that
prefix, `out3` full-source integrity flag, `out4` maximum elapsed microseconds,
`out5` API (`0` blocking, `1` try), `out6` trial count, `out7` failed-trial
count, `out8` source-redzone mutation count, `out9` destination trailing
redzone mutations, `out10` destination leading redzone mutations, `out11`
data alignment, `out12` allocation bytes per buffer, `out13` partition id,
`out14` redzone bytes, `out15` source data address, `out16` destination data
address, and `out17`/`out18` source/destination block UIDs. Before every call,
the probe initializes both owned blocks and writes back then invalidates both
full block ranges. After the call it invalidates both full block ranges before
inspection. Each completed API/size record is appended to host0 and its file
is closed before the next cell. A PASS record
requires every trial to return zero, copy the complete requested prefix, leave
the requested tail and all source/destination redzones unchanged, and emit all
16 matrix records in full-matrix mode. `dma-size-matrix-cell` emits the two
API records for exactly one selected size and is used for incremental hardware
sessions; the runner validates the requested size, block ownership fields,
guard counts, and complete two-record pair before that session qualifies.
These partition-owned spans control for transfer size without assuming that
unallocated VRAM is available to the probe. The probe itself does not promote
results to public hardware evidence.

The existing runner can retain and validate one selected size after launching
the PRX. Build one `dma-size-matrix-cell` PRX for a supported request and use
the matching campaign case id; include `transport-write` first in every
session to requalify host0:

```powershell
python tools/psp_oracle/run_psplink.py `
  --host0-root fixtures/psp_oracle/build/w6-size-bfff `
  --campaign-case transport-write=fixtures/psp_oracle/build/w6-size-bfff/transport_write.prx `
  --campaign-case dmac-size-matrix-size-0x0000bfff=fixtures/psp_oracle/build/w6-size-bfff/dmac_size_matrix_cell.prx `
  --source-commit <exact-clean-commit> --model <operator-recorded-model> --firmware <operator-recorded-firmware> `
  --session-id <hardware-lock-holder-session>
```

The runner reads `dmac_size_matrix_cell_log.txt` from the host0 scratch root
after the module unloads. Use a fresh scratch root per size so each session's
host0 log and envelope remain available; the first transport failure stops
before the matrix cell runs.

The bounded invalid-tail cells use five isolated launches:

- `dma-invalid-tail-s0` (Tier S pointer/size controls for both APIs);
- `dma-invalid-tail-memcpy-dst`;
- `dma-invalid-tail-memcpy-src`;
- `dma-invalid-tail-try-dst`;
- `dma-invalid-tail-try-src`.

The Makefile keeps these variants at `PSP_LARGE_MEMORY=0`. Each allocates one
page-aligned `0x11000`-byte block from the high end of partition 2. The block
contains a `0x1000` pre-guard, the `0xC000` payload, a `0x1000` post-guard, a
`0x2000` owned overflow band, and a `0x1000` tail-guard. While holding it, the
probe uses `PSP_SMEM_Addr` to check one page at both `block_end` and
`block_begin - 0x1000`; either successful neighbor allocation makes the run
`SKIP` before DMAC. The full request is statically constrained to the owned
post-guard and overflow band. Cache writeback/invalidation brackets each
transfer. `setup_mask=0x0F` means block allocation, page geometry, end-neighbor
refusal, and begin-neighbor refusal passed (bits 0 through 3 respectively).
Any pre/tail guard mutation fails the record and its parser.

Tier S emits eight size-zero records: S0-a null destination, S0-b null source,
S0-c both pointers owned, and S0-d destination `0xFFFFFFFF`, each for memcpy
and try. Every cell must keep `P`, payload mutations, and all guards at zero.
Tier B emits four records for each existing launch, with deltas 1, 4, `0x1000`,
and `0x2000`; destination overrun bytes are classified separately in the
post-guard and overflow band. These are bounded declared-payload controls,
not actually invalid physical spans. All Tier S/B hardware results remain
`NOT_RUN`; the K3/K4 invalid-span distinction remains in the works under #303.

Terminal outcomes are deliberately distinct:

| Outcome | Required evidence |
| --- | --- |
| Result | The complete Tier S set (8 records) or one Tier B delta set (4 records) exists; retain every scalar and status. |
| Skip | An explicit `status=SKIP` record proves that a pre-call safety gate stopped the case. |
| Hang | No test record, host `process_status=TIMEOUT`, and a human observes that the device remains stalled without rebooting. |
| Reset | No test record and a human observes a device reboot/reset and PSPLink session loss. Never infer this from host process exit alone. |
| Inconclusive | No record and neither physical observation is established, including launch or transport failures. |

Run the capture with `--out <case>.runner.json`. After a no-record outcome, the
operator records the physical observation without altering the original report:

```powershell
python tools/psp_oracle/run_psplink.py `
  --annotate-report <case>.runner.json `
  --observed-terminal-outcome HANG `
  --out <case>.hang.json
```

Use `RESET` instead only after observing a reset. The annotation command
rejects any capture that already contains a test record and marks terminal
outcomes ineligible for scalar-result acceptance.

## Display / interrupt-mask cases (issue #70)

Three cases answer what happens to the display domain while CPU interrupt
delivery is masked. Build one per launch, as with every other case.

`CASE=display-mask-vcount` holds `sceKernelCpuSuspendIntr` for 4000, 16700,
30000 and 50000 us — 12 trials each — and samples `sceDisplayGetVcount`
*immediately after* `sceKernelCpuResumeIntr`. That sample is the one the
accepted interrupt-conformance record (historical tracker item #88) never took,
and it is what separates "the counter catches up by every elapsed period" from "exactly one deferred event is credited" from "the
elapsed periods are gone". The case never assumes a period length: it calibrates
the device's own vblank period in the same run over 60 `sceDisplayWaitVblankStart`
intervals and reports it in `result` and `out26`. Every spin is bounded by BOTH
elapsed system time and an iteration cap, so a stopped clock cannot turn a probe
into a hang.

One record is emitted per duration, `case_id=display-mask-vcount-<N>us`:

| Field | Meaning |
| --- | --- |
| `out0` / `out1` | Trials completed / requested mask microseconds |
| `out2` / `out3` | Minimum / maximum measured masked span |
| `out4` / `out5` | Minimum / maximum source periods the span crossed |
| `out6` / `out7` | Minimum / maximum VCOUNT delta observed *during* the mask |
| `out8` / `out9` | Minimum / maximum VCOUNT delta immediately *after* resume |
| `out10` / `out11` | Trials crediting exactly 0 / exactly 1 |
| `out12` / `out13` | Trials crediting the full period count / a partial catch-up |
| `out14` / `out15` | Minimum / maximum VCOUNT step at the next WaitVblankStart |
| `out16`–`out19` | Accumulated-Hcount delta during the mask / immediately after |
| `out20` / `out21` | Summed credited increments / summed elapsed periods |
| `out22` / `out23` | Mean span; trials in which system time advanced |
| `out24` / `out25` | Minimum / maximum current Hcount sampled under the mask |
| `out26` / `out27` | Calibrated period in nanoseconds; CPU clock in MHz |
| `out28` | Trials whose next WaitVblankStart advanced VCOUNT by exactly 1 |

`out12` and `out13` are the falsifiers. `PASS` means only that every trial ran
and the masked window really elapsed; it makes no claim about which semantic the
numbers show.

`CASE=display-mask-duty` is the same question asked as a sustained duty cycle —
repeated 33 ms or 66 ms masks separated by 2 ms servicing gaps across a 2 s
window — where the three candidate semantics separate by a factor of about four
in accumulated VCOUNT. It is built and warning-free but was not needed once the
per-trial case came back unanimous, and no result from it is recorded.

`CASE=display-ge-mask` asks whether the GE keeps executing while CPU interrupts
are masked, and is deliberately stall-gated so that "the GE simply finished
before the mask opened" cannot produce a false positive. A list of 16 chained
block transfers (512x32 px at 4 bpp, 1 MiB total) is enqueued **already stalled
at its first word**; the destination is proven still at its sentinel; only then
are interrupts suspended and the stall released. Every destination read goes
through the uncached VRAM mirror, so a stale CPU cache line cannot be misread as
"the GE did not progress". Three records are emitted:

- `ge-mask-controlB-enabled-release` — released with interrupts enabled; the
  destination MUST change.
- `ge-mask-controlA-stall-held` — masked with the stall still held; the
  destination MUST NOT change.
- `ge-mask-primary-masked-release` — released while masked; the measurement.

`out4`/`out5`/`out6` count trials in which all tiles / some tiles / no tiles
changed before resume, and `out13`/`out14` separate a completion callback seen
*during* the mask from one seen only after resume. Those two are reported as
distinct facts: GE memory work and GE completion notification are not the same
hardware domain.

## Plain-mutex cases

Four cases isolate the unresolved plain-Mutex cells left open by PR #52. The
mutex syscalls are absent from the installed PSPSDK headers, so
`mutex_imports.S` declares a complete `ThreadManForUser` import block (both the
custom mutex NIDs and every `ThreadManForUser` function referenced by the probe
and PSPSDK CRT) so that the library's stubs remain contiguous in `.sceStub.text`.
The Makefile overrides `FIXUP` to fail closed if `psp-fixup-imports` emits
out-of-order stub warnings (issue #400). `probe.c` mirrors the documented
`SceKernelMutexInfo` layout. Only scalar return values are treated as evidence.

- `CASE=mutex-refer-unlocked` — creates one unlocked and one locked mutex,
  refers both, unlocks the second, and refers again. The raw `lockThread`
  words for all three states plus the referring thread id are recorded so the
  unlocked-value convention (`0` vs `0xffffffff`) is decided by bits, not by
  documentation.
- `CASE=mutex-timeout-quanta` — a worker thread performs timed locks across
  1, 25, 250, and 1000 us requests (10 trials each), records the remaining-
  time word and measured min/max elapsed per interval, one `LockMutexCB`
  sample at 250 us, and a final lock with a 100 ms timeout released early via
  the owning unlock. This arbitrates alleged 25 us/250 us quantization against
  the measured clock.
- `CASE=mutex-priority-inheritance` — a low-priority owner holds an
  attr-`0x100` mutex while a higher-priority waiter blocks. Owner priority is
  sampled before, during, and after the wait (both via `ReferThreadStatus`
  from main and `GetThreadCurrentPriority` from the owner itself), deciding
  whether PSP boosts the owner.
- `CASE=mutex-interrupt-context` — registers a VBLANK sub-interrupt handler
  that samples 20 firings; each firing first records
  `sceKernelIsCpuIntrEnable()` (a trial counts only when it is 0, meaning the
  handler ran with CPU interrupts disabled), then measures `LockMutex`/
  `LockMutexCB`/`TryLockMutex` return precedence across bad UID, bad count,
  valid-unlocked, and non-owner unlock cells. One header record and one record
  per trial are emitted (`mutex-interrupt-context-t00`..`t19`). The kernel
  query `sceKernelIsIntrContext()` is not used: it lives in the kernel-only
  library InterruptManagerForKernel, which a user-mode probe cannot import.

All four emit `PSP-MUTEX-001` records. `status=PASS` means only that the
machinery ran and every scalar was captured; it makes no claim that any host
implementation matches until the comparison protocol runs on the capture.

> **Operator note.** Never send the PSPLink shell command `exit` from a capture
> driver. `exit` terminates PSPLink on the device and returns it to the XMB,
> which looks exactly like a probe-induced reset and is not one. Close the
> client's stdin instead.

## Exception and kernel-object probes (campaign psp-hw-20260917)

These cases are standalone probes, each in its own source file. Unlike
`probe.c`, they do not print protocol records. Each probe writes its header and
results to a file on `host0:` (the PSPLink host share) and records the
`PROBE_BUILD_COMMIT` value (default: the short `HEAD` hash). Build one case per
launch and power-cycle between launches.

| Case | Source | What it measures | Ends by |
| --- | --- | --- | --- |
| `exception-a1` | `probe_exception_a1.c` | A user-mode `break`: EPC, Cause and Status in PSPLink's exception frame. | Raising the exception. |
| `exception-a2` | `probe_exception_a2.c` | The same `break` in the delay slot of an always-taken branch (the Cause BD bit). | Raising the exception. |
| `exception-a3` | `probe_exception_a3.c` | A user-mode load from a kernel-segment address (AdEL and BadVAddr). | Raising the exception. |
| `kobj-b1` | `psp_b1.c` | Wait-free callback, semaphore, event-flag, FPL, VPL, VTimer and LwMutex return codes and status layouts. | Returning from `main`. |
| `wait-b2` | `psp_b2.c` | Contended waits with one bounded helper thread: wake, delete, cancel and LwMutex hand-off. | Returning from `main`. |
| `kernel-b3` | `psp_b3.c` | Message pipes, mailboxes, sleep/wakeup/suspend/terminate, release-wait, alarms and delay timing. | Parking in `sceKernelSleepThread()`. |

After an exception probe, read the frame with `pspsh -e "exprint"`, one
command at a time. On the measured PSPLink setup, a probe that returned from
`main` after creating threads (`wait-b2`) left PSPLink and exited to the XMB,
which is why `kernel-b3` parks instead.

The kernel-object probes link `threadman_user_imports.S`, one complete
ThreadManForUser import block: PSPSDK ships heavyweight-mutex stubs only for
the kernel library, and a second partial block would split the library's stub
run. Add any newly used ThreadManForUser NID there, and nothing else: every
PRX that links the file gets every stub in it. `display_user_imports.S` is the
user sceDisplay block (`sceDisplaySetHoldMode`,
`sceDisplayWaitVblankStartMultiCB`), linked only into `kernel-misc`.

### User-mode import gate

Every probe is a user-mode module, and the PSP refuses to load a user-mode PRX
that imports a kernel-only library (`0x8002013C`, library not found), so no
probe code runs. After `psp-fixup-imports`, the Makefile runs
`tools/psp_oracle/user_mode_imports.py` on the linked ELF. The build fails when
a user-mode module imports a library whose name ends in `_driver` or
`ForKernel`, or another known kernel-only library, and the message names each
library and its NIDs. A rejected ELF is deleted, so a later `make` cannot
package it. Use the user library that exports the same NIDs (for example
`sceDisplay`, not `sceDisplay_driver`), and never link a PSPSDK `*_kernel*` or
`*_driver` archive into a probe. The gate can also be run by hand on a PRX:
`python3 tools/psp_oracle/user_mode_imports.py build/nakagawa_psp_oracle.prx`.

## Bounded follow-up probes (hardware NOT_RUN)

These probes write only scalar records to `host0:`. No `ms0:` output or private
input is used. Their ordered result streams have strict completion parsers in
`tools/psp_oracle/parse_golden.py`, registered by campaign case in
`tools/psp_oracle/run_psplink.py`.

- `CASE=ge-nan` (`ge-nan`, `PSP-GE-001`) records raw input and VFPU result
  words for qNaN, `+Inf`, `-Inf`, `-0`, and the minimum positive denormal. For
  each input it also records a framebuffer hash and changed-pixel count after
  2D screen-coordinate, 3D clip-position, and lit-normal GE draws. It emits 20
  measurement records and `ge-nan-done`; it does not repeat the already
  measured `vasin` domain-edge words. The renderer records integer bit
  patterns and hashes, never formatted floats.
- `CASE=audio-query` (`audio-query`, `PSP-AUDIO-001`) records both
  `sceAudioGetChannelRestLen` and `sceAudioGetChannelRestLength`, two
  `sceAudioOutputBlocking` calls at 512 samples maximum, one
  `sceAudioOutput2OutputBlocking` call, rest-sample queries, return values,
  and elapsed microseconds. The output buffer contains silence. Each reserved
  channel is released, including when a later query or output call fails. It
  emits 13 measurement records and `audio-done`.
- `CASE=dma-cells` (`dma-cells`, `PSP-DMAC-001`) exercises both copy APIs with
  source-only, destination-only, and paired offsets 1, 2, 3, 5, 6, 7, 9, 10,
  11, 13, 14, and 15 bytes. These cover the requested 1/2/3 residues modulo 4
  and the unaligned residues modulo 16. It also measures overlap in both
  directions at 1, 2, 3, 7, and 15 bytes. Every span and guard is inside a
  probe-owned buffer, and cache writeback/invalidation brackets each call. It
  emits 92 measurement records and `dmac-cells-done`; the existing size matrix
  is not repeated.
- `CASE=delay-zero` (`delay-zero`, `PSP-KERNEL-002`) calls
  `sceKernelDelayThreadCB(0)` and `sceKernelDelayThread(0)` once each with an
  equal-priority ready worker and a pending callback. Each record carries
  before/after worker and callback counts, the raw notify and delay returns,
  thread priorities/status, cleanup return, and the system-time delta. It emits
  two measurement records and `delay-zero-done`. A `SKIP` means setup did not
  leave the worker ready and callback pending at the call boundary.

The five `CASE=dma-invalid-tail-*` launches remain one case per launch. Tier S
uses only size-zero requests. Tier B keeps the complete requested source and
destination spans inside the probe-owned scratch block or module-owned array;
it measures bounded overrun classification and never treats `K` as a physical
invalid boundary. An unowned destination tail remains `SKIP`; invalid-span
validation and K3/K4 remain `NOT_RUN` under #303.

## Pending PSP-3000 campaign

The cases below are new synthetic probes. Their hardware expectations
remain `NOT_RUN`; a complete parser only proves that the record stream is
complete and well formed. It does not decide which PSP return values or
ordering semantics are correct.

| Case | Test id | Measurements |
| --- | --- | --- |
| `kernel-alarm` | `PSP-ALARM-001` | `SetAlarm` with a null handler and zero clock, alarm-table exhaustion count and error (capped at 1024 pending alarms; `out2` is the cap), cancellation after firing/cancel/unknown UID, handler-return re-arm timing, and a bounded semaphore wait from an alarm handler with interrupt state. |
| `thread-scheduler` | `PSP-THREAD-003` | Suspend UID 0 and self, resume UID 0, out-of-range ready-queue rotation, ready order after rotation (bounded wait; `TIMEOUT` if the third thread never runs), and timeout of a waiting thread while suspended. |
| `wait-outcomes` | `PSP-WAIT-001` | Semaphore and event-flag signal/cancel operations that occur before a deadline but are dispatched afterward. |
| `ge-break-continue` | `PSP-GE-CONTROL-001` | Break without an active list, continue without a paused list, invalid break mode, list/draw sync statuses for paused work, the continue result with a bounded drain (`ge-continue-drain`, `TIMEOUT` with the last states if the GE does not go idle), a queue reset when needed (`ge-quiesce-after-continue`), cancelled work, and a final quiesce (`ge-quiesce-after-cancel`). |
| `refer-status-size` | `PSP-KERNEL-STATUS-001` | Semaphore, event-flag, and mailbox status structures with size words 0, 8, 40, and full size; records the changed bytes and returned size word. |
| `registry-readonly` | `PSP-REGISTRY-001` | Read-only registry/category enumeration under `/CONFIG`, key names/types/sizes, modeled setting values only, and unknown-category/key, small-buffer, and handle-exhaustion results (`registry-errors`; at most 256 opens, `out6` says whether an open failed first), then the forged-handle call last (`registry-bad-handle`). |
| `kernel-misc` | `PSP-KERNEL-MISC-001` | Wide system-clock conversion, default controller mode, thread/global profiler returns, basic VTimer behavior, display calls, battery-icon status, and UMD-popup return values. |
| `vfpu-compare` | `PSP-VFPU-CMP-001` | `vscmp.s`, `vsge.s`, and `vslt.s` on 15 operand pairs (less-than, equal, greater, signed zeros in both orders, quiet, signalling, and negative NaN on each side, infinities). Each record carries the observed word, the project's model word (UNMEASURED for the console), and both operands. |

The registry case opens the registry and every category in read mode. It never
calls a set, create, remove, or flush API. It records values only for the
runtime-modeled keys `language`, `button_assign`, date/time format, `timezone`,
`summer_time`, and `adhoc_channel`; all other keys contribute only name, type,
and size. Registry and console-specific capture output stays in the private
campaign output directory.

The host campaign plan requires one staged PRX for each queued case. It launches
one PRX per boot and issues a PSPLink soft reset between completed cases. The
queue starts with `transport-write`, then runs the new cases in the table
order, then every remaining README `NOT_RUN` probe: `smoke`,
`thread-exit-delete`, `teardown-test`, `io-matrix`, `display-mask-duty`,
`display-wait-late`, `display-wait-priority`, `display-vblank-window`,
`fpu-vector`, `cache-alias`, `audio-query`, `ge-nan`, `dma-cells`, the five
`dma-invalid-tail-*` launches, and the four `mutex-*` launches. `delay-zero` is
not queued because it is already measured.

The private plan is a schema 1 JSON object with these fields: `campaign_id`,
`session_id` (the hardware lock holder), `source_commit`, `console_model`,
`host0_root`, `report_path`, `checkpoint_path`, and the ordered `cases` (each with
`case_id`, `prx`, and `timeout_seconds`). The optional `expected_firmware` is
compared exactly with PSPLink's `pspver` version, so it must use that form:
`6.6.1` for firmware 6.61. Validation refuses `6.61` and any other form. The
optional `model_code` is the raw non-negative PspModel integer.

Validate the private queue offline before a hardware session:

```powershell
python tools/psp_oracle/run_psplink.py --campaign-plan <private-campaign-plan.json> --dry-run
```

The actual campaign refuses to start unless the hardware lock is `HELD`,
confirms a power cycle, and names the same session as the private plan. Every
mode that touches the PSP goes through the same lock gate. `--campaign-case` and
`--command` require `--session-id`, which names the lock holder. The gate is
checked before the USBHostFS transport starts, and it is read again before each
case's soft reset, before each launch, and before every PSPLink `reset`. A
refusal stops with `HARDWARE_LOCK_REFUSED` before the next PSP action. In plan
mode it keeps the checkpoint at the unlaunched case.
After an interrupted case, the maintainer power-cycles the PSP and resumes with
`--confirm-power-cycle`; the failed case is retained as incomplete and the next
case starts on the new boot. No automatic retry or semantic result is inferred
from a host timeout.

The checkpoint never loses its resume position. A stop before the next case is
launched, such as `TRANSPORT_START_FAILED` or a failed baseline snapshot, leaves
the checkpoint `IN_PROGRESS` at that case and owes no power cycle; rerun without
`--confirm-power-cycle`. Only a launched case without a clean completion, or a
failed soft reset, waits for a power cycle. The checkpoint also records whether
the `transport-write` preflight qualified host0; a campaign resumes past the
preflight only when it did, and an interrupted preflight resumes at
`transport-write`. Interrupted cases accumulate in `interrupted_cases`.

The runner waits for each case's host0 log until the probe appends its
completion marker, which every probe writes after its last record. A slow stream
keeps the wait open up to the plan's `timeout_seconds` for that case; for example,
the registry census writes hundreds of records and a durable step marker before
each risky call. Once the marker arrives the stream is final, and the runner
checks the records against their contract. A finished stream that violates its
contract is torn down normally and its envelope names the protocol failure, so it
is not mistaken for an unfinished probe. Only a stream without a marker at the
timeout (or a failed launch) stops the queue. That stop reason gives the record
count, the time of the last host0 write after launch, and the last step marker.

A host-side exception never crashes the queue. The case ends as `HOST_ERROR`, and
the report's `host_error` names the exception type, the raising function and the
message. If the probe was launched, the runner still unloads it and compares S2
with S0. A verified-clean teardown records the case as interrupted and keeps the
checkpoint `IN_PROGRESS` at the next case. Any other outcome waits for a power
cycle, as an incomplete case does. Verbatim console captures from the
2026-10-08 session under `fixtures/psp_oracle_captures/` pin the parsers and
runner stages against real probe output.

## Build and hardware handoff

### Device build identity

The main `probe.c` reports its full `PROBE_BUILD_COMMIT_FULL` Git object id in
`source_commit`. The Makefile requires that id to resolve to the checked-out
`HEAD` and refuses to build from a dirty source tree. The host binds only a full
40- or 64-digit hexadecimal object id, using case-insensitive equality; a short
hash prefix is `IDENTITY_NOT_BOUND`, never a match. A different full id is
`IDENTITY_MISMATCH`, and either status blocks acceptance while preserving the
device's reported value in the envelope.

The probe cannot hash its own loaded PRX, so `binary_sha256` remains zero on the
device. The host records the staged PRX digest as a host measurement and labels
the binary binding `PLACEHOLDER`; this does not replace the separate device
`source_commit` comparison.

Build when PSPDEV is installed:

```powershell
make -C fixtures/psp_oracle
```

For example, to build the callback case in WSL with PSPDEV:

```bash
make -C fixtures/psp_oracle clean
make -C fixtures/psp_oracle CASE=callback-notify-check EBOOT.PBP
```

To build the complete DMA matrix without running it:

```bash
for case in dma-concurrency dma-invalid-tail-s0 \
  dma-invalid-tail-memcpy-dst dma-invalid-tail-memcpy-src \
  dma-invalid-tail-try-dst dma-invalid-tail-try-src; do
  make -C fixtures/psp_oracle clean
  make -C fixtures/psp_oracle CASE="$case" EBOOT.PBP
done
```

The loop above deliberately overwrites the ignored build output. A hardware
operator must build, launch, and capture each case before moving to the next;
do not use the loop as a hardware runner. Start with `dma-concurrency`. Run the
four invalid-tail cases separately only after confirming the expected PSP
model/firmware and a working PSPLink reset path.

The expected output is a PRX under the fixture's ignored `build/` directory.
Signing, PSPLink launch, and USB/driver setup remain explicit maintainer
actions. No firmware or driver installer is part of this repository.
