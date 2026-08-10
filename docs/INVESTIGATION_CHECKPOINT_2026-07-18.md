# Investigation checkpoint — 2026-07-18

> **Snapshot, not the live issue tracker.** This document records the evidence and conclusions
> reached before the next strict runtime experiment. [`ISSUES.md`](../ISSUES.md) remains the
> authoritative list of current priorities.

## Executive summary

Nakagawa Recomp now builds a native Windows executable and reaches substantial parts of Hot
Shots Tennis: the title/menu flow, Story Mode, character selection, and several audio paths.
That progress is real, but the game is not release-ready or broadly playable yet.

The most important current conclusions are:

1. **Correctness must remain ahead of performance.** The earlier UFL record-vector
   use-after-free is root-caused, repaired, and independently revalidated, but severe heap
   fragmentation, incomplete movie playback, rendering defects, and route-specific stalls
   remain. Profiling those broken routes would optimize unstable behavior.
2. **The latest full build repaired one genuine static translation omission.** Ghidra proved
   `0x00002688` is real code called by `0x00002408`. The generator no longer discards it. The
   2026-07-18 full build emitted 14,379 functions, reported zero fallbacks, linked successfully,
   and contains `f_00002688`.
3. **Heap exhaustion is currently fragmentation, not a simple lack of free bytes.** Runs have
   failed allocations while 15–25 MiB remained free because the largest individual free block
   was too small. Coalescing may eventually be appropriate, but it is a separate allocator
   improvement—not the repair for the now-resolved UFL lifetime bug.
4. **The Story Mode visuals are not explained by a missing extraction root.** The runtime
   indexed 58,498 extracted files from the canonical private data tree. Some requested optional
   files are genuinely absent, but primary lobby archives and textures exist. Allocation
   failures and graphics-state correctness are stronger leads.
5. **The first-save freeze occurs before the savedata HLE state machine is entered.** The
   confirmation dialog appears, but that run contains no `sceUtilitySavedataInitStart` event.
   The savedata implementation is therefore not yet the proven blocker.
6. **The Exhibition stall reaches a real asynchronous character/resource loader.** The route
   creates thread entry `0x00048f64`, which invokes a worker function stored at `0x00311144`.
   The final trace formats a `game/400_pc/...xb` character resource. The exact worker and stop
   PC still need a strict post-build reproduction.
7. **Publication preparation is much stronger, but the local Git object database remains a
   blocker.** The working tree has no commits, tracked files, branches, or remotes, yet `.git`
   still contains unreachable historical objects. Reinitializing `.git` before the first
   commit is the clearest cleanup, but it is intentionally deferred because it is a destructive
   metadata operation that should be done explicitly.

## Runtime evidence

### What visibly works

- The native SDL3/Vulkan window opens at 960×544 on the NVIDIA GeForce RTX 3080.
- The Vulkan GE backend initializes and presents game-rendered frames.
- A connected controller is detected.
- Title and menu navigation work far enough to reach Story Mode and Exhibition selection.
- Zeta appears, speaks a time-of-day greeting, and returns to a stable model after a brief
  initial glitch.
- Voice clips and many interface/tennis sound effects play.
- The game can display Story Mode NPCs and parts of a lobby scene.
- The Story Mode save confirmation dialog is rendered and accepts navigation to the prompt.

These observations are useful reachability evidence; they are not proof that the corresponding
subsystems are complete or semantically correct.

### Current visible failures

| Route | Confirmed observation | Current classification |
| --- | --- | --- |
| Main menu | No background music | Audio resource/streaming path incomplete |
| Main menu return | Club interior disappears; court background and a black lower band remain | Scene/resource retention or GE target/state issue |
| Options | Repeated or misplaced `NOW LOADING` graphics and broken layout | Sprite/texture/state corruption |
| Intro movie | Black or duplicated-menu frame; manual playback becomes unresponsive | Known incomplete PSMF player path |
| Story Mode | White/textureless buildings and black void floor | Missing retained resources, failed allocations, or texture/render state |
| Story movement | Character polygons intermittently morph | Vertex/VFPU/GE synchronization or corrupted resource data; not yet isolated |
| Story messages | Overhead bubbles appear as garbled icons | Sprite atlas, CLUT, texture cache, or corrupt resource state |
| First save | Freezes after confirming `YES` | Guest/UI path before savedata HLE |
| Exhibition | `Exhibition → Singles → Nov → opponent -> character select` remains on `NOW LOADING` | Character/resource loader frontier |

