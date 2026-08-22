# Porting to Another PSP Game

This guide walks through adapting the recompiler for a different PSP title.

## Overview

The code generator and much of the runtime are reusable, but this repository still contains
HST-specific addresses, module bases, HLE coverage, and manager defaults. Porting another title
is engineering work, not a supported one-command workflow. The main title-specific inputs are:

1. **Input files** — the decrypted ELF/PRX and ISO
2. **Build variables** — base address, entry point, extra PRXs
3. **HLE coverage and runtime assumptions** — behavior exercised by that title

Everything else (runtime, codegen, GPU backend) is shared.

## Step 1: Choose a versioned public title manifest

Start with a checked-in, versioned manifest under `assets/titles/`. It is the
public source of title semantics: identity, disc/revision policy, executable
base/entry, codegen profile, BSS metadata policy, required module names/load
addresses, and public filesystem/profile requirements. Validate it before using
it:

```powershell
python tools/title_manifest.py assets/titles/synthetic.json
python tools/title_codegen_plan.py assets/titles/hst-ucus98701.json `
  --game-name=hst `
  --game-elf=place_game_here/EBOOT.elf `
  --build-dir=build/hst `
  --module-dir=place_game_here/EXTRACTED/decrypted `
  --psp-header=place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN `
  --manager-plan
```

The manifest is not a storage location for absolute paths, usernames, hashes,
keys, retail bytes, routes, saves, or oracle evidence. Those are private
workspace bindings supplied locally and remain outside Git.

For the privately route-validated HST title, the opt-in manager path is:

```powershell
.\hst_manager.ps1 -Action BuildFull `
  -TitleManifest assets/titles/hst-ucus98701.json
