# PGD/KIRK constant history scrub — validated component runbook

The PSP KIRK/amctrl constants were removed from the development working tree in historical commit `a2738e0` and now load only from a local gitignored file; see [PGD_KEYS.md](PGD_KEYS.md). They remain recoverable from old Git objects in the **separate historical development archive**. They are not part of this public repository's ancestry.

This document describes the **validated key-specific scrub component for that separate archive**. Do **not** run/push it as a standalone rewrite merely because the procedure is ready. The broader archive history/privacy/proprietary-material audit must first confirm every required removal so every required removal can be handled in **one coordinated rewrite**.

> [!IMPORTANT]
> The public architecture is the established **sanitized public repository (`public-safe-v1`)**, while the separate historical development repository remains private. The rewrite below is therefore for sanitizing that private/archive graph itself if desired/required; it is not the mechanism for constructing, replacing, or reconnecting the public repository.

## Known exposure

The constants were introduced in the historical archive's initial import (`7ac90b2 "Moving to GitHub"`). Removing them from a later development tree did not remove the older archive objects. Anyone who already has access to the private archive or an old clone may still possess those values.

They are public PSP platform constants that predate this project, not project secrets that can be rotated. Their removal is nevertheless part of the project's conservative archive/history-hygiene plan because a generic public recompiler need not redistribute crypto constants merely because the archive once contained them.

## Why the key scrub must be combined with full-history privacy audit

A rewrite changes every descendant SHA and disrupts clones/PR references. The archive's full-history secret & privacy audit also records other historical/privacy decisions, including:

- personal author email / AI-session metadata;
- an orphaned/force-pushed commit containing small retail-EBOOT disassembly snippets that remained resolvable by SHA at the time of audit;
- any additional secret/private-path/proprietary object surfaced by the exhaustive history scan.

Rewriting the keys today and another class tomorrow would pay the disruption twice. Finish the archive audit, build one removal plan, rewrite once.

## Key-specific verification tooling

The values appeared in multiple textual encodings: contiguous hex, C/Python byte arrays, mixed case/spacing and bare small integers. A simple `git log -S` or grep is insufficient.

| Tool | Role | Contains keys? |
| --- | --- | --- |
| `tools/verify_key_scrub.py` | Encoding-aware reachability scan. Exit 3 = key material reachable; exit 0 = absent. | No; reads local key file |
| `tools/gen_key_scrub_spec.py` | Generates `git filter-repo --replace-text` entries for the known encodings. | Script no; generated temporary output yes |
| `tools/test_key_scrub_tools.py` | Hermetic regression tests. | No |

The key-only procedure was dry-run validated on a throwaway mirror on 2026-07-22: all known encodings were removed and the current tip tree remained byte-identical. That validates the **key transform**, not the completeness of the full-history audit.

## Prerequisites for the final combined rewrite

- Full-history audit complete and every history/privacy disposition recorded.
- archive development frozen for the rewrite window;
- `git-filter-repo` installed (Git ≥2.24);
- local `keys/pgd_keys.txt` available to the verification/generator tools and still ignored;
- an explicit list of refs intended to survive in the private archive;
- a secure offline backup plan. The backup itself contains the material being removed and must never be uploaded or used as a future public source.

## 1. Verify current exposure and record the pre-rewrite tip

Run this step in the **historical archive**, not in the public repository:

```bash
cd "$REPO"
python tools/verify_key_scrub.py          # archive currently expected: exit 3 / reachable
PRE_HEAD=$(git rev-parse HEAD)
PRE_TREE=$(git rev-parse HEAD^{tree})
PRE_COUNT=$(git rev-list --count --all)
```

Create a recoverable backup only in a protected offline location:

```bash
git bundle create "$SECURE_BACKUP/pre-scrub-private.bundle" --all
```

That bundle deliberately contains the old sensitive/proprietary history. Treat it as private archival material, not a distribution artifact.

## 2. Generate the key replacement fragment outside the repository

```bash
python tools/gen_key_scrub_spec.py --out "$TMP/pgd-key-replacements.txt"
```

The output contains the constants. Never put it inside the repository, logs, issue comments or cloud/public artifacts. Delete it after use.

## 3. Construct the **combined** filter-repo plan

Merge the key replacement fragment with every other transformation the archive's full-history audit requires. Depending on the completed audit, that can include path/blob removal, commit-message replacements and/or mailmap/identity decisions.

Review the complete plan before running it. Do not blindly remove historical material merely because it is embarrassing; every transformation should correspond to a recorded legal/privacy/security disposition.

## 4. Rewrite a fresh private mirror

```bash
git clone --mirror "$REPO" "$TMP/scrub.git"
cd "$TMP/scrub.git"
# Run the reviewed combined git-filter-repo command/spec here.
# The key-only fragment uses:
git filter-repo --force --replace-text "$TMP/pgd-key-replacements.txt" <other-reviewed-options>
```

