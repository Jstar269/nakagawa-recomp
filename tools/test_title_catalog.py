# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for title catalog generation, drift detection, and native C lookup parity."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import title_catalog_codegen
from nk_core.title_registry import TitleRegistry


class SyntheticDiscIdTests(unittest.TestCase):
    """Synthetic disc-id assignment has exactly one home and fails closed.

    It used to live in two private copies -- one in tools/title_catalog_codegen.py
    for the native catalog, one in tools/nk_core/title_registry.py for the Python
    tooling -- and they drifted. Both then fell back to a SHARED "TEST00000"
    sentinel for an unassigned title, so the second unassigned title to appear
    collided with the first: the registry reported a conflict with the public
    canonical catalog that did not exist, and the native catalog emitted two
    entries claiming one disc id.
    """

    def test_both_modules_use_the_canonical_assignment(self) -> None:
        import title_catalog_codegen
        from nk_core import synthetic_disc_ids, title_registry

        self.assertIs(title_catalog_codegen.SYNTHETIC_DISC_ID_MAP,
                      synthetic_disc_ids.SYNTHETIC_DISC_IDS)
        self.assertIs(title_registry.SYNTHETIC_DISC_ID_MAP,
                      synthetic_disc_ids.SYNTHETIC_DISC_IDS)

    def test_the_generic_manifest_module_does_not_own_it(self) -> None:
        """tools/title_manifest.py is a generic helper and must not name a title.

        That is the contract tools/test_generic_title_planning_proof.py enforces,
        and it is the reason this assignment does not live there despite being
        manifest-shaped data.
        """
        import title_manifest

        self.assertFalse(hasattr(title_manifest, "SYNTHETIC_DISC_IDS"))

    def test_unassigned_synthetic_title_fails_closed(self) -> None:
        from nk_core import synthetic_disc_ids

        with self.assertRaisesRegex(synthetic_disc_ids.SyntheticDiscIdError,
                                    "has no assigned disc id"):
            synthetic_disc_ids.synthetic_disc_id("a-title-nobody-assigned-v1")

    def test_assignments_are_unique(self) -> None:
        from nk_core import synthetic_disc_ids

        values = list(synthetic_disc_ids.SYNTHETIC_DISC_IDS.values())
        self.assertEqual(len(values), len(set(values)),
                         "two synthetic titles share a disc id")

    def test_every_public_synthetic_manifest_is_assigned(self) -> None:
        import json
        from nk_core import synthetic_disc_ids

        for manifest in sorted((ROOT / "assets" / "titles").glob("*.json")):
            data = json.loads(manifest.read_text(encoding="utf-8"))
            if data.get("kind") != "synthetic":
                continue
            # Fails closed rather than returning a sentinel.
            synthetic_disc_ids.synthetic_disc_id(data["id"])


class TitleCatalogTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_catalog_test_"))

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_live_catalog_verify_passes(self) -> None:
        """Verify that current live repo catalog matches assets/titles/ exactly."""
        res = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "title_catalog_codegen.py"), "--verify"],
            capture_output=True,
            text=True,
            cwd=str(ROOT),
        )
        self.assertEqual(res.returncode, 0, f"Catalog verification failed: {res.stderr}")
        self.assertIn("Native public title catalog is up to date", res.stdout)

    def test_catalog_codegen_drift_detection(self) -> None:
        """Verify that any modification to manifests causes --verify to fail."""
        # This used to stage a copy of assets/titles/ in a temp directory and
        # point --manifest-dir at it. The generator now refuses a manifest
        # directory outside the repository, because publication policy is
        # decided per tracked PATH and a copy in /tmp has no policy identity of
        # its own to be judged by. So drift is exercised where it actually
        # happens: against the tracked manifests, restored afterwards.
        titles_dir = ROOT / "assets" / "titles"
        temp_gen = self.temp_dir / "generated"

        def run_codegen(*extra: str) -> subprocess.CompletedProcess:
            return subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "title_catalog_codegen.py"),
                    "--output-dir", str(temp_gen),
                    *extra,
                ],
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )

        res_gen = run_codegen()
        self.assertEqual(res_gen.returncode, 0, res_gen.stderr)

        # Verify passes initially
        res_ver = run_codegen("--verify")
        self.assertEqual(res_ver.returncode, 0, res_ver.stderr)

        # Mutate a tracked manifest, then always put it back.
        manifest_to_modify = sorted(titles_dir.glob("*.json"))[0]
        original_bytes = manifest_to_modify.read_bytes()
        try:
            data = json.loads(original_bytes.decode("utf-8"))
            data["display_name"] = "Drifted Title Name"
            manifest_to_modify.write_text(json.dumps(data, indent=2), encoding="utf-8")

            res_drift = run_codegen("--verify")
        finally:
            manifest_to_modify.write_bytes(original_bytes)

        self.assertNotEqual(res_drift.returncode, 0)
        self.assertIn("Native public title catalog is out of date", res_drift.stderr)

    def test_out_of_repository_manifest_dir_is_refused(self) -> None:
        """--manifest-dir outside the repository must fail, not borrow policy by name."""
        temp_titles = self.temp_dir / "titles"
        shutil.copytree(ROOT / "assets" / "titles", temp_titles)
        res = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "title_catalog_codegen.py"),
                "--manifest-dir", str(temp_titles),
                "--output-dir", str(self.temp_dir / "generated"),
            ],
            capture_output=True,
            text=True,
            cwd=str(ROOT),
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("outside the repository root", res.stderr)

    def test_native_c_catalog_compilation_and_lookup(self) -> None:
        """Compile generated C catalog with gcc and test native lookups in a C harness."""
        gcc = shutil.which("gcc")
        if not gcc:
            self.skipTest("gcc not available in PATH on this host")

        test_c_file = self.temp_dir / "test_harness.c"
        cat_h = ROOT / "src" / "core" / "generated" / "nk_title_catalog.h"
        cat_c = ROOT / "src" / "core" / "generated" / "nk_title_catalog.c"

        # Derived from the manifests, not a literal: adding a public title
        # should not require editing a number in a test harness.
        expected_titles = len(sorted((ROOT / "assets" / "titles").glob("*.json")))

        test_c_source = f"""#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "{cat_h.as_posix()}"

int main(void) {{
    printf("Catalog count: %d\\n", nk_title_catalog_count);
    assert(nk_title_catalog_count == {expected_titles});

    /* Test synthetic title lookups in public catalog */
    const NkTitleEntry *t_synth = nk_title_catalog_find_by_disc_id("TEST00001");
    assert(t_synth != NULL);
    assert(strcmp(t_synth->id, "synthetic-allegrex-v1") == 0);
    assert(t_synth->kind == NK_TITLE_KIND_SYNTHETIC);

    const NkTitleEntry *t_p5 = nk_title_catalog_find_by_disc_id("TEST00005");
    assert(t_p5 != NULL);
    assert(strcmp(t_p5->id, "pspdev-phase5-v1") == 0);

    /* Test hyphen and whitespace normalization */
    const NkTitleEntry *t2 = nk_title_catalog_find_by_disc_id("test-00001");
    assert(t2 == t_synth);
    const NkTitleEntry *t3 = nk_title_catalog_find_by_disc_id("  TEST_00005  ");
    assert(t3 == t_p5);

    /* Retail IDs must return NULL in public catalog */
    assert(nk_title_catalog_find_by_disc_id("UCUS98701") == NULL);
    assert(nk_title_catalog_find_by_disc_id("UCES01402") == NULL);
    assert(nk_title_catalog_find_by_id("hst-ucus98701-v1") == NULL);

    /* Test in-memory private overlay */
    NkTitleEntry overlay;
    memset(&overlay, 0, sizeof(overlay));
    overlay.id = "private-test-v1";
    overlay.display_name = "Private Overlay Test";
    overlay.kind = NK_TITLE_KIND_RETAIL;
    overlay.primary_disc_id = "UCUS98701";

    nk_title_catalog_register_overlay(&overlay);
    assert(nk_title_catalog_find_by_disc_id("UCUS98701") == &overlay);
    assert(nk_title_catalog_find_by_id("private-test-v1") == &overlay);

    nk_title_catalog_clear_overlay();
    assert(nk_title_catalog_find_by_disc_id("UCUS98701") == NULL);
    assert(nk_title_catalog_find_by_id("private-test-v1") == NULL);

    /* Test nonexistent disc ID */
    assert(nk_title_catalog_find_by_disc_id("ULUS99999") == NULL);
    assert(nk_title_catalog_find_by_disc_id("") == NULL);
    assert(nk_title_catalog_find_by_disc_id(NULL) == NULL);

    printf("ALL_NATIVE_CATALOG_C_ASSERTIONS_PASSED\\n");
    return 0;
}}
"""
        test_c_file.write_text(test_c_source, encoding="utf-8")
        exe_path = self.temp_dir / ("test_harness.exe" if sys.platform == "win32" else "test_harness")

        cmd_compile = [
            gcc,
            "-Wall", "-Wextra", "-Werror", "-std=c99",
            "-I", str(cat_h.parent),
            str(test_c_file),
            str(cat_c),
            "-o", str(exe_path),
        ]
        res_comp = subprocess.run(cmd_compile, capture_output=True, text=True)
        self.assertEqual(res_comp.returncode, 0, f"Compilation failed: {res_comp.stderr}")

        res_exec = subprocess.run([str(exe_path)], capture_output=True, text=True)
        self.assertEqual(res_exec.returncode, 0, f"Execution failed: {res_exec.stderr}")
        self.assertIn("ALL_NATIVE_CATALOG_C_ASSERTIONS_PASSED", res_exec.stdout)

    def test_parity_between_python_registry_and_c_catalog(self) -> None:
        """Ensure Python TitleRegistry and C catalog headers have the exact same titles."""
        reg = TitleRegistry(include_defaults=True)
        _, manifests = title_catalog_codegen.collect_public_manifests(ROOT / "assets" / "titles")

        self.assertEqual(len(reg._profiles), len(manifests))
        for m in manifests:
            p = reg.lookup_by_id(m["id"])
            self.assertIsNotNone(p)
            self.assertEqual(p.name, m["display_name"])


if __name__ == "__main__":
    unittest.main()
