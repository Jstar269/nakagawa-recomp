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
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
NK_CLI = ROOT / "tools" / "nk_cli.py"
DEFAULT_TIME_BUDGET_SECONDS = 120
MACHINE_POLL_SECONDS = 60
MACHINE_MAX_WAIT_SECONDS = 20 * 60
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


@dataclass(frozen=True)
class RouteOutcome:
    report: dict | None
    timed_out: bool = False
    return_code: int | None = None


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def _atomic_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(payload, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )
    if os.name != "nt":
        temporary.chmod(0o600)
    os.replace(temporary, path)


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
    for path in (Path(__file__).resolve(), NK_CLI, ROOT / "assets" / "bringup_report.schema.json"):
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
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
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
    retained: dict[str, dict] = {}
    for row in rows:
        if not isinstance(row, dict):
            continue
        key = row.get("source_key")
        if isinstance(key, str) and key in source_keys:
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
    base = environment.get("LOCALAPPDATA") or environment.get("APPDATA") or environment.get("USERPROFILE")
    if base:
        key_path = Path(base) / "Nakagawa" / "data" / "keys" / "psp-keyfile.json"
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
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
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
            payload = json.loads(sidecar_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            payload = None
        if isinstance(payload, dict) and payload.get("schema_version") == 1:
            rows = _normalize_nid_rows(payload.get("unsupported_imports"))
            if rows:
                return rows, "CAPTURED"
    build_report_rows = []
    for build_report_path in sorted(sidecar_path.parent.rglob("build-report.json")):
        try:
            build_report = json.loads(build_report_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
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


def _public_aggregate(
    rows: list[dict],
    source_commit: str,
    source_fingerprint: str,
    time_budget_seconds: int,
) -> dict:
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
            family_counts[family] += 1
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
        for family, count in sorted(family_counts.items(), key=lambda item: (-item[1], item[0].casefold()))
    ]
    return {
        "schema_version": 1,
        "source_commit": source_commit,
        "source_fingerprint": source_fingerprint,
        "time_budget_seconds": time_budget_seconds,
        "coverage": {
            "iso_count": len(rows),
            "completed_routes": sum(row.get("run_status") == "COMPLETED" for row in rows),
            "time_budget_exits": sum(row.get("run_status") == "TIMED_OUT" for row in rows),
            "route_errors": sum(row.get("run_status") == "NO_REPORT" for row in rows),
        },
        "furthest_stage_histogram": histogram,
        "top_blocker_classes": blockers[:10],
        "nid_families_by_title_count": families,
        "input_responsiveness": "NOT_MEASURED_BY_HEADLESS_BRINGUP",
    }


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
) -> None:
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
    _atomic_json(private_path, private_payload)
    _atomic_json(
        public_path,
        _public_aggregate(rows, source_commit, source_fingerprint, time_budget_seconds),
    )


def run_sweep(
    iso_dir: Path,
    private_dir: Path,
    public_output: Path,
    *,
    time_budget_seconds: int = DEFAULT_TIME_BUDGET_SECONDS,
    decrypted_titles: Path | None = None,
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
        resumed_this_invocation=previous_rows,
    )
    from nk_core import inspect_iso

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
        try:
            metadata = inspect_iso(iso_path)
            disc_id = metadata.disc_id.upper()
            title_name = metadata.title
        except Exception:
            pass
        safe_key = hashlib.sha256(key.encode("utf-8")).hexdigest()[:20]
        work_dir = private_dir / "work" / safe_key
        report_path = work_dir / "bringup.json"
        private_import_report_path = work_dir / "sweep-imports.json"
        try:
            staged_input_count, decrypted_input_status = _stage_decrypted_inputs(
                decrypted_titles, disc_id, work_dir
            )
        except OSError:
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
            resumed_this_invocation=resumed_count,
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
        resumed_this_invocation=resumed_count,
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
