# Clean-room behaviour specification — cooperative PSP thread scheduler

Status: spec — no code in this document. Written under the campaign protocol
(`../INDEPENDENCE_CAMPAIGN.md` §5) as a **spec-session** output: the derived
implementation (`src/rt/sched.c`, sal063 lineage, ledger record `scheduler`)
was readable while writing, and nothing from it is reproduced here beyond the
interface spellings §2 fixes as `SEAM:` identifiers.

**Re-scope (maintainer decision, 2026-10-20).** The clean-room scheduler rewrite this spec was written for
is no longer planned: [#340](https://github.com/Jstar269/nakagawa-recomp/issues/340) was closed as not planned, and working inherited code is no longer
rewritten only to make it original (`../INDEPENDENCE_CAMPAIGN.md`). The spec stays as a behaviour
reference; the derived `src/rt/sched.c` is the shipped scheduler. The rank below is the campaign's
original order.

Rank: first (campaign §6, item 1). The scheduler is the permanent host
boundary every LLE domain crosses, it owns the strongest source-owned tests
in the tree, and LLE PR 6 needs its fatal-signal consumption defined before
that PR lands.

## 0. Scope, workspace, and reading rule

**In scope.** Guest-visible thread semantics: lifecycle, priority selection,
blocking waits and their timeouts, wake banking, join/exit-status capture,
interrupt and dispatch suspension, the monotonic guest timeline and its
display observations, callback waiting and dispatch discipline, role-UID
capture, and the exact seam identifiers in §2 that unchanged callers
(`src/rt/hle.c`, `src/rt/driver.c`, `src/rt/recomp.c`, `src/rt/guest_interp.c`,
generated-code yield points) invoke.

**Out of scope (carry-over symbols).** The same translation unit today also
provides boot diagnostics, guest-PC profiler hooks, and a field-writer
intercept consumed through yield points. The implementation re-provides those
symbols with behaviour per their header comments in `src/rt/recomp.h`,
unchanged and unextended; they receive no new specification here and must not
gain PSP-semantic behaviour. Host context mechanism (`src/rt/sr_coro.h`,
fibres on Windows / ucontext where supported) is an unchanged given header:
the implementation uses it, never re-specifies it.

**Reading rule.** Every numbered behaviour cites `[Cn]` from the citation
catalog (§9) or is marked `project decision` with its rationale. A fact with
neither is a defect in this spec — file it before implementing.

**Workspace allow-list** (campaign §5.2: export exactly these, no `.git`,
record the inventory with SHA-256):

- this spec;
- ABI headers the implementation must satisfy: `src/rt/recomp.h`,
  `src/rt/sr_coro.h`, `src/rt/sr_coro_selftest.c` (usage example only),
  `src/rt/intr_conformance.h` (conformance contract, read-only);
- landing LLE headers for seam positions only (read-only, may move before
  admission — re-check at build time): `src/rt/cpu_lle.h`,
  `src/rt/domain_mode.h` on `main` (landed with #219 and #223);
- black-box tests (§8);
- minimum build files to compile the above.

**Deletion list.** No derived file is in the allow-list. If the export
contains `src/rt/sched.c`, `src/rt/hle.c`, `src/rt/driver.c`,
`src/rt/recomp.c`, `src/ref/*`, or any file under `build/`, `logs/`,
`oracle/`, `place_game_here/`, delete it and record the deletion before
starting. The pre-replacement `sched.c` and any upstream text must never
enter the clean directory — the admission similarity check runs outside it.

## 1. Component role

The scheduler multiplexes PSP threads onto host execution contexts. Guest
code is straight-line translated C that cannot suspend mid-call-stack, so
each guest thread runs on its own host context and the single shared
`CpuState` register file follows whichever thread is running [C1], [C2].
Preemption is cooperative at yield points the code generator emits at
function entries and loop back-edges; when the scheduler is inactive those
yield points are inert no-ops that change nothing observable [C1].

The scheduler never invents PSP semantics: unknown operations fail closed
and missing behaviour stays visible [C3], [C4].

## 2. Normative interface (`SEAM:` identifiers)

Spellings are fixed because the callers listed in §0 are unchanged by this
rewrite. These identifiers are project seam names defined by this spec, not
upstream expression; the admission similarity check exempts exactly this
table plus ABI names from §3. `SEAM:` marks each fixed spelling.

Thread lifecycle: `SEAM: sched_init` (bind the shared CpuState before any
guest execution), `SEAM: sched_run` (run from the entry thread with entry
address, argument length, argument pointer), `SEAM: sched_create_thread`
(entry address, priority, stack size → UID, or zero on exhaustion),
`SEAM: sched_start_thread` (UID, argument length, argument pointer →
launch the dormant thread), `SEAM: sched_exit_current` (non-delete exit;
signed-negative status values normalise to an illegal-argument indication
— project decision pinned by black-box tests [C5]),
`SEAM: sched_exit_current_unchecked` (raw status for non-ThreadMan
teardown paths), `SEAM: sched_exit_current_delete` (exit and remove the
thread object), `SEAM: sched_set_priority` (change priority),
`SEAM: sched_terminate_thread`, `SEAM: sched_delete_thread`,
`SEAM: sched_is_dormant`, `SEAM: sched_current_uid` (zero when no thread
is current — PSP ABI fact [C6]), `SEAM: sched_current_priority`.

Blocking and time: `SEAM: sched_delay_current` (block the running thread
for a microsecond count — a delay always parks, even for zero; the
`sceKernelDelayThread`/`sceKernelDelayThreadCB` handlers return before
calling it for zero, per PSP-3001 6.6.1 measurements),
`SEAM: sched_block_on` (block until the matching wake),
`SEAM: sched_block_on_timeout` (block until wake or microsecond timeout;
returns true on timeout), `SEAM: sched_wake` (ready every thread blocked
on the object), `SEAM: sched_wake_one_object_waiter` (ready one named
thread), `SEAM: sched_preempt` (yield now if a higher-priority thread is
ready), `SEAM: sched_vtime_us`, `SEAM: sched_vtime_deadline_after`
(saturating addition — project decision, overflow must never wrap a
deadline into the past), `SEAM: sched_vtime_refresh`,
`SEAM: sr_hle_advance_time` (the single guest-time charge entry),
`SEAM: sched_wait_vblank_start` (always blocks to the next vblank start
edge), `SEAM: sched_wait_vblank` (returns true without blocking when
already inside the vblank interval, else blocks and returns false).

Sleep/wakeup banking: `SEAM: sched_thread_sleep`,
`SEAM: sched_thread_sleep_cb` (callback-receiving sleep variant),
`SEAM: sched_thread_wakeup` (banks the wake when the target is not
asleep), `SEAM: sched_thread_cancel_wakeup` (UID zero means current
thread), `SEAM: sched_current_has_pending_wakeup`.

Join: `SEAM: sched_set_current_join_target`,
`SEAM: sched_clear_current_join_target`,
`SEAM: sched_take_current_join_result` (consuming take),
`SEAM: sched_current_join_result_pending` (non-consuming peek),
`SEAM: sched_thread_exit_status`.

Interrupts and dispatch: `SEAM: sched_suspend_interrupts` (returns the
previous enable state), `SEAM: sched_resume_interrupts` (restores a prior
state), `SEAM: sched_interrupts_enabled`,
`SEAM: sched_suspend_dispatch`, `SEAM: sched_resume_dispatch`,
`SEAM: sched_dispatch_enabled`, `SEAM: sched_wait_permitted` (state-only
query: interrupts enabled AND dispatch enabled; carries no blocking policy
itself — each handler asks at its own point [C1]),
`SEAM: sched_raise_interrupt` (latch a source; delivery happens at a
scheduler boundary, never inline), `SEAM: sched_pending_interrupts`,
`SEAM: sched_is_intr_context`, interrupt source constants
`SEAM: SCHED_INTR_VBLANK` (value 1, coalescing display source) and
`SEAM: SCHED_INTR_GE` (value 2, reserved until its handler lands).

Display observations: `SEAM: sched_display_current_hcount`,
`SEAM: sched_display_accumulated_hcount`,
`SEAM: sched_display_is_vblank`,
`SEAM: sr_display_advance_vcount` (period accounting, separate from
vblank servicing), `SEAM: sr_vblank_quantum_due`.

Callbacks: `SEAM: sched_set_current_cb_wait`,
`SEAM: sched_wake_callbacks`; argument packing and single-callback
dispatch follow the static-inline helpers in `src/rt/recomp.h`, which are
given (unchanged) headers, not specified here.

Roles and status: `SEAM: SR_ROLE_UID_NONE` (value all-bits-one; the only
representation of "no role" — project decision [C5]),
`SEAM: sched_root_uid`, `SEAM: sched_worker_uid`,
`SEAM: sched_launcher_uid`, `SEAM: sched_role_uid_captured`,
`SEAM: sched_uid_is_root`, `SEAM: sched_uid_is_worker`,
`SEAM: sched_uid_is_launcher`, `SEAM: sched_current_is_worker`,
`SEAM: sched_current_is_launcher`, `SEAM: sched_thread_run_status` filling
the `SEAM: SrThreadRunStatus` record whose fields mirror the PSP thread
status structure [C6], `SEAM: CTRL_WAIT_OBJ` (project-coined wait-object
constant, value `0xC471D000`, for controller blocking reads).

Yield and switch globals: `SEAM: sr_sched_on`, `SEAM: sr_timeslice`,
`SEAM: sr_yield`, `SEAM: SR_YIELD` macro behaviour per its header comment
(timeslice decrement, program-counter handoff before switching) [C1].

Teardown seam: `SEAM: sr_hle_release_thread_resources` (release HLE-side
resources when a thread object goes away; ordering pinned by [C5]).

Consumed symbols that carry no `SEAM:` marker above are mapped in §10. The
coverage test `SpecSeamCoverageTests` in `tools/test_sched_invariants.py` checks
every outside consumer against the two mappings together.

## 3. Behaviour

### 3.1 Lifecycle and states

A thread is dormant after creation until started; only ready threads are
eligible; exactly one thread is running; blocked/sleeping threads are
excluded from selection until their wake condition fires; exited threads
stay queryable for exit status until deleted [C6], [C5]. Creating outside
table or stack-arena capacity fails with a zero UID, never a partial
thread [C5]. Entry-function return is thread exit (the guest has no other
way out of its root call) [C5]. Terminate removes eligibility without
deleting the object; delete removes the object; double-delete and
terminate-after-delete fail closed [C3], [C5].

### 3.2 Priority selection and preemption

Strict priority wins; equal priority rotates fairly; a sleeping, blocked,
or dormant thread never wins over a ready one; a running thread that
becomes non-runnable loses the CPU at the next boundary [C5], [C7]. An
expired timed wait promotes its thread into strict-priority contention
rather than granting the CPU directly [C5]. Retail firmware's allocator
mutations assume interrupt suspension also suppresses preemption on the
single CPU [C1]. The timeslice quantum itself is a project decision
(internal pacing, not PSP truth); what is normative is fairness (no ready
thread starves while a lower-or-equal peer spins) and the inert-when-off
rule from §1 [C1], [C5].

### 3.3 UIDs

UIDs are allocator outcomes, never configuration: internally consistent,
unique among live objects, never reused while queryable state survives,
and never zero for a real thread (zero is PSP's "current thread" value)
[C6]. The starting value and stride are a project decision (not
guest-visible: handlers and traces must treat UIDs as opaque — the
established internal-consistency rule [C8]); the role accessors in §2 fail
closed on uncaptured roles and never treat UID zero as a captured role
[C5].

### 3.4 Monotonic guest time

One authoritative microsecond timeline. Reading any time API never advances
it; it advances only at scheduler/emulation progression boundaries via the
single charge entry; updates are forward-only so host clock corrections can
never move guest time backward [C1]. Delay deadlines, timeout deadlines,
and remaining-time computations all derive from this timeline through the
shared refresh/deadline/block plumbing — no wait object duplicates time
arithmetic [C1]. Calendar (RTC) reads map the same timeline through a
one-time epoch offset sampled at first RTC use; thereafter they are pure
guest-time arithmetic [C1]. The local-time offset is the console's time
zone setting, owned outside the scheduler and never the host's (#1382).
In turbo mode time is fully
deterministic at scheduler boundaries and never host-dependent [C1].

### 3.5 Sleep/wakeup banking and join

Wake-before-sleep is not lost: a wakeup against a non-sleeping thread banks
exactly one count, a subsequent sleep consumes it instead of blocking, and
cancel clears the bank (UID zero addresses the current thread) [C6], [C5].
Exiting records the status for joiners; the take operation consumes once;
the peek operation never consumes [C5]. Waking an unrelated sleeping thread
as a side effect of an exit path is forbidden — exits wake only their
joiners [C5].

### 3.6 Interrupts, dispatch suspension, wait permission

Suspend returns prior state; resume restores exactly that state (nestable
by construction) [C6], [C5]. While masked, the display source keeps running
but the interrupt-gated counter stops (see §3.7); elapsed periods collapse
into one coalesced pending indication rather than replaying [C9]. Clearing
the mask is itself a timeline boundary: periods already due before the
clear are consumed first, so a period that elapsed while enabled is never
misclassified as masked [C1]. Raising a source latches it; servicing
happens at scheduler boundaries in interrupt context; handler execution
preserves the interrupted frame and restores it afterwards [C10], [C5].
Suspending thread dispatch (the `sceKernelSuspendDispatchThread` pair) is
independent of interrupt masking; blocking handlers consult the combined
state-only query at their own point after their own validation, and a
universal pre-handler gate is explicitly ruled out [C1]. Blocking where
waits are not permitted returns PSP's cannot-wait error, never a silent
park and never fabricated success [C6], [C3].
The opt-in GE completion source (`SR_GE_TIMING=1`, issue #1296) is serviced
the same way: after any owed display episode and before alarms, in interrupt
context, and only while interrupts are enabled; while a thread waits, its
deadline is an idle wake source. Its deadlines come from the GE time
model in `src/rt/ge.c`, measured on a PSP-3000 (issue #1296,
`CASE=ge-timing`). Project decision.

### 3.7 Vblank waits and display observations

Wait-for-vblank-start always blocks to the next start edge, even inside the
interval; wait-for-vblank returns immediately (true) inside the interval and
blocks (returning false) outside it [C6], [C5]. The display source and the
delivered counter are different quantities: the HCOUNT source runs at the
rational 60000/1001 Hz model regardless of masking, while the guest-visible
VCOUNT advances only when the scheduler latches elapsed source periods —
plus zero when a mask crossed no period, plus exactly one when it crossed
one or more (hardware-measured; never N) [C9]. Servicing (framebuffer,
interrupt, callback side effects) runs at most once per latched event and
never re-increments the counter [C1]. Vblank-only waits are not deadlocks
and unwakeable vblank states are reported, not hung [C5]. Enabled-but-starved
multi-period servicing stays corroborative-only until measured [C9].

### 3.8 Callbacks

Callback functions take (notify count, notify argument, registered common
argument) in the first three argument registers — PSP ABI fact [C6]. Each
armed callback runs as a nested call on the owning thread's live register
file touching only what a real branch-and-link sets; the full pre-call
snapshot is restored on return; callee-saved state and the guest stack are
the callback's own prologue/epilogue responsibility per the MIPS calling
convention [C11]. A callback returning nonzero is automatically deleted
[C6], [C5]. A sleeping callback-wait consumes a wake banked during dispatch;
dispatch reports the total serviced count; notifying a non-waiting thread
is a no-op [C5]. The existing header comment cites an emulator
implementation for the argument order — this spec replaces that citation
with [C6] plus the black-box packing test in §8; the implementation must
never cite the emulator.

### 3.9 Roles and title configuration

Role UIDs capture whichever UID the allocator gave the thread that took the
role; unconfigured entries are inert and historical launcher/worker UIDs are
ordinary threads when unconfigured [C5]. The title-configuration binding
surface itself is unchanged project-owned interface (not derived code) and
is out of scope except that role capture keeps working through it [C5].

### 3.10 LLE seam (PR 6 forward reference)

The implementation exposes the consumption point for the import seam's
fatal signal and for exception/interrupt delivery metadata as defined by
the landing LLE headers (`cpu_lle.h` flow kinds, `domain_mode.h` lock and
mode table) [C12]. It implements the scheduler side only — table locking
order, fatal-signal propagation to thread teardown, delivery at scheduler
boundaries — and invents no LLE policy: the LLE lane owns the producers.
If those headers moved since this spec, the implementation follows the
landed versions and records the exact base in the provenance record.

### 3.11 Failure policy

Invalid UIDs, double teardown, waits in non-waitable context, unmapped
interrupt sources, and vblank waits with no display owner all fail closed
with the PSP error or handler-level rejection the black-box tests assert —
never success, never a hang, never a latch that hides the gap [C3], [C4], [C5].

## 4. Non-requirements (explicitly not specified)

Host context mechanism and stack layout; timeslice quantum counts;
diagnostic/log categories and formats (minimal presence-only logging is
acceptable); profiler and boot-probe internals beyond §0 carry-over;
performance characteristics; any behaviour of excluded HLE (audio, ISO,
PGF, PGD) beyond failing through the existing seams; title-specific
threading quirks — a title profile describes titles, never behaviour
[C13].

## 5. Clean-room constraints for this item

- Upstream text in the authoring context: the spec session read the derived
  `sched.c` and its history; the implementation session must not (allow-list
  §0). Record both facts in the provenance entry.
- The `sched_*`/`sr_*`/`SCHED_*`/`CTRL_WAIT_OBJ`/`SrThreadRunStatus`
  spellings in §2 are spec-defined seam identifiers. All other names,
  decompositions, helper shapes, and constant orderings must be the
  implementer's own; identical helper decomposition or control-flow shape
  with renamed identifiers fails review regardless of the similarity score.
- Sony API names (thread, error, callback, status identifiers) are ABI
  facts [C6]; PSP error values and GE/interrupt encodings likewise. Citing
  them is required, and the similarity check's identifier exemption covers
  only §2 plus these ABI names.
- The old implementation may serve as a black-box oracle only: same inputs
  in, compare guest-visible outputs, localise divergences without reading
  its code; resolve every divergence from this spec and [C1]–[C13], with
  hardware envelopes [C9] outranking both implementations.

## 6. Similarity admission for this item

Run the proposed similarity check of campaign §5.1 at admission, candidate
= new `src/rt/sched.c` (+ touched declaration lines in `src/rt/recomp.h`,
if any), reference = pre-replacement `sched.c` at the recorded base:
normalised token-trigram overlap below 0.15 after exempting §2/ABI
identifiers and includes; no identical 6-line run outside cited-ABI tables;
constant orderings above 8 entries justified or reordered. Calibrate the
thresholds on this item before they bind later items (campaign open
question 7). Attach the report to the provenance record either way.

## 7. Provenance record fields (fill at admission)

Classification proposed: `project-authored-independent`. Behaviour sources:
this spec + [C1]–[C13] by section. Upstream consulted by spec session: yes
(derived `sched.c`, recorded here); by implementation session: no (workspace
inventory attached). Evidence tiers claimed: S for PSPSDK/MIPS-convention
contracts, H for hardware-envelope-backed cells (§3.7 mask rule and any Loop
C cells available at admission), project-decision for §3.2-quantum/§3.3
allocation/§3.5-ordering details pinned by [C5]. Commit that replaced the
old code: (fill). Maintainer attestation: (maintainer signs; agents do not).

## 8. Black-box test plan (what the implementation session receives)

Existing (all source-owned, game-input-free; all must pass unmodified):

1. `sched-selftest` build target running `src/rt/sched_selftest.c`
   (~50 cases: selection fairness, lifecycle, timeouts, exit-status
   normalisation, callbacks, clocks, vblank edges, interrupt frames).
2. `python tools/test_sched_invariants.py` (scheduled-behaviour invariants).
3. `hle-thread-selftest` build target + `python tools/test_callback_correctness.py`
   (production HLE handlers through registered NIDs against the new scheduler).
4. `production-smoke` and `production-smoke-gap` make targets (real driver,
   scheduler startup, AOT-gap interpreter crossing).
5. `cosim-selftest` (AOT/interpreter agreement across CALL/TAIL cells) and,
   once landed, the PR 6/PR 8 LLE-mode assertions that touch §3.10.
6. `python tools/test_intr_waits_matrix.py` (intr-conformance totals).

To author at implementation time (from sources, not from the old code):

1. A timeout-remaining arithmetic test derived from §3.4 (deadline
   construction + refresh-only advancement), citing [C1].
2. Any Loop C kernel cells measured by admission time (banked-wakeup
   races, callback ordering, pending-state visibility), citing envelope
   IDs; unmeasured cells stay project-decision with the test asserting the
   specified behaviour, never the hardware's.

Negative tests (must fail closed, never hang the harness): double delete,
terminate-after-delete, wait where waits are not permitted, join-take
twice, vblank wait with display disabled, callback notify on a non-waiting
thread, suspend/resume state restoration depth. Timeouts bound every
blocking test; a test that needs a wall-clock wait uses the turbo
(deterministic) mode.

## 9. Citation catalog

- [C1] `docs/ARCHITECTURE.md` — scheduler/coroutine model, clock ownership
  (reads never advance; single charge entry; RTC epoch; turbo
  determinism), display-timeline model (source latch vs serviced episode,
  suspend/resume consumption rule), yield-point contract.
- [C2] `src/rt/sr_coro.h` — host coroutine primitive interface (given,
  unchanged).
- [C3] `AGENTS.md` §6 — fail-closed evidence rules (no fake success, no
  hidden gaps, no latch hacks).
- [C4] `docs/ARCHITECTURE.md` HLE-boundary section — unknown operations
  stay visible; missing behaviour fails rather than fabricating.
- [C5] `src/rt/sched_selftest.c` + `tools/test_sched_invariants.py` —
  source-owned behavioural expectations (selection, lifecycle, banking,
  join, callbacks, clocks, vblank edges, interrupt frames, exit-status
  normalisation, role inertness). Tests are behaviour definitions here,
  not just checks.
- [C6] PSPSDK public headers (pspdev/pspsdk,
  `https://github.com/pspdev/pspsdk`): kernel thread/error/callback/status
  declarations — exact header paths and the pin to be confirmed by the
  implementation session against `tools/pspsdk_sync.py` and the lockfile;
  Sony NIDs, error codes, `SceKernelCallbackFunction` signature, UID-zero
  current-thread convention, and the thread-status record layout are ABI
  facts from this source.
- [C7] `docs/research/PSP_THREADING_SEMANTICS.md` — frozen thread-lifecycle
  research and bounded evidence classes (CT/ST oracle itself NOT_RUN;
  reused lifecycle facts as marked).
- [C8] `src/rt/hle.c` header comment — internal-consistency rule for UIDs
  and allocation addresses (project-established; functional equivalence is
  checked by call sequence, not by identifier values).
- [C9] `docs/HARDWARE_ORACLE.md` measured index + `docs/ARCHITECTURE.md`
  display section — masked-window +0/+1 rule (12/12 per delay,
  HARDWARE_MEASURED), HCOUNT source independence, device period
  calibration; enabled-starved multi-period behaviour stays
  CORROBORATIVE_ONLY.
- [C10] `src/rt/intr_conformance.h` — interrupt registration/enable/latch/
  resume/delivery contract the implementation must satisfy.
- [C11] MIPS calling convention (MIPS32 Architecture for Programmers,
  Volume II — cf. the precedent citation of revision 2.62 in
  `src/rt/fp_convert.h`): argument registers, return register, link
  register, callee-saved discipline for nested guest calls.
- [C12] LLE Phase 1 headers (`cpu_lle.h`, `domain_mode.h`, on `main` since
  #219 and #223): flow kinds, COP0-adjacent transfer
  metadata positions, domain table lock and HLE-default rule — seam
  positions only.
- [C13] `docs/TITLE_PROFILE_ARCHITECTURE.md` — title profiles describe,
  never implement; no title identity in generic code.

## 10. Consumed-symbol map

Every scheduler symbol with external linkage that a source file or tool outside
`src/rt/sched.c` consumes must be mapped, either by a `SEAM:` marker in §2 or by a
row below. `SpecSeamCoverageTests` in `tools/test_sched_invariants.py` enforces this
and also fails on a row that names an undeclared symbol, a missing section, or a
duplicate. The rows add no seam names and change no caller; each states what the
consumed entry point must do. Each row cites [Cn] or is a project decision with its
rationale, per the reading rule in §0.

| Symbol | Section | Contract |
| --- | --- | --- |
| `sched_dump_threads` | §0 | Diagnostic dump of the thread table, carried over under §0. It reads scheduler state and guest memory for logging and changes no guest-visible state. Project decision: a carry-over diagnostic with no PSP semantics. |
| `s_cpu` | §1 | Shared `CpuState` pointer that the running thread's register file follows. Within the scheduler it is written only by `sched_init`, and it is NULL before that call [C1]. |
| `sr_thread_k0` | §3.1 | Returns the running thread's initial k0 value, or zero when no thread is current, the same rule as `sched_current_uid` [C6]. Generated code calls it only as the fallback for an out-of-window `$k0` (`tools/codegen.py`). |
| `sched_create_thread_ex` | §3.1 | Extended create that also takes an attribute word and a name. Failure follows `sched_create_thread`: a zero UID and no partial thread on exhaustion [C5]. The attribute word and name are PSP ABI inputs [C6]; this spec defines no further meaning for them. |
| `sched_guest_stack_floor` | §3.1 | Lowest guest address held by any live thread-stack reservation, or the stack-arena ceiling when none is live. A diagnostic and test view of `sched_guest_stack_floor_above(0)` [C5]. |
| `sched_set_stack_range_refusal` | §3.1 | Installs the HLE user partition's check that refuses a new thread stack over memory the partition has handed out; with none installed (standalone scheduler fixtures) every range is accepted [C5]. |
| `sched_guest_stack_floor_above` | §3.1 | Lowest live thread-stack reservation at or above an address, or the arena ceiling. The HLE user partition's contiguous free space above its bump pointer ends there, so memory reported free never contains a live stack, and a create whose stack would overlap the partition's handed-out bump region fails like stack exhaustion [C5]. Project decision: on a PSP, thread stacks come from the same user partition, so the arena is that partition, `[0x08800000, 0x0a000000)`, carved from its top; the runtime's VBLANK stack and nested host->guest frames lie below it in kernel RAM. |
| `sched_thread_info` | §3.1 | Fills `SrThreadInfo` for a UID and returns zero. Returns nonzero when the UID cannot be queried, which the thread handler reports as UNKNOWN_THID [C6]. |
| `sched_visit_threadman_ids` | §3.1 | `sceKernelGetThreadmanIdList` for the classes the scheduler owns: threads, the SleepThread, DelayThread, SuspendThread and DormantThread lists, and alarms, by their PSPSDK `SceKernelIdListType` values [C6]. Visits the UID of every live object of the class, read-only, and returns nonzero; returns zero and visits nothing for any other class. A deleted thread is in no list, and the state lists use the status and wait type `sched_thread_run_status` reports [C5]. Project decision: table order, because the PSP's list order is unmeasured. |
| `sched_suspend_thread` | §3.1 | `sceKernelSuspendThread`. Returns the PSP status: UNKNOWN_THID for an unknown UID, ILLEGAL_THID for UID zero or the calling thread, DORMANT for a dormant thread, and SUSPEND for a second suspend. A suspended thread is never selected until resumed (§3.2) [C5], [C6]. |
| `sched_resume_thread` | §3.1 | `sceKernelResumeThread`. Returns the PSP status: UNKNOWN_THID for an unknown UID, DORMANT for a dormant thread, and NOT_SUSPEND for a thread that is not suspended, including the calling thread [C5], [C6]. A resumed thread is selectable again under §3.2. |
| `sched_change_current_thread_attr` | §3.1 | `sceKernelChangeCurrentThreadAttr`. Applies `clear_mask` and `set_mask` to the running thread's attribute word and returns the PSP status code; the bit meanings are PSP ABI facts [C6]. |
| `sched_transfer_begin` | §3.2 | Opens the single device-transfer window for the calling thread on `obj`, the DMA HLE's active transfer. Returns 1, or 0 when a window is already open, `obj` is zero, or no thread is current. The window carries no guest time: it stays open through the next scheduling decision that selects another thread, closes at the decision after that, and closes at once when no other thread is runnable. Closing wakes every thread blocked on `obj`, as `sched_wake` does. Project decision: the PSP measured the ordering (a second thread's Try returns BUSY while the transfer is pending, and its blocking call waits), not the transfer's duration in guest time (public issue #303). |
| `sched_transfer_pending` | §3.2 | Read-only query: returns 1 while the window opened on `obj` is open and writes its owner's UID when `owner_uid` is not NULL; otherwise returns 0. Project decision, as for `sched_transfer_begin` (public issue #303). |
| `sched_rotate_thread_ready_queue` | §3.2 | `sceKernelRotateThreadReadyQueue`. Rotates the ready queue at the given priority, where zero selects the caller's priority. Rotation reorders only threads of equal priority; strict priority still decides between levels [C5]. |
| `sched_alarm_set` | §3.4 | `sceKernelSetAlarm`. The delay is relative to the guest timeline (§3.4). Returns an alarm UID, or an error at or above 0x80000000 when no alarm slot can be allocated [C5]. The handler runs at the deadline in interrupt context with the common word in `$a0`; a nonzero return re-arms the same alarm that many microseconds later, and zero ends it [C5], [C6]. |
| `sched_alarm_cancel` | §3.4 | `sceKernelCancelAlarm`. Returns zero when the alarm is cancelled. Returns SCHED_ERROR_UNKNOWN_ALMID for an unknown UID, for a one-shot alarm that has already run, and for an alarm that ended through its handler's return value [C5]. |
| `sched_count_waiters` | §3.5 | Returns the number of threads currently blocked on `obj`, or zero when there are none. It is read-only [C5]. |
| `sched_wake_with_result` | §3.5 | Readies every thread blocked on `obj`, as `sched_wake` does, and attaches `result` to each wake for the take operations `sched_take_wake_result` and `sched_wait_take_result` [C5]. |
| `sched_take_wake_result` | §3.5 | Consuming take of the result that a wake attached to the calling thread. Returns 1 and writes the code when one is pending, otherwise 0 [C5]. |
| `sched_wake_one_object_waiter_with_result` | §3.5 | Readies the named thread only when it is blocked on an object, attaching `result` as `sched_wake_with_result` does. Returns nonzero when it readied the thread and zero otherwise [C5]. |
| `sched_set_current_wait_kind` | §3.5 | Latches the PSP wait-type value reported for the calling thread before it blocks. It neither blocks nor wakes anything [C6]. |
| `sched_wait_begin` | §3.5 | Opens a wait invocation on `object`. A timed wait passes an absolute deadline built with `sched_vtime_deadline_after` (§3.4); an untimed wait passes zero and selects no timeout through the `timed` argument of `sched_wait_block`. The detach hook is the object's policy and must not preempt during owner teardown (`recomp.h`). Returns the handle, or zero when none can be allocated [C1], [C5]. |
| `sched_wait_block` | §3.5 | Parks the calling thread on the handle until it is notified or, when `timed` is set, until `remaining` guest microseconds have passed. `remaining` is the caller's difference between the deadline and the current guest time (§3.4). Returns nonzero when the wait ended by timeout [C5]. |
| `sched_wait_callback` | §3.8 | Sets the callback-parked state of a wait invocation when `active` is nonzero and clears it when zero. Only the innermost block attaches to the thread; callback-parked parents stay live [C5]. |
| `sched_wait_state` | §3.5 | Reports the handle's `SrWaitState` and owning UID. Returns nonzero for a live handle and zero for a stale or foreign one; a stale handle is refused and never applied to another waiter [C3]. |
| `sched_wait_notify` | §3.5 | Records a notification on a live handle. A notification does not grant the object. Returns nonzero when a notification was recorded [C5]. |
| `sched_wait_complete` | §3.5 | Completes exactly the given invocation, including callback parents, with `result`. A stale handle is refused and affects no other waiter [C3]. |
| `sched_wait_begin_join` | §3.5 | Opens a join wait on thread `target` with a guest-time deadline (zero when the guest set no timeout) and optional callback participation. Returns the handle, or zero when none can be allocated. The exit status is delivered through `sched_wait_take_result` [C5]. |
| `sched_wait_take_result` | §3.5 | Consuming take of a completed invocation's result. Returns nonzero and writes `result` when one is present [C5]. |
| `sched_wait_finish` | §3.5 | Closes the invocation and returns its notification count. A nonzero return means the caller should run `sched_preempt`; the caller owns that preemption point (`recomp.h`) [C1]. |
| `sched_wait_cancel_object` | §3.5 | Completes the waiters on `object` with `result`, as the object deletion and cancel paths require [C5]. |
| `sched_wait_vblank_cb` | §3.7 | Callback-aware display wait. A nonzero `wait_start` selects WaitVblankStart semantics, which always block to the next start edge. Zero selects WaitVblank semantics, which return at once inside the interval [C6]. |
| `sr_cpu_for_callbacks` | §3.8 | Returns the `CpuState` that callback dispatch runs on, the shared register file bound by `sched_init` (§1). Returns NULL before that binding [C1]. |
| `g_master_reent` | §3.9 | Exported global holding the master libc reent address. A launcher-role thread sets it at its libc registration, and the title binding sets it while it is still zero; later threads copy from it when they initialise their own reent. Project decision: the launcher-role seeding follows the role capture rule of §3.9, and the title binding is title configuration (§3.9). |
| `sr_cpustate_abi_version` | §3.10 | Exported constant equal to `SR_CPUSTATE_ABI_VERSION` in `recomp.h`. `tools/mem_debug.py` reads it by name to identify the `CpuState` layout of an attached build. The value is fixed by the header, not chosen by the scheduler [C12]. |
| `sched_vbl_paced` | §4 | Host pacing query: returns true when vblank pacing is active, in which case the presenter's frame-pacing step returns early (`src/rt/gui.c`). This is host performance behaviour outside the PSP contract (§4). The `SR_NOVBPACE` value rule is pinned by `NovbpaceSemanticsTests`. Project decision. |
| `sched_pause_request` | §3.4 | Latches a player pause for the next legal scheduler boundary; a request latched by a VBLANK the idle scheduler delivers is entered before the thread that edge woke resumes. The host stall freezes guest time and shifts the host-clock anchors on resume. Project decision, as summarized by public issue #1171. |
| `sched_pause_active` | §3.4 | Read-only query for the scheduler-context player pause state. It does not advance or otherwise change guest state. Project decision, as summarized by public issue #1171. |
| `sched_pause_pending` | §3.4 | Read-only query for a requested or active player pause. The controller sample at its scheduler rendezvous is withheld so the first post-resume VCOUNT owns that guest-time input sample. Project decision, as summarized by public issue #1171. |
| `sched_pause_total_ns` | §3.4 | Read-only total of completed host pause intervals in nanoseconds. It is diagnostic and has no guest-visible effect. Project decision, as summarized by public issue #1171. |
| `sr_sched_pause_host` | §3.4 | Host callback table for audio set-aside and the later presenter seam. NULL is valid in tests and headless runs; this table cannot execute guest code. Project decision, as summarized by public issue #1171. |
| `sched_thread_set_gp` | §3.1 | Sets the thread's initial `$gp`, which the scheduler installs before the guest entry runs and again when a dormant thread starts. Used for runtime-placed module entry points. Project decision; the register value comes from the loaded image. |
| `sched_module_range_busy` | §3.1 | Reports whether a READY or RUNNING thread (except `ignored_uid`) has a continuation PC or return address inside `[start, end)`. WAIT threads do not keep a module image alive: PSP-3000 issue #1106 measured that module-owned waiters may remain blocked after unload. Project decision: refusing an executing continuation is a named synthetic safety rule, not measured PSP error behavior. |
| `sched_set_thread_exit_hook` | §3.1 | Installs the scheduler-owned callback invoked after a thread object is removed. It is used to finish deferred self-stop/unload cleanup after the caller can no longer execute in its module image. Project decision; the callback does not run guest code. |
| `sched_set_ge_timing_hooks` | §3.6 | Installs the HLE GE completion source: a next-deadline query and a service step the scheduler calls from interrupt service once the deadline is reached. NULL hooks leave the scheduler unchanged; the hooks report nothing unless `SR_GE_TIMING=1`. Project decision (issue #1296); the deadline comes from the GE time model in `src/rt/ge.c`, measured on a PSP-3000 with `CASE=ge-timing`. |
| `sched_guest_thread_running` | §3.1 | Returns nonzero while a guest thread is current, and zero when scheduler control is outside a guest thread. Used to refuse LoadExec when no caller continuation can be retired. Project decision. |
| `sched_request_guest_reset` | §3.1 | Latches a request for the scheduler to stop the current guest dispatch at its next boundary after the requesting thread exits. The driver consumes it before replacing guest state. Project decision; LoadExec reset behavior is documented in `ARCHITECTURE.md`. |
| `sched_reset_for_loadexec` | §3.1 | Runs only after `sched_run` returns to the host scheduler coroutine. Releases each old TCB's resources, wait handles, AOT return state and host fiber before clearing scheduler-owned guest threads. It refuses calls made while a guest coroutine is current. Project decision; `sched_init` remains process-start-only. |
| `sched_set_guest_process_gp` | §3.1 | Seeds the replacement program's initial `$gp` in both the scheduler default and bound `CpuState` before its root thread is created. Project decision; the value is supplied by the loaded project module. |
| `sched_watchdog_summary` | §4 | Diagnostic snapshot for the runtime's no-new-frame `BOOT_EVENT phase=stalled` line, which the native player reads (public issue #1289). Writes the counts of ready, delay-waiting and object-waiting threads, and the number of scheduler spin-watchdog observations since the previous call, then clears that observation count; a NULL output is skipped. It changes no thread state and nothing guest-visible, and the counts and event format are diagnostics outside the PSP contract (§4). Project decision, as summarized by public issue #1289. |
