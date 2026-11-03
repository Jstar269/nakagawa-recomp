# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""A library-wide regression net over ``tools/library_build.py`` runs.

    python tools/library_regress.py record <library-build.json> --store <folder> [--mode full|fast]
    python tools/library_regress.py compare <old> <new> [--repo <checkout>] [--json <file>]
    python tools/library_regress.py history --store <folder> <disc ID or source key>
    python tools/library_regress.py fast --store <folder> --baseline <snapshot> --disc-root <folder>
        --scratch-root <folder> [--budget-minutes 60] [--jobs 2] [--slot-dir <folder>]

Each title of a run gets one place on a short ladder, read from what ``library_build.py``
already records for its bring-up and first hidden launch:

    build_failed < built < first_frame < frames

``first_frame`` is the launch's presentation evidence (``FRAME_SUBMITTED``); ``frames`` is at
least ``--frames`` frame submissions (60 by default). The runtime prints one line per submitted
frame and no content hash, so "frames" counts submissions, not visually different pictures.

How the launch ended is recorded beside the rung: ``running``, ``exited``, ``named_stop``,
``crash`` or ``hang``. The first two are alive and rank above the three that stopped.
``named_stop`` keeps the name the runtime or the bring-up gave the missing piece (the failure
class and its runtime boundary or first runtime import, for example
``UNSUPPORTED_IMPORT:sceFoo:sceFooBar``). ``running`` is a title still presenting frames when
the launch timeout ended it; ``hang`` is a launch timeout with no frame at all. A launch the
host could not run (``environment``) or a built title whose launch recorded no status
(``unknown``) gives no verdict.

``record`` turns a manifest into a ladder snapshot in the store, one file per run, with each
title's commit and the titles the run did not end. ``compare`` reports every title that went
down or up between two snapshots (or two manifests), with both commits, and exits 1 when any
title went down: a lower rung, or a title that was alive at a rung and stopped there. It exits
3 (no verdict) when a title the old run judged has no verdict in the new one (missing, left
PENDING or RUNNING, or a launch with no verdict), or when nothing could be compared at all; it
never exits 0 for a run that proves nothing. ``--partial`` judges only the titles the new run
ended, for a run still in progress; a fast snapshot is judged on the titles it was given.
Titles are keyed by their path in the disc folder, so a disc that was moved or renamed is a
different title: its old key has no verdict until the baseline is recorded again. A title that stops at the same rung in a
different way (another missing piece, or a hang where a missing import stopped it before, which
is often the import now working) is listed as changed, for review, and is not a regression. Two
runs are compared like for like only when their launch timeout and frame threshold match;
otherwise only the rung is compared, up to ``first_frame``. A fast snapshot is compared up to
``first_frame``.

``fast`` is the check before landing a batch: boot to first frame on a rotating subset of the
titles that reached a frame in the baseline, sized from the baseline's per-title times to fit
the budget. It waits for a title slot (``--slot-dir``: one lock file per running job, at most
``--max-slots`` machine-wide), builds the subset with ``library_build.py --only`` into a fresh
scratch folder, records the run and compares it with the baseline. Every selected title must
end with a verdict; one that does not makes the run exit 3, like a run with no slot or a
failed build. The full ladder is a whole
``library_build.py`` run followed by ``record``.

Snapshots, manifests and comparisons name the user's own discs: they are private records and
are never published. This tool holds no title data of its own.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from typing import Any, Callable, Iterable

