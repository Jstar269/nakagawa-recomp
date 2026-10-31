# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Build every PSP disc image in a folder into one player data folder.

    python tools/library_build.py <disc folder> --user-data-root <data folder>
        --work-dir <work folder> [--manifest-dir <folder>] [--jobs N]

Each disc first gets the player's disc check (its encrypted executable and modules decrypted
from the disc into the title's folder, ``nk_core.decrypt_tool.decrypt_disc``), then runs the
production bring-up (``nk_cli.py bringup --user-data-root``): identify, import, analyse,
generate, compile and package into the shared data folder, then a short hidden launch that names
the first failure. The player opened with ``--user-data-root`` on
the same folder lists every title that built.

The run never stops on one title: each disc ends with a record (built, or the exact failure)
in ``library-build.json`` and ``library-build.md``, written after every title to the data
folder and to ``--manifest-dir``. A stopped run resumes where it left off. Only ``.iso``
files are discs; other files are counted by kind and skipped.

Discs are keyed by disc ID plus the hash of the whole image (``nk_core.disc_verify``): a
file with the disc ID and the same SHA-1 as one seen before is recorded as a duplicate of the
first, while a fan translation or other modified image with a retail disc's ID is built as a
disc of its own. Only discs that share a disc ID are hashed, unless ``--redump-dat`` names the
user's Redump PSP datafile: then every disc is hashed and given its verification tier
(verified retail, modified or unknown), the list is imported into the data folder for the
player, and ``--verify-only`` hashes and classifies without building. Hashes are kept in the
manifest and reused while a file's size and modification time stay the same.

Builds run below normal priority, at most ``--jobs`` at a time, with no window. A ``PAUSE``
file in the work folder holds new titles until it goes; a ``STOP`` file ends the run after
the titles already started. The manifest records the run's settings (time budget, launch
timeout, source commit), so runs are compared like for like.

The manifest names the user's own discs and files: it is a private record, never published.
``tools/compat_table.py`` turns it into the public summary, which holds counts only.
"""

from __future__ import annotations

import argparse
from collections import Counter
import concurrent.futures
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from typing import Any, Callable

from library_sweep import _read_bringup_progress, _terminate_process_tree
from nk_core import decrypt_tool, disc_verify, inspect_iso

ROOT = Path(__file__).resolve().parents[1]
NK_CLI = ROOT / "tools" / "nk_cli.py"
MANIFEST_FORMAT = "nakagawa-library-build"
MANIFEST_SCHEMA_VERSION = 1
MANIFEST_NAME = "library-build"
DISC_SUFFIXES = frozenset({".iso"})
STAGES = ("inspect", "prepare_import", "analyze", "codegen", "compile", "build_package", "launch")
# A title that has ended; RUNNING and PENDING titles run again on resume.
FINAL_OUTCOMES = frozenset({"BUILT", "BUILD_FAILED", "TIMED_OUT", "NO_REPORT", "DUPLICATE"})
RETRY_OUTCOMES = frozenset({"BUILD_FAILED", "TIMED_OUT", "NO_REPORT"})
DEFAULT_TITLE_TIMEOUT_SECONDS = 2400
DEFAULT_LAUNCH_TIMEOUT_SECONDS = 30
PAUSE_POLL_SECONDS = 30.0
_BELOW_NORMAL_PRIORITY_CLASS = 0x00004000
_IDLE_PRIORITY_CLASS = 0x00000040
_CREATE_NO_WINDOW = 0x08000000


def default_jobs(cpu_count: int | None) -> int:
    """Titles built at once: one per eight logical CPUs, from one to four.

    Each package build already runs several compile jobs, sized by the free memory when it
    starts, so a few titles at a time keep the PC usable for everything else.
    """
    cpus = cpu_count if cpu_count and cpu_count > 0 else 1
    return max(1, min(4, cpus // 8))


def _utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _source_key(path: Path, disc_root: Path) -> str:
    return path.relative_to(disc_root).as_posix().casefold()


def discover(disc_root: Path) -> tuple[list[Path], Counter]:
    """The disc images under `disc_root` in path order, and the other files by suffix."""
    discs: list[Path] = []
    skipped: Counter = Counter()
    for path in disc_root.rglob("*"):
        if not path.is_file():
            continue
        suffix = path.suffix.casefold()
        if suffix in DISC_SUFFIXES:
            discs.append(path)
        else:
            skipped[suffix or "(none)"] += 1
    discs.sort(key=lambda path: _source_key(path, disc_root))
    return discs, skipped


def identify(path: Path) -> dict[str, Any]:
    """The disc ID, title and version from the disc's own PARAM.SFO."""
    metadata = inspect_iso(path)
    return {
        "disc_id": metadata.disc_id.upper(),
        "title": metadata.title,
        "disc_version": metadata.version,
    }


