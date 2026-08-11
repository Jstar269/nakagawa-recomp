# Title-driven code-generation plan

`tools/title_codegen_plan.py` is a read-only bridge between the public title
manifest and the existing `prxload.py`, `codegen.py`, and `imports.py` command
lines. It prints deterministic JSON; it does not execute commands, inspect private
inputs, or modify the manifest.

With `--manager-plan`, the same validated configuration produces a bounded,
versioned manager/build contract. `hst_manager.ps1` consumes that contract only
when `-TitleManifest` is supplied; the legacy no-manifest path remains unchanged.
The manager contract contains title semantics, a protected-contract digest, and
private-binding requirements, not absolute private paths or command strings.

The manager adapter builds every make argument from the plan (`plan.make.*`, the
selected guest modules, and the analyzer environment) and validates the plan's
`protected_digest` against the single constant for the checked-in HST manifest.
It no longer re-encodes HST values (base/entry, spans, modules, profile, disc);
any mutation of the protected title contract changes the digest and is rejected
before Make runs. Regenerate the constant with:

```powershell
python tools/title_codegen_plan.py --print-protected-digest assets/titles/hst-ucus98701.json
```

Private workspace bindings remain explicit:

```powershell
python tools/title_codegen_plan.py assets/titles/hst-ucus98701.json `
  --game-name=hst `
  --game-elf=place_game_here/EBOOT.elf `
  --build-dir=build/hst `
  --module-dir=place_game_here/EXTRACTED/decrypted `
  --psp-header=place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN `
  --profile=hst
```

The resulting plan reproduces the current HST base, entry, PSP-header policy,
module names and load addresses, analyzer span, and codegen profile. Paths are
normalized for deterministic output but are not resolved or checked for existence.

Manifests may declare `codegen_profile` (`hst` or `none`); when present it is
authoritative and an optional `--profile` must match it. A manifest without that
field still requires an explicit `--profile`. Optional guest PRXs are excluded
unless selected with `--include-optional-module=<name>`.

Current fail-closed limits are deliberate:

- the analyzer accepts at most one explicit extra executable span;
- an explicit span cannot yet be combined with a nonzero executable base; and
- only the generator's current `hst` and `none` profile choices are accepted.

The manager adapter is deliberately fail-closed: this slice accepts only the
checked-in HST manifest for HST manager actions (identity + protected-contract
digest), and it rejects unsupported plan versions, digest mismatches, missing
required private bindings, and unsupported span/profile configurations before Make
runs. This does not make the runtime general-purpose or prove a private HST build
or route.

`assets/titles/pspdev-phase5.json` is a second, meaningfully different source-owned
fixture (PSPDEV/PSPSDK sources in `fixtures/pspdev_phase5`) exercised through the
same planner; see `tools/test_title_pspdev_phase5.py`.
