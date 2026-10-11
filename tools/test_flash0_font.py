# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression for the read-only flash0: font device (src/rt/flash0_font.c).

The driver lays out a synthetic root with tools/pgf_writer.py: the project fonts for the latin and
japanese slots, and the user-imported latin cache. The cache is written by the Python importer
(tools/nk_core/fonts.py, ``import_fonts``), which also writes the manifest the native reader and
the player check, so the two sides share one writer. File names and the cache layout come from
src/core/nk_font_slots.h, the header the device and the import flow share.

The native part runs the HLE selftest's ``--flash0-font`` mode once per case, against production
hle.c IO entry points:

* ``valid``: the manifest lists the cache file and its size and SHA-256 match, so the device serves
  the imported latin font;
* ``tampered``: one bit of the cache file changes after the import (same size, still accepted by
  the reader), so the SHA-256 refuses it and the project latin font is served;
* ``missing-manifest``: the manifest is removed, so the cache file is refused and the project font
  is served;
* ``malformed-manifest``: the manifest is not valid JSON, so the cache file is refused;
* ``unlisted``: the manifest is valid but lists only another slot, so the latin file is refused.

In every case the selftest also points ``SR_FONTDIR`` at the imported cache, the launcher's state
after an import, and checks that the project source does not serve the cache's files. Each refusal
is named on stderr once per slot and reason, however many opens resolve the slot. The native part
needs mingw32-make. The contract tests need no compiler: they check the fixture, the Python
checker's verdict for each case, and that every link that compiles the device also supplies
src/core/nk_font.c.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import pgf_writer  # noqa: E402
from nk_core import fonts  # noqa: E402

SLOT_HEADER = ROOT / "src" / "core" / "nk_font_slots.h"
MAKEFILE = ROOT / "Makefile"

CASES = ("valid", "tampered", "missing-manifest", "malformed-manifest", "unlisted")
#: The Python checker's verdict on the cache for each case (tools/nk_core/fonts.py). The unlisted
#: manifest lists a korean entry whose file is absent, so the checker names that entry.
CACHE_STATUS = {
    "valid": "OK",
    "tampered": "INVALID",
    "missing-manifest": "MISSING",
    "malformed-manifest": "INVALID",
    "unlisted": "INVALID",
}
#: The refusal each case names for the latin slot, once. None: the cache file is served.
USER_REFUSAL = {
    "valid": None,
    "tampered": "its size or SHA-256 differs from the manifest",
    "missing-manifest": "the font cache has no manifest.json",
    "malformed-manifest": "PSP font manifest is malformed",
    "unlisted": "the manifest does not list the Latin slot",
}
USER_REFUSAL_PREFIX = "flash0: user-imported font for slot 'latin' refused:"
PROJECT_CACHE_PREFIX = "flash0: project font for slot 'latin' refused:"

# Glyph sets. Each names its slot under the reader's probes: latin covers 'A' and 'a' and no kana or
# Hangul; japanese covers hiragana. The cache copy and the project latin font differ in size, so
# the source choice is observable.
LATIN_CACHE_CODES = (0x41, 0x61, 0x62, 0x63, 0x64)
LATIN_PROJECT_CODES = tuple(range(0x41, 0x5B)) + tuple(range(0x61, 0x7B))
JAPANESE_PROJECT_CODES = (0x3042, 0x3044, 0x3046, 0x3048, 0x304A)


def header_string(name: str) -> str:
    """The string value of ``#define <name> "<value>"`` in the shared slot header."""
    match = re.search(
        rf'^#define {name} "([^"]+)"$', SLOT_HEADER.read_text(encoding="utf-8"), re.MULTILINE
    )
    if match is None:
        raise AssertionError(f"{name} is not a string define in {SLOT_HEADER.name}")
    return match.group(1)


# The Makefile default (GAME_NAME ?= mygame). It is passed explicitly, spelled the same way,
# so make reuses the ordinary objects and the selftest binary lands at a known path.
BUILD_DIR = "build/mygame"


def synthetic_pgf(codes: tuple[int, ...]) -> bytes:
    """A dense PGF with one 4x4 glyph per code point in ``codes``."""
    glyphs = [
        pgf_writer.Glyph(
            code=code,
            width=4,
            height=4,
            samples=tuple((index + sample) % 16 for sample in range(16)),
        )
        for index, code in enumerate(codes)
    ]
    return pgf_writer.build_pgf(glyphs)


def cache_dir(root: Path) -> Path:
    """The per-user cache directory the device and the importer both read: <data>/fonts/v2."""
    return root / "data" / header_string("NK_FONT_CACHE_PARENT") / header_string("NK_FONT_CACHE_SUBDIR")