def source_commit(root: Path = ROOT) -> str | None:
    """The commit the tools run from, recorded with each title's settings; None outside git."""
    try:
        result = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                                capture_output=True, text=True, timeout=30, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    commit = result.stdout.strip()
    return commit if result.returncode == 0 and len(commit) == 40 else None


def record_digest(record: dict[str, Any]) -> disc_verify.DiscDigest | None:
    """The whole-image hashes a record holds, when they are for the file as it is now."""
    verification = record.get("verification")
    if not isinstance(verification, dict):
        return None
    sha1, crc32, size = (verification.get("sha1"), verification.get("crc32"),
                         verification.get("size_bytes"))
    if (not isinstance(sha1, str) or not isinstance(crc32, str) or type(size) is not int
            or size != record.get("size_bytes")):
        return None
    return disc_verify.DiscDigest(sha1=sha1, crc32=crc32, size_bytes=size)


def content_key(record: dict[str, Any]) -> tuple[str, str | None]:
    """What makes two files the same disc: the disc ID and, once hashed, the image's SHA-1."""
    digest = record_digest(record)
    return (record["disc_id"], digest.sha1 if digest else None)


class DiscDecryption:
    """The player's disc check: the project's own engine, built once per run."""

    def __init__(self) -> None:
        self.tool: Path | None = None

    def __call__(self, iso: Path, user_data_root: Path, disc_id: str) -> list[dict]:
        if self.tool is None:
            self.tool = decrypt_tool.find_or_build_tool(ROOT)
        return decrypt_tool.decrypt_disc(iso, user_data_root, disc_id, self.tool)


def bringup_command(iso: Path, user_data_root: Path, work_dir: Path,
                    launch_timeout_seconds: int) -> list[str]:
    return [
        sys.executable, str(NK_CLI), "bringup", str(iso),
        "--user-data-root", str(user_data_root),
        "--work-dir", str(work_dir),
        "--report", str(work_dir / "bringup.json"),
        "--sweep-progress-report", str(work_dir / "bringup-progress.json"),
        "--launch-timeout", str(launch_timeout_seconds),
    ]


def _creation_flags(priority: str) -> int:
    if os.name != "nt":
        return 0
    flags = _CREATE_NO_WINDOW | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    if priority == "below":
        flags |= _BELOW_NORMAL_PRIORITY_CLASS
    elif priority == "idle":
        flags |= _IDLE_PRIORITY_CLASS
    return flags


def _preexec(priority: str) -> Callable[[], None] | None:
    if os.name == "nt" or priority == "normal":
        return None
    increment = 10 if priority == "below" else 19
    return lambda: os.nice(increment)


