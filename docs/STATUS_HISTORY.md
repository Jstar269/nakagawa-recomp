# Status history

This document preserves dated investigations, resolved issues, and superseded
status reports. It is evidence, not the current task list. For live priorities,
see [`ISSUES.md`](../ISSUES.md).

<!-- markdownlint-disable MD028 -->

## 2026-08-04 — worktree reconciliation and PSP oracle acceptance gate

The local clone was behind `origin/main` (`f374b310c3e46f886b5f6c2ae9deda5b6f154468` versus
`de269b01c8f38a395cdac075d58053ab4dfbcc34`) while carrying an uncommitted pre-PSP worktree. The
worktree was proven to be a stale earlier draft of already-landed work: zero non-whitespace source
deltas against `origin/main`, three trailing blank lines as its only local-only content, and
pre-#266 copies of the `test_hst_doctor*.py` fixtures. It was stashed and tagged
`backup/worktree-20260804`, and the clone was fast-forwarded. No work was lost.

Three defects were found and fixed while preparing for physical PSP debugging. First and most
serious, the oracle comparator could present fixture placeholders as a hardware pass: the probe
emits `model=unknown`, `firmware=unknown`, and all-zero `binary_sha256`/`source_commit`, the parser
accepted those as syntactically valid, and `run_psplink.py` silently skipped canonicalization when
the provenance flags were omitted — so a first capture would report `classification: MATCH` with no
measured provenance anywhere. `compare_outputs()` now emits `acceptance_eligible` and
`acceptance_blockers`, `provenance_issues()` names each placeholder field, swapped
`--psp-output`/`--nakagawa-output` streams are rejected by a `source=` role check, and the four
provenance flags are all-or-nothing. Seven regression cases cover this and fail against the previous
comparator. Second, `tools/psp_readiness.py` reported `pre-commit` as missing because it only probed
the console script; it now falls back to `python -m pre_commit` and reports PASS on this host.
Third, the stale `assets/release_manifest.json` PGD digest recorded as a known failure in
`TEST_DISCOVERY.md` was re-measured against the private backend, matches the tracked value, and
`tools/test_release_manifest.py` passes its nine cases.

`TEST_SKIPS.md` previously stated a flat 33-skip inventory; the count is conditional on
private-input availability and is 27 on a checkout that has `place_game_here/`. Both columns are now
recorded with their condition. `HARDWARE_ORACLE.md` (proposal) and `PSP_HARDWARE_ORACLE.md`
(implemented) had no cross-reference, and the proposal's `tools/hw_doctor.py` duplicated the shipped
`tools/psp_readiness.py`; that item is now marked superseded.

Measured at the patched head with private inputs present: 689 tests, 0 failures, 0 errors, 27 skips;
discovery contract 694 loader / 689 started with 5 explained A-only and 0 B-only IDs; `ruff` and
`markdownlint-cli2` clean on the changed files. The aggregate manager `Verify` route was not run in
this pass and no CI result is inferred. PSPDEV/PSPLINK remain absent, so hardware readiness is still
`false` pending the maintainer's manual install.

## 2026-08-04 — pre-PSP software and oracle preparation

The exact starting `main` head was `f374b310c3e46f886b5f6c2ae9deda5b6f154468` with no open PRs.
The ignored PGD/amctrl backend manifest had a stale expected SHA-256 after the finite-span source
change; the tracked release manifest now records the current source digest without exposing the private
backend. The HST import parser accepts only a consistent window-paired prefix when named sections contain
an unreferenced tail, and codegen now dispatches conditional foreign targets instead of emitting
undefined labels. A synthetic import fixture and continuation regression cover both fixes. The
high-confidence #184 VFPU slice now rejects non-triple `vcrs` widths before source reads and limits
the `vrot` overlap scan to active lanes; the focused codegen/source guards pass and the one-trial
private VFPU fuzz route reports 446/446 words with zero divergence. #184's quad-span atomicity and
the broader #187 table-authentication work remain open.

`BuildFull`, manager `Test`, manager `Verify`, the 682-test Python suite (27 skips), publication audit,
PSPDEV lock validation, import audit, native selftests, reference selftest, and GPU coherence selftest
passed locally with the canonical ignored private inputs available through a local read-through junction.
The Python pre-commit module 4.6.0 was available even though its console script was absent from PATH;
`python -m pre_commit run --all-files` passed. No CI result is inferred from these local gates.
PSPDEV/PSPLINK discovery found no local executables; the source-owned synthetic probe,
strict result comparator, issue matrix, runbook, and redacted readiness command are now tracked and remain
hardware-not-connected until the maintainer performs the manual PSPLink setup. No private game input,
capture, key, firmware, or generated PRX/PBP was added.

The #248 upstream check was refreshed on 2026-08-04: npm reports
`typescript-eslint@8.66.0` with peer range `typescript >=4.8.4 <6.1.0`, so the repository's TypeScript
7.0.2 lint failure remains externally blocked. No parser suppression or baseline downgrade was made.

## 2026-08-04 — finite #15/#170/#171 input-safety matrix completed