ROOT = Path(__file__).resolve().parents[1]
LIBRARY_BUILD = ROOT / "tools" / "library_build.py"
SNAPSHOT_FORMAT = "nakagawa-library-ladder"
SNAPSHOT_SCHEMA_VERSION = 1
MANIFEST_FORMAT = "nakagawa-library-build"
RUNGS = ("build_failed", "built", "first_frame", "frames")
# Alive (1) ranks above stopped (0); the kinds of stop are told apart, never ranked.
END_RANK = {"crash": 0, "hang": 0, "named_stop": 0, "exited": 1, "running": 1}
DEFAULT_FRAMES = 60
DEFAULT_BUDGET_MINUTES = 60
DEFAULT_TITLE_COST_SECONDS = 1800
EXIT_DOWN = 1
EXIT_NO_VERDICT = 3
# A title in this state has no verdict: it is never compared, and the comparer says so.
NO_VERDICT_ENDS = frozenset({"environment", "unknown"})
STOP_NAME_MAX = 120
# A launch the host could not give the title: never a verdict on the title.
ENVIRONMENT_CLASSES = frozenset({
    "HEADLESS_UNAVAILABLE", "RUNTIME_INPUT_UNAVAILABLE", "RUNTIME_TRACE_UNAVAILABLE",
    "RUNTIME_ARGUMENT_FAILURE",
})
CRASH_CLASSES = frozenset({"NATIVE_RUNTIME_CRASH", "LAUNCH_FAILED"})
# Titles that have not ended in this run (still to build, building, or a copy of another disc).
UNTESTED_OUTCOMES = frozenset({"PENDING", "RUNNING", "DUPLICATE"})


def _utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(payload, indent=1, sort_keys=True, ensure_ascii=False) + "\n",
                         encoding="utf-8")
    os.replace(temporary, path)


def _stop_name(*parts: object) -> str | None:
    text = ":".join(str(part) for part in parts if part not in (None, ""))
    return text[:STOP_NAME_MAX] or None


def _first_runtime_import(launch: dict[str, Any]) -> str | None:
    imports = launch.get("runtime_imports")
    if not isinstance(imports, list) or not imports:
        return None
    first = imports[0]
    if isinstance(first, str) and first.startswith("{"):
        try:
            row = json.loads(first)
        except ValueError:
            return first
        if isinstance(row, dict):
            return _stop_name(row.get("library"), row.get("nid_name") or row.get("nid"))
    return str(first)


# -- the ladder ------------------------------------------------------------------------------

def classify(record: dict[str, Any], frames_threshold: int = DEFAULT_FRAMES) -> dict[str, Any] | None:
    """One manifest record's place on the ladder, or None for a title this run did not end."""
    outcome = record.get("outcome")
    if outcome in UNTESTED_OUTCOMES or outcome is None:
        return None
    if outcome != "BUILT":
        if outcome == "TIMED_OUT":
            end = "hang"
        elif outcome == "NO_REPORT":
            end = "crash"
        else:
            end = "named_stop"
        return {"rung": "build_failed", "end": end, "frames": 0,
                "stop": _stop_name(record.get("failed_stage") or record.get("reached_stage"),
                                   record.get("failure_class") or outcome)}
    raw_launch = record.get("launch")
    launch: dict[str, Any] = raw_launch if isinstance(raw_launch, dict) else {}
    raw_frames = launch.get("frame_submissions")
    frames = raw_frames if isinstance(raw_frames, int) else 0
    presented = launch.get("presentation") == "FRAME_SUBMITTED" and frames > 0
    rung = "frames" if presented and frames >= frames_threshold else (
        "first_frame" if presented else "built")
    failure = launch.get("failure_class") or record.get("failure_class")
    status = launch.get("status")
    if failure in (None, "NONE") and status not in (None, "PASS", "NOT_RUN"):
        # A launch that failed without a class: a timeout is still one, anything else a stop.
        failure = "LAUNCH_TIMEOUT" if launch.get("exit_classification") == "TIMED_OUT" else None
        if failure is None:
            return {"rung": rung, "end": "crash", "frames": frames,
                    "stop": _stop_name(f"LAUNCH_{status}_UNCLASSIFIED")}
    stop = None
    if status is None:
        end = "unknown"
        stop = "LAUNCH_STATUS_MISSING"
    elif status == "NOT_RUN" or failure in ENVIRONMENT_CLASSES:
        end = "environment"
        stop = _stop_name(failure or "LAUNCH_NOT_RUN")
    elif failure in (None, "NONE"):
        end = "exited"
    elif failure == "LAUNCH_TIMEOUT":
        end = "running" if presented else "hang"
    elif failure in CRASH_CLASSES:
        end = "crash"
        stop = _stop_name(failure, launch.get("process_exit_code"))
    else:
        end = "named_stop"
        stop = _stop_name(failure, launch.get("runtime_boundary") or _first_runtime_import(launch))
    return {"rung": rung, "end": end, "frames": frames, "stop": stop}


