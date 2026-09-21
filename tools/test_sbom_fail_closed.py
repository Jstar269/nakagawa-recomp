# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#
# test_sbom_fail_closed.py — hostile-input regression suite for issue #375.
#
# Proves that SBOM dependency-lock parsing fails closed: malformed, truncated,
# unsupported, duplicate, partially parseable, and unreadable lockfiles are
# hard errors, and neither tools/generate_sbom.py nor tools/verify_sbom.py can
# turn "could not inventory dependencies" into an apparently valid (often
# empty) dependency inventory. Valid controls — the repository's real
# lockfiles, a synthetic valid populated lockfile, and valid intentionally
# empty lockfiles — must keep parsing successfully and stay distinguishable
# from every hostile fixture.

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import generate_sbom
import verify_sbom

RELEASE_MANIFEST = ROOT / "assets" / "release_manifest.json"
NPM_LOCK = ROOT / "interface" / "package-lock.json"
PY_LOCK = ROOT / "tools" / "requirements-lock.txt"

VALID_NPM_LOCK = {
    "name": "fixture-app",
    "version": "1.0.0",
    "lockfileVersion": 3,
    "requires": True,
    "packages": {
        "": {"name": "fixture-app", "version": "1.0.0"},
        "node_modules/react": {
            "version": "18.2.0",
            "resolved": "https://registry.npmjs.org/react/-/react-18.2.0.tgz",
            "integrity": "sha512-1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef",
            "license": "MIT",
        },
        "node_modules/app/node_modules/react": {"version": "18.2.0", "license": "MIT"},
        "node_modules/typed-component/@scope/pkg": {"version": "2.0.0"},
    },
}

VALID_PY_LOCK = (
    "# SPDX-License-Identifier: GPL-2.0-or-later\n"
    "# comment lines are ignored\n"
    "\n"
    "compiledb==0.10.7 --hash=sha256:d1d36d4df6c5c723ae4e447b973b306b3a0df47a50dfa7243c2c19e5db4d12bb\n"
    "ruff==0.9.9\n"
)


def write(tmp_path: Path, name: str, content: str | bytes) -> Path:
    path = tmp_path / name
    if isinstance(content, bytes):
        path.write_bytes(content)
    else:
        path.write_text(content, encoding="utf-8", newline="\n")
    return path


def assert_lockfile_parse_error(testcase: unittest.TestCase, path: Path, expected_fragment: str) -> generate_sbom.LockfileParseError:
    """Require a LockfileParseError whose message contains expected_fragment."""
    with testcase.assertRaises(generate_sbom.LockfileParseError) as ctx:
        generate_sbom.parse_npm_lockfile(path)
    testcase.assertIn(expected_fragment, str(ctx.exception))
    return ctx.exception


