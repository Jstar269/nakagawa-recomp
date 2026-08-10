# Performance and iteration evidence

This page records reproducible performance/build measurements. GitHub Actions are account-blocked;
all evidence below is local-only.

## Current build profiles

- Native runtime objects default to `RUNTIME_OPT=-O0`.
- Generated guest translation units remain `RECOMP_OPT=-O0`.
- `RUNTIME_OPT=-O2` is verified and selectable, but not promoted without a scene-identical benchmark.
- `FUNCS_PER_CHUNK=2000` is still the default. It is configurable for experiments, but the compile
  matrix below did not justify changing it independently of generated-code optimization.

Runtime, generated-code, and codegen profiles have separate content-addressed stamps and manifests.
Changing a profile invalidates its complete object set. Every C compilation emits `-MMD -MP`
dependency metadata, so transitive header edits rebuild affected objects and deleted headers do not
strand stale dependency rules. Direct Make and `hst_manager.ps1` builds canonicalize the Vulkan SDK
path to the same profile.

## Generated-code compile matrix (2026-07-26)

All cells regenerated the same 14,379 functions/register records and compiled in isolated directories
with the same UCRT64 GCC. Peak memory is the sampled compiler process-tree working set. Source size was
about 200.35 MiB in every cell.

| Functions/TU | Opt | Compile time | Peak memory | Object bytes | Objects |
| ---: | :---: | ---: | ---: | ---: | ---: |
| 2000 | O1 | 325.594 s | 2382.4 MiB | 37.41 MiB | 9/9 |
| 2000 | O2 | 607.962 s | 2487.6 MiB | 38.97 MiB | 9/9 |
| 1000 | O1 | 312.476 s | 1784.5 MiB | 37.44 MiB | 16/16 |
| 1000 | O2 | 561.836 s | 1935.5 MiB | 38.98 MiB | 16/16 |
| 500 | O1 | 311.367 s | 1166.5 MiB | 37.55 MiB | 30/30 |
| 500 | O2 | 554.557 s | 1159.1 MiB | 39.16 MiB | 30/30 |
| 250 | O1 | 313.782 s | 1029.6 MiB | 37.82 MiB | 59/59 |
| 250 | O2 | 555.366 s | 1031.3 MiB | 39.45 MiB | 59/59 |

`500/O1` is the compile-memory knee, but generated O1/O2 is not promoted: O1 still costs about 5.2
minutes and no live correctness/performance benefit was established; O2 is slower and larger. The
first discarded matrix attempt did not have GCC's runtime DLL directory on `PATH`; it failed at
compiler startup and is not included above.

## Native runtime O0 versus O2 (instrumented pair)

Both runs used generated O0/2000, the same private savedata baseline, the same pad script, strict
vblank pacing, fatal/default dispatch behavior, the Benchmark profile, captures over vblanks
27,000-30,000, and a clean exit at vblank 30,000.

| Runtime | Wall time | Whole-route vblanks/s | Result |
| :---: | ---: | ---: | --- |
| O0 | 847.5 s | 35.4 | clean; 10 live-court captures |
| O2 | 777.2 s | 38.6 | clean; 10 live-court captures |

O2 improved throughput by 9.0% within this observation-instrumented pair. Both image sets show coherent
3D court, HUD, players, ball, shadows, and active play. This is not a scene-identical comparison: pad
input and savedata were held constant, but the title selected Takehito in O0 and Ivan in O2. The number
is therefore diagnostic only, not promotion evidence.

An additional detached, clean build of literal starting main `13ce647ace6bf4c3e7b0366ea5ef10c6ce4fcd28`
(executable SHA-256 `E2E520AC9DA008DD998384374487F4A81B973426A8A1BBBEEB42D03DC151C438`)
completed the same 30,000-vblank command in 599.6 s, or 50.0 guest vblanks/s. Its captures were still on
match setup and the tutorial at vblanks where both instrumented runs were rallying. Comparing those wall
times would compare different scenes, and the reason telemetry itself also adds overhead. O2 is thus
not promoted; the default remains O0 until a replay holds the GE/gameplay workload constant.

The private shortened input route is `logs/route_perf_live_30000_20260726.pad`; its SHA-256 is
`741BDF042A846C0424B0ED628890043F0CF78237C3140B74BED7F826C768B590`. It ends at the last input before
the 30,000-vblank exit, removing the original route's 34,000-41,200 pause/give-up tail. Route inputs are
deterministic; title choice and animation timing are not yet a deterministic gameplay oracle.

## Renderer reason telemetry

No synchronization or blending behavior was changed. Existing submits and waits are now classified as
render batch, destination snapshot, texture/target/depth upload, depth/target readback or transition,
transfer blit, and initialization. Shader-blend selection records framebuffer-16-bit, dither, absdiff,
doubled-alpha and dual-FIX reasons; snapshot requests, cache hits, and actual copies are separate.

