# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent.parent


class TestTelemetryExportSecurity(unittest.TestCase):
    def test_private_diagnostic_data_guidance(self):
        doc_file = ROOT / "docs" / "AI_USAGE.md"
        self.assertTrue(doc_file.is_file(), "AI_USAGE.md file missing")

        content = doc_file.read_text(encoding="utf-8")
        self.assertIn("PRIVATE DIAGNOSTIC DATA", content)
        self.assertIn("Do not upload them to public AI services", content)
        self.assertNotIn("/api/recompiler/telemetry/export", content)


if __name__ == "__main__":
    unittest.main()
