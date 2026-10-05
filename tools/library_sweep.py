# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Run the production ISO bring-up route across a library and aggregate blockers."""

from __future__ import annotations

import argparse
from collections import Counter
import csv
from dataclasses import dataclass
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

from nk_core import inspect_iso, package_cache
from nk_core.decrypt_boundary import key_file_path
from nk_core.prereq_fetcher import PrerequisiteFetchError, default_data_root


ROOT = Path(__file__).resolve().parents[1]
NK_CLI = ROOT / "tools" / "nk_cli.py"
DEFAULT_TIME_BUDGET_SECONDS = 120
MACHINE_POLL_SECONDS = 60
MACHINE_MAX_WAIT_SECONDS = 20 * 60
# External per-title reports carry variable-size import inventories. The
# project-artifact budgets accept the tested 20,000-entry reports while keeping
# one route's parser work bounded independently of build-report.json.
_SWEEP_TITLE_JSON_MAX_BYTES = 32 * 1024 * 1024
_SWEEP_TITLE_JSON_MAX_DEPTH = 32
_SWEEP_TITLE_JSON_MAX_MEMBERS = 262144
_SWEEP_TITLE_JSON_MAX_ITEMS = 262144
_SWEEP_TITLE_JSON_MAX_NODES = 1048576
# Resume reports have one row per discovered ISO and no input-count ceiling.
# The limits admit the tested 10,000 rich-row checkpoint and larger sweeps; the
# writer applies the same limits before replacing files.
_SWEEP_RESUME_JSON_MAX_BYTES = 64 * 1024 * 1024
_SWEEP_RESUME_JSON_MAX_DEPTH = 32
_SWEEP_RESUME_JSON_MAX_MEMBERS = 1048576
_SWEEP_RESUME_JSON_MAX_ITEMS = 1048576
_SWEEP_RESUME_JSON_MAX_NODES = 4194304
STAGES = (
    "identify",
    "decrypt",
    "analyze",
    "codegen",
    "compile",
    "launch",
    "first presented frame",
    "input-responsive",
)
BLOCKING_PROCESS_NAMES = frozenset({"verify_flagship", "hst", "nakagawa_player"})

_PUBLIC_AGGREGATE_REQUIRED_KEYS = frozenset({
    "schema_version",
    "source_commit",
    "source_fingerprint",
    "time_budget_seconds",
    "coverage",
    "furthest_stage_histogram",
    "top_blocker_classes",
    "nid_families_by_title_count",
    "input_responsiveness",
})
_PUBLIC_AGGREGATE_OPTIONAL_KEYS = frozenset({"ratchet", "comparison"})
_PUBLIC_COVERAGE_REQUIRED_KEYS = frozenset({
    "iso_count",
    "completed_routes",
    "time_budget_exits",
    "route_errors",
})
_PUBLIC_COVERAGE_OPTIONAL_KEYS = frozenset({"recorded_routes"})
_PUBLIC_COMPARISON_KEYS = frozenset({
    "status",
    "reason",
    "stage_delta",
    "completed_routes_delta",
})
_PUBLIC_LIBRARY_FAMILY = re.compile(r"^[A-Za-z][A-Za-z0-9_.$-]{0,63}$")
_PUBLIC_COMPARISON_STATUSES = frozenset({"NO_BASELINE", "NEW_BASELINE", "MATCHED"})
_PUBLIC_COMPARISON_REASONS = frozenset({
    "no_previous_aggregate",
    "source_identity_changed",
    "time_budget_changed",
    "input_count_changed",
    "same_source_and_input_count",
})
_SWEEP_RUN_STATUSES = frozenset({"COMPLETED", "TIMED_OUT", "NO_REPORT"})
_PUBLIC_LIBRARY_FAMILY_SOURCE = ROOT / "tools" / "nid_corpus.json"
_PUBLIC_LIBRARY_MODULE_PREFIX = re.compile(r"^(?:_+)?(sce[A-Z][a-z0-9]*)")
_PUBLIC_BLOCKER_CODE_SOURCE = ROOT / "assets" / "bringup_report.schema.json"
_PUBLIC_SWEEP_BLOCKER_CODES = frozenset({
    "SWEEP_TIME_BUDGET",
    "SWEEP_ROUTE_FAILED",
    "SWEEP_REPORT_INVALID",
})


