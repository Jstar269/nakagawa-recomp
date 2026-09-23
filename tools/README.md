# `tools/` — Host-side recompiler scripts

These scripts require Python 3.14.x. PowerShell entrypoints require PowerShell 7.4+ (`pwsh`); they
run on the development host and are never executed by `hst.exe` at runtime. For HST, use the canonical `nk_manager.ps1` with `-GameName hst` and `-TitleManifest`; it supplies the required zero base/entry values and drives the Makefile's two-phase build.

## Pipeline (in order)

1. **`prxload.py <elf> <base> --out=build/<g>/<g>_image.bin`**
   Rebase the PRX/ELF at `<base>`, apply type-A relocs (`SHT_PRX_RELOC = 0x700000A0`),
   dump a flat image suitable for `--image`.

2. **`imports.py <elf> <base> --toml=build/<g>/<g>_imports.toml`**
   Walk the import tables, resolve MIPS NIDs to HLE handler names.
   `codegen.py` reads this TOML at translation time.

3. **`codegen.py <elf> build/<g>/<g>_recomp.c --base=<base>`**
   MIPS → C. Consumes `analyze.py`'s function boundaries. The generator splits the
   output into `<g>_recomp_0.c` through `<g>_recomp_N.c`; the number of translation
   units is determined by the discovered function count and `FUNCS_PER_CHUNK`, not by
   a fixed HST-specific chunk count. The split keeps individual files practical for
   gcc `-O0` compilation.

4. **`mingw32-make GAME_NAME=<g> GAME_ELF=<elf> GAME_BASE=<b> GAME_ENTRY=<e> all`**
   Drives the two-phase pipeline and compile. Set `VULKAN_SDK` for direct Make invocations; the
   manager discovers and validates it automatically. Do not replace `all` with a single dependency
   line: generated chunk discovery occurs in the second Make process. For HST, use
   `.\nk_manager.ps1 -Action BuildFull -TitleManifest assets/titles/hst-ucus98701.json -GameName hst` from the repository root.

## Gates

- **`codegen_gate.py <elf> <oracle.trace> <workdir>`** — external-oracle gate.
  Generates C, compiles with `$CC` (or `gcc`), runs to the first HLE boundary, and compares the
  pre-HLE trace with a user-supplied oracle trace (captured from PPSSPP or the reference interpreter).
- **`verify_gates.py`** — orchestrates the optional codegen and microtest gates used by
  `make verify`; it reports blocked when their external inputs are absent.
- **`funcdiff_cmp.py <oracle-trace> <my-trace> <entry-step>`** — compares per-function
  traces supplied by the developer. Fail-closed evidence gate (issue #381): exit 0 requires
  at least one compared step, oracle coverage of every recomp step from `<entry-step>`
  onward (the oracle may continue beyond that slice), and all steps matched. Empty traces,
  oracle truncation, entry beyond the oracle, or malformed records exit nonzero.
- **`microtest_gate.py`, `gen_microtest.py`, `vfpu_fuzz_gen.py`** —
  per-instruction / per-function tests for translator regressions.
- **`tracediff.py`** — trace-format diff during bring-up (`TRACE_FORMAT.md`).
- **`ppmdiff.py`, `ppm2png.py`** — A/B framebuffer diffs and PPM-to-PNG conversion.
  `SR_FBSNAP=<N>` writes rotating PPM snapshots every N vblanks.
- **`nidseq.py`, `gen_nidnames.py`** — NID-table tooling. `nidseq.py <imports.toml>
  <trace>` is informational extraction (no equivalence claim, exit 0). With a second trace,
  `nidseq.py <imports.toml> <trace> <oracle-trace>` is verification: exit 0 means exact
  import-sequence equality with at least one import compared (issue #381); divergence,
  zero compared imports, either-side sequence mismatch, or malformed data exit nonzero.
  `--allow-prefix` explicitly accepts strict-prefix agreement for documented prefix
  analysis and is never the default.
- **`import_audit_gate.py`** — public import-coverage/fake-success gate:
  fail-closed HLE manifest from `src/rt/hle.c` (`hle_manifest.py` +
  `hle_registry_meta.py`), classification baseline drift, and synthetic malformed-ELF
  fixtures (`import_fixtures.py`, `psp_import_table.py`). `import_audit.py` classifies a
  developer-supplied private ELF locally — see [`docs/IMPORT_AUDIT.md`](../docs/IMPORT_AUDIT.md).
- **`lint_docs.py`** — deterministic offline documentation-freshness gate. It scans tracked Markdown
  and rejects current-facing stale-status patterns while preserving explicitly historical evidence.
  The shared pre-commit/pre-push hooks run it automatically.
- **`audit_public_issue_links.py`** — networked public Issue/PR reference audit for tracked Markdown.
  It verifies URL type, current-facing shorthand references, and explicit current-tracker state labels.
  Normal mode reports `SKIPPED` if GitHub is unavailable; use `--strict` as a live review/merge audit.
  It is deliberately separate from the offline pre-commit hook so lack of network access cannot make
  ordinary local commits nondeterministically fail.
- **`xb_probe.py <archive.xb> [--lookup <inner-key>]`** — bounded, read-only direct-XB
  metadata/lookup prototype (see [`docs/ISSUE196_DIRECT_XB.md`](../docs/ISSUE196_DIRECT_XB.md)). It uses synthetic tests in `test_xb_probe.py`,
  never dumps archive contents by default, and does not participate in production HLE lookup.
- **`extract_xb.py <xbdata-dir>`** — batch XB extractor with no third-party dependency
  (see [`docs/SETUP.md`](../docs/SETUP.md)). The whole pipeline is repository-owned:
  `xb_probe.py` parses and decodes each member — including the nested `DEFLATE → LZS`
  layer — under the budget contract at the top of the module, and `extract_xb.py`
  normalizes each member name once and writes it at that same identity, so the validated
  path and the written path are the same on every host. Archives are staged and promoted
  only on full success, the produced tree is re-verified for reparse points, whole-file
  reads are size-gated, destinations and generated files are not replaced without
  `--overwrite`, and both worker count and in-flight task count are bounded. Synthetic
  tests live in `test_extract_xb_security.py` and `test_extract_xb_gim.py`.

Run the generator regression suite without game inputs:

```powershell
python -m unittest discover -s tools -p "test_*.py" -v
```

For a documentation change with network access, also run:

```powershell
python tools/audit_public_issue_links.py --strict
```

## Hard rules

- **Never** hand-edit generated files under `build/<game>/`.
- Translation fixes belong here or in the runtime; tests are the gate.
- After any generator change, run `BuildFull` and confirm generated object files are newer than
  their chunk sources before trusting runtime results.
