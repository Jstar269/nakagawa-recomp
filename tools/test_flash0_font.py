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
is named on stderr once per slot and reason, however many opens resolve the slot. The selftest also
checks that ``SR_USER_DATA_ROOT`` names the user source (its per-user directory is an empty decoy
there), and that a relative value is named once and skips that source instead of falling back. The native part
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
import scratch_dir  # noqa: E402
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
#: Named once when SR_USER_DATA_ROOT is set but is not an absolute path (the selftest sets one).
USER_ROOT_PREFIX = "flash0: SR_USER_DATA_ROOT"

# Glyph sets. Each names its slot under the reader's probes: latin covers 'A' and 'a' and no kana or
# Hangul; japanese covers hiragana. The cache copy and the project latin font differ in size, so
# the source choice is observable.
LATIN_CACHE_CODES = (0x41, 0x61, 0x62, 0x63, 0x64)
LATIN_PROJECT_CODES = tuple(range(0x41, 0x5B)) + tuple(range(0x61, 0x7B))
JAPANESE_PROJECT_CODES = (0x3042, 0x3044, 0x3046, 0x3048, 0x304A)
FLASH0_HEADER = ROOT / "src" / "rt" / "flash0_font.h"
VFS_HEADER = ROOT / "src" / "rt" / "vfs_contained.h"
SELFTEST_SOURCE = ROOT / "src" / "rt" / "hle_thread_selftest.c"


def header_string(name: str) -> str:
    """The string value of ``#define <name> "<value>"`` in the shared slot header."""
    match = re.search(
        rf'^#define {name} "([^"]+)"$', SLOT_HEADER.read_text(encoding="utf-8"), re.MULTILINE
    )
    if match is None:
        raise AssertionError(f"{name} is not a string define in {SLOT_HEADER.name}")
    return match.group(1)


def c_unsigned_define(path: Path, name: str) -> int:
    """The value of ``#define <name> 0x...u`` in a C source file."""
    match = re.search(
        rf"^#define {name}\s+(0x[0-9A-Fa-f]+)u\b", path.read_text(encoding="utf-8"), re.MULTILINE
    )
    if match is None:
        raise AssertionError(f"{name} is not an unsigned hex define in {path.name}")
    return int(match.group(1), 16)


# The Makefile default (GAME_NAME ?= mygame). It is passed explicitly, spelled the same way,
# so make reuses the ordinary objects and the selftest binary lands at a known path.
BUILD_DIR = "build/mygame"

# hle-thread-selftest-build writes hle_thread_selftest.exe on every host: the output name is
# spelled in the recipe, not taken from EXE_EXT. Only the make program varies by platform.
# The harness includes <windows.h> without a _WIN32 guard and calls Win32 file APIs, so it does
# not build off Windows. Until it is ported, that is a named SKIP there, decided from the source
# before any build (docs/LINUX_DEVELOPMENT.md lists the gap). The check retires itself: once the
# include is guarded, the same test builds the selftest with make.


def unguarded_windows_include(source: str) -> bool:
    """True when ``#include <windows.h>`` is outside every conditional that names ``_WIN32``."""
    guards: list[bool] = []
    for line in source.splitlines():
        stripped = line.strip()
        if not stripped.startswith("#"):
            continue
        directive = stripped[1:].strip()
        if re.match(r"if(n?def)?\b", directive):
            guards.append("_WIN32" in directive)
        elif re.match(r"endif\b", directive):
            if guards:
                guards.pop()
        elif re.match(r"include\s*<windows\.h>", directive, re.IGNORECASE) and not any(guards):
            return True
    return False


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


def function_body(source: str, signature: str) -> str:
    """The text of one C function, from its signature to the closing brace at column zero."""
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


