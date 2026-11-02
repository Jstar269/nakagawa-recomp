# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""One-click local problem report bundle (#1849).

A user who hits a problem gets one ZIP they can attach to an issue themselves. The bundle holds
the player's own logs and settings, a summary of the flight recorder and the ``nk_doctor``
results, and ``about.txt`` with the versions and the machine. It is built from an **allow-list**:
only files this module names are ever opened, so nothing is "everything but a few exclusions".
Each file is then checked against the exclusion rules (game files, decrypted modules, saves, frame
or memory dumps, traces, keys and console-bound values) and must be plain text, and every line is
redacted: the user's home folder becomes ``~``, the account name ``<user>`` and the machine name
``<host>``.

Nothing here touches the network (AGENTS.md section 5): the module uses only the standard
library, the bundle is written under the user's data folder, and the caller opens that folder.
The preview (``BundlePlan.preview``) lists exactly the files that ``write_bundle`` will write,
because both read the same already-redacted bytes.
"""

from __future__ import annotations

import datetime as _dt
import getpass
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass, field
import zipfile

SCHEMA_VERSION = 1

#: The tail of a log kept when the log is longer, from its first whole line.
MAX_LOG_BYTES = 2 * 1024 * 1024
#: The largest settings or summary file taken.
MAX_SMALL_BYTES = 1024 * 1024
#: How many of a title's runtime logs the bundle keeps (the last run and the one before).
RUNTIME_LOG_COUNT = 2

#: Folder names whose contents are never taken, at any depth.
EXCLUDED_DIR_NAMES = frozenset({
    "saves", "savedata", "memstick", "ms0", "keys", "key", "decrypted", "discs", "oracle", "fs",
    "captures", "frames", "dumps", "traces", "flash0", "idstorage", "psp", "titles", "packages",
    "input_profiles",
})
#: File suffixes that are game data, dumps, traces or key material.
EXCLUDED_SUFFIXES = frozenset({
    ".iso", ".cso", ".zso", ".chd", ".cue", ".bin", ".img", ".pbp", ".prx", ".elf", ".self",
    ".bmp", ".png", ".jpg", ".jpeg", ".ppm", ".tga", ".dds", ".raw", ".pmem", ".mem", ".dmp",
    ".mdmp", ".dump", ".trace", ".tr", ".etl", ".key", ".keys", ".pem", ".pfx", ".p12", ".der",
    ".sav", ".dat", ".sfo", ".pgf", ".at3", ".atrac", ".pmf", ".mp4", ".wav", ".zip", ".7z",
    ".rar", ".gz", ".exe", ".dll", ".o", ".obj", ".so", ".psarc", ".db", ".sqlite", ".ghidra",
    ".gzf", ".rep",
})
#: Words in a file name that mark console-bound or key material.
EXCLUDED_NAME_WORDS = ("fuseid", "fuse_id", "secure_install", "secureinstall", "idstorage",
                       "licence", "license.rif", ".rif", "keyfile", "private_key", "secret",
                       "dump", "trace", "savedata", "memdump", "framedump", "keys.", "keystore")
#: Names a user account can have that are not a person.
GENERIC_PROFILE_NAMES = frozenset({"public", "default", "default user", "all users", "shared",
                                   "defaultuser0", "guest"})


class ReportError(Exception):
    """A named refusal; the message is for the person who asked for the report."""


# --------------------------------------------------------------------------------------------
# Redaction
# --------------------------------------------------------------------------------------------

_SEP = r"[\\/]+"
_WORD_AFTER = r"(?![A-Za-z0-9_\-])"
_USERS_DIR = re.compile(
    r"(?:\\\\\?\\|\\\\\.\\|file:/+)?"
    r"(?:(?:/mnt/[A-Za-z]|[A-Za-z]:))?"
    r"[\\/]+(?:Users|Documents and Settings)[\\/]+"
    r"(?P<name>[^\\/\s\"'<>|:*?,;(){}\[\]]+)",
    re.IGNORECASE)
_POSIX_HOME = re.compile(r"(?<![A-Za-z0-9_.\-])(?:/var)?/(?:home|Users)/(?P<name>[^/\s\"'<>|:*?,;(){}\[\]]+)")


@dataclass(frozen=True)
class Identity:
    """Who the report is about: the strings that must not survive in the bundle."""

    home_dirs: tuple[str, ...] = ()
    user_names: tuple[str, ...] = ()
    host_names: tuple[str, ...] = ()

    @classmethod
    def from_environment(cls) -> "Identity":
        homes: list[str] = []
        names: list[str] = []
        hosts: list[str] = []
        try:
            homes.append(str(Path.home()))
        except (RuntimeError, KeyError):
            pass
        for var in ("USERPROFILE", "HOME"):
            if os.environ.get(var):
                homes.append(os.environ[var])
        if os.environ.get("HOMEDRIVE") and os.environ.get("HOMEPATH"):
            homes.append(os.environ["HOMEDRIVE"] + os.environ["HOMEPATH"])
        try:
            names.append(getpass.getuser())
        except (KeyError, OSError, ImportError):
            pass
        for var in ("USERNAME", "USER", "LOGNAME"):
            if os.environ.get(var):
                names.append(os.environ[var])
        for var in ("COMPUTERNAME", "HOSTNAME"):
            if os.environ.get(var):
                hosts.append(os.environ[var])
        hosts.append(platform.node())
        return cls(tuple(dict.fromkeys(h for h in homes if h)),
                   tuple(dict.fromkeys(n for n in names if n)),
                   tuple(dict.fromkeys(h for h in hosts if h)))


def _path_regex(path: str) -> re.Pattern[str] | None:
    """A pattern for a folder in any spelling a log might hold: either slash, doubled slashes
    (JSON escapes), the ``\\\\?\\`` and ``file:///`` prefixes, a ``/mnt/c`` mount."""
    text = path.strip().rstrip("\\/")
    if len(text) < 3:
        return None
    flags = re.NOFLAG
    drive = re.match(r"^([A-Za-z]):[\\/]*", text)
    if drive:
        parts = [p for p in re.split(r"[\\/]+", text[drive.end():]) if p]
        head = (r"(?:\\\\\?\\|\\\\\.\\|file:/+|/)?(?:/mnt/)?" +
                re.escape(drive.group(1)) + r":" + (_SEP if parts else ""))
        flags = re.IGNORECASE
    else:
        parts = [p for p in re.split(r"[\\/]+", text) if p]
        head = r"(?<![A-Za-z0-9_.\-])" + (_SEP if text[0] in "\\/" else "")
    if not parts:
        return None
    body = _SEP.join(re.escape(p) for p in parts)
    return re.compile(head + body + _WORD_AFTER, flags)