The finite malformed-input campaign was closed on exact `main` after merged PR #260 (merge
`f39e9cfc270ca672aa873232a841f5404d92868f`), PR #261 (merge
`b3a659cd0d7d07f8c53fd877308f146b3c248741`), and PR #262 (merge
`ecbd21823bde613bfec771bdc6120bf717d7690f`). #260 hardens ELF envelopes and relocations, function-diff
parsing, savedata/PGD fields, HLE export/registration, and full guest spans; #261 bounds MPEG ring
metadata, H.264 PS/ES/chunk/output growth, and complete feed/frame spans; #262 bounds GIM block parsing,
nested work, dimensions, offsets, raw bytes, pitch, and decoded-pixel allocation. Focused tests passed
(#170: 7 arithmetic/static contracts plus 29 HLE-manifest checks; #171: 5 GIM fixtures plus 17 XB-probe
tests), and the production runtime object set compiled. Full local discovery at the final head ran 669
tests with 641 passes, 27 skips, and 1 failure; the failure is the known ignored private PGD-backend
manifest hash mismatch. Windows/MinGW ASan/UBSan libraries remain unavailable, the private BuildFull import-stub/NID
route remains blocked, and no private media/visual, hosted-CI, legal, or DCO claim is made. Remaining
subsystem-specific guest spans, extraction-destination containment (#149), media behavior (#31/#32), and
publication/legal blockers remain separate work.

The maintained status reconciliation itself landed in PR #263 at `fc9bcd7220e51a00c8268bc9fbcd42f92567bf3b`;
the source/runtime evidence above remains tied to the exact code head stated for each merged fix.

## 2026-08-04 — tracker corrections after targeted closure review

Issue #32 was reopened because its latest evidence explicitly says the ATRAC ABI/parser prerequisite did
not prove resource selection, decoder output, or BGM playback. Issue #181 was likewise reopened because
its recorded follow-ups leave progress-tracker and dashboard evidence consumers incomplete after the
shared evidence foundation and boot-gate integrations. Both now carry additive bug/security/tooling
labels appropriate to their remaining work (#32 also carries `audio`); no closure was inferred from stale
state, and no new implementation claim is made here.

## 2026-08-04 — #243 guest FD namespace resolved

Issue #243 was resolved by merged PR #250 (`fa5cd14498894e96770bb4aa54540de9c74b4922`, exact
head `c98b0e839aa57be1a1a1ae05fe16cd468a3ac279`). The root cause was a shared descriptor table that
allocated the first ordinary file at fd 1 while `h_IoWrite` classified every numeric fd below 2 as a
console stream. The fix gives each slot typed state, reserves guest standard descriptors 0/1/2 before
registration, starts ordinary allocation at fd 3, and uses the same teardown path for synchronous and
asynchronous close. The native HLE fixture records 311 checks with zero failures, including an exact host
file assertion for `NAKAGAWA_MINIMAL SUM=5050\n`, descriptor invalidation/reuse, and the unchanged
standard-fd identity/import audit. This is production-helper/white-box evidence, not PSP hardware or a
full-game runtime proof. The private HST `BuildFull` attempt stopped before compilation at the unchanged
`scePsmfP_library.prx` import-table mismatch; the aggregate manager Verify route retained its unrelated
baseline `heap-selftest` duplicate-definition failure. No private inputs, captures, or DCO identity were
published; the owner-authorized DCO waiver remains recorded on the PR.

## 2026-08-04 — #142/#143/#223 closures and remote branch consolidation

Issues [#142](https://github.com/Jstar269/nakagawa-recomp/issues/142),
[#143](https://github.com/Jstar269/nakagawa-recomp/issues/143), and
[#223](https://github.com/Jstar269/nakagawa-recomp/issues/223) were closed after the recorded
current-main evidence: display-latch behavior from merged PR #241, PSP-EABI formatter behavior from
merged PR #162, and configured-root long-path parity from merged PR #237. The unavailable literal
greater-than-260-character process-CWD route for #223 is an explicit Windows-host limitation, not an
unresolved implementation defect. Seven remote branches whose complete work was already represented on
`main` or duplicated a merged PR head were retired; remaining remote branches retain unique or
unverified work and were left intact. A focused candidate also removes the duplicate test-local heap
stub exposed by the current-main `heap-selftest`; at that point #17 remained open until that candidate was
reviewed and merged. No private inputs, captures, credentials, or contributor attestations were published.

## 2026-08-04 — #17 allocator acceptance completed

Issue [#17](https://github.com/Jstar269/nakagawa-recomp/pull/17) was closed after exact-main
verification at `e0ebaeea1248e0be407968b9566aeed11076af5c`. PR #257 added bounded free-list footer,
size, arena, overlap, and cycle validation plus corruption/overflow negative cases; PR #258 directly
characterized interior, low foreign, and out-of-arena frees as safe no-ops. Together with merged PRs #124,
#165, and #256, the heap selftest covers coalescing, fragmentation stress, zero-fill, realloc split and
exact successor growth, fallback behavior, pointer stability, payload preservation, metadata validity,
and randomized churn. ASan/UBSan could not link on the Windows/MinGW host because both runtime libraries
are unavailable; this is recorded as unavailable rather than treated as sanitizer success. Broader
guest-memory and lifetime work remains under #15 and #16.

## 2026-07-27 — post-PR #155/#156 tracker reconciliation

GitHub issue #145 (CLOSED) was resolved by merged PR #155, which preserves GPU-new framebuffer pixels across partial CPU VRAM writes rather than invalidating the entire target. Bounded sampled vertex profiling was rejected by PR #156 because observer effect remained ~25–28%; sub-phase ranking is not decision-grade. Coarse GE hierarchy remains acceptable. The local ISSUES.md dashboard was reconciled: #145 moved to Closed, the #145 defect section removed from Immediate runtime work, the rejected profiling target removed from the #33 section, #145 removed from the Known limitations table, and the #142 section's stale reference to #145 removed. NEXT_SESSION.md's stale "split the remaining primitive frontend cost" heading was renamed. ROADMAP.md's stale "completely unmeasured" and "never been measured" claims about #33 were corrected. GitHub API inspection confirmed PR #155's body previously contained PowerShell backtick/control-character damage around inline-code SHA and route names; that prose was subsequently repaired (issue #145 body renders normally).

## 2026-07-26 — build truth, shader provenance, and first measured runtime optimization

Issues #146, #147, and #150 were fixed locally with content-addressed compiler/codegen profiles,
complete profile invalidation, `-MMD -MP` transitive dependencies, and a deterministic shader-embed
manifest/reproducibility gate. A corrected clean HST BuildFull completed, same-profile BuildFast was a
no-op, and the full local Verify suite passed under both native O0 and native O2 while generated game
code remained O0. GitHub Actions were account-blocked, so none of this is CI evidence.

The isolated 2000/1000/500/250 functions-per-TU matrix found 500/O1 to be the compile-memory knee
(1166.5 MiB peak versus 2382.4 MiB at 2000/O1), but generated optimization was not promoted without a
live benefit. An instrumented native O0/O2 pair improved from 847.5 to 777.2 seconds (35.4 to 38.6 guest
vblanks/s), but selected different opponents. A clean literal `13ce647` run reached only setup/tutorial
screens in that vblank window, proving the workload was not held constant. Native O2 therefore remains
a verified experiment and O0 remains the default.

Zero-behavior-change reason telemetry showed 206,407 optimized-window destination snapshot requests,
zero cache hits, and 206,407 copies. Render-batch waits cost 198.0 ms/s and snapshot-copy waits 189.7
ms/s. Every sampled blend state required both 16-bit framebuffer semantics and dither, so no fence,
snapshot, or blend-semantic removal was attempted. Exact measurements and the next replay task are in
[`PERFORMANCE.md`](PERFORMANCE.md).

## 2026-07-25 — Branch retirement: converged to `main` only

Final convergence. Every remote branch other than `main` was audited against current `main` and
deleted. This entry records each one's disposition so the deletions are reviewable without the
branches.

| Branch | Tip | Why it was deletable |
| --- | --- | --- |
| `docs/font-notice-provenance` | `0a1137f` | PR #128, draft, conflicting. Its defensible content was moved to `main` in this same commit (see below); its stale 2026-07-23 #29/#19-era notes were deliberately **not** resurrected. #99 stays open. |
| `codex/callback-correctness-audit-archive` | `52c55be` | Audit's real result (UID-keyed callback dispatcher) is on `main`. Deferred items are recorded on [#116](https://github.com/Jstar269/nakagawa-recomp/issues/116) with the tip SHA and file inventory. PR #136 from the same lineage was closed for *regressing* callback correctness. |
| `archive/issue-126-singleton-provenance` | `aaeed1a` | #126 closed; root cause (ExitThread's fabricated launcher wake, not singleton replacement) is in the 2026-07-24 entry below. |
| `archive/resume-live-validation` | `eed2c5a` | Continuation/resume entry semantics landed via PR #130 — `r_` symbols are in `tools/codegen.py` on `main`, with `tools/test_codegen_entry_semantics.py` and `docs/issue-51-entry-semantics.md`. |
| `backup/mixed-alloc-vfs-20260723` | `d4c1f02` | Pre-split backup. Both halves landed: heap coalescing via PR #124, VFS path joining via PR #127. `main`'s `vfs_path.h`/`vfs_selftest.c` are *ahead* of this branch's. |
| `fix/heap-coalescing-allocator` | `7e8d03f` | PR #123, closed; superseded by merged PR #124. |
| `fix/vfs-path-provenance` | `3b3b52b` | PR #125, closed; superseded by merged PR #127. |
| `codex/first-match-heap-integration` | `f5f8c1a` | PR #134, closed. Integration canary, never a merge vehicle; profiler integrity landed separately via PR #132. |
| `wip/callback-cb-waits` | `ff25fc5` | PR #136, closed as a correctness regression. |

The three older font-provenance branches each carried a `THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt`
(blob `e416d52`) that **asserted GPL-2.0-or-later for the font binaries**. That assertion is wrong
and was retracted on `docs/font-notice-provenance`; the retracted version (blob `c2318ce`) is the
one preserved on `main`. Deleting those branches removes the incorrect assertion from the tree.

No branch held a documentation file absent from `main`, and no branch held production code absent
from `main`.

## 2026-07-25 — #29 closed: the court-to-club return completes and shows no corruption

The deep `drive_court` route was finished and #29 closed as not reproducible.

**Route correction.** The exit path failed twice before it worked. After the give-up confirm there
are *two* dialogs: `Checking storage media. System should not be turned off.` (modal, no button,
presses ignored) and then `Finished saving data.` (dismissed only by CROSS). Exactly **one** CROSS
is correct — a burst of three walked post-match -> court select -> coin toss into a brand-new
match, and that run ended on a court instead of at the club. The corrected route is
`logs/route_E_deep_return_20260725.pad`.

**Replays were not reproducible, and that was a harness defect, not a game one.** Two runs of the
identical route diverged: the first wrote a save that cleared a first-time tutorial popup, the
second hit that popup and ended somewhere else. A pad-script replay is deterministic in its
*inputs* only. `-SaveBase` now snapshots and restores guest save state, so runs start
byte-identical. Before that, "run it twice" was not producing two samples of the same thing.

**Evidence.** `deep_return_iso1` / `deep_return_iso2`, both from the same save baseline, both
reaching vblank 43,500 with exit 0 and 66 captures. The MATCH TYPE screen is visited both on the
way in and on the way back, which makes it the strongest available comparison:

| Comparison | iso1 | iso2 |
| --- | --- | --- |
| MATCH TYPE, first visit vs post-match, full frame | 99.68% | 99.56% |
| — crops `285,25,340,120` and `150,25,285,60` | 100.00%, delta 0 | 100.00%, delta 0 |
| Club submenu, first visit vs returned, crop `150,25,285,60` | 100.00%, delta 0 | 100.00%, delta 0 |
| Club submenu, header badge `0,5,110,30` | 100.00%, delta 0 | 100.00%, delta 0 |

Controls matter here. MATCH TYPE has an animated cloud backdrop: two adjacent frames *from the same
visit* are 99.62% identical, so the cross-transition figures sit at the screen's own noise floor.
And crop `285,25,340,120` on the club submenu reads ~80% across the transition but ~93% between two
**first-visit** frames with no match involved — it contains the moving club camera and is not a
discriminator. Scanning every post-36,000 frame in both runs found exactly one near-black frame
each, in both cases the one-frame fade of the same screen transition.

**Runner defects fixed first.** `Run-HstEngine` slept its whole `-RunDuration` regardless of when
the process exited, which for a 43,500-vblank backstop meant ~50 idle minutes appended to a
~22-minute replay; it now returns on exit and kills only at the deadline. Oracle archives reject a
reused name instead of merging two runs into one numbered capture set. Every run writes
`oracle_manifest.json` (Git HEAD, exe and route SHA-256, parameters, exit code, capture count, wall
time, observed vblanks) and a completeness verdict. `SR_EXIT_AT_VBLANK` moved to the *last*
statement of `sr_vblank_tick()` so vblank V's controller sample is latched before the exit — the
previous "fully accounted" wording was broader than the placement.

Found while testing: `Start-Process -PassThru` with redirected streams returns a process whose
`ExitCode` reads back empty unless the native handle is cached first, so the manifest recorded no
exit code and a crashed run would have passed a verdict that only rejected *nonzero* codes.

**Two real defects found while capturing, filed separately.** The post-match screen renders without
its right-side choice panel and the racket/costume screen without its left-side selection UI
([#143](https://github.com/Jstar269/nakagawa-recomp/issues/143)) — no menu asset fails to load, so
it is a draw problem, not a VFS one. Zeta's model disappears for a moment whenever it updates or
changes ([#142](https://github.com/Jstar269/nakagawa-recomp/issues/142)); this was initially and
wrongly attributed to the club's moving camera.

**Turnaround.** Runs measured 32-43 guest vblanks/s. A 43,500-vblank route is therefore ~22 minutes
of irreducible guest execution, and ~26,000 of those vblanks are match points #29 never needed. The
largest remaining lever is a shorter route, not more host-side gating.

## 2026-07-25 — #29 deep `drive_court`: match/exit path mapped, four heavy transitions clean, return-to-club not yet reached

Follow-on to the shallow-route result below. Four long runs on `main` = `d88b321` (38k–49k vblanks
each) drove the recorded first-match route into a **rendered 3D match** and then exercised the
match-exit path. **No runtime code was changed.**

**The live-match pause/exit path is now fully mapped** — this was the blocker called out previously
and it took four runs to establish, so it is recorded here rather than re-derived:

| Step | Input | Result |
| --- | --- | --- |
| 1 | `START` (`0x0008`) | SCORECARD overlay. Footer: `SELECT` = GIVE UP, `□` = SHOW SKILLS, `○` = RESUME |
| 2 | `SELECT` (`0x0001`) | "Do you want to give up?" — **`NO` is pre-selected** |
| 3 | `LEFT` (`0x0080`) | moves the cursor to `YES` |
| 4 | `CROSS` (`0x4000`) | confirms; the match ends |
| 5 | **`CIRCLE`**, not `CROSS` | backs out toward the club. `CROSS` here advances *forward* through a COIN TOSS into a **new match** |

Two dead ends worth not repeating: the scorecard is modal and ignores `CROSS` entirely (only the
three footer bindings respond), and `CIRCLE` on the scorecard is RESUME, so a `CIRCLE` burst there
simply returns to play. Input delivery was verified with `SR_INLOG` — clean press/release edges at
the scripted vcounts — so a screen that does not react is a binding fact, not a lost input.

**What the deep runs establish.** Across all four runs, spanning the match, the scorecard overlay,
the give-up dialog, a character/costume screen rendered over a live 3D court, a coin toss, and a
second match: **zero** `NULL_CALL`, dispatch miss, `NONPLT_MISS`, `HEAP_ALLOC: fail`,
`HEAP_FREE_LIST_CORRUPT`, `HEAP_SMASH`, unknown NID, no-frame watchdog, or `TRACE_STOPUNLOADSELF`.
Every captured frame renders correctly — **no black bar, no stale court background behind a menu,
no geometry corruption** — across four distinct venues (outdoor clay, indoor hard, park grass,
meadow grass) and four opponents (Momo, Ivan, Meilin, Yuko), which vary run to run from the same
deterministic input script.

**What they do NOT establish, and why #29 stays open.** None of these runs reached the **club
interior** from a match. The exit consistently lands back inside the Exhibition flow, because the
route used `CROSS` where step 5 above needs `CIRCLE` — which is only now known. So the specific
`drive_court` transition #29 is named for, *rendered court → club interior*, remains uncaptured, and
was therefore not replayed twice. Closing #29 on the evidence above would be overreach.

The remaining work is one route revision — replace the post-confirm `CROSS` presses with `CIRCLE`
— and two clean runs of it, roughly 45 minutes of wall clock. Everything else it needs is recorded.

**Found along the way:** the two character-portrait circles in the match scorecard render as empty
rings. Filed as **#139** with a proven cause: the guest requests
`host0:data/chara/model/face/00/100_f_face{0,1,2}.gim`, but those files exist only under
`.../face/100/`, while directory `00` belongs to a different archive (`face000.xb`) and holds
different files. The VFS is not at fault — it only strips the device prefix. Whether the guest or
the archive-mount emulation is wrong is unresolved; do not paper over it with a path rewrite.

## 2026-07-25 — #29's two menu symptoms do not reproduce on current `main`

Fresh deterministic routes were authored against the current boot timeline (the 2026-07-23 note
below established that `route_replay.pad` and `story_save_replay.pad` are stale) and each was
replayed twice from a clean start. **Neither of #29's two menu symptoms reproduces.** No runtime
code was changed — there was no proven incorrect transition to change.

Routes, all `SR_NOINPUT=1` + `SR_PADSCRIPT`, all sharing the same 39-line boot prefix:

| Route | Path | Purpose |
| --- | --- | --- |
| A | boot → Main Menu → **Options** | clean reference, Exhibition never entered |
| B | boot → **Exhibition** → back out → Main Menu → Options | the suspected-corruption path |
| C | boot → **Exhibition** → back out → Main Menu (stop) | the club-interior return |
| CTRL | boot → Main Menu (stop) | first-visit control, no Exhibition |

**Symptom "Options renders repeated `NOW LOADING` textures instead of its intended controls" —
does not reproduce.** Options renders its intended controls on both routes: MATCH MUSIC / QUICK
REPLAYS / SHOT TARGETS / HST VOICEBOX, plus two `? ? ? ? ?` rows that are the game's own
locked-until-unlocked entries, not placeholders. Comparing the Options panel region pixel-wise:

```
A run1 vs A run2   99.68% identical   (same route, two clean starts)
A run1 vs B run2   99.64% identical   (Options-first vs Options-after-Exhibition)
A run2 vs B run2   99.96% identical
```

The Exhibition visit changes the Options screen **no more than replaying the same route twice
does**; the residual is the animated selection highlight. There is no signal here.

**Symptom "the tennis-club interior is not restored after returning from Exhibition" — does not
reproduce.** In the static background regions of the Main Menu, the screen returned to after
Exhibition is *pixel-exact* against the first-visit capture: `(285,25)-(340,120)` and
`(150,25)-(285,60)` both **100.00% identical, worst channel delta 0**.

**A real run-to-run background variation exists, and it is not the transition.** Two stable
club-interior states were observed (differing ~12.6% over a fixed background crop). Runs within a
state agree at 100.00% / delta 0. The split does **not** follow first-visit versus return — the
no-Exhibition **CTRL** route reproduces both states, and CTRL matched a *return* capture at
100.00%. What the split does follow is wall-clock: every capture before ~14:00 local is one state,
every capture after is the other. The title imports `sceRtcGetCurrentClockLocalTime`
(NID `0xe7c27d1b`), which this runtime answers with the real host local time, so a time-of-day club
backdrop is the natural reading. **That last step is a hypothesis, not proven** — what *is* proven
is that the Exhibition round-trip is not the variable, because a route that never enters Exhibition
reproduces both states.

**Run health.** Across all seven runs (A×2, B×2, C×2, CTRL×1): zero `NULL_CALL`, dispatch miss,
`NONPLT_MISS`, `HEAP_ALLOC: fail`, `HEAP_FREE_LIST_CORRUPT`, unknown NID, or
`TRACE_STOPUNLOADSELF`. Two runs (A run 1, B run 2) each logged a single transient 600-vblank
no-frame watchdog during boot, then recovered and completed their route. In A run 1 the #135
stalled-thread diagnostic resolved the live thread's argument registers and found small integers
rather than a resource-name pointer, and the thread was `live` rather than blocked, so this is a
slow boot-time frame and not the #126 loader-stall signature.

**Working hypothesis for why the symptoms are gone** (offered as a lead, not a conclusion): #29's
evidence predates #126. `ExitThread` was fabricating a launcher wakeup, letting a teardown destroy
character-resource state under a live worker — a scene whose resources were retired underneath it
is exactly what renders as stale or placeholder textures. Removing that wake (PR #131) plausibly
removed both menu symptoms. This is not established; it is recorded so the next investigation
starts from it rather than from the renderer.

**What remains untested, and why #29 stays open.** These routes back out of Exhibition from the
**MATCH TYPE menu**. The milestone is named `drive_court`, and the deep return — into a rendered 3D
court and back out to the club — is a materially heavier scene transition that was not exercised
here. The "court background behind Zeta with a black bar" and "character geometry briefly glitches
during scene transitions" symptoms are also untested. Evidence is under `logs/issue29_20260725/`
(gitignored — game-derived pixel data).

## 2026-07-25 — Branch/PR convergence, and the first-match route re-proved on exact `main`

Seven draft PRs that had accumulated in parallel were reviewed for scope and dependency and landed
individually rather than through the combined integration branch:

| PR | Change | `main` commit |
| --- | --- | --- |
| #127 | Host-neutral VFS guest→host directory path join (#19) | `021c09d` |
| #124 | Guest-heap boundary-tag coalescing (#122) | `7c6197b` |
| #133 | PGD/PGF legal review packets; retracts three PGD overclaims | `d4e8bcc` |
| #132 | Profiler integrity, `lookup_drops`, `SR_GPU_STATS` (#33 prerequisite) | `39e97c1` |
| #130 | Codegen callable vs resume entry semantics (#51) | `a28abb3` |
| #131 | `ExitThread` launcher-wake removal (#126); first production-HLE specimen (#76) | `b716310` |
| #135 | Loader-stall diagnostics; corrected `CpuState.pc` semantics | `c9688a7` |

Splitting the stack rather than merging the combined branch (#134) surfaced three defects that the
combined branch would have carried silently: an `ISSUES.md` entry attributing live play to the heap
fix alone when it was measured on the whole stack; a #51 design document still describing a plan the
implementation deliberately did not follow; and a `heap-selftest` target that was unreachable from
the manager's verification route, so a coalescing regression could not have failed a local gate.
`hst_manager.ps1 -Action Verify` now runs eight suites, including the heap, profiler, and
production-HLE ThreadMan harnesses.

Two PRs were closed without merge and the reasoning recorded on each: **#134**, whose content is
entirely on `main`, and **#136**, a recovered callback-aware `Delay`/`SleepThreadCB` prototype from
62 commits behind `main`. Merging #136 would have *regressed* three things — it reinstates
per-vblank dispatch of every generic callback (the first confirmed defect in #1, which `main` fixed
by making `sr_vblank_dispatch_registered()` a deliberate no-op), replaces the thread-scoped pending
check with a global one, and reverts the UID dispatcher's `a0 = count, a1 = notify arg, a2 = common
arg` ABI. Nothing in it was missing from `main`.

**Route evidence on exact `main`.** A deterministic `SR_PADSCRIPT` replay of
`logs/route_issue126_firstmatch_20260724.pad` (`SR_NOINPUT=1`, Standard profile, `SR_FBSNAP`
enabled) advances from boot through the tutorial, Exhibition match type, difficulty and opponent
selection, the court loading carousel, umpire selection, the coin toss, and into **a live rally in
which points were scored** — captured framebuffers show the match HUD (`Jstar` vs `Lucy`, NOV, 1
SET(S) / 4 GAMES) with the score advancing `0-0` → `0-15` → `0-30`, both players and the ball on a
rendered court, and the stamina meter live.

Across the whole ~36,650-vblank run there were **zero** occurrences of `NULL_CALL`, dispatch miss,
`NONPLT_MISS`, `HEAP_ALLOC: fail`, `HEAP_FREE_LIST_CORRUPT`, `HEAP_SMASH`, unknown NID,
`TRACE_STOPUNLOADSELF`, or the no-frame watchdog. The `SR_FBSNAP` captures stay in the ignored
`logs/` tree — they are game-derived pixel data and must never enter Git history.

That establishes the two gating defects are gone and that the project reaches active gameplay. It
does **not** establish visual fidelity, audio completeness, or performance: the P0 visual route
(#29) is still unfixed, music still does not play (#32), the intro-movie PSMF path still delivers no
data (#31), performance is unmeasured (#33), the controller path used scripted rather than physical
input (#34), and no route has been validated against an external oracle. GitHub Actions remained
unavailable repository-wide throughout (#27), so every gate above is local-only.

## 2026-07-24 — The character-resource singleton teardown race, and how it was proved (#126)

**Root cause.** `h_ExitThread` carried a runtime-only compatibility convention: any finishing
thread that was neither root nor launcher credited the launcher with a `sched_thread_wakeup`,
modelling an SDK teardown path in which a worker issues `sceKernelWakeupThread` before exiting.
The PSP kernel does no such thing — `sceKernelExitThread` terminates the caller and releases
joiners; it does not bank a wakeup for an unrelated sleeping thread. In HST that shortcut let a
short-lived character-resource worker (`0x13a`) wake launcher `0x111`, whose teardown then cleared
the singleton at `0x00341518` while sibling worker `0x13b` was still live. `0x13b` subsequently
dispatched through null. The fix removes the fabricated wake rather than seeding or preserving
guest state artificially.

**How it was proved — and why that instrument is not in the tree.** The adjudication used a
purpose-built, allocation-free provenance timeline (`SR_TRACE126`): a 128-entry ring recording
every write to the singleton global, every write to the `+0x3e8` / `+0x3ec` handler fields of the
object it points at, and the consumer dispatch at `ra = 0x0017dbcc`, dumped when the consumer
observed the transitions or when the no-frame watchdog fired. A plain `fprintf` watchpoint in the
store/dispatch hot paths perturbed timing enough to make the competing "second construction
replaces the singleton before its handler is installed" hypothesis impossible to adjudicate; the
ring did not. It refuted that replacement hypothesis and established the wake-teardown ordering
instead.

That instrument is deliberately **not** merged. It hard-codes this title's addresses
(`0x00341518`, `0x0017dbcc`, `+0x3e8`/`+0x3ec`) inside the generic `src/rt/recomp.c`, and it adds a
fourth cold branch to `sr_w32_pc` — the hottest guest store path — for a defect that is now fixed.
Both are exactly what [#20](https://github.com/Jstar269/nakagawa-recomp/pull/20) exists to retire.
It is preserved verbatim and can be resurrected as-is if a #126-class singleton/teardown ordering
question recurs:

| Item | Where |
| --- | --- |
| `SR_TRACE126` provenance ring, watchdog hook, `sr_trace126_*` API | commit `aaeed1a` on branch `archive/issue-126-singleton-provenance` |
| Corrected dispatch call site / stalled-thread string resolution | commits `8da0d7e`, `d590778`, `5c01d7c` on the same branch (see PR #135) |

Resurrecting it means cherry-picking `aaeed1a`'s `recomp.c`/`recomp.h` hunks onto a scratch branch;
it is not intended to return to `main`.

**What did land** (PR #131): the `h_ExitThread` correction, plus the first executable
production-HLE specimen for [#76](https://github.com/Jstar269/nakagawa-recomp/issues/76) —
`src/rt/hle_thread_selftest.c` links production `hle.c` and drives `sr_syscall(0xaa73c935)` against
the real scheduler and coroutine backend, asserting that the exiting worker becomes dormant with
its signed exit status, that a `WaitThreadEnd` joiner is released with no stale wait reason, and
that an unrelated sleeping launcher neither wakes nor accrues a `wakeupCount`. A source-level
tripwire in `tools/test_sched_invariants.py` fails if `sched_thread_wakeup` reappears in
`h_ExitThread`.

## 2026-07-24 — Guest code address zero made first-class in the dispatch table (#45)

The dispatch table used `addr == 0` as its empty-slot sentinel and guarded the L1 fast path
with `addr != 0`, so `sr_register(0, f_00000000)` was indistinguishable from an unused slot
and `sr_lookup(0)` could never succeed — even though the recompiled image is based at 0 and
`f_00000000` (`jr ra; nop` at image offset 0) is a real, registered function reached by two
direct `f_00000000(s)` call sites. The table logic was extracted to `src/rt/dispatch_table.h`
(matching the `evf.h`/`vfs_path.h` pure-header pattern) and reworked so occupancy lives in a
dedicated `state` field, independent of the key; the L1 packs `((slot+1)<<32)|addr` so an
all-zero word still means "empty" for the legitimate `(slot 0, addr 0)` entry. `recomp.c`
now wraps the shared primitives. The same zero-as-empty error was fixed here in the
dispatch-trace dump (`g_dtrace`), which previously dropped a real null-dispatch record; the
matching profiler (`g_prof_table`) fix landed separately as part of the #33 profiler-integrity
work (#132), which owns profiler occupancy, `lookup_drops`, and `make profiler-selftest`.

Making address 0 representable does **not** alter the currently observed HST NULL path,
because the existing HST-specific `NULL_CALL_B` exact hook runs **before** `sr_lookup` in
`dispatch()` and consumes a computed target 0 there — so #126's genuine null handler stays
diagnosed, not silently executed. This is current-HST compatibility policy, not a generic
rule: the dispatch **table** is deliberately policy-free (it never decides what a pointer
*value* of 0 means), and the general question — an address-taken offset-0 code pointer
carried through guest data also arrives as integer 0 and is indistinguishable from NULL
without image/module identity — remains **unresolved under #45** (its policy generalization
is owned by #20/#45). Host-neutral coverage: `src/rt/dispatch_selftest.c` /
`tools/test_dispatch_c.py` (register/look up address 0, hash collisions involving 0 in both
orders, L1, re-registration, a real offset-0 function that executes vs. an unregistered
lookup that does not; the `NULL_CALL_B` check is a labelled structural tripwire). Full local
gates green; GitHub Actions remains account-blocked (see #27). Representation fixed; the
general module-base-identity residual is ProgramImage work, not exhibited by this title.

## 2026-07-23 — Upstream notice inventory (#103 resolved); font provenance recorded (#99 **still open**)

Established the Upstream Source File Inventory in `NOTICE.md` covering the PPSSPP-derived runtime
C modules (`ge.c`, `hle.c`, `pgf.c`, `mpeg.c`, `audio.c`, `vfpu_interp.c`, `nid_names.h`), and added
`THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt`, enforced by `tools/publish_audit.py` `REQUIRED_PATHS`.

**Corrected 2026-07-24.** This entry originally read "#99, #103 Resolved" and said the font work
"documented exact replacement PGF font licensing". Both claims were too strong and #99 has been
reopened. What was actually established is *byte* provenance — each blob is byte-identical to a
pinned PPSSPP commit, with ancestry traced through full upstream history, and the embedded
PSP-style family names shown to be deliberate compatibility identifiers (upstream `dc34bea8d`,
"camouflage the Font name"). What was **not** established is the source-font license chain: the
originating TTF is unrecorded upstream for `kr0.pgf`, `ltn0.pgf`, and `ltn8.pgf`, and `jpn0.pgf`'s
Ume lineage is known only through an upstream issue quote. The file initially asserted
GPL-2.0-or-later; that is PPSSPP's *program* license and is not inherited by a redistributed font
binary, so the assertion has been removed rather than reworded. #103's notice inventory is
independent of this and stands on its own — it does not imply #99 is answered.

## 2026-07-23 — Host VFS path joining normalization (#19 resolved)

`host_dir_path()` concatenated the VFS root and the guest path with no separator, so root `fs` and
guest `ms0:/PSP/SAVEDATA` produced `fsPSP\SAVEDATA` instead of `fs\PSP\SAVEDATA`. The join now lives
in the host-neutral `src/rt/vfs_path.h` (`sr_vfs_host_dir_path`) — device-prefix stripping,
leading-separator handling, `..` traversal refusal, bounded output with explicit overflow failure —
with `host_dir_path()` in `src/rt/hle.c` delegating to it and keeping the `_mkdir` side effect and
the Windows separator at the call site. Covered by `src/rt/vfs_selftest.c` and `tools/test_vfs_c.py`,
which compile and run the helper without the Windows-only runtime.

<details>
<summary><strong>Show the complete historical record through 2026-07-18</strong></summary>

## Archived issue tracker

**Last verified before archival:** 2026-07-18

**Superseding provenance annotation (2026-08-09):** The quoted July 18 claims below are preserved as
contemporary development evidence, not maintained provenance conclusions. Recovered private-archive
timestamps/blobs support Python-first → C-port to high confidence, but full source archaeology now
classifies the PSP-specific BBMac/BBCipher/PGD flow as **derived-translated**. The computed AES and
later hardening remain independently expressed, and no substantial near-verbatim Nakagawa function
body was found. See
[`provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md`](provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md).

> **2026-07-18: headless Ghidra pipeline stood up; 16 latent dispatch-miss functions
> fixed durably; both "unexplained" stubs eliminated; NID name table populated.**
> The user supplied Ghidra 12.1 + the ghidra-allegrex 12.1 extension (user-level
> install) + PPSSPP binaries under `third_party/` (all gitignored). Everything below
> is scripted/headless — see the new `docs/GHIDRA.md`; no GUI steps anywhere.
>
> 1. **New tooling (committed):** `tools/ghidra_headless.py` (analyze /
>    export-functions / decompile / refs / info; logs to `logs/ghidra_*.log`; project
>    at `third_party/ghidra/projects/HST`, image base confirmed 0x00000000 = the
>    pipeline's own base-0 view, so no address translation), plain-Java GhidraScripts
>    in `tools/ghidra_scripts/` (`ExportFunctionsCSV`, `DecompileList`, `ListRefsTo`),
>    and `tools/ghidra_crosscheck.py` (diff Ghidra's 8.5k-function inventory against
>    `tools/analyze.py`; `--strict`/`--json`). Gotcha encoded in docs: installing
>    ghidra-allegrex at BOTH user level and distro level kills every launch with
>    "Multiple modules collided".
> 2. **Crosscheck found 16 real analyze.py misses on day one** — functions reached
>    only by cross-function `j` tail jumps (shared-return / split-cold-path /
>    trampoline idiom; one had 12 distinct callers), absorbed into whichever function
>    linearly covered them, leaving the tail `j` with no dispatch entry: silent
>    `NONPLT_MISS` of the `0xe1724` class, now fatal-by-default per 2026-07-17 §1.
>    **Durable fix in `tools/analyze.py`** (not another `known.add` list): a new
>    tail-call promotion pass promotes any swept `j` target whose preceding slot pair
>    ends in a hard terminator (`jr`/`j`/unconditional `b` — no fallthrough can enter
>    it) to a real entry; codegen's continuation machinery then handles both owners
>    correctly. +84 entries binary-wide (13511 → 13595); the 2 non-promoted
>    Ghidra-only leftovers (`0x3d334`, `0x3d370`) are verified benign bottom-tested-
>    loop artifacts on Ghidra's side (its `thunk_FUN_*` splits), documented here so
>    nobody re-triages them. Unconditional-`b` tails deliberately NOT promoted (503
>    candidates, zero evidence of need — gcc emits `j` for tails; crosscheck is the
>    standing tripwire if one ever appears). Tests: `tools/test_analyze_tailcall.py`
>    (pure predicate everywhere + EBOOT-gated end-to-end, including must-NOT-promote).
> 3. **Both `unexplained` custom stubs eliminated — the category is now empty.**
>    Ghidra decompile+refs+raw-byte review proved each was *shadowing real,
>    fully-translatable code*: `0x1c008` is `jr ra; sw a1,0x4028(a0)` (the stub
>    dropped the delay-slot store — jal-called from `0x46c4c`/`0x46cc4` in the
>    game-loop region — and faked `v0=0x30ab9c`); `0x1a5f8` is a computed-goto
>    state-machine resume point (`andi v0,s5,0x80` chain) reached via `.data`
>    pointer tables at `+0x21f8`/`+0x2200` (stub faked `v0=0x1000000`). Both are
>    discovered entries; the stubs were deleted from `tools/codegen.py` and the
>    manifest, and the real emissions are guarded by
>    `tools/test_codegen_no_shadow_stubs.py`. All opcodes in both flows verified
>    supported (`ext`/`ins` SPECIAL3, `beql` likely-branch machinery).
> 4. **`src/rt/nid_names.h` populated: 0 → 1623 entries** (was an empty table since
>    its PPSSPP source files were never in-tree). `tools/gen_nidnames.py` now reads a
>    sparse PPSSPP clone (`third_party/ppsspp-src`, Core/HLE only, pinned commit
>    recorded in the generated header), searches both expected source dirs, and
>    refuses to regress a populated table to empty. Spot-verified against known NIDs
>    (`sceUmdWaitDriveStatCB=0x4a9e5e29` matches the 2026-07-13 boot-fix finding).
>    Unknown/unimplemented NIDs now name themselves in logs.
> 5. **Backlog reference:** a user-supplied legal/redundancy/publishing audit
>    (`Downloads/legal_and_redundancy_audit_.md`, 2026-07-18) lists pre-publish work
>    (dead-file deletions, libxb vendoring, repo disassociation w/ GPL attribution
>    constraints, CI/CD/CodeQL/issue-forms, perf leads incl. Vulkan barrier precision
>    and codegen register caching, and a claimed post-savegame `sceIoIoctl`
>    `0x63632449` exit(7) blocker). Claims are unverified leads — same rule as the
>    2026-07-17 external review (which was wrong once): verify before acting.
> 6. **`sceIoIoctl` implemented (`src/rt/hle.c` `h_IoIoctl`) — the audit's claimed
>    blocker was confirmed live and is cleared.** With the `fs/` GAMEDATA fixture
>    present, boot takes the save-read path and exited(7) on the unregistered NID
>    (verified in a strict 120s run before the fix; the populated NID table named it
>    unprompted). Implemented PPSSPP-faithfully over the flat `Fd` model: sector
>    size / offset / ioctl-seek / start sector / 64-bit size / ioctl-read / tell
>    (`0x01020003/04, 0x01010005, 0x01020006/07, 0x01030008, 0x01d20001`) plus the
>    PGD trio (`0x04100001/02/10`) for the plaintext path; unknown cmds print one
>    loud line per unique cmd and return `0x80010086` like PPSSPP's fallback.
>    Post-fix strict 120s run: healthy, frames presenting, zero misses/heap
>    failures. **Discovery unlocked by the handler's magic probe:** the fixture
>    `fs/ms0__PSP_SAVEDATA_UCUS98701GAMEDATA_GAMEDATA.BDL` is a real `\0PGD`-
>    encrypted file — and it is **433 MB dated May 19**, i.e. the game's *installed
>    data cache* from a real PSP, not a small savegame. The game passes its own
>    16-byte PGD key in the ioctl (`indata=0x002c0db4`, `.data`). Current behavior
>    is honest-fail (`0x80510204` → game proceeds via fresh-boot/UMD-streaming
>    path). **Follow-up (own session): port PPSSPP's KIRK engine + PGD layer**
>    (`ext/libkirk`, `Core/PGD.cpp`/`amctrl` — extend the sparse clone) and wire
>    `pgd_open`/block-decrypting reads into `Fd` for `0x40000001`-flag opens; the
>    game-supplied key makes decryption feasible without console secrets. Plausibly
>    material to the reported slow/briefly-corrupted court loads (the game
>    currently cannot use its install cache at all).
> 7. **Original, third-party-free PGD decryptor written and fully verified**
>    (`tools/pgd_decrypt.py`, `tools/test_pgd_decrypt.py`). Directly serves the
>    "minimize third-party reliance / make the code original" goal: rather than
>    vendor PPSSPP's GPL libkirk or depend on an external decrypt tool
>    (pspdecrypt.exe only does PRX/IPL/PSAR — NOT PGD savedata, confirmed), this
>    is a clean-room implementation. AES-128 is built from the GF(2^8) field
>    (S-box and Rcon computed, no copied tables) and self-checks against the NIST
>    FIPS-197 vector; KIRK cmd4/7 + amctrl BBMac/BBCipher follow the documented
>    algorithm using only **public** PSP constants (keyvault seeds 0x38/0x39/0x63,
>    the DNAS/amctrl mixing keys). **No console fuse key is involved** for this
>    file (drm_type=1 → fixed-key path), so no per-console secret is needed.
>    Verification is intrinsic and complete: the header's MAC(0x80) is computed
>    under a public key and my code reproduces it byte-for-byte; MAC(0x70)
>    additionally confirms the game's version key (EBOOT `.data` @0x2c0db4 =
>    `dcc1da82…d959`, derivable, not supplied); decrypted params are sane
>    (data_size 427,069,696, block_size 1024, data_offset 0x90); block-0 plaintext
>    is structured real data. **Consumer-setup impact: nothing to download** — the
>    key is in the game, the algorithm is ours.
> 8. **PGD decryption is now LIVE in the runtime — the game reads its install
>    cache.** Ported the verified algorithm to C (`src/rt/pgd.c`/`pgd.h`,
>    self-contained: AES from a computed S-box, KIRK cmd4/7 + amctrl, no CpuState
>    deps) and wired decrypt-on-read into `src/rt/hle.c`: on `sceIoIoctl`
>    0x04100001 the fd's version key opens a `SrPgd`; on success the fd flips to
>    the logical decrypted view and `h_IoRead` decrypts on demand with a one-block
>    cache. **Safe-by-construction**: `sr_pgd_open` returns NULL unless the header
>    MACs verify, and on NULL the handler returns the same 0x80510204 as before,
>    so a wrong/unsupported file still falls back to UMD streaming — no regression
>    path. Verified in three layers: (a) the C AES passes NIST FIPS-197; (b) the C
>    output is **bit-exact** against `pgd_decrypt.py` on blocks 0..3 (guarded by
>    `tools/test_pgd_c.py`, which compiles `pgd.c` standalone and diffs the oracle
>    — the first port had an uninitialized GF-table entry, caught here); (c) a live
>    120s strict headless run shows `PGD decryption active (logical size=427069696,
>    block=1024)` and the game's own reads returning the correct decrypted bytes
>    (`first8=46421f2aab143265` = block-0 plaintext), staying healthy (frames
>    presenting, zero misses/heap failures — same depth as baseline). The version
>    key is captured from the game's ioctl at runtime (no build-time secret).
>    `libkirk` was read once as an algorithm reference only; nothing from it ships.
>    - Not independently confirmed yet: whether using the cache measurably speeds
>      up court loads (needs a driven in-match run; the decrypt mechanism itself is
>      proven working). The AES here is straightforward byte-oriented C — fine for
>      the observed read sizes; revisit with T-tables only if a full-cache read
>      ever shows up as a load stall.

> **2026-07-17: correctness-tooling hardening pass + first live match played.** The
> player reached and completed part of a real match interactively (Hugo vs. Emily,
> court loaded, points scored) — see the match-session findings and the four items
> below. Separately, a source review (external, user-relayed) flagged risk areas;
> its claims were independently verified against the actual code before acting (one
> claim, a supposed watchpoint-size typo in `sr_check_mem_watch`, was **refuted** —
> that function's third parameter is a `write` boolean, not a size, and `1` is
> correct for all three write-width accessors). Confirmed and fixed:
>
> 1. **`SR_DISPATCH_FATAL=1` is now the default** in every `hst_manager.ps1` profile
>    (Standard/Performance/Benchmark/Diagnostics/Software) — a screen reached only
>    because an unresolved indirect-call target returned a silent sentinel zero can
>    no longer pass as verified progress by default. Explicit, alarmingly-named
>    opt-out for controlled bring-up: `SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS=1` (set
>    before invoking `hst_manager.ps1`). The "emit every new unique dispatch miss"
>    mechanism (`DISPATCH_MISS_NEW` live print + `dump_dispatch_misses()` at
>    `atexit`) was audited and found **already correct** — it runs unconditionally
>    for both PLT-range and non-PLT misses before any fatal/permissive branching;
>    no code change was needed there.
> 2. **Root-caused and fixed the one dispatch miss this flip would otherwise have
>    hit immediately**: `target=0x000e1724` (seen live during last night's match,
>    `stderr_run.log:629`). It is a two-instruction identity-leaf element
>    constructor (`jr ra; move v0,a0`), address-taken only via `la` and passed as a
>    callback into the generic array-initializer `f_000008d8` — completely benign
>    (writes no memory), but undiscoverable by `tools/analyze.py`'s
>    `_is_trailing_epilogue` heuristic, which false-positives whenever such a leaf
>    sits 8 bytes after an unrelated function's real epilogue. Immediate fix (same
>    pattern as the pre-existing `0x5a648`/`0x42998` entries): `tools/codegen.py`
>    now force-includes five confirmed siblings of the same shape in `known`
>    (`0xe1724`, `0xe3b24`, `0x56098`, `0x57344`, `0x14430` — each independently
>    byte-verified in `hst_image.bin` before adding). Durable root-cause fix
>    (`_is_trailing_epilogue` should not suppress `la`-materialized addresses) is
>    **not yet done** — tracked below. Rebuilt and verified: a 100s strict headless
>    run (`SR_DISPATCH_FATAL=1`) now completes with zero `DISPATCH_MISS_NEW` for
>    `0xe1724` and zero fatal exits, reaching the same depth as prior healthy runs.
> 3. **Semantic-debt manifest landed**: `tools/compat_overrides.py` classifies every
>    custom codegen stub (`GUEST_PATCHES`, `HST_SIMPLE_STUBS`, the per-address
>    custom-stub block) and every `src/rt/recomp.c` dispatch hook
>    (`g_exact_hooks[]`/`g_range_hooks[]`, 18+1 entries) as `faithful_abi_bridge`,
>    `hle_boundary`, `temporary_compatibility_patch`, `diagnostic`, or
>    `unexplained` (two stubs, `0x1a5f8` and `0x1c008`, have no recorded rationale
>    and are genuinely unexplained — flagged, not fixed). `tools/test_compat_manifest.py`
>    is a **CI-enforced completeness gate**: it mechanically re-extracts both
>    sources from the live source files and fails if either contains an address the
>    manifest doesn't know about, or if the manifest claims an address that no
>    longer exists (stale-entry detection). Verified the gate actually catches a
>    new undocumented hook (not vacuously green). Deliberately out of scope for the
>    automated gate (documented manually instead, in `compat_overrides.DIAGNOSTIC_GROUPS`/
>    `SCHEDULER_HOOKS`): the several dozen scattered `s->pc == 0x...`/`entry == 0x...`
>    diagnostic trace points across `src/rt/sched.c` (too many call shapes for a
>    robust regex, and read-only/env-gated by construction), and the
>    behavior-altering scheduler hooks (worker thread reuse, launcher priority
>    demotion, one callback-walker terminal-miss patch) — each is documented with a
>    real category but not mechanically cross-checked yet. Per-hook *behavioral*
>    regression tests (proving what each hook does, not just that it's documented)
>    remain a follow-up for all but the allocator bridge, which already has one.
> 4. **Three narrow, independently-verified correctness bugs fixed**, each with a
>    new regression test: (a) `tools/prxload.py`'s `R_MIPS_HI16` pairing loop
>    accepted *any* relocation type as the paired low half instead of specifically
>    `R_MIPS_LO16`/`R_MIPS_16` — an intervening unrelated relocation (e.g.
>    `R_MIPS_32`) could be misread as the low addend, corrupting the reconstructed
>    high immediate (proven with a synthetic case: `hi=0x0000` correct vs.
>    `hi=0x0001` under the old code) — fixed, `tools/test_prxload_relocations.py`
>    (6 tests). (b) `src/rt/recomp.h`'s `sr_inrange` bounds check validated only the
>    *first* byte of a multi-byte access, so a 4-byte read/write starting near the
>    top of the 0x0c000000 arena could read/write up to 3 bytes past the actual
>    allocation — fixed with a new width-aware `sr_inrange_n`, used by
>    `sr_r16`/`sr_r32`/`sr_w16_pc`/`sr_w32_pc` only (unrelated call sites
>    elsewhere in the tree were left on the original `sr_inrange`, out of scope).
>    Also replaced those same four accessors' raw pointer casts with
>    `memcpy`-based load/store (undefined behavior under strict aliasing/alignment
>    rules, though harmless in practice at this project's current `-O0`-only
>    generated-code policy). (c) `tools/codegen.py`'s `madd`/`msub` (SPECIAL funct
>    `0x1C`/`0x2E`) accumulator was built via
>    `(int64_t)(int32_t)s->hi << 32` — left-shifting a *negative* signed integer,
>    undefined behavior in C (the sibling `maddu`/`msubu` were already correct).
>    Fixed to match their unsigned-accumulator shape; the fix is bit-identical on
>    every real compiler (verified: the pre-shift sign vs. zero extension is
>    provably irrelevant once shifted left exactly 32 on a 64-bit value — proven
>    both by hand and by a compiled test that runs the actual emitted C snippet at
>    `-O0` and `-O2` and checks the numeric result against an independent Python
>    oracle). `tools/test_codegen_madd_msub.py` (4 tests, requires a host C
>    compiler; skips gracefully if none is on `PATH`). All 37 Python tests pass
>    (`python -m unittest discover -s tools -p "test_*.py"`, plus `ruff check tools`
>    clean). No git actions taken.
>
> **Also investigated this session (from the match log, `logs/stderr_run.log`,
> 1094 lines) but not yet fixed:**
>
> - **Random pause-screen mid-match**: `src/rt/hle.c`'s `h_CtrlButtons()` (~line
>   3742) has a genuine, confirmed latent defect — its "auto-pulse START to skip
>   the unattended intro" feature (meant for headless/no-input runs) is suppressed
>   only by `SR_NOINPUT` or a real gamepad being connected, with **no check for
>   live keyboard activity or game phase** — the author's own comment names this
>   exact failure mode. However, adversarial re-verification found the *specific*
>   `dialog_open`/`dialog_close` sound-cue pairs in last night's log do **not**
>   match this mechanism's fixed ~4-second cadence (they cluster right after the
>   coin-toss/pre-match confirmation sequence, then go silent for minutes during
>   real rally play — the opposite of a periodic phantom press) — those specific
>   events are more likely the game's own normal UI dialog cues. The code defect is
>   real and worth hardening regardless (add a keyboard-seen or game-phase gate,
>   being careful not to break existing headless/CI runs that rely on the
>   unconditional pulse), but is **not confirmed** as the cause of an actual pause
>   screen appearing. Next diagnostic: an `SR_`-gated log at the pulse-fire site
>   correlated with the actual pause-dialog open, from an interactive session.
> - **Heap corruption / UFL record-vector use-after-free — ROOT-CAUSED AND FIXED
>   2026-07-18.** A fresh allocator trace, guest-memory watch, native GDB backtrace,
>   and Ghidra decompilation established the complete causal chain. Worker `0x115`
>   allocates the `0x220` vector at `0x0a0655b0` while parsing
>   `disc0:/PSP_GAME/USRDIR/umd.ufl`. Before launcher `0x111` reaches its first
>   `sceKernelSleepThread`, the UMD HLE banks two hardcoded
>   `sched_thread_wakeup(0x111)` tokens. Sleep therefore returns immediately and
>   `f_000465dc` runs the global-destructor list; `f_00048e7c` frees the vector while
>   worker `0x115` still owns it. The worker's `swc1` record stores at guest
>   `0x00048014..0x00048020` then overwrite the recycled header (the corrupting
>   field-2 store is `0x0004801c`). This was an HLE lifecycle bug, not an allocator
>   metadata-design failure. The fix removes launcher wakeups from
>   `sr_umd_signal_ready` and `h_UmdRegisterUMDCallBack`; UMD readiness now wakes
>   only UMD-object waiters and dispatches its callback, matching PSP behavior.
>   Post-fix, launcher sleep reports `wakeups=0`; the vector grows to `0x370`, frees
>   the old block normally, and the watch shows no stale record write after reuse.
>   `BuildFast`, the C++ selftest, all 54 Python tests, and a 45-second
>   `SR_DISPATCH_FATAL=1` smoke pass completed with zero heap/dispatch failures.
>   The remaining deterministic free-list quarantine was a separate allocator-ABI
>   problem, root-caused and fixed below.
> - **Host/retail allocator metadata mismatch in `_memalign_r` and `_realloc_r` —
>   ROOT-CAUSED AND FIXED 2026-07-18.** After the UFL lifetime repair, the new
>   `SR_HEAP_WATCH` free-header provenance diagnostic caught the first corruption
>   exactly: header `0x0b238518` changed from size `0x16930` to `0x16931` at guest
>   PC `0x000102b0` (`ra=0x00010248`, worker `0x115`). Ghidra decompilation identifies
>   the containing routine as retail newlib `_memalign_r`. Its translated body
>   correctly sets dlmalloc's bit-0 `PREV_INUSE` flag in the following chunk, but the
>   host arena uses bit 0 to mean the *current* block is allocated. The next host
>   allocation therefore saw an apparently allocated block in its free list and
>   quarantined the list. `_realloc_r` was audited at the same time and also directly
>   walks, unlinks, and rewrites dlmalloc chunks. Both entries are now codegen ABI
>   bridges (`0x000101c4` → `sr_newlib_memalign`, `0x00013524` →
>   `sr_newlib_realloc`), alongside the existing malloc/free bridges; no guest
>   dlmalloc metadata is mixed with host headers. Verification: all 54 Python tests,
>   a full generated-chunk rebuild, the C++ selftest, a 45-second replay, and an
>   extended 120-second `SR_HEAP_WATCH=1` headless replay pass with zero
>   `HEAP_HEADER_WRITE`, `HEAP_FREE_LIST_CORRUPT`, `HEAP_SMASH`, or `HEAP_ALLOC`
>   failures. The host bridge is now an explicit allocator-ABI boundary, not a
>   temporary mask for this corruption.
> - **Missing textures/backgrounds and audio silence**: not conclusively
>   root-caused this session (the audio probe and one heap-verify pass both hit a
>   session/rate limit before completing) — do not treat as diagnosed.

This is the only live status document. Detailed superseded investigations and the former full tracker are retained locally under the git-ignored `Archive/2026-07-13-release-prep/`.

> **2026-07-18 supersession:** the first P0 row's final “find the bad-free /
> use-after-free source” follow-up is resolved by the UMD lifecycle finding above.
> Its later one-per-boot free-list quarantine is also resolved: retail
> `_memalign_r`/`_realloc_r` were editing dlmalloc metadata inside the host-header
> arena. All four metadata-manipulating allocator entries now share the host ABI.
> The historical P0 table row below is retained as evidence, but its “remaining
> correctness follow-up” paragraph is superseded by these two findings.

> **Fresh agent session?** Read `docs/archive/NEXT_SESSION_PLAYBOOK.md` (local-only archived file) —
> a self-contained roadmap from the current main menu to a playable match (build/run/verify
> toolkit, the frontier-clearing method, and the four workstreams in priority order).

## Current execution status

The US PlayStation Store build now boots through the English title, accepts keyboard input,
creates/names a saved profile through the host OSK, enters Story Mode, passes the first tip, and
loads the fully rendered 3D Nakagawa Tennis Club lobby on the real Vulkan path. Multiple animated
characters render correctly, the story advances, and the player can close the window normally.
The title logo and menu text render, but a fresh Standard run confirmed that the background is
incorrectly filled with repeated UI/localization words instead of the intended title backdrop.

There is no longer a known hard loading wall before the lobby via interactive/GUI runs, but a
**2026-07-16 headless regression was found and fixed**: a bounded `--sched` smoke run
(`SR_HLELOG=1`) reproducibly hung forever inside `sceUtilityLoadModule(module=0x302)` (libfont) —
see the P0 row below for the root cause and fix. The next progression milestone is to drive the
controllable lobby route into the first tennis activity/match and capture the first precise
failure there. The character-loader lifetime issue, `midashi.lay` parser failure, and post-tip
malformed model paths are all root-caused and live-verified past their former screens.

> **2026-07-16 late evening: boot restored and improved.** The 16:44 build's no-frame
> regression (retail-allocator un-bridging corrupting its own dlmalloc bins — see the resolved
> P0 row below) was fixed by re-bridging `_malloc_r`/`_free_r` to the hardened host allocator
> and moving that allocator's arena to `[0x0a000008, 0x0c000000)` (32 MB, overlap-free — the
> old 16 MB home was genuinely too small and its former 0x08000000 ceiling had silently
> overlapped VRAM). Verified same evening: 120 s strict headless (`SR_DISPATCH_FATAL=1`) and a
> 100 s GUI/Vulkan run both reach the **fully rendered TITLE screen** (logo sequence → backdrop
> → NEW GAME/CONTINUE), zero dispatch misses, zero heap failures, and the title backdrop now
> shows the real court scene — the P1 "background filled with repeated UI text" symptom is
> absent in both captures.

## Active blockers

| Priority | Issue | Current evidence | Next action |
| --- | --- | --- | --- |
| P0 | ~~Boot never presents a frame since the 2026-07-16 16:44 rebuild~~ **FIXED 2026-07-16 late evening** — retail newlib `_malloc_r` walked a corrupted bin forever while holding the malloc interrupt-suspension, freezing the cooperative scheduler (`--sched` AND `--gui`). **Fix (two parts):** (1) reinstated the `_malloc_r`/`_free_r` → `sr_newlib_malloc/free` bridge as codegen custom stubs — now at the inner entries `f_00010738`/`f_0000f538` (safer than the old public-wrapper placement: direct `_free_r` callers are bridged too); the real dlmalloc body must not run again until the underlying bad-free/overrun source below is fixed (guarded by `tools/test_codegen_retail_allocator.py`). (2) Moved the host arena from `[0x03000008, 0x04000000)` (16 MB — exhausted during the logo sequence once its former VRAM-overlapping 0x08000000 ceiling was correctly shrunk) to `[0x0a000008, 0x0c000000)` (32 MB, provably overlap-free; game's own UserSbrk budget is 20.25 MB). Verified: 120 s strict headless run presents 267 frames (f=1500..6907) reaching the correct TITLE screen; 100 s GUI/Vulkan run matches; zero `HEAP_ALLOC` failures, zero dispatch misses/`NULL_CALL`s. A new `HEAP_SMASH` failure dump (recomp.c) names the walk-breaking header + predecessor block if exhaustion ever recurs. | Verified 2026-07-16 evening with three independent probes (all runtime, no source reverts). (1) `SR_PCSAMPLE=1`: vblank delivery dies ~frame 31; last sample `pc=0x00025a50` (the known cache-flush/plane-copy emulator `f_00025a18`, which `SR_COPYSPIN` shows completing healthily — red herring). (2) New stuck-suspension detector in `sr_yield`: `200k consecutive yields with interrupts suspended (uid=0x115 pc=0x00010c70 ra=0x00010784)` — `0x10c70` is `_malloc_r`'s bin-chain walk (`node = MEM[node+0xc]` until sentinel); its chain dump shows `sentinel=0x2cf8d4 cursor=0x00000000` — a NULL link the circular bin can never recover from, so `__malloc_unlock` (`f_000115b4` → `sceKernelCpuResumeIntr`) is never reached and `sr_yield`'s suspension early-return no-ops every yield (also silencing HEAPSPIN/PCSAMPLE — they sit below that return). (3) `SR_DEBUG=0x1 SR_WATCH_0=0x2cf6d0,0x2cf960,AVBINS` write-watch on the static dlmalloc `av_` array (its self-sentinel init in `hst_image.bin` is byte-verified correct): after thousands of well-formed frontlink/unlink writes, the binblocks bitmap word `0x2cf6d8` — previously only ever 2 or 7 — receives garbage `0xc500022f` from the frontlink/bitmap helper at `pc=0xf640`, one chunk is linked into a wrong large bin (`0x2cf73c` ← `0x3b3b78`), and the next `malloc(0x200)` hangs in the walk. Pattern is a smashed adjacent-chunk header being consolidated (the retired host-bridge allocator did not consolidate and silently tolerated such overruns; real dlmalloc propagates them fatally). The PSP-header BSS repair itself is NOT the cause: malloc lock state (`0x30AA80/84`) and every other probed restored-BSS slot hold correct values at runtime, and the failing code path (translated retail `_malloc_r`/frontlink) had simply never executed before this rebuild — the last healthy build (14:13 matrix `gui_baseline`, JAPAN Studio logo frames captured) predates the entire batch. | Remaining correctness follow-up (now a P2 hygiene item, tracked here): find the bad-free / use-after-free source. Evidence that survives the fix: exactly one `HEAP_FREE_LIST_CORRUPT` quarantine per boot (a freed block's header gets overwritten through a stale pointer — e.g. `header=0x030657a8 size=0x2c7 next=0x00025136`), and `ALLOC_FREE_REJECT` (under `SR_HEAP_DIAG`) shows tens of thousands of garbage frees per run (values like 1/0xff/0xcf). The hardened allocator makes all of this survivable (quarantine + membership check + zero-fill), but the same bad frees are what poisoned real dlmalloc — do not retry the retail-allocator un-bridging until this is root-caused. Diagnostics available: `SR_HEAP_DIAG`, `HEAP_SMASH` dump, `SR_COPYSPIN`, stuck-suspension one-shot + bin-walk dump (sched.c); the `f_0000f538` stub could additionally pass the guest `ra` through to `sr_newlib_free` to name callers. |
| P0 | First match/gameplay has not yet been reached | Story Mode now runs through TIP No. 1 and into a rendered, controllable 3D lobby. No crash or loader spin is present at the former frontier. | Navigate the lobby objective into the first tennis activity, then classify the next failure by NID, dispatch target, resource wait, or renderer state. |
| P0 | HLE NID `0x63632449` (sceIoIoctl) is unimplemented, crashing save-load boot | The game crashes with exit code 7 (unimplemented NID) during save data load: `HLE: unimplemented nid 0x63632449 (sceIoIoctl)` after loading the save file. | Implement `sceIoIoctl` in `src/rt/hle.c` and register the NID (NID: `0x63632449u`, handler signature matches PSP standard `sceIoIoctl`). |
| P0 | ~~`sceUtilityLoadModule(module=0x302)` (libfont) hung forever in a headless `--sched` smoke run~~ **FIXED 2026-07-16** | `h_UtilityLoadModule` in `src/rt/hle.c` called the real recompiled PRX `module_start` for all three system modules it loads (libfont `f_32200000`, psmf `f_32280000`, libpsmfplayer `f_322f8868`) with `a0=a1=0`. Disassembly showed libfont's and psmf's real entry unconditionally dereference/write through `a0`/`a1` with no null check (psmf: `*a0 = 0` is the fourth instruction) — genuine hand-written Sony SDK init code that sets up thread/semaphore machinery assuming a real PSP kernel; fed null args it produced an infinite `sceKernelWaitSema` retry (`HLE: WaitSema uid=0x125 count=1 need=1 (from 0x133)` repeating tens of thousands of times with zero other scheduler activity). Every API these three PRXs expose is already a complete host-side HLE reimplementation (`sceFont*`, `sceMpeg*` "faithful port... from PPSSPP", `scePsmfPlayer*`), so none of it needs the guest `module_start` to have run — the same "don't execute system-module code you've already HLE'd" rule real emulators use. (libpsmfplayer's `f_322f8868` turned out on inspection to not even be real init logic — none of the three PRXs export a func-type `module_start`/`module_stop` NID, and that address is a self-contained 64-bit division helper with no jal/syscalls, apparently just linked first in `.text`.) **A second, independent call path into the same three PRXs was found the same day:** the game also loads `libfont.prx` (and, by the same code, would load `psmf.prx`/`libpsmfplayer.prx`) via the ordinary `sceKernelLoadModule("disc0:/PSP_GAME/USRDIR/module/libfont.prx")` + `sceKernelStartModule(uid,...)` pair, handled by `h_LoadModule`/`h_StartModule` in `src/rt/hle.c` (not `h_UtilityLoadModule`), which called the real `f_32200000`/`f_32280000`/`f_322f8868` entries directly by NID-recorded path. | **Fix applied (both call paths):** `h_UtilityLoadModule` no longer calls `f_32200000`/`f_32280000`/`f_322f8868`; it still registers each PRX's exports (`populate_known_module`) and keeps libfont's `MEM_W32(0x002d132cu, 1u)` "module loaded" flag write. Re-verified with the same headless `--sched` + `SR_HLELOG=1 SR_DLGLOG=1` invocation: the `uid=0x125` avalanche is gone, all three `sceUtilityLoadModule` calls (module 0x300/0x301/0x302) complete, and execution proceeds into audio-subsystem init and further `sceDisplaySetFrameBuf` presents (watchdog `no_frame_vblanks` counter resets past the title screen) within a 90s bounded run. The three `h_StartModule` branches got the identical treatment (skip `f_32200000`/`f_32280000`/`f_322f8868`, keep the informational log line, don't repeat `populate_known_module` since `h_LoadModule` already called it) — confirmed via log that `sceKernelLoadModule("...libfont.prx")` and the subsequent `sceKernelStartModule(uid=...)` still fire and no longer invoke the real entry. **Honest caveat found during this second re-verification:** a bounded (~110s) headless `--sched` run with no pad script still stalls — `BOOT_EVENT phase=stalled no_frame_vblanks=...` climbs indefinitely and the presented frame never advances past `vcount=3` — but the log proves this is *not* caused by either module_start fix: the `WaitSema uid=0x125 count=1 need=1 (from 0x133)` loop (thread 0x133 = the audio-output thread, spinning on `sceAudioOutput2Reserve`/`WaitSema`/`SignalSema`/`sceAudioOutput2OutputBlocking`) begins and repeats *before* `sceKernelLoadModule("...libfont.prx")` is even called in the same trace, so it predates and is independent of the `h_StartModule` code path this task targeted. This matches the already-known "headless runs never reach menu" audio-pipeline gap (see the P1 audio row) rather than a new regression or a third libfont/psmf/libpsmfplayer call site — do not extend this fix pattern to chase it without new evidence pointing at a fourth call site. |
| P1 | ~~Title-screen background samples a text/glyph surface instead of the intended backdrop~~ **APPARENTLY FIXED 2026-07-16 evening; interactive confirmation pending** | The 2026-07-14 symptom (repeated `CONTINUE`/`PURCHASE`/`CHANGE MODES` strings covering the background) is absent in both fresh 2026-07-16-evening captures of the same screen state (title + NEW GAME/CONTINUE): the software-rasterizer headless run and the GUI/Vulkan run both show the correct court backdrop. Most plausible cause was the same heap/BSS corruption class eliminated by today's BSS restoration + hardened-allocator work (a stomped texture pointer/global). | Confirm once in a normal interactive Standard run (a human at the title screen). If the text-backdrop ever recurs, capture with `SR_GELOG=1` and a bounded texture/CLUT trace as originally planned. |
| P1 | Main-menu (mode-select) background renders black and the "toplady" hostess character is missing | **2026-07-15, ROOT CAUSE FOUND AND FIXED; visual confirmation pending.** The map-find failure was caused by a rogue write planted inside the `SR_YIELD` macro in `src/rt/recomp.h`: `if (target_pc == 0x00065c60) MEM_W32(r5+4, MEM_R32(r5))` — on every entry to the map-find wrapper `f_00065c60` it overwrote the RB-tree root `[map+4]` with the map's size `[map+0]` (`r5` = the map argument at that entry). That is why inserts always worked (they bypass `f_00065c60`), why every `find` missed, and why the root always read as a "small int matching plausible entry counts" (`0xac`=172, `0x21`=33, `0xfd`=253 — literally the sizes). Proven by host backtrace through `f_00162338→f_00065364→f_00065c60→sr_w32` plus an in-order key dump showing `"ID"`/`"Label"` present in perfectly sorted maps. The earlier RB-tree rebalance write-back hypothesis was fully audited and refuted (rotations write the root back through the pseudo-header node at `map+4`). Trap removed; headless verification now shows `find("ID")` strcmp==0 hits, stable roots across all rows, and zero `NULL_CALL` at `0x00162404`. A separate genuine codegen bug found during the audit (delay-slot-that-is-also-a-branch-label emitted twice; miscompiled `f_00022d7c`'s fill tail) was also fixed in `tools/codegen.py`. | Visually confirm the mode-select screen renders backdrop + toplady once a save fixture or an interactive NEW GAME run is available (headless path still blocked by the Win32 OSK dialog / missing `GAMEDATA.BDL`). Downstream name-keyed lookups (e.g. `sgxsnd` `unknown name ()`) should be re-checked — they shared this root cause. |
| P1 | Audio is partial; some sound resources do not resolve | Footsteps are audible on the tennis court in Story Mode. The EBOOT module log reports `sgxsnd.c` `unknown sequence (0,0,0)` and `unknown name ()`. The independent asset-link validator also found one concrete broken reference: `game/900_sound/020_court07.xb.d/data/sound/SE/co_se07.sgd` names `sq_l44_11.vag`, which is absent from the extracted-file index. | Determine whether `sq_l44_11.vag` exists under another archive/path/case in the source data, then trace how the guest resolves that SGD link before changing fallback behavior. |
| P1 | PSMF player data getters do not produce queued data | `h_PsmfGetVideo` and `h_PsmfGetAudio` in `src/rt/hle.c` return `PSMF_ERR_NO_DATA`; other registered player lifecycle handlers are present. **User-visible instance**: the title screen's idle-timeout attract-mode intro movie shows a black screen instead of video (zero frames ever produced), and a button press correctly triggers the legitimate skip-transition (white flash) back to the start menu — the skip path itself works, only movie frame production is missing. | Implement demux/production and EOS semantics after P0 is cleared. |
| P1 | Vulkan gameplay performance remains single-digit FPS | The lobby is stable but user-observed presentation remains roughly single-digit FPS on an RTX 3080. Bounded 1 Hz telemetry is now implemented. On the reproducible no-input title/main-menu route, the capped O0 runtime averaged 21.1 presented FPS across 98 active intervals, with 538 ms/s CPU/host work, 176 ms/s synchronous GE fence waiting, 10 ms/s presentation, and 349 submits/s. This proves both CPU and GPU-synchronization pressure on that route, but it is not a substitute for a lobby capture. An isolated hand-runtime `-O2` experiment averaged 24.4 FPS over a broad active window; it was not promoted because scene timing differed and lobby parity was not tested. | Navigate to the lobby with `-Profile Benchmark`, capture a stable stationary and movement window, then rank hot guest functions. Preserve `logs/perf.csv`; optimize B1/B4 only if lobby wait time remains material, and B2 generated code if CPU/host time dominates. |
| P1 | ~~Long play sessions freeze on the next allocation after the guest heap arena fills~~ **FIXED 2026-07-16** | `f_00010738`/`f_000104e0` (`src/rt/codegen.py` custom stubs for newlib `_malloc_r`/`_free_r`) delegate to `sr_newlib_malloc`/`sr_newlib_free` in `src/rt/recomp.c`. `_free_r` was always a no-op, so the bump arena (`0x03000008`..`0x08000000`) never reclaimed memory; an extended Story Mode session exhausted it, freezing the cooperative scheduler (`sched: spin on uid ...`). A real free-list-reuse allocator existed but was disabled by default because enabling it traded this bug for a faster-reproducing `NULL_CALL` onto a static vtable at `0x3070c0` within ~45s headless. **Root cause found:** not an `INIT_ARRAY`/constructor-ordering race (that theory was checked and refuted) — `sr_newlib_free` had no check that the pointer it was asked to free actually came from this allocator's arena. A guest free() call path was freeing a foreign/non-heap pointer landing near `0x3070c0` (a static C++ vtable baked into the image as link-time rodata), and the zero-fill loop wiped it out, causing every subsequent object whose vptr pointed there to null-fault on dispatch. | **Fix applied:** added an arena-membership bounds check (`hdr` must lie in `[SR_HEAP_BASE, s_heap_bump_ptr)`) in `sr_newlib_free` before trusting the block header; reuse is now on by default (`SR_HEAP_REUSE_OFF` is the new opt-out). Verified via live A/B repro: 65s headless run with reuse on (default) produces zero `NULL_CALL`s, matching the old reuse-off baseline. |
| P2 | Full verification gates need external oracle inputs | `make verify` requires `CODEGEN_ORACLE`, `MICROTEST_MODULE`, and `MICROTEST_ORACLE`, none of which are redistributable repository inputs | Provide local/CI-secured traces and module, or use the synthetic CI gate and static verification. |
| P2 | Physical controller needs end-to-end user confirmation | SDL has detected the attached gamepad and keyboard navigation works through Story Mode. | Confirm axes/buttons with the user's controller in a fresh Standard run and record the mapping/device name. |
| P2 | NID diagnostics lack names | `src/rt/nid_names.h` contains no database (confirmed: 30 lines, empty table) because its generator (`tools/gen_nidnames.py`) sources from `third_party/ppsspp/Core/HLE/*.cpp`, which is absent from this checkout | Generate names from official PSPSDK NID tables where available; keep any auxiliary source and its license explicit. |
| P2 | Character models transiently corrupt/disappear on scene entry and during movement | Two symptoms may be related: a brief polygon pop at Main Menu entry and player-model corruption/disappearance while moving in the lobby/court. `pick_next()` can force-rotate after three wins without checking priority, which is a plausible but unproven source of GE-list interleaving. The bounded `SR_ROTLOG` diagnostic records `(uid,priority -> uid,priority)`; it does not record a PC. | Capture one movement-triggered glitch with `SR_ROTLOG=1 SR_GELOG=1` and correlate the two streams by order/time. Only if rotations cluster with interleaved GE submissions should the rule be restricted to same-priority threads and then live-tested. |
| P3 | Boot logo/attract sequence may be running faster than the intended ~30 FPS | The new wall-clock counter empirically measured approximately 59.94 delivered vblanks/s in the light startup path, refuting double-vblank delivery. It instead found 42-102 actual host presents/s after rendering began because every `sceDisplaySetFrameBuf` call presented immediately. Host output is now capped at 30 FPS without sleeping guest execution (`SR_FPS_CAP=30`; `0` disables), while PSP vblank remains ~59.94 Hz. | Visually confirm logo duration and animation pacing in a short Performance run. If still fast, the remaining bug is guest timer/logic cadence rather than vblank or host-present frequency. |

## Open backlog

| Priority | Area | Work |
| --- | --- | --- |
| P2 | Audio/video | ATRAC3+/PSMF production remains incomplete and non-Windows H.264 uses the null backend. |
| P2 | Performance | Recompiled MIPS dispatch still pays a table lookup and native call per dispatch. |
| P3 | Graphics | Mipmapping is level-zero only; BJUMP bounding-box behavior remains incomplete. |
| P3 | Savedata | Dialog behavior remains partial/stubbed. |
| P3 | Reference interpreter | COP0, COP2/VFPU, double-precision FPU, `eret`, and TLB operations are incomplete. |
| P4 | Vblank | Runtime handles a single registered handler rather than all callback slots. |
| P4 | Portability | The coroutine layer has Win32-fiber and POSIX `ucontext` backends, but the supported core build remains Windows-only because of the build/link setup, Win32 GUI/OSK paths, and Media Foundation H.264 backend. |
| P4 | Build | Generated chunks require `-O0 -w`; higher optimization can exhaust compiler memory. |
| P4 | Porting | Makefile defaults are generic and HST-specific extra-PRX bases remain centralized constants that must stay synchronized. |
| P4 | Dashboard | Several panels are demonstrations; the downloadable recompile bundle is simulated rather than a real release artifact. **Verified 2026-07-14**: `interface/` builds and typechecks cleanly (`npm run build`, all 31 API routes + static pages compile) — not bit-rotted, just incomplete. **Fixed same day**: added a `postinstall: prisma generate` script to `interface/package.json` so a fresh `bun install`/`npm install` no longer fails on `@prisma/client did not initialize`; re-verified by deleting the generated client and reinstalling. |

## Forward-looking (beyond original-PSP parity — do not start before P0 is cleared)

These are explicitly *not* bugs: the original UMD build never did any of this. Listed here so the ambition is tracked somewhere, deliberately deprioritized behind reaching an actual playable match.

| Idea | Notes |
| --- | --- |
| Performance beyond the PSP's native ~30 FPS ceiling | Once the P1 single-digit-FPS GE bottleneck is profiled and fixed, the Vulkan backend already has headroom to exceed original hardware: `SR_GPU_SCALE` (1-4x internal resolution) and the shader-based blending path are already in place per prior session notes. Real target-framerate uncapping is a design decision to make deliberately, not a byproduct of an unmeasured fix. |
| Visual fidelity beyond original PSP assets | Same rationale — texture filtering/AA/higher internal resolution are cheap to add to the existing Vulkan pipeline once it's not fighting a correctness bug. Do not start on this while P0/P1 are open; it multiplies the surface area to regression-test for no player-facing benefit yet. |
| Online multiplayer (original was local ad-hoc Wi-Fi only) | This is net-new architecture, not a recompilation task — the original game never had a matchmaking/relay server, so this means designing and building one from scratch (or bridging ad-hoc packets over a relay), independent of anything in this recompiler. Do not scope this until single-player is fully playable; the two are unrelated bodies of work. |

## Recently verified repairs

- **The PSP header's omitted 0x42460-byte BSS is restored and the heap can no longer overlap the
  loaded image (2026-07-16, runtime-verified).** The decrypted `eboot.elf` lost its trailing BSS
  (`p_memsz == p_filesz`), so the flat image ended at `0x0030a020` while the original `~PSP`
  header (`original_game/PSP_GAME/SYSDIR/EBOOT.BIN`) declares one load segment of `0x34c480`
  in-memory bytes with `bss=0x42460`. `tools/prxload.py` now reads the header
  (`--psp-header`, wired in the Makefile) and extends the segment, `hst_image.bin` is exactly
  `0x34c480` bytes with a byte-verified zero tail, and `user_partition_init` (`src/rt/hle.c`)
  derives the heap base from `sr_loaded_end()` (`0x0034d000`) and aborts on any override that
  would overlap the image. Runtime-verified in strict headless (`SR_DISPATCH_FATAL=1`, 100 s) and
  GUI/Vulkan runs: partition line correct, first block at `0x0034d000`, zero dispatch misses,
  zero `NULL_CALL`s, zero libc main-thread diagnostics, and restored-BSS globals
  (`libc_main_thid` `0x0030a040`, malloc lock state `0x30AA80/84`) hold live values instead of
  being heap-aliased. The old `0x0030b000 → 0x0030c000` "BSS tail reach-through guard" band-aid
  is retired. Loader regression tests: `tools/test_prxload_psp_header.py` (19/19 Python tests
  pass). Note: the same rebuild's allocator un-bridging regressed boot — see the P0 row above;
  that regression is in the allocator switch, not in this repair.

- **The system allocator (`f_00000a90`) had a codegen custom stub giving it a separate,
  tiny, never-freeing bump region that overlapped the partition/FPL allocator and
  chronically exhausted, breaking the 49157-slot resource-name hash table** (`HASH_GUARD:
  dropped ... base=0`, `A90_ALLOC: fail`). The raw guest function is a trivial 7-instruction
  wrapper (`jal 0x00000bcc`); removing the stub in `tools/codegen.py` lets it translate
  normally and delegate to the same 128MB `sr_newlib_malloc` arena that 6539 other call
  sites already use directly. `f_0001b6c4` (hash insert)'s custom stub was a band-aid over
  the resulting `base=0` and was removed too now that the root allocator issue is fixed.
  Verified via before/after headless smoke test: `HASH_GUARD`/`A90_ALLOC` occurrences go
  from 11/20 to 0; unrelated symptoms (`NULL_CALL`/`WATCHDOG`) are unchanged, confirming
  the fix is real and isolated. This is a plausible contributor to the P1 long-session
  freeze (two allocators handing out overlapping guest memory) but was not re-tested with
  an extended session this pass.
- **Missing dialogue glyphs were stale Vulkan texture-cache data.** The PGF rasterizer now
  notifies the GPU backend of its exact guest buffer range, and cached textures record their
  source range so even small atlas writes force an update without replacing the sparse fast
  hash globally. The formerly incomplete Story Mode sentence rendered with every letter in a
  fresh Vulkan run; clean window exit remained intact.
- **Story progression now reaches the 3D lobby.** `sceKernelStartThread` copies argument blocks
  to the child stack and initializes TLS without overlapping them; the former character loader
  no longer dereferences an expired creator-stack pointer. `sceKernelGetThreadExitStatus` and
  the RTC calls used along this path now have real runtime implementations.
- **The `midashi.lay` parser now builds its tables.** Static call analysis proved
  `f_00010738` is `_malloc_r(reent,size)`, with `size` in `a1`; the custom allocator had guessed
  between `a0`/`a1` and interpreted bytes such as `LLOW` as allocations. The public
  `f_000104b0` `malloc(size)` wrapper now adapts its argument explicitly. Removing the false
  no-op for `f_00107174` also restored the game's vector-capacity growth routine.
- **Post-tip character model paths are ABI-correct.** The custom `sprintf` replacement used
  desktop o32 stack varargs even though this title passes PSP EABI arguments 1-8 in `r4..r11`.
  It now consumes the six varargs after `dst,fmt` from `r6..r11` and only then uses the stack.
  Live output changed from `(null)`/binary paths to valid `face/00/face00.i3r`-style paths, and
  the former black screen advanced into Nakagawa Tennis Club.
- **Build dependency/reporting repairs.** Generated output now depends on
  `tools/host_stubs.py`, and `hst_manager.ps1` waits/refreshes the build process before reading
  its exit state (with a newly linked executable as the safe fallback), eliminating blank false
  failure reports.

- **US locale/resource routing restored.** `tools/extract_xb.py` keeps `.xb`, `.xb0`, `.xb2`,
  and `.xb3` outputs distinct, and the runtime maps `data_00_USE` to the US `.xb0` set. The
  warning and title menu are now English.
- **Input and shutdown are host-event correct.** SDL keyboard/gamepad button-down edges are
  retained until `sr_ctrl_sample` consumes one PSP sample; this prevents short taps from being
  lost during slow rendering. SDL quit is propagated through the Vulkan GE path, so the window
  close button now terminates the process normally. SDL detects the attached controller, though
  physical-controller navigation still needs a user confirmation.
- **Standard-mode telemetry was re-audited.** `DISPLAY_SET_FB`, vblank, GE enqueue/update/sync,
  callback, and the known HLE success traces are behind diagnostic switches. The 2026-07-14 audit
  found two missed steady-path emitters (`GEGPU stats` and `WaitEventFlag`) plus several lower-rate
  HLE messages; all are now gated. Renderer performance must be remeasured with the rebuilt binary.
- **The first `NEW GAME` kernel gap is implemented.** `sceKernelCreateMsgPipe`,
  `sceKernelDeleteMsgPipe`, `sceKernelTrySendMsgPipe`, and `sceKernelTryReceiveMsgPipe` now use a
  bounded host-owned FIFO with PSP UIDs, complete/partial transfers, result sizes, and PSP error
  returns. The game creates `PlayRequest` and its first send succeeds; the next failure is a
  separate PLT/codegen frontier.
- **Audio initialization is honest.** The runtime implements the sceAudioOutput2 family and
  opens an SDL3 44.1-kHz stereo signed-16 stream. Audible/nonzero output is still an open check.
- **False guest `strlen` replacement removed.** Address `0x00014a40` is the game's actual
  `strlen`, not a controller initializer; restoring its translation fixed one-character strings,
  duplicate CSV-key assertions, and malformed resource paths.
- **Message-layout node generation restored.** Removed the `tools/codegen.py` custom dummy for
  `f_00101798`/`f_001018d0`; codegen now emits the original ELF functions. This produces the
  expected nine layout factory calls, removes the false SGXD alias dispatch, advances the
  startup frames, and reaches the interactive main menu. `BuildFull` and the C++ `selftest`
  both pass; a fatal-dispatch headless run remained healthy past vcount 4200.
- **`sceUmdWaitDriveStatCB` (0x4a9e5e29) registered** (`src/rt/hle.c` → `h_UmdWaitDriveStat`).
  It was the only unimplemented NID reached during boot; under the fiber scheduler an
  unimplemented NID does `_Exit(7)`, which was the "closes after several seconds" symptom.
  A full import-vs-handler audit (`build/hst/hst_imports.toml` vs `sr_hle_register`) shows 64
  imported NIDs unregistered, but the other 63 are networking/ad-hoc (sceNet*/sceNetAdhoc*) or
  secondary audio and are not called on the single-player boot path.
- Boot binary consistency: the chunk `.o` were stale relative to the regenerated chunk `.c`
  (older codegen linked into `hst.exe`); a `make compile` recompiled all 8 chunks so the binary
  matches current `tools/codegen.py`. This is the most likely source of the earlier on-screen
  garbage text (a stale-chunk artifact), which is absent in the reference-rasterizer capture.
- Resource-table construction now routes the unavailable guest-only host branch through `sceIoOpen`; address-taken leaf `0x00042998` is included in codegen discovery.
- Extracted-XB indexing grows dynamically beyond the former 32,768-file cap and normalizes the `PSP_GAME/USRDIR` prefix case-insensitively.
- Continuation-call boundaries, computed-jump landing entries, and caller stack restoration removed the earlier vtable/memset/`NULL_CALL` corruption chain.
- All LOOP_CAPS were removed after their underlying control-flow problems were resolved. Do not reintroduce them.
- HLE dispatch uses `sr_syscall` → `s_hle[]` plus the late-import bridge. No linker `--wrap=` contract exists.
- sceFont now uses parsed PGF metrics and raster data; the former synthetic fallback is retired.
- The Makefile's two-phase `all` target, shader regeneration, C11 atomic type, ISO9660 extensions, and covered VFPU instruction set were repaired and previously verified.

When a current claim changes, update this file and include the exact source/log evidence. Put long investigative narratives in the ignored archive, not back into the live tracker.

</details>

<!-- markdownlint-enable MD028 -->