class Flash0UserRootSourceContractTests(unittest.TestCase):
    """SR_USER_DATA_ROOT validation, checked in the source on every host.

    The device selftest that exercises these branches on Windows does not build off Windows yet,
    so these contracts run everywhere, WSL included. They pin the checks the selftest proves.
    """

    def setUp(self) -> None:
        self.source = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")

    def test_relative_user_root_is_refused_on_every_host(self) -> None:
        """#827 review: Win32 refuses a relative SR_USER_DATA_ROOT inside the wide helper, but POSIX
        resolves it against the working directory. flash0_user_data_root must check absoluteness
        itself, on every host, and name the refusal with the same diagnostic."""
        signature = "static void flash0_user_data_root(char *dst, size_t capacity) {"
        self.assertIn(signature, self.source)
        body = function_body(self.source, signature)
        # sr_wide_is_absolute is the host's own rule: a drive or UNC path on Win32, a leading '/' on POSIX.
        self.assertIn("sr_wide_is_absolute(configured)", body)
        self.assertIn("is configured but is not a valid absolute path", body)

    def test_empty_user_root_is_named_not_treated_as_unset(self) -> None:
        """An empty SR_USER_DATA_ROOT is present, so it is refused by name. Only an absent variable
        takes the platform directory. The launcher relies on this to mask an inherited value."""
        body = function_body(self.source, "static void flash0_user_data_root(char *dst, size_t capacity) {")
        self.assertIn("configured[0] != L'\\0'", body)
        self.assertIn("if (!configured_present)", body)


class Flash0ConfiguredRootSourceContractTests(unittest.TestCase):
    """Configured roots (SR_FONTDIR, SR_DATAROOT, SR_EXE_DIR) must be absolute on every host, checked in
    the source so it runs everywhere, WSL included.

    sr_wide_configured_root_wide_alloc has a Win32 and a POSIX definition in hle.c. Win32 refused a relative
    value; POSIX joined it to the working directory, so a child process started in a package directory read
    a different directory from the one the caller configured (#827 review of #1025).
    """

    SIGNATURE = "static int sr_wide_configured_root_wide_alloc(const wchar_t *value, wchar_t **out) {"

    def setUp(self) -> None:
        self.source = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        self.assertEqual(self.source.count(self.SIGNATURE), 2, "expected one Win32 and one POSIX definition")
        self.first = self.source.index(self.SIGNATURE)
        self.second = self.source.index(self.SIGNATURE, self.first + 1)

    def test_posix_configured_root_refuses_a_relative_value(self) -> None:
        """The POSIX helper refuses a non-absolute value and never anchors it to the working directory."""
        posix = self.source[self.second:self.source.index("\n}\n", self.second)]
        self.assertNotIn("sr_wide_full_path_alloc(wide", posix)
        self.assertRegex(posix, r"if \(!sr_wide_is_absolute\(wide\)\) \{\s*free\(wide\);\s*return 0;")

    def test_win32_configured_root_refuses_a_relative_value(self) -> None:
        """The Win32 definition, which the POSIX one now matches, refuses anything not absolute."""
        win32 = self.source[self.first:self.source.index("\n}\n", self.first)]
        self.assertRegex(
            win32,
            r"if \(!\(sr_wide_is_extended\(wide\) \|\| sr_wide_is_absolute\(wide\)\)\) \{\s*free\(wide\);\s*return 0;",
        )

    def test_each_configured_root_reader_goes_through_the_helper(self) -> None:
        """SR_EXE_DIR (module directory), SR_FONTDIR (font_load and the module font root) and SR_DATAROOT
        (host_data) are read through the helper, so one refusal covers all three."""
        for call in (
            "sr_wide_configured_root_wide_alloc(configured, &full)",
            "sr_wide_configured_root_wide_alloc(configured, out)",
            "sr_wide_configured_root_wide_alloc(configured, &root)",
            "sr_wide_configured_root_wide_alloc(configured_root, &root_wide)",
        ):
            with self.subTest(call=call):
                self.assertIn(call, self.source)

    def test_flash0_font_sources_names_a_refused_font_dir(self) -> None:
        """The flash0: device names a refused SR_FONTDIR once, and does not fall back to another root."""
        body = function_body(self.source, "static void flash0_font_sources(Flash0Sources *out) {")
        self.assertIn('flash0_refuse_font_dir("could not be read")', body)
        self.assertIn('flash0_refuse_font_dir("is configured but is not a valid absolute path")', body)
        self.assertIn("atomic_exchange_explicit(&s_font_dir_refusal_named", self.source)


#: A scratch harness for the native check. It #includes the production hle.c and resolves the flash0:
#: project font root twice, so the one-time refusal is counted across two opens.
FONT_DIR_HARNESS_C = r'''
#include "src/rt/hle.c"

int main(void) {
    Flash0Sources first;
    Flash0Sources second;
    flash0_font_sources(&first);
    flash0_font_sources(&second);
    printf("PROJECT=%s\n", first.project_dir);
    printf("PROJECT_AGAIN=%s\n", second.project_dir);
    return 0;
}
'''

