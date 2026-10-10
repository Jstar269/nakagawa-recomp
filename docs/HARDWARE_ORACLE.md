# Hardware oracle plan — a real PSP as an external verification source

**Status: the resident PSP-side trace producer described below is still a proposal.** The v2
trace format, strict comparator, and verifier consumer are implemented; throughput figures
for the producer remain estimates and are labelled as such. This document is a plan in the
same sense as
[DECOMPME_INTEGRATION.md](DECOMPME_INTEGRATION.md), not a record of completed work.

> **A narrower subset of this plan is now implemented.**
> The [source-owned scalar-probe runbook](../fixtures/psp_oracle/README.md), its
> [strict result protocol](../tools/psp_oracle/protocol.py), and the shipped
> [`tools/psp_readiness.py`](../tools/psp_readiness.py) cover the implemented subset. Read those
> first. In particular, the `tools/hw_doctor.py` proposed in §7 is superseded by the readiness tool
> — extend that tool rather than adding a second precondition checker.
> What remains unbuilt here is the PSP-side v2 instruction-trace producer and capture path.
> `verify_gates.py` can consume a supplied `PSP_HARDWARE` + `LOCAL_COSIM` pair, but the scalar
> probe and this verifier change do not produce a real-PSP trace. The producer is in the works
> as a later hardware slice under issue #312.

## GE raster pixel corpus (#343)

`assets/ge_corpus.schema.json` defines version 1 of the source-owned GE case corpus. The first
case is an untextured solid triangle in a 16×16 8888 framebuffer. Its selected pixels and full
framebuffer digest are deliberately unset until a qualified PSP run records them. The case uses
the PSP GE command word layout; address operands are explicit relocations to its source-owned
vertex words. A result records `framebuffer_sha256` and one `pixel_<x>_<y>` value for each selected
coordinate, packed according to the case framebuffer format.

