# Next session handoff

Reconciled 2026-08-04 at `main` = `de269b01c8f38a395cdac075d58053ab4dfbcc34`
(#268, on top of the stacked #266/#267 landing). This is a starting point, not a
status report — [`ISSUES.md`](../ISSUES.md) is the live dashboard and GitHub
Issues are canonical.

## Working-tree reconciliation, 2026-08-04

The local clone was behind `origin/main` at `f374b31` while carrying an
uncommitted pre-PSP worktree. That worktree was verified to be a **stale earlier
draft of work that had already landed**: against `origin/main` it contained zero
non-whitespace source deltas, its only local-only content was three trailing
blank lines (which the pre-commit EOF fixer rejects), and its
`test_hst_doctor*.py` copies predated the #266 refactor into
`tools/hst_test_fixtures.py`. It was stashed (`stash@{0}`, also tagged
`backup/worktree-20260804`) and the clone fast-forwarded to `de269b0`. Nothing
was lost; the stash can be dropped once this note is read.

## Pre-PSP campaign handoff

The landed tree contains the source-only pre-PSP patch. It
updates the private-backend manifest digest after the finite-span source change,
handles a bounded named-section import tail in `tools/imports.py`, routes
conditional foreign codegen targets through dispatch, and applies the narrow
issue #184 VFPU guards for illegal `vcrs` widths and inactive `vrot` lanes.
A clean clone without the ignored private inputs must report `BuildFull` as
unavailable. No private bytes are tracked.

The `assets/release_manifest.json` PGD digest is re-measured after the
provenance-header correction: the tracked value `95d61eea…a4270a` matches the
private backend and `tools/test_release_manifest.py` passes.

Measured locally at `de269b0` with private inputs present:

| Gate | Result at `de269b0` | With this session's oracle-gate patch |
| --- | --- | --- |
| `python -m unittest discover -s tools -p "test_*.py"` | 682 run, 0 fail, **27 skips** | 707 run, 0 fail, 27 skips |
| `tools/discovery_contract.py --run --assert-contract` | 687 loader / 682 started | 712 loader / 707 started |
| A-only / B-only IDs | 5 explained / 0 | 5 explained / 0 |
| `tools/psp_readiness.py --run-focused` | `software_ready=True` | same; `hardware_ready=False` |
| `tools/test_release_manifest.py` | 9 passed | 9 passed |
| `ruff` on changed tools, `markdownlint-cli2` on changed docs | — | clean |

The 27-vs-33 skip difference is the private-input condition, not a regression —
see [`TEST_SKIPS.md`](TEST_SKIPS.md).

The aggregate `hst_manager.ps1 -Action Verify` route was **not** run in this
pass; the gates above were invoked directly instead. Nothing above should be
read as a `Verify` result, and the native selftests, `import_audit_gate.py`,
`publish_audit.py`, and `gpu-coherence-selftest` legs were not re-run here.

## PSP hardware-oracle readiness

The open-issue snapshot and hardware queue are in
[`PSP_ISSUE_MATRIX.json`](PSP_ISSUE_MATRIX.json) and
[`PSP_HARDWARE_ORACLE.md`](PSP_HARDWARE_ORACLE.md). The PSPDEV lock is valid.

PSPDEV is installed under WSL Ubuntu (`psp-gcc 15.2.0`) and the PSPLINK PC
clients run natively on Windows; `psp_readiness.py` resolves both. The probe
builds warning-free and its records were verified end-to-end through PPSSPP
headless. Run `python tools/psp_readiness.py --json --run-focused` before
connecting a physical PSP. Keep all PRX/PBP output and hardware captures under
ignored local paths.

The PSP-3001 on 6.61 ARK CFW with PSPLINK was verified in the latest local
session. The Nakagawa production-HLE stream is implemented in the existing
`hle_thread_selftest.exe` path: its three kernel cases enter the registered NIDs
through `sr_syscall`, derive scalar records from runtime returns, and hash the
artifact that emits them. The corrected coroutine lifecycle path (`16fbb0a`
adoption fix plus `1d8d494` bounded instrumentation) is covered by the safe,
runnable `mingw32-make hle-thread-selftest` gate, measured at 341 checks and 0
failures. Use `mingw32-make psp-oracle-nakagawa
PSP_ORACLE_CASE=...` for one bounded host stream at a time. The exact-head
2026-08-05 PSP-3001/6.61-ARK session at source commit
`6a8bc30796686e5194e369c35e80870f988b36a8` captured callback, wait-cancel, and
thread-lifecycle; all three compare `MATCH` with
`acceptance_eligible=true`, including raw `out2`/`out3` return codes. The
manifest records each PRX digest and the current running-executable
selftest SHA-256 (`7d7efe835e5fbe36185fea48fc031d5b3ce95a57d7805e440afebf766171d084`).
An additional delete-lifecycle diagnostic at the same source/model/firmware is
acceptance-eligible but `DIFFERENCE` (`out0=0x00001fff` Nakagawa versus
`0x00001fdf` PSP); that capture predates the thread-delete lifecycle fix, and
[#26](https://github.com/Jstar269/nakagawa-recomp/issues/26) was closed as completed
on 2026-08-05 after the deletion lifecycle implementation landed. The capture
remains dated historical evidence at its recorded source commit and remains a
separate record.
The synchronized `thread-delete-followup` capture now matches the PSP on
`out1..out14` and establishes the implicit negative-entry-return normalization
(`0x800201ac` inner -> `0x800200d2` outer/exit status, while positive `0x77`
propagates). The synchronized `thread-delete-explicit` sibling also matches:
explicit `sceKernelExitThread(0x800201ac)` produces
`0x800200d2`, while explicit positive `0x78` propagates; both status queries
report STOPPED with no wait object. The implementation consequently normalizes
signed-negative status at the shared non-delete ThreadMan exit boundary:
`0x800201a8`, `0x800201ac`, and ordinary `-17` all map to `0x800200d2`, while
positive values propagate. `ExitDeleteThread` and module self-unload remain
unmeasured and raw.
The boundary probe PRX is
`826a97e79c0770c6aba30077dd9b4a2927c88cf45cac622a22088e5de78a31d5` at source
commit `88cc5ea709f65bf4e219fa56e51c704181070167`; its canonicalized companion
comparison is `MATCH` with no blockers.
A fresh 2026-08-05 smoke capture at source commit
`570600e9dcbe7ecc8693974647accea5a9b3bfb5` also compares `MATCH` with
`acceptance_eligible=true`; the host stream now hashes its running executable,
so a caller cannot substitute a PRX or unrelated file as the Nakagawa artifact.
This establishes the probe-group invariant but does not close the broader routed
kernel issues whose individual acceptance criteria remain open.

At source commit `7a6d70d`, `BuildFull` completed successfully after a true
clean regeneration (Make returned 0; `compile_commands.json`, SDL3.dll, and
fonts were verified beside the binary). The authorized
`hst_manager.ps1 -Action Verify` route also completed all 10/10 legs: 712
Python tests, 27 skips, and 0 failures, plus scheduler/profiler/heap/asset-index
selftests, production HLE ThreadMan, the reference interpreter, import and
publication audits, and GPU coherence. The subsequent HST game route was
controller-stopped after 364.67 seconds; its logs reported no generic failure,
crash, or fatal diagnostic, but did show the known scheduler-spin telemetry.
This remains bounded runtime smoke evidence, not full gameplay acceptance or
proof that the changed thread-lifetime path is exercised by the game.

The comparator now enforces the acceptance boundary that was previously prose
only: `compare_outputs()` reports `acceptance_eligible` / `acceptance_blockers`,
placeholder fixture provenance (`model=unknown`, all-zero digests) can no longer
present as a hardware pass, swapped `--psp-output`/`--nakagawa-output` streams
are rejected, and `run_psplink.py` treats the four provenance flags as
all-or-nothing. `PSP-KERNEL-001` and `PSP-SMOKE-001` are implemented from
measured sessions. The smoke case uses the generated guest body from the
source-owned PSP ELF and the fresh exact-commit report is recorded in the
manifest; the other four manifest groups are also planned. The old ignored
smoke captures from the removed host-only emitter do not qualify as acceptance
evidence.

Hosted Actions have not executed for the merged #266/#267/#268 heads. Recent
`main` runs are for other heads and include failures; local results must not be
described as CI-green.

## Repository state

- `main` contains the merged #236–#262 integration fixes, including long-path asset indexing, zero-relative
  exports, ATRAC ABI alignment, utility AV identity, display framebuffer latching, build truth, deterministic
  private-fixture GE replay, eight in-flight Vulkan slots, ordered render/snapshot batching, fence-owned
  upload staging, and the typed guest file-descriptor namespace from #250. Hosted Actions are active; the
  latest full successful run is #30733971304 on its recorded candidate head, while the exact #250 manual
  dispatch (run #30879667467) remained queued with no jobs and is not CI evidence.
- #243 is resolved on exact `main`: guest standard descriptors 0/1/2 are reserved by typed state, ordinary
  files allocate from fd 3, and the native HLE fixture asserts the exact Phase 5 payload
  `NAKAGAWA_MINIMAL SUM=5050\n` plus synchronous/asynchronous close-and-reuse behavior. A private HST
  `BuildFull` now completes after the bounded import-section-tail compatibility fix in this worktree; the
  duplicate heap-selftest definition failure was removed by merged PR #256. The allocator
  acceptance work in #17 was completed by merged PRs #257/#258 and exact-main verification. The finite #15
  matrix is closed by PR #260, MPEG/H.264 bounds by #261, and GIM/XB parser bounds by #262; no private game
  inputs or captures were published.
- Native and generated code still default to O0. Native O2 is selectable but unpromoted because the
  full-runtime pair selected different scenes. Do not promote it from GE replay evidence.
- Publication is not legally cleared. #98, #99, #102, #104 and the
  [key-history scrub](KEY_HISTORY_SCRUB.md) remain open blockers.
- The repository finished a reconciliation pass on 2026-08-01; the current dashboard and merged
  integration state were refreshed again on 2026-08-04. The next session should start new engineering,
  not revive completed integration investigations.

## Start here — #33 performance and the remaining bounded backlog

The long-path implementation from #237 is merged and its configured-root short/long visual parity is
accepted. Issue #223 was closed as completed on 2026-08-04; direct coverage of a process working directory
longer than the Windows host permits is recorded as an environment limitation, not an unresolved asset
index implementation defect. Valid route evidence must still report **`host_data: indexed 56672 files`**.

The implementation details and acceptance evidence remain recorded in [#223](https://github.com/Jstar269/nakagawa-recomp/issues/223). Do not revive the pre-#237 path/index diagnosis as current implementation state.

The merged implementation uses checked wide APIs, dynamic host-path storage, an atomic fail-closed
index, and an executable-anchored default root. Exact configured-root short/long visual parity passed;
direct >260-character process-CWD coverage is unavailable on this Windows host. The executable-anchored
default and walked descendants reject reparse points, while explicitly configured roots remain
operator-trusted for lawful junction fixtures. Any route with an index other than **`56672`** must be
discarded.

The #142 display-latch correction from #241 is merged. Exact-main dense replay did not reproduce the
original Zeta absence, and the transition trace localized the prior symptom to front/back display handoff;
no renderer/culling workaround is justified. #143's formatter fix is also merged, with exact-main visual
evidence for both reported UI surfaces. Both issues were closed as completed on 2026-08-04.

Start new engineering with the #33 performance loop or another explicitly scoped open owner such as #31/#32
media, #148 VFS containment, or the publication/legal blockers. The finite #15/#170/#171 hardening matrices
are complete; keep their residual criteria and evidence labels synchronized in [`ISSUES.md`](../ISSUES.md)
before launching private routes, and do not reopen completed campaigns merely to increase the closure count.

## Then: the #33 performance loop

The private `logs/UCUS98701_0002.ngef` fixture is the performance loop. The fence-owned staging ring
reduced same-binary policy-8 Vulkan replay from 96.633 to 81.862 ms/frame (15.286%, 1.180x throughput)
and 294.10 to 106.15 physical submits/frame (63.907%). Upload waits are no longer the leading measured
boundary: default-ring replay has 106.0 render/snapshot-chain submits, 0.05 texture-upload submits and
0.10 readback submits per frame.

End-to-end timing closes the old accounting gap: on the exact old strip path, deterministic
reset/apply/restore costs 0.814 ms/frame (5.004%, harness-only), all GE lists 14.669 ms/frame (90.205%,
production-relevant), final materialization 0.779 ms/frame (4.790%), and outer residual is effectively
zero. Do not optimize the fixture reset as a runtime win.

A coarse nested `ge.c` profile showed the list-dominant frontend repeatedly decoded/transformed all three
vertices for every triangle-strip triangle. Same-draw reuse of the previous two vertices reduced actual
decode/transform uses 39,012 -> 24,874/frame (36.240%). Same-binary unprofiled replay improved 16.645 ->
15.071 ms/frame (**9.456%**, 1.104x throughput); submissions stayed 90.95/frame and every output remained
canonical. `SR_GE_STRIP_CACHE_DISABLE=1` selects the exact old loop.

Triangle-strip reuse is landed on `main` at `78508d219c2c59bcad3e8e4bd6aadb7625207702`. A strict
post-strip hierarchy now reconciles the 12.967 ms/frame profiled `ge_run_list()` total: command dispatch
0.305, primitive frontend excluding the GPU hook 3.507, GPU hook 9.070, CLUT/flush 0.084 and list residual
0.000 ms/frame. The hook reconciles to existing renderer phases 4.948, submit 1.343, wait 0.098 and backend
residual 2.681 ms/frame. The previous approximately 3.3 ms unknown was therefore inside the GPU hook, not
the GE command loop or measurement overlap. GE-only profiling adds 4.377%; the combined diagnostic profile
adds 12.823%, so use unprofiled binaries for A/B.

Bounded sampled vertex profiling was rejected because observer effect remained ~25–28%;
sub-phase ranking is not decision-grade. Coarse GE hierarchy remains acceptable. Do not select
lighting or vertex_decode optimizations from the rejected ranking. Preserve every destination
snapshot, existing image barrier, PSP fb16+dither rule and shader-blend semantic. Keep
`SR_GPU_SYNC_SUBMIT=1`, `SR_GPU_XFER_RING_KB=0`, `SR_GPU_TEX_SHADOW_DISABLE=1`, and
`SR_GE_STRIP_CACHE_DISABLE=1` as exact fallbacks.

Count-only backend probes found 12,717 target checks (12,716 fast hits), 12,355 room checks (zero flushes)
and 11,996 full batch comparisons (11,995 merges) per frame. A state-serial experiment reduced the full
comparisons to one/frame but improved unprofiled same-binary replay only 14.782 to 14.593 ms/frame (1.284%,
inside variance), so it was removed. Do not repeat it without a materially different mechanism or workload.

## Staging-ring evidence

- Boundary baseline, per frame: texture upload 126 submits/10.857 measured submit+wait ms, target upload
  1/0.101 ms, depth upload 1/0.081 ms, readback 0.1/0.016 ms, present 0, lifetime drain 0 submits/0.077
  wait ms, other slot recycle 0 submits/0.204 wait ms, plus 166 render/snapshot-chain submits/3.419 ms.
- Uploads were 128.0 of 128.1 non-render/snapshot submissions (99.922%), satisfying the experiment gate.
- Same-binary policy 1/2/4/8 ring results: 95.429/88.433/84.327/81.862 ms/frame and
  845.10/423.15/212.15/106.15 physical submits/frame.
- Every policy, ring-disabled baseline, tiny-ring stress and synchronous fallback produced canonical
  PPM SHA-256 `81C0294C7BF7DDCEF197BEFCE9086CFECD9791092ABC55D72E8FE36E58337A0B`.
- A 64 KiB ring forced 80 wraps and 65 oversized-upload fallbacks over five frames. Vulkan core and
  synchronization validation reported zero warning, error, VUID or hazard in policy-8, tiny-ring and
  synchronous modes.

## Fresh active-rally setup

The private baseline `logs/issue33_native_savebase_20260726` was explicitly captured from Nakagawa's
live plaintext save; it was not copied from PPSSPP and does not reuse `logs/oracle_savebase`.
`DATA0.BIN` SHA-256 is `5A7C79705682785FD244E2237D9C9C74E6343123E7268276600F5C8482092C45`.
The PPSSPP save now has the same bytes by user action, but Nakagawa remains the baseline source.

One bounded Benchmark restored that baseline, replayed `logs/route_E_deep_return_20260725.pad`, and
exited cleanly at 33,000/33,000 vblanks: 590.1 s, 55.9 guest vblanks/s and 49 captures in the
30,000-33,000 window. Captures at 32,217-32,844 show normal-sized players and active serve/return play
with stable court, HUD and ball rendering. The 32,200-32,850 window averaged 10.103 FPS, 47.691
vblanks/s, 871.538 GE submits/s and 99.695 ms/s GE wait. This proves the setup only; no second long run
or gameplay improvement percentage was claimed.

The later strip/shadow telemetry run used the same baseline and route, exited at 33,000 vblanks in
593.7 s (55.6 guest vblanks/s), and again produced 49 active-window captures. Its last periodic sample
at vblank 32,948 reported shadow invalidations/checks/hits/misses = 21/5/0/5, 655,360 compared bytes,
0 avoided and 18 required decodes/uploads. From the vblank-29,835 sample through the capture window the
deltas were 2 invalidations, 0 checks/hits and 2 required uploads. Replay's repeated-memory shadow win
therefore did not apply to this sampled rally. Nine inspected frames had normal-sized players, court,
HUD, ball and serve/bounce markers. No gameplay performance percentage is claimed.

## Earlier completed #33 evidence

- PPSSPP software replay: warm mean 74.58 ms, deterministic BMP SHA-256
  `A74A503FA123BC4C678AF7FFC65EA76202AA2FCB9FA2FF295298E4145652EA9A`.
- Nakagawa fixture SHA-256 `C3D851CF02AE429CC1BC20025D46603F4A09654CF7E2FA75A0026A39C1B0414A`;
  software mean 266.742 ms/frame; Vulkan canonical output hash above.
- Eight in-flight slots: synchronous 224.201 -> asynchronous 140.150 ms/frame (37.489%).
- Render/snapshot batching policy 1/2/4/8: 101.315/95.774/93.392/92.956 ms/frame on its measurement
  binary. `cmd_drain()` has a truthful mixed wait bucket and batched draws use distinct vertex storage.

## Preserved private inputs

Keep `place_game_here/`, `game.iso`, `eboot.elf`, `keys/`, `memstick/`, the PPSSPP checkout/memstick,
all `.ngef`/`.ppdmp` fixtures, pad routes, save baselines and oracle evidence private and untracked.
`git clean -fdx` would delete them; never run it here.

## Evidence discipline

- Trust `oracle_manifest.json`, not capture count alone.
- A pad file fixes input timing, not selected opponent, tutorial state or animation workload.
- Renderer agreement is localization evidence, not an external PSP oracle.
- Keep game-derived dumps, screenshots, traces and saves out of Git.
- Update #33 and [`PERFORMANCE.md`](PERFORMANCE.md) whenever a measurement changes the next experiment.
