# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Exact-incident regression against preserved private evidence.

This asserts the hardened gate rejects **every one** of the fifteen excluded paths
in the real 2026-08-11 tree, not a synthetic reconstruction of it. The permanent,
self-contained equivalent is ``test_publication_policy_gate.py``; this module adds
the one thing a synthetic fixture cannot provide -- proof against the actual bytes.

The evidence is private and deliberately not referenced by any hard-coded path, so
this file leaks no local layout and can live in a public tree. Point
``NAKAGAWA_INCIDENT_TREE_TAR`` at the preserved ``git archive`` of the incident
commit to run it:

    NAKAGAWA_INCIDENT_TREE_TAR=/path/to/incident-tree-ee398561.tar \\
        python -m unittest tools.test_incident_regression_private

Without that variable every test skips, so the public suite stays green after the
public repository is replaced and the incident commit no longer exists anywhere.
This test must never become the only proof the gate works.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AUDIT = ROOT / "tools" / "publish_audit.py"

ENV_VAR = "NAKAGAWA_INCIDENT_TREE_TAR"

#: Expected rejections. The gate must produce POLICY_EXCLUDED_PRESENT for each.
EXPECTED_EXCLUDED = (
    "font/jpn0.pgf",
    "font/kr0.pgf",
    "font/ltn0.pgf",
    "font/ltn8.pgf",
    "src/rt/pgd.c",
    "src/rt/pgd.h",
    "src/rt/pgf.c",
    "src/rt/pgf.h",
    "tools/pgd_decrypt.py",
    "tools/pgd_e2e_harness.c",
    "tools/pgd_test_keys.py",
    "tools/test_pgd_c.py",
    "tools/test_pgd_decrypt.py",
    "tools/test_pgd_hardening.py",
    "tools/test_pgd_malformed.py",
)


def _incident_tar() -> Path | None:
    raw = os.environ.get(ENV_VAR)
    if not raw:
        return None
    path = Path(raw)
    return path if path.is_file() else None


@unittest.skipIf(_incident_tar() is None, f"{ENV_VAR} not set to a readable incident archive")
class TestExactIncidentRejected(unittest.TestCase):
    """The real ee398561 tree must be rejected path-for-path."""

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory()
        cls.candidate = Path(cls._tmp.name) / "incident"
        cls.candidate.mkdir()
        with tarfile.open(_incident_tar()) as archive:
            archive.extractall(cls.candidate, filter="data")
        # The policy deliberately comes from THIS repository, never from the
        # extracted tree -- which carries its own, older profile.
        cls.result = subprocess.run(
            [sys.executable, str(AUDIT), "--candidate-root", str(cls.candidate)],
            cwd=ROOT, capture_output=True, text=True,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def _rejected_paths(self) -> set[str]:
        rejected = set()
        for line in self.result.stderr.splitlines():
            if line.startswith("POLICY_EXCLUDED_PRESENT:"):
                rejected.add(line.split(":", 2)[1].strip())
        return rejected

    def test_audit_fails(self):
        self.assertEqual(self.result.returncode, 1,
                         "the incident tree must fail the publication audit")

    def test_every_excluded_path_is_rejected(self):
        rejected = self._rejected_paths()
        missed = [p for p in EXPECTED_EXCLUDED if p not in rejected]
        self.assertEqual(missed, [], f"excluded paths NOT rejected: {missed}")

    def test_zero_missed_excluded_paths(self):
        """The historical result was 6 of 15 under --public-scope and 0 of 15 by default."""
        self.assertEqual(len(self._rejected_paths() & set(EXPECTED_EXCLUDED)), 15)

    def test_rejection_happens_in_default_mode(self):
        """No flag should be required to reject an excluded path."""
        self.assertNotIn("--public-scope", " ".join(self.result.args))
        self.assertEqual(self.result.returncode, 1)

    def test_stale_export_and_manifest_are_also_reported(self):
        """The incident tree carries the pre-hardening export and manifest."""
        codes = {line.split(":", 1)[0] for line in self.result.stderr.splitlines() if ":" in line}
        self.assertIn("POLICY_EXPORT_STALE", codes)
        self.assertIn("POLICY_MANIFEST_MISSING", codes)


if __name__ == "__main__":
    unittest.main()
