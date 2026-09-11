# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Test proving multi-title scalability: ingests two distinct non-HST profiles

without requiring any UI or core source modifications.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from test_iso_parity import create_test_iso


class SecondTitleIngestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_second_title_test_"))
        self.gcc = shutil.which("gcc")
        if not self.gcc:
            self.skipTest("gcc not available")

        # Compile runner
        self.harness_c = self.temp_dir / "multi_title_harness.c"
        self.exe_path = self.temp_dir / ("multi_title_harness.exe" if sys.platform == "win32" else "multi_title_harness")

        core_srcs = [
            ROOT / "src" / "core" / "nk_iso.c",
            ROOT / "src" / "core" / "nk_library.c",
            ROOT / "src" / "core" / "nk_launch.c",
            ROOT / "src" / "core" / "generated" / "nk_title_catalog.c",
        ]
        if sys.platform == "win32":
            core_srcs.append(ROOT / "src" / "core" / "nk_platform_win32.c")
        else:
            core_srcs.append(ROOT / "src" / "core" / "nk_platform_posix.c")

        harness_code = """#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "nk_iso.h"
#include "nk_library.h"
#include "nk_launch.h"

int main(int argc, char **argv) {
    if (argc < 4) return 1;
    const char *iso1 = argv[1];
    const char *iso2 = argv[2];
    const char *lib_file = argv[3];

    NkIsoMetadata m1, m2;
    assert(nk_iso_inspect(iso1, &m1) == NK_OK);
    assert(nk_iso_inspect(iso2, &m2) == NK_OK);

    assert(m1.is_supported == true);
    assert(m1.matched_title != NULL);
    assert(strcmp(m1.matched_title->id, "synthetic-allegrex-v1") == 0);

    assert(m2.is_supported == true);
    assert(m2.matched_title != NULL);
    assert(strcmp(m2.matched_title->id, "pspdev-phase5-v1") == 0);

    /* Verify distinct profile attributes */
    assert(strcmp(m1.matched_title->id, m2.matched_title->id) != 0);
    assert(strcmp(m1.matched_title->primary_disc_id, m2.matched_title->primary_disc_id) != 0);

    /* Add both to native library and persist */
    NkLibrary lib;
    nk_library_init(&lib);

    NkGameEntry g1, g2;
    memset(&g1, 0, sizeof(g1));
    snprintf(g1.disc_id, sizeof(g1.disc_id), "%s", m1.disc_id);
    snprintf(g1.title_name, sizeof(g1.title_name), "%s", m1.title_name);
    snprintf(g1.title_id, sizeof(g1.title_id), "%s", m1.matched_title->id);
    snprintf(g1.iso_path, sizeof(g1.iso_path), "%s", iso1);
    g1.status = NK_STATUS_VERIFIED;
    g1.is_prepared = true;

    memset(&g2, 0, sizeof(g2));
    snprintf(g2.disc_id, sizeof(g2.disc_id), "%s", m2.disc_id);
    snprintf(g2.title_name, sizeof(g2.title_name), "%s", m2.title_name);
    snprintf(g2.title_id, sizeof(g2.title_id), "%s", m2.matched_title->id);
    snprintf(g2.iso_path, sizeof(g2.iso_path), "%s", iso2);
    g2.status = NK_STATUS_VERIFIED;
    g2.is_prepared = true;

    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_add_or_update(&lib, &g2) == NK_OK);
    assert(nk_library_count(&lib) == 2);
    assert(nk_library_save(&lib, lib_file) == NK_OK);

    /* Reload and assert distinct records */
    NkLibrary reloaded;
    assert(nk_library_load(&reloaded, lib_file) == NK_OK);
    assert(nk_library_count(&reloaded) == 2);

    const NkGameEntry *r1 = nk_library_find_by_disc_id(&reloaded, m1.disc_id);
    const NkGameEntry *r2 = nk_library_find_by_disc_id(&reloaded, m2.disc_id);
    assert(r1 != NULL && r2 != NULL);
    assert(strcmp(r1->title_id, "synthetic-allegrex-v1") == 0);
    assert(strcmp(r2->title_id, "pspdev-phase5-v1") == 0);

    /* Test typed launch session creation and execution for second title */
    if (argc >= 5) {
        const char *test_root = argv[4];
        NkLaunchSession session;
        memset(&session, 0, sizeof(session));
        assert(nk_launch_prepare_session(&session, r2, test_root) == NK_OK);
        printf("SECOND_TITLE_SESSION_CREATED\\n");
        assert(strcmp(session.title_id, "pspdev-phase5-v1") == 0);
        assert(strcmp(session.disc_id, "TEST00005") == 0);
        assert(session.config.resolution_scale == 1);
        assert(session.config.diagnostic_mode == false);

        assert(nk_launch_start(&session) == NK_OK);
        printf("SECOND_TITLE_RUNTIME_LAUNCHED\\n");
        int exit_code = nk_launch_wait(&session, 5000);
        printf("SECOND_TITLE_EXIT_CODE_%d\\n", exit_code);
        assert(exit_code == 0);
        nk_launch_stop(&session);
    }

    printf("MULTI_TITLE_TEST_PASSED\\n");
    return 0;
}
"""
        self.harness_c.write_text(harness_code, encoding="utf-8")

        cmd = [
            self.gcc,
            "-Wall", "-Wextra", "-Werror", "-std=c99",
            "-I", str(ROOT / "src" / "core"),
            "-I", str(ROOT / "src" / "core" / "generated"),
            str(self.harness_c),
        ] + [str(s) for s in core_srcs] + [
            "-o", str(self.exe_path),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Compilation failed: {res.stderr}")

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_multi_title_ingest_and_library_coexistence(self) -> None:
        """Prove that two distinct non-HST titles are recognized and stored without UI changes."""
        iso1 = self.temp_dir / "title1.iso"
        iso2 = self.temp_dir / "title2.iso"
        lib_json = self.temp_dir / "multi_library.json"

        create_test_iso(iso1, disc_id="TEST00001", title="Synthetic Allegrex Test")
        create_test_iso(iso2, disc_id="TEST00005", title="PSPDev Phase 5 Sample")

        # Compile mock runtime for second title (pspdev-phase5-v1)
        bin_dir = self.temp_dir / "build" / "pspdev-phase5-v1"
        bin_dir.mkdir(parents=True, exist_ok=True)
        mock_c = self.temp_dir / "mock_runtime.c"
        mock_exe = bin_dir / ("pspdev-phase5-v1.exe" if sys.platform == "win32" else "pspdev-phase5-v1")

        mock_code = """#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int main(void) {
    const char *iso = getenv("PSP_ISO");
    assert(iso != NULL && strlen(iso) > 0);
    const char *fps = getenv("SR_FPS_CAP");
    assert(fps != NULL && strcmp(fps, "60") == 0);
    /* Diagnostic fatal dispatch must NOT be set in consumer mode */
    const char *fatal = getenv("SR_DISPATCH_FATAL");
    assert(fatal == NULL);
    return 0;
}
"""
        mock_c.write_text(mock_code, encoding="utf-8")
        comp = subprocess.run([self.gcc, str(mock_c), "-o", str(mock_exe)], capture_output=True, text=True)
        self.assertEqual(comp.returncode, 0, f"Mock runtime compile failed: {comp.stderr}")

        res = subprocess.run(
            [str(self.exe_path), str(iso1), str(iso2), str(lib_json), str(self.temp_dir)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(res.returncode, 0, f"Execution failed: {res.stderr}")
        self.assertIn("SECOND_TITLE_SESSION_CREATED", res.stdout)
        self.assertIn("SECOND_TITLE_RUNTIME_LAUNCHED", res.stdout)
        self.assertIn("SECOND_TITLE_EXIT_CODE_0", res.stdout)
        self.assertIn("MULTI_TITLE_TEST_PASSED", res.stdout)


if __name__ == "__main__":
    unittest.main()
