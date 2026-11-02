# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The local problem report bundle (#1849): redaction, exclusions, the preview and no network."""

from __future__ import annotations

import ast
import datetime as dt
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from nk_core import report_bundle as rb  # noqa: E402

USER = "Jasonquill"
HOST = "QUILL-DESKTOP"
WIN_HOME = "C:\\Users\\Jasonquill"
POSIX_HOME = "/home/jasonquill"
NOW = dt.datetime(2026, 11, 2, 12, 30, 44, tzinfo=dt.timezone.utc)
IDENTITY = rb.Identity(home_dirs=(WIN_HOME, POSIX_HOME, "D:\\Profiles\\jasonquill"),
                       user_names=(USER, "jasonquill"), host_names=(HOST,))


def no_doctor(_root: Path) -> str:
    return "doctor ok for <user>"


def no_gpu() -> list[str]:
    return ["Test GPU | driver 1.2.3"]


def build(inputs: rb.ReportInputs, **kwargs: object) -> rb.BundlePlan:
    return rb.build_plan(inputs, IDENTITY, ROOT, now=NOW, doctor=no_doctor, gpu=no_gpu,
                         memory=lambda: 16 * 1024 ** 3, **kwargs)  # type: ignore[arg-type]


class RedactionTests(unittest.TestCase):
    def assertClean(self, text: str) -> str:
        out = rb.redact_text(text, IDENTITY)
        for needle in ("jasonquill", "quill-desktop"):
            self.assertNotIn(needle, out.lower(), msg=f"{text!r} -> {out!r}")
        return str(out)

    def test_windows_path_forms(self) -> None:
        cases = {
            "C:\\Users\\Jasonquill\\AppData\\Local\\Nakagawa": "~\\AppData\\Local\\Nakagawa",
            "C:/Users/Jasonquill/AppData/Local": "~/AppData/Local",
            "c:\\users\\jasonquill\\Desktop\\a.iso": "~\\Desktop\\a.iso",
            "C:\\\\Users\\\\Jasonquill\\\\x": "~\\\\x",           # JSON-escaped
            "\\\\?\\C:\\Users\\Jasonquill\\x": "~\\x",
            "file:///C:/Users/Jasonquill/x": "~/x",
            "/mnt/c/Users/Jasonquill/x": "~/x",
            "D:\\Profiles\\jasonquill\\games": "~\\games",
        }
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(self.assertClean(text), expected)

    def test_posix_path_forms(self) -> None:
        cases = {
            "/home/jasonquill/.local/share/nakagawa-recomp": "~/.local/share/nakagawa-recomp",
            "/var/home/jasonquill/x": "~/x",
            "/Users/jasonquill/Library/Application Support/NakagawaRecomp":
                "~/Library/Application Support/NakagawaRecomp",
            "path=/home/jasonquill": "path=~",
            "(/home/jasonquill/x)": "(~/x)",
        }
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(self.assertClean(text), expected)

    def test_another_account_and_shape_only(self) -> None:
        """A path under a profile this machine does not name is still a home folder."""
        # A profile name with a space is matched whole when this machine names it.
        spaced = rb.Identity(home_dirs=("C:\\Users\\Jane Quill",), user_names=("Jane Quill",))
        self.assertEqual(rb.redact_text("C:\\Users\\Jane Quill\\x and Jane Quill", spaced),
                         "~\\x and <user>")
        self.assertEqual(rb.redact_text("C:\\Users\\other\\x", IDENTITY), "~\\x")
        self.assertEqual(rb.redact_text("/home/other/x", IDENTITY), "~/x")
        self.assertEqual(rb.redact_text("C:\\Users\\Public\\Documents", IDENTITY),
                         "C:\\Users\\Public\\Documents")
        self.assertEqual(rb.redact_text("C:\\Users\\Jasonquill2\\x", IDENTITY), "~\\x")

    def test_names(self) -> None:
        self.assertEqual(self.assertClean("user=Jasonquill, USER: JASONQUILL."), "user=<user>, USER: <user>.")
        self.assertEqual(self.assertClean("host QUILL-DESKTOP up"), "host <host> up")
        # Whole words only: a longer word that merely contains the name stays.
        self.assertEqual(rb.redact_text("Jasonquills_game", IDENTITY), "Jasonquills_game")
        # A name under three characters is not a safe thing to search for.
        short = rb.Identity(user_names=("ab",))
        self.assertEqual(rb.redact_text("a cab ab", short), "a cab ab")

    def test_unrelated_text_is_unchanged(self) -> None:
        text = "[PLAYER] boot ok\nC:\\Games\\psp\\game.iso\n/opt/nakagawa/bin\n"
        self.assertEqual(rb.redact_text(text, IDENTITY), text)

    def test_environment_identity_reads_the_account(self) -> None:
        env = {"USERPROFILE": "C:\\Users\\Zed", "HOME": "/home/zed", "USERNAME": "Zed", "USER": "zed",
               "COMPUTERNAME": "ZED-PC"}
        with mock.patch.dict(os.environ, env, clear=False):
            identity = rb.Identity.from_environment()
        self.assertIn("C:\\Users\\Zed", identity.home_dirs)
        self.assertIn("Zed", identity.user_names)
        self.assertIn("ZED-PC", identity.host_names)