FONT_DIR_REFUSAL = (
    "flash0: SR_FONTDIR is configured but is not a valid absolute path; the project font source is skipped"
)
FONT_DIR_UNREADABLE = "flash0: SR_FONTDIR could not be read; the project font source is skipped"


class Flash0FontDirRefusalNativeTests(unittest.TestCase):
    """A refused SR_FONTDIR is named once on stderr, and the project source is skipped, on every host with a
    C compiler (WSL and hosted Linux included). Elsewhere the class is skipped by name.

    The harness links the production hle.c with its other entry points left unresolved. That is safe here:
    flash0_font_sources reaches only SR_USER_DATA_ROOT (set absolute, so nk_platform_get_path is never
    called), SR_FONTDIR (always set in these cases), and the wide helpers.
    """

    @classmethod
    def setUpClass(cls) -> None:
        if sys.platform == "win32":
            # The mingw linker has no unresolved-symbol option, and section GC still leaves the hle.c
            # globals referenced, so the harness cannot link here. The Windows lane is covered by the
            # source contract above and by hle-thread-selftest.
            raise unittest.SkipTest("the native harness links on POSIX only; Windows is covered by the source contract")
        compiler = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
        if compiler is None:
            raise unittest.SkipTest("no C compiler on PATH")
        cls.scratch = scratch_dir.TemporaryDirectory(prefix="flash0-fontdir-")
        cls.tmp = Path(cls.scratch.name)
        source = cls.tmp / "harness.c"
        source.write_text(FONT_DIR_HARNESS_C, encoding="utf-8")
        cls.exe = cls.tmp / "harness"
        build = subprocess.run(
            [compiler, "-std=gnu11", "-w", f"-I{ROOT}", f"-I{ROOT / 'src' / 'rt'}",
             f"-I{ROOT / 'src' / 'core'}", f"-I{ROOT / 'src' / 'core' / 'generated'}",
             f"-I{ROOT / 'src' / 'rt' / 'atrac3p'}", f"-I{ROOT / 'src' / 'rt' / 'atrac3p' / 'libavcodec'}",
             f"-I{ROOT / 'src' / 'rt' / 'atrac3p' / 'libavutil'}",
             str(source), "-o", str(cls.exe), "-Wl,--unresolved-symbols=ignore-all",
             "-lpthread", "-lm"],
            capture_output=True, text=True, check=False, timeout=600,
        )
        if build.returncode != 0:
            cls.scratch.cleanup()
            raise AssertionError("the flash0 font-dir harness did not build: " + build.stderr[-4000:])

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "scratch"):
            cls.scratch.cleanup()

    def run_case(self, cwd: Path, font_dir: str | None) -> dict:
        env = dict(os.environ)
        env.pop("SR_FONTDIR", None)
        env["SR_USER_DATA_ROOT"] = str(self.tmp / "user-data")
        if font_dir is not None:
            env["SR_FONTDIR"] = font_dir
        run = subprocess.run([str(self.exe)], cwd=cwd, env=env, capture_output=True, text=True,
                             check=False, timeout=120)
        self.assertEqual(run.returncode, 0, run.stderr[-4000:])
        fields = dict(line.split("=", 1) for line in run.stdout.splitlines() if "=" in line)
        return {"project": fields.get("PROJECT"), "again": fields.get("PROJECT_AGAIN"),
                "stderr": run.stderr}

    @staticmethod
    def named(stderr: str, line: str) -> int:
        return sum(1 for text in stderr.splitlines() if text == line)

    def test_relative_font_dir_is_named_once_and_skipped(self) -> None:
        case = self.tmp / "relative-case"
        (case / "relative-project").mkdir(parents=True, exist_ok=True)
        result = self.run_case(case, "relative-project")
        self.assertEqual(result["project"], "")
        self.assertEqual(result["again"], "")
        self.assertEqual(self.named(result["stderr"], FONT_DIR_REFUSAL), 1, result["stderr"])

    def test_empty_font_dir_is_named_once_and_skipped(self) -> None:
        result = self.run_case(self.tmp, "")
        self.assertEqual(result["project"], "")
        self.assertEqual(self.named(result["stderr"], FONT_DIR_REFUSAL), 1, result["stderr"])

    @unittest.skipIf(sys.platform == "win32", "Windows environment values are UTF-16; no invalid UTF-8 spelling")
    def test_unreadable_font_dir_is_named_once_and_skipped(self) -> None:
        result = self.run_case(self.tmp, "\udcff")
        self.assertEqual(result["project"], "")
        self.assertEqual(self.named(result["stderr"], FONT_DIR_UNREADABLE), 1, result["stderr"])

    def test_absolute_font_dir_is_served_without_a_refusal(self) -> None:
        project = self.tmp / "project"
        project.mkdir(parents=True, exist_ok=True)
        result = self.run_case(self.tmp, str(project))
        self.assertIn("project", result["project"] or "")
        self.assertIn("project", result["again"] or "")
        self.assertFalse([text for text in result["stderr"].splitlines() if text.startswith("flash0: SR_FONTDIR")])