def _directory_bytes(path: Path) -> int | None:
    if not path.is_dir():
        return None
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def summarize_report(report: dict) -> dict[str, Any]:
    """The manifest's view of one bring-up report."""
    stages = report.get("stages") if isinstance(report.get("stages"), dict) else {}
    stage_rows = {
        name: {"status": detail.get("status"), "duration_ms": detail.get("duration_ms")}
        for name, detail in stages.items() if isinstance(detail, dict)
    }
    failed_stage = next(
        (name for name in STAGES
         if stage_rows.get(name, {}).get("status") in {"FAIL", "TIMED_OUT"}),
        None,
    )
    built = stage_rows.get("build_package", {}).get("status") == "PASS"
    counts = report.get("counts") if isinstance(report.get("counts"), dict) else {}
    imports = sorted({
        f"{row.get('library')}:{row.get('nid_name') or '?'}"
        for row in report.get("unsupported_imports", []) if isinstance(row, dict)
    })
    runtime_imports = sorted({
        json.dumps(row, sort_keys=True) if not isinstance(row, str) else row
        for row in report.get("runtime_imports", [])
    })
    presentation = report.get("presentation") if isinstance(report.get("presentation"), dict) else {}
    return {
        "outcome": "BUILT" if built else "BUILD_FAILED",
        "reached_stage": report.get("reached_stage"),
        "failed_stage": None if built else failed_stage,
        "failure_class": report.get("failure_class"),
        "stages": stage_rows,
        "decrypt": {
            "status": stage_rows.get("prepare_import", {}).get("status"),
            "modules": counts.get("modules"),
            "encrypted_modules": counts.get("encrypted_modules"),
        },
        "unsupported_imports": imports,
        "launch": {
            "status": stage_rows.get("launch", {}).get("status"),
            "exit_classification": report.get("exit_classification"),
            "runtime_output_kind": report.get("runtime_output_kind"),
            "process_exit_code": report.get("process_exit_code"),
            "presentation": presentation.get("status"),
            "frame_submissions": presentation.get("frame_submissions"),
            "runtime_imports": runtime_imports,
            "runtime_boundary": report.get("runtime_boundary"),
            "failure_class": report.get("failure_class") if built else None,
        },
    }