In the O2 live-court window, all 206,407 snapshot requests missed and copied. Those requests matched the
206,407 shader-blend states, and every state carried both the 16-bit-framebuffer and dither reasons;
absdiff, doubled-alpha, and dual-FIX did not occur. Per wall-second, render-batch waits cost 198.0 ms and
snapshot-copy waits cost 189.7 ms. Submit rates were 1,986.5/s for render batches, 1,979.6/s for
snapshots, and 6.9/s for target-readback transitions. This strengthens the destination-snapshot lead,
but does not justify removing snapshots or fences globally.

A PPSSPP v1.20.4 software-renderer live-match frame dump is preserved privately at
`third_party/ppsspp/memstick/PSP/SYSTEM/DUMP/UCUS98701_0002.ppdmp` (SHA-256
`A87169416CC6A2DA9BBEF911F95E0970071123655B9F4592114356EE005D55DB`). A local
`PPSSPPHeadless` built from the existing PPSSPP checkout at
`f0c28c67446fd9a08b124ea2bfb0e997fe909de5` replayed it through the software renderer; pre-existing
local PPSSPP worktree changes remain private and are not part of this repository. The dump renders to a
GE target without changing the emulated display pointer, so the capture selected
`GPU_DBG_FRAMEBUF_RENDER`; that temporary local selection was reverted after measurement. The cold
replay took 453.89 ms and two warm replays took 72.35 and 76.81 ms
(74.58 ms mean). All three produced the same 512x272 BMP (SHA-256
`A74A503FA123BC4C678AF7FFC65EA76202AA2FCB9FA2FF295298E4145652EA9A`), visibly the intended live
clay-court frame with both players, HUD, ball/landing markers and serve overlay.

## Seconds-scale Nakagawa GE replay

The generic `.ngef` format records initial `GeState`, software depth, all 2 MiB of VRAM, the frame's GE
list addresses, and sparse first-touched non-VRAM pages containing command, vertex, index, texture and
CLUT data. `make ge-replay` builds a standalone software/Vulkan runner; game-derived fixtures remain
ignored. Synthetic tests cover sparse round-trip, multiple list addresses, VRAM/depth restoration and
the untracked-fixture policy.

The one permitted live capture attempt was rejected: the restored baseline entered a miniature-player
match state, and interruption produced neither a fixture nor an admissible snapshot. No second gameplay
run was made. Instead, a narrow private converter translated the already-verified PPSSPP dump's command
and payload stream into the same generic fixture without adding a general PPSSPP importer to the tracked
tree. The private 4,871,180-byte fixture has SHA-256
`C3D851CF02AE429CC1BC20025D46603F4A09654CF7E2FA75A0026A39C1B0414A`.

Three independent three-frame software samples took 265.085, 267.179 and 267.962 ms/frame (266.742 ms
mean). Every output was byte-identical (480x272 PPM SHA-256
`A8897DF4B704188A62AA57D7A3D30B9DE72FA962893D216338B67AEB4D4DB612`). Against the PPSSPP software
image cropped to the same 480x272 visible area, the scene/layout is the same but the pixels are not an
exact cross-renderer match: 43,795/130,560 pixels match exactly (33.544%), mean absolute channel error is
7.226, and PSNR is 22.957 dB. PPSSPP remains the visual reference; exact hashes gate Nakagawa changes.

## First host-synchronization optimization: eight in-flight slots

The Vulkan GE now uses eight command slots, each with its own command buffer, fence and mapped vertex
buffer. Render and destination-snapshot submissions remain in the same queue and may stay in flight;
the CPU waits only when recycling a slot or reaching a CPU-visible materialization/resource-lifetime
boundary. A boundary retires all outstanding fences with one wait-all. Every snapshot copy, 16-bit
framebuffer/dither rule, shader-blend path and render -> copy -> next-render ordering barrier remains.
Set `SR_GPU_SYNC_SUBMIT=1` to select the previous exact submit-and-wait path.

Stable results are the mean of three independent 20-frame processes on the same private fixture and
binary:

| Path | ms/frame | Render submits / waits | Render wait ms | Snapshot req/copy/submit/waits | Snapshot wait ms | Batches/draws |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| synchronous fallback | 224.201 | 359 / 359 | 44.188 | 358 / 358 / 358 / 358 | 35.621 | 360 / 360 |
| 8-slot default | 140.150 | 359 / 356 | 4.463 | 358 / 358 / 358 / 355 | 1.465 | 360 / 360 |

The new path is 37.489% faster (1.600x throughput). All six 20-frame outputs and the final explicit
fallback/default pair are byte-identical, SHA-256
`81C0294C7BF7DDCEF197BEFCE9086CFECD9791092ABC55D72E8FE36E58337A0B`. This does not change or promote
native-runtime O2; generated and native optimization profiles remain the separate question above.

## Second host-synchronization optimization: command-buffer batching