def write_fixture(root: Path) -> dict[Path, bytes]:
    """Lay out the project fonts and the imported cache under ``root``.

    The user's cache copy is imported with the Python importer, which copies it into the cache and
    writes the manifest. Returns the bytes each written file holds.
    """
    latin = header_string("NK_FONT_SLOT_LATIN_FILE")
    japanese = header_string("NK_FONT_SLOT_JAPANESE_FILE")
    project_latin = synthetic_pgf(LATIN_PROJECT_CODES)
    project_japanese = synthetic_pgf(JAPANESE_PROJECT_CODES)
    cache_latin = synthetic_pgf(LATIN_CACHE_CODES)
    files = {root / "project" / latin: project_latin, root / "project" / japanese: project_japanese}
    for path, data in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    source = root / "source"
    source.mkdir(parents=True, exist_ok=True)
    (source / latin).write_bytes(cache_latin)
    result = fonts.import_fonts(source, user_data_root=root / "data")
    if result["status"] != "OK":
        raise AssertionError(f"the fixture import did not write a cache: {result['outcomes']}")
    files[cache_dir(root) / latin] = cache_latin
    return files


def tampered_cache_bytes(original: bytes) -> bytes:
    """The cache file with one bit changed: same size, still a latin image the reader accepts."""
    for index in range(len(original) - 1, -1, -1):
        for bit in range(8):
            candidate = bytearray(original)
            candidate[index] ^= 1 << bit
            try:
                info = fonts.validate_pgf_data(bytes(candidate))
            except fonts.FontValidationError:
                continue
            if info["slot"] == "latin":
                return bytes(candidate)
    raise AssertionError("no same-size variant of the cache file is a latin image the reader accepts")


def apply_case(root: Path, case: str) -> None:
    """Change the fixture the way the case names, after the import wrote the manifest."""
    latin = cache_dir(root) / header_string("NK_FONT_SLOT_LATIN_FILE")
    manifest = cache_dir(root) / fonts.MANIFEST_NAME
    if case == "valid":
        return
    if case == "tampered":
        latin.write_bytes(tampered_cache_bytes(latin.read_bytes()))
    elif case == "missing-manifest":
        manifest.unlink()
    elif case == "malformed-manifest":
        manifest.write_text("{\n  \"schema_version\": 2,\n", encoding="utf-8")
    elif case == "unlisted":
        # A valid manifest that lists only a korean entry, so the latin file is not vouched for.
        korean = header_string("NK_FONT_SLOT_KOREAN_FILE")
        document = {
            "schema_version": fonts.MANIFEST_SCHEMA_VERSION,
            "import_time": "2026-10-10T00:00:00Z",
            "files": {korean: {"slot": "korean", "size": 1, "sha256": "0" * 64,
                               "source": fonts.MANIFEST_SOURCE_USER,
                               "reader_version": fonts.READER_VERSION}},
        }
        manifest.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    else:
        raise ValueError(f"unknown case {case!r}")


def refusal_lines(stderr: str, prefix: str) -> list[str]:
    return [line for line in stderr.splitlines() if line.startswith(prefix)]


def assert_refusals_named(test: unittest.TestCase, stderr: str, case: str) -> None:
    """Each refusal the case names appears exactly once on stderr, whatever the open count."""
    user = refusal_lines(stderr, USER_REFUSAL_PREFIX)
    expected = USER_REFUSAL[case]
    if expected is None:
        test.assertEqual(user, [], "a vouched-for cache file must not be refused")
    else:
        test.assertEqual(len(user), 1, "expected one refusal line, got: " + repr(user))
        test.assertIn(expected, user[0])
    project = refusal_lines(stderr, PROJECT_CACHE_PREFIX)
    test.assertEqual(
        len(project), 0 if case == "valid" else 1,
        "the SR_FONTDIR-at-cache refusal for " + case + " is expected once: " + repr(project),
    )


