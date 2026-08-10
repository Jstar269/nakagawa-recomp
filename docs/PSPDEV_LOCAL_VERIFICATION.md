# PSPDEV integration: local verification and continuation

This document is the exact boundary between the remote, source-only work in the
first PSPDEV integration slice and the operations that require a local checkout,
a PSPDEV installation, or controlled downloads.

## Completed immutable baseline

The `v20260501` local evidence gate is complete in
`assets/upstream/pspdev.evidence.json`:

- the official Debian release asset was downloaded and its 172,418,140-byte
  payload independently matched GitHub's published SHA-256;
- the official Docker Hub `v20260501` tag was pulled by immutable digest and
  inspected as `linux/amd64`;
- all 13 required/optional tools were executed inside that exact image and their
  executable sizes, SHA-256 values, and bounded identity output were recorded;
- `assets/upstream/pspdev.lock.json` now passes `--require-local`.

This proves the locked distribution and tool identities. It still does not
assert that the independently pinned component repository heads are the exact
sources used to build the release artifacts; the lock preserves that boundary.

## Current remote-safe scope

The first implementation slice is intentionally limited to:

- an exact source/provenance lock;
- strict offline lock validation;
- a bounded local executable probe that writes only under `build/audit/`;
- a read-only PSPSDK import/prototype extractor;
- a deterministic comparison with Nakagawa's authoritative HLE manifest;
- tests using source-owned synthetic declarations.

It does **not** install PSPDEV, download release archives, mutate HLE code, build
homebrew binaries, contact a PSP, or commit generated ELF/PRX/PBP/SFO files.

## Required local prerequisites

Use a fresh worktree based on the implementation PR's exact head. Keep the normal
HST build independent from PSPDEV.

Required software for the first local verification:

- Python 3.14.x;
- Git;
- a PSPDEV installation or a clean PSPSDK checkout;
- optionally Docker/Podman or a downloaded PSPDEV release archive for artifact
  digest verification.

Do not place an installed SDK, package cache, firmware, keys, retail files,
hardware dumps, or private HST evidence inside the Git worktree.

## Phase 1: repository-only gates

Run from the repository root:

```powershell
python -m unittest tools/test_pspdev_lock.py -v
python -m unittest tools/test_pspdev_probe.py -v
python -m unittest tools/test_pspsdk_sync.py -v
python -m unittest discover -s tools -p "test_*.py" -v
python tools/pspdev_lock.py
python tools/publish_audit.py --tracked-only
python -m pre_commit run --all-files
git diff --check origin/main...HEAD
```

Expected behavior:

- the lock validates but reports pending local artifact/tool evidence;
- `--require-local` fails until the lock is deliberately completed;
- no command accesses the network;
- no command modifies tracked files;
- no private/game-derived data appears in reports.

## Phase 2: verify the pinned PSPSDK checkout

Prepare a clean checkout of the exact commit recorded in
`assets/upstream/pspdev.lock.json`. Then run:

```powershell
python tools/pspsdk_sync.py `
  --pspsdk-root C:\path\outside\repo\pspsdk `
  --manifest-out build/audit/pspsdk-platform-manifest.json `
  --comparison-out build/audit/pspsdk-nakagawa-comparison.json `
  --report build/audit/pspsdk-nakagawa-comparison.md
```

The default verification must prove:

- `git rev-parse HEAD` equals the full pinned commit;
- the PSPSDK checkout has no tracked or untracked changes;
- all scanned source files remain inside the checkout and are not symlinks;
- every `IMPORT_START` and `IMPORT_FUNC` occurrence matches the supported narrow
  grammar;
- duplicate and conflicting declarations fail closed.

Do not use `--allow-unverified-source` for acceptance evidence. That flag exists
only for synthetic tests and explicitly weaker archive investigations.

Review every non-exact comparison category. PSPSDK is declaration evidence, not
a hardware-semantic oracle. Do not automatically rewrite NIDs, names, handlers,
prototypes, structures, or implementation status.

## Phase 3: capture local tool identities

Run:

```powershell
python tools/pspdev_probe.py --out build/audit/pspdev-tool-probe.json
```

Use `--require-all` only on a machine expected to contain every listed optional
tool. Absolute paths are excluded by default; `--include-paths` is for private
local diagnosis and its output must never be committed.

The probe report is evidence input, not an automatic lock update. Review:

- resolved executable basenames;
- executable SHA-256 values and sizes;
- version/help output and return codes;
- missing, timed-out, truncated, or nonzero results;
- whether wrappers resolve to the intended PSPDEV installation.

A local model may add a separate, reviewed command to merge approved probe fields
into the lock, but that command must fail closed and must not silently replace
source pins.

## Phase 4: resolve release/container digests

The source lock deliberately leaves release archive and container digests null.
A local model may resolve them only from official PSPDEV release/package sources.
Record:

- exact download URL or registry reference;
- size;
- SHA-256 or immutable container digest;
- retrieval time;
- signature/attestation information when available;
- relationship to the independently pinned component source commits.

Do not claim that the component heads built a release artifact unless the
upstream build metadata proves that relationship.

## Hard stop before fixture generation

After phases 1-4, stop and review the PR before adding synthetic PSP binaries.
Nakagawa's publication audit currently rejects `.elf`, `.prx`, `.pbp`, and `.sfo`
files by extension. Do not weaken that gate casually.

The later fixture PR must first define a narrowly scoped exception or a
source-only regeneration policy, with per-artifact manifests containing source
hashes, exact toolchain identity, commands, output hashes, licensing, expected
parser behavior, and a statement excluding Sony SDK/firmware/retail material.

## Required local Codex review questions

Before the implementation PR can be marked ready, a local reviewer must answer:

1. Does the lock match the exact official sources and licenses actually used?
2. Are the lock validator and source scanner fail-closed under malformed JSON,
   malformed assembly, symlinks, untracked files, oversized inputs, timeouts, and
   output floods?
3. Does the PSPSDK parser account for every relevant import macro form in the
   pinned source, rather than silently skipping a form?
4. Are extracted prototypes clearly declaration evidence and sufficiently
   narrow to avoid misclassifying macros, inline definitions, or call sites?
5. Does comparison preserve all Nakagawa HLE status/classification distinctions?
6. Are reports deterministic, bounded, path-safe, and free of HST/private data?
7. Do all repository-wide Python, publication-audit, pre-commit, and diff gates
   pass on the exact head?
8. Is the PR still read-only/report-only with zero runtime or HLE semantic edits?

## Evidence to return

Return an exact-head report containing:

- base and head SHAs;
- changed files;
- PSPDEV/PSPSDK source pins used;
- complete commands and exit codes;
- focused and aggregate test counts;
- local tool probe summary;
- PSPSDK extraction statistics and comparison-category counts;
- every failure, skip, unsupported macro form, and unresolved license field;
- confirmation that no private/game-derived material was read or pushed;
- `PRIME-TIME READY: YES` or `NO` under the project's engineering definition.

Do not push generated audit outputs. Do not merge. DCO is intentionally not
enforced during the current private-development phase.