def _name_regex(name: str) -> re.Pattern[str] | None:
    name = name.strip()
    if len(name) < 3:
        return None
    return re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])", re.IGNORECASE)


def redact_text(text: str, identity: Identity) -> str:
    """The text with the home folder as ``~``, the account name as ``<user>`` and the machine
    name as ``<host>``. The home folder is found both as the known value and by shape
    (``X:\\Users\\<name>``, ``/mnt/c/Users/<name>``, ``/home/<name>``, ``/Users/<name>``), so a
    report from another account on the same machine is covered too."""
    for home in sorted(identity.home_dirs, key=len, reverse=True):
        pattern = _path_regex(home)
        if pattern is not None:
            text = pattern.sub("~", text)

    def users_dir(match: re.Match[str]) -> str:
        return match.group(0) if match.group("name").lower() in GENERIC_PROFILE_NAMES else "~"

    text = _USERS_DIR.sub(users_dir, text)
    text = _POSIX_HOME.sub(users_dir, text)
    for name in sorted(identity.user_names, key=len, reverse=True):
        pattern = _name_regex(name)
        if pattern is not None:
            text = pattern.sub("<user>", text)
    for host in sorted(identity.host_names, key=len, reverse=True):
        pattern = _name_regex(host)
        if pattern is not None:
            text = pattern.sub("<host>", text)
    return text


# --------------------------------------------------------------------------------------------
# The exclusion rules
# --------------------------------------------------------------------------------------------

