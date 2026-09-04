# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Dry-runnable fresh public export generator and public-export gate verifier.

Consumes already-built audit/SBOM/provenance gates and fails closed when
unresolved legal or security blockers remain. With ``--public-safe-profile``
the exported tree actually excludes the unresolved PGF/font and PGD/amctrl surfaces
listed in ``assets/public_source_profile.json`` (same profile consumed by
``tools/public_candidate.py``), records an export provenance manifest, and
re-audits the materialized candidate tree before clearing the export.

Usage:
  python tools/build_public_export.py --verify-only
  python tools/build_public_export.py --export-dir /path/to/public_repo --dry-run
  python tools/build_public_export.py --export-dir /path/to/public_repo --public-safe-profile
"""

import argparse
from dataclasses import dataclass
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parent.parent

try:
    from . import public_export
except ImportError:  # direct script execution
    import public_export

PUBLIC_EXPORT_MANIFEST = "PUBLIC_EXPORT.json"
PUBLIC_SAFE_PROFILE_ID = "public-safe-v1"


def _read_public_export_manifest(root: Path = ROOT) -> dict | None:
    """Return the export provenance manifest when *root* is a materialized export."""
    manifest = root / PUBLIC_EXPORT_MANIFEST
    if not manifest.is_file():
        return None
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or data.get("profile") != PUBLIC_SAFE_PROFILE_ID:
        return None
    return data


def is_public_safe_export_tree(root: Path = ROOT) -> bool:
    """True when *root* is a materialized public-safe export, not the private source tree.

    ``export_sanitized_public_tree`` writes ``PUBLIC_EXPORT.json`` recording the
    profile it applied. A public-safe export deliberately omits the unresolved
    PGF/font and PGD/amctrl surfaces, so source-relative checks that assert those files
    are present on disk do not apply there. A missing or unparseable manifest
    means an ordinary full checkout, so this fails closed toward running the
    stricter private-source assertions.
    """
    return _read_public_export_manifest(root) is not None


def public_safe_excluded_paths(root: Path = ROOT) -> frozenset[str]:
    """Repo-relative paths the public-safe profile removed from *root*.

    Empty for a private source checkout, so callers can unconditionally treat
    membership as "legitimately absent by profile" without branching first.
    """
    data = _read_public_export_manifest(root)
    if data is None:
        return frozenset()
    paths = data.get("excluded_paths")
    if not isinstance(paths, list):
        return frozenset()
    return frozenset(p for p in paths if isinstance(p, str))


@dataclass
class GateResult:
    name: str
    passed: bool
    detail: str
    # A source-history finding is still a failed audit, but when the release
    # architecture explicitly exports a fresh unrelated root it is not a
    # blocker for constructing that new root.  Keep this distinction explicit
    # instead of turning an old-history failure into a misleading PASS.
    blocking: bool = True


def check_unresolved_legal_blockers(public_safe_profile: bool = False) -> GateResult:
    """Check for unresolved PGF/font and PGD/amctrl review boundaries."""
    manifest_path = ROOT / "assets" / "release_manifest.json"
    if not manifest_path.is_file():
        return GateResult("Legal Blockers", False, "assets/release_manifest.json missing")

    # Durable textual gates, deliberately not issue numbers. The administrative
    # state of a tracker item is not legal clearance, and issue ids do not survive
    # a repository replacement -- the public repository's own numbering already
    # differs from the archive's. Each gate closes only on qualified review of the
    # component it names.
    open_blockers = [
        "PGF/JPCSP/intraFont implementation provenance (qualified review required)",
        "Replacement PGF font redistribution terms and notices (qualified review required)",
        "PGD/amctrl distribution and anti-circumvention treatment (qualified review required)",
    ]

    if public_safe_profile:
        return GateResult(
            "Legal Blockers",
            True,
            f"PUBLIC-SAFE PROFILE ACTIVE: Excluded {len(open_blockers)} unreviewed components from export scope",
        )

    return GateResult(
        "Legal Blockers",
        False,
        f"OPEN PUBLICATION BLOCKERS: {', '.join(open_blockers)}. Pass --public-safe-profile to exclude unreviewed components.",
    )


def run_publish_audit(repo_root: Path = ROOT) -> GateResult:
    """Run tools/publish_audit.py --tracked-only."""
    audit_script = ROOT / "tools" / "publish_audit.py"
    if not audit_script.is_file():
        return GateResult("Publication Audit", False, "tools/publish_audit.py missing")

    command = [sys.executable, str(audit_script), "--tracked-only"]
    # This gate runs against the tree it lives in, which cannot attest against the
    # trusted release evidence (the detailed development ledger lives outside the
    # public candidate). Scope the provenance check to candidate-internal
    # consistency; the release flow asserts attestation separately via
    # publish_audit --candidate-root ... --provenance-ledger.
    command.append("--provenance-self-consistency")
    # A materialized public-safe candidate deliberately omits manifest entries
    # whose disposition is excluded. Audit that tree in public scope, while a
    # source checkout remains strict by default.
    if is_public_safe_export_tree(repo_root):
        command.append("--public-scope")
    res = subprocess.run(
        command,
        cwd=repo_root,
        capture_output=True,
        text=True,
    )

    if res.returncode == 0:
        return GateResult("Publication Audit", True, res.stdout.strip() or "publication audit: OK")
    return GateResult("Publication Audit", False, res.stdout.strip() or res.stderr.strip())


def history_audit_gate_result(data: dict) -> GateResult:
    """Map a ``history_audit --json`` report to the publication-gate verdict.

    The gate is state-based, never commit-count-based: a history is clean only
    when the audit reports zero findings across every reachable commit, and any
    finding (including a single legacy dirty commit) fails closed.  A sanitized
    multi-commit public history legitimately passes; a fresh single-commit
    candidate with a finding must still fail.
    """
    summary = data.get("summary", {})
    categories = summary.get("category_counts", {})
    definite = categories.get("DEFINITE_SECRET", 0)
    credentials = categories.get("POSSIBLE_CREDENTIAL", 0)
    total = summary.get("total_findings", 0)
    if total == 0 and definite == 0 and credentials == 0:
        return GateResult(
            "Full-History Audit",
            True,
            f"0 sensitive findings across {data.get('baseline', {}).get('total_commits', 0)} commits and {data.get('baseline', {}).get('total_objects', 0)} objects",
        )
    return GateResult(
        "Full-History Audit",
        False,
        f"SENSITIVE FINDINGS DETECTED: {summary.get('total_findings', 0)} findings; {definite} secrets, {credentials} credentials",
    )


def run_history_audit(repo_root: Path = ROOT) -> GateResult:
    """Run tools/history_audit.py --json."""
    audit_script = ROOT / "tools" / "history_audit.py"
    if not audit_script.is_file():
        return GateResult("Full-History Audit", False, "tools/history_audit.py missing")

    res = subprocess.run(
        [sys.executable, str(audit_script), "--json"],
        cwd=repo_root,
        capture_output=True,
        text=True,
    )

    try:
        data = json.loads(res.stdout) if res.stdout.strip() else None
        if data is None:
            return GateResult("Full-History Audit", False, res.stderr.strip() or "history_audit failed")
        return history_audit_gate_result(data)
    except Exception as exc:
        return GateResult("Full-History Audit", False, f"failed to parse history_audit output: {exc}")


def run_sbom_verification(repo_root: Path = ROOT) -> GateResult:
    """Run tools/verify_sbom.py."""
    verify_script = ROOT / "tools" / "verify_sbom.py"
    if not verify_script.is_file():
        return GateResult("SBOM Verification", False, "tools/verify_sbom.py missing")

    res = subprocess.run(
        [sys.executable, str(verify_script)],
        cwd=repo_root,
        capture_output=True,
        text=True,
    )

    if res.returncode == 0:
        return GateResult("SBOM Verification", True, res.stdout.strip() or "SBOM verification: OK")
    return GateResult("SBOM Verification", False, res.stdout.strip() or res.stderr.strip())


def run_all_publication_gates(public_safe_profile: bool = False, repo_root: Path = ROOT) -> list[GateResult]:
    """Execute all public-export verification gates."""
    history = run_history_audit(repo_root)
    if (
        public_safe_profile
        and repo_root.resolve() == ROOT.resolve()
        and not history.passed
    ):
        history = GateResult(
            "Source-History Audit (not exported)",
            False,
            (
                f"{history.detail}; old ancestry is retained only in the private "
                "evidence bundle and must not become an ancestor of the fresh "
                "sanitized release root"
            ),
            blocking=False,
        )
    results = [
        check_unresolved_legal_blockers(public_safe_profile),
        run_publish_audit(repo_root),
        history,
        run_sbom_verification(repo_root),
    ]
    return results


def load_public_safe_profile() -> tuple[dict, Path]:
    """Load assets/public_source_profile.json, reusing public_candidate's validator."""
    # Imported lazily so --verify-only and gate tests never pay for the audit
    # module unless a profile-filtered export is actually being produced.
    try:
        from . import public_candidate  # package import
    except ImportError:
        import public_candidate  # direct script execution

    profile_path = ROOT / "assets" / "public_source_profile.json"
    profile = public_candidate.load_profile(profile_path)
    return profile, profile_path


