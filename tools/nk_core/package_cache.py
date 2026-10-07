# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Content-addressed private AOT package cache contract."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import sysconfig
import tempfile
from typing import Any, Mapping

CACHE_FORMAT = "nakagawa-aot-cache"
COMPLETION_FORMAT = "nakagawa-aot-cache-completion"
CACHE_SCHEMA_VERSION = 2
COMPLETION_SCHEMA_VERSION = 2
TITLE_INPUT_IDENTITY_FORMAT = "nakagawa-title-input-identity"
TITLE_INPUT_IDENTITY_SCHEMA_VERSION = 2
ANALYZER_CODEGEN_SEMANTICS_EPOCH = "analyzer-codegen-v1"
GENERATED_CODE_ABI_EPOCH = 1
RUNTIME_ABI_EPOCH = 1
RUNTIME_ABI_COMPATIBILITY: dict[int, frozenset[int]] = {
    1: frozenset({1}),
}
CACHE_ROOT_PARTS = ("cache", "packages")
COMPLETION_MANIFEST = "completion-manifest.json"
DEFAULT_MAX_CACHE_ENTRIES = 8
CACHE_LIMIT_ENV = "NK_AOT_CACHE_MAX_ENTRIES"
LOCAL_IDENTITIES_DIR = "title-input-identities"


class PackageCacheError(ValueError):
    """Named failure in the private package cache contract."""


@dataclass(frozen=True)
class CacheDecision:
    action: str
    generated_c_reusable: bool
    native_objects_reusable: bool
    reasons: tuple[str, ...]

    @property
    def rebuild_components(self) -> tuple[str, ...]:
        return self.reasons


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n"


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _digest(value: Any) -> str:
    return sha256_bytes(canonical_json(value).encode("utf-8"))