```

The manager accepts only the checked-in HST manifest in this slice. Every
build-facing value comes from the validated plan — the manager keeps no second copy
of the title contract — and it re-checks the manifest's protected digest immediately
before running Make, so a manifest edited after planning fails closed rather than
building half of each contract. `-VulkanSdk`, `-RuntimeOpt`, `-RecompOpt`, and
`-FuncsPerChunk` remain operational overrides. An explicit override wins only where
the contract permits it. Without `-TitleManifest`, the existing HST
discovery/default path is preserved exactly.

`assets/titles/pspdev-phase5.json` is a second, materially different source-owned
fixture (`fixtures/pspdev_phase5`, a standard PSPDEV/PSPSDK `BUILD_PRX=1` module).
It proves the planner is genuinely multi-title: a different load base, no guest
modules, and a different feature surface flow through the same manifest → plan →
codegen path.

The analyzer never applies a title-specific executable span of its own. If your
title needs a code range that lives outside the section table, declare it in the
manifest's `executable.extra_executable_spans` (the manager then supplies it) or
pass `--extra-span=LO,HI` to `codegen.py` for a direct Make build.

## Step 2: Obtain the decrypted ELF

The recompiler needs a **decrypted** PSP ELF (not encrypted PRX). Tools like `pspdecrypt` or `PRXDecrypter` can extract it from the ISO.

Keep the result in a Git-ignored private-input location such as `place_game_here/EBOOT.elf`, or pass its actual path to Make. Do not commit the decrypted game executable.

## Step 3: Determine game-specific variables

You need four values:

| Variable | How to find it |
| ---------- | --------------- |
| `GAME_NAME` | Short identifier (e.g. `hst` for Hot Shots Tennis). Used in build dir names. |
| `GAME_ELF` | Path to the decrypted ELF (e.g. `place_game_here/EBOOT.elf`). |
| `GAME_BASE` | Base address where the ELF loads. Check the ELF header or Ghidra. PSP default: `0x08804000`. Some games use `0`. |
| `GAME_ENTRY` | Entry point address (first PC value). Found in ELF header or Ghidra. |

### Finding the base address and entry point

**From the ELF header** (using `readelf` or a hex editor):

```bash
readelf -h place_game_here/EBOOT.elf | grep "Entry point"
```

**From Ghidra:**

1. Load the decrypted ELF
2. Check the ImageBase and the entry function address

**From PPSSPP source:**
Some games have their base/entry in `Core/Load/PSPELF.cpp` or game-specific config files.

## Step 4: Check for extra PRXs

Some games load additional modules (libraries). HST loads 3 extra PRXs from `place_game_here/EXTRACTED/decrypted/`. Your game may need different ones.

Check the game's `MODULE.SYS` or use `pspdecrypt` to list all PRXs in the ISO.

If your game has extra PRXs, declare their names and load addresses in the
manifest. Keep the decrypted module directory as a local private binding. Do
not edit HST Makefile constants to configure a new title. Each module needs:

- Path to the decrypted PRX
- Load address (base + offset for that module)

## Step 5: Build

```bash
mingw32-make GAME_NAME=mygame GAME_ELF=place_game_here/EBOOT.elf GAME_BASE=0x08804000 GAME_ENTRY=0x08804000
```

The checked-in `hst_manager.ps1` is still an HST-specific orchestration layer,
not a generic title runner. The manifest adapter is read-only and currently
accepts only the checked-in HST manifest; it does not prove runtime portability
or correctness for another title. Use Make directly with that title's validated
manifest and explicit private bindings until a title-specific manager path has
been deliberately added and verified.

The first build can be substantially slower than a runtime-only rebuild because codegen must translate the title's MIPS functions and compile the generated translation units.

## Step 6: Run and observe

Launch the resulting executable with the arguments and private inputs required by the port. If you
have deliberately generalized the manager for the new title, its diagnostic profiles can then be
used as the front end.

For the existing HST configuration, `-Profile Performance` is a log-free visual/audio smoke test
and `-Profile Benchmark` records one-second aggregates plus `logs/perf.csv`; those presets should
not be treated as automatically valid for a new game until reviewed.

Watch `stderr` output for:

- **HLE calls** — `SR_HLELOG=1` traces syscall dispatch
- **Unknown NIDs** — a scheduled runtime run fails fast for an unhandled syscall
- **Stubs** — check `build/<game>/<game>_recomp_stubs.txt` for functions that could not be translated

## Step 7: Fix game-specific issues

### Unknown NIDs

When the recompiler hits a syscall it doesn't know, it stops. You have two options:

1. **Add an HLE handler** in `src/rt/hle.c` — implement the syscall on the host side
2. Add or map the missing NID in the runtime/import tables; unknown calls must not be silently treated as successful.

### Codegen stubs

Check `build/<game>/<game>_recomp_stubs.txt` for functions that generated empty stubs. These are guest PCs that didn't match any known MIPS pattern. Each needs either:

- A codegen fix in `tools/codegen.py`
- A faithful HLE boundary where the original operation is genuinely a system/API call

Do not substitute a fabricated successful return merely to cross the frontier.

### VFPU issues

If VFPU instructions produce wrong results:

- Check `assets/vfpu/*.dat` tables against the pinned upstream provenance documented in the repository
- Compare behavior against an independent PSP/PPSSPP/reference-interpreter oracle where available

### Graphics issues

- **Software renderer** (`SR_GPU_GE=0`): the primary in-repository comparison path, but still under
  correctness validation. Use it for A/B localization, not as an external oracle.
- **Vulkan renderer** (`SR_GPU_GE=1`): faster but may have rendering bugs.
- Capture PPM snapshots with `SR_FBSNAP=<N>` from both and diff them with `tools/ppmdiff.py`.

## Step 8: Extend translation only with evidence

Prefer a general analyzer/codegen correction over an address-specific replacement. If a narrow
translation is truly unavoidable, first disassemble the complete guest function, document its
ABI and side effects, and add a focused generator test. Never use a loop cap, forced return, or
fabricated success value to cross a frontier. Rebuild the full pipeline after any generator edit.

## Checklist

- [ ] Decrypted ELF obtained and kept outside Git history
- [ ] `GAME_BASE` and `GAME_ENTRY` determined
- [ ] Extra PRXs identified (if any)
- [ ] Build succeeds without errors
- [ ] `<game>_recomp_stubs.txt` reviewed — all stubs accounted for
- [ ] Unknown NIDs handled with faithful HLE behavior
- [ ] First HLE call reached (`codegen_gate.py` can verify this when its required inputs are available)
- [ ] Graphics: first frame renders and is validated against an appropriate comparison/oracle path

## Title coupling in the generic core

Nakagawa is a generic PSP static recompiler whose first mature profile happens to be
*Hot Shots Tennis: Get a Grip* (HST). This section records where that distinction does
not currently hold: where generic core (runtime/tooling) carries knowledge that is true
only of one title. It is the second-title readiness record; the machine-enforced
inventory lives in `tools/compat_overrides.py` (`HLE_GUEST_ADDRESS_GROUPS`) and is
gated by `tools/test_compat_manifest.py`. Retiring these entries is tracked by
[issue #98](https://github.com/Jstar269/nakagawa-recomp/issues/98). (This previously
cited #20, which is a merged pull request about `sceSasCore` routing and has no relation
to this surface — so the surface had no tracker at all.)

**Readiness criterion.** A newly supplied, lawfully obtained PSP executable should be
able to receive a profile, run analysis, produce its target/import/capability census,
attempt compilation, and expose its first unsupported semantic boundary **without
title-specific edits to generic core**.

### C-1 — Title guest addresses in `src/rt/hle.c`

Before 2026-08-20 the semantic-debt inventory's checked sources were `tools/codegen.py`
and `src/rt/recomp.c`, with manual groups covering `src/rt/sched.c`. `src/rt/hle.c` was
in none of them. A census found 38 distinct guest addresses across 50 sites in it that
mean something only in this title's memory map, and none were inventoried. They now
are.

That census was itself incomplete, and said so with confidence: it reported 38/38
covered while eight further title addresses sat in
`ensure_runtime_sync_callbacks` — a configuration block base, a semaphore name
pointer, and six guest wrapper entry points, all reached from an unconditionally
registered `sceDisplaySetMode`. Not one of them is written inside a `MEM_*` call: each
is bound to a local or assigned into a `CpuState` register first, so the extractor's
direct-literal regex matched none of them. The gate now also recognizes those indirect
shapes (`bound_local`, `cpu_state_register`); its grammar and its explicit limits are
documented in `tools/test_compat_manifest.py`. Classification summary (census buckets,
per group):

| Group | Bucket | Addresses | Sites |
| --- | --- | --- | --- |
| `guest_bss_snapshots` | DIAGNOSTIC_ONLY | 27 | 30 |
| `exit_path_context` | DIAGNOSTIC_ONLY | 2 | 3 |
| `umd_ufl_head_dump` | DIAGNOSTIC_ONLY | 1 | 3 |
| `libfont_ready_flag` | EXPLICIT_COMPATIBILITY_OVERRIDE | 1 | 1 |
| `frame_ready_latch_assist` | EXPLICIT_COMPATIBILITY_OVERRIDE | 1 | 7 |
| `runtime_sync_callback_config` | EXPLICIT_COMPATIBILITY_OVERRIDE | 8 | 8 |
| `display_setmode_guest_init` | EXPLICIT_COMPATIBILITY_OVERRIDE | 6 | 7 |

`TOTAL_COUPLINGS`: 46 distinct addresses / 59 sites. By bucket:

- `GENERIC_PSP_SEMANTIC`: 0 (generic PSP constants are exempted only through
  the narrow, explicit site rules in `HLE_GENERIC_SITE_RULES`: an exact
  function, shape and literal triple, e.g. the EDRAM base returned by
  `sceGeEdramGetAddr`).
  There is no blanket numeric ceiling and no whole-region VRAM exemption: a
  direct `MEM_R`/`MEM_W` at an arbitrary VRAM address (`0x04000000`..
  `0x041fffff`) is inventoried like any other absolute guest address.
- `PROFILE_OWNED_CONFIGURATION`: 0 inside the hle.c gate; two documented build/profile
  couplings below (C-2, C-3) are this bucket. The eight `runtime_sync_callback_config`
  addresses are this bucket *in shape* — a config base with a fixed field layout, a
  name pointer, and three mode-keyed pairs of wrapper entries — but they are not typed
  configuration today, so they are counted where they actually are.
- `EXPLICIT_COMPATIBILITY_OVERRIDE`: 16 addresses / 23 sites (the four groups above;
  each answers the five review questions in `tools/compat_overrides.py`).
- `DIAGNOSTIC_ONLY`: 30 addresses / 36 sites (read-only, env-gated).
- `PRIVATE_ACCEPTANCE_ONLY`: 0.
- `FALSE_POSITIVE`: 0 (a deliberately injected generic constant never flags; the
  explicit generic-site rules are regression-tested).
- `UNRESOLVED_COUPLING`: 0 (all sites classified; the gate fails closed on any new one).

**Why this blocks title #2.** The sixteen override addresses are not merely wrong for
another title — three of them are *dispatch targets*, and eight more are installed as
guest callback pointers. A different guest executable reaching `h_DisplaySetMode` is
called at whatever lives at `0x00000bcc`, `0x0029a8bc` and `0x0001dc00` in its own map,
has whatever lives at `0x00333138` read *and written*, and ends up with two of the six
wrapper addresses stored where its own code will later call them. The failure is
arbitrary rather than diagnosable, which is the opposite of exposing a clean
unsupported-semantic boundary.

### Top title-#2 blockers

1. `display_setmode_guest_init` dispatch targets (`0x00000bcc`, `0x0029a8bc`,
   `0x0001dc00`) — a generic `sceDisplaySetMode` handler calls three fixed guest
   functions that only exist in this title's map.
2. `runtime_sync_callback_config` (`0x00333138` + seven) — the same handler reads and
   writes a title configuration block, may create a semaphore named through a title
   string pointer, and installs one of three mode-keyed pairs of guest wrapper entry
   points. Newly visible: the coupling gate could not see any of these eight until the
   indirect-shape grammar landed.
3. `display_setmode_guest_init` forced globals (`0x0031fcc0`, `0x00311140`,
   `0x002d0738`) — render-context magic, render-command-table ready flag and context
   word are seeded to this title's expected values.
4. `frame_ready_latch_assist` (`0x00331b80`) — the runtime seeds, decrements, and after
   30 stuck vblanks force-clears this title's frame-ready counter.
5. `libfont_ready_flag` (`0x002d132c`) — any `libfont.prx` load writes 1 to a title
   global from a generic module-load handler.
6. `SR_DATA_EXPECTED_COUNT` in the build driver (`Makefile`, 56672) — a hard-coded
   extracted-asset count for one specific release; the runtime side already defaults to
   "unset is safe", so the constant should move into the title manifest.

The minimal generic interface is a per-title manifest section (`tools/title_manifest.py`
already carries a per-title profile) naming these entry points and globals by role —
allocator, vblank-device creator, render-context initialiser, frame-ready counter — so
the handler stays generic and consults the profile, or reports an unsupported boundary
when the profile is silent. This PR establishes the boundary and evidence; it does not
retire the sites.

### C-2 — `SR_DATA_EXPECTED_COUNT` in the build driver

`Makefile` hard-codes `-DSR_DATA_EXPECTED_COUNT=56672`, the extracted-asset count of one
specific release. `src/rt/hle.c` defaults it to 0 (check disabled) when undefined, so
the runtime side is already generic. Building title #2 requires editing the shared build
driver, and a stale value silently fails the new title's asset index instead of the old
one's. `PROFILE_OWNED_CONFIGURATION` — deferred (touches manifest schema and build
driver together).

### C-3 — Disc ID duplicated outside the manifest

`tools/hst_doctor_core.py` defines `EXPECTED_DISC_ID = "UCUS98701"` while the title
manifest independently validates `disc.id`. Two sources of truth for the same fact.
`PROFILE_OWNED_CONFIGURATION` — deferred (doctor must keep working with no manifest).

### C-4 — One shared scratch stack for every nested guest call

`SR_CALL_GUEST_STACK` (`src/rt/hle.c`) and the equivalent literal in `src/rt/mpeg.c`
give every nested guest call the same fixed guest stack address. Not title-specific by
address, but it assumes a guest map in which that address is free, and it makes nested
callbacks unsafe for any title that nests them. The PSP's real nested-call contract is
`NOT_ESTABLISHED`; a hardware probe is needed before a design can be chosen.
`GENERIC_PSP_SEMANTIC` (open question) — deferred deliberately.

## Reference

- `AGENTS.md` — project conventions and file reference
- `docs/ARCHITECTURE.md` — module-level breakdown of what lives where
- `tools/README.md` — tool-specific documentation
