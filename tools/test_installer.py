# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The per-user Windows installer: its script, pinned compiler and build tool (#2208, #324).

Everything here runs without Inno Setup except ``RealCompilerTests``, which skips (reported SKIP, never a
pass) when the pinned compiler is not installed.
"""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from typing import Any

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import build_installer  # noqa: E402
import stage_install  # noqa: E402
import verify_installer  # noqa: E402

SCRIPT = ROOT / "assets" / "installer" / "nakagawa.iss"
FAKE_BYTES = b"installer test bytes"


def _directive(text: str, name: str) -> str | None:
    match = re.search(rf"^{name}=(.*)$", text, re.MULTILINE)
    return match.group(1).strip() if match else None


def _code(text: str) -> str:
    """The script without its ``;`` comment lines, which name the very things the rules forbid."""
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith(";"))


def _section(text: str, name: str) -> str:
    match = re.search(rf"^\[{name}\]\n(.*?)(?=^\[|\Z)", text, re.MULTILINE | re.DOTALL)
    return match.group(1) if match else ""


class InstallerScriptTests(unittest.TestCase):
    """Rules the script keeps; each is a promise in the design (docs and issue #2208)."""

    @classmethod
    def setUpClass(cls):
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_it_installs_per_user_and_never_asks_for_administrator(self):
        self.assertEqual(_directive(self.text, "PrivilegesRequired"), "lowest")
        # Present and empty: the dialog never offers an all-users install.
        self.assertEqual(_directive(self.text, "PrivilegesRequiredOverridesAllowed"), "")
        self.assertEqual(_directive(self.text, "DefaultDirName"), r"{autopf}\{#AppName}")
        self.assertIn("x64compatible", _directive(self.text, "ArchitecturesAllowed") or "")

    def test_it_writes_nothing_all_users_and_no_registry_values_of_its_own(self):
        lowered = _code(self.text).lower()
        for forbidden in ("hklm", "{commonpf", "{commonappdata}", "{commondesktop", "{commonprograms",
                          "{sys}", "{win}", "{cf", "[registry]", "[ini]", "[services]", "{pf}", "{pf32}", "{pf64}"):
            self.assertNotIn(forbidden, lowered, forbidden)
        self.assertEqual(_directive(self.text, "ChangesEnvironment"), "no")
        self.assertEqual(_directive(self.text, "ChangesAssociations"), "no")

    def test_it_uses_no_network(self):
        self.assertEqual(verify_installer.egress_findings(_code(self.text)), [])
        self.assertEqual(verify_installer.egress_findings("x := DownloadTemporaryFile('https://a', 'b', '', nil);"),
                         ["INSTALLER_NETWORK_CALL: downloadtemporaryfile"])

    def test_the_app_id_is_the_one_the_verifier_and_upgrades_rely_on(self):
        self.assertEqual(_directive(self.text, "AppId"), "{{" + verify_installer.APP_ID + "}")

    def test_both_shortcuts_carry_the_players_taskbar_identity(self):
        self.assertIn(f'#define AppUserModelID "{stage_install.APP_USER_MODEL_ID}"', self.text)
        icons = [line for line in _section(self.text, "Icons").splitlines() if line.startswith("Name:")]
        self.assertEqual(len(icons), 2)
        for line in icons:
            self.assertIn('AppUserModelID: "{#AppUserModelID}"', line)
        self.assertIn("{userprograms}", icons[0])
        self.assertIn("{userdesktop}", icons[1])
        self.assertIn("Tasks: desktopicon", icons[1])
        self.assertIn("Flags: unchecked", _section(self.text, "Tasks"))

    def test_the_only_run_entry_is_the_optional_launch_and_never_silent(self):
        entries = [line for line in _section(self.text, "Run").splitlines() if line.strip()]
        self.assertEqual(len(entries), 1)
        self.assertIn("postinstall", entries[0])
        self.assertIn("skipifsilent", entries[0])
        self.assertIn("runasoriginaluser", entries[0])

    def test_the_files_are_the_package_and_get_deterministic_times(self):
        files = [line for line in _section(self.text, "Files").splitlines() if line.strip()]
        self.assertEqual(len(files), 1)
        self.assertTrue(files[0].startswith('Source: "{#PackageDir}\\*"'))
        self.assertIn("touch", files[0].split("Flags:")[1])
        self.assertEqual(_directive(self.text, "TouchDate"), "{#TouchDate}")
        self.assertEqual(_directive(self.text, "TouchTime"), "{#TouchTime}")
        self.assertEqual(_directive(self.text, "TimeStampsInUTC"), "yes")

    def test_a_running_player_is_closed_by_the_restart_manager_not_a_fixed_mutex(self):
        # The player names its single-instance mutex from a hash of its data root, so no fixed AppMutex exists.
        self.assertEqual(_directive(self.text, "CloseApplications"), "yes")
        self.assertIsNone(_directive(self.text, "AppMutex"))

    def test_the_developer_install_tools_folder_is_refused(self):
        self.assertIn(stage_install.MARKER, self.text)
        self.assertIn("function PrepareToInstall", self.text)


class UninstallRuleTests(unittest.TestCase):
    """The uninstall keeps user data unless the person ticks one box; a silent uninstall never deletes it."""

    @classmethod
    def setUpClass(cls):
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_there_is_no_silent_delete_flag(self):
        code = _code(self.text)
        self.assertNotIn("/DELETEUSERDATA", code.upper())  # the variable DeleteUserData is the box, not a flag
        self.assertNotIn("GetCmdTail", code)
        self.assertNotIn("CmdLineParamExists", code)

    def test_the_data_folders_are_the_five_the_docs_list(self):
        names = dict(re.findall(r"^\s+(\d): Result := '(\w+)';", self.text, re.MULTILINE))
        self.assertEqual([names[str(index)] for index in range(5)], ["config", "data", "cache", "logs", "saves"])
        self.assertIn("USER_DATA_FOLDER_COUNT = 5", self.text)
        setup_doc = (ROOT / "docs" / "SETUP.md").read_text(encoding="utf-8")
        rows = re.findall(r"^\| `(\w+)/` \|", setup_doc, re.MULTILINE)
        if not rows:
            self.skipTest("docs/SETUP.md has no data-folder table yet (#2164); this check starts then")
        self.assertEqual(sorted(names.values()), sorted(rows))

    def test_the_delete_is_reached_only_from_the_unchecked_box_and_never_when_silent(self):
        self.assertEqual(self.text.count("RemoveUserData"), 2)  # the procedure and its one call
        self.assertIn("Box.Checked := False;", self.text)
        call = re.search(r"CurUninstallStepChanged.*?end;\s*\Z", self.text, re.DOTALL)
        self.assertIsNotNone(call)
        self.assertIn("DeleteUserData and (not UninstallSilent)", call.group(0))
        init = self.text[self.text.index("function InitializeUninstall"):]
        # Silent returns before any form is built, with the flag still False.
        self.assertLess(init.index("if UninstallSilent then"), init.index("CreateCustomForm"))
        self.assertLess(init.index("DeleteUserData := False;"), init.index("if UninstallSilent then"))

    def test_the_delete_follows_no_link_and_names_only_the_per_user_root(self):
        body = self.text[self.text.index("procedure RemoveUserData"):self.text.index("{ The uninstall page")]
        self.assertIn("IsReparsePoint(Root)", body)
        self.assertIn("IsReparsePoint(Folder)", body)
        self.assertEqual(self.text.count("DelTree("), 1)
        self.assertIn("ExpandConstant('{localappdata}') + '\\Nakagawa'", self.text)
        # A second, defaulting-to-No confirmation precedes it.
        self.assertIn("MB_YESNO or MB_DEFBUTTON2", self.text)


def _lock() -> dict[str, Any]:
    lock: dict[str, Any] = build_installer.load_lock()
    return lock


class LockTests(unittest.TestCase):
    def test_the_checked_in_lock_is_valid_and_names_the_version_it_documents(self):
        lock = _lock()
        self.assertEqual(lock["version"], "6.7.3")
        self.assertEqual(lock["license"]["name"], "Inno Setup License")
        self.assertIn("ISCC.exe", lock["files"])

    def _written(self, mutate) -> Path:
        lock = copy.deepcopy(_lock())
        mutate(lock)
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        path = Path(temp.name) / "lock.json"
        path.write_text(json.dumps(lock), encoding="utf-8")
        return path

    def test_a_lock_that_is_not_pinned_by_hash_is_refused(self):
        cases = {
            "schema": lambda lock: lock.update(schema_version=2),
            "version": lambda lock: lock.update(version="six"),
            "url host": lambda lock: lock["installer"].update(
                url="https://example.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe"),
            "installer hash": lambda lock: lock["installer"].update(sha256="ABC"),
            "no compiler": lambda lock: lock["files"].pop("ISCC.exe"),
            "path in files": lambda lock: lock["files"].update({"sub/x.dll": "0" * 64}),
            "file hash": lambda lock: lock["files"].update({"ISCC.exe": "short"}),
            "licence": lambda lock: lock["license"].update(text_sha256="x"),
            "licence name": lambda lock: lock["license"].update(name="MIT"),
        }
        for label, mutate in cases.items():
            with self.subTest(label), self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_LOCK_INVALID"):
                build_installer.load_lock(self._written(mutate))

    def test_a_missing_or_unreadable_lock_is_refused(self):
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_LOCK_INVALID"):
            build_installer.load_lock(Path("does-not-exist.json"))


class BuildToolTests(unittest.TestCase):
    """build_installer with a fake compiler: the checks come before it and nothing is written when they fail."""

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.root = Path(self._temp.name)
        self.iscc_dir = self.root / "inno"
        self.iscc_dir.mkdir()
        self.lock = copy.deepcopy(_lock())
        self.lock["files"] = {"ISCC.exe": self._file(self.iscc_dir / "ISCC.exe", b"compiler"),
                              "Setup.e32": self._file(self.iscc_dir / "Setup.e32", b"loader")}
        self.package = self.root / "package"
        (self.package / "bin").mkdir(parents=True)
        for name, data in (("bin/nakagawa_player.exe", b"player"), ("bin/runtime-manifest.json", b"{}"),
                           ("LICENSE", b"licence"), ("docs/README.md", b"readme")):
            path = self.package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        stage_install.write_sha256sums(self.package)
        self.calls: list[list[str]] = []

    @staticmethod
    def _file(path: Path, data: bytes) -> str:
        path.write_bytes(data)
        digest: str = build_installer.sha256_file(path)
        return digest

    def _fake_run(self, *, returncode=0, produce=True):
        def run(command, **_kwargs):
            self.calls.append(command)
            if produce:
                out = Path(next(part[2:] for part in command if part.startswith("/O")))
                name = next(part[2:] for part in command if part.startswith("/F"))
                (out / f"{name}.exe").write_bytes(FAKE_BYTES)
            return subprocess.CompletedProcess(command, returncode, "", "boom" if returncode else "")
        return run

    def build(self, **overrides):
        arguments = dict(source_date_epoch=1793900000, iscc=self.iscc_dir / "ISCC.exe", lock=self.lock,
                         run=self._fake_run(), verify_runtime=lambda _bin: None, environ={})
        arguments.update(overrides)
        return build_installer.build_installer(self.package, "0.0.2", self.root / "out", **arguments)

    def test_it_compiles_with_the_documented_inputs_and_writes_the_checksum(self):
        summary = self.build()
        installer = Path(summary["installer"])
        self.assertEqual(installer.name, "nakagawa-recomp-0.0.2-windows-x64-setup.exe")
        self.assertEqual(Path(summary["checksum_file"]).read_text(encoding="utf-8"),
                         f"{summary['sha256']}  {installer.name}\n")
        command = self.calls[0]
        self.assertEqual(command[0], str(self.iscc_dir / "ISCC.exe"))
        for expected in (f"/DPackageDir={self.package.resolve()}", "/DAppVersion=0.0.2",
                         "/DTouchDate=2026-11-05", "/DTouchTime=17:33:20", "/Onakagawa"[:2]):
            self.assertTrue(any(part.startswith(expected) for part in command), expected)
        self.assertEqual(command[-1], str(build_installer.SCRIPT_PATH))
        self.assertEqual(summary["inno_setup"], "6.7.3")

    def test_the_time_inside_the_installer_follows_the_epoch_not_the_clock(self):
        self.assertEqual(build_installer.touch_for_epoch(1793900000), ("2026-11-05", "17:33:20"))
        self.assertEqual(build_installer.touch_for_epoch(0), ("1980-01-01", "00:00:00"))
        self.assertEqual(build_installer.compiler_command(
            Path("ISCC.exe"), Path("p"), "0.0.2", Path("o"), 1793900000),
            build_installer.compiler_command(Path("ISCC.exe"), Path("p"), "0.0.2", Path("o"), 1793900000))
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_EPOCH_MISSING"):
            build_installer.resolve_epoch(None, {})
        self.assertEqual(build_installer.resolve_epoch(None, {"SOURCE_DATE_EPOCH": "42"}), 42)

    def test_an_unpinned_compiler_is_refused_before_anything_is_compiled(self):
        (self.iscc_dir / "Setup.e32").write_bytes(b"patched loader")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_COMPILER_UNPINNED: Setup.e32"):
            self.build()
        self.assertEqual(self.calls, [])
        self.assertFalse((self.root / "out").exists())

    def test_a_missing_compiler_says_how_to_install_it(self):
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_COMPILER_MISSING"):
            self.build(iscc=self.root / "nowhere" / "ISCC.exe")

    def test_a_package_that_changed_after_its_checksums_is_refused(self):
        (self.package / "bin" / "nakagawa_player.exe").write_bytes(b"tampered")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_PACKAGE_CHANGED.*CHANGED: bin/nakagawa_player.exe"):
            self.build()
        (self.package / "extra.txt").write_text("x", encoding="utf-8")
        with self.assertRaisesRegex(build_installer.InstallerError, "UNLISTED: extra.txt"):
            self.build()
        self.assertEqual(self.calls, [])

    def test_a_folder_that_is_not_a_package_is_refused(self):
        (self.package / "LICENSE").unlink()
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_PACKAGE_INVALID: LICENSE"):
            self.build()
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_PACKAGE_MISSING"):
            build_installer.check_package(self.root / "nothing")

    def test_a_runtime_manifest_that_does_not_verify_is_refused(self):
        def reject(_bin):
            raise RuntimeError("RUNTIME_FILE_CHANGED: bin/SDL3.dll")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_RUNTIME_UNVERIFIED.*RUNTIME_FILE_CHANGED"):
            self.build(verify_runtime=reject)

    def test_an_existing_output_is_never_overwritten(self):
        self.build()
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_OUTPUT_EXISTS"):
            self.build()

    def test_an_output_inside_the_package_is_refused(self):
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_OUTPUT_INSIDE_PACKAGE"):
            build_installer.build_installer(self.package, "0.0.2", self.package / "out", source_date_epoch=1,
                                            iscc=self.iscc_dir / "ISCC.exe", lock=self.lock, environ={})

    def test_a_failed_compile_leaves_no_installer_and_shows_the_compilers_words(self):
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_COMPILE_FAILED: ISCC exited 2"):
            self.build(run=self._fake_run(returncode=2))
        self.assertEqual(list((self.root / "out").glob("*")), [])

    def test_a_version_that_is_not_semver_is_refused(self):
        for version in ("", "0.0", "v0.0.2", "0.0.2 beta", "../0.0.2"):
            with self.subTest(version), self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_VERSION_INVALID"):
                build_installer.build_installer(self.package, version, self.root / "o", source_date_epoch=1,
                                                lock=self.lock, environ={})

    def test_the_package_is_never_modified(self):
        before = {name: build_installer.sha256_file(path) for name, path in build_installer.files_under(self.package).items()}
        self.build()
        after = {name: build_installer.sha256_file(path) for name, path in build_installer.files_under(self.package).items()}
        self.assertEqual(before, after)


class VerifierPureTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.root = Path(self._temp.name)
        self.package = self.root / "package"
        (self.package / "bin").mkdir(parents=True)
        (self.package / "bin" / "a.dll").write_bytes(b"a")
        (self.package / "LICENSE").write_bytes(b"l")
        stage_install.write_sha256sums(self.package)
        self.installed = self.root / "installed"
        shutil.copytree(self.package, self.installed)

    def test_an_install_equal_to_its_package_matches_even_with_the_uninstaller_beside_it(self):
        (self.installed / "unins000.exe").write_bytes(b"u")
        (self.installed / "unins000.dat").write_bytes(b"d")
        self.assertEqual(verify_installer.compare_installed(self.package, self.installed), [])

    def test_a_changed_missing_or_extra_file_is_reported(self):
        (self.installed / "bin" / "a.dll").write_bytes(b"changed")
        (self.installed / "LICENSE").unlink()
        (self.installed / "bin" / "unins001.exe").write_bytes(b"x")  # only unins* at the top level is Inno's
        (self.installed / "stray.txt").write_text("x", encoding="utf-8")
        self.assertEqual(sorted(verify_installer.compare_installed(self.package, self.installed)),
                         ["CHANGED: bin/a.dll", "MISSING: LICENSE", "UNLISTED: bin/unins001.exe", "UNLISTED: stray.txt"])

    def test_a_malformed_checksum_list_is_refused(self):
        (self.package / "SHA256SUMS.txt").write_text("nothex  bin/a.dll\n", encoding="utf-8")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_PACKAGE_INVALID"):
            build_installer.read_sums(self.package)
        (self.package / "SHA256SUMS.txt").write_text(f"{'0' * 64}  ../outside\n", encoding="utf-8")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_PACKAGE_INVALID"):
            build_installer.read_sums(self.package)

    def test_the_check_refuses_to_run_off_windows(self):
        if os.name == "nt":
            self.skipTest("this host is Windows")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_VERIFY_UNSUPPORTED"):
            verify_installer.verify_installer(Path("x.exe"), self.package)


def _pinned_compiler() -> Path | None:
    try:
        iscc: Path = build_installer.find_iscc()
        build_installer.verify_toolchain(iscc, _lock())
    except build_installer.InstallerError:
        return None
    return iscc


@unittest.skipUnless(os.name == "nt" and _pinned_compiler() is not None, "the pinned Inno Setup is not installed (SKIP, not a pass)")
class RealCompilerTests(unittest.TestCase):
    """Compile the real script with the pinned compiler: two builds are byte-identical, and the file times follow the epoch."""

    def test_the_script_compiles_and_the_bytes_do_not_depend_on_the_path_or_the_clock(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            package = root / "package"
            for name, data in (("bin/runtime-manifest.json", b"{}"), ("LICENSE", b"licence"), ("docs/README.md", b"readme")):
                path = package / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            shutil.copy(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "notepad.exe",
                        package / "bin" / "nakagawa_player.exe")
            stage_install.write_sha256sums(package)
            moved = root / "elsewhere" / "copy"
            shutil.copytree(package, moved)
            for path in moved.rglob("*"):
                os.utime(path, (4_000_000_000, 4_000_000_000))
            digests = []
            for index, source in enumerate((package, moved)):
                summary = build_installer.build_installer(source, "0.0.2", root / f"out{index}",
                                                          source_date_epoch=1793900000, verify_runtime=lambda _bin: None)
                digests.append(summary["sha256"])
            self.assertEqual(digests[0], digests[1])
            later = build_installer.build_installer(package, "0.0.2", root / "out-later", source_date_epoch=1793900000 + 86400,
                                                    verify_runtime=lambda _bin: None)
            self.assertNotEqual(later["sha256"], digests[0])


if __name__ == "__main__":
    unittest.main()