class LibraryBuild:
    """One batch over a disc folder; the manifest is its state."""

    def __init__(self, disc_root: Path, user_data_root: Path, work_root: Path,
                 manifest_dirs: list[Path], *, jobs: int, priority: str,
                 title_timeout_seconds: int, launch_timeout_seconds: int,
                 retry_failed: bool = False, limit: int | None = None,
                 identify_disc: Callable[[Path], dict[str, Any]] = identify,
                 decrypt: Callable[[Path, Path, str], list[dict]] | None = None,
                 command: Callable[[Path, Path, Path, int], list[str]] = bringup_command,
                 log: Callable[[str], None] = print,
                 pause_poll_seconds: float = PAUSE_POLL_SECONDS,
                 dat: disc_verify.RedumpDat | None = None, verify_only: bool = False,
                 hash_disc: Callable[[Path], disc_verify.DiscDigest] = disc_verify.hash_image,
                 commit: str | None = None) -> None:
        self.disc_root = disc_root.resolve(strict=True)
        self.user_data_root = user_data_root.resolve()
        self.work_root = work_root.resolve()
        self.manifest_dirs = [path.resolve() for path in manifest_dirs]
        self.jobs = max(1, jobs)
        self.priority = priority
        self.title_timeout_seconds = title_timeout_seconds
        self.launch_timeout_seconds = launch_timeout_seconds
        self.retry_failed = retry_failed
        self.limit = limit
        self.identify_disc = identify_disc
        self.decrypt = decrypt
        self.command = command
        self.log = log
        self.pause_poll_seconds = pause_poll_seconds
        self.dat = dat
        self.verify_only = verify_only
        self.hash_disc = hash_disc
        self.commit = commit
        self.lock = threading.Lock()
        self.records: dict[str, dict[str, Any]] = {}
        self.skipped: Counter = Counter()
        self.started_at = _utc_now()
        self.datafile: dict[str, Any] | None = None

    # -- state ---------------------------------------------------------------------------

    def _load_previous(self) -> None:
        for directory in [self.user_data_root, *self.manifest_dirs]:
            payload = _read_json(directory / f"{MANIFEST_NAME}.json")
            if (isinstance(payload, dict) and payload.get("format") == MANIFEST_FORMAT
                    and payload.get("schema_version") == MANIFEST_SCHEMA_VERSION
                    and isinstance(payload.get("titles"), list)):
                for record in payload["titles"]:
                    if isinstance(record, dict) and isinstance(record.get("source_key"), str):
                        self.records[record["source_key"]] = record
                if isinstance(payload.get("started_at"), str):
                    self.started_at = payload["started_at"]
                if isinstance(payload.get("datafile"), dict):
                    self.datafile = payload["datafile"]
                return

    def _manifest(self) -> dict[str, Any]:
        titles = sorted(self.records.values(), key=lambda record: record["source_key"])
        return {
            "format": MANIFEST_FORMAT,
            "schema_version": MANIFEST_SCHEMA_VERSION,
            "started_at": self.started_at,
            "updated_at": _utc_now(),
            "disc_root": str(self.disc_root),
            "user_data_root": str(self.user_data_root),
            "skipped_files": dict(sorted(self.skipped.items())),
            "settings": {
                "title_timeout_seconds": self.title_timeout_seconds,
                "launch_timeout_seconds": self.launch_timeout_seconds,
                "jobs": self.jobs,
                "priority": self.priority,
                "decrypt": self.decrypt is not None,
                "source_commit": self.commit,
            },
            "datafile": self.datafile,
            "counts": dict(sorted(Counter(record["outcome"] for record in titles).items())),
            "verification_counts": dict(sorted(Counter(
                (record.get("verification") or {}).get("tier") or disc_verify.NOT_CHECKED
                for record in titles).items())),
            "titles": titles,
        }

    def write_manifest(self) -> None:
        """Both files in every manifest folder, each replaced whole (workers share one lock)."""
        with self.lock:
            manifest = self._manifest()
            text = json.dumps(manifest, indent=1, ensure_ascii=False) + "\n"
            markdown = render_markdown(manifest)
            for directory in [self.user_data_root, *self.manifest_dirs]:
                directory.mkdir(parents=True, exist_ok=True)
                for name, content in ((f"{MANIFEST_NAME}.json", text),
                                      (f"{MANIFEST_NAME}.md", markdown)):
                    temporary = directory / f".{name}.{os.getpid()}.tmp"
                    temporary.write_text(content, encoding="utf-8", newline="\n")
                    os.replace(temporary, directory / name)

    # -- planning ------------------------------------------------------------------------

    def plan(self) -> list[dict[str, Any]]:
        """Records for every disc, with the titles still to build in order."""
        self._load_previous()
        discs, self.skipped = discover(self.disc_root)
        present: list[dict[str, Any]] = []
        for path in discs:
            key = _source_key(path, self.disc_root)
            stat = path.stat()
            record = self.records.get(key)
            unchanged = (record is not None and record.get("size_bytes") == stat.st_size
                         and record.get("mtime_ns") == stat.st_mtime_ns)
            if not unchanged:
                record = {
                    "source_key": key,
                    "source": path.relative_to(self.disc_root).as_posix(),
                    "size_bytes": stat.st_size,
                    "mtime_ns": stat.st_mtime_ns,
                    "outcome": "PENDING",
                }
                try:
                    record.update(self.identify_disc(path))
                    record["identify_error"] = None
                    record.pop("identify_detail", None)
                except Exception as exc:  # the bring-up still runs and names its boundary
                    record.update({"disc_id": None, "title": None, "disc_version": None,
                                   "identify_error": getattr(exc, "boundary_code", None)
                                   or "DISC_IDENTIFY_FAILED",
                                   "identify_detail": f"{type(exc).__name__}: {exc}"[:300]})
                self.records[key] = record
            present.append(record)
        keys = {record["source_key"] for record in present}
        for key in list(self.records):
            if key not in keys:
                del self.records[key]  # the file is gone from the folder
        self.hash_discs(present)
        self.classify()
        first_by_content: dict[tuple[str, str | None], str] = {}
        for record in present:
            if record.get("outcome") == "BUILT" and record.get("disc_id"):
                first_by_content.setdefault(content_key(record), record["source_key"])
        todo: list[dict[str, Any]] = []
        for record in present:
            key = record["source_key"]
            content = content_key(record) if record.get("disc_id") else None
            first = first_by_content.get(content) if content else None
            if first is not None and first != key:
                record.update({"outcome": "DUPLICATE", "duplicate_of":
                               self.records[first]["source"]})
                continue
            if content:
                first_by_content[content] = key
            if record.get("outcome") == "DUPLICATE":
                record["outcome"] = "PENDING"
                record.pop("duplicate_of", None)
            outcome = record.get("outcome")
            if self.verify_only or (outcome in FINAL_OUTCOMES
                                    and not (self.retry_failed and outcome in RETRY_OUTCOMES)):
                continue
            record["outcome"] = "PENDING"
            todo.append(record)
        if self.limit is not None:
            todo = todo[:self.limit]
        return todo

    # -- verification --------------------------------------------------------------------

    def hash_discs(self, present: list[dict[str, Any]]) -> None:
        """Hash the discs that need it: all of them with a datafile, else those sharing a disc ID.

        A disc hashed before keeps its hashes while its file is unchanged. A file that cannot
        be read records its boundary code and is hashed again on the next run.
        """
        shared = Counter(record.get("disc_id") for record in present if record.get("disc_id"))
        wanted = [
            record for record in present
            if record_digest(record) is None and (
                self.dat is not None or self.verify_only
                or (record.get("disc_id") and shared[record["disc_id"]] > 1))
        ]
        if not wanted:
            return
        self.log(f"hashing {len(wanted)} discs")

        def hash_one(record: dict[str, Any]) -> None:
            if (self.work_root / "STOP").exists():
                return
            try:
                digest = self.hash_disc(self.disc_root / record["source"])
                verification: dict[str, Any] = digest.to_dict()
            except disc_verify.DiscVerifyError as exc:
                verification = {"error": exc.boundary_code}
            with self.lock:
                record["verification"] = verification

        with concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs) as pool:
            for done, _ in enumerate(pool.map(hash_one, wanted), start=1):
                if done % 25 == 0 or done == len(wanted):
                    self.log(f"hashed {done} of {len(wanted)} discs")
                    self.write_manifest()

    def classify(self) -> None:
        """Give every record its tier against this run's datafile, for the whole set at once."""
        if self.dat is None:
            return
        self.datafile = {"name": self.dat.name, "version": self.dat.version,
                         "discs": len(self.dat), "serials": len(self.dat.serials),
                         "checked_at": _utc_now()}
        images = {key: (record.get("disc_id"), record_digest(record))
                  for key, record in self.records.items()}
        for key, tier in disc_verify.classify_all(images, self.dat).items():
            record = self.records[key]
            verification = record.get("verification")
            if not isinstance(verification, dict):
                verification = record["verification"] = {}
            verification["tier"] = tier
            digest = images[key][1]
            rom = self.dat.roms_by_sha1.get(digest.sha1) if digest else None
            if tier == disc_verify.VERIFIED_RETAIL and rom is not None:
                verification["redump_name"] = rom.name
            else:
                verification.pop("redump_name", None)

    # -- building ------------------------------------------------------------------------

    def _work_dir(self, record: dict[str, Any]) -> Path:
        name = record.get("disc_id") or "UNKNOWN"
        digest = hashlib.sha256(record["source_key"].encode("utf-8")).hexdigest()[:10]
        return self.work_root / f"{name}-{digest}"

    def _wait_while_paused(self) -> bool:
        """False once STOP asks the run to end; waits while PAUSE is present."""
        while True:
            if (self.work_root / "STOP").exists():
                return False
            if not (self.work_root / "PAUSE").exists():
                return True
            time.sleep(self.pause_poll_seconds)

    def build_one(self, record: dict[str, Any]) -> None:
        work_dir = self._work_dir(record)
        work_dir.mkdir(parents=True, exist_ok=True)
        for name in ("bringup.json", "bringup-progress.json"):
            (work_dir / name).unlink(missing_ok=True)
        with self.lock:
            record.update({"outcome": "RUNNING", "started_at": _utc_now(),
                           "work_dir": str(work_dir),
                           # This title's comparison identity: a later run compares like for like.
                           "run_settings": {
                               "title_timeout_seconds": self.title_timeout_seconds,
                               "launch_timeout_seconds": self.launch_timeout_seconds,
                               "source_commit": self.commit,
                           }})
        self.write_manifest()
        iso = self.disc_root / record["source"]
        started = time.monotonic()
        if self.decrypt is not None and record.get("disc_id"):
            # A failure here is recorded and the bring-up still runs: it names the boundary.
            try:
                rows = self.decrypt(iso, self.user_data_root, record["disc_id"])
                failed = [row for row in rows if row.get("status") != 0]
                decrypt_step = {
                    "files": len(rows), "failed": [row["name"] for row in failed],
                    "first_failure": failed[0].get("message") if failed else None,
                    "first_failure_code": (failed[0].get("code")
                                           or decrypt_tool.status_code(failed[0]["status"]))
                    if failed else None,
                }
            except Exception as exc:
                decrypt_step = {"files": None, "failed": None,
                                "first_failure": f"{type(exc).__name__}: {exc}"[:300],
                                "first_failure_code": "IO_FAILURE" if isinstance(exc, OSError)
                                else "INTERNAL_ERROR"}
            with self.lock:
                record["decrypt_step"] = decrypt_step
        command = self.command(iso, self.user_data_root, work_dir, self.launch_timeout_seconds)
        timed_out = False
        return_code: int | None = None
        try:
            with open(work_dir / "bringup-stdout.log", "wb") as output:
                process = subprocess.Popen(
                    command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                    stdin=subprocess.DEVNULL, creationflags=_creation_flags(self.priority),
                    preexec_fn=_preexec(self.priority),
                )
                try:
                    return_code = process.wait(timeout=self.title_timeout_seconds)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    _terminate_process_tree(process)
        except OSError as exc:
            result: dict[str, Any] = {"outcome": "NO_REPORT", "failure_class": None,
                                      "error": f"{type(exc).__name__}: {exc}"}
        else:
            report = _read_json(work_dir / "bringup.json")
            if timed_out:
                progress = _read_bringup_progress(work_dir / "bringup-progress.json") or {}
                running = [name for name, detail in progress.get("stages", {}).items()
                           if isinstance(detail, dict) and detail.get("status") == "RUNNING"]
                result = {"outcome": "TIMED_OUT", "failed_stage": running[0] if running else None,
                          "failure_class": "TITLE_TIME_BUDGET"}
            elif isinstance(report, dict):
                result = summarize_report(report)
            else:
                result = {"outcome": "NO_REPORT", "failure_class": None}
        disc_id = record.get("disc_id")
        package_bytes = (_directory_bytes(self.user_data_root / "packages" / disc_id)
                         if disc_id and result.get("outcome") == "BUILT" else None)
        with self.lock:
            for stale in ("failed_stage", "stages", "decrypt", "unsupported_imports", "launch",
                          "reached_stage", "error"):
                record.pop(stale, None)
            record.update(result)
            record.update({
                "exit_code": return_code,
                "wall_seconds": round(time.monotonic() - started, 1),
                "package_bytes": package_bytes,
                "finished_at": _utc_now(),
            })
        self.write_manifest()
        self.log(f"{record.get('disc_id') or record['source']}: {record['outcome']}"
                 f"{' ' + str(record.get('failure_class')) if record['outcome'] != 'BUILT' else ''}"
                 f" ({record['wall_seconds']} s)")

    def run(self) -> dict[str, Any]:
        todo = self.plan()
        self.write_manifest()
        self.log(f"{len(self.records)} discs, {len(todo)} to build, {self.jobs} at a time; "
                 f"{sum(self.skipped.values())} other files skipped")
        stopped = False
        with concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs) as pool:
            pending: set[concurrent.futures.Future] = set()
            queue = list(todo)
            while queue or pending:
                while queue and len(pending) < self.jobs and not stopped:
                    if not self._wait_while_paused():
                        stopped = True
                        break
                    pending.add(pool.submit(self.build_one, queue.pop(0)))
                if stopped:
                    queue.clear()
                if not pending:
                    break
                done, pending = concurrent.futures.wait(
                    pending, return_when=concurrent.futures.FIRST_COMPLETED)
                for future in done:
                    exception = future.exception()
                    if exception is not None:  # recorded per title; the batch goes on
                        self.log(f"title worker raised {type(exception).__name__}: {exception}")
        for record in self.records.values():
            if record.get("outcome") == "RUNNING":
                record["outcome"] = "PENDING"
        self.write_manifest()
        return self._manifest()