class Flash0FontFixtureContractTests(unittest.TestCase):
    """Compiler-free: the fixture is a valid import, and each case changes only what it names."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="flash0-fixture-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.files = write_fixture(self.root)
        self.latin = header_string("NK_FONT_SLOT_LATIN_FILE")
        self.japanese = header_string("NK_FONT_SLOT_JAPANESE_FILE")

    def manifest(self) -> dict:
        return json.loads((cache_dir(self.root) / fonts.MANIFEST_NAME).read_text(encoding="utf-8"))

    def test_fixture_fonts_name_their_slots(self) -> None:
        cache = fonts.validate_pgf_data((cache_dir(self.root) / self.latin).read_bytes())
        project_latin = fonts.validate_pgf_data((self.root / "project" / self.latin).read_bytes())
        project_japanese = fonts.validate_pgf_data((self.root / "project" / self.japanese).read_bytes())
        self.assertEqual(cache["slot"], "latin")
        self.assertEqual(project_latin["slot"], "latin")
        self.assertEqual(project_japanese["slot"], "japanese")
        self.assertNotEqual(cache["size"], project_latin["size"])

    def test_imported_manifest_vouches_for_the_cache_file(self) -> None:
        manifest = self.manifest()
        self.assertEqual(manifest["schema_version"], fonts.MANIFEST_SCHEMA_VERSION)
        self.assertEqual(sorted(manifest["files"]), [self.latin])
        entry = manifest["files"][self.latin]
        data = (cache_dir(self.root) / self.latin).read_bytes()
        self.assertEqual(entry["slot"], "latin")
        self.assertEqual(entry["size"], len(data))
        self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(entry["source"], "user")
        self.assertEqual(entry["reader_version"], fonts.READER_VERSION)
        self.assertEqual(fonts.inspect_font_cache(cache_dir=cache_dir(self.root))[0], "OK")

    def test_tampered_case_keeps_size_and_reader_acceptance(self) -> None:
        before = (cache_dir(self.root) / self.latin).read_bytes()
        apply_case(self.root, "tampered")
        after = (cache_dir(self.root) / self.latin).read_bytes()
        entry = self.manifest()["files"][self.latin]
        self.assertEqual(len(after), len(before))
        self.assertEqual(len(after), entry["size"])
        self.assertNotEqual(hashlib.sha256(after).hexdigest(), entry["sha256"])
        self.assertEqual(fonts.validate_pgf_data(after)["slot"], "latin")
        self.assertEqual(fonts.inspect_font_cache(cache_dir=cache_dir(self.root))[0], CACHE_STATUS["tampered"])

    def test_missing_manifest_case_keeps_only_the_manifest_out(self) -> None:
        apply_case(self.root, "missing-manifest")
        self.assertFalse((cache_dir(self.root) / fonts.MANIFEST_NAME).exists())
        self.assertTrue((cache_dir(self.root) / self.latin).is_file())
        self.assertEqual(fonts.inspect_font_cache(cache_dir=cache_dir(self.root))[0], CACHE_STATUS["missing-manifest"])

    def test_each_case_has_the_checker_verdict_it_names(self) -> None:
        for case in CASES:
            with self.subTest(case=case):
                root = Path(tempfile.mkdtemp(prefix=f"flash0-{case}-"))
                self.addCleanup(shutil.rmtree, root, True)
                write_fixture(root)
                apply_case(root, case)
                status = fonts.inspect_font_cache(cache_dir=cache_dir(root))[0]
                self.assertEqual(status, CACHE_STATUS[case])

    def test_every_link_of_the_device_supplies_nk_font(self) -> None:
        makefile = MAKEFILE.read_text(encoding="utf-8")
        logical = makefile.replace("\\\n", " ")
        recipes = [line for line in logical.splitlines()
                   if line.startswith("\t") and "src/rt/flash0_font.c" in line]
        self.assertTrue(recipes, "no Makefile recipe compiles the flash0 device")
        for line in recipes:
            self.assertIn("src/core/nk_font.c", line, line[:160])
        runtime = re.search(r"(?ms)^RT_SRCS\s*:=\s*(.*?)^RT_OBJS\s*:=", makefile)
        self.assertIsNotNone(runtime)
        self.assertIn("src/core/nk_font.c", runtime.group(1).replace("\\\n", " ").split())


class Flash0FontDeviceTests(unittest.TestCase):
    def test_native_device_contract_on_synthetic_fonts(self) -> None:
        make = shutil.which("mingw32-make")
        if not make:
            raise unittest.SkipTest("mingw32-make is not available")
        with tempfile.TemporaryDirectory(prefix="flash0-font-") as tmp:
            tmp_root = Path(tmp)
            # Build beneath the scratch tree, never the checkout's build/.
            build_root = tmp_root / "build"
            build_dir = build_root / "flash0-font"
            build = subprocess.run(
                [make, "--no-print-directory", f"BUILD_ROOT={build_root.as_posix()}",
                 f"BUILD_DIR={build_dir.as_posix()}", "hle-thread-selftest-build"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr[-4000:] or build.stdout[-4000:])
            exe = build_dir / "hle_thread_selftest.exe"
            for case in CASES:
                with self.subTest(case=case):
                    root = tmp_root / case
                    write_fixture(root)
                    apply_case(root, case)
                    env = dict(os.environ)
                    env["SR_FLASH0_TEST_ROOT"] = str(root)
                    env["SR_FONTDIR"] = str(root / "project")
                    env["SR_FLASH0_TEST_CASE"] = case
                    env.pop("SR_FONTLOG", None)
                    run = subprocess.run(
                        [str(exe), "--flash0-font"],
                        cwd=ROOT,
                        env=env,
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout=300,
                    )
                    self.assertIn("flash0-font:", run.stderr)
                    self.assertEqual(run.returncode, 0, run.stderr[-6000:])
                    assert_refusals_named(self, run.stderr, case)


if __name__ == "__main__":
    unittest.main()