def _record_commit(record: dict[str, Any], manifest: dict[str, Any]) -> str | None:
    for settings in (record.get("run_settings"), manifest.get("settings")):
        commit = settings.get("source_commit") if isinstance(settings, dict) else None
        if isinstance(commit, str):
            return commit
    return None


def _launch_timeout(manifest: dict[str, Any]) -> int | None:
    settings = manifest.get("settings")
    timeout = settings.get("launch_timeout_seconds") if isinstance(settings, dict) else None
    return timeout if isinstance(timeout, int) else None


def snapshot_from_manifest(manifest: dict[str, Any], *, mode: str = "full",
                           frames_threshold: int = DEFAULT_FRAMES, commit: str | None = None,
                           launch_timeout: int | None = None,
                           recorded_at: str | None = None) -> dict[str, Any]:
    """A ladder snapshot of one ``library-build.json``. ``commit`` and ``launch_timeout``
    stand in for a manifest that predates its recorded settings; they never override one."""
    if not isinstance(manifest, dict) or manifest.get("format") != MANIFEST_FORMAT \
            or not isinstance(manifest.get("titles"), list):
        raise ValueError("not a library-build manifest")
    if mode not in ("full", "fast"):
        raise ValueError(f"unknown mode {mode!r}")
    titles: dict[str, dict[str, Any]] = {}
    untested: dict[str, dict[str, Any]] = {}
    for record in manifest["titles"]:
        if not isinstance(record, dict) or not isinstance(record.get("source_key"), str):
            continue
        place = classify(record, frames_threshold)
        if place is None:
            untested[record["source_key"]] = {"outcome": record.get("outcome") or "NO_OUTCOME",
                                              "disc_id": record.get("disc_id"), "title": record.get("title")}
            continue
        place.update({
            "disc_id": record.get("disc_id"),
            "title": record.get("title"),
            "commit": _record_commit(record, manifest) or commit,
            "wall_seconds": record.get("wall_seconds"),
        })
        titles[record["source_key"]] = place
    commits = sorted({place["commit"] for place in titles.values() if place["commit"]})
    return {
        "format": SNAPSHOT_FORMAT,
        "schema_version": SNAPSHOT_SCHEMA_VERSION,
        "mode": mode,
        "recorded_at": recorded_at or _utc_now(),
        "manifest_updated_at": manifest.get("updated_at"),
        "commits": commits,
        "ladder": {
            "frames_threshold": frames_threshold,
            "ceiling": "first_frame" if mode == "fast" else "frames",
            "launch_timeout_seconds": _launch_timeout(manifest) or launch_timeout,
        },
        "untested": len(untested),
        "untested_titles": untested,
        "titles": titles,
    }


def load_snapshot(path: Path, **manifest_options: Any) -> dict[str, Any]:
    """A snapshot file, or a manifest read as a full snapshot."""
    payload = _read_json(path)
    if isinstance(payload, dict) and payload.get("format") == SNAPSHOT_FORMAT:
        if payload.get("schema_version") != SNAPSHOT_SCHEMA_VERSION:
            raise ValueError(f"{path.name}: unsupported snapshot schema {payload.get('schema_version')}")
        return payload
    return snapshot_from_manifest(payload, **manifest_options)


def _snapshot_name(snapshot: dict[str, Any]) -> str:
    stamp = snapshot["recorded_at"].replace(":", "").replace("-", "")
    commit = snapshot["commits"][-1][:12] if len(snapshot["commits"]) == 1 else (
        f"{len(snapshot['commits'])}commits" if snapshot["commits"] else "nocommit")
    return f"{stamp}-{snapshot['mode']}-{commit}.json"


def record(snapshot: dict[str, Any], store: Path) -> Path:
    """Keep a snapshot in the store's ``snapshots`` folder under the next sequence number, so
    the names sort in the order the runs were recorded and no run replaces another."""
    folder = store / "snapshots"
    folder.mkdir(parents=True, exist_ok=True)
    numbers = [int(path.name.split("-", 1)[0]) for path in folder.glob("*.json")
               if path.name.split("-", 1)[0].isdigit()]
    number = max(numbers, default=0) + 1
    while True:
        path = folder / f"{number:05d}-{_snapshot_name(snapshot)}"
        try:
            os.close(os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY))  # claim the number
        except FileExistsError:
            number += 1  # another record took it at the same moment
            continue
        _write_json(path, snapshot)
        return path


