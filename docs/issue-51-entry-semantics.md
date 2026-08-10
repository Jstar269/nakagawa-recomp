# Issue #51 — callable function boundaries vs continuation / resume PCs

Status: **implementation complete on the draft branch; synthetic, private-build, and strict live Exhibition evidence through the rendered court/costume scene pass.** A first playable rally and external-oracle coverage remain. The failure model and design below are retained as the rationale for the implemented entry catalog and exit contract.

> Provenance note: this document records only structural facts — addresses, frame sizes,
> control-flow relationships, and classifications — derived from the private retail image. It
> deliberately contains no retail instruction listings or opcodes. The illustrative
> instruction sequences belong in the synthetic mini-ELF regression (our own code), not here.

## The defect

`tools/codegen.py` uses a single `known` set for every address it emits as a `void f_<addr>(CpuState*)`, and gives every such entry the **standalone-function frame contract**:

- at entry it captures the entry stack pointer (`_sp_entry = s->r[29]`, codegen.py:1115);
- at `jr $ra`, and at every other exit form, it forces `s->r[29]` back to `_sp_entry` (codegen.py:1421, :1430, :1435, :1462/1465, :1475, :1478).

That is correct for a real callable function: entered with the caller's SP, its prologue lowers SP and its epilogue raises it back to exactly the entry value, so forcing the entry SP on return is a redundant no-op.

It is **wrong** for an *interior continuation* — an address that begins execution inside an already-active guest frame (no prologue of its own) and reaches the owner's epilogue. There the captured entry SP is a *mid-frame* value; the guest's own epilogue restores the caller's SP correctly; and the synthetic restore then overwrites that correct value back to the mid-frame SP, corrupting SP by the owner's frame size on return.

The governing invariants:

1. **A valid place to resume execution is not automatically a callable function boundary.**
2. **A host entry needed for dispatch is not automatically a recovered source function.**

## Implemented result

- `EntryInfo` records independent callable/resumable roles, owner, and provenance.
- HST address-specific metadata is enabled only by the explicit `--profile=hst` build profile.
- callable source boundaries remain `f_<addr>`; resume host entries are distinct `r_<addr>` symbols.
- all three resume entries remain dispatch-registered, but never capture or restore `_sp_entry`.
- `function_flow()` keeps an owner's resume region native instead of truncating it as another callable.
- the hand-written `0x310b0` epilogue stub is retired; the ordinary translator now emits its guest path.
- catalog construction stops with `DUAL-ROLE ENTRY DETECTED` rather than guessing when address-only dispatch cannot choose a contract.

The owned synthetic ELF regression first reproduced the pre-fix failure (`resume_sp=0x00000f90` versus caller SP `0x00001000`), then passed after the implementation. It also covers the owner-native path, an adjacent callable, an address-taken tiny leaf, ordinary `jalr`, non-linking `jr` tail transfer, resume registration, and the dual-role stop invariant.

## Pre-fix evidence (structural, no listings)

- `f_00021c78` reaches its owner's epilogue, which restores a **0x70-byte** frame (loads saved `ra`/`s0`, then raises SP by 0x70); the generated standalone wrapper then overwrites SP with the mid-frame entry value. Owner: callable `0x00021ac0`, whose prologue lowers SP by 0x70.
- `f_000b26a0` has **no prologue** and, at entry, already addresses its owner's frame via `sp + 0x90` (a large frame is live); it later restores `s0–s7` and raises SP by **0xd0** in the owner's epilogue, after which the standalone wrapper overwrites SP with the mid-frame entry value. Owner: callable `0x000b237c`, whose **first instruction** lowers SP by 0xd0. (`0x000b2378` is not a function start — it is the preceding function's `jr $ra` delay slot, which raises that function's SP by 0x30.)
- `f_000310b0` never reached the generic path: it is a **hand-written stub** (codegen.py:1615) that manually restores the owner's saved registers and raises SP by 0x20 before returning. That workaround is the existing admission that the generic contract cannot express this entry. Owner: callable `0x00030fdc` (prologue lowers SP by 0x20); `0x310b0` is an alternate/resume entry that jumps into that owner's shared epilogue near `0x0003104c`.

## The three seeds, classified from the image

Each has **no conventional prologue** at the seed address and **zero direct incoming edges** (no `jal`, `j`, or branch anywhere in `.text` targets them) — they are reached only through computed/indirect control flow. So each needs a *dispatch entry* (or the indirect target misses) but must run with *resume* (continuation) semantics.

