# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Fail-closed coverage for the retained Python release dependency lock."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import generate_sbom
import verify_sbom

RELEASE_MANIFEST = ROOT / "assets" / "release_manifest.json"
PY_LOCK = ROOT / "tools" / "requirements-lock.txt"
VALID_PY_LOCK = PY_LOCK.read_text(encoding="utf-8")


def write(directory: Path, name: str, content: str | bytes) -> Path:
    path = directory / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(content, bytes):
        path.write_bytes(content)
    else:
        path.write_text(content, encoding="utf-8", newline="\n")
    return path


def assert_lockfile_parse_error(testcase, path: Path, expected: str) -> None:
    with testcase.assertRaises(generate_sbom.LockfileParseError) as ctx:
        generate_sbom.parse_python_lockfile(path)
    testcase.assertIn(expected, str(ctx.exception))


class TestPythonLockfileHostileInputs(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)

    def test_valid_control_parses_all_pins_and_hashes(self):
        path = write(self.tmp_path, "valid.txt", VALID_PY_LOCK)
        packages = generate_sbom.parse_python_lockfile(path)
        self.assertEqual(
            [(p["name"], p["version"], len(p["declared_sha256"])) for p in packages],
            [("compiledb", "0.10.7", 2), ("ruff", "0.16.8", 18),
             ("click", "8.5.0", 2), ("bashlex", "0.18", 2)],
        )

    def test_valid_intentionally_empty_lockfile_parses_to_empty_inventory(self):
        for content in ("", "\n", "# only comments\n", "# c1\n\n# c2\n", "   \n\t\n"):
            with self.subTest(content=content):
                path = write(self.tmp_path, "empty.txt", content)
                self.assertEqual(generate_sbom.parse_python_lockfile(path), [])

    def test_malformed_requirement_lines_fail_closed(self):
        bad_lines = [
            ("this is not a requirement", "exact name==version pin"),
            ("compiledb 0.10.7", "exact name==version pin"),
            ("compiledb>=0.10.7", "exact name==version pin"),
            ("compiledb==0.10.*", "exact name==version pin"),
            ("compiledb==0.10.7 extra-junk", "unsupported token"),
            ("compiledb==0.10.7 --hash=md5:aaa", "malformed SHA-256 hash"),
            ("compiledb==0.10.7 --hash=sha256:abc", "malformed SHA-256 hash"),
            ("-e git+https://example.invalid/repo#egg=x", "exact name==version pin"),
        ]
        for line, expected in bad_lines:
            with self.subTest(line=line):
                path = write(self.tmp_path, "bad.txt", line + "\n")
                with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
                    generate_sbom.parse_python_lockfile(path)
                self.assertIn(expected, str(ctx.exception))
                self.assertIn("line 1:", str(ctx.exception))

    def test_malformed_hash_continuation_line_fails_closed(self):
        content = "compiledb==0.10.7 " + "\\" + "\n    --hash=sha256:abc\n"
        path = write(self.tmp_path, "cont.txt", content)
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            generate_sbom.parse_python_lockfile(path)
        self.assertIn("malformed SHA-256 hash", str(ctx.exception))
        self.assertIn("line 1:", str(ctx.exception))

    def test_duplicate_requirement_pin_fails_closed(self):
        entry = next(block + "\n" for block in VALID_PY_LOCK.split("\n\n") if block.startswith("compiledb=="))
        path = write(self.tmp_path, "duplicate.txt", entry * 2)
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            generate_sbom.parse_python_lockfile(path)
        self.assertIn("duplicate requirement pin compiledb==0.10.7", str(ctx.exception))

    def test_unreadable_path_and_oserror_fail_closed(self):
        assert_lockfile_parse_error(self, self.tmp_path / "missing.txt", "not an existing regular file")
        path = write(self.tmp_path, "locked.txt", VALID_PY_LOCK)
        with mock.patch.object(Path, "read_bytes", side_effect=OSError("I/O error")):
            with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
                generate_sbom.parse_python_lockfile(path)
        self.assertIn("cannot read Python lockfile", str(ctx.exception))

    def test_invalid_utf8_fails_closed(self):
        assert_lockfile_parse_error(self, write(self.tmp_path, "utf8.txt", b"ruff==0.16.0\n\xff\xfe garbage\n"), "not valid UTF-8")

    def test_partially_parseable_file_is_all_or_nothing(self):
        entry = next(block + "\n" for block in VALID_PY_LOCK.split("\n\n") if block.startswith("compiledb=="))
        path = write(self.tmp_path, "partial.txt", entry + "this line is poisoned\n")
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            generate_sbom.parse_python_lockfile(path)
        self.assertIn("this line is poisoned", str(ctx.exception))


class TestRepositoryLockfileStaysValid(unittest.TestCase):
    def test_repository_python_lockfile_parses(self):
        packages = generate_sbom.parse_python_lockfile(PY_LOCK)
        self.assertEqual(len(packages), 4)
        self.assertTrue(all(package["name"] and package["version"] for package in packages))


class ReleaseManifestFixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.manifest_path = self.root / "assets" / "release_manifest.json"
        self.manifest_path.parent.mkdir()

    def write_manifest(self, rel_path: str, lock_content: str | None = VALID_PY_LOCK) -> tuple[Path, Path]:
        manifest = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        manifest["lockfiles"] = {"python": rel_path}
        self.manifest_path.write_text(json.dumps(manifest), encoding="utf-8", newline="\n")
        lock_path = self.root / rel_path
        if lock_content is not None:
            write(lock_path.parent, lock_path.name, lock_content)
        return self.manifest_path, lock_path