def exclusion_reason(path: Path | str) -> str | None:
    """Why a file is never put in a bundle, or None. Applied to every candidate."""
    parts = [p.lower() for p in re.split(r"[\\/]+", str(path)) if p]
    if not parts:
        return "empty path"
    for part in parts[:-1]:
        if part in EXCLUDED_DIR_NAMES:
            return f"inside a '{part}' folder"
    name = parts[-1]
    suffix = Path(name).suffix
    if suffix in EXCLUDED_SUFFIXES:
        return f"'{suffix}' files are game data, dumps, traces or keys"
    for word in EXCLUDED_NAME_WORDS:
        if word in name:
            return f"the name marks a dump, trace or key ('{word}')"
    return None


def looks_like_text(data: bytes) -> bool:
    """Plain text: no NUL byte and under 1% control characters. A planted binary renamed to
    ``.txt`` or ``.log`` is refused here."""
    if b"\x00" in data:
        return False
    if not data:
        return True
    control = sum(1 for b in data if b < 32 and b not in (9, 10, 13, 12))
    return control * 100 <= len(data)


def _decode(data: bytes) -> str:
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return data.decode("utf-16", errors="replace")
    return data.decode("utf-8", errors="replace")


def _tail(data: bytes, limit: int) -> tuple[bytes, int]:
    """The last ``limit`` bytes from the first whole line, and how many bytes were cut."""
    if len(data) <= limit:
        return data, 0
    cut = len(data) - limit
    newline = data.find(b"\n", cut)
    if newline == -1:
        return data[cut:], cut
    return data[newline + 1:], newline + 1


# --------------------------------------------------------------------------------------------
# The plan: what goes in, with the bytes already redacted
# --------------------------------------------------------------------------------------------

@dataclass(frozen=True)
class BundleEntry:
    arcname: str
    data: bytes
    note: str = ""


@dataclass
class BundlePlan:
    archive_name: str
    entries: list[BundleEntry] = field(default_factory=list)
    skipped: list[tuple[str, str]] = field(default_factory=list)

    def preview(self) -> list[dict[str, object]]:
        """Every file the bundle will hold, with its size, as the launcher shows before writing."""
        return [{"name": e.arcname, "bytes": len(e.data), "note": e.note} for e in self.entries]


@dataclass(frozen=True)
class ReportInputs:
    user_data_root: Path
    logs_dir: Path
    config_dir: Path
    title_id: str = ""
    title_name: str = ""
    region: str = ""
    flight_recorder: Path | None = None


def resolve_layout(user_data_root: Path, logs_dir: Path | None = None,
                   config_dir: Path | None = None) -> tuple[Path, Path]:
    """The logs and config folders. The player passes both; without them, the layout an
    ``--user-data-root`` gives (``<root>/logs``) or the Windows default (siblings of ``data``)."""
    def pick(given: Path | None, leaf: str) -> Path:
        if given is not None:
            return given
        inside = user_data_root / leaf
        return inside if inside.is_dir() else user_data_root.parent / leaf

    return pick(logs_dir, "logs"), pick(config_dir, "config")


_ID_OK = re.compile(r"^[A-Za-z0-9_\-]{1,32}$")


def _slug(text: str, fallback: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:48]
    return slug or fallback


def _read_allowed(path: Path, limit: int, plan: BundlePlan, label: str,
                  identity: Identity) -> BundleEntry | None:
    """Read one allow-listed file: refuse it by rule, require plain text, keep a tail, redact."""
    # The rules judge the name inside the bundle, not the folders above the source file.
    reason = exclusion_reason(label)
    if reason:
        plan.skipped.append((label, reason))
        return None
    try:
        raw = path.read_bytes()
    except OSError as exc:
        plan.skipped.append((label, f"could not be read ({exc.strerror or exc})"))
        return None
    if not looks_like_text(raw):
        plan.skipped.append((label, "is not plain text"))
        return None
    raw, cut = _tail(raw, limit)
    text = redact_text(_decode(raw), identity)
    note = f"first {cut} bytes left out" if cut else ""
    if cut:
        text = f"[{cut} earlier bytes left out]\n" + text
    return BundleEntry(label, text.encode("utf-8"), note)


