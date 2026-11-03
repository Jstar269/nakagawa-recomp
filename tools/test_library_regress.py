# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""tools/library_regress.py: the library ladder, its snapshots, the comparer and the fast subset."""

from __future__ import annotations

import argparse
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Callable
import unittest

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import library_regress  # noqa: E402

OLD = "a" * 40
NEW = "b" * 40


def built(frames: int = 0, failure: str | None = "LAUNCH_TIMEOUT", *, presentation: str | None = None,
          boundary: str | None = None, imports: list[str] | None = None, status: str = "FAIL",
          exit_code: int | None = None) -> dict:
    """A BUILT record's fields as library_build.summarize_report writes them."""
    return {"outcome": "BUILT", "failure_class": failure, "launch": {
        "status": status, "presentation": presentation or ("FRAME_SUBMITTED" if frames else "NO_FRAME_SUBMISSIONS"),
        "frame_submissions": frames, "failure_class": failure, "runtime_boundary": boundary,
        "runtime_imports": imports or [], "process_exit_code": exit_code}}


def manifest(titles: dict[str, dict], *, commit: str | None = OLD, launch_timeout: int | None = 30) -> dict:
    """A synthetic library-build.json: one record per source key."""
    records = []
    for index, (key, fields) in enumerate(titles.items()):
        row = {"source_key": key, "source": key, "disc_id": f"TEST{index:05d}", "title": f"Title {index}",
               "wall_seconds": 600.0}
        if commit is not None and fields.get("outcome") not in ("PENDING", "DUPLICATE"):
            row["run_settings"] = {"source_commit": commit, "launch_timeout_seconds": launch_timeout}
        row.update(fields)
        records.append(row)
    settings = None if launch_timeout is None else {"launch_timeout_seconds": launch_timeout,
                                                    "source_commit": commit}
    return {"format": "nakagawa-library-build", "schema_version": 1, "updated_at": "2026-11-03T00:00:00Z",
            "settings": settings, "titles": records}


class LadderTests(unittest.TestCase):
    def place(self, record: dict, frames: int = 60) -> dict | None:
        return library_regress.classify(record, frames)

    def test_every_launch_ending_gets_its_rung_and_end(self) -> None:
        cases = [
            (built(400), ("frames", "running", None)),
            (built(5), ("first_frame", "running", None)),
            (built(0), ("built", "hang", None)),
            (built(0, "NONE", status="PASS"), ("built", "exited", None)),
            (built(900, "NONE", status="PASS"), ("frames", "exited", None)),
            (built(3, "UNRESOLVED_DISPATCH_TARGET", boundary="target-not-executable"),
             ("first_frame", "named_stop", "UNRESOLVED_DISPATCH_TARGET:target-not-executable")),
            (built(0, "UNSUPPORTED_IMPORT", imports=[json.dumps({"library": "sceFoo", "nid_name": "sceFooBar"})]),
             ("built", "named_stop", "UNSUPPORTED_IMPORT:sceFoo:sceFooBar")),
            (built(0, "UNSUPPORTED_IMPORT", imports=["sceFoo:sceFooBaz"]),
             ("built", "named_stop", "UNSUPPORTED_IMPORT:sceFoo:sceFooBaz")),
            (built(70, "NATIVE_RUNTIME_CRASH", exit_code=3221225477),
             ("frames", "crash", "NATIVE_RUNTIME_CRASH:3221225477")),
            (built(0, "HEADLESS_UNAVAILABLE"), ("built", "environment", "HEADLESS_UNAVAILABLE")),
            (built(0, None, status="NOT_RUN"), ("built", "environment", "LAUNCH_NOT_RUN")),
            ({"outcome": "BUILD_FAILED", "failed_stage": "compile", "failure_class": "COMPILE_FAILED"},
             ("build_failed", "named_stop", "compile:COMPILE_FAILED")),
            ({"outcome": "TIMED_OUT", "failure_class": "TITLE_TIME_BUDGET", "reached_stage": "compile"},
             ("build_failed", "hang", "compile:TITLE_TIME_BUDGET")),
            ({"outcome": "NO_REPORT", "failure_class": None}, ("build_failed", "crash", "NO_REPORT")),
        ]
        for record, expected in cases:
            with self.subTest(expected=expected):
                place = self.place(record)
                assert place is not None
                self.assertEqual((place["rung"], place["end"], place["stop"]), expected)

    def test_a_frame_count_without_presentation_evidence_is_no_frame(self) -> None:
        place = self.place(built(80, presentation="NO_FRAME_SUBMISSIONS"))
        assert place is not None
        self.assertEqual((place["rung"], place["end"]), ("built", "hang"))

    def test_the_frame_threshold_sets_the_top_rung(self) -> None:
        self.assertEqual(self.place(built(59))["rung"], "first_frame")  # type: ignore[index]
        self.assertEqual(self.place(built(60))["rung"], "frames")  # type: ignore[index]
        self.assertEqual(self.place(built(59), frames=10)["rung"], "frames")  # type: ignore[index]

    def test_titles_this_run_did_not_end_are_not_on_the_ladder(self) -> None:
        for outcome in ("PENDING", "RUNNING", "DUPLICATE"):
            self.assertIsNone(self.place({"outcome": outcome}))


