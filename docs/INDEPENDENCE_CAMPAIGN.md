# Independence campaign — engineering plan

Status: plan — nothing below is implemented unless marked.

This is the public-safe engineering plan for reaching the owner goal: a
definitive PSP static-recompilation platform built from exclusively original
code wherever possible, with as much low-level emulation (LLE) as makes
sense. It performs no legal clearance, states no legal conclusion, and changes
no classification by itself. Classifications live in
`assets/public_provenance_ledger.json`; the vocabulary and the rules for
changing a classification live in
`docs/provenance/INDEPENDENCE_MODEL.md`. Nothing here overrides either.

Base of this plan: public `main` at `6a4f359` (the mission statement named
`a36c296`; `origin/main` has since advanced past it through the LLE Phase 1
PR 1 landing and two follow-ups — all three are present in this checkout).
The LLE Phase 1 stack (`CpuState` ABI v2, COP0/exceptions, domain modes +
import seam) was read from its PR branch while PR 1 alone had landed. PR 1
(#217), PR 2 (#219) and PR 3 (#223) have all since landed on `main`.

Companion: `docs/cleanroom/SCHED_SPEC.md` is the first clean-room behaviour
specification produced under this plan's protocol (scheduler, ranked first
below). The flagship title named in the mission (Hot Shots Tennis: Get a
Grip, UCUS-98701) is a private-inputs-only acceptance target: no title bytes,
addresses, saves, captures, or run details appear in this plan.

## 0. Terminology

- **Derived code** below means the ledger class `upstream_derived` (file
  headers say `Derived from sal063/...` and/or `Derived from PPSSPP`), which
  covers both `derived-translated` and `derived-data` in the
  `INDEPENDENCE_MODEL.md` vocabulary. The model vocabulary is the target
  language: each campaign item ends in `project-authored-independent`,
  `behavior-informed` (kept only where explicitly bounded), or a
  clearly-bounded optional/excluded component.
- **LLE path** means executing original guest code (game, middleware PRXs,
  firmware modules supplied by the user) on the recompiled/interpreter
  platform, with host code only at genuine hardware/OS boundaries, per
  `docs/LLE_FIDELITY_ARCHITECTURE.md`.
- **Line counts** are `wc -l` at the base commit; they size the work, not its
  difficulty.

## 1. Claim verification — what the ledger and headers actually say

The mission's upstream list was checked path by path against the public
ledger, file headers, and the public-source profile. It is mostly accurate
for files present in the public tree, with the corrections below. The list
was also incomplete; additions follow.

Verified present-and-derived (header + ledger agree, immediate upstream in
parentheses):

- Recompiler pipeline `tools/prxload.py`, `tools/imports.py`,
  `tools/analyze.py`, `tools/codegen.py`, `tools/codegen_gate.py`
  (sal063; record `recompiler-pipeline-inherited`, tier S). Microtest and
  diff tooling in the same record: `tools/gen_microtest.py`,
  `tools/microtest_gate.py`, `tools/funcdiff_cmp.py`, `tools/tracediff.py`,
  `tools/vfpu_fuzz_gen.py`, `tools/nidseq.py`, `tools/ppm2png.py`,
  `tools/ppmdiff.py`.
- `src/rt/sched.c` (sal063; record `scheduler`, tier H).
- `src/ref/*` — all five files (sal063; record `reference-interpreter`,
  tier S).
- `src/rt/gpu_sdl3vk/*` — backend sources, headers, GLSL, and checked-in
  shader embeds (sal063; record `ge-vulkan-backend`, tier R). Note the
  header/body nuance: `ge_gpu.c` describes itself as an original,
  AI-assisted Vulkan backend that reproduces GE pixel rules derived from
  PPSSPP's software renderer via `ge.c` — i.e. original expression over
  derived behaviour, recorded honestly as `upstream_derived`.
- `src/rt/gui.c`, `src/rt/osk_win.c`, `src/rt/driver.c`,
  `src/rt/funcdiff.c` (sal063; record `host-shell-inherited`, tier N).
- `src/rt/iso.h` (sal063; record `iso-vfs-header`, tier N). The
  implementation `src/rt/iso.c` is publication-excluded (see below).
- `src/rt/h264_mf.c` (sal063; record `media-decode-mediafoundation`,
  tier S).
- `src/rt/vfpu_fuzz.c` (sal063; record `vfpu-differential-fuzz`, tier R).
- `src/rt/recomp.c`, `src/rt/recomp.h` (PPSSPP lineage through sal063;
  record `cpu-core-runtime`, tier H; upstream paths name PPSSPP
  `Core/MIPS/MIPSInt.cpp` unaligned handling and `MIPSVFPUUtils.cpp`
  prefix/transcendental helpers).
- `src/rt/hle.c` (PPSSPP lineage; record `hle-core`, tier N; names
  `sceKernelThread`, `sceCtrl`, `sceFont`, and related PPSSPP sources).
- `src/rt/ge.c`, `src/rt/ge_shared.h` (PPSSPP lineage; record
  `ge-software-rasterizer`, tier N; names `GPU/Software/*`,
  `GPU/Common/VertexDecoder*`, GE command headers).
- `src/rt/vfpu_interp.c` (PPSSPP lineage; record `vfpu-interpreter`,
  tier R; names `MIPSVFPUUtils.cpp`, `MIPS.cpp` `vcst` constants).
- `src/rt/savedata.c` (PPSSPP lineage; record `savedata`, tier N).
- `src/rt/mpeg.c` (PPSSPP lineage; record `mpeg-psmf`, tier N;
  `Core/HLE/sceMpeg.cpp`).