class ExclusionTests(unittest.TestCase):
    def test_game_data_saves_dumps_traces_and_keys_are_refused(self) -> None:
        refused = [
            "game.iso", "boot.prx", "EBOOT.PBP", "disc.cso", "disc.chd", "memory.pmem",
            "frame-0001.png", "capture.ppm", "crash.dmp", "run.trace", "gpu.dump",
            "keys/psp.key", "saves/ULUS/DATA.BIN", "memstick/PSP/SAVEDATA/x.txt", "ms0/x.txt",
            "decrypted/boot.txt", "discs/x.txt", "titles/ULUS/log.txt", "savedata.sav",
            "idstorage.txt", "fuseid.txt", "license.rif", "private_key.txt", "x.pem",
            "console-secret.txt", "libs.dll", "tool.exe",
        ]
        for name in refused:
            with self.subTest(name=name):
                self.assertIsNotNone(rb.exclusion_reason(name))

    def test_logs_and_settings_pass(self) -> None:
        for name in ("logs/game-ULUS10537-last.txt", "logs/developer-log.txt", "settings/settings.json",
                     "about.txt", "nk_doctor.txt"):
            with self.subTest(name=name):
                self.assertIsNone(rb.exclusion_reason(name))

    def test_binary_renamed_to_text_is_not_text(self) -> None:
        self.assertFalse(rb.looks_like_text(b"~PSP\x00\x01\x02" + bytes(range(256))))
        self.assertTrue(rb.looks_like_text("plain log line\nwith é\r\n".encode("utf-8")))