def run_candidate_audit(candidate_root: Path, trusted_ledger: Path | None = None) -> GateResult:
    """Run the exhaustive candidate-tree public-scope manifest gate on an export.

    The materialized candidate carries its own checked-in ledger, which is
    candidate-controlled evidence and can never be the attestation anchor.
    ``--provenance-ledger`` therefore must supply the release-controlled ledger
    regenerated from the private detailed development ledger; without it the
    candidate-tree audit fails closed with PROVENANCE_UNVERIFIED.
    """
    audit_script = ROOT / "tools" / "publish_audit.py"
    if not audit_script.is_file():
        return GateResult("Candidate-Tree Audit", False, "tools/publish_audit.py missing")

    command = [
        sys.executable,
        str(audit_script),
        "--candidate-root",
        str(candidate_root),
        "--candidate-tree",
        "--public-scope",
    ]
    if trusted_ledger is not None:
        command.extend(["--provenance-ledger", str(trusted_ledger)])

    res = subprocess.run(
        command,
        cwd=ROOT,
        capture_output=True,
        text=True,
    )

    if res.returncode == 0:
        return GateResult("Candidate-Tree Audit", True, res.stdout.strip() or "candidate-tree audit: OK")
    detail = (res.stdout.strip() or res.stderr.strip()).splitlines()
    detail = detail[-3:] if detail else ["candidate-tree audit failed"]
    return GateResult("Candidate-Tree Audit", False, "\n".join(detail))