# -- comparison ------------------------------------------------------------------------------

def _score(place: dict[str, Any], ceiling: int, like: bool) -> tuple[int, ...]:
    rung = min(RUNGS.index(place["rung"]), ceiling)
    return (rung, END_RANK[place["end"]]) if like else (rung,)


def same_stop(old: str | None, new: str | None) -> bool:
    """One stop named by two runs, allowing for an older manifest's coarser name: the same
    failure class with no detail on one side, or the same import with and without its library."""
    if old == new:
        return True
    if not old or not new:
        return False
    old_parts, new_parts = old.split(":"), new.split(":")
    if old_parts[0] != new_parts[0]:
        return False
    return len(old_parts) == 1 or len(new_parts) == 1 or old_parts[-1] == new_parts[-1]


def _row(key: str, before: dict[str, Any], after: dict[str, Any] | None,
         untested: dict[str, Any] | None = None) -> dict[str, Any]:
    fields = ("rung", "end", "stop", "frames", "commit")
    return {
        "source_key": key,
        "disc_id": (after or {}).get("disc_id") or before.get("disc_id") or (untested or {}).get("disc_id"),
        "title": (after or {}).get("title") or before.get("title") or (untested or {}).get("title"),
        "old": {field: before.get(field) for field in fields},
        "new": {field: after.get(field) for field in fields} if after is not None else None,
    }


def compare(old: dict[str, Any], new: dict[str, Any], *, keys: Iterable[str] | None = None,
            partial: bool = False) -> dict[str, Any]:
    """Judge every title the old run judged (or ``keys``, or a fast run's own titles): down, up,
    changed at the same place, the same, or no verdict in the new run. ``partial`` skips the
    titles the new run has not ended (still to build, or not in it at all) instead of
    counting them as no verdict."""
    old_ladder, new_ladder = old["ladder"], new["ladder"]
    like = (old_ladder.get("launch_timeout_seconds") is not None
            and old_ladder.get("launch_timeout_seconds") == new_ladder.get("launch_timeout_seconds")
            and old_ladder.get("frames_threshold") == new_ladder.get("frames_threshold"))
    ceiling = min(RUNGS.index(old_ladder["ceiling"]), RUNGS.index(new_ladder["ceiling"]))
    if not like:
        ceiling = min(ceiling, RUNGS.index("first_frame"))
    untested_titles: dict[str, Any] = new.get("untested_titles") or {}
    if keys is not None:
        scope = set(keys)
    elif new["mode"] == "fast":
        scope = set(new["titles"]) | set(untested_titles)
    else:
        scope = set(old["titles"])
    rows: dict[str, list[dict[str, Any]]] = {"down": [], "up": [], "changed": [], "not_compared": [],
                                             "no_verdict": []}
    same = skipped = 0
    for key in sorted(scope):
        before, after = old["titles"].get(key), new["titles"].get(key)
        if before is None:
            # The old run has no place for it: nothing to judge against.
            rows["not_compared"].append({"source_key": key, "disc_id": (after or {}).get("disc_id"),
                                         "title": (after or {}).get("title"), "old": None,
                                         "new": after, "reason": "not in the old run"})
            continue
        if after is None:
            if partial:
                skipped += 1
                continue
            untested = untested_titles.get(key)
            row = _row(key, before, None, untested)
            row["reason"] = untested["outcome"] if untested else "missing"
            rows["no_verdict"].append(row)
            continue
        row = _row(key, before, after)
        if after["end"] in NO_VERDICT_ENDS:
            row["reason"] = after.get("stop") or after["end"]
            rows["no_verdict"].append(row)
            continue
        if before["end"] in NO_VERDICT_ENDS:
            row["reason"] = f"old run: {before.get('stop') or before['end']}"
            rows["not_compared"].append(row)
            continue
        old_score, new_score = _score(before, ceiling, like), _score(after, ceiling, like)
        if new_score < old_score:
            rows["down"].append(row)
        elif new_score > old_score:
            rows["up"].append(row)
        elif like and (before["end"] != after["end"] or not same_stop(before["stop"], after["stop"])):
            rows["changed"].append(row)
        else:
            same += 1
    compared = len(rows["down"]) + len(rows["up"]) + len(rows["changed"]) + same
    if rows["down"]:
        verdict = "down"
    elif rows["no_verdict"] or compared == 0:
        verdict = "no_verdict"
    else:
        verdict = "pass"
    return {
        "verdict": verdict,
        "like_for_like": like,
        "compared_up_to": RUNGS[ceiling],
        "old": {"mode": old["mode"], "commits": old["commits"], "recorded_at": old["recorded_at"]},
        "new": {"mode": new["mode"], "commits": new["commits"], "recorded_at": new["recorded_at"]},
        "counts": {"down": len(rows["down"]), "up": len(rows["up"]),
                   "changed": len(rows["changed"]), "same": same,
                   "no_verdict": len(rows["no_verdict"]), "not_compared": len(rows["not_compared"]),
                   "skipped": skipped,
                   "only_new": len(set(new["titles"]) - set(old["titles"]))},
        **rows,
    }


