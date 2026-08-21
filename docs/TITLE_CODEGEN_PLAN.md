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

The compiled runtime is a second consumer of the same validated configuration, on
its own branch of the same ownership chain:

```text
title manifest  ->  title_manifest.validate_manifest  ->  validated manifest
                ->  title_runtime_config.py  ->  build/<game>/sr_title_config.h
                ->  src/rt/title_config.c  ->  SrTitleRuntimeConfig
                ->  src/rt/driver.c, src/rt/sched.c
```

`tools/codegen.py` is deliberately **not** on that branch: runtime configuration is
owned by the manifest and its generator, so a runtime binding needs neither a guest
executable nor generated retail C to exist. See
[Runtime title configuration](#runtime-title-configuration).

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

## Runtime title configuration

Until this slice the native runtime carried five guest addresses of its own: a
module-start fallback entry in `driver.c`, and a worker entry, a launcher entry, and
a VBLANK frame/vsync counter pair in `sched.c`. Those were title data compiled into
generic code, so every build of the runtime behaved as though one particular title
were loaded. They are now optional, validated *bindings*.

### The manifest block

`runtime_bindings` is one narrow optional block. Every field is individually optional
and every field is a guest address:

| Field | Consumed by |
| --- | --- |
| `fallback_entry` | `driver.c` when the image entry is not compiled |
| `worker_thread_entry` | `sched.c` worker role capture and create-reuse |
| `launcher_thread_entry` | `sched.c` launcher role capture and priority demotion |
| `vblank_frame_counter_addr` | `sched.c` on each delivered VBLANK |
| `vblank_vsync_counter_addr` | `sched.c` on each delivered VBLANK |

Validation fails closed on an unknown field, a non-integer or out-of-range address, a
misaligned address, an explicit zero (the runtime's "not configured" value, so a
configured zero would be ambiguous), a half-specified VBLANK counter pair, two equal
counter addresses, and equal worker and launcher entries. Omitting the block entirely
is valid and configures nothing.

The block may name addresses and roles. It cannot redefine a PSP semantic: what a
worker entry, a launcher demotion, or a counter increment *means* stays in the
runtime, and a binding only decides whether — and where — that meaning applies.

### The generic runtime interface

`src/rt/title_config.h` declares one small typed interface:

```c
typedef struct SrTitleRuntimeConfig { unsigned valid; uint32_t ...; const char *source_id; }
    SrTitleRuntimeConfig;

uint32_t sr_title_config_fallback_entry(void);          /* 0 when unconfigured */
int      sr_title_config_is_worker_entry(uint32_t entry);
int      sr_title_config_is_launcher_entry(uint32_t entry);
int      sr_title_config_vblank_counters(uint32_t *frame, uint32_t *vsync);
```

The predicates answer 0 for *every* entry when the corresponding binding is absent, so
an unconfigured build cannot match a role by accident, and there are no per-title
preprocessor branches anywhere in the runtime. `src/rt/title_config.c` is the only
translation unit that sees the generated artifact; its include path stops at that one
Make rule rather than entering `CFLAGS`.

### The generated artifact

`tools/title_runtime_config.py` emits `build/<game>/sr_title_config.h`. It reads the
manifest and nothing else — no guest executable, no analysis product, no generated
retail C — so `make runtime-objects` with no title inputs at all still builds and
produces the generic configuration in which every optional binding is disabled:

```bash
mingw32-make GAME_NAME=generic runtime-objects
```

A title configuration is supplied with `TITLE_MANIFEST=<path>` (the HST manager passes
the same validated manifest it plans from):

```bash
mingw32-make GAME_NAME=generic runtime-objects TITLE_MANIFEST=assets/titles/synthetic.json
```

The artifact's digest is bound into `RUNTIME_PROFILE_HASH`/`RUNTIME_PROFILE_STAMP`, so
changing a title binding changes the runtime profile and invalidates the stale runtime
objects rather than relinking them silently. The generated header also carries a schema
version that `title_config.c` refuses to compile against if it does not recognise it.

### Evidence

`make sched-selftest` builds the *same* scheduler source three times, against three
generated configurations, and runs all three:

| Flavour | Configuration |
| --- | --- |
| `generic` | no manifest; every binding disabled |
| `fixture-a` | `assets/titles/pspdev-phase5.json` |
| `fixture-b` | `assets/titles/synthetic.json` |

The two public fixtures carry deliberately disjoint source-owned addresses. Each build
asserts that the scheduler acts at its own configured addresses and that every other
candidate address — the other fixture's, and the five values the runtime used to
hardcode — claims no role, is never reused, is never demoted, and never has its counter
word written. The generic build asserts that of all of them. This is production-helper
(tier-2) evidence: the real `sched_create_thread` and `deliver_vblank` run, but the
scheduler world is a white-box test fixture rather than a title route.

`driver.c`'s use of `sr_title_config_fallback_entry()` is covered by source-shape checks
plus the generic build's assertion that the accessor returns 0; exercising the fallback
itself needs generated code and is not asserted here.

`tools/test_title_runtime_config.py` covers the validator's fail-closed rules, the
generator's determinism, and a source-shape check that no generic runtime source still
names one of the five retired addresses.

### HST

HST's real values live only in the local, Git-ignored `assets/titles/hst-ucus98701.json`
and reach the build through `hst_manager.ps1 -TitleManifest` (or `TITLE_MANIFEST=` on a
direct Make line). They are deliberately not encoded in the `Makefile`, in `src/rt`, or
in any checked-in manifest. A direct `GAME_NAME=hst` build with no `TITLE_MANIFEST`
therefore builds a runtime with no title bindings — the honest generic behavior, not a
silently inherited one.

## Profile isolation

The generator's invariant is that **a guest address must never change what
`--profile=none` emits**. Generic implementation may be core; the binding of a
numeric address to a semantic is title-owned. Every HST-specific translation in
`tools/codegen.py` — the address-specific custom stubs, `HST_SIMPLE_STUBS`,
`GUEST_PATCHES`, `NULL_BASE_WORD_LOADS`, the MEMSET/ARRSHIFT fastpaths,
`GUEST_ABORT`, the boot and inline probes, the `EMIT_DIAG_PROBES` diagnostics,
the `HST_MANUAL_CALLABLES`/`HST_RESUME_OWNERS` entry roles, and the `_SV_SPECIAL`
`--static-verify` exclusion — is therefore reachable only when
`profile == "hst"`.

`tools/test_codegen_profile_isolation.py` proves this differentially rather than
by sampling output strings. Its synthetic ET_EXEC carries two byte-identical
copies of every body: one at the HST guest address and one at a control address
outside every HST table. Under `--profile=none` the two emitted functions must
be identical after normalising each against its own start address, so any
surviving address-coupled site — named in that file or not — makes the pair
diverge. The same run under `--profile=hst` must make every pair *differ*, which
is what keeps the equality above from passing vacuously.

Entry-role and `--static-verify` couplings do not appear in function text and
are asserted directly against `build_entry_catalog` and `sv_plan`.

A static census re-derives the gate inventory from `codegen.py`'s AST. Its scope
is one declared grammar: inside an emitter function, an `if` comparing
`addr`/`a`/`start` with `==` or `in` against either an integer literal or a bare
name. Within that grammar it is complete — a literal of any magnitude must be
`hst_profile`-gated and must have a specimen body, and a named comparator must be
declared either as a title-owned address table or as an image-derived set, with
an undeclared name failing. Mutation tests hold both halves down, including a
sub-`0x1000` literal, since `GUEST_ABORT` already sits at `0x00000a1c`.

The census is a source-shape check over that grammar, not a proof about arbitrary
Python. A coupling written some other way is invisible to it — the
`insns & _SV_SPECIAL` set intersection this slice had to fix is the worked
example, and only the differential catches that shape. The differential is the
load-bearing proof; the census exists so it cannot quietly go out of date.
