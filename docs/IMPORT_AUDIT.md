# Import-coverage and fake-success audit (issue #71)

The audit distinguishes three materially different states for every NID a PSP
title imports, plus one honest refusal state:

| Classification | Meaning at runtime |
| --- | --- |
| `missing` | No static HLE registration. Under the fiber scheduler the first call terminates the process (`_Exit(7)` in `src/rt/hle.c`). |
| `fake_success` | Routed to a generic always-success handler (`h_ok` family) or a handler curated as `stub`. The call reports success but performs none of the API's contract — the silent-corruption case. |
| `dedicated` | Has its own handler. Not a claim of completeness: the handler carries a status (`complete`, `partial`, `compatibility`, `unreviewed`). |
| `controlled_unsupported` | A dedicated handler that deliberately refuses with the API's documented error (e.g. the PSMF getters returning `PSMF_ERR_NO_DATA` until issue #31 lands). |

## Components

| Piece | Role |
| --- | --- |
| `tools/psp_import_table.py` | Defensive PSP ELF import-table parser. Bounds-checked reads, checked address arithmetic, hard resource caps, structured errors. Locates `SceModuleInfo` via the `.rodata.sceModuleInfo` section, or — for stripped/sectionless PRX-style inputs — via the `phdr[0].p_paddr` file-offset convention (kernel bit masked, offset validated against loaded ranges). Function stub spans must be 4-byte aligned, non-wrapping, and fully file-backed. Never executes or disassembles guest code. |
| `tools/hle_manifest.py` | Fail-closed extraction of every `sr_hle_register` in `src/rt/hle.c` into a deterministic JSON manifest with per-NID classification. Any registration form it cannot prove it captured is a hard error. |
| `tools/hle_registry_meta.py` | Curated, reviewed metadata: handler statuses, alias-consistency rules, canonical NID names, and acknowledged-defect waivers (each with an issue link). |
| `tools/import_audit.py` | Classifies a developer-supplied ELF's imports against the manifest; writes machine-readable JSON and a human-readable triage report. |
| `tools/import_audit_gate.py` | The public CI gate. Runs on `src/rt/hle.c` plus synthetic in-memory fixtures only. |
| `tools/import_audit_baseline.json` | Committed classification baseline the gate diffs against to catch regressions from dedicated/controlled handling to fake-success/missing. |
| `tools/import_fixtures.py` | Synthetic ELF builders (well-formed multi-library, cross-library duplicate NIDs, and ten malformed variants). No binary fixture is committed. |

## Public CI gate

```bash
python tools/import_audit_gate.py
```

Fails on: unaccounted registration forms in `hle.c`, duplicate NID
registrations, an alias/mislabel finding without a waiver, a stale waiver, any
drift from the committed baseline (classification downgrades are called out
explicitly), a malformed fixture that is not rejected cleanly, a mixed-fixture
classification mismatch, or nondeterministic report output. It never reads a
game file, and a private title importing unsupported APIs is deliberately not
a public CI failure.

After intentionally changing registrations in `src/rt/hle.c`, refresh the
baseline and review the diff for downgrades before committing it:

```bash
python tools/hle_manifest.py --write-baseline
```

## Auditing a private EBOOT locally

Reports for a real title stay on your machine: write them under `build/`
(ignored and publication-audited; nothing under `build/` can be committed).

```bash
python tools/hle_manifest.py --out build/hle_manifest.json
python tools/import_audit.py --elf "<path to your decrypted EBOOT ELF>" \
    --manifest build/hle_manifest.json \
    --out build/private/import_audit.json --text build/private/import_audit.txt
```

By default the report is API/library-level only: library names, NIDs, public
API names, classification, and handler status. Guest stub addresses are
emitted only with `--with-addresses`; keep such reports out of commits,
issues, and pasted logs. The tool exits nonzero only for unreadable, oversized,
or malformed input — malformed ELFs produce a one-line `import_audit:
malformed input: …` error, never a traceback; oversized files are refused
before they are read into memory. A `--manifest` file is fully validated
(schema, required fields, NID format, classification/status vocabulary,
duplicate NIDs) and rejected with a one-line `import_audit: invalid
manifest: …` error if it cannot be trusted.

Because the JSON is deterministic (sorted, ASCII, LF, no timestamps), two
private runs can be diffed across machines or revisions without ever
committing their contents.

## Issue triage

The text report ends with a `Triage:` section listing every `missing` and
`fake_success` import the title actually references. File one focused
implementation issue per API family (as with issues #59–#70) instead of
broadly mapping NIDs to `h_ok`, and link the manifest classification in the
issue. When a fix retires an acknowledged defect, the waiver in
`tools/hle_registry_meta.py` must be removed in the same change — a stale
waiver fails the gate.

## Retired waivers

`0x1b4217bc` (`sceKernelSetCompiledSdkVersion603_605`) was originally
registered under the generic `sceKernelSetCompiledSdkVersion` name routed to
`h_ok`, fabricating success without updating `g_sdk_version` (see the audit
note on [issue #71](https://github.com/Jstar269/nakagawa-recomp/issues/71)).
It now routes to `h_SetCompiledSdkVersion` under its canonical name, and both
waivers were retired in the same change. The alias rule and canonical-name
map in `tools/hle_registry_meta.py` remain, so a regression of either fix
fails the gate; `tools/test_sdkver_c.py` additionally executes the
retained-state contract (`src/rt/sdkver_selftest.c`).
