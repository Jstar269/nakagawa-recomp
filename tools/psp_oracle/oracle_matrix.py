# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""One command for a bounded PSP oracle matrix: build, stage, qualify and run (issue #352).

A matrix plan names a source commit and an ordered list of probe cases. This tool

1. builds every case, and the ``transport-write`` preflight, from a clean clone of
   that exact commit and stages the PRXs in a host0 scratch directory, recording
   each digest;
2. runs each case in its own fresh device boot ("one launch per boot"): before a
   launch it requalifies the PSP, issues one PSPLink soft reset, requalifies again,
   and requires the hardware lock's device-state attestation to name a boot newer
   than every boot this matrix has already seen;
3. launches ``transport-write`` plus the case through the configured probe command
   (normally the resident agent's guarded ``probe`` wrapper around
   ``tools/psp_oracle/run_psplink.py``, or the runner itself);
4. accepts a case only from a runner report written by that launch whose envelopes
   carry the plan's source commit, the staged PRX digests, the expected firmware and
   a host0 run window that starts after the launch; and
5. treats a launch during which the device rebooted, or after which the device had
   to be recovered, as a contained failure: its evidence is never accepted, the next
   case needs a newer qualified boot, and a bounded failure budget ends the matrix.
   A launch the probe command could not recover ends the matrix in an explicit
   ``PHYSICAL_INTERVENTION_REQUIRED`` state.

The PSP-side commands are explicit argv templates in the private plan, as the
runner's ``--pspsh-argv-json`` is: this tool never installs drivers, never writes the
Memory Stick or flash, and never retries a semantic result. Every reset and launch is
gated on the maintainer's hardware lock, and every launch is noted in it.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
from typing import Callable

_PACKAGE_PARENT = str(Path(__file__).resolve().parents[1])
if _PACKAGE_PARENT not in sys.path:
    sys.path.insert(0, _PACKAGE_PARENT)

try:
    from . import run_psplink as _runner
except ImportError:  # direct ``python tools/psp_oracle/oracle_matrix.py`` invocation
    from psp_oracle import run_psplink as _runner  # type: ignore[no-redef]


ROOT = Path(__file__).resolve().parents[2]
FIXTURE_ROOT = ROOT / "fixtures" / "psp_oracle"
PREFLIGHT_CASE = "transport-write"
MAX_CASES = 64
MAX_CASE_TIMEOUT_SECONDS = 3600
MAX_CONTAINED_FAILURES = 4
WATCHDOG_DEADLINE_RANGE = (30, 900)          # Nakagawa Link accepts 10..900 s; below 30 s pets race the runner
DEFAULT_WATCHDOG_DEADLINE = 180
RESET_COMMAND_TIMEOUT_SECONDS = 40.0         # the PSPLink reset drops USB, so its command usually times out
RECOVER_COMMAND_TIMEOUT_SECONDS = 420.0      # boot wait plus an armed watchdog's grace
BUILD_TIMEOUT_SECONDS = 1800.0
# Beyond the case timeout, the probe command may spend this long recovering the device (boot wait, armed
# watchdog countdown and its containment grace) before its own exit code reports the outcome.
PROBE_RECOVERY_ALLOWANCE_SECONDS = 900.0

_SIMPLE_ID_RE = re.compile(r"[A-Za-z0-9._-]{1,64}")
_CASE_ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
_SHA256_RE = re.compile(r"[0-9a-f]{64}")
_PLACEHOLDER_RE = re.compile(r"\{([A-Za-z_][A-Za-z0-9_]*)\}")
_COMMAND_PLACEHOLDERS = {
    "reset_argv": frozenset({"session_id", "host0_root", "host0_root_wsl", "repo_root"}),
    "recover_argv": frozenset({"session_id", "host0_root", "host0_root_wsl", "repo_root"}),
    "probe_argv": frozenset({"session_id", "host0_root", "host0_root_wsl", "repo_root",
                             "deadline", "runner_log"}),
    "build_shell_argv": frozenset(),
}
_RUNNER_TEMPLATES = ("pspsh_argv", "usbhostfs_argv")

# Case outcomes. Only ACCEPTED is evidence; every other status says why not.
ACCEPTED = "ACCEPTED"
NOT_ELIGIBLE = "NOT_ELIGIBLE"
REJECTED = "REJECTED"
CONTAINED = "CONTAINED"
INTERVENTION = "PHYSICAL_INTERVENTION_REQUIRED"
NOT_RUN = "NOT_RUN"


class MatrixError(ValueError):
    """The plan, the staged inputs or the checkpoint cannot be used; nothing touched the PSP."""


@dataclass(frozen=True)
class MatrixCase:
    case_id: str
    timeout: float
    prebuilt: Path | None = None
    sha256: str | None = None

    @property
    def staged_name(self) -> str:
        return self.case_id.replace("-", "_") + ".prx"


