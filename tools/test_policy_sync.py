# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Offline tests for the mismatch report of tools/policy_sync.py."""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

import policy_sync  # noqa: E402

POLICY = {
    "name": "synthetic",
    "profile_version": "1.0.0",
    "min_tool_version": "1.0.0",
    "build_mode": "public-source",
    "default_disposition": "REJECT",
    "exclude_prefixes": [],
    "exclude_globs": [],
    "exclude_paths": [],
    "include_paths": ["docs/a.md", "tools/b.py"],
}


class PolicySyncTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.policy = Path(tmp.name) / "policy.json"
        self.policy.write_text(json.dumps(POLICY), encoding="utf-8")

    def run_sync(self, tracked, *args):
        out, err = io.StringIO(), io.StringIO()
        with patch.object(policy_sync, "tracked_paths", return_value=tracked), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = policy_sync.main(["--policy", str(self.policy), *args])
        return rc, out.getvalue(), err.getvalue()

    def test_in_sync_copy_passes(self):
        rc, out, _ = self.run_sync(["docs/a.md", "tools/b.py"])
        self.assertEqual(rc, 0)
        self.assertIn("policy sync: OK", out)

    def test_deliberate_mismatch_names_the_file(self):
        rc, out, err = self.run_sync(["docs/a.md", "tools/new_unlisted.py"])
        self.assertEqual(rc, 1)
        self.assertIn("unclassified: tools/new_unlisted.py", out)
        self.assertNotIn("docs/a.md", out)
        self.assertIn("include_paths", err)

    def test_high_risk_mismatch_is_flagged_and_apply_refused(self):
        before = self.policy.read_text(encoding="utf-8")
        rc, out, err = self.run_sync(["docs/a.md", "src/secret_key.c"], "--apply")
        self.assertEqual(rc, 1)
        self.assertIn("HIGH RISK", out)
        self.assertIn("src/secret_key.c", out)
        self.assertIn("refusing --apply", err)
        self.assertEqual(self.policy.read_text(encoding="utf-8"), before)

    def test_apply_adds_routine_path_only_to_the_copy(self):
        rc, out, _ = self.run_sync(["docs/a.md", "tools/c.py"], "--apply")
        self.assertEqual(rc, 0)
        self.assertIn("added 1 path(s)", out)
        added = json.loads(self.policy.read_text(encoding="utf-8"))["include_paths"]
        self.assertIn("tools/c.py", added)
        self.assertEqual(self.run_sync(["docs/a.md", "tools/c.py"])[0], 0)

    def test_is_high_risk_classes(self):
        for path in ("assets/font/x.txt", "a/b.pgf", "x/kirk.c", "oracle/n.txt"):
            self.assertTrue(policy_sync.is_high_risk(path), path)
        self.assertFalse(policy_sync.is_high_risk("tools/test_x.py"))


if __name__ == "__main__":
    unittest.main()
