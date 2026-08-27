# Genericity census — second-platform campaign (2026-08-26)

Source-level census of main (`af5c4f4`, 2026-08-26) for behavior that makes the
generic runtime depend implicitly on one title, plus the portability boundary
map measured during the same campaign. Revalidated against main (`1c672f5`,
2026-08-27) after #131/#133/#134/#135/#136: every title-coupling row below was
re-confirmed against live source, and the two interpreter-floor finding rows
from the original campaign are retracted — the gap fixture was malformed
fixture-side, not a runtime store-commitment defect. Dated evidence record; the
live source remains authoritative.

## Title-coupling census

| # | Location | Observation | Classification |
| --- | --- | --- | --- |
| 1 | `src/rt/recomp.c` `g_exact_hooks[]` | Exact-address dispatch hooks keyed to retail-title guest addresses, including `INIT_LANG` which writes a JP-language flag byte into guest memory | COMPATIBILITY_PROFILE_REQUIRED |
| 2 | `src/rt/recomp.c` INIT_WALKER_GUARD | Behavioral r16 save/restore around dispatches originating at two exact caller PCs | COMPATIBILITY_PROFILE_REQUIRED |
| 3 | `src/rt/recomp.c` inline diagnostics | Several log paths keyed to exact PCs/address ranges of one title's walker functions | DEBUG_ONLY |
| 4 | `src/rt/recomp.c` VFPU fallback diagnostic | Reads a title font-table pointer from a hardcoded guest address inside the generic fallback path | DEBUG_ONLY (title-shaped read) |
| 5 | `src/rt/hle.c` `populate_known_module` | Hardcoded `place_game_here/.../libfont.prx` and `psmf*.prx` host paths and load bases on the module-load/late-import path | TITLE_CONFIGURATION_REQUIRED |
| 6 | `src/rt/hle.c` data-root default | `SR_DATAROOT` defaults to an executable-relative `place_game_here/.../xbdata_extracted` tree and is scanned at HLE init even for guests that never open an XB-served path | TITLE_CONFIGURATION_REQUIRED |
| 7 | `Makefile` + `hle.c` | `SR_DATA_EXPECTED_COUNT=56672` bound only under `GAME_NAME=hst` | TITLE_CONFIGURATION_REQUIRED (properly gated) |
| 8 | `tools/codegen.py` + `tools/host_stubs.py` | Retail semantic stubs, custom bodies, and abort handling — all gated behind `--profile=hst` | COMPATIBILITY_PROFILE_REQUIRED (properly gated; this is the model) |
| 9 | `src/rt/gui.c` present cap | 30 Hz default motivated by the title's frame cadence; env-overridable host pacing policy | DOCUMENTATION_ONLY |
| 10 | `src/rt/savedata.c` memstick default | PSP-standard storage layout, no title identity | FALSE_POSITIVE (as coupling) |

Target architecture remains: generic PSP core + explicit title configuration +
explicit versioned compatibility profile + evidence/removal criterion. Items
1, 2, 5, and 6 are the remaining silent-title-patch surfaces in generic code.

## New platform findings (this campaign)

| Finding | Evidence | Owner / next step |
| --- | --- | --- |
| `tools/imports.py` rejected legal zero-import modules ("import stub table is empty") | `fixtures/platform_ladder` ladder-zero/ladder-reloc pipeline runs | FIXED here: declared-empty tables now return an empty mapping with a finding |
| ~~Interpreter floor (#118) drops SW commits on the dispatch-miss path~~ RETRACTED (2026-08-27): the original `ladder-gap` workload was malformed fixture-side — its encoders emitted `0x24020000` (v0 cleared in a delay slot), `0x8d480000`, and `0x8dac0000` (loads with wrong base registers) where the documented guest intended other instructions, so the recorded "expected" result was never the intended guest's result. Interpreter canonical memory was always shared with AOT; cross-tier store visibility is independently exercised by #128 cosim and by the repaired `ladder-gap`, which now passes as an ordinary workload with no BLOCKED classification in this ladder | Repaired `fixtures/platform_ladder/generate.py`: intended address -> encoder -> emitted LUI/ADDIU words -> independent decode -> reconstructed effective address -> decoded mid-to-end handoff/load -> executed result; a distinct A guard makes an end-load-to-A mutation fail | Runtime needed no change; #128 remains the standing cross-tier memory evidence |
| Floor form coverage (`src/rt/guest_interp.c`) is broader than first recorded: shifts, hi/lo, mult/multu, addu/subu/and/or/xor/slt/sltu, width-1/2/4 loads and stores, branches including likely/REGIMM forms, j/jal; unsupported forms still fail closed (#118 contract) | Live-source read at `1c672f5` | Dispatch lane owns any further floor expansion; no gap-shaped restriction applies to ladder workloads |

## Portability blocker graph (Linux x86-64, gcc-13 / WSL2 Ubuntu 24.04)

Measured by per-file compile probes recorded under
`build/platform-ladder/portability-linux*.log` (2026-08-26 at `af5c4f4`; not
re-measured at `1c672f5`).

```text
L1 python toolchain ....................... PASS (3.12)
L2 analyzer/prxload/imports/codegen ....... PASS; loader output byte-identical
                                            to Windows (HOST_DIFFERENTIAL,
                                            sha256 9d8ec91b… on ladder-zero)
L3 reference interpreter (g++) ............ PASS (builds and runs)
L4 portable-core objects (recomp, coro,
   guest_interp, vfpu_*, ge, savedata,
   mpeg, h264_null, debug, perf-less …) ... PASS with -DSR_PUBLIC_SAFE
L5 driver.o ............................... PASS
L6 hle.c .................................. BLOCKED: process.h (Win32 header)
L7 sched.c / perf.c ....................... BLOCKED: SDL3 timer headers only
                                            (SDL_GetTicksNS); no semantic dep
L8 gui.c / osk_win.c ...................... BLOCKED: windows.h (GDI/dialog)
L9 sdl3vk/ge_gpu link ..................... BLOCKED: SDL3+Vulkan SDK presence
```

- FIRST_HARD_HOST_BOUNDARY: L6 (`hle.c` process.h), immediately followed by L7.
- SMALLEST_PORTABILITY_ABSTRACTION: a host clock/time shim (monotonic
  nanoseconds; `clock_gettime` on POSIX vs SDL on Windows) replacing direct
  SDL_timer includes in `sched.c`/`perf.c`, plus moving `hle.c`'s
  `process.h` usage behind the existing platform seams. With those two moves,
  layers through L7 become Linux-compilable without touching any PSP
  semantics. GUI/presentation split is already covered by
  [PLATFORM_PORTABILITY.md](../PLATFORM_PORTABILITY.md).
- AARCH64/host-FP note: `src/rt/fp_convert.h` documents the x86/x64 SSE2
  scoped-environment boundary and fails closed elsewhere; FPCR backend stays
  recorded follow-up work.

## Second-retail-title readiness snapshot

GREEN: loader/relocation, codegen profile isolation, scheduler + ThreadMan
HLE basics, scalar-FPU contract, IoFileMgr VFS route, build-truth stamps,
provenance shape for synthetic fixtures.

YELLOW: title-config
coverage for exact hooks/walker guards (items 1–2 still live in generic
dispatch), data-root ownership (item 6), import-library coverage breadth.

RED: none newly identified beyond known graphics/audio acceptance domains,
which remain private-title/hardware evidence lanes rather than genericity
debt.
