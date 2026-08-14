# Copilot Instructions for Nakagawa Recomp

Nakagawa Recomp statically translates a user-supplied decrypted PSP ELF/PRX into C, links the
generated translation with a native C runtime, and currently targets *Hot Shots Tennis: Get a
Grip*. `interface/` is a separate Next.js dashboard and is not part of the native runtime build.

## Read these first

Use project information in this order when surfaces disagree:

1. **Source code, tests, and Makefile** for implementation behavior.
2. [`AGENTS.md`](../AGENTS.md) for repository-wide development rules and guardrails.
3. **GitHub Issues** for actionable defects, priorities, and acceptance criteria.
4. [`ISSUES.md`](../ISSUES.md) as a concise linked status dashboard.
5. Maintained `docs/` pages for setup, architecture, debugging, and verification.
6. Dated investigation/history documents only as historical evidence that must be re-verified.

Do not treat ignored/local AI-tool configuration as repository policy.

## Build and test

For HST on Windows/MSYS2 UCRT64, prefer the checked-in manager because it supplies the canonical
private-input paths and HST's required `GAME_BASE=0 GAME_ENTRY=0` values:

```powershell
.\hst_manager.ps1 -Action BuildFull
.\hst_manager.ps1 -Action BuildFast
.\hst_manager.ps1 -Action Test
.\hst_manager.ps1 -Action Run
```

A direct Make invocation for the canonical HST ELF is:

```bash
mingw32-make GAME_NAME=hst GAME_ELF=place_game_here/EBOOT.elf GAME_BASE=0 GAME_ENTRY=0 all
```

Direct Make invocations must export `VULKAN_SDK` (or pass it on the command line); the manager
discovers and validates the current SDK automatically. Both managers require `pwsh` 7.6+; Windows
PowerShell 5.1 is not a supported host (the Makefile compile step itself invokes `pwsh` for
`copy_build_assets.ps1`).

The `all` target is intentionally two-phase: generated chunks are discovered only after the
pipeline has run and Make reparses the build. Do not collapse it into a single dependency pass.

Run checks proportional to the change:

```powershell
python -m unittest discover -s tools -p "test_*.py" -v
python tools/publish_audit.py --tracked-only --worktree --provenance-self-consistency
pre-commit run --all-files
```

`--worktree` reads the bytes on disk; the bare `--tracked-only` form reads staged Git blobs and
will not see an unstaged edit.

The canonical non-interactive aggregate gate is `.\hst_manager.ps1 -Action Verify` (15 steps:
Python unit suite, sched/profiler/heap/asset-index/HLE-thread/FP-conversion/VFPU-table/watchpoints/
VFPU-interp selftests, `src/ref`, `import_audit_gate.py`, `publish_audit.py` over both content
sources, and the two GPU selftests — exit 77 = Vulkan/validation layer unavailable → SKIP). The
suite ends with a machine-checkable `VERIFY_SUMMARY` line naming each subgate's PASS/SKIP/FAIL
status and the private-input gates reported as NOT_RUN.

For dashboard changes:

```powershell
cd interface
npm ci
npm test
npm run lint
npm run typecheck
npm run build
```

External-oracle gates that lack their private inputs are **blocked/unavailable**, not passing.
State exactly what was executed and what could not be executed.

## Private inputs and publication safety

Never commit or publish:

- retail game executables, ISOs/CSOs, decrypted PRXs, firmware modules, or extracted proprietary
  game assets;
- private oracle traces, raw decompiler/Ghidra exports containing game bytes, memory dumps, or
  private framebuffer/reference captures that should remain local;
- title-specific/private keys, credentials, tokens, local databases, or secrets;
- personal/local paths or machine-specific configuration that should not enter public history.

The canonical ignored HST input area is `place_game_here/`. Refer to private inputs by role/path in
issues and PRs; do not attach their contents.

## Generated code

Never hand-edit `build/<game>/<game>_recomp_*.c` or generated object files.

- Change `tools/codegen.py`, analysis/import tooling, or runtime source instead.
- The number of generated chunks is dynamic and depends on discovered function count plus
  `FUNCS_PER_CHUNK`; do not assume HST always has exactly eight chunks.
- Generated translation units intentionally use conservative compile flags.
- `ge.c` has a dedicated `-O2` rule; general runtime objects use the Makefile's `$(CFLAGS)`, whose
  repository default begins at `-O0`.
- After generator changes, perform a full pipeline rebuild and verify no stale generated object is
  linked.

## Correctness rules

### Root-cause fixes only

Do not add loop caps, forced returns, fabricated success, latch-flip hacks, or arbitrary state
writes merely to advance a route. If a required PSP-visible state is absent, identify and implement
the missing behavior.