Run `python tools/psp_oracle/run_psplink.py --ge-corpus-gate` to validate the corpus and report each
case. A case is `MEASURED` only when its source tier is `PSP_HARDWARE`, the oracle envelope is
acceptance-eligible, and the raw result protocol identifies `source=psp`. The public corpus never
inlines raw hardware result text: its envelope's `RESULT_RECORD` names a file under the private
results directory (`--results-directory`, default `oracle/hardware-results/`) by relative path and
SHA-256, and the gate verifies that file. Without that directory a hardware claim reports
`NOT_RUN`, never `MEASURED`. Software, Vulkan, or
PPSSPP data cannot be promoted by setting a hardware label. The current case reports `NOT_RUN`.
**GE raster pixel conformance is IN THE WORKS (#343);** a real run still depends on the resident
runner and recovery substrate tracked by #352.

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
- **Out-of-domain transcendental arguments** (issue #69): `HARDWARE_MEASURED`
  for these 14 exact raw words on the authorized PSP-3000-series / 6.61 /
  ARK-5.1.0 route (2026-09-30). Fixture: `fixtures/vfpu_oracle/vfpu_probe.c`
  (`FIXTURE_BUILD_ID nakagawa-vfpu-oracle-v1`), whose source-owned
  `VASIN_DOMAIN_INPUTS` list fed the console; each word produced one record,
  `vfpu-vasin-domain-arg00` through `vfpu-vasin-domain-arg13`, in the table's
  order. These were two fresh USBHostFS/PSPLink sessions of that single probe
  (no multi-fixture campaign identifier applies); both returned
  byte-identical records, and the raw captures and the PRX digest stay
  private. `FIXTURE_BUILD_ID` names the source fixture, not the built PRX: this
  probe's `NAKAGAWA_PSP_META` record still carries the all-zero binary and
  commit placeholders (unlike `fixtures/psp_oracle/probe.c`, which refuses to
  build without a commit), so this is a documented-run measurement of these
  exact words, not a device-bound capture, and the PSP oracle runner would
  classify it `IDENTITY_NOT_BOUND` / not acceptance-eligible. The project's shared runtime `vasin` helper (`sr_vfpu_asin`)
  returns the signed invalid signaling NaN that the PSP returned for every
  sampled out-of-domain word, and the endpoints return themselves. Do not
  clamp these NaNs in VASIN: downstream handling is required, and both
  rasterizer paths already drop and count non-finite primitives. This is
  evidence for these exact words, not every possible out-of-domain encoding.

  | Input word | PSP result word |
  | :--- | :--- |
  | `0xBF800000` | `0xBF800000` |
  | `0x3F800000` | `0x3F800000` |
  | `0xBF800001` | `0xFF800001` |
  | `0x3F800001` | `0x7F800001` |
  | `0x3F80000B` | `0x7F800001` |
  | `0xBF80000B` | `0xFF800001` |
  | `0xBF80DABC` | `0xFF800001` |
  | `0xBF82026A` | `0xFF800001` |
  | `0xBF8FA2B7` | `0xFF800001` |
  | `0xBF9A419C` | `0xFF800001` |
  | `0xBFB63DDA` | `0xFF800001` |
  | `0xBFFB5A51` | `0xFF800001` |
  | `0xBFFFFE00` | `0xFF800001` |
  | `0xC0000000` | `0xFF800001` |

  A qualified private-title route reaches this edge: its `NAN_TRAP`
  diagnostic on the main-menu transition shows the guest rotation solve
  leaving the arc-sine domain (`|x|` in `[1.026, 2.0]`) and propagating to the
  uploaded bone matrices. The measured result does not establish the root
  cause of the one-frame skinned-model corruption; that issue remains in the
  works under #69.
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
  verdict is implied by its source either). Both rasterizer paths therefore
  fail closed: `src/rt/ge.c` drops the primitive and counts it (`GESTAT+ ...
  nonfinite=`, and `drop-nonfinite` in the per-draw `TRIDRW+` line) instead of
  rasterizing it from a host `(int)NaN` conversion, and the Vulkan rasterizer
  (`src/rt/gpu_sdl3vk/ge_gpu.c`) applies the same rule at its capture seam
  (`GEGPU stats: ... nonfinite=`) instead of handing a NaN `gl_Position` to the
  fixed-function clipper, whose verdict for NaN is undefined. The two share one
  predicate (`ge_vtx_finite` in `src/rt/ge_shared.h`) so they cannot disagree.
  `CASE=ge-nan` now emits `PSP-GE-001` raw-word records for qNaN, positive and
  negative infinity, negative zero, and the minimum positive denormal. It also
  hashes the PSP framebuffer after direct screen-coordinate, clip-position,
  and lit-normal triangles. This probe is `NOT_RUN` pending the physical
  campaign, does not repeat the already measured VASIN edge words, and has not
  yet established a hardware rasterization rule; this GE boundary remains in
  the works under #69. The out-of-domain arc-sine argument that can produce a
  non-finite bone matrix remains a separate cell.
- **Audio query and blocking-output semantics** (issue #311; `CASE=audio-query`):
  `NOT_RUN`. The probe captures both channel rest-length spellings, two bounded
  channel output blocks, one silent Output2 block, rest-sample queries, raw
  returns, and elapsed microseconds. Each successfully reserved channel is
  released. No audio timing or return rule is hardware-measured by this build;
  the remaining #311 audio contract is in the works.
- **DMAC alignment and overlap cells** (issue #303; `CASE=dma-cells`):
  `NOT_RUN`. The source-owned buffer matrix covers both APIs, unaligned source
  and destination residues, paired unaligned addresses, and overlap in both
  directions. It records cache-disciplined copy observations and owned-buffer
  guards; the previously measured size matrix is not repeated. The Tier S and
  Tier B invalid-tail cells below are built but `NOT_RUN`; they exercise only
  zero-length pointer validation and bounded copies wholly inside owned RAM.
  Kernel validation of an actually invalid span, including the K3/K4 atomicity
  distinction, remains in the works under #303.

**DMA invalid-tail cells (issue #303).**

The old destination-tail shape issued a DMA write beyond probe ownership and
therefore remains `SKIP`. The new `dma-invalid-tail-s0` and four existing
`dma-invalid-tail-*` launch names use two bounded tiers. Their results are
`NOT_RUN` until a physical PSP run is recorded. These are oracle measurements,
not claims about the emulator or runtime.

Each launch obtains one page-aligned partition-2 block from `PSP_SMEM_High`
with `sceKernelAllocPartitionMemory`. Its `0x11000` bytes have this fixed
layout:

| Region | Size | Fill | Purpose |
| --- | ---: | ---: | --- |
| Pre-guard | `0x1000` | `0x5A` | Detect writes before the payload |
| Payload | `K = 0xC000` | `0xC3` | Disputed oracle payload boundary |
| Post-guard | `0x1000` | `0xC3` | Classify the first bounded overrun bytes |
| Overflow band | `0x2000` | `0xA5` | Own and classify further bounded overrun bytes |
| Tail-guard | `0x1000` | `0x96` | Detect writes past the owned band |

A compile-time `_Static_assert` proves `K + max_delta` fits between the payload
start and the end of the post-guard plus overflow band. While the block is
held, the probe asks `PSP_SMEM_Addr` to allocate one page at `block_end` and at
`block_begin - 0x1000`. Both must be refused and recorded in `setup_mask`; an
allocation success or invalid geometry emits `SKIP` before any DMAC call. Each
transfer is bracketed by `sceKernelDcacheWritebackInvalidateRange` and
`sceKernelDcacheInvalidateRange` through the shared DMAC cache helpers.
`setup_mask=0x0F` means allocation, page geometry, end-neighbor refusal, and
begin-neighbor refusal all passed (bits 0 through 3 respectively).

The setup-failure **SKIP rule is shared by Tier S (S0) and Tier B (B1-B4)**:
`setup_mask != 0x0F`, `executed=0`, `cache_discipline=0`, and
`source_intact=0` (unmeasured, not a failed integrity observation). `P`,
`matches`, `guards_outside`, `post_guard`, `overflow_band`, and
`payload_mutations` must all be zero; SKIP cannot claim transfer observations.
The native producer regression compiles the real `dmac_invalid_emit_skips`
calls and feeds their S0/B1-B4 output to the shared parser. It also executes
wrong-integrity-argument mutants and requires parser rejection. This is
public-safe producer/protocol evidence, not a physical PSP capture.

Records separate `guards_outside` (pre-guard plus tail-guard), `post_guard`,
and `overflow_band` mutation counts. Any outside-guard mutation fails the
record and the parser rejects it. Raw addresses are diagnostics only. A Tier B
destination full copy changes `min(delta, 0x1000)` post-guard bytes and
`max(delta - 0x1000, 0)` overflow-band bytes; their sum is the observed copy
beyond `K`.

Tier S runs S0-a through S0-d for both APIs with `size=0`: null destination,
null source, both pointers owned, and destination `0xFFFFFFFF` respectively.
Every PASS record must show `P=0`, no payload or guard mutations, intact
source, and the complete ownership proof. `S0-c` is the control for firmware that may
reject a zero size even when both pointers are owned. These cells isolate
pointer handling at zero length; the K1-K4 large-span hypotheses do not define
all zero-length pointer outcomes, so raw return codes remain observations.

Tier B keeps each entire request inside the same allocation or a module-owned
array. The four existing launch names map to B1-B4, each with
`delta ∈ {1, 4, 0x1000, 0x2000}`:

| Cell | API | Endpoint | Owned shape |
| --- | --- | --- | --- |
| B1 | `sceDmacMemcpy` | destination | Destination begins at payload; the post-guard and overflow band absorb the requested tail |
| B2 | `sceDmacMemcpy` | source | Source begins at payload and the owned post-guard/band contain the read tail; destination is fully owned |
| B3 | `sceDmacTryMemcpy` | destination | Same bounded destination geometry as B1 |
| B4 | `sceDmacTryMemcpy` | source | Same bounded source geometry as B2 |

The candidate predictions for these declared-boundary controls are:

| Candidate | B1/B3 destination observation | B2/B4 source observation |
| --- | --- | --- |
| K1 refuse-too-large | Error, `P=0`, no guard mutations | Error, `P=0`, no guard mutations |
| K2 prefix-truncate at `0xC000` | `P=K`, no post-guard/band mutations | `P=K`, source stays intact |
| K2′ stop at the declared payload end | `P=K`, no post-guard/band mutations | `P=K`, source stays intact |
| K3 full copy without atomic validation | `P=K+delta`; mutations stay in the owned post-guard/band | `P=K+delta`; source stays intact |
| K4 validate the complete physical span | Same as K3 because the whole request is owned | Same as K3 because the whole request is owned |

The 2026-10-01 valid-span matrix already showed complete copies through 1 MiB,
so K1/K2 remain listed for comparison but are refuted for those qualified
valid spans. Tier B proves the guards can observe a contained destination
overrun; it cannot distinguish K3 from K4 and does not make the declared
payload end an invalid physical address. No cell sends a destination tail
outside ownership, enters VRAM (`0x04000000..0x041FFFFF`), targets kernel/MMIO
memory, uses an assumed top-of-RAM address, or re-enables the 1 MiB invalid-tail
variant (`PSP_LARGE_MEMORY=0` remains in force). An actually invalid-span probe
and its K3/K4 result remain in the works under #303.

- **Zero-duration thread yield and callback behavior** (scheduler work tracked
  by #340, related to #290; `CASE=delay-zero`; exact cells
  `delay-threadcb-zero`, `delay-thread-zero`, and `delay-zero-done`):
  **HARDWARE_MEASURED** on PSP-3000-series / 6.61, from the accepted 2026-10-01
  PSPLink campaign, build `8e95dc24`. With an equal-priority ready worker
  thread and a pending callback configured before each call:
  - `sceKernelDelayThread(0)` returns 0 without yielding to the equal-priority
    ready thread and without dispatching the pending callback.
  - `sceKernelDelayThreadCB(0)` returns 0 without yielding to the equal-priority
    ready thread, and dispatches the pending callback (exactly once).
  For these exact cells, zero-duration delays do not park or switch to the
  equal-priority ready thread; only `sceKernelDelayThreadCB` dispatches the
  pending callback before returning. Broader scheduler and callback-pump
  behavior remains in the works under #340.
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

- **PSP-3000 campaign on public commit `f6ccfb33` (2026-10-10)**: the campaign queue
  described under "Pending PSP-3000 measurement campaign" ran on the qualified PSP-3000 /
  6.61 / ARK-5.1.0 / PSPLink 3.2.1 route, one PRX per boot with a soft reset between cases:
  30 launched, 30 exited, teardown check PASS on all 30, 22 envelopes acceptance-eligible. A
  `MEASURED` row below cites exactly the cells named in its row; the eight captures that were
  not acceptance-eligible (the probe's own assertions or SKIP records differed from the
  console) are `CAPTURED` only and establish nothing. Raw envelopes, registry values and
  console identifiers stay in the private campaign directory. Rows measured earlier keep their
  own citations. The seven HLE measurement families (`probe_hle_measure.c`) ran the same day as
  single-case runs beside the campaign: every stream was complete, but no case was
  acceptance-eligible because the runner's post-unload shell qualification and host0 round trip
  failed after each unload, so their rows are `CAPTURED` only and cite nothing.

  | Evidence id | Cells cited | Not cited | Outcome (campaign case) |
  | --- | --- | --- | --- |
  | `PSP-TRANSPORT-001` | `host0-write-readback` | (none) | MEASURED (`transport-write`) |
  | `PSP-ALARM-001` | `alarm-null-handler`, `alarm-zero-clock`, `alarm-table-exhaustion`, `alarm-cancel-fired-once`, `alarm-cancel-cancelled`, `alarm-cancel-unknown`, `alarm-rearm-base`, `alarm-blocking-in-handler`, `kernel-alarm-done` | (none) | MEASURED (`kernel-alarm`) |
  | `PSP-THREAD-003` | `thread-suspend-idle`, `thread-suspend-self`, `thread-resume-idle`, `thread-rotate-range`, `thread-ready-order-after-rotate`, `thread-suspend-wait-timeout`, `thread-scheduler-done` | (none) | MEASURED (`thread-scheduler`) |
  | `PSP-WAIT-001` | `sema-signal-before-deadline-late-dispatch`, `event-signal-before-deadline-late-dispatch`, `sema-cancel-before-deadline-late-dispatch`, `event-cancel-before-deadline-late-dispatch`, `wait-outcomes-done` | (none) | MEASURED (`wait-outcomes`) |
  | `PSP-GE-CONTROL-001` | (none) | `ge-break-no-active-list`, `ge-continue-no-paused-list`, `ge-break-invalid-mode`, `ge-list-sync-paused`, `ge-draw-sync-paused`, `ge-list-sync-cancelled`, `ge-draw-sync-cancelled`, `ge-break-continue-done` | CAPTURED only (not acceptance-eligible: `ge-break-continue`) |
  | `PSP-KERNEL-STATUS-001` | `sema-size-zero`, `sema-size-8`, `sema-size-40`, `sema-size-full`, `event-size-zero`, `event-size-8`, `event-size-40`, `event-size-full`, `mbx-size-zero`, `mbx-size-8`, `mbx-size-40`, `mbx-size-full`, `refer-status-size-done` | (none) | MEASURED (`refer-status-size`) |
  | `PSP-REGISTRY-001` | `registry-open`, `registry-errors`, `registry-category-*`, `registry-key-*`, `registry-done` | (none) | MEASURED (`registry-readonly`) |
  | `PSP-KERNEL-MISC-001` | `sysclock-wide`, `ctrl-sampling-mode`, `thread-profiler`, `global-profiler`, `vtimer-basic`, `display-basic`, `impose-basic`, `kernel-misc-done` | (none) | MEASURED (`kernel-misc`) |
  | `PSP-SMOKE-001` | apis `module_start`, `sceKernelExitGame` | (none) | MEASURED (`smoke`) |
  | `PSP-THREAD-EXIT-001` | `ED-R77`, `ED-R00`, `ED-RNEG`, `ED-RERR`, `ED-X77`, `ED-X00`, `ED-XNEG`, `ED-XERR`, `ED-D77`, `ED-D00`, `ED-DNEG`, `ED-DERR` | (none) | MEASURED (`thread-exit-delete`) |
  | `PSP-TEARDOWN-001` | (none) | `exitdelete-main` | CAPTURED only (not acceptance-eligible: `teardown-test`) |
  | `PSP-IO-001` | apis `open`, `seek`, `async I/O`, `devctl`, `filesystem mutation` | (none) | MEASURED (`io-matrix`) |
  | `PSP-DISPLAY-001` | row unchanged: cited from `docs/ARCHITECTURE.md#clocks` | `display-mask-duty` (still uncited) | re-run by campaign case `display-mask-duty`: records `display-mask-duty-33000us`, `display-mask-duty-66000us` PASS |
  | `PSP-DISPLAY-002` | `late-waitvblankstart-{2,6,10,14,20}eighths`, `late-waitvblank-{2,6,10,14,20}eighths`, `invblank-waitvblankstart`, `invblank-waitvblank`, `calibration` | (none) | MEASURED (`display-wait-late`) |
  | `PSP-DISPLAY-003` | `calibration`, `priority-control`, `priority-experiment` | (none) | MEASURED (`display-wait-priority`) |
  | `PSP-DISPLAY-004` | `vblank-window` | (none) | MEASURED (`display-vblank-window`) |
  | `PSP-FPU-001` | `fpu-boot-fcr31`, `fpu-cvt-rm0`, `fpu-cvt-rm1`, `fpu-cvt-rm2`, `fpu-cvt-rm3`, `fpu-ccast-trunc`, `fpu-cvt-s-w`, `fpu-flag-overflow`, `fpu-flag-div0`, `fpu-flag-invalid`, `fpu-flag-underflow`, `fpu-flag-inexact`, `fpu-ftz-contrast`, `fpu-signed-zero`, `fpu-nan-payload`, `fpu-done` | (none) | MEASURED (`fpu-vector`) |
  | `PSP-CACHE-001` | `cache-alias-init`, `cache-writeback-contrast`, `cache-inval-contrast`, `cache-wball`, `cache-done` | (none) | MEASURED (`cache-alias`) |
  | `PSP-AUDIO-001` | `audio-ch-reserve`, `audio-ch-query-before`, `audio-ch-output-blocking-0`, `audio-ch-output-blocking-1`, `audio-ch-query-after`, `audio-ch-release`, `audio-ch-query-released`, `audio-out2-reserve`, `audio-out2-query-before`, `audio-out2-output-blocking`, `audio-out2-query-after`, `audio-out2-release`, `audio-src-reserve`, `audio-done` | (none) | MEASURED (`audio-query`) |
  | `PSP-GE-001` | `ge-nan-vfpu-qnan`, `ge-nan-screen2d-qnan`, `ge-nan-clip3d-qnan`, `ge-nan-litnormal-qnan`, `ge-nan-vfpu-pinf`, `ge-nan-screen2d-pinf`, `ge-nan-clip3d-pinf`, `ge-nan-litnormal-pinf`, `ge-nan-vfpu-ninf`, `ge-nan-screen2d-ninf`, `ge-nan-clip3d-ninf`, `ge-nan-litnormal-ninf`, `ge-nan-vfpu-nzero`, `ge-nan-screen2d-nzero`, `ge-nan-clip3d-nzero`, `ge-nan-litnormal-nzero`, `ge-nan-vfpu-denorm`, `ge-nan-screen2d-denorm`, `ge-nan-clip3d-denorm`, `ge-nan-litnormal-denorm`, `ge-nan-done` | (none) | MEASURED (`ge-nan`) |
  | `PSP-DMAC-001` | row unchanged: apis `sceDmacMemcpy`, `sceDmacTryMemcpy` cited | the 43 concurrency, invalid-tail and size-matrix cells (unchanged) | campaign case `dma-cells`: 93 records PASS (`align-*` and `overlap-*` cells, recorded privately); the five `dma-invalid-tail-*` campaign cases emitted SKIP and are CAPTURED only |
  | `PSP-MUTEX-001` | `mutex-refer-unlocked`, `mutex-timeout-quanta`, `mutex-priority-inheritance` | `mutex-interrupt-context` | MEASURED (`mutex-refer-unlocked`, `mutex-timeout-quanta`, `mutex-priority-inheritance`) (not acceptance-eligible: `mutex-interrupt-context`) |
  | `PSP-HLE-KERNEL-STATUS-001` | (none) | `sys-status-size-0x1c`, `sys-status-size-0x20`, `sys-status-size-0x08`, `sys-status-size-0x00`, `fpl-create`, `fpl-refer-full-all-free`, `fpl-try-alloc-a`, `fpl-try-alloc-b`, `fpl-try-alloc-exhausted`, `fpl-refer-full-two-held`, `fpl-refer-size-0x08-two-held`, `fpl-refer-size-0x00-two-held`, `fpl-free-a`, `fpl-refer-full-one-free`, `fpl-free-b`, `fpl-delete`, `fpl-refer-full-after-delete` | CAPTURED only (single-case run `hle-kernel-status`, 2026-10-10; not acceptance-eligible: post-unload shell qualification and host0 round trip failed) |
  | `PSP-HLE-VTIMER-001` | (none) | `vtimer-create`, `vtimer-refer-created`, `vtimer-stop-not-started`, `vtimer-start`, `vtimer-time-running-early`, `vtimer-refer-running-early`, `vtimer-delay-20ms`, `vtimer-time-running-20ms`, `vtimer-stop`, `vtimer-time-stopped`, `vtimer-refer-stopped`, `vtimer-delay-20ms-stopped`, `vtimer-time-stopped-later`, `vtimer-start-restart`, `vtimer-refer-size-0x08`, `vtimer-delete-running`, `vtimer-refer-after-delete`, `vtimer-delete-again` | CAPTURED only (single-case run `hle-vtimer`, 2026-10-10; not acceptance-eligible: post-unload shell qualification and host0 round trip failed) |
  | `PSP-HLE-POWER-001` | (none) | `pll-clock-int`, `pll-clock-float`, `cpu-clock-int`, `cpu-clock-float`, `cpu-clock-alias`, `bus-clock-int`, `bus-clock-float`, `bus-clock-alias`, `cpu-clock-int-repeat` | CAPTURED only (single-case run `hle-power-clock`, 2026-10-10; not acceptance-eligible: post-unload shell qualification and host0 round trip failed) |
  | `PSP-HLE-HPRM-001` | (none) | `hprm-remote-first`, `hprm-headphone-first`, `hprm-microphone-first`, `hprm-remote-second`, `hprm-headphone-second`, `hprm-microphone-second` | CAPTURED only (single-case run `hle-hprm`, 2026-10-10; not acceptance-eligible: post-unload shell qualification and host0 round trip failed) |
  | `PSP-HLE-CTRL-LATCH-001` | (none) | `latch-read-initial`, `latch-peek-initial`, `latch-poll-window`, `latch-read-after-poll`, `latch-peek-after-poll`, `latch-read-clears-peek-keeps` | CAPTURED only (single-case run `hle-ctrl-latch`, 2026-10-10; not acceptance-eligible: post-unload shell qualification and host0 round trip failed) |
  | `PSP-HLE-SYSPARAM-001` | (none) | `int-id-2`, `int-id-3`, `int-id-4`, `int-id-5`, `int-id-6`, `int-id-7`, `int-id-8`, `int-id-9`, `int-unknown-0`, `int-unknown-10`, `int-unknown-64`, `string-id-1-len-0x80`, `string-id-1-len-0x04` | CAPTURED only (single-case run `hle-sysparam`, 2026-10-10; not acceptance-eligible: post-unload shell qualification and host0 round trip failed) |
  | `PSP-HLE-GE-EDRAM-001` | (none) | `edram-size`, `edram-addr`, `edram-width-query-initial`, `edram-width-set-512`, `edram-width-set-1024`, `edram-width-set-2048`, `edram-width-set-4096`, `edram-width-restore`, `edram-width-query-final` | CAPTURED only (single-case run `hle-ge-edram`, 2026-10-10; not acceptance-eligible: post-unload shell qualification and host0 round trip failed) |

## Pending PSP-3000 measurement campaign

The following seven source-owned cases are built for a future PSP-3000 / 6.61
session. Six of the seven were measured by the 2026-10-10 campaign on `f6ccfb33`
(index above); `ge-break-continue` was captured but not acceptance-eligible.
Their parsers check record order, scalar fields, and completion;
they do not encode expected PSP return values.

| Case | Evidence id | Hardware status | Contract being measured |
| --- | --- | --- | --- |
| `kernel-alarm` | `PSP-ALARM-001` | `MEASURED` (2026-10-10, `f6ccfb33`) | Alarm creation with a null handler and zero clock; alarm-table exhaustion capped at 1024 pending alarms; cancel after one-shot fire, repeat cancel, unknown UID; handler-return re-arm base; interrupt state and a bounded blocking call in an alarm handler. |
| `thread-scheduler` | `PSP-THREAD-003` | `MEASURED` (2026-10-10, `f6ccfb33`) | Suspend UID 0 and self; resume UID 0; invalid ready-queue priority; ready-thread ordering after rotation; a timed wait expiring while its thread is suspended. |
| `wait-outcomes` | `PSP-WAIT-001` | `MEASURED` (2026-10-10, `f6ccfb33`) | Semaphore and event-flag signal/cancel before the timeout deadline, followed by dispatch after that deadline. |
| `ge-break-continue` | `PSP-GE-CONTROL-001` | `CAPTURED` (2026-10-10, `f6ccfb33`; not acceptance-eligible) | `sceGeBreak`/`sceGeContinue` return values without active/paused lists, invalid break mode, list/draw sync states for paused/cancelled lists, and a bounded continue drain and queue quiesce (`TIMEOUT` instead of a hang). |
| `refer-status-size` | `PSP-KERNEL-STATUS-001` | `MEASURED` (2026-10-10, `f6ccfb33`) | Bytes written and size-word results for `ReferSemaStatus`, `ReferEventFlagStatus`, and `ReferMbxStatus` at size 0, 8, 40, and full size. |
| `registry-readonly` | `PSP-REGISTRY-001` | `MEASURED` (2026-10-10, `f6ccfb33`) | Read-only root/category opening and `/CONFIG` category/key enumeration, metadata, selected modeled settings, error returns, handle exhaustion capped at 256 opens, and the forged-handle call last. |
| `kernel-misc` | `PSP-KERNEL-MISC-001` | `MEASURED` (2026-10-10, `f6ccfb33`) | Wide clock conversion, controller default mode, profiler-pointer returns, basic VTimer behavior, display return values, battery-icon status, and UMD-popup returns. |

The registry probe opens the registry and categories in read mode and never
calls a write, create, remove, or flush API. Values are emitted only for keys
the runtime models (`language`, `button_assign`, date/time format, `timezone`,
`summer_time`, and `adhoc_channel`); other keys emit name, type, and size only.
The resulting registry and console-specific files remain in the private
campaign output directory.

The campaign queue is ordered in `tools/psp_oracle/run_psplink.py`. It starts
with the host0 `transport-write` preflight, then the seven cases above, followed
by the remaining README `NOT_RUN` cases: smoke, thread exit/delete, teardown,
I/O matrix, display mask duty, display waits, FPU, cache alias, audio, GE
non-finite, DMA cells, five invalid-tail cases, and four mutex cases. The
already measured `delay-zero` case is excluded. The runner launches one PRX per
boot, performs a PSPLink soft reset between completed cases, validates the
whole private plan in offline dry-run mode, and checkpoints each case. A timed
out or incomplete launch stops the queue. After the maintainer power-cycles
and confirms that action, the runner resumes at the next case without
reclassifying the interrupted capture. A stop before a launch, such as a
transport start failure, keeps the checkpoint at the same case and needs no
power-cycle confirmation.

## 1. The gap this closes

With no pair of strict v2 hardware inputs, `tools/verify_gates.py` reports:

```text
NOT_RUN: set both PSP_HARDWARE_TRACE and LOCAL_COSIM_TRACE
```

The codegen and microtest comparisons still accept legacy v1 traces, and the verifier reports
those as `CORROBORATIVE_ONLY`. A v2 `PPSSPP_CORROBORATIVE` trace is also corroborative only.
Neither can satisfy the hardware gate: that gate requires a matching `PSP_HARDWARE` and
`LOCAL_COSIM` pair, validated by `strict_hardware_diff`. Agreement with another reimplementation
remains corroboration, not hardware evidence.

A PSP running custom firmware executes on real Allegrex silicon and is the only ground truth
available in the absence of documentation.

## 2. Integration surface that already exists

| Existing piece | Role |
| --- | --- |
| [`TRACE_FORMAT.md`](../tools/TRACE_FORMAT.md) | Versioned textual CPU-state-diff format |
| `tools/gen_microtest.py` | Emits CRT-free Allegrex test modules; takes `--groups` / `--opcodes`; seeded and deterministic |
| `tools/microtest_gate.py` | Compares `src/ref` against an oracle trace, truncated at the first syscall so everything compared is pure CPU |
| `tools/codegen_gate.py` | Same shape, generated code vs oracle |
| `tools/verify_gates.py` | Reports trace tiers and runs the strict hardware gate; absent hardware inputs are `NOT_RUN` |
| `src/rt/vfpu_interp.c`, `vfpu_fuzz.c` | Existing differential harness over pinned tables |

The legacy v1 trace header is:

```text
# psp-recomp trace v1 target=<name> oracle=<ppsspp|interp|recomp> start_pc=<hpc> steps=<N>
```

Strict v2 adds the `source_tier` field (`PSP_HARDWARE`, `LOCAL_COSIM`, or
`PPSSPP_CORROBORATIVE`) and the identity fields described in
[`TRACE_FORMAT.md`](../tools/TRACE_FORMAT.md). The producer that emits
`PSP_HARDWARE` traces on the PSP is in the works as a later hardware slice under issue #312.

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

### Probe teardown for repeated launches

A probe can leave child threads asleep or runnable after its records finish. Those
threads share PSPLink's kernel namespace with the next probe: names can collide,
priorities can change scheduling, and thread stacks consume partition memory. The
probe's main thread also remains parked until its module is stopped.

Do not end the probe's main thread with `sceKernelExitDeleteThread(0)`. That bypasses
the PSPSDK CRT exit path. The observed result was a broken PSPLink
`modstun` handshake (`Module Stop/Unload 0x00000000/` was not reported), which
escalated cleanup. The exact kernel transition is not established.

Stopping and unloading a module does not end the threads it created. PSPSDK's PRX
CRT (`crt0_prx`) creates main in `module_start` and exports no `module_stop`. Its
only exit path, `exit()` to `_exit()`, runs `_fini` and `__libcglue_deinit` and then
calls `sceKernelExitGame()`. PSPLink hooks that call: with `resetonexit=1` (its
default) it resets, and otherwise it exits the calling thread without deleting it.
PSPLink's `modstun` calls `sceKernelStopModule` and then `sceKernelUnloadModule`,
and nothing in that sequence ends main. On 2026-10-08 a PSP-3000 run showed the
result. `transport-write` passed its records and `modstun` unloaded the module, but
S2 still listed one more thread than S0 (the parked main), and about 52.5 MB of
the probe's default newlib heap stayed allocated.

The probe therefore owns the stop half of its lifecycle. Main records its thread
UID first. After the completion marker it sleeps until a stop request arrives;
a wakeup sent before the sleep is counted, not lost. The probe's exported
`module_stop` sets the request, wakes main, waits up to one second for main to end,
deletes it, and returns 0. On the request, main runs the CRT runtime
de-initialisation (`_fini`, then `__libcglue_deinit`, which frees the newlib heap
and the C runtime's kernel objects) without the `sceKernelExitGame` tail, then
calls `sceKernelExitThread(0)`. If main does not end within the bound,
`module_stop` returns 1 rather than remove a thread that may still be running; the
module stays loaded and the unload and snapshot checks below report the failure.
The Makefile generates the PRX export table (`module_start`, `module_stop`,
`module_info`) into the build directory for every `probe.c` case. Releasing the heap
at stop is the general fix for repeated launches, so the default heap size is kept.
A smaller global bound would change the memory environment that existing cases
measure; only the DMAC cases bound it, for their own measurement reasons.

The probe now runs one ordered teardown after its case records: write back the data
cache; terminate, delete, and verify its created child threads; delete tracked kernel
objects and registered sub-interrupts; release audio channels and close descriptors;
free partition blocks; restore CPU/bus clocks, captured FCR31, and any pending
interrupt-resume tokens; remove tracked disposable `host0:` files; write and read a
64-byte `host0:` round-trip file for the host; append the completion marker last; and
park main in `sceKernelSleepThread()` until `module_stop` ends it. The case result log
and round-trip file remain available until the host has captured and checked them.

`tools/psp_oracle/run_psplink.py` compares three PSPLink snapshots around each launch.
It uses `modlist` for the full loaded-module inventory; `modinfo <uid>` is the
single-module query used for the unload handshake. S0 records threads,
per-partition total/largest free bytes, and modules before load.
S1 records them after the completion marker and before unload; `modinfo <uid> t`
must show that the probe's only remaining thread is its main thread. Right after
the `modstun` stop/unload handshake the runner settles the link before any
post-unload PSPLink command: it issues `ver` through the same bounded shell
verifier as qualification (at most three attempts within
`--shell-verification-timeout`) until PSPLink answers. On 2026-10-10 seven
single-case runs captured complete data and confirmed the unload, then failed the
post-unload shell qualification and host0 round-trip the way `ver` fails right
after USBHostFS connects. The teardown report records `settle_status` (`PASS`,
`EXHAUSTED`, or `NOT_RUN` when no `modstun` was issued or the session had already
stopped) and `settle_attempts` (the number of settle `ver` commands); recovery
events carry each attempt with a `post-unload settle:` prefix. The settle never
decides the verdict: an exhausted settle adds no issue, and S2, the shell
qualification, `exprint`, and the round-trip then run in their usual order and
classify the case as they would without it. After the handshake and the settle,
S2 must match S0 for thread UID/name pairs and module UID/name pairs, with the
probe UID absent. The verdict lists the threads S2
gained or lost relative to S0. When the only extra threads are the module's own S1
threads, it also names the boundary: the probe main thread survived the module stop
and unload. The verdict also keeps PSPLink's `modstun` reply in `modstun_reply`.
Its `Status` field is the probe's `module_stop` return value: 0 only after main
ended and was deleted. Per-partition free-memory changes
are retained as diagnostics; allocator equality is not a teardown gate. The runner
also requires a qualified shell and the host0 round-trip. `exprint` output remains
diagnostic because its interpretation is not qualified, so its status is `NOT_RUN`.
Missing snapshots, incomplete host0 capture, or a failed probe block recovery. Only a
completed probe with passing case records and a confirmed teardown failure may enter
the L0/L1/L2 recovery ladder. The ladder allows one PSP reset per runner campaign/session,
shared across every case in that launch: post-restart qualification, unload,
host-stack restart, or L1 transport re-attach failure may proceed to the
bounded L2 reset and transport re-attach when PSPLink shell qualification still
answers. If qualification is lost or the L1 re-attach budget is exhausted, the
runner withholds the reset command and stops at L4. An L2 re-attach failure
stops at L4.
**Repeated-launch PSPLink
teardown hardware acceptance** remains `NOT_RUN`; source and unit-test success do not
establish that a qualified console passes these checks.

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

1. `tools/TRACE_FORMAT.md` — strict v2 tier and identity envelope implemented; PSP-side trace
   production remains unbuilt.
2. `tools/hwtest_gate.py` — **new**, result-vector comparison.
3. `fixtures/psp_runner/` — PSPSDK sources for the resident runner, excluded from the native
   build. Tracked; the `hw-verify` target and `tools/hwtest_gate.py` below are still unbuilt.
4. ~~`tools/hw_doctor.py` — **new**, the machine-checkable precondition check.~~ Superseded by the
   shipped `tools/psp_readiness.py`; add the live `host0:` round-trip check there.
5. `Makefile` — a `hw-verify` target reporting BLOCKED when no device is attached.
6. `tools/verify_gates.py` — strict v2 `PSP_HARDWARE` + `LOCAL_COSIM` consumer implemented;
   missing inputs report `NOT_RUN`.
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