@dataclass(frozen=True)
class MatrixPlan:
    path: Path
    matrix_id: str
    session_id: str
    source_commit: str
    console_model: str
    expected_firmware: str
    repo_root: Path
    host0_root: Path
    evidence_dir: Path
    cases: tuple[MatrixCase, ...]
    commands: dict[str, list[str]]
    path_style: str
    watchdog_deadline: int
    max_contained_failures: int

    @property
    def checkpoint_path(self) -> Path:
        return self.evidence_dir / f"{self.matrix_id}.checkpoint.json"

    @property
    def report_path(self) -> Path:
        return self.evidence_dir / f"{self.matrix_id}.report.json"

    def shell_path(self, path: Path) -> str:
        return _runner._wsl_path(path) if self.path_style == "wsl" else path.as_posix()


def _argv_template(value: object, name: str, allowed: frozenset[str] | None) -> list[str]:
    if (
        not isinstance(value, list)
        or not value
        or not all(isinstance(part, str) and part for part in value)
    ):
        raise MatrixError(f"commands.{name} must be a nonempty JSON array of nonempty strings")
    if allowed is not None:
        for part in value:
            for placeholder in _PLACEHOLDER_RE.findall(part):
                if placeholder not in allowed:
                    raise MatrixError(f"commands.{name} uses unknown placeholder {{{placeholder}}}")
    return list(value)


def _resolve_under(base: Path, value: object, name: str) -> Path:
    if not isinstance(value, str) or not value:
        raise MatrixError(f"{name} must be a nonempty path")
    path = Path(value)
    if not path.is_absolute():
        path = base / path
    path = path.resolve()
    try:
        path.relative_to(base.resolve())
    except ValueError as exc:
        raise MatrixError(f"{name} must stay under {base}") from exc
    return path


def load_plan(path: Path, *, repo_root: Path = ROOT) -> MatrixPlan:
    """Read and validate a schema 1 matrix plan; refuse anything the runner would refuse later."""

    path = Path(path).resolve()
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise MatrixError(f"matrix plan could not be read ({type(exc).__name__})") from exc
    if not isinstance(raw, dict) or raw.get("schema") != 1:
        raise MatrixError("matrix plan must be a schema 1 JSON object")
    for key in ("matrix_id", "session_id", "source_commit", "console_model",
                "expected_firmware", "host0_root", "evidence_dir", "cases", "commands"):
        if key not in raw:
            raise MatrixError(f"matrix plan is missing {key}")
    for key in ("matrix_id", "session_id"):
        if not isinstance(raw[key], str) or not _SIMPLE_ID_RE.fullmatch(raw[key]):
            raise MatrixError(f"{key} must be a simple identifier")
    commit = raw["source_commit"]
    if not isinstance(commit, str) or not _runner._FULL_COMMIT_RE.fullmatch(commit):
        raise MatrixError("source_commit must be a full 40- or 64-digit object id")
    model = raw["console_model"]
    if not isinstance(model, str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,48}", model):
        raise MatrixError("console_model must be a non-identifying model label")
    firmware_problem = _runner._expected_firmware_problem(raw["expected_firmware"])
    if firmware_problem:
        raise MatrixError(f"expected_firmware: {firmware_problem}")

    repo_root = Path(repo_root).resolve()
    plan_dir = path.parent
    host0_root = _resolve_under(repo_root / "fixtures" / "psp_oracle", raw["host0_root"], "host0_root")
    evidence_dir = _resolve_under(plan_dir, raw["evidence_dir"], "evidence_dir")

    raw_cases = raw["cases"]
    if not isinstance(raw_cases, list) or not raw_cases:
        raise MatrixError("cases must be a nonempty ordered JSON array")
    if len(raw_cases) > MAX_CASES:
        raise MatrixError(f"a matrix runs at most {MAX_CASES} cases")
    cases: list[MatrixCase] = []
    seen: set[str] = set()
    for row in raw_cases:
        if not isinstance(row, dict):
            raise MatrixError("each case must be a JSON object")
        case_id = row.get("case_id")
        if not isinstance(case_id, str) or not _CASE_ID_RE.fullmatch(case_id):
            raise MatrixError("case_id must be a simple case identifier")
        if case_id == PREFLIGHT_CASE:
            raise MatrixError("transport-write runs before every case on its own; do not list it")
        if case_id in _runner.RETIRED_CAMPAIGN_CASES:
            raise MatrixError(f"campaign case {case_id} is retired and cannot be scheduled")
        if case_id in seen:
            raise MatrixError(f"duplicate case id: {case_id}")
        seen.add(case_id)
        timeout = row.get("timeout_seconds")
        if (
            not isinstance(timeout, (int, float))
            or isinstance(timeout, bool)
            or not math.isfinite(timeout)
            or not 0 < timeout <= MAX_CASE_TIMEOUT_SECONDS
        ):
            raise MatrixError(f"{case_id}: timeout_seconds must be in (0, {MAX_CASE_TIMEOUT_SECONDS}]")
        minimum = _runner.CAMPAIGN_CASE_MIN_TIMEOUT_SECONDS.get(case_id)
        if minimum is not None and timeout < minimum:
            raise MatrixError(f"{case_id}: needs timeout_seconds {minimum:g} or more")
        prebuilt = None
        sha256 = None
        if "prebuilt_prx" in row:
            prebuilt = _resolve_under(plan_dir, row["prebuilt_prx"], f"{case_id}: prebuilt_prx")
            sha256 = row.get("sha256")
            if not isinstance(sha256, str) or not _SHA256_RE.fullmatch(sha256):
                raise MatrixError(f"{case_id}: a prebuilt PRX needs its lowercase sha256 pinned")
            if prebuilt.suffix.lower() != ".prx":
                raise MatrixError(f"{case_id}: prebuilt_prx must be a .prx file")
        elif "sha256" in row:
            raise MatrixError(f"{case_id}: sha256 pins a prebuilt_prx only; built cases are hashed when built")
        cases.append(MatrixCase(case_id, float(timeout), prebuilt, sha256))

    commands_raw = raw["commands"]
    if not isinstance(commands_raw, dict):
        raise MatrixError("commands must be a JSON object")
    commands: dict[str, list[str]] = {}
    for name, allowed in _COMMAND_PLACEHOLDERS.items():
        if name not in commands_raw:
            raise MatrixError(f"commands.{name} is required")
        commands[name] = _argv_template(commands_raw[name], name, allowed)
    for name in _RUNNER_TEMPLATES:
        if name not in commands_raw:
            raise MatrixError(f"commands.{name} is required")
        commands[name] = _argv_template(commands_raw[name], name, None)
    unknown = set(commands_raw) - set(commands) - {"path_style"}
    if unknown:
        raise MatrixError(f"unknown commands: {', '.join(sorted(unknown))}")
    path_style = commands_raw.get("path_style", "native")
    if path_style not in ("native", "wsl"):
        raise MatrixError("commands.path_style must be native or wsl")

    deadline = raw.get("watchdog_deadline_seconds", DEFAULT_WATCHDOG_DEADLINE)
    low, high = WATCHDOG_DEADLINE_RANGE
    if not isinstance(deadline, int) or isinstance(deadline, bool) or not low <= deadline <= high:
        raise MatrixError(f"watchdog_deadline_seconds must be an integer in [{low}, {high}]")
    budget = raw.get("max_contained_failures", 1)
    if (
        not isinstance(budget, int)
        or isinstance(budget, bool)
        or not 0 <= budget <= MAX_CONTAINED_FAILURES
    ):
        raise MatrixError(f"max_contained_failures must be an integer in [0, {MAX_CONTAINED_FAILURES}]")
    return MatrixPlan(
        path=path,
        matrix_id=raw["matrix_id"],
        session_id=raw["session_id"],
        source_commit=commit.lower(),
        console_model=model,
        expected_firmware=raw["expected_firmware"],
        repo_root=repo_root,
        host0_root=host0_root,
        evidence_dir=evidence_dir,
        cases=tuple(cases),
        commands=commands,
        path_style=path_style,
        watchdog_deadline=deadline,
        max_contained_failures=budget,
    )