| Seed | Structural facts | Callable owner | Classification |
| --- | --- | --- | --- |
| `0x000310b0` | alternate entry: sets a result, jumps to a shared epilogue near `0x3104c` **inside its owner** | **`0x00030fdc`** (frame 0x20) | resume / alternate entry |
| `0x00021c78` | no prologue; mid-computation on live registers; flows to a shared epilogue restoring a 0x70 frame | **`0x00021ac0`** (frame 0x70) | interior continuation |
| `0x000b26a0` | no prologue; addresses `sp + 0x80..0xa0` at entry; restores `s0–s7` and a 0xd0 frame at exit | **`0x000b237c`** (frame 0xd0) | interior continuation in a framed routine |

`0x000310b8` (right after the `0x310b0` resume region) is **not** part of owner `0x30fdc` — it is a **separate adjacent callable function** with its own 0x20-frame prologue. It is the natural "adjacent independent callable" test case (below), not a second entry into the same function.

## Dual-role audit (issue #51 Part 6) — exclusivity is NOT assumed

**Question:** can one PSP PC be *both* a legitimate fresh callable entry *and* an interior/resume entry, depending on how it is reached?

**Finding:** for the three current seeds, no — each is resume-only in this image (no prologue, no direct callable edge). The cleanest multi-entry evidence here is **one function with two entries**: owner `0x30fdc` has its ordinary fresh-call entry *and* an alternate/resume entry at `0x310b0` into its own shared epilogue. That is two *distinct* PCs with different roles in one function — not a single PC that is both. Multi-entry and shared-tail idioms are ordinary in hand-written PSP assembly, so a single PC being reached with a fresh frame on one path and mid-frame on another is plausible in general and **cannot be proven impossible from this image** — but no such same-PC case is observed.

**Consequence for the metadata model:** do not encode a single mutually-exclusive `kind` per address. Use independent roles:

```python
@dataclass(frozen=True)
class EntryInfo:
    addr: int
    callable: bool           # a fresh-call entry: caller SP on entry, owns a frame
    resumable: bool          # valid to enter with the owner's frame already active
    owner: int | None        # the callable a resume entry belongs to (see owners note)
    provenance: frozenset    # direct-jal / address-taken / indirect / callback / vtable /
                             # manual-seed / synthetic-split  (WHY the address exists)
```

`callable` and `resumable` are not exclusive; the three seeds are `callable=False, resumable=True`. **`kind` (execution contract) is kept orthogonal to incoming-edge provenance** — a continuation is *not* defined as "indirect-only"; it may in principle be reached natively from within its owner, fallen into, or dispatched, and being externally addressable does not grant it fresh-call semantics.

**Represent ≠ execute.** The metadata being *able to describe* `callable=True, resumable=True` does not mean the runtime can *execute* both role-specific contracts from one address: the current dispatch key is a bare guest address mapping to **one** host implementation, which carries no provenance to choose fresh-call vs resume by incoming path. So this is a **stop condition, not an accommodation**: if catalog construction ever finds an externally-dispatchable address with both roles, the implementation must halt and report `DUAL-ROLE ENTRY DETECTED` (with incoming paths / frame states) rather than silently registering one host entry. No such case exists for the three seeds, so it is a future-detection requirement, not a present blocker.

**`owner` vs `owners`.** Each of the three resume entries currently has exactly one owner, so `owner: int | None` is honest today. But a shared tail/resume label can be reachable from more than one callable body in general. Rather than pre-generalize to `owners: frozenset[int]`, the implementation keeps the singular field **and adds an invariant that discovering incompatible multiple owners stops and reports** rather than silently picking one. `owner` is analysis metadata, not documentation.

## Boundary vs registration vs emission are three separate decisions

Today, adding an address to `known` does three things at once: (1) it becomes an emitted `f_<addr>`, (2) it is registered in the dispatch table, and (3) `function_flow()`'s `stop_at_continuation()` treats it as a boundary that **truncates** any other function whose straight-line flow reaches it. #51 exists because those are not the same decision.

- **Callable boundary** (affects recovered function extents): only true fresh-call entries. An interior resume PC must **not** truncate its owner — the owner stays one recovered function.
- **Dispatch registration** (externally reachable): a resume entry that is an indirect target is still registered — `resumable != unregistered`. #51 must not be "fixed" by dropping these from `sr_register`.
- **Host emission** (a callable body vs a resume body): a resume entry gets a host entry that starts translation at the resume PC and lets the guest's own epilogue own SP — it may overlap guest instructions already represented inside the owning callable's body; that host-side duplication does not imply two source functions exist.