class Fixture:
    """A synthetic user data tree: the allow-listed inputs plus every kind of file that must stay out."""

    def __init__(self, base: Path) -> None:
        self.data = base / "Nakagawa" / "data"
        self.logs = base / "Nakagawa" / "logs"
        self.config = base / "Nakagawa" / "config"
        for folder in (self.data, self.logs, self.config):
            folder.mkdir(parents=True)
        home = WIN_HOME
        (self.logs / "game-ULUS10537-last.txt").write_text(
            f"boot {home}\\AppData\\Local\\Nakagawa\\data\nuser {USER} on {HOST}\n", encoding="utf-8")
        (self.logs / "game-ULUS10537-prev.txt").write_text(f"older run {POSIX_HOME}/x\n", encoding="utf-8")
        (self.logs / "game-ULUS99999-last.txt").write_text("another title\n", encoding="utf-8")
        (self.logs / "developer-log.txt").write_text(f"[player] {home}\\x\n", encoding="utf-8")
        (self.logs / "player-output.txt").write_text("player output\n", encoding="utf-8")
        (self.config / "settings.json").write_text(json.dumps(
            {"library_dir": f"{home}\\Games", "update_token": "SECRET-TOKEN-VALUE", "volume": 7}), encoding="utf-8")
        # Planted files that must never appear: beside the allow-list, so a "walk and filter"
        # implementation would take them.
        (self.logs / "save-DATA.sav").write_bytes(b"SAVE-MARKER" + bytes(64))
        (self.logs / "memory.pmem").write_bytes(b"DUMP-MARKER" + bytes(64))
        (self.logs / "run.trace").write_text("TRACE-MARKER\n", encoding="utf-8")
        (self.logs / "psp.key").write_text("KEY-MARKER\n", encoding="utf-8")
        (self.logs / "frame-0001.png").write_bytes(b"\x89PNG FRAME-MARKER")
        (self.config / "fuseid.json").write_text('{"fuse": "FUSE-MARKER"}', encoding="utf-8")
        (self.config / "keys.json").write_text('{"k": "KEY-MARKER"}', encoding="utf-8")
        (self.config / "input_profiles").mkdir()
        (self.config / "input_profiles" / "ULUS10537.json").write_text("{}", encoding="utf-8")
        (self.data / "saves" / "ULUS10537").mkdir(parents=True)
        (self.data / "saves" / "ULUS10537" / "DATA.BIN").write_bytes(b"SAVE-MARKER")
        (self.data / "titles" / "ULUS10537" / "decrypted").mkdir(parents=True)
        (self.data / "titles" / "ULUS10537" / "decrypted" / "boot.bin").write_bytes(b"GAME-MARKER")
        (self.data / "discs").mkdir()
        (self.data / "discs" / "ULUS10537.iso").write_bytes(b"GAME-MARKER")
        (self.data / "packages" / "ULUS10537-authentic").mkdir(parents=True)
        (self.logs / "flight-recorder.json").write_text(json.dumps({
            "schema": 7, "reason": "fault", "build": f"{home}\\build", "events": [{"pc": 1}] * 500,
            "memory": {"0x1000": "MEMORY-MARKER"}}), encoding="utf-8")

    def inputs(self, **kwargs: object) -> rb.ReportInputs:
        values: dict[str, object] = dict(user_data_root=self.data, logs_dir=self.logs, config_dir=self.config,
                                         title_id="ULUS10537", title_name="Test Game: The Report", region="US")
        values.update(kwargs)
        return rb.ReportInputs(**values)  # type: ignore[arg-type]


FORBIDDEN_MARKERS = (b"SAVE-MARKER", b"DUMP-MARKER", b"TRACE-MARKER", b"KEY-MARKER", b"FRAME-MARKER",
                     b"FUSE-MARKER", b"GAME-MARKER", b"MEMORY-MARKER", b"SECRET-TOKEN-VALUE")
IDENTITY_STRINGS = (USER.lower().encode(), HOST.lower().encode(), b"c:\\users\\jasonquill",
                    b"/home/jasonquill")


class BundleTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.base = Path(self._tmp.name)
        self.fx = Fixture(self.base)

    def written(self, plan: rb.BundlePlan) -> zipfile.ZipFile:
        path = rb.write_bundle(plan, self.base / "reports", now=NOW)
        return zipfile.ZipFile(path)

    def test_expected_files_are_present(self) -> None:
        plan = build(self.fx.inputs())
        with self.written(plan) as archive:
            names = set(archive.namelist())
        self.assertEqual(names, {
            "about.txt", "logs/game-ULUS10537-last.txt", "logs/game-ULUS10537-prev.txt",
            "logs/developer-log.txt", "logs/player-output.txt", "settings/settings.json",
            "flight-recorder-summary.json", "nk_doctor.txt",
        })
        self.assertEqual(plan.archive_name, "20261102T123044Z-test-game-the-report.zip")

    def test_planted_save_dump_trace_and_key_are_absent(self) -> None:
        plan = build(self.fx.inputs())
        with self.written(plan) as archive:
            for info in archive.infolist():
                data = archive.read(info)
                for marker in FORBIDDEN_MARKERS:
                    self.assertNotIn(marker, data, msg=f"{marker!r} in {info.filename}")
            for name in archive.namelist():
                for bad in (".sav", ".pmem", ".trace", ".key", ".png", "fuseid", "keys.json", "input_profiles"):
                    self.assertNotIn(bad, name)
        skipped = {label for label, _ in plan.skipped}
        self.assertNotIn("logs/save-DATA.sav", skipped)  # never even a candidate, not merely refused
        self.assertFalse(any("DATA.BIN" in label or "iso" in label for label in skipped))

    def test_no_line_holds_the_user_name_or_home_path(self) -> None:
        plan = build(self.fx.inputs())
        with self.written(plan) as archive:
            for info in archive.infolist():
                data = archive.read(info).lower()
                for needle in IDENTITY_STRINGS:
                    self.assertNotIn(needle, data, msg=f"{needle!r} in {info.filename}")
            log = archive.read("logs/game-ULUS10537-last.txt").decode()
            self.assertIn("boot ~\\AppData\\Local\\Nakagawa\\data", log)
            self.assertIn("user <user> on <host>", log)
            settings = json.loads(archive.read("settings/settings.json"))
            self.assertEqual(settings["library_dir"], "~\\Games")
            self.assertEqual(settings["update_token"], "<removed>")
            self.assertEqual(settings["volume"], 7)

    def test_flight_recorder_summary_has_sizes_not_contents(self) -> None:
        plan = build(self.fx.inputs())
        with self.written(plan) as archive:
            summary = json.loads(archive.read("flight-recorder-summary.json"))
        self.assertEqual(summary["schema"], 7)
        self.assertEqual(summary["events_count"], 500)
        self.assertEqual(summary["memory_count"], 1)
        self.assertNotIn("events", summary)
        self.assertEqual(summary["build"], "~\\build")

    def test_about_names_the_machine_title_and_files(self) -> None:
        plan = build(self.fx.inputs())
        about = next(e for e in plan.entries if e.arcname == "about.txt").data.decode()
        for needle in ("Nothing was uploaded", "Nakagawa version:", "Source commit:", "Tools version:", "OS:",
                       "CPU:", "RAM: 16384 MiB", "GPU and driver: Test GPU | driver 1.2.3",
                       "Title id: ULUS10537", "Region: US", "package built; game staged",
                       "logs/game-ULUS10537-last.txt", "Made: 2026-11-02 12:30:44Z"):
            self.assertIn(needle, about)

    def test_preview_lists_exactly_what_is_written(self) -> None:
        plan = build(self.fx.inputs())
        preview = {item["name"]: item["bytes"] for item in plan.preview()}
        with self.written(plan) as archive:
            self.assertEqual({i.filename: i.file_size for i in archive.infolist()}, preview)

    def test_without_a_title_the_newest_runtime_logs_are_taken(self) -> None:
        os.utime(self.fx.logs / "game-ULUS10537-last.txt", (1, 1))
        plan = build(self.fx.inputs(title_id="", title_name=""))
        names = {e.arcname for e in plan.entries}
        self.assertIn("logs/game-ULUS99999-last.txt", names)
        self.assertEqual(plan.archive_name, "20261102T123044Z-launcher.zip")

    def test_a_binary_renamed_to_a_log_is_skipped(self) -> None:
        (self.fx.logs / "game-ULUS10537-prev.txt").write_bytes(b"~PSP\x00\x00" + bytes(range(256)))
        plan = build(self.fx.inputs())
        self.assertNotIn("logs/game-ULUS10537-prev.txt", {e.arcname for e in plan.entries})
        self.assertIn(("logs/game-ULUS10537-prev.txt", "is not plain text"), plan.skipped)

    def test_long_log_keeps_its_tail_from_a_whole_line(self) -> None:
        lines = "".join(f"line {i:07d}\n" for i in range(300000))
        (self.fx.logs / "player-output.txt").write_bytes(lines.encode("utf-8"))
        plan = build(self.fx.inputs())
        entry = next(e for e in plan.entries if e.arcname == "logs/player-output.txt")
        text = entry.data.decode()
        self.assertLessEqual(len(entry.data), rb.MAX_LOG_BYTES + 64)
        self.assertTrue(text.startswith("["))
        self.assertTrue(text.split("\n", 1)[1].startswith("line "))
        self.assertTrue(text.endswith("line 0299999\n"))
        self.assertIn("left out", entry.note)

    def test_bad_title_id_is_refused(self) -> None:
        with self.assertRaises(rb.ReportError):
            build(self.fx.inputs(title_id="../../etc"))

    def test_write_failure_is_named_and_leaves_nothing(self) -> None:
        blocker = self.base / "reports"
        blocker.write_text("a file where the folder belongs", encoding="utf-8")
        plan = build(self.fx.inputs())
        with self.assertRaises(rb.ReportError):
            rb.write_bundle(plan, blocker, now=NOW)
        out = self.base / "out"
        out.mkdir()
        with mock.patch("os.replace", side_effect=OSError(5, "denied")):
            with self.assertRaises(rb.ReportError):
                rb.write_bundle(plan, out, now=NOW)
        self.assertEqual(list(out.iterdir()), [])

    def test_a_missing_logs_and_config_folder_still_gives_about(self) -> None:
        empty = self.base / "empty"
        plan = build(rb.ReportInputs(user_data_root=empty, logs_dir=empty / "logs", config_dir=empty / "config"))
        self.assertEqual([e.arcname for e in plan.entries], ["about.txt", "nk_doctor.txt"])

    def test_layout_resolution(self) -> None:
        logs, config = rb.resolve_layout(self.fx.data)
        self.assertEqual((logs, config), (self.fx.logs, self.fx.config))   # the Windows default layout
        (self.fx.data / "logs").mkdir()
        self.assertEqual(rb.resolve_layout(self.fx.data)[0], self.fx.data / "logs")  # --user-data-root layout
        given = self.base / "somewhere"
        self.assertEqual(rb.resolve_layout(self.fx.data, given, given)[0], given)