def _megabytes(value: int | None) -> str:
    return "" if value is None else f"{value / 2**20:.0f}"


def render_markdown(manifest: dict[str, Any]) -> str:
    titles = manifest["titles"]
    counts = manifest["counts"]
    lines = [
        "# Library build (private: names the user's own discs)",
        "",
        f"Discs: {len(titles)}. Started {manifest['started_at']}, updated {manifest['updated_at']}.",
        f"Data folder: `{manifest['user_data_root']}`.",
        "",
        "## Outcomes",
        "",
        "| Outcome | Discs |",
        "| --- | --- |",
    ]
    lines += [f"| {outcome} | {count} |" for outcome, count in counts.items()]
    skipped = manifest.get("skipped_files") or {}
    if skipped:
        lines += ["", "Other files skipped: "
                  + ", ".join(f"{suffix} {count}" for suffix, count in skipped.items()) + "."]
    verification = manifest.get("verification_counts") or {}
    if set(verification) - {disc_verify.NOT_CHECKED}:
        datafile = manifest.get("datafile") or {}
        lines += ["", "## Disc verification", "",
                  f"Against {datafile.get('name') or 'the datafile'} "
                  f"{datafile.get('version') or ''} ({datafile.get('discs', '?')} discs).",
                  "", "| Tier | Discs |", "| --- | --- |"]
        lines += [f"| {tier} | {verification.get(tier, 0)} |" for tier in disc_verify.TIERS]
    failures = Counter(
        (record.get("failed_stage") or "?", record.get("failure_class") or "?")
        for record in titles if record["outcome"] in RETRY_OUTCOMES
    )
    if failures:
        lines += ["", "## Build failures by cause", "", "| Stage | Failure | Discs |",
                  "| --- | --- | --- |"]
        lines += [f"| {stage} | {failure} | {count} |"
                  for (stage, failure), count in failures.most_common()]
    launches = Counter(
        " / ".join(str(part) for part in (
            (record.get("launch") or {}).get("exit_classification"),
            (record.get("launch") or {}).get("runtime_output_kind"),
            (record.get("launch") or {}).get("presentation"),
        ))
        for record in titles if record["outcome"] == "BUILT"
    )
    if launches:
        lines += ["", "## First launch of built titles", "",
                  "| Exit / runtime output / presentation | Titles |", "| --- | --- |"]
        lines += [f"| {key} | {count} |" for key, count in launches.most_common()]
    stops = Counter(
        ((record.get("launch") or {}).get("failure_class"),
         (record.get("launch") or {}).get("runtime_boundary"))
        for record in titles
        if record["outcome"] == "BUILT" and (record.get("launch") or {}).get("runtime_boundary")
    )
    if stops:
        lines += ["", "## Where first launches stopped", "", "| Failure | Runtime stop | Titles |",
                  "| --- | --- | --- |"]
        lines += [f"| {failure} | `{stop}` | {count} |" for (failure, stop), count in stops.most_common()]
    imports = Counter(name for record in titles for name in record.get("unsupported_imports") or [])
    if imports:
        lines += ["", "## Unsupported imports found at build (top 40)", "",
                  "| Import | Titles |", "| --- | --- |"]
        lines += [f"| `{name}` | {count} |" for name, count in imports.most_common(40)]
    lines += ["", "## Titles", "",
              "| Disc | Title | Outcome | Stage | Failure | Build s | Package MB | Launch | Disc check |",
              "| --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for record in titles:
        launch = record.get("launch") or {}
        title = str(record.get("title") or record["source"]).replace("|", "/")
        failure = record.get("failure_class") if record["outcome"] != "BUILT" else ""
        if record["outcome"] == "DUPLICATE":
            failure = f"same disc as {record.get('duplicate_of')}".replace("|", "/")
        launch_text = (f"{launch.get('exit_classification')} {launch.get('runtime_output_kind')}"
                       if record["outcome"] == "BUILT" else "")
        lines.append(
            f"| {record.get('disc_id') or ''} | {title} | {record['outcome']} | "
            f"{record.get('failed_stage') or ''} | {failure or ''} | "
            f"{record.get('wall_seconds', '')} | {_megabytes(record.get('package_bytes'))} | "
            f"{launch_text} | {(record.get('verification') or {}).get('tier') or ''} |"
        )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("disc_root", type=Path, help="Folder of disc images (searched recursively)")
    parser.add_argument("--user-data-root", required=True, type=Path,
                        help="Player data folder every title builds into")
    parser.add_argument("--work-dir", required=True, type=Path,
                        help="Folder for each title's bring-up files and logs, and PAUSE/STOP")
    parser.add_argument("--manifest-dir", type=Path, action="append", default=[],
                        help="Another folder that gets library-build.json and .md (repeatable)")
    parser.add_argument("--jobs", type=int, default=default_jobs(os.cpu_count()),
                        help="Titles built at once (default: one per eight logical CPUs, 1-4)")
    parser.add_argument("--priority", choices=("below", "idle", "normal"), default="below",
                        help="Process priority of the builds (default: below normal)")
    parser.add_argument("--title-timeout", type=int, default=DEFAULT_TITLE_TIMEOUT_SECONDS,
                        help="Seconds one title may take before it is stopped and recorded")
    parser.add_argument("--launch-timeout", type=int, default=DEFAULT_LAUNCH_TIMEOUT_SECONDS,
                        help="Seconds of the hidden first launch (nk_cli bringup --launch-timeout)")
    parser.add_argument("--retry-failed", action="store_true",
                        help="Build again the titles whose last attempt failed or timed out")
    parser.add_argument("--limit", type=int, default=None, help="Build at most this many titles")
    parser.add_argument("--no-decrypt", action="store_true",
                        help="Skip the disc check that decrypts each disc's executable and modules")
    parser.add_argument("--redump-dat", type=Path, default=None,
                        help="Your Redump PSP datafile: hash every disc and give it its "
                             "verification tier; the list is imported into the data folder")
    parser.add_argument("--verify-only", action="store_true",
                        help="Hash and classify the discs without building any")
    args = parser.parse_args(argv)
    if args.jobs < 1 or args.title_timeout < 1 or args.launch_timeout < 1:
        parser.error("--jobs, --title-timeout and --launch-timeout must be positive")
    dat = None
    if args.redump_dat is not None:
        try:
            dat = disc_verify.import_dat(args.redump_dat, args.user_data_root)
        except disc_verify.DiscVerifyError as exc:
            parser.error(f"{exc} [{exc.boundary_code}]")
    build = LibraryBuild(
        args.disc_root, args.user_data_root, args.work_dir, args.manifest_dir,
        jobs=args.jobs, priority=args.priority, title_timeout_seconds=args.title_timeout,
        launch_timeout_seconds=args.launch_timeout, retry_failed=args.retry_failed,
        limit=args.limit, decrypt=None if args.no_decrypt else DiscDecryption(),
        dat=dat, verify_only=args.verify_only, commit=source_commit(),
    )
    manifest = build.run()
    print(json.dumps({"counts": manifest["counts"],
                      "verification": manifest["verification_counts"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