`cmd_drain()` now assigns a wait-all covering different submission reasons to a single `mixed` bucket;
it no longer charges the complete duration to both render and snapshot. The Vulkan backend can record
consecutive render -> destination-copy -> render operations into one command buffer while retaining
every image-layout barrier. Each recorded render gets a distinct mapped vertex-arena range until the
containing submission retires. Texture/target/depth uploads, CPU readback, presentation and resource
lifetime changes remain boundaries because they still share `s_xfer` or expose data to the CPU.
`SR_GPU_SYNC_SUBMIT=1` remains the exact synchronous fallback, and `SR_GPU_SUBMIT_BATCH=1..8` selects
the cheap policy experiment (default 8).

The first validation-layer run blocked promotion by finding 710 live Vulkan objects at device
destruction. Explicit backend teardown fixed that lifecycle defect. The final rebuilt executable was
then clean under `VK_LAYER_KHRONOS_validation` with synchronization validation enabled, in both the
8-operation path and `SR_GPU_SYNC_SUBMIT=1`; no validation error, VUID or synchronization hazard was
reported.

Stable timings are means of three independent 20-frame processes using the same binary and private
fixture. Physical queue submits include render/snapshot chains plus upload/readback/resource boundaries.

| Operations/submit | ms/frame | Physical queue submits/frame | Mixed submits/frame | Output SHA-256 |
| ---: | ---: | ---: | ---: | :--- |
| 1 | 101.315 | 845.1 | 0.0 | `81C0294C...37A0B` |
| 2 | 95.774 | 487.1 | 358.0 | `81C0294C...37A0B` |
| 4 | 93.392 | 352.1 | 224.0 | `81C0294C...37A0B` |
| 8 | 92.956 | 294.1 | 166.0 | `81C0294C...37A0B` |

The 8-operation policy lowers replay time by **8.251%** versus the one-operation policy on the same
binary (1.090x throughput) and lowers physical submissions by 65.199%. The 4-to-8 gain is only 0.467%,
so lower submit count is already yielding diminishing wall-time returns. Every policy, the post-build
validation pair and the synchronous fallback produced the exact canonical 480x272 PPM SHA-256
`81C0294C7BF7DDCEF197BEFCE9086CFECD9791092ABC55D72E8FE36E58337A0B`.

One post-promotion private Benchmark run used the shortened 30,000-vblank input, native/generated O0,
the 8-operation default, no save restoration, and captures from 27,000-30,000. It exited cleanly at
30,000/30,000 with 39 captures in 569.4 s (52.7 guest vblanks/s). The captures were stable and showed no
vertex corruption, stale regions or barrier-ordering artifact. They remained on match setup through
vblank 28,841, then shot-timing tutorial/court intro through vblank 29,954; no active rally occurred.
The window averaged 11.426 FPS, 41.813 vblanks/s, 348.443 GE submits/s and 57.661 ms/s GE wait. Because
the older baseline was an active match (4.679 FPS, 24.662 vblanks/s, 4,459.930 GE submits/s and
439.321 ms/s GE wait in its 27,000-30,000 window), the gameplay numbers are **not** a scene-identical
before/after and no gameplay improvement percentage is claimed. The current Nakagawa save is the old
`Jstar` oracle baseline; PPSSPP's expected `Z` save is encrypted and was not substituted or modified.

## Third host-synchronization optimization: fence-owned upload staging

Exact physical-boundary telemetry was added before changing `s_xfer`. On the same binary with the
staging ring explicitly disabled (`SR_GPU_XFER_RING_KB=0`), policy 8 averaged 294.1 queue submits/frame.
The measured submit-call plus fence-wait cost below is the mean of three independent 20-frame processes;
counts and times are per frame.

| Boundary reason | Submits | Submit ms | Waits | Wait ms | Total ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| render/snapshot chain | 166.0 | 3.419 | 0.0 | 0.000 | 3.419 |
| texture upload | 126.0 | 1.423 | 126.0 | 9.434 | 10.857 |
| target upload | 1.0 | 0.022 | 1.0 | 0.079 | 0.101 |
| depth upload | 1.0 | 0.018 | 1.0 | 0.063 | 0.081 |
| readback/materialize | 0.1 | 0.002 | 0.15 | 0.013 | 0.016 |
| present | 0.0 | 0.000 | 0.0 | 0.000 | 0.000 |
| lifetime/resource drain | 0.0 | 0.000 | 0.95 | 0.077 | 0.077 |
| other (slot recycle) | 0.0 | 0.000 | 158.0 | 0.204 | 0.204 |

Uploads were therefore 128.0 of 128.1 non-render/snapshot submissions (99.922%) and cost 11.039
measured ms/frame. That passed the experiment's dominance gate.

Each of the eight submit slots now owns a persistently mapped transfer-source ring. Every upload in an
open chain gets a device-aligned, non-overlapping range; the range is reset only after that slot's fence
retires. Capacity exhaustion submits and advances to another fenced slot. An upload larger than the
configured ring uses the prior shared-`s_xfer` synchronous path, and `SR_GPU_XFER_RING_KB=0` disables the
experiment entirely. CPU readback/materialization stays a hard boundary. All pre-existing image
barriers, destination snapshots, fb16/dither quantization, shader blending and
`SR_GPU_SYNC_SUBMIT=1` remain.

