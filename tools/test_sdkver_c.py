# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Retained-state regression for the compiled-SDK-version contract (issue #71).

Three layers, each proving a distinct claim -- none of them executes the
production HLE dispatch path or a production SDK-dependent consumer (no such
consumer of g_sdk_version exists in the runtime yet):

1. Helper-level execution: compile and run ``src/rt/sdkver_selftest.c``
   against the pure helpers in ``src/rt/sdkver.h``. This exercises a
   TEST-LOCAL state word and a TEST-LOCAL consumer model, proving the helper
   contract -- every variant in the selftest's NID table updates the same
   state word through one setter, and the model consumer reads exactly the
   retained value.
2. Manifest-backed routing proof: the fail-closed extraction of src/rt/hle.c
   (tools/hle_manifest.py) proves the PRODUCTION registrations -- every
   sceKernelSetCompiledSdkVersion* NID, including 0x1b4217bc under its
   canonical sceKernelSetCompiledSdkVersion603_605 name -- route to the
   single shared handler, with no waiver remaining.
3. Source guard: ``h_SetCompiledSdkVersion`` (in the Windows-only hle.c TU,
   not compilable on the Linux CI host) is proven to call the same
   sr_sdkver_set helper the selftest executes, tying layers 1 and 2
   together.

The selftest's variant NID table is cross-checked against the manifest so the
helper-level coverage cannot silently drift from the runtime registration
set.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import hle_registry_meta as meta  # noqa: E402
from hle_manifest import build_manifest  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SELFTEST_C = ROOT / "src" / "rt" / "sdkver_selftest.c"
HLE_SOURCE = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

STATEFUL_HANDLER = "h_SetCompiledSdkVersion"
SDK_NAME_PREFIX = "sceKernelSetCompiledSdkVersion"


def sdk_variant_registrations():
    manifest = build_manifest()
    return [
        r for r in manifest["registrations"] if r["name"].startswith(SDK_NAME_PREFIX)
    ]


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestSdkverSelftestC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="sdkverc_")
        cls.exe = os.path.join(cls.tmp, "sdkver_selftest.exe")
        result = subprocess.run(
            [
                CC,
                "-std=c11",
                "-O0",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'src' / 'rt'}",
                "-o",
                cls.exe,
                str(SELFTEST_C),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError("sdkver_selftest.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_retained_state_invariants_hold(self):
        result = subprocess.run([self.exe], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("sdkver selftest: OK", result.stdout)

    def test_selftest_variant_nids_match_the_registered_set(self):
        """The executable coverage must track the real registration table."""
        selftest_nids = {
            int(h, 16)
            for h in re.findall(
                r"\{\s*0x([0-9a-fA-F]{8})u\s*,\s*0x[0-9a-fA-F]{8}u\s*\}",
                SELFTEST_C.read_text(encoding="utf-8"),
            )
        }
        registered_nids = {int(r["nid"], 16) for r in sdk_variant_registrations()}
        self.assertEqual(
            selftest_nids,
            registered_nids,
            "a SetCompiledSdkVersion variant was added/removed in hle.c "
            "without updating src/rt/sdkver_selftest.c",
        )


class TestSdkVersionRegistrationRouting(unittest.TestCase):
    def test_every_variant_routes_to_the_stateful_handler(self):
        variants = sdk_variant_registrations()
        self.assertGreaterEqual(len(variants), 3)
        for r in variants:
            self.assertEqual(r["handler"], STATEFUL_HANDLER, r["name"])
            self.assertEqual(r["classification"], "dedicated", r["name"])

    def test_603_605_is_canonical_and_unwaived(self):
        by_nid = {int(r["nid"], 16): r for r in sdk_variant_registrations()}
        self.assertIn(0x1B4217BC, by_nid)
        self.assertEqual(by_nid[0x1B4217BC]["name"], "sceKernelSetCompiledSdkVersion603_605")
        self.assertEqual(by_nid[0x1B4217BC]["handler"], STATEFUL_HANDLER)
        self.assertEqual([w for w in meta.WAIVERS if w["nid"] == 0x1B4217BC], [])


class TestHleSdkVersionWiring(unittest.TestCase):
    def test_handler_uses_the_shared_state_helper(self):
        match = re.search(
            r"static uint32_t h_SetCompiledSdkVersion\(CpuState \*s\)\s*\{([^}]*)\}",
            HLE_SOURCE,
        )
        self.assertIsNotNone(match, "h_SetCompiledSdkVersion not found in src/rt/hle.c")
        body = match.group(1)
        self.assertIn(
            "sr_sdkver_set(&g_sdk_version, A0)",
            body,
            "the handler must store through the tested sr_sdkver_set helper",
        )


if __name__ == "__main__":
    unittest.main()
