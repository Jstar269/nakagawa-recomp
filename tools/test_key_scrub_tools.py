# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Hermetic tests for the key-history-scrub helpers (no git history / network).

History-search tests build a throwaway temporary Git repository per test, inject
a synthetic 16-byte value as the key file, and assert the three CLI verdict
classes (#377) with precedence exposure > unverifiable > clean: exit 0 only for
completed searches with no match, exit 3 whenever a constant is positively
reachable (even alongside later search failures), and exit 2 when verification
could not complete and no definitive exposure was found.
"""

import contextlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_key_scrub_spec as gen  # noqa: E402
import verify_key_scrub as vks  # noqa: E402

# A synthetic 16-byte value with a sub-16 byte (0x01) and a >=16 byte, to exercise
# the bare-int Python-list spelling as well as the padded/hex forms.
SAMPLE = bytes([0x01, 0x02, 0xab, 0xCD, 0x10, 0x0f, 0x7e, 0x80,
                0x00, 0xff, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc])


def _run_git(repo: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(repo), *args], capture_output=True, text=True, check=False, timeout=60,
    )


def _init_repo(*, bare: bool = False, empty: bool = False, initial_branch: str = "main") -> Path:
    """Create a hermetic temp repository with a deterministic local identity."""

    def seeded_repo(path: Path) -> None:
        init = subprocess.run(
            ["git", "init", "--initial-branch", initial_branch, str(path)],
            capture_output=True, text=True, check=False, timeout=60,
        )
        assert init.returncode == 0, init.stderr
        _run_git(path, "config", "user.name", "Key Scrub Tests")
        _run_git(path, "config", "user.email", "key-scrub-tests@example.invalid")
        _run_git(path, "config", "commit.gpgsign", "false")
        _run_git(path, "config", "tag.gpgsign", "false")
        if not empty:
            (path / "seed.txt").write_text("seed\n", encoding="utf-8")
            assert _run_git(path, "add", "-A").returncode == 0
            assert _run_git(path, "commit", "-m", "seed").returncode == 0

    root = Path(tempfile.mkdtemp(prefix="key-scrub-test-"))
    if not bare:
        seeded_repo(root)
        return root
    # A bare repository cannot receive a working-tree commit: seed a normal
    # repository first, then clone it bare (leaves refs, no work tree).
    seed = Path(tempfile.mkdtemp(prefix="key-scrub-seed-"))
    try:
        seeded_repo(seed)
        clone = subprocess.run(
            ["git", "clone", "--bare", str(seed), str(root)],
            capture_output=True, text=True, check=False, timeout=60,
        )
        assert clone.returncode == 0, clone.stderr
    finally:
        shutil.rmtree(seed, ignore_errors=True)
    return root


@contextlib.contextmanager
def _chdir(path: Path):
    previous = os.getcwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(previous)


def _write_keys(directory: Path, names_and_values: dict[str, bytes]) -> Path:
    keys = directory / "keys.txt"
    keys.write_text(
        "".join(f"{name} = {raw.hex()}\n" for name, raw in names_and_values.items()),
        encoding="utf-8",
    )
    return keys


def _commit_secret(repo: Path, needle: str) -> None:
    """Make one textual encoding of the synthetic value reachable in `repo`."""
    target = repo / "payload.txt"
    target.write_text(f"leaked = {needle}\n", encoding="utf-8")
    assert _run_git(repo, "add", "-A").returncode == 0
    assert _run_git(repo, "commit", "-m", "commit payload").returncode == 0


class TestEncodings(unittest.TestCase):
    def test_covers_hex_and_byte_array_forms(self):
        forms = set(vks.encodings(SAMPLE))
        self.assertIn(SAMPLE.hex(), forms)                     # lowercase hex
        self.assertIn(SAMPLE.hex().upper(), forms)             # uppercase hex
        self.assertIn(",".join(f"0x{b:02x}" for b in SAMPLE), forms)   # C no-space
        self.assertIn(", ".join(f"0x{b:02x}" for b in SAMPLE), forms)  # spaced
        # The irregular Python bytes([...]) spelling: bare decimal for values < 16.
        bare = ", ".join(str(b) if b < 16 else f"0x{b:02x}" for b in SAMPLE)
        self.assertIn(bare, forms)
        self.assertIn("1", bare.split(", "))   # 0x01 rendered as bare "1"

    def test_key_values_parses_name_equals_hex(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "keys.txt"
            p.write_text(
                "# comment\nfoo = " + SAMPLE.hex() + "\nbad = notenoughhex\n",
                encoding="utf-8",
            )
            values = vks.key_values(str(p))
        self.assertEqual(values, {"foo": SAMPLE})


class TestSearchVerdicts(unittest.TestCase):
    """Unit-level FOUND / NOT_FOUND / ERROR semantics."""

    def setUp(self):
        self.repo = _init_repo()

    def tearDown(self):
        # Keep the throwaway repo small; ignore errors if Windows still holds
        # a file handle briefly while the rmtree below runs.
        subprocess.run(["git", "-C", str(self.repo), "gc", "--prune=now"],
                       capture_output=True, timeout=60, check=False)
        shutil.rmtree(self.repo, ignore_errors=True)

    def test_not_found_in_clean_repo(self):
        with _chdir(self.repo):
            verdict, error = vks.search_history(SAMPLE.hex(), [SAMPLE.hex()])
        self.assertIs(verdict, vks.Verdict.NOT_FOUND)
        self.assertIsNone(error)

    def test_found_in_repo_with_committed_value(self):
        _commit_secret(self.repo, SAMPLE.hex())
        with _chdir(self.repo):
            verdict, error = vks.search_history(SAMPLE.hex(), [SAMPLE.hex()])
        self.assertIs(verdict, vks.Verdict.FOUND)
        self.assertIsNone(error)

    def test_error_when_git_log_fails(self):
        needle = SAMPLE.hex()
        with mock.patch.object(vks.subprocess, "run", side_effect=subprocess.CalledProcessError(128, "git")):
            verdict, error = vks.search_history(needle, [needle])
        self.assertIs(verdict, vks.Verdict.ERROR)
        self.assertIsNotNone(error)
        self.assertIn("git", error.detail.lower())

    def test_error_when_git_missing(self):
        needle = SAMPLE.hex()
        with mock.patch.object(vks.subprocess, "run", side_effect=OSError("git not found")):
            verdict, error = vks.search_history(needle, [needle])
        self.assertIs(verdict, vks.Verdict.ERROR)
        self.assertIsNotNone(error)
        self.assertIn("git", error.detail.lower())

    def test_error_on_timeout(self):
        needle = SAMPLE.hex()
        with mock.patch.object(vks.subprocess, "run", side_effect=subprocess.TimeoutExpired("git", 5)):
            verdict, error = vks.search_history(needle, [needle])
        self.assertIs(verdict, vks.Verdict.ERROR)
        self.assertIsNotNone(error)
        self.assertIn("timed out", error.detail)

    def test_error_on_nonzero_exit_reports_redacted_stderr(self):
        needle = SAMPLE.hex()

        def failing_run(*_args, **_kwargs):
            return subprocess.CompletedProcess(args=[], returncode=128, stdout="", stderr=f"fatal: bad object {needle}")

        with mock.patch.object(vks.subprocess, "run", side_effect=failing_run):
            verdict, error = vks.search_history(needle, [needle])
        self.assertIs(verdict, vks.Verdict.ERROR)
        self.assertEqual(error.exit_code, 128)
        self.assertNotIn(needle, error.detail)
        self.assertIn("[redacted]", error.detail)


class TestRepositoryPreconditions(unittest.TestCase):
    def test_valid_repo_with_history_passes(self):
        repo = _init_repo()
        with _chdir(repo):
            self.assertIsNone(vks.check_repository())

    def test_bare_repository_passes(self):
        repo = _init_repo(bare=True)
        with _chdir(repo):
            self.assertIsNone(vks.check_repository())

    def test_non_git_directory_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            plain = Path(d) / "not-a-repo"
            plain.mkdir()
            with _chdir(plain):
                problem = vks.check_repository()
        self.assertIsNotNone(problem)

    def test_empty_repository_without_refs_is_rejected(self):
        repo = _init_repo(empty=True)
        with _chdir(repo):
            problem = vks.check_repository()
        self.assertIsNotNone(problem)
        self.assertIn("no refs", problem)

    def test_shallow_clone_is_rejected(self):
        full = _init_repo()
        shallow = Path(tempfile.mkdtemp(prefix="key-scrub-shallow-"))
        clone = _run_git(full.parent, "clone", "--no-local", "--depth", "1", f"file://{full.as_posix()}", shallow.name)
        self.assertEqual(clone.returncode, 0, clone.stderr)
        try:
            with _chdir(shallow):
                problem = vks.check_repository()
            self.assertIsNotNone(problem)
            self.assertIn("shallow", problem)
        finally:
            shutil.rmtree(shallow, ignore_errors=True)
            shutil.rmtree(full, ignore_errors=True)


class TestCliVerdicts(unittest.TestCase):
    """End-to-end verdict classes through main()."""

    def _main_in(self, repo: Path, keys: Path) -> int:
        with _chdir(repo):
            return vks.main(["--keys", str(keys)])

    @contextlib.contextmanager
    def _capture(self):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "stdout", out), mock.patch.object(sys, "stderr", err):
            yield out, err

    def test_exit_zero_clean_history(self):
        repo = _init_repo()
        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE})
            with self._capture() as (out, _err):
                rc = self._main_in(repo, keys)
        self.assertEqual(rc, 0)
        self.assertIn("clean", out.getvalue())
        self.assertIn("k: clean", out.getvalue())

    def test_exit_three_when_constant_reachable(self):
        repo = _init_repo()
        _commit_secret(repo, SAMPLE.hex())
        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE})
            with self._capture() as (out, _err):
                rc = self._main_in(repo, keys)
        self.assertEqual(rc, 3)
        self.assertIn("k: REACHABLE", out.getvalue())

    def test_every_encoding_is_actually_searched(self):
        repo = _init_repo()
        # Commit a non-hex spelling (C byte-array form); only a full encoding
        # sweep can classify the constant as reachable.
        _commit_secret(repo, ", ".join(f"0x{b:02X}" for b in SAMPLE))
        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE})
            with self._capture() as (_out, _err):
                rc = self._main_in(repo, keys)
        self.assertEqual(rc, 3)

    def test_exit_two_outside_git_repository(self):
        with tempfile.TemporaryDirectory() as d:
            outside = Path(d) / "plain"
            outside.mkdir()
            keys = _write_keys(outside, {"k": SAMPLE})
            with self._capture() as (_out, err):
                with _chdir(outside):
                    rc = vks.main(["--keys", str(keys)])
        self.assertEqual(rc, 2)
        self.assertIn("cannot verify history", err.getvalue())

    def test_exit_two_on_empty_repository(self):
        repo = _init_repo(empty=True)
        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE})
            with self._capture() as (_out, err):
                rc = self._main_in(repo, keys)
        self.assertEqual(rc, 2)
        self.assertIn("no refs", err.getvalue())

    def test_injected_git_log_failure_is_never_clean(self):
        repo = _init_repo()
        real_run = vks.subprocess.run

        def failing_pickaxe(command, *args, **kwargs):
            # Only the pickaxe search fails; the precondition probes still run.
            if "-S" in command:
                raise subprocess.TimeoutExpired("git", 5)
            return real_run(command, *args, **kwargs)

        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE})
            with self._capture() as (out, err):
                with mock.patch.object(vks.subprocess, "run", side_effect=failing_pickaxe):
                    rc = self._main_in(repo, keys)
        self.assertEqual(rc, 2)
        self.assertIn("UNVERIFIABLE", out.getvalue())
        self.assertIn("could not be completed", err.getvalue())
        self.assertIn("No clean verdict is possible", err.getvalue())
        self.assertNotIn("History is clean", out.getvalue())

    def test_failure_after_a_match_still_exits_three(self):
        """Exposure outranks ERROR: a confirmed finding is never hidden behind exit 2."""
        repo = _init_repo()
        _commit_secret(repo, SAMPLE.hex())

        real_run = vks.subprocess.run
        # Fail only the last-sorted encoding, so the hex spelling still
        # completes as FOUND: the constant is both reachable and unverifiable.
        last_encoding = max(vks.encodings(SAMPLE))

        def flaky_run(command, *args, **kwargs):
            if "-S" in command and last_encoding in command:
                raise subprocess.TimeoutExpired("git", 5)
            return real_run(command, *args, **kwargs)

        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE})
            with self._capture() as (out, err):
                with mock.patch.object(vks.subprocess, "run", side_effect=flaky_run):
                    rc = self._main_in(repo, keys)
        self.assertEqual(rc, 3)
        self.assertIn("k: REACHABLE", out.getvalue())
        self.assertIn("could not be completed", err.getvalue())
        self.assertIn("are still reachable", err.getvalue())
        self.assertNotIn("History is clean", out.getvalue())

    def test_reachable_constant_plus_unverifiable_constant_exits_three(self):
        """One constant positively found + another entirely unverifiable -> exit 3, both facts visible."""
        repo = _init_repo()
        _commit_secret(repo, SAMPLE.hex())
        other = bytes(range(16, 32))
        other_encodings = set(vks.encodings(other))
        real_run = vks.subprocess.run

        def flaky_run(command, *args, **kwargs):
            # Every search for `other` fails; every search for `k` succeeds.
            if "-S" in command and any(enc in command for enc in other_encodings):
                raise subprocess.TimeoutExpired("git", 5)
            return real_run(command, *args, **kwargs)

        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE, "other": other})
            with self._capture() as (out, err):
                with mock.patch.object(vks.subprocess, "run", side_effect=flaky_run):
                    rc = self._main_in(repo, keys)
        self.assertEqual(rc, 3)
        self.assertIn("k: REACHABLE", out.getvalue())
        self.assertIn("other: UNVERIFIABLE", out.getvalue())
        self.assertIn("could not be completed", err.getvalue())
        self.assertIn("could not be verified: other", err.getvalue())
        self.assertIn("are still reachable", err.getvalue())
        self.assertIn("k", err.getvalue().split("are still reachable in Git history:")[1].splitlines()[0])
        self.assertNotIn("History is clean", out.getvalue())
        self.assertNotIn("History is clean", err.getvalue())

    def test_diagnostics_never_contain_key_values(self):
        repo = _init_repo()
        needles = vks.encodings(SAMPLE)
        real_run = vks.subprocess.run

        def failing_pickaxe(command, *args, **kwargs):
            if "-S" in command:
                # Simulate git echoing a search expression back on stderr.
                return subprocess.CompletedProcess(
                    args=[], returncode=128, stdout="", stderr="fatal: bad object " + SAMPLE.hex(),
                )
            return real_run(command, *args, **kwargs)

        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE})
            with self._capture() as (out, err):
                with mock.patch.object(vks.subprocess, "run", side_effect=failing_pickaxe):
                    rc = self._main_in(repo, keys)
        self.assertEqual(rc, 2)
        combined = out.getvalue() + err.getvalue()
        for needle in needles:
            self.assertNotIn(needle, combined)

    def test_second_constant_still_searched_after_first_error(self):
        """An error on one constant neither stops the sweep nor leaks into its neighbours."""
        repo = _init_repo()
        other = bytes(range(16, 32))
        real_run = vks.subprocess.run
        calls = {"n": 0}

        def flaky_run(command, *args, **kwargs):
            if "-S" in command:
                calls["n"] += 1
                if calls["n"] == 1:
                    raise subprocess.TimeoutExpired("git", 5)
            return real_run(command, *args, **kwargs)

        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE, "other": other})
            with self._capture() as (out, err):
                with mock.patch.object(
                    vks.subprocess, "run", side_effect=flaky_run,
                ):
                    rc = self._main_in(repo, keys)
        self.assertEqual(rc, 2)
        self.assertIn("k: UNVERIFIABLE", out.getvalue())
        self.assertIn("other: clean", out.getvalue())
        self.assertNotIn("History is clean", out.getvalue())

    def test_exception_text_is_redacted(self):
        """An exception carrying the command line never leaks the needle."""
        needle = SAMPLE.hex()
        boom = subprocess.CalledProcessError(128, ["git", "log", "--all", "-S", needle])
        self.assertIn(needle, str(boom))  # the mock genuinely carries the needle
        with mock.patch.object(vks.subprocess, "run", side_effect=boom):
            verdict, error = vks.search_history(needle, [needle])
        self.assertIs(verdict, vks.Verdict.ERROR)
        self.assertIn("git could not be executed", error.detail)
        self.assertNotIn(needle, error.detail)

    def test_exception_from_cli_is_redacted_and_exits_two(self):
        repo = _init_repo()
        needles = vks.encodings(SAMPLE)
        real_run = vks.subprocess.run

        def exploding_pickaxe(command, *args, **kwargs):
            if "-S" in command:
                raise subprocess.CalledProcessError(128, ["git", "log", "--all", "-S", SAMPLE.hex()])
            return real_run(command, *args, **kwargs)

        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE})
            with self._capture() as (out, err):
                with mock.patch.object(vks.subprocess, "run", side_effect=exploding_pickaxe):
                    rc = self._main_in(repo, keys)
        self.assertEqual(rc, 2)
        combined = out.getvalue() + err.getvalue()
        for needle in needles:
            self.assertNotIn(needle, combined)
        self.assertIn("git could not be executed", combined)
        self.assertNotIn("History is clean", combined)

    def test_precondition_probe_exception_exits_two_without_traceback(self):
        """A raised probe failure becomes the documented exit 2, not a traceback."""
        repo = _init_repo()
        real_run = vks.subprocess.run

        def exploding_probe(command, *args, **kwargs):
            if "rev-parse" in command:
                raise OSError("git exploded mid-probe")
            return real_run(command, *args, **kwargs)

        with tempfile.TemporaryDirectory() as d:
            keys = _write_keys(Path(d), {"k": SAMPLE})
            with self._capture() as (out, err):
                with mock.patch.object(vks.subprocess, "run", side_effect=exploding_probe):
                    rc = self._main_in(repo, keys)
        self.assertEqual(rc, 2)
        self.assertIn("cannot verify history", err.getvalue())
        combined = out.getvalue() + err.getvalue()
        self.assertNotIn("Traceback", combined)
        self.assertNotIn("History is clean", combined)

    def test_missing_key_file_exits_two(self):
        with tempfile.TemporaryDirectory() as d:
            outside = Path(d) / "plain"
            outside.mkdir()
            with self._capture() as (_out, err):
                with _chdir(outside):
                    rc = vks.main(["--keys", str(Path(d) / "absent.txt")])
        self.assertEqual(rc, 2)
        self.assertIn("key file not found", err.getvalue())


class TestSpecGeneration(unittest.TestCase):
    def test_spec_line_count_matches_encodings_and_uses_placeholder(self):
        values = {"a": SAMPLE, "b": bytes(range(16, 32))}
        lines = gen.build_spec(values)
        expected = len({f for raw in values.values() for f in vks.encodings(raw)})
        self.assertEqual(len(lines), expected)
        for line in lines:
            self.assertTrue(line.startswith("literal:"))
            self.assertTrue(line.endswith("==>" + gen.PLACEHOLDER))

    def test_refuses_to_write_inside_repo(self):
        repo_root = Path(gen.__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as d:
            keys = Path(d) / "keys.txt"
            keys.write_text("k = " + SAMPLE.hex() + "\n", encoding="utf-8")
            in_repo = repo_root / "should_not_be_written.txt"
            rc = gen.main(["--keys", str(keys), "--out", str(in_repo)])
        self.assertEqual(rc, 2)
        self.assertFalse(in_repo.exists())

    def test_writes_to_external_path(self):
        with tempfile.TemporaryDirectory() as d:
            keys = Path(d) / "keys.txt"
            keys.write_text("k = " + SAMPLE.hex() + "\n", encoding="utf-8")
            out = Path(d) / "spec.txt"
            rc = gen.main(["--keys", str(keys), "--out", str(out)])
            self.assertEqual(rc, 0)
            body = out.read_text(encoding="utf-8")
        self.assertIn(gen.PLACEHOLDER, body)
        self.assertIn(SAMPLE.hex(), body)


if __name__ == "__main__":
    unittest.main()
