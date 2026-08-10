# Coverage ledger — issue #179

Issue [#179](https://github.com/Jstar269/nakagawa-recomp/issues/179) requires a
tracked, per-file audit record. This ledger extends that record for every file
inspected in a session; it is not a claim of defect absence. Each entry
records the exact reviewed state and the disposition.

**Baseline commit for the first audit pass:** `7199d82`.
**This session (2026-08-06):** NID→name→signature→handler integrity audit
(issues 75/78/83/86) plus tracker reconciliation (72/74/89/139/296/304 and dashboard drift). Review evidence: source inspection, the HLE
registration manifest, the generated `nid_names.h` table, and the unit/gate
suites. No private game inputs were read.

Files whose reviewed content changed after this entry must be marked stale and
re-reviewed by diff.

## Per-file records

### `src/rt/hle.c` — registration surface + handlers (audit pass 2026-08-06)

1. **Path/blob:** `src/rt/hle.c`; reviewed ranges: registration block
   (≈7500–7960), `sr_hle_register`/`hle_find` (156–185), display handlers
   (5805–6140), I/O handlers (4700–5110), SAS handlers (6755–6820), power
   handlers (853–905).
2. **Role/trust:** host runtime; the NID registry is the guest→host ABI.
3. **Inputs:** guest-controlled NIDs/arguments; maintainer-controlled
   registrations.
4. **Memory-safety:** display/I/O handlers preflight guest spans
   (`sr_guest_span_writable`, `display_address_valid`, 16-byte alignment);
   no new native buffers in the reviewed surface.
5. **State/semantic:** registration labels corrected to the canonical table
   (see `docs/NID_INTEGRITY_AUDIT_2026-08-06.md`); handler behavior unchanged
   by the routing fix; residual handler-shape gaps tracked by the four issues.
6. **FS/process/network:** n/a for this pass.
7. **Parser:** the registration text is parsed by `tools/hle_manifest.py`
   (fail-closed; see that entry).
8. **Build/supply-chain:** n/a.
9. **Legal:** SPDX header unchanged; no new copied material.
10. **Tests/evidence:** `tools/test_hle_manifest.py` locks every corrected
    NID/name/handler pair; `import_audit_gate.py` runs on the live manifest.
11. **Disposition:** corrected routing applied; **stale if** any registration
    block is edited without re-running the manifest gate.

### `src/rt/nid_names.h` — generated canonical table (usage, 2026-08-06)

Generated from `tools/nid_corpus.json` (do not hand-edit); 1,638 entries.
Independently verified by `tools/nid_name_proof.py` (corpus↔header, NID
derivation as `sha1(name)[0:4]` little-endian). Used as the authoritative
reference for the exhaustive name-integrity pass. **Disposition:** clean for
its stated boundary; regenerated via `tools/gen_nidnames.py` only.

### `tools/nid_corpus.json` — PPSSPP-derived corpus (usage, 2026-08-06)

Pinned derivation (PPSSPP `Core/HLE` tables at `f0c28c67`, PSPSDK hash-
verified subset); counts: 840 pspsdk-sourced / 623 hash-verified /
54 library-attributed / 10 editorial-alias / 88 unresolved. Note for future
work: `0xD5EBBCDC` (likely `__sceSasSetSteepness`) has no tracked entry; add
with provenance if the SAS surface is implemented (#75).

### `tools/hle_manifest.py` — fail-closed registration extractor (full, 2026-08-06)

Extraction equivalence guarantees (every `sr_hle_register(` occurrence
accounted for; duplicates and undefined handlers rejected); `#if 0` and
comment hygiene; curated-metadata cross-checks; **new:** exhaustive
`load_canonical_names()` cross-check against `nid_names.h`, `KNOWN_NID_ISSUES`
validation, `RETIRED_NIDS` re-registration rejection. Deterministic sorted
ASCII/LF output. **Disposition:** clean; covered by 34 unit tests +
`import_audit_gate.py`.

### `tools/hle_registry_meta.py` — curated classification metadata (full, 2026-08-06)

`GENERIC_SUCCESS_HANDLERS`, `HANDLER_STATUS`, `ALIAS_RULES` (#71), extended
`KNOWN_NID_NAMES` (15 canonical labels), `KNOWN_NID_ISSUES` (issue
attribution), `RETIRED_NIDS` (4 fabricated power NIDs, #86). Every entry is
cross-checked both directions by `hle_manifest.py`. **Disposition:** clean.

### `tools/import_audit_gate.py` — public CI gate (full, 2026-08-06)

Findings-vs-waivers exact-balance enforcement; baseline drift detection with
downgrade call-outs; malformed-fixture matrix; **new:** retired-NID handling
so a reviewed removal of a fabricated NID is not reported as a coverage
regression, while re-registration fails via `hle_manifest.py`. Public-input
only. **Disposition:** clean; gate passes (371 registrations, 0 findings, 16
fixtures rejected cleanly).

### `tools/test_hle_manifest.py` — manifest tests (full, 2026-08-06)

34 tests: extraction forms, conditional/comment hygiene, live-manifest
classification, #71 SDK-version alias rule, the new per-issue NID regressions
(#75/#78/#83/#86), exhaustive table cross-check cleanliness, baseline
reproducibility, determinism. **Disposition:** clean; `python -m unittest
test_hle_manifest` → OK.

### `tools/import_audit_baseline.json` — committed classification baseline (2026-08-06)

Refreshed with `hle_manifest.py --write-baseline` after the routing fix;
diff reviewed: name-only updates plus the 4 canonical-NID key replacements
(dedicated→dedicated) and the 4 fabricated-NID removals (now `RETIRED_NIDS`).
No classification downgrades. **Disposition:** current; any future
classification change requires a reviewed refresh.

### `tools/nid_auditor.py` — legacy regex scraper (inspected, 2026-08-06)

Superseded by `tools/hle_manifest.py`; the dashboard's legacy route was
already corrected under #181. Not used by any gate. **Disposition:** legacy;
candidates for deletion are the maintainer's call (it is still referenced by
docs/IMPORT_AUDIT.md).

### `docs/PSP_ISSUE_MATRIX.json` — issue snapshot (reviewed, 2026-08-06)

Snapshot was dated 2026-08-05 at `main` `29fe4902`; regenerated this session
from live GitHub state (see the regeneration entry below). Generated artifact,
not closure evidence.

### `docs/provenance/INDEPENDENCE_BACKLOG.md` — IND rows (reviewed, 2026-08-06)

IND-2 status updated: VFPU register addressing now hardware-verified
(HQ-1/#296 closed, verifier committed), which the row previously marked
"probe filed".

## Cross-file passes performed

1. **HLE NID integrity (full):** every registered NID compared against
   `nid_names.h`; pre-fix 28 mismatches + 4 fabricated NIDs, post-fix 0;
   now enforced by the gate (see the audit doc).
2. **NID-signature shape review (four subsystems):** the corrected
   registrations' handlers were read for argument layout; residual shape
   gaps are attributed to #75/#78/#83/#86 in the audit doc.
3. **Missing-NID policy:** registrations whose NID is absent from the table
   were enumerated (7 at audit time); all are HST/synthetic names or the
   untracked `0xD5EBBCDC` — recorded, not silently accepted.
4. **Tracker truth:** ISSUES.md dashboard entries, IND-2 row, and the
   priority issues' on-GitHub state reconciled against merged work
   (see the audit doc "Related tracker reconciliations").

## Open items surfaced by this pass

- `0xD5EBBCDC` canonical name needs corpus provenance work (#75-adjacent).
- `tools/nid_auditor.py` deletion or decommission decision (docs/IMPORT_AUDIT
  still references it).
- PSP_ISSUE_MATRIX regeneration is a generated artifact; re-run before the
  next hardware session.

## Reconciliation note — 2026-08-10 (recovery into current `main`)

This ledger was recovered from an uncommitted 2026-08-06 record. Re-verified
against current `main` (`e0aa4e28`): the reviewed `hle.c` registration surface,
the generated `nid_names.h` (1,638 entries), the `KNOWN_NID_NAMES`/`KNOWN_NID_ISSUES`
curated maps, and the residual gaps recorded above all still match `main`.

Two mechanism claims in the per-file records above are stale by design choice
on `main` and are corrected here rather than in the dated record:

- `tools/hle_manifest.py` — the "**new:** exhaustive `load_canonical_names()`
  cross-check ... `RETIRED_NIDS` re-registration rejection" clause was **not
  adopted**. On `main` the exhaustive table cross-check is enforced in
  `tools/test_hle_manifest.py` (`REVERSE_NID_DEBT` + minimum-parse guard) and
  the curated `KNOWN_NID_NAMES` map is the `mislabeled_nid` finding source;
  there is no retired-NID registry.
- `tools/import_audit_gate.py` — the "**new:** retired-NID handling" clause
  was not adopted; `main` refreshes `tools/import_audit_baseline.json` (PR
  #322) and requires reviewed refreshes, without a retired-NID concept.

The ledger's per-file inspection record (ranges, trust, memory-safety notes,
dispositions) remains valid as dated evidence. Files whose reviewed content
changes after this note must be marked stale and re-reviewed by diff.
