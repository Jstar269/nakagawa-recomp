# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Static checks of the Android build (android/, #360) against the rest of the repository.

The Android app is built with Gradle and the NDK, which hosted CI does not run, so the contracts
it shares with the desktop build are checked here, without an SDK:

- the runtime inventory nk_add_title_runtime() compiles (CMakeLists.txt) is the Makefile's
  runtime link (RT_SRCS with the public backends and the POSIX platform, RT_GE_O, the ATRAC3+
  import and bridge, the FFmpeg H.264 subset), so a source added to one is not missing from the
  other;
- SDL3 and SDL3_ttf are pinned to the commits hosted CI builds;
- the app asks for no permission at all (the product is offline, AGENTS.md section 5);
- the names the native and Java halves use to find each other agree;
- machine-local Android state stays out of the repository.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / "android"


def _make_assignments(*paths: Path) -> dict[str, list[str]]:
    """Every `NAME :=|?=|= value` in the files, continuation lines joined, in order."""
    out: dict[str, list[str]] = {}
    for path in paths:
        text = path.read_text(encoding="utf-8").replace("\\\n", " ")
        for line in text.splitlines():
            match = re.match(r"^([A-Za-z0-9_]+)\s*(?::=|\?=|=)\s*(.*)$", line)
            if match:
                out.setdefault(match.group(1), []).append(match.group(2))
    return out


def _expand(value: str, assignments: dict[str, list[str]], choose: dict[str, str]) -> list[str]:
    words: list[str] = []
    for word in value.split():
        match = re.fullmatch(r"\$\(([A-Za-z0-9_]+)\)", word)
        if not match:
            words.append(word)
            continue
        name = match.group(1)
        candidates = assignments.get(name, [])
        chosen = choose.get(name)
        if chosen is not None:
            candidates = [c for c in candidates if chosen in c] or candidates
        words += _expand(candidates[0], assignments, choose) if candidates else []
    return words


def _cmake_list(text: str, name: str) -> list[str]:
    match = re.search(r"^set\(" + re.escape(name) + r"\b(.*?)\)", text, re.S | re.M)
    if not match:
        raise AssertionError(f"CMakeLists.txt has no set({name} ...)")
    return [w for w in match.group(1).split() if not w.startswith("#")]


class AndroidRuntimeInventoryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.make = _make_assignments(ROOT / "Makefile", ROOT / "mk" / "nk_ui.mk")
        # The Android runtime is a POSIX build: the POSIX platform backend, and the H.264 subset
        # (H264_FFMPEG=1 off Windows) rather than the empty list.
        self.choose = {"PLAYER_PLATFORM_SRC": "posix", "H264_SUB_SRCS": "nk_h264_api.c"}

    def test_runtime_sources_match_the_makefile_link(self) -> None:
        make_rt = _expand(self.make["RT_SRCS"][0], self.make, self.choose)
        self.assertIn("src/core/nk_platform_posix.c", make_rt)
        ui = (_cmake_list(self.cmake, "NK_UI_CORE_SOURCES") +
              _cmake_list(self.cmake, "NK_UI_EDGE_SOURCES") + ["src/ui/backend/nk_ui_sdlr.c"] +
              _cmake_list(self.cmake, "NK_UI_OVERLAY_C9C_SOURCES") +
              ["src/rt/overlay/overlay_host.c"])
        cmake_rt = _cmake_list(self.cmake, "NK_RUNTIME_SOURCES") + ui
        self.assertEqual(sorted(cmake_rt), sorted(make_rt))
        self.assertEqual(len(cmake_rt), len(set(cmake_rt)), "a runtime source is listed twice")

    def test_ge_atrac_and_h264_sources_match(self) -> None:
        self.assertIn("$(RT_GE_O): src/rt/ge.c", (ROOT / "Makefile").read_text(encoding="utf-8"))
        self.assertEqual(_cmake_list(self.cmake, "NK_RUNTIME_GE_SOURCE"), ["src/rt/ge.c"])
        self.assertEqual(_cmake_list(self.cmake, "NK_RUNTIME_ATRAC3P_SOURCES"),
                         _expand(self.make["ATRAC3P_SRCS"][0], self.make, self.choose))
        self.assertEqual(_cmake_list(self.cmake, "NK_RUNTIME_ATRAC3P_BRIDGE_SOURCE"),
                         ["src/rt/atrac3p_bridge.c"])
        self.assertEqual(_cmake_list(self.cmake, "NK_RUNTIME_H264_SUB_SOURCES"),
                         _expand("$(H264_SUB_SRCS)", self.make, self.choose))

    def test_every_listed_source_exists(self) -> None:
        for name in ("NK_RUNTIME_SOURCES", "NK_RUNTIME_ATRAC3P_SOURCES",
                     "NK_RUNTIME_H264_SUB_SOURCES"):
            for source in _cmake_list(self.cmake, name):
                self.assertTrue((ROOT / source).is_file(), f"{name}: {source} does not exist")