def exit_code(result: dict[str, Any]) -> int:
    """1 when a title went down, 3 when the comparison has no verdict, 0 only when it passed."""
    return {"down": EXIT_DOWN, "no_verdict": EXIT_NO_VERDICT}.get(result["verdict"], 0)


_COMMIT = re.compile(r"[0-9a-fA-F]{7,64}")


def commit_ranges(result: dict[str, Any], repo: Path,
                  run: Callable[..., subprocess.CompletedProcess] = subprocess.run) -> dict[str, Any]:
    """The first-parent commits between each old and new commit of a title that went down."""
    ranges: dict[str, Any] = {}
    for row in result["down"]:
        old_commit, new_commit = row["old"]["commit"], row["new"]["commit"]
        if not old_commit or not new_commit:
            continue
        span = f"{old_commit}..{new_commit}"
        if span in ranges:
            continue
        if not all(_COMMIT.fullmatch(commit) for commit in (old_commit, new_commit)):
            ranges[span] = {"error": "not a commit hash"}
            continue
        completed = run(["git", "-C", str(repo), "log", "--first-parent", "--format=%h %s", span, "--"],
                        capture_output=True, text=True, encoding="utf-8", check=False)
        ranges[span] = (completed.stdout.splitlines() if completed.returncode == 0
                        else {"error": completed.stderr.strip()[:300]})
    return ranges


def _place_text(place: dict[str, Any] | None) -> str:
    if place is None:
        return "no result"
    text = f"{place['rung']}/{place['end']}"
    if place.get("stop"):
        text += f" {place['stop']}"
    return text


def _short(commit: str | None) -> str:
    return commit[:12] if commit else "?"


def render_comparison(result: dict[str, Any]) -> str:
    counts = result["counts"]
    lines = [
        f"old {result['old']['mode']} {', '.join(_short(c) for c in result['old']['commits']) or '?'}"
        f" -> new {result['new']['mode']} {', '.join(_short(c) for c in result['new']['commits']) or '?'}",
        f"verdict {result['verdict'].upper()}; compared up to {result['compared_up_to']}"
        f"{'' if result['like_for_like'] else ' (rung only: launch timeout or frame threshold differ)'}",
        "down {down}  up {up}  changed {changed}  same {same}  no verdict {no_verdict}"
        "  not compared {not_compared}  skipped {skipped}  only new {only_new}".format(**counts),
    ]
    for kind, label in (("down", "DOWN"), ("no_verdict", "NO-VERDICT"), ("not_compared", "NOT-COMPARED"),
                        ("up", "UP"), ("changed", "CHANGED")):
        for row in result[kind]:
            old_commit = (row["old"] or {}).get("commit")
            new_commit = (row["new"] or {}).get("commit")
            line = (f"{label:7} {row['disc_id'] or '?':10} {row['title'] or row['source_key']}: "
                    f"{_place_text(row['old'])} -> {_place_text(row['new'])} "
                    f"[{_short(old_commit)}..{_short(new_commit)}]")
            if row.get("reason"):
                line += f" ({row['reason']})"
            lines.append(line)
    for span, commits in (result.get("commit_ranges") or {}).items():
        lines.append(f"range {span}:")
        if isinstance(commits, dict):
            lines.append(f"  {commits['error']}")
        else:
            lines.extend(f"  {line}" for line in commits)
    return "\n".join(lines) + "\n"