def _load_public_library_families() -> frozenset[str]:
    """Load the public library-family vocabulary from the checked-in NID corpus.

    Direct library attributions are accepted as-is. Public API names also contribute
    their PSP module prefix, using the same projection documented by hle_manifest.py.
    Values absent from this source-backed vocabulary are omitted from public output.
    """
    try:
        payload = json.loads(_PUBLIC_LIBRARY_FAMILY_SOURCE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError("public library-family vocabulary is unavailable") from exc
    entries = payload.get("entries") if isinstance(payload, dict) else None
    if not isinstance(entries, list):
        raise RuntimeError("public library-family vocabulary is malformed")
    families: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        library = entry.get("library")
        if isinstance(library, str) and _PUBLIC_LIBRARY_FAMILY.fullmatch(library):
            families.add(library)
        name = entry.get("name")
        if isinstance(name, str):
            match = _PUBLIC_LIBRARY_MODULE_PREFIX.match(name)
            if match:
                families.add(match.group(1))
    return frozenset(families)


_PUBLIC_LIBRARY_FAMILIES = _load_public_library_families()


def _load_public_blocker_codes() -> frozenset[str]:
    """Load report failure classes and the runner's public synthetic boundaries."""
    try:
        payload = json.loads(_PUBLIC_BLOCKER_CODE_SOURCE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError("public blocker-code vocabulary is unavailable") from exc
    failure_class = payload.get("properties", {}).get("failure_class")
    codes = failure_class.get("enum") if isinstance(failure_class, dict) else None
    if not isinstance(codes, list) or any(not isinstance(code, str) for code in codes):
        raise RuntimeError("public blocker-code vocabulary is malformed")
    return frozenset(codes) | _PUBLIC_SWEEP_BLOCKER_CODES


_PUBLIC_BLOCKER_CODES = _load_public_blocker_codes()


def _load_public_issue_numbers() -> frozenset[int]:
    """Load the non-networked issue allowlist from the public report schema."""
    try:
        payload = json.loads(_PUBLIC_BLOCKER_CODE_SOURCE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError("public issue-number vocabulary is unavailable") from exc
    issue_numbers = payload.get("properties", {}).get("issue_numbers")
    items = issue_numbers.get("items") if isinstance(issue_numbers, dict) else None
    values = items.get("enum") if isinstance(items, dict) else None
    if (
        not isinstance(values, list)
        or any(not isinstance(value, int) or isinstance(value, bool) or value < 0
               for value in values)
        or len(values) != len(set(values))
    ):
        raise RuntimeError("public issue-number vocabulary is malformed")
    return frozenset(values)


_PUBLIC_ISSUE_NUMBERS = _load_public_issue_numbers()


@dataclass(frozen=True)
class RouteOutcome:
    report: dict | None
    timed_out: bool = False
    return_code: int | None = None


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def _json_document_bytes(payload: dict) -> bytes:
    return (
        json.dumps(payload, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n"
    ).encode("utf-8")


def _atomic_json_bytes(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    if os.name != "nt":
        temporary.chmod(0o600)
    os.replace(temporary, path)


def _atomic_json(path: Path, payload: dict) -> None:
    _atomic_json_bytes(path, _json_document_bytes(payload))


def _source_commit() -> str:
    if any(name in os.environ for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR")):
        raise RuntimeError("unset Git directory overrides before running the sweep")
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
        timeout=10,
    )
    commit = result.stdout.strip()
    if len(commit) != 40 or any(char not in "0123456789abcdef" for char in commit):
        raise RuntimeError("Git did not return a full source commit")
    return commit


def _source_fingerprint() -> str:
    digest = hashlib.sha256()
    for path in (
        Path(__file__).resolve(),
        NK_CLI,
        ROOT / "assets" / "bringup_report.schema.json",
        _PUBLIC_LIBRARY_FAMILY_SOURCE,
    ):
        digest.update(path.relative_to(ROOT).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def _source_key(iso_path: Path, iso_root: Path) -> str:
    return iso_path.relative_to(iso_root).as_posix().casefold()


def _iso_files(iso_root: Path) -> list[Path]:
    if not iso_root.is_dir():
        raise ValueError("ISO input directory is unavailable")
    return sorted(
        (path for path in iso_root.rglob("*")
         if path.is_file() and path.suffix.casefold() == ".iso"),
        key=lambda path: path.relative_to(iso_root).as_posix().casefold(),
    )


def _read_existing_rows(
    path: Path,
    source_commit: str,
    source_fingerprint: str,
    source_keys: set[str],
) -> dict[str, dict]:
    if not path.is_file():
        return {}
    try:
        payload = package_cache.read_bounded_json(
            path,
            max_bytes=_SWEEP_RESUME_JSON_MAX_BYTES,
            max_depth=_SWEEP_RESUME_JSON_MAX_DEPTH,
            max_members=_SWEEP_RESUME_JSON_MAX_MEMBERS,
            max_items=_SWEEP_RESUME_JSON_MAX_ITEMS,
            max_nodes=_SWEEP_RESUME_JSON_MAX_NODES,
        )
    except (OSError, ValueError) as exc:
        raise ValueError("existing private sweep report is unreadable") from exc
    if not isinstance(payload, dict) or payload.get("schema_version") != 1:
        raise ValueError("existing private sweep report has an unsupported schema")
    if (
        payload.get("source_commit") != source_commit
        or payload.get("source_fingerprint") != source_fingerprint
    ):
        return {}
    rows = payload.get("rows")
    if not isinstance(rows, list):
        raise ValueError("existing private sweep report has no row array")
    _validate_sweep_rows(rows)
    retained: dict[str, dict] = {}
    existing_keys: set[str] = set()
    for row in rows:
        key = row["source_key"]
        if key in existing_keys:
            raise ValueError("existing private sweep report has duplicate source_key rows")
        existing_keys.add(key)
        if key in source_keys:
            retained[key] = row
    return retained


def _process_names() -> set[str]:
    if os.name == "nt":
        completed = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH"],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        if completed.returncode != 0:
            raise RuntimeError("could not inspect running processes before the next title")
        names = {row[0].casefold().removesuffix(".exe")
                 for row in csv.reader(completed.stdout.splitlines()) if row}
    else:
        completed = subprocess.run(
            ["ps", "-A", "-o", "comm="],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        if completed.returncode != 0:
            raise RuntimeError("could not inspect running processes before the next title")
        names = {Path(line.strip()).name.casefold().removesuffix(".exe")
                 for line in completed.stdout.splitlines() if line.strip()}
    return names


def _blocking_processes(process_reader=_process_names) -> set[str]:
    running = process_reader()
    return {
        name for name in running
        if name in BLOCKING_PROCESS_NAMES or name.startswith("ppsspp")
    }


def _wait_for_machine_idle(
    *,
    process_reader=_process_names,
    sleeper=time.sleep,
    poll_seconds: int = MACHINE_POLL_SECONDS,
    max_wait_seconds: int = MACHINE_MAX_WAIT_SECONDS,
) -> tuple[int, bool]:
    """Wait at most 20 minutes for another verifier/player/emulator to exit."""
    started = time.monotonic()
    deadline = started + max_wait_seconds
    while True:
        if not _blocking_processes(process_reader):
            return int((time.monotonic() - started) * 1000), False
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return int((time.monotonic() - started) * 1000), True
        sleeper(min(poll_seconds, remaining))


def _route_environment() -> dict[str, str]:
    environment = os.environ.copy()
    environment["MAKEFLAGS"] = "-j4"
    if environment.get("NAKAGAWA_PSP_KEY_FILE"):
        return environment
    try:
        key_path = key_file_path(default_data_root())
    except PrerequisiteFetchError:
        return environment
    if key_path.is_file():
        # The key is consumed in place by the existing private boundary;
        # it is never read or copied by this tool.
        environment["NAKAGAWA_PSP_KEY_FILE"] = str(key_path)
    return environment


def _copy_plain_decrypted_file(source: Path, target_dir: Path, name: str, source_root: Path) -> bool:
    if (
        source.is_symlink()
        or not source.is_file()
        or Path(name).name != name
        or not name[:1].isalnum()
        or any(char not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-" for char in name)
        or Path(name).suffix.casefold() not in {".elf", ".prx"}
    ):
        return False
    try:
        source.resolve(strict=True).relative_to(source_root)
    except (OSError, ValueError):
        return False
    target_dir.mkdir(parents=True, exist_ok=True)
    target = target_dir / name
    if target.is_symlink() or (target.exists() and not target.is_file()):
        return False
    shutil.copyfile(source, target)
    return True


def _stage_decrypted_inputs(
    decrypted_titles: Path | None,
    disc_id: str | None,
    work_dir: Path,
) -> tuple[int, str]:
    """Stage user-decrypted inputs from <decrypted-titles>/<DISC_ID>/decrypted/."""
    if decrypted_titles is None or disc_id is None:
        return 0, "NOT_CONFIGURED"
    source_root = decrypted_titles.resolve(strict=False)
    user_dir = work_dir / "user-data" / "titles" / disc_id / "decrypted"
    direct_dir = source_root / disc_id / "decrypted"
    if direct_dir.is_dir():
        try:
            direct_dir.resolve(strict=True).relative_to(source_root)
        except (OSError, ValueError):
            return 0, "NO_MATCH"
        copied = sum(
            _copy_plain_decrypted_file(source, user_dir, source.name, source_root)
            for source in sorted(direct_dir.iterdir(), key=lambda path: path.name.casefold())
        )
        return copied, "DIRECT_FOLDER" if copied else "NO_VALID_FILES"

    return 0, "NO_MATCH"


def _terminate_process_tree(process: subprocess.Popen) -> None:
    if os.name == "nt" and getattr(process, "pid", None) is not None:
        try:
            subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
        except (OSError, subprocess.TimeoutExpired):
            pass
    try:
        process.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        process.kill()
        try:
            process.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            pass


def _run_bringup(
    iso_path: Path,
    work_dir: Path,
    report_path: Path,
    private_import_report_path: Path,
    time_budget_seconds: int,
) -> RouteOutcome:
    report_path.unlink(missing_ok=True)
    private_import_report_path.unlink(missing_ok=True)
    command = [
        sys.executable,
        str(NK_CLI),
        "bringup",
        str(iso_path),
        "--work-dir",
        str(work_dir),
        "--report",
        str(report_path),
        "--launch-timeout",
        str(max(1, min(20, time_budget_seconds))),
        "--private-sweep-import-report",
        str(private_import_report_path),
    ]
    kwargs = {
        "cwd": ROOT,
        "env": _route_environment(),
        "stdout": subprocess.DEVNULL,
        "stderr": subprocess.STDOUT,
        "text": True,
    }
    if os.name == "nt":
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    process = None
    try:
        process = subprocess.Popen(command, **kwargs)
        process.communicate(timeout=time_budget_seconds)
    except KeyboardInterrupt:
        if process is not None:
            _terminate_process_tree(process)
        raise
    except subprocess.TimeoutExpired:
        _terminate_process_tree(process)
        return RouteOutcome(None, timed_out=True)
    except OSError:
        return RouteOutcome(None)
    if not report_path.is_file():
        return RouteOutcome(None, return_code=process.returncode)
    try:
        report = package_cache.read_bounded_json(
            report_path,
            max_bytes=_SWEEP_TITLE_JSON_MAX_BYTES,
            max_depth=_SWEEP_TITLE_JSON_MAX_DEPTH,
            max_members=_SWEEP_TITLE_JSON_MAX_MEMBERS,
            max_items=_SWEEP_TITLE_JSON_MAX_ITEMS,
            max_nodes=_SWEEP_TITLE_JSON_MAX_NODES,
        )
    except (OSError, ValueError):
        return RouteOutcome(None, return_code=process.returncode)
    if not isinstance(report, dict):
        return RouteOutcome(None, return_code=process.returncode)
    return RouteOutcome(report, return_code=process.returncode)


def _normalize_nid_rows(values) -> list[dict]:
    if not isinstance(values, list):
        return []
    rows = []
    seen = set()
    for value in values:
        if not isinstance(value, dict):
            continue
        library = value.get("library")
        nid = value.get("nid")
        name = value.get("nid_name", value.get("name"))
        if isinstance(nid, int) and not isinstance(nid, bool) and 0 <= nid <= 0xFFFFFFFF:
            nid = f"0x{nid:08x}"
        if (
            not isinstance(nid, str)
            or len(nid) != 10
            or nid[:2].casefold() != "0x"
            or any(char not in "0123456789abcdefABCDEF" for char in nid[2:])
        ):
            continue
        identity = (library, nid.casefold(), name)
        if identity in seen:
            continue
        seen.add(identity)
        rows.append({
            "library": library if isinstance(library, str) else None,
            "nid": nid,
            "nid_name": name if isinstance(name, str) else None,
        })
    return rows


def _read_private_nid_rows(sidecar_path: Path, report: dict | None) -> tuple[list[dict], str]:
    if sidecar_path.is_file():
        try:
            payload = package_cache.read_bounded_json(
                sidecar_path,
                max_bytes=_SWEEP_TITLE_JSON_MAX_BYTES,
                max_depth=_SWEEP_TITLE_JSON_MAX_DEPTH,
                max_members=_SWEEP_TITLE_JSON_MAX_MEMBERS,
                max_items=_SWEEP_TITLE_JSON_MAX_ITEMS,
                max_nodes=_SWEEP_TITLE_JSON_MAX_NODES,
            )
        except (OSError, ValueError):
            payload = None
        if isinstance(payload, dict) and payload.get("schema_version") == 1:
            rows = _normalize_nid_rows(payload.get("unsupported_imports"))
            if rows:
                return rows, "CAPTURED"
    build_report_rows = []
    for build_report_path in sorted(sidecar_path.parent.rglob("build-report.json")):
        try:
            build_report = package_cache.read_build_report_json(build_report_path)
        except (OSError, ValueError):
            continue
        if not isinstance(build_report, dict):
            continue
        unsupported = build_report.get("unsupported", {})
        if isinstance(unsupported, dict):
            build_report_rows.extend(_normalize_nid_rows(unsupported.get("imports")))
    if build_report_rows:
        return _normalize_nid_rows(build_report_rows), "BUILD_REPORT"
    if isinstance(report, dict):
        runtime_imports = _normalize_nid_rows(report.get("runtime_imports"))
        if runtime_imports:
            return runtime_imports, "RUNTIME_ONLY"
        stages = report.get("stages", {})
        if isinstance(stages, dict) and stages.get("analyze", {}).get("status") == "PASS":
            return [], "UNAVAILABLE"
    return [], "NOT_REACHED"


def _furthest_stage(report: dict | None) -> str:
    stage = "identify"
    if not isinstance(report, dict):
        return stage
    stages = report.get("stages", {})
    for report_stage, sweep_stage in (
        ("inspect", "identify"),
        ("prepare_import", "decrypt"),
        ("analyze", "analyze"),
        ("codegen", "codegen"),
        ("compile", "compile"),
        ("build_package", "compile"),
        ("launch", "launch"),
    ):
        detail = stages.get(report_stage, {}) if isinstance(stages, dict) else {}
        if isinstance(detail, dict) and detail.get("status") in {"PASS", "FAIL", "TIMED_OUT"}:
            stage = sweep_stage
    presentation = report.get("presentation", {})
    if isinstance(presentation, dict) and presentation.get("frame_submissions", 0) > 0:
        return "first presented frame"
    return stage


def _issue_numbers(report: dict | None) -> list[int]:
    if not isinstance(report, dict) or not isinstance(report.get("issue_numbers"), list):
        return []
    return sorted({number for number in report["issue_numbers"]
                   if isinstance(number, int) and not isinstance(number, bool)})


def _nonnegative_int(value, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"public aggregate field {field} must be a nonnegative integer")
    return value


def _signed_int_or_none(value, field: str) -> int | None:
    if value is None:
        return None
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError(f"public aggregate field {field} must be an integer or null")
    return value


def _validate_stage_map(value, field: str, *, nonnegative: bool) -> dict[str, int]:
    if not isinstance(value, dict) or set(value) != set(STAGES):
        raise ValueError(f"public aggregate field {field} has an invalid stage set")
    result = {}
    for stage in STAGES:
        item = value[stage]
        if nonnegative:
            result[stage] = _nonnegative_int(item, f"{field}.{stage}")
        else:
            result[stage] = _signed_int_or_none(item, f"{field}.{stage}")
            if result[stage] is None:
                raise ValueError(f"public aggregate field {field}.{stage} cannot be null")
    return result


def _validate_public_aggregate(payload: object) -> dict:
    """Validate the title-free aggregate before it is read or compared."""
    if not isinstance(payload, dict):
        raise ValueError("public aggregate must be a JSON object")
    keys = set(payload)
    if not _PUBLIC_AGGREGATE_REQUIRED_KEYS <= keys:
        missing = sorted(_PUBLIC_AGGREGATE_REQUIRED_KEYS - keys)
        raise ValueError(f"public aggregate is missing fields: {', '.join(missing)}")
    unknown = keys - _PUBLIC_AGGREGATE_REQUIRED_KEYS - _PUBLIC_AGGREGATE_OPTIONAL_KEYS
    if unknown:
        raise ValueError("public aggregate contains unrecognized fields")
    if (
        not isinstance(payload["schema_version"], int)
        or isinstance(payload["schema_version"], bool)
        or payload["schema_version"] != 1
    ):
        raise ValueError("public aggregate has an unsupported schema")

    source_commit = payload["source_commit"]
    if (
        not isinstance(source_commit, str)
        or len(source_commit) != 40
        or any(char not in "0123456789abcdef" for char in source_commit)
    ):
        raise ValueError("public aggregate source_commit is invalid")
    source_fingerprint = payload["source_fingerprint"]
    if (
        not isinstance(source_fingerprint, str)
        or len(source_fingerprint) != 64
        or any(char not in "0123456789abcdef" for char in source_fingerprint)
    ):
        raise ValueError("public aggregate source_fingerprint is invalid")
    time_budget = payload["time_budget_seconds"]
    if not isinstance(time_budget, int) or isinstance(time_budget, bool) or time_budget < 1:
        raise ValueError("public aggregate time_budget_seconds is invalid")

    coverage = payload["coverage"]
    if not isinstance(coverage, dict):
        raise ValueError("public aggregate coverage must be an object")
    coverage_keys = set(coverage)
    if (
        not _PUBLIC_COVERAGE_REQUIRED_KEYS <= coverage_keys
        or coverage_keys - _PUBLIC_COVERAGE_REQUIRED_KEYS - _PUBLIC_COVERAGE_OPTIONAL_KEYS
    ):
        raise ValueError("public aggregate coverage contains an invalid field set")
    for field in sorted(coverage_keys):
        _nonnegative_int(coverage[field], f"coverage.{field}")
    iso_count = coverage["iso_count"]
    recorded_routes = coverage.get("recorded_routes", iso_count)
    if recorded_routes > iso_count:
        raise ValueError("public aggregate recorded routes exceed discovered ISO count")
    route_total = (
        coverage["completed_routes"]
        + coverage["time_budget_exits"]
        + coverage["route_errors"]
    )
    if route_total != recorded_routes:
        raise ValueError("public aggregate route status counts do not match recorded routes")

    histogram = _validate_stage_map(
        payload["furthest_stage_histogram"], "furthest_stage_histogram", nonnegative=True
    )
    if sum(histogram.values()) != recorded_routes:
        raise ValueError("public aggregate stage histogram does not match recorded routes")
    blockers = payload["top_blocker_classes"]
    if not isinstance(blockers, list):
        raise ValueError("public aggregate top_blocker_classes must be a list")
    blocker_keys = {"boundary_code", "title_count", "issue_numbers", "tracking"}
    blocker_names: set[str] = set()
    previous_blocker_sort_key: tuple[int, str] | None = None
    blocker_title_total = 0
    for blocker in blockers:
        if not isinstance(blocker, dict) or set(blocker) != blocker_keys:
            raise ValueError("public aggregate blocker contains unrecognized fields")
        boundary_code = blocker["boundary_code"]
        if (
            not isinstance(boundary_code, str)
            or boundary_code == "NONE"
            or boundary_code not in _PUBLIC_BLOCKER_CODES
        ):
            raise ValueError("public aggregate blocker code is not source-backed")
        title_count = _nonnegative_int(blocker["title_count"], "top_blocker_classes.title_count")
        if title_count < 1 or title_count > recorded_routes or boundary_code in blocker_names:
            raise ValueError("public aggregate blocker count or ordering is invalid")
        blocker_title_total += title_count
        blocker_names.add(boundary_code)
        blocker_sort_key = (-title_count, boundary_code)
        if previous_blocker_sort_key is not None and blocker_sort_key < previous_blocker_sort_key:
            raise ValueError("public aggregate blocker count or ordering is invalid")
        previous_blocker_sort_key = blocker_sort_key
        issue_numbers = blocker["issue_numbers"]
        if (
            not isinstance(issue_numbers, list)
            or any(not isinstance(number, int) or isinstance(number, bool) or number < 0
                   for number in issue_numbers)
            or any(number not in _PUBLIC_ISSUE_NUMBERS for number in issue_numbers)
            or issue_numbers != sorted(set(issue_numbers))
        ):
            raise ValueError("public aggregate blocker issue numbers are invalid")
        tracking = blocker["tracking"]
        expected_tracking = (
            ", ".join(f"#{number}" for number in issue_numbers)
            if issue_numbers else "no issue: propose one"
        )
        if tracking != expected_tracking:
            raise ValueError("public aggregate blocker tracking is invalid")
    if blocker_title_total > recorded_routes:
        raise ValueError("public aggregate blocker counts exceed recorded routes")

    families = payload["nid_families_by_title_count"]
    if not isinstance(families, list):
        raise ValueError("public aggregate nid families must be a list")
    family_keys = {"library_family", "title_count"}
    family_names: set[str] = set()
    previous_family_sort_key: tuple[int, str, str] | None = None
    for family in families:
        if not isinstance(family, dict) or set(family) != family_keys:
            raise ValueError("public aggregate family contains unrecognized fields")
        name = family["library_family"]
        if (
            not isinstance(name, str)
            or _PUBLIC_LIBRARY_FAMILY.fullmatch(name) is None
            or name not in _PUBLIC_LIBRARY_FAMILIES
        ):
            raise ValueError("public aggregate library family is not source-backed")
        title_count = _nonnegative_int(
            family["title_count"], "nid_families_by_title_count.title_count"
        )
        if title_count < 1 or title_count > recorded_routes or name in family_names:
            raise ValueError("public aggregate family count or ordering is invalid")
        family_names.add(name)
        family_sort_key = (-title_count, name.casefold(), name)
        if previous_family_sort_key is not None and family_sort_key < previous_family_sort_key:
            raise ValueError("public aggregate family count or ordering is invalid")
        previous_family_sort_key = family_sort_key
    if payload["input_responsiveness"] != "NOT_MEASURED_BY_HEADLESS_BRINGUP":
        raise ValueError("public aggregate input responsiveness is unsupported")

    ratchet = payload.get("ratchet")
    if "ratchet" in payload and ratchet is None:
        raise ValueError("public aggregate ratchet cannot be null")
    if ratchet is not None:
        if not isinstance(ratchet, dict) or set(ratchet) != {"furthest_stage_high_water"}:
            raise ValueError("public aggregate ratchet contains unrecognized fields")
        high_water = _validate_stage_map(
            ratchet["furthest_stage_high_water"],
            "ratchet.furthest_stage_high_water",
            nonnegative=True,
        )
        for stage in STAGES:
            if high_water[stage] > iso_count or high_water[stage] < histogram[stage]:
                raise ValueError("public aggregate ratchet high-water is inconsistent")

    comparison = payload.get("comparison")
    if "comparison" in payload and comparison is None:
        raise ValueError("public aggregate comparison cannot be null")
    if comparison is not None:
        if not isinstance(comparison, dict) or set(comparison) != _PUBLIC_COMPARISON_KEYS:
            raise ValueError("public aggregate comparison contains unrecognized fields")
        status = comparison["status"]
        reason = comparison["reason"]
        if (
            not isinstance(status, str)
            or status not in _PUBLIC_COMPARISON_STATUSES
            or not isinstance(reason, str)
            or reason not in _PUBLIC_COMPARISON_REASONS
        ):
            raise ValueError("public aggregate comparison status is invalid")
        stage_delta = comparison["stage_delta"]
        stage_delta_map = None
        if stage_delta is not None:
            stage_delta_map = _validate_stage_map(
                stage_delta, "comparison.stage_delta", nonnegative=False
            )
        completed_routes_delta = _signed_int_or_none(
            comparison["completed_routes_delta"], "comparison.completed_routes_delta"
        )
        if status == "NO_BASELINE":
            if reason != "no_previous_aggregate" or stage_delta_map is not None or completed_routes_delta is not None:
                raise ValueError("public aggregate no-baseline comparison is inconsistent")
        elif status == "NEW_BASELINE":
            if (
                reason not in {
                    "source_identity_changed",
                    "time_budget_changed",
                    "input_count_changed",
                }
                or stage_delta_map is not None
                or completed_routes_delta is not None
            ):
                raise ValueError("public aggregate new-baseline comparison is inconsistent")
        elif (
            reason != "same_source_and_input_count"
            or stage_delta_map is None
            or completed_routes_delta is None
            or any(abs(stage_delta_map[stage]) > iso_count for stage in STAGES)
            or abs(completed_routes_delta) > iso_count
        ):
            raise ValueError("public aggregate matched comparison is inconsistent")
    return payload


def _read_previous_public_aggregate(path: Path | None) -> dict | None:
    if path is None:
        return None
    if not path.is_file():
        raise ValueError("previous public aggregate is unavailable")
    try:
        payload = package_cache.read_bounded_json(path)
    except (OSError, ValueError) as exc:
        raise ValueError("previous public aggregate is unreadable") from exc
    try:
        return _validate_public_aggregate(payload)
    except ValueError as exc:
        raise ValueError(f"previous public aggregate is invalid: {exc}") from exc


def _nid_families(report: dict | None, nid_rows: list[dict]) -> list[str]:
    families = {
        row["library"] for row in nid_rows
        if isinstance(row.get("library"), str) and row["library"]
    }
    if isinstance(report, dict):
        for field in ("unsupported_imports", "runtime_imports"):
            values = report.get(field)
            if not isinstance(values, list):
                continue
            for item in values:
                if isinstance(item, dict) and isinstance(item.get("library"), str) and item["library"]:
                    families.add(item["library"])
    return sorted(families, key=str.casefold)


def _public_library_family(value: object) -> str | None:
    if not isinstance(value, str) or value not in _PUBLIC_LIBRARY_FAMILIES:
        return None
    return value


def _validate_sweep_rows(rows: object) -> None:
    """Reject malformed sweep rows before either output file can advance.

    Rows retained from an earlier private report must match the row schema and
    source-backed value domains the aggregate and checkpoint writer rely on.
    A wrong field type or value would otherwise fail after the private report
    was already rewritten, leaving the private and public outputs on different
    checkpoints, so every checked shape and domain fails closed as
    ``ValueError`` here.
    """
    if not isinstance(rows, list):
        raise ValueError("sweep rows must be a list")
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValueError(f"sweep row {index} must be a JSON object")
        if "source_key" not in row:
            raise ValueError(f"sweep row {index} is missing field source_key")
        if not isinstance(row["source_key"], str):
            raise ValueError(f"sweep row {index} field source_key must be a string")
        if not row["source_key"]:
            raise ValueError(f"sweep row {index} field source_key must be non-empty")
        if "furthest_stage" in row and not isinstance(row["furthest_stage"], str):
            raise ValueError(f"sweep row {index} field furthest_stage must be a string")
        stage = row.get("furthest_stage", "identify")
        if stage not in STAGES:
            raise ValueError(
                f"sweep row {index} furthest_stage is not source-backed"
            )
        if "run_status" in row and not isinstance(row["run_status"], str):
            raise ValueError(f"sweep row {index} field run_status must be a string")
        run_status = row.get("run_status")
        if run_status not in _SWEEP_RUN_STATUSES:
            raise ValueError(f"sweep row {index} run_status is not supported")
        boundary = row.get("boundary_code")
        if boundary is not None and not isinstance(boundary, str):
            raise ValueError(
                f"sweep row {index} field boundary_code must be a string or null"
            )
        if boundary not in (None, "NONE") and boundary not in _PUBLIC_BLOCKER_CODES:
            raise ValueError(
                f"sweep row {index} boundary_code is not source-backed"
            )
        if "issue_numbers" in row:
            numbers = row["issue_numbers"]
            if not isinstance(numbers, list) or any(
                not isinstance(number, int) or isinstance(number, bool)
                for number in numbers
            ):
                raise ValueError(
                    f"sweep row {index} field issue_numbers must be a list of integers"
                )
            if numbers != sorted(set(numbers)):
                raise ValueError(
                    f"sweep row {index} issue numbers must be sorted and unique"
                )
            if any(number < 0 or number not in _PUBLIC_ISSUE_NUMBERS for number in numbers):
                raise ValueError(
                    f"sweep row {index} issue numbers are not source-backed"
                )
        if "nid_families" in row:
            families = row["nid_families"]
            if not isinstance(families, list) or any(
                not isinstance(family, str) for family in families
            ):
                raise ValueError(
                    f"sweep row {index} field nid_families must be a list of strings"
                )


def _public_aggregate(
    rows: list[dict],
    source_commit: str,
    source_fingerprint: str,
    time_budget_seconds: int,
    total_isos: int,
    *,
    previous_aggregate: dict | None = None,
) -> dict:
    _validate_sweep_rows(rows)
    if not isinstance(total_isos, int) or isinstance(total_isos, bool) or total_isos < len(rows):
        raise ValueError("public aggregate total ISO count is smaller than the number of recorded rows")
    stage_counts = Counter(row.get("furthest_stage", "identify") for row in rows)
    histogram = {stage: stage_counts.get(stage, 0) for stage in STAGES}
    blocker_rows: dict[str, dict] = {}
    family_counts: Counter[str] = Counter()
    for row in rows:
        code = row.get("boundary_code")
        if code and code != "NONE":
            entry = blocker_rows.setdefault(code, {"boundary_code": code, "title_count": 0, "issue_numbers": set()})
            entry["title_count"] += 1
            entry["issue_numbers"].update(row.get("issue_numbers", []))
        for family in set(row.get("nid_families", [])):
            public_family = _public_library_family(family)
            if public_family is not None:
                family_counts[public_family] += 1
    blockers = []
    for code, item in blocker_rows.items():
        issues = sorted(item["issue_numbers"])
        blockers.append({
            "boundary_code": code,
            "title_count": item["title_count"],
            "issue_numbers": issues,
            "tracking": ", ".join(f"#{number}" for number in issues) if issues else "no issue: propose one",
        })
    blockers.sort(key=lambda item: (-item["title_count"], item["boundary_code"]))
    families = [
        {"library_family": family, "title_count": count}
        for family, count in sorted(
            family_counts.items(), key=lambda item: (-item[1], item[0].casefold(), item[0])
        )
    ]
    # Blocker rows carry one boundary code each; a title can name several NID families,
    # so the family title counts can legitimately add up to more than recorded_routes.
    aggregate = {
        "schema_version": 1,
        "source_commit": source_commit,
        "source_fingerprint": source_fingerprint,
        "time_budget_seconds": time_budget_seconds,
        "coverage": {
            "iso_count": total_isos,
            "recorded_routes": len(rows),
            "completed_routes": sum(row.get("run_status") == "COMPLETED" for row in rows),
            "time_budget_exits": sum(row.get("run_status") == "TIMED_OUT" for row in rows),
            "route_errors": sum(row.get("run_status") == "NO_REPORT" for row in rows),
        },
        "furthest_stage_histogram": histogram,
        "top_blocker_classes": blockers,
        "nid_families_by_title_count": families,
        "input_responsiveness": "NOT_MEASURED_BY_HEADLESS_BRINGUP",
    }

    if previous_aggregate is not None:
        # The CLI loader already validates its file input. Keep this check so direct
        # callers of _public_aggregate cannot bypass the previous-output contract.
        previous_aggregate = _validate_public_aggregate(previous_aggregate)
    current_histogram = aggregate["furthest_stage_histogram"]
    current_coverage = aggregate["coverage"]
    if previous_aggregate is None:
        comparison_status = "NO_BASELINE"
        comparison_reason = "no_previous_aggregate"
        previous_high_water = {}
        stage_delta = None
        completed_routes_delta = None
    else:
        previous_coverage = previous_aggregate["coverage"]
        same_source = (
            previous_aggregate["source_commit"] == source_commit
            and previous_aggregate["source_fingerprint"] == source_fingerprint
        )
        same_budget = previous_aggregate["time_budget_seconds"] == time_budget_seconds
        same_input_count = previous_coverage["iso_count"] == current_coverage["iso_count"]
        if same_source and same_budget and same_input_count:
            comparison_status = "MATCHED"
            comparison_reason = "same_source_and_input_count"
            previous_high_water = (
                previous_aggregate.get("ratchet", {})
                .get("furthest_stage_high_water",
                     previous_aggregate["furthest_stage_histogram"])
            )
            previous_histogram = previous_aggregate["furthest_stage_histogram"]
            stage_delta = {
                stage: current_histogram[stage] - previous_histogram[stage]
                for stage in STAGES
            }
            completed_routes_delta = (
                current_coverage["completed_routes"] - previous_coverage["completed_routes"]
            )
        else:
            comparison_status = "NEW_BASELINE"
            if not same_source:
                comparison_reason = "source_identity_changed"
            elif not same_budget:
                comparison_reason = "time_budget_changed"
            else:
                comparison_reason = "input_count_changed"
            previous_high_water = {}
            stage_delta = None
            completed_routes_delta = None
    high_water = {
        stage: max(current_histogram[stage], previous_high_water.get(stage, 0))
        for stage in STAGES
    }
    aggregate["ratchet"] = {"furthest_stage_high_water": high_water}
    aggregate["comparison"] = {
        "status": comparison_status,
        "reason": comparison_reason,
        "stage_delta": stage_delta,
        "completed_routes_delta": completed_routes_delta,
    }
    return _validate_public_aggregate(aggregate)


def _write_outputs(
    private_path: Path,
    public_path: Path,
    source_commit: str,
    source_fingerprint: str,
    time_budget_seconds: int,
    rows: list[dict],
    *,
    total_isos: int,
    ran_this_invocation: int,
    resumed_this_invocation: int,
    previous_aggregate: dict | None = None,
) -> None:
    _validate_sweep_rows(rows)
    private_payload = {
        "schema_version": 1,
        "source_commit": source_commit,
        "source_fingerprint": source_fingerprint,
        "time_budget_seconds": time_budget_seconds,
        "updated_at_utc": _utc_now(),
        "coverage": {
            "iso_count": total_isos,
            "ran_this_invocation": ran_this_invocation,
            "resumed_this_invocation": resumed_this_invocation,
        },
        "rows": sorted(rows, key=lambda row: row.get("source_key", "")),
    }
    public_aggregate = _public_aggregate(
        rows,
        source_commit,
        source_fingerprint,
        time_budget_seconds,
        total_isos,
        previous_aggregate=previous_aggregate,
    )
    private_document = _json_document_bytes(private_payload)
    if len(private_document) > _SWEEP_RESUME_JSON_MAX_BYTES:
        raise ValueError("private sweep report exceeds its configured byte budget")
    try:
        package_cache.bounded_json_loads(
            private_document.decode("utf-8"),
            max_depth=_SWEEP_RESUME_JSON_MAX_DEPTH,
            max_members=_SWEEP_RESUME_JSON_MAX_MEMBERS,
            max_items=_SWEEP_RESUME_JSON_MAX_ITEMS,
            max_nodes=_SWEEP_RESUME_JSON_MAX_NODES,
        )
    except ValueError as exc:
        raise ValueError("private sweep report exceeds its configured JSON limits") from exc
    _atomic_json_bytes(private_path, private_document)
    _atomic_json(public_path, public_aggregate)


def run_sweep(
    iso_dir: Path,
    private_dir: Path,
    public_output: Path,
    *,
    time_budget_seconds: int = DEFAULT_TIME_BUDGET_SECONDS,
    decrypted_titles: Path | None = None,
    previous_public_output: Path | None = None,
    source_commit: str | None = None,
    process_reader=_process_names,
    sleeper=time.sleep,
    poll_seconds: int = MACHINE_POLL_SECONDS,
    max_wait_seconds: int = MACHINE_MAX_WAIT_SECONDS,
) -> dict:
    if time_budget_seconds < 1:
        raise ValueError("time budget must be at least one second")
    iso_root = iso_dir.resolve(strict=True)
    paths = _iso_files(iso_root)
    source_commit = source_commit or _source_commit()
    source_fingerprint = _source_fingerprint()
    if len(source_commit) != 40 or any(char not in "0123456789abcdef" for char in source_commit):
        raise ValueError("source commit must be a full lowercase Git SHA")
    private_dir.mkdir(parents=True, exist_ok=True)
    private_path = private_dir / "library-sweep.json"
    previous_aggregate = _read_previous_public_aggregate(previous_public_output)
    source_keys = {_source_key(path, iso_root) for path in paths}
    rows_by_key = _read_existing_rows(
        private_path, source_commit, source_fingerprint, source_keys
    )
    path_by_key = {_source_key(path, iso_root): path for path in paths}
    for key, row in list(rows_by_key.items()):
        current_stat = path_by_key[key].stat()
        if (
            row.get("input_size_bytes") != current_stat.st_size
            or row.get("input_mtime_ns") != current_stat.st_mtime_ns
        ):
            del rows_by_key[key]
    previous_rows = len(rows_by_key)
    run_count = 0
    resumed_count = 0

    _write_outputs(
        private_path, public_output, source_commit, source_fingerprint, time_budget_seconds,
        list(rows_by_key.values()), total_isos=len(paths), ran_this_invocation=0,
        resumed_this_invocation=previous_rows, previous_aggregate=previous_aggregate,
    )
    for index, iso_path in enumerate(paths, start=1):
        key = _source_key(iso_path, iso_root)
        stat = iso_path.stat()
        existing = rows_by_key.get(key)
        if (
            existing is not None
            and existing.get("input_size_bytes") == stat.st_size
            and existing.get("input_mtime_ns") == stat.st_mtime_ns
        ):
            resumed_count += 1
            print(f"[{index}/{len(paths)}] resumed recorded result")
            continue

        load_wait_ms, load_remained_active = _wait_for_machine_idle(
            process_reader=process_reader,
            sleeper=sleeper,
            poll_seconds=poll_seconds,
            max_wait_seconds=max_wait_seconds,
        )
        disc_id = None
        title_name = None
        inspect_error = None
        try:
            metadata = inspect_iso(iso_path)
            disc_id = metadata.disc_id.upper()
            title_name = metadata.title
        except Exception as exc:  # the bring-up route still runs and names its own boundary
            inspect_error = type(exc).__name__
            print(f"[{index}/{len(paths)}] ISO inspection failed ({inspect_error}); "
                  "running the route without decrypted-input staging", file=sys.stderr)
        safe_key = hashlib.sha256(key.encode("utf-8")).hexdigest()[:20]
        work_dir = private_dir / "work" / safe_key
        report_path = work_dir / "bringup.json"
        private_import_report_path = work_dir / "sweep-imports.json"
        try:
            staged_input_count, decrypted_input_status = _stage_decrypted_inputs(
                decrypted_titles, disc_id, work_dir
            )
        except (OSError, ValueError):
            staged_input_count = 0
            decrypted_input_status = "COPY_FAILED"
        started = time.perf_counter()
        outcome = _run_bringup(
            iso_path,
            work_dir,
            report_path,
            private_import_report_path,
            time_budget_seconds,
        )
        route_wall_time_ms = int((time.perf_counter() - started) * 1000)
        report = outcome.report
        if outcome.timed_out:
            run_status = "TIMED_OUT"
            boundary_code = "SWEEP_TIME_BUDGET"
            issues = []
        elif report is None:
            run_status = "NO_REPORT"
            boundary_code = "SWEEP_ROUTE_FAILED"
            issues = []
        else:
            run_status = "COMPLETED"
            boundary_code = report.get("failure_class") or "SWEEP_REPORT_INVALID"
            issues = _issue_numbers(report)
        nid_rows, nid_status = _read_private_nid_rows(private_import_report_path, report)
        row = {
            "source_key": key,
            "source_file": iso_path.name,
            "disc_id": disc_id,
            "title_name": title_name,
            "inspect_error": inspect_error,
            "input_size_bytes": stat.st_size,
            "input_mtime_ns": stat.st_mtime_ns,
            "furthest_stage": _furthest_stage(report),
            "boundary_code": boundary_code,
            "issue_numbers": issues,
            "first_missing_nids": nid_rows[:5],
            "nid_families": _nid_families(report, nid_rows),
            "nid_evidence_status": nid_status,
            "wall_time_ms": route_wall_time_ms + load_wait_ms,
            "bringup_wall_time_ms": route_wall_time_ms,
            "machine_load_wait_ms": load_wait_ms,
            "machine_load_still_active_after_wait": load_remained_active,
            "decrypted_inputs_staged": staged_input_count,
            "decrypted_input_status": decrypted_input_status,
            "run_status": run_status,
            "route_exit_code": outcome.return_code,
            "input_responsive": "NOT_MEASURED",
        }
        rows_by_key[key] = row
        run_count += 1
        _write_outputs(
            private_path, public_output, source_commit, source_fingerprint, time_budget_seconds,
            list(rows_by_key.values()), total_isos=len(paths), ran_this_invocation=run_count,
            resumed_this_invocation=resumed_count, previous_aggregate=previous_aggregate,
        )
        print(f"[{index}/{len(paths)}] {row['furthest_stage']} ({run_status})")

    result = {
        "private_report": str(private_path),
        "public_aggregate": str(public_output),
        "source_commit": source_commit,
        "source_fingerprint": source_fingerprint,
        "iso_count": len(paths),
        "ran_this_invocation": run_count,
        "resumed_this_invocation": resumed_count,
        "rows": list(rows_by_key.values()),
    }
    _write_outputs(
        private_path, public_output, source_commit, source_fingerprint, time_budget_seconds,
        list(rows_by_key.values()), total_isos=len(paths), ran_this_invocation=run_count,
        resumed_this_invocation=resumed_count, previous_aggregate=previous_aggregate,
    )
    return result


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso_dir", type=Path, help="Directory containing PSP ISO images")
    parser.add_argument("--private-dir", required=True, type=Path,
                        help="Private directory for per-title reports and build outputs")
    parser.add_argument("--public-output", required=True, type=Path,
                        help="Destination for the aggregate without title names or disc IDs")
    parser.add_argument("--decrypted-titles", type=Path,
                        help="Optional root containing per-title decrypted input folders")
    parser.add_argument("--previous-public-output", type=Path,
                        help="Existing title-free aggregate used as a comparison baseline")
    parser.add_argument("--time-budget", type=_positive_int, default=DEFAULT_TIME_BUDGET_SECONDS,
                        help="Hard per-title route limit in seconds (default: 120)")
    args = parser.parse_args(argv)
    try:
        result = run_sweep(
            args.iso_dir,
            args.private_dir,
            args.public_output,
            time_budget_seconds=args.time_budget,
            decrypted_titles=args.decrypted_titles,
            previous_public_output=args.previous_public_output,
        )
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        print(f"library sweep failed: {exc}", file=sys.stderr)
        return 2
    print(
        f"Completed {result['iso_count']} ISO(s): {result['ran_this_invocation']} run, "
        f"{result['resumed_this_invocation']} resumed; public aggregate written."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