Address-specific/game-specific compatibility behavior is semantic debt. Any unavoidable override
must have concrete evidence, narrow scope, a regression/route, and a retirement criterion. See
the compatibility-override inventory in `tools/compat_overrides.py`, enforced by
`tools/test_compat_manifest.py`.

### Unknown operations must stay visible

Do not turn an unimplemented NID or dispatch miss into success. Use the project's fatal/default
diagnostic route when establishing correctness evidence and implement/register the real behavior.

### `CpuState` is a load-bearing ABI

The shared state is defined in `src/rt/recomp.h`. There is no separate `lr` member: MIPS `$ra` is
`r[31]`. Coordinate every mirror/consumer and relevant tests whenever the layout changes.

### Scheduler/HLE progress is not proof

Route advancement does not prove PSP kernel fidelity. Callback, mutex, semaphore, async-I/O, wait,
lifetime, timeout, GE, and parser/memory-boundary behavior have dedicated GitHub issues and must be
verified behaviorally.

### Renderer agreement is not a PSP oracle

Software-vs-Vulkan parity is useful for localization/regression testing but does not independently
prove PSP hardware behavior. Use an external PSP/PPSSPP/reference oracle where the claim requires
one.

## Debugging

Use `docs/DEBUGGING.md`, `hst_manager.ps1`, and the implementing source for current diagnostic
switches. Many legacy Boolean `SR_*` variables are enabled by **presence**, so assigning the string
`"0"` can still enable them; unset the variable unless its implementation explicitly parses a
value.

Common controls include:

- `SR_DEBUG=<bitmask>` — centralized debug categories;
- `SR_DISPATCH_FATAL=1` — fail on dispatch misses during verification;
- `SR_GPU_GE=0|1` — software/Vulkan GE path selection;
- `SR_HLELOG=1` — HLE diagnostics;
- `SR_FBSNAP=<N>` — bounded framebuffer snapshots;
- `SR_PADSCRIPT=<file>` — deterministic scripted controller input.

Do not invent or document an `SR_HLE_CONTINUE` switch; it does not exist.

## Architecture entry points

- `tools/prxload.py` — rebase/relocate supported ELF/PRX inputs and produce the flat guest image.
- `tools/imports.py` — import/NID extraction and mapping.
- `tools/analyze.py` / `tools/codegen.py` — function analysis and MIPS-to-C translation.
- `src/rt/recomp.c` — generated-code/runtime dispatch and shared execution support.
- `src/rt/sched.c` — cooperative PSP-thread scheduling, lifecycle, waits, and virtual time.
- `src/rt/hle.c` — NID registry and major PSP kernel/user HLE behavior.
- `src/rt/atrac3p/` + `src/rt/atrac3p_bridge.c` — ATRAC3+ decoder (FFmpeg n4.4-derived,
  LGPL-2.1-or-later) and the HLE bridge behind `sceAtracDecodeData`; compiling `hle.c`
  needs the atrac3p include paths.
- `src/rt/ge.c` — software GE comparison rasterizer.
- `src/rt/gpu_sdl3vk/` — SDL3/Vulkan host/input/rendering path.
- `src/rt/iso.c` — UMD/ISO and host-filesystem mapping support.
- `src/ref/` — separate C++ reference interpreter used for verification/selftests.

See `docs/ARCHITECTURE.md` for the maintained module map.

## GitHub work tracking

Search GitHub Issues before filing or implementing a new defect.

- Put detailed evidence, reproduction, acceptance criteria, and partial-resolution state in the
  canonical issue.
- Keep `ISSUES.md` concise and linked to those issues.
- Do not mark a GUI/runtime issue fixed solely because CI is green or a headless route advances.
- Link PRs to the canonical issue and list exact verification results.

## Licensing and provenance

Preserve SPDX, copyright, and provenance notices. For third-party or materially derived work,
record the exact upstream project/path/revision and preserve applicable notices. Material
AI-assisted translation/reimplementation must not obscure source lineage.

`Jstar269/nakagawa-recomp` is the active sanitized public source repository.
Its publication controls are engineering and provenance gates, not legal
clearance. Residual public-source boundaries include:

- unresolved PGF implementation and replacement-font provenance/distribution review;
- unresolved PGD/amctrl provenance and distribution review; and
- continued exclusion of private title inputs, game-derived output, and private
  engineering evidence.

Do not resolve these by guessing a license, changing SPDX text alone, treating
AI analysis as legal advice, or reconnecting private/pre-sanitization history to
the public repository.

## AI-assisted development

When using Copilot, Claude, or another AI tool:

- keep proprietary/private game inputs and credentials out of public AI contexts;
- review generated changes against source/tests rather than accepting plausible output;
- preserve licensing/provenance boundaries;
- disclose material AI-assisted translation/reimplementation in the PR;
- do not autonomously publish releases, change repository visibility/security settings, rewrite
  history, or merge unverified work.

See `docs/AI_USAGE.md` for the maintained policy.