class TestGenerationAbortsOnParseFailure(ReleaseManifestFixture):
    def _run_main(self, manifest: Path, lock: Path) -> tuple[int, list[Path]]:
        outputs = [self.root / "out" / "spdx.json", self.root / "out" / "spdx3.json",
                   self.root / "out" / "cyclonedx.json"]
        code = generate_sbom.main([
            "--manifest", str(manifest), "--py-lock", str(lock),
            "--spdx-out", str(outputs[0]), "--spdx3-out", str(outputs[1]),
            "--cyclonedx-out", str(outputs[2]),
        ])
        return code, outputs

    def test_malformed_python_lock_aborts_without_outputs(self):
        manifest, lock = self.write_manifest("tools/requirements-lock.txt", "not a requirement\n")
        code, outputs = self._run_main(manifest, lock)
        self.assertEqual(code, 1)
        self.assertFalse(any(path.exists() for path in outputs))

    def test_missing_python_lock_aborts_without_outputs(self):
        manifest, lock = self.write_manifest("tools/requirements-lock.txt", None)
        code, outputs = self._run_main(manifest, lock)
        self.assertEqual(code, 1)
        self.assertFalse(any(path.exists() for path in outputs))

    def test_valid_python_lock_generates_all_outputs(self):
        manifest, lock = self.write_manifest("tools/requirements-lock.txt")
        code, outputs = self._run_main(manifest, lock)
        self.assertEqual(code, 0)
        self.assertTrue(all(path.is_file() for path in outputs))


class TestVerifySbomFailsClosed(ReleaseManifestFixture):
    def test_verify_release_locks_parses_declared_lock(self):
        errors = verify_sbom.verify_release_locks(RELEASE_MANIFEST)
        self.assertEqual(errors, [], str(errors))
        self.assertEqual(verify_sbom.verify_release_locks.last_validated_py_lock, PY_LOCK.resolve())

    def test_malformed_python_lock_fails_verification(self):
        manifest, _lock = self.write_manifest("fixtures/bad-requirements.txt", "not a requirement\n")
        errors = verify_sbom.verify_release_locks(manifest)
        self.assertTrue(any("exact name==version pin" in error for error in errors), str(errors))
        self.assertIsNone(verify_sbom.verify_release_locks.last_validated_py_lock)

    def test_missing_python_lock_fails_verification(self):
        manifest, _lock = self.write_manifest("fixtures/absent-requirements.txt", None)
        errors = verify_sbom.verify_release_locks(manifest)
        self.assertTrue(any("declared Python lockfile" in error for error in errors), str(errors))

    def test_extra_lockfile_declaration_fails_closed(self):
        manifest, _lock = self.write_manifest("tools/requirements-lock.txt")
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["lockfiles"]["npm"] = "interface/package-lock.json"
        manifest.write_text(json.dumps(data), encoding="utf-8")
        errors = verify_sbom.verify_release_locks(manifest)
        self.assertTrue(any("must declare only Python" in error for error in errors), str(errors))

    def test_verify_sbom_matches_reports_python_parser_failure(self):
        manifest, lock = self.write_manifest("fixtures/bad-requirements.txt", "not a requirement\n")
        spdx = write(self.root, "sbom.json", json.dumps({"packages": []}))
        errors, _digests = verify_sbom.verify_sbom_matches(spdx, manifest, lock)
        self.assertTrue(any("exact name==version pin" in error for error in errors), str(errors))

    def test_missing_locked_dependency_is_detected(self):
        parsed = generate_sbom.parse_lockfiles(PY_LOCK, repo_root=ROOT)
        data = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        packages = [package for package in parsed["py_packages"] if package["name"] != "compiledb"]
        spdx = generate_sbom.generate_spdx23(
            data, packages, lock_files=parsed["lock_files"],
            lock_relationships=parsed["lock_relationships"],
        )
        spdx_path = write(self.root, "sbom.json", json.dumps(spdx))
        errors, _digests = verify_sbom.verify_sbom_matches(spdx_path, RELEASE_MANIFEST, PY_LOCK)
        self.assertTrue(any("pkg:pypi/compiledb@0.10.7" in error
                            and "missing or under-represented" in error for error in errors), str(errors))

    def test_verify_main_fails_on_malformed_declared_lock(self):
        manifest, _lock = self.write_manifest("fixtures/bad-requirements.txt", "not a requirement\n")
        self.assertEqual(verify_sbom.main(["--manifest", str(manifest)]), 1)

    def test_verify_main_succeeds_on_repository_lock(self):
        self.assertEqual(verify_sbom.main(["--manifest", str(RELEASE_MANIFEST)]), 0)

    def test_verify_main_rejects_cli_lock_outside_manifest_declaration(self):
        spdx = write(self.root, "sbom.json", json.dumps({"packages": []}))
        other_lock = write(self.root, "other.txt", VALID_PY_LOCK)
        code = verify_sbom.main([
            "--manifest", str(RELEASE_MANIFEST), "--py-lock", str(other_lock),
            "--spdx", str(spdx),
        ])
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
