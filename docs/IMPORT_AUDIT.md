# Import-coverage and fake-success audit (historical tracker item #71)

> **Tracker numbering.** Bare `#N` references in this document are
> **pre-republication tracker numbers**. GitHub numbers issues and pull requests from one
> sequence, and the sanitized public repository restarted that sequence, so a number here
> may now resolve to an unrelated live public object. Read every bare number below as a
> historical identifier unless it is written as an explicit link.

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
| `tools/hle_manifest.py --evidence-chain` | Joins each registration to the evidence behind it (see [Evidence chain](#evidence-chain)). Adds no tracker; reuses `nid_name_proof.py`, `intr_conformance.h`, the HLE selftest, and the PSP-oracle manifest. |
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

## Evidence chain

Classification answers *what kind of handler is registered*. It does not answer
*what evidence stands behind that registration*, and the two get conflated: a
`dedicated` handler with no test is routinely read as covered, while a test that
enters `h_ok` is routinely read as coverage.

The evidence chain makes both explicit. It adds no tracker and re-derives
nothing — it joins the tools that already own each link:

```bash
python tools/hle_manifest.py --evidence-chain build/hle_evidence_chain.json
```

| Link | Owned by |
| --- | --- |
| canonical name → independently derived NID | `tools/nid_name_proof.py` (`nid == sha1(name)[0:4]` little-endian) |
| imported NID | `--imports <manifest>`; retail import manifests are private inputs, so this link is **opt-in** |
| registered handler + classification + status | `tools/hle_manifest.py` extraction and `tools/hle_registry_meta.py` |
| production dispatch reachability | conditional-scope analysis over `src/rt/hle.c` |
| conformance cell | `src/rt/intr_conformance.h` (`kIcMatrix`) |
| executable dispatch | `src/rt/hle_thread_selftest.c` |
| hardware exercise | `tools/psp_oracle/manifest.json` |

### Production reachability

A registration inside a helper inherits the scopes of that helper's call sites.
That is what turns a shared `hle_register_*_handlers()` into evidence that
`sr_hle_init()`'s production branch really reaches it, rather than evidence that
the text exists somewhere in the file. A registration reachable only under
`SR_HLE_THREAD_SELFTEST` is reported in `summary.not_reachable_from_production`
— a test-only registration must never be presented as production evidence.

Only a conditional that actually tests the selftest macro narrows the scope. A
registration guarded by an unrelated `#ifdef` still ships, and treating it as
test-only would understate production reachability.

### Tiers

| Tier | Assigned when |
| --- | --- |
| `HARDWARE_MEASURED` | a source-owned PSP probe calls this API on hardware |
| `HOST_TESTED` | an executable test enters a **dedicated** handler |
| `STATICALLY_SUPPORTED` | dedicated handler registered, no executable coverage |
| `NOT_EVIDENCE` | a generic success stub, whether or not a test enters it |

Deliberately conservative in two places. Hardware truth transcribed into
`intr_conformance.h` describes the PSP, not this runtime, so a conformance cell
never promotes a registration past `HOST_TESTED`. And `HARDWARE_MEASURED`
describes the *API exercise*, not the correctness of this handler.

`summary.exercised_stubs` lists NIDs an executable test enters that still
dispatch to a generic success handler. Those are tiered `NOT_EVIDENCE`, not
`HOST_TESTED`: entering `h_ok` proves the registry resolves, and nothing more.
On current `main` the list is `sceKernelLockMutex` (`0xb011b11f`) and
`sceKernelLockMutexCB` (`0x5bf4dd27`), both still on `h_ok` while the #88
conformance matrix probes them.

Without `--imports` the imported link records
`"unknown: no import manifest supplied"`. It is never reported as "this NID is
not imported" — an absent private input is not a negative result.

### Triage

The chain leaves a large `STATICALLY_SUPPORTED` population, and a flat list of a
few hundred NIDs is not actionable. `--triage-top N` (default 30) ranks the
registrations that carry **no** executable evidence:

```bash
python tools/hle_manifest.py --evidence-chain build/hle_evidence_chain.json --triage-top 20
```

Every score component is emitted alongside the score, so a ranking can be argued
with rather than merely accepted: module family size, how many public test files
mention the name or NID (saturating at three, so one chatty API cannot dominate),
and whether the registration is an unexercised generic stub. A stub nothing
exercises outranks a dedicated handler nothing exercises, because it silently
reports success.

A public test *mentioning* a NID is a reference, not a test of the API. It ranks
attention and never promotes a tier; the chain's `exercised` links are what carry
coverage, and they are computed separately.

This reincorporates the census concept from PR #76 into already-tracked tooling,
**minus** that proposal's curated per-module weight table. Those weights are
unsourced editorial judgement, and an unattested judgement is exactly what a
provenance-blocked tool should not carry into the tree.

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
note on the historical tracker item #71).
It now routes to `h_SetCompiledSdkVersion` under its canonical name, and both
waivers were retired in the same change. The alias rule and canonical-name
map in `tools/hle_registry_meta.py` remain, so a regression of either fix
fails the gate; `tools/test_sdkver_c.py` additionally executes the
retained-state contract (`src/rt/sdkver_selftest.c`).