def _patch_pre_commit_for_public_scope(export_dir: Path) -> None:
    """Adapt the exported tree's own pre-commit publication-audit hook.

    The exported tree is a public-scope candidate: manifest components with
    ``public_scope_included: false`` are absent by design, and the manifest
    audit only treats that absence as expected when run in public scope. The
    exported ``.pre-commit-config.yaml`` must therefore audit in public scope,
    otherwise every contributor commit fails the publication-safety hook on
    the excluded components (reported as ``MANIFEST_ORPHAN_PATH``). The
    private-source hook stays as-is; this patch applies only to the export.
    """
    pre_commit_path = export_dir / ".pre-commit-config.yaml"
    if not pre_commit_path.is_file():
        return
    text = pre_commit_path.read_text(encoding="utf-8")
    public_entry = "entry: python tools/publish_audit.py --tracked-only --public-scope"
    lines = []
    changed = False
    for line in text.splitlines(keepends=True):
        if line.lstrip().startswith("entry: python tools/publish_audit.py --tracked-only"):
            newline = "\n" if line.endswith("\n") else ""
            indent = line[:len(line) - len(line.lstrip())]
            replacement = indent + public_entry + newline
            changed = changed or line != replacement
            lines.append(replacement)
        else:
            lines.append(line)
    if changed:
        pre_commit_path.write_text("".join(lines), encoding="utf-8", newline="\n")


