# Offline static verification (`codegen.py --static-verify`)

**Added:** 2026-07-11. This is an oracle-free local-consistency check that complements, but does
not replace, the external-oracle verification paths described by the project.

## What it does

`tools/codegen.py --static-verify` runs a compile-time basic-block trace simulation during code
generation and emits local runtime assertions into the recompiled chunks:

1. Every maximal straight-line run inside a basic block is abstractly interpreted over a
   Known/Unknown register lattice (all registers Unknown at each block head, `r0 = 0`, and
   syscalls/unmodeled operations conservatively reset or degrade modeled state).
2. Register values that the static model can determine at the end of a run — for example constant
   materialization (`lui`/`addiu`/`ori` chains) and supported pure ALU folds — become
   `sr_sv_check(s, pc, reg, expected)` calls attached to the run's last instruction.
3. At runtime, disagreement is reported as
   `SV_MISMATCH pc=... rN=... expected=...` on stderr (rate-limited per generated chunk).

A mismatch demonstrates disagreement between the static model's prediction and the executed
translation for that asserted state. It is therefore strong regression evidence for the covered
operation/path, but its root cause still needs diagnosis: a bug in the generated translation, the
static verifier/model, shared state assumptions, or another covered implementation detail can all
produce disagreement.

Conversely, the absence of mismatches proves only that the assertions actually reached during that
run agreed with this static model. It does **not** prove unexecuted paths, unmodeled instructions,
special/excluded functions, PSP hardware behavior, HLE semantics, scheduler behavior, or renderer
correctness.

## Scope and bounds

Historical HST measurements recorded roughly 48.6k emitted assertions across roughly 8.3k
functions for the then-current private ELF, with bounded checks per flush/function and modest
chunk-size growth. Treat those counts as dated measurements rather than fixed properties of later
codegen revisions or different game inputs.

Important exclusions/limits include:

- operations or states the abstract interpreter does not model precisely degrade to Unknown and are
  not asserted;
- functions containing identified hand-injected/special codegen blocks are excluded through the
  generator's `_SV_SPECIAL` handling;
- coverage is execution-dependent: an emitted assertion provides no runtime evidence until its path
  is actually executed;
- the check compares two implementations/representations inside this project, so shared assumptions
  can agree while still differing from PSP hardware.

Default builds do not emit these assertions unless static verification is requested.

## Usage

Use private inputs only from Git-ignored paths. For example:

```powershell
python tools/codegen.py place_game_here/EBOOT.elf build/hst/hst_recomp.c --base=0 --static-verify `
    --extra-elf=...
mingw32-make compile GAME_NAME=hst GAME_ELF=place_game_here/EBOOT.elf GAME_BASE=0 GAME_ENTRY=0
build/hst/hst.exe   # inspect stderr for SV_MISMATCH
```

A clean run means that every **executed assertion** agreed with the static verifier's prediction.
Combine that evidence with the relevant synthetic/reference-interpreter tests and, when available,
an independent PSP/PPSSPP/external oracle before making broader correctness claims.

Unit tests: `tools/test_codegen_static_verify.py`.
