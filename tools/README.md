# `tools/` — Host-side recompiler scripts

These scripts require Python 3.14.x. PowerShell entrypoints require PowerShell 7.6+ (`pwsh`); they
run on the development host and are never executed by `hst.exe` at runtime. For HST, prefer `hst_manager.ps1`; it supplies the required
zero base/entry values and preserves the Makefile's two-phase build.

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
   `.\hst_manager.ps1 -Action BuildFull` from the repository root.

## Gates

- **`codegen_gate.py <elf> <oracle.trace> <workdir>`** — external-oracle gate.
  Generates C, compiles with `$CC` (or `gcc`), runs to the first HLE boundary, and compares the
  pre-HLE trace with a user-supplied oracle trace (captured from PPSSPP or the reference interpreter).
- **`verify_gates.py`** — orchestrates the optional codegen and microtest gates used by
  `make verify`; it reports blocked when their external inputs are absent.
- **`funcdiff_cmp.py …`** — compares per-function traces supplied by the developer.
- **`microtest_gate.py`, `gen_microtest.py`, `vfpu_fuzz_gen.py`** —
  per-instruction / per-function tests for translator regressions.
- **`tracediff.py`** — trace-format diff during bring-up (`TRACE_FORMAT.md`).
- **`ppmdiff.py`, `ppm2png.py`** — A/B framebuffer diffs and PPM-to-PNG conversion.
  `SR_FBSNAP=<N>` writes rotating PPM snapshots every N vblanks.
- **`nidseq.py`, `gen_nidnames.py`** — NID-table tooling.
- **`import_audit_gate.py`** — public import-coverage/fake-success gate:
  fail-closed HLE manifest from `src/rt/hle.c` (`hle_manifest.py` +
  `hle_registry_meta.py`), classification baseline drift, and synthetic malformed-ELF
  fixtures (`import_fixtures.py`, `psp_import_table.py`). `import_audit.py` classifies a
  developer-supplied private ELF locally — see [`docs/IMPORT_AUDIT.md`](../docs/IMPORT_AUDIT.md).
- **`xb_probe.py <archive.xb> [--lookup <inner-key>]`** — bounded, read-only direct-XB
  metadata/lookup prototype (see [`docs/ISSUE196_DIRECT_XB.md`](../docs/ISSUE196_DIRECT_XB.md)). It uses synthetic tests in `test_xb_probe.py`,
  never dumps archive contents by default, and does not participate in production HLE lookup.

Run the generator regression suite without game inputs:

```powershell
python -m unittest discover -s tools -p "test_*.py" -v
```

## Hard rules

- **Never** hand-edit generated files under `build/<game>/`.
- Translation fixes belong here or in the runtime; tests are the gate.
- After any generator change, run `BuildFull` and confirm generated object files are newer than
  their chunk sources before trusting runtime results.