class TestNpmLockfileHostileInputs(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)

    def test_valid_control_parses_full_inventory(self):
        path = write(self.tmp_path, "valid.json", json.dumps(VALID_NPM_LOCK))
        packages = generate_sbom.parse_npm_lockfile(path)
        self.assertEqual(
            [(p["name"], p["version"]) for p in packages],
            [("react", "18.2.0"), ("react", "18.2.0"), ("typed-component/@scope/pkg", "2.0.0")],
        )
        # Duplicate name+version at distinct install paths must keep distinct
        # SPDX ids (the SBOM element-id contract from test_sbom.py).
        spdx_ids = [p["spdx_id"] for p in packages]
        self.assertEqual(len(spdx_ids), len(set(spdx_ids)))

    def test_root_entry_and_valid_empty_packages_map_parse_to_empty_inventory(self):
        # A lockfile whose packages map is genuinely empty is valid and empty.
        path = write(self.tmp_path, "empty.json", json.dumps(
            {"lockfileVersion": 3, "packages": {}}))
        self.assertEqual(generate_sbom.parse_npm_lockfile(path), [])

        # A lockfile whose only entry is the root "" record is also valid empty.
        path = write(self.tmp_path, "root-only.json", json.dumps(
            {"lockfileVersion": 3, "packages": {"": {"name": "x", "version": "1.0.0"}}}))
        self.assertEqual(generate_sbom.parse_npm_lockfile(path), [])

    def test_malformed_json_fails_closed(self):
        path = write(self.tmp_path, "malformed.json",
                     '{"lockfileVersion": 3, "packages": {node_modules/a": {"version": "1.0"}}}')
        assert_lockfile_parse_error(self, path, "not valid JSON")

    def test_truncated_json_fails_closed(self):
        path = write(self.tmp_path, "truncated.json",
                     json.dumps(VALID_NPM_LOCK)[:60])
        assert_lockfile_parse_error(self, path, "not valid JSON")

    def test_unsupported_lockfile_version_fails_with_actionable_diagnostic(self):
        path = write(self.tmp_path, "future.json", json.dumps(
            {"lockfileVersion": 99, "packages": {"node_modules/a": {"version": "1.0.0"}}}))
        err = assert_lockfile_parse_error(self, path, "unsupported lockfileVersion 99")
        self.assertIn("3", str(err), "diagnostic must name the supported versions")

    def test_missing_or_invalid_lockfile_version_fails_closed(self):
        path = write(self.tmp_path, "noversion.json", json.dumps(
            {"packages": {"node_modules/a": {"version": "1.0.0"}}}))
        assert_lockfile_parse_error(self, path, "missing or invalid lockfileVersion")
        path = write(self.tmp_path, "stringversion.json", json.dumps(
            {"lockfileVersion": "3", "packages": {}}))
        assert_lockfile_parse_error(self, path, "missing or invalid lockfileVersion")

    def test_unsupported_legacy_dependencies_shape_fails_closed(self):
        # v1 "dependencies"-shaped lockfiles are rejected on their declared
        # version: the tool implements only the v2/v3 `packages` representation.
        path = write(self.tmp_path, "legacy.json", json.dumps(
            {"lockfileVersion": 1,
             "dependencies": {"react": {"version": "18.2.0"}}}))
        err = assert_lockfile_parse_error(self, path, "unsupported lockfileVersion 1")
        self.assertIn("legacy `dependencies` map", str(err))

    def test_lockfile_version_1_is_explicitly_rejected_even_with_packages_map(self):
        # The tool implements the v2/v3 `packages` representation only. A v1
        # document is rejected on its declared version, never best-effort
        # parsed just because it happens to carry a `packages` map.
        path = write(self.tmp_path, "v1-with-packages.json", json.dumps(
            {"lockfileVersion": 1,
             "packages": {"node_modules/react": {"version": "18.2.0"}}}))
        err = assert_lockfile_parse_error(self, path, "unsupported lockfileVersion 1")
        self.assertIn("legacy `dependencies` map", str(err))

    def test_packages_map_wrong_type_fails_closed(self):
        path = write(self.tmp_path, "badshape.json", json.dumps(
            {"lockfileVersion": 3, "packages": ["node_modules/a"]}))
        assert_lockfile_parse_error(self, path, "non-object `packages`")

    def test_invalid_package_record_fails_closed(self):
        cases = [
            ("missing version", {"name": "react"}),
            ("empty version", {"name": "react", "version": ""}),
            ("non-string version", {"name": "react", "version": 18}),
            ("invalid name type", {"name": 42, "version": "1.0.0"}),
        ]
        for label, meta in cases:
            with self.subTest(case=label):
                path = write(self.tmp_path, "badrecord.json", json.dumps(
                    {"lockfileVersion": 3, "packages": {"node_modules/react": meta}}))
                assert_lockfile_parse_error(self, path, "node_modules/react")

    def test_non_object_package_record_fails_closed(self):
        path = write(self.tmp_path, "nonobject.json", json.dumps(
            {"lockfileVersion": 3, "packages": {"node_modules/react": "18.2.0"}}))
        assert_lockfile_parse_error(self, path, "must be an object")

    def test_non_string_packages_key_fails_closed(self):
        # JSON object keys are always strings, so this exercises the parser's
        # defensive non-string-key guard directly.
        path = write(self.tmp_path, "badkey.json", "{}")
        hostile = {"lockfileVersion": 3, "packages": {42: {"version": "1.0.0"}}}
        with unittest.mock.patch.object(generate_sbom.json, "loads", return_value=hostile):
            with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
                generate_sbom.parse_npm_lockfile(path)
        self.assertIn("invalid `packages` key", str(ctx.exception))

    def test_top_level_non_object_fails_closed(self):
        path = write(self.tmp_path, "array.json", "[]")
        assert_lockfile_parse_error(self, path, "must contain a JSON object")

    def test_unreadable_path_fails_closed(self):
        missing = self.tmp_path / "does-not-exist.json"
        assert_lockfile_parse_error(self, missing, "not an existing regular file")

        directory = self.tmp_path / "a-directory.json"
        directory.mkdir()
        assert_lockfile_parse_error(self, directory, "not an existing regular file")

    def test_unreadable_file_oserror_fails_closed(self):
        path = write(self.tmp_path, "locked.json", json.dumps(VALID_NPM_LOCK))
        with unittest.mock.patch.object(Path, "read_bytes", side_effect=PermissionError(13, "denied")):
            with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
                generate_sbom.parse_npm_lockfile(path)
        self.assertIn("cannot read npm lockfile", str(ctx.exception))

    def test_invalid_utf8_fails_closed(self):
        path = write(self.tmp_path, "utf8.json",
                     b'{"lockfileVersion": 3, "packages": {}, "\xff\xfe": 1}')
        assert_lockfile_parse_error(self, path, "not valid UTF-8")

    def test_partially_parseable_file_is_all_or_nothing(self):
        # Every record before the poisoned one must NOT survive as a partial
        # inventory: the poison record aborts the whole parse.
        poisoned = json.loads(json.dumps(VALID_NPM_LOCK))
        poisoned["packages"]["node_modules/zz-last"] = {"name": "zz-last"}  # no version
        path = write(self.tmp_path, "partial.json", json.dumps(poisoned))
        assert_lockfile_parse_error(self, path, "node_modules/zz-last")


