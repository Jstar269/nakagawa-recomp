# PSP interrupt / dispatch-context conformance matrix (historical issue #88)

<!-- markdownlint-disable MD013 -->

Historical scope: this is a pre-republication snapshot captured against the
former development `main`. Its issue and PR numbers are historical identifiers,
not current public tracker mappings, and its implementation classifications are
not current sanitized-`main` status.

Hardware-derived expectations for the blocking and polling APIs that PSPAutotests
`tests/intr/waits.cpp` exercises, the executable Nakagawa harness built from them, and
the captured then-current-`main` classification of every cell.

This document is **historical evidence**, not a current plan or status dashboard.
It records what the cited hardware oracle and captured Nakagawa revision did,
and which cells could not then be exercised. See historical
[issue #88](https://github.com/Jstar269/nakagawa-recomp/issues/88) for the semantics work,
and the PR ordering at the end of this file. PR-A, PR-B and PR-C1 are implemented; PR-C2
onward remain future work.

Some cells in this matrix are **not #88's to fix**. Where the deviation is really a missing
subsystem object model rather than a missing context rule, implementation ownership sits with
that subsystem's issue. Those cells stay in the tables below, stay pinned, and stay counted as
known deviations — deferring the work does not remove the evidence.

## Provenance

| | |
| --- | --- |
| Oracle | `third_party/ppsspp-src/pspautotests/tests/intr/waits.expected` (421 lines) |
| Producer | `tests/intr/waits.cpp`, run on real PSP hardware; output captured by PSPAutotests |
| Upstream | <https://github.com/hrydgard/pspautotests> |
| In-tree status | `third_party/` is **Git-ignored**. The oracle file is not part of this repository; the tables below and `src/rt/intr_conformance.h` are transcriptions that cite it by line number |
| Transcription gate | `tools/test_intr_waits_matrix.py` re-derives every hardware value from the oracle when a checkout is present, and SKIPs (never passes) when it is not |

**A second oracle covers the column this one does not.** `waits.expected` has no
normal-context cells for the semaphore family, which is why every normal-context
entry below is a *control*. `tests/threads/semaphores/wait.expected` (31 lines,
same upstream, same Git-ignored status) is a normal-context hardware capture of
`sceKernelWaitSema` specifically, and issue #43 used it to implement the
ordinary count contract. It is cited by line number in `src/rt/hle.c` and in
`test_wait_sema_count_validation()` in `src/rt/hle_thread_selftest.c`; it does
not feed `hw[]` here, because these tables are `waits.expected` transcriptions
and mixing two sources into one column would make the gate unverifiable.

`pspautotests` ships a `LICENSE.txt` containing only the placeholder text
"TO FILL WITH THE LEAST RESTRICTIVE COMPATIBLE LICENSE". No SPDX identifier is asserted
for this data on that basis. What is reproduced here is a set of measured return codes
with attribution, not upstream source.

## What the evidence establishes

`0x800201a7` is `SCE_KERNEL_ERROR_CAN_NOT_WAIT`; `0x80020064` is
`SCE_KERNEL_ERROR_ILLEGAL_CONTEXT`. The broad shape is that a blocking API returns
`CAN_NOT_WAIT` when interrupts or dispatch are disabled, and `ILLEGAL_CONTEXT` from
inside an interrupt handler.

**There is no universal context gate.** Four independent facts in the oracle rule one out:

1. **Some APIs validate a parameter first and never reach the gate.**
   `sceKernelWaitEventFlag` with mode `0xFF` returns `ILLEGAL_MODE` (L72/L73);
   `sceCtrlReadBufferPositive` with count 256 returns `INVALID_SIZE` (L220/L221);
   `sceUmdWaitDriveStat` with type 0 returns `ERRNO_INVALID_ARGUMENT` (L296/L297);
   `sceDisplayWaitVblankStartMulti(0)` returns `INVALID_VALUE` (L42/L43);
   `sceKernelReceiveMsgPipe` with size `-1` returns `ILLEGAL_ADDR` (L138/L139).
2. **Some validate the object first.** `sceKernelWaitThreadEnd(0)` returns `ILLEGAL_THID`
   in *all three* contexts (L204, L205, L383); every `sceIo*` bad-descriptor cell returns
   `BADF` in all three (L258, L259, L401).
3. **Precedence changes with the context.** `sceKernelWaitEventFlag` mode `0xFF` returns
   `ILLEGAL_MODE` with interrupts disabled (L72) but `ILLEGAL_CONTEXT` from interrupt
   context (L338). One flag on a registry entry cannot express an ordering that flips
   between contexts.
4. **Some calls legitimately succeed.** `sceGeListSync` mode 1 and `sceGeDrawSync` mode 1
   poll and return 0 in every context (L244, L252, L396, L399); `sceIoGetAsyncStat` peek
   returns 1 (L284/L285); `sceAudioOutputBlocking` on a 64-sample channel returns the
   sample count (L228/L229); and `sceDisplayWaitVblank` **succeeds from inside an
   interrupt**, returning 1 (L323) where every neighbouring API returns `ILLEGAL_CONTEXT`.

**Interrupts-disabled and dispatch-disabled are genuinely different states.** They agree
in all but two cells out of roughly 110, which is exactly why the two exceptions matter:

| API | scenario | interrupts disabled | dispatch disabled |
| --- | --- | --- | --- |
| `sceIoWaitAsyncCB` | Valid | `00000000` (L278) | `8002032a` `NOASYNC` (L279) |
| `sceAudioOutputBlocking` | Valid channel - 128 | `800201a7` `CAN_NOT_WAIT` (L230) | `80260002` `CHANNEL_BUSY` (L231) |

A harness that modelled dispatch-disabled as an alias of interrupts-disabled would assert
a distinction the runtime does not make. Nakagawa provides a dedicated dispatch-suspension
state `s_dispatch_enabled` (via `sceKernelSuspendDispatchThread` / `sceKernelResumeDispatchThread`),
so this column is executed directly against the hardware matrix.

**Normal context is not covered.** `waits.cpp` never prints a normal-context result for
these calls; the only normal-context data points in the whole file are
`sceKernelDelayThread: 00000000` (L419) and the four sub-interrupt lifecycle calls
(L314, L315, L420, L421). Every normal-context cell below is therefore marked *unknown*
and used as a control, never compared against hardware.

## Coverage of `waits.cpp` by the Nakagawa registry

`waits.cpp` names 103 `sce*` entry points. 31 are not registered anywhere in
`src/rt/hle.c`, so no hardware cell for them can be exercised at any evidence tier:

`sceAudioSRCChRelease`, `sceAudioSRCChReserve`, `sceAudioSRCOutputBlocking`,
`sceDisplayWaitVblankCB`, `sceDisplayWaitVblankStartCB`, `sceDisplayWaitVblankStartMulti`,
`sceDisplayWaitVblankStartMultiCB`, `sceGeListDeQueue`, `sceIoGetAsyncStat`, `sceIoRemove`,
`sceKernelAllocateVpl`, `sceKernelAllocateVplCB`, `sceKernelCreateMbx`,
`sceKernelCreateTlspl`, `sceKernelCreateVpl`, `sceKernelDelaySysClockThread`,
`sceKernelDelaySysClockThreadCB`, `sceKernelDeleteMbx`, `sceKernelDeleteTlspl`,
`sceKernelDeleteVpl`, `sceKernelFreeTlspl`, `sceKernelGetTlsAddr`, `sceKernelReceiveMbx`,
`sceKernelReceiveMbxCB`, `sceKernelReceiveMsgPipe`, `sceKernelReceiveMsgPipeCB`,
`sceKernelReferTlsplStatus`, `sceKernelSendMsgPipe`,
`sceKernelSendMsgPipeCB`, `sceKernelTerminateThread`.

Of the registered remainder the harness covers 54 probe cases. The registered APIs
deliberately left out of the executable matrix, with their reasons:

| API | why not exercised yet |
| --- | --- |
| `sceGeListSync`, `sceGeDrawSync` | registered, but arranging a list requires the GE display-list lifecycle, which this pass is explicitly not to touch. Hardware cells recorded at L240-L255 and L394-L400 |
| `sceAudioOutputBlocking` | registered, but `audio.c` is not linked into the selftest and a channel reservation is required. Hardware cells at L226-L231 and L391-L392 |
| `sceKernelStartModule`, `sceKernelStopModule` | registered, but both handlers mutate module-table state and log unconditionally, so a context probe would not be isolated. Hardware cells at L196, L197, L200, L201, L381, L382 |
| `sceIoRead` / `sceIoWrite` / `sceIoWaitAsync*` valid-descriptor cells | need a real host file, which would make the outcome depend on the filesystem rather than on the context. Only the bad-descriptor cells are exercised |
| `sceCtrlReadBufferPositive` | registered and present in the table, but declared `skip`: `ctrl_fill_n()` reads `hle.c` file-static ring state that `reset_fixture()` cannot clear, so the result depends on test order. Needs a test-only ring reset |

## The harness

`src/rt/intr_conformance.h`, included once by `src/rt/hle_thread_selftest.c`, which already
links production `hle.c`, includes production `sched.c`, and enters every handler through
`sr_syscall`'s registered-NID lookup. Reusing that target avoids a second Make recipe with
a hand-copied flag list.

### Contexts

| Context | How it is built | Status |
| --- | --- | --- |
| normal | ordinary call on a fixture thread | control only; hardware has no column |
| interrupts disabled | `sceKernelCpuSuspendIntr` (NID `0x092968f4`) held across the call, entered through the production NID | executed |
| interrupt context | `sceKernelRegisterSubIntrHandler(30, 1, entry)` + `sceKernelEnableSubIntr(30, 1)` through production NIDs, then `sched_raise_interrupt(SCHED_INTR_VBLANK)` and a production `sceKernelCpuResumeIntr`, which drives `sched_resume_interrupts` -> `scheduler_service_pending` -> `deliver_vblank` -> `dispatch`. Same registration path `waits.cpp` uses | executed |
| dispatch disabled | `sceKernelSuspendDispatchThread` (`0x3ad58b8c`) and `sceKernelResumeDispatchThread` (`0x27e22ec2`) registered in `src/rt/hle.c` with scheduler dispatch-suspension state `s_dispatch_enabled` | executed |

### Outcomes

Hardware returns immediately in every cell here. The genuinely blocking waits now consult
interrupt and dispatch state (PR-B), as do the blocking FPL allocate forms (PR-C1) and
`sceKernelWaitSema`/`CB` unconditionally (issue #43), but the remaining immediate/satisfied
cases, the interrupt-context cells and the parameter/object corrections do not yet, so a
probe can still enter a real wait. Each probe therefore runs on its own coroutine standing in for a
guest thread; if it does not come back, its TCB is parked in `TH_WAIT_*` and the outcome is
`WOULD_BLOCK` - a first-class measurement, not a hang.

### How known failures are represented

Every cell records both the hardware value (`hw[]`) and the exact value current main
produces (`base[]`). The runner classifies:

| condition | result |
| --- | --- |
| `actual == hw` and `base == hw` | CONFORMS |
| `actual == base != hw` | known deviation - reported, not a suite failure |
| `actual == hw` but `base != hw` | **FAILURE**: "promote the baseline" |
| anything else | **FAILURE**: regression |

A baseline is one exact value, never a wildcard. A future regression produces a third
value and fails. A future fix makes `actual == hw` and fails until the baseline is
deliberately promoted to `hw` in the table. Known failures therefore cannot rot into
silent passes in either direction. This was demonstrated rather than assumed: the first
run with predicted (rather than measured) baselines produced 18 failures naming each
mismatch, and mutating a single `hw` cell makes `tools/test_intr_waits_matrix.py` fail
with the exact oracle line it disagrees with.

### Bounded interrupt-context execution

In interrupt context `s_cur` is `-1`, and every scheduler blocking primitive
(`sched_block_on`, `sched_block_on_timeout`, `sched_delay_current`, `sched_thread_sleep`)
returns immediately when `s_cur < 0`. A handler that *loops* on an unsatisfied wait
condition - `h_WaitSema`, `h_WaitEventFlag`, `h_WaitThreadEnd`, `lwmutex_acquire` -
therefore spins forever rather than blocking. That is a real defect, and it is also
unobservable from inside the process.

The runner bounds it with a measured gate rather than a guess: the interrupt-context leg
runs for a probe only when that probe's **normal-context** leg *returned*. If that leg
blocked, this probe's wait condition is unsatisfiable and the interrupt-context call would
spin, so the cell is recorded NOT RUN with reason `spin-unbounded`. The gate is derived at
run time from the measurement, so it re-opens by itself once the wait stops being entered.
It is conservative - `sceKernelDelayThread` is gated even though `sched_delay_current`
early-returns - and being conservative here costs coverage, never correctness.

The gate reads the normal-context leg rather than the interrupts-disabled leg, which is a
correction PR-B forced. The two were equivalent only while no handler consulted interrupt
state. Once `CAN_NOT_WAIT` landed, the interrupts-disabled leg returns for a reason that
does **not** hold inside an interrupt handler: `ic_run_in_interrupt()` reaches the handler
through a production `sceKernelCpuResumeIntr`, which restores the prior *enabled*
interrupt state before driving `scheduler_service_pending()`, and never touches dispatch
state. `sched_wait_permitted()` is therefore true in interrupt context and the wait loops
are entered exactly as before. Keeping the old gate would have opened it for the six
looping probes - `WaitSema`/`CB`, `WaitEventFlag`/`CB`, `WaitThreadEnd`/`CB` - and hung
the suite. The normal-context leg measures the underlying condition instead and is
unaffected by any context semantics; it produced identical gating on the PR-A baseline,
where `base[normal]` and `base[intr-off]` agree in every row. Retiring the remaining 12
cells needs PR-D.

The gate re-opening by itself is not hypothetical: issue #43 made
`sceKernelWaitSema`/`CB` **"Invalid count"** return `ILLEGAL_COUNT` in normal context
instead of blocking, and the two `intr-ctx` cells behind it went from NOT RUN to executed
on the next run with no change to the harness. 14 spin-unbounded cells became 12.

### Registry scope

`sr_hle_init()` registers a deliberately narrow set under `SR_HLE_THREAD_SELFTEST`. Before
this work the executable harness could reach only 6 of the 37 NIDs the matrix needs -
including neither `sceKernelRegisterSubIntrHandler` nor `sceKernelEnableSubIntr`, without
which no interrupt context can be constructed at all.
`hle_register_wait_conformance_handlers()` closes that gap. It is compiled only into the
selftest, every triple in it is the same `(nid, name, handler)` the production branch
registers, and `tools/test_intr_waits_matrix.py` fails if any triple diverges. The runner
still probes registry membership at run time (`sr_hle_test_is_registered`) and reports
`registry-scope` for anything it cannot reach, because calling an unregistered NID would
`_Exit(7)`.

## Current-`main` classification

Measured on `hle_thread_selftest.exe` built from this branch. Every value in the
"current main" column is the pinned `base[]` entry in `src/rt/intr_conformance.h`,
asserted on every run.

* **CONFORMS** - current main already produces the hardware value.
* **known deviation** - current main produces a different, exactly pinned value.
* **NOT RUN** - deliberately not executed; the reason is printed at run time.
* **UNTESTABLE** - no substrate exists to construct the context.
* **control** - executed and pinned, but hardware has no cell to compare against.

Totals over 54 probes x 4 columns (216 cells): **62 CONFORMS, 79 known deviations, 20 NOT RUN, 0 UNTESTABLE, 55 controls.**

The 79 known deviations by implementation owner, recomputed from the run rather than carried
forward: **PR-C2 20, PR-D 18, PR-E 30, #2 (plain Mutex) 6, #79 (VolatileMemLock) 5.**

Of the 62: 10 were already right before this campaign (below), PR-B added 28, PR-C1
added the 8 `sceKernelAllocateFpl` / `...CB` cells, issue #43 added the 4
`sceKernelWaitSema` / `...CB` **"Bad sema"** `intr-off`/`disp-off` cells, and the plain-Mutex
campaign added the 12 `sceKernelLockMutex` / `...CB` `intr-off`/`disp-off` cells
(context-check-before-object-lookup, `src/rt/mutex.c`).

### What issue #43 moved

Issue #43 is a count-validation fix, not a context-semantics PR, but it takes the
first slice of PR-C2 with it because the two are the same statement: putting the
context check ahead of the object lookup is what the bad-sema cells measure, and
count validation cannot be inserted without deciding where the context check
sits relative to it. Twelve cells in group B changed:

| cells | before | after | nature |
| ---: | --- | --- | --- |
| 4 | `0x80020000` known deviation | `0x800201a7` **CONFORMS** | `WaitSema`/`CB` bad sema, `intr-off`+`disp-off`. PR-C2 scope, landed here |
| 2 | `0x80020000` known deviation | `0x80020199` known deviation | `WaitSema`/`CB` bad sema, `intr-ctx`. Still not `ILLEGAL_CONTEXT`; still PR-D |
| 2 | `0x80020000` control | `0x80020199` control | `WaitSema`/`CB` bad sema, normal. Hardware-measured, but by `wait.expected` L21/L23, not by this oracle |
| 2 | `WOULD_BLOCK` control | `0x800201bd` control | `WaitSema`/`CB` invalid count, normal. Same: measured by `wait.expected` L3/L13, recorded here as a control |
| 2 | NOT RUN `spin-unbounded` | `0x800201bd` known deviation | `WaitSema`/`CB` invalid count, `intr-ctx`. The gate reads the normal leg; that leg now RETURNS, so these execute for the first time |

The last row is a coverage gain, not a regression: two cells that could never be
measured now are, and they report honestly against `ILLEGAL_CONTEXT`.

**One ordering in the new implementation is not hardware-established.** An
unknown Sema id combined with an invalid count is not a cell in either oracle:
`waits.cpp` probes bad-id and invalid-count separately, and `wait.c` never
combines them. `sceKernelWaitSema` resolves it object-first (`UNKNOWN_SEMID`),
which is what the public reference set supports and what PPSSPP does. That is
**corroborated, not measured**, and `test_wait_sema_count_validation()` pins it
as a regression guard with that label attached. Do not promote it to a hardware
claim without a cell.

Ten of the conforming cells are precedence cells the runtime already got right because it
validates before it would have blocked: `sceKernelWaitEventFlag` and
`sceKernelWaitEventFlagCB` invalid-mode (with interrupts disabled and dispatch disabled),
and `sceKernelWaitThreadEnd` / `sceKernelWaitThreadEndCB` bad-thread (with interrupts disabled,
from interrupt context, and with dispatch disabled). PR-B added the other 28, all
`0x800201a7`, all in the `intr-off` and `disp-off` columns.

PR-B's stated scope is 24 cells: 12 genuinely blocking probes x 2 context columns. It
lands 28. The extra four are `sceKernelWaitSema` and `sceKernelWaitSemaCB` **"Invalid
count"** (`waits.expected` L56/L57, L64/L65), and they are not a widened fix - they are the
same fix observed through a second fixture. The PR-B/PR-C split was drawn on *hardware's*
blocking classification, where invalid count is an immediate case because hardware
validates the signal against `maxCount`. Nakagawa had no such validation, so
`sceKernelWaitSema(sema, 9, NULL)` against a max-1 semaphore was simply a wait that could
never be satisfied, and it reached the same `m->count < need` decision point as the valid
case through the same statement. Hardware's answer for those four cells is `CAN_NOT_WAIT`
either way, so they were promoted rather than left pinned to a value the runtime no longer
produced.

That paragraph closed by predicting adding `maxCount` validation "would not change these
four cells at all - hardware checks the context first - and would additionally move two
normal-context control cells." **Issue #43 added the validation and measured exactly
that**: the four cells kept `CAN_NOT_WAIT`, and the two normal-context controls moved from
`WOULD_BLOCK` to `0x800201bd`. The prediction held; the four cells are now reached by the
route hardware uses rather than by an unsatisfiable-wait accident.

### Full matrix

| API | scenario | ctx | evidence | hardware | current main | verdict | precedence |
| --- | --- | --- | ---: | --- | --- | --- | --- |
| `sceKernelDelayThread` | - | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceKernelDelayThread` | - | intr-off | L2 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelDelayThread` | - | intr-ctx | L317 | 0x80020064 | not run | NOT RUN | context |
| `sceKernelDelayThread` | - | disp-off | L3 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelDelayThreadCB` | - | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceKernelDelayThreadCB` | - | intr-off | L6 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelDelayThreadCB` | - | intr-ctx | L318 | 0x80020064 | not run | NOT RUN | context |
| `sceKernelDelayThreadCB` | - | disp-off | L7 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelSleepThread` | - | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceKernelSleepThread` | - | intr-off | L18 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelSleepThread` | - | intr-ctx | L321 | 0x80020064 | not run | NOT RUN | context |
| `sceKernelSleepThread` | - | disp-off | L19 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelSleepThreadCB` | - | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceKernelSleepThreadCB` | - | intr-off | L22 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelSleepThreadCB` | - | intr-ctx | L322 | 0x80020064 | not run | NOT RUN | context |
| `sceKernelSleepThreadCB` | - | disp-off | L23 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceDisplayWaitVblank` | - | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceDisplayWaitVblank` | - | intr-off | L26 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceDisplayWaitVblank` | - | intr-ctx | L323 | 0x00000001 | not run | NOT RUN | n/a |
| `sceDisplayWaitVblank` | - | disp-off | L27 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceDisplayWaitVblankStart` | - | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceDisplayWaitVblankStart` | - | intr-off | L34 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceDisplayWaitVblankStart` | - | intr-ctx | L325 | 0x80020064 | not run | NOT RUN | context |
| `sceDisplayWaitVblankStart` | - | disp-off | L35 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSema` | Bad sema | normal | - | unknown | 0x80020199 | control | n/a |
| `sceKernelWaitSema` | Bad sema | intr-off | L54 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSema` | Bad sema | intr-ctx | L331 | 0x80020064 | 0x80020199 | known deviation | context |
| `sceKernelWaitSema` | Bad sema | disp-off | L55 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSema` | Invalid count | normal | - | unknown | 0x800201bd | control | n/a |
| `sceKernelWaitSema` | Invalid count | intr-off | L56 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSema` | Invalid count | intr-ctx | L332 | 0x80020064 | 0x800201bd | known deviation | context |
| `sceKernelWaitSema` | Invalid count | disp-off | L57 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSema` | Valid sema | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceKernelWaitSema` | Valid sema | intr-off | L58 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSema` | Valid sema | intr-ctx | L333 | 0x80020064 | not run | NOT RUN | context |
| `sceKernelWaitSema` | Valid sema | disp-off | L59 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSemaCB` | Bad sema | normal | - | unknown | 0x80020199 | control | n/a |
| `sceKernelWaitSemaCB` | Bad sema | intr-off | L62 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSemaCB` | Bad sema | intr-ctx | L334 | 0x80020064 | 0x80020199 | known deviation | context |
| `sceKernelWaitSemaCB` | Bad sema | disp-off | L63 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSemaCB` | Invalid count | normal | - | unknown | 0x800201bd | control | n/a |
| `sceKernelWaitSemaCB` | Invalid count | intr-off | L64 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSemaCB` | Invalid count | intr-ctx | L335 | 0x80020064 | 0x800201bd | known deviation | context |
| `sceKernelWaitSemaCB` | Invalid count | disp-off | L65 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSemaCB` | Valid sema | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceKernelWaitSemaCB` | Valid sema | intr-off | L66 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitSemaCB` | Valid sema | intr-ctx | L336 | 0x80020064 | not run | NOT RUN | context |
| `sceKernelWaitSemaCB` | Valid sema | disp-off | L67 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitEventFlag` | Bad flag | normal | - | unknown | 0x80020000 | control | n/a |
| `sceKernelWaitEventFlag` | Bad flag | intr-off | L70 | 0x800201a7 | 0x80020000 | known deviation | context |
| `sceKernelWaitEventFlag` | Bad flag | intr-ctx | L337 | 0x80020064 | 0x80020000 | known deviation | context |
| `sceKernelWaitEventFlag` | Bad flag | disp-off | L71 | 0x800201a7 | 0x80020000 | known deviation | context |
| `sceKernelWaitEventFlag` | Invalid mode | normal | - | unknown | 0x80020195 | control | n/a |
| `sceKernelWaitEventFlag` | Invalid mode | intr-off | L72 | 0x80020195 | 0x80020195 | **CONFORMS** | param |
| `sceKernelWaitEventFlag` | Invalid mode | intr-ctx | L338 | 0x80020064 | 0x80020195 | known deviation | context |
| `sceKernelWaitEventFlag` | Invalid mode | disp-off | L73 | 0x80020195 | 0x80020195 | CONFORMS | param |
| `sceKernelWaitEventFlag` | Valid flag | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceKernelWaitEventFlag` | Valid flag | intr-off | L74 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitEventFlag` | Valid flag | intr-ctx | L339 | 0x80020064 | not run | NOT RUN | context |
| `sceKernelWaitEventFlag` | Valid flag | disp-off | L75 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitEventFlag` | Already set | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelWaitEventFlag` | Already set | intr-off | L76 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelWaitEventFlag` | Already set | intr-ctx | - | unknown | 0x00000000 | control | unknown |
| `sceKernelWaitEventFlag` | Already set | disp-off | L77 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelWaitEventFlagCB` | Bad flag | normal | - | unknown | 0x80020000 | control | n/a |
| `sceKernelWaitEventFlagCB` | Bad flag | intr-off | L80 | 0x800201a7 | 0x80020000 | known deviation | context |
| `sceKernelWaitEventFlagCB` | Bad flag | intr-ctx | L340 | 0x80020064 | 0x80020000 | known deviation | context |
| `sceKernelWaitEventFlagCB` | Bad flag | disp-off | L81 | 0x800201a7 | 0x80020000 | known deviation | context |
| `sceKernelWaitEventFlagCB` | Invalid mode | normal | - | unknown | 0x80020195 | control | n/a |
| `sceKernelWaitEventFlagCB` | Invalid mode | intr-off | L82 | 0x80020195 | 0x80020195 | **CONFORMS** | param |
| `sceKernelWaitEventFlagCB` | Invalid mode | intr-ctx | L341 | 0x80020064 | 0x80020195 | known deviation | context |
| `sceKernelWaitEventFlagCB` | Invalid mode | disp-off | L83 | 0x80020195 | 0x80020195 | CONFORMS | param |
| `sceKernelWaitEventFlagCB` | Valid flag | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceKernelWaitEventFlagCB` | Valid flag | intr-off | L84 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitEventFlagCB` | Valid flag | intr-ctx | L342 | 0x80020064 | not run | NOT RUN | context |
| `sceKernelWaitEventFlagCB` | Valid flag | disp-off | L85 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitEventFlagCB` | Already set | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelWaitEventFlagCB` | Already set | intr-off | L86 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelWaitEventFlagCB` | Already set | intr-ctx | - | unknown | 0x00000000 | control | unknown |
| `sceKernelWaitEventFlagCB` | Already set | disp-off | L87 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelAllocateFpl` | Bad fpl | normal | - | unknown | 0x800200d3 | control | n/a |
| `sceKernelAllocateFpl` | Bad fpl | intr-off | L102 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelAllocateFpl` | Bad fpl | intr-ctx | L347 | 0x80020064 | 0x800200d3 | known deviation | context |
| `sceKernelAllocateFpl` | Bad fpl | disp-off | L103 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelAllocateFpl` | Valid fpl | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelAllocateFpl` | Valid fpl | intr-off | L104 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelAllocateFpl` | Valid fpl | intr-ctx | L348 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceKernelAllocateFpl` | Valid fpl | disp-off | L105 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelAllocateFplCB` | Bad fpl | normal | - | unknown | 0x800200d3 | control | n/a |
| `sceKernelAllocateFplCB` | Bad fpl | intr-off | L108 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelAllocateFplCB` | Bad fpl | intr-ctx | L349 | 0x80020064 | 0x800200d3 | known deviation | context |
| `sceKernelAllocateFplCB` | Bad fpl | disp-off | L109 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelAllocateFplCB` | Valid fpl | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelAllocateFplCB` | Valid fpl | intr-off | L110 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelAllocateFplCB` | Valid fpl | intr-ctx | L350 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceKernelAllocateFplCB` | Valid fpl | disp-off | L111 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutex` | Bad mutex | normal | - | unknown | 0x800201c3 | control | n/a |
| `sceKernelLockMutex` | Bad mutex | intr-off | L168 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutex` | Bad mutex | intr-ctx | L371 | 0x80020064 | 0x800201c3 | known deviation | context |
| `sceKernelLockMutex` | Bad mutex | disp-off | L169 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutex` | Bad count | normal | - | unknown | 0x800201bd | control | n/a |
| `sceKernelLockMutex` | Bad count | intr-off | L170 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutex` | Bad count | intr-ctx | L372 | 0x80020064 | 0x800201bd | known deviation | context |
| `sceKernelLockMutex` | Bad count | disp-off | L171 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutex` | Valid mutex | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelLockMutex` | Valid mutex | intr-off | L172 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutex` | Valid mutex | intr-ctx | L373 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceKernelLockMutex` | Valid mutex | disp-off | L173 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutexCB` | Bad mutex | normal | - | unknown | 0x800201c3 | control | n/a |
| `sceKernelLockMutexCB` | Bad mutex | intr-off | L176 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutexCB` | Bad mutex | intr-ctx | L374 | 0x80020064 | 0x800201c3 | known deviation | context |
| `sceKernelLockMutexCB` | Bad mutex | disp-off | L177 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutexCB` | Bad count | normal | - | unknown | 0x800201bd | control | n/a |
| `sceKernelLockMutexCB` | Bad count | intr-off | L178 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutexCB` | Bad count | intr-ctx | L375 | 0x80020064 | 0x800201bd | known deviation | context |
| `sceKernelLockMutexCB` | Bad count | disp-off | L179 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutexCB` | Valid mutex | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelLockMutexCB` | Valid mutex | intr-off | L180 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockMutexCB` | Valid mutex | intr-ctx | L376 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceKernelLockMutexCB` | Valid mutex | disp-off | L181 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelLockLwMutex` | Bad count | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelLockLwMutex` | Bad count | intr-off | L184 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutex` | Bad count | intr-ctx | L377 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutex` | Bad count | disp-off | L185 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutex` | Valid mutex | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelLockLwMutex` | Valid mutex | intr-off | L186 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutex` | Valid mutex | intr-ctx | L378 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutex` | Valid mutex | disp-off | L187 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutexCB` | Bad count | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelLockLwMutexCB` | Bad count | intr-off | L190 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutexCB` | Bad count | intr-ctx | L379 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutexCB` | Bad count | disp-off | L191 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutexCB` | Valid mutex | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelLockLwMutexCB` | Valid mutex | intr-off | L192 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutexCB` | Valid mutex | intr-ctx | L380 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceKernelLockLwMutexCB` | Valid mutex | disp-off | L193 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelWaitThreadEnd` | Bad thread | normal | - | unknown | 0x80020197 | control | n/a |
| `sceKernelWaitThreadEnd` | Bad thread | intr-off | L204 | 0x80020197 | 0x80020197 | **CONFORMS** | object |
| `sceKernelWaitThreadEnd` | Bad thread | intr-ctx | L383 | 0x80020197 | 0x80020197 | **CONFORMS** | object |
| `sceKernelWaitThreadEnd` | Bad thread | disp-off | L205 | 0x80020197 | 0x80020197 | CONFORMS | object |
| `sceKernelWaitThreadEnd` | Not running | normal | - | unknown | 0x800201a2 | control | n/a |
| `sceKernelWaitThreadEnd` | Not running | intr-off | L206 | 0x800201a7 | 0x800201a2 | known deviation | context |
| `sceKernelWaitThreadEnd` | Not running | intr-ctx | L384 | 0x80020064 | 0x800201a2 | known deviation | context |
| `sceKernelWaitThreadEnd` | Not running | disp-off | L207 | 0x800201a7 | 0x800201a2 | known deviation | context |
| `sceKernelWaitThreadEnd` | Running | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceKernelWaitThreadEnd` | Running | intr-off | L208 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitThreadEnd` | Running | intr-ctx | L385 | 0x80020064 | not run | NOT RUN | context |
| `sceKernelWaitThreadEnd` | Running | disp-off | L209 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitThreadEndCB` | Bad thread | normal | - | unknown | 0x80020197 | control | n/a |
| `sceKernelWaitThreadEndCB` | Bad thread | intr-off | L212 | 0x80020197 | 0x80020197 | **CONFORMS** | object |
| `sceKernelWaitThreadEndCB` | Bad thread | intr-ctx | L386 | 0x80020197 | 0x80020197 | **CONFORMS** | object |
| `sceKernelWaitThreadEndCB` | Bad thread | disp-off | L213 | 0x80020197 | 0x80020197 | CONFORMS | object |
| `sceKernelWaitThreadEndCB` | Not running | normal | - | unknown | 0x800201a2 | control | n/a |
| `sceKernelWaitThreadEndCB` | Not running | intr-off | L214 | 0x800201a7 | 0x800201a2 | known deviation | context |
| `sceKernelWaitThreadEndCB` | Not running | intr-ctx | L387 | 0x80020064 | 0x800201a2 | known deviation | context |
| `sceKernelWaitThreadEndCB` | Not running | disp-off | L215 | 0x800201a7 | 0x800201a2 | known deviation | context |
| `sceKernelWaitThreadEndCB` | Running | normal | - | unknown | WOULD_BLOCK | control | n/a |
| `sceKernelWaitThreadEndCB` | Running | intr-off | L216 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelWaitThreadEndCB` | Running | intr-ctx | L388 | 0x80020064 | not run | NOT RUN | context |
| `sceKernelWaitThreadEndCB` | Running | disp-off | L217 | 0x800201a7 | 0x800201a7 | **CONFORMS** | context |
| `sceKernelVolatileMemLock` | While not locked | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelVolatileMemLock` | While not locked | intr-off | L290 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelVolatileMemLock` | While not locked | intr-ctx | L412 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceKernelVolatileMemLock` | While not locked | disp-off | L291 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelVolatileMemLock` | While locked | normal | - | unknown | 0x00000000 | control | n/a |
| `sceKernelVolatileMemLock` | While locked | intr-off | L292 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceKernelVolatileMemLock` | While locked | intr-ctx | - | unknown | 0x00000000 | control | unknown |
| `sceKernelVolatileMemLock` | While locked | disp-off | L293 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceUmdWaitDriveStat` | Invalid type | normal | - | unknown | 0x00000000 | control | n/a |
| `sceUmdWaitDriveStat` | Invalid type | intr-off | L296 | 0x80010016 | 0x00000000 | known deviation | param |
| `sceUmdWaitDriveStat` | Invalid type | intr-ctx | L413 | 0x80010016 | 0x00000000 | known deviation | param |
| `sceUmdWaitDriveStat` | Invalid type | disp-off | L297 | 0x80010016 | 0x00000000 | known deviation | param |
| `sceUmdWaitDriveStat` | Valid type | normal | - | unknown | 0x00000000 | control | n/a |
| `sceUmdWaitDriveStat` | Valid type | intr-off | L298 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceUmdWaitDriveStat` | Valid type | intr-ctx | L414 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceUmdWaitDriveStat` | Valid type | disp-off | L299 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceUmdWaitDriveStatWithTimer` | Invalid type | normal | - | unknown | 0x00000000 | control | n/a |
| `sceUmdWaitDriveStatWithTimer` | Invalid type | intr-off | L302 | 0x80010016 | 0x00000000 | known deviation | param |
| `sceUmdWaitDriveStatWithTimer` | Invalid type | intr-ctx | L415 | 0x80010016 | 0x00000000 | known deviation | param |
| `sceUmdWaitDriveStatWithTimer` | Invalid type | disp-off | L303 | 0x80010016 | 0x00000000 | known deviation | param |
| `sceUmdWaitDriveStatWithTimer` | Valid type | normal | - | unknown | 0x00000000 | control | n/a |
| `sceUmdWaitDriveStatWithTimer` | Valid type | intr-off | L304 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceUmdWaitDriveStatWithTimer` | Valid type | intr-ctx | L416 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceUmdWaitDriveStatWithTimer` | Valid type | disp-off | L305 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceUmdWaitDriveStatCB` | Invalid type | normal | - | unknown | 0x00000000 | control | n/a |
| `sceUmdWaitDriveStatCB` | Invalid type | intr-off | L308 | 0x80010016 | 0x00000000 | known deviation | param |
| `sceUmdWaitDriveStatCB` | Invalid type | intr-ctx | L417 | 0x80010016 | 0x00000000 | known deviation | param |
| `sceUmdWaitDriveStatCB` | Invalid type | disp-off | L309 | 0x80010016 | 0x00000000 | known deviation | param |
| `sceUmdWaitDriveStatCB` | Valid type | normal | - | unknown | 0x00000000 | control | n/a |
| `sceUmdWaitDriveStatCB` | Valid type | intr-off | L310 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceUmdWaitDriveStatCB` | Valid type | intr-ctx | L418 | 0x80020064 | 0x00000000 | known deviation | context |
| `sceUmdWaitDriveStatCB` | Valid type | disp-off | L311 | 0x800201a7 | 0x00000000 | known deviation | context |
| `sceCtrlReadBufferPositive` | Bad count | normal | - | unknown | not run | NOT RUN | n/a |
| `sceCtrlReadBufferPositive` | Bad count | intr-off | L220 | 0x80000104 | not run | NOT RUN | param |
| `sceCtrlReadBufferPositive` | Bad count | intr-ctx | L389 | 0x80000104 | not run | NOT RUN | param |
| `sceCtrlReadBufferPositive` | Bad count | disp-off | L221 | 0x80000104 | not run | NOT RUN | param |
| `sceCtrlReadBufferPositive` | Valid | normal | - | unknown | not run | NOT RUN | n/a |
| `sceCtrlReadBufferPositive` | Valid | intr-off | L222 | 0x800201a7 | not run | NOT RUN | context |
| `sceCtrlReadBufferPositive` | Valid | intr-ctx | L390 | 0x80020064 | not run | NOT RUN | context |
| `sceCtrlReadBufferPositive` | Valid | disp-off | L223 | 0x800201a7 | not run | NOT RUN | context |
| `sceIoRead` | Bad file | normal | - | unknown | 0x80010009 | control | n/a |
| `sceIoRead` | Bad file | intr-off | L258 | 0x80020323 | 0x80010009 | known deviation | object |
| `sceIoRead` | Bad file | intr-ctx | L401 | 0x80020323 | 0x80010009 | known deviation | object |
| `sceIoRead` | Bad file | disp-off | L259 | 0x80020323 | 0x80010009 | known deviation | object |
| `sceIoWrite` | Bad file | normal | - | unknown | 0x80010009 | control | n/a |
| `sceIoWrite` | Bad file | intr-off | L264 | 0x80020323 | 0x80010009 | known deviation | object |
| `sceIoWrite` | Bad file | intr-ctx | L403 | 0x80020323 | 0x80010009 | known deviation | object |
| `sceIoWrite` | Bad file | disp-off | L265 | 0x80020323 | 0x80010009 | known deviation | object |
| `sceIoWaitAsync` | Bad file | normal | - | unknown | 0x00000000 | control | n/a |
| `sceIoWaitAsync` | Bad file | intr-off | L270 | 0x80020323 | 0x00000000 | known deviation | object |
| `sceIoWaitAsync` | Bad file | intr-ctx | L405 | 0x80020323 | 0x00000000 | known deviation | object |
| `sceIoWaitAsync` | Bad file | disp-off | L271 | 0x80020323 | 0x00000000 | known deviation | object |
| `sceIoWaitAsyncCB` | Bad file | normal | - | unknown | 0x00000000 | control | n/a |
| `sceIoWaitAsyncCB` | Bad file | intr-off | L276 | 0x80020323 | 0x00000000 | known deviation | object |
| `sceIoWaitAsyncCB` | Bad file | intr-ctx | L407 | 0x80020323 | 0x00000000 | known deviation | object |
| `sceIoWaitAsyncCB` | Bad file | disp-off | L277 | 0x80020323 | 0x00000000 | known deviation | object |

## Missing substrate, and the prerequisite work it implies

Ordered by dependency. S1 is implemented by PR #346. Each remaining item is what must
exist before the cells it unblocks can be measured at all.

| # | Substrate | Cells unblocked | Nature |
| ---: | --- | --- | --- |
| S1 | `sceKernelSuspendDispatchThread` / `sceKernelResumeDispatchThread` registered, backed by dispatch-suspension state `s_dispatch_enabled` | 54-cell dispatch-disabled column | Done (#346) |
| S2 | Test-only reset for `hle.c`'s controller sample ring | 6 `sceCtrlReadBufferPositive` cells | test-only export, production-neutral |
| S3 | A bounded-execution guard for HLE calls made from interrupt context (today an unsatisfied wait spins with no yield point) | the 12 `spin-unbounded` NOT RUN cells become directly measurable instead of inferred | production; subsumed by S5 |
| S4 | `audio.c` linked into the selftest, or a narrower audio channel fixture | `sceAudioOutputBlocking` cells (L226-L231, L391-L392) | test wiring |
| S5 | `ILLEGAL_CONTEXT` / `CAN_NOT_WAIT` returned by the handlers themselves, per-API, respecting each API's own error precedence | PR-B landed 28, PR-C1 8, issue #43 4, #2 (plain Mutex) 12. #88 still owns **50** context-semantics cells: PR-C2's 20 known deviations, plus PR-D's 18 known deviations and 12 spin-unbounded NOT RUN. PR-E separately owns **30** parameter/object-precedence cells. The 6 plain-Mutex `intr-ctx` and **5** VolatileMemLock cells stay deferred to #2/#79 and are counted as known deviations here | production; the actual #88 semantics work. `sched_wait_permitted()` provides the state query; the precedence lives in each handler |

## Dependency-ordered implementation PRs derived from the failing matrix

Each is scoped so its acceptance criterion is "these named matrix cells flip from
known deviation to CONFORMS, and their `base[]` entries are promoted to `hw`". No PR may
introduce a universal pre-handler gate: fact 3 above rules it out.

1. **PR-A - dispatch-suspension state (S1) [IMPLEMENTED #346].** Added real dispatch-suspend/resume
   state `s_dispatch_enabled` to the scheduler, registered both NIDs, and wired the 4th context column (`disp-off`).
   52/54 cells executed (2 fixture-skipped). Acceptance: 10 CONFORMS, 129 known deviations, 22 NOT RUN, 0 UNTESTABLE.
2. **PR-B - `CAN_NOT_WAIT` for the genuinely blocking waits with interrupts or dispatch
   disabled [IMPLEMENTED].** `sceKernelDelayThread`/`CB`, `sceKernelSleepThread`/`CB`,
   `sceDisplayWaitVblank`/`Start`, `sceKernelWaitSema`/`CB` valid,
   `sceKernelWaitEventFlag`/`CB` valid, `sceKernelWaitThreadEnd`/`CB` running. Each handler
   checks after its own parameter and object validation, not before, and only once it has
   established that this invocation would genuinely block; `sched_wait_permitted()` supplies
   the state and no policy. Scoped at 12 probes x 2 context columns = 24 cells; landed 28,
   the extra 4 being `WaitSema`/`CB` invalid count, which share the valid case's code path
   (see the classification section above). Acceptance: 38 CONFORMS, 101 known deviations,
   22 NOT RUN, 0 UNTESTABLE.
3. **PR-C - `CAN_NOT_WAIT` for the immediate/satisfied cases with interrupts or dispatch disabled.**
   Discovery scope was **54 cells** (27 probes x `intr-off` + `disp-off`): `WaitSema` bad sema,
   `WaitEventFlag` bad flag and already-set, `AllocateFpl` bad and valid, `LockMutex` bad,
   invalid count and valid, `LockLwMutex` invalid count and valid, `WaitThreadEnd` not running,
   `VolatileMemLock` while not locked and while locked, `UmdWaitDriveStat*` valid type.
   Audit of the handlers behind those cells moved 22 of them out of #88 (see below), leaving
   **32 cells of implementation ownership**, split into PR-C1 and PR-C2.
   This is where each API's context-vs-object precedence gets inverted to match hardware:
   `WaitSema`, `WaitEventFlag` **and `AllocateFpl`** check the context *before* the object
   lookup (L54/L55, L70/L71, L102/L103), which PR-B deliberately did not do. `WaitSema`/`CB`
   invalid count was originally counted here; PR-B already landed those 4 cells.
   * **PR-C1 - blocking FPL allocate [IMPLEMENTED].** `sceKernelAllocateFpl` / `...CB`, bad and
     valid, `intr-off` + `disp-off` = **8 cells**. `h_AllocateFpl` was an alias of
     `h_TryAllocateFpl`; splitting it lets one leading check answer all four cells, because
     hardware puts the context decision ahead of the object lookup. `sceKernelTryAllocateFpl`
     keeps its own registration and is unchanged - it does not block, so the rule does not
     reach it. No #16 reclamation/lifetime work absorbed.
     Acceptance: 46 CONFORMS, 93 known deviations, 22 NOT RUN, 0 UNTESTABLE.
   * **PR-C2 - the remaining wait APIs.** Discovery scope was `WaitSema`/`CB` bad sema,
     `WaitEventFlag`/`CB` bad flag and already-set, `LockLwMutex`/`CB` bad count and valid,
     `WaitThreadEnd`/`CB` not running: **12 probes x 2 columns = 24 cells.** Note it reverses
     PR-B's "only once it would genuinely block" placement for `WaitSema` and `WaitEventFlag`:
     the measured bad-object cells put the check ahead of the lookup, which necessarily puts it
     ahead of the satisfaction test too.
     **Issue #43 landed the `WaitSema`/`CB` bad-sema pair (4 cells)** as part of the semaphore
     count-validation fix, for exactly that reason - the reversal and the validation are one
     edit. **20 cells remain**: `WaitEventFlag`/`CB` bad flag and already-set,
     `LockLwMutex`/`CB` bad count and valid, `WaitThreadEnd`/`CB` not running.
     These API families have live HST import exposure, so the remaining leg wants a bounded
     HST route alongside the matrix.
   **Deferred out of #88 by the same audit** - the cells remain in the tables above, pinned and
   counted as known deviations, until the owning subsystem work lands:
   * `sceKernelLockMutex` / `...CB`, all 3 scenarios -> **historical PR #2**.
     The plain-Mutex campaign (issue #2) landed a real typed handler in `src/rt/mutex.c` whose
     context check precedes the object lookup, so the 12 `intr-off`/`disp-off` cells now CONFORM.
     The 6 `intr-ctx` cells still answer the real object/count value instead of `ILLEGAL_CONTEXT`
     and stay counted as known deviations; they belong to PR-D's `ILLEGAL_CONTEXT` work.
   * `sceKernelVolatileMemLock` while-free and while-locked -> **[#79](https://github.com/Jstar269/nakagawa-recomp/issues/79)**.
     The handler ignores lock state entirely, so both scenarios are one code path. **5 cells**
     (4 `intr-off`/`disp-off` + 1 `intr-ctx`; while-locked `intr-ctx` is a control).
   * `sceUmdWaitDriveStat*` valid type -> **PR-E**, with the invalid-type cells. Not an ownership
     carve-out but a sequencing one: the fixture uses type `0x00` for invalid and `0x20` for
     valid, both of which satisfy the drive mask, so they are indistinguishable until PR-E's type
     validation exists. A context check placed ahead of that validation would drive the
     invalid-type cells to a third value and register as a **regression**, not a deviation.
4. **PR-D - `ILLEGAL_CONTEXT` from interrupt context (S3+S5).** Requires PR-B/PR-C so the
   handlers already have a context check to extend. After the Mutex, VolatileMem and UMD
   ownership moves it retires the 12 `spin-unbounded` NOT RUN cells plus **18** `intr-ctx`
   known-deviation cells = **30 cells**. (Issue #43 moved two `WaitSema`/`CB` invalid-count
   cells from the first group to the second by making that call return instead of spin; the
   total is unchanged.) Must preserve the two documented inversions:
   `sceKernelWaitThreadEnd(0)` keeps returning `ILLEGAL_THID` (`0x80020197`), and `sceDisplayWaitVblank`
   keeps *succeeding* with 1 (`0x00000001`).
5. **PR-E - parameter-precedence & object-error corrections.** `sceUmdWaitDriveStat*` invalid type must
   return `ERRNO_INVALID_ARGUMENT` (`0x80010016`) across 3 contexts (9 cells) and valid type must
   return `CAN_NOT_WAIT` in `intr-off`/`disp-off` and `ILLEGAL_CONTEXT` in `intr-ctx` (9 cells) -
   the two are inseparable, since the valid/invalid distinction does not exist until the type
   check does; `sceIoRead`/`sceIoWrite`/`sceIoWaitAsync`/`sceIoWaitAsyncCB` bad
   descriptor must return `BADF` `0x80020323` rather than `0x80010009` or `0x00000000` across 3 contexts (12 cells).
   **30 cells total.**
6. **PR-F - controller ring reset (S2), then `sceCtrlReadBufferPositive` precedence.**
   `INVALID_SIZE` (`0x80000104`) for count 256 and `CAN_NOT_WAIT` (`0x800201a7`) for valid across 3 contexts (6 cells). Test-only prerequisite first.
7. **PR-G - registration of the 31 unregistered `waits.cpp` APIs**, in whatever order
   their subsystems land (Mbx, Vpl, Tlspl, MsgPipe blocking forms, `DelaySysClockThread`,
   the `sceDisplay` CB/Multi variants, `sceIoGetAsyncStat`). Each expands the matrix
   rather than changing it.

`sceGeListSync` / `sceGeDrawSync` and `sceAudioOutputBlocking` are intentionally absent
from this ordering: their hardware cells are recorded above, but exercising them belongs
with the GE lifecycle (#25/#44) and audio work respectively, not with #88.

## Related

* [#88](https://github.com/Jstar269/nakagawa-recomp/issues/88) - pending interrupt delivery and CPU interrupt-state semantics
* [#76](https://github.com/Jstar269/nakagawa-recomp/issues/76) - executable production-HLE behavioral tests and semantic CI gates
* [`HARDWARE_ORACLE.md`](HARDWARE_ORACLE.md) - bounded hardware-oracle proposal and limits
* [AGENTS.md](../AGENTS.md) - evidence-tier definitions used throughout this document
