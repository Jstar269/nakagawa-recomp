# decomp.me integration (decompilation track)

**Status: forward-looking plan + a first read-only tool.** This is decompilation
infrastructure. It does **not** touch the recompiler (`codegen.py`) or the
runtime. Game-derived output stays outside the public candidate; see
[`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) for the boundary.

## What decomp.me is, and where it fits

[decomp.me](https://decomp.me) is a collaborative decompilation workbench with a
**native PSP platform** (little-endian MIPS `mipsel:4000`), server-side
disassembly, an automatic decompiler, and real PSP compilers — GCC (`psp-gcc`),
Sony SNC (`pspsnc`), and several **Metrowerks CodeWarrior** builds (`mwccpsp_3.0.1_*`).
It answers a question Nakagawa does not: **"what source produced this machine code?"**

It is a **downstream, optional matching backend**, never part of the runtime:

```text
private PSP ELF/PRX → analyze.py → function + CFG + context
                                        ├─→ Nakagawa recompilation (execution truth)
                                        └─→ decomp.me scratch → compiler-matched C
                                                → symbols / types → private HST decomp
```

Nakagawa's reference interpreter / recompiler prove **execution** correctness; a
decomp.me 100% match proves **machine-code equivalence** for a compiler+flags. They
answer different questions and are strongest together — neither is authority over the
other (a match is *evidence*, e.g. for function boundaries and #51).

## Privacy discipline (non-negotiable)

Target-game assembly is proprietary-derived (see the publication review). Therefore:

- **Self-host decomp.me locally** (`git clone https://github.com/decompme/decomp.me`
  then `docker compose up --build`, ~6 GB RAM; the PSP platform is enabled in the dev
  compose). Treat that as a private laboratory.
- **Do not upload retail-game functions to the public service.** Its privacy policy
  retains submitted scratch data and allows public exports.
- The exporter writes only to the **gitignored build dir** and never uploads. Any
  submission (even local) is a separate, deliberate step; a future `--submit` must
  default to `localhost` and require an explicit `--allow-external-upload` for the
  public service.
- Matched source and symbols are **game data** and stay in a private local
  workspace, not in this public-source candidate.

## The exporter — `tools/decompme_export.py`

Read-only, offline. Reuses `analyze.py` (no second function finder) to locate a
function, extract its bytes, and emit a decomp.me-ready bundle:

```powershell
python tools/decompme_export.py place_game_here/EBOOT.elf --function 0x0005A648
# -> build/hst/decompme/f_0005a648/  (gitignored)
#      metadata.json  context.c  function.bin  target.o  starter.c  target.s.note
```

- **`target.o`** is a minimal little-endian MIPS ELF object (decomp.me's `target_obj`).
  This is the robust path: decomp.me disassembles it server-side, so a local MIPS
  objdump is not required (this host ships none). Pass `--objdump <mips-objdump>` to
  additionally emit GNU-syntax `target.s`.
- **`context.c`** currently supplies base PSP typedefs; it grows as the PSP API
  database (NID → prototype) and recovered game structs come online — better context
  yields better automatic decompilation.
- **`metadata.json`** records provenance (address, size, byte sha256, source-input
  sha256, commit) so a match can later be re-verified independently.

Local verification stays canonical: when a scratch reaches 100%, recompile the
matched source with the identified PSP compiler locally and byte-compare against the
original function before marking it `MATCHED`. The project must survive decomp.me
going offline or changing.

## Known PSP caveat to test early

decomp.me's hosted PSP assembler path uses a generic MIPS `-march`, not an explicit
Allegrex mode. Ordinary MIPS32 functions are fine, but **VFPU / Allegrex-specific**
instructions may not round-trip. First integration set should include a leaf integer
fn, a stack-frame fn, an FP fn, a switch/jump-table fn, one Allegrex-specific fn, and
one VFPU-heavy fn. If the last two fail, prefer the `target.o` object path (already
the default here) and/or contribute Allegrex assembler support upstream.

## Staged plan

1. **Self-host** decomp.me locally; manually create one PSP scratch from a simple HST
   function; try GCC / SNC / several CodeWarrior builds.
2. **Exporter** (this tool) — read-only bundle emission. *(done — first version)*
3. **Context generator** — grow `context.c` from the NID/API database, strings, and
   recovered names.
4. **Compiler-fingerprint campaign** — 20–50 non-library functions across the PSP
   compilers/flags to determine, statistically, which compiler+config built HST
   (prologue/regalloc/delay-slot/switch/FP idioms). A major step toward matching.
5. **Private local API bridge** — `--submit` to `localhost/api/scratch`, external
   upload gated.
6. **Matched-function database** — in the private game-data repo, as human/matching
   evidence (not execution semantics).
7. **Analyzer feedback** — feed matches back as boundary/call-graph/type evidence
   (directly serves #51's callable-vs-continuation distinction).
8. **Upstream collaboration** — a PSP compiler preset, improved Allegrex/VFPU support
   — only after the publication/legal questions are settled.

## Related

- [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) — why decompiled output is private.
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — `analyze.py` and the pipeline the exporter reuses.