_SENSITIVE_KEY = re.compile(r"(token|secret|password|passwd|credential|api[_-]?key|private)", re.I)


def _scrub_json(value: object) -> object:
    if isinstance(value, dict):
        return {k: ("<removed>" if _SENSITIVE_KEY.search(str(k)) and not isinstance(v, (dict, list))
                    else _scrub_json(v)) for k, v in value.items()}
    if isinstance(value, list):
        return [_scrub_json(v) for v in value]
    return value


def flight_recorder_summary(path: Path, identity: Identity) -> str:
    """A summary of a flight-recorder bundle: its scalar fields and the size of each list or
    table, never the memory, register or event contents."""
    data = json.loads(path.read_bytes().decode("utf-8", errors="replace"))
    summary: dict[str, object] = {"file": path.name, "bytes": path.stat().st_size}
    if isinstance(data, dict):
        for key, value in data.items():
            if isinstance(value, (str, int, float, bool)) or value is None:
                summary[key] = value
            elif isinstance(value, (list, dict)):
                summary[f"{key}_count"] = len(value)
    else:
        summary["note"] = "the file is not a JSON object"
    return redact_text(json.dumps(_scrub_json(summary), indent=2, sort_keys=True), identity)


def _newest(paths: Iterable[Path]) -> list[Path]:
    return sorted(paths, key=lambda p: (p.stat().st_mtime, p.name), reverse=True)


def runtime_logs(logs_dir: Path, title_id: str) -> list[Path]:
    """The last two runtime logs for the title (``game-<id>-last.txt`` and ``-prev.txt``), or,
    with no title, the two newest runtime logs of any title."""
    if title_id:
        names = [f"game-{title_id}-last.txt", f"game-{title_id}-prev.txt"]
        return [logs_dir / n for n in names if (logs_dir / n).is_file()]
    found = [p for p in logs_dir.glob("game-*-last.txt") if p.is_file()]
    return _newest(found)[:RUNTIME_LOG_COUNT]


def settings_files(config_dir: Path) -> list[Path]:
    """The settings files: the ``.json`` files directly in the config folder."""
    if not config_dir.is_dir():
        return []
    return sorted(p for p in config_dir.glob("*.json") if p.is_file())


def run_doctor(project_root: Path, timeout: float = 90.0) -> str:
    """``tools/nk_doctor.py --json`` output, or a line saying why there is none."""
    script = project_root / "tools" / "nk_doctor.py"
    if not script.is_file():
        return "nk_doctor is not installed beside this player; no results."
    try:
        done = subprocess.run([sys.executable, str(script), "--json"], capture_output=True,
                              text=True, timeout=timeout, cwd=str(project_root), check=False)
    except (OSError, subprocess.SubprocessError) as exc:
        return f"nk_doctor could not run: {exc}"
    text = done.stdout.strip()
    return text if text else f"nk_doctor printed nothing (exit {done.returncode})."


def _memory_bytes() -> int | None:
    try:
        if sys.platform == "win32":
            import ctypes

            class Status(ctypes.Structure):
                _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong),
                            ("total", ctypes.c_ulonglong), ("avail", ctypes.c_ulonglong),
                            ("tpage", ctypes.c_ulonglong), ("apage", ctypes.c_ulonglong),
                            ("tvirt", ctypes.c_ulonglong), ("avirt", ctypes.c_ulonglong),
                            ("ext", ctypes.c_ulonglong)]

            status = Status()
            status.length = ctypes.sizeof(Status)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):  # type: ignore[attr-defined]
                return int(status.total)
            return None
        if sys.platform.startswith("linux"):
            for line in Path("/proc/meminfo").read_text(encoding="ascii", errors="replace").splitlines():
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) * 1024
        elif hasattr(os, "sysconf"):
            return int(os.sysconf("SC_PAGE_SIZE")) * int(os.sysconf("SC_PHYS_PAGES"))
    except (OSError, ValueError, AttributeError, KeyError):
        return None
    return None


