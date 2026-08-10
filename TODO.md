# TODO — Nakagawa Recomp performance: single-digit FPS → 30 FPS

**Goal:** Take the recompiled PSP game from single-digit FPS (3D lobby) to the PSP's native ~30 FPS ceiling, and eliminate (a) very slow loading and (b) occasional polygon glitches.

> [!NOTE]
> This is the performance hypothesis backlog, not the live task list. Correct and
> visually accept the `drive_court` route in [`ISSUES.md`](ISSUES.md) before
> profiling or applying an optimization.

**Status:** Re-audited against source and a live Standard-mode run on 2026-07-14. The earlier handoff
overstated certainty and missed an unsafe generated-code `SR_YIELD` override, regular CPU writes that
bypass texture invalidation, correctness-critical dispatch hooks, and two ungated steady-path loggers.
The verified corrections are recorded below and in
[`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md). Logging fixes and removal of
the generator override are implemented; performance changes still require
measurement and live regression testing.

## Disposition audit (2026-07-14 performance pass)

Every named item below is now classified; an item is not silently treated as complete merely because
its old proposed fix was unsafe.

| Item | Disposition | Evidence / remaining decision |
| --- | --- | --- |
| Phase 0 timing, submit/wait, vblank counters | **Implemented** | `SR_PERF=1` emits 1 Hz aggregates and optional CSV for presented FPS, vblank rate, CPU/host, GE waits, present, idle/scheduler, submits, waits, readback waits, and capped-present skips. `hst_manager.ps1 -Action Run -Profile Benchmark` writes `logs/perf.csv`. |
| Reproducible lobby capture + hot guest ranking | **Open** | The no-input title/main-menu path is measured. A lobby run and `SR_PROFILE` ranking still require navigation; do not substitute title numbers for the lobby. |
| 30 FPS output ceiling | **Implemented; lobby verification open** | Host presentation is capped at 30 without sleeping guest execution (`SR_FPS_CAP=30`, `0` disables). Before the cap the title path issued 42-102 real presents/s despite ~60 vblanks/s. |
| B1/B4/B15/B26 GPU submission redesign | **Measured, open** | Capped O0 title path averaged 349 submits/s and 176 ms/s in synchronous GE waits: material, but CPU/host averaged 538 ms/s, so GPU waits are not the only bottleneck. Deferred waits still require a correctness-preserving command/fence ring. |
| B2 generated-code optimization | **Open, highest CPU candidate** | Eight generated chunks remain `-O0`; current files are 0.5-43.8 MiB and still contain release trace-wrapper text. Shrink/regenerate before trying optimized GCC builds. |
| B3 dispatch diagnostics | **Open, measure first** | Unconditional 32-entry crash ring remains; functional hook coverage must stay enabled. |
| B18 hash removal | **Old fix refuted** | Hashes remain correctness-critical until all guest/host VRAM writes participate in dirty tracking. |
| B6/B19/B21/B22 logging | **Completed** | Verified steady-path messages are gated. New `Performance` mode redirects both C streams to the null device; `Benchmark` retains only bounded startup/1 Hz telemetry. |
| B10 hand-written runtime `-O2` | **Measured; not promoted** | Identical 140 s title runs improved the broad active-window mean from 21.1 to 24.4 FPS, but scene timing differed and no lobby parity run was performed. It is a modest candidate, not the 30 FPS solution; default stays `-O0`. |
| B12 accessor watchpoint overhead | **Mostly complete / old premise refuted** | Watch scan is already debug-gated; only the low-value GE-MMIO compare remains. |
| B16 pipeline cache | **Open, startup/stutter only** | No `VkPipelineCache`; not a steady-state 30 FPS fix. |
| B17 atomic dispatch loads | **Old weakening proposal refuted** | Atomics preserve concurrent late-import publication. Redesign publication before changing memory semantics. |
| B20 clock-read relocation | **Old relocation proposal refuted** | Early wall-clock preemption is functional. Replace only with a semantics-equivalent deadline/due design after profiling. |
| B5 scheduler forced rotation | **Implemented 2026-07-18** | The anti-starvation rotation was removed; `pick_next` now rotates only among best-priority READY threads. See the B5 section below. |
| B9 VFPU SIMD | **Conditional/open** | Do only if a hot-guest profile proves VFPU-bound. |
| B13 yield frequency | **Open, coupled to B20** | Reducing yield sites without preserving vblank/preemption behavior is unsafe. |
| B7 async I/O | **Open** | Async calls still execute synchronously; affects streaming/loading, not yet attributed to steady lobby FPS. |
| B11 ISO data cache | **Partly complete** | Metadata caches exist; file contents/read-ahead do not, and the global lock covers seek/read. |
| B14 heap reuse | **Fixed** | Root cause was a missing pointer-provenance check in `sr_newlib_free`, not `INIT_ARRAY` ordering; free-list reuse is now on by default. |
| B23 range-hook calls | **Old gating proposal refuted** | Both hooks are functional; profile/restructure predicates without disabling them. |
| B24 vblank setup calls | **Open, low value** | Lazy setup functions return immediately after initialization; optimize only with B20 evidence. |
| B25 texture decode | **Conditional/open** | Cache decoded pixels only if profiling attributes meaningful time here. |
| B27 VFPU prefix stores | **Conditional/open** | Couple to B9/profile evidence and verify exact prefix semantics. |
| B28 fallback diagnostic reads | **Refuted as performance work** | Reads occur only on the rare fatal `SR_VFPU_OTHER` path; no hot-path action. |

## What changed vs the prior version

1. **B2 RE-FRAMED (Clang-OOM premise busted).** The prior claim "Clang ≈65% of GCC's peak RSS, ~4×
   faster frontend, compiled a 24 MB TU that OOM'd GCC" is **FALSE / UNVERIFIABLE**. Real 2024 data
   shows Clang often uses *more* memory than GCC on huge TUs (LLVM issue #83122: Clang 14 GB vs GCC
   1.2 GB; LLVM discourse #35204: Clang 12.5 GB vs GCC 0.5 GB). The OOM-safe lever is **shrinking the
   generated TUs** (`FUNCS_PER_CHUNK` 2000→250–400) + **stripping the trace-wrapper text** under the
   existing GCC toolchain — NOT switching to Clang. Clang moves to an *optional experiment* (with
   `-fno-strict-aliasing`), not the primary recommendation.
2. **B12 CORRECTED.** `sr_check_mem_watch` (`debug.h:150`) is **already gated** by
   `if (!SR_DBG(SR_DBG_MEM)) return 0;` — the store watchpoint scan costs ~nothing in release. The
   prior claim "active in release (not gated by `SR_INSTRUCTION_TRACE`)" is misleading. B12's only
   real remaining item is routing the unconditional `0x04084000` GE-MMIO branch out of the hot
   `sr_r32` (cheap, low value). B12 is downgraded to low-priority / mostly-done.
3. **B18 corrected (hash is currently load-bearing).** `tex_get` computes `tex_hash()` and, for CLUT
   formats, `clut_hash()` before the cache-hit check. `hook_vram_dirty` invalidates some DMA, HLE,
   MPEG, and GE-transfer writes, but ordinary generated `MEM_W8/16/32` stores do not call it. Removing
   the hash would therefore reuse stale textures after normal guest CPU writes. Optimize this only
   after implementing complete write-range invalidation and proving it with texture mutation tests.
4. **B1 caller list corrected.** There are **11 active `cmd_submit_wait()` call sites**:
   `620, 678, 712, 790, 903, 920, 1085, 1201, 1417, 1777, 1997`. (Prior list missed `:712`.) All
   share the single `s_fence` (`ge_gpu.c:188`). Present path uses the *separate* async ring
   `s_frame[PRESENT_FRAMES=3]` (`sdl3vk.c:53`), not `cmd_submit_wait`.
5. **GE/display telemetry correction.** The named HLE GE/display messages are gated, but
   `ge_gpu.c::stats_tick()` still printed `GEGPU stats` every five CPU seconds in Standard mode. A
   main-menu interval showed 3,780 submissions/snapshots for 60 presents (about 63 per presented
   frame). `stats_tick()` is now gated by `SR_GPU_LOG`; the ratio is evidence of submission pressure,
   not proof that fence waits dominate lobby wall time.
6. **B21 corrected.** Of the five messages the handoff called newly ungated, `IoOpen` was already
   behind `SR_IOLOG`; `Getstat` behind `SR_STATLOG`; and `CreateSema`/`WaitSema` behind `hle_log_on()`.
   Only `WaitEventFlag` was ungated. It and the verified B6/B19 messages are now gated.
7. **B23 corrected (do not gate correctness hooks).** The two range hooks are called each dispatch,
   but `hook_resource_handle` and `hook_corrupt_callback_queue` are active compatibility/guard logic,
   not diagnostics. Several exact hooks are also functional patches. Keep them enabled unless each
   hook is independently retired or replaced; profile the diagnostic ring separately.
8. **B5 nuance (superseded 2026-07-18).** The priority-blind rotation and its `SR_ROTLOG` gate
   were removed when B5 was fixed — `pick_next` now rotates only among best-priority READY
   threads. `SR_GEDUMP` is **not** in `sched.c`; it lives in `ge.c:2071,2580` and `hle.c:73,79`.
9. **Research appendix corrected** (see Research section): Clang-OOM myth removed; timeline semaphores,
   PPSSPP #16900, NVIDIA/zeux submit budgets, vkguide pool recycling, ThinLTO (Chromium-evidenced,
   arXiv 2507.16649v1 flagged UNVERIFIABLE), and VFPU 8×4×4 layout all re-confirmed with live URLs.
10. **Generated scheduler override removed.** `tools/codegen.py` injected a Ponytail-era replacement
    `SR_YIELD` macro into the main generated file and every chunk. It bypassed the canonical
    scheduler-off guard, profiler, atomic timeslice update, vblank-quantum check, and the reviewed
    `0x00065c60` compatibility hook. Codegen now uses the canonical `recomp.h` macro exclusively.

---

## Phase 0 — Establish a baseline & measure (do this before touching anything)

- [x] **Per-frame timer split.** Added an opt-in 1 Hz aggregate logger (`SR_PERF=1`, or the manager's
      `Benchmark` profile) and `logs/perf.csv`, splitting CPU/host, synchronous GE waits, present,
      and idle/scheduler time. It reports wall-clock intervals rather than pretending every display
      syscall is a unique guest frame.
- [x] **Count submits + waits per interval.** Counters cover the shared GE submit/wait path, async
      readback-ring submits and blocking retirements, presentation submits/fence waits, and cap skips.
- [ ] **Capture one live lobby session** with `SR_THLOG=1 SR_GELOG=1` (`SR_ROTLOG` was retired with
      the B5 rotation on 2026-07-18; `SR_GEDUMP` is a different gate, in `ge.c`/`hle.c`, not
      `sched.c`) to confirm CPU-bound (recomp) vs GPU-sync-bound.
- [ ] **Rank hot guest functions** with the existing profiler: `g_prof_enabled` (`recomp.h:259`) /
      `sr_profile_block` (`:270`), emitted via `SR_PROFILE` through `codegen.py`. Decide whether **B9
      (VFPU)** matters for *this* game before investing in SIMD.
- [ ] **Record baseline lobby FPS** (e.g. ~6 FPS) so each later phase has a number to beat.
- [x] **Add a vblank-pacing counter**: `deliver_vblank()` increments the 1 Hz vblank-rate metric. The
      light startup path measured approximately 59.94 Hz; the earlier double-delivery hypothesis is
      empirically refuted. Excess immediate host presents—not excess vblanks—were observed instead.

**Acceptance:** a reproducible FPS number + a CPU-vs-GPU-vs-idle breakdown + a submit/wait-per-frame
count for the target scene. Instrumentation itself has no behavior change; the separately documented
30 FPS output cap intentionally drops redundant host presents without delaying guest execution.

---

## Tier 1 — High-priority hypotheses (measure before implementation)

### B1 — Per-batch synchronous GPU fence wait

- **Symptom:** 3D lobby runs single-digit FPS despite an RTX 3080.
- **Location:** `ge_gpu.c:337-345` `cmd_submit_wait()` = `vkEndCommandBuffer` + `vkQueueSubmit(s_fence)`
  - `vkWaitForFences(s_fence, …, UINT64_MAX)` + `vkResetFences`. Called by **11 sites**:
  `submit_pending:620`, `target_readback:678`, `:712`, `target_upload:790`, `depth_from_cpu:903`,
  `depth_to_cpu:920`, `tex_upload:1085`, `snapshot_refresh:1201`, `build_state:1417`, `hook_xfer:1777`,
  `gegpu_init:1997`. **All share the single `s_fence` (`ge_gpu.c:188`).**
- **Root cause:** Each call fully serializes the recompiled-MIPS worker thread behind the GPU. Zero
  CPU↔GPU overlap. When the game does many small texture uploads / target switches / depth syncs per
  frame, the CPU waits on the GPU dozens–hundreds of times per frame.
- **Fix (code-level):**
  - Split into `cmd_submit_nb()` (end + submit, return fence) and a *deferred* wait:

    ```c
    static VkFence cmd_submit_nb(void) {            /* returns s_fence; caller keeps recording */
        VKC(vkEndCommandBuffer(s_cmd));
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd;
        VKC(vkQueueSubmit(s_queue, 1, &si, s_fence));
        return s_fence;
    }
    ```

  - **Keep the synchronous wait only where a result is actually consumed:** present ring, VRAM readback
    for a CPU consumer, target overwrite. Everywhere else, record batch N+1 while batch N renders (B4's
    ring holds the in-flight fences).
  - In steady state, **poll** `vkGetFenceStatus(s_dev, s_fence)` instead of `UINT64_MAX` waits, exactly
    like the existing readback ring already does (`ge_gpu.c:405` `readback_finish`, `:427`
    `readback_poll`).
- **Research backing:** Khronos `wait_idle` sample: per-frame fences gave a **22% frame-time reduction**
  vs `vkDeviceWaitIdle`. PPSSPP #16900 (hrydgard): *"it's essential to have a frame or two 'in
  progress', pipelined between CPU and GPU… stopping to read back tells the system to sleep."* NVIDIA:
  aim for **5–10 submits/frame**, never `vkQueueWaitIdle`-style drains.
- **Risk:** Correctness around VRAM readbacks (xfer/CLUT). Mitigation: present ring (`sdl3vk.c:53`) and
  readback ring (`ge_gpu.c:189`) remain the correctness anchors; never defer a wait past the point the
  CPU consumes the data.
- **Acceptance:** Correctness-required waits remain, avoidable waits are reduced, GPU utilization rises,
  and a before/after lobby trace quantifies the actual FPS effect.

### B2 — Generated chunks compiled (near-)unoptimized (no register allocation)

- **Symptom:** Every guest register lives in RAM; every instruction is load/store/load; the optimizer
  is forbidden from keeping registers in host registers.
- **Location:** `Makefile:112` `RECOMP_FLAGS := -O0 -w -fno-var-tracking -ftrack-macro-expansion=0`
  (applied to the 8 chunks via `Makefile:128-132`).
- **Root cause:** The `-O0` guard exists because the 45 MB generated files once OOM'd the compiler.
  **CORRECTION (this pass):** the prior plan's claim that *Clang* is the OOM-safe lever is **false** —
  Clang frequently uses *more* memory than GCC on huge TUs (LLVM #83122 2024: Clang 14 GB vs GCC 1.2
  GB; discourse #35204: Clang 12.5 GB vs GCC 0.5 GB). The real, evidence-backed lever is to **make the
  TUs small enough that GCC itself can compile them at `-O1`/`-O2` without OOM**. The `CpuState
  `s->r[i]` design is correct and matches N64Recomp — do **not** hand-promote to locals (B8 retired).
- **Fix (code-level, two complementary fronts):**
  1. **Shrink the source so optimization is feasible (no semantic change):**
     - Drop the trace-wrapper *text* from the release emission. `sr_begin`/`sr_end` already expand to
       `((void)0)` in release (`recomp.h:181-188`), but the literal text is still **emitted for ~100%
       of guest instructions** by `normal_line()` (`codegen.py:858` def, composed at `:869` as
       `f"    sr_begin(s, 0x{addr:08x}u,...); {eff} sr_end(...)"`, first call site `:1311`). This is the
       dominant 45 MB bloat driver (a parser/AST-memory cost, not a runtime cost). Gate the wrapper
       *text* behind `TRACE`/`SR_INSTRUCTION_TRACE` so the release build emits just `{eff}`.
     - Lower `FUNCS_PER_CHUNK` from 2000 to ~250–400 (`codegen.py:2016`; chunks written `:2060-2078`) so
       each TU is small (N64Recomp emits many modest files precisely so they optimize without blowing up).
     - Fold `lui`+`addiu`/`ori` constant-address materialization into one precomputed literal
       (`codegen.py:149`/`143` emit separate `s->r[i]=` writes with no cross-instruction folding). Reuse
       the existing const lattice `_sv_step` (`codegen.py:939`) only as a folding source — it is
       read-only and does NOT fold addresses (confirmed).
  2. **Re-enable optimization on the (now small) chunks under the existing GCC toolchain:** after the
     shrink above, set `RECOMP_FLAGS` to `-O1` (then `-O2`) with `-fno-strict-aliasing` (strict aliasing
     turns ON at `-O2`; `CpuState` unions + `memcpy` reinterpretation would miscompile without it). Keep
     `-w` to suppress warnings on generated code. **Clang remains an optional experiment** (`CC := clang`
     for the chunk rule, also `-fno-strict-aliasing`) but is no longer the recommended primary path.
- **Research backing:** N64Recomp emits one C function per guest function and relies on the *compiler* to
  promote `ctx->rN` — HST should do the same via `-O2`, not manual locals. Vulkan/Clang memory evidence
  is **corrected** (see Research appendix B2).
- **Risk:** Build-time increase; possible opt-driven miscompiles. Mitigation: incremental (chunk by
  chunk), keep an `-O0` fallback flag, **must** keep `-fno-strict-aliasing` at `-O2`; validate against
  `selftest`/`verify`.
- **Acceptance:** chunks build at `-O1`/`-O2` without OOM after TU shrink, differential gates pass, and
  an A/B lobby trace quantifies any CPU-simulation improvement.

### B3 — `dispatch()` diagnostic scaffolding on the hot path

- **Symptom:** Every computed jump pays a ring-buffer write + a 21-entry hook *comparison* loop, not
  just the table lookup.
- **Location:** `recomp.c:1181-1631` (`dispatch()`). Ring write `1309-1317` (+ `sched_current_uid()` at
  `:1314`); spin detector `1241-1269`; `SR_DISPLOG` `1324-1327` (**one-time cached tristate — NOT a
  per-dispatch env walk**); exact-hook loop `1330-1337` (calls `h->fn` only on key/mask match — cheap);
  range-hook loop `1340-1343` (2 entries, calls `h->fn` unconditionally every dispatch — see B23).
  Bounded diagnostic blocks `INIT_ARRAY_WALK:1349` (≤50), `CALL_F_00000FA0:1361` (≤30),
  `RELOC_FIXUP:1379` (≤20), `DISPATCH_SPIN_GUARD:1250` (20000-streak) are negligible on the hot path.
- **Root cause:** The ring-buffer write runs unconditionally on the hottest indirect-transfer path.
- **Safe investigation:** Measure the ring write, exact-hook comparisons, and two predicate hooks
  separately. The ring may be made opt-in if crash-diagnostic tradeoffs are accepted. Do not gate the
  hook loops: they contain live compatibility and corruption-handling behavior. If hook cost matters,
  move proven predicates into table metadata or replace the underlying workarounds.
- **Research backing:** N64Recomp's computed-jump path is `LOOKUP_FUNC(target)(rdram, ctx)` — a hash
  lookup + indirect call, *nothing else*. HST's `dispatch()` carries far more weight per transfer.
- **Risk:** High if hooks are disabled; low-to-medium for an isolated, opt-in ring change.
- **Acceptance:** Measured overhead falls while every functional hook remains covered in regression and
  live progression tests.

### B4 — Single shared command buffer + fence (no overlap ring)

- **Symptom:** No CPU/GPU overlap even where B1 is partially addressed.
- **Location:** `ge_gpu.c:187-189` — one `s_cmd` + one `s_fence` for ALL synchronous GE submits; present
  ring (`sdl3vk.c:53` `s_frame[PRESENT_FRAMES=3]`, each with its own cmd+fence — `present_common:461-543`)
  and readback ring (`ge_gpu.c:189` `s_readback[READBACK_FRAMES]`, non-blocking poll at `:427`,
  `readback_finish:403-425`) are **separate and async** in the normal present path. **Nuance:** ring-full
  fallback (`ge_gpu.c:712`) and explicit `target_readback` (`:678`) still fall back to the blocking shared
  fence — these are exactly the sites B1 should re-route through the deferred-wait ring.
- **Fix (code-level):** Introduce a **ring of N≥3 command buffers/fences** for the synchronous GE path
  (double/triple-buffering). Reset via `vkResetCommandPool` per frame (preferred; current single-CB reset
  is `vkResetCommandBuffer(s_cmd,0)` at `ge_gpu.c:331`). Keep
  `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` (already set on `s_pool`). The worker records batch N+1 while
  batch N renders. **Refinement (research):** for the *readback* ring specifically, prefer a **single
  Vulkan timeline semaphore** over N recycled binary fences (Khronos timeline-semaphore blog; Vulkan
  Samples `timeline_semaphore`) — one monotonic counter, no reset, bridges device↔host; WSI present still
  needs binary semaphores.
- **Research backing:** vkguide.dev: *"while the GPU is busy rendering one buffer, we can write into a
  different one."* Vulkan Samples `command_buffer_usage`: pool reset preferred over allocate/free;
  ~15% multithreaded recording. NVIDIA: 5–10 submits/frame.
- **Risk:** Low once B1's deferred waits are in place.
- **Acceptance:** B1 + B4 together = continuous GPU utilization; lobby FPS in the tens.

### B18 — Per-draw texture hash cost (correctness constraint first)

- **Symptom:** Every textured draw computes a stride-subsampled texture hash, and CLUT formats also
  hash palette data, before the cache-hit check.
- **Location:** `ge_gpu.c::tex_hash`, `clut_hash`, and `tex_get`; the hit requires both
  `e->content_valid` and `e->hash == hash`.
- **Why the prior proposed fix is unsafe:** `hook_vram_dirty` invalidates writes reported explicitly
  by DMA/HLE, MPEG, and GE-transfer paths. Ordinary translated guest stores use `MEM_W8/16/32` in
  `recomp.h`, which currently write memory without notifying `sr_gpu_vram_dirty`. A key-only or
  `content_valid`-only hit can therefore return stale texture data.
- **Safe investigation:** Measure hash time first. If material, add complete page/range dirty tracking
  to every guest-memory write path (including bulk host writes), then skip hashing only when the
  relevant texture and palette pages are provably clean. Differential texture-mutation tests and a
  live glyph/character regression run are required before shipping.
- **Risk:** High without complete invalidation coverage; do not implement the old checklist item.
- **Acceptance:** Hash work falls on proven-clean hits without stale glyphs, palettes, or model textures.

### B6 / B19 / B21 — Ungated HLE logging on hot I/O / syscall paths

- **Symptom:** "loading very slowly"; stalls during asset streaming; spurious stderr spam.
- **Verified ungated messages:** `h_IoWaitAsync`, `h_IoDevctl`, `h_WaitThreadEnd`, the rate-limited
  `h_GetSystemTimeLow`, guest stdout/stderr in `h_IoWrite`, and `h_WaitEventFlag`.
- **Prior false positives:** `h_IoOpen` already used `SR_IOLOG`; `h_Getstat` used `SR_STATLOG`; and
  `h_CreateSema`/`h_WaitSema` already used `hle_log_on()`.
- **Implemented:** The six verified messages now use `hle_log_on()`. Intentional guest/module logging
  sinks remain available, and diagnostic mode preserves the gated messages.
- **Risk:** Low; this changes default observability, not guest state. Recheck loading with and without
  `SR_HLELOG=1` when profiling.
- **Acceptance:** Standard mode stays quiet while diagnostic mode retains the messages.

### B10 — `recomp.c` (dispatch core) compiled `-O0`

- **Symptom:** The hottest hand-written CPU code (`dispatch()`, `SR_YIELD`, per-basic-block machinery)
  is unoptimized.
- **Location:** `recomp.c` is in `RT_SRCS` (`Makefile:60`), built with `CFLAGS = -O0 -fno-strict-aliasing`
  (`Makefile:30`). `ge.c` is already `-O2` (`Makefile:105-106`), proving the runtime *can* be optimized.
- **Fix (code-level):** Compile the hand-written runtime (at minimum `recomp.c`, `sched.c`, `hle.c`,
  `sdl3vk.c`, `ge_gpu.c`) at `-O2`. Add a Makefile rule mirroring `ge.c`'s (`:105-106`) for these, e.g.
  replace the generic `$(CC) $(CFLAGS) …` (`Makefile:135-136`) with
  `$(CC) -O2 -fno-strict-aliasing -fno-math-errno …`. **MUST keep `-fno-strict-aliasing`** (dropping it
  at `-O2` miscompiles the `CpuState` union access). Does **not** touch the chunk `-O0` constraint.
- **Risk:** Medium until the full runtime is regression-tested at the new optimization level; undefined
  behavior can be latent at `-O0`, and the performance gain is not yet measured.
- **Acceptance:** A/B benchmark shows a gain, selftests pass, and a full live progression run matches
  the `-O0` build before the flag becomes default.

### B12 — Memory-accessor overhead (MOSTLY ALREADY DONE)

- **Symptom:** (prior) every `lw`/`sw` pays more than a range check.
- **Location:** `recomp.h:68-75` `sr_r32` compares against `0x04084000` (GE status) on **every** 32-bit
  read (`if ((a & 0xFFFFFFFC) == 0x04084000) return sr_get_ge_status();`). `sr_check_mem_watch`
  (`debug.h:150-162`) is called from the store hooks `recomp.h:76-90` on **every** store.
- **CORRECTION (this pass):** `sr_check_mem_watch` is **already gated** by
  `if (!SR_DBG(SR_DBG_MEM)) return 0;` at `debug.h:151` — so the watchpoint scan costs ~nothing in
  release (the prior claim that it is "active in release" is misleading; it is gated by a *different*
  flag, `SR_DBG_MEM`, which defaults off).
- **Fix (code-level):** Only remaining item is to route the GE MMIO branch out of the hot `sr_r32` into a
  dedicated MMIO read path (minor; removes one compare from every `MEM_R32`). The store-watch change is
  **not needed** (already free). Low priority.
- **Risk:** Low.
- **Acceptance:** Ordinary `lw` is a single range check + load; the one GE-MMIO compare is hoisted out
  of the common path.

### B16 — No `VkPipelineCache`

- **Symptom:** Every pipeline is recompiled from scratch each run (slow cold start, repeated shader
  compile stutter).
- **Location:** `ge_gpu.c:528` `vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &pci, NULL, &p)`.
  Grep `VkPipelineCache` over `gpu_sdl3vk/` → 0 matches.
- **Fix (code-level):** Create one `VkPipelineCache` at `gegpu_init` (persist to disk under `logs/` for
  cross-run warm start), pass it as the 2nd arg to `vkCreateGraphicsPipelines`. (`pipe_create` is at
  `ge_gpu.c:461`; `pipe_get` at `:533`.)
- **Risk:** Low; persistent cache blobs must be validated against the Vulkan cache header/device and
  driver identity, and cache creation must fall back cleanly.
- **Acceptance:** Faster cold-start pipeline creation; minor steady-state (pipelines cached after first
  build).

### B17 — Dispatch table uses `_Atomic` loads on every lookup

- **Symptom:** Every indirect transfer pays acquire-atomic loads (+ an L1 backfill atomic store) on the
  hottest lookup path.
- **Location:** `recomp.c:721-745` `sr_lookup` — two-level hash `g_dispatch_l1[4096]` (`:686`) fronting
  `g_dispatch_table[131072]` (`:672`); `DispatchEntry{_Atomic addr; _Atomic fn}` (`:668-669`); linear
  probing via `atomic_load_explicit(...,memory_order_acquire)` at `:723,729,734,737`; L1 backfill
  `atomic_store_explicit` at `:739`.
- **Correction:** The source documents concurrent lookup/registration, and `dispatch()` can call
  `sr_register()` when resolving a target dynamically. The packed L1 atomic also prevents a stale key
  from pairing with a newly published function. A one-time fence followed by plain accesses would
  violate that contract.
- **Safe investigation:** Measure generated assembly and lookup cost first. If it matters, redesign
  publication (for example immutable tables with atomic pointer swaps) rather than weakening loads in
  place.
- **Risk:** High for the old proposal; do not implement it.
- **Acceptance:** Any replacement preserves concurrent publication semantics under a stress test.

### B20 — Per-yield `SDL_GetTicksNS()` clock read in the inner sim loop

- **Symptom:** A host clock syscall fires on essentially every backward branch and function entry, not
  just at timeslice expiry.
- **Location:** `SR_YIELD` macro `recomp.h:264-275`; the `||` RHS `sr_vblank_quantum_due()`
  (`sched.c:315-320`, calls `SDL_GetTicksNS()` at `:318`) is evaluated on the **common path** (whenever
  `atomic_fetch_sub_explicit(&sr_timeslice,1,…) <= 1` is **false** — i.e. almost always). `SR_YIELD` is
  emitted at **every** backward branch (`codegen.py:1364`/`1371`) and **every** function entry
  (`codegen.py:1080`), so this is higher frequency than `dispatch()`.
- **Correction:** The wall-clock check exists specifically to preempt sparse guest yield cadences before
  the timeslice counter expires. Moving it only into the expiry branch changes timing semantics and was
  one of the unsafe simplifications in the removed Ponytail override.
- **Safe investigation:** Profile the call on the target system. If material, replace it with a
  scheduler-maintained atomic deadline/due flag or another design that preserves early wall-clock
  preemption. Test logo timing, busy-wait progress, vblank callbacks, the canonical compatibility hook,
  profiler accounting, and scheduler-off behavior.
- **Risk:** High for the old move-to-expiry proposal; do not implement it.
- **Acceptance:** Host-clock calls fall without changing vblank cadence or guest progression.

---

## Tier 2 — Moderate / High

### B5 — `pick_next()` priority-blind anti-starvation rotation (polygon glitches + wasted GE work)

- **Symptom:** Character models corrupt/disappear while moving, recover at rest; occasional polygon pop.
- **Location:** `sched.c:821` `pick_next`; anti-starvation block `:826-850` — `if (other >= 0)` returns the
  **first** other ready thread by array index (`:832-833`) after `same_count >= 3` (`:836`), with **no
  priority comparison**. Priority is only consulted later at `:856`
  (`s_tcb[i].priority < s_tcb[best].priority`). `LAUNCHER_DEMOTE` (`sched.c:606-619`,
  `demoted_priority = 50`) *already* solves the only known starvation via priority. PSP: lower number =
  higher priority (confirmed `:819`/`:856`).
- **Root cause:** `ge.c` keeps one global `GeState ge` whose `ge.bone[]`/`ge.world[]`/`ge.view[]` are
  **not** in `CpuState`. A rotation between an "upload-bones" list and a "draw" list lets the rotated
  thread clobber the worker's bone matrices → glitch, and forces competing GE-list submissions (wasted
  GPU work). The rotation fights `LAUNCHER_DEMOTE` (redundant + harmful).