**As built:** `build_entry_catalog()` produces the single `EntryInfo` catalog, and the two sets are derived from it separately —

```python
known         = {addr for addr, info in catalog.items() if info.callable}
resume_owners = {addr: info.owner for addr, info in catalog.items() if info.resumable}
```

`known` alone drives `function_flow()`'s boundary/truncation behavior; `resume_owners` is threaded into `function_flow()` and `emit_function()` so a resume PC is recognized as its owner's interior rather than as another callable. Emission uses `host_entries = set(known) | set(resume_owners)`, and dispatch registration is driven off the emitted `void f_…`/`void r_…` symbols, so a resume entry is registered without becoming a boundary. There is no separate `callable_boundaries` name in the code; `known` *is* that set, now narrowed to callables by the catalog.

## Codegen contract table

| Role | Captures entry SP? | Dispatch-registered when externally addressable? | Truncates another function's extent? | Return frame behavior |
| --- | --- | --- | --- | --- |
| **callable** | yes | yes | yes (it is a boundary) | guest epilogue returns to entry SP; synthetic restore is a redundant no-op |
| **resumable (continuation)** | **no** | **yes** | **no** — belongs to its owner | **guest epilogue owns SP; no synthetic restore** |
| **translation-split only** | n/a | only with real indirect-reachability evidence | n/a | n/a |

## The centralized exit contract — as built

Before the change the entry SP was restored inline at every exit form. The implementation routes all of them through **two** helpers, not the three this document originally sketched: a separate `emit_tail_transfer` / `emit_external_transfer` pair turned out to be unnecessary, because a tail transfer and an external transfer differ only in how the target is computed, and both then take the *same* host-exit decision.

```python
def emit_host_return(resumable, comment=None)   # every guest-return / transfer-then-return exit
def emit_host_fallthrough(resumable)            # the natural end of the emitted body
```

`emit_host_return(resumable)` answers the one question — "does the host return restore an entry SP, or does the guest own SP?" — for all eight exit sites in `emit_function`: `jr $ra`; `jr $reg` (dispatch then return); `jr` with a syscall in the delay slot; `j` to a host entry; `j` to a dispatched target; both conditional-branch-to-dispatch forms (likely and ordinary); and the synthetic continuation return. `emit_host_fallthrough(resumable)` covers the ninth, the body's fall-through end, and deliberately does **not** emit a guest return — for a resume body it emits only a comment, so fall-through is not silently treated as a guest return.

`_sp_entry` itself is only declared when `not resumable` (codegen.py, `emit_function`), so a resume body cannot reference it even by accident; the regression asserts `_sp_entry` is absent from the generated `r_…` body. The three seeds are not special-cased inside either helper — they reach them through the ordinary `resumable` flag carried from the catalog.

## Interaction with #45 (kept distinct)

- **#45 residual:** what does an integer code-address value mean when carried as guest *data* (e.g. offset 0 vs NULL)? — value/identity.
- **#51:** what execution *contract* does a target have — fresh callable, or active-frame continuation? — control-flow semantics.

A future `ProgramImage` will likely need both (image identity + relative address + entry roles + provenance). #51's fix needs only the entry-role metadata above; it does **not** require building `ProgramImage`. If implementation shows the contract cannot be made correct without image/module identity, that is a stop condition — report a ProgramImage blocker rather than forcing it.

## Regression matrix (issue #51 Part F/11–16) — implemented