def _gpu_lines() -> list[str]:
    """The video adapters and drivers the OS reports, best effort and read-only."""
    try:
        if sys.platform == "win32":
            command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                       "Get-CimInstance Win32_VideoController | ForEach-Object "
                       "{ $_.Name + ' | driver ' + $_.DriverVersion }"]
        elif sys.platform.startswith("linux"):
            command = ["lspci"]
        else:
            return []
        done = subprocess.run(command, capture_output=True, text=True, timeout=20, check=False)
    except (OSError, subprocess.SubprocessError):
        return []
    lines = [ln.strip() for ln in done.stdout.splitlines() if ln.strip()]
    if sys.platform.startswith("linux"):
        lines = [ln for ln in lines if re.search(r"VGA|3D|Display", ln)]
    return lines[:4]


def _project_identity(project_root: Path) -> tuple[str, str]:
    """The Nakagawa version and source commit, best effort."""
    version = "unknown"
    header = project_root / "src" / "player" / "ui" / "player_ui_nk.h"
    try:
        match = re.search(r'#define\s+NK_PLAYER_VERSION\s+"([^"]+)"', header.read_text(encoding="utf-8"))
        if match:
            version = match.group(1)
    except OSError:
        pass
    commit = "unknown"
    try:
        done = subprocess.run(["git", "-C", str(project_root), "rev-parse", "--short=12", "HEAD"],
                              capture_output=True, text=True, timeout=10, check=False)
        if done.returncode == 0 and done.stdout.strip():
            commit = done.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return version, commit


def _tools_version(project_root: Path) -> str:
    try:
        match = re.search(r'^version\s*=\s*"([^"]+)"', (project_root / "pyproject.toml").read_text(encoding="utf-8"), re.M)
        return match.group(1) if match else "unknown"
    except OSError:
        return "unknown"


def build_about(inputs: ReportInputs, entries: Sequence[BundleEntry], project_root: Path,
                now: _dt.datetime, gpu: Callable[[], list[str]] = _gpu_lines,
                memory: Callable[[], int | None] = _memory_bytes) -> str:
    """``about.txt``: versions, machine, title, package state and the file list with times."""
    version, commit = _project_identity(project_root)
    ram = memory()
    lines = [
        "Nakagawa Recomp problem report",
        f"Made: {now.strftime('%Y-%m-%d %H:%M:%SZ')} on this PC. Nothing was uploaded.",
        "Redacted: your home folder is shown as ~, your account name as <user> and this PC's",
        "name as <host>. Game files, saves, dumps, traces and keys are never included.",
        "",
        f"Report format: {SCHEMA_VERSION}",
        f"Nakagawa version: {version}",
        f"Source commit: {commit}",
        f"Tools version: {_tools_version(project_root)}",
        f"Python: {platform.python_version()}",
        f"OS: {platform.platform()}",
        f"CPU: {platform.processor() or platform.machine() or 'unknown'}",
        f"RAM: {f'{ram // (1024 * 1024)} MiB' if ram else 'unknown'}",
    ]
    adapters = gpu()
    lines.append("GPU and driver: " + (adapters[0] if adapters else "unknown"))
    lines.extend("                " + extra for extra in adapters[1:])
    lines += [
        f"Title id: {inputs.title_id or 'none chosen'}",
        f"Title name: {inputs.title_name or 'none chosen'}",
        f"Region: {inputs.region or 'unknown'}",
        f"Package state: {package_state(inputs)}",
        "",
        "Files in this report:",
    ]
    lines.extend(f"  {e.arcname}  ({len(e.data)} bytes){('  ' + e.note) if e.note else ''}" for e in entries)
    return "\n".join(lines) + "\n"


def package_state(inputs: ReportInputs) -> str:
    """Whether the title has a built package and a staged game, by name only."""
    if not inputs.title_id:
        return "no title chosen"
    root = inputs.user_data_root
    built = any((root / "packages").glob(f"{inputs.title_id}*")) if (root / "packages").is_dir() else False
    staged = (root / "titles" / inputs.title_id).is_dir()
    return f"package {'built' if built else 'not built'}; game {'staged' if staged else 'not staged'}"


