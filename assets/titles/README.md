# Public title manifests

Files in this directory are source-owned configuration. They may identify a supported title or fixture and describe load policy, module names, public runtime requirements, and synthetic verification profiles.

They must not contain:

- retail executable or asset bytes;
- hashes or inventories derived from private game inputs;
- keys, decrypted output, local absolute paths, or private workspace bindings;
- decompiler output, recovered source, oracle traces, savedata, screenshots, or route evidence.

Private bindings are explicit local inputs — command-line bindings
(`--game-elf`/`--build-dir`/`--module-dir`/`--psp-header`), `TITLE_MANIFEST=`
and `GAME_*` on a direct Make line, or `-TitleManifest` plus the manifest's
declared input locations for the manager (see below) — and are never written
back into a checked-in manifest file. The
checked-in synthetic manifests prove the schema and validator without claiming
that the current runtime is general-purpose. HST title configuration remains
local-only unless a later, separately reviewed publication decision changes the
profile.

The JSON Schema is a portable editor/review contract. `tools/title_manifest.py` is the normative validator and additionally enforces semantic invariants such as portable Windows names, non-overlapping executable spans, and case-insensitive uniqueness.

Validate a manifest:

```powershell
python tools/title_manifest.py assets/titles/synthetic.json
```

Print deterministic canonical JSON:

```powershell
python tools/title_manifest.py assets/titles/synthetic.json --print-normalized
```

## Declared input locations

The manager assumes no input layout. A local manifest declares where its private
inputs live, as relative paths in the `filesystem` block:

| Key | Needed when | Path rule |
| --- | --- | --- |
| `executable` | the title's ELF is not at `eboot.elf` or a fixture path | Make-safe: `[A-Za-z0-9._-]` components |
| `psp_header` | `executable.bss_metadata_source` is `psp-header` | Make-safe |
| `module_dir` | any module has role `guest-prx` | Make-safe |
| `disc_image` | running a retail title | Ordinary file names: spaces, brackets and Unicode are allowed; Windows-forbidden characters, control characters, `.`/`..` components, a leading space and a trailing space or dot are not |

Paths the manager passes to Make keep the strict component rule. The disc image
reaches only the runtime (`PSP_ISO`), so a dump can keep its original file name.
When a required declaration is missing, the manager stops before planning and
names the key to add.

## Checked-in manifests

- `synthetic.json` is a source-owned public fixture for schema and tool testing.
  It also carries the Wave-1 `psp-core-v1` / `profile-zero-v1` contract,
  source-program/build binding, and an acceptance scaffold. The scaffold marks
  the full runtime route as planned rather than claiming a runnable end-to-end
  product path.
- `pspdev-phase5.json` is a second wholly source-owned fixture whose sources live
  in `fixtures/pspdev_phase5` (a standard PSPDEV/PSPSDK `BUILD_PRX=1` module). It
  is deliberately configured *differently* from `synthetic.json` — the canonical
  user-module load base `0x08804000` rather than the user-memory region start, no
  guest PRX of any kind, and an HLE-dependent feature set — so the manifest-driven
  planner is proven genuinely multi-title rather than parameterized for one game.
  It contains synthetic addresses and build paths only, and no retail metadata.

Both checked-in fixtures also carry an optional `runtime_bindings` block, with
deliberately **disjoint** source-owned addresses. That block is the only way a title
address reaches the compiled runtime (`src/rt/title_config.h`); `make sched-selftest`
builds one scheduler source against a generic configuration and against each of these
two, so behavior bound to one fixture's addresses cannot pass as generic. Neither
fixture reuses any address the runtime previously hardcoded.

Every binding family inside that block is individually optional, and that is what
keeps the schema generic: a title needing none of them is a valid title, and no
family is globally mandatory. Optionality alone, though, cannot tell "this title
does not use display bring-up" apart from "this title's display bring-up was
lost" — the runtime reads both as the same disabled binding and silently falls
back to generic PSP semantics. `required_runtime_bindings` is the optional
root-level list where a title names the families it cannot function without:

```json
"required_runtime_bindings": ["display_bringup", "frame_ready_latch_addr"]
```

Declaring a family makes its loss a validation failure instead of a run-time
behavior change. A declared family must be present and completely configured;
the existing per-family rules continue to reject a half-configured family and an
explicitly zero address, so all three shapes — whole family absent, family
partially present, required address zero — are refused. Names are checked
against the schema's own family list, so a typo cannot silently declare nothing.
`tools/title_runtime_config.py` re-checks the same contract before it emits, so
no build path can turn a required family into a header full of disabled macros.
Titles that omit the key are unaffected.

- `synthetic-title2.json` is a third source-owned fixture added for the
  generic-title planning proof: it uses a deliberately distinct synthetic
  address family (`0x0A4xxxxx`, never HST's `0x003xxxxx` or the other
  synthetics' `0x088xxxxx`), a distinct module name (`synthetic2.prx` at
  `0x0A800000`), and disjoint `dispatch_aliases`/`callback_terminators`.
  It validates that the generic planner accepts a non-HST identity without
  adding a title-specific conditional, inheriting HST constants, or reading
  private inputs. Publication-safe and deterministic.

- `display-smoke.json` is a fourth source-owned fixture, and the only one whose
  runtime is *built* under the layout `src/core/nk_launch.c` resolves
  (`build/<title_id>/<title_id>`), which is what makes it the one public title
  the native player can launch. Its guest is emitted by
  `fixtures/display_smoke/generate.py` as hand-assembled MIPS -- no PSPDEV
  toolchain -- and it fills the PSP framebuffer and flips it through
  `sceDisplaySetFrameBuf` once per frame, so it is also the only public fixture
  that exercises the display/vblank/present path at all. Its address family
  (`0x0881xxxx`) is distinct from the other synthetics and from HST.
  Publication-safe and deterministic.

The analyzer applies **no** title-specific executable span by default: a raw
base-zero image never silently inherits another title's span. An extra executable
span is manifest data, and it reaches `analyze`/`codegen` only through an explicit
`--extra-span` argument or the `TITLE_EXTRA_SPANS` seam that the manager fills from
the validated plan (the host-portable generic contract). See [`docs/TITLE_CODEGEN_PLAN.md`](../../docs/TITLE_CODEGEN_PLAN.md).

`hst-ucus98701.json` is intentionally not checked in: it contains title-specific
identity, module addresses, and private-route filesystem configuration. It is
publication-excluded via `assets/public_source_profile.json`, with an explicit
`.gitignore` accident guard. The opt-in `nk_manager.ps1 -TitleManifest` path may
consume a local copy of that manifest;
that does not make the runtime generic or prove portability/correctness for
another title.