class TestPythonLockfileHostileInputs(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)

    def test_valid_control_parses_all_pins_and_hashes(self):
        path = write(self.tmp_path, "valid.txt", VALID_PY_LOCK)
        packages = generate_sbom.parse_python_lockfile(path)
        self.assertEqual(
            [(p["name"], p["version"], p["sha256"]) for p in packages],
            [("compiledb", "0.10.7", "d1d36d4df6c5c723ae4e447b973b306b3a0df47a50dfa7243c2c19e5db4d12bb"),
             ("ruff", "0.9.9", "")],
        )

    def test_valid_intentionally_empty_lockfile_parses_to_empty_inventory(self):
        for content in ("", "\n", "# only comments\n", "# c1\n\n# c2\n", "   \n\t\n"):
            with self.subTest(content=content):
                path = write(self.tmp_path, "empty.txt", content)
                self.assertEqual(generate_sbom.parse_python_lockfile(path), [])

    def test_malformed_requirement_line_fails_closed(self):
        bad_lines = [
            "this is not a requirement",
            "compiledb 0.10.7",                      # missing ==
            "compiledb>=0.10.7",                      # range specifiers are not pins
            "compiledb==0.10.*",                      # wildcard version
            "compiledb==0.10.7 extra-junk",           # unsupported trailing options
            "compiledb==0.10.7 --hash=md5:aaa",       # unsupported hash algorithm
            "compiledb==0.10.7 --hash=sha256:abc",    # truncated digest
            "-e git+https://example.invalid/repo#egg=x",  # editable/VCS requirement
        ]
        for line in bad_lines:
            with self.subTest(line=line):
                path = write(self.tmp_path, "bad.txt", line + "\n")
                with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
                    generate_sbom.parse_python_lockfile(path)
                self.assertIn("declared pinned format", str(ctx.exception))
                self.assertIn("line 1:", str(ctx.exception))

    def test_malformed_hash_continuation_line_fails_closed(self):
        # A pip-style multi-line hash block is NOT the repository's declared
        # single-line format; the continuation line is a hard error, not a
        # silently skipped requirement.
        path = write(self.tmp_path, "cont.txt",
                     "pkg==1.0 \\\n    --hash=sha256:" + "a" * 64 + "\n")
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            generate_sbom.parse_python_lockfile(path)
        self.assertIn("line 1:", str(ctx.exception))

    def test_duplicate_requirement_pin_fails_closed(self):
        path = write(self.tmp_path, "dup.txt", "ruff==0.9.9\nruff==0.9.9\n")
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            generate_sbom.parse_python_lockfile(path)
        self.assertIn("duplicate requirement pin ruff==0.9.9", str(ctx.exception))
        self.assertIn("line 2", str(ctx.exception))

    def test_unreadable_path_and_oserror_fail_closed(self):
        missing = self.tmp_path / "missing-requirements.txt"
        assert_lockfile_parse_error(self, missing, "not an existing regular file")

        path = write(self.tmp_path, "locked.txt", VALID_PY_LOCK)
        with unittest.mock.patch.object(Path, "read_bytes", side_effect=OSError("I/O error")):
            with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
                generate_sbom.parse_python_lockfile(path)
        self.assertIn("cannot read Python lockfile", str(ctx.exception))

    def test_invalid_utf8_fails_closed(self):
        path = write(self.tmp_path, "utf8.txt", b"ruff==0.9.9\n\xff\xfe garbage\n")
        assert_lockfile_parse_error(self, path, "not valid UTF-8")

    def test_partially_parseable_file_is_all_or_nothing(self):
        # A valid first pin followed by a poisoned line must abort, never
        # return the successfully parsed prefix.
        path = write(self.tmp_path, "partial.txt",
                     "compiledb==0.10.7\nthis line is poisoned\n")
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            generate_sbom.parse_python_lockfile(path)
        self.assertIn("this line is poisoned", str(ctx.exception))


