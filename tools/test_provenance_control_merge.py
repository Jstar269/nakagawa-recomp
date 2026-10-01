# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The committed provenance controls must not conflict across independent changes.

Every merge to the default branch used to rewrite the same lines of
``PUBLIC_EXPORT.json``: the included-content digest, the file counts, and the
digest of the provenance ledger blob.  Each of those describes the whole tree at
one instant, so any merge changed them, and every open pull request that carried
its own copy then conflicted -- which is why refreshing those two files needed
private tooling on every single pull request.

Two properties are asserted here, on a real Git repository built by the real
generators:

* two pull requests that touch *different* paths merge with no conflict, and
  the merged controls equal a fresh regeneration of the merged tree, so the
  conflict is gone without the controls going stale;
* a tampered control is still refused.  Omitting a tree-derived field is the
  new normal shape, but declaring one that disagrees, dropping a field the
  recomputation requires, reclassifying a field as tree-derived, or editing a
  ledger hash all still fail closed.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

import public_export  # noqa: E402
import provenance_attest_verify as verifier  # noqa: E402
import provenance_ledger  # noqa: E402
from publication_policy import load_policy  # noqa: E402

LEDGER = "assets/public_provenance_ledger.json"
EXPORT = "PUBLIC_EXPORT.json"
POLICY = "assets/public_source_profile.json"

#: Two paths far apart in the ledger's sort order, so a passing merge cannot be
#: an accident of adjacent lines.
PATH_A = "docs/alpha.md"
PATH_B = "tools/beta.py"


def _git(root: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args], cwd=root, capture_output=True, text=True, check=check,
    )


def _canonical(document: dict) -> bytes:
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def _ledger_document(policy, files: dict[str, bytes]) -> dict:
    """The per-path ledger this repository commits, for *files*.

    Only the per-path entries are mutable, and each one names exactly one path,
    which is the property that keeps two independent changes from colliding.
    """
    entries = []
    for path in sorted(files):
        if path in provenance_ledger.REFRESH_CONTROL_PATHS or path == EXPORT:
            continue
        if policy.resolve(path).disposition != "included":
            continue
        classification, evidence = provenance_ledger._class_for(path, None)
        entries.append({
            "path": path,
            "classification": classification,
            "evidence": evidence,
            "sha256": hashlib.sha256(files[path]).hexdigest(),
        })
    return {
        "schema_version": 1,
        "generated_by": "tools/provenance_ledger.py",
        "policy_profile": policy.name,
        "classification_vocabulary": sorted(provenance_ledger.ALLOWED_CLASSES),
        "entries": entries,
    }


def _generate_controls(root: Path, files: dict[str, bytes]) -> tuple[bytes, bytes]:
    """Return (ledger bytes, committed control bytes) for the given tree."""
    policy = load_policy(root / POLICY)
    ledger_document = _ledger_document(policy, files)
    ledger_bytes = _canonical(ledger_document)
    full = public_export.build_document(
        policy,
        [(path, files[path]) for path in sorted(files) if path != EXPORT],
        provenance_ledger=ledger_bytes,
    )
    return ledger_bytes, _canonical(public_export.build_control_document(full))


class SyntheticControlRepository:
    """A tiny repository whose controls are produced by the real generators."""

    #: A publication policy of this repository's own shape, small enough that the
    #: test does not silently depend on which paths the real profile enumerates.
    POLICY_DOCUMENT = {
        "name": "contrib-check-synthetic",
        "profile_version": "1.0.0",
        "min_tool_version": "0.4.0",
        "build_mode": "PUBLIC_SAFE=1",
        "default_disposition": "REJECT",
        "exclude_prefixes": [],
        "exclude_globs": [],
        "exclude_paths": [],
        "include_paths": [EXPORT, LEDGER, PATH_A, PATH_B, POLICY],
    }

    def __init__(self, root: Path) -> None:
        self.root = root
        self.files: dict[str, bytes] = {
            POLICY: (_canonical(self.POLICY_DOCUMENT)),
            PATH_A: b"# alpha\n\noriginal\n",
            PATH_B: b'"""beta."""\n\nVALUE = 1\n',
        }
        _git(self.root, "init", "-q", "-b", "main")
        _git(self.root, "config", "user.email", "contrib-check@example.invalid")
        _git(self.root, "config", "user.name", "contrib check")
        _git(self.root, "config", "commit.gpgsign", "false")
        # Compare bytes, not line endings: a host default of CRLF would
        # otherwise make the merged controls differ from the regenerated
        # ones for a reason that has nothing to do with the merge.
        _git(self.root, "config", "core.autocrlf", "false")
        _git(self.root, "config", "core.eol", "lf")
        self._write(self.files)
        self.refresh()
        _git(self.root, "add", "-A")
        _git(self.root, "commit", "-qm", "base")
        self.base = _git(self.root, "rev-parse", "HEAD").stdout.strip()

    def _write(self, files: dict[str, bytes]) -> None:
        for path, raw in files.items():
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)

    def refresh(self) -> None:
        """Regenerate both committed controls from the current file set."""
        ledger_bytes, export_bytes = _generate_controls(self.root, self.files)
        for path, raw in ((LEDGER, ledger_bytes), (EXPORT, export_bytes)):
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)

    def commit(self, message: str) -> str:
        self._write(self.files)
        self.refresh()
        _git(self.root, "add", "-A")
        _git(self.root, "commit", "-qm", message)
        return _git(self.root, "rev-parse", "HEAD").stdout.strip()

    def read_controls(self) -> tuple[bytes, bytes]:
        return (self.root / LEDGER).read_bytes(), (self.root / EXPORT).read_bytes()


class ControlConflictTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = SyntheticControlRepository(Path(self._tmp.name))

    def test_two_independent_path_changes_merge_without_conflict(self) -> None:
        """Two pull requests touching different paths must merge cleanly.

        The control document is byte-identical on both sides -- that is the
        whole point of not committing tree-wide digests -- and the merge is
        checked for conflicts rather than assumed from the byte comparison.
        """
        _git(self.repo.root, "checkout", "-q", "-b", "change-a", self.repo.base)
        self.repo.files[PATH_A] = b"# alpha\n\nedited by the first change\n"
        self.repo.commit("edit the documentation path")
        export_a = (self.repo.root / EXPORT).read_bytes()

        _git(self.repo.root, "checkout", "-q", "-b", "change-b", self.repo.base)
        self.repo.files[PATH_B] = b'"""beta."""\n\nVALUE = 2\n'
        self.repo.commit("edit the tool path")
        export_b = (self.repo.root / EXPORT).read_bytes()

        self.assertEqual(
            export_a, export_b,
            "an unrelated path change must not alter the committed export control",
        )
        for field in public_export.EXPORT_TREE_DERIVED_FIELDS:
            self.assertNotIn(field, json.loads(export_a))

        _git(self.repo.root, "checkout", "-q", "change-a")
        merge = _git(self.repo.root, "merge", "--no-edit", "change-b", check=False)
        self.assertEqual(
            merge.returncode, 0,
            f"independent path changes must merge without a conflict:\n{merge.stdout}\n{merge.stderr}",
        )
        for path in (LEDGER, EXPORT):
            self.assertNotIn(b"<<<<<<<", (self.repo.root / path).read_bytes())

    def test_merged_controls_equal_a_fresh_regeneration(self) -> None:
        """Conflict-free is not enough: the merged controls must still be right."""
        _git(self.repo.root, "checkout", "-q", "-b", "change-a", self.repo.base)
        self.repo.files[PATH_A] = b"# alpha\n\nedited by the first change\n"
        self.repo.commit("edit the documentation path")
        _git(self.repo.root, "checkout", "-q", "-b", "change-b", self.repo.base)
        self.repo.files[PATH_B] = b'"""beta."""\n\nVALUE = 2\n'
        self.repo.commit("edit the tool path")
        _git(self.repo.root, "checkout", "-q", "change-a")
        self.assertEqual(_git(self.repo.root, "merge", "--no-edit", "change-b", check=False).returncode, 0)

        merged_ledger, merged_export = self.repo.read_controls()
        self.repo.refresh()
        self.assertEqual((self.repo.root / LEDGER).read_bytes(), merged_ledger)
        self.assertEqual((self.repo.root / EXPORT).read_bytes(), merged_export)


