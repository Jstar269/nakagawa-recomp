#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contributor quick check in a checkout whose own path contains spaces (#368).

An outside contributor clones the public repository wherever their tooling puts
it, and a checkout under a user-profile directory whose name contains spaces is
not an exotic location. This module
copies the TRACKED tree -- the bytes on disk, including uncommitted work, so the
check exercises the change under review and not the last commit -- into a
temporary directory whose path contains spaces, and runs a bounded contributor
command set there.

Bounded on purpose: ``make check`` and ``make readiness`` remain the
authoritative gates and ``mingw32-make native-core-tests`` covers the native
build. This is a relocation smoke, so it runs the Makefile parse, one hermetic C
selftest that compiles and executes, and the Python tooling tests whose fixtures
touch the build system. Every subprocess carries an explicit timeout, and the
module is skipped -- with the reason printed -- only when a tool it needs is
absent from this host.

Why it is not ``git archive``: that reads committed blobs, so an uncommitted fix
would be invisible to the very test meant to pin it.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

#: The directory name embeds spaces on purpose; the assertion below proves the
#: harness still tested a spaced path if a host overrides TEMP.
SPACED_DIR_NAME = "nakagawa relocated clone"

#: Per-command wall-clock budget. Generous for a cold C compile on a loaded
#: runner, far below the hosted shard timeout.
COMMAND_TIMEOUT_S = 900


def tracked_paths(root: Path) -> list[str]:
    """Tracked paths, read from Git with NUL separation so no name is split."""
    proc = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        capture_output=True,
        check=True,
        timeout=120,
    )
    return [
        item.decode("utf-8", errors="surrogateescape")
        for item in proc.stdout.split(b"\0")
        if item
    ]


def copy_tracked_tree(source: Path, destination: Path) -> int:
    """Copy every tracked file that still exists. Returns the number copied.

    A path deleted in the working tree is skipped rather than failing: the
    question this test asks is whether the surviving tree builds and checks from
    an arbitrary path, and a staged deletion is an unrelated concern.
    """
    copied = 0
    for relative in tracked_paths(source):
        origin = source / relative
        if not origin.is_file():
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(origin, target)
        copied += 1
    return copied


class RelocatedCloneTests(unittest.TestCase):
    """The documented contributor commands, run from a path with spaces."""

    clone: Path
    make: str
    failures: list[str]

    @classmethod
    def setUpClass(cls) -> None:
        cls.make = shutil.which("mingw32-make") or shutil.which("make")
        if not cls.make:
            reason = "GNU Make is required for the relocated-contributor check"
            print(f"SKIP {__name__}: {reason} (no mingw32-make or make on PATH)")
            raise unittest.SkipTest(reason)
        if not (shutil.which("git") or (ROOT / ".git").exists()):
            reason = "Git is required to enumerate the tracked tree"
            print(f"SKIP {__name__}: {reason}")
            raise unittest.SkipTest(reason)
        if not (os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")):
            reason = "a C compiler is required to build the relocated selftest"
            print(f"SKIP {__name__}: {reason} (no gcc or cc on PATH)")
            raise unittest.SkipTest(reason)

        base = Path(tempfile.mkdtemp(prefix=f"{SPACED_DIR_NAME} "))
        cls.clone = base / "nakagawa"
        cls.clone.mkdir()
        cls.copied = copy_tracked_tree(ROOT, cls.clone)
        cls.failures = []
        if " " not in str(cls.clone):
            # Unreachable on a sane host, but a harness that silently lost its
            # space would report a meaningless pass.
            raise AssertionError(f"relocated clone path carries no space: {cls.clone}")

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(getattr(cls, "clone", Path()).parent, ignore_errors=True)

    def run_step(self, label: str, command: list[str]) -> None:
        proc = subprocess.run(
            command,
            cwd=self.clone,
            capture_output=True,
            text=True,
            check=False,
            timeout=COMMAND_TIMEOUT_S,
        )
        if proc.returncode == 0:
            return
        self.failures.append(
            f"--- {label} ({proc.returncode})\n"
            f"  command: {' '.join(command)}\n"
            f"  stdout: {proc.stdout[-2000:]}\n"
            f"  stderr: {proc.stderr[-2000:]}"
        )

    def test_the_relocated_path_really_contains_a_space(self) -> None:
        self.assertIn(" ", str(self.clone))
        self.assertGreater(self.copied, 100, "the tracked tree was not copied")
        self.assertTrue((self.clone / "Makefile").is_file())
        self.assertTrue((self.clone / "tools" / "contrib_check.py").is_file())

    def test_relocated_contributor_quick_check(self) -> None:
        """Makefile parse, one hermetic C selftest, and the path-sensitive tests."""
        self.run_step("make help", [self.make, "--no-print-directory", "help"])
        self.run_step(
            "strbuf-selftest",
            [self.make, "--no-print-directory", "strbuf-selftest"],
        )
        self.run_step(
            "documentation lint",
            [sys.executable, "tools/lint_docs.py"],
        )
        self.run_step(
            "build-truth tooling tests",
            [
                sys.executable, "-m", "unittest",
                "tools.test_build_truth.BuildTruthTests",
                "tools.test_build_truth.BuildArtifactLifecycleTests",
            ],
        )
        # A whitespace-bearing BUILD_DIR must never reach a Make recipe: it used to
        # be split into several targets, which left a spaces/ tree in the checkout
        # root and made the publication audit reject the litter.
        self.assertFalse(
            (self.clone / "spaces").exists(),
            "the relocated run created a spaces/ tree in the checkout root",
        )
        # The step labels go last on purpose: a caller that keeps only the tail of
        # a failure (mingw32-make contrib-check prints the final lines) still sees
        # which step failed.
        self.assertEqual(
            self.failures, [],
            "\n\n".join(self.failures)
            + f"\n\nfailed steps: {len(self.failures)} in {self.clone}",
        )


if __name__ == "__main__":
    unittest.main()
