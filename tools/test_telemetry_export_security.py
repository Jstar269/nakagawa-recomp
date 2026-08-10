# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent


class TestTelemetryExportSecurity(unittest.TestCase):
    def test_export_route_file_contents(self):
        route_file = ROOT / "interface" / "src" / "app" / "api" / "recompiler" / "telemetry" / "export" / "route.ts"
        self.assertTrue(route_file.is_file(), "Export route file missing")

        content = route_file.read_text(encoding="utf-8")
        self.assertIn("private-diagnostic-telemetry-export.zip", content, "Attachment filename must be renamed")
        self.assertIn("README_PRIVATE_DIAGNOSTIC_DATA.txt", content, "Warning manifest must be included in export ZIP")
        self.assertIn("MAX_DB_BYTES", content, "Database size must be bounded")
        self.assertIn("MAX_INVENTORY_BYTES", content, "Inventory map size must be bounded")
        self.assertIn("MAX_ZIP_BYTES", content, "Export ZIP size must be bounded")
        self.assertIn("realpathSync", content, "Symlink realpath containment check must be enforced")

    def test_zip_builder_implementation(self):
        zip_file = ROOT / "interface" / "src" / "lib" / "recompiler" / "zip.ts"
        self.assertTrue(zip_file.is_file(), "zip.ts file missing")

        content = zip_file.read_text(encoding="utf-8")
        self.assertIn("Uint8Array", content, "zip.ts must use Uint8Array")
        self.assertNotIn("const chunks: number[] = []", content, "zip.ts must not allocate unbounded number[] arrays")

    def test_ai_usage_documentation(self):
        doc_file = ROOT / "docs" / "AI_USAGE.md"
        self.assertTrue(doc_file.is_file(), "AI_USAGE.md file missing")

        content = doc_file.read_text(encoding="utf-8")
        self.assertIn("private-diagnostic-telemetry-export.zip", content, "AI_USAGE.md must document diagnostic export classification")


if __name__ == "__main__":
    unittest.main()