class NoNetworkTests(unittest.TestCase):
    def test_module_imports_no_network_library(self) -> None:
        tree = ast.parse((ROOT / "tools" / "nk_core" / "report_bundle.py").read_text(encoding="utf-8"))
        imported: set[str] = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported.update(a.name.split(".")[0] for a in node.names)
            elif isinstance(node, ast.ImportFrom) and node.module:
                imported.add(node.module.split(".")[0])
        self.assertFalse(imported & {"socket", "urllib", "http", "ftplib", "smtplib", "requests", "ssl",
                                     "asyncio", "xmlrpc", "webbrowser"}, imported)

    def test_building_and_writing_opens_no_connection(self) -> None:
        def refuse(*_a: object, **_k: object) -> None:
            raise AssertionError("the report touched the network")

        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(socket, "socket", side_effect=refuse), \
                mock.patch.object(socket, "create_connection", side_effect=refuse), \
                mock.patch.object(socket, "getaddrinfo", side_effect=refuse):
            fx = Fixture(Path(tmp))
            plan = build(fx.inputs())
            rb.write_bundle(plan, Path(tmp) / "reports", now=NOW)


class CliTests(unittest.TestCase):
    def run_cli(self, tmp: Path, *extra: str, fx: Fixture) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ, USERPROFILE=WIN_HOME, HOME=POSIX_HOME, USERNAME=USER, USER=USER,
                   COMPUTERNAME=HOST)
        return subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "report", "--no-doctor", "--json",
             "--user-data-root", str(fx.data), "--logs-dir", str(fx.logs), "--config-dir", str(fx.config),
             "--title", "ULUS10537", "--title-name", "Test Game", *extra],
            capture_output=True, text=True, env=env, cwd=str(ROOT), timeout=120, check=False)

    def test_preview_writes_nothing_and_report_writes_one_zip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            preview = self.run_cli(Path(tmp), "--preview", fx=fx)
            self.assertEqual(preview.returncode, 0, preview.stderr)
            listed = json.loads(preview.stdout)
            self.assertIsNone(listed["path"])
            self.assertFalse((fx.data / "reports").exists())
            self.assertIn("about.txt", [f["name"] for f in listed["files"]])
            done = self.run_cli(Path(tmp), fx=fx)
            self.assertEqual(done.returncode, 0, done.stderr)
            result = json.loads(done.stdout)
            written = Path(result["path"])
            self.assertEqual(written.parent, fx.data / "reports")
            self.assertEqual([f["name"] for f in result["files"]], [f["name"] for f in listed["files"]])
            with zipfile.ZipFile(written) as archive:
                self.assertEqual(sorted(archive.namelist()), sorted(f["name"] for f in listed["files"]))
                for info in archive.infolist():
                    data = archive.read(info)
                    for marker in FORBIDDEN_MARKERS:
                        self.assertNotIn(marker, data)
                    self.assertNotIn(USER.lower().encode(), data.lower())

    def test_bad_title_id_exits_with_a_message(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            done = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "report", "--no-doctor", "--preview",
                 "--user-data-root", str(fx.data), "--title", "../x"],
                capture_output=True, text=True, cwd=str(ROOT), timeout=120, check=False)
            self.assertEqual(done.returncode, 1)
            self.assertIn("Report error", done.stderr)


if __name__ == "__main__":
    unittest.main()