- **Fix (IMPLEMENTED 2026-07-18):** The anti-starvation rotation was removed outright. The earlier
  proposal kept here (`s_tcb[other].priority >= s_tcb[prev_pick].priority`) was itself wrong under
  the lower-is-higher PSP convention: `>=` still rotates to a numerically larger — i.e. *worse* —
  priority. `pick_next` now computes the best runnable priority and round-robins deterministically
  among READY threads at that priority only (scan cursor starts one slot after the previous
  winner). `SR_ROTLOG`/`SCHED_ROT` no longer exist; use `SR_THLOG` for scheduler-event traces.
  Regression coverage: `make sched-selftest` (`src/rt/sched_selftest.c`) and
  `tools/test_sched_invariants.py`. `LAUNCHER_DEMOTE` is kept; do **not** disable it.
- **Research backing:** Real PSP scheduling is strict-priority preemption with round-robin *only among
  equal priorities*. Matches real hardware; removes both the glitch and the extra GE submissions.
- **Risk:** Behavioral change to scheduling — validated 2026-07-18 with a strict 60 s headless run
  (58,498-file index, display flip, advancing double-buffered FBSNAP frames, zero dispatch/heap/
  scheduler faults). Whether the lobby polygon glitch disappears still needs the GUI route check.
- **Acceptance:** Rotations only occur among equal priorities (now guaranteed by construction and
  regression-tested); polygon glitches during lobby movement disappear (pending GUI confirmation).

