# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent.parent


class TestTelemetryExportSecurity(unittest.TestCase):
    def test_private_diagnostic_data_guidance(self):
        doc_file = ROOT / "docs" / "AI_USAGE.md"
        self.assertTrue(doc_file.is_file(), "AI_USAGE.md file missing")

        content = doc_file.read_text(encoding="utf-8")
        blocks = [
            " ".join(line.strip() for line in block.splitlines() if line.strip())
            for block in re.split(r"\n\s*\n", content)
            if block.strip()
        ]
        self.assertTrue(
            any(
                all(term in block.lower() for term in ("private diagnostic data", "public ai services"))
                and re.search(r"\bdo not\b.{0,40}\bupload\b", block.lower())
                for block in blocks
            ),
            "AI_USAGE.md must keep diagnostic bundles private and prohibit public uploads",
        )
        self.assertNotIn("/api/recompiler/telemetry/export", content)


if __name__ == "__main__":
    unittest.main()
