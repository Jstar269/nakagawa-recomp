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
  waited and returned 0 in 64/64 trials. Re-measured on 2026-10-23 on a
  PSP-3000 / 6.6.1 / PSPLink v3.2.1 (probe `c2632954`, historical issue #303):
  the same 64/64 results for all three combinations, a 1 MiB VRAM-to-VRAM
  first call lasting 4.43 to 4.55 ms for both APIs, a BUSY second `Try` returning
  in 14 to 90 us, a blocking second call returning 0 after 8.73 to 8.88 ms, and
  the full 1 MiB destination intact in every trial. Invalid-tail
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
- **Non-finite vertex position and lit colour in the GE** (issue #69; scalar words
  measured, rasterization not): what the PSP GE rasterizes for a vertex whose clip position, projected
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
  and lit-normal triangles. It ran on a PSP-3000 on 2026-10-10 (`f6ccfb33`) and
  again on 2026-10-15 (`f2d6c91a`) with identical records, and the measured
  index publishes its words. It does not repeat the already measured VASIN
  edge words, and its framebuffer hashes have not established a hardware
  rasterization rule; this GE boundary remains in the works under #69. The out-of-domain arc-sine argument that can produce a
  non-finite bone matrix remains a separate cell.
- **Audio query and blocking-output semantics** (issue #311; `CASE=audio-query`):
  MEASURED in the index row above (2026-10-10, PSP-3000, 6.61 / ARK-5.1.0). The
  probe captures both channel rest-length spellings, two bounded channel output
  blocks, one silent Output2 block, rest-sample queries, raw returns, and elapsed
  microseconds, with no acoustic claims. The probe drains Output2 before its
  release, so that index row does not show the busy return. The Output2 release
  rule comes from the 2026-10-01 comment on issue #311 (PSP-3001, firmware 6.6.1,
  PSPLink 3.2.1; raw records private): releasing while samples are queued returns
  `0x80268002` and leaves Output2 reserved, a following `sceAudioSRCChReserve`
  also fails with `0x80268002`, draining `sceAudioOutput2GetRestSample` to 0
  first lets the release succeed, and `sceAudioChRelease` succeeds right after
  blocking output. `h_AudioOutput2Release` in `src/rt/hle.c` implements the
  release rule. The `sceAudioSRC*` handlers (`h_AudioSRCChReserve`, `h_AudioSRCChRelease` and
  `h_AudioSRCOutputBlocking`, #339) share its slot and implement the 2026-10-15 SRC measurements.
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
  | `PSP-IO-001` | cells `io-open-create`, `io-write`, `io-read-verify`, `io-lseek`, `io-append`, `io-errors`, `io-done`; APIs `sceIoOpen`, `sceIoWrite`, `sceIoRead`, `sceIoLseek`, `sceIoClose`, `sceIoRemove` | (none) | MEASURED (`io-matrix`; 7 records PASS). It does not measure `sceIoIoctl`, `sceIoRename`, async I/O, or devctl semantics. |
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

- **PSP-3000 CPU-semantics runs on commit `e45e667b` (2026-10-15)**: three campaign runs on
  the qualified PSP-3000 / 6.61 / PSPLink 3.2.1 route, each the host0 `transport-write`
  preflight followed by one case (`fpu-cc` at 00:49Z, `vfpu-compare` at 00:59Z, `llsc-link`
  at 01:17Z), all built from `e45e667b`, the commit that adds the `fpu-cc` probe (it is in
  this branch's history). Every envelope is acceptance-eligible: evidence class
  `PSP_HARDWARE`, source commit binding `MATCH` on a clean committed tree, teardown check
  `PASS`, every record `PASS`. Device identity is `PARTIAL` (an operator-recorded model label,
  no serial or MAC) and the PRX digest is the host-side `PLACEHOLDER` binding described under
  "Device build identity" in the [fixture runbook](../fixtures/psp_oracle/README.md). Raw
  envelopes stay in the private campaign directory. The records hold only probe-defined words,
  so those words are published here. Each row cites exactly the cells it names.

  | Evidence id | Cells cited | Not cited | Outcome (campaign case) |
  | --- | --- | --- | --- |
  | `PSP-TRANSPORT-001` | `host0-write-readback` | (none) | re-run three times as the preflight; same words as the 2026-10-10 citation (`transport-write`) |
  | `PSP-FPU-CC-001` | `fpu-cc-fcr31-bits`, `fpu-cc0-ceq`, `fpu-cc0-branch`, `fpu-cc1-ceq`, `fpu-cc1-branch`, `fpu-cc2-ceq`, `fpu-cc2-branch`, `fpu-cc3-ceq`, `fpu-cc3-branch`, `fpu-cc4-ceq`, `fpu-cc4-branch`, `fpu-cc5-ceq`, `fpu-cc5-branch`, `fpu-cc6-ceq`, `fpu-cc6-branch`, `fpu-cc7-ceq`, `fpu-cc7-branch`, `fpu-cc-done` | (none) | MEASURED (`fpu-cc`) |
  | `PSP-VFPU-CMP-001` | `vscmp-lt`, `vscmp-eq`, `vscmp-gt`, `vscmp-zero-pos-neg`, `vscmp-zero-neg-pos`, `vscmp-nan-left-quiet`, `vscmp-nan-right-quiet`, `vscmp-nan-left-signal`, `vscmp-nan-right-signal`, `vscmp-nan-left-negative`, `vscmp-inf-pos-neg`, `vscmp-inf-neg-pos`, `vscmp-inf-pos-pos`, `vscmp-inf-neg-neg`, `vscmp-inf-pos-finite`, `vsge-lt`, `vsge-eq`, `vsge-gt`, `vsge-zero-pos-neg`, `vsge-zero-neg-pos`, `vsge-nan-left-quiet`, `vsge-nan-right-quiet`, `vsge-nan-left-signal`, `vsge-nan-right-signal`, `vsge-nan-left-negative`, `vsge-inf-pos-neg`, `vsge-inf-neg-pos`, `vsge-inf-pos-pos`, `vsge-inf-neg-neg`, `vsge-inf-pos-finite`, `vslt-lt`, `vslt-eq`, `vslt-gt`, `vslt-zero-pos-neg`, `vslt-zero-neg-pos`, `vslt-nan-left-quiet`, `vslt-nan-right-quiet`, `vslt-nan-left-signal`, `vslt-nan-right-signal`, `vslt-nan-left-negative`, `vslt-inf-pos-neg`, `vslt-inf-neg-pos`, `vslt-inf-pos-pos`, `vslt-inf-neg-neg`, `vslt-inf-pos-finite`, `vfpu-compare-done` | (none) | MEASURED (`vfpu-compare`) |
  | `PSP-LLSC-001` | `llsc-control`, `llsc-second-sc`, `llsc-other-address`, `llsc-link-done` | (none) | MEASURED (`llsc-link`) |

  What the cells establish, for these exact words only:
  - *COP1 condition codes (`PSP-FPU-CC-001`).* The FPU implements FCC0 only. A `ctc1` of
    `0xFE800000` (FCC0 to FCC7 under MIPS IV) reads back `0x00800000`, so FCR31 bits 25..31
    are not stored. For each condition code 0..7, `c.eq.s` on equal operands from FCR31 = 0
    leaves `0x00800000`, and on unequal operands from `0xFE800000` leaves `0x00000000`. `bc1t`
    with each code is taken under FCC0 and not taken with no flag, `bc1f` the reverse, and
    for codes 1..7 `bc1t` is not taken after a `ctc1` of that code's own MIPS IV bit, because
    the bit was dropped. The probe wrote no other FCR31 bit, so nothing is claimed about bit
    24 (FS) or the cause, enable and flag fields. The project already translated codes 1..7
    as FCC0 in `tools/codegen.py` and `src/ref/interp.cpp`; refusing a nonzero code, as
    discussed for PR #934, would have rejected encodings the console executes. `ctc1` stored
    the whole guest word, so `cfc1` returned bits 25..31; it now drops them
    (`SR_FCR31_CTC1_UNWRITABLE` in `src/rt/fp_convert.h`).
  - *VFPU set-compares (`PSP-VFPU-CMP-001`, issue #326).* Raw result words for
    `vscmp.s`/`vsge.s`/`vslt.s S002, S000, S001`:

    | Pair (S000, S001) | `vscmp.s` | `vsge.s` | `vslt.s` |
    | --- | --- | --- | --- |
    | `lt` (`0x3FC00000`, `0x40200000`) | `0xBF800000` | `0x00000000` | `0x3F800000` |
    | `eq` (`0x3FC00000`, `0x3FC00000`) | `0x00000000` | `0x3F800000` | `0x00000000` |
    | `gt` (`0x40200000`, `0x3FC00000`) | `0x3F800000` | `0x3F800000` | `0x00000000` |
    | `zero-pos-neg` (`0x00000000`, `0x80000000`) | `0x00000000` | `0x3F800000` | `0x00000000` |
    | `zero-neg-pos` (`0x80000000`, `0x00000000`) | `0x00000000` | `0x3F800000` | `0x00000000` |
    | `nan-left-quiet` (`0x7FC00000`, `0x3FC00000`) | `0x3F800000` | `0x00000000` | `0x00000000` |
    | `nan-right-quiet` (`0x3FC00000`, `0x7FC00000`) | `0xBF800000` | `0x00000000` | `0x00000000` |
    | `nan-left-signal` (`0x7F800001`, `0x3FC00000`) | `0x3F800000` | `0x00000000` | `0x00000000` |
    | `nan-right-signal` (`0x3FC00000`, `0x7F800001`) | `0xBF800000` | `0x00000000` | `0x00000000` |
    | `nan-left-negative` (`0xFFC00000`, `0x3FC00000`) | `0xBF800000` | `0x00000000` | `0x00000000` |
    | `inf-pos-neg` (`0x7F800000`, `0xFF800000`) | `0x3F800000` | `0x3F800000` | `0x00000000` |
    | `inf-neg-pos` (`0xFF800000`, `0x7F800000`) | `0xBF800000` | `0x00000000` | `0x3F800000` |
    | `inf-pos-pos` (`0x7F800000`, `0x7F800000`) | `0x00000000` | `0x3F800000` | `0x00000000` |
    | `inf-neg-neg` (`0xFF800000`, `0xFF800000`) | `0x00000000` | `0x3F800000` | `0x00000000` |
    | `inf-pos-finite` (`0x7F800000`, `0x3FC00000`) | `0x3F800000` | `0x3F800000` | `0x00000000` |

    Signed zeros compare equal and infinities order normally. `vsge.s` and `vslt.s` treat a
    NaN as unordered and write 0.0. `vscmp.s` does not: it orders a NaN by sign and magnitude,
    so a NaN with a clear sign bit is above 1.5 and one with a set sign bit below, quiet or
    signalling alike. Forty words matched the project's model; the five `vscmp` NaN words were
    0.0 in both the generated code and `sr_vfpu_interp` and are now the measured words.
    A pair of NaNs, a NaN against an infinity and a negative NaN on the right are not
    measured; the runtime extends the same sign-and-magnitude order to them.
  - *LL/SC link windows (`PSP-LLSC-001`).* With `A = 0x11111111` and `B = 0x44444444`
    before each window: `ll A; sc A` loads `0x11111111`, reports 1 and stores; `ll A; sc A; sc
    A` reports 1 for both `sc` and leaves the second value (`0x33333333`) at A, so a
    successful `sc` does not clear the link; `ll A; sc B` reports 1 and stores
    `0x55555555` at B with A unchanged, so `sc` does not compare its address with the `ll`
    address. Interrupts were suspended around each window. All three match the runtime's model
    (`docs/ARCHITECTURE.md`, "LL/SC link state"); nothing was changed.

- **PSP-3000 single-case runs on public commit `e45e667b` (2026-10-15)**: firmware 6.6.1, PSPLink 3.2.1, operator-declared model PSP-3000. Each case launched one PRX on a fresh boot after the `transport-write` preflight, and the preflight passed on every launch (PSP-TRANSPORT-001 is not re-cited). A row cites only the cells named in it, and cells not named here are not measured by this entry. This entry supersedes the 2026-10-10 single-case HLE captures for the cells it names. Raw envelopes, host0 logs, user-configuration values and console identifiers stay in the private campaign directory.

  | Evidence id | Cells cited | Not cited | Outcome (2026-10-15) |
  | --- | --- | --- | --- |
  | `PSP-SYSTEM-001` | `model-profile`: raw `kuKernelGetModel` ordinal 3, `sceKernelDevkitVersion` word 0x06060110, CPU clock 222 MHz (APIs `kuKernelGetModel`, `sceKernelDevkitVersion`, `scePowerGetCpuClockFrequencyInt`) | The physical model label: operator-declared, and the ordinal's PSP-3000 reading under the PSPSDK ordinal rule is recorded as agreement, not as a device reading (#356) | MEASURED: 1 record, PASS |
  | `PSP-KERNEL-001` | `mbx-delete-wait`: `DeleteMbx` with one waiter returned 0. The waiter's `ReceiveMbx` returned 0x800201B5 (WAIT_DELETE) with its message pointer unchanged, rewrote its timeout word to the remaining 499950 us of 500000 us, and then reached the dormant state. `ReferMbxStatus` on the deleted UID returned 0x8002019B. `mbx-timeout-control`: `ReceiveMbx` with a 50000 us timeout returned 0x800201A8 with remaining time 0 (APIs `sceKernelCreateMbx`, `sceKernelDeleteMbx`, `sceKernelReceiveMbx`, `sceKernelReferMbxStatus`) | `callback-notify-check`, `wait-cancel`, `thread-lifecycle` (not cited by this entry); the thread-delete follow-up stream | MEASURED: 3 records, PASS. PASS gates the probe's own integrity checks only |
  | `PSP-HLE-SYSPARAM-001` | `int-id-2`, `int-id-3`, `int-id-4`, `int-id-5`, `int-id-6`, `int-id-7`, `int-id-8`, `int-id-9`, `int-unknown-0`, `int-unknown-10`, `int-unknown-64`, `string-id-1-len-0x80`, `string-id-1-len-0x04`, `hle-sysparam-done` (APIs `sceUtilityGetSystemParamInt`, `sceUtilityGetSystemParamString`). Ids 2 to 10 return 0 and write a value. Ids 0 and 64 return 0x80110103 and leave the output untouched. `string-id-1-len-0x80` returns 0 with a NUL-terminated string inside the span. `string-id-1-len-0x04` returns 0x80110102 and writes nothing | The integer values and the string length and extent fields (user configuration; the nickname bytes stay in a private host0 file) | MEASURED: 14 records, PASS |
  | `PSP-HLE-CTRL-LATCH-001` | `latch-read-initial`, `latch-peek-initial`, `latch-poll-window`, `latch-read-after-poll`, `latch-peek-after-poll`, `latch-read-clears-peek-keeps`, `hle-ctrl-latch-done` (APIs `sceCtrlReadLatch`, `sceCtrlPeekLatch`). With no input, make, break and press read 0 and release reads 0xFFFFFFFF, and all 200 bounded polls read a non-zero latch | Read-versus-peek clearing semantics: the release field stays saturated, so the semantics record cannot tell clearing from keeping | MEASURED: 7 records, PASS |
  | `PSP-HLE-KERNEL-STATUS-001` | `sys-status-size-0x1c`, `sys-status-size-0x20`, `sys-status-size-0x08`, `sys-status-size-0x00`, `fpl-create`, `fpl-refer-full-all-free`, `fpl-try-alloc-a`, `fpl-try-alloc-b`, `fpl-try-alloc-exhausted`, `fpl-refer-full-two-held`, `fpl-refer-size-0x08-two-held`, `fpl-refer-size-0x00-two-held`, `fpl-free-a`, `fpl-refer-full-one-free`, `fpl-free-b`, `fpl-delete`, `fpl-refer-full-after-delete`, `hle-kernel-status-done` (APIs `sceKernelReferSystemStatus`, `sceKernelCreateFpl`, `sceKernelTryAllocateFpl`, `sceKernelFreeFpl`, `sceKernelDeleteFpl`, `sceKernelReferFplStatus`). `ReferSystemStatus` returns 0 and writes min(size, 0x1c) bytes. For each non-zero size (0x08, 0x1c and 0x20) the size word reads back 0x1c, and size 0 writes nothing. An FPL refer writes exactly the requested bytes of its 0x38-byte block (block size 0x40, two blocks, free count 2, then 0 with both held and 1 after one free). An exhausted allocate returns 0x80020190, and a refer after delete returns 0x8002019D and writes nothing | Time-dependent status words and tick counters | MEASURED: 18 records, PASS |
  | `PSP-HLE-POWER-001` | `pll-clock-int`, `pll-clock-float`, `cpu-clock-int`, `cpu-clock-float`, `cpu-clock-alias`, `bus-clock-int`, `bus-clock-float`, `bus-clock-alias`, `cpu-clock-int-repeat`, `hle-power-clock-done` (APIs `scePowerGetPllClockFrequencyInt`, `scePowerGetPllClockFrequencyFloat`, `scePowerGetCpuClockFrequency`, `scePowerGetCpuClockFrequencyInt`, `scePowerGetCpuClockFrequencyFloat`, `scePowerGetBusClockFrequency`, `scePowerGetBusClockFrequencyInt`, `scePowerGetBusClockFrequencyFloat`). PLL 222 MHz, CPU 222 MHz, bus 111 MHz. The Float getters return the float bit patterns 0x435E0000 (222.0) and 0x42DE0000 (111.0) | No `Set` call is made | MEASURED: 10 records, PASS |
  | `PSP-HLE-HPRM-001` | `hprm-remote-first`, `hprm-headphone-first`, `hprm-microphone-first`, `hprm-remote-second`, `hprm-headphone-second`, `hprm-microphone-second`, `hle-hprm-done` (APIs `sceHprmIsRemoteExist`, `sceHprmIsHeadphoneExist`, `sceHprmIsMicrophoneExist`). Remote 0, headphone 0, microphone 1, each read twice | The accessory state at run time, which the probe does not record | MEASURED: 7 records, PASS |
  | `PSP-HLE-GE-EDRAM-001` | `edram-size`, `edram-addr`, `edram-width-query-initial`, `edram-width-set-512`, `edram-width-set-1024`, `edram-width-set-2048`, `edram-width-set-4096`, `edram-width-restore`, `edram-width-query-final`, `hle-ge-edram-done` (APIs `sceGeEdramGetSize`, `sceGeEdramGetAddr`, `sceGeEdramSetAddrTranslation`). Size 0x00200000, address 0x04000000, boot width 1024. Each Set returns the width it replaced (0 after the initial query), and the next query returns the width just set. The restore to 1024 is verified | Nothing unrestored: the width is put back and verified before exit | MEASURED: 10 records, PASS |
  | `PSP-HLE-VTIMER-001` | `vtimer-create`, `vtimer-refer-created`, `vtimer-stop-not-started`, `vtimer-start`, `vtimer-time-running-early`, `vtimer-refer-running-early`, `vtimer-delay-20ms`, `vtimer-time-running-20ms`, `vtimer-stop`, `vtimer-time-stopped`, `vtimer-refer-stopped`, `vtimer-delay-20ms-stopped`, `vtimer-time-stopped-later`, `vtimer-start-restart`, `vtimer-refer-size-0x08`, `vtimer-delete-running`, `vtimer-refer-after-delete`, `vtimer-delete-again`, `hle-vtimer-done` (APIs `sceKernelCreateVTimer`, `sceKernelReferVTimerStatus`, `sceKernelStartVTimer`, `sceKernelStopVTimer`, `sceKernelGetVTimerTime`, `sceKernelDeleteVTimer`). Create returns a valid UID. StopVTimer returns 1 while running and 0 when not started. Start returns 0, also when it restarts a stopped timer. Deleting a running VTimer returns 0. Refer and delete on the deleted UID return 0x800201BE, and that refer writes nothing. Refer returns 0 and fills the 0x48-byte `SceKernelVTimerInfo`: size word 0x48, the create-time name, `active` (offset 0x24) 1 while running and 0 when created or stopped, and `base` (0x28) non-zero while running and 0 when created or stopped. `current` (0x30) is 0 when created, non-zero while running, and holds the accumulated time while stopped: the stopped refer and both stopped `GetVTimerTime` reads, before and after a 20 ms delay, return the same value. `schedule`, `handler` and `common` read 0. A refer with size word 0x08 writes 8 bytes, the size word 0x48 and the first name word, and leaves the rest untouched | Absolute time values and the time unit (the running deltas include the probe's own logging time) | MEASURED: 19 records, PASS |

- **Captured only, not acceptance-eligible (2026-10-15, `e45e667b`)**: these establish nothing about PSP behaviour.
  - `PSP-GE-CONTROL-001`, case `ge-break-continue`: 9 records PASS and 2 SKIP (`ge-list-sync-cancelled` and `ge-draw-sync-cancelled`). The SKIP records are the probe's own gate: `sceGeListDeQueue` on the stalled list returned 0x80000021 (BUSY), so the envelope is not eligible.
  - `PSP-MUTEX-001`, case `mutex-interrupt-context`: the header record PASS and 20 of 20 trial records FAIL. In every trial `sceKernelIsCpuIntrEnable` returned 1 where the probe expected 0. All six mutex calls returned 0x80020064 (ILLEGAL_CONTEXT).

- **PSP-3000 re-run of the revised `ge-break-continue` probe (2026-10-15, `c642039a`)**: firmware 6.6.1, PSPLink 3.2.1, operator-declared model PSP-3000. The case launched one PRX on a fresh boot after the `transport-write` preflight, which passed. The case envelope is acceptance-eligible with 0 blockers, so this entry supersedes the captured-only `ge-break-continue` runs above for the cells it names. The row cites only the cells named in it. The raw envelope and host0 log stay in the private campaign directory.

  | Evidence id | Cells cited | Not cited | Outcome (2026-10-15) |
  | --- | --- | --- | --- |
  | `PSP-GE-CONTROL-001` | APIs `sceGeBreak`, `sceGeContinue`, `sceGeListSync`, `sceGeDrawSync`, `sceGeListDeQueue`. `ge-break-no-active-list`: `sceGeBreak(0)` with nothing queued returns 0x80000020 and leaves the parameter buffer untouched. `ge-continue-no-paused-list`: `sceGeContinue` returns 0. `ge-break-invalid-mode`: `sceGeBreak(-1)` returns 0x80000107 and leaves the parameter buffer untouched. `ge-list-sync-paused` and `ge-draw-sync-paused`: a list enqueued with no stall address and broken at once by `sceGeBreak(0)` returns its queue id, then `sceGeListSync(qid, 1)` reads 4 and `sceGeDrawSync(1)` reads 2. `ge-continue-drain`: `sceGeContinue` returns 0 and the list drains. `ge-break-reset-no-dequeue`: `sceGeBreak(1)` with one list stalled at its start address returns 0. `ge-list-sync-cancelled`: `sceGeListSync(qid, 1)` on that list then returns 0x80000100. `ge-draw-sync-cancelled`: `sceGeDrawSync(1)` then reads 0. `ge-list-dequeue-stalled`: `sceGeListDeQueue` on a list stalled at its start returns 0x80000021 (BUSY), and `sceGeListSync(qid, 1)` and `sceGeDrawSync(1)` then read 3 | `ge-quiesce-after-continue`, `ge-quiesce-after-cancel`, `ge-quiesce-after-dequeue` and `ge-break-continue-done` (probe machinery). Not measured: breaking a stalled list, a reset with a paused list, break modes above 1, syncType 0, a dequeue in any other list state, an unknown queue id, and command-boundary timing | MEASURED: 13 records and the done record, PASS |

- **Recorded, not granted (2026-10-15, `e45e667b`)**: `display-mask-duty` (`PSP-DISPLAY-001`). Both records PASS, which is machinery only. Calibrated period 16,683,050 ns. For the 33000 us masks, VCOUNT advanced 65 over 58 masked windows against about 121 periods of wall time. For the 66000 us masks, VCOUNT advanced 34 over 30 windows against about 122 periods. The unmasked gaps account for the remainder, which fits one credit per masked window rather than catch-up. This cell is not granted through `PSP-DISPLAY-001`, because that row cites `docs/ARCHITECTURE.md#clocks`.

- **PSP-3000 re-run of the corrected `mutex-interrupt-context` probe (2026-10-15, `ec5dd563`)**: firmware 6.6.1, PSPLink 3.2.1, operator-declared model PSP-3000. The case launched one PRX on a fresh boot after the `transport-write` preflight, which passed. The case envelope is acceptance-eligible with 0 blockers, so this entry supersedes the captured-only `mutex-interrupt-context` runs above for the cells it names. The corrected pass rule records only that a trial completed; no returned value is compared. The raw envelope and host0 log stay in the private campaign directory.

  | Evidence id | Cells cited | Not cited | Outcome (2026-10-15) |
  | --- | --- | --- | --- |
  | `PSP-MUTEX-001` | `mutex-interrupt-context` (APIs `sceKernelLockMutex`, `sceKernelLockMutexCB`, `sceKernelTryLockMutex`, `sceKernelUnlockMutex`). In 20 of 20 trials run from a VBLANK sub-interrupt handler, `LockMutex` with an unknown UID, `LockMutex` with count 0, `LockMutex`, `LockMutexCB` and `TryLockMutex` on an unlocked mutex, and `UnlockMutex` of a mutex the main thread owns each returned 0x80020064 (ILLEGAL_CONTEXT) | The raw `sceKernelIsCpuIntrEnable` return inside the handler (context only); `UnlockMutex` with an unknown UID or a bad count from interrupt context | MEASURED: the header record and 20 trial records, PASS |

- **PSP-3000 single-case runs on public commit `b5d6fc5b` (2026-10-15)**: firmware 6.6.1, operator-declared model PSP-3000. The commit is the `origin/main` of that day and includes the five initial single-launch `hle-*` cases. Each case was one runner invocation with the `transport-write` preflight, which passed on every launch (PSP-TRANSPORT-001 is not re-cited). The first case ran on a fresh power cycle, and each later one after a PSPLink soft reset and re-qualification. A row cites only the cells named in it, and cells not named here are not measured by this entry. Raw envelopes and host0 logs stay in the private campaign directory.

  | Evidence id | Cells cited | Not cited | Outcome (2026-10-15) |
  | --- | --- | --- | --- |
  | `PSP-HLE-VTIMER-002` | `vtimer-unit-create`, `vtimer-unit-start`, `vtimer-unit-time-a`, `vtimer-unit-systime-a`, `vtimer-unit-delay`, `vtimer-unit-time-b`, `vtimer-unit-systime-b`, `vtimer-unit-base-running`, `vtimer-unit-refer-running`, `vtimer-unit-stop`, `vtimer-unit-base-stopped`, `vtimer-unit-refer-stopped`, `vtimer-unit-delete`, `hle-vtimer-unit-done` (APIs `sceKernelCreateVTimer`, `sceKernelStartVTimer`, `sceKernelGetVTimerTime`, `sceKernelGetSystemTimeWide`, `sceKernelGetVTimerBase`, `sceKernelReferVTimerStatus`, `sceKernelStopVTimer`, `sceKernelDeleteVTimer`). Create returns a UID, Start returns 0, `sceKernelDelayThread(20000)` returns 0, Stop of the running timer returns 1, and Delete of the stopped timer returns 0. While the timer runs, `GetVTimerBase` and `ReferVTimerStatus` return 0 and give the same 64-bit base, which has the system clock's high word. After Stop both return 0 and the base reads 0. `GetVTimerTime` reads 38,316 and 242,695 either side of the 20 ms delay (delta 204,379), while `sceKernelGetSystemTimeWide` advances 220,608 between its two reads. The probe's host0 record writes offset the two intervals, so the deltas agree only within about 8%. That is consistent with the system clock's microsecond unit, and it is not a precise rate measurement. At both reads, the base plus the VTimer time stays below the next system-clock read, which is consistent with time = system time - base and base = system time at Start | Absolute system-clock values; the system time at Start (no reading was taken there); the VTimer rate beyond the 8% agreement | MEASURED: 13 records and the done record, PASS |
  | `PSP-HLE-VTIMER-003` | `vtimer-err-create-stale`, `vtimer-err-delete-stale`, `vtimer-err-start-deleted`, `vtimer-err-stop-deleted`, `vtimer-err-time-deleted`, `vtimer-err-cleanup-stale`, `vtimer-err-create-live`, `vtimer-err-delete-live`, `vtimer-err-noslot-create`, `vtimer-err-noslot-cleanup`, `hle-vtimer-errors-done` (APIs `sceKernelCreateVTimer`, `sceKernelDeleteVTimer`, `sceKernelStartVTimer`, `sceKernelStopVTimer`, `sceKernelGetVTimerTime`). On a deleted UID, Start, Stop and `GetVTimerTime` with a valid out pointer each return 0x800201BE, and `GetVTimerTime` leaves both 0xA5A5A5A5 sentinel words untouched. A second Delete of that UID returns 0x800201BE. A live timer is created and deleted with 0. Sixty-four creates in a row each return a UID with no refusal (out0 64, out1 0), and the cleanup issues 64 deletes, the first returning 0 | The limit above 64 timers and its refusal code; the UID values | MEASURED: 10 records and the done record, PASS |
  | `PSP-HLE-POWER-002` | `pll-clock-float-v0`, `cpu-clock-float-v0`, `bus-clock-float-v0`, `hle-power-float-v0-done` (APIs `scePowerGetPllClockFrequencyFloat`, `scePowerGetCpuClockFrequencyFloat`, `scePowerGetBusClockFrequencyFloat`), one call each through a raw `jal` wrapper that presets `$f0` to 0xA5A5A5A5. `$f0` holds 0x435E0000 (222.0) after the PLL and CPU getters and 0x42DE0000 (111.0) after the bus getter. `$v0` holds 0x088043A0, 0x088043E0 and 0x08804420: neither 0 nor the clock, and a different user-memory-range word after each call | Whether a getter writes `$v0` or leaves the caller's value, because the wrapper does not preset `$v0`. The runtime does not model `$v0` for these getters | MEASURED: 3 records and the done record, PASS |

- **Captured only, not acceptance-eligible (2026-10-15, `b5d6fc5b`)**: these establish nothing about PSP behaviour.
  - `PSP-HLE-KERNEL-STATUS-002`, case `hle-kernel-pool-bounds`: the stream was complete, with 7 records PASS and 2 SKIP (`fpl-delete-size-0x00` and `fpl-delete-size-0x7fffffff`). Both FPL creates were refused, so the probe had no pool to delete and recorded those delete cells SKIP, and a SKIP record makes the envelope ineligible. The probe has since been revised: such a delete cell is now PASS with out0 0 and issues no delete, and the revised form has not run.
  - `PSP-HLE-NULL-OUT-001`, case `hle-null-out-pointers`: incomplete, not measured. `null-vtimer-create` was recorded PASS. The stream then stopped after the step marker `null-sys-status`, which the probe writes just before its first NULL call (`sceKernelReferSystemStatus(NULL)`), 0.9 s after launch. No record for that cell or any later one arrived, no completion arrived within the 240 s case timeout, the module was not proven unloaded, and the console needed a power cycle. The capture itself does not establish whether that call faulted, hung or returned. Maintainer retirement: the kernel pointer check rejects kernel-space addresses but not address 0, so `ReferSystemStatus(NULL)` writes through NULL and faults; the VTimer getters also write through their out pointers. No game can rely on these results. This case is retired and must never be scheduled.

- **PSP-3000 single-case runs on public commits `22b027f6` and `7fc1641e` (2026-10-15)**: firmware 6.6.1, PSPLink 3.2.1, operator-declared model PSP-3000. `22b027f6` is the pre-merge head of the change that added the `vfpu-transcendental-corpus`, `hle-timer-order` and `hle-audio-src` probes. The first two probes are unchanged in the merged commit `7fc1641e`, which also changed the `hle-audio-src` pass rule: its two final-cleanup cells became PASS records in place of SKIP records. That case was therefore re-run from `7fc1641e`. Each case was one runner invocation with the `transport-write` preflight, which passed on every launch (PSP-TRANSPORT-001 is not re-cited). A row cites only the cells named in it, and cells not named here are not measured by this entry. Raw envelopes and host0 logs stay in the private campaign directory.

  | Evidence id | Cells cited | Not cited | Outcome (2026-10-15) |
  | --- | --- | --- | --- |
  | `PSP-VFPU-TRANSCENDENTAL-001` | `vsin-chunk-00`, `vsin-chunk-01`, `vsin-chunk-02`, `vsin-chunk-03`, `vsin-chunk-04`, `vsin-chunk-05`, `vsin-chunk-06`, `vsin-chunk-07`, `vcos-chunk-00`, `vcos-chunk-01`, `vcos-chunk-02`, `vcos-chunk-03`, `vcos-chunk-04`, `vcos-chunk-05`, `vcos-chunk-06`, `vcos-chunk-07`, `vexp2-chunk-00`, `vexp2-chunk-01`, `vexp2-chunk-02`, `vexp2-chunk-03`, `vexp2-chunk-04`, `vexp2-chunk-05`, `vexp2-chunk-06`, `vexp2-chunk-07`, `vlog2-chunk-00`, `vlog2-chunk-01`, `vlog2-chunk-02`, `vlog2-chunk-03`, `vlog2-chunk-04`, `vlog2-chunk-05`, `vlog2-chunk-06`, `vlog2-chunk-07`, `vrsq-chunk-00`, `vrsq-chunk-01`, `vrsq-chunk-02`, `vrsq-chunk-03`, `vrsq-chunk-04`, `vrsq-chunk-05`, `vrsq-chunk-06`, `vrsq-chunk-07`, `vrcp-chunk-00`, `vrcp-chunk-01`, `vrcp-chunk-02`, `vrcp-chunk-03`, `vrcp-chunk-04`, `vrcp-chunk-05`, `vrcp-chunk-06`, `vrcp-chunk-07`, `vsqrt-chunk-00`, `vsqrt-chunk-01`, `vsqrt-chunk-02`, `vsqrt-chunk-03`, `vsqrt-chunk-04`, `vsqrt-chunk-05`, `vsqrt-chunk-06`, `vsqrt-chunk-07`, `vasin-chunk-00`, `vasin-chunk-01`, `vasin-chunk-02`, `vasin-chunk-03`, `vasin-chunk-04`, `vasin-chunk-05`, `vasin-chunk-06`, `vasin-chunk-07`, `vnrcp-chunk-00`, `vnrcp-chunk-01`, `vnrcp-chunk-02`, `vnrcp-chunk-03`, `vnrcp-chunk-04`, `vnrcp-chunk-05`, `vnrcp-chunk-06`, `vnrcp-chunk-07`, `vnsin-chunk-00`, `vnsin-chunk-01`, `vnsin-chunk-02`, `vnsin-chunk-03`, `vnsin-chunk-04`, `vnsin-chunk-05`, `vnsin-chunk-06`, `vnsin-chunk-07`, `vrexp2-chunk-00`, `vrexp2-chunk-01`, `vrexp2-chunk-02`, `vrexp2-chunk-03`, `vrexp2-chunk-04`, `vrexp2-chunk-05`, `vrexp2-chunk-06`, `vrexp2-chunk-07`, `vfpu-transcendental-corpus-done`: the raw single-lane result words of `vsin.s`, `vcos.s`, `vexp2.s`, `vlog2.s`, `vrsq.s`, `vrcp.s`, `vsqrt.s`, `vasin.s`, `vnrcp.s`, `vnsin.s` and `vrexp2.s` over 63 input words each | Anything the words do not show: the probe compares no value, and the PASS status records only that the instruction result was recorded | MEASURED: 88 data records (693 words) and the done record, PASS (`22b027f6`) |
  | `PSP-HLE-TIMER-ORDER-001` | `alarm-same-delay-set`, `alarm-same-delay-a`, `alarm-same-delay-b`, `alarm-long-then-short-set`, `alarm-long-event`, `alarm-short-event`, `timer-order-vtimer-create`, `timer-order-vtimer-handler`, `timer-order-vtimer-alarm-set`, `timer-order-vtimer-event`, `timer-order-vtimer-alarm-event`, `alarm-zero-set`, `alarm-zero-event`, `alarm-same-delay-cleanup`, `alarm-long-short-cleanup`, `timer-order-vtimer-cleanup`, `alarm-zero-cleanup`, `hle-timer-order-done` (APIs `sceKernelSetAlarm`, `sceKernelCancelAlarm`, `sceKernelCreateVTimer`, `sceKernelSetVTimerHandler`, `sceKernelStartVTimer`, `sceKernelStopVTimer`, `sceKernelDeleteVTimer`) | Absolute delays: no cell compares a due time with a clock; a tie-break at an equal due time (the pairs below were set with a host0 step record between the two calls); the UID values | MEASURED: 17 records and the done record, PASS (`22b027f6`) |
  | `PSP-HLE-AUDIO-SRC-001` | `src-release-unreserved`, `src-output-unreserved`, `src-reserve-valid`, `src-reserve-already-held`, `src-output-blocking-0`, `src-release-after-output`, `src-release-again`, `src-reserve-invalid-samples-zero`, `src-reserve-invalid-samples-65`, `src-reserve-invalid-samples-4096`, `src-reserve-invalid-frequency`, `src-reserve-invalid-channels-1`, `src-reserve-invalid-channels-4`, `out2-then-src-reserve`, `src-then-out2-reserve`, `src-final-cleanup`, `out2-final-cleanup`, `hle-audio-src-done` (APIs `sceAudioSRCChReserve`, `sceAudioSRCChRelease`, `sceAudioSRCOutputBlocking`, `sceAudioOutput2Reserve`, `sceAudioOutput2Release`) | The upper sample-count bound; frequencies other than 44100 and 48000; channel counts other than 1, 2 and 4; volume validation; releasing one reservation through the other API; the audible output (silence); an SRC rest-sample query (the public NID corpus has none) | MEASURED: 17 records and the done record, PASS (`7fc1641e`) |

  What the cells establish, for these exact words only:
  - *Alarm and VTimer callback order (`PSP-HLE-TIMER-ORDER-001`).* Each `SetAlarm` returned a UID. The callback sequence numbers and the system-time low words they stored were:
    - alarms set 50000 us ahead, first A then B: A ran first (sequence 1), B second (sequence 2), 17,125 us later;
    - a 100000 us alarm set before a 30000 us alarm: the 30000 us alarm ran first (sequence 3), the 100000 us alarm second (sequence 4), 53,561 us later;
    - a VTimer handler due 60000 us after `StartVTimer` and a 60000 us alarm set after it: the handler ran first (sequence 5), the alarm second (sequence 6), 13,579 us later (`SetVTimerHandler` and `StartVTimer` both returned 0);
    - `SetAlarm(0)`: a UID was returned and the handler had run (sequence 7) by the end of a 100000 us wait.

    A host0 step record sits between each pair of `SetAlarm` calls (and between `StartVTimer` and the alarm), so the nominally equal delays were not equal due times. The 17,125 us and 13,579 us gaps are of the order of that record's cost, and the 53,561 us gap is the 70,000 us difference of the two delays less about 16,400 us. The cells therefore show delivery in due-time order (the 30000 us alarm ran before the earlier-set 100000 us alarm). In the equal-delay pairs the callback that ran first was also the one set first and due first, so they do not show how two alarms with the same due time are ordered. After their handlers returned 0, `CancelAlarm` on each of the six alarm UIDs returned 0x8002019F, `StopVTimer` of the still-running timer returned 1, and `DeleteVTimer` returned 0.

    Runtime comparison (`src/rt/sched.c`, `src/rt/hle.c`): the alarm cells match. Due alarms run in
    deadline order, ties in set order (`scheduler_alarm_due_slot`), so the 30000 us alarm precedes
    the earlier-set 100000 us one and the first-set alarm precedes the second. `SetAlarm(0)` is due at
    the next interrupt-service point, and a one-shot whose handler returned 0 is released, so
    `CancelAlarm` on it returns `SCHED_ERROR_UNKNOWN_ALMID` (0x8002019F).
    `test_alarm_delivery_order_matches_the_timer_order_probe` pins these orders. The VTimer handler
    cells do not match: `sceKernelSetVTimerHandler` is refused by name with 0x80020002, because the
    handler family needs the timer interrupt model (issue #325), where the console returned 0 and ran the
    handler. Nothing in the runtime changed for it.
  - *Audio SRC channel (`PSP-HLE-AUDIO-SRC-001`, issue #339).* With nothing reserved, `sceAudioSRCChRelease()` and `sceAudioSRCOutputBlocking` (one silent 1024-sample stereo block at the maximum volume) each returned 0x80260008. `sceAudioSRCChReserve(1024, 44100, 2)` returned 0, an identical second reserve returned 0x80268002, and `sceAudioSRCOutputBlocking` then returned 0x400, the sample count. `sceAudioSRCChRelease()` returned 0 and a second release returned 0x80260008. Reserve shapes, with nothing reserved:

    | Shape (samples, Hz, channels) | Return |
    | --- | --- |
    | (0, 44100, 2) | 0x80000104 |
    | (65, 44100, 2) | 0 (accepted; released at once, 0) |
    | (4096, 44100, 2) | 0 (accepted; released at once, 0) |
    | (1024, 48000, 2) | 0 (accepted; released at once, 0) |
    | (1024, 44100, 1) | 0x80000104 |
    | (1024, 44100, 4) | 0x80000003 |

    `sceAudioOutput2Reserve(512)` then `sceAudioSRCChReserve(1024, 44100, 2)` returned 0 then 0x80268002, and the Output2 release returned 0. `sceAudioSRCChReserve` then `sceAudioOutput2Reserve(512)` returned 0 then 0x80268002, and the SRC release returned 0. The two final-cleanup cells record that nothing was left reserved (0xFFFFFFFF, no call).

    The runtime had no SRC handlers before this entry. `h_AudioSRCChReserve`, `h_AudioSRCChRelease` and
    `h_AudioSRCOutputBlocking` in `src/rt/hle.c` now return every word above, on the Output2 slot, and
    `test_audio_src_matches_the_measured_contract` in `src/rt/hle_thread_selftest.c` pins them. A reserve
    at 48000 Hz is accepted as measured. The host mixer runs at 44100 Hz, so those blocks pass through
    the band-limited converter in `src/rt/audio_resample.c` and keep their source-rate pacing. The
    values listed as not cited are unmeasured; the handler metadata names the choices made for them.
  - *Transcendental words (`PSP-VFPU-TRANSCENDENTAL-001`, issue #347).* The eleven scalar operations returned 693 result words over the probe's fixed inputs: 20 special encodings (signed zeros, the smallest signed denormals, plus and minus one, two and one half, the smallest signed normals, plus and minus 2^127, the infinities, and quiet and signalling NaN encodings) and 43 evenly spaced domain constants per operation. The raw words stay private. Compared with the runtime (the generated code and `sr_vfpu_interp`), ten operations matched on all 63 inputs: `vsin.s`, `vcos.s`, `vexp2.s`, `vlog2.s`, `vrsq.s`, `vrcp.s`, `vsqrt.s`, `vasin.s`, `vnrcp.s` and `vnsin.s`. `vrexp2.s` matched on none: the runtime computed the negated `exp2(x)` (`0xBF800000` for an input of zero) where the console returned `exp2(-x)` (`0x3F800000` for zero, `0x40000000` for -1.0 and `0x3EFFFFFC` for +1.0). Both paths now compute `exp2(-x)`, and the 63 measured words are pinned in `src/rt/vfpu_interp_selftest.c`. The same opcode group still decodes five more negated forms (optypes 25, 27, 29, 30 and 31) as the negation of the positive operation; no probe covers them, so they are unmeasured.

- **Captured only, not acceptance-eligible (2026-10-15, `22b027f6`)**: these establish nothing about PSP behaviour.
  - `PSP-HLE-AUDIO-SRC-001`, case `hle-audio-src`: the stream was complete, with 16 records PASS and 2 SKIP (`src-final-cleanup` and `out2-final-cleanup`). Both SKIP records were the probe's own gate for "nothing left to release", a pass-rule flaw that `7fc1641e` fixed, and a SKIP record makes the envelope ineligible. The 16 other records carry the same words as the eligible re-run above.

- **PSP-3000 requalification of the 32-case campaign queue on public commit `f2d6c91a` (2026-10-15)**: firmware 6.6.1, PSPLink 3.2.1, operator-declared model PSP-3000. Each case was one runner invocation with the `transport-write` preflight, which was acceptance-eligible on all 32 launches. All 32 cases launched and exited, the teardown check passed on all 32, every envelope binds the source commit (`MATCH`, clean committed tree), and 26 case envelopes are acceptance-eligible. Device identity is `PARTIAL` (an operator-declared model label) and the PRX digest is the host-side `PLACEHOLDER` binding described under "Device build identity" in the [fixture runbook](../fixtures/psp_oracle/README.md). This entry is a newer acceptance citation for the campaign cases below. It cites no new cell, and each row keeps the cells it already names. A record is "identical" when its status and every word match the 2026-10-10 run (`f6ccfb33`); in the cases whose probe did not change, the other records are time-stamped or carry UIDs, and their statuses agree. Raw envelopes and host0 logs stay in the private campaign directory.

  | Campaign case | Evidence id | Outcome (2026-10-15, acceptance-eligible) |
  | --- | --- | --- |
  | `audio-query` | `PSP-AUDIO-001` | 14 records PASS; 11 identical to 2026-10-10; the three blocking-output records differ only in the elapsed-microsecond word |
  | `cache-alias` | `PSP-CACHE-001` | 5 records PASS, all identical |
  | `display-mask-duty` | `PSP-DISPLAY-001` | 2 records PASS; clock and VBLANK-counter words differ |
  | `display-vblank-window` | `PSP-DISPLAY-004` | 1 record PASS; clock and VBLANK-counter words differ |
  | `display-wait-late` | `PSP-DISPLAY-002` | 13 records PASS; clock and VBLANK-counter words differ |
  | `display-wait-priority` | `PSP-DISPLAY-003` | 3 records PASS; clock and VBLANK-counter words differ |
  | `dma-cells` | `PSP-DMAC-001` | 93 records PASS, all identical |
  | `fpu-cc` | `PSP-FPU-CC-001` | first campaign-queue run of this case; not compared |
  | `fpu-vector` | `PSP-FPU-001` | 16 records PASS, all identical |
  | `ge-break-continue` | `PSP-GE-CONTROL-001` | 14 records PASS (the revised probe; the 2026-10-10 run was of an earlier revision) |
  | `ge-nan` | `PSP-GE-001` | 21 records PASS, all identical |
  | `io-matrix` | `PSP-IO-001` | 7 records PASS, all identical |
  | `kernel-alarm` | `PSP-ALARM-001` | 9 records PASS; 4 identical, the others differ in UID and clock words |
  | `kernel-misc` | `PSP-KERNEL-MISC-001` | 8 records PASS; 7 identical, `vtimer-basic` differs in clock words |
  | `llsc-link` | `PSP-LLSC-001` | first campaign-queue run of this case; not compared |
  | `mutex-interrupt-context` | `PSP-MUTEX-001` | 21 records PASS (the corrected pass rule; the 2026-10-10 run failed the probe's own assertions) |
  | `mutex-priority-inheritance` | `PSP-MUTEX-001` | 1 record PASS, identical |
  | `mutex-refer-unlocked` | `PSP-MUTEX-001` | 1 record PASS; two UID words differ |
  | `mutex-timeout-quanta` | `PSP-MUTEX-001` | 1 record PASS; timing words differ |
  | `refer-status-size` | `PSP-KERNEL-STATUS-001` | 13 records PASS, all identical |
  | `registry-readonly` | `PSP-REGISTRY-001` | 414 records PASS, all identical |
  | `smoke` | `PSP-SMOKE-001` | 1 record PASS, identical |
  | `thread-exit-delete` | `PSP-THREAD-EXIT-001` | 12 records PASS, all identical |
  | `thread-scheduler` | `PSP-THREAD-003` | 7 records PASS, all identical |
  | `vfpu-compare` | `PSP-VFPU-CMP-001` | first campaign-queue run of this case; not compared |
  | `wait-outcomes` | `PSP-WAIT-001` | 5 records PASS, all identical |

  Six cases ran to completion but were not acceptance-eligible, so they stay `CAPTURED` only and establish nothing:
  - `teardown-test` (`PSP-TEARDOWN-001`): `exitdelete-main` is a SKIP record, as on 2026-10-10.
  - `dma-invalid-tail-s0`, `dma-invalid-tail-memcpy-src`, `dma-invalid-tail-memcpy-dst`, `dma-invalid-tail-try-src` and `dma-invalid-tail-try-dst` (`PSP-DMAC-001`): every record is a SKIP, identical to the 2026-10-10 run.

  What the `ge-nan` cells establish (`PSP-GE-001`, issue #69), for these exact words only. All 21 records were identical in both runs. Each input word was loaded into `S000`, with `S001` set to `0.0` for `vadd.s` and `1.0` for `vmul.s`:

  | Input word | `vadd.s S002, S000, S001` (+0) | `vmul.s S002, S000, S001` (x1) |
  | --- | --- | --- |
  | `0x7FC00000` (qNaN) | `0x7F800001` | `0x7F800001` |
  | `0x7F800000` (+Inf) | `0x7F800000` | `0x7F800000` |
  | `0xFF800000` (-Inf) | `0xFF800000` | `0xFF800000` |
  | `0x80000000` (-0) | `0x00000000` | `0x80000000` |
  | `0x00000001` (minimum positive denormal) | `0x00000000` | `0x00000000` |

  A quiet NaN comes back as `0x7F800001`, and the denormal is flushed to zero. Negative zero plus zero is positive zero, and negative zero times one keeps its sign. Which of the two operands a NaN or a denormal flush applies to is not separable from these cells, and no other VFPU arithmetic form (`vsub`, `vdiv`, the pair, triple and quad widths) was measured.

  The draw cells put the same input word in one vertex of a triangle (the x of a 2D screen-coordinate vertex, the x of a 3D clip-position vertex, and a lit-normal vertex) and record a framebuffer hash, a changed-pixel count and the draw state after `sceGuFinish` and a bounded drain:

  | Input word | `screen2d` hash | `clip3d` hash | `litnormal` hash |
  | --- | --- | --- | --- |
  | `0x7FC00000` (qNaN) | `0x2B6647A7` | `0x9DBEB5C5` | `0x9DBEB5C5` |
  | `0x7F800000` (+Inf) | `0x2B6647A7` | `0x9DBEB5C5` | `0x9DBEB5C5` |
  | `0xFF800000` (-Inf) | `0xA1F002BD` | `0x9DBEB5C5` | `0x9DBEB5C5` |
  | `0x80000000` (-0) | `0xA929B835` | `0x9DBEB5C5` | `0x9DBEB5C5` |
  | `0x00000001` (denormal) | `0xA929B835` | `0x9DBEB5C5` | `0x9DBEB5C5` |

  In all fifteen draw records the changed-pixel count is `0x0001FBFE` (130,046 of the 480 x 272 = 130,560 scanned pixels) and the draw-state word is 0. These are probe observations. A hash is a function of the whole scanned frame: it shows that two draws left the same frame or different frames, and nothing about which pixels differ or why, and with almost every scanned pixel differing from the probe's background word the cells do not isolate the triangle either. No rasterization rule for a non-finite vertex follows from them, and that verdict stays NOT_MEASURED. The runtime's `vadd.s` and `vmul.s` (the generated code and `sr_vfpu_interp` alike) return the host IEEE result: the qNaN input stays `0x7FC00000` and the denormal stays `0x00000001`, where the console wrote `0x7F800001` and `0x00000000`. The other six words match. This entry does not change that.

### PSP-VRAM-MIRROR-001 measured run 2026-10-16

Three source-owned `vram-mirror` probe revisions ran on a PSP-3000 / 6.61 on
2026-10-16: `24c1472e`, `fd3efc64`, and `df41dba2`. VRAM restoration was verified
for each; the final `df41dba2` run was acceptance-eligible. The raw host0 logs
remain private; this entry publishes only derived rules and probe-defined inputs.

The full position-encoded fill had zero mismatches across 4,134 sampled words
per alias. Cached and uncached aliases agreed:

| Cached alias | Uncached alias | Physical VRAM offset for sampled word-aligned alias offset `o` |
| --- | --- | --- |
| `0x04000000` | `0x44000000` | `o` |
| `0x04200000` | `0x44200000` | `o ^ 0x2040` |
| `0x04400000` | `0x44400000` | `o` |
| `0x04600000` | `0x44600000` | bits 5..9 of `o` rotated left one (bit 9 to bit 5), then `^ 0x2040` |

The position samples use 32-bit words, so they establish the transform constant
and columns for offset bits 2..20. Address bits 0 and 1 are not independently
decoded by these samples.

Mirror writes landed at the same transformed offset; for example, a write to
`0x04200100` was found at physical offset `0x002140`.

The GE depth buffer was 16-bit with stride 512 and `zbp=0x0CC000`. After a
`0x5A5A` clear, the GE drew through-mode sprites with raw Z values: A at x
0..31/y 0..15 with Z `0x1111`, B at x 64..95/y 8..11 with Z `0x2222`, and C
at x 200..207/y 100 with Z `0x3333`. The reported depth address rule is
`(y * 1024 + 2x) ^ 0x2040`; the `0x04200000` view presents depth linearly and
colour remains linear. The clear leaves physical x 448..479 unwritten and
writes x 480..511 in each row. It remains unresolved whether the XOR applies
to the absolute VRAM offset or to the offset within the depth buffer, because
`zbp=0x0CC000` has bits 6 and 13 clear.

The host analyzer derives each cached/uncached alias's affine constant and
measured columns from the position samples, decodes the mirror-write scans, and
reports depth XOR candidates from the raw-Z rows. The flat sprite footprints
leave byte-phase candidates that the current samples do not distinguish; the
reported common mask is `0x2040`. The analyzer preserves that ambiguity rather
than selecting an unobserved value. No raw PSP words are stored in this document.

Probe APIs cited: `sceDisplayGetFrameBuf`, `sceGeEdramGetAddr`, `sceGeDrawSync`, `sceGuDepthBuffer`, `sceGuDrawArray`, `sceGuClearDepth`, `sceGuClear`, `sceGuEnable`, `sceGuDepthFunc`, `sceGuDepthMask`, `sceKernelDcacheWritebackInvalidateRange`, `sceKernelDcacheInvalidateRange`.

Exact strict result IDs (726 ordered records including the terminal record):

```text
vram-mirror-info
vram-cpu-pattern-000
vram-cpu-pattern-001
vram-cpu-pattern-002
vram-cpu-pattern-003
vram-cpu-pattern-004
vram-cpu-pattern-005
vram-cpu-pattern-006
vram-cpu-pattern-007
vram-cpu-pattern-008
vram-cpu-pattern-009
vram-cpu-pattern-010
vram-cpu-pattern-011
vram-cpu-pattern-012
vram-cpu-pattern-013
vram-cpu-pattern-014
vram-cpu-pattern-015
vram-cpu-pattern-016
vram-cpu-pattern-017
vram-cpu-pattern-018
vram-cpu-pattern-019
vram-cpu-pattern-020
vram-cpu-pattern-021
vram-cpu-pattern-022
vram-cpu-pattern-023
vram-cpu-pattern-024
vram-cpu-pattern-025
vram-cpu-pattern-026
vram-cpu-pattern-027
vram-cpu-pattern-028
vram-cpu-pattern-029
vram-cpu-pattern-030
vram-cpu-pattern-031
vram-cpu-pattern-032
vram-cpu-pattern-033
vram-cpu-pattern-034
vram-cpu-pattern-035
vram-cpu-pattern-036
vram-cpu-pattern-037
vram-cpu-pattern-038
vram-cpu-pattern-039
vram-cpu-pattern-040
vram-cpu-pattern-041
vram-cpu-pattern-042
vram-cpu-pattern-043
vram-cpu-pattern-044
vram-cpu-pattern-045
vram-cpu-pattern-046
vram-cpu-pattern-047
vram-cpu-pattern-048
vram-cpu-pattern-049
vram-cpu-pattern-050
vram-cpu-pattern-051
vram-cpu-pattern-052
vram-cpu-pattern-053
vram-cpu-pattern-054
vram-cpu-pattern-055
vram-cpu-pattern-056
vram-cpu-pattern-057
vram-cpu-pattern-058
vram-cpu-pattern-059
vram-cpu-pattern-060
vram-cpu-pattern-061
vram-cpu-pattern-062
vram-cpu-pattern-063
vram-cpu-pattern-064
vram-cpu-pattern-065
vram-cpu-pattern-066
vram-cpu-pattern-067
vram-cpu-pattern-068
vram-cpu-pattern-069
vram-cpu-pattern-070
vram-cpu-pattern-071
vram-cpu-pattern-072
vram-cpu-pattern-073
vram-cpu-pattern-074
vram-cpu-pattern-075
vram-cpu-pattern-076
vram-cpu-pattern-077
vram-cpu-pattern-078
vram-cpu-pattern-079
vram-cpu-pattern-080
vram-cpu-pattern-081
vram-cpu-pattern-082
vram-cpu-pattern-083
vram-cpu-pattern-084
vram-cpu-pattern-085
vram-cpu-pattern-086
vram-cpu-pattern-087
vram-cpu-pattern-088
vram-cpu-pattern-089
vram-cpu-pattern-090
vram-cpu-pattern-091
vram-cpu-pattern-092
vram-cpu-pattern-093
vram-cpu-pattern-094
vram-cpu-pattern-095
vram-cpu-pattern-096
vram-cpu-pattern-097
vram-cpu-pattern-098
vram-cpu-pattern-099
vram-cpu-pattern-100
vram-cpu-pattern-101
vram-cpu-pattern-102
vram-cpu-pattern-103
vram-cpu-pattern-104
vram-cpu-pattern-105
vram-cpu-pattern-106
vram-cpu-pattern-107
vram-cpu-pattern-108
vram-cpu-pattern-109
vram-cpu-pattern-110
vram-cpu-pattern-111
vram-cpu-pattern-112
vram-cpu-pattern-113
vram-cpu-pattern-114
vram-cpu-pattern-115
vram-cpu-pattern-116
vram-cpu-pattern-117
vram-cpu-pattern-118
vram-cpu-pattern-119
vram-cpu-pattern-120
vram-cpu-pattern-121
vram-cpu-pattern-122
vram-cpu-pattern-123
vram-cpu-pattern-124
vram-cpu-pattern-125
vram-cpu-pattern-126
vram-cpu-pattern-127
vram-cpu-pattern-128
vram-cpu-pattern-129
vram-cpu-pattern-130
vram-cpu-pattern-131
vram-cpu-pattern-132
vram-cpu-pattern-133
vram-cpu-pattern-134
vram-cpu-pattern-135
vram-cpu-pattern-136
vram-cpu-pattern-137
vram-cpu-pattern-138
vram-cpu-pattern-139
vram-cpu-pattern-140
vram-cpu-pattern-141
vram-cpu-pattern-142
vram-cpu-pattern-143
vram-cpu-pattern-144
vram-cpu-pattern-145
vram-cpu-pattern-146
vram-cpu-pattern-147
vram-cpu-pattern-148
vram-cpu-pattern-149
vram-cpu-pattern-150
vram-cpu-pattern-151
vram-cpu-pattern-152
vram-cpu-pattern-153
vram-cpu-pattern-154
vram-cpu-pattern-155
vram-cpu-pattern-156
vram-cpu-pattern-157
vram-cpu-pattern-158
vram-cpu-pattern-159
vram-cpu-pattern-160
vram-cpu-pattern-161
vram-cpu-pattern-162
vram-cpu-pattern-163
vram-cpu-pattern-164
vram-cpu-pattern-165
vram-cpu-pattern-166
vram-cpu-pattern-167
vram-cpu-pattern-168
vram-cpu-pattern-169
vram-cpu-pattern-170
vram-cpu-pattern-171
vram-cpu-pattern-172
vram-cpu-pattern-173
vram-cpu-pattern-174
vram-cpu-pattern-175
vram-cpu-pattern-176
vram-cpu-pattern-177
vram-cpu-pattern-178
vram-cpu-pattern-179
vram-cpu-pattern-180
vram-cpu-pattern-181
vram-cpu-pattern-182
vram-cpu-pattern-183
vram-cpu-pattern-184
vram-cpu-pattern-185
vram-cpu-pattern-186
vram-cpu-pattern-187
vram-cpu-pattern-188
vram-cpu-pattern-189
vram-cpu-pattern-190
vram-cpu-pattern-191
vram-cpu-pattern-192
vram-cpu-pattern-193
vram-cpu-pattern-194
vram-cpu-pattern-195
vram-cpu-pattern-196
vram-cpu-pattern-197
vram-cpu-pattern-198
vram-cpu-pattern-199
vram-cpu-pattern-200
vram-cpu-pattern-201
vram-cpu-pattern-202
vram-cpu-pattern-203
vram-cpu-pattern-204
vram-cpu-pattern-205
vram-cpu-pattern-206
vram-cpu-pattern-207
vram-cpu-pattern-208
vram-cpu-pattern-209
vram-cpu-pattern-210
vram-cpu-pattern-211
vram-cpu-pattern-212
vram-cpu-pattern-213
vram-cpu-pattern-214
vram-cpu-pattern-215
vram-cpu-pattern-216
vram-cpu-pattern-217
vram-cpu-pattern-218
vram-cpu-pattern-219
vram-cpu-pattern-220
vram-cpu-pattern-221
vram-cpu-pattern-222
vram-cpu-pattern-223
vram-cpu-pattern-224
vram-cpu-pattern-225
vram-cpu-pattern-226
vram-cpu-pattern-227
vram-cpu-pattern-228
vram-cpu-pattern-229
vram-cpu-pattern-230
vram-cpu-pattern-231
vram-cpu-pattern-232
vram-cpu-pattern-233
vram-cpu-pattern-234
vram-cpu-pattern-235
vram-cpu-pattern-236
vram-cpu-pattern-237
vram-cpu-pattern-238
vram-cpu-pattern-239
vram-cpu-pattern-240
vram-cpu-pattern-241
vram-cpu-pattern-242
vram-cpu-pattern-243
vram-cpu-pattern-244
vram-cpu-pattern-245
vram-cpu-pattern-246
vram-cpu-pattern-247
vram-cpu-pattern-248
vram-cpu-pattern-249
vram-cpu-pattern-250
vram-cpu-pattern-251
vram-cpu-pattern-252
vram-cpu-pattern-253
vram-cpu-pattern-254
vram-cpu-pattern-255
vram-cpu-pattern-256
vram-cpu-pattern-257
vram-cpu-pattern-258
vram-cpu-pattern-259
vram-cpu-pattern-260
vram-cpu-pattern-261
vram-cpu-pattern-262
vram-cpu-pattern-263
vram-cpu-pattern-264
vram-cpu-pattern-265
vram-cpu-pattern-266
vram-cpu-pattern-267
vram-cpu-pattern-268
vram-cpu-pattern-269
vram-cpu-pattern-270
vram-cpu-pattern-271
vram-cpu-pattern-272
vram-cpu-pattern-273
vram-cpu-pattern-274
vram-cpu-pattern-275
vram-cpu-pattern-276
vram-cpu-pattern-277
vram-cpu-pattern-278
vram-cpu-pattern-279
vram-cpu-pattern-280
vram-cpu-pattern-281
vram-cpu-pattern-282
vram-cpu-pattern-283
vram-cpu-pattern-284
vram-cpu-pattern-285
vram-cpu-pattern-286
vram-cpu-pattern-287
vram-cpu-pattern-288
vram-cpu-pattern-289
vram-cpu-pattern-290
vram-cpu-pattern-291
vram-cpu-pattern-292
vram-cpu-pattern-293
vram-cpu-pattern-294
vram-cpu-pattern-295
vram-cpu-pattern-296
vram-cpu-pattern-297
vram-cpu-pattern-298
vram-cpu-pattern-299
vram-cpu-pattern-300
vram-cpu-pattern-301
vram-cpu-pattern-302
vram-cpu-pattern-303
vram-cpu-pattern-304
vram-cpu-pattern-305
vram-cpu-pattern-306
vram-cpu-pattern-307
vram-cpu-pattern-308
vram-cpu-pattern-309
vram-cpu-pattern-310
vram-cpu-pattern-311
vram-cpu-pattern-312
vram-cpu-pattern-313
vram-cpu-pattern-314
vram-cpu-pattern-315
vram-cpu-pattern-316
vram-cpu-pattern-317
vram-cpu-pattern-318
vram-cpu-pattern-319
vram-cpu-pattern-320
vram-cpu-pattern-321
vram-cpu-pattern-322
vram-cpu-pattern-323
vram-cpu-pattern-324
vram-cpu-pattern-325
vram-cpu-pattern-326
vram-cpu-pattern-327
vram-cpu-pattern-328
vram-cpu-pattern-329
vram-cpu-pattern-330
vram-cpu-pattern-331
vram-cpu-pattern-332
vram-cpu-pattern-333
vram-cpu-pattern-334
vram-cpu-pattern-335
vram-cpu-pattern-336
vram-cpu-pattern-337
vram-cpu-pattern-338
vram-cpu-pattern-339
vram-cpu-pattern-340
vram-cpu-pattern-341
vram-cpu-pattern-342
vram-cpu-pattern-343
vram-cpu-pattern-344
vram-cpu-pattern-345
vram-cpu-pattern-346
vram-cpu-pattern-347
vram-cpu-pattern-348
vram-cpu-pattern-349
vram-cpu-pattern-350
vram-cpu-pattern-351
vram-cpu-pattern-352
vram-cpu-pattern-353
vram-cpu-pattern-354
vram-cpu-pattern-355
vram-cpu-pattern-356
vram-cpu-pattern-357
vram-cpu-pattern-358
vram-cpu-pattern-359
vram-cpu-pattern-360
vram-cpu-pattern-361
vram-cpu-pattern-362
vram-cpu-pattern-363
vram-cpu-pattern-364
vram-cpu-pattern-365
vram-cpu-pattern-366
vram-cpu-pattern-367
vram-cpu-pattern-368
vram-cpu-pattern-369
vram-cpu-pattern-370
vram-cpu-pattern-371
vram-cpu-pattern-372
vram-cpu-pattern-373
vram-cpu-pattern-374
vram-cpu-pattern-375
vram-cpu-pattern-376
vram-cpu-pattern-377
vram-cpu-pattern-378
vram-cpu-pattern-379
vram-cpu-pattern-380
vram-cpu-pattern-381
vram-cpu-pattern-382
vram-cpu-pattern-383
vram-cpu-pattern-384
vram-cpu-pattern-385
vram-cpu-pattern-386
vram-cpu-pattern-387
vram-cpu-pattern-388
vram-cpu-pattern-389
vram-cpu-pattern-390
vram-cpu-pattern-391
vram-cpu-pattern-392
vram-cpu-pattern-393
vram-cpu-pattern-394
vram-cpu-pattern-395
vram-cpu-pattern-396
vram-cpu-pattern-397
vram-cpu-pattern-398
vram-cpu-pattern-399
vram-cpu-pattern-400
vram-cpu-pattern-401
vram-cpu-pattern-402
vram-cpu-pattern-403
vram-cpu-pattern-404
vram-cpu-pattern-405
vram-cpu-pattern-406
vram-cpu-pattern-407
vram-cpu-pattern-408
vram-cpu-pattern-409
vram-cpu-pattern-410
vram-cpu-pattern-411
vram-cpu-pattern-412
vram-cpu-pattern-413
vram-cpu-pattern-414
vram-cpu-pattern-415
vram-cpu-pattern-416
vram-cpu-pattern-417
vram-cpu-pattern-418
vram-cpu-pattern-419
vram-cpu-pattern-420
vram-cpu-pattern-421
vram-cpu-pattern-422
vram-cpu-pattern-423
vram-cpu-pattern-424
vram-cpu-pattern-425
vram-cpu-pattern-426
vram-cpu-pattern-427
vram-cpu-pattern-428
vram-cpu-pattern-429
vram-cpu-pattern-430
vram-cpu-pattern-431
vram-cpu-pattern-432
vram-cpu-pattern-433
vram-cpu-pattern-434
vram-cpu-pattern-435
vram-cpu-pattern-436
vram-cpu-pattern-437
vram-cpu-pattern-438
vram-cpu-pattern-439
vram-cpu-pattern-440
vram-cpu-pattern-441
vram-cpu-pattern-442
vram-cpu-pattern-443
vram-cpu-pattern-444
vram-cpu-pattern-445
vram-cpu-pattern-446
vram-cpu-pattern-447
vram-cpu-pattern-448
vram-cpu-pattern-449
vram-cpu-pattern-450
vram-cpu-pattern-451
vram-cpu-pattern-452
vram-cpu-pattern-453
vram-cpu-pattern-454
vram-cpu-pattern-455
vram-cpu-pattern-456
vram-cpu-pattern-457
vram-cpu-pattern-458
vram-cpu-pattern-459
vram-cpu-pattern-460
vram-cpu-pattern-461
vram-cpu-pattern-462
vram-cpu-pattern-463
vram-cpu-pattern-464
vram-cpu-pattern-465
vram-cpu-pattern-466
vram-cpu-pattern-467
vram-cpu-pattern-468
vram-cpu-pattern-469
vram-cpu-pattern-470
vram-cpu-pattern-471
vram-cpu-pattern-472
vram-cpu-pattern-473
vram-cpu-pattern-474
vram-cpu-pattern-475
vram-cpu-pattern-476
vram-cpu-pattern-477
vram-cpu-pattern-478
vram-cpu-pattern-479
vram-cpu-pattern-480
vram-cpu-pattern-481
vram-cpu-pattern-482
vram-cpu-pattern-483
vram-cpu-pattern-484
vram-cpu-pattern-485
vram-cpu-pattern-486
vram-cpu-pattern-487
vram-cpu-pattern-488
vram-cpu-pattern-489
vram-cpu-pattern-490
vram-cpu-pattern-491
vram-cpu-pattern-492
vram-cpu-pattern-493
vram-cpu-pattern-494
vram-cpu-pattern-495
vram-cpu-pattern-496
vram-cpu-pattern-497
vram-cpu-pattern-498
vram-cpu-pattern-499
vram-cpu-pattern-500
vram-cpu-pattern-501
vram-cpu-pattern-502
vram-cpu-pattern-503
vram-cpu-pattern-504
vram-cpu-pattern-505
vram-cpu-pattern-506
vram-cpu-pattern-507
vram-cpu-pattern-508
vram-cpu-pattern-509
vram-cpu-pattern-510
vram-cpu-pattern-511
vram-cpu-pattern-512
vram-cpu-pattern-513
vram-cpu-pattern-514
vram-cpu-pattern-515
vram-cpu-pattern-516
vram-cpu-write-mirror-00-00
vram-cpu-write-mirror-00-01
vram-cpu-write-mirror-00-02
vram-cpu-write-mirror-01-00
vram-cpu-write-mirror-01-01
vram-cpu-write-mirror-01-02
vram-cpu-write-mirror-02-00
vram-cpu-write-mirror-02-01
vram-cpu-write-mirror-02-02
vram-cpu-write-mirror-03-00
vram-cpu-write-mirror-03-01
vram-cpu-write-mirror-03-02
vram-cpu-write-mirror-04-00
vram-cpu-write-mirror-04-01
vram-cpu-write-mirror-04-02
vram-cpu-write-mirror-05-00
vram-cpu-write-mirror-05-01
vram-cpu-write-mirror-05-02
vram-ge-clear-sync
vram-ge-sprite-a-sync
vram-ge-sprite-b-sync
vram-ge-sprite-c-sync
vram-ge-depth-row0-00
vram-ge-depth-row0-01
vram-ge-depth-row0-02
vram-ge-depth-row0-03
vram-ge-depth-row0-04
vram-ge-depth-row0-05
vram-ge-depth-row0-06
vram-ge-depth-row0-07
vram-ge-depth-row0-08
vram-ge-depth-row0-09
vram-ge-depth-row0-10
vram-ge-depth-row0-11
vram-ge-depth-row0-12
vram-ge-depth-row0-13
vram-ge-depth-row0-14
vram-ge-depth-row0-15
vram-ge-depth-row1-00
vram-ge-depth-row1-01
vram-ge-depth-row1-02
vram-ge-depth-row1-03
vram-ge-depth-row1-04
vram-ge-depth-row1-05
vram-ge-depth-row1-06
vram-ge-depth-row1-07
vram-ge-depth-row1-08
vram-ge-depth-row1-09
vram-ge-depth-row1-10
vram-ge-depth-row1-11
vram-ge-depth-row1-12
vram-ge-depth-row1-13
vram-ge-depth-row1-14
vram-ge-depth-row1-15
vram-ge-depth-row2-00
vram-ge-depth-row2-01
vram-ge-depth-row2-02
vram-ge-depth-row2-03
vram-ge-depth-row2-04
vram-ge-depth-row2-05
vram-ge-depth-row2-06
vram-ge-depth-row2-07
vram-ge-depth-row2-08
vram-ge-depth-row2-09
vram-ge-depth-row2-10
vram-ge-depth-row2-11
vram-ge-depth-row2-12
vram-ge-depth-row2-13
vram-ge-depth-row2-14
vram-ge-depth-row2-15
vram-ge-depth-row3-00
vram-ge-depth-row3-01
vram-ge-depth-row3-02
vram-ge-depth-row3-03
vram-ge-depth-row3-04
vram-ge-depth-row3-05
vram-ge-depth-row3-06
vram-ge-depth-row3-07
vram-ge-depth-row3-08
vram-ge-depth-row3-09
vram-ge-depth-row3-10
vram-ge-depth-row3-11
vram-ge-depth-row3-12
vram-ge-depth-row3-13
vram-ge-depth-row3-14
vram-ge-depth-row3-15
vram-ge-depth-row4-00
vram-ge-depth-row4-01
vram-ge-depth-row4-02
vram-ge-depth-row4-03
vram-ge-depth-row4-04
vram-ge-depth-row4-05
vram-ge-depth-row4-06
vram-ge-depth-row4-07
vram-ge-depth-row4-08
vram-ge-depth-row4-09
vram-ge-depth-row4-10
vram-ge-depth-row4-11
vram-ge-depth-row4-12
vram-ge-depth-row4-13
vram-ge-depth-row4-14
vram-ge-depth-row4-15
vram-ge-depth-row5-00
vram-ge-depth-row5-01
vram-ge-depth-row5-02
vram-ge-depth-row5-03
vram-ge-depth-row5-04
vram-ge-depth-row5-05
vram-ge-depth-row5-06
vram-ge-depth-row5-07
vram-ge-depth-row5-08
vram-ge-depth-row5-09
vram-ge-depth-row5-10
vram-ge-depth-row5-11
vram-ge-depth-row5-12
vram-ge-depth-row5-13
vram-ge-depth-row5-14
vram-ge-depth-row5-15
vram-ge-depth-row6-00
vram-ge-depth-row6-01
vram-ge-depth-row6-02
vram-ge-depth-row6-03
vram-ge-depth-row6-04
vram-ge-depth-row6-05
vram-ge-depth-row6-06
vram-ge-depth-row6-07
vram-ge-depth-row6-08
vram-ge-depth-row6-09
vram-ge-depth-row6-10
vram-ge-depth-row6-11
vram-ge-depth-row6-12
vram-ge-depth-row6-13
vram-ge-depth-row6-14
vram-ge-depth-row6-15
vram-ge-depth-row7-00
vram-ge-depth-row7-01
vram-ge-depth-row7-02
vram-ge-depth-row7-03
vram-ge-depth-row7-04
vram-ge-depth-row7-05
vram-ge-depth-row7-06
vram-ge-depth-row7-07
vram-ge-depth-row7-08
vram-ge-depth-row7-09
vram-ge-depth-row7-10
vram-ge-depth-row7-11
vram-ge-depth-row7-12
vram-ge-depth-row7-13
vram-ge-depth-row7-14
vram-ge-depth-row7-15
vram-ge-depth-row8-00
vram-ge-depth-row8-01
vram-ge-depth-row8-02
vram-ge-depth-row8-03
vram-ge-depth-row8-04
vram-ge-depth-row8-05
vram-ge-depth-row8-06
vram-ge-depth-row8-07
vram-ge-depth-row8-08
vram-ge-depth-row8-09
vram-ge-depth-row8-10
vram-ge-depth-row8-11
vram-ge-depth-row8-12
vram-ge-depth-row8-13
vram-ge-depth-row8-14
vram-ge-depth-row8-15
vram-ge-depth-row9-00
vram-ge-depth-row9-01
vram-ge-depth-row9-02
vram-ge-depth-row9-03
vram-ge-depth-row9-04
vram-ge-depth-row9-05
vram-ge-depth-row9-06
vram-ge-depth-row9-07
vram-ge-depth-row9-08
vram-ge-depth-row9-09
vram-ge-depth-row9-10
vram-ge-depth-row9-11
vram-ge-depth-row9-12
vram-ge-depth-row9-13
vram-ge-depth-row9-14
vram-ge-depth-row9-15
vram-ge-color-row0-00
vram-ge-color-row0-01
vram-ge-color-row0-02
vram-ge-color-row0-03
vram-ge-color-row0-04
vram-ge-color-row0-05
vram-ge-color-row0-06
vram-ge-color-row0-07
vram-ge-color-row1-00
vram-ge-color-row1-01
vram-ge-color-row1-02
vram-ge-color-row1-03
vram-ge-color-row1-04
vram-ge-color-row1-05
vram-ge-color-row1-06
vram-ge-color-row1-07
vram-ge-color-row2-00
vram-ge-color-row2-01
vram-ge-color-row2-02
vram-ge-color-row2-03
vram-ge-color-row2-04
vram-ge-color-row2-05
vram-ge-color-row2-06
vram-ge-color-row2-07
vram-restore
vram-mirror-done
```

## Pending PSP-3000 measurement campaign

The following twenty-one source-owned cases are in the PSP-3000 / 6.61 measurement
registry; the status column distinguishes measured, captured-only and pending evidence.
Six were measured by the 2026-10-10 campaign on `f6ccfb33`; `ge-break-continue`
was measured by its acceptance-eligible 2026-10-15 re-run on `c642039a` after two
captured-only runs; `llsc-link`, `vfpu-compare` and `fpu-cc` were measured on
2026-10-15 from `e45e667b`. The five initial single-launch HLE cases (97-101)
ran on 2026-10-15 from `b5d6fc5b`: the two VTimer cases and power-float case
were measured, the earlier pool-bounds revision was captured only (the revised
form has not run), and null-out was captured incomplete. The added HLE timer-order
case (103) and the VFPU transcendental corpus were measured on 2026-10-15 from
`22b027f6`, and the HLE audio case (102) from `7fc1641e` after a first run from
`22b027f6` was captured only.
These eight probes run as single launches outside the campaign queue
(`CAMPAIGN_QUEUE_CASES`); the manifest distinguishes measured, captured and
not-run states. Their parsers check record order, scalar fields and completion,
not expected PSP values.

| Case | Evidence id | Hardware status | Contract being measured |
| --- | --- | --- | --- |
| `kernel-alarm` | `PSP-ALARM-001` | `MEASURED` (2026-10-10, `f6ccfb33`) | Alarm creation with a null handler and zero clock; alarm-table exhaustion capped at 1024 pending alarms; cancel after one-shot fire, repeat cancel, unknown UID; handler-return re-arm base; interrupt state and a bounded blocking call in an alarm handler. |
| `thread-scheduler` | `PSP-THREAD-003` | `MEASURED` (2026-10-10, `f6ccfb33`) | Suspend UID 0 and self; resume UID 0; invalid ready-queue priority; ready-thread ordering after rotation; a timed wait expiring while its thread is suspended. |
| `wait-outcomes` | `PSP-WAIT-001` | `MEASURED` (2026-10-10, `f6ccfb33`) | Semaphore and event-flag signal/cancel before the timeout deadline, followed by dispatch after that deadline. |
| `ge-break-continue` | `PSP-GE-CONTROL-001` | `MEASURED` (2026-10-15, `c642039a`; the earlier revision's 2026-10-10 `f6ccfb33` and 2026-10-15 `e45e667b` runs were not acceptance-eligible) | `sceGeBreak`/`sceGeContinue` return values without active/paused lists, invalid break mode, list/draw sync states for paused/cancelled lists, and a bounded continue drain and queue quiesce (`TIMEOUT` instead of a hang). The probe was revised after that capture: the cancelled-list cells are read after a `sceGeBreak(1)` reset, a stalled-list `sceGeListDeQueue` is a PASS cell whatever it returns, and `sceGeBreak(1)` with no prior dequeue is its own cell. |
| `vram-mirror` | `PSP-VRAM-MIRROR-001` | `MEASURED` (2026-10-16, PSP-3000 / 6.61; revisions `24c1472e`, `fd3efc64`, `df41dba2`) | Full position-encoded EDRAM fill followed by eight-alias reads: 0 mismatches over 4,134 samples per alias. `0x04000000`/`0x04400000` and uncached peers are identity; `0x04200000` and peers map `o ^ 0x2040`; `0x04600000` and peers rotate bits 5..9 left one, then XOR `0x2040`. Mirror write scans find the same transforms. The GE depth rule reported for the 512-pixel, 16-bit buffer is `(y * 1024 + 2x) ^ 0x2040`; `0x04200000` reads depth linearly and colour remains linear. Absolute-VRAM versus buffer-relative XOR is unresolved because zbp `0x0CC000` has bits 6 and 13 clear. Three through-mode raw-Z sprites, ten full depth rows and three color rows were captured; VRAM restore was verified. Raw PSP words remain private. |
| `refer-status-size` | `PSP-KERNEL-STATUS-001` | `MEASURED` (2026-10-10, `f6ccfb33`) | Bytes written and size-word results for `ReferSemaStatus`, `ReferEventFlagStatus`, and `ReferMbxStatus` at size 0, 8, 40, and full size. |
| `registry-readonly` | `PSP-REGISTRY-001` | `MEASURED` (2026-10-10, `f6ccfb33`) | Read-only root/category opening and `/CONFIG` category/key enumeration, metadata, selected modeled settings, error returns, handle exhaustion capped at 256 opens, and the forged-handle call last. |
| `llsc-link` | `PSP-LLSC-001` | `MEASURED` (2026-10-15, `e45e667b`) | Three ll/sc link windows: a plain ll and sc pair (control), a second sc after a successful sc with no new ll, and an sc to an address other than the one ll linked. Each record is the sc result, the word ll loaded, the words at both addresses, and the project's model words; all three cells match the model. |
| `kernel-misc` | `PSP-KERNEL-MISC-001` | `MEASURED` (2026-10-10, `f6ccfb33`) | Wide clock conversion, controller default mode, profiler-pointer returns, basic VTimer behavior, display return values, battery-icon status, and UMD-popup returns. |
| `vfpu-compare` | `PSP-VFPU-CMP-001` | `MEASURED` (2026-10-15, `e45e667b`) | `vscmp.s`, `vsge.s`, and `vslt.s` on 15 operand pairs: less-than, equal, greater, signed zeros in both orders, quiet, signalling, and negative NaN on each side, and infinities. Each record is the raw result word against the project's model at the time; the five `vscmp` NaN words differed from it, and the model now returns the measured words. |
| `fpu-cc` | `PSP-FPU-CC-001` | `MEASURED` (2026-10-15, `e45e667b`) | COP1 condition codes: which FCR31 condition bits exist, which bit `c.eq.s` sets for each condition code 0..7, and whether `bc1t`/`bc1f` with each code are taken under FCC0, no flag, and that code's own flag. Condition code 0 is the control. Only FCC0 exists; every code writes and tests it. |
| `vfpu-transcendental-corpus` | `PSP-VFPU-TRANSCENDENTAL-001` | `MEASURED` (2026-10-15, `22b027f6`; single launch, not in the campaign queue) | Raw single-lane result words for vsin, vcos, vexp2, vlog2, vrsq, vrcp, vsqrt, vasin, vnrcp, vnsin, and vrexp2. Each operation uses 63 raw-word inputs: 20 special encodings and 43 evenly spaced domain constants. VASIN substitutes three in-domain words for its previously measured edge inputs. Eight-word chunks produce 88 data records plus one completion record (89 result records total); parsing checks format only and asserts no hardware values. |
| `registry-font` | `PSP-REGISTRY-002` | `NOT_RUN` | Whether the registry handle lists the root, then the `/DATA/FONT` and `/SYSPROFILE/RESOLUTION` subtrees: every key's name, type, size, value and enumeration order, reached only through categories the console listed first. |
| `umd-io` | `PSP-UMDIO-001` | `MEASURED` (2026-10-17, PSP-3000 / 6.61 / ARK CFW; `e2180970`) | `sceIoOpen` and `sceIoDopen` results for bare UMD devices, disc paths, the two extra-flag opens, and `msstor0p1:/`; successful descriptors were closed with result 0. The UMD remained inserted and was not activated or changed. |
| `umd-raw-read` | `PSP-UMDRAW-001` | `CAPTURED` (2026-10-17 and 2026-10-18, incomplete; source commits `6a9a39aa`, `97ccbf87`, `61574d37`) | Three attempts used 360 s, 900 s and 1800 s host case timeouts. The two 2026-10-18 runs emitted 31 complete records through `size-4096-read`, then no 32 KiB read result. On the 1800 s run, `usbipd` reported the PSP absent at 3m46, before the 1800-second deadline; L1 reattach and three `ver` retries failed. Teardown/unload remains unverified. This records USB loss before the host timeout but does not distinguish a PSP-side read hang from PSPLink/USB transport failure. No raw UMD read or seek semantics are inferred; a fresh physical power-cycle is required before another launch. |
| `hle-vtimer-unit` | `PSP-HLE-VTIMER-002` | `MEASURED` (2026-10-15, `b5d6fc5b`; single launch, not in the campaign queue) | One VTimer: start; a `sceKernelGetVTimerTime` and a system-clock reading either side of a bounded 20 ms delay; the VTimer base and Refer base field while running and after stop; stop; delete. Each 64-bit clock is recorded as its low word then its high word. |
| `hle-vtimer-errors` | `PSP-HLE-VTIMER-003` | `MEASURED` (2026-10-15, `b5d6fc5b`; single launch, not in the campaign queue) | Start, stop, and `sceKernelGetVTimerTime` on a deleted UID, then a cleanup delete of that UID after its last use; a live timer created and deleted; up to 64 creates; out1 records whether a refusal occurred; every created timer deleted. The NULL option pointer of `sceKernelCreateVTimer` is the optional argument. |
| `hle-kernel-pool-bounds` | `PSP-HLE-KERNEL-STATUS-002` | `NOT_RUN` (revised form; single launch, not in the campaign queue; the earlier revision's 2026-10-15 `b5d6fc5b` run was captured only) | FPL creation with block size 0 and with a 0x7fffffff-byte block, each deleted when its create returned a pool; a refused create gives a PASS delete cell with out0 0 and no delete call; VPL allocation: up to 64 allocations; out1 records whether a refusal occurred; every block freed and the VPL deleted. The NULL option pointers of the pool creates are the optional argument. |
| `hle-power-float-v0` | `PSP-HLE-POWER-002` | `MEASURED` (2026-10-15, `b5d6fc5b`; single launch, not in the campaign queue) | Raw `$v0` and `$f0` bits of `scePowerGetPllClockFrequencyFloat`, `scePowerGetCpuClockFrequencyFloat`, and `scePowerGetBusClockFrequencyFloat`, one call each, through a short assembly wrapper. |
| `hle-null-out-pointers` | `PSP-HLE-NULL-OUT-001` | `CAPTURED` (2026-10-15, `b5d6fc5b`; incomplete, not measured; retired, never to be scheduled) | The existing capture stopped at the first mandatory NULL output, `sceKernelReferSystemStatus(NULL)`, and the console needed a power cycle. The kernel pointer check rejects kernel-space addresses but not address 0, so that call writes through NULL and faults; the VTimer getters also write through their output pointers. No game can rely on these results. |
| `hle-audio-src` | `PSP-HLE-AUDIO-SRC-001` | `MEASURED` (2026-10-15, `7fc1641e`; the earlier run of `22b027f6` was captured only; single launch, not in the campaign queue) | Unreserved SRC release and silent output; reserve 1024 samples at 44100 Hz stereo; second reserve; one silent blocking output; release after the blocking drain and release again; six invalid reserve shapes, immediately releasing any unexpected success; Output2-then-SRC and SRC-then-Output2 reservation attempts with cleanup. 18 result records. No SRC rest-sample query exists in the public NID corpus. Risk high: two silent blocking SRC calls, at most one block per call; schedule after the other new probes. |
| `hle-timer-order` | `PSP-HLE-TIMER-ORDER-001` | `MEASURED` (2026-10-15, `22b027f6`; single launch, not in the campaign queue) | Two equal-delay alarms set A then B; long-delay then short-delay alarm; nominally equal VTimer handler and alarm; SetAlarm(0). Each callback stores a shared sequence number and system-time low/high words. Every wait has an elapsed-time deadline; cancel all alarms and stop/delete the VTimer. Ordering only, no absolute-time claim; 18 result records. |

### PSP-UMDIO-001 measured run 2026-10-17

Acceptance-eligible measurement on a PSP-3000 with firmware 6.61 and ARK CFW,
at public commit `e2180970`. A UMD was inserted; `sceUmdCheckMedium` returned 1
and `sceUmdGetDriveStat` returned `0x32` both before and after the calls. Every
successful open or directory open returned descriptor 3 and its matching close
returned 0. The probe did not activate the UMD or perform any read, write, seek,
ioctl, devctl, or getstat through a raw descriptor; those semantics remain
unmeasured.

| Call | Flags | Result |
| --- | --- | --- |
| `sceIoOpen("umd0:")` | `PSP_O_RDONLY` | descriptor `3`; close `0` |
| `sceIoOpen("umd1:")` | `PSP_O_RDONLY` | descriptor `3`; close `0` |
| `sceIoOpen("umd:")` | `PSP_O_RDONLY` | descriptor `3`; close `0` |
| `sceIoOpen("umd0:/")` | `PSP_O_RDONLY` | descriptor `3`; close `0` |
| `sceIoOpen("umd1:/")` | `PSP_O_RDONLY` | descriptor `3`; close `0` |
| `sceIoOpen("disc0:")` | `PSP_O_RDONLY` | `0x80010016` |
| `sceIoOpen("disc0:/")` | `PSP_O_RDONLY` | `0x80010002` |
| `sceIoOpen("disc0:/PSP_GAME")` | `PSP_O_RDONLY` | descriptor `3`; close `0` |
| `sceIoOpen("disc0:/PSP_GAME/SYSDIR")` | `PSP_O_RDONLY` | descriptor `3`; close `0` |
| `sceIoOpen("disc0:/UMD_DATA.BIN")` | `PSP_O_RDONLY` | descriptor `3`; close `0` |
| `sceIoOpen("umd0:")` | `PSP_O_RDONLY \| 0x000f0000` | descriptor `3`; close `0` |
| `sceIoOpen("umd1:")` | `PSP_O_RDONLY \| 0x000f0000` | descriptor `3`; close `0` |
| `sceIoOpen("msstor0p1:/")` | `PSP_O_RDONLY` | descriptor `3`; close `0` |
| `sceIoDopen("umd0:")` | — | `0x80020325` |
| `sceIoDopen("umd1:")` | — | `0x80020325` |
| `sceIoDopen("umd:")` | — | `0x80020325` |
| `sceIoDopen("disc0:")` | — | `0x80010016` |
| `sceIoDopen("disc0:/")` | — | descriptor `3`; close `0` |
| `sceIoDopen("disc0:/PSP_GAME")` | — | descriptor `3`; close `0` |

The recorded cells are `umd-state-before`, `io-open-0000`, `io-open-0001`,
`io-open-0002`, `io-open-0003`, `io-open-0004`, `io-open-0005`, `io-open-0006`,
`io-open-0007`, `io-open-0008`, `io-open-0009`, `io-open-0010`, `io-open-0011`,
`io-open-0012`, `io-dopen-0000`, `io-dopen-0001`, `io-dopen-0002`,
`io-dopen-0003`, `io-dopen-0004`, `io-dopen-0005`, `umd-state-after`, and
`umdio-done`.

### PSP-UMDRAW-001 raw UMD read probe

Status: CAPTURED, not measured. The source-owned case opens only umd0: with read-only flags.
It records 64-bit seek results as low then high words, sceIoLseek32 results,
read counts, the single immediate async poll and its following wait,
sceIoGetstat, and UMD state before and after. Reads request at most 32,768
bytes. The only disc bytes emitted are the first eight bytes of designated
sector-16 reads, plus a whole-buffer-zero flag; no checksum or other payload is
recorded. The async request is waited before the descriptor closes. The host
fixture uses a synthetic byte-addressed disc and is not hardware evidence.

Two incomplete hardware captures are now recorded. The first, from local
source commit 6a9a39aa on 2026-10-17, emitted 31 result records through
size-4096-read, then stopped at the combined size-32768-read progress marker.
It reached the 360-second host case timeout without umdraw-done; the runner
could not verify module unload or the host0 round-trip.

The second, from local source commit 97ccbf87 on 2026-10-18 after an
owner-confirmed power-cycle, passed the transport-write preflight and recorded
all raw-read results through 4096 bytes. The updated probe emitted separate
size-32768-seek and size-32768-read markers. Its last host0 write was 27.2
seconds after launch; no 32 KiB result or umdraw-done arrived within the
900-second host case timeout. The runner stopped with HOST0_ROUNDTRIP_FAILED.
PSPLink was absent from usbipd list during recovery, later ver retries failed,
and module unload was not verified.

The 900-second event is a host-harness timeout followed by transport loss. It
does not establish the PSP sceIoRead return semantics or distinguish a PSP call
hang from a USBHostFS/PSPLink loss. No raw UMD read or seek semantics are
measured. Do not retry this boot; a fresh physical power-cycle is required
before any new target launch.

Captured result-record IDs:

```text
umd-state-before, raw-open-extra, seek-set-0, seek-set-16, seek-set-32768,
seek-cur-0, seek-end-0, seek-set-1, seek-set-neg1, seek32-set-0,
seek32-set-16, seek32-set-32768, seek32-end-0, pos-advance-seek,
pos-advance-first, pos-advance-second, pos-advance-cur, byte-sector16-seek,
byte-sector16-read, size-1-seek, size-1-read, size-512-seek, size-512-read,
size-2047-seek, size-2047-read, size-2048-seek, size-2048-read,
size-2049-seek, size-2049-read, size-4096-seek, size-4096-read
```

The fixed output sequence is umd-state-before, raw-open-extra, seven 64-bit
seek cells, four 32-bit seek cells, pos-advance-seek plus two consecutive reads
and pos-advance-cur, byte-sector16-seek and byte-sector16-read, seek/read pairs
for sizes 1, 512, 2047, 2048, 2049, 4096, 32768 and 0,
unaligned-seek/unaligned-read, far-seek/far-read, async-seek, async-read,
async-poll, async-wait, async-data, getstat, raw-close, readonly-open with
three seeks and readonly-close, umd-state-after, and umdraw-done.

The registry probe opens the registry and categories in read mode and never
calls a write, create, remove, or flush API. Values are emitted only for keys
the runtime models (`language`, `button_assign`, date/time format, `timezone`,
`summer_time`, and `adhoc_channel`); other keys emit name, type, and size only.
The resulting registry and console-specific files remain in the private
campaign output directory.

The font-registry probe (`registry-font`, `fixtures/psp_oracle/registry_walk.h`)
opens no category by a literal path. On PSP-3000 6.6.1 opening a missing
category creates it, so the probe lists the root through the registry handle
itself and opens a category only after its open parent listed it as a
directory with a name shorter than the 26-byte stored-name limit. If the
registry handle does not list the root, or its listing does not show the
measured `CONFIG` category as a directory, the probe opens nothing below it and
records that result. It enters only `/DATA`, `/DATA/FONT`, `/SYSPROFILE` and
`/SYSPROFILE/RESOLUTION` and what lies below the last two, and reads values only
there. The build pins the probe's `sceReg` imports to the eight read calls.

The `font-files` case (`PSP-FONTFILES-001`, `fixtures/psp_oracle/font_files.h`)
is not in the campaign queue; it runs only when the maintainer asks for it. It
lists `flash0:/font`, opens each regular file read-only, and copies it to
`host0:/fonts/` for the maintainer's private use, verifying each copy with
SHA-256 over the flash0 read, the host0 read-back and a second flash0 read. The
copies and digests are private evidence and are never committed.

The campaign queue is ordered in `tools/psp_oracle/run_psplink.py`. It starts
with the host0 `transport-write` preflight, then the new cases above, followed
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

Requires a PSP that can run PSPLink homebrew (1000/2000/3000 use **mini-USB Type B**; the Go uses a
proprietary connector), a **Memory Stick PRO Duo**, a data-capable cable, and mains power.

1. **Device** — the measured cells above were taken on a PSP running the ARK custom firmware
   (version 5.1.0; "ARK-4" elsewhere in this document is the upstream project name). Do not cite a
   measurement against a setup you did not run it on.
2. **PSPLink** — extract the psplinkusb release to `ms0:/PSP/GAME`.
3. **Windows driver** — with PSPLink running and USB connected, use Zadig: *Options → List All
   Devices*, select **`"PSP" type B`**, install the **`libusb-win32`** driver.
4. **Connect** — run `usbhostfs_pc` and `pspsh` in two terminals, **both opened in the build
   directory**. `pspsh` gives a `host0:/>` prompt.
5. **Toolchain** — PSPSDK on the host; build homebrew as an unencrypted `.prx` with `BUILD_PRX=1`.
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
per-partition total/largest free bytes, and modules before load. Before S0, the
runner issues `ls host0:/`, a listing that writes nothing, then re-reads the thread
list, which must match S0; a failed warm-up or a thread set that still moves blocks
the case before load. Measured 2026-10-12 (session `psp-session-1012`, PSP-3000,
PSPLink v3.2.1, commit `5ce131f4`): four `transport-write` runs all classified with
S0 = S2 = 17 threads, the qualified one as `QUALIFIED_CAPTURE`. On that device the
listing started no thread `thlist` reports — a freshly reset shell read 17 threads
before any host0 access and 17 after the listing, byte-identical — and control runs
with the warm-up disabled, and with the warm-up and the post-unload settle both
disabled, passed teardown as well. The warm-up is a conditional baseline guard: if
first host0 I/O starts a service thread on a particular boot, taking S0 afterwards
includes it in the baseline. These campaigns did not observe a new thread and did
not establish that a service thread caused the original 17-versus-18 discrepancy,
which did not reproduce on the measured boot.
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
  [Adrenaline](https://github.com/TheOfficialFloW/Adrenaline) ·
  [VitaShell](https://github.com/TheOfficialFloW/VitaShell)