def build_title_input_identity(
    *,
    manifest: Mapping[str, Any],
    executable_name: str,
    executable_sha256: str,
    modules: list[Mapping[str, Any]],
    psp_header_sha256: str | None = None,
    psp_header_magic: str | None = None,
    disc_id: str | None = None,
    region: str | None = None,
    disc_version: str | None = None,
    param_sfo: Mapping[str, Any] | None = None,
    container: Mapping[str, Any] | None = None,
    source_media: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Build the private, versioned identity record for one package input set."""
    module_records = [
        {"name": item.get("name"), "sha256": item.get("sha256")}
        for item in sorted(modules, key=lambda item: str(item.get("name", "")))
    ]
    identity = {
        "format": TITLE_INPUT_IDENTITY_FORMAT,
        "schema_version": TITLE_INPUT_IDENTITY_SCHEMA_VERSION,
        "manifest": {
            "id": manifest.get("id"),
            "schema_version": manifest.get("schema_version"),
        },
        "disc": {
            "id": disc_id.upper() if isinstance(disc_id, str) else None,
            "region": region,
            "disc_version": disc_version,
        },
        "param_sfo": dict(param_sfo) if param_sfo is not None else None,
        "container": dict(container) if container is not None else None,
        "main_executable": {"name": executable_name, "sha256": executable_sha256},
        "modules": module_records,
        "psp_header": (
            {"sha256": psp_header_sha256, "magic": psp_header_magic}
            if psp_header_sha256 is not None else None
        ),
        "source_media": dict(source_media) if source_media is not None else None,
    }
    validate_title_input_identity(identity)
    return identity


def validate_title_input_identity(value: Any) -> dict[str, Any]:
    """Validate the local identity schema without exposing or logging hashes."""
    root_keys = {
        "format", "schema_version", "manifest", "disc", "param_sfo",
        "container", "main_executable", "modules", "psp_header", "source_media",
    }
    if not isinstance(value, dict) or set(value) != root_keys:
        raise PackageCacheError("title input identity fields do not match schema v2")
    if value.get("format") != TITLE_INPUT_IDENTITY_FORMAT or value.get("schema_version") != TITLE_INPUT_IDENTITY_SCHEMA_VERSION:
        raise PackageCacheError("title input identity format or schema is unsupported")
    manifest = value.get("manifest")
    if not isinstance(manifest, dict) or set(manifest) != {"id", "schema_version"}:
        raise PackageCacheError("title input identity manifest record is invalid")
    if not isinstance(manifest.get("id"), str) or not manifest["id"]:
        raise PackageCacheError("title input identity manifest ID is invalid")
    if type(manifest.get("schema_version")) is not int or manifest["schema_version"] < 1:
        raise PackageCacheError("title input identity manifest schema is invalid")
    disc = value.get("disc")
    if not isinstance(disc, dict) or set(disc) != {"id", "region", "disc_version"}:
        raise PackageCacheError("title input identity disc record is invalid")
    disc_id = disc.get("id")
    if disc_id is not None and (
        not isinstance(disc_id, str) or re.fullmatch(r"[A-Z]{4}[0-9]{5}", disc_id) is None
    ):
        raise PackageCacheError("title input identity DISC_ID is invalid")
    region = disc.get("region")
    if region is not None and region not in {"JP", "NA", "EU", "KR", "ASIA", "OTHER", "TEST", "HOMEBREW"}:
        raise PackageCacheError("title input identity region is invalid")
    version = disc.get("disc_version")
    if version is not None and (not isinstance(version, str) or not version or len(version) > 32):
        raise PackageCacheError("title input identity DISC_VERSION is invalid")
    sfo = value.get("param_sfo")
    if sfo is not None:
        if not isinstance(sfo, dict) or set(sfo) - {
            "DISC_ID", "TITLE", "DISC_VERSION", "APP_VER", "PSP_SYSTEM_VER", "CATEGORY",
        }:
            raise PackageCacheError("title input identity PARAM.SFO facts are invalid")
        if any(not isinstance(item, str) or len(item) > 256 for item in sfo.values()):
            raise PackageCacheError("title input identity PARAM.SFO fact is invalid")
    container = value.get("container")
    if container is not None:
        if not isinstance(container, dict) or set(container) != {
            "format", "volume_id", "size_bytes", "pvd_sector", "sector_size",
        }:
            raise PackageCacheError("title input identity container metadata is invalid")
        if not isinstance(container.get("format"), str) or not isinstance(container.get("volume_id"), str):
            raise PackageCacheError("title input identity container label is invalid")
        for field in ("size_bytes", "pvd_sector", "sector_size"):
            if type(container.get(field)) is not int or container[field] < 0:
                raise PackageCacheError(f"title input identity container {field} is invalid")
    executable = value.get("main_executable")
    if not isinstance(executable, dict) or set(executable) != {"name", "sha256"}:
        raise PackageCacheError("title input identity main executable record is invalid")
    if not isinstance(executable.get("name"), str) or not executable["name"]:
        raise PackageCacheError("title input identity main executable name is invalid")
    _input_sha(executable, "title input identity main executable")
    modules = value.get("modules")
    if not isinstance(modules, list):
        raise PackageCacheError("title input identity modules must be an array")
    names: set[str] = set()
    for item in modules:
        if not isinstance(item, dict) or set(item) != {"name", "sha256"}:
            raise PackageCacheError("title input identity module record is invalid")
        name = item.get("name")
        if not isinstance(name, str) or not name or name in names:
            raise PackageCacheError("title input identity module name is invalid or repeated")
        names.add(name)
        _input_sha(item, f"title input identity module {name}")
    header = value.get("psp_header")
    if header is not None:
        if not isinstance(header, dict) or set(header) != {"sha256", "magic"}:
            raise PackageCacheError("title input identity PSP header record is invalid")
        _input_sha(header, "title input identity PSP header")
        if header.get("magic") is not None and not isinstance(header["magic"], str):
            raise PackageCacheError("title input identity PSP header magic is invalid")
    source_media = value.get("source_media")
    if source_media is not None:
        if not isinstance(source_media, dict) or set(source_media) != {"executable", "modules"}:
            raise PackageCacheError("title input identity source media record is invalid")
        if executable["name"] not in {"EBOOT.BIN", "BOOT.BIN"}:
            raise PackageCacheError("title input identity source executable name is unsupported")
        executable_source = source_media.get("executable")
        if not isinstance(executable_source, dict) or set(executable_source) != {"path", "sha256"}:
            raise PackageCacheError("title input identity source executable record is invalid")
        executable_path = _validate_source_iso_path(
            executable_source.get("path"), "title input identity source executable path"
        )
        expected_executable_path = f"PSP_GAME/SYSDIR/{executable['name']}"
        if executable_path.casefold() != expected_executable_path.casefold():
            raise PackageCacheError("title input identity source executable path does not match its name")
        _input_sha(executable_source, "title input identity source executable")
        source_modules = source_media.get("modules")
        if not isinstance(source_modules, list):
            raise PackageCacheError("title input identity source modules must be an array")
        source_module_names: set[str] = set()
        for item in source_modules:
            if not isinstance(item, dict) or set(item) != {"name", "path", "sha256"}:
                raise PackageCacheError("title input identity source module record is invalid")
            name = item.get("name")
            if not isinstance(name, str) or name not in names or name in source_module_names:
                raise PackageCacheError("title input identity source module name is invalid or repeated")
            source_module_names.add(name)
            _validate_source_iso_path(item.get("path"), f"title input identity source module {name} path")
            _input_sha(item, f"title input identity source module {name}")
    return value


def _validate_source_iso_path(value: Any, field: str) -> str:
    """Accept only an ISO-relative PSP_GAME path, never a host path."""
    if (not isinstance(value, str) or not value or len(value) > 512 or
        "\\" in value or ":" in value or value.startswith("/") or
        any(ord(character) < 0x20 or ord(character) == 0x7F for character in value)):
        raise PackageCacheError(f"{field} is invalid")
    parts = value.split("/")
    if (len(parts) < 3 or parts[0].casefold() != "psp_game" or
        any(part in {"", ".", ".."} for part in parts)):
        raise PackageCacheError(f"{field} is invalid")
    return value


def title_input_identity_digest(value: Any) -> str:
    return _digest(validate_title_input_identity(value))


def title_input_identity_changes(previous: Any, current: Any) -> tuple[str, ...]:
    """Return human-readable identity classes; never include hashes or source bytes."""
    try:
        old = validate_title_input_identity(previous)
        new = validate_title_input_identity(current)
    except PackageCacheError:
        return ("identity record changed",)
    changes: list[str] = []
    if old["manifest"] != new["manifest"]:
        changes.append("manifest/profile changed")
    if old["disc"]["id"] != new["disc"]["id"]:
        changes.append("DISC_ID changed")
    if old["disc"]["region"] != new["disc"]["region"]:
        changes.append("region changed")
    if (old["disc"]["disc_version"] != new["disc"]["disc_version"] or
        (old["param_sfo"] or {}).get("DISC_VERSION") != (new["param_sfo"] or {}).get("DISC_VERSION") or
        (old["param_sfo"] or {}).get("APP_VER") != (new["param_sfo"] or {}).get("APP_VER")):
        changes.append("SFO revision changed")
    if old["param_sfo"] != new["param_sfo"] and "SFO revision changed" not in changes:
        changes.append("PARAM.SFO facts changed")
    if old["main_executable"] != new["main_executable"]:
        changes.append("main executable changed")
    old_modules = {item["name"]: item["sha256"] for item in old["modules"]}
    new_modules = {item["name"]: item["sha256"] for item in new["modules"]}
    for name in sorted(set(old_modules) | set(new_modules)):
        if old_modules.get(name) != new_modules.get(name):
            changes.append(f"module {name} changed")
    if old["container"] != new["container"]:
        changes.append("container metadata changed")
    if old["psp_header"] != new["psp_header"]:
        changes.append("PSP header changed")
    old_source = old["source_media"]
    new_source = new["source_media"]
    if (old_source is None) != (new_source is None):
        changes.append("source media binding changed")
    elif old_source is not None and new_source is not None:
        if old_source["executable"] != new_source["executable"]:
            changes.append("source executable changed")
        old_source_modules = {item["name"]: item for item in old_source["modules"]}
        new_source_modules = {item["name"]: item for item in new_source["modules"]}
        for name in sorted(set(old_source_modules) | set(new_source_modules)):
            if old_source_modules.get(name) != new_source_modules.get(name):
                changes.append(f"source module {name} changed")
    return tuple(changes)


def local_title_input_identity_path(user_data_root: Path | str, disc_id: str) -> Path:
    if re.fullmatch(r"[A-Z]{4}[0-9]{5}", disc_id or "") is None:
        raise PackageCacheError("title input identity DISC_ID is invalid")
    return Path(user_data_root).expanduser().resolve(strict=False) / LOCAL_IDENTITIES_DIR / disc_id / "title-input-identity.json"


def write_local_title_input_identity(
    user_data_root: Path | str, identity: Mapping[str, Any]
) -> Path:
    normalized = validate_title_input_identity(dict(identity))
    disc_id = normalized["disc"]["id"]
    if not disc_id:
        raise PackageCacheError("synthetic package identity has no user ISO record")
    user_root = Path(user_data_root).expanduser().resolve(strict=False)
    repository_root = Path(__file__).resolve().parents[2]
    if not cache_root_is_private(user_root, repository_root):
        raise PackageCacheError("title input identity must be stored outside the public repository")
    destination = local_title_input_identity_path(user_root, disc_id)
    for directory in (destination.parent.parent, destination.parent):
        if directory.is_symlink():
            raise PackageCacheError("title input identity directory is a symlink")
    destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if user_root not in destination.parent.resolve(strict=True).parents:
        raise PackageCacheError("title input identity path escaped the user data directory")
    _atomic_write(destination, canonical_json(normalized).encode("utf-8"))
    return destination


def read_local_title_input_identity(user_data_root: Path | str, disc_id: str) -> dict[str, Any] | None:
    path = local_title_input_identity_path(user_data_root, disc_id)
    if not path.is_file() or path.is_symlink():
        return None
    try:
        return validate_title_input_identity(_read_json(path))
    except (OSError, ValueError, PackageCacheError):
        return None  # malformed or hostile identity input is simply absent


# Explicit resource ceilings for externally supplied JSON artifacts on the
# package/cache route (#319). The byte ceiling is enforced with a bounded read
# through one open handle -- never a whole-file read -- so a hostile or corrupt
# file cannot be ingested wholesale before it is rejected. The depth/count scan
# runs on the raw text before json.loads, so pathological nesting cannot first
# explode inside the standard parser. The limits exceed any legitimate package,
# cache, completion, or identity document by orders of magnitude; build-report
# has its own, larger set below.
MAX_CACHE_JSON_BYTES = 1024 * 1024
MAX_CACHE_JSON_DEPTH = 32
MAX_CACHE_JSON_MEMBERS = 16384
MAX_CACHE_JSON_ITEMS = 16384
MAX_CACHE_JSON_NODES = 65536
# build-report.json is the one package artifact whose bulk scales with the
# analysed title: it carries one record per AOT gap, unsupported instruction and
# analysis diagnostic, and a measured flagship report is about 1.3 MB. It
# therefore reads under its own ceilings instead of the shared ones above,
# while package.json, completion-manifest.json and title-input-identity.json
# keep the 1 MiB bound. The reader stays bounded -- only the four numbers below
# differ.
#
# Members and separators are counted per object and per document rather than
# per parse, so the shared 16384 ceilings bind before the shared node ceiling
# does: a compact report of AOT-gap records reaches the separator ceiling while
# still under 65536 nodes, and node counts alone therefore cannot size it. Both
# are set equal to the build-report node ceiling here, which is the coarsest
# sound choice: every object member and every comma separator in valid JSON
# corresponds to at least one structural node, so these two remain bounds that
# cannot reject a document the node ceiling already accepted.
MAX_BUILD_REPORT_JSON_BYTES = 4 * 1024 * 1024
MAX_BUILD_REPORT_JSON_MEMBERS = 262144
MAX_BUILD_REPORT_JSON_ITEMS = 262144
MAX_BUILD_REPORT_JSON_NODES = 262144
# Keep decimal conversion bounded even when Python's process-wide guard is
# disabled. This matches the usual 4,300-digit Python integer ceiling.
MAX_CACHE_JSON_INTEGER_DIGITS = 4300


class BoundedJsonError(ValueError):
    """Named, controlled failure while reading a bounded external JSON artifact."""


# Diagnostics are attacker-reachable: every key and scalar in an externally
# supplied artifact is attacker-chosen text, and a rejection message is usually
# the one thing that gets logged or echoed back to a user. Bounding the artifact
# therefore does not by itself bound the message it produces, so untrusted
# values are quoted only as a short prefix and untrusted name lists only as a
# short sample. Rejection itself is unchanged; only the echoed detail is.
MAX_JSON_ECHO_CHARS = 64
MAX_JSON_ECHO_FIELDS = 8


def bounded_echo(value: Any, *, max_chars: int = MAX_JSON_ECHO_CHARS) -> str:
    """Render one untrusted value for a message without echoing bulk data.

    Strings are truncated before any quoting, so a multi-hundred-kilobyte key
    cannot inflate an exception message (or the log record that captures it)
    past ``max_chars``. Non-strings are rendered with ``repr`` and truncated
    afterwards; that transient repr is bounded by the artifact ceiling already
    accepted by the reader, so it is not a new amplification path.
    """
    if type(value) is str:
        if len(value) <= max_chars:
            return value
        return value[:max_chars] + "...[truncated]"
    text = repr(value)
    if len(text) <= max_chars:
        return text
    return text[:max_chars] + "...[truncated]"


def bounded_echo_fields(names: Any, *, max_chars: int = MAX_JSON_ECHO_CHARS) -> str:
    """Render a set of untrusted field names as a short, fixed-size sample."""
    ordered = sorted(names)
    shown = ", ".join(bounded_echo(name, max_chars=max_chars) for name in ordered[:MAX_JSON_ECHO_FIELDS])
    if len(ordered) > MAX_JSON_ECHO_FIELDS:
        shown += f", ... (+{len(ordered) - MAX_JSON_ECHO_FIELDS} more)"
    return shown


def _bounded_json_bytes(path: Path, max_bytes: int) -> bytes:
    """Read at most ``max_bytes`` (+1) bytes of ``path`` through one handle.

    ``fstat`` and the reads share the descriptor, so a replacement or growth
    race cannot substitute a different file behind the size gate. The extra
    byte detects a file that grew while it was being read; nothing is ever
    truncated silently.
    """
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0)
    fd = os.open(path, flags)
    try:
        size = os.fstat(fd).st_size
        if size > max_bytes:
            raise BoundedJsonError(
                f"{path} is {size} bytes, over the {max_bytes}-byte JSON artifact limit"
            )
        chunks: list[bytes] = []
        remaining = max_bytes + 1
        while remaining > 0:
            chunk = os.read(fd, remaining)
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
    finally:
        os.close(fd)
    if len(data) > max_bytes:
        raise BoundedJsonError(
            f"{path} grew past the {max_bytes}-byte JSON artifact limit"
        )
    return data


def _bounded_json_scan(
    text: str,
    *,
    max_depth: int,
    max_members: int,
    max_items: int,
    max_nodes: int,
) -> None:
    """Count containers, members, comma separators, and scalars in raw JSON text.

    Runs before json.loads so hostile nesting or bulk fails at the named
    ceiling instead of first recursing inside the standard parser. Strings and
    escapes are skipped with a state machine; counts are conservative upper
    bounds, so a document that passes can never exceed the ceilings after
    parsing either. The node count is re-checked after the loop because the
    in-string branch continues before the per-character test, which would
    otherwise let a document whose final node is a string closer finish one
    node past the ceiling.
    """
    depth = 0
    members = 0
    separators = 0
    nodes = 0
    in_string = False
    escaped = False
    # Tracks whether the previous character can continue a number, so a number
    # is counted once at its leading character. A boolean rather than a
    # sentinel character: "" is a substring of every string, so a ""-seeded
    # "not previously numeric" test would silently never fire for a document
    # that starts with a digit.
    prev_numeric = False
    for char in text:
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
                nodes += 1
            continue
        if char == '"':
            in_string = True
        elif char in "{[":
            depth += 1
            nodes += 1
            if depth > max_depth:
                raise BoundedJsonError(
                    f"JSON nesting exceeds {max_depth} levels"
                )
        elif char in "}]":
            depth -= 1
        elif char == ":":
            members += 1
        elif char == ",":
            separators += 1
        elif char in "-0123456789":
            if not prev_numeric:
                nodes += 1
        elif char in "tfn":
            nodes += 1
        if nodes > max_nodes:
            raise BoundedJsonError(
                f"JSON document exceeds {max_nodes} structural nodes"
            )
        prev_numeric = char in "-0123456789.eE"
    if nodes > max_nodes:
        raise BoundedJsonError(
            f"JSON document exceeds {max_nodes} structural nodes"
        )
    if members > max_members:
        raise BoundedJsonError(
            f"JSON document exceeds {max_members} object members"
        )
    if separators > max_items:
        raise BoundedJsonError(
            f"JSON document exceeds {max_items} comma separators"
        )


def _json_integer(text: str) -> int:
    if len(text.removeprefix("-")) > MAX_CACHE_JSON_INTEGER_DIGITS:
        raise BoundedJsonError(
            f"JSON integer exceeds {MAX_CACHE_JSON_INTEGER_DIGITS} digits"
        )
    try:
        return int(text)
    except ValueError as exc:
        # A stricter host-wide digit guard may reject before the local ceiling.
        raise BoundedJsonError("JSON integer exceeds the host integer digit limit") from exc


def _json_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value):
        raise BoundedJsonError("JSON number must be finite and representable as a float")
    return value


def _json_nonfinite(_text: str) -> None:
    raise BoundedJsonError("JSON number must be finite; NaN and Infinity are unsupported")


def bounded_json_loads(
    text: str,
    *,
    max_depth: int = MAX_CACHE_JSON_DEPTH,
    max_members: int = MAX_CACHE_JSON_MEMBERS,
    max_items: int = MAX_CACHE_JSON_ITEMS,
    max_nodes: int = MAX_CACHE_JSON_NODES,
) -> Any:
    """Parse JSON text under the cache-artifact ceilings with named errors."""
    _bounded_json_scan(
        text,
        max_depth=max_depth,
        max_members=max_members,
        max_items=max_items,
        max_nodes=max_nodes,
    )
    try:
        return json.loads(
            text, object_pairs_hook=_no_duplicate_pairs, parse_int=_json_integer,
            parse_float=_json_float, parse_constant=_json_nonfinite,
        )
    except BoundedJsonError:
        raise
    except UnicodeDecodeError as exc:  # defensive: callers decode bytes first
        raise BoundedJsonError(f"JSON artifact is not valid UTF-8: {exc}") from exc
    except RecursionError as exc:
        raise BoundedJsonError(
            "JSON artifact nesting exceeded the parser recursion budget"
        ) from exc
    except json.JSONDecodeError as exc:
        raise BoundedJsonError(f"JSON artifact is not valid JSON: {exc}") from exc


def read_bounded_json(
    path: Path,
    *,
    max_bytes: int = MAX_CACHE_JSON_BYTES,
    max_depth: int = MAX_CACHE_JSON_DEPTH,
    max_members: int = MAX_CACHE_JSON_MEMBERS,
    max_items: int = MAX_CACHE_JSON_ITEMS,
    max_nodes: int = MAX_CACHE_JSON_NODES,
) -> Any:
    """Read one externally supplied JSON artifact under explicit ceilings.

    Byte ceiling first (bounded read through one handle), deterministic UTF-8
    decode, pre-parse depth/count scan, duplicate-key and nonfinite-number
    rejection, bounded decimal integer conversion, and
    RecursionError containment. Every malformed outcome raises the named
    ``BoundedJsonError`` rather than a parser-implementation exception.
    """
    if type(max_bytes) is not int or max_bytes < 1:
        raise ValueError("max_bytes must be a positive integer")
    data = _bounded_json_bytes(path, max_bytes)
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise BoundedJsonError(f"{path} is not valid UTF-8: {exc}") from exc
    return bounded_json_loads(
        text,
        max_depth=max_depth,
        max_members=max_members,
        max_items=max_items,
        max_nodes=max_nodes,
    )


def _no_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise BoundedJsonError(f"duplicate JSON field: {bounded_echo(key)}")
        result[key] = value
    return result


def _read_json(path: Path) -> Any:
    return read_bounded_json(path)


def read_build_report_json(path: Path) -> Any:
    """Read one build-report.json under the build-report-specific ceilings.

    The report is generated by this project's own planner and scales with the
    analysed title, so it is bounded by MAX_BUILD_REPORT_JSON_* rather than the
    shared 1 MiB package/cache ceilings. Every rejection stays a named
    ``BoundedJsonError`` exactly as on the shared route.
    """
    return read_bounded_json(
        path,
        max_bytes=MAX_BUILD_REPORT_JSON_BYTES,
        max_members=MAX_BUILD_REPORT_JSON_MEMBERS,
        max_items=MAX_BUILD_REPORT_JSON_ITEMS,
        max_nodes=MAX_BUILD_REPORT_JSON_NODES,
    )


_ARTIFACT_COMPONENT_RE = re.compile(r"[A-Za-z0-9._+-]+")
_WINDOWS_RESERVED = frozenset(
    {"CON", "PRN", "AUX", "NUL"}
    | {f"COM{i}" for i in range(1, 10)}
    | {f"LPT{i}" for i in range(1, 10)}
)


def _safe_relative(value: Any) -> str:
    """Validate one cache/package artifact relative path.

    Mirrors is_valid_artifact_path() in src/core/nk_title_manifest.c so the
    completion manifest never records a path the native package validator
    rejects: components are [A-Za-z0-9._+-] (the '+' is required by GCC/MSYS2
    runtime library names such as libstdc++-6.dll), with no traversal,
    separators, reserved device names, or trailing dot/space.
    """
    if (
        not isinstance(value, str)
        or not value
        or len(value.encode("utf-8")) > 240
        or "\\" in value
        or ":" in value
        or value.startswith("/")
        or value.endswith("/")
        or "//" in value
    ):
        raise PackageCacheError("cache artifact path is not a portable relative path")
    for part in value.split("/"):
        if part in {"", ".", ".."}:
            raise PackageCacheError("cache artifact path escapes the package")
        if (
            not _ARTIFACT_COMPONENT_RE.fullmatch(part)
            or part.endswith((".", " "))
            or part.split(".", 1)[0].upper() in _WINDOWS_RESERVED
        ):
            raise PackageCacheError("cache artifact path is not a portable relative path")
    return value


def _resolve_within(root: Path, relative: str) -> Path:
    safe = _safe_relative(relative)
    candidate_path = root
    for part in PurePosixPath(safe).parts:
        candidate_path = candidate_path / part
        if candidate_path.is_symlink():
            raise PackageCacheError("cache artifact path is a symlink")
    candidate = candidate_path.resolve(strict=False)
    resolved_root = root.resolve(strict=False)
    try:
        candidate.relative_to(resolved_root)
    except ValueError as exc:
        raise PackageCacheError("cache artifact path escapes the package") from exc
    return candidate


def cache_root(user_data_root: Path | str, disc_id: str) -> Path:
    if not isinstance(disc_id, str) or len(disc_id) != 9:
        raise PackageCacheError("cache disc ID must be nine characters")
    normalized = disc_id.upper()
    if not normalized[:4].isalpha() or not normalized[4:].isdigit():
        raise PackageCacheError("cache disc ID is not a PSP identity")
    root = Path(user_data_root).expanduser().resolve(strict=False)
    return root.joinpath(*CACHE_ROOT_PARTS, normalized)


def cache_root_is_private(user_data_root: Path | str, repository_root: Path | str) -> bool:
    root = Path(user_data_root).expanduser().resolve(strict=False)
    repository = Path(repository_root).expanduser().resolve(strict=False)
    return root != repository and repository not in root.parents


def cache_entry_limit(environment: Mapping[str, str] | None = None) -> int:
    env = os.environ if environment is None else environment
    raw = env.get(CACHE_LIMIT_ENV)
    if raw is None or not raw.strip():
        return DEFAULT_MAX_CACHE_ENTRIES
    try:
        value = int(raw)
    except ValueError as exc:
        raise PackageCacheError(f"{CACHE_LIMIT_ENV} must be a positive integer") from exc
    if value < 1:
        raise PackageCacheError(f"{CACHE_LIMIT_ENV} must be a positive integer")
    return value


def _is_cache_digest(value: str) -> bool:
    return len(value) == 64 and all(char in "0123456789abcdef" for char in value)


def _path_key(path: Path) -> str:
    """Comparison key for filesystem paths.

    MSYS2's Windows Python joins iterdir() children with a backslash while
    resolve() returns forward slashes, and its Path equality does not treat the
    two spellings as the same path. Normalise separators and case explicitly.
    """
    return os.path.normcase(os.path.normpath(str(path)))


def prune_cache(
    cache_root_path: Path | str,
    *,
    max_entries: int = DEFAULT_MAX_CACHE_ENTRIES,
    protected_entry: Path | str | None = None,
) -> tuple[int, int]:
    if type(max_entries) is not int or max_entries < 1:
        raise PackageCacheError("cache entry limit must be a positive integer")
    root = Path(cache_root_path).expanduser().resolve(strict=False)
    entries_root = root.joinpath("entries")
    if not entries_root.is_dir() or entries_root.is_symlink():
        return 0, 0
    candidates: list[tuple[int, Path]] = []
    try:
        aot_entries = list(entries_root.iterdir())
        for aot_entry in aot_entries:
            if not aot_entry.is_dir() or aot_entry.is_symlink() or not _is_cache_digest(aot_entry.name):
                continue
            for native_entry in aot_entry.iterdir():
                if not native_entry.is_dir() or native_entry.is_symlink() or not _is_cache_digest(native_entry.name):
                    continue
                try:
                    modified = native_entry.stat().st_mtime_ns
                except OSError:
                    continue
                candidates.append((modified, native_entry))
    except OSError:
        return 0, 0
    protected = (
        _path_key(Path(protected_entry).expanduser().resolve(strict=False))
        if protected_entry is not None else None
    )
    candidates.sort(key=lambda item: (_path_key(item[1]) == protected, item[0]), reverse=True)
    removed = 0
    for _, entry in candidates[max_entries:]:
        if protected is not None and _path_key(entry) == protected:
            continue
        if entry.is_symlink():
            continue
        try:
            shutil.rmtree(entry)
        except OSError:
            continue
        removed += 1
    return removed, len(candidates) - removed


def source_tree_digest(
    repository_root: Path | str,
    directories: tuple[str, ...] = ("src/rt", "src/core", "src/player"),
) -> str:
    root = Path(repository_root).resolve(strict=False)
    entries: list[tuple[str, Path]] = []
    makefile = root / "Makefile"
    if makefile.is_file() and not makefile.is_symlink():
        entries.append(("Makefile", makefile))
    for directory in directories:
        base = root / directory
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.is_file() and not path.is_symlink() and path.suffix.lower() in {".c", ".h", ".cpp", ".hpp"}:
                entries.append((path.relative_to(root).as_posix(), path))
    assets_root = root / "assets" / "vfpu"
    if assets_root.is_dir() and not assets_root.is_symlink():
        for path in assets_root.rglob("*"):
            if path.is_file() and not path.is_symlink():
                entries.append((path.relative_to(root).as_posix(), path))
    digest = hashlib.sha256()
    for relative, path in sorted(entries):
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def compiler_identity(
    command: str | None = None,
    *,
    repository_root: Path | str | None = None,
    environment: Mapping[str, str] | None = None,
) -> str:
    env = os.environ if environment is None else environment
    selected = command or env.get("CC") or "gcc"
    executable = shutil.which(selected, path=env.get("PATH"))
    if executable is None and repository_root is not None:
        candidate = Path(repository_root) / selected
        if candidate.is_file():
            executable = str(candidate)
    if executable is None:
        return f"{selected}:unavailable"
    path = Path(executable)
    if not path.is_file():
        return f"{selected}:unavailable"
    return f"{path.name}:{sha256_file(path)}"


def compiler_target(environment: Mapping[str, str] | None = None) -> str:
    env = os.environ if environment is None else environment
    explicit = env.get("NK_TARGET_TRIPLE") or env.get("CC_TARGET")
    if explicit:
        return explicit
    return f"{platform.machine()}-{sysconfig.get_platform()}"


def native_compile_flags(
    *,
    public_safe: bool = False,
    environment: Mapping[str, str] | None = None,
) -> str:
    env = os.environ if environment is None else environment
    values = (
        env.get("CFLAGS", ""),
        env.get("RUNTIME_OPT", "-O0"),
        env.get("GE_CFLAGS", "-O2 -fno-math-errno -Wall -Wextra -Isrc/rt -DSR_SDL3VK"),
        env.get("RECOMP_OPT", "-O0"),
        env.get("RECOMP_FLAGS", "-O0 -w -fno-var-tracking -ftrack-macro-expansion=0"),
        env.get("TRACE", "0"),
        env.get("SR_STALE_DETECT", "0"),
        env.get("LLE_CPU", ""),
        env.get("STALE_CODE_POLICY", env.get("SR_STALE_POLICY", "")),
        "PUBLIC_SAFE=1" if public_safe else "PUBLIC_SAFE=0",
    )
    return "|".join(values)


def _input_sha(value: Any, label: str) -> str:
    if not isinstance(value, Mapping) or not isinstance(value.get("sha256"), str):
        raise PackageCacheError(f"{label} input digest is missing")
    digest = value["sha256"]
    if len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest):
        raise PackageCacheError(f"{label} input digest is invalid")
    return digest


def _canonical_load_address(value: Any) -> Any:
    if type(value) is int:
        return f"0x{value:08x}"
    if isinstance(value, str):
        try:
            return f"0x{int(value, 0):08x}"
        except ValueError:
            return value
    return value


def _normal_modules(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        raise PackageCacheError("module inputs must be an array")
    result = []
    for item in value:
        if not isinstance(item, Mapping):
            raise PackageCacheError("module input record is invalid")
        result.append({
            "name": item.get("name"),
            "load_address": _canonical_load_address(item.get("load_address")),
            "sha256": item.get("sha256"),
        })
    return result


def build_cache_key(
    *,
    input_hashes: Mapping[str, Any],
    title_input_identity: Mapping[str, Any] | None = None,
    codegen_options: Mapping[str, Any],
    analyzer_sha256: str,
    codegen_sha256: str,
    compiler: str,
    target: str,
    runtime_source_digest: str,
    compile_flags: str = "",
    link_flags: str = "",
    analyzer_codegen_epoch: str = ANALYZER_CODEGEN_SEMANTICS_EPOCH,
    generated_code_abi_epoch: int = GENERATED_CODE_ABI_EPOCH,
    runtime_abi_epoch: int = RUNTIME_ABI_EPOCH,
) -> dict[str, Any]:
    if not isinstance(input_hashes, Mapping):
        raise PackageCacheError("package input hashes must be an object")
    executable_sha256 = _input_sha(input_hashes.get("executable"), "executable")
    manifest_sha256 = _input_sha(input_hashes.get("manifest"), "manifest")
    modules = _normal_modules(input_hashes.get("modules"))
    identity_digest = (
        title_input_identity_digest(dict(title_input_identity))
        if title_input_identity is not None else None
    )
    psp_header = input_hashes.get("psp_header")
    psp_header_sha256 = None
    if psp_header is not None:
        psp_header_sha256 = _input_sha(psp_header, "PSP header")
    if not all(isinstance(value, str) and value for value in (
        analyzer_sha256, codegen_sha256, compiler, target, runtime_source_digest,
    )):
        raise PackageCacheError("cache identity components must be non-empty strings")
    options = json.loads(canonical_json(codegen_options))
    aot_components = {
        "executable_sha256": executable_sha256,
        "manifest_sha256": manifest_sha256,
        "modules_sha256": _digest(modules),
        "title_input_identity_sha256": identity_digest,
        "psp_header_sha256": psp_header_sha256,
        "analyzer_codegen_epoch": analyzer_codegen_epoch,
        "analyzer_sha256": analyzer_sha256,
        "codegen_sha256": codegen_sha256,
        "codegen_options_sha256": _digest(options),
        "generated_code_abi_epoch": int(generated_code_abi_epoch),
        "runtime_abi_epoch": int(runtime_abi_epoch),
    }
    generated_components = dict(aot_components)
    generated_components.pop("runtime_abi_epoch")
    generated_code_digest = _digest(generated_components)
    native_components = {
        "generated_code_digest": generated_code_digest,
        "compiler_identity": compiler,
        "compiler_target": target,
        "runtime_source_digest": runtime_source_digest,
        "compile_flags": compile_flags,
        "link_flags": link_flags,
        "runtime_abi_epoch": int(runtime_abi_epoch),
    }
    return {
        "schema_version": CACHE_SCHEMA_VERSION,
        "aot": {
            "digest": _digest(aot_components),
            "components": aot_components,
        },
        "native": {
            "digest": _digest(native_components),
            "components": native_components,
        },
    }


def cache_metadata(
    key: Mapping[str, Any],
    codegen_options: Mapping[str, Any],
    *,
    generated_code_reusable: bool = True,
) -> dict[str, Any]:
    components = key.get("aot", {}).get("components", {})
    runtime_epoch = components.get("runtime_abi_epoch")
    return {
        "format": CACHE_FORMAT,
        "schema_version": CACHE_SCHEMA_VERSION,
        "key": key,
        "codegen_options": json.loads(canonical_json(codegen_options)),
        "runtime_abi_compatibility": {
            "current_epoch": runtime_epoch,
            "generated_code_reusable": bool(generated_code_reusable),
        },
    }


def _key_components(key: Mapping[str, Any], branch: str) -> tuple[Mapping[str, Any], str]:
    if not isinstance(key, Mapping):
        raise PackageCacheError("cache key is not an object")
    value = key.get(branch)
    if not isinstance(value, Mapping) or not isinstance(value.get("components"), Mapping):
        raise PackageCacheError(f"cache key {branch} components are missing")
    digest = value.get("digest")
    if not isinstance(digest, str) or len(digest) != 64:
        raise PackageCacheError(f"cache key {branch} digest is invalid")
    if _digest(value["components"]) != digest:
        raise PackageCacheError(f"cache key {branch} digest does not match its components")
    return value["components"], digest


def runtime_abi_compatible(previous_epoch: Any, current_epoch: Any) -> bool:
    if type(previous_epoch) is not int or type(current_epoch) is not int:
        return False
    if previous_epoch == current_epoch:
        return True
    return current_epoch in RUNTIME_ABI_COMPATIBILITY.get(previous_epoch, frozenset())


def _key_from_document(value: Mapping[str, Any] | None) -> Mapping[str, Any] | None:
    if not isinstance(value, Mapping):
        return None
    cache = value.get("cache")
    if isinstance(cache, Mapping) and isinstance(cache.get("key"), Mapping):
        return cache["key"]
    if isinstance(value.get("key"), Mapping) and "aot" not in value:
        return value["key"]
    if "aot" in value and "native" in value:
        return value
    return None


def compare_cache_keys(
    previous: Mapping[str, Any] | None,
    current: Mapping[str, Any] | None,
) -> CacheDecision:
    current_key = _key_from_document(current)
    if current_key is None:
        return CacheDecision("aot-regenerate", False, False, ("cache metadata missing",))
    try:
        current_aot, current_aot_digest = _key_components(current_key, "aot")
        current_native, current_native_digest = _key_components(current_key, "native")
    except PackageCacheError as exc:
        return CacheDecision("aot-regenerate", False, False, (str(exc),))
    previous_key = _key_from_document(previous)
    if previous_key is None:
        return CacheDecision("aot-regenerate", False, False, ("cache metadata missing",))
    try:
        previous_aot, previous_aot_digest = _key_components(previous_key, "aot")
        previous_native, previous_native_digest = _key_components(previous_key, "native")
    except PackageCacheError as exc:
        return CacheDecision("aot-regenerate", False, False, (str(exc),))

    reasons: list[str] = []
    generated_reusable = True
    for component in (
        "executable_sha256", "manifest_sha256", "modules_sha256", "title_input_identity_sha256",
        "psp_header_sha256",
        "analyzer_codegen_epoch", "analyzer_sha256", "codegen_sha256",
        "codegen_options_sha256", "generated_code_abi_epoch",
    ):
        if previous_aot.get(component) != current_aot.get(component):
            generated_reusable = False
            reasons.append(f"aot:{component}")
    previous_epoch = previous_aot.get("runtime_abi_epoch")
    current_epoch = current_aot.get("runtime_abi_epoch")
    if previous_epoch != current_epoch:
        reasons.append("aot:runtime_abi_epoch")
        if not runtime_abi_compatible(previous_epoch, current_epoch):
            generated_reusable = False
    if not generated_reusable:
        return CacheDecision("aot-regenerate", False, False, tuple(reasons))

    native_reusable = True
    if previous_native.get("generated_code_digest") != current_native.get("generated_code_digest"):
        native_reusable = False
        reasons.append("native:generated_code_digest")
    for component in (
        "compiler_identity", "compiler_target", "runtime_source_digest", "compile_flags",
        "link_flags", "runtime_abi_epoch",
    ):
        if previous_native.get(component) != current_native.get(component):
            native_reusable = False
            reasons.append(f"native:{component}")
    if native_reusable and previous_aot_digest == current_aot_digest and previous_native_digest == current_native_digest:
        return CacheDecision("reuse", True, True, tuple(reasons))
    return CacheDecision("native-recompile", True, False, tuple(reasons))


def _artifact_records(package_dir: Path) -> list[dict[str, str]]:
    records: list[dict[str, str]] = []
    for path in sorted(package_dir.rglob("*")):
        if path.is_symlink():
            raise PackageCacheError("package contains a symlink artifact")
        if not path.is_file():
            continue
        relative = path.relative_to(package_dir).as_posix()
        if relative == COMPLETION_MANIFEST:
            continue
        records.append({"path": _safe_relative(relative), "sha256": sha256_file(path)})
    return records


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", prefix=f".{path.name}.", suffix=".tmp", dir=path.parent, delete=False
        ) as stream:
            temporary_path = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if os.name != "nt":
            temporary_path.chmod(0o600)
        os.replace(temporary_path, path)
        temporary_path = None
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass


def write_completion_manifest(
    package_dir: Path,
    key: Mapping[str, Any],
    *,
    title_input_identity: Mapping[str, Any] | None = None,
    backends: str | None = None,
    limits: list[str] | None = None,
) -> Path:
    package_dir = package_dir.resolve(strict=False)
    artifacts = _artifact_records(package_dir)
    if title_input_identity is None:
        try:
            package = _read_json(package_dir / "package.json")
        except (OSError, ValueError) as exc:
            raise PackageCacheError(f"package input identity is unavailable: {exc}") from exc
        title_input_identity = package.get("title_input_identity") if isinstance(package, dict) else None
    identity = validate_title_input_identity(dict(title_input_identity)) if title_input_identity is not None else None
    document: dict[str, Any] = {
        "format": COMPLETION_FORMAT,
        "schema_version": COMPLETION_SCHEMA_VERSION,
        "status": "complete",
        "cache_key": key,
        "title_input_identity": identity,
        "artifacts": artifacts,
    }
    if backends is not None:
        document["backends"] = backends
    if limits is not None:
        document["limits"] = limits
    destination = package_dir / COMPLETION_MANIFEST
    _atomic_write(destination, canonical_json(document).encode("utf-8"))
    return destination


def validate_completion_manifest(
    package_dir: Path,
    *,
    expected_key: Mapping[str, Any] | None = None,
    required_paths: set[str] | None = None,
) -> tuple[bool, str, dict[str, Any] | None]:
    package_dir = package_dir.resolve(strict=False)
    path = package_dir / COMPLETION_MANIFEST
    if not path.is_file() or path.is_symlink():
        return False, "completion manifest is missing or not a regular file", None
    try:
        document = _read_json(path)
    except (OSError, ValueError) as exc:
        return False, f"completion manifest is unreadable: {exc}", None  # includes BoundedJsonError
    if not isinstance(document, dict):
        return False, "completion manifest must be a JSON object", None
    allowed = {"format", "schema_version", "status", "cache_key", "title_input_identity", "artifacts", "backends", "limits"}
    required = {"format", "schema_version", "status", "cache_key", "title_input_identity", "artifacts"}
    doc_keys = set(document)
    if not required.issubset(doc_keys) or not doc_keys.issubset(allowed):
        return False, "completion manifest fields do not match the cache contract", None
    if document["format"] != COMPLETION_FORMAT or document["schema_version"] != COMPLETION_SCHEMA_VERSION:
        return False, "completion manifest format or schema is unsupported", None
    if document["status"] != "complete":
        return False, "completion manifest does not mark a complete build", None
    key = document["cache_key"]
    if not isinstance(key, dict):
        return False, "completion manifest cache key is missing", None
    try:
        _key_components(key, "aot")
        _key_components(key, "native")
    except PackageCacheError as exc:
        return False, str(exc), None
    if expected_key is not None and key != expected_key:
        return False, "completion manifest cache key does not match the package", None
    try:
        identity = validate_title_input_identity(document["title_input_identity"])
    except PackageCacheError as exc:
        return False, str(exc), None
    try:
        aot_components, _ = _key_components(key, "aot")
    except PackageCacheError as exc:
        return False, str(exc), None
    if aot_components.get("title_input_identity_sha256") != title_input_identity_digest(identity):
        return False, "completion manifest title input identity does not match the cache key", None
    artifacts = document["artifacts"]
    if not isinstance(artifacts, list) or not artifacts:
        return False, "completion manifest artifact list is empty", None
    seen: set[str] = set()
    for record in artifacts:
        if not isinstance(record, dict) or set(record) != {"path", "sha256"}:
            return False, "completion manifest artifact record is invalid", None
        try:
            relative = _safe_relative(record["path"])
            digest = record["sha256"]
            if not isinstance(digest, str) or len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest):
                raise PackageCacheError("artifact digest is invalid")
            resolved = _resolve_within(package_dir, relative)
        except PackageCacheError as exc:
            return False, str(exc), None
        if relative in seen:
            return False, f"completion manifest repeats artifact {relative}", None
        seen.add(relative)
        if not resolved.is_file() or resolved.is_symlink():
            return False, f"completion artifact is missing: {relative}", None
        if sha256_file(resolved) != digest:
            return False, f"completion artifact digest mismatch: {relative}", None
    for relative in required_paths or set():
        if relative not in seen:
            return False, f"completion manifest does not cover {relative}", None
    try:
        actual = {record["path"] for record in _artifact_records(package_dir)}
    except PackageCacheError as exc:
        return False, str(exc), None
    if actual != seen:
        return False, "completion manifest artifact set does not match the package", None
    return True, "", document


def package_cache_key(package_dir: Path) -> Mapping[str, Any] | None:
    try:
        document = _read_json(package_dir / "package.json")
    except (OSError, ValueError):
        return None  # unreadable or hostile package metadata carries no cache key
    return _key_from_document(document)


def validate_package_cache(
    package_dir: Path,
    *,
    expected_key: Mapping[str, Any] | None = None,
) -> tuple[bool, str]:
    package_dir = package_dir.resolve(strict=False)
    try:
        package = _read_json(package_dir / "package.json")
        report = read_build_report_json(package_dir / "build-report.json")
    except (OSError, ValueError) as exc:
        # BoundedJsonError names byte/depth/count/duplicate-key/UTF-8 failures.
        return False, f"package metadata is unreadable: {exc}"
    if not isinstance(package, dict) or not isinstance(report, dict):
        return False, "package metadata must contain JSON objects"
    if package.get("format") != "nakagawa-aot-package" or package.get("schema_version") != 2:
        return False, "package format or schema is unsupported"
    cache = package.get("cache")
    key = _key_from_document(package)
    if not isinstance(cache, dict) or key is None:
        return False, "package cache metadata is missing"
    if set(cache) != {"format", "schema_version", "key", "codegen_options", "runtime_abi_compatibility"}:
        return False, "package cache fields do not match the cache contract"
    if cache.get("format") != CACHE_FORMAT or cache.get("schema_version") != CACHE_SCHEMA_VERSION:
        return False, "package cache format or schema is unsupported"
    if not isinstance(cache.get("codegen_options"), dict):
        return False, "package cache codegen options are invalid"
    compatibility = cache.get("runtime_abi_compatibility")
    if not isinstance(compatibility, dict) or set(compatibility) != {"current_epoch", "generated_code_reusable"}:
        return False, "package cache runtime compatibility fields are invalid"
    if report.get("cache") != cache:
        return False, "build report cache metadata does not match package.json"
    try:
        aot_components, _ = _key_components(key, "aot")
        native_components, _ = _key_components(key, "native")
    except PackageCacheError as exc:
        return False, str(exc)
    if key.get("schema_version") != CACHE_SCHEMA_VERSION:
        return False, "package cache key schema is unsupported"
    components = aot_components
    if (
        components.get("analyzer_codegen_epoch") != ANALYZER_CODEGEN_SEMANTICS_EPOCH
        or components.get("generated_code_abi_epoch") != GENERATED_CODE_ABI_EPOCH
        or components.get("runtime_abi_epoch") != RUNTIME_ABI_EPOCH
        or native_components.get("runtime_abi_epoch") != RUNTIME_ABI_EPOCH
        or components.get("codegen_options_sha256") != _digest(cache["codegen_options"])
        or compatibility.get("current_epoch") != components.get("runtime_abi_epoch")
        or type(compatibility.get("generated_code_reusable")) is not bool
    ):
        return False, "package cache compatibility metadata is invalid"
    generated_components = dict(components)
    generated_components.pop("runtime_abi_epoch", None)
    if native_components.get("generated_code_digest") != _digest(generated_components):
        return False, "package generated-code digest disagrees with cache key"
    inputs = package.get("inputs", {})
    if not isinstance(inputs, dict):
        return False, "package input metadata is invalid"
    try:
        identity = validate_title_input_identity(package.get("title_input_identity"))
    except PackageCacheError as exc:
        return False, str(exc)
    if title_input_identity_digest(identity) != components.get("title_input_identity_sha256"):
        return False, "package title input identity disagrees with cache key"
    executable_input = inputs.get("executable", {})
    manifest_input = inputs.get("manifest", {})
    if not isinstance(executable_input, dict) or not isinstance(manifest_input, dict):
        return False, "package input metadata is invalid"
    if executable_input.get("sha256") != components.get("executable_sha256"):
        return False, "package executable digest disagrees with cache key"
    if manifest_input.get("sha256") != components.get("manifest_sha256"):
        return False, "package manifest digest disagrees with cache key"
    try:
        modules_digest = _digest(_normal_modules(inputs.get("modules")))
    except PackageCacheError:
        return False, "package module input metadata is invalid"
    if modules_digest != components.get("modules_sha256"):
        return False, "package module digest disagrees with cache key"
    title = package.get("title")
    if not isinstance(title, dict) or identity["manifest"]["id"] != title.get("id"):
        return False, "package title input identity does not match its manifest/profile"
    if identity["main_executable"]["sha256"] != executable_input.get("sha256"):
        return False, "package title input identity does not match the main executable"
    identity_modules = {item["name"]: item["sha256"] for item in identity["modules"]}
    package_modules = {
        item.get("name"): item.get("sha256")
        for item in inputs.get("modules", [])
        if isinstance(item, Mapping)
    }
    if identity_modules != package_modules:
        return False, "package title input identity does not match its module inputs"
    psp_header = inputs.get("psp_header")
    expected_psp_header = components.get("psp_header_sha256")
    if psp_header is None:
        if expected_psp_header is not None:
            return False, "package PSP header digest disagrees with cache key"
    elif not isinstance(psp_header, dict) or psp_header.get("sha256") != expected_psp_header:
        return False, "package PSP header digest disagrees with cache key"
    executable = package.get("executable", {})
    executable_path = executable.get("path") if isinstance(executable, dict) else None
    if not isinstance(executable_path, str):
        return False, "package executable path is missing"
    try:
        executable_file = _resolve_within(package_dir, executable_path)
    except PackageCacheError as exc:
        return False, str(exc)
    if not executable_file.is_file() or sha256_file(executable_file) != executable.get("sha256"):
        return False, "package executable digest is stale"
    image_path = str(Path(executable_path).with_name(
        f"{Path(executable_path).stem}_image.bin"
    ).as_posix())
    try:
        image_file = _resolve_within(package_dir, image_path)
    except PackageCacheError as exc:
        return False, str(exc)
    if not image_file.is_file():
        return False, "package runtime image is missing"
    objects = package.get("generated_objects", [])
    if not isinstance(objects, list):
        return False, "package generated object list is invalid"
    required = {"package.json", "build-report.json", executable_path, image_path}
    for item in objects:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str):
            return False, "package generated object record is invalid"
        object_path = item["path"]
        try:
            object_file = _resolve_within(package_dir, object_path)
        except PackageCacheError as exc:
            return False, str(exc)
        if not object_file.is_file() or sha256_file(object_file) != item.get("sha256"):
            return False, f"package generated object digest is stale: {object_path}"
        required.add(object_path)
    valid, reason, completion = validate_completion_manifest(
        package_dir,
        expected_key=expected_key,
        required_paths=required,
    )
    if not valid:
        return False, reason
    if completion is None or completion.get("title_input_identity") != identity:
        return False, "completion manifest title input identity does not match package.json"
    return True, ""