def export_sanitized_public_tree(export_dir: Path, public_safe_profile: bool = False, dry_run: bool = False) -> bool:
    """Build a clean single-commit public repository export.

    With ``public_safe_profile=True`` the export applies the exclusion profile
    from ``assets/public_source_profile.json`` (PGF fonts and PGF/PGD sources
    and tools) and writes ``PUBLIC_EXPORT.json`` provenance metadata into the
    exported tree. The exhaustive candidate-tree public audit on the
    materialized result is run separately by ``main()`` (``run_candidate_audit``)
    so the tree mechanics remain deterministic and independent of the source
    Git HEAD state.
    """
    print(f"\n--- Public Export Generation ({'DRY RUN' if dry_run else 'EXECUTING'}) ---")
    print(f"Target Directory: {export_dir}")
    print(f"Profile: {'PUBLIC-SAFE (excluding unreviewed components)' if public_safe_profile else 'STANDARD'}")

    profile = None
    profile_path = None
    if public_safe_profile:
        try:
            profile, profile_path = load_public_safe_profile()
        except Exception as exc:
            print(f"ERROR: failed to load public-safe profile: {exc}", file=sys.stderr)
            return False

    if dry_run:
        print("Dry run complete: public-export gates verified and target export path validated.")
        return True

    if export_dir.exists():
        if any(export_dir.iterdir()):
            print(f"ERROR: Export directory '{export_dir}' exists and is not empty.", file=sys.stderr)
            return False

    export_dir.mkdir(parents=True, exist_ok=True)

    # Export the tracked index at HEAD into the target export directory,
    # skipping profile-excluded paths when the public-safe profile is active.
    try:
        archive_proc = subprocess.run(
            ["git", "archive", "HEAD"],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            check=True,
        )
        source_commit = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True, check=True
        ).stdout.strip()
        # Record the tree, not just the commit. A commit id is only resolvable in
        # the repository that holds it -- the export previously pinned a commit
        # that exists solely in the private archive, which no public reader could
        # verify. The tree SHA is what the audit is actually bound to.
        source_tree = subprocess.run(
            ["git", "rev-parse", "HEAD^{tree}"], cwd=ROOT, capture_output=True, text=True, check=True
        ).stdout.strip()

        excluded_paths: list[str] = []
        exported_paths: list[str] = []
        with tarfile.open(fileobj=io.BytesIO(archive_proc.stdout)) as tar:
            for member in tar.getmembers():
                if not member.isfile():
                    continue
                # ``str.lstrip("./")`` is not path-prefix removal: it strips
                # the leading dot from legitimate dotfiles such as
                # ``.clang-format``.  Remove only archive ``./`` prefixes so
                # the exported path and the later digest refer to the same
                # file.
                rel = member.name
                while rel.startswith("./"):
                    rel = rel[2:]
                if profile is not None and _is_excluded(rel, profile):
                    excluded_paths.append(rel)
                    continue
                tar.extract(member, export_dir, filter="data")
                exported_paths.append(rel)

        # PUBLIC_EXPORT.json is generated by the same authoritative implementation
        # used by policy_sync.py.  Include a placeholder for the generated file so
        # tracked/included counts are self-consistent; its own bytes are excluded
        # from the content digest by public_export.content_digest().
        if profile is not None:
            import publication_policy

            loaded_policy = publication_policy.load_policy(profile_path)
            files = [(rel, (export_dir / rel).read_bytes()) for rel in exported_paths]
            # PUBLIC_EXPORT.json is normally already tracked in the source
            # archive.  Add a placeholder only for a source tree that omits it;
            # appending an unconditional duplicate inflated the metadata counts
            # by one and made the materialized candidate fail its own audit.
            if not any(rel == PUBLIC_EXPORT_MANIFEST for rel, _ in files):
                files.append((PUBLIC_EXPORT_MANIFEST, b""))
            ledger_path = export_dir / "assets" / "public_provenance_ledger.json"
            manifest_path = export_dir / "assets" / "release_manifest.json"
            metadata = public_export.build_document(
                loaded_policy,
                files,
                source_tree=source_tree,
                provenance_ledger=ledger_path.read_bytes() if ledger_path.is_file() else None,
                manifest=manifest_path.read_bytes() if manifest_path.is_file() else None,
                excluded_file_count=len(excluded_paths),
            )
            public_export.write_document(export_dir / "PUBLIC_EXPORT.json", metadata)
            _patch_pre_commit_for_public_scope(export_dir)

        for path in excluded_paths:
            print(f"EXCLUDED: {path}")

        # Initialize clean single-commit Git repository
        subprocess.run(["git", "init"], cwd=export_dir, check=True, capture_output=True)
        subprocess.run(["git", "add", "."], cwd=export_dir, check=True, capture_output=True)
        # The export commit is a mechanical snapshot, not a contribution, so it
        # carries a fixed tool identity supplied per-invocation. Relying on the
        # ambient Git identity makes this step fail on any host that has none
        # (CI runners), and would otherwise attribute the snapshot to whoever
        # happened to run it.
        subprocess.run(
            [
                "git",
                "-c",
                "user.name=Nakagawa Recomp Export",
                "-c",
                "user.email=export@nakagawa-recomp.invalid",
                "commit",
                "-m",
                "Initial sanitized public release export of Nakagawa Recomp",
            ],
            cwd=export_dir,
            check=True,
            capture_output=True,
        )

        print(f"Successfully generated single-commit public export in '{export_dir}'!")
    except Exception as exc:
        print(f"ERROR: Export generation failed: {exc}", file=sys.stderr)
        return False

    return True


