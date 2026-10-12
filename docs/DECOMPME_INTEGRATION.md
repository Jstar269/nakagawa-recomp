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
other (a match is *evidence*, e.g. for function boundaries and callable-vs-continuation
classification).

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
- **`context.c`** currently supplies base PSP typedefs. When it is given a list of
  imported NIDs it now annotates each one with its known PSP function name, but
  nothing collects that list yet, so today's exports carry typedefs only. Wiring the
  exporter's import discovery to that list, and adding the PSP API database
  (NID → prototype) and recovered game structs, is what grows this file — better
  context yields better automatic decompilation.
- **`metadata.json`** records provenance (address, size, byte sha256, source-input
  sha256, commit) so a match can later be re-verified independently.
- **`--with-cfg`** additionally emits **`cfg.json`**, an advisory function view
  from `analyze.canonical_cfg_report`. It uses the same legacy analyzer entry set
  as byte extraction; it does not enable `--cfg-gate` or change the extracted
  extent. The default bundle remains unchanged.

### Function CFG view

```powershell
python tools/decompme_export.py place_game_here/EBOOT.elf --function 0x0005A648 --with-cfg
```

`cfg.json` uses schema `nakagawa.decomp-function-cfg`, version 1, and records the
underlying canonical CFG schema version. Its `evidence` binds the source-input
SHA-256 and tool commit and labels the result `ANALYZER_INFERRED`. The bundle's
`metadata.json` binds the exact `cfg.json` bytes by SHA-256. These bindings identify
inputs and artifacts; they do not attest correctness, provenance, or a compiler match.

The view contains the instructions inside the approximate bundle extent **plus**
all instructions the canonical CFG assigns to the selected entry. This exposes
shared epilogues and other code beyond the next discovered start without silently
enlarging `function.bin` or `target.o`. `owned_outside_extent` and `extent_not_owned`
make the difference between matching bytes and inferred ownership explicit.

Outgoing `edges` and `successor_classifications` retain canonical call, tail-call,
continuation/resume, and fail-closed indirect classifications. `incoming_edges`
provides cross-references from outside the view; `boundary_instructions` records
the known owners and classifications at external endpoints. Delay-slot/likely
annulment flags, conflicting owners, interior entries, unresolved indirect edges,
and jump-table ownership observations remain visible. An unknown indirect target
stays `null`; no function boundary or target is invented to make the view complete.

The exporter checks the full canonical report's structural consistency before
writing the bundle. The function view is a projection, **not** a standalone
canonical report accepted by `canonical_cfg_gate`. Ownership conflicts are retained
as observations, not repaired or promoted into execution authority. This is a
bounded step toward issue #305; typed dataflow, imported annotations, and pseudocode
recovery remain future work. CFG output inherits the input's privacy boundary.

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
   (directly serves the callable-vs-continuation distinction).
8. **Upstream collaboration** — a PSP compiler preset, improved Allegrex/VFPU support
   — only after the publication/legal questions are settled.

## Related

- [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) — why decompiled output is private.
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — `analyze.py` and the pipeline the exporter reuses.