- VFPU tables `assets/vfpu/*.dat` (15 files, class
  `generated_from_public_source`, source family PPSSPP, exact pin
  `f0baf3ade7bcb6c86f0835962b36eb4e51559d8f` with blob IDs in
  `assets/vfpu/PROVENANCE.json`).

Corrections to the mission list:

1. `src/rt/iso.c`, `src/rt/audio.c`, `src/rt/pgf.c`, `src/rt/pgd.c`,
   `tools/pgd_decrypt.py`, `src/rt/evf.c`, `src/rt/fp_convert.c` are
   **absent from the public tree**. The public-source profile
   (`assets/public_source_profile.json`, `exclude_paths`) excludes them;
   the public build links project-authored seams instead
   (`iso_unavailable.c`, `audio_unavailable.c` and `pgd_unavailable.c`, all
   `project_authored_attested`, and the clean-room PGF reader `pgf_public.c`,
   #349). They remain
   campaign items — a platform without ISO/VFS, audio, fonts, and
   install-data handling is incomplete — but the work is clean-room
   admission of *new* files against the already-public seam headers
   (`iso.h`, `pgf_api.h`, `pgd_api.h`), not replacement of public files.
2. "Audio mixer from PPSSPP" is imprecise. The excluded `audio.c` is
   recorded as a materially modified **sal063** derivative (profile
   rationale); its *deeper* PPSSPP layer is explicitly unresolved
   (`docs/provenance/MODIFIED_FILE_NOTICES.md`: `src/rt/audio.c` is the one
   unresolved deeper-provenance path in that campaign).
3. `guest_interp` (`src/rt/guest_interp.c`/`.h`), `evf` (`src/rt/evf.h`),
   `fp_convert` (`src/rt/fp_convert.h`) are **not derived code**. They are
   `project_authored_attested` with `upstream_attribution: ppsspp` — i.e.
   `behavior-informed` in model vocabulary (tiers R, R, S respectively).
   Two caveats: `evf.h`'s own header still carries a `Derived from PPSSPP`
   line while its ledger entry claims project-authored expression (a
   header/ledger mismatch the campaign must resolve, not by deleting the
   comment but by earning the replacement citation per the model rules);
   and the `guest_interp` record (`daybreak4-guest-interpreter`) was
   **unbacked** until the maintainer promoted the existing human-authored
   record into the trusted authority; it is backed as of 2026-09-24 (#332,
   `docs/provenance/GUEST_INTERP_ATTESTATION.md`).

Additions the mission list missed (all `upstream_derived` in the ledger):

- `src/rt/atrac3p/*` — ~60-file FFmpeg n4.4 ATRAC3+ decoder subset
  (LGPL-2.1-or-later) with a byte-exact blob manifest in
  `src/rt/atrac3p/PROVENANCE.md`. Dependency, not campaign code (see §3).
- `tools/codegen_gate.py` is listed in the mission; the same record also
  covers the other gates named above — all are in scope together.

## 2. Inventory

One row per rewrite unit. "Depends" names in-tree consumers (production or
test). "Tests today" names only tests that run without private inputs;
anything needing PPSSPP-captured traces or retail inputs is marked BLOCKED
accordingly. Sizes at base.

| # | Group (paths) | Upstream | Lines | Depended on by | Tests today | LLE relevance |
| --- | --- | --- | --- | --- | --- | --- |
| G1 | Scheduler `src/rt/sched.c` (+ API declared in `src/rt/recomp.h`) | sal063 | 3,331 | `hle.c` thread/wait handlers, `driver.c` boot/run, `recomp.c` dispatch, `guest_interp.c` AOT-gap floor, LLE cpu/domain layers (PR 6 consumes its fatal signal) | `sched-selftest` (`sched_selftest.c`, ~50 cases), `tools/test_sched_invariants.py`, `hle-thread-selftest`, `test_callback_correctness.py`, `test_intr_waits_matrix.py`, `production-smoke-gap` | Highest: scheduling is one of the permanent host boundaries (§5 of the fidelity doc); every LLE domain switch crosses it |
| G2 | ISO/VFS reader (excluded `src/rt/iso.c`; public backend `src/rt/iso_public.c` using `nk_iso`; seam `src/rt/iso.h` + `iso_unavailable.c` stub) | sal063 (excluded pending review); public backend project-authored | UNKNOWN in public tree (private history only) | `hle.c` IoFileMgr handlers, `driver.c`, production-smoke link, `nk_iso`/`xb` native helpers | `test_iso_parity.py`, `test_vfs_c.py`, `test_vfs_contained.py`, `test_extract_xb_*`, `xb_probe.py` (all against the seam + native helpers, not the excluded backend) | Highest: "program + ISO" route and transparent-archive VFS (§3.5 of the fidelity doc) |
| G3 | PRX/ELF loader slice `tools/prxload.py` (1,529) + `tools/imports.py` (377) | sal063 | 1,906 | `analyze.py`, `codegen.py`, Makefile `pipeline`, `hle_manifest.py`, title codegen plan | `test_prxload_relocations.py`, `test_prxload_psp_header.py`, `test_imports.py`, `test_import_audit.py`, `test_production_smoke.py` | Highest: authentic loader/linker/relocation is the LLE pipeline entry (§3.2); KIRK/decrypt stage is a new clean-room item beside it |
| G4 | PGF reader (excluded `src/rt/pgf.c`/`.h`; seam `pgf_api.h`) + project-owned PGF converter (new) + replacement payloads (new, excluded until review) | PPSSPP `Core/Font/PGF.cpp` lineage; payloads PPSSPP-camouflaged Ume/Adobe outlines | UNKNOWN in public tree | `hle.c` sceFont path (links `pgf_unavailable.c` today), guest `libfont.prx` metric-compatibility work | None against a backend (seam has no backend in public); converter/payload requirements in the font-origins record (route 2) | High: flagship UI text; LLE runs guest `libfont.prx` but host HLE + fallback need an honest reader; converter must be clean-room (pgftool has no licence — must not be adopted) |
| G5 | VFPU interpreter `src/rt/vfpu_interp.c` (681) + PPSSPP-origin tables `assets/vfpu/*.dat` + project-authored loader `vfpu_tables.c` | PPSSPP (`MIPSVFPUUtils.cpp`, `vcst`) | 681 + 15 data files | codegen VFPU fallback emission, `vfpu_fuzz.c` differential harness, production interpreter fallback | `vfpu-interp-selftest`, `vfpu_tables_selftest`, `test_vfpu_*` (nan_payload, synth_corpus, oracle, provenance, table_manifest, trace_bits), hardware Group A/B cells (measured, indexed in `HARDWARE_ORACLE.md`) | High: VFPU executes as guest code under LLE, but every host evaluation (fallback, fuzz, oracle comparison) needs independently established semantics |
| G6 | Reference interpreter `src/ref/*` (5 files) | sal063 | 1,650 | `microtest_gate.py`, `codegen_gate.py` (`oracle=interp` mode), funcdiff flows; **not** linked into any game executable | `src/ref/selftest.cpp`, gates (need PPSSPP traces → BLOCKED without; `oracle=interp` self-modes run) | Medium: differential oracle for the pipeline; partly superseded by source-owned cosimulation (AOT vs production interpreter) |
| G7 | Analyzer + codegen `tools/analyze.py` (2,473) + `tools/codegen.py` (2,468) + gates/tooling slice (~1,395) | sal063 | ~6,336 | Makefile `pipeline`, all generated C, `codegen_gate.py`, cosim fixtures, title codegen plan | `test_analyze_*`, `test_codegen_*` (continuations, entry, fp_convert, madd/msub, vfpu_fallback, transfer timing…), `production-smoke`, `cosim-selftest` + `cosim-mutants`, `test_nid_name_proof.py` | Highest: the translation core is the "definitive static-recompilation platform" claim; codegen gates are LLE-touched (ABI v2, flow metadata) |
| G8 | CPU core runtime `src/rt/recomp.c` (2,364) + `src/rt/recomp.h` (827) | PPSSPP lineage (unaligned LWL/LWR/SWL/SWR, VFPU prefix/transcendentals) | 3,191 | Every generated translation unit, all of `src/rt`, `src/ref`, LLE `cpu_lle`/`domain_mode` layers | `test_dispatch_c.py`, `test_dispatch_call_boundary.py`, `test_dispatch_fatal_policy.py`, `dispatch-selftest`s, cosim four-channel comparison, `test_codegen_fp_convert.py` | Highest: load-bearing ABI; LLE PRs 1–3 already modify it — collision ground, not first-mover ground |
| G9 | HLE core `src/rt/hle.c` | PPSSPP lineage (`sceKernel*`, `sceCtrl`, `sceFont`, `sceGe`, …) | 11,733 | Generated-code `sr_syscall` dispatch, all title boot paths, `hle-thread-selftest` | `hle-thread-selftest` (game-input-free NID-driven harness), `test_hle_*`, `test_compat_manifest.py` (live census), production-smoke (real NID dispatch) | Highest — but direction is *replacement*, not rewrite: per-domain LLE guest execution (PR 3 seam) shrinks this file module by module |
| G10 | Software GE rasterizer `src/rt/ge.c` (3,604) + `src/rt/ge_shared.h` (226) | PPSSPP (`GPU/Software/*`, `GPU/Common/VertexDecoder*`) | 3,830 | `gpu_sdl3vk` (consumes screen-space primitives via hooks), `ppmdiff.py` A/B, `ge_capture`/`ge_replay` | `ge_capture_selftest`, `gpu_capture/coherence-selftest`s, `test_ge_capture.py`, A/B snapshot agreement (local consistency only, never PSP truth) | Medium: host must present pixels regardless; software path is the comparison oracle, Vulkan path is the presentation path |
| G11 | Vulkan backend `src/rt/gpu_sdl3vk/*` (sources + shaders + embeds) | sal063 record, tier R (original-expression-over-derived-behaviour; see §1) | ~6,300 | Window/input/presentation ownership, `gui.c` forwarding, `SR_GPU_GE=1` runs | `gpu-coherence-selftest`, `gpu-capture-selftest`, `test_native_host_backends.py`, `test_shader_embed.py` | Medium: IS the hardware-domain boundary (submit/present stay host-side under LLE); pixel rules inherit G10's disposition |
| G12 | Media `src/rt/mpeg.c` (623, PPSSPP `sceMpeg.cpp` port) + `src/rt/h264_mf.c` (497, sal063 MF glue) + `src/rt/savedata.c` (1,093, PPSSPP SavedataParam lineage) | PPSSPP / sal063 | 2,213 | `hle.c` sceMpeg/sceUtility/sceVideocodec handlers; `h264_null.c` is the project-authored null path | `test_mpeg_h264_bounds.py`, `test_savedata_security.py`, `test_savedata_spans.py`, `atrac3p-selftest`s (decoder side) | High for mpeg (LLE runs guest PSMF, host decodes at the `sceMpeg`/`sceVideocodec` boundary per fidelity §3.4); low for savedata (host-filesystem mapping is inherently host code) |
| G13 | Host shell `src/rt/driver.c` (558) + `funcdiff.c` (157) + `gui.c` (349) + `osk_win.c` (96); VFPU fuzz harness `src/rt/vfpu_fuzz.c` (350) | sal063 | 1,510 | Final link of every title executable; verification flows (funcdiff, fuzz) | `test_native_driver_hardening.py`, `test_ref_run_elf_hardening.py`, production-smoke (real driver + registration table), fuzz harness runs | Low: host glue with no PSP semantics; ideal parallel clean-room filler |
| G14 | PGD/amctrl (excluded `src/rt/pgd.c`/`.h`, `tools/pgd_decrypt.py` + pgd tests) | excluded pending qualified provenance/distribution review | UNKNOWN in public tree | `pgd_api.h` seam (`pgd_unavailable.c` linked) | Excluded tests only (`test_pgd_*`, harness — all excluded) | None today: stays excluded; see §4 disposition (c) |
| G15 | Behaviour-hardening (no rewrite): `evf.h` (64), `fp_convert.h` (360), `guest_interp.c`/`.h` (704) | project-authored expression, PPSSPP-consulted (tiers R/R/S) | 1,128 | `hle.c` event-flag + float paths; AOT-gap execution floor; cosim lane | `evf_selftest` + `test_evf_c.py`, `fp-convert-selftest` + `test_fp_scalar_mutations.py`, cosim suite + `test_cosim_fixture.py` | Medium: `fp_convert` is nearest to independent (PSPAutotests pin + MIPS32 rationale already in its header); `guest_interp` blocked on the maintainer attestation item (§1.3) |

## 3. Third-party dependencies — keep, with reasons

These are ordinary dependencies or vendored components with retained
notices, not derived code. None should be replaced by original code:

| Dependency | Position | Verdict and why |
| --- | --- | --- |
| FFmpeg n4.4 ATRAC3+ subset (`src/rt/atrac3p/`, LGPL-2.1-or-later, byte-exact manifest) | Vendored decoder for ATRAC3plus audio | **Keep.** A working ATRAC3+ decoder is codec mathematics plus a maintained standard library; rewriting it gains no independence (the format is Sony's regardless) and loses a pinned, auditable, licence-clean implementation. The project wrapper (`atrac3p_bridge`) is already project-authored. A future move from vendoring to dynamic FFmpeg linkage is packaging hygiene, not an independence item. |
| PSPSDK / PSPDEV headers and fixtures (BSD-family notices; `assets/upstream/`, `THIRD_PARTY_LICENSES/PSPSDK.txt`) | External ABI facts + local verification fixtures | **Keep.** Public ABI declarations are facts (names, numbers, layouts), not protectable expression; the tree already enforces identity by pin (`pspdev.lock.json`, `pspsdk_sync.py`). Replacing them would *remove* the independent anchor the clean-room specs cite. |
| SDL3 (zlib) | Host window/input/audio abstraction | **Keep.** Host-platform abstraction is exactly what the portability rules require (`AGENTS.md`, `PLATFORM_PORTABILITY.md`); a project-owned OS layer per platform would be larger, worse-tested, and no more independent. The excluded `audio.c` SDL backend is still rewritten as original SDL3 API use (G2-adjacent audio item), which is not the same as replacing SDL. |
| Vulkan loader/headers (Apache-2.0; SDK not redistributed) | Host GPU submission | **Keep**, same reason as SDL3. The independence-relevant code is what is *submitted* (GE semantics, G10/G11), not the submission API. |
| Microsoft Media Foundation (OS component) | `h264_mf.c` decode path | **Keep** as the OS facility; the campaign item is only the thin glue file (G12), rewritten from Microsoft's public API documentation. |
| System PGF tooling (`pgftool`, `ttf2pgfj.exe`) | Named in upstream font history | **Must not adopt.** `pgftool` has no licence file (terms UNKNOWN) and the converter binary is of unknown provenance (font-origins record §4). The project-owned converter (G4) is specified from the PGF format and metric targets, never from these tools. |

## 4. Target architecture per group

Verdicts: **(a)** clean-room rewrite of the same component · **(b)**
replacement by an LLE path that makes the component unnecessary ·
**(c)** keep as a clearly-bounded optional/excluded component.

- **G1 scheduler → (a).** Scheduling is a permanent host boundary: a guest
  cannot schedule itself on a host. The rewrite specifies guest-visible
  semantics (lifecycle, priority, waits, wakes, interrupts, time) from
  PSPSDK declarations, the frozen threading research, and hardware-oracle
  cells; host mechanism (fibres/contexts, timeslice accounting) stays free.
  First spec is `docs/cleanroom/SCHED_SPEC.md`.
- **G2 ISO/VFS → (a).** ISO9660/UMD parsing and the transparent container
  VFS are fully specifiable from public disc-format facts plus the
  project's own container format; no upstream behaviour is load-bearing.
  The existing `iso.h` seam bounds the work.
- **G3 loader slice → (a).** ELF32, `~PSP`/`~SCE` containers, and MIPS
  relocation application are public-format facts; NID/import-table
  resolution cites PSPSDK structures and the project's NID corpus
  (`tools/nid_corpus.json`, IND-1). Any decryption capability is outside
  this campaign pending the maintainer's legal decision (#295).
- **G4 PGF → (a) + (a).** Both the publishable reader (against `pgf_api.h`,
  from the PGF format and the font-origins metric targets) and the
  project-owned converter (from pinned OFL/Ume inputs, deterministic
  byte-identical rebuilds, honest names, shipped licence texts) are
  clean-room items per the font-origins route decision. Payloads stay
  excluded until every admission condition in that record holds.
- **G5 VFPU → (a) + (b)-for-data.** The interpreter is rewritten from the
  Allegrex VFPU operation definitions with computed (not table-copied)
  transcendental kernels; the PPSSPP-origin `.dat` files are replaced by
  computed values validated against hardware-oracle Loop A fuzz
  (bit-exactness is the requirement, so the tables are *regenerated or
  re-reviewed*, never cosmetically relabelled — model rule 1). The loader
  (`vfpu_tables.c`) is already project-authored and stays.
- **G6 reference interpreter → (a), scoped.** A clean-room Allegrex
  integer/FPU interpreter specified from MIPS architecture documentation
  and hardware-measured cells becomes a genuinely independent second
  oracle. Scope is deliberately the *verification-relevant* subset the
  gates execute (user-mode integer/FPU/VFPU-compute, fail-closed
  otherwise), not an all-Allegrex implementation — mirroring the production
  interpreter's documented floor (#118) so the two floors stay comparable.
- **G7 analyzer + codegen → (a), staged.** `prxload`/`imports` behaviour is
  format facts; analysis (function discovery, CFG ownership) is specified
  from observable machine-code structure; codegen emission is specified
  from the `CpuState` ABI header plus per-instruction semantic contracts
  shared with G6/G8. Staged loader-first because downstream tests
  (`production-smoke`, cosim) pin behaviour incrementally.
- **G8 CPU core runtime → (a), coordinated.** Guest-memory helpers,
  unaligned-access semantics, prefix application, and dispatch support are
  rewritten from MIPS/PSPSDK facts and hardware cells. The `CpuState`
  *layout* itself is a functional interface (LLE PR 1 versioned it as ABI
  v2 with offset tests), so layout stability is kept while expression is
  replaced. Sequenced with — not against — LLE PRs 4–9 (see §7).
- **G9 HLE core → (b), staged by domain.** The PR 3 import seam already
  routes generated stubs through a domain table defaulting to HLE. Each
  LLE phase moves one library family (THREADMAN, IO, GE, AUDIO, MEDIA,
  INTC, TIMER) to guest execution of real firmware modules; the
  corresponding `hle.c` handlers shrink to the documented host-boundary
  remainder (host I/O, presentation, audio buffering). What remains at the
  end — dispatch registry, NID tables from the owned corpus, host-boundary
  handlers — gets an (a) rewrite. Title-specific behaviour is never
  reintroduced (title-profile architecture holds throughout).
- **G10 software GE → (a), late.** Specified from the GE command encoding,
  PSPSDK graphics declarations, and hardware-measured pixel cells
  (homebrew GE lists through the resident runner — no retail frames).
  Late because it needs the pixel-measurement corpus first, and because
  its current role (comparison oracle) is load-bearing for every visual
  check until then. Interim disposition is bounded-(c): explicitly the
  non-oracle comparison path, never cited as PSP truth.
- **G11 Vulkan backend → (a), after G10.** Resubmission of
  independently-established GE semantics through public Vulkan/SDL3 APIs
  (Khronos and SDL documentation are the citations). No independent GE
  semantics, no rewrite — sequencing is a dependency, not a preference.
- **G12 media → split.** `mpeg.c` → (b): guest PSMF middleware runs as
  guest code; the host keeps only packet handoff at the
  `sceMpeg`/`sceVideocodec` boundary plus host decode (FFmpeg/MF), per
  fidelity §3.4. `h264_mf.c` → (a) micro-rewrite from Microsoft's public
  Media Foundation documentation (thin glue, small blast radius).
  `savedata.c` → (a) rewrite from PSPSDK utility declarations and the
  observable memstick layout (host mapping is inherently host code).
- **G13 host shell + fuzz harness → (a), anytime/parallel.** No PSP
  semantics involved: window/input/dialog/presenter code from SDL3/Win32
  documentation; `funcdiff.c`/`vfpu_fuzz.c` harnesses from the project's
  own trace-format and selftest contracts. Ideal filler while LLE-heavy
  items wait on hardware cells.
- **G14 PGD/amctrl → (c) keep excluded.** No code work until the qualified
  provenance/distribution review it is waiting on completes. A review
  packet exists outside the public tree; the public seam stays
  fail-closed. Any future item here needs maintainer + qualified review,
  not just this plan.
- **G15 behaviour-hardening → evidence, not rewrite.** `evf.h`: resolve
  the header/ledger mismatch by replacing the PPSSPP cross-check citation
  with Loop C event-flag cells + PSPSDK declarations, then drop the stale
  header line together with the citation change (model rule: earn first).
  `fp_convert.h`: closest to done — PSPAutotests pin + MIPS32 rationale
  already in-header; remaining work is confirming the exact pin's licence
  permits the use and recording hardware corroboration where Loop A/B
  covers it. `guest_interp`: maintainer-only attestation promotion (§1.3);
  engineering work is already project-authored.

## 5. Clean-room protocol for AI-agent work

Binding on every (a) item. It implements `INDEPENDENCE_MODEL.md` Phases
A–E with the AI-specific controls the model demands (upstream source in
the authoring context must be recorded; paraphrase is derivation whoever
typed it).

### 5.1 Session separation

1. **Spec session.** May read the derived implementation, the ledger, issue
   history, and any upstream needed to understand *what behaviour matters*.
   Must output only a behaviour specification under `docs/cleanroom/`:
   prose + tables, **no code** (no function bodies, no struct
   initialisers, no pseudocode with control flow), identifiers only where
   they are ABI (Sony NIDs, PSP error codes, GE command encodings, ELF
   constants — all external facts) or where the spec itself defines a new
   project seam identifier (marked `SEAM:`). Every behavioural fact cites
   one of: a public document URL, a public header (PSPSDK pin, MIPS
   documentation, OS vendor API docs), a hardware-oracle record
   (envelope ID + console route), an in-tree source-owned test or fixture,
   or `project decision` (explicitly marked, with rationale). The spec ends
   with the black-box test plan the implementation session receives.
2. **Implementation session.** Receives *only*: the spec, the ABI headers
   it must satisfy (named by path), and the black-box tests. It must not
   receive the derived files, upstream URLs beyond the spec's citations,
   or repository history. Enforced by workspace construction (§5.2), not by
   asking nicely.
3. **Similarity check before admission.** Proposed
   `tools/similarity_check.py` (design, not yet built — building it is a
   docs-allowed tooling task for a later session): token-sequence and
   line-overlap comparison of the candidate against the removed derived
   file(s) plus pinned upstream blobs where available. Suggested admission
   thresholds (to be calibrated on G1 before they bind): normalised
   token-trigram overlap below 0.15 excluding the spec's `SEAM:`/ABI
   identifier list and `#include` lines; no identical 6-line run outside
   cited-ABI tables; identical constant *ordering* in tables above 8
   entries fails pending justification. The check is evidence for review,
   not a verdict — a pass never authorises a classification change by
   itself.
4. **Provenance record + maintainer attestation.** The implementation lands
   with: classification `project-authored-independent` proposed in the
   ledger entry, `behavior_sources` listing the spec and every citation
   class used, an explicit `upstream source consulted / deliberately not
   consulted` statement, the similarity report, the black-box test results,
   and hardware-oracle envelope IDs where applicable. The maintainer
   attests authorship process and classification; per `AGENTS.md` §3–4 no
   agent invents attestation, sign-off, or lineage.

### 5.2 Clean workspace construction (exact)

On a host with the repository checked out at the pinned base, with
`CLEANROOM_DIR` set to a new, empty directory outside any checkout:

```text
git archive <base> <spec> <abi-headers...> <tests...> <build-files...> \
  | tar -x -C "$CLEANROOM_DIR"/
```

i.e. export *only* the allow-listed paths (spec, named headers, named
tests, and the minimum build files to compile them) into an empty
directory — no `.git` (so no history, no private objects, no excluded
files that happen to be present locally), then delete any derived file
that the allow-list accidentally includes (deletion list recorded in the
spec's §"workspace allow-list"). The implementation session works only in
that directory. Verification that the workspace is clean: `git status`
must fail (not a repository), `grep -ri <upstream-path-fragment>` over the
directory must be empty, and the file inventory (relative paths + SHA-256)
is committed alongside the result as the clean-room evidence link. The
similarity check runs outside the clean workspace, at admission time, so
the implementation session never sees the derived text it is measured
against.

### 5.3 Behaviour verification hierarchy (binding)

1. Hardware-oracle measurements outrank everything.
2. Source-owned black-box tests (named in the spec) are the admission
   gate.
3. Differential comparison against the old implementation is allowed
   **only as a black-box oracle**: same inputs, compare guest-visible
   outputs, no inspection of the old code to explain a divergence beyond
   localising it. A divergence localised this way is resolved by
   re-reading the spec and the cited sources — never by reading the old
   implementation into the clean workspace.
4. Agreement between two project implementations (e.g. new scheduler vs
   old scheduler, software vs Vulkan GE) is tier R: it localises, it never
   establishes PSP truth.

## 6. Sequencing

Ordered by LLE leverage × legal risk × dependency position × test
coverage. Effort figures are *estimates* (session counts for an
AI-agent + maintainer-review loop, speculative by nature).

| Order | Item | Why now | Estimate |
| --- | --- | --- | --- |
| 1 | G1 scheduler (spec in this mission) | Strongest source-owned tests in the tree; every LLE domain crosses it; PR 6 needs its fatal-signal consumption defined — the spec fixes that interface before PR 6 lands | Spec done; impl 1–2 sessions; integration/hardening 1–2 |
| 2 | G2 ISO/VFS reader | Unblocks the "program + ISO" route and every title ingest test; fully specifiable today (formats + own containers); seam already public | Spec 1 session; impl 1–2 sessions |
| 3 | G3 loader slice (prxload + imports) | LLE's authentic-loader work (§3.2) needs an owned loader; format facts, no hardware wait; unlocks G7 staging | Spec 1; impl 1–2 |
| 4 | G4 PGF reader + converter | Flagship UI text; reader is small (format + metric targets), converter is mechanical once inputs are pinned; both blocked only on spec writing, not on hardware | Reader spec+impl 1–2; converter spec 1, impl 2–3 (CJK coverage is the long tail) |
| 5 | G5 VFPU interp + computed tables | Needs Loop A hardware fuzz for full confidence, but computed-kernel work and the Group A/B cells already pin the frame; start spec now, gate admission on fuzz results | Spec 1; impl 2–3; admission waits on Loop A corpus (hardware-schedule dependent) |
| 6 | G6 reference interpreter (scoped) | Independent-oracle value; no production blast radius; feeds G7/G8 verification | Spec 1; impl 2 |
| 7 | G7 analyzer + codegen (staged after G3) | Largest, most entangled; LLE codegen gates keep moving it — stage loader-adjacent parts first, emission core after PRs 4–9 settle | Staged specs 2–3; impl 3–5 total |
| 8 | G8 CPU core runtime (with LLE PRs) | Same code the LLE lane is actively extending; rewrite tracks behind PRs 4–9, not ahead | After PR 8; 2–3 |
| 9 | G12–G13 media glue, savedata, host shell, fuzz harness | Small, parallelisable, no hardware waits; filler between hardware-gated items | 1 each, parallel |
| 10 | G10/G11 GE software + Vulkan | Needs the homebrew pixel-measurement corpus first; current oracle role must be replaced before the code, not with it | Corpus 2–4 (hardware-bound); rewrites 3–5 after |
| — | G9 HLE core | Not sequenced as a rewrite: shrinks via LLE domain takeovers; residual (a) rewrite scoped once the domain map stabilises | Per-domain with LLE phases |
| — | G14 PGD/amctrl | Excluded until qualified review; no engineering sequence to give | — |
| — | G15 hardening | Fast wins alongside: fp_convert licence-use confirmation, evf citation swap, guest_interp maintainer attestation | Days, maintainer-paced |

## 7. Interaction with LLE Phase 1 PRs 4–9

The LLE phase plan itself is not published in-tree (the fidelity doc states
doctrine and subsystem direction, not PR contents). The only PR 4–9 facts
established in-tree are from comments in the PR 3 headers: **PR 6** is
scheduler integration (top-level consumption of the seam's `SR_FLOW_FATAL`)
and **PR 8** is dual (HLE+LLE) execution. Everything else about PRs 4–9 is
undetermined from public sources — the collision rules below assume the
worst case (any of `recomp.h`, `guest_interp.*`, `codegen.py`,
`sched.c`-adjacent behaviour may move).

1. **Ownership rule** (from the independence model): do not rewrite a
   subsystem an active LLE PR owns. Concretely: `cpu_lle.*`,
   `domain_mode.*`, and the LLE-touched regions of `guest_interp.*`,
   `recomp.h`, and `codegen.py` are LLE-owned until their PRs land. The
   G1 spec therefore specifies scheduler behaviour *up to* the seam and
   defines the PR 6 consumption point as an interface requirement
   (`SEAM:`), without implementing LLE internals.
2. **G1 before PR 6.** The scheduler spec's fatal-signal and
   interrupt-delivery interfaces should land first so PR 6 implements
   against a specified seam rather than inventing one mid-rewrite.
3. **G8 tracks, never leads.** The CPU-core rewrite is explicitly sequenced
   after PR 8; any earlier (a) work there is limited to regions the LLE
   lane declares stable.
4. **G9 rides the domain map.** Each LLE domain takeover shrinks `hle.c`;
   the campaign does not spec HLE-handler behaviour the LLE lane is about
   to obsolete. The residual-rewrite scope is set once per domain, at
   takeover time.
5. **Shared evidence.** Loop A/B/C hardware corpora serve both campaigns;
   probe proposals are filed once and cited twice (LLE spec section +
   clean-room spec citation). The G5 admission gate (Loop A corpus) is the
   first joint milestone.

## 8. Hardware-oracle needs

Console route wherever hardware is named: PSP-3000-series, 6.61, ARK,
PSPLink (`HARDWARE_ORACLE.md` §6; the autonomy/runner design in
`HARDWARE_RUNNER_AUTONOMY.md` governs how probes run). Standing boundary:
**homebrew probes are committable evidence; anything involving the retail
UMD/game on hardware is private, permanently** (oracle scope discipline).
No title-run evidence appears in any clean-room record.

| Rewrite | Facts needed | Homebrew probe only? | Needs retail UMD on HW? | Notes |
| --- | --- | --- | --- | --- |
| G1 scheduler | Thread/wait/timeout edge semantics (already partly in frozen threading research + intr-conformance); callback-delivery ordering; delay/timeout remaining-time arithmetic | Yes — Loop C kernel probes | No. Title acceptance (private) validates, never specifies | Conformance suite + simulated transport exist; resident-runner isolation is the blocker for crash-class probes |
| G2 ISO/VFS | None from silicon (formats, not behaviour) | n/a | No | UMD sector-layout quirks, if any, are observed via homebrew reads of the user's own disc — still private handling, public facts only |
| G3 loader | Relocation edge cases; packed-Type-B forms; module-lifecycle argument blocks | Yes — homebrew PRXs exercising relocation types + `module_start` arg passing | No | Real firmware-module loading behaviour is observed with user-supplied modules locally; only format facts enter the spec |
| G4 PGF | PGF metric/layout edge cases (ascender/descender targets, glyph dimension caps); sceFont query semantics | Yes — homebrew sceFont probes against user-supplied flash0 fonts (fonts never enter the tree) | No | Pixel-comparison of rasterised glyphs is tier R; metric values are the committable facts |
| G5 VFPU | Full opcode/prefix/NaN/rounding/denormal matrix | Yes — Loop A fuzz (highest-value loop; bulk fuzz unbuilt, Groups A/B measured) | No | Bit-exactness required; one console is one data point — record route per envelope |
| G6 ref interp | Per-opcode semantics incl. Allegrex extensions | Yes — Loop B microtests (`gen_microtest.py` already emits the module shape) | No | Turns the microtest gate into a real gate; also serves G7/G8 |
| G7/G8 codegen + core | Translator edge cases (delay-slot control encodings, LWL/LWR/SWL/SWR merges, prefix application) | Yes — Loops A+B localise; cosim negative corpus fails closed without HW | No | |
| G9 HLE remainder | Per-domain kernel semantics where "PPSSPP does X" is not proof (callbacks, mutex/LwMutex, semaphores, async I/O, FPL/stack lifetime, interrupt pending) | Yes — Loop C bespoke probes | No. Retail-title HLE *sequences* stay private acceptance only | Display-mask/vcount cells already measured; VBLANK-sub-interrupt remainder open |
| G10/G11 GE | Pixel-exact command semantics (blending, dither, stencil-in-alpha, CLUT offsets, depth/fog, sampler edges) | Yes — homebrew GE lists through the resident runner; result vectors compared on host | No — retail frames/captures are game-derived and permanently private | Whole-game hardware tracing is explicitly not realistic; corpus is synthetic lists only |
| G12 mpeg | Stream-relative timestamp progression is media-domain, not wall time (already modelled); PES/NAL handoff validation | Homebrew-generated streams | No | Real title streams are private inputs |
| Private acceptance (all) | Flagship route advancement | — | Yes, private-only | Proves only its qualified route; never a specification source |

## 9. Exit criteria for "independent"

The word is earned per unit, never declared for the tree, and the tree
claim stays narrow per the model ("this unit's behavior is independently
established, and its expression is project-authored" — never "clean room"
or "zero influence" as a blanket).

Ledger end-state (checked mechanically):

1. No `upstream_derived` record remains under `src/` or `tools/` except
   entries explicitly carrying a bounded-(c) disposition with an expiry or
   review condition (G10-interim, G14) — and every (c) entry must name the
   exclusion or bound that contains it.
2. `guest_interp`/`evf`/`fp_convert` carry `upstream_attribution: null`
   with tier H/S/A citations, or stay honestly `behavior-informed` with the
   consultation recorded — no silent upgrades (the merge gate's T5 already
   treats a claim change as unbacked without authority).
3. `assets/vfpu/*.dat` are regenerated-from-measurement or re-reviewed
   with the regeneration proof in `assets/vfpu/PROVENANCE.json`; the
   PPSSPP pin entry survives in history either way (Phase E: historic
   attribution is never deleted because current code is new).
4. `NOTICE.md`, `THIRD_PARTY_LICENSES/SAL063_CREDITS.txt`, and per-file
   SPDX/upstream lines keep telling the true lineage story, including for
   fully replaced files (replacement recorded, history preserved, notices
   retired only per the model, never by editing).
5. Dependencies (§3) stay declared with pins and notices; no dependency is
   mislabelled as project-authored.

Enforcing gate (proposed, to be built as tooling, not prose): extend the
trusted merge-gate posture with `LEDGER_DERIVED_ADMISSION` — the check
loads the trusted ledger at the base ref and the candidate ledger, and
fails when the candidate's `upstream_derived` set under `src/`/`tools/`
grows by any path (new file, relabel, or re-add), unless the delta carries
an explicit bounded-(c) exemption record signed by the same authority that
backs other provenance claims. Tightening (fewer derived paths) always
passes; loosening never passes without that authority. Until the tooling
exists, the same comparison is a manual review step with the two sorted
path lists pasted into the PR.

## 10. Open questions for the maintainer

1. LLE PRs 4–9 have no published plan in-tree; §7 assumes worst-case
   collision. Is there a publishable slice (per-PR file-ownership list)
   that would let the campaign sequence tighter?
2. `guest_interp` attestation (§1.3) is maintainer-only. Is the existing
   human-authored record promotable verbatim, or should the campaign
   schedule a from-spec re-verification of the AOT-gap floor first?
3. `fp_convert`'s PSPAutotests pin: confirm the licence permits the
   behavioural use and record the answer with the pin, or drop the pin
   citation to hardware-only evidence.
4. `evf.h` header/ledger mismatch: confirm the citation-swap approach
   (earn Loop C cells, then change header + ledger together).
5. PGF payload admission ultimately needs counsel answers (font-origins
   §8: OFL aggregation vs GPL program, RFN rename sufficiency, metric
   reproduction exposure, camouflage-name redistribution). Is counsel
   engaged, and does the G4 converter start before those answers?
6. PGD/amctrl (G14): is qualified review active, dormant, or out of scope
   for this campaign? The plan assumes dormant-but-open.
7. Threshold calibration for the similarity check (§5.1.3): accept the
   calibrate-on-G1 proposal, or set fixed thresholds now?
8. Are the session-count estimates in §6 the right unit, or should the
   campaign estimate in ledger records closed per quarter instead?

## 11. Lineage unknowns (could not be determined from public sources)

- Exact sizes of the excluded implementations (`iso.c`, `audio.c`,
  `pgf.c`/`.h`, `pgd.c`/`.h`, `pgd_decrypt.py`, and the historical
  `evf.c`/`fp_convert.c` from which today's headers descend): not
  measurable from the public tree; record from the private checkout at
  implementation time. They do not affect sequencing.
- Whether the `sched_*` flat-C API spellings originated with sal063 or
  earlier: believed project-coined (PPSSPP's kernel is C++ with different
  structure), but the sal063 pin `da17b0e` tree is not vendored here, so
  the similarity check at G1 admission must confirm no PPSSPP-identifier
  carryover rather than this plan asserting it.
- sal063-side deeper lineage for `host-shell-inherited` and
  `recompiler-pipeline-inherited` records (`upstream_paths` empty in the
  public ledger): the detailed ledger is outside the public tree by
  design; the campaign treats sal063 as immediate upstream throughout and
  PPSSPP-depth per file headers where present.
- `src/rt/audio.c` deeper PPSSPP layer: unresolved by design
  (MODIFIED_FILE_NOTICES.md); the G2-adjacent audio rewrite specs from
  SDL3/PSPSDK sources without resolving it, and the old file stays
  excluded either way.
- Exact Source Han Sans / Ume input releases behind the PPSSPP font
  payloads, pgftool terms, `ttf2pgfj.exe` provenance: UNKNOWN per the
  font-origins record; the G4 converter pins its own inputs instead of
  resolving these.
- LLE PR 4–9 contents beyond PR 6/PR 8 as named in PR 3 header comments:
  undetermined; §7 rules are written to survive any content.
