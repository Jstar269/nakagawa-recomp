# Title Profile and Launch Session Architecture

> **Status: CURRENT.** This page describes the public title-manifest and launch contracts visible in source. Private title manifests, game-derived bindings, and acceptance evidence stay outside the public repository.

## 1. Goal and current evidence

The architecture goal is to keep title identity and title-specific inputs out of generic UI and runtime defaults. Source-owned title data is validated before it reaches the registry, build planner, or launch path.

The current source proves a narrower claim than complete multi-title acceptance: Python and native launch resolution bind a session to a validated title identity, and the test suite covers fail-closed identity/path selection and parity on synthetic fixtures. A real second-title bring-up remains open in [issue #285](https://github.com/Jstar269/nakagawa-recomp/issues/285). The project does not claim that every title can be supported without presentation or integration work.

```text
public title manifest
        │
        ▼
tools/title_manifest.py ──► validated catalog ──► registry / build planner
                                                       │
selected source ──► preparation ──► local manifest ──┤
                                                       ▼
                                      validated launch session
                                                       │
                                                       ▼
                                      child runtime + launch inputs
```

The title catalog, per-user preparation output, and child-process environment are separate contracts. A value may pass from one to another only through the corresponding validator or launch-session builder.

## 2. Public title catalog contract

The authoritative public shape is [`assets/title_manifest.schema.json`](../assets/title_manifest.schema.json), enforced and normalized by [`tools/title_manifest.py`](../tools/title_manifest.py). The source-owned [`display-smoke.json` fixture](../assets/titles/display-smoke.json) is an actual synthetic manifest checked by the manifest tests; it is a better example of the current format than an independent schema sketch in this document. The format rejects unknown fields and separates retail disc identity from the shared synthetic/homebrew shape.

The schema covers manifest identity and kind, retail disc identity, executable layout, module inventory, filesystem roots, game/HLE/code-generation identifiers, feature requirements, compatibility-manifest and verification references, and notes. Optional `runtime_contract` and `profile_zero` blocks have additional machine-checked constraints; `profile_zero` is limited to synthetic manifests. Their exact required fields belong to the schema and validator, not to a second prose-defined schema.

### Typed loose-content roots (#289)

`filesystem.loose_content_roots` optionally declares up to 16 extra host roots
for the runtime's loose-file VFS. Each entry has `root`, `mount`, and
`precedence`, with optional `skip_primary_root` and `exclude`. `root` is a safe
relative path resolved from the parent directory of `filesystem.data_root`; `.`
names that parent. `mount` is an empty string or a guest-relative prefix.
`precedence` is a unique integer from 0 through 65535; the lower number wins
when loose roots expose the same guest file key. `skip_primary_root` defaults
to true exactly for `root: "."`, so the primary data-root child is not walked a
second time. `exclude` lists up to two safe, root-relative paths; matching
directories are skipped before descent. A file under the primary
`filesystem.data_root` wins a duplicate against any loose root, preserving the
pre-migration extracted-tree behavior.
During ISO staging, the player looks for each configured relative root under
PSP_GAME/USRDIR and stages any matching directory at that same relative path.
With no configured roots, it stages the executable only and performs no
neighbor-directory discovery.

Roots that are duplicated or overlap are rejected, including a parent root and
one of its descendants. Unknown keys, absolute paths, traversal, malformed
mounts, and duplicate precedence values also fail validation. Within one loose
root, case-folded duplicate guest-file keys and file/directory key collisions
refuse the index. Duplicate keys within one extracted archive subtree also
refuse the index.

The primary extracted root preserves the legacy resolution for copies of the
same guest key and archive variant from distinct extracted-XB subtrees (for
example, `<archive>.xb.d`). The index retains every copy and sorts equal keys by
variant, root precedence, then bytewise UTF-8 host path; opening the key selects
the first path. This makes the lexically first extracted archive path win
independently of host enumeration order, whether the copies contain identical
or different bytes. A copy in the primary root wins over copies in loose roots.
Packed-archive mode has a separate order: archive paths are sorted bytewise,
then members retain archive insertion order, so the first member with the
selected key and variant wins. Unreadable selected files fail closed.

For example, a synthetic profile can mount two sibling roots into the same
guest namespace:

```json
"loose_content_roots": [
  {"root": "assets_a", "mount": "data", "precedence": 10},
  {"root": "assets_b", "mount": "data", "precedence": 20}
]
```

The private flagship manifest expresses its roots with these same typed entries;
its local manifest and inputs remain outside this repository. A manifest that
omits `loose_content_roots` still loads and normalizes to an empty list. That is
the explicit migration default: no neighboring-directory discovery occurs, so
profiles that need extra roots must add the field before expecting those files
to resolve. The named runtime boundary is **loose-content root binding — in the
works ([#289](https://github.com/Jstar269/nakagawa-recomp/issues/289))**; the
private flagship parity route has not been established by public fixtures.

For a previous layout with `filesystem.data_root` at
`<something>/USRDIR/xbdata` or
`<something>/USRDIR/xbdata_extracted`, the declaration below recreates the
former parent walk and its file set. It contributes files such as
`data/sound/menu.csv` at their original guest paths, skips only the primary
data-root directory while walking the parent, and keeps primary files ahead of
duplicate loose files:

```json
"loose_content_roots": [
  {"root": ".", "mount": "", "precedence": 0, "skip_primary_root": true}
]
```

The old inference activated only for its recognized parent/archive-root
layout. It indexed the primary root, then visited every sibling directory
recursively while skipping that direct primary child. It rejected links,
non-regular entries, and files larger than the guest size limit; it did not
filter packed-archive or module siblings. Leaving `exclude` absent preserves
that traversal. A title profile can exclude additional root-relative subtrees
when its compatibility contract establishes that they are outside the guest
namespace or unnecessary to resolve. For this extracted-tree layout, duplicate
keys from the primary extracted root take priority over adjacent loose files;
the manifest transport now carries that ordering explicitly.

A retail disc may set `disc.require_local_compatibility_record` when its
revision needs an explicit local qualification. That switch contains no retail
hashes: the user's package builder records exact executable/module inputs under
the per-user data root. A missing or changed record is reported as an
`Unqualified revision` boundary; the registry does not select a nearby profile
or infer compatibility from DISC_ID alone. See [issue #315](https://github.com/Jstar269/nakagawa-recomp/issues/315) and the [runtime package identity contract](RUNTIME_PACKAGING_ARCHITECTURE.md#5-local-aot-package-contract-v2).

### Temporary compatibility configuration

The current schema also permits an optional, typed `runtime_bindings` block for specific compatibility seams. These bindings are not structural facts about a disc and do not establish generic PSP semantics. They are bounded configuration consumed by the runtime; `required_runtime_bindings` names the binding families the selected profile depends on so a missing required family fails validation. These mechanisms remain compatibility debt and are expected to retire as the underlying generic behavior becomes correct. [Issue #363](https://github.com/Jstar269/nakagawa-recomp/issues/363) tracks that work; [`TITLE_CODEGEN_PLAN.md`](TITLE_CODEGEN_PLAN.md) documents validation and runtime consumption, and [`PORTING.md`](PORTING.md) inventories the remaining title coupling.

The target remains a generic core that does not require title-specific patches. The current contract records the temporary bindings honestly rather than describing the target state as already achieved. Public schema fields do not authorize private paths, retail bytes, captures, or derived evidence.

The generic dispatcher has no exact or range title-hook table. Historical resource-shaped
targets now reach ordinary lookup and fail at a named interpreter boundary; the runtime
reports the boundary as in the works under [#285](https://github.com/Jstar269/nakagawa-recomp/issues/285).
Unsupported interpreter forms remain fail-closed. HST-specific diagnostic probes in the
HLE, scheduler, and recompiler were removed; use generic SR_TRACE_PC or SR_WATCH
instrumentation when those observations are needed. The init-walker r16 save/restore
no longer names caller addresses in generic dispatch. Real hardware does not restore
$s0-$s7 or $fp/$s8 for a callee, so a generic title keeps every register write its
callee makes. Only the HST title configuration arms a restore of those registers at a
returning CALL (`sr_title_config_preserve_callee_saved_at_calls`), recorded as #363 debt
until the flagship's clobber is root-caused.

The HST Newlib master and guest thread-table addresses are isolated behind the typed
SrTitleReentBindings accessor in src/rt/title_config.c. It returns values only for the
validated hst codegen profile with the matching HST source id. Other profiles receive no
addresses and perform no guest-table seeding.

## 3. Catalog identity and generic launch resolution

The registry maps validated manifest identity to title metadata. Preparation
and launch use that mapping rather than a default title or a title-shaped path
guess. The Python launcher in `tools/nk_core/launcher.py` requires its local
manifest to be a JSON object, binds its declared `title_id` and/or `disc_id` to
exactly one validated registry profile, and checks any restated build name
against that profile. This prepared-game manifest does not go through the
public title-catalog schema; the launcher checks identity and the fields it
consumes. The native launcher in `src/core/nk_launch.c` applies the typed
`NkLaunchSession` contract. Both resolve the selected runtime and image from
validated identity and fail closed when identity or a matching artifact is
missing.

The launcher contract and its synthetic cross-title regressions are tracked by [issue #366](https://github.com/Jstar269/nakagawa-recomp/issues/366). This source-level result does not replace the second-title production route in #285, nor does it prove private retail acceptance.

## 4. Local prepared-game metadata

Preparation writes a local `manifest.json` beside the staged game. This is operational metadata for one prepared copy, not a public catalog manifest. The current preparation code records the schema and engine versions, validated title and disc identities, display metadata, the source ISO path and size, creation time, and the runtime/archive/save settings derived from the selected profile. Because it can contain a user-supplied source path, keep it in local application state and do not copy it into public source or evidence.

The preparation manifest's `engine_version` is emitted from `PREP_ENGINE_VERSION` in `tools/nk_core/prep_engine.py`. Its relationship to project release and package versions is not defined here. [Issue #333](https://github.com/Jstar269/nakagawa-recomp/issues/333) tracks reconciliation of the release manifest, SBOM root, and package/component version sources; it does not establish that this preparation-engine marker is the release version.

## 5. Launch-session transport

Both launchers form runtime inputs for a child process from the selected title and prepared-game data. The native path uses the typed `NkLaunchSession` in `src/core/nk_launch.h`; the Python path binds the local manifest's identity to the registry and checks the fields it consumes, as described above. The launchers derive runtime values such as `PSP_ISO`, `SR_DATAROOT`, frame-rate and graphics settings, and diagnostic flags from the session and its configuration, then pass them through the child environment.

Environment variables are the process transport; they are not the source of title identity. The Python launcher starts with a copy of the host environment, removes any inherited `PSP_ISO`, and adds a resolved ISO path only when the local manifest supplies a nonempty path that passes its file checks. The native launcher supplies `PSP_ISO` for every session: the resolved path when available, or an empty value that masks the parent's key when no ISO was resolved. Both native platform backends overlay that session value onto their inherited environment. The runtime ISO reader uses only a nonempty `PSP_ISO`, so a staged-EBOOT session without a resolved ISO cannot select media from the parent environment. Other parent variables remain available to the child. This isolation is covered by [issue #391](https://github.com/Jstar269/nakagawa-recomp/issues/391).

For the native player’s current implementation boundary and process lifecycle, see [`NATIVE_PLAYER_ARCHITECTURE.md`](NATIVE_PLAYER_ARCHITECTURE.md) and [`RUNTIME_PACKAGING_ARCHITECTURE.md`](RUNTIME_PACKAGING_ARCHITECTURE.md). Those pages describe a prepared-runtime launch path; they do not claim arbitrary-ISO package provisioning is complete.

## 6. Evidence and remaining boundaries

- Manifest structure is enforced by the shared schema/validator and public fixture tests.
- Python/native identity and path resolution have source-owned parity and fail-closed regression coverage under #366.
- A real second-title bring-up remains pending under #285.
- Temporary profile-owned compatibility bindings remain visible debt under #363.
- HST reent-table configuration remains title-gated compatibility debt under #363; generic
  dispatch rejection and diagnostic cleanup do not establish full second-title support.
- Release and preparation version authority remains unresolved under #333.
- Private title inputs and private acceptance remain separate from public schema and synthetic-fixture results.