Stable timings are means of three independent 20-frame processes using one rebuilt binary and the same
private fixture:

| Policy | ms/frame | Physical submits/frame | Output SHA-256 |
| --- | ---: | ---: | :--- |
| policy 8, ring disabled | 96.633 | 294.10 | `81C0294C...37A0B` |
| ring, policy 1 | 95.429 | 845.10 | `81C0294C...37A0B` |
| ring, policy 2 | 88.433 | 423.15 | `81C0294C...37A0B` |
| ring, policy 4 | 84.327 | 212.15 | `81C0294C...37A0B` |
| ring, policy 8 | 81.862 | 106.15 | `81C0294C...37A0B` |
| exact synchronous fallback | 147.429 | 845.10 | `81C0294C...37A0B` |

The default policy-8 ring is **15.286% lower ms/frame** than its same-binary ring-disabled path
(1.180x throughput), with **63.907% fewer physical submissions**. Its remaining per-frame boundaries
are 106.0 render/snapshot-chain submits, 0.05 texture-upload submits, 0.10 readback submits and no
target/depth/present/lifetime/other submits; lifetime and slot-recycle waits cost 0.158 and 0.116
ms/frame respectively. All measured outputs have the full canonical SHA-256
`81C0294C7BF7DDCEF197BEFCE9086CFECD9791092ABC55D72E8FE36E58337A0B`.

A deliberately tiny 64 KiB ring exercised 80 wraps and 65 oversized-upload fallbacks over five replayed
frames without overwriting in-flight data. `VK_LAYER_KHRONOS_validation` with synchronization validation
enabled reported zero warning, error, VUID or synchronization hazard in policy-8, tiny-ring and
`SR_GPU_SYNC_SUBMIT=1` runs; each produced the canonical hash.

Finally, `logs/issue33_native_savebase_20260726` was captured privately from the live Nakagawa plaintext
save (not PPSSPP encrypted data and not `logs/oracle_savebase`). `DATA0.BIN` SHA-256 is
`5A7C79705682785FD244E2237D9C9C74E6343123E7268276600F5C8482092C45`. One bounded Benchmark route
restored that baseline and exited cleanly at 33,000/33,000 vblanks in 590.1 s (55.9 guest vblanks/s),
with 49 captures. Captures at vblanks 32,217-32,844 show normal-sized players and active serve/return
play with stable court/HUD/ball rendering. The 32,200-32,850 window averaged 10.103 FPS, 47.691
vblanks/s, 871.538 GE submits/s and 99.695 ms/s GE wait. This is route/setup evidence only: no second
long run was made and no gameplay improvement percentage is claimed.

## CPU renderer profile and exact texture-shadow reuse

The replay target previously compiled `ge.c` inside one aggregate `CFLAGS=-O0` link command even though
production has a dedicated `GE_CFLAGS=-O2` object rule. It now links the same `ge.o` used by production;
this is measurement truth, not a native-runtime O2 promotion. Fresh results below use only this rebuilt
binary and are not compared with historical 81.862 ms/frame evidence.

`SR_GPU_CPU_PROFILE=1` adds exclusive aggregate CPU timers and one process summary. Profiling is off by
default. On exact old behavior, profiler-off averaged 32.277 ms/frame and profiler-on 33.438 ms/frame,
a measured 1.160 ms/frame (3.595%) overhead. Three independent profiled 20-frame processes identified
texture decode as the dominant category:

| CPU phase | Calls/frame | ms/frame | Measured CPU share |
| --- | ---: | ---: | ---: |
| texture decode | 126.00 | 16.176 | 75.081% |
| texture/sampler/target lookup and hashing | 1,426.00 | 1.406 | 6.524% |
| snapshot copy-region/barrier recording | 358.00 | 0.998 | 4.631% |
| other Vulkan command recording | 359.00 | 0.914 | 4.242% |
| vertex preparation | 12,355.00 | 0.768 | 3.562% |
| state preparation/cache comparison | 12,357.00 | 0.678 | 3.148% |
| measured memcpy | 485.00 | 0.302 | 1.400% |
| descriptor/pipeline bind recording | 719.00 | 0.179 | 0.829% |
| all remaining measured phases | - | 0.124 | 0.575% |

Per frame the steady profile had 360 pipeline lookups (359.75 hits, 0.25 misses/creations), 6.4
descriptor allocations, 12.8 descriptor writes, 359 pipeline binds plus one already-avoided redundant
bind, 360 descriptor binds with none redundant, 360 state-key builds, and 358 snapshot requests/copies.
The compact parser rejects missing or duplicate phase summaries so nested categories cannot silently be
merged in future measurements.

The one selected optimization retains a bounded (64 MiB maximum) byte shadow for cached texture inputs.
When a guest write or deterministic replay reset invalidates a texture, the backend skips decode/upload
only if the complete raw source range and, for indexed textures, the complete 2 KiB CLUT plus CLUT format
are byte-identical. Swizzled and DXT source extents cover the exact decoder-accessed storage. A mismatch,
unrepresentable wrap, allocation failure or capacity exhaustion uses the original decode/upload path.
`SR_GPU_TEX_SHADOW_DISABLE=1` selects exact old behavior in the same binary.

