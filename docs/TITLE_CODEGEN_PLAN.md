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

## Ownership

There is one owner for every title-derived value:

```text
title manifest  ->  title_manifest.validate_manifest  ->  validated manifest
                ->  title_codegen_plan.build_manager_plan  ->  canonical plan
                ->  title_manager_plan.ps1  ->  Make variables + analyzer seam
                ->  codegen.py / analyze.py
```

The planner is the only place a manifest becomes build configuration. PowerShell
adapts that plan to a process invocation and re-derives nothing of its own: it
checks that each build-facing projection (`make.*`, `environment.*`) follows from
the plan's own semantic fields, then pins the single title the HST manager
orchestrates. Make consumes explicit values and contributes no title-specific
default beyond the direct-build HST bindings at the top of the `Makefile`.

Executable spans follow the same rule. `analyze.py` has no built-in span: an extra
executable span is title configuration and reaches the analyzer only as an explicit
argument. The environment variable `HST_EXTRA_SPANS` is read at CLI entry points
only, and only for the primary image, so a rebased extra guest module can never
inherit another module's span. Make passes the span as `--extra-span=LO,HI` rather
than a recipe environment prefix, which keeps the binding working when Make falls
back to `cmd.exe`. If both an option and an environment value are present and they
disagree, the run fails closed.

## Protected digest

`compute_protected_digest()` hashes the *validated, canonically serialized*
manifest with the free-text `notes` field removed, and the result travels in the
plan as `protected_digest`. Because validation normalizes ordering, numeric types,
and optional fields first, the digest depends on meaning alone: key order,
indentation, and line endings cannot move it, a notes-only edit cannot move it, and
any operative change does. Unknown fields are rejected by validation rather than
excluded from the digest, so nothing operative can travel unprotected.

The manager re-derives the digest from the manifest on disk immediately before
spawning Make and refuses to build when it no longer matches the plan, closing the
window between planning and execution. Print it directly with:

```powershell
python tools/title_codegen_plan.py assets/titles/synthetic.json --print-protected-digest
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

The optional `runtime_contract` is the Wave-1 core/profile boundary. Version
`psp-core-v1` owns PSP semantic capabilities; `profile_id` selects title policy;
all HLE dispositions require an explicit reason and evidence class; and
`unknown_capability_policy` is fixed to `fail-closed`. The profile cannot add a
silent semantic override, and `enhancements.enabled_by_default` is separate from
core acceptance. Existing HST manifests remain valid without this field while the
manager continues to use its legacy, privately bound path; migration is additive,
not a production switch.

The public synthetic manifest additionally carries `profile_zero`, which points to
source-owned PSPDEV/PSPSDK input and a portable Make build path. Its acceptance
cases distinguish planned production-dispatch/helper evidence from the currently
implemented source-shape ProgramImage test. `runnable: false` is intentional until
the actual end-to-end AOT/runtime route is wired and asserted.

Current fail-closed limits are deliberate:

- the analyzer accepts at most one explicit extra executable span;
- an explicit span cannot yet be combined with a nonzero executable base; and
- only the generator's current `hst` and `none` profile choices are accepted.

Codegen enforces profile isolation: `codegen.py`'s HST-specific translations
(address-specific stubs, GUEST_PATCHES, MEMSET/ARRSHIFT fastpaths,
`GUEST_ABORT`, null-base loads, boot probes and the `EMIT_DIAG_PROBES`
diagnostics) are gated behind `profile == "hst"`. With `--profile=none` the
translator is faithful — no HST numeric address alone implies HST semantics.
`tools/test_codegen_profile_isolation.py` proves `profile=none` emits no
`sr_newlib_*`, native `memcpy`/`memset`, `MEMSET`/`ARRSHIFT` fastpaths,
`GUEST_ABORT`, `sr_boot_probe` or `GUEST_PATCHES` text, while `--profile=hst`
preserves the legacy HST behaviours on the same synthetic specimen.

The manager adapter is deliberately fail-closed: this slice accepts only the
checked-in HST manifest for HST manager actions, and it rejects unsupported plan
versions, unknown plan fields, malformed digests, projections that disagree with
the plan's semantics, a manifest that changed after planning, missing required
private bindings, and unsupported span/profile configurations before Make runs.
This does not make the runtime general-purpose or prove a private HST build or
route.

`assets/titles/pspdev-phase5.json` is a second, materially different source-owned
fixture (PSPDEV/PSPSDK sources in `fixtures/pspdev_phase5`) driven through the same
planner; see `tools/test_title_pspdev_phase5.py`. The adapter's own contract is
covered by `tools/test_title_manager_adapter.py` and the digest by
`tools/test_title_protected_digest.py`, all using public manifests only.