class SnapshotTests(unittest.TestCase):
    def test_a_snapshot_keeps_each_titles_commit_and_counts_the_untested(self) -> None:
        payload = manifest({"a.iso": built(100), "b.iso": {"outcome": "PENDING"}})
        payload["titles"][0]["run_settings"]["source_commit"] = NEW  # resumed on a later tree
        snapshot = library_regress.snapshot_from_manifest(payload, recorded_at="2026-11-03T00:00:00Z")
        self.assertEqual(snapshot["titles"]["a.iso"]["commit"], NEW)
        self.assertEqual(snapshot["untested"], 1)
        self.assertEqual(snapshot["commits"], [NEW])
        self.assertEqual(snapshot["ladder"], {"frames_threshold": 60, "ceiling": "frames",
                                              "launch_timeout_seconds": 30})

    def test_stand_in_commit_and_timeout_fill_only_an_older_manifest(self) -> None:
        old = manifest({"a.iso": built(100)}, commit=None, launch_timeout=None)
        snapshot = library_regress.snapshot_from_manifest(old, commit=OLD, launch_timeout=30)
        self.assertEqual(snapshot["titles"]["a.iso"]["commit"], OLD)
        self.assertEqual(snapshot["ladder"]["launch_timeout_seconds"], 30)
        recorded = library_regress.snapshot_from_manifest(manifest({"a.iso": built(100)}), commit=NEW,
                                                          launch_timeout=99)
        self.assertEqual(recorded["titles"]["a.iso"]["commit"], OLD)
        self.assertEqual(recorded["ladder"]["launch_timeout_seconds"], 30)

    def test_anything_but_a_manifest_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            library_regress.snapshot_from_manifest({"format": "something-else", "titles": []})
        with self.assertRaises(ValueError):
            library_regress.snapshot_from_manifest(manifest({}), mode="weekly")


