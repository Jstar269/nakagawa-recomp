# Independence backlog

Ranked candidates for moving Nakagawa's implementation from derived to independently evidenced.
Governed by [INDEPENDENCE_MODEL.md](INDEPENDENCE_MODEL.md); classifications live in
[IMPLEMENTATION_PROVENANCE.json](IMPLEMENTATION_PROVENANCE.json).

Ranking is `leverage x confidence x evidence availability / risk`. **A PPSSPP name in a comment is
not leverage.** Removing a required upstream dependency, or replacing an actual derived unit with a
hardware-backed one, is.

## Ranked

| # | Item | Leverage | Confidence | Evidence | Risk | State |
| --- | --- | --- | --- | --- | --- | --- |
| IND-1 | Self-authenticating NID name table | High | High | **Available now** | Low | **DONE** |
| IND-2 | VFPU register addressing from hardware | High | High | HQ-1 measured ([#296](https://github.com/Jstar269/nakagawa-recomp/issues/296)) | Low | **DONE** (partial scope) |
| IND-4 | Measure sal063 retention | High | n/a (audit) | Available now | None | **DONE** |
| IND-6 | Reconcile NOTICE.md with file headers | High | High | Available now | None | **DONE** |
| IND-3 | Event-flag semantics from hardware | Medium | High | Needs HQ-2 | Low | defer to #93 |
| IND-7 | Correct the stale `exact ports` claim | Medium | High | Available now | None | ready |
| IND-8 | Savedata parameter block from PSPSDK | Medium | Medium | Partial | Medium | defer to #91 |
| IND-5 | VFPU table regeneration feasibility | Medium | Low | Needs HQ-3 | High | study only |
| IND-9 | Decompose `hle.c` into evidenced sub-units | High | Low | Per-issue | High | long-run |
| IND-10 | GE rasterizer independence | High | Very low | None | Very high | not a candidate |

## IND-1 — Self-authenticating NID name table — **COMPLETE**

**Delivered.** 1615 entries (was 1623). **1463 copied numeric constants removed** — those NIDs are
now recomputed from their name at generation time and are physically absent from the tracked
corpus. 152 NIDs that genuinely cannot be derived remain stored, and now stand as the explicit
visible residue. The 8 sentinels are gone; `tools/gen_nidnames.py` no longer reads a PPSSPP
checkout.

**The classification did not change.** The table is still `derived-data`: the name list remains
PPSSPP-suggested and no independence is claimed for it. What changed is how much is copied.

One bug worth recording, because the guard caught it and it would have been silent data loss: an
early sentinel heuristic treated any `__`-prefixed name as emulator-internal. The PSP's sceSasCore
library genuinely exports 33 `__sceSas*` names, 32 of them independently confirmed in the pinned
PSPSDK headers. A hash-verified NID is *proof* of non-sentinel allocation, so the check now applies
only to non-derivable entries, and a test locks all 33 in place.

The original analysis follows.

`src/rt/nid_names.h` is 1623 NID-to-name pairs scraped from PPSSPP's `Core/HLE/*.cpp` tables by
`tools/gen_nidnames.py`, which requires a PPSSPP source checkout to reproduce. It is the only
tracked artifact in the tree whose regeneration needs upstream source.

The independence mechanism is that PSP NIDs are not an arbitrary registry. For the overwhelming
majority of firmware exports the NID is `SHA-1(export_name)[0:4]` interpreted little-endian — the
rule `psp-build-exports` in the pspdev toolchain implements when it builds a PRX. A pair that
satisfies that identity is a **fact anyone can recompute from the name alone**; no upstream source
is needed to establish or check it.

Measured by `tools/nid_name_proof.py` against the shipped table:

| classification | count |
| --- | --- |
| `verified` — NID reproducible from the name | **1463** (90.1%) |
| of those, also regenerable from PSPSDK headers with no PPSSPP input | 840 |
| `unresolved` | 88 |
| `library-attributed-unknown-name` | 54 |
| `editorial-alias` | 10 |
| `emulator-internal-sentinel` | 8 |

### What the 90.1% does and does not mean

For a verified entry the claim is exactly one sentence: **given the name, the NID is independently
reproducible factual data.**

It does **not** establish independent provenance of the name *list*, originality of the surrounding
table structure, correctness of the 160 unverified entries, or absence of PPSSPP influence. Only 840
of the 1463 have a second, PPSSPP-free source; the other 623 are correct-but-PPSSPP-suggested.

### The 160 unverified entries are not wrong

Ten alternative derivations were tested against all 160 — big-endian prefix, other digest windows,
digest of the name with a trailing NUL, first-character case variants, MD5, and library-prefixed
forms. **None explained a single entry.** A separate probe confirmed that no unverified name's
computed NID appears anywhere else in the table, so there is no "right name, wrong NID" pairing
either.

That is a clean negative result. It means these names are neither confirmed nor refuted. They are
classified by observable structure, not by correctness:

- **54 `library-attributed-unknown-name`** — shape `<Library>_<own NID in hex>`, e.g.
  `sceUtility_043ebe3e`, `sceMpeg_11CAB459`. An earlier draft of this backlog called these
  zero-information; **that was wrong**. The export name is genuinely unknown, but the entry records
  *which library owns the NID* — `sceUtility` 9, `sceImpose` 5, `sceMpeg` 5, `scePower` 5, and 16
  further libraries — and the bare hex the diagnostic prints does not carry that. **Retain.**
- **10 `editorial-alias`** — `sceKernelSetCompiledSdkVersion401_402` and siblings, plus
  `scePowerSetClockFrequency350`. Each is a firmware-range suffix on a base name that is itself
  present *and verifiable* in the same table (`sceKernelSetCompiledSdkVersion` verifies at
  `0x7591c7db`). Almost certainly not export names, but the NIDs they label are real. **Retain and
  flag.**
- **8 `emulator-internal-sentinel`** — `0x13370001 __IoAsyncFinish`, `0xc0de0001-3 __Utility*`,
  `0x756e6e6f`/`0x756e6f00`/`0x756e6f10 __Net*Callbacks`, `0xdeadbeaf pspeDebugWrite`. Reserved `__`
  identifiers on NID values no digest realistically produces; `0xc0de0001-0003` are allocated
  *sequentially* across three related names, which a hash cannot do. **The only entries recommended
  for removal** (finding PROV-F5) — and note this is a structural inference from numeric shape, not
  a proof about PSP firmware.
- **88 `unresolved`** — 71 `sceNp*`, 6 `sceNet*`, 6 `sceKernel*`, 5 stragglers. The `sceNp`
  concentration is consistent with those libraries using another derivation, but nothing confirms
  it. **Retain, marked unresolved.**

Zero of the 160 appear in HST's import set, so the recommended removal changes no HST diagnostic.

### Plan — a bounded provenance improvement, not an independence claim

The goal is **not** "replace the PPSSPP NID table and declare independence". The name list stays
PPSSPP-suggested and the table stays `behavior-informed`. What is achievable is narrower and real:
remove copied *numeric* constants where they can be recomputed, remove entries that are not PSP
facts, and make the remaining provenance visible per entry.

**Step A — remove only the 8 sentinels, and prove consumer behavior first.**

Consumer proof, measured across the whole tracked tree (`git grep sr_nid_name`): the table has
**exactly two consumers**, both in `src/rt/hle.c`, and both are `fprintf(stderr, ...)` diagnostics.

| site | context | on NULL |
| --- | --- | --- |
| `hle.c:7885` | HLE call log, consulted **only when no handler is registered** (`e ? e->name : sr_nid_name(nid)`) | prints `unknown`, still prints the raw NID |
| `hle.c:7891` | unimplemented-NID trap, reached only under `if (!e)` | prints `unknown`, still prints the raw NID |

Three consequences. Import resolution is unaffected — dispatch runs through `s_hle[]` and
`sr_syscall`, never through this table. Both sites already tolerate a NULL return. And the table is
consulted *only* for NIDs that have no handler, so removing the eight sentinels could only change a
message that would require guest code to invoke an emulator-internal identifier in the first place.

Removing them degrades no information: the raw hex is printed either way. This must be re-verified
against the exact head at implementation time, not taken from this document.

**Step B — carry an evidence class per retained entry.**
Restructure generation so each name records how it is known:

| class | meaning |
| --- | --- |
| `pspsdk-sourced` | name independently present in PSPSDK headers **and** hash-verified (840) |
| `hash-verified` | hash-verified, name currently PPSSPP-suggested, no second source (623) |
| `library-attributed` | export name unknown; entry records the owning library (54) |
| `editorial-alias` | firmware-range suffix on a base name that verifies in-table (10) |
| `unresolved` | no tested derivation reproduces the NID from the name (88) |

**Step C — derive the NID from the name wherever the name is retained.**
This is the actual provenance gain. For every hash-verified entry the table need not store the NID
at all: it is `sha1(name)[0:4]` and can be computed at generation time from the name alone. That
removes **1463 copied numeric constants**, leaving only the 160 NIDs that genuinely cannot be
derived — which then stand as the explicit, visible residue rather than being lost among the rest.
It claims nothing about the name list, which is exactly the point.

**Step D — do not delete the 54, the 10, or the 88.**
Failing the hash-name rule is not grounds for removal. The library-attributed entries carry real
library ownership, the aliases label real NIDs, and the unresolved entries are unproven rather than
wrong. All three groups are retained with their class recorded.

**Then:** retire `tools/gen_nidnames.py`'s requirement for a PPSSPP checkout, and keep the NOTICE
entry and the historical attribution. Classification after the change stays `behavior-informed`.

**Trap recorded during the audit:** any identifier hashes to *some* 32-bit value. The PSPSDK corpus
produced 617 `sce*`/`psp*` tokens absent from the table, overwhelmingly SDK-internal helpers
(`sceGuBoneMatrix`, `pspDebugSioDisableKprintf`) that are not firmware exports at all. The corpus may
only be used to **confirm** a pair already believed real. It must never mint new entries.

## IND-2 — VFPU register addressing from hardware — **COMPLETE (partial scope)**

HQ-1 ([#296](https://github.com/Jstar269/nakagawa-recomp/issues/296)) returned acceptance-eligible
real-PSP evidence, and it changed what this item *is*.

**Hardware confirmed the existing decode.** No production algorithm changed, and none should have:
"making it independent" by rewriting a decode that silicon says is already correct would have been
pure regression risk with no provenance gain. IND-2 became an **authority** change — what the
correctness rests on — not a rewrite.

### What was measured

PSP-3001 / 6.61-ARK, 2/2 byte-identical runs, PRX digest and source commit recorded.

| | |
| --- | --- |
| scalar encodings | **128 of 128** — all agree with `phys = ((E>>2)&7)*16 + (E&3)*4 + ((E>>5)&3)` |
| wide encodings | **14 of 512** |
| triple row selector | **bit 6 alone** (`E=0x20 → [0,4,8]` vs `E=0x40 → [1,2,3]`) |
| transpose | **wraps** as `(row+lane)&3` (`E=0x60 → [8,12,0,4]`); no saturation or aliasing |

### What it covers — and what it does not

It covers **`vreg_indices` / `vreg_idx` only**. Of the four addressing citations this item was
opened against, exactly one is now hardware-backed. The other three are **not**:

- `vreg_names` — a *different* index space (PPSSPP's `GetVectorRegs` naming, used for the VROT
  overlap quirk). Never probed.
- `mreg_index` / `mreg_idx` — matrix addressing. Never probed.
- the packed-size destination choice `oz_n` at `codegen.py:464` — it *calls* the measured function,
  but which size it passes was never probed.

**498 of 512 wide encodings were never observed on silicon.** They are covered only by derived
cross-implementation tests, labelled as such, which are not hardware evidence.

### What landed

- `fixtures/vfpu_addressing/hardware_vfpu_addr_001.json` — published results and provenance only.
  Raw captures stay in the gitignored private area, and a test asserts no capture path leaks in.
- `src/rt/vfpu_addr_selftest.c` — dumps the **production** `vreg_idx` across all 512 entries by
  including `vfpu_interp.c` and calling the real function, so a disagreement with the Python decoder
  is a genuine two-implementation divergence rather than a test agreeing with itself.
- `tools/test_vfpu_addressing.py` — 16 tests split into `HardwareAgreementTest` (tier H) and
  `DerivedConsistencyTest` (tier S). The split lives in the class names so the distinction survives
  someone reading only the test output.
- Source comments at both decode sites recording origin *and* authority as separate facts.

**The PPSSPP attribution is unchanged.** The code was written from `MIPSVFPUUtils.cpp` and still
says so; `vfpu_interp.c` remains `derived-translated`. An authority change for one function does not
re-author a file.

### Remaining

A future exhaustive 128×4 sweep can reuse the same probe and runner to close the other 498.
`mreg_index` would need its own probe.

## IND-4 — Measure sal063 retention — **COMPLETE**

Full record: [SAL063_RETENTION_2026-08-06.md](SAL063_RETENTION_2026-08-06.md).

**83.9% of sal063's code is still present** across 44 common source files (11,310 of 13,482
normalized lines); 40.1% of the current code in those files is shared with sal063.

The hypothesis this item was created to test — that the headers might be boilerplate — **was wrong
in every particular**. `h264_mf.c`, which the prior audit argued had "no plausible counterpart in a
PSP recompilation toolkit", retains 89.7% of an upstream file that already existed. `sched.c`
retains 79.8%. `osk_win.c`, `iso.h`, `ge_shared.h` and `interp.h` are at 100%.

Four consequences, all recorded as findings:

- **PROV-F6** — sal063 ships a detailed `CREDITS.md` that Nakagawa does not carry. It contains
  specifics `NOTICE.md` lacks, including a pinned PPSSPP revision for the sceMpeg port and a note
  that a since-removed `gpu_vk/` bridge reused PPSSPP's `GPU_Vulkan` wholesale (relevant to #102).
- **PROV-F7** — the recompiler pipeline is substantially inherited and declares nothing. This
  retracts the prior ledger's "overwhelmingly project-authored" claim.
- **PROV-F8** — Nakagawa added the standardized `Derived from` source-header form to the PPSSPP
  files. The earlier wording incorrectly conflated that header work with the underlying attribution:
  sal063's PGF source and `CREDITS.md` already identify `pgf.c`/`pgf.h` as a PPSSPP C port.
- `ge_shared.h`'s PROV-F1 contradiction resolves as a **chain**, not a conflict; `audio.c`'s does
  not and stays `unresolved`.

The asymmetry that governed the conservative call still holds and is worth keeping: over-attribution
costs accuracy, under-attribution costs a license obligation. When ambiguous, keep the attribution.

The PGF part of that formerly open work is now complete to recoverable public evidence. The
function-level PPSSPP/JPCSP/intraFont comparison and exact-revision bounds are in
[PGF_SOURCE_ARCHAEOLOGY_2026-08-08.md](PGF_SOURCE_ARCHAEOLOGY_2026-08-08.md); it confirms direct
translated/structural implementation lineage and does not create an independence claim. Other
PPSSPP-attributed units still require their own comparisons and must not inherit the PGF result by
analogy.

## IND-6 — Reconcile NOTICE.md with file headers — **COMPLETE**

Addressed findings PROV-F1, PROV-F6 and PROV-F7 together, **by adding to `NOTICE.md` and changing no
file header**. Also closes the acceptance criteria of [#304](https://github.com/Jstar269/nakagawa-recomp/issues/304).

What landed:

- The upstream's own attribution document is reproduced **verbatim** at
  [`THIRD_PARTY_LICENSES/SAL063_CREDITS.txt`](../../THIRD_PARTY_LICENSES/SAL063_CREDITS.txt) (7,320
  characters, content-identical). It uses the `.txt` extension its three siblings already use, which
  keeps it outside markdownlint's `**/*.md` glob — reformatting a third-party notice to satisfy our
  lint would defeat the point of carrying it, and this avoids touching the lint config at all.
- `NOTICE.md` gained an **`Upstream source file inventory (sal063-derived modules)`** table covering
  the recompiler pipeline and every other measurably inherited file, and naming the four tools
  verified absent upstream.
- The PPSSPP table gained the five files that declared derivation in-file but were missing from it:
  `recomp.c`, `recomp.h`, `savedata.c`, `evf.h`, `gpu_sdl3vk/ge_gpu.c`.
- The **pinned PPSSPP revision `4e109dd6`** for the sceMpeg port is now recorded, sourced from
  sal063's `CREDITS.md`.
- The lineage section states plainly that PPSSPP material reached this project *through* sal063, and
  that sal063 declares derivation in only one of its own files — so the per-file PPSSPP attributions
  here were added rather than inherited.

Two things were deliberately **not** resolved, because doing so would require guessing:

- **`audio.c`** keeps its PPSSPP attribution, now with an explicit caveat in `NOTICE.md` itself
  recording that sal063's `CREDITS.md` does not list it. Needs a PPSSPP source comparison.
- **`ge_shared.h`** is documented as a chain rather than picking a winner.

Still open and routed elsewhere: the `gpu_vk`/`GPU_Vulkan` history question goes to #102, and
qualified human review of the resulting notice presentation remains required before publication.

## IND-7 — Correct the stale "exact ports" claim

Finding PROV-F3. `src/rt/recomp.c:949` says the VFPU transcendental kernels are "exact ports of
PPSSPP's table-based kernels". The 2026-08-05 PSP-3001 capture proved they diverge from PPSSPP on
six of eight operations and that **hardware agrees with Nakagawa on all eight**.

The comment is stale in the direction of overstating derivation. Correcting it states origin,
divergence, and hardware backing together. It is not attribution retirement and must not be
presented as such: the kernel structure and the tables they index still originate upstream.

## IND-3 — Event-flag semantics from hardware

`src/rt/evf.h` is already the ideal shape: a pure header with no runtime dependency and a standalone
selftest, so the contract is isolated. Its only missing ingredient is a hardware capture of
wait/clear/mode precedence (**HQ-2**). Issue [#93](https://github.com/Jstar269/nakagawa-recomp/issues/93)
owns the behavior; coordinate rather than pre-empt.

## IND-5 — VFPU table regeneration feasibility (study only)

`assets/vfpu/*.dat` is 4.9 MB of PPSSPP-derived correction and delta data. [#282](https://github.com/Jstar269/nakagawa-recomp/issues/282)
authenticates it, which is good supply-chain hygiene and **does not make it independent**.

The open question is whether each table can be (a) deterministically regenerated from a documented
mathematical construction, (b) reconstructed from sufficiently dense hardware probes, or (c) replaced
by an independently generated equivalent that preserves PSP bit behavior exactly.

Study, do not implement. Two hard constraints:

1. **Bit-exactness is the requirement.** Do not modify a single byte to remove PPSSPP provenance.
2. If independent regeneration cannot be *proven*, the answer is "no" and the ledger says so.

The tractable part is validation rather than transmission: a PSP probe can digest an operation over
a large or exhaustive input sweep, so a candidate regenerated table can be proven equivalent without
ever moving table bytes off the device (**HQ-3**).

## IND-9 — Decompose `hle.c`

7575 LOC, classified `derived-translated` because a whole file takes the classification of its
most-derived content. Most of it is project-authored PSP ABI implementation; the derived parts are
specific and listed in the ledger record.

A whole-file rewrite is an explicitly **bad** candidate. The natural decomposition already exists:
the 19 routed kernel issues are the sub-unit boundaries, and each one that lands with hardware
evidence carves an independently evidenced unit out of the file. Independence here is a byproduct of
the existing correctness campaign, not a separate project.

## IND-10 — GE rasterizer (not a candidate)

`ge.c` is 3353 LOC with 34 citations naming specific upstream functions. It is the largest derived
unit in the tree and it has no independent behavioral corpus. A rewrite would be a
pseudo-clean-room exercise of exactly the kind [LEGAL_REWRITE_ASSESSMENT.md](../LEGAL_REWRITE_ASSESSMENT.md)
warns against. Recorded here so that it is visibly *decided against* rather than merely unstarted.

## Hardware-oracle questions

Small discriminating probes for the Freebuff PSPLINK lane. Each is scoped to one measurement, not to
reverse-engineering a subsystem.

### HQ-1 — VFPU register addressing decode

*Unblocks IND-2.* Filed as [#296](https://github.com/Jstar269/nakagawa-recomp/issues/296) with the
exact discriminating encodings and a two-stage method.

For a vector register encoding `E` (7 bits) and width `W`, which physical VFPU registers, in which
lane order, does the instruction touch? The decode currently used treats bits as
`matrix = (E>>2)&7`, `column = E&3`, `transpose = (E>>5)&1`, with the row selector varying by width:
`(E>>5)&3` for single, `(E>>5)&2` for pair and quad, and `(E>>6)&1` for triple.

Two spots are worth a probe rather than a full sweep, because a plausible alternative decode gives a
different answer only there:

1. **Triple width.** Encodings where `(E>>5)&1` and `(E>>6)&1` disagree — does the row selector use
   bit 6 alone, as assumed?
2. **Transpose with wraparound.** Encodings where `row + lane >= 4`, so the `&3` wrap is exercised
   under `transpose = 1`.

Suggested probe: write 128 distinct float bit patterns into the register file, execute one
`vmov`/`sv` at the encoding under test, and report which values appear in which destination lanes.
Roughly 16 encodings per width is enough to discriminate; a full 512-case sweep is welcome but not
required.

### HQ-2 — sceKernelEventFlag precedence and state mutation on failure

*Unblocks IND-3, supports #93.*

Three questions, all scalar:

1. When both an invalid mode bit **and** a bad UID are supplied, which error wins?
2. On a failed `sceKernelWaitEventFlag`, is the caller's `outBits` pointer written at all?
3. With `PSP_EVENT_WAITCLEAR` and a wait that times out, is the pattern cleared anyway?

### HQ-3 — Bulk transcendental digest over a wide input sweep

*Unblocks the validation half of IND-5.*

The existing VFPU oracle covers 46 inputs. To decide whether a regenerated table is bit-equivalent
we need coverage, but table bytes must never leave the device. Ask instead for block digests: sweep
an operation over a dense input range and emit one digest per block of the input space.

Start with `vsin` over `2^24` sampled patterns to establish timing and record shape. If that is
tractable, the exhaustive `2^32` sweep is a long-running but bounded capture, and a candidate table
can then be proven equivalent by digest comparison alone.

### HQ-4 — Savedata SIZES error precedence

*Supports IND-8 and #91.*

`ERR_SIZES_NO_DATA (0x801103C7)` on a SIZES query against a save that does not exist is currently
believed only because PPSSPP does it. What does the real utility module return, and does it write
the size fields before returning the error?

## Non-goals

Recorded so they stay decided rather than drifting back in:

- Rewriting PPSSPP-derived code to make it *look* original.
- Removing attribution because current code has changed.
- Generating replacement fonts to close #99.
- Writing an "independent" PGF parser as a way around #98. That is a legal question.
- Modifying `assets/vfpu/*.dat` for provenance cosmetics.
- Any claim of "clean room" or "zero PPSSPP influence".