def history(store: Path, needle: str) -> list[dict[str, Any]]:
    """Every stored snapshot's place for the titles whose disc ID or source key match."""
    folded = needle.casefold()
    rows = []
    for path in sorted((store / "snapshots").glob("*.json")):
        snapshot = _read_json(path)
        for key, place in sorted(snapshot.get("titles", {}).items()):
            if folded in (key, str(place.get("disc_id") or "").casefold()):
                rows.append({"snapshot": path.name, "mode": snapshot.get("mode"), "source_key": key,
                             **{field: place.get(field) for field in
                                ("rung", "end", "stop", "frames", "commit")}})
    return rows


# -- fast mode -------------------------------------------------------------------------------

def _rotation_key(source_key: str) -> str:
    return hashlib.sha256(source_key.encode("utf-8")).hexdigest()


def select_fast(baseline: dict[str, Any], *, budget_seconds: int, jobs: int,
                cursor: int = 0) -> dict[str, Any]:
    """The next titles of the rotation that fit ``jobs`` lanes of ``budget_seconds``.

    The rotation is every title that reached a frame in the baseline, in a fixed hashed order,
    so each run continues where the last one stopped and every title comes round in turn. A
    title that alone takes longer than the budget is passed over and named."""
    candidates = sorted(
        (key for key, place in baseline["titles"].items()
         if RUNGS.index(place["rung"]) >= RUNGS.index("first_frame")),
        key=_rotation_key)
    known = [place["wall_seconds"] for place in baseline["titles"].values()
             if isinstance(place.get("wall_seconds"), (int, float)) and place["wall_seconds"] > 0]
    default_cost = sorted(known)[len(known) // 2] if known else DEFAULT_TITLE_COST_SECONDS
    lanes = [0.0] * max(1, jobs)
    selected: list[str] = []
    too_slow: list[str] = []
    walked = 0
    start = cursor % len(candidates) if candidates else 0
    for offset in range(len(candidates)):
        key = candidates[(start + offset) % len(candidates)]
        wall = baseline["titles"][key].get("wall_seconds")
        cost = float(wall) if isinstance(wall, (int, float)) and wall > 0 else float(default_cost)
        if cost > budget_seconds:
            too_slow.append(key)
            walked += 1
            continue
        lane = lanes.index(min(lanes))
        if lanes[lane] + cost > budget_seconds:
            break
        lanes[lane] += cost
        selected.append(key)
        walked += 1
    return {"selected": selected, "too_slow": too_slow, "candidates": len(candidates),
            "cursor": start, "next_cursor": (start + walked) % len(candidates) if candidates else 0,
            "estimated_seconds": round(max(lanes))}


def _lock_count(slot_dir: Path) -> int:
    return len(list(slot_dir.glob("*.lock")))


def acquire_slot(slot_dir: Path, name: str, max_slots: int, *, quiet_globs: Iterable[str] = (),
                 sleep: Callable[[float], None] = time.sleep, log: Callable[[str], None] = print,
                 max_wait_seconds: float | None = None, poll_seconds: float = 30.0) -> Path | None:
    """Hold one title slot: a lock file of our own, created only while fewer than ``max_slots``
    lock files exist and none matches ``quiet_globs`` (a timing run that wants the machine)."""
    slot_dir.mkdir(parents=True, exist_ok=True)
    lock = slot_dir / name
    waited = 0.0
    told = False
    while max_wait_seconds is None or waited <= max_wait_seconds:
        quiet = next((path.name for pattern in quiet_globs for path in slot_dir.glob(pattern)), None)
        if quiet is None and _lock_count(slot_dir) < max_slots:
            try:
                descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            except FileExistsError:
                descriptor = None
            if descriptor is not None:
                os.write(descriptor, f"library-regress pid={os.getpid()} start={_utc_now()}\n".encode())
                os.close(descriptor)
                if _lock_count(slot_dir) <= max_slots:
                    return lock
                lock.unlink(missing_ok=True)  # another job took the last slot at the same moment
        if not told:
            log(f"waiting for a title slot ({quiet + ' held' if quiet else f'{_lock_count(slot_dir)} held'})")
            told = True
        sleep(poll_seconds)
        waited += poll_seconds
    return None


def _fast_state_path(store: Path) -> Path:
    return store / "fast-state.json"


def run_fast(args: argparse.Namespace, *,
             run: Callable[..., subprocess.CompletedProcess] = subprocess.run,
             log: Callable[[str], None] = print) -> int:
    store: Path = args.store
    baseline = load_snapshot(args.baseline)
    state_path = _fast_state_path(store)
    state = _read_json(state_path) if state_path.is_file() else {"cursor": 0, "runs": 0}
    cursor = args.cursor if args.cursor is not None else int(state.get("cursor", 0))
    choice = select_fast(baseline, budget_seconds=args.budget_minutes * 60, jobs=args.jobs,
                         cursor=cursor)
    log(f"fast: {len(choice['selected'])} of {choice['candidates']} first-frame titles from cursor "
        f"{choice['cursor']}, about {choice['estimated_seconds'] // 60} min on {args.jobs} jobs"
        + (f"; {len(choice['too_slow'])} over the budget alone, passed over" if choice["too_slow"] else ""))
    if not choice["selected"]:
        log("fast: nothing to run")
        return EXIT_NO_VERDICT
    run_id = f"{_utc_now().replace(':', '').replace('-', '')}-fast"
    run_dir = store / "runs" / run_id
    run_dir.mkdir(parents=True)
    only = run_dir / "only.txt"
    only.write_text("".join(f"{key}\n" for key in choice["selected"]), encoding="utf-8")
    _write_json(run_dir / "selection.json", choice)
    if args.dry_run:
        log(f"fast: dry run; the list is {only}")
        return 0
    scratch = args.scratch_root / run_id
    lock = None
    if args.slot_dir is not None:
        lock = acquire_slot(args.slot_dir, args.slot_name, args.max_slots, quiet_globs=args.quiet_lock,
                            log=log)
        if lock is None:
            log("fast: no title slot")
            return EXIT_NO_VERDICT
    try:
        command = [sys.executable, str(LIBRARY_BUILD), str(args.disc_root),
                   "--user-data-root", str(scratch / "data"), "--work-dir", str(scratch / "work"),
                   "--manifest-dir", str(run_dir), "--jobs", str(args.jobs), "--only", str(only)]
        if baseline["ladder"].get("launch_timeout_seconds"):
            command += ["--launch-timeout", str(baseline["ladder"]["launch_timeout_seconds"])]
        completed = run(command, check=False)
    finally:
        if lock is not None:
            lock.unlink(missing_ok=True)
    manifest_path = run_dir / "library-build.json"
    if completed.returncode != 0 or not manifest_path.is_file():
        log(f"fast: library_build.py ended {completed.returncode}; no verdict")
        return EXIT_NO_VERDICT
    snapshot = snapshot_from_manifest(_read_json(manifest_path), mode="fast",
                                      frames_threshold=baseline["ladder"]["frames_threshold"])
    path = record(snapshot, store)
    result = compare(baseline, snapshot, keys=choice["selected"])  # every selected title needs a verdict
    if args.repo is not None:
        result["commit_ranges"] = commit_ranges(result, args.repo, run=run)
    _write_json(run_dir / "comparison.json", result)
    sys.stdout.write(render_comparison(result))
    state.update(cursor=choice["next_cursor"], runs=int(state.get("runs", 0)) + 1, last_run=run_id,
                 last_snapshot=path.name)
    _write_json(state_path, state)
    if not args.keep_scratch:
        shutil.rmtree(scratch, ignore_errors=True)
    return exit_code(result)


# -- command line ----------------------------------------------------------------------------

def _snapshot_options(args: argparse.Namespace) -> dict[str, Any]:
    return {"frames_threshold": args.frames, "commit": args.commit,
            "launch_timeout": args.launch_timeout}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    def manifest_options(command: argparse.ArgumentParser) -> None:
        command.add_argument("--frames", type=int, default=DEFAULT_FRAMES,
                             help="Frame submissions that make the 'frames' rung (default 60)")
        command.add_argument("--commit", default=None,
                             help="Commit for a manifest that records none (an older run)")
        command.add_argument("--launch-timeout", type=int, default=None,
                             help="Launch timeout for a manifest that records none (an older run)")

    record_command = commands.add_parser("record", help="Keep a run's ladder snapshot")
    record_command.add_argument("manifest", type=Path)
    record_command.add_argument("--store", type=Path, required=True)
    record_command.add_argument("--mode", choices=("full", "fast"), default="full")
    manifest_options(record_command)

    compare_command = commands.add_parser("compare", help="Titles that went down or up between two runs")
    compare_command.add_argument("old", type=Path, help="Snapshot or library-build.json")
    compare_command.add_argument("new", type=Path, help="Snapshot or library-build.json")
    compare_command.add_argument("--repo", type=Path, default=None,
                                 help="A checkout: list the first-parent commits of each range that went down")
    compare_command.add_argument("--json", type=Path, default=None, help="Also write the result here")
    compare_command.add_argument("--partial", action="store_true",
                                 help="Judge only the titles the new run ended (a run still in progress)")
    compare_command.add_argument("--old-commit", default=None, help="Commit for an old manifest with none")
    compare_command.add_argument("--new-commit", default=None, help="Commit for a new manifest with none")
    compare_command.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    compare_command.add_argument("--launch-timeout", type=int, default=None,
                                 help="Launch timeout for a manifest that records none")

    history_command = commands.add_parser("history", help="One title's place in every stored run")
    history_command.add_argument("title", help="Disc ID or source key")
    history_command.add_argument("--store", type=Path, required=True)

    fast_command = commands.add_parser("fast", help="Boot to first frame on a rotating subset")
    fast_command.add_argument("--store", type=Path, required=True)
    fast_command.add_argument("--baseline", type=Path, required=True, help="Full snapshot to compare with")
    fast_command.add_argument("--disc-root", type=Path, required=True)
    fast_command.add_argument("--scratch-root", type=Path, required=True,
                              help="Folder for each fast run's data and work folders (removed after)")
    fast_command.add_argument("--budget-minutes", type=int, default=DEFAULT_BUDGET_MINUTES)
    fast_command.add_argument("--jobs", type=int, default=2)
    fast_command.add_argument("--cursor", type=int, default=None, help="Start of the rotation (default: stored)")
    fast_command.add_argument("--slot-dir", type=Path, default=None, help="Folder of title-slot lock files")
    fast_command.add_argument("--slot-name", default="library-regress.lock")
    fast_command.add_argument("--max-slots", type=int, default=3)
    fast_command.add_argument("--quiet-lock", action="append", default=[],
                              help="Lock-file pattern in --slot-dir that holds the run (repeatable)")
    fast_command.add_argument("--repo", type=Path, default=None)
    fast_command.add_argument("--keep-scratch", action="store_true")
    fast_command.add_argument("--dry-run", action="store_true", help="Choose and list the subset only")

    args = parser.parse_args(argv)
    if getattr(args, "frames", 1) < 1:
        parser.error("--frames must be positive")
    if args.command == "record":
        snapshot = snapshot_from_manifest(_read_json(args.manifest), mode=args.mode, **_snapshot_options(args))
        path = record(snapshot, args.store)
        print(json.dumps({"snapshot": str(path), "titles": len(snapshot["titles"]),
                          "untested": snapshot["untested"], "commits": snapshot["commits"]}))
        return 0
    if args.command == "compare":
        common = {"frames_threshold": args.frames, "launch_timeout": args.launch_timeout}
        old = load_snapshot(args.old, commit=args.old_commit, **common)
        new = load_snapshot(args.new, commit=args.new_commit, **common)
        result = compare(old, new, partial=args.partial)
        if args.repo is not None:
            result["commit_ranges"] = commit_ranges(result, args.repo)
        if args.json is not None:
            _write_json(args.json, result)
        sys.stdout.write(render_comparison(result))
        return exit_code(result)
    if args.command == "history":
        for row in history(args.store, args.title):
            print(json.dumps(row, ensure_ascii=False))
        return 0
    if args.jobs < 1 or args.budget_minutes < 1 or args.max_slots < 1:
        parser.error("--jobs, --budget-minutes and --max-slots must be positive")
    return run_fast(args)


if __name__ == "__main__":
    raise SystemExit(main())