Three independent 20-frame processes per mode produced:

| Profiler | Mode | Run 1 | Run 2 | Run 3 | Mean ms/frame | Range | Submits/frame | Output SHA-256 |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| off | shadow disabled | 32.397 | 32.142 | 32.293 | 32.277 | 0.255 | 106.15 | `81C0294C...37A0B` |
| off | shadow enabled | 16.348 | 16.483 | 16.333 | 16.388 | 0.150 | 90.95 | `81C0294C...37A0B` |
| on | shadow disabled | 33.742 | 33.283 | 33.288 | 33.438 | 0.459 | 106.15 | `81C0294C...37A0B` |
| on | shadow enabled | 17.503 | 18.011 | 17.387 | 17.634 | 0.624 | 90.95 | `81C0294C...37A0B` |

The profiler-off same-binary improvement is **49.227% lower ms/frame** (1.969x throughput) and
**14.319% fewer physical submissions**. In the profiled candidate, decode fell from 16.176 to
0.834 ms/frame; 119.7 exact shadow checks/hits cost 0.130 ms/frame. Snapshot requests/copies remained
358/frame. The fixture intentionally restores identical captured memory each repeat, so its 100% shadow
hit rate after the first frame is deterministic replay evidence. The bounded live measurement below
shows that this favorable reuse pattern does not generalize to the sampled rally.

`VK_LAYER_KHRONOS_validation` with synchronization validation enabled reported zero warning, error,
VUID or hazard in promoted policy-8, shadow-disabled, `SR_GPU_SYNC_SUBMIT=1`, and
`SR_GPU_XFER_RING_KB=0` runs. All validation and A/B outputs retained full SHA-256
`81C0294C7BF7DDCEF197BEFCE9086CFECD9791092ABC55D72E8FE36E58337A0B`. No long gameplay run was made.

## End-to-end replay wall budget and triangle-strip vertex reuse

`SR_GE_REPLAY_WALL_PROFILE=1` adds monotonic aggregate timers around deterministic reset, fixture apply,
GE state restore, all list execution, miscellaneous loop work and final materialization. Each emitted
`GE_REPLAY_WALL_PHASE` line is explicitly classified as `HARNESS-ONLY` or `PRODUCTION-RELEVANT`; the
strict parser rejects missing, duplicate, extra or misclassified phases. Three independent 20-frame
wall-profile processes on the exact old strip path accounted for the fresh 16.261 ms/frame mean:

| Non-overlapping wall category | ms/frame | Wall share | Production relevant? |
| --- | ---: | ---: | :--- |
| deterministic reset/apply/restore/loop overhead | 0.814 | 5.004% | no |
| all `ge_run_list()` calls | 14.669 | 90.205% | yes |
| final GPU materialization, amortized | 0.779 | 4.790% | yes, replay CPU-visible boundary |
| unaccounted outer-loop residual | 0.000 | 0.001% | unknown |

This fresh instrumented baseline is consistent with the landed 16.388 ms/frame result; process-to-process
variance is why the optimization A/B below uses one newly built binary and alternates exact paths. The
wall-only timers did not expose a material harness target: GE list execution is the dominant global phase.

`SR_GE_CPU_PROFILE=1` therefore adds a coarse `ge.c` profile, off by default, while preserving the
dedicated production `GE_CFLAGS=-O2` object. The deep profile is intentionally nested: `list_total` and
`primitive` are inclusive, while the GPU-hook time is inside primitive time. On the old strip path its
instrumented 16.403 ms/frame list total decomposed as follows without double-counting:

| `ge_run_list()` category | ms/frame | Share of list total |
| --- | ---: | ---: |
| GE frontend exclusive (dispatch + CLUT/flush + primitive excluding GPU hook) | 5.473 | 33.367% |
| renderer CPU exclusive phases | 5.995 | 36.551% |
| queue submit calls | 1.515 | 9.236% |
| fence/slot waits | 0.118 | 0.718% |
| remaining uninstrumented/timer residual | 3.302 | 20.128% |

The deep timers add 1.296 ms/frame (7.784%) over the unprofiled old-path mean because the fixture invokes
12,357 timed GPU hooks per frame. They are diagnostic, not the headline performance comparison. Counts
showed 35,913 commands and 1,417 primitive commands per frame: 311 triangle lists, 1,029 triangle strips
and 77 sprite lists. The legacy strip loop decoded/transformed 39,012 vertex uses for 24,874 submitted
vertices because it reloaded all three vertices for every triangle.

