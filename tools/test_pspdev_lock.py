# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest

import pspdev_lock


ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT / "assets" / "upstream" / "pspdev.lock.json"
EVIDENCE_PATH = ROOT / "assets" / "upstream" / "pspdev.evidence.json"


class LockValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))

    def test_committed_lock_has_complete_local_evidence(self) -> None:
        pending = pspdev_lock.validate_lock(copy.deepcopy(self.lock))
        self.assertEqual(pending, [])
        self.assertEqual(self.lock["local_verification"]["status"], "complete")

    def test_require_local_accepts_committed_evidence(self) -> None:
        self.assertEqual(
            pspdev_lock.validate_lock(copy.deepcopy(self.lock), require_local=True),
            [],
        )

    def test_abbreviated_or_uppercase_commit_fails(self) -> None:
        for bad in ("314b208", "A" * 40):
            data = copy.deepcopy(self.lock)
            data["components"]["pspsdk"]["commit"] = bad
            with self.assertRaises(pspdev_lock.LockError):
                pspdev_lock.validate_lock(data)

    def test_moving_or_unofficial_repository_fails(self) -> None:
        data = copy.deepcopy(self.lock)
        data["components"]["pspsdk"]["repository"] = (
            "https://example.invalid/pspsdk"
        )
        with self.assertRaises(pspdev_lock.LockError) as ctx:
            pspdev_lock.validate_lock(data)
        self.assertIn("official", str(ctx.exception))

    def test_unknown_keys_fail_closed(self) -> None:
        data = copy.deepcopy(self.lock)
        data["components"]["pspsdk"]["surprise"] = True
        with self.assertRaises(pspdev_lock.LockError) as ctx:
            pspdev_lock.validate_lock(data)
        self.assertIn("unknown keys", str(ctx.exception))

    def test_release_formats_are_strict(self) -> None:
        data = copy.deepcopy(self.lock)
        data["distribution"]["release"] = "latest"
        with self.assertRaises(pspdev_lock.LockError):
            pspdev_lock.validate_lock(data)
        data = copy.deepcopy(self.lock)
        data["components"]["psplinkusb"]["release"] = "3.2.1"
        with self.assertRaises(pspdev_lock.LockError):
            pspdev_lock.validate_lock(data)

    def test_private_absolute_paths_are_rejected(self) -> None:
        windows_profile_path = "C:" + r"\Users\Example\pspdev"
        unc_path = r"\\" + "server" + r"\private\pspdev"
        home_path = "/" + "home/example/pspdev"
        posix_profile_path = "/" + "Users/example/pspdev"
        for private_path in (
            windows_profile_path,
            unc_path,
            home_path,
            posix_profile_path,
        ):
            data = copy.deepcopy(self.lock)
            data["local_verification"]["installation_method"] = private_path
            with self.assertRaises(pspdev_lock.LockError) as ctx:
                pspdev_lock.validate_lock(data)
            self.assertIn("absolute path", str(ctx.exception))

    def test_policy_cannot_enable_network_or_retail_material(self) -> None:
        for key in (
            "network_access_from_normal_build",
            "mandatory_for_hst_build_or_runtime",
            "allow_moving_refs",
            "automatic_runtime_rewrite",
            "allow_retail_or_firmware_material",
        ):
            data = copy.deepcopy(self.lock)
            data["policy"][key] = True
            with self.assertRaises(pspdev_lock.LockError):
                pspdev_lock.validate_lock(data)

    def test_complete_status_requires_complete_evidence(self) -> None:
        data = copy.deepcopy(self.lock)
        data["local_verification"]["tool_versions"]["psp-gcc"] = None
        with self.assertRaises(pspdev_lock.LockError):
            pspdev_lock.validate_lock(data)

    def test_public_evidence_matches_lock_and_covers_every_tool(self) -> None:
        evidence = json.loads(EVIDENCE_PATH.read_text(encoding="utf-8"))
        self.assertEqual(evidence["release"], self.lock["distribution"]["release"])
        self.assertEqual(
            evidence["archive"]["local_sha256"],
            self.lock["distribution"]["archive_sha256"],
        )
        self.assertTrue(evidence["archive"]["verified"])
        self.assertEqual(
            evidence["container"]["digest"],
            self.lock["distribution"]["container_digest"],
        )
        self.assertTrue(evidence["container"]["verified_pull"])
        tools = {item["name"]: item for item in evidence["tools"]}
        self.assertEqual(set(tools), pspdev_lock.EXPECTED_TOOLS)
        for name, item in tools.items():
            self.assertRegex(item["sha256"], r"^[0-9a-f]{64}$")
            self.assertGreater(item["size"], 0)
            self.assertIn(item["sha256"], self.lock["local_verification"]["tool_versions"][name])

    def test_canonical_json_is_deterministic(self) -> None:
        a = pspdev_lock.canonical_json(copy.deepcopy(self.lock))
        b = pspdev_lock.canonical_json(copy.deepcopy(self.lock))
        self.assertEqual(a, b)
        self.assertTrue(a.endswith("\n"))
        self.assertEqual(json.loads(a), self.lock)

    def test_report_is_deterministic_and_has_every_component(self) -> None:
        pending = pspdev_lock.validate_lock(copy.deepcopy(self.lock))
        a = pspdev_lock.render_report(self.lock, pending)
        b = pspdev_lock.render_report(self.lock, pending)
        self.assertEqual(a, b)
        for component in pspdev_lock.EXPECTED_COMPONENTS:
            self.assertIn(f"`{component}`", a)

    def test_cli_writes_only_requested_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            out = root / "canonical.json"
            report = root / "report.md"
            rc = pspdev_lock.main(
                [
                    "--lock",
                    str(LOCK_PATH),
                    "--json-out",
                    str(out),
                    "--report",
                    str(report),
                ]
            )
            self.assertEqual(rc, 0)
            self.assertEqual(json.loads(out.read_text(encoding="ascii")), self.lock)
            self.assertIn(
                "# PSPDEV lock audit", report.read_text(encoding="utf-8")
            )
            self.assertEqual(
                sorted(path.name for path in root.iterdir()),
                ["canonical.json", "report.md"],
            )


if __name__ == "__main__":
    unittest.main()