### Audio

The SDL3 audio stream opens successfully at 44.1 kHz stereo signed 16-bit. Audible voices,
swings, selection sounds, and other effects prove that audio is not globally broken.

The remaining audio problem is narrower:

- No music was heard during the reported menu routes.
- Logs still contain named-resource lookup failures such as `unknown name`.
- A prior trace mentioned a referenced VAG asset that was not found as an extracted standalone
  file.

The next audio investigation should trace the resource registry from archive load, through name
insertion, to lookup. It should not replace the working SDL stream or guess missing names in HLE.

### Intro movie

The movie failure has a direct implementation explanation. The high-level
`scePsmfPlayerGetVideoData` and `scePsmfPlayerGetAudioData` paths currently return
`PSMF_ERR_NO_DATA`. A lower-level MPEG demuxer exists, but it is connected only to `sceMpeg*`
calls, not to those PSMF player getters.

Consequences:

- A black title/attract movie is expected with the current implementation.
- Repeatedly waiting on the current manual movie screen will not produce a decoded movie.
- Fixing the route means connecting the real demux/decode state to the PSMF player API, not
  returning a fabricated success.

### Story Mode data and rendering

The canonical data root is working:

```text
place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted
```

The runtime indexed **58,498 files** there. Main `L01` lobby data and textures are present in the
extracted archive tree. Several logged requests do not appear in the extraction or inventory,
including:

- `data/common/shadow/shadow_07.gim`;
- some optional `L01_hiyoko` motion/collision files;
- `lobby01/npc10.esd` and `lobby01/npc11.esd`.

Those misses may be optional/fallback requests. They do not establish that the extraction root
is wrong. The white buildings and black floor correlate more strongly with large failed
allocations and possibly retained-target/texture-state errors.

The correct differential test remains:

1. reproduce the same Story frame after the heap lifetime issue is fixed;
2. capture it through the Vulkan path;
3. capture the same frame through the software reference rasterizer;
4. only classify it as a Vulkan bug if software output is materially correct.

### First-save freeze

The run reached the visible prompt:

```text
About to save. OK?
YES    NO
```

It did **not** log `sceUtilitySavedataInitStart`, `sceUtilitySavedataUpdate`, or another
savedata-HLE transition before the process was closed. The safest current conclusion is:

> The observed freeze is before the savedata HLE boundary, probably in guest UI/resource/heap
> processing. The savedata state machine is not yet exonerated for later phases, but it is not
> the demonstrated first blocker.

A deterministic replay was preserved locally as `logs/story_save_replay.pad` with 142 recorded
input events. It is diagnostic evidence and is intentionally Git-ignored.

### Exhibition loading stall

The preserved log is:

```text
logs/exhibition_stall_20260718-161627.log
```

The relevant guest flow, confirmed with Ghidra, is:

```text
caller
  → FUN_00048fc8(worker, argument)
      → create thread at 0x00048f64
      → store worker at 0x00311144
      → copy the four-byte argument block to the new thread stack
      → start thread
          → initialize VFPU constants
          → lock resource semaphore
          → call worker(argument)
          → unlock
          → exit thread
```

The run created UID `0x13a` at entry `0x00048f64`. Its final diagnostic was a formatter
processing `%s.xb`. Address `0x002e06ad` lies inside:

```text
game/400_pc/%s/%s%03d%s.xb
```

Ghidra shows the likely Exhibition character workers at:

- `0x0027028c`;
- `0x002704c0`;
- `0x002705fc`;
- `0x00270738`.

Those functions allocate a character/resource object, format the `game/400_pc` path, and invoke
the resource loader. The log ended too soon to identify which worker was stored at
`0x00311144` or where it stopped. The next reproduction must be allowed to reach the 10- or
20-second watchdog dump with strict dispatch and thread-argument logging enabled.

## Static translation and Ghidra findings

### Ghidra setup

The developer-only Ghidra workflow is installed and validated with:

