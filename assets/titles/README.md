# Public title manifests

Files in this directory are source-owned configuration. They may identify a supported title or fixture and describe load policy, module names, public runtime requirements, and synthetic verification profiles.

They must not contain:

- retail executable or asset bytes;
- hashes or inventories derived from private game inputs;
- keys, decrypted output, local absolute paths, or private workspace bindings;
- decompiler output, recovered source, oracle traces, savedata, screenshots, or route evidence.

Private bindings belong in a separate Git-ignored workspace manifest. The initial checked-in `synthetic.json` proves the schema and validator without claiming that the current runtime is general-purpose. HST migration into a public title manifest is a later behavior-preserving phase.

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
- `pspdev-phase5.json` is a second wholly source-owned fixture whose sources live in
  `fixtures/pspdev_phase5` (a standard PSPDEV/PSPSDK `BUILD_PRX=1` module). It
  deliberately exercises a *different* configuration from `synthetic.json` — a
  canonical user-module load base (`0x08804000`), no optional guest PRX, and an
  HLE-dependent feature set — so the manifest-driven planner is proven genuinely
  multi-title rather than parameterized HST.
- `hst-ucus98701.json` records the current source-owned HST configuration: supported
  disc identity, zero-based executable policy, PSP-header metadata source, extra
  executable span, PRX module names/load addresses, filesystem conventions, and
  public feature/profile identifiers.

The HST manager consumes the HST manifest through `hst_manager.ps1 -TitleManifest`:
the manager builds every make argument from the validated plan (base, entry, profile,
module list, analyzer span) instead of re-encoding HST values, and fails closed when
the plan's protected-contract digest does not match the checked-in manifest. The
no-manifest manager path and the direct-Make HST constants remain unchanged; both
are equivalent by test, and neither proves the runtime generic for another title.

The analyzer applies **no** title-specific executable span by default (issue #151).
The HST span reaches `analyze`/`codegen` only through the explicit `HST_EXTRA_SPANS`
override supplied by the manager (from the manifest) or by the direct-Make HST
binding — a raw base-zero image never silently inherits it.