The one selected optimization retains the previous two decoded/transformed vertices within a single
triangle-strip draw. Each new triangle decodes only its new vertex; no data survives the draw or crosses
a GE command. `SR_GE_STRIP_CACHE_DISABLE=1` selects the exact legacy loop in the same binary. Culling,
provoking-vertex order, clipping and renderer semantics are unchanged. The candidate reduced actual
decode/transform uses from 39,012 to 24,874 per frame (14,138 reused, 36.240%). In the deep profile,
primitive work excluding the GPU hook fell from 5.048 to 3.580 ms/frame (29.085%), while renderer and
queue categories remained flat.

Three independent unprofiled 20-frame processes per exact path produced:

| Mode | Run 1 | Run 2 | Run 3 | Mean ms/frame | Range | Submits/frame | Output SHA-256 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| strip reuse disabled | 16.495 | 16.820 | 16.619 | 16.645 | 0.325 | 90.95 | `81C0294C...37A0B` |
| strip reuse enabled | 15.228 | 14.794 | 15.191 | 15.071 | 0.434 | 90.95 | `81C0294C...37A0B` |

That is **9.456% lower ms/frame** (1.104x throughput) with no submit-count change. Snapshot requests and
copies remained 358/frame. All 18 outputs across unprofiled, wall-only and combined-profile A/B processes
had full SHA-256 `81C0294C7BF7DDCEF197BEFCE9086CFECD9791092ABC55D72E8FE36E58337A0B`.
Software old/candidate outputs were also mutually identical at
`A8897DF4B704188A62AA57D7A3D30B9DE72FA962893D216338B67AEB4D4DB612`.

Vulkan core and synchronization validation reported zero warning, error, VUID or hazard in the promoted
candidate, strip-disabled, texture-shadow-disabled, `SR_GPU_SYNC_SUBMIT=1`, and
`SR_GPU_XFER_RING_KB=0` modes. Every Vulkan mode produced the canonical PPM hash.

Low-overhead `SR_GPU_STATS=1` counters then measured the exact-shadow applicability in the one permitted
live run. The established native save/route exited cleanly at 33,000/33,000 vblanks in 593.7 seconds
(55.6 guest vblanks/s) and produced 49 private captures. The last periodic sample at vblank 32,948
reported 21 texture invalidations, 5 shadow checks, 0 hits, 5 misses, 655,360 compared bytes, 0 avoided
decodes/uploads and 18 required decodes/uploads. Between the sample immediately before the capture window
(vblank 29,835) and the last sample, active setup/play added 2 invalidations and 2 required uploads but
no checks or hits. The whole-route checked-hit rate was therefore 0%; the active-window hit rate is not
defined because no check occurred. Direct inspection of nine frames showed normal-sized players, court,
HUD, ball and serve/bounce markers. This is applicability and visual evidence only: no gameplay A/B or
gameplay speedup percentage is claimed.

## Closed `ge_run_list()` hierarchy after triangle-strip reuse

Commit `78508d219c2c59bcad3e8e4bd6aadb7625207702` (triangle-strip reuse) is landed on `main`.
The follow-up profiler duplicates the existing exclusive renderer phase totals only while a primitive
GPU hook is active and separately attributes hook-local queue-submit and wait time. It adds no clock
call to the existing renderer timers. The replay emits a strict, machine-readable hierarchy whose
parent/child equalities are parser-tested; list residual is not inferred from overlapping totals.

Three independent 200-frame processes measured profiler overhead on the same binary:

| Mode | Run 1 | Run 2 | Run 3 | Mean ms/frame | Range | Overhead vs off |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| profiling off | 12.220 | 12.302 | 12.061 | 12.194 | 0.241 | - |
| GE hierarchy only | 12.813 | 12.783 | 12.589 | 12.728 | 0.224 | 4.377% |
| GE + renderer hierarchy | 13.803 | 13.770 | 13.701 | 13.758 | 0.103 | 12.823% |

GE-only meets the preferred 5% ceiling. The combined profile is diagnostic only; unprofiled processes
remain the source for optimization A/B conclusions. The combined runs reconcile `ge_run_list()` as:

| Non-overlapping category | ms/frame |
| --- | ---: |
| command/non-primitive dispatch | 0.305 |
| primitive GE/frontend excluding GPU hook | 3.507 |
| GPU hook total | 9.070 |
| block transfer | 0.000 |
| CLUT load | 0.084 |
| explicit flush/list control | 0.000 |
| list residual | 0.000 |
| **list total** | **12.967** |

The 9.070 ms GPU hook then reconciles to 4.948 ms in existing exclusive renderer phases, 1.343 ms in
queue submission, 0.098 ms in waits and **2.681 ms renderer/backend residual**. Thus the historical
approximately 3.3 ms unknown is case B: inside the GPU hook but outside the existing renderer phases;
it is not command dispatch, GE-list residual or double-counting. The largest existing hook phases were
object lookup 1.356, snapshot-region recording 0.902, command recording 0.813, vertex preparation
0.772 and state preparation 0.645 ms/frame.