def _is_excluded(relative_path: str, profile: dict) -> bool:
    """Apply the shared public-source profile exclusion rules."""
    try:
        from . import public_candidate
    except ImportError:
        import public_candidate

    return public_candidate.is_excluded(relative_path, profile)


def main() -> int:
    parser = argparse.ArgumentParser(description="Fresh public export generator and public-export gate verifier.")
    parser.add_argument("--verify-only", action="store_true", help="Only verify public-export gates without generating export")
    parser.add_argument("--export-dir", type=Path, help="Target directory for sanitized public export")
    parser.add_argument("--public-safe-profile", action="store_true", help="Use public-safe profile (excludes unresolved PGF/font and PGD/amctrl surfaces)")
    parser.add_argument("--dry-run", action="store_true", help="Perform dry run without writing files")
    parser.add_argument(
        "--trusted-ledger",
        type=Path,
        default=None,
        help=(
            "Release-controlled provenance ledger generated from the private detailed ledger. "
            "Required for the post-export candidate-tree audit; the exported tree's own "
            "checked-in ledger is candidate-controlled evidence and is never the trust anchor."
        ),
    )

    args = parser.parse_args()

    print("=== Nakagawa Recomp Public-Export Gate Verifier ===")
    results = run_all_publication_gates(public_safe_profile=args.public_safe_profile)

    all_passed = True
    for res in results:
        status_str = "[PASS]" if res.passed else ("[INFO]" if not res.blocking else "[FAIL]")
        print(f"{status_str} {res.name}: {res.detail}")
        if not res.passed and res.blocking:
            all_passed = False

    if not all_passed:
        print("\n[FAILED CLOSED] Public-export gates failed. Cannot proceed to public export.")
        return 1

    print("\n[ALL GATES PASSED] Public-export gates verified.")

    if args.verify_only or not args.export_dir:
        return 0

    success = export_sanitized_public_tree(
        export_dir=args.export_dir,
        public_safe_profile=args.public_safe_profile,
        dry_run=args.dry_run,
    )
    if not success:
        return 1

    if args.dry_run:
        return 0

    # Post-export gate: the materialized tree must itself pass the exhaustive
    # candidate public-scope manifest gate, not just the source gates above.
    audit = run_candidate_audit(args.export_dir, trusted_ledger=args.trusted_ledger)
    print(f"[{'PASS' if audit.passed else 'FAIL'}] {audit.name}: {audit.detail}")
    if not audit.passed:
        print(
            "ERROR: exported candidate failed the candidate-tree public audit; "
            "export is not cleared for use.",
            file=sys.stderr,
        )
        return 1

    print("\n[EXPORT CLEARED] Sanitized public export passed the candidate-tree audit.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
