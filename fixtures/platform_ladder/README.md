# Second-platform workload ladder

`generate.py` is a source-owned recipe that emits seven positive small PSP-shaped
ELF32 PRX guests (`~PSP` headers included where used), drives each through
the ordinary two-phase production build, and qualifies loader output,
generated chunks, import dispatch, runtime boot events, and a checked result
word. See that module's docstring for the per-workload contract.

## Running

```powershell
mingw32-make platform-ladder            # all workloads + negative control
mingw32-make platform-ladder-zero       # one workload at a time
mingw32-make platform-ladder-fs-negative
mingw32-make platform-ladder-clean
```

Every workload builds with `PUBLIC_SAFE=1`, an empty `TITLE_MANIFEST`, no
`HST_EXTRA_SPANS`, no `GAME_EXTRA_ELFS`, and (at runtime) an explicitly empty
`SR_DATAROOT`, so nothing can inherit any retail title's data tree or
compatibility behavior.

## Ladder

| Workload | Base | Entry offset | Imports | Exercises |
| --- | --- | --- | --- | --- |
| `ladder-zero` | 0x08940000 | +0x40 | none | pure CPU chain, no ~PSP header, no relocs |
| `ladder-reloc` | 0x088C0000 | +0x20 | none | R_MIPS_26/HI16/LO16/R_MIPS_32, fn-pointer jalr, .bss result |
| `ladder-gap` | 0x08A00000 | +0x10 | none | omitted AOT function executed by the interpreter floor |
| `ladder-sched` | 0x08900000 | +0x10 | ThreadManForUser ×6 | thread create/start/exit + event flag handoff |
| `ladder-fpu` | 0x08980000 | +0x08 | none | #120 scalar-FPU contract: RM/FCC0/FS/cvt.w.s |
| `ladder-fs` | 0x089C0000 | +0x18 | IoFileMgrForUser ×3 | SR_FSDIR open/read/close + failure sentinel |
| `ladder-title2` | 0x08A40000 | +0x20 | ThreadManForUser ×9 + IoFileMgrForUser ×3 | real callback/thread/event path + synchronous SR_FSDIR I/O + AOT gap + nine-word result oracle |

`ladder-title2-negative` uses base `0x08A80000` and one deliberately absent
ThreadManForUser NID. Its real import stub must reach the production unknown-HLE
fatal boundary and exit exactly 7; it has no positive result expectations.

Statuses: all seven positive workloads are ordinary PASS results end-to-end; the
ladder-gap result is derived from the intended guest memory round trip. Its
critical address proof follows the emitted bytes: intended address, encoder,
LUI/ADDIU words, independent decode, reconstructed effective address, and the
decoded mid-to-end jump/load seam. A guard value remains in `scratch_A` after
the copy to `scratch_B`, so an end-load-to-A mutation also changes the
executed result.
The former `interpreter-floor-store-commitment` diagnosis was disproven: the
malformed fixture instructions were the cause.

## Provenance

Everything here is project-authored synthetic content. No retail bytes,
firmware binaries, pspautotests sources, or private inputs are used or
referenced. The recipe, not a binary, is the committed artifact.