def build_plan(inputs: ReportInputs, identity: Identity, project_root: Path,
               now: _dt.datetime | None = None,
               doctor: Callable[[Path], str] = run_doctor,
               gpu: Callable[[], list[str]] = _gpu_lines,
               memory: Callable[[], int | None] = _memory_bytes) -> BundlePlan:
    """Collect, check and redact everything the report will hold; write nothing."""
    now = now or _dt.datetime.now(_dt.timezone.utc)
    if inputs.title_id and not _ID_OK.match(inputs.title_id):
        raise ReportError("The title id may hold only letters, digits, '-' and '_' (at most 32).")
    stamp = now.strftime("%Y%m%dT%H%M%SZ")
    slug = _slug(inputs.title_name or inputs.title_id, "launcher")
    plan = BundlePlan(archive_name=f"{stamp}-{slug}.zip")

    def take(path: Path, arcname: str, limit: int) -> None:
        entry = _read_allowed(path, limit, plan, arcname, identity)
        if entry is not None:
            plan.entries.append(entry)

    logs = inputs.logs_dir
    if logs.is_dir():
        for path in runtime_logs(logs, inputs.title_id):
            take(path, f"logs/{path.name}", MAX_LOG_BYTES)
        for name in ("player-output.txt", "developer-log.txt"):
            if (logs / name).is_file():
                take(logs / name, f"logs/{name}", MAX_LOG_BYTES)
    for path in settings_files(inputs.config_dir):
        entry = _read_allowed(path, MAX_SMALL_BYTES, plan, f"settings/{path.name}", identity)
        if entry is None:
            continue
        try:
            scrubbed = json.dumps(_scrub_json(json.loads(entry.data.decode("utf-8"))), indent=2, sort_keys=True)
            entry = BundleEntry(entry.arcname, (scrubbed + "\n").encode("utf-8"), entry.note)
        except ValueError:
            pass  # not valid JSON: it stays as redacted text, which is how it reads for a person
        plan.entries.append(entry)

    recorder = inputs.flight_recorder
    if recorder is None and logs.is_dir():
        found = _newest(logs.glob("flight-recorder*.json"))
        recorder = found[0] if found else None
    if recorder is not None and recorder.is_file():
        reason = exclusion_reason(recorder.name)
        if reason:
            plan.skipped.append(("flight-recorder summary", reason))
        else:
            try:
                summary = flight_recorder_summary(recorder, identity)
                plan.entries.append(BundleEntry("flight-recorder-summary.json", (summary + "\n").encode("utf-8"),
                                                "scalar fields and sizes only"))
            except (OSError, ValueError) as exc:
                plan.skipped.append(("flight-recorder summary", f"could not be summarised ({exc})"))

    doctor_text = redact_text(doctor(project_root), identity)
    plan.entries.append(BundleEntry("nk_doctor.txt", (doctor_text.rstrip() + "\n").encode("utf-8")))
    about = build_about(inputs, plan.entries, project_root, now, gpu, memory)
    plan.entries.insert(0, BundleEntry("about.txt", redact_text(about, identity).encode("utf-8")))
    return plan


def write_bundle(plan: BundlePlan, output_dir: Path,
                 now: _dt.datetime | None = None) -> Path:
    """Write the plan's files as one ZIP in ``output_dir`` (made if missing). The archive is
    written beside its final name and renamed, so a failed write leaves no half-made report."""
    now = now or _dt.datetime.now(_dt.timezone.utc)
    stamp = (max(now.year, 1980), now.month, now.day, now.hour, now.minute, now.second - now.second % 2)
    try:
        output_dir.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise ReportError(f"The reports folder could not be made: {exc.strerror or exc}.") from exc
    final = output_dir / plan.archive_name
    partial = output_dir / (plan.archive_name + ".partial")
    try:
        with zipfile.ZipFile(partial, "w", zipfile.ZIP_DEFLATED) as archive:
            for entry in plan.entries:
                info = zipfile.ZipInfo(entry.arcname, date_time=stamp)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                archive.writestr(info, entry.data)
        os.replace(partial, final)
    except OSError as exc:
        try:
            partial.unlink()
        except OSError:
            pass
        raise ReportError(f"The report could not be written: {exc.strerror or exc}.") from exc
    return final