- Ghidra 12.1;
- [`kotcrab/ghidra-allegrex`](https://github.com/kotcrab/ghidra-allegrex);
- loader `PspElfLoader`;
- processor `Allegrex:LE:32:default`.

`python tools/ghidra_headless.py validate` passed the clean import check. The headless logs also
show warnings from an unrelated local `GhidraMCP` extension whose `Module.manifest` uses invalid
lines. Those warnings did not prevent Allegrex validation or decompilation, but that extension
should be repaired or removed from the local Ghidra profile later to keep analysis logs clean.

### The `0x00002688` omission

Ghidra proved:

- `0x00002688` has a normal function prologue;
- `0x00002408` calls it directly;
- it parses/skips compact encoded metadata using three calls to `0x00001040`;
- `tools/codegen.py` explicitly discarded it without a defensible reason.

The generator discard was removed and a regression test now rejects its reintroduction.

Verification completed before this checkpoint:

- `tools/test_codegen_no_shadow_stubs.py`: 3/3 passed;
- `tools/test_codegen_retail_allocator.py`: 2/2 passed;
- canonical `BuildFull`: passed;
- generated functions: 14,379;
- generated fallbacks: 0;
- `f_00002688` present in `hst_recomp_funcs.h`;
- every generated chunk object is newer than its source.

The repaired path has now also completed a strict 60-second headless run through the full
58,498-file index and a display flip with zero dispatch misses or generic telemetry failures.
The rebuilt executable has **not yet received a fresh GUI route acceptance test**.

### Interpreting completion

This project is a **static recompiler**, not a conventional source decompilation. A single
“decompilation percentage” mixes unrelated work and is misleading.

The last structural comparison found:

- 13,595 analyzer-discovered functions in the base game;
- one missing intended base function, `0x00002688`;
- additional generated functions from the three private PRX inputs.

The new build contains the repaired function and zero fallbacks. A fresh automated
analyzer/generated intersection still needs to be recorded before labeling intended static
translation coverage as exactly 100%.

The prior HLE snapshot found:

- 259 imported NIDs;
- 209 registered handlers, about 80.7%;
- 185 nontrivial handlers, about 71.4%.

Those percentages measure registry coverage, not correctness. Runtime compatibility must remain
a separate route matrix covering boot, movies, menus, Story, Exhibition, gameplay, audio,
savedata, and clean shutdown.

## Guest heap findings

### UFL vector use-after-free: root cause and repair

The complete archived and newly revalidated chain is:

1. Worker `0x115` parses `disc0:/PSP_GAME/USRDIR/umd.ufl` into a vector of 16-byte records.
2. The vector-growth routine at `0x00048a24` calls allocation helper `0x00048dd0`; the live
   allocation site is `0x00048e18`. Capacity grows by roughly 60%:
   `21 → 34 → 55 → 88 → 141 → 226 → 362 → 579 → 927 → 1483 → 2373`.
3. The old buffer is released through vector cleanup at `0x00048d9c` only after the replacement
   has been allocated and existing records copied.
4. In the broken lifecycle, UMD HLE readiness banked two wake tokens for launcher thread
   `0x111`. Its first sleep returned immediately and global destruction freed the active UFL
   vector while worker `0x115` still owned it.
5. The worker's record stores at `0x00048014..0x00048020` continued through the old buffer. The
   field-two store at `0x0004801c` eventually replaced the split free-block header at
   `0x0a0657a8` with `0x00025136/0x000002c7`, producing
   `HEAP_FREE_LIST_CORRUPT`.

The repair removes launcher wakeups from UMD readiness paths. Readiness now wakes UMD-object
waiters and dispatches the UMD callback without waking the launcher or starting global teardown.
This is an HLE lifecycle correction, not an allocator workaround.

The new provenance run independently confirmed the normal vector sequence:

- allocation call site `0x00048e18`;
- release call site `0x00048d9c`;
- grow/copy/swap owner `0x00048a24`;
- replacement allocation precedes every old-buffer release;
- a strict 60-second run indexed all 58,498 files and reached a display flip;
- zero `HEAP_HEADER_WRITE`, `HEAP_FREE_LIST_CORRUPT`, `HEAP_SMASH`, `HEAP_ALLOC` failure,
  or dispatch miss occurred.

The `0x00002688` function-discovery repair is a separate correctness fix. The pre-repair
generator emitted calls to it as unresolved dynamic dispatches; the function's returned metadata
cursor is consumed by `0x00002408`. It must remain translated, but it was not the cause of the
UMD launcher lifetime bug.

### Why allocator redesign remains separate

The lifetime violation is resolved without moving metadata or weakening frees. Separate runs
still prove significant external fragmentation, so coalescing may be useful after route
correctness is restored. It should be evaluated as an allocator-capacity improvement with its
own tests, not presented as the UFL fix.

### Diagnostic plumbing added

The host allocator bridge now forwards the guest return address to:

- `sr_newlib_malloc`;
- `sr_newlib_free`;
- `sr_newlib_memalign`;
- `sr_newlib_realloc`.

Opt-in allocation diagnostics now include:

- call site (`guest_ra - 8`);
- return address;
- thread UID;
- allocation source and size;
- valid free provenance;
- foreign/interior free provenance.

For public `malloc`/`free` calls, diagnostics unwrap the exact verified retail wrapper frame.
For the known C++ `operator new`/`operator delete` paths they also unwrap the second verified
frame, making the actual container allocation and release sites visible while leaving every
other call path and all guest state unchanged.

`SR_HEAP_WATCH=1` records the first write to each dynamically freed allocator header with the
writer PC, return address, width, value, and thread UID. This changes diagnostics only; it does
not alter guest lifetime or add a workaround.

### Fragmentation

Separate Story/menu runs showed valid heap metadata but severe external fragmentation:

| Request | Total free bytes | Largest free block | Result |
| --- | --- | --- | --- |
| `0x40040` | `0xf06a50` | `0x3afb0` | Failed |
| `0x20040` | `0xf11260` | `0x1fee0` | Failed |
| `0x100040` | `0x19910e0` | `0x92d00` | Failed |
| `0x404d0` | roughly `0x18xxxx0` | about `0x40050` | Repeatedly failed |

Thus the runtime can have roughly 24–25 MiB free and still fail a 256 KiB allocation. This
explains why later textures or model resources may disappear, but the premature free/stale
writer remains the first root cause to resolve.

## Build, private inputs, and local data

The canonical private layout is documented in [`SETUP.md`](SETUP.md):

```text
place_game_here/
├── EBOOT.elf
├── ISO/<game>.iso
└── EXTRACTED/
    ├── decrypted/
    │   ├── libfont.prx
    │   ├── scePsmf_library.prx
    │   └── scePsmfP_library.prx
    └── PSP_GAME/
        ├── SYSDIR/EBOOT.BIN
        └── USRDIR/xbdata_extracted/
```

Root-level `eboot.elf` and `game.iso` are legacy fallbacks. `original_game/` is no longer
required when the canonical layout is present.

The large private `GAMEDATA.BDL`:

- is not needed to compile;
- is not required for a clean first-run path;
- is needed by current preloaded/menu routes that read the installed PGD cache;
- currently maps through ignored `fs/ms0__PSP_SAVEDATA_UCUS98701GAMEDATA_GAMEDATA.BDL`;
- may be hard-linked to a `memstick/` copy to avoid storing the bytes twice.

The `fs/` folder cannot be removed yet. Generic `ms0:` file I/O still uses its flat mapping,
while utility savedata uses the hierarchical `memstick/PSP/SAVEDATA/...` tree. Unifying those
paths is future portability work.

## Cleanup and repository preparation

### Completed local cleanup

Old caches, recovery material, scratch output, and diagnostic captures were moved under the
ignored:

```text
Archive/pre-cleanup-2026-07-18/
```

That includes the old recovery snapshots and the pre-fix UAF logs. Private game inputs, current
diagnostics, and generated build outputs remain ignored.

### Corrected audit conclusions

The supplied legal/redundancy audit was useful as an idea list, but several deletion or
performance recommendations were stale:

- keep `funcdiff.c` and `funcdiff_cmp.py` as differential-debugging tools;
- keep `font/ltn0.pgf`; it is a real Latin fallback;
- keep the active font fallback code;
- keep the small per-asset provenance READMEs;
- do not vendor optional `libxb` merely for convenience;
- preserve upstream attribution even if the repository begins with fresh Git history;
- do not replace the hash/L1 dispatcher with an obsolete binary-search proposal;
- do not enable LTO, raise generated chunks above `-O0`, or resize chunks before profiling;
- do not put proprietary `GAMEDATA.BDL` in public CI.

### Publication safeguards now present

- GPL-2.0-or-later `LICENSE`;
- expanded `NOTICE.md`;
- `THIRD_PARTY_LICENSES/PSPSDK.txt`;
- `THIRD_PARTY_LICENSES/SHADCN_UI.txt`;
- `SECURITY.md`, `CONTRIBUTING.md`, and community templates;
- pinned GitHub Actions with minimal `contents: read` permissions;
- Linux host-neutral and synthetic Windows runtime compile gates;
- Python, reference-interpreter, codegen, visual-regression, and dashboard CI gates;
- Dependabot configuration;
- pre-commit hygiene, publication audit, Ruff, Markdownlint, mypy, and Gitleaks;
- explicit game-input, generated-output, logs, dump, cache, and secret exclusions;
- private PGD version-key tests now require `HST_PGD_VKEY_HEX` and local data rather than
  embedding the title-specific value;
- an OpenSSF OSPS Level 1 evidence matrix in [`OSPS_BASELINE.md`](OSPS_BASELINE.md);
- an AI information/provenance policy in [`AI_USAGE.md`](AI_USAGE.md).

### Remaining first-publication blockers

1. **Reinitialize or otherwise sanitize `.git`.** The repository has an unborn `main`, zero
   tracked files, zero commits, and zero remotes, but its object database still contains 419
   loose objects and 1,388 packed objects from prior local history. No first push should happen
   from that database.
2. **Stage from an allowlist.** Do not begin with `git add .`. Inspect the exact staged file
   names and sizes, then run the publication audit and Gitleaks over that set.
3. **Configure the remote controls after repository creation.** Protect `main`, require CI,
   enable private vulnerability reporting and secret scanning/push protection where available,
   and require MFA for maintainers.
4. **Obtain focused legal advice if distributing the PGD implementation.** Documentation can
   describe interoperability goals, but it cannot declare DMCA or jurisdiction-wide legal
   compliance.
5. **Do not publish binaries yet.** Release signing, SBOMs, provenance, immutable release
   policy, and artifact review should be added only after runtime correctness and a hosted
   release pipeline exist.

No commit, staging action, remote creation, push, or GitHub publication has been performed.

## OpenSSF and supply-chain guidance

The reviewed sources apply as follows:

| Source | Best implementation for this project |
| --- | --- |
| OpenSSF Best Practices Badge | Use as a post-publication evidence checklist; do not claim a badge before public evidence and sustainable response commitments exist. |
| OpenSSF best-practices index | Use it to select relevant controls, not as a requirement to adopt every OpenSSF project. |
| Compiler hardening guide | Add supported warning/hardening flags incrementally to handwritten runtime code. Preserve the generated `-O0 -w` build constraint and test MinGW/Linux flags separately. |
| Sigstore | Sign or attest official hosted-CI release artifacts, not ordinary local builds. |
| SLSA | Produce hosted build provenance for the first real binary release; strengthen isolation later. |
| Zarf | Not applicable to a desktop game runtime; it targets Kubernetes/air-gapped deployment bundles. |
| OpenSSF public policy | Monitor as policy context, not as a technical control or legal certification. |
| OSPS Baseline | Highest-value immediate framework; the Level 1 evidence matrix is now checked in. |
| AI/ML Security group | Supports the repository's AI information-boundary and human-review policy; this project is not itself an ML model supply chain. |
| Security Baseline talk | Maintainer education; it does not add requirements beyond the written baseline. |
| gittuf | Defer until multiple maintainers or valuable signed releases justify independent policy/threshold approval. |
| Package-repository principles | Borrow MFA, recovery, CI isolation, provenance, and release-integrity practices; this project is not a package registry. |
| Attestations style guide | Explain that provenance links source to a build; it does not certify safety or correctness. |
| Package deletion policies | Treat release artifacts as immutable; supersede or deprecate bad releases instead of silently replacing them. |
| Trusted publishers | Use OIDC instead of long-lived publishing tokens only if a future npm, PyPI, Homebrew, or similar publication exists. |
| Registry build provenance | Distinguish hosted, attested builds from untrusted local builds. |
| Homebrew signing proposal | Relevant only to a future formula/cask; attest the final archive digest and filename. |
| Securing Software Repositories WG | Reference/watch source; there is no component to integrate directly. |

## Dashboard and documentation

The dashboard is separate from the core runtime. Work completed before this checkpoint included:

- cleaning generated `node_modules`/Next output before a fresh install;
- preserving `outputFileTracingRoot` so production standalone output does not copy the entire
  repository;
- testing PowerShell argument handling and debug-console contracts;
- adding production-build checks that reject proprietary inputs or repository leakage;
- passing the dashboard test, lint, type-check, and production-build gates during the earlier
  validation pass.

One dashboard issue remains: its progress display still presents a misleading
“Decompilation Complete” percentage. It must be replaced with separate static-translation,
HLE-coverage, and route-compatibility measures.

The public documentation has been reorganized for GitHub browsing through
[`docs/README.md`](README.md), with `ISSUES.md` as the only live status document and dated
investigations kept separate from current acceptance criteria.

## Platform portability

The detailed staged plan is in [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md).

Current position:

- **Portable in principle:** most recompiler/guest-memory code, SDL3 audio, SDL3 input,
  Vulkan presentation, portions of savedata/ISO access.
- **Windows-specific:** Win32 fibers, GDI fallback, Media Foundation H.264, Win32 OSK,
  several direct filesystem/time/sleep calls, PowerShell/MSYS2 packaging.
- **POSIX seam exists:** `ucontext`/`mmap` coroutine backend, useful for an initial Linux port.
- **Non-Windows movie gap:** the current null H.264 backend preserves timing but does not decode.
- **Build gap:** Linux has a host-neutral object gate, not a complete linked/running game port.

Recommended order:

1. keep the known Windows build green;
2. extract host filesystem/time/sleep/OSK interfaces;
3. split portable SDL code from GDI;
4. add CMake alongside the current Makefile and prove source parity;
5. complete and visually validate Linux desktop;
6. add Android lifecycle, storage, audio focus, input, decoder, and coroutine backends;
7. treat consoles as later platform-authorized ports with their own graphics and SDK rules.

A platform is not “supported” until it has a clean build, synthetic tests, bounded runtime
smoke test, input, audio, persistent savedata, and visually inspected frames.

## Codex warning context

The user identified the timing as the task approaching their Codex session/weekly usage limit.
That is the best available explanation for why the interruption appeared when it did; it was
not evidence that the local runtime investigation had performed an unsafe external action.

The warning text itself referenced additional cybersecurity checks, so the exact product-side
classification cannot be verified from the repository. The actual work remained defensive
correctness debugging of a local game runtime plus pre-publication repository hardening.

To reduce false positives while still helping with low-level diagnosis:

- describe the task as “local emulator/runtime correctness debugging”;
- supply a bounded route log and the three guest addresses to inspect;
- ask for allocation/free/writer provenance rather than exploitability;
- avoid combining the runtime-memory investigation and broad cybersecurity research in one
  prompt;
- keep requests tied to this repository, this executable, and deterministic test evidence.

## Next actions

Work should resume in this order:

1. Reproduce Exhibition long enough to capture the exact worker stored at `0x00311144` and its
   watchdog PC.
2. Re-test Story rendering after the lifetime fix, then compare Vulkan and software output.
3. Re-test first save; only debug the savedata state machine if execution reaches its HLE entry.
4. Build a deterministic strict `boot → single-player match → first rally` route.
5. Profile court loading, a stationary court, and a rally only after that route is correct.
6. Replace the dashboard's single percentage with evidence-based coverage dimensions.
7. Run the full native/Python/dashboard/publication validation set and update `ISSUES.md`.

## Evidence boundary

Local logs, Ghidra exports, private PRXs, the game ELF/ISO, extracted assets, screenshots,
framebuffer captures, and savedata/install data are intentionally excluded from Git. This report
records factual conclusions and independently authored implementation status without publishing
those private inputs or raw decompiler output.
