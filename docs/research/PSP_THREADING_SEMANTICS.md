# PSP threading semantics: CreateThread and StartThread

This is the canonical public research note for PSP thread creation and start
semantics used by Nakagawa and the broader PSP recompilation documentation.
It consolidates the current source boundary, public declarations, emulator
comparators, measured observations, disagreements, and the next hardware
oracle work. It is intentionally title-neutral.

**Status:** `PSP_THREADING_ORACLE_DESIGN = FROZEN / RESEARCH COMPLETE`.
`HARDWARE_EXECUTION = NOT RUN` (`HARDWARE_NOT_RUN`). The CreateThread and
StartThread hardware-oracle specifications below are frozen design material,
not execution results. No emulator agreement is firmware fact.

The next phase is outside this documentation task: harness implementation,
explicit hardware authorization, global hardware-lock acquisition, and
controlled campaign execution. None of those actions is performed here.

**Review base:** Nakagawa commit
[`d8b0d4f0581ad4a3a09c973b1ecb393846bc0420`](https://github.com/Jstar269/nakagawa-recomp/tree/d8b0d4f0581ad4a3a09c973b1ecb393846bc0420/).
Source behavior must be rechecked against a newer source head before this note
is used as an implementation review.

## How to read this note

The evidence class on every substantive observation is part of the claim. A
public declaration is not a measurement, an emulator implementation is not
silicon truth, and a Nakagawa source observation is not automatically PSP
firmware behavior.

| Class | Meaning in this note |
| --- | --- |
| `HARDWARE_MEASURED` | Observed on a real PSP in the stated, bounded test scope. |
| `PUBLIC_HEADER_FACT` | Declared by a public SDK/header or generated public reference. |
| `OPEN_FIRMWARE_IMPLEMENTATION` | Behavior visible in an open emulator or firmware-like implementation. |
| `EMULATOR_CONSENSUS` | PPSSPP and JPCSP model the same behavior; this remains comparator evidence. |
| `IMPLEMENTATION_DISAGREEMENT` | Public implementations or models differ. |
| `SOURCE_VERIFIED_NAKAGAWA` | Confirmed in the reviewed Nakagawa source tree. |
| `INFERENCE` | A reasoned interpretation that is not directly measured or declared. |
| `HARDWARE_UNKNOWN` | The available evidence does not establish the real-PSP behavior. |
| `HARDWARE_NOT_RUN` | The planned hardware observation has not been executed. |

When a row has both an observation and a boundary, the boundary controls the
claim. In particular, a result from one return seam must not be generalized to
all thread exits, and a comparator consensus must not be promoted to a
hardware fact.

## Established evidence

These are the bounded facts that this consolidation may reuse.

| Observation | Class | Boundary |
| --- | --- | --- |
| `USER_CALL_ARG5_8_T0_T3`: measured PSP user-call arguments 5 through 8 arrive in `$t0` through `$t3`. | `HARDWARE_MEASURED` | This is the measured user-call argument-placement scope; it is not a claim about every kernel entry path. |
| `FIRMWARE_CREATE_THREAD_ARG5_T0`: a real-firmware `sceKernelCreateThread` measurement established argument 5 in `$t0`. | `HARDWARE_MEASURED` | `FIRMWARE_GENERIC_ARG6_8 = NOT_MEASURED`; generic firmware placement for arguments 6 through 8 was not separately measured in that run. |
| Nakagawa `stack_arg()` reads arguments 5 through 8 from `$t0` through `$t3` and reads argument 9 onward from the caller stack. | `SOURCE_VERIFIED_NAKAGAWA` | This is a source fact, not a hardware remeasurement. |
| In the measured ordinary `sceKernelWaitEventFlag` sequence, the wait does NOT dispatch the pending callback; `sceKernelCheckCallback()` is the observed execution boundary. | `HARDWARE_MEASURED` | This exact observation is for ordinary `sceKernelWaitEventFlag`; it does not define every callback-capable wait API or callback entry path. |
| At the measured non-delete thread-exit seam, positive `0x77` exits as `0x77`, while error-shaped `0x800201ac` exits as `0x800200d2`. | `HARDWARE_MEASURED` | Exact seam and statuses only. This does not define `sceKernelExitDeleteThread`, every signed-negative thread return, or every status-normalization path. |

The named ABI measurements are:

```text
USER_CALL_ARG5_8_T0_T3 = HARDWARE_MEASURED
FIRMWARE_CREATE_THREAD_ARG5_T0 = HARDWARE_MEASURED
FIRMWARE_GENERIC_ARG6_8 = NOT_MEASURED
```

The callback and exit rows are useful lifecycle context but are not evidence for
CreateThread allocation, StartThread register initialization, or scheduler
ordering.

## Public sources and comparator rule

The following sources are deliberately kept separate by evidence class.

| Source | Use | Evidence boundary |
| --- | --- | --- |
| [PSPSDK `pspthreadman.h` at a pinned commit](https://github.com/pspdev/pspsdk/blob/314b2083f2e1eaf145fc5de342736336fe1f0148/src/user/pspthreadman.h) | Public names, declarations, attributes, and option structure. | `PUBLIC_HEADER_FACT`; declarations do not settle undocumented firmware behavior. |
| [PSPSDK ThreadMan reference](https://pspdev.github.io/pspsdk/pspthreadman_8h.html) | Generated public API reference and cross-check. | `PUBLIC_HEADER_FACT`; generated documentation may omit implementation semantics. |
| [PPSSPP ThreadMan implementation](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceKernelThread.cpp) | Open implementation comparator for thread state, stack setup, arguments, and register modeling. | `OPEN_FIRMWARE_IMPLEMENTATION`; the moving branch is not a PSP measurement. |
| [JPCSP `ThreadManForUser`](https://github.com/jpcsp/jpcsp/blob/master/src/jpcsp/HLE/modules/ThreadManForUser.java) | Independent open implementation comparator. | `OPEN_FIRMWARE_IMPLEMENTATION`; source behavior is not a hardware oracle. |
| [Nakagawa `src/rt/hle.c` at the review base](https://github.com/Jstar269/nakagawa-recomp/blob/d8b0d4f0581ad4a3a09c973b1ecb393846bc0420/src/rt/hle.c) | Guest ABI extraction and HLE route ownership. | `SOURCE_VERIFIED_NAKAGAWA`. |
| [Nakagawa `src/rt/sched.c` at the review base](https://github.com/Jstar269/nakagawa-recomp/blob/d8b0d4f0581ad4a3a09c973b1ecb393846bc0420/src/rt/sched.c) | Current scheduler model and lifecycle implementation. | `SOURCE_VERIFIED_NAKAGAWA`; not a claim of firmware equivalence. |
| [Nakagawa PSP oracle fixture boundary](../../fixtures/psp_oracle/README.md) | Public-safe fixture protocol, scalar evidence rules, and existing bounded status notes. | Source-owned method and recorded evidence remain distinct from planned work. |

## CreateThread

### Call boundary

The public API supplies a name, entry point, initial priority, stack size,
attributes, and an optional `SceKernelThreadOptParam`. The measured user-call
ABI places the fifth through eighth user arguments in `$t0` through `$t3`.
Nakagawa’s HLE extraction implements that placement in `stack_arg()` and routes
the CreateThread call into its scheduler. Those are separate claims:

- ABI placement is `HARDWARE_MEASURED` only within the established measurement
  scope.
- `stack_arg()` and the HLE route are `SOURCE_VERIFIED_NAKAGAWA`.
- The current scheduler’s allocation and initial-state choices are
  implementation behavior until a corresponding hardware observation exists.

### Attributes

The public header declares the following thread attributes. The KERNEL and
LOW_STACK values are additionally modeled by PPSSPP/JPCSP; they should not be
mistaken for public-header declarations in this table.

| Attribute | Value | Evidence class | Current interpretation |
| --- | ---: | --- | --- |
| `PSP_THREAD_ATTR_VFPU` | `0x00004000` | `PUBLIC_HEADER_FACT` | Public attribute declaration; actual hardware register/context consequences are outside this note. |
| `PSP_THREAD_ATTR_USER` | `0x80000000` | `PUBLIC_HEADER_FACT` | Public attribute declaration; legality and normalization remain an oracle question. |
| `PSP_THREAD_ATTR_USBWLAN` | `0xa0000000` | `PUBLIC_HEADER_FACT` | Public attribute declaration; exact acceptance and normalization remain an oracle question. |
| `PSP_THREAD_ATTR_VSH` | `0xc0000000` | `PUBLIC_HEADER_FACT` | Public attribute declaration; exact acceptance and normalization remain an oracle question. |
| `PSP_THREAD_ATTR_SCRATCH_SRAM` | `0x00008000` | `PUBLIC_HEADER_FACT` | Public attribute declaration; legality for the tested caller remains unknown. |
| `PSP_THREAD_ATTR_NO_FILLSTACK` | `0x00100000` | `PUBLIC_HEADER_FACT` | Public attribute declaration; fill behavior requires an in-allocation observation. |
| `PSP_THREAD_ATTR_CLEAR_STACK` | `0x00200000` | `PUBLIC_HEADER_FACT` | Public attribute declaration; exact initialization pattern and precedence require measurement. |
| `PSP_THREAD_ATTR_KERNEL` | `0x00001000` | `EMULATOR_CONSENSUS` | Modeled by PPSSPP/JPCSP; do not present it as a public-header fact. |
| `PSP_THREAD_ATTR_LOW_STACK` | `0x00400000` | `EMULATOR_CONSENSUS` | Modeled by PPSSPP/JPCSP; allocation behavior requires hardware evidence. |

The public declarations and emulator models disagree on more than naming. The
following remain open compatibility questions: the legal user attribute mask,
whether a user caller may request KERNEL, SCRATCH_SRAM legality,
USBWLAN/VSH normalization, treatment of unknown bits, and priority bounds.
They are not resolved by a successful Nakagawa launch or by emulator parity.

### Thread option and stack identity

The public option type is:

```c
typedef struct {
    SceSize size;
    SceUID stackMpid;
} SceKernelThreadOptParam;
```

The relevant public partition constants are:

```text
KERNEL = 1
USER = 2
JPCSP_VSHELL = 5
```

The JPCSP VSHELL partition constant is 5. JPCSP interprets `stackMpid` as a
MEMORY PARTITION ID passed to its allocator. PPSSPP observes the option but does
not model a `stackMpid` allocation effect. PSPSDK wording is uncertain
(`"UID of memory block (?)"`), so a genuine block UID is retained only as a
competing hardware hypothesis. The hardware meaning of `stackMpid` remains
`HARDWARE_UNKNOWN`.

`SceKernelThreadInfo.stack` is exposed as a low allocation/base address by
PPSSPP and JPCSP. That is `EMULATOR_CONSENSUS`, not confirmed PSP semantics;
thread-info stack orientation remains `HARDWARE_UNKNOWN`. Before any bounded
in-range stack-content read, the oracle must establish whether the reported
value is the low allocation address, another base, or an orientation marker. No
`stack - 1` probing or other adjacent/outside-allocation read is permitted.

### Current disagreements

| Question | Current evidence | Required disposition |
| --- | --- | --- |
| Is a requested attribute mask legal for a user caller? | `IMPLEMENTATION_DISAGREEMENT` / `HARDWARE_UNKNOWN` | Record raw return code and resulting thread state for each mask. |
| Is `KERNEL` accepted, rejected, or normalized for a user caller? | `IMPLEMENTATION_DISAGREEMENT` / `HARDWARE_UNKNOWN` | Measure explicit positive and negative cases; do not infer from emulator permission checks. |
| Is `SCRATCH_SRAM` legal in the tested context? | `HARDWARE_UNKNOWN` | Use an isolated, explicitly labeled cell. |
| Are USBWLAN and VSH normalized? | `IMPLEMENTATION_DISAGREEMENT` / `HARDWARE_UNKNOWN` | Compare raw accepted attributes if the API exposes them; otherwise retain only return/state evidence. |
| What happens to unknown attribute bits? | `IMPLEMENTATION_DISAGREEMENT` / `HARDWARE_UNKNOWN` | Include a single-bit mutation with a control mask. |
| What are the priority bounds and failure codes? | `IMPLEMENTATION_DISAGREEMENT` / `HARDWARE_UNKNOWN` | Test bounded values around a known control, preserving raw codes. |
| Does firmware honor `stackMpid`, and does it mean a partition ID or block UID? | `IMPLEMENTATION_DISAGREEMENT` / `HARDWARE_UNKNOWN` | Keep partition IDs primary and block UID as the competing PSPSDK hypothesis. |
| What does a malformed option size do? | `HARDWARE_UNKNOWN` | Test only in a dedicated malformed-input phase; do not call it lower-risk. |

### Planned CreateThread hardware oracle

The oracle is a plan, not a result. Its required status marker is
`CREATE_THREAD_HARDWARE_ORACLE_SPEC_READY` followed by `HARDWARE_NOT_RUN` until
an authorized real-device run produces records.

The obsolete minimum arithmetic is retired. The corrected first-principles
minimum is:

```text
MINIMUM_GATING_UNIQUE_CASES = 28
MINIMUM_GATING_CONTROLS = 6
MINIMUM_GATING_DISCRIMINATORS = 22
MINIMUM_GATING_RECORDS = 28
MINIMUM_GATING_LAUNCHES = 5
MINIMUM_WITH_ONE_REPEAT_RECORDS = 56
MINIMUM_WITH_ONE_REPEAT_LAUNCHES = 10
```

Each minimum case emits one record. A lower-risk label describes sequencing and
scope only; it is not a guarantee that a probe cannot destabilize a device or
session. The single boot/build proof is `CT-A01` in L1. No boot duplicates are
added to the minimum.

The first cells are:

| Cell | Option input | Purpose |
| --- | --- | --- |
| CT-C01 | `NULL` | Baseline control with no option structure. |
| CT-C02 | Non-NULL, `size=4` | Prefix-sized option, isolating size handling. |
| CT-C03 | Non-NULL, `size=8`, `stackMpid=USER (2)` | Primary partition-ID hypothesis. |
| CT-C04 | Non-NULL, `size=8`, invalid partition `7` | Invalid partition discriminator. |
| CT-C05 | Non-NULL, `size=8`, `stackMpid=KERNEL (1)` | Kernel partition discriminator. |
| CT-C06 | Non-NULL, `size=8`, genuine block UID | Competing PSPSDK block-UID hypothesis; not the primary interpretation. |

The remaining records should cover the declared attributes, one unknown-bit
mutation, bounded priority edges, stack-size controls, and malformed option
inputs. Every record should preserve the raw return value, whether a thread UID
was produced, and any observable thread-info state. A failure must not be
silently converted into a generic “unsupported” label.

The allocation and fill phases have specific constraints:

- Use partition IDs as the primary `stackMpid` input. Do not collapse the
  partition-ID and block-UID hypotheses into one cell.
- For `SceKernelThreadInfo.stack`, establish address orientation before taking
  any bounded in-stack sample. Do not use the old adjacent/outside-allocation
  probe.
- For `PSP_THREAD_ATTR_LOW_STACK`, use the paired simultaneous-allocation
  design; do not create and delete sequentially and then treat address reuse as
  an allocation result.
- For `PSP_THREAD_ATTR_NO_FILLSTACK`, collect a bounded in-range
  statistical/checksum observation over an explicitly defined region. A
  one-byte probe is not sufficient evidence.
- Separate malformed pointers and malformed sizes from the lower-risk launch
  phases. Do not describe any phase as harmless or guaranteed safe.

### Canonical five-launch minimum and full-campaign accounting

The canonical minimum is exactly five physical launches:

| Launch | Phase | Cases | Unique |
| --- | --- | --- | ---: |
| L1 | `CT-ATTR` | `CT-A01`, `CT-A03`, `CT-A04`, `CT-A05`, `CT-A07`, `CT-A11`, `CT-A12`, `CT-D00` | 8 |
| L2 | `CT-PRIO+OPT` | `CT-B01`, `CT-B02`, `CT-B05`, `CT-B06`, `CT-C01`, `CT-C02`, `CT-C03`, `CT-C04` | 8 |
| L3 | `ST-ARGS` | `ST-SA01`, `ST-SA02`, `ST-SA03` | 3 |
| L4 | `ST-LIFE+RET` | `ST-SL01`, `ST-SL02`, `ST-SL04`, `ST-SR01`, `ST-SR02`, `ST-SR03` | 6 |
| L5 | `ST-SCHED` | `ST-SP01`, `ST-SP02`, `ST-SP03` | 3 |

This is 28 unique cases and 28 emitted records across five launches. The
exact six controls are `CT-A01`, `CT-D00`, `CT-C01`, `ST-SA01`, `ST-SR01`, and
`ST-SR02`; the remaining 22 cases are discriminators. One complete repeat
would produce 56 records across 10 launches.

`CT-C01..C04` share L2 with the priority cells; no dedicated option launch is
required. They are the same lower-risk creation-only class, use sane/aligned
pointers for the minimum cells, create independent thread objects, have no
scheduling-state dependency, and do not conflict with `CT-D00`.

`CT-D00` currently assumes or proposes `sceKernelReferThreadStatus(0, ...)`
for a current-thread stack-orientation observation. The thid-0 selector is a
preliminary control assumption, not a measured PSP fact. Harness work must first
verify thid-0 behavior or use an explicitly obtained current-thread UID. This
caveat does not change the 28/5 accounting.

The previously consolidated full-campaign accounting is unchanged by this
minimum correction:

```text
FULL LOWER-RISK = 49 unique cases
DEFERRED = 3
HIGHER-RISK = 7
TOTAL UNIQUE = 59
TOTAL EMITTED RECORDS = 71
TOTAL PHYSICAL LAUNCHES = 13
```

This note does not claim a fresh independent recount after the minimum
correction; it records that the corrected minimum was reconciled while the
previously consolidated full/deferred/higher-risk totals remained unchanged.

## StartThread

### Argument block

PPSSPP and JPCSP agree that a nonzero argument block is copied to the child
stack and that the child receives the argument byte count in `$a0` and the
child-stack copy address in `$a1`. This is `EMULATOR_CONSENSUS`, not a hardware
fact.

The important disagreement is a non-NULL pointer with `argSize=0`:

| Input | PPSSPP model | JPCSP model | Evidence class |
| --- | --- | --- | --- |
| `argSize > 0`, valid pointer | `$a0` is the requested byte count; `$a1` points at the child-stack copy. | Same broad model. | `EMULATOR_CONSENSUS` |
| `argSize=0`, `argp != NULL` | `$a0=0`, `$a1=0`. | `$a0=0`, `$a1` is a child-stack address. | `IMPLEMENTATION_DISAGREEMENT` |
| `argSize=0`, `argp=NULL` | Model-specific zero-argument path. | Model-specific zero-argument path. | `HARDWARE_UNKNOWN` |

The hardware oracle must distinguish NULL, non-NULL, and misaligned pointers,
and must record raw failure codes. Public implementations also disagree on
negative `argSize` handling and pointer validation; retain that as
`IMPLEMENTATION_DISAGREEMENT` until a bounded hardware cell measures it. Huge
sizes and malformed pointers are high-risk cases and are deferred until the
bounded normal cases establish the observation path. Zero-sized arguments must
never dereference `$a1`.

For the positive-size copy proof, preserve all four observations:

1. source address is not equal to child `$a1`;
2. source checksum equals child-copy checksum;
3. `$a1` lies inside the child stack allocation;
4. `$a1 - $sp == 0x40`.

A checksum or memory read for a zero-sized argument is not permitted.

### Initial machine state

The following values are comparator models, not PSP measurements:

| State | Current comparator model | Evidence class / boundary |
| --- | --- | --- |
| Stack placement | Both models reserve a modeled kernel area below the stack base, align the argument area, and use a `0x40`-byte frame. | `EMULATOR_CONSENSUS` for the broad model; exact PSP spacing is `HARDWARE_UNKNOWN`. |
| Initial SP | Both model the initial SP from stack base and size, with modeled reservation and alignment. | `EMULATOR_CONSENSUS`; do not use the current formula as firmware truth. |
| Initial GP | PPSSPP derives module metadata; JPCSP uses the creator GP. | `IMPLEMENTATION_DISAGREEMENT`; a cross-module launch is a high-value discriminator. |
| Initial RA | PPSSPP uses a stack return stub; JPCSP uses an internal HLE exit handler. | `IMPLEMENTATION_DISAGREEMENT`; real-PSP initial RA is `HARDWARE_UNKNOWN`. |
| Initial `k0` / `k1` | The exact values and whether the entry path preserves or sanitizes them are not settled by the comparators. | `HARDWARE_UNKNOWN`. |
| Initial GPR/FPU/VFPU sentinel values | `0xDEADBEEF`, `0x7F800001`, and analogous debug/initialization sentinels occur in emulator models. | `OPEN_FIRMWARE_IMPLEMENTATION`; these are emulator initialization models, not hardware facts. |
| Return-value sanitization | The exact return-value sanitization rule, including behavior for signed-negative values, is not established. | `HARDWARE_UNKNOWN`; the measured non-delete exit seam is not universal. |

The planned register capture must state which registers are directly observed,
which are inferred from a wrapper, and which are unavailable. It must not turn
an emulator sentinel into a PSP ABI requirement.

### Lifecycle and scheduling

The current emulator and public implementation models provide the following
lifecycle cases. Where both emulators agree, the evidence is
`EMULATOR_CONSENSUS`; individual model details are `OPEN_FIRMWARE_IMPLEMENTATION`.
This is emulator/public-implementation evidence, not hardware fact, and exact
PSP status codes and ordering remain bounded oracle questions.

| Case | Current model summary | Evidence boundary |
| --- | --- | --- |
| Start a dormant thread | Child can start. | `EMULATOR_CONSENSUS` where both models agree; exact firmware result is `HARDWARE_UNKNOWN`. |
| Start an active thread | Returns an error. | `OPEN_FIRMWARE_IMPLEMENTATION` / `EMULATOR_CONSENSUS` for model behavior; preserve raw model/device code and keep the firmware code `HARDWARE_UNKNOWN`. |
| Start an exited thread | Restarts from its entry point. | `OPEN_FIRMWARE_IMPLEMENTATION` / `EMULATOR_CONSENSUS` for model behavior; hardware lifecycle semantics require measurement. |
| Delete a never-started dormant thread | Delete is modeled as valid. | `OPEN_FIRMWARE_IMPLEMENTATION`; do not generalize from the non-delete exit seam. |
| Wait on a never-started dormant thread | A dormant wait path is modeled. | `OPEN_FIRMWARE_IMPLEMENTATION`; exact wait result and timeout behavior are `HARDWARE_UNKNOWN`. |
| Invalid thread ID | Rejected. | `OPEN_FIRMWARE_IMPLEMENTATION` / `EMULATOR_CONSENSUS` for model behavior; use an explicit invalid-ID control and retain the raw result. |

For scheduling, both models reschedule immediately when the started child has a
strictly better priority (`EMULATOR_CONSENSUS` where they agree).
Equal-priority and worse-priority timing/order details differ, and physical
ordering is `HARDWARE_UNKNOWN`. The source-owned preemption oracle uses phase
state:

```text
controller: PHASE = BEFORE immediately before sceKernelStartThread(...)
child:      phase_snapshot = PHASE as its first observation
controller: sceKernelStartThread(...) returns
controller: PHASE = AFTER immediately after the return
```

A child observing `BEFORE` proves that it ran before the syscall returned. In
this design, `PHASE` is the scheduling authority and `ENTRY_COUNT` is lifecycle
evidence only; the entry count must not be used to establish the ordering.

Capture the initial machine state before the child entry wrapper performs its
prologue, GP setup, stack usage, or `jal`. The final conceptual ordering is:

```text
PHASE
ENTRY_COUNT
mfhi / mflo
captured GPR stores
jr handler
```

Do not add sleeps, arbitrary delays, priority changes, or hardcoded timing
constants to make the ordering appear deterministic.

### VFPU boundary

VFPU-related thread attributes and register/context behavior have
`OPEN_FIRMWARE_IMPLEMENTATION` / `EMULATOR_CONSENSUS` models, but real hardware
consequences are `HARDWARE_UNKNOWN` and unmeasured here. This document records
that boundary only. It does not modify or expand the separate VFPU work.

### Planned StartThread hardware oracle

The specification status is `START_THREAD_HARDWARE_ORACLE_SPEC_READY` and
`HARDWARE_NOT_RUN`. The canonical minimum uses the StartThread cases in L3,
L4, and L5 above. The planned signed-negative return threshold is
`ST-SR03 = 0x80000000`; it is planned hardware work, not a measured result.
The normal and bounded controls must run before malformed-pointer and huge-size
cases. A planned record is not evidence merely because it is listed, and a
launch labeled lower-risk is not guaranteed harmless.

## Nakagawa source boundary

At the review base, the relevant source ownership is:

| Path / route | Current source fact | Class |
| --- | --- | --- |
| `src/rt/hle.c` ABI helpers | `$a0` through `$a3` are register arguments and `stack_arg()` supplies later user-call arguments from `$t0` through `$t3`, then the stack. | `SOURCE_VERIFIED_NAKAGAWA` |
| `h_CreateThread` | Extracts the HLE call arguments and routes creation to the scheduler. | `SOURCE_VERIFIED_NAKAGAWA` |
| `h_StartThread` | Extracts the call arguments, routes start to the scheduler, and requests preemption after a successful start. | `SOURCE_VERIFIED_NAKAGAWA` |
| `sched_create_thread` | Current model creates a dormant thread, allocates a runtime stack, and seeds runtime state. | `SOURCE_VERIFIED_NAKAGAWA`; not firmware equivalence. |
| `sched_start_thread` | Current model validates the UID/state, copies nonzero arguments to the child stack, re-seeds runtime state, and makes the thread ready. | `SOURCE_VERIFIED_NAKAGAWA`; not firmware equivalence. |

This source table identifies where an implementation change would belong. It
does not authorize a runtime change as part of this documentation mission.
The compatibility debt that remains after this source review is:

- legal attribute masks and normalization;
- option allocation semantics and malformed option handling;
- `SceKernelThreadInfo.stack` orientation and allocation meaning;
- initial SP, GP, RA, and raw-register values;
- zero-size/non-NULL argument handling;
- exact lifecycle return codes and restart semantics;
- marker ordering for equal and worse priorities;
- hardware consequences of thread attributes, including VFPU context.

## Oracle method and publication boundary

The planned probes must remain source-owned, title-neutral, scalar-first, and
independent of private retail inputs. Each result should identify the exact
fixture, firmware/device scope, call inputs, raw return code, resulting state,
and whether the record is a control or discriminator. Pointer values and raw
memory are not stable public evidence without an explicitly documented capture
and redaction boundary.

The repository’s [hardware-oracle protocol](../HARDWARE_ORACLE.md) and
[interrupt/wait evidence matrix](../PSP_INTR_WAITS_MATRIX.md) define the
broader evidence and publication limits. A passing source fixture, emulator
comparison, or local build cannot be reported as hardware validation. Physical
device execution, private trace handling, provenance authority, and release or
merge decisions remain outside this documentation change.

## Local Wiki synchronization drafts

This section is local source/draft material only. It records the text that can
later be synchronized to the two public knowledge surfaces; no remote Wiki was
edited by this documentation change.

### Nakagawa project Wiki draft

#### PSP threading semantics in Nakagawa

- Runtime mappings: `src/rt/hle.c` extracts `$a0` through `$a3` directly and
  `stack_arg()` supplies user-call arguments 5 through 8 from `$t0` through
  `$t3`; `h_CreateThread` and `h_StartThread` own the HLE routes into the
  scheduler. `src/rt/sched.c` owns the current dormant-thread creation,
  argument-copy, ready-state, and preemption model. These are
  `SOURCE_VERIFIED_NAKAGAWA`, not firmware claims.
- Existing bounded hardware facts: user-call arguments 5 through 8 in `$t0`
  through `$t3`, CreateThread argument 5 in `$t0`, the ordinary
  `sceKernelWaitEventFlag` callback boundary at `sceKernelCheckCallback()`,
  and the exact measured exit pair `0x77 -> 0x77` and
  `0x800201ac -> 0x800200d2`. These claims retain their bounded
  `HARDWARE_MEASURED` scope.
- Harness design: use the `PHASE`/`ENTRY_COUNT` preemption oracle, capture
  entry state before prologue/GP/stack/`jal` work in the order
  `PHASE`, `ENTRY_COUNT`, `mfhi / mflo`, GPR stores, and `jr handler`, and prove
  positive argument copying with the four-way address/checksum/stack-offset
  observations. Keep `CT-D00` preliminary until thid-0 selection is verified
  or an explicit current-thread UID is obtained.
- Remaining implementation debt: attribute legality and normalization,
  `stackMpid` allocation meaning, stack orientation, initial SP/GP/RA/
  `k0`/`k1` and sentinel state, zero-size and negative-size argument handling,
  lifecycle return codes and restart behavior, priority ordering, and hardware
  attribute/context effects. The exact return sanitization rule remains
  `HARDWARE_UNKNOWN`.
- Campaign design: the corrected minimum is 28 unique cases, 6 controls, 22
  discriminators, 28 records, and 5 launches, with one-repeat totals of 56
  records and 10 launches. The full consolidated totals remain 59 unique
  cases, 71 records, and 13 launches; those totals were not freshly recounted
  for this correction. `HARDWARE_EXECUTION = NOT RUN`.

### General PSP recompilation Wiki draft

#### PSP ThreadMan and scheduler model

- Treat public headers as `PUBLIC_HEADER_FACT`, open implementations as
  `OPEN_FIRMWARE_IMPLEMENTATION`, agreement between emulators as
  `EMULATOR_CONSENSUS`, disagreement as `IMPLEMENTATION_DISAGREEMENT`, and
  real-device observations as bounded `HARDWARE_MEASURED` records. Use
  `HARDWARE_UNKNOWN` for unresolved meaning and `HARDWARE_NOT_RUN` for planned
  probes that have not executed.
- Thread creation semantics include public attributes, partition-option
  uncertainty, stack-orientation uncertainty, and explicit disagreement over
  whether `stackMpid` is a MEMORY PARTITION ID or the competing block-UID
  hypothesis. Do not turn either emulator's interpretation into firmware fact.
- Thread start semantics include the positive-size child `$a0`/`$a1` copy model,
  the `argSize=0` non-NULL disagreement, negative-size/pointer-validation
  disagreement, emulator-only GPR/FPU/VFPU initialization sentinels, and
  unresolved SP/GP/RA/`k0`/`k1`/return-sanitization details.
- A hardware oracle should be scalar-first, control/discriminator based,
  bounded in memory, explicit about raw return codes, and honest about what was
  not run. Use phase state as scheduling authority and a separate entry count
  as lifecycle evidence. Never use sleeps or guessed timing to manufacture
  ordering, and never read outside a verified allocation.
- The corrected minimum campaign structure is five launches: CT attributes,
  priority plus option cells, StartThread arguments, lifecycle plus returns,
  and scheduling. It is 28 unique cases with 6 controls and 22
  discriminators; one complete repeat is 56 records across 10 launches. The
  unchanged broader accounting is 59 unique cases, 71 records, and 13 launches.

The following statements must not appear in either surface:

- that PPSSPP/JPCSP consensus proves PSP hardware behavior;
- that the planned CreateThread or StartThread oracle has already run;
- that a probe phase is guaranteed safe or cannot destabilize a device;
- that a block UID is the established meaning of `stackMpid`;
- that an adjacent/outside-allocation probe establishes stack orientation;
- that a current Nakagawa implementation detail is universal PSP semantics;
- private provenance paths, ledgers, retail inputs, traces, or run metadata.
