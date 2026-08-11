# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Offline deterministic unit tests for tools/lint_docs.py documentation linter."""

import pathlib
import unittest

from tools.lint_docs import run_all_doc_lints, ROOT


class TestDocFreshnessLinter(unittest.TestCase):
    def test_repository_documentation_freshness(self) -> None:
        """Assert that the repository documentation passes all offline freshness lint checks."""
        errors = run_all_doc_lints(ROOT)
        self.assertEqual(
            errors,
            [],
            f"Documentation freshness linter found staleness defects:\n" + "\n".join(errors),
        )


if __name__ == "__main__":
    unittest.main()