class Flash0FontDeviceTests(unittest.TestCase):
    def test_windows_include_detector(self) -> None:
        """The skip below is decided by this reader, so it must tell a guarded include apart."""
        cases = {
            "#include <windows.h>\n": True,
            "#ifdef _WIN32\n#include <windows.h>\n#endif\n": False,
            "#if defined(_WIN32) && !defined(X)\n# include <Windows.h>\n#endif\n": False,
            "#ifdef _WIN32\n#if X\n#endif\n#include <windows.h>\n#endif\n": False,
            "#ifdef _WIN32\n#endif\n#include <windows.h>\n": True,
            "#if SR_FEATURE\n#include <windows.h>\n#endif\n": True,
            "/* #include <windows.h> */\n#include <stdio.h>\n": False,
        }
        for source, expected in cases.items():
            with self.subTest(source=source):
                self.assertIs(unguarded_windows_include(source), expected)

    def test_not_found_code_matches_the_vfs_and_the_selftest_pin(self) -> None:
        # The device has the one not-found code the rest of the VFS returns: sr_cd_psp_error
        # maps SR_CD_NOT_FOUND to 0x80010002u (ENOENT), while 0x80010014u is ENOTDIR. The
        # selftest pins the same value, so a drift on any side fails here without a compiler.
        vfs = VFS_HEADER.read_text(encoding="utf-8")
        mapping = re.search(r"case SR_CD_NOT_FOUND: return (0x[0-9A-Fa-f]+)u;", vfs)
        self.assertIsNotNone(mapping, "sr_cd_psp_error must map SR_CD_NOT_FOUND")
        vfs_not_found = int(mapping.group(1), 16)
        self.assertEqual(vfs_not_found, 0x80010002)
        header_value = c_unsigned_define(FLASH0_HEADER, "SR_FLASH0_ERR_NOT_FOUND")
        self.assertEqual(header_value, vfs_not_found)
        self.assertEqual(c_unsigned_define(SELFTEST_SOURCE, "F0T_ERR_NOT_FOUND"), header_value)

    def test_native_device_contract_on_synthetic_fonts(self) -> None:
        if sys.platform != "win32" and unguarded_windows_include(
            SELFTEST_SOURCE.read_text(encoding="utf-8")
        ):
            raise unittest.SkipTest(
                f"{SELFTEST_SOURCE.name} includes <windows.h> without a _WIN32 guard, so the "
                f"selftest does not build on {sys.platform} yet "
                "(docs/LINUX_DEVELOPMENT.md, 'What doesn't yet')"
            )
        make_tool = "mingw32-make" if sys.platform == "win32" else "make"
        make = shutil.which(make_tool)
        if not make:
            raise unittest.SkipTest(f"{make_tool} is not available")
        with scratch_dir.TemporaryDirectory(prefix="flash0-font-") as tmp:
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
                    env.pop("SR_USER_DATA_ROOT", None)
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
                    # The relative SR_USER_DATA_ROOT the selftest sets is named once, however many opens follow.
                    named = refusal_lines(run.stderr, USER_ROOT_PREFIX)
                    self.assertEqual(len(named), 1, "expected one SR_USER_DATA_ROOT refusal, got: " + repr(named))
                    self.assertIn("is configured but is not a valid absolute path", named[0])


if __name__ == "__main__":
    unittest.main()
