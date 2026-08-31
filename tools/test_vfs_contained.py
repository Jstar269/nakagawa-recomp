# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Executable evidence for the host-neutral contained-delete seam.

src/rt/vfs_contained_selftest.c builds synthetic hostile fixtures in a
temporary directory and asserts the containment contract against whichever
backend this host selects. This harness compiles and runs it three ways:

1. the host's real backend (Windows verified-handle, or POSIX
   descriptor-relative);
2. the same source with SR_CD_FORCE_UNSUPPORTED_BACKEND, proving the
   fail-closed backend refuses every entry point and destroys nothing;
3. under a strict-ISO feature profile, proving the seam still reaches a real
   backend rather than silently degrading (the #error is what makes a
   degradation impossible to miss).

No retail or game input is involved and nothing outside the process's own
temporary directory is touched.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
SELFTEST_C = ROOT / "src" / "rt" / "vfs_contained_selftest.c"
SEAM_H = ROOT / "src" / "rt" / "vfs_contained.h"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
RT = str(ROOT / "src" / "rt")

# -std=c11 is strict ISO; on glibc that hides the POSIX.1-2008 declarations the
# seam needs, so a POSIX build states its feature profile. Windows needs none.
POSIX_PROFILE = [] if os.name == "nt" else ["-D_POSIX_C_SOURCE=200809L"]


def _compile(tmpdir, name, extra_flags=(), std="c11"):
    exe = os.path.join(tmpdir, name + (".exe" if os.name == "nt" else ""))
    cmd = [CC, "-std=" + std, "-O1", "-Wall", "-Wextra", "-Werror", "-I", RT]
    cmd += list(extra_flags)
    cmd += ["-o", exe, str(SELFTEST_C)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        raise AssertionError(
            "vfs_contained_selftest.c did not compile ({}):\n{}".format(
                " ".join(extra_flags) or "default", build.stderr[-4000:]))
    return exe


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestContainedDeleteSelftest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import tempfile
        cls.tmp = tempfile.mkdtemp(prefix="vfscd_")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _run(self, exe):
        run = subprocess.run([exe], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("vfs_contained selftest: OK", run.stdout)
        return run.stdout

    def test_hostile_fixtures_pass_on_this_host_backend(self):
        exe = _compile(self.tmp, "vfscd_native", list(POSIX_PROFILE) + ["-DSR_CD_TEST_HOOKS"])
        out = self._run(exe)
        self.assertIn("contained=1",
                      "this host must select a real containment backend:\n" + out)
        # A run that quietly skipped every hostile case would look like a pass.
        # Report what actually executed so a skipped matrix stays visible.
        self.assertIn(": 233 checks, 0 skipped hostile case(s)", out)
        sys.stderr.write("\n[vfs_contained] " + out.strip().replace("\n", "\n[vfs_contained] ") + "\n")

    def test_unsupported_host_refuses_every_entry_point(self):
        exe = _compile(self.tmp, "vfscd_unsup",
                       list(POSIX_PROFILE) + ["-DSR_CD_FORCE_UNSUPPORTED_BACKEND"])
        out = self._run(exe)
        self.assertIn("backend=unsupported", out)
        self.assertIn("contained=0", out)

    def test_strict_iso_build_still_reaches_a_real_backend(self):
        """A feature-profile mistake must not silently demote containment.

        Two halves of the documented contract, both checked here:

        - Included FIRST (as savedata.c includes it), the seam selects its own
          POSIX.1-2008 feature profile, so even a strict -std=c11 build gets the
          descriptor-relative backend.
        - Included LATE, after a system header has already fixed the profile,
          the seam cannot recover -- and stops the build with its #error instead
          of quietly falling back to pathname deletion. Loud, never quiet."""
        exe = _compile(self.tmp, "vfscd_strict", std="c11")
        out = self._run(exe)
        self.assertIn("contained=1", out,
                      "a strict-ISO build must still reach a real backend")
        if os.name != "nt":
            self.assertIn("backend=posix-descriptor-relative", out)

        late = os.path.join(self.tmp, "late_include.c")
        with open(late, "w", encoding="utf-8", newline="\n") as handle:
            handle.write('#include <stdio.h>\n'
                         '#include "vfs_contained.h"\n'
                         'int main(void) { return sr_cd_backend_is_contained() ? 0 : 1; }\n')
        bare = subprocess.run([CC, "-std=c11", "-I", RT, "-fsyntax-only", late],
                              capture_output=True, text=True)
        if os.name == "nt":
            # Windows selects its backend from _WIN32 alone, so include order
            # cannot demote it and there is nothing for the #error to catch.
            self.assertEqual(bare.returncode, 0, bare.stderr[-2000:])
            return
        self.assertNotEqual(bare.returncode, 0,
                            "a POSIX build that includes the seam too late must FAIL, not degrade")
        self.assertIn("no contained-delete backend", bare.stderr)

    def test_savedata_includes_the_seam_before_any_system_header(self):
        """The include-order contract above is only honoured if savedata.c
        actually follows it, so pin that too."""
        text = (ROOT / "src" / "rt" / "savedata.c").read_text(encoding="utf-8")
        seam = text.index('#include "vfs_contained.h"')
        for other in ('#include "recomp.h"', "#include <dirent.h>", "#include <stdio.h>",
                      "#include <sys/stat.h>"):
            self.assertLess(seam, text.index(other),
                            "vfs_contained.h must be included before " + other)


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestSeamPortability(unittest.TestCase):
    """The seam must stay buildable for hosts this machine cannot execute.

    macOS and the BSDs are not available here, so their execution is NOT
    claimed. What is checked is the property that would break portability
    silently: the POSIX backend must depend only on POSIX.1-2008 primitives,
    never on a Linux-only one."""

    LINUX_ONLY = ("openat2", "RENAME_NOREPLACE", "statx", "AT_EMPTY_PATH",
                  "O_PATH", "SYS_openat2", "__NR_", "memfd_create")

    def test_posix_backend_uses_no_linux_only_primitive(self):
        text = SEAM_H.read_text(encoding="utf-8")
        for name in self.LINUX_ONLY:
            self.assertNotIn(name, text,
                             f"the POSIX backend must not depend on the Linux-only {name!r}; "
                             "a Linux-specific route may only ever be an optional enhancement")

    def test_posix_backend_uses_only_posix_2008_primitives(self):
        """Every primitive the backend calls is POSIX.1-2008, so macOS and the
        BSDs compile the same code path even though they are not executed here."""
        text = SEAM_H.read_text(encoding="utf-8")
        posix_2008 = ("openat", "fdopendir", "unlinkat", "fstatat", "AT_REMOVEDIR",
                      "AT_SYMLINK_NOFOLLOW", "O_DIRECTORY", "O_NOFOLLOW")
        for name in posix_2008:
            self.assertIn(name, text, f"{name} is expected in the POSIX backend")
        self.assertIn("_POSIX_C_SOURCE 200809L", text,
                      "the required feature profile must be stated in the header itself")
        self.assertIn("defined(O_DIRECTORY)", text,
                      "backend selection must be a capability probe, not a platform guess")


if __name__ == "__main__":
    unittest.main()