class CompareTests(unittest.TestCase):
    def snapshots(self, old: dict[str, dict], new: dict[str, dict], **new_options) -> tuple[dict, dict]:
        mode = new_options.pop("mode", "full")
        return (library_regress.snapshot_from_manifest(manifest(old)),
                library_regress.snapshot_from_manifest(manifest(new, commit=NEW, **new_options), mode=mode))

    def test_titles_that_went_backwards_are_named_with_both_commits(self) -> None:
        old, new = self.snapshots(
            {"down.iso": built(400), "up.iso": built(0, "UNSUPPORTED_IMPORT", imports=["sceA:f"]),
             "crash.iso": built(400), "stop.iso": built(0, "UNSUPPORTED_IMPORT", imports=["sceA:f"]),
             "same.iso": built(400), "env.iso": built(400), "gone.iso": built(1)},
            {"down.iso": built(2), "up.iso": built(300), "crash.iso": built(400, "NATIVE_RUNTIME_CRASH"),
             "stop.iso": built(0, "UNSUPPORTED_IMPORT", imports=["sceB:g"]), "same.iso": built(500),
             "env.iso": built(0, "HEADLESS_UNAVAILABLE"), "new.iso": built(1)})
        result = library_regress.compare(old, new)
        self.assertTrue(result["like_for_like"])
        self.assertEqual([row["source_key"] for row in result["down"]], ["crash.iso", "down.iso"])
        self.assertEqual([row["source_key"] for row in result["up"]], ["up.iso"])
        self.assertEqual([row["source_key"] for row in result["changed"]], ["stop.iso"])
        self.assertEqual(result["counts"], {"down": 2, "up": 1, "changed": 1, "same": 1, "not_compared": 1,
                                            "only_old": 1, "only_new": 1})
        down = result["down"][1]
        self.assertEqual((down["old"]["commit"], down["new"]["commit"]), (OLD, NEW))
        self.assertEqual((down["old"]["rung"], down["new"]["rung"]), ("frames", "first_frame"))
        text = library_regress.render_comparison(result)
        self.assertIn(f"DOWN    TEST00000  Title 0: frames/running -> first_frame/running [{OLD[:12]}..{NEW[:12]}]",
                      text)

    def test_a_different_way_of_stopping_at_the_same_rung_is_changed_not_down(self) -> None:
        # A missing import that now works and leaves the title hanging, or a new crash after it,
        # is listed for review; a title that was running and now stops at its rung went down.
        old, new = self.snapshots(
            {"hang.iso": built(0, "UNSUPPORTED_IMPORT", imports=["scePower:f"]),
             "crash.iso": built(0, "UNSUPPORTED_IMPORT", imports=["sceRtc:g"]),
             "stopped.iso": built(5)},
            {"hang.iso": built(0), "crash.iso": built(0, "NATIVE_RUNTIME_CRASH"),
             "stopped.iso": built(5, "RUNTIME_SEMANTIC_BOUNDARY", boundary="null-code-pointer")})
        result = library_regress.compare(old, new)
        self.assertEqual([row["source_key"] for row in result["changed"]], ["crash.iso", "hang.iso"])
        self.assertEqual([row["source_key"] for row in result["down"]], ["stopped.iso"])

    def test_an_older_manifests_coarser_stop_name_is_the_same_stop(self) -> None:
        same = library_regress.same_stop
        self.assertTrue(same("UNRESOLVED_DISPATCH_TARGET", "UNRESOLVED_DISPATCH_TARGET:incomplete-executable-span"))
        self.assertTrue(same("UNSUPPORTED_IMPORT:sceRtc:sceRtcGetTime_t", "UNSUPPORTED_IMPORT:sceRtcGetTime_t"))
        self.assertFalse(same("UNSUPPORTED_IMPORT:sceRtc:sceRtcGetTime_t", "UNSUPPORTED_IMPORT:sceUsb:sceUsbStart"))
        self.assertFalse(same("UNRESOLVED_DISPATCH_TARGET:a", "RUNTIME_SEMANTIC_BOUNDARY:a"))
        self.assertFalse(same(None, "UNSUPPORTED_IMPORT"))

    def test_a_fast_run_is_compared_up_to_the_first_frame(self) -> None:
        old, new = self.snapshots({"a.iso": built(400), "b.iso": built(400)},
                                  {"a.iso": built(3), "b.iso": built(0)}, mode="fast")
        result = library_regress.compare(old, new)
        self.assertEqual(result["compared_up_to"], "first_frame")
        self.assertEqual([row["source_key"] for row in result["down"]], ["b.iso"])
        self.assertEqual(result["counts"]["same"], 1)

    def test_runs_with_different_launch_timeouts_compare_rungs_only(self) -> None:
        old, new = self.snapshots({"a.iso": built(400), "b.iso": built(400)},
                                  {"a.iso": built(30, "NATIVE_RUNTIME_CRASH"), "b.iso": built(0)},
                                  launch_timeout=10)
        result = library_regress.compare(old, new)
        self.assertFalse(result["like_for_like"])
        self.assertEqual(result["compared_up_to"], "first_frame")
        self.assertEqual([row["source_key"] for row in result["down"]], ["b.iso"])

    def test_the_commit_range_of_a_regression_is_listed_from_the_checkout(self) -> None:
        old, new = self.snapshots({"a.iso": built(400)}, {"a.iso": built(0)})
        result = library_regress.compare(old, new)
        calls: list[list[str]] = []

        def fake_git(command: list[str], **_options) -> subprocess.CompletedProcess:
            calls.append(command)
            return subprocess.CompletedProcess(command, 0, stdout="b0b0b0b merge two\na1a1a1a merge one\n",
                                               stderr="")

        ranges = library_regress.commit_ranges(result, Path("repo"), run=fake_git)
        self.assertEqual(ranges, {f"{OLD}..{NEW}": ["b0b0b0b merge two", "a1a1a1a merge one"]})
        self.assertEqual(calls[0][-1], f"{OLD}..{NEW}")
        self.assertIn("--first-parent", calls[0])


class CommandLineTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="library-regress-test-")
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, name: str, payload: dict) -> Path:
        path = self.root / name
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def main(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = library_regress.main(list(argv))
        return code, out.getvalue()

    def test_compare_exits_one_only_when_a_title_went_down(self) -> None:
        old = self.write("old.json", manifest({"a.iso": built(400)}))
        worse = self.write("worse.json", manifest({"a.iso": built(0)}, commit=NEW))
        better = self.write("better.json", manifest({"a.iso": built(800)}, commit=NEW))
        code, text = self.main("compare", str(old), str(worse), "--json", str(self.root / "out.json"))
        self.assertEqual(code, 1)
        self.assertIn("down 1", text)
        self.assertEqual(json.loads((self.root / "out.json").read_text(encoding="utf-8"))["counts"]["down"], 1)
        self.assertEqual(self.main("compare", str(old), str(better))[0], 0)

    def test_record_keeps_snapshots_and_history_reads_them_back(self) -> None:
        store = self.root / "store"
        first = self.write("first.json", manifest({"a.iso": built(400)}))
        second = self.write("second.json", manifest({"a.iso": built(0, "UNSUPPORTED_IMPORT", imports=["sceA:f"])},
                                                     commit=NEW))
        self.main("record", str(first), "--store", str(store))
        code, text = self.main("record", str(second), "--store", str(store), "--mode", "fast")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(text)["titles"], 1)
        snapshots = sorted((store / "snapshots").glob("*.json"))
        self.assertEqual(len({path.name for path in snapshots}), 2)
        rows = library_regress.history(store, "test00000")
        self.assertEqual([(row["rung"], row["commit"]) for row in rows],
                         [("frames", OLD), ("built", NEW)])
        self.assertEqual(self.main("compare", str(snapshots[0]), str(snapshots[1]))[0], 1)


class FastSelectionTests(unittest.TestCase):
    def baseline(self, count: int, wall: float = 600.0) -> dict:
        titles = {f"t{index:03d}.iso": built(400) for index in range(count)}
        titles["noframe.iso"] = built(0)
        snapshot = library_regress.snapshot_from_manifest(manifest(titles))
        for place in snapshot["titles"].values():
            place["wall_seconds"] = wall
        return snapshot

    def test_the_rotation_fits_the_budget_and_comes_round_to_every_title(self) -> None:
        baseline = self.baseline(20)
        seen: list[str] = []
        cursor = 0
        for _ in range(5):
            choice = library_regress.select_fast(baseline, budget_seconds=1800, jobs=2, cursor=cursor)
            self.assertEqual(len(choice["selected"]), 6)  # three 10-minute titles per lane
            self.assertLessEqual(choice["estimated_seconds"], 1800)
            seen.extend(choice["selected"])
            cursor = choice["next_cursor"]
        self.assertEqual(choice["candidates"], 20)
        self.assertEqual(set(seen), {f"t{index:03d}.iso" for index in range(20)})
        self.assertNotIn("noframe.iso", seen)
        again = library_regress.select_fast(baseline, budget_seconds=1800, jobs=2, cursor=0)
        self.assertEqual(again["selected"], seen[:6])

    def test_a_title_slower_than_the_budget_is_passed_over_and_named(self) -> None:
        baseline = self.baseline(3)
        slow = sorted(baseline["titles"], key=library_regress._rotation_key)
        slow = [key for key in slow if key != "noframe.iso"][0]
        baseline["titles"][slow]["wall_seconds"] = 99999
        choice = library_regress.select_fast(baseline, budget_seconds=3600, jobs=1, cursor=0)
        self.assertEqual(choice["too_slow"], [slow])
        self.assertEqual(len(choice["selected"]), 2)
        self.assertNotIn(slow, choice["selected"])


class SlotTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="library-regress-slots-")
        self.slots = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_a_slot_is_taken_only_below_the_machine_limit(self) -> None:
        for name in ("other-a.lock", "other-b.lock"):
            (self.slots / name).write_text("x", encoding="utf-8")
        sleeps: list[float] = []
        self.assertIsNone(library_regress.acquire_slot(self.slots, "mine.lock", 2, sleep=sleeps.append,
                                                       log=lambda _text: None, max_wait_seconds=60))
        self.assertFalse((self.slots / "mine.lock").exists())
        self.assertTrue(sleeps)
        lock = library_regress.acquire_slot(self.slots, "mine.lock", 3, log=lambda _text: None)
        self.assertEqual(lock, self.slots / "mine.lock")
        self.assertIn("library-regress pid=", lock.read_text(encoding="utf-8"))  # type: ignore[union-attr]

    def test_a_quiet_lock_holds_the_run(self) -> None:
        (self.slots / "recomp-perf.lock").write_text("x", encoding="utf-8")
        held: list[float] = []

        def sleep(seconds: float) -> None:
            held.append(seconds)
            (self.slots / "recomp-perf.lock").unlink(missing_ok=True)

        lock = library_regress.acquire_slot(self.slots, "mine.lock", 3, quiet_globs=["recomp-perf*.lock"],
                                            sleep=sleep, log=lambda _text: None)
        self.assertEqual(len(held), 1)
        self.assertIsNotNone(lock)


class FastRunTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="library-regress-fast-")
        self.root = Path(self._tmp.name)
        self.store = self.root / "store"
        self.slots = self.root / "slots"
        titles = {f"t{index}.iso": built(400) for index in range(4)}
        baseline = library_regress.snapshot_from_manifest(manifest(titles))
        self.baseline = self.root / "baseline.json"
        self.baseline.write_text(json.dumps(baseline), encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def args(self, **overrides) -> argparse.Namespace:
        options = dict(store=self.store, baseline=self.baseline, disc_root=self.root / "discs",
                       scratch_root=self.root / "scratch", budget_minutes=20, jobs=1, cursor=None,
                       slot_dir=self.slots, slot_name="library-regress.lock", max_slots=3, quiet_lock=[],
                       repo=None, keep_scratch=False, dry_run=False)
        options.update(overrides)
        return argparse.Namespace(**options)

    def fake_library_build(self, frames: int) -> tuple[list[list[str]], Callable[..., subprocess.CompletedProcess]]:
        calls: list[list[str]] = []

        def run(command: list[str], **_options) -> subprocess.CompletedProcess:
            calls.append(command)
            self.assertTrue((self.slots / "library-regress.lock").exists())  # built inside the slot
            only = Path(command[command.index("--only") + 1]).read_text(encoding="utf-8").split()
            scratch = Path(command[command.index("--work-dir") + 1])
            scratch.mkdir(parents=True)
            out = Path(command[command.index("--manifest-dir") + 1]) / "library-build.json"
            out.write_text(json.dumps(manifest({key: built(frames) for key in only}, commit=NEW)),
                           encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)

        return calls, run

    def test_a_fast_run_builds_its_subset_in_a_slot_and_flags_a_title_with_no_frame(self) -> None:
        calls, run = self.fake_library_build(frames=0)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = library_regress.run_fast(self.args(), run=run, log=lambda _text: None)
        self.assertEqual(code, 1)
        self.assertEqual(len(calls), 1)
        self.assertIn("--launch-timeout", calls[0])
        self.assertFalse((self.slots / "library-regress.lock").exists())  # released
        self.assertFalse(any((self.root / "scratch").iterdir()))  # scratch removed
        state = json.loads((self.store / "fast-state.json").read_text(encoding="utf-8"))
        self.assertEqual((state["cursor"], state["runs"]), (2, 1))  # two 10-minute titles in 20 minutes
        self.assertIn("down 2", out.getvalue())
        snapshot = json.loads((self.store / "snapshots" / state["last_snapshot"]).read_text(encoding="utf-8"))
        self.assertEqual(snapshot["mode"], "fast")

    def test_the_next_fast_run_continues_the_rotation(self) -> None:
        _calls, run = self.fake_library_build(frames=5)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(library_regress.run_fast(self.args(), run=run, log=lambda _text: None), 0)
        first = sorted(self.store.glob("runs/*/only.txt"))[0].read_text(encoding="utf-8").split()
        for run_dir in (self.store / "runs").iterdir():
            run_dir.rename(run_dir.with_name("first-" + run_dir.name))
        with contextlib.redirect_stdout(io.StringIO()):
            library_regress.run_fast(self.args(), run=run, log=lambda _text: None)
        second = next(self.store.glob("runs/2*/only.txt")).read_text(encoding="utf-8").split()
        self.assertEqual(len(set(first) | set(second)), 4)

    def test_a_dry_run_only_lists_the_subset(self) -> None:
        def run(*_args, **_options) -> subprocess.CompletedProcess:
            raise AssertionError("a dry run builds nothing")

        self.assertEqual(library_regress.run_fast(self.args(dry_run=True), run=run, log=lambda _text: None), 0)
        self.assertEqual(len(next(self.store.glob("runs/*/only.txt")).read_text(encoding="utf-8").split()), 2)
        self.assertFalse((self.store / "fast-state.json").exists())


if __name__ == "__main__":
    unittest.main()