class TestRepositoryLockfilesStayValid(unittest.TestCase):
    """Guards: the strict parser must keep accepting the repository's real locks."""

    def test_repository_npm_lockfile_parses(self):
        packages = generate_sbom.parse_npm_lockfile(NPM_LOCK)
        self.assertGreater(len(packages), 0)
        self.assertTrue(all(p["name"] and p["version"] for p in packages))

    def test_repository_python_lockfile_parses(self):
        packages = generate_sbom.parse_python_lockfile(PY_LOCK)
        self.assertGreater(len(packages), 0)
        self.assertEqual([p["name"] for p in packages], ["compiledb", "ruff"])


class TestGenerationAbortsOnParseFailure(unittest.TestCase):
    """generate_sbom.main must exit nonzero and write nothing when a declared
    lockfile cannot be parsed completely (issue #375)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)

    def _run_main(self, npm_lock: Path, py_lock: Path) -> tuple[int, dict[str, bool]]:
        outs = {
            "spdx": self.tmp_path / "out" / "spdx23.json",
            "spdx3": self.tmp_path / "out" / "spdx301.json",
            "cdx": self.tmp_path / "out" / "cyclonedx.json",
        }
        code = generate_sbom.main([
            "--manifest", str(RELEASE_MANIFEST),
            "--npm-lock", str(npm_lock),
            "--py-lock", str(py_lock),
            "--spdx-out", str(outs["spdx"]),
            "--spdx3-out", str(outs["spdx3"]),
            "--cyclonedx-out", str(outs["cdx"]),
        ])
        return code, {k: p.exists() for k, p in outs.items()}

    def test_malformed_npm_lock_aborts_with_no_outputs(self):
        bad = write(self.tmp_path, "bad.json", '{"lockfileVersion": 3, "packages": {')
        code, exists = self._run_main(bad, PY_LOCK)
        self.assertEqual(code, 1)
        self.assertFalse(any(exists.values()),
                         "no SBOM artifact may be written from an unparseable lockfile")

    def test_missing_npm_lock_aborts(self):
        code, exists = self._run_main(self.tmp_path / "missing.json", PY_LOCK)
        self.assertEqual(code, 1)
        self.assertFalse(any(exists.values()))

    def test_malformed_python_lock_aborts_with_no_outputs(self):
        bad = write(self.tmp_path, "bad.txt", "ruff==0.9.9\npoisoned line\n")
        code, exists = self._run_main(NPM_LOCK, bad)
        self.assertEqual(code, 1)
        self.assertFalse(any(exists.values()))

    def test_valid_locks_generate_all_outputs(self):
        # Happy path uses the repository's real locks: generation requires the
        # canonical repo-relative lockfile identities for release evidence.
        code, exists = self._run_main(NPM_LOCK, PY_LOCK)
        self.assertEqual(code, 0)
        self.assertTrue(all(exists.values()))


class TestVerifySbomFailsClosed(unittest.TestCase):
    """verify_sbom must parse/validate the declared lockfiles; verification
    must never pass on a hostile lockfile, and a parser cannot return an
    empty/partial inventory and still produce a passing verification."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)
        # Always reset the module-level "last validated" state to keep tests
        # independent of call order.
        self.addCleanup(setattr, verify_sbom.verify_release_locks,
                        "last_validated_npm_lock", None)
        self.addCleanup(setattr, verify_sbom.verify_release_locks,
                        "last_validated_py_lock", None)

    def _manifest_declaring(self, npm_rel: str, py_rel: str) -> Path:
        """Write a sandboxed manifest at <tmp>/assets/release_manifest.json.

        verify_release_locks resolves declared lockfile paths relative to the
        manifest's parent.parent (the sandbox root), mirroring the canonical
        <root>/assets/release_manifest.json layout.
        """
        manifest = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        manifest["lockfiles"] = {"npm": npm_rel, "python": py_rel}
        assets_dir = self.tmp_path / "assets"
        assets_dir.mkdir(exist_ok=True)
        path = assets_dir / "release_manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8", newline="\n")
        return path

    def test_verify_release_locks_parses_declared_lockfiles(self):
        errors = verify_sbom.verify_release_locks(RELEASE_MANIFEST)
        self.assertEqual(errors, [], f"repository locks must verify: {errors}")
        self.assertEqual(verify_sbom.verify_release_locks.last_validated_npm_lock, NPM_LOCK)
        self.assertEqual(verify_sbom.verify_release_locks.last_validated_py_lock, PY_LOCK)

    def test_malformed_npm_lockfile_fails_verification(self):
        # The malformed npm fixture and a valid Python lock are laid out in the
        # sandbox so the manifest-relative paths resolve to exactly those bytes.
        bad_rel = "fixtures_hostile/bad-npm.json"
        repo_fixtures = self.tmp_path / "fixtures_hostile"
        repo_fixtures.mkdir()
        write(repo_fixtures, "bad-npm.json", '{"lockfileVersion": 3, "packages": {')
        py_dir = self.tmp_path / "pylock"
        py_dir.mkdir()
        shutil.copy(PY_LOCK, py_dir / "requirements-lock.txt")
        manifest = self._manifest_declaring(bad_rel, "pylock/requirements-lock.txt")
        errors = verify_sbom.verify_release_locks(manifest)
        self.assertTrue(any("not valid JSON" in e for e in errors), str(errors))
        self.assertIsNone(verify_sbom.verify_release_locks.last_validated_npm_lock)

    def test_malformed_python_lockfile_fails_verification(self):
        # Hostile Python lock and the real npm lock laid out in the sandbox.
        bad_rel = "fixtures_hostile/bad-py.txt"
        repo_fixtures = self.tmp_path / "fixtures_hostile"
        repo_fixtures.mkdir()
        write(repo_fixtures, "bad-py.txt", "ruff==0.9.9\nnot a requirement\n")
        npm_dir = self.tmp_path / "npmlock"
        npm_dir.mkdir()
        shutil.copy(NPM_LOCK, npm_dir / "package-lock.json")
        manifest = self._manifest_declaring("npmlock/package-lock.json", bad_rel)
        errors = verify_sbom.verify_release_locks(manifest)
        self.assertTrue(any("declared pinned format" in e for e in errors), str(errors))
        self.assertIsNone(verify_sbom.verify_release_locks.last_validated_py_lock)

    def test_unreadable_declared_lockfile_fails_verification(self):
        manifest = self._manifest_declaring("fixtures_hostile/absent.json",
                                            "tools/requirements-lock.txt")
        errors = verify_sbom.verify_release_locks(manifest)
        self.assertTrue(any("npm lockfile missing or unreadable" in e for e in errors), str(errors))

    def test_missing_lockfiles_declaration_fails_verification(self):
        manifest = self._manifest_declaring("", "tools/requirements-lock.txt")
        errors = verify_sbom.verify_release_locks(manifest)
        self.assertTrue(any("npm lockfile path missing" in e for e in errors), str(errors))

    def test_verify_sbom_matches_reports_parser_failure_instead_of_passing(self):
        # The core #375 regression: parse_npm_lockfile raising must surface as
        # a verification error, never as an empty inventory that passes.
        bad_rel = "fixtures_hostile/bad-npm.json"
        fixtures = self.tmp_path / "fixtures_hostile"
        fixtures.mkdir()
        write(fixtures, "bad-npm.json", '{"lockfileVersion": 3, "packages": {')
        manifest = self._manifest_declaring(bad_rel, "tools/requirements-lock.txt")

        spdx_path = self.tmp_path / "sbom.json"
        spdx_path.write_text(json.dumps({"packages": []}), encoding="utf-8")

        errors = verify_sbom.verify_sbom_matches(
            spdx_path, manifest,
            self.tmp_path / "fixtures_hostile" / "bad-npm.json",
            PY_LOCK,
        )
        self.assertTrue(any("not valid JSON" in e for e in errors), str(errors))

    def test_removing_a_real_dependency_from_parser_output_is_detected(self):
        # Mutation: a real dependency (compiledb) is dropped from the parsed
        # inventory while it remains in the lockfile; the SPDX doc generated
        # from the subverted inventory no longer lists it. Verification runs
        # with the REAL parser and must detect the missing dependency.
        with unittest.mock.patch.object(
            generate_sbom, "parse_python_lockfile",
            return_value=[p for p in generate_sbom.parse_python_lockfile(PY_LOCK)
                          if p["name"] != "compiledb"],
        ):
            manifest_data = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
            npm_pkgs = generate_sbom.parse_npm_lockfile(NPM_LOCK)
            py_pkgs = generate_sbom.parse_python_lockfile(PY_LOCK)
            spdx = generate_sbom.generate_spdx23(manifest_data, npm_pkgs, py_pkgs)
            spdx_path = self.tmp_path / "mutated-sbom.json"
            spdx_path.write_text(json.dumps(spdx), encoding="utf-8")

        errors = verify_sbom.verify_sbom_matches(spdx_path, RELEASE_MANIFEST, NPM_LOCK, PY_LOCK)
        # The subverted SBOM (a) lacks the standards-conformant lock binding
        # (`files` entries), (b) lacks the DEPENDENCY_MANIFEST_OF lock
        # relationships, and (c) is missing the exact compiledb identity; all
        # must be reported.
        self.assertTrue(
            any("no `files` entries" in e for e in errors),
            str(errors),
        )
        self.assertTrue(
            any("missing DEPENDENCY_MANIFEST_OF relationship" in e for e in errors),
            str(errors),
        )
        self.assertTrue(
            any("pkg:pypi/compiledb@0.10.7" in e and "missing or under-represented" in e
                for e in errors),
            str(errors),
        )

    def test_verify_main_fails_on_malformed_declared_lockfile(self):
        fixtures = self.tmp_path / "fixtures_hostile"
        fixtures.mkdir()
        write(fixtures, "bad.json", '{"lockfileVersion": 3, "packages": {')
        manifest = self._manifest_declaring("fixtures_hostile/bad.json",
                                            "tools/requirements-lock.txt")
        code = verify_sbom.main(["--manifest", str(manifest)])
        self.assertEqual(code, 1)

    def test_verify_main_succeeds_on_repository_locks(self):
        code = verify_sbom.main(["--manifest", str(RELEASE_MANIFEST)])
        self.assertEqual(code, 0)

    def test_verify_main_rejects_spdx_outside_declared_lockfiles(self):
        # --spdx with --npm-lock pointing somewhere other than the manifest's
        # declared lockfile must fail closed (SBOM/lock binding).
        spdx_path = self.tmp_path / "sbom.json"
        spdx_path.write_text(json.dumps({"packages": []}), encoding="utf-8")
        code = verify_sbom.main([
            "--manifest", str(RELEASE_MANIFEST),
            "--npm-lock", str(self.tmp_path / "other-package-lock.json"),
            "--py-lock", str(PY_LOCK),
            "--spdx", str(spdx_path),
        ])
        self.assertEqual(code, 1)


