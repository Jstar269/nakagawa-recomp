# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Checks on the Authenticode signing script and the pages that describe it (#1284).

The script needs a certificate and signtool, so these tests read it rather than run it.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tools" / "sign_release_binaries.ps1"
SIGNING_DOC = ROOT / "docs" / "RELEASE_SIGNING.md"
RELEASE_DOC = ROOT / "docs" / "PREVIEW_RELEASE.md"


class SignReleaseBinariesTests(unittest.TestCase):
    def setUp(self):
        self.script = SCRIPT.read_text(encoding="utf-8")

    @unittest.skipUnless(shutil.which("pwsh"), "PowerShell 7 is not installed")
    def test_script_parses(self):
        command = ("$e = $null; [System.Management.Automation.Language.Parser]::ParseFile("
                   f"'{SCRIPT}', [ref]$null, [ref]$e) | Out-Null; "
                   "if ($e.Count) { $e | ForEach-Object { $_.Message }; exit 1 }")
        result = subprocess.run(["pwsh", "-NoProfile", "-Command", command],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_only_the_two_project_executables_are_signed(self):
        match = re.search(r"\$SignedFiles = @\(([^)]*)\)", self.script)
        self.assertIsNotNone(match)
        names = re.findall(r"'([^']+)'", match.group(1))
        self.assertEqual(names, ["bin/nakagawa_player.exe", "bin/production_smoke.exe"])
        # A demo's completion-manifest.json lists its executable's SHA-256, so a signed demo would be hidden.
        self.assertNotIn("demos", match.group(1))

    def test_no_secret_reaches_a_command_line(self):
        # signtool's /p (password) and /f (key file) put a secret in the process list; the script has neither.
        self.assertIsNone(re.search(r"'/(p|f|csp|kc)'", self.script))
        self.assertNotIn("Read-Host", self.script)
        self.assertNotIn("-AsSecureString", self.script)

    def test_a_release_signature_is_timestamped_and_verified(self):
        for needle in ("'/tr'", "'/td'", "'sha256'", "verify /pa /all", "SIGN_NO_TIMESTAMP", "SIGN_ALREADY_SIGNED",
                       "--write-runtime-manifest-only", "--verify-runtime-manifest", "signing-record.json"):
            self.assertIn(needle, self.script)

    def test_pages_name_the_script_and_its_place_in_the_recipe(self):
        signing = SIGNING_DOC.read_text(encoding="utf-8")
        release = RELEASE_DOC.read_text(encoding="utf-8")
        self.assertIn("tools/sign_release_binaries.ps1", signing)
        self.assertIn("tools/sign_release_binaries.ps1", release)
        recipe = release.index("sign_release_binaries.ps1")
        self.assertLess(recipe, release.index("python tools/generate_sbom.py"))
        self.assertLess(recipe, release.index("python tools/stage_install.py --portable-zip"))

    def test_a_failed_run_restores_the_stage_and_the_record_covers_the_refreshed_manifest(self):
        for needle in ("} catch {", "restored to its unsigned state", "refreshed_files", "attested_sha256",
                       "refreshed_sha256"):
            self.assertIn(needle, self.script)

    def test_signtool_discovery_ignores_unversioned_sdk_folders(self):
        self.assertIn("bin/[0-9][0-9.]*/x64/signtool", self.script)


if __name__ == "__main__":
    unittest.main()