### B9 — VFPU fully scalarized through `memcpy` helpers

- **Symptom:** Per-vertex/skinning/matrix math is slow *if this game is VFPU-bound* (confirm via Phase-0
  profiler — do **not** invest blindly).
- **Location:** `codegen.py:422-424` emits `sr_vread`/`sr_vwrite` (`recomp.c:602-641`) per 4-lane op;
  `sr_vread` does **per-lane 4-byte `memcpy` round-trips** for sign/abs fiddling (`:632-638`, not one
  16-byte copy). `_EAT` (`codegen.py:198`: `s->vfpuCtrl[0]=0xe4u;[1]=0xe4u;[2]=0u;`) rewrites 3 control
  words after *almost every* VFPU op (larger cost than implied — most ops don't dirty all three).
  `lv.q`/`sv.q` split into 4 scalar `MEM_R32`/`MEM_W32` (`codegen.py:467-473`). `sr_vfpu_interp` fallback
  `codegen.py:459`/`867`.
- **Fix (only if profiler shows VFPU-bound):**
  - When `vfpuCtrl` prefix is identity, emit `s->v[d]=s->v[s]+s->v[t]` directly — no `sr_vread`/`sr_vwrite`/memcpy.
  - Use **host SIMD** for 4-lane ops: the Allegrex VFPU is **128×32-bit FP registers as 8×4×4 matrices**
    (pspdev VFPU docs; confirmed) → a row/column maps to one `__m128`; a 4×4 matrix to `__m128[4]`. Emit
    `-msse2` + explicit `_mm_*` intrinsics (or `float _s[4] __attribute__((aligned(16)))`). Gate behind a
    flag; keep the scalar fallback.
  - Reduce `_EAT` to only the control words an op actually touches.
- **Research backing:** VFPU 8×4×4 layout maps cleanly to SSE/NEON; PPSSPP and other PSP JITs do the same
  vectorization.
- **Risk:** Moderate (SIMD correctness, strict-aliasing). Gate; keep scalar fallback.
- **Acceptance:** VFPU-heavy scenes speed up; correctness unchanged on non-VFPU paths.

### B13 — `SR_YIELD` on every backward branch (yield frequency)

- **Symptom:** Frequent cooperative yields add scheduling overhead on hot loops.
- **Location:** `codegen.py:1080` (per-function-entry `SR_YIELD`), `:1364`/`:1371` (per back-edge). Macro
  `recomp.h:264-275` does `atomic_fetch_sub` on `sr_timeslice` + `sr_vblank_quantum_due()` per yield (the
  clock-read part is split into **B20**).
- **Fix (code-level):** Emit yield only at loop headers above a size threshold (not every back-edge); use
  a per-thread timeslice counter, only doing the global atomic on an actual yield. Trim the
  **per-function-entry** `SR_YIELD` to loop/root functions only (leaves don't need it). (Keep B20's
  clock-read fix — that is the higher-frequency win.)
- **Risk:** Low; verify scheduler still preempts (vblank still delivered).
- **Acceptance:** Far fewer atomic ops per guest loop.

### B7 — Async I/O faked as synchronous (blocks the one host thread)

- **Symptom:** Streaming cannot overlap with simulation.
- **Location:** `hle.c:3587-3595` `h_IoReadAsync` just calls `h_IoRead` and stashes the result;
  `h_IoWaitAsync` (`hle.c:3605-3612`) returns it immediately. Single host thread (only the SDL audio
  callback is separate).
- **Fix (later phase):** Move `sceIo*Async` onto a real background host read (or host-side async read) so
  the fiber yields while the OS fetches, instead of blocking the whole sim.
- **Risk:** Moderate (concurrency). Do after FPS is playable.
- **Acceptance:** Streaming worker yields during reads; sim keeps advancing.

### B11 — ISO9660: no data cache, global spinlock

- **Location:** `iso.c:50` `static atomic_flag s_iso_lock` busy-wait (`lock_iso` `:52-54`, bare spin with
  **no `_mm_pause`/`yield`**) taken for the **entire** `iso_read` (`read_at_locked` `:60` → `_fseeki64`
  `:63` + `fread` `:67` every call) / `iso_lookup`; only metadata cached (`iso.c:47-49`
  `s_dirs`/`s_paths`/`s_chains`); file CONTENT never cached.
- **Fix:** Sector/data cache + read-ahead window in `iso_read`; relax the per-call global lock (a few
  sharded locks, or lockless read under a shared fd) so concurrent streamers don't serialize. Add a
  `_mm_pause()` (or `Sleep(0)`) inside the `lock_iso` spin.
- **Risk:** Low–moderate.
- **Acceptance:** Repeated/streaming ISO reads avoid disk + lock contention.

### B14 — No-op `_free_r` → long-session spin / freeze — **FIXED**

- **Symptom (historical):** Documented "sched: spin on uid …" freeze on long sessions (ISSUES.md P1).
  The allocator was a real free-list-reuse allocator (`sr_newlib_free`, `recomp.c`), but reuse was
  gated OFF by default because enabling it reproduced a NULL_CALL avalanche onto a static vtable at
  `0x3070c0` within ~45s. That was previously (mis)attributed to an `INIT_ARRAY`/constructor ordering
  race.
- **Actual root cause:** `sr_newlib_free` had no check that the pointer it was asked to free actually
  came from this allocator's arena. A guest free() call path was freeing a foreign/non-heap pointer
  that landed near `0x3070c0` — a static C++ vtable baked into the image as link-time rodata — and the
  function's zero-fill loop, trusting that address's in-place size/flags word, wiped the vtable out.
  Every subsequent object whose vptr pointed into it then null-faulted on dispatch forever. The
  `INIT_ARRAY`-ordering theory was checked and refuted: nothing at runtime legitimately writes
  `0x3070c0` except this bug.
- **Fix:** Added an arena-membership bounds check (`hdr` must lie in `[SR_HEAP_BASE, s_heap_bump_ptr)`)
  in `sr_newlib_free` before trusting the block header, making foreign frees a safe no-op instead of
  corrupting the arena/vtable. Reuse is now **on by default**; `SR_HEAP_REUSE_OFF` is the opt-out for
  A/B comparison against the old bump-only behavior.
- **Risk:** Low (bounds check only narrows accepted pointers; verified via live A/B repro).
- **Acceptance:** Long sessions don't freeze; reuse ships on by default. Verified: 65s headless run
  with reuse on (default) produces zero NULL_CALLs, matching the old reuse-off baseline.

### B21 (syscall-stream logging) — see Tier 1 B6/B19/B21 grouping above

---

## Tier 3 — Minor / specialized

### B15 — `to_layout()` uses `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT`

- **Location:** `ge_gpu.c:311-327` emits full-stage barriers (2× in `submit_pending`, 2× in `tex_upload`,
  1× in `target_readback`, 2× each in `depth_from_cpu`/`depth_to_cpu`, etc. — **11 callers**, easily a
  dozen+ per frame under texture/depth pressure).
- **Fix:** Target specific stages once the B4 ring exists; batch barriers where possible.
- **Risk:** Low.
- **Acceptance:** Narrower barriers; less GPU serialization.

### B22 — GE/display telemetry audit

- `DISPLAY_SET_FB` (`hle.c:3922`), `GE_ENQ` (`:4403`/`:4413`), `GE_UPDATE_STALL` (`:4443`/`:4448`/`:4458`/`:4464`)
  are wrapped in `if (ge_log_on())`.
- The later audit found `ge_gpu.c::stats_tick()` was still unconditional and emitted every five CPU
  seconds. It is now gated by `SR_GPU_LOG`. Keep the Standard-mode quiet-log smoke test in release QA.

### B23 — `dispatch()` range-hook loop calls `h->fn` every dispatch

- **Location:** `recomp.c:1340-1343` iterates `g_range_hooks` (only 2 entries: `hook_resource_handle`,
  `hook_corrupt_callback_queue`, `recomp.c:1175-1179`) and calls each `h->fn` **unconditionally** on every
  dispatch (unlike the exact loop which only calls on key/match). Each predicate hook is a per-dispatch
  function call.
- **Correction:** Both range hooks implement compatibility/corruption handling. Gating them behind a
  diagnostic flag would change behavior. A tri-state return does not avoid calling the predicate.
- **Safe investigation:** Profile each hook. If material, move its address predicate into table metadata
  or replace the underlying workaround; do not disable the hook wholesale.
- **Risk:** High for the prior diagnostic-gate proposal.
- **Acceptance:** Fewer predicate calls with identical hook coverage and live progression.

### B24 — `sr_vblank_quantum_due()` re-runs setup every call

- **Location:** `sched.c:315-320` calls `pace_setup()` + `vblank_pace_quantum_init()` on **every** call
  (i.e. every yield, via B20 path). If non-trivial, cache the setup state.
- **Fix:** Compute `pace_setup()`/`vblank_pace_quantum_init()` once at init; have
  `sr_vblank_quantum_due()` only read `s_last_vblank_ns` + the cached quantum.
- **Risk:** Low.
- **Acceptance:** Per-yield vblank check is a pure timestamp compare.

### B25 — Texture decode cost on update/miss path

- **Location:** `ge_decode_tex_rgba` (`ge.c:768`) is called on every texture update (`:1126`) and miss
  (`:1139`). For 512×512 RGBA that is ~1M texels of unpack per re-decode. This is the *dominant* texture
  CPU cost (distinct from the hash walk, B18).
- **Fix (if profiler shows texture-heavy):** cache the decoded `s_texscratch`/`TexEnt` RGBA and skip
  re-decode when `content_valid` is already set and only the *bind* changed; or keep a decoded RGBA backup
  per `TexEnt` to avoid re-decoding on every re-upload.
- **Risk:** Moderate (memory).
- **Acceptance:** Texture re-bind without VRAM change is near-free.

### B26 — `cmd_begin()` resets a single shared CB each submission

- **Location:** `ge_gpu.c:331` `vkResetCommandBuffer(s_cmd,0)` inside `cmd_begin()`, called by every
  `cmd_submit_wait` path. Part of B4's single-CB limitation.
- **Fix:** Fold into the B4 ring (allocate N CBs from `s_pool` with `RESET_COMMAND_BUFFER_BIT`).
- **Risk:** Low (covered by B4).
- **Acceptance:** No single-CB serialization.

### B27 — `_EAT` rewrites 3 VFPU control words per op

- **Location:** `codegen.py:198` concatenates `s->vfpuCtrl[0..2]=…` after nearly every VFPU op (60+ sites).
- **Fix:** Emit `_EAT` only for the control words an op actually dirties (most ops touch 0–1).
- **Risk:** Low (correctness: must still write the words an op reads as prefix).
- **Acceptance:** Fewer VFPU control-word stores per op.

### B28 — Repeated `MEM_R32` inside `dispatch()` diagnostic fallbacks

- **Location:** `recomp.c:1198-1211` `VFPU_FALLBACK_OTHER` does ~6 `MEM_R32` calls inside an **ungated**
  `fprintf` (only fires when `sr_vfpu_interp` returns `SR_VFPU_OTHER`, which is rare). Negligible on the
  hot path but note it so any future VFPU-interp tuning doesn't trip on it.
- **Fix:** Gate the `MEM_R32` reads behind the same rare-condition (already effectively so).
- **Risk:** None.

---

## Phase ordering (recommended sequence — re-tiered by *measured likelihood* + risk)

| Phase | Items | Why here | Expected impact |
| --- | --- | --- | --- |
| 0 | Measure (Phase 0 above) | Baseline + CPU/GPU split + submit/wait count | — (a number to beat) |
| 1 (implemented; verify) | **B6, B19, B21, B22** | Gate only the logging paths confirmed by source and the Standard-mode run; rebuild and measure before claiming a gain. | quieter baseline; impact unknown |
| 2 (days) | **B1 → B4 → B15 → B26** | The 11 `cmd_submit_wait` sites serialize CPU↔GPU and the live sample shows high submit pressure; wall-time attribution is still required. B4's ring would be part of making deferred waits safe. | measure after each change |
| 3 (days) | **B2** (+ B13, B27) | Recomp CPU experiment: shrink TUs (`FUNCS_PER_CHUNK`) + strip trace text, then test `-O1`/`-O2` under GCC with `-fno-strict-aliasing`. (B8 retired.) | unknown until benchmarked |
| 4 (with P2) | **B5** (+ measured B23/B24 investigation) | Glitch diagnosis first; preserve correctness hooks and vblank semantics. | correctness first; FPS unknown |
| 5 (if profiler says VFPU-bound) | **B9** (+ B25) | SIMD for skinning/matrix math + decoded-tex cache | scene-dependent |
| 6 (after playable) | **B7, B11, B14** | Loading/streaming + long-session freeze | smoother loads, no freeze |
| 7 (advanced) | PGO/ThinLTO on runtime+`ge.c`; multi-threaded cmd recording; `VK_KHR_present_wait` pacing; `SR_GPU_SCALE` past 30 FPS | Polish past 30 FPS | headroom |

The earlier phase table's projected multipliers were hypotheses, not measurements. B18, B20, B23, and
B17 are specifically blocked on safer designs; B10 and B16 require isolated A/B validation. B12 remains
low priority because the store-watch scan is already debug-gated.

---

## Research appendix (2026 sources, re-confirmed + corrected this pass)

- **Vulkan `wait_idle` sample (Khronos):** ~22% frame-time improvement replacing `vkDeviceWaitIdle` with
  per-frame fences — fences let the CPU keep submitting while the GPU renders. → B1.
  (<https://www.khronos.org/> — Vulkan samples `wait_idle`; the principle is documented across the Vulkan
  Samples repo.)
- **PPSSPP GPU-readback issue #16900 (hrydgard):** *"it's essential to have a frame or two 'in progress',
  pipelined between the CPU and GPU at the same time. The CPU runs a frame or two ahead of the GPU.
  Stopping this pipeline to read back data basically tells the system to sleep."* Recommends readback
  queues + loose async readback (fence + background continuation). → B1/B4.
  (<https://github.com/hrydgard/ppsspp/issues/16900> — opened 2015, content current; #16916 implements
  delayed readbacks for Dangan Ronpa.)
- **NVIDIA Advanced API Performance: Command Buffers:** *"Aim for 5–10 ExecuteCommandList calls per frame
  with sufficient GPU work to hide the OS scheduling overhead"*; *"Don't block on ExecuteCommandList
  calls"*; *"Each command queue can use its own thread."* → B1/B4.
  (<https://developer.nvidia.com/blog/advanced-api-performance-command-buffers/> — 2021-10-25.)
- **NVIDIA Vulkan Do's and Don'ts:** *"Minimize the number of queue submissions by batching command
  buffers, but be aware that aggressive batching can introduce latency."* → B1.
  (<https://developer.nvidia.com/blog/vulkan-dos-donts/> — 2019-06-06.)
- **zeux.io efficient Vulkan renderer:** *"a Vulkan application should target <10 submits per frame … and
  <100 command buffers per frame"*; prefers `vkResetCommandPool` reuse + per-thread pools. → B1/B4.
  (<https://zeux.io/2020/02/27/writing-an-efficient-vulkan-renderer/> — 2020-02-27.)
- **vkguide.dev command-buffer lifecycle:** *"while the GPU is busy rendering one buffer, we can write
  into a different one"* (double/triple-buffering); `vkResetCommandPool` once per frame;
  `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT`. → B4.
  (<https://www.vkguide.dev/docs/chapter-4/double_buffering/> ;
  <https://www.vkguide.dev/docs/chapter-1/vulkan_command_flow/>.)
- **Vulkan Samples `command_buffer_usage`:** pool reset (`vkResetCommandPool`) preferred over
  allocate/free; ~15% from multithreaded recording; keep secondary-CB count low. → B4.
  (<https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html>.)
- **Khronos timeline semaphores blog + Vulkan Samples `timeline_semaphore`:** a single 64-bit monotonic
  counter replaces a web of binary semaphores + fences; *"no need to reset after a signal operation
  before reuse"*; *"Enable omnidirectional synchronization between device and host using a single
  primitive"*; WSI present still needs binary semaphores. → prefer for the B4 readback ring.
  (<https://www.khronos.org/blog/vulkan-timeline-semaphores> — 2020-01-15;
  <https://docs.vulkan.org/samples/latest/samples/extensions/timeline_semaphore/README.html>.)
- **N64Recomp (Mr-Wiseguy/Nefarious):** one C function per guest function; `jal`→direct C call;
  `jr`→`LOOKUP_FUNC(ctx->rN)(...)`. **Correction:** N64Recomp keeps guest registers in a **context struct**
  (`ctx->r4 = ADD32(ctx->r4,…)`), it does **NOT** promote to C locals. HST's `s->r[]` already matches this
  proven design → **B8 retired**.
  (<https://github.com/Mr-Wiseguy/N64Recomp>.)
- **Clang vs GCC memory (CORRECTED — prior claim FALSE):** The prior plan claimed "Clang ≈65% of GCC's
  peak RSS, ~4× faster frontend, compiled a 24 MB TU that OOM'd GCC." This is **not supported** by
  evidence and is contradicted by it:
  - LLVM issue #83122 (2024-02): Clang consumed **14 GB** where GCC used **1.2 GB** on a medium TU
    (the OPPOSITE of the claim). (<https://github.com/llvm/llvm-project/issues/83122>.)
  - LLVM Discourse #35204 (2015): Clang **12.5 GB** vs GCC **0.5 GB** on a generated TU (again Clang worse).
    (<https://discourse.llvm.org/t/clang-3-6-and-trunk-high-rss-usage-compared-to-gcc-12-5gb-vs-0-5gb/35204>.)
  - GNU Radio 2024 GCC mailing-list post: Clang **10.8 GB** vs GCC **15–30 GB** (mixed — Clang *less* here,
    but results vary wildly by code shape). The "Clang is reliably lower memory" claim is unsafe.
  - **Conclusion:** the OOM-safe lever is **shrinking the generated TUs** (`FUNCS_PER_CHUNK` 2000→250–400) +
    **stripping the trace-wrapper text** under the existing GCC toolchain — NOT switching to Clang. Clang
    is demoted to an *optional experiment* (with `-fno-strict-aliasing`), not the primary recommendation.
- **PGO / ThinLTO (Chromium-evidenced):** Chromium's own build docs show ThinLTO links **3.5×–10× faster
  than full LTO** (<https://chromium.googlesource.com/chromium/src/+/master/docs/pgo.md> ; ThinLTO GN arg
  history: *"linking time is 3.5x smaller compared to the full LTO. With additional profiling it might
  reach 10x"*). Apply to **runtime + `ge.c`** (`-O2 -flto=thin -fprofile-use`), NOT the `-O0` chunks; keep
  profile opt-level stable. **Note:** the prior plan cited "arXiv 2507.16649v1" — that specific paper could
  **not be verified** this pass and is flagged UNVERIFIABLE; cite Chromium's real-world evidence instead.
  (<https://clang.llvm.org/docs/ThinLTO.html> ; <https://www.chromium.org/>.)
- **Clang + Windows fibers (CORRECTED nuance):** `sr_coro.c` uses **Win32 fibers** (`ConvertThreadToFiber`
  / `CreateFiberEx` / `SwitchToFiber`, `sr_coro.c:45,65,73`), not ucontext, on Windows. So the MinGW-
  ucontext caveat does not apply — Clang on MSYS2 UCRT64 is *safe to try* for B2, but per the memory
  correction above it is no longer the recommended OOM fix. (The LLVM "no C++20 coroutines on 32-bit x86
  Windows" change is about coroutines, not ucontext/setjmp.)
- **Allegrex VFPU (pspdev VFPU docs; confirmed):** *"The VFPU has 8 matrices, each containing 16 elements
  (4 rows by 4 columns)… a vector is composed of elements from a single matrix."* → direct SSE/NEON
  `__m128` (one row/column = one `__m128`; one 4×4 matrix = `__m128[4]`). → B9.
  (<https://pspdev.github.io/vfpu-docs/> ; <https://github.com/pspdev/vfpu-docs>.)
- **Khronos descriptor_management sample:** caching descriptor sets cut frame time ~38% — but **HST
  already caches descriptors per-texture** (`make_descriptor` at `tex_upload` `ge_gpu.c:1144`; cached
  `e->set`/`src->set_l` returned on hit; draw-batching at `:1509`). Do **not** add a descriptor-caching
  item. → B18 is the real per-draw CPU cost.
- **`VK_KHR_present_wait` / `present_id`:** gate frame production to display rate once FPS clears P0 —
  correct pacing later, not a now-win. See the official
  [`VK_KHR_present_wait` reference](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_present_wait.html).
- **`VK_EXT_descriptor_buffer`:** deletes `vkUpdateDescriptorSets` from the hot path — not needed here
  (descriptors already cached per-texture).

---

## Notes / guardrails (from AGENTS.md)

- **RETIRED B8** — do **not** implement "promote guest registers to C locals." The premise (N64Recomp does
  this) is false; HST's `s->r[]` struct design is already correct. Manual caching risks ABI breakage and
  is unnecessary at `-O2`.
- Do **not** reintroduce `LOOP_CAPS` (root-caused and retired).
- Do **not** add/remove `--wrap=` linker flags.
- Do **not** hand-edit `build/hst/hst_recomp_*.c`; change `tools/codegen.py`.
- Keep `GAME_BASE=0 GAME_ENTRY=0` for HST; prefer `.\hst_manager.ps1` over bare `make`.
- Don't uncap framerate past 30 FPS until P0/P1 (ISSUES.md) are cleared — `SR_GPU_SCALE` headroom is a
  *later* win.
- After each phase: run `selftest` and the `verify` gates; validate against an **extended live lobby
  session**, not just a short headless smoke test.

---

## Current implementation and next measured steps

1. **Implemented:** gate the six actually ungated HLE messages and `GEGPU stats` behind their existing
   diagnostic controls.
2. **Implemented:** remove the generated Ponytail `SR_YIELD` macro override; regenerate all chunks so
   the canonical scheduler/profiler/compatibility behavior is used.
3. **Verify now:** run `BuildFull`, selftests, Python tests, dashboard lint/build, and a fresh Standard
   run. Confirm no generated file contains the override and Standard logs contain no periodic stats.
4. **Instrument next:** record wall-clock CPU simulation, GPU submit/wait, readback, and present time in
   the 3D lobby. The main-menu submit/present ratio is a lead, not a complete profile.
5. **Do not implement without a safer design:** B18 texture-hash removal, B20 clock-check relocation,
   B23 hook gating, or B17 non-atomic dispatch loads.
6. **A/B experiments only after the baseline:** B10 runtime optimization and B16 pipeline cache. Keep
   each change isolated and revert it if the measured benefit or live parity is absent.

Line numbers in this document are investigation aids, not stable identifiers; prefer the named symbols
when source edits move them.