def render_argv(template: list[str], values: dict[str, str]) -> list[str]:
    """Fill ``{name}`` placeholders; a placeholder without a value is a refusal, never left literal."""

    rendered: list[str] = []
    for part in template:
        missing = [name for name in _PLACEHOLDER_RE.findall(part) if name not in values]
        if missing:
            raise MatrixError(f"no value for placeholder {{{missing[0]}}}")
        rendered.append(_PLACEHOLDER_RE.sub(lambda match: values[match.group(1)], part))
    return rendered


# ---------------------------------------------------------------------------------------------- build


def build_script(plan: MatrixPlan, source_repo: str, cases: list[MatrixCase]) -> str:
    """The POSIX shell script that builds and stages ``cases`` from a clean clone of the plan's commit.

    ``source_repo`` is the repository's common Git directory in the shell's path form. The probe Makefile
    embeds ``git rev-parse HEAD`` and refuses a dirty tree, so the build runs in a real clone checked out at
    the exact commit. Each case gets its own ignored build directory; exactly one PRX must come out of it.
    """

    q = shlex.quote
    lines = [
        "set -u",
        'export PSPDEV="${PSPDEV:-/usr/local/pspdev}"',
        'export PATH="$PSPDEV/bin:$PATH"',
        "unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_INDEX_FILE",
        'fail() { echo "NK_MATRIX_BUILD_FAILED $*"; exit 1; }',
        'work=$(mktemp -d "${TMPDIR:-/tmp}/nk-oracle-matrix.XXXXXX") || fail mktemp',
        "trap 'rm -rf \"$work\"' EXIT",
        f'git clone -q --no-checkout --no-hardlinks {q(source_repo)} "$work/src" || fail clone',
        'cd "$work/src" || fail cd',
        f"git checkout -q --detach {q(plan.source_commit)} || fail checkout",
        f'[ "$(git rev-parse HEAD)" = {q(plan.source_commit)} ] || fail head',
        '[ -z "$(git status --porcelain --untracked-files=normal)" ] || fail dirty',
        "build_case() {",
        '  make -C fixtures/psp_oracle CASE="$1" BUILD_DIR="build/$1" EBOOT.PBP > "$work/$1.log" 2>&1'
        ' || { tail -n 20 "$work/$1.log"; fail "make $1"; }',
        '  set -- "$1" "$2" fixtures/psp_oracle/build/"$1"/*.prx',
        '  [ "$#" -eq 3 ] && [ -f "$3" ] || fail "prx $1"',
        '  cp "$3" "$2" || fail "stage $1"',
        '  echo "NK_MATRIX_BUILT $1 $(sha256sum "$2" | cut -c1-64)"',
        "}",
    ]
    for case in cases:
        staged = plan.shell_path(plan.host0_root / case.staged_name)
        lines.append(f"build_case {q(case.case_id)} {q(staged)}")
    lines.append("echo NK_MATRIX_BUILD_DONE")
    return "\n".join(lines) + "\n"