Count-only probes avoided more high-frequency clock calls. Per frame, `begin_target()` ran 12,717
times (12,716 fast hits, one acquisition), `ensure_room()` ran 12,355 times with zero flushes, and
`append()` ran 12,355 times with 11,996 full comparisons and 11,995 merges. This localizes the residual
to hot hook scaffolding such as target/room/batch checks and comparisons plus uncharged API/stat work,
without pretending counts establish time cost.

A single reversible state-serial experiment replaced those repeated full `append()` comparisons when
the complete cached batch template was identical; `SR_GPU_BATCH_SERIAL_DISABLE=1` selected the exact old
comparison. The diagnostic counter moved full comparisons from 11,996 to 1/frame and reported 11,995
serial merges/frame, but the required unprofiled same-binary A/B did not clear the gate:

| Mode | Run 1 | Run 2 | Run 3 | Mean ms/frame | Range | Submits/frame | Output SHA-256 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| exact full comparison | 14.931 | 14.685 | 14.731 | 14.782 | 0.246 | 90.95 | `81C0294C...37A0B` |
| state serial | 14.796 | 14.445 | 14.537 | 14.593 | 0.351 | 90.95 | `81C0294C...37A0B` |

The apparent 1.284% reduction was below the normal 3% threshold and within run variance, so the
experiment and switch were removed; no optimization was promoted. Per-helper timers were also rejected
after inflating a representative combined-profile run from about 16.3 to 19.4 ms/frame. WPR sampling
was unavailable under the current Windows system-performance policy (`0xc5585011`), and MinGW `gprof`
produced call counts but no usable PC samples. None changed production behavior.

The final profiling-only tree retained Vulkan SHA-256
`81C0294C7BF7DDCEF197BEFCE9086CFECD9791092ABC55D72E8FE36E58337A0B` in default,
strip-disabled, texture-shadow-disabled, synchronous-submit and transfer-ring-disabled modes. Software
retained `A8897DF4B704188A62AA57D7A3D30B9DE72FA962893D216338B67AEB4D4DB612`. Vulkan core plus
synchronization validation, using the current `VK_LAYER_VALIDATE_SYNC=1` setting, reported zero warning,
error, VUID or hazard in every Vulkan mode. The deprecated validation setting was discarded after it
emitted a settings warning. No gameplay run was made.

## Sampled vertex profiler rejection

Bounded sampled vertex profiling was rejected because observer effect remained ~25–28%;
sub-phase ranking is not decision-grade. Coarse GE hierarchy remains acceptable.

The largest still-unsplit exclusive bucket remains the 3.507 ms/frame primitive GE/frontend path.
Do not select optimizations from the rejected sampled ranking. Preserve the 12.194 ms/frame
unprofiled reference. The backend residual remains second at 2.681 ms/frame; the rejected
batch-serial result proves comparison frequency alone is not sufficient reason to optimize it.
Retain every destination copy, barrier, PSP fb16+dither rule and exact fallback.

## Low-overhead primitive-front-end split (measurement-only)

The follow-up `SR_GE_PRIM_PROFILE=1` diagnostic keeps the production GE object at `-O2` and
uses one rotating timer phase per sparse vertex or triangle event. It reports fetch/decode,
transform, lighting, clipping/acceptance and assembly as separate phase samples; it does not
enable the inclusive GE hierarchy or emit per-vertex logs. `SR_GE_PRIM_PROFILE_STRIDE=64` was
used for the phase ranking after an observer-effect check on the same Vulkan replay binary and
private deterministic fixture: ten paired 20-frame processes measured 18.025 ms/frame with
profiling off and 17.837 ms/frame with the split enabled (-1.046%, paired 95% interval
-3.851% to +1.758%). The negative point estimate is noise, not a speedup claim; the upper
confidence bound remains below the 5% observer-effect gate.

The resulting sparse estimates were vertex fetch/decode 1.417, transform 0.795, lighting 1.591,
clipping/acceptance 0.377 and primitive assembly 0.374 ms/frame (sample standard deviations
0.060, 0.028, 0.176, 0.016 and 0.018 ms respectively). These figures are localization evidence,
not an optimization authorization: their sampled sum does not reconcile tightly enough with the
3.507 ms historical coarse frontend bucket to justify a code change. No optimization was
promoted; the next experiment should improve the phase estimator/reconciliation before ranking
an implementation target. Software and Vulkan output hashes remained the canonical
`A8897DF4...` and `81C0294C...` values.

## Primitive-front-end calibration (measurement-only)

`SR_GE_PRIM_CALIBRATION=1` adds two opt-in measurements at the same stride: an outer
`sampled_total` interval around each sampled triangle frontend and an empty control around the
sparse sample-selection/accounting machinery. The production object remains the `ge.o -O2`
build, and no optimization or state-serial experiment was made. On the same private deterministic
Vulkan replay and final binary, 20 paired 20-frame processes (profiling off versus profiling plus
calibration, stride 64) measured:

| Measurement | Mean | Sample SD | 95% paired/mean interval |
| --- | ---: | ---: | ---: |
| profiling off (ms/frame) | 17.348 | 0.359 | — |
| profiling + calibration (ms/frame) | 17.597 | 0.612 | — |
| paired perturbation | +0.249 ms (+1.435%) | 0.436 ms | +0.260% to +2.610% |