class AndroidAppContractTests(unittest.TestCase):
    def test_sdl_pins_match_hosted_ci(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        deps = (ANDROID / "app" / "deps.cmake").read_text(encoding="utf-8")
        for ci_name, cmake_name in (("SDL3_COMMIT", "NK_SDL3_COMMIT"),
                                    ("SDL3_TTF_COMMIT", "NK_SDL3_TTF_COMMIT")):
            ci_pin = re.search(ci_name + r"=([0-9a-f]{40})", ci)
            android_pin = re.search(r"set\(" + cmake_name + r" ([0-9a-f]{40})\)", deps)
            self.assertIsNotNone(ci_pin, ci_name)
            self.assertIsNotNone(android_pin, cmake_name)
            self.assertEqual(android_pin.group(1), ci_pin.group(1), cmake_name)

    def test_manifest_requests_no_permissions(self) -> None:
        for manifest in ANDROID.glob("app/src/*/AndroidManifest.xml"):
            text = manifest.read_text(encoding="utf-8")
            self.assertNotIn("uses-permission", text, f"{manifest} asks for a permission")

    def test_title_activity_runs_in_its_own_process(self) -> None:
        text = (ANDROID / "app" / "src" / "main" / "AndroidManifest.xml").read_text(encoding="utf-8")
        block = re.search(r'<activity\s+android:name="\.TitleActivity"(.*?)/>', text, re.S)
        self.assertIsNotNone(block)
        self.assertIn('android:process=":title"', block.group(1))
        self.assertIn('android:exported="false"', block.group(1))
        # One runtime per process: a second start must not run main() again in it.
        self.assertIn('android:launchMode="singleTask"', block.group(1))

    def test_native_and_java_names_agree(self) -> None:
        java = ANDROID / "app" / "src" / "main" / "java" / "io" / "github" / "jstar269" / "nakagawa"
        launcher = (java / "LauncherActivity.java").read_text(encoding="utf-8")
        title = (java / "TitleActivity.java").read_text(encoding="utf-8")
        posix = (ROOT / "src" / "core" / "nk_platform_posix.c").read_text(encoding="utf-8")
        shim = (ANDROID / "app" / "src" / "main" / "cpp" / "nk_android_title.c").read_text(encoding="utf-8")
        self.assertIn('FindClass(env, "io/github/jstar269/nakagawa/LauncherActivity")', posix)
        self.assertIn('"startTitle"', posix)
        self.assertRegex(launcher, r"public static boolean startTitle\(String library, String\[\] args, "
                                   r"String\[\] env, String cwd,\s+String logPath\)")
        # (String, String[], String[], String, String) -> boolean
        self.assertIn('"(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;"\n'
                      '        "Ljava/lang/String;)Z"', posix)
        self.assertIn('return "nk_android_title_main";', title)
        self.assertIn("int nk_android_title_main(int argc, char **argv)", shim)

    def test_local_android_state_is_ignored(self) -> None:
        ignore = (ROOT / ".gitignore").read_text(encoding="utf-8")
        for entry in ("/android/local.properties", "/android/.gradle/", "/android/app/.cxx/",
                      "/android/.idea/", "/android/app/build/"):
            self.assertIn(entry, ignore.splitlines())


if __name__ == "__main__":
    unittest.main()
