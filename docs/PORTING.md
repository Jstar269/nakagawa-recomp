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

## Reference

- `AGENTS.md` — project conventions and file reference
- `docs/ARCHITECTURE.md` — module-level breakdown of what lives where
- `tools/README.md` — tool-specific documentation