The raw five-phase estimates were fetch/decode **1.381**, transform **0.721**, lighting
**1.531**, clipping/acceptance **0.371** and assembly **0.362 ms/frame** (sum **4.366**;
sample SD **0.167**). The outer `sampled_total` estimate was **3.954 ms/frame** (sample SD
**0.098**), so the sum-minus-total difference was **0.412 ms/frame** (sample SD **0.091**;
95% mean interval **0.369–0.454**). The empty control was **0.01877 ms/frame** (sample SD
**0.00043**), about **31.3 ns per selected event** and **0.11%** of wall time; it is too small to
explain the frontend discrepancy.

The control is a separate interval and the calibrated timestamp pair is already removed from every
timed interval, so subtracting the control from phase or total estimates would double-count it.
The reported calibration-adjusted values are therefore explicitly equal to the raw values. The
outer total still exceeds the historical **3.507 ms/frame** bucket by about **0.447 ms/frame**;
the sum exceeds it by about **0.859 ms/frame**. Lighting is the largest current sparse estimate,
but the residual coverage/frame-extrapolation uncertainty and the unreconciled coarse bucket keep
the ranking below the decision-grade threshold. No optimization target is recommended; the next
experiment should reconcile triangle/vertex sampling (including strip-cache reuse and other
primitive types) with the coarse bucket before any runtime change.

The final count-only pass added no timer. With the integer counters enabled, a separate ten-pair
check measured **+1.919%** profiling perturbation (95% paired interval **+0.382% to +3.457%**),
remaining below the 10% gate. The 20-pair calibrated result above remains the authoritative
profiling-overhead estimate.

## Final count-only reconciliation

Issue **#33 remains OPEN**: this branch is a calibrated measurement candidate, not a runtime
optimization.

The final replay pass added only integer population counters; it added no timer and did not alter
the production path. Counts below are totals for the same 20-frame Vulkan replay, followed by the
per-frame population in parentheses:

| Population | Total (per frame) |
| --- | ---: |
| GE primitive commands | 28,340 (1,417) |
| submitted primitives | 260,640 (13,032) |
| submitted triangles (list + strip) | 258,960 (12,948) |
| logical vertex references before strip reuse | 780,240 (39,012) |
| effective vertex-use population after strip reuse | 497,480 (24,874) |
| actual decoded vertices | 497,480 (24,874) |
| actual transformed vertices | 494,120 (24,706) |
| through-mode vertices | 3,360 (168) |
| transform-mode vertices | 494,120 (24,706) |
| strip-cache commands / reuse hits | 20,580 / 282,760 (1,029 / 14,138) |
| non-triangle primitives (sprites) | 1,680 (84) |

By command type, type 3 lists were 6,220 commands / 97,000 triangles (311 / 4,850 per frame),
type 4 strips were 20,580 / 161,960 (1,029 / 8,098 per frame), and type 6 sprites were 1,540 /
1,680 (77 / 84 per frame). Types 0, 1, 2, 5 and 7 were absent. There were no patches, points,
lines or non-triangle vertex rejects. Strip reuse reduced the no-cache vertex-use population from
39,012 to 24,874 per frame.

The transform-triangle outcomes were 245,460 drawn (12,273/frame), 280 clipped (14/frame) and
13,220 early-rejected (661/frame). The sparse populations were fetch/decode 2,572 calls / 497,480
eligible vertices, transform 2,572 / 494,120, lighting 2,571 / 493,720, clipping 2,024 / 258,960
and assembly 1,926 / 258,960; the outer total was 4,047 / 258,960. Assembly has fewer calls because
early-rejected triangles do not reach assembly, while its denominator remains all candidates.

These counts rule out a large hidden population mismatch in the current replay: the logical vertex
reference population is 780,240, strip-cache reuse reduces it to the 497,480 effective fetch/decode
events used by the sparse denominators, and actual decoded/transformed counts match those effective
populations. Non-triangle work is 0.43% of logical references (0.68% of effective vertex uses). The
old coarse `primitive` timer wraps the complete GE_PRIM draw (plus patch commands, of which
this replay has none), including draw setup, sprites and rejects; the sampled outer total covers
triangle frontend work before raster/GPU submission. Thus non-triangles cannot explain the 0.447
ms/frame gap to the historical 3.507 ms reference, and the 5.1% reject population is represented in
both complete-draw paths. The remaining explanation is a combination of current-main behavior versus
the historical run and estimator composition: the phase sum includes independently extrapolated
vertex/triangle samples, while the outer total uses one triangle denominator. Count-only evidence
cannot separate those two causes, so the residual remains unresolved rather than being forced away.
In the requested attribution terms: current-main drift is plausible but unproven; a large denominator
mismatch is not supported; work included only by the sampled total is not supported (the direction is
opposite, and patches are absent); and the remaining **0.447 ms/frame** is unresolved estimator or
historical-composition bias.
