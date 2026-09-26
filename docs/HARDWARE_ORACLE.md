# Hardware oracle plan — a real PSP as an external verification source

**Status: proposal. The trace-oracle design below has not been built or tested.** Throughput figures
are estimates and labelled as such. This document is a plan in the same sense as
[DECOMPME_INTEGRATION.md](DECOMPME_INTEGRATION.md), not a record of completed work.

> **A narrower subset of this plan is now implemented.**
> The [source-owned scalar-probe runbook](../fixtures/psp_oracle/README.md), its
> [strict result protocol](../tools/psp_oracle/protocol.py), and the shipped
> [`tools/psp_readiness.py`](../tools/psp_readiness.py) cover the implemented subset. Read those
> first. In particular, the `tools/hw_doctor.py` proposed in §7 is superseded by the readiness tool
> — extend that tool rather than adding a second precondition checker.
> What remains genuinely unbuilt here is the *instruction-trace* oracle
> (`CODEGEN_ORACLE`/`MICROTEST_ORACLE` capture on real silicon), which the scalar probe does not
> provide.

## Measured to date (index — exact cells only, do not generalize)

The loops below are proposals. These cells are already measured; they are not
proposals. Each claim covers only the exact fixture named:

- **DMA concurrency** (live record: issue #23): on the qualified
  PSP-3000-series / 6.61 / ARK-5.1.0 route (2026-08-27), a second-context
  `sceDmacTryMemcpy` returned BUSY (`0x80000021`) in 64/64 trials while the
  first transfer was pending, and a concurrent blocking `sceDmacMemcpy`
  waited and returned 0 in 64/64 trials. Invalid-tail
  full-span-validation-vs-truncation precedence remains NOT_MEASURED (the
  probe's setup gate SKIPped; the assumed boundary was invalid for the
  64 MiB route), and main keeps its conservative behavior there.
- **VFPU fixtures** (live record: issue #40): Group A vhdp/vdot NaN/Inf
  matrix, 13 records PASS across 4 launches in two sessions; Group B 91-cell
  overlap matrix, all PASS across 3 bitwise-identical launches; same qualified
  route and date. Bulk random differential fuzz (Loop A) remains unbuilt, and
  the PPSSPP-derived-table warning stands for every unmeasured encoding.
- **Display/vblank masking** (detail: `ARCHITECTURE.md` display-mask section
  and the shipped `PSP-DISPLAY-001` oracle results): masked-window behavior is
  HARDWARE_MEASURED (+0 when no period crossed, +1 when one or two crossed,
  12/12 per delay); enabled-starved behavior stays CORROBORATIVE_ONLY and the
  software/hardware renderers stay non-oracles.
- **CPU exception entry** (runs PSP-A1-01, PSP-A2-01, PSP-A3-01; campaign
  `psp-hw-20260917`, 2026-09-17, PSP-3000 / 6.61 / ARK-5.1.0, user-mode PRX
  probes loaded through PSPLink; fixtures `exception-a1`..`exception-a3` in
  [`fixtures/psp_oracle/`](../fixtures/psp_oracle/README.md)):
  - A user-mode `break` reports EPC = the `break` address (not +4) and Cause
    `0x10000024` (ExcCode 9, BD 0). The frame's Status is `0x00088613` (EXL 1,
    user mode, IE 1, IM `0x86`), and the loaded argument and temporary
    registers are preserved.
  - The same `break` in the delay slot of an always-taken branch reports
    EPC = the branch address and Cause `0x90000024` (BD 1). Neither successor
    path executed.
  - A user-mode `lw` from a kernel-segment address raises AdEL (ExcCode 4):
    EPC = the load instruction, BadVAddr = the effective address, and the
    destination register is unchanged.
  - Cause bit 28 read as 1 in all three runs, so treat the CE field as
    undefined for non-coprocessor-unusable exceptions.
- **Non-finite vertex position and lit colour in the GE** (issue #69; no run
  yet): what the PSP GE rasterizes for a vertex whose clip position, projected
  screen position or lit colour channel is NaN or infinite is **NOT_MEASURED**.
  No public PSP documentation states a result, and PPSSPP's software GE was not
  usable as an oracle for the cell either (its screen acceptance and its
  bounding-box minimum/maximum both compare false against NaN, so no culling
  verdict is implied by its source either). `src/rt/ge.c` therefore fails
  closed: the primitive is dropped and counted (`GESTAT+ ... nonfinite=`, and
  `drop-nonfinite` in the per-draw `TRIDRW+` line) instead of being rasterized
  from a host `(int)NaN` conversion. A hardware capture through the existing
  Loop B probe — one triangle with a NaN bone matrix and one with a NaN vertex
  normal, read back through `sceGuGetMemoryStick`/`sceGuFinish` into a host
  framebuffer hash — would settle whether the hardware culls, clamps or
  rasterizes such a vertex, and is the measurement this cell is waiting for.
  The related producer is a separate cell: the out-of-domain arc-sine argument
  that makes a bone matrix non-finite in the first place.
- **Misaligned data access** (runs PSP-A3-02 and PSP-A3-03; same route,
  campaign and console; fixtures `exception-a3-mload` and `exception-a3-mstore`,
  which are `probe_exception_a3.c` built with `-DA3_CASE=2` and `=3`):
  - A misaligned user-mode **load** (`lw $t6, 2($t5)`) raises AdEL, Cause
    `0x10000010` (ExcCode 4). A misaligned **store** (`sw $t6, 2($t5)`) raises
    AdES, Cause `0x10000014` (ExcCode 5). The two are distinct codes, not one
    shared address error.
  - EPC is the faulting access itself, cross-checked against the address the
    probe recorded for its own instruction before faulting.
  - **BadVAddr is the effective address including the misaligned low bits**
    (base + 2), not the address rounded down to alignment.
  - The destination register is unchanged on the faulting load, and execution
    did not continue past the access.
  - The base in both runs is a 16-byte-aligned, mapped, writable buffer the
    probe module owns, so the fault is attributable to the low address bits
    rather than to an absent page.
  - Cause bit 31 (BD) was clear in both. The delay-slot, halfword,
    `lwl`/`lwr`/`swl`/`swr` and VFPU load/store cases were measured afterwards
    (PSP-A3-04 to PSP-A3-15, below); no VFPU alignment cell from that list
    remains unmeasured.
  - Cause bit 28 read as 1 in all three runs, so treat the CE field as
    undefined for non-coprocessor-unusable exceptions.
- **CPU exception delay-slot and width cells** (runs PSP-A3-04, PSP-A3-05,
  PSP-A3-06; same route and campaign `psp-hw-20260917`; fixtures
  `exception-a3-delayslot`, `exception-a3-half-load`, `exception-a3-half-store`
  and `exception-a3-unaligned`, which are `probe_exception_a3.c` built with
  `-DA3_CASE=4`, `=5`, `=6` and `=7`):
  - A misaligned `lw $t6, 2($t5)` in the delay slot of an always-taken branch
    raises AdEL: EPC = the branch address (`0x088043B8`, not the load at
    `0x088043BC`), Cause `0x90000010` (ExcCode 4, BD 1), BadVAddr =
    the effective address `0x08821C52` (t5 `0x08821C50` + 2, low bits kept).
    The destination register is unchanged and neither successor executed.
    This mirrors PSP-A2-01, which measured the same BD/EPC rule for `break`.
  - A halfword load (`lh $t6, 1($t5)`) at an odd address raises AdEL, Cause
    `0x10000010` (ExcCode 4, BD 0): EPC = the access `0x08838CB8`, BadVAddr =
    `0x088564F1`. A halfword store (`sh $t6, 1($t5)`) at an odd address
    raises AdES, Cause `0x10000014` (ExcCode 5, BD 0): EPC = `0x0886D4B8`,
    BadVAddr = `0x0888ACF1`. Both bases are 16-byte-aligned owned buffers,
    so the rule is width-relative (odd faults for a halfword).
  - `lwl`/`lwr`/`swl`/`swr` across alignment boundaries complete normally
    (negative control: the probe returned via `sceKernelExitGame` and wrote
    `host0:/a3_unaligned_results.txt`, no exception). For source bytes
    `11 22 33 44 55 66 77 88 99 aa bb cc`, the `lwl v0,1(t0)` +
    `lwr v0,4(t0)` pair loaded `0x88776655`, single `lwl`/`lwr` at +2 gave
    `0x332211ff` / `0x00004433`, matching the `sr_lwl`/`sr_lwr` merge model
    bit-for-bit. The exemption in `LLE_ACCESS`, the interpreter width table
    and `sr_cpu_data_access_fault` (never guarded) is thereby measured.
  - Status in all faulting runs is `0x00088613`, as in PSP-A1-01/A2-01/A3-01.
- **VFPU load/store alignment cells** (runs PSP-A3-08, PSP-A3-09 (+4 and +8
  variants), PSP-A3-10, PSP-A3-11; same route and campaign `psp-hw-20260917`;
  fixtures `exception-a3-vfpu-lvs`, `exception-a3-vfpu-lvq4`,
  `exception-a3-vfpu-lvq8`, `exception-a3-vfpu-svq4` and
  `exception-a3-vfpu-aligned`, which are `probe_exception_a3.c` built with
  `-DA3_CASE=8`, `=9`, `=10`, `=11` and `=12`):
  - Every VFPU probe runs with main-thread attribute
    `THREAD_ATTR_USER | THREAD_ATTR_VFPU`. No run reported CpU (ExcCode 11),
    which confirms the attribute held: the measured codes are AdEL/AdES (4/5),
    not coprocessor-unusable. Status reads `0x40088613` in all four faulting
    runs -- the historical `0x00088613` plus CU2 (`0x40000000`), the VFPU-enable
    bit the attribute sets.
  - A single VFPU load (`lv.s S000, 0($t5)`, `0xC9A00000`) at base+2 of an
    owned 16-byte-aligned buffer raises AdEL: EPC = the access `0x088043B8`
    (cross-checked against the recorded `a3_load_instruction`), Cause
    `0x10000010` (ExcCode 4, BD 0), BadVAddr = `0x08821C92` (t5, i.e. base
    `0x08821C90` + 2, low bits kept). Singles therefore need 4-byte alignment.
  - A quad VFPU load (`lv.q C000, 0($t5)`, `0xD9A00000`) at base+4 raises AdEL:
    EPC = `0x08838CB8`, Cause `0x10000010`, BadVAddr = `0x08856584` (base
    `0x08856580` + 4). The same load at base+8 also raises AdEL: EPC =
    `0x0886D5B8`, Cause `0x10000010`, BadVAddr = `0x0888AE78` (base + 8). Quads
    therefore need 16-byte alignment -- a 4-byte or 8-byte rule would have let
    one or both of these through.
  - A quad VFPU store (`sv.q C000, 0($t5)`, `0xF9A00000`) at base+4 raises
    AdES: EPC = the store `0x088A1EB8`, Cause `0x10000014` (ExcCode 5, BD 0),
    BadVAddr = `0x088BF794` (base `0x088BF790` + 4). Quad stores match the
    16-byte rule with the store code.
  - The negative control returns normally (via `sceKernelExitGame`) and writes
    `host0:/a3_vfpu_aligned_results.txt`, no exception: an aligned `lv.q` /
    `sv.q` round-trip reproduces the source quad bit-for-bit (`11223344
    55667788 99aabbcc ddeeff00`), and `lvl.q` / `lvr.q` at the 4-byte-aligned
    unaligned-to-16 address src+4 complete without faulting (C010 lanes land as
    `aaaaaaaa bbbbbbbb 11223344 55667788`, C020 as `55667788 99aabbcc ddeeff00
    dddddddd` from the `aaaaaaaa..dddddddd` fill). The left/right bypass in
    `LLE_ACCESS`-style guarding is thereby measured for the VFPU group, exactly
    as PSP-A3-06 did for `lwl`/`lwr`/`swl`/`swr`.
  - Encoding note the probes worked around: the low two bits of a VFPU memory
    offset belong to the register encoding (`lv.s S000, 2($t5)` assembles to
    `lv.s S002, 0($t5)`), so every faulting VFPU probe holds the full effective
    address in `$t5` with offset 0.
  - Guard wiring is now wired in both CPU tiers under `--lle-cpu`:
    `tools/codegen.py` emits the VFPU memory forms from `vfpu_effect()`
    through the same `sr_cpu_guard_access()` call as scalars (lv.s/sv.s width
    4, lv.q/sv.q width 16, lvl/lvr/svl/svr width 0 bypass), `sr_vfpu_interp()`
    checks before the access, and the interpreter width table carries the VFPU
    rows at the same point relative to the access as scalar loads/stores.
    Default (non-LLE) output is byte-identical. The remaining VFPU cells
    (`sv.s` at +2, `sv.q` at +8, VFPU in a delay slot, left/right at odd) are
    measured in the next section, which confirms this guard rather than
    contradicting it.
- **VFPU remaining alignment cells** (runs PSP-A3-12, PSP-A3-13, PSP-A3-14,
  PSP-A3-15; same route and campaign `psp-hw-20260917`; fixtures
  `exception-a3-vfpu-svs`, `exception-a3-vfpu-svq8`,
  `exception-a3-vfpu-lvq-delay` and `exception-a3-vfpu-lvl-odd`, which are
  `probe_exception_a3.c` built with `-DA3_CASE=13`, `=14`, `=15` and `=16`):
  - Every probe runs with main-thread attribute
    `THREAD_ATTR_USER | THREAD_ATTR_VFPU`. No run reported CpU (ExcCode 11):
    the measured codes are AdEL/AdES (4/5), not coprocessor-unusable, and
    Status reads `0x40088613` in all three faulting runs.
  - A single VFPU store (`sv.s S000, 0($t5)`, `0xE9A00000`) at base+2 raises
    AdES: EPC = the store `0x088FB1B8` (cross-checked against the recorded
    `a3_load_instruction`), Cause `0x10000014` (ExcCode 5, BD 0), BadVAddr =
    `0x08918A92` (base `0x08918A90` + 2, low bits kept). Singles therefore
    need 4-byte alignment on the store side too, matching the `lv.s` load
    cell (PSP-A3-08) with the store code.
  - A quad VFPU store (`sv.q C000, 0($t5)`, `0xF9A00000`) at base+8 raises
    AdES: EPC = `0x0892FAB8`, Cause `0x10000014`, BadVAddr = `0x0894D388`
    (base `0x0894D380` + 8). Together with PSP-A3-10 (`sv.q` at +4), quads
    need 16-byte alignment on the store side -- an 8-byte rule would have let
    this through.
  - A quad VFPU load (`lv.q C000, 0($t5)`, `0xD9A00000`) at base+4 in the
    delay slot of an always-taken branch (`b`, `0x10000005` at `0x089643B8`,
    load at `0x089643BC`) raises AdEL with Cause `0x90000010` (ExcCode 4,
    BD 1): EPC = the branch `0x089643B8` (not the load), BadVAddr =
    `0x08981C64` (base `0x08981C60` + 4). This mirrors PSP-A3-04, which
    measured the same BD/EPC rule for a scalar word, so the `in_delay`
    bookkeeping in `sr_cpu_guard_access()` is thereby measured for the VFPU
    group.
  - The odd-address control returns normally (via `sceKernelExitGame`) and
    writes `host0:/a3_vfpu_lvl_odd_results.txt`, no exception: `lvl.q C010,
    0($t5)` (`0xD5A10000` at `0x08998C9C`) at the odd address `0x089B6421`
    (src base `0x089B6420` + 1) completes without faulting, leaving
    `aaaaaaaa bbbbbbbb cccccccc 11223344` from the `aaaaaaaa..dddddddd` fill
    (only the merged lane takes the source word `11223344`). The width-0
    left/right bypass is thereby measured at an odd address, exactly as
    PSP-A3-11 did at a 4-byte-aligned address.
  - The same offset-0 encoding note applies: every faulting probe holds the
    full effective address in `$t5` with offset 0, and each frame's `t5`
    equals BadVAddr while `t6` stays `0x0badc0de` and `t7` stays unwritten.
- **Kernel-object semantics** (runs PSP-B1-01, PSP-B2-01, PSP-B3-01; same
  route and campaign; fixtures `kobj-b1`, `wait-b2`, `kernel-b3`):
  - *Error codes for unknown IDs:*

    | Object | Code |
    | --- | --- |
    | Semaphore | `0x80020199` |
    | Event flag | `0x8002019A` |
    | Mailbox | `0x8002019B` |
    | VPL | `0x8002019C` |
    | FPL | `0x8002019D` |
    | Message pipe | `0x8002019E` |
    | Alarm | `0x8002019F` |
    | Callback | `0x800201A1` |
    | VTimer | `0x800201BE` |
    | Thread | `0x80020198` |

  - *Wait outcomes:*
    - A timeout returns `0x800201A8` and writes the remaining timeout (0).
    - A cancel returns `0x800201A9`, from CancelSema, CancelEventFlag or
      CancelReceiveMbx.
    - ReleaseWaitThread returns `0x800201AA`.
    - Deleting the object being waited on (semaphore, FPL) returns
      `0x800201B5` to the waiter while the delete itself succeeds.
  - *Semaphores:*
    - Signalling past max returns `0x800201AE` and leaves the count unchanged.
      `Signal(0)` returns 0. `Signal(-1)` returns 0 and decrements the count.
    - Create accepts init > max and max 0. Attr `0xFFFFFFFF` returns
      `0x80020191`.
    - `CancelSema(-1)` resets the count to the initial count.
  - *Event flags:*
    - `ClearEventFlag(mask)` keeps only the bits in mask.
    - Wait mode `0x20` clears only the matched bits; `0x10` clears the whole
      pattern.
    - An unmet poll writes the current pattern to outBits. Bits 0 returns
      `0x800201B1`.
    - A second waiter on a single-wait flag returns `0x800201B0` immediately.
  - *FPL and VPL:*
    - FPL blocks are exactly blockSize apart.
    - A 1024-byte VPL reports 992 bytes. Each VPL allocation costs
      `roundup(size, 8) + 8`, and allocations run from high addresses
      downward.
    - Both pool types can be deleted while blocks are held, and a freed block
      is handed directly to a waiter.
  - *LwMutex:*
    - The workarea is `lockLevel`, `lockThread`, `attr`, `numWaitThreads`,
      `uid`.
    - An owner's unlock hands ownership directly to the waiter.
    - A TryLock that cannot own, or has count 0, returns `0x800201C4`.
    - A non-owner or underflowing unlock returns `0x800201CC`.
    - A self-held non-recursive timed Lock fails immediately with
      `0x800201CF`.
    - Delete writes `0xFFFFFFFF` to `lockThread` and `uid`.
  - *Message pipes:*
    - An all-or-nothing send without room returns `0x800201B3`; mode 1 sends
      what fits.
    - A size larger than the buffer returns `0x800201BC`.
    - A blocked receiver is served directly by a sender.
  - *Mailboxes:*
    - FIFO mailboxes link packets circularly.
    - Attr `0x400` delivers by ascending `msgPriority` (FIFO among equals).
    - An empty poll returns `0x800201B2`.
  - *Threads:*
    - Status codes:

      | Condition | Code |
      | --- | --- |
      | Dormant target | `0x800201A2` |
      | Double suspend | `0x800201A3` |
      | Resume of a thread that is not suspended | `0x800201A5` |
      | Exit status of a live thread | `0x800201A4` |
      | Exit status of a terminated thread | `0x800201AC` |

    - Wakeups accumulate in `wakeupCount` while the target is not sleeping.
    - ReferThreadStatus waitType values:

      | Wait | waitType |
      | --- | --- |
      | Sleep | 1 |
      | Delay | 2 |
      | Semaphore | 3 |
      | Event flag | 4 |
      | Mailbox | 5 |
      | FPL | 7 |
      | Message pipe | 8 |
      | LwMutex | 13 |

  - *Callbacks, VTimer, alarms and delays:*
    - Two notifies coalesce into one handler call with count 2.
    - StartVTimer returns 1 if the timer is already running.
    - StopVTimer returns 1 if the timer was running, otherwise 0.
    - An alarm handler's non-zero return reschedules it.
    - `DelayThread(n)` takes about n + 30-40 µs, with a floor near 236 µs for
      1-100 µs requests.
  - Heavyweight mutexes are covered by the plain-mutex cases, not these runs.

One console is one data point; see §11. Nothing here closes issue #70
(residual VBLANK delivery/coalescing) or generalizes to unmeasured cells.

> **Tracker numbering.** Bare `#N` references in the loops below are
> **pre-republication tracker numbers**, not current public tracker mappings:
> the sanitized public repository restarted GitHub's single issue/PR sequence,
> so a bare number here may resolve to an unrelated live public object. The
> measured index above names live issues in words (`issue #23`); read every
> other bare number as a historical identifier.

## 1. The gap this closes

`tools/verify_gates.py` reports, verbatim:

```text
BLOCKED: CODEGEN_ORACLE not set (need a PPSSPP-captured .trace for <elf>)
```

**PPSSPP-captured** is the problem. Full verification currently compares this project against
another reimplementation. Where PPSSPP is wrong or approximate, we inherit the error invisibly.
[AGENTS.md](../AGENTS.md) already forbids describing renderer agreement as an external oracle; the
same limit applies to trace agreement.

A PSP running custom firmware executes on real Allegrex silicon and is the only ground truth
available in the absence of documentation.

## 2. Integration surface that already exists

| Existing piece | Role |
| --- | --- |
| [`TRACE_FORMAT.md`](../tools/TRACE_FORMAT.md) | Versioned textual CPU-state-diff format |
| `tools/gen_microtest.py` | Emits CRT-free Allegrex test modules; takes `--groups` / `--opcodes`; seeded and deterministic |
| `tools/microtest_gate.py` | Compares `src/ref` against an oracle trace, truncated at the first syscall so everything compared is pure CPU |
| `tools/codegen_gate.py` | Same shape, generated code vs oracle |
| `tools/verify_gates.py` | Orchestrates gates and reports BLOCKED rather than silently downgrading |
| `src/rt/vfpu_interp.c`, `vfpu_fuzz.c` | Existing differential harness over pinned tables |

The trace header is currently:

```text
# psp-recomp trace v1 target=<name> oracle=<ppsspp|interp|recomp> start_pc=<hpc> steps=<N>
```

Hardware support means extending that enum and recording provenance — see §9.

## 3. Architecture: a resident runner, not per-test rebuilds

Generating and cross-compiling a module per test costs a `psp-gcc` build and an upload per
iteration. Instead, build **one resident runner PRX that interprets test descriptors**:

```text
  Host                                     PSP (CFW + PSPLink)
  ----                                     -------------------
  write descriptor  ──> host0:in/NNN.bin ──> set state, execute, dcache writeback
  read + compare   <── host0:out/NNN.bin <── write result vector
  localize divergence, patch, repeat
```

The runner is built once. Each iteration is a file write, a device-side execution, and a file
read. This is the difference between a message pass and a build step, and it is what makes the
loop viable at all.

**`host0:` maps to the host directory `usbhostfs_pc` was started in.** That is the automation
primitive: no memory-stick swapping, no manual copying.

### Why not emit full per-instruction traces on hardware

Single-stepping via the PSPLink GDB stub would let the existing gates run unmodified, but it is
slow (est. tens to low hundreds of steps/sec) and does not scale.

**Recommended split:** result-vector comparison for bulk work (needs a small new
`tools/hwtest_gate.py`), and single-step traces only to localize a divergence once found. Bulk
compare to learn *that* something diverged; single-step to learn *where*.

## 4. Three loops, ranked

### Loop A — VFPU differential fuzz (highest value)

VFPU is under-documented, `assets/vfpu/` tables are PPSSPP-derived, and PPSSPP approximates parts
of it. Generate random (opcode, prefix, register state) → execute on hardware → compare against
`sr_vfpu_interp` → minimize → regression test. Fully mechanical, no game content, serves #36.
Target prefixes, NaN propagation, rounding modes, `vrot`, divide-by-zero, denormals.
Bulk fuzz is still unbuilt; the measured Group-A/Group-B fixture cells indexed
above are the exception, not the rule.

### Loop B — Per-opcode microtests (lowest integration cost)

`gen_microtest.py` already emits the right module shape and already accepts `--opcodes`. Pointing
its oracle at hardware turns the microtest gate into a real gate.

### Loop C — Kernel/HLE semantics (largest issue payoff)

Where "PPSSPP does X" is explicitly not proof: #1 callbacks, #2 mutex/LwMutex, #13 semaphore
waiter cancellation, #14 async I/O, #16 FPL/thread-stack lifetime, #64 VBLANK sub-interrupts, #88
interrupt pending state. Bespoke per probe, but it is where the unanswered questions live.
Measured-index carve-outs (these cells need no new probe): #2 mutex context
expectations are now implemented against (dedicated handlers in `src/rt/hle.c`;
see the intr-conformance snapshot), and display-mask/vcount coalescing is
HARDWARE_MEASURED per the index above. The loops remain for the unmeasured
remainder — VBLANK sub-interrupts beyond the masked-window cells, async I/O,
FPL/thread-stack lifetime, and interrupt pending state stay NOT_MEASURED until
a probe measures them.

## 5. Device choice: PSP vs PS Vita

A Vita does **not** contain Allegrex. It runs PSP titles through Sony's PspEmu on ARM. But
Adrenaline and ARK-4 do not reimplement the PSP kernel — they modify PspEmu to boot **real PSP
6.61 firmware modules**. So the Vita splits the two layers:

| Oracle | CPU | Kernel/HLE |
| --- | --- | --- |
| PSP | real silicon | real Sony firmware |
| Vita ePSP | emulated on ARM | **real Sony firmware** |
| PPSSPP | reimplemented | reimplemented |
| This project | reimplemented | reimplemented |

That split makes the disagreement pattern diagnostic:

| Pattern | Conclusion |
| --- | --- |
| PSP = Vita, PPSSPP differs | PPSSPP bug |
| PSP = PPSSPP, Vita differs | ePSP emulation artifact; discount the Vita for that class |
| PSP differs from both | Real silicon behaviour both emulators approximate — highest value |
| PSP ≠ Vita on kernel behaviour | Implicates the CPU-emulation layer or a firmware version gap |

**Calibrate before trusting.** Run the same microtest corpus on both devices. Where they agree, the
Vita is an empirically validated proxy *for that class*; where they diverge, the ePSP approximation
boundary is mapped. This replaces assumption with measurement, and the calibration corpus itself
becomes a committable regression asset.

**Do not use the Vita for instruction semantics.** Recording an emulation layer's approximations as
hardware truth would produce something that looks like tier-1 evidence and is not.

## 6. Physical setup

Requires a CFW-capable PSP (1000/2000/3000 use **mini-USB Type B**; the Go uses a proprietary
connector), a **Memory Stick PRO Duo**, a data-capable cable, and mains power.

1. **Firmware** — official 6.60 or 6.61 is a prerequisite for the ARK CFW.
2. **CFW** — install ARK (`ARK_01234` → `PSP/SAVEDATA/`, `ARK_Loader` → `PSP/GAME/`), then make
   it permanent with Infinity so a reboot cannot silently drop the device out of CFW mid-session.
   **This workspace's qualified route is ARK-5.1.0**, which is what every measured cell above was
   taken on; "ARK-4" elsewhere in this document is the upstream project name, not the version to
   install. Do not cite a measurement against a route you did not run it on.
3. **PSPLink** — extract the psplinkusb release to `ms0:/PSP/GAME`.
4. **Windows driver** — with PSPLink running and USB connected, use Zadig: *Options → List All
   Devices*, select **`"PSP" type B`**, install the **`libusb-win32`** driver.
5. **Connect** — run `usbhostfs_pc` and `pspsh` in two terminals, **both opened in the build
   directory**. `pspsh` gives a `host0:/>` prompt.
6. **Toolchain** — PSPSDK on the host; build homebrew as an unencrypted `.prx` with `BUILD_PRX=1`.
   Keep PSPSDK out of the native build, like the other optional tools in [SETUP.md](SETUP.md).

**Vita transport (if used):** Adrenaline exposes `ux0:/pspemu/` as the ePSP's virtual memory stick,
so `ms0:` ↔ `ux0:pspemu/`, and VitaShell serves FTP on port 1337. That routes around USB
device-mode entirely. Caveat: VitaShell's FTP needs VitaShell in the foreground while Adrenaline
takes the foreground when running, making this a batch loop rather than a tight one.

### First milestone

A hello-world `.prx` built on the host runs on the device without touching the memory stick, and
something it writes appears in the host build directory. If that is painful, reconsider before
building further.

## 7. Determinism rules

Violating these makes the oracle lie:

- **Set `fcr31` explicitly per test.** Allegrex flush-to-zero and denormal behaviour depends on it,
  and that is often exactly what is being measured.
- **Keep the measured window single-threaded / interrupts disabled.**
- **`sceKernelDcacheWritebackAll`** (or the range variant) before the host reads a result buffer,
  or stale bytes are read over USB.
- **Pin the CPU clock** (222 vs 333 MHz) for anything timing-sensitive.
- **Record model, firmware, CFW version and clock** in every trace header.

## 8. Agent execution contract

An AI agent may own the host-side work. It must **never**:

- install or flash custom firmware, or update console firmware;
- install USB drivers (the Zadig step);
- handle retail game content in any form.

Physical gates requiring a human: inserting the memory stick, firmware/CFW install, the Zadig
driver step, launching PSPLink, connecting USB, power-cycling after a hang, and Vita foreground
app switching.

An agent may own: the PSPSDK toolchain, the runner PRX and its build, the descriptor format and
codecs, `tools/hwtest_gate.py`, driving `pspsh` non-interactively, comparison, divergence
bisection, fuzz-input minimization, and writing regression tests.

**The handoff must be machine-checkable.** This is now partly shipped as
`tools/psp_readiness.py`, which verifies `psp-gcc`/`usbhostfs_pc`/`pspsh` presence and reports
`OPTIONAL_MISSING`/`HARDWARE_NOT_CONNECTED` rather than silently downgrading. Still outstanding is
the *real* precondition: that a file round-trips through `host0:` in both directions, and that
`usbhostfs_pc` is actually running and `pspsh` responds. Extend `tools/psp_readiness.py` with those
live checks; do not add a separate `tools/hw_doctor.py`.

Agents without a persistent background process cannot host `usbhostfs_pc` and cannot drive the USB
loop; route those to the batch path.

## 9. Repository changes required

1. `tools/TRACE_FORMAT.md` — extend `oracle=` with `hardware-psp` and `vita-epsp`; add
   model/firmware/CFW/clock fields.
2. `tools/hwtest_gate.py` — **new**, result-vector comparison.
3. `tools/psp_runner/` — **new**, PSPSDK sources for the resident runner, excluded from the native
   build.
4. ~~`tools/hw_doctor.py` — **new**, the machine-checkable precondition check.~~ Superseded by the
   shipped `tools/psp_readiness.py`; add the live `host0:` round-trip check there.
5. `Makefile` — a `hw-verify` target reporting BLOCKED when no device is attached.
6. `tools/verify_gates.py` — register the hardware gate with the same BLOCKED semantics.
7. `publish_audit.py` / `.gitignore` — permit homebrew traces, reject retail-derived captures.

## 10. Scope discipline

| Artifact | Status |
| --- | --- |
| Homebrew module authored here + its hardware trace | Ours. Committable. |
| VFPU fuzz corpus + hardware results | Ours. Committable. |
| Any trace of the **retail game** on hardware | Game-derived. Private, permanently. |
| Frame or memory dumps from the retail title | Game-derived. Private, permanently. |

This is the strategic argument for the whole plan. A pre-republication tracker
item, historically numbered #35, was blocked on obtaining oracle evidence
without redistributing proprietary inputs. **Hardware microtests authored here are the
only oracle path identified so far that is committable**, and therefore the only one that could
ever run in public CI. Loops A and B stay entirely on the committable side; Loop C does too, as
long as probes are homebrew rather than instrumentation of the shipped title.

## 11. Limits and anti-patterns

- **Not a decompilation aid.** Recovering source that compiles to identical bytes is a
  compiler-output matching problem ([DECOMPME_INTEGRATION.md](DECOMPME_INTEGRATION.md)). Hardware
  has nothing to say about it. This is behavioural verification only.
- **Never majority-vote oracles.** With several oracles it is tempting to take best-of-N. Two
  emulators can outvote real silicon, which is exactly how an emulation artifact becomes enshrined
  as ground truth. Disagreement is data.
- **Never merge provenance.** A Vita result must never be readable as a PSP result.
- **One console is one data point.** Model and firmware differences exist; do not generalize.
- **A hardware result proves the behaviour actually executed** — one opcode, not a subsystem.
- **All oracles agreeing does not mean correct.** They may share an assumption inherited from the
  same documentation.
- **Crash recovery needs a human** or a USB-controlled relay; a hard hang requires a power cycle.
- **A whole-game hardware trace is not realistic.** Do not plan around it.

## Sources

- [PSPLink Windows setup](https://pspdev.github.io/psplink/windows.html) ·
  [PSPLink debugging](https://pspdev.github.io/debugging.html) ·
  [pspdev/psplinkusb](https://github.com/pspdev/psplinkusb) ·
  [PSPSDK](https://github.com/pspdev/pspsdk)
- [ARK-4](https://github.com/PSP-Archive/ARK-4) ·
  [Installing ARK-4](https://consolemods.org/wiki/PSP:Installing_ARK-4_CFW) ·
  [ARK-4 Infinity](https://github.com/PSP-Archive/ARK-4/wiki/Infinity)
- [Adrenaline](https://github.com/TheOfficialFloW/Adrenaline) ·
  [Adrenaline (ConsoleMods)](https://consolemods.org/wiki/Vita:Adrenaline) ·
  [VitaShell FTP](https://consolemods.org/wiki/Vita:Transferring_Files_with_FTP)