class TamperStillFailsTests(unittest.TestCase):
    """Omitting a tree-derived field is normal; forging one is not."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = SyntheticControlRepository(Path(self._tmp.name))
        _ledger, export_bytes = self.repo.read_controls()
        self.declared = json.loads(export_bytes)
        self.policy = load_policy(self.repo.root / POLICY)
        self.files = [
            (path, self.repo.files[path])
            for path in sorted(self.repo.files) if path != EXPORT
        ]
        self.generated = public_export.build_document(
            self.policy, self.files,
            provenance_ledger=(self.repo.root / LEDGER).read_bytes(),
        )

    def _codes(self, declared: dict) -> list[str]:
        return [f.code for f in verifier._export_field_findings(declared, self.generated, label="export")]

    def test_the_committed_control_shape_is_accepted(self) -> None:
        self.assertEqual(self._codes(self.declared), [])

    def test_a_declared_but_stale_tree_derived_field_is_refused(self) -> None:
        forged = dict(self.declared, included_content_sha256="0" * 64)
        self.assertIn("EXPORT_FIELD_MISMATCH", self._codes(forged))
        forged = dict(self.declared, tracked_file_count=999)
        self.assertIn("EXPORT_FIELD_MISMATCH", self._codes(forged))

    def test_a_stale_ledger_digest_is_refused(self) -> None:
        forged = dict(self.declared, provenance_ledger_sha256="1" * 64)
        self.assertIn("EXPORT_FIELD_MISMATCH", self._codes(forged))

    def test_omitting_a_policy_decided_field_is_refused(self) -> None:
        forged = dict(self.declared)
        del forged["policy_sha256"]
        self.assertIn("EXPORT_FIELD_MISMATCH", self._codes(forged))

    def test_reclassifying_a_required_field_as_tree_derived_is_refused(self) -> None:
        forged = dict(
            self.declared,
            tree_derived_fields=sorted(public_export.EXPORT_TREE_DERIVED_FIELDS + ("policy_sha256",)),
        )
        self.assertIn("EXPORT_FIELD_MISMATCH", self._codes(forged))

    def test_an_invented_field_is_refused(self) -> None:
        self.assertIn("EXPORT_FIELD_MISMATCH", self._codes(dict(self.declared, smuggled="hello")))

    def test_a_legacy_full_export_is_still_accepted(self) -> None:
        """A control that predates this change keeps working unchanged."""
        self.assertEqual(self._codes(self.generated), [])

    def test_a_tampered_ledger_entry_is_still_a_content_mismatch(self) -> None:
        """Editing a file without regenerating the ledger still fails closed.

        Checked through the publication audit's own ledger gate rather than
        through a whole-repository audit, so the assertion is about this
        property and not about which other controls a synthetic tree is
        missing.
        """
        import publish_audit

        self.repo.files[PATH_A] = b"# alpha\n\nedited without regenerating the controls\n"
        self.repo._write(self.repo.files)  # deliberately NOT followed by refresh()
        policy = load_policy(self.repo.root / POLICY)
        entries = [
            publish_audit.GitEntry("100644", "", "0", path, "file")
            for path in sorted(self.repo.files)
        ]
        findings = publish_audit._provenance_ledger_findings(
            policy=policy,
            entries=entries,
            content_map={},
            content_source=publish_audit.CONTENT_CANDIDATE,
            repo_root=self.repo.root,
            trusted_ledger_path=None,
            self_consistency=True,
        )
        codes = [(f.code, f.path) for f in findings]
        self.assertIn(("PROVENANCE_CONTENT_MISMATCH", PATH_A), codes)
        self.assertNotIn(("PROVENANCE_CONTENT_MISMATCH", PATH_B), codes)

    def test_the_gate_names_a_path_only_a_maintainer_can_admit(self) -> None:
        """The plain-language path is wired to the finding, not to the exit code."""
        finding = verifier.Finding(
            "TRUSTED_PATH_MISSING", "tools/new_file.py",
            "trusted detailed authority has no exact path-specific record",
            maintainer_action=True,
        )
        self.assertTrue(finding.fatal)
        self.assertTrue(finding.as_dict()["maintainer_action"])
        verdict = {
            "findings": [finding.as_dict()],
            "verdict": "fail",
            "fatal_count": 1,
        }
        import io
        from contextlib import redirect_stdout

        buffer = io.StringIO()
        with redirect_stdout(buffer):
            verifier._print_guidance(verdict)
        text = buffer.getvalue()
        self.assertIn("nothing for you to do", text)
        self.assertIn("tools/new_file.py", text)

    def test_an_older_verdict_without_the_flag_is_still_explained(self) -> None:
        """A verdict written before the flag carries the codes, so it still reads."""
        import io
        from contextlib import redirect_stdout

        buffer = io.StringIO()
        with redirect_stdout(buffer):
            verifier._print_contributor_summary({
                "verdict": "fail",
                "fatal_count": 1,
                "maintainer_action_paths": [],
                "findings": [{
                    "code": "TRUSTED_PATH_MISSING",
                    "path": "tools/old_style.py",
                    "detail": "no exact record",
                    "fatal": True,
                }],
            })
        self.assertIn("nothing for you to do", buffer.getvalue())
        self.assertIn("tools/old_style.py", buffer.getvalue())

    def test_a_contributor_fixable_finding_is_not_disclaimed(self) -> None:
        verdict = {
            "findings": [verifier.Finding("CONTENT_MISMATCH", "src/rt/x.c", "ledger hash differs").as_dict()],
            "verdict": "fail",
            "fatal_count": 1,
        }
        import io
        from contextlib import redirect_stdout

        buffer = io.StringIO()
        with redirect_stdout(buffer):
            verifier._print_guidance(verdict)
        self.assertIn("fix the finding", buffer.getvalue())
        self.assertNotIn("nothing for you to do", buffer.getvalue())


if __name__ == "__main__":
    unittest.main()
