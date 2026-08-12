# Public title manifests

Files in this directory are source-owned configuration. They may identify a supported title or fixture and describe load policy, module names, public runtime requirements, and synthetic verification profiles.

They must not contain:

- retail executable or asset bytes;
- hashes or inventories derived from private game inputs;
- keys, decrypted output, local absolute paths, or private workspace bindings;
- decompiler output, recovered source, oracle traces, savedata, screenshots, or route evidence.

Private bindings belong in a separate Git-ignored workspace manifest. The
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

## Checked-in manifests

- `synthetic.json` is a source-owned public fixture for schema and tool testing.
- `pspdev-phase5.json` is a source-owned PSPDEV/PSPSDK Phase 5 fixture with
  synthetic addresses and build paths; it contains no retail metadata.

`hst-ucus98701.json` is intentionally not checked in: it contains title-specific
identity, module addresses, and private-route filesystem configuration. The
opt-in `hst_manager.ps1 -TitleManifest` path may consume a local ignored manifest;
that does not make the runtime generic or prove portability/correctness for
another title.