def parse_build_output(text: str, expected: list[str]) -> dict[str, str]:
    """Digests the build script reported, in order; anything short of a complete run is a refusal."""

    built: dict[str, str] = {}
    done = False
    for line in text.splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == "NK_MATRIX_BUILD_FAILED":
            raise MatrixError("build failed: " + " ".join(words[1:]))
        if words[0] == "NK_MATRIX_BUILT" and len(words) == 3 and _SHA256_RE.fullmatch(words[2]):
            built[words[1]] = words[2]
        elif words[0] == "NK_MATRIX_BUILD_DONE":
            done = True
    if not done or list(built) != expected:
        raise MatrixError("build did not report every case it was asked for")
    return built


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


# ------------------------------------------------------------------------------------- lock and epochs


def _utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _read_lock(path: Path) -> dict[str, object]:
    try:
        lock = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise MatrixError(f"HARDWARE_LOCK_UNAVAILABLE:{type(exc).__name__}") from exc
    if not isinstance(lock, dict):
        raise MatrixError("HARDWARE_LOCK_UNAVAILABLE:not-an-object")
    return lock


def lock_holder_problem(lock: dict[str, object], session_id: str) -> str | None:
    if lock.get("state") != "HELD":
        return "HARDWARE_LOCK_NOT_HELD"
    if lock.get("holder_session") != session_id:
        return "HARDWARE_LOCK_SESSION_MISMATCH"
    return None


def attested_boot(lock: dict[str, object], session_id: str) -> tuple[int | None, str]:
    """The boot number of a fresh, session-bound QUALIFIED device-state attestation, or a refusal status."""

    problem = lock_holder_problem(lock, session_id)
    if problem:
        return None, problem
    attestation, status = _runner._validate_device_state_attestation(lock, session_id)
    if attestation is None:
        return None, status
    return int(attestation["boot_number"]), status


def note_launch(path: Path, session_id: str, text: str) -> None:
    """Append one ``probes_run`` line to the lock this session holds (atomic replace)."""

    lock = _read_lock(path)
    problem = lock_holder_problem(lock, session_id)
    if problem:
        raise MatrixError(problem)
    runs = lock.get("probes_run")
    lock["probes_run"] = [*(runs if isinstance(runs, list) else []), f"{session_id} {_utc_now()}: {text}"]
    temporary = path.with_name(f".{path.name}.{os.getpid()}.matrix.tmp")
    temporary.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


# --------------------------------------------------------------------------------- launch classification


@dataclass
class CaseOutcome:
    case_id: str
    status: str
    reasons: list[str] = field(default_factory=list)
    boot: int | None = None
    boots_seen: list[int] = field(default_factory=list)
    probe_exit: int | None = None
    report: str | None = None

    def as_dict(self) -> dict[str, object]:
        return dict(self.__dict__)


def _probe_command_report(stdout: str) -> dict[str, object] | None:
    """The JSON object a guarded probe command prints last (Nakagawa Link's probe report), if any."""

    start = stdout.find("{")
    if start < 0:
        return None
    try:
        value = json.loads(stdout[start:])
    except json.JSONDecodeError:
        return None
    return value if isinstance(value, dict) else None


def _boots(report: dict[str, object] | None) -> list[int]:
    if report is None or not isinstance(report.get("boots"), list):
        return []
    return [boot for boot in report["boots"] if isinstance(boot, int) and not isinstance(boot, bool)]