class TestEndToEndCliFailClosed(unittest.TestCase):
    """The real CLI binaries must fail closed on hostile locks (exit != 0),
    and succeed on the repository's real locks."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)

    def _cli(self, *argv: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(ROOT / "tools" / "generate_sbom.py"), *argv],
            capture_output=True, text=True, cwd=ROOT,
        )

    def test_cli_generation_fails_on_truncated_npm_lockfile(self):
        truncated = write(self.tmp_path, "truncated.json",
                          (NPM_LOCK).read_text(encoding="utf-8")[:200])
        out = self.tmp_path / "spdx.json"
        proc = self._cli("--npm-lock", str(truncated), "--py-lock", str(PY_LOCK),
                         "--spdx-out", str(out))
        self.assertNotEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("not valid JSON", proc.stderr)
        self.assertFalse(out.exists())

    def test_cli_generation_fails_on_unreadable_python_lockfile(self):
        out = self.tmp_path / "spdx.json"
        proc = self._cli("--npm-lock", str(NPM_LOCK),
                         "--py-lock", str(self.tmp_path / "absent.txt"),
                         "--spdx-out", str(out))
        self.assertNotEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(out.exists())

    def test_cli_generation_succeeds_on_real_lockfiles(self):
        out = self.tmp_path / "spdx.json"
        proc = self._cli("--npm-lock", str(NPM_LOCK), "--py-lock", str(PY_LOCK),
                         "--spdx-out", str(out))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(out.exists())


if __name__ == "__main__":
    unittest.main()
