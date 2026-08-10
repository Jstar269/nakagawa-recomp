# Public title manifests

Files in this directory are source-owned configuration. They may identify a supported title or fixture and describe load policy, module names, public runtime requirements, and synthetic verification profiles.

They must not contain:

- retail executable or asset bytes;
- hashes or inventories derived from private game inputs;
- keys, decrypted output, local absolute paths, or private workspace bindings;
- decompiler output, recovered source, oracle traces, savedata, screenshots, or route evidence.

Private bindings belong in a separate Git-ignored workspace manifest. The initial checked-in `synthetic.json` proves the schema and validator without claiming that the current runtime is general-purpose. HST migration into a public title manifest is a later behavior-preserving phase tracked by issue #197.

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
- `hst-ucus98701.json` records the current source-owned HST configuration: supported
  disc identity, zero-based executable policy, PSP-header metadata source, extra
  executable span, PRX module names/load addresses, filesystem conventions, and
  public feature/profile identifiers.

The HST manifest remains a declarative parity anchor, and is now consumed only
through the opt-in `hst_manager.ps1 -TitleManifest` path. The no-manifest manager
path and duplicated Makefile constants remain unchanged until a later reviewed
equivalence slice retires them. The manifest does not make the runtime generic or
prove portability/correctness for another title.