def classify_launch(
    *,
    case: MatrixCase,
    epoch_boot: int,
    probe_exit: int | None,
    probe_stdout: str,
    runner_report_path: Path,
    launch_started_ns: int,
    staged: dict[str, str],
    source_commit: str,
    expected_firmware: str,
) -> CaseOutcome:
    """Decide what one launch proves. Fail closed: only a fresh, identity-bound, eligible pair is ACCEPTED."""

    outcome = CaseOutcome(case.case_id, NOT_ELIGIBLE, boot=epoch_boot, probe_exit=probe_exit,
                          report=runner_report_path.name)
    command_report = _probe_command_report(probe_stdout)
    outcome.boots_seen = _boots(command_report)
    if probe_exit is None:
        outcome.status = INTERVENTION
        outcome.reasons.append("probe command exceeded its wall-clock bound and was stopped; device state unknown")
        return outcome
    if probe_exit == 3:
        outcome.status = INTERVENTION
        detail = command_report.get("detail") if command_report else None
        outcome.reasons.append("probe command could not recover the device"
                               + (f": {detail}" if isinstance(detail, str) and detail else ""))
        return outcome
    rebooted = [boot for boot in outcome.boots_seen if boot != epoch_boot]
    if probe_exit == 4 or rebooted:
        outcome.status = CONTAINED
        if probe_exit == 4:
            outcome.reasons.append("the probe failed and the device had to be recovered")
        if rebooted:
            outcome.reasons.append(f"the device rebooted during the launch (boot {epoch_boot} -> "
                                   f"{', '.join(str(boot) for boot in rebooted)})")
        return outcome
    if probe_exit not in (0, 2):
        outcome.reasons.append(f"probe command exited {probe_exit}")
        return outcome

    try:
        info = runner_report_path.stat()
    except OSError:
        outcome.reasons.append("the runner wrote no report for this launch (the probe command refused or "
                               "stopped before the runner finished)")
        return outcome
    try:
        report = json.loads(runner_report_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        outcome.status = REJECTED
        outcome.reasons.append(f"runner report from this launch is unreadable ({type(exc).__name__})")
        return outcome
    if info.st_mtime_ns < launch_started_ns - _runner.HOST0_MTIME_TOLERANCE_NS:
        outcome.status = REJECTED
        outcome.reasons.append("runner report predates this launch (stale)")
        return outcome
    envelopes = report.get("envelopes") if isinstance(report, dict) else None
    if not isinstance(envelopes, list) or not all(isinstance(item, dict) for item in envelopes):
        outcome.status = REJECTED
        outcome.reasons.append("runner report has no envelope list")
        return outcome
    expected_ids = [PREFLIGHT_CASE, case.case_id]
    case_ids = [item.get("CASE_ID") for item in envelopes]
    if case_ids != expected_ids[:len(case_ids)] or not case_ids:
        outcome.status = REJECTED
        outcome.reasons.append(f"runner report names cases {case_ids}, not {expected_ids}")
        return outcome
    for envelope in envelopes:
        name = envelope.get("CASE_ID")
        if str(envelope.get("SOURCE_COMMIT", "")).lower() != source_commit:
            outcome.status = REJECTED
            outcome.reasons.append(f"{name}: envelope source commit is not the plan's (wrong runner)")
        if envelope.get("BINARY_SHA256") != staged.get(str(name)):
            outcome.status = REJECTED
            outcome.reasons.append(f"{name}: envelope PRX digest is not the staged fixture's")
        if envelope.get("FW") != expected_firmware:
            outcome.status = REJECTED
            outcome.reasons.append(f"{name}: envelope firmware {envelope.get('FW')!r} is not {expected_firmware!r}")
        started = envelope.get("HOST0_RUN_STARTED_NS")
        if (
            not isinstance(started, int)
            or isinstance(started, bool)
            or started < launch_started_ns - _runner.HOST0_MTIME_TOLERANCE_NS
        ):
            outcome.status = REJECTED
            outcome.reasons.append(f"{name}: envelope run window does not belong to this launch (stale)")
    if outcome.status == REJECTED:
        return outcome
    if case_ids != expected_ids:
        outcome.reasons.append(f"the runner stopped before {case.case_id}: "
                               f"{report.get('terminal_reason') or 'no terminal reason'}")
        return outcome
    for envelope in envelopes:
        if envelope.get("ACCEPTANCE_ELIGIBLE") is not True:
            blockers = envelope.get("ACCEPTANCE_BLOCKERS")
            outcome.reasons.append(f"{envelope.get('CASE_ID')}: not acceptance-eligible"
                                   + (f" ({'; '.join(map(str, blockers))})" if isinstance(blockers, list)
                                      and blockers else ""))
    if report.get("terminal_reason"):
        outcome.reasons.append(f"runner terminal reason {report['terminal_reason']}")
    if probe_exit != 0:
        outcome.reasons.append(f"probe command exited {probe_exit}")
    if not outcome.reasons:
        outcome.status = ACCEPTED
    return outcome


# ------------------------------------------------------------------------------------------ the matrix


CommandRunner = Callable[[list[str], float, "str | None"], tuple[int | None, str, str]]


def run_command(argv: list[str], timeout: float, stdin: str | None = None) -> tuple[int | None, str, str]:
    """Run one bounded host command without a shell; ``None`` means it was stopped at its timeout."""

    def text(value: bytes | None) -> str:
        return (value or b"").decode("utf-8", "replace")

    env = {**os.environ, "MSYS_NO_PATHCONV": "1"}
    try:
        # Bytes, not text mode: on Windows text mode would turn the script's LF line ends into CRLF, which
        # bash then reads as part of each command.
        result = subprocess.run(argv, input=None if stdin is None else stdin.encode("utf-8"),
                                capture_output=True, timeout=timeout, check=False, env=env)
    except subprocess.TimeoutExpired as exc:
        return None, text(exc.stdout), text(exc.stderr)  # type: ignore[arg-type]
    except OSError as exc:
        return 127, "", f"{type(exc).__name__}: {exc}"
    return result.returncode, text(result.stdout), text(result.stderr)


class MatrixRunner:
    def __init__(
        self,
        plan: MatrixPlan,
        *,
        command_runner: CommandRunner = run_command,
        lock_path: Path | None = None,
        clock_ns: Callable[[], int] = time.time_ns,
        source_tree_problem: Callable[[str], str | None] | None = None,
        git_common_dir: Callable[[Path], Path] | None = None,
    ) -> None:
        self.plan = plan
        self.run = command_runner
        self.lock_path = Path(lock_path) if lock_path is not None else _runner.HARDWARE_LOCK_PATH
        self.clock_ns = clock_ns
        self.source_tree_problem = source_tree_problem or _runner._check_source_tree
        self.git_common_dir = git_common_dir or _git_common_dir
        self.events: list[str] = []

    # -- inputs
    def _values(self) -> dict[str, str]:
        return {
            "session_id": self.plan.session_id,
            "host0_root": str(self.plan.host0_root),
            "host0_root_wsl": _runner._wsl_path(self.plan.host0_root),
            "repo_root": str(self.plan.repo_root),
        }

    def build(self) -> dict[str, str]:
        """Build and stage every PRX of the matrix; return {case_id: sha256} verified on the host side."""

        plan = self.plan
        plan.host0_root.mkdir(parents=True, exist_ok=True)
        to_build = [MatrixCase(PREFLIGHT_CASE, 0.0)] + [case for case in plan.cases if case.prebuilt is None]
        source_repo = plan.shell_path(self.git_common_dir(plan.repo_root))
        script = build_script(plan, source_repo, to_build)
        code, out, err = self.run(plan.commands["build_shell_argv"], BUILD_TIMEOUT_SECONDS, script)
        if code is None:
            raise MatrixError("build exceeded its time bound")
        try:
            reported = parse_build_output(out, [case.case_id for case in to_build])
        except MatrixError as exc:
            tail = " | ".join((out + err).strip().splitlines()[-6:])
            raise MatrixError(f"{exc} (build shell exit {code}: {tail[-400:]})") from exc
        if code != 0:
            raise MatrixError(f"build shell exited {code}: {err.strip()[-200:]}")
        staged: dict[str, str] = {}
        for case in to_build:
            digest = _sha256(plan.host0_root / case.staged_name)
            if digest != reported[case.case_id]:
                raise MatrixError(f"{case.case_id}: staged PRX digest differs from the one the build reported")
            staged[case.case_id] = digest
        for case in plan.cases:
            if case.prebuilt is None:
                continue
            try:
                digest = _sha256(case.prebuilt)
            except OSError as exc:
                raise MatrixError(f"{case.case_id}: prebuilt PRX unreadable ({type(exc).__name__})") from exc
            if digest != case.sha256:
                raise MatrixError(f"{case.case_id}: prebuilt PRX digest is not the pinned one")
            shutil.copyfile(case.prebuilt, plan.host0_root / case.staged_name)
            staged[case.case_id] = _sha256(plan.host0_root / case.staged_name)
            if staged[case.case_id] != case.sha256:
                raise MatrixError(f"{case.case_id}: staged copy differs from the pinned prebuilt PRX")
        return {name: staged[name] for name in [PREFLIGHT_CASE, *(case.case_id for case in plan.cases)]}

    def verify_staged(self, staged: dict[str, str]) -> None:
        for name, digest in staged.items():
            path = self.plan.host0_root / (name.replace("-", "_") + ".prx")
            try:
                current = _sha256(path)
            except OSError as exc:
                raise MatrixError(f"{name}: staged PRX is missing ({type(exc).__name__})") from exc
            if current != digest:
                raise MatrixError(f"{name}: staged PRX changed since it was built; rebuild the matrix")

    # -- checkpoint
    def _load_checkpoint(self) -> dict[str, object] | None:
        path = self.plan.checkpoint_path
        if not path.is_file():
            return None
        try:
            state = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise MatrixError(f"checkpoint unreadable ({type(exc).__name__})") from exc
        identity = {
            "matrix_id": self.plan.matrix_id,
            "source_commit": self.plan.source_commit,
            "session_id": self.plan.session_id,
            "cases": [case.case_id for case in self.plan.cases],
        }
        if not isinstance(state, dict) or any(state.get(key) != value for key, value in identity.items()):
            raise MatrixError("checkpoint belongs to a different matrix, commit, session or case list")
        if not isinstance(state.get("staged"), dict) or not isinstance(state.get("outcomes"), list):
            raise MatrixError("checkpoint is malformed")
        return state

    def _save(self, state: dict[str, object]) -> None:
        self.plan.evidence_dir.mkdir(parents=True, exist_ok=True)
        _runner._write_campaign_json(self.plan.checkpoint_path, state)

    # -- device steps
    def _gate(self, stage: str) -> None:
        problem = lock_holder_problem(_read_lock(self.lock_path), self.plan.session_id)
        if problem:
            raise MatrixError(f"{problem} ({stage})")

    def _recover(self, stage: str) -> tuple[int | None, str]:
        self._gate(stage)
        argv = render_argv(self.plan.commands["recover_argv"], self._values())
        code, _out, err = self.run(argv, RECOVER_COMMAND_TIMEOUT_SECONDS, None)
        self.events.append(f"{stage}: recover exit {code}")
        if code != 0:
            return None, f"recover command exited {code}: {err.strip()[-200:]}"
        boot, status = attested_boot(_read_lock(self.lock_path), self.plan.session_id)
        if boot is None:
            return None, f"no fresh qualified device-state attestation after recovery ({status})"
        return boot, status

    def fresh_epoch(self, used_boots: list[int]) -> tuple[int | None, str, int | None]:
        """Requalify, soft-reset once, requalify; return (new boot, detail, boot before the reset)."""

        before, detail = self._recover("before the soft reset")
        if before is None:
            return None, detail, None
        self._gate("before the soft reset")
        argv = render_argv(self.plan.commands["reset_argv"], self._values())
        code, _out, _err = self.run(argv, RESET_COMMAND_TIMEOUT_SECONDS, None)
        self.events.append(f"PSPLink soft reset from boot {before}: command exit {code} (a timeout is expected)")
        after, detail = self._recover("after the soft reset")
        if after is None:
            return None, detail, before
        newest = max([before, *used_boots])
        if after <= newest:
            return None, (f"the soft reset did not produce a newer qualified boot (attested boot {after}, "
                          f"newest seen {newest})"), before
        return after, "QUALIFIED", before

    def launch(self, case: MatrixCase, boot: int, index: int, staged: dict[str, str]) -> CaseOutcome:
        plan = self.plan
        tag = f"{index:02d}-{case.case_id}-boot{boot}"
        out = plan.host0_root / f"{plan.matrix_id}-{tag}.json"
        runner_log = plan.evidence_dir / f"{plan.matrix_id}-{tag}.runner.log"
        values = {**self._values(), "deadline": str(plan.watchdog_deadline), "runner_log": str(runner_log)}
        argv = render_argv(plan.commands["probe_argv"], values) + [
            "--model", plan.console_model,
            "--firmware", plan.expected_firmware,
            "--source-commit", plan.source_commit,
            "--timeout", f"{case.timeout:g}",
            "--session-id", plan.session_id,
            "--host0-root", str(plan.host0_root),
            "--pspsh-argv-json", json.dumps(plan.commands["pspsh_argv"]),
            "--usbhostfs-argv-json", json.dumps(plan.commands["usbhostfs_argv"]),
            "--campaign-case", f"{PREFLIGHT_CASE}={plan.host0_root / 'transport_write.prx'}",
            "--campaign-case", f"{case.case_id}={plan.host0_root / case.staged_name}",
            "--out", str(out),
        ]
        out.unlink(missing_ok=True)
        self._gate(f"before launching {case.case_id}")
        note_launch(self.lock_path, plan.session_id,
                    f"matrix {plan.matrix_id} case {index + 1}/{len(plan.cases)}: boot {boot}, launching "
                    f"transport-write + {case.case_id} ({plan.source_commit[:8]}, PRX sha256 "
                    f"{staged[case.case_id][:8]})")
        started = self.clock_ns()
        wall = 2 * (case.timeout + 120.0) + plan.watchdog_deadline + PROBE_RECOVERY_ALLOWANCE_SECONDS
        code, stdout, _err = self.run(argv, wall, None)
        return classify_launch(
            case=case,
            epoch_boot=boot,
            probe_exit=code,
            probe_stdout=stdout,
            runner_report_path=out,
            launch_started_ns=started,
            staged=staged,
            source_commit=plan.source_commit,
            expected_firmware=plan.expected_firmware,
        )

    # -- the whole matrix
    def execute(self, *, build_only: bool = False) -> tuple[int, dict[str, object]]:
        plan = self.plan
        problem = self.source_tree_problem(plan.source_commit)
        if problem:
            raise MatrixError(f"source checkout: {problem}")
        state = self._load_checkpoint()
        if state is None:
            staged = self.build()
            state = {
                "schema": 1,
                "matrix_id": plan.matrix_id,
                "source_commit": plan.source_commit,
                "session_id": plan.session_id,
                "cases": [case.case_id for case in plan.cases],
                "staged": staged,
                "used_boots": [],
                "outcomes": [],
                "contained_failures": 0,
                "state": "BUILT",
            }
            self._save(state)
        staged = dict(state["staged"])  # type: ignore[arg-type]
        self.verify_staged(staged)
        if build_only:
            return 0, self._report(state, "BUILT")
        outcomes: list[dict[str, object]] = list(state["outcomes"])  # type: ignore[arg-type]
        used_boots: list[int] = list(state.get("used_boots") or [])  # type: ignore[arg-type]
        contained = int(state.get("contained_failures") or 0)  # type: ignore[arg-type]
        terminal: str | None = None
        for index in range(len(outcomes), len(plan.cases)):
            case = plan.cases[index]
            if contained > plan.max_contained_failures:
                terminal = "CONTAINED_FAILURE_BUDGET_EXHAUSTED"
                break
            state["state"] = "IN_PROGRESS"
            boot, detail, before = self.fresh_epoch(used_boots)
            if before is not None and before not in used_boots:
                used_boots.append(before)
            if boot is None:
                outcome = CaseOutcome(case.case_id, INTERVENTION, [f"no fresh qualified boot: {detail}"])
                terminal = INTERVENTION
            else:
                used_boots.append(boot)
                outcome = self.launch(case, boot, index, staged)
                for seen in outcome.boots_seen:
                    if seen not in used_boots:
                        used_boots.append(seen)
                if outcome.status == CONTAINED:
                    contained += 1
                if outcome.status == INTERVENTION:
                    terminal = INTERVENTION
                try:
                    note_launch(self.lock_path, plan.session_id,
                                f"matrix {plan.matrix_id} case {index + 1}/{len(plan.cases)} {case.case_id} on boot "
                                f"{boot}: {outcome.status}"
                                + (f" ({'; '.join(outcome.reasons)[:300]})" if outcome.reasons else ""))
                except MatrixError as exc:
                    self.events.append(f"probes_run note after {case.case_id} not written: {exc}")
            outcomes.append(outcome.as_dict())
            state.update({"outcomes": outcomes, "used_boots": used_boots, "contained_failures": contained})
            self._save(state)
            if terminal:
                break
        final = terminal or "COMPLETE"
        state["state"] = final
        self._save(state)
        report = self._report(state, final)
        if terminal == INTERVENTION:
            return 3, report
        if terminal or any(item.get("status") != ACCEPTED for item in outcomes):
            return 2, report
        return 0, report

    def _report(self, state: dict[str, object], final: str) -> dict[str, object]:
        outcomes = list(state.get("outcomes") or [])  # type: ignore[arg-type]
        done = {item.get("case_id") for item in outcomes if isinstance(item, dict)}
        not_run = [case.case_id for case in self.plan.cases if case.case_id not in done]
        report = {
            "schema": 1,
            "matrix_id": self.plan.matrix_id,
            "source_commit": self.plan.source_commit,
            "console_model": self.plan.console_model,
            "expected_firmware": self.plan.expected_firmware,
            "state": final,
            "staged": state.get("staged"),
            "used_boots": state.get("used_boots"),
            "outcomes": outcomes,
            "not_run": [{"case_id": case_id, "status": NOT_RUN} for case_id in not_run],
            "accepted": [item.get("case_id") for item in outcomes
                         if isinstance(item, dict) and item.get("status") == ACCEPTED],
            "events": list(self.events),
        }
        self.plan.evidence_dir.mkdir(parents=True, exist_ok=True)
        _runner._write_campaign_json(self.plan.report_path, report)
        return report


def _git_common_dir(repo_root: Path) -> Path:
    result = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=repo_root,
                            capture_output=True, text=True, check=False)
    if result.returncode != 0 or not result.stdout.strip():
        raise MatrixError("could not locate the repository's Git directory")
    return Path(result.stdout.strip())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("plan", type=Path, help="private schema 1 matrix plan (JSON)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true",
                      help="validate the plan and the source checkout; build nothing, touch no device")
    mode.add_argument("--build-only", action="store_true",
                      help="build and stage the PRXs and record their digests; touch no device")
    args = parser.parse_args(argv)
    try:
        plan = load_plan(args.plan)
        if args.dry_run:
            problem = _runner._check_source_tree(plan.source_commit)
            if problem:
                raise MatrixError(f"source checkout: {problem}")
            print(_runner.dump_json({
                "status": "VALIDATED_OFFLINE",
                "matrix_id": plan.matrix_id,
                "cases": [case.case_id for case in plan.cases],
                "launches": len(plan.cases),
                "soft_resets": len(plan.cases),
                "hardware_started": False,
            }), end="")
            return 0
        code, report = MatrixRunner(plan).execute(build_only=args.build_only)
    except MatrixError as exc:
        print(_runner.dump_json({"status": "REFUSED", "reason": str(exc)}), end="")
        return 2
    print(_runner.dump_json(report), end="")
    return code


if __name__ == "__main__":
    sys.exit(main())
