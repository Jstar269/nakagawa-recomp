# 0004: Provenance is recorded per path

Date: 2026-10-29
Status: Accepted (owner); migration in progress

## Context

Every public file has a provenance record: where it came from, the evidence and a hash of its bytes. Some older
records cover a whole folder with a pattern. A pattern cannot say where one particular file came from, and it
quietly covers files added later.

## Decision

Each public path gets its own record. A new implementation file is admitted only with a record that names it.
Pattern records are being replaced by per-path records.

## Consequences

- Adding a file takes an admission step; the step is scripted (`tools/admit_paths.py`).
- A file's origin can be read from its own record, without guessing which pattern covers it.
- Until the migration ends, some existing files are still covered by pattern records.

## Links

- [`docs/PROVENANCE_MERGE_GATE.md`](../PROVENANCE_MERGE_GATE.md) and
  [`docs/PUBLICATION_READINESS.md`](../PUBLICATION_READINESS.md).
