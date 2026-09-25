# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Content-addressed private AOT package cache contract."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import sysconfig
import tempfile
from typing import Any, Mapping

CACHE_FORMAT = "nakagawa-aot-cache"
COMPLETION_FORMAT = "nakagawa-aot-cache-completion"
CACHE_SCHEMA_VERSION = 1
COMPLETION_SCHEMA_VERSION = 1
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


def _no_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_no_duplicate_pairs)


def _safe_relative(value: Any) -> str:
    if not isinstance(value, str) or not value or "\\" in value:
        raise PackageCacheError("cache artifact path is not a portable relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise PackageCacheError("cache artifact path escapes the package")
    return path.as_posix()


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
        "executable_sha256", "manifest_sha256", "modules_sha256", "psp_header_sha256",
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
    backends: str | None = None,
    limits: list[str] | None = None,
) -> Path:
    package_dir = package_dir.resolve(strict=False)
    artifacts = _artifact_records(package_dir)
    document: dict[str, Any] = {
        "format": COMPLETION_FORMAT,
        "schema_version": COMPLETION_SCHEMA_VERSION,
        "status": "complete",
        "cache_key": key,
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
        return False, f"completion manifest is unreadable: {exc}", None
    if not isinstance(document, dict):
        return False, "completion manifest must be a JSON object", None
    allowed = {"format", "schema_version", "status", "cache_key", "artifacts", "backends", "limits"}
    required = {"format", "schema_version", "status", "cache_key", "artifacts"}
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
        return None
    return _key_from_document(document)


def validate_package_cache(
    package_dir: Path,
    *,
    expected_key: Mapping[str, Any] | None = None,
) -> tuple[bool, str]:
    package_dir = package_dir.resolve(strict=False)
    try:
        package = _read_json(package_dir / "package.json")
        report = _read_json(package_dir / "build-report.json")
    except (OSError, ValueError) as exc:
        return False, f"package metadata is unreadable: {exc}"
    if not isinstance(package, dict) or not isinstance(report, dict):
        return False, "package metadata must contain JSON objects"
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
    valid, reason, _ = validate_completion_manifest(
        package_dir,
        expected_key=expected_key,
        required_paths=required,
    )
    if not valid:
        return False, reason
    return True, ""