`tools/test_codegen_entry_semantics.py`. The fixture is an **owned synthetic mini-ELF** built in-test (owner establishes a frame; an interior resume PC uses the owner's frame, restores it, and returns), run through the real `tools/codegen.py --profile=hst`, compiled with the host C compiler against a minimal `recomp.h` shim, and **executed**; the test skips cleanly when no C compiler is on PATH. All six cases are asserted by that one executable — its `main()` drives each entry from a fresh `CpuState` and the harness returns nonzero unless every SP and result register matches:

1. **resume entered indirectly, mid-frame** — `r_<resume>` entered with SP already lowered by the frame size must return `SP == caller SP` (`resume_sp=0x00001000`). Captured failing first: pre-fix this returned `0x00000f90`, i.e. low by the 0x70 frame.
2. **owner's own native path through the same region** — `f_<owner>` entered normally must still return `owner_sp == caller SP`. Structurally asserted too: the owner's generated body still contains the resume address as a *dispatch constant* and does **not** call `r_<resume>(s)`, proving the region stayed native to the owner rather than being deleted from it.
3. **adjacent real callable** — the function immediately after the resume region returns its own result (`r2 == 7`) with SP restored, so it stayed an independent callable boundary.
4. **address-taken tiny leaf** — stays callable and returns its argument (`r2 == 5`) with SP restored.
5. **normal `jalr` to a callable** — `indirect_v0=9` with SP restored; the resume mechanism did not give all indirect targets resume semantics.
6. **non-linking `jr $reg` tail transfer** — `tail_v0=11` with SP restored and no extra frame restoration.

Two further invariants are asserted directly: `sr_register(0x<resume>u, r_<resume>)` is present in the generated chunks (`resumable != unregistered`), and `_sp_entry` never appears in the `r_<resume>` body. `EntryCatalogInvariantTests` asserts the `DUAL-ROLE ENTRY DETECTED` stop by constructing a catalog where a resume address is already analyzer-callable.

**Scope limit, previously stated plainly — now partly closed:** the real-address claims — `function_flow(0x30fdc)` still covering `0x310b0`, `function_flow(0x21ac0)` covering `0x21c78`, `function_flow(0xb237c)` covering `0xb26a0` — used to have no assertion anywhere, because this test uses synthetic addresses and the private regeneration diff cannot be committed or re-run in CI. They are now **machine-checked on every regeneration** by the frame-balance verification described in *Structural role verification* below: the check runs against whatever image is being translated, so it does not need the private bytes in CI to be meaningful. The remaining unasserted real-address claim is that `0x310b8` stays a separate callable, whose evidence is still the private regeneration diff.

## Manual-seed inventory after implementation

- resume entries: `0x310b0`, `0x21c78`, `0xb26a0`;
- manual callable leaves still hidden by `_is_trailing_epilogue`: `0x5a648`, `0x42998`, `0x3db3c`, `0xe1724`, `0xe3b24`, `0x14430`;
- redundant manual seeds removed because the analyzer already discovers their direct calls: `0x104b0`, `0x104e0`, `0x56098`, `0x57344`.

All nine remaining HST profile roles live in `codegen.HST_MANUAL_CALLABLES` (6) and `codegen.HST_RESUME_OWNERS` (3), and are mechanically cross-checked against `compat_overrides.HST_ENTRY_ROLES` by `tools/test_compat_manifest.py::test_hst_entry_roles_are_documented_exactly` — an exact set-and-owner equality, so adding or retiring a role without updating the manifest fails the suite. The manifest also lost the `0x310b0` custom-stub entry, because that stub no longer exists. Repairing `_is_trailing_epilogue` remains a separately-proven analyzer improvement rather than part of this resume-semantics change.

## Structural role verification (`tools/entry_frame_balance.py`)

Wiki doc 26 section 31 names the general inference strategy for resume roles —
*"track SP delta along the CFG from a candidate callable entry; if a target is
reachable with a deterministic nonzero SP delta and no new frame prologue, it is
a strong resumable candidate"* — as research rather than implementation. That
strategy is now implemented and wired into catalog construction, so the three
seed classifications are **re-derived from the image on every run** instead of
being trusted as constants.

The measurement is the net `$sp` delta from an entry PC to each `jr $ra`:

| Role | Net delta at every `jr $ra` |
| --- | --- |
| callable | `0` — its prologue allocates exactly what its epilogue releases |
| continuation | `+owner depth` — it releases a frame it never allocated |
| frame-leak | negative — returns having consumed stack it never released; normally a mis-split extent, surfaced separately rather than hidden |
| indeterminate | `$sp` written by a form the module refuses to model, the walk hit its budget, or return paths disagree |

**Delay slots are load-bearing.** MIPS compilers routinely hoist
`addiu $sp, $sp, +N` into the `jr $ra` delay slot. A walker that stops at the
jump without executing its delay instruction reports *every* such function as
unbalanced; the first prototype of this analysis did exactly that and had to be
corrected. Branch-likely forms nullify their slot on the not-taken path, and
that is modelled too.

**Depth, not prologue frame, is the owner-compatibility test.** The authoritative
expectation for a resume PC is the owner's *live stack depth at that PC*, which
equals the prologue frame only when the owner makes no further `$sp` adjustment
before reaching it. The three HST seeds are in that coinciding case, but the
general check does not assume it.

`build_entry_catalog(..., elf=...)` now stops the build on: a resume PC that has
its own prologue; a resume PC whose release does not equal the owner's depth
there; an owner that is not a balanced callable; an owner whose extent never
reaches the resume PC (`OWNER DOES NOT COVER RESUME`); a resume PC reachable
inside its owner at more than one depth; and stack behavior it cannot model
(reported `indeterminate` rather than passed silently). Confirmed entries gain
the `frame-balance-verified` provenance tag.

**Result on the private image.** All three declared pairs are confirmed
independently of the original manual audit, with the release exactly matching the
owner's depth: `0x310b0`→`0x30fdc` at `0x20`, `0x21c78`→`0x21ac0` at `0x70`,
`0xb26a0`→`0xb237c` at `0xd0`. A negative control — repointing `0x21c78` at the
wrong owner `0x30fdc` — is rejected with `never reaches`, so the check has teeth.
Catalog construction with verification enabled costs ~0.1 s. Before the
direct-`j` narrowing slice, a full regeneration produced the three declared
`r_` symbols, all three `sr_register`ed, and zero `_sp_entry` in any resume
body. The current slice keeps those seed invariants and adds the mechanically
audited direct-`j` resumes documented below; bare `codegen.py` counts are not
the manager's canonical multi-input build and should not be compared against
the recorded 14,376/8-chunk figure.

## The broader analyzer role audit — no longer deferred

The *Analyzer provenance* note below establishes only that `analyze()` is not
role-mixed **for these three entries**, and defers the general question. The
census (`entry_frame_balance.py <elf> --census`) answers it, and the answer is
that `analyze()` **is** role-mixed at scale:

| Verdict over `analyze()`'s 13,595 starts | Count |
| --- | --- |
| callable | 9,985 |
| continuation | 3,439 |
| frame-leak | 4 |
| indeterminate | 167 |

So the three manual seeds are not a special class — they are three members of a
large population of analyzer-discovered "function starts" that carry the
continuation stack signature and are nevertheless emitted as standalone
`f_<addr>` entries with the `_sp_entry` contract.

**This is a latent modeling gap, not a demonstrated live corruption, and it is
deliberately not acted on here.** Two facts bound the risk:

- **No continuation-classified start is reached by a direct `jal`** (0 of 3,439);
  53 are reached by `j`, 182 by a branch, and 3,252 have no direct edge at all.
  The unambiguous defect — calling an interior PC as a fresh function — does not
  occur in this image.
- When an interior entry *is* reached from its own owner, the owner's own
  `_sp_entry` restore runs after it and masks the interior entry's wrong restore.
  Corruption needs the entry to be dispatched from outside its owner.

Reclassifying 3,439 entries on structural signature alone would be a large
behavior change justified by shape rather than by runtime evidence, which is
exactly the inversion this issue exists to prevent. The census is therefore
shipped as a diagnostic, and narrowing it — starting with the 53 `j`-reached
entries, which are the shared-tail idiom most likely to be dispatched
cross-function — is left as tracked follow-up work.

## Direct-unconditional-`j` narrowing slice (2026-08-08)

The first bounded follow-up now audits only those 53 continuation-signature
starts that are direct targets of an unconditional `j`.  `analyze.direct_j_edges`
records the source PCs from executable ranges; the independent stack walk then
requires all of the following before changing production metadata:

1. the target has no frame prologue and a deterministic positive return delta;
2. one balanced analyzer callable reaches every direct-`j` source and the target
   at the same live `$sp` depth; and
3. the target CFG does not reach an incoming source, which would make the edge
   a backwards loop rather than a one-way shared-tail transfer.

The image-independent audit classified **28** candidates as decisive interior
continuation/resume entries and reclassified them as dispatch-registered
`r_<addr>` entries. **25** remain **ambiguous** (16 loop/back-edge cases and 9
owner/source-control-flow gaps) and retain their existing analyzer boundary and
callable codegen contract. None of the 53 had a balanced callable stack
signature, so there was no new callable/tail-call reclassification in this
slice; balanced direct-`j` targets outside the continuation-signature set were
left unchanged. The three historical HST seeds (`0x310b0`, `0x21c78`, and
`0xb26a0`) have no direct-`j` edge in this evidence model and remain governed by
their existing profile declarations plus frame-balance verification.

This rule is deliberately conservative and address-free. It does not infer a
resume contract from a `j` target, a positive stack delta, or a function-shaped
sequence alone; ambiguous evidence is retained for a later slice. The owned
synthetic analyzer/codegen regressions cover one-way shared tails, loop
back-edges, incomplete owner/source control flow, balanced callable `j` targets,
and an indirect dispatch into the generated resume body. The latter executes
the guest epilogue with a live frame and asserts that `$sp` returns to the
caller value without `_sp_entry` restoration.

The original census was 3,439 continuation signatures. After this bounded
increment, **3,411** remain outside production reclassification; branch-reached
and no-direct-edge candidates are explicitly out of scope here.

## Shared tails: why the multiple-owner stop cannot fire yet

Wiki doc 26 section 14 requires stopping when one resume PC has incompatible
multiple owners. Implementing the check produced a result worth recording: for a
shared tail with a **fixed** `$sp` release `R`, every balanced owner is
necessarily at depth exactly `R` there, because an owner at any other depth
cannot balance. So all discovered owners always agree, and the stop is provably
unreachable for that shape. It fires only for a path-dependent or indeterminate
tail — which this analysis already reports as `indeterminate` — so it is retained
as a guard, and the regression asserts the *reachable* property instead: an owner
at a mismatched depth is **excluded and reported**, never silently chosen.

## Analyzer provenance — settled

Verified against the image: `analyze()` returns 13595 known starts and **none** of the three resume PCs (`0x310b0`, `0x21c78`, `0xb26a0`) is among them — they are **only `codegen.py` overlays** (manual `known.add`). The three owners (`0x30fdc`, `0x21ac0`, `0xb237c`) and the adjacent callable `0x310b8` **are** analyzer-discovered starts. So `analyze()` output is **not** role-mixed for these entries: `analyze()` compatibility can be preserved and the role catalog built immediately above it for this increment. (This does not prove `analyze()` never mixes roles anywhere — only that it does not for these three; a broader audit is deferred.)

## decomp.me exporter — out of scope for this increment

`tools/decompme_export.py` takes its function starts directly from `analyze.analyze(elf)` and already labels its next-start extents *approximate pending #51*. Since the three continuation seeds are added later inside `codegen.py` and are confirmed absent from `analyze()`'s set (above), they are **not** in the exporter's start set. Therefore do **not** modify the exporter merely because resume host entries will exist — only touch it if the analyzer result/API changes such that resumable PCs would otherwise enter its source-function start set, and add a regression only if behavior actually changes. Preserve the invariant that Product 2 consumes callable/source-function boundaries, not every generated host entry.

## Remaining verification

The private full regeneration produces **14,376 callable functions + 3 resume entries**, eight dynamic chunks, and zero fallbacks. Compared with the preserved pre-change output, the only entry renames are the three `f_` to `r_` resumes; the only source-callable bodies that materially expand are owners `0x21ac0` and `0xb237c`. Owner `0x30fdc` was already structurally complete, and its former custom resume stub is replaced by generic translation.

A strict-dispatch GUI run on the exact private ELF rendered the warning, title/load flow, Nakagawa Tennis Club dialogue, and the 3D-court tutorial tip without a dispatch miss, non-PLT miss, fatal error, unknown NID, or unimplemented-operation diagnostic. The SDL3 audio stream opened at 44.1 kHz stereo s16, and a read-only Windows CoreAudio session sample measured a nonzero per-process peak (`0.3832` over 300 samples in 15 seconds). This proves that the live route produced host audio signal; it does not establish music, PSMF, or audio fidelity.

Automated window-key injection did not advance one static modal screen, but the source already pumps SDL events both from presentation and periodically from the scheduler, so that observation is not enough to establish an input-pump defect. Direct periodic PSP Cross input was used only as a run-time diagnostic override to continue the route; no production latch or progression behavior was added. Physical-controller verification remains tracked separately in issue #34.

A subsequent strict run replayed the maintained Exhibition route, then used normal focused SDL keyboard input to select Singles, Normal difficulty, and an opponent. The runtime rendered each selection screen and the 3D court loading scene with zero strict-dispatch diagnostic hits. It did not reach play: after the court loading screen, worker `0x13b` reproduced issue #126's established 58-entry resource-name search loop at `0x14934` / `0x2fc70`, and the watchdog reported no frame for 3,000 vblanks. That was fresh exact-head evidence that the next acceptance step was blocked by the canonical loader stall, not evidence that #51 caused or fixed it.

Issue #126's provenance trace subsequently proved that the first loader worker exited through `sceKernelExitThread`, where runtime-only compatibility code fabricated a wakeup for the sleeping launcher. The launcher then tore down the character-resource singleton while a sibling loader worker was live. Removing that unrelated wake preserves the installed `+0x3ec` handler across both workers. A strict trace-free replay on the clean fix branch rendered the animated 3D court/costume scene and match-settings UI without a dispatch miss, unknown NID, or no-frame watchdog.

Before calling #51 fully complete, exercise the exact resume paths through a first playable rally and, when the private trace inputs are available, the external-oracle route. The strict Exhibition-to-match-setup result is strong live evidence but is not proof of every callback/resume path or PSP correctness.

## Direct-conditional-branch narrowing slice (2026-08-08)

The second bounded follow-up audits the analyzer starts reached by a **direct
PC-relative branch**. `analyze.direct_branch_edges` records
`target -> ((source, kind), …)` over the whole branch family, and
`analyze.code_pointer_evidence` separates the four ways an address can be
reached other than by falling into it (`jal`, linking branch, `la`-materialized
constant, code pointer in a data section).

### Why the direct-`j` rule was not reused

A conditional branch is not a weaker `j`, and four differences each create a
proof obligation the earlier slice never had:

1. **A branch has a fall-through.** Its target therefore normally has a *linear*
   predecessor as well, so "one owner reaches every branch source" does not by
   itself establish a single incoming contract.
2. **The family contains calls.** `bal` / `bgezal` / `bltzal` and their likely
   forms write `$ra`; their target is a callable boundary. `j` has no such form.
3. **Backward branches are the loop idiom.** A backward `j` is comparatively
   rare; a backward conditional branch is what every loop compiles to.
4. **Multiple predecessors are the common case, not the exception** — 111 of the
   182 continuation-signature branch targets have more than one — so the owner
   search is exact rather than windowed.

A fifth, smaller correction belongs to the census rather than the rule: REGIMM
`rt` selectors outside `{0,1,2,3,0x10–0x13}` are trap-immediates and `synci`,
whose immediate field is a code, not a displacement. `trace_function` treats
every REGIMM word as a branch, which only makes it over-explore; carrying that
into an evidence census would instead report an incoming edge that does not
exist, so `analyze.branch_target` decodes the family exactly. `trace_function`
itself is deliberately unchanged — narrowing it would move recovered function
extents, which is a separate change.

### The rules

Promotion to a resume contract requires **all** of B1–B13:

| Rule | Requirement |
| --- | --- |
| B1 | an analyzer start in an executable range with at least one direct branch edge into it |
| B2 | stack role is `continuation`: no unmodelled `$sp` write, walk not truncated, one strictly positive net delta on every return path |
| B3 | no frame prologue of its own |
| B4 | no source is a linking branch |
| B5 | not a `jal` or linking-branch target anywhere |
| B6 | not address-taken — no `la`-materialized constant, no code pointer in any data section |
| B7 | not the delay slot of a hard terminator |
| B8 | every source lies strictly below it |
| B9 | its own CFG does not reach any of its sources |
| B10 | exactly one balanced framed callable in the **whole** start set reaches it |
| B11 | that owner reaches it, and every source, at exactly one live `$sp` depth, and its depth at the target equals the target's own release |
| B12 | if the address can be fallen into, that linear predecessor is inside the same owner |
| B13 | "source" means **every direct predecessor**, branch *and* `j` — B8, B9 and B11 range over the union |
| — | the owner's *recovered extent* must contain the target, not merely a stack walk through it |

`index_frame_owners` replaces `find_frame_owners` for B10 because a window is a
bound on the *search*, not on reality: an owner outside it is silently not
found, and "no second owner was found" then reads like "no second owner
exists". One pass over the whole start set, keeping depths only at the
addresses under audit, costs about the same as one census.

**B13 was found by reading the generated diff, not by reasoning.** The first
version of this audit proved the branch edges and ignored the `j` edges into the
same address. Two candidates passed that way, and the regeneration diff showed
their transfer sites changing in four *other* functions — bodies that reach the
same address by direct `j` and that the proven owner never reaches. Those two
addresses are shared tails with several reaching owners, and the direct-`j`
slice had already declined them for precisely that reason. A branch edge selects
a candidate; it does not license ignoring the other ways in. Widening the source
set moved 20 candidates out of the promoted set.

### Result on the private image

| Verdict over the 224 branch-reached starts | Count |
| --- | --- |
| proven interior continuation | **12** |
| genuine callable boundary (balanced role) | **32** |
| ambiguous, unchanged | **180** |

The 180 ambiguous split as 132 address-taken (B6), 38 loop headers with a
backward predecessor (B8, ranging over the B13 union), and 10 indeterminate
stack role (B2). B3, B4, B5, B7, B9, B10, B11, B12 and the extent check
disqualified nothing *directly* on this image; they are nevertheless live rules,
each proven load-bearing by an owned synthetic regression and by the mutation
sweep below rather than by a title address.

That B6 dominates is not an artifact of a loose test: in-range code pointers
occupy 0.6 % of the data words in this image, while 59 % of the branch-reached
starts are hit — these addresses are in the analyzer's start set *because* they
are table targets. They are plausibly interior continuations, but the incoming
contract of whatever reads the table is not visible to this analysis, so they
are left unchanged rather than promoted on shape.

**The overlap is a cross-check, not a merge.** 2 of the 12 were already resume
entries from the direct-`j` slice, so this audit re-derives their owner by a
different and stricter search; both agree exactly. `build_entry_catalog` raises
`CONFLICTING RESUME OWNER` if the two derivations ever disagree, rather than
letting the later slice overwrite the earlier owner. That leaves **10 new
promotions**.

All 12 are preceded by a hard terminator, so none has a linear fall-in, and none
is a `jal` target. All ten new ones share one shape: an **outlined shared
epilogue** — a bare `jr $ra` whose delay slot releases the owner's frame,
sitting immediately after an unconditional `b`, entered by a forward conditional
branch from inside its owner. `analyze()` promotes exactly that shape to a
*high-confidence* start (`b`, then `jr $ra` two slots on), which is why these
were emitted as standalone `f_` callables carrying the `_sp_entry` contract —
the #51 defect in its original form.

### Regeneration delta

Against `2a3c636` on the same private input, single-ELF `codegen.py`:

| Measure | baseline | after |
| --- | --- | --- |
| callable functions | 13,573 | 13,563 |
| resume entries | 31 | 41 |
| total emitted entries | 13,604 | 13,604 |
| `sr_register` calls | 13,604 | 13,604 |
| fallbacks | 0 | 0 |
| chunks | 7 | 7 |

Exactly **10 generated bodies differ**, and they are exactly the 10 promoted
entries; no other function's body changes at all. None of the 41 resume bodies
contains `_sp_entry`. (Bare `codegen.py` counts are not the manager's canonical
multi-input build; the comparison above is baseline-to-branch on identical
inputs, which is what it is for.)

Because none of the ten has a `jal`, an address-taken constant, a data-section
pointer, or a `j` transfer site — B5, B6 and B13 each verified — nothing in the
image transfers to them except their owner's own branches, which remain native
inside the owner. **This slice is therefore latent-defect removal**: it corrects
the contract these entries would run under if dispatched, and changes no
natively-executed path. A route replay can show no regression; it cannot show
the corrected path executing, and is not claimed to.

### Mutation coverage

Every rule is mutation-tested: disabling each in turn and re-running the suite
kills all 14 mutants (B2–B13, the extent check, and the REGIMM trap decode).

An earlier draft of this document recorded the extent check as an unreachable
backstop, on the reasoning that `trace_function` only stops at a foreign start
carrying a prologue or a known call target, both already excluded by B3 and B5.
That was wrong, and the synthetic fixture that exposed it is retained: a `j`
whose target is an analyzer start is treated as a **tail call** by
`trace_function`, which stops there without covering the target — so the check
is live and does fire.

Two branches of B11 *are* guards rather than live paths, for the reason already
recorded above for shared tails: an owner reaching a fixed-release target at two
depths cannot balance on both paths, so it is excluded from the owner index
before any depth comparison happens. The regression asserts that reachable
consequence — such an owner is dropped and named, never silently chosen.

After this increment, **3,401** of the original 3,439 continuation signatures
remain outside production reclassification. The no-direct-edge population
(3,252 starts with no direct `j` or branch edge at all) is untouched and remains
the largest tracked remainder.
