# PGD/KIRK constant history scrub — validated component runbook

The PSP KIRK/amctrl constants were removed from the development working tree in historical commit `a2738e0` and now load only from a local gitignored file; see [PGD_KEYS.md](PGD_KEYS.md). They remain recoverable from old Git objects in the **separate historical development archive**. They are not part of this public repository's ancestry.

This document describes the **validated key-specific scrub component for that separate archive**. Do **not** run/push it as a standalone rewrite merely because the procedure is ready. The broader archive history/privacy/proprietary-material audit must first confirm every required removal so the archive pays the disruption of a rewrite only once.

> [!IMPORTANT]
> The public architecture is the established **sanitized public repository (`public-safe-v1`)**, while the separate historical development repository remains private. The rewrite below is for sanitizing that private/archive graph itself if desired or required. It is **not** a mechanism for constructing, replacing, or reconnecting the public repository.

## Known exposure

The constants were introduced in the historical archive's initial import (`7ac90b2 "Moving to GitHub"`). Removing them from a later development tree did not remove the older archive objects. Anyone who already has access to that private archive or an old clone may still possess those values.

They are public PSP platform constants that predate this project, not project secrets that can be rotated. Their removal is nevertheless part of the project's conservative archive/history-hygiene plan because a generic public recompiler need not redistribute crypto constants merely because the archive once contained them.

## Why the key scrub must be combined with the archive privacy audit

A rewrite changes every descendant SHA and disrupts clones/PR references. The archive's full-history secret & privacy audit also records other historical/privacy decisions, including:

- personal author email / AI-session metadata;
- an orphaned/force-pushed commit containing small retail-EBOOT disassembly snippets that remained resolvable by SHA at the time of audit;
- any additional secret/private-path/proprietary object surfaced by the exhaustive history scan.

Rewriting one class today and another tomorrow would pay the disruption twice. Finish the archive audit, build one removal plan, rewrite once.

## Key-specific verification tooling

The values appeared in multiple textual encodings: contiguous hex, C/Python byte arrays, mixed case/spacing and bare small integers. A simple `git log -S` or grep is insufficient.

| Tool | Role | Contains keys? |
| --- | --- | --- |
| `tools/verify_key_scrub.py` | Encoding-aware reachability scan. Exit 3 = key material reachable; exit 0 = absent. | No; reads local key file |
| `tools/gen_key_scrub_spec.py` | Generates `git filter-repo --replace-text` entries for the known encodings. | Script no; generated temporary output yes |
| `tools/test_key_scrub_tools.py` | Hermetic regression tests. | No |

## Archive-only procedure

Run this procedure only in an isolated clone/worktree of the **historical archive**, never in the public repository and never against the public remote.

1. Freeze archive history-changing work and fetch every archive ref intended to be retained.
2. Complete the broader secret/privacy/proprietary-material audit and produce one combined removal specification.
3. Make a verified backup of the private archive before any rewrite.
4. Generate the combined `git filter-repo` replacement/path specification, including the key encodings described by the tooling above.
5. Rewrite the archive once.
6. Run `tools/verify_key_scrub.py` plus the generic secret/privacy/proprietary scans over every retained archive ref/object.
7. Compare the intended sanitized tip tree against the pre-rewrite tip, allowing only the explicitly approved removals/metadata changes.
8. Fresh-clone the rewritten archive and repeat the verification before any decision to publish or retain the sanitized archive remotely.
9. Repair private/archive SHA references as needed.

## What this runbook does not authorize

- It does not authorize exposing the historical archive.
- It does not authorize pushing rewritten archive refs into `Jstar269/nakagawa-recomp`.
- It does not authorize force-pushing public `main`, mirroring refs, or merging unrelated histories.
- It does not turn a successful technical scrub into legal clearance.
- It does not tell a user how to obtain PSP/game cryptographic material.

The established public repository should continue to evolve through its own ancestry and public-safe content. Any private↔public synchronization remains content-only under [`DUAL_REPO_SYNC.md`](DUAL_REPO_SYNC.md).