The literal command must be frozen in the audit plan before the destructive run. Do not copy the placeholder `<other-reviewed-options>` blindly.

## 5. Verify before any shared update

From inside the rewritten mirror:

```bash
SR_PGD_KEYS="$REPO/keys/pgd_keys.txt" \
  python "$REPO/tools/verify_key_scrub.py" --keys "$REPO/keys/pgd_keys.txt"
# expect exit 0
```

Also run the completed secret/proprietary/privacy scanners. Verify intended ref/commit counts and, critically, that the archive's intended tip source tree is unchanged unless the audit explicitly required a tip-tree edit:

```bash
git rev-parse HEAD^{tree}
git rev-list --count --all
```

Compare with the recorded pre-rewrite expectations. A key/history scrub must not silently alter source semantics.

## 6. Update the **private archive** only after every verifier passes

A mirror force-update is appropriate only if the owner intentionally wants the private archive's historical refs rewritten and has reviewed exactly which refs will survive:

```bash
git remote add origin <PRIVATE_ARCHIVE_REMOTE>
git push --force --mirror origin
```

Temporarily changing branch/ruleset protection is an owner-controlled maintenance action; restore it immediately afterward.

This command is **not** used to update or create the public repository.

## 7. Relationship to the established public repository

The sanitized public repository already exists. Do **not** create another public history from this runbook, push an archive mirror into it, or connect the two histories. Continue evolving public `main` through its own ancestry and use the content-only synchronization contract in [`DUAL_REPO_SYNC.md`](DUAL_REPO_SYNC.md).

If archive work later reveals a generic, public-safe correction that belongs publicly, reproduce/backport the content under that contract rather than transferring private commits or refs.

GitHub documents that changing a private repository to public exposes Actions history/logs and disables push rulesets, another reason not to flip the archive's visibility.

## 8. Cleanup and post-rewrite validation

```bash
rm -f "$TMP/pgd-key-replacements.txt"
rm -rf "$TMP/scrub.git"
```

Re-clone the rewritten private archive for archive development rather than continuing from an old graph. Old local clones/backups still contain pre-rewrite objects; keep only deliberately protected archival copies and never push them into sanitized/public remotes.

On the fresh archive clone, run the key/history verification appropriate to that archive and the complete scanners. If public-safe content must subsequently be synchronized, use [`DUAL_REPO_SYNC.md`](DUAL_REPO_SYNC.md) and verify the public repository independently.

## GitHub cached/orphaned object caveat

A force-push is not a guarantee that every old object/reference immediately becomes inaccessible on GitHub. Old PR refs, forks/clones, issue links and cached SHA views can outlive branch reachability. GitHub Support has specific sensitive-data-removal procedures; do **not** assume arbitrary public-known PSP constants or proprietary snippets qualify for support purging. The separate sanitized-public-repository strategy avoids relying on old-object garbage collection for public cleanliness.

## 2026-08-04 technical scan checkpoint

This is a read-only checkpoint for the historical archive's full-history audit. It does not rewrite, delete, or publish any ref.

- Temporary scanner: Gitleaks v8.30.1 for Windows x64, downloaded from the
  official gitleaks/gitleaks v8.30.1 release. The downloaded archive SHA-256
  was verified as
  d29144deff3a68aa93ced33dddf84b7fdc26070add4aa0f4513094c8332afc4e against
  the release checksums file.
- Reachable/ref scope command (run from the isolated campaign worktree):

  ```bash
  gitleaks git --redact --log-opts="--all --reflog" --report-format json \
    --report-path <temporary-outside-repository-file> <repository>
  ```

  The scan reported 612 commits, approximately 9.08 MB, and no leaks
  (exit 0; the JSON report was an empty array).
- The known orphan object
  d9d5484521b2253cece4830c1a0f748c59cf7724 is still addressable locally as a
  commit (parent 978cd4f7179a7068d074b07fa9117009daf48a55, dated 2026-07-24).
  It was scanned separately with Gitleaks' no-walk commit scope and by
  archiving its tree to temporary local space; both scans reported no leaks
  (exit 0). No file contents or retail-derived text are reproduced here.
- The ref inventory at that checkpoint was broad: 106 refs total (80 local heads,
  12 remotes, 6 tags, 2 temporary refs, plus archive/Codex/Copilot refs) and
  94 distinct ref tips. Git reported 550 commits under `--all` and 698 under
  `--all --reflog`. These are dated inventory facts, not a recommendation to delete refs.

The scan is technical secret-scan evidence, not a complete legal or proprietary-material disposition. Any archive rewrite still requires an owner-approved combined history plan, including the known key/object exposure and metadata decisions. It does not authorize a publication-architecture change.

## Rollback

Before the private remote force-update, delete the throwaway mirror and stop. After a shared archive rewrite, the protected offline bundle can reconstruct the prior private graph:

```bash
git clone "$SECURE_BACKUP/pre-scrub-private.bundle" recovered
```

Restoring that graph also restores the material intentionally removed. It is emergency private recovery only.
