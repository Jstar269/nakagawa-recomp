#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Build a deterministic code-generation plan or AOT package from a title manifest.

The public manifest supplies source-owned title configuration. Private executable,
module, and PSP-header paths remain explicit command-line bindings and are never
written back to the manifest.

GENERIC TITLE CONTRACT (title-neutral, host-portable):
  - title identifier (manifest id) and kind
  - build identifier / game_name (portable, no .exe semantics)
  - executable base/entry and bss_metadata_source
  - codegen profile selection (none vs hst) and funcs_per_chunk
  - extra executable spans (title extra spans, portable)
  - guest module configuration (names and load addresses)
  - runtime fallback entry (from runtime_bindings or executable entry)
  - generated-output locations (build_dir/name derived, forward-slash portable)
  - optional title capabilities (runtime_bindings, disc, etc. when present)
  Rendered paths use forward slashes and no Windows drive or .exe semantics;
  the plan JSON itself is host-portable and requires no PowerShell/MSYS2/C:\\ paths.

HST PROFILE (isolated, not in generic planner):
  - HST disc identity and exact-disc-id policy (retail)
  - HST private-address values (0-base, synthetic-HST span, module addresses)
  - HST private-input expectations (psp-header, decrypted modules, ISO)
  - HST-specific routes/assets and compatibility defaults (HST codegen profile)
  These are validated only by the HST adapter (tools/title_manager_plan.ps1:
  Get-HstManifestMakeArgs) and never by the generic planner, which treats every
  manifest id as equal and fails closed on unknown fields rather than defaulting to HST.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any

import title_manifest

GAME_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
MIN_FUNCS_PER_CHUNK = 1
MAX_FUNCS_PER_CHUNK = 100_000
MANAGER_PLAN_VERSION = 1
PACKAGE_SCHEMA_VERSION = 1
PACKAGE_FORMAT = "nakagawa-aot-package"
BUILD_REPORT_FORMAT = "nakagawa-build-report"
ROOT = Path(__file__).resolve().parents[1]

#: Manifest fields that carry no operational meaning and are therefore excluded
#: from the protected digest, so a prose edit never invalidates a build.
NON_OPERATIVE_FIELDS = frozenset({"notes"})


class TitleCodegenPlanError(ValueError):
    """Fail-closed plan construction error."""


class PackageRouteError(Exception):
    """A named, fail-closed package route error."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def compute_protected_digest(manifest: dict[str, Any]) -> str:
    """Return the deterministic digest of a manifest's operational semantics.

    The digest is taken over the *validated, canonically serialized* manifest with
    the free-text ``notes`` removed. Because validation normalizes ordering, numeric
    types, and optional fields before serialization, the digest depends only on
    meaning: key order, whitespace, and line endings in the source file cannot move
    it, while any operative mutation does. Validation also rejects unknown fields
    outright, so nothing operative can slip past the digest unnoticed. Text is
    compared by its exact UTF-8 bytes (no Unicode re-composition), so two spellings
    that merely *look* alike are treated as different contracts rather than equal.
    """
    normalized = title_manifest.validate_manifest(manifest)
    protected = {
        key: value for key, value in normalized.items() if key not in NON_OPERATIVE_FIELDS
    }
    rendered = json.dumps(
        protected, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    )
    return hashlib.sha256(rendered.encode("utf-8")).hexdigest()


def _path_text(value: Path, label: str) -> str:
    rendered = str(value).replace("\\", "/")
    if not rendered or rendered in {".", ".."}:
        raise TitleCodegenPlanError(f"{label} must identify a file or directory")
    if any(ord(char) < 0x20 for char in rendered):
        raise TitleCodegenPlanError(f"{label} must not contain control characters")
    return rendered


def _required_path(value: Path | None, label: str) -> Path:
    if value is None:
        raise TitleCodegenPlanError(f"{label} is required")
    _path_text(value, label)
    return value


def _hex(value: int) -> str:
    return f"0x{value:08x}"


def _make_address(value: int) -> str:
    """Render a Make address exactly as the existing manager does for HST."""
    return str(value) if value == 0 else _hex(value)


def _resolve_run_entry(normalized: dict[str, Any]) -> int:
    """The guest address a run of this title starts at.

    A title whose real entry is not compiled names a fallback entry in its runtime
    bindings; the runtime already consumes that same binding through
    ``sr_title_config_fallback_entry()``. Where one is configured it is what a run must
    start at, so the manager can read it from here instead of carrying its own copy.

    Note this cannot be replaced by "pass the ELF entry and let the runtime fall back":
    ``sr_lookup`` treats 0 as a first-class key (``src/rt/dispatch_table.h`` -- an image
    based at 0 has a real function at address 0), so an entry of 0 resolves rather than
    missing, and the runtime's fallback never fires.
    """
    bindings = normalized.get("runtime_bindings") or {}
    fallback = bindings.get("fallback_entry")
    if fallback is not None:
        return int(fallback)
    return int(normalized["executable"]["entry"])


def _resolve_codegen_profile(
    manifest: dict[str, Any], requested: str | None
) -> str:
    """Resolve a profile, making a manifest declaration authoritative."""
    declared = manifest.get("codegen_profile")
    if declared is not None:
        if requested is not None and requested != declared:
            raise TitleCodegenPlanError(
                "codegen profile conflicts with the manifest "
                f"(requested {requested!r}, manifest {declared!r})"
            )
        return declared
    if requested is None:
        raise TitleCodegenPlanError(
            "codegen_profile is absent from the manifest; an explicit profile is required"
        )
    return requested


def _span_environment(manifest: dict[str, Any]) -> dict[str, str]:
    """Project executable addresses and spans into analyzer/make environment.

    GENERIC: GAME_BASE, GAME_ENTRY, and the extra-span rendering are derived
    solely from the validated manifest's executable block.  No HST constant is
    consulted; an empty span set renders as the empty string, not an inherited HST default.

    TITLE_EXTRA_SPANS is the sole host-portable key carrying the rendering.
    No title-specific environment alias is emitted.
    """
    executable = manifest["executable"]
    base = executable["base"]
    spans = executable["extra_executable_spans"]
    if len(spans) > 1:
        raise TitleCodegenPlanError(
            "the current analyzer accepts at most one explicit extra executable span"
        )
    if spans and base != 0:
        raise TitleCodegenPlanError(
            "explicit extra executable spans are not yet supported with a nonzero base"
        )
    rendered = ""
    if spans:
        rendered = f"{_hex(spans[0]['start'])},{_hex(spans[0]['end'])}"
    return {
        "GAME_BASE": _hex(base),
        "GAME_ENTRY": _hex(executable["entry"]),
        "TITLE_EXTRA_SPANS": rendered,
    }


def build_plan(
    manifest: dict[str, Any],
    *,
    game_name: str,
    game_elf: Path,
    build_dir: Path,
    module_dir: Path | None = None,
    psp_header: Path | None = None,
    codegen_profile: str | None = None,
    include_optional_modules: set[str] | None = None,
    funcs_per_chunk: int = 2000,
    python_command: str = "python",
) -> dict[str, Any]:
    manifest = title_manifest.validate_manifest(manifest)
    if not GAME_NAME_RE.fullmatch(game_name):
        raise TitleCodegenPlanError("game_name is not a portable identifier")
    codegen_profile = _resolve_codegen_profile(manifest, codegen_profile)
    if codegen_profile not in {"none", "hst"}:
        raise TitleCodegenPlanError(
            f"unsupported codegen profile for the current generator: {codegen_profile}"
        )
    if type(funcs_per_chunk) is not int or not (
        MIN_FUNCS_PER_CHUNK <= funcs_per_chunk <= MAX_FUNCS_PER_CHUNK
    ):
        raise TitleCodegenPlanError(
            f"funcs_per_chunk must be in range {MIN_FUNCS_PER_CHUNK}..{MAX_FUNCS_PER_CHUNK}"
        )
    if not python_command or python_command != python_command.strip():
        raise TitleCodegenPlanError("python_command must be a non-empty trimmed string")
    if any(ord(char) < 0x20 for char in python_command):
        raise TitleCodegenPlanError("python_command must not contain control characters")

    executable = manifest["executable"]
    metadata_source = executable["bss_metadata_source"]
    if metadata_source == "psp-header":
        psp_header = _required_path(psp_header, "psp_header")
    elif psp_header is not None:
        raise TitleCodegenPlanError(
            f"psp_header is incompatible with bss_metadata_source={metadata_source!r}"
        )

    requested_optional = include_optional_modules or set()
    if any(not isinstance(name, str) for name in requested_optional):
        raise TitleCodegenPlanError("optional module selections must be strings")
    available_optional = {
        module["name"] for module in manifest["modules"]
        if module["role"] == "optional-guest-prx"
    }
    unknown_optional = sorted(requested_optional - available_optional)
    if unknown_optional:
        raise TitleCodegenPlanError(
            "unknown optional module(s): " + ", ".join(unknown_optional)
        )
    guest_modules = [
        module for module in manifest["modules"]
        if module["role"] == "guest-prx"
        or (module["role"] == "optional-guest-prx" and module["name"] in requested_optional)
    ]
    if guest_modules:
        module_dir = _required_path(module_dir, "module_dir")
        if "@" in _path_text(module_dir, "module_dir"):
            raise TitleCodegenPlanError("module_dir must not contain '@'")
    elif module_dir is not None:
        raise TitleCodegenPlanError(
            "module_dir was provided but no guest modules were selected"
        )

    game_elf_text = _path_text(game_elf, "game_elf")
    build_dir_text = _path_text(build_dir, "build_dir")
    build_prefix = Path(build_dir_text) / game_name
    base_text = _hex(executable["base"])
    if codegen_profile == "hst" and executable["base"] != 0:
        raise TitleCodegenPlanError(
            "the hst codegen profile requires a zero-based executable"
        )
    codegen = [
        python_command,
        "tools/codegen.py",
        game_elf_text,
        _path_text(build_prefix.with_name(f"{game_name}_recomp.c"), "codegen_output"),
        f"--base={base_text}",
    ]
    if codegen_profile != "none":
        codegen.append(f"--profile={codegen_profile}")
    for module in guest_modules:
        assert module_dir is not None
        codegen.append(
            "--extra-elf="
            f"{_path_text(module_dir / module['name'], 'module_path')}"
            f"@{_hex(module['load_address'])}"
        )
    codegen.append(f"--funcs-per-chunk={funcs_per_chunk}")

    prxload = [
        python_command,
        "tools/prxload.py",
        game_elf_text,
        base_text,
    ]
    if psp_header is not None:
        prxload.append(f"--psp-header={_path_text(psp_header, 'psp_header')}")
    image_output = build_prefix.with_name(f"{game_name}_image.bin")
    prxload.append(f"--out={_path_text(image_output, 'image_output')}")

    imports = [
        python_command,
        "tools/imports.py",
        game_elf_text,
        base_text,
        "--toml="
        f"{_path_text(build_prefix.with_name(f'{game_name}_imports.toml'), 'imports_output')}",
    ]

    return {
        "schema_version": 1,
        "title_manifest_id": manifest["id"],
        "game_name": game_name,
        "game_base": executable["base"],
        "game_entry": executable["entry"],
        "codegen_profile": codegen_profile,
        "bss_metadata_source": metadata_source,
        "environment": _span_environment(manifest),
        "commands": {
            "prxload": prxload,
            "codegen": codegen,
            "imports": imports,
        },
    }


def build_manager_plan(
    manifest: dict[str, Any],
    *,
    game_name: str,
    game_elf: Path,
    build_dir: Path,
    module_dir: Path | None = None,
    psp_header: Path | None = None,
    codegen_profile: str | None = None,
    include_optional_modules: set[str] | None = None,
    funcs_per_chunk: int = 2000,
    python_command: str = "python",
) -> dict[str, Any]:
    """Return the bounded, path-minimal contract consumed by the manager.

    The existing codegen plan remains the source of truth for manifest semantics. This
    adapter intentionally omits command vectors and private paths so a manager diagnostic
    cannot accidentally turn a local plan into a public binding manifest.
    """
    normalized = title_manifest.validate_manifest(manifest)
    effective_profile = _resolve_codegen_profile(normalized, codegen_profile)
    codegen_plan = build_plan(
        normalized,
        game_name=game_name,
        game_elf=game_elf,
        build_dir=build_dir,
        module_dir=module_dir,
        psp_header=psp_header,
        codegen_profile=effective_profile,
        include_optional_modules=include_optional_modules,
        funcs_per_chunk=funcs_per_chunk,
        python_command=python_command,
    )
    executable = normalized["executable"]
    requested_optional = include_optional_modules or set()
    selected_guest = [
        module
        for module in normalized["modules"]
        if module["role"] == "guest-prx"
        or (module["role"] == "optional-guest-prx" and module["name"] in requested_optional)
    ]
    required_guest = [module for module in selected_guest if module["required"]]
    optional_guest = [module for module in selected_guest if not module["required"]]
    build_dir_text = _path_text(build_dir, "build_dir")
    return {
        "plan_version": MANAGER_PLAN_VERSION,
        "plan_kind": "title-manager-build",
        "protected_digest": compute_protected_digest(normalized),
        "title_manifest_id": normalized["id"],
        "title_kind": normalized["kind"],
        "game_name": game_name,
        "game_base": executable["base"],
        "game_entry": executable["entry"],
        "codegen_profile": effective_profile,
        "bss_metadata_source": executable["bss_metadata_source"],
        "disc": normalized.get("disc"),
        "extra_executable_spans": executable["extra_executable_spans"],
        "required_guest_modules": [
            {"name": module["name"], "load_address": module["load_address"]}
            for module in required_guest
        ],
        "optional_guest_modules": [
            {"name": module["name"], "load_address": module["load_address"]}
            for module in optional_guest
        ],
        "private_binding_requirements": {
            "game_elf": True,
            "module_dir": bool(selected_guest),
            "psp_header": executable["bss_metadata_source"] == "psp-header",
        },
        # The guest address the RUNTIME is started at, which is not the same question as
        # the executable's ELF entry. HST's real entry is not compiled -- the analyzer
        # treats its first instruction as an HLE boundary -- so a run starts at the
        # title's configured fallback entry instead. That value already has an owner:
        # runtime_bindings.fallback_entry, the same binding src/rt/title_config.c
        # compiles in. Projecting it here is what lets the manager stop carrying a
        # hardcoded copy of it.
        #
        # Falling back to executable.entry is NOT a title default: with no
        # runtime_bindings block there is no configured fallback, and the ELF entry is
        # the only thing a generic title can be started at.
        "run_entry": _make_address(_resolve_run_entry(normalized)),
        "environment": codegen_plan["environment"],
        "make": {
            "game_name": game_name,
            "game_base": _make_address(executable["base"]),
            "game_entry": _make_address(executable["entry"]),
            "codegen_profile_arg": (
                f"--profile={effective_profile}" if effective_profile != "none" else ""
            ),
            "build_dir": build_dir_text,
            "funcs_per_chunk": funcs_per_chunk,
        },
    }


def canonical_json(value: dict[str, Any]) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


_MAKE_UNSAFE_PATH_CHARS = "$%#&|<>^"


def _make_unsafe(rendered: str) -> bool:
    return any(char.isspace() or char in _MAKE_UNSAFE_PATH_CHARS for char in rendered)


def _absolute_path(path: Path, label: str, *, make_safe: bool = True) -> Path:
    try:
        resolved = path.resolve()
    except OSError as exc:
        raise PackageRouteError("PACKAGE_INVALID_PATH", f"{label}: {exc}") from exc
    rendered = resolved.as_posix()
    if any(ord(char) < 0x20 for char in rendered):
        raise PackageRouteError("PACKAGE_INVALID_PATH", f"{label} contains a control character")
    if make_safe and _make_unsafe(rendered):
        raise PackageRouteError(
            "PACKAGE_UNSUPPORTED_PATH",
            f"{label} {rendered!r} contains spaces or characters the Make recipes cannot "
            "quote; choose a location without them (quoting support is in the works, "
            "issue #296)",
        )
    return resolved


def _require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise PackageRouteError("PACKAGE_INPUT_MISSING", f"{label} is not a file")


def _stage_for_make(path: Path, stage_dir: Path, label: str) -> Path:
    """Return a Make-safe path to the same bytes.

    User inputs commonly live under paths with spaces (for example a Windows
    profile directory). The Make recipes cannot quote them, so such an input
    is copied into the untracked package directory instead of being refused.
    Content hashes are unchanged, so the package contract stays deterministic.
    """
    if not _make_unsafe(path.as_posix()):
        return path
    _require_file(path, label)
    name = re.sub(r"[^A-Za-z0-9._-]", "_", path.name) or "input"
    stage_dir.mkdir(parents=True, exist_ok=True)
    staged = stage_dir / name
    shutil.copyfile(path, staged)
    return staged


def _package_game_name(manifest: dict[str, Any], requested: str | None) -> str:
    candidate = requested or manifest.get("game_name") or manifest["id"].lower()
    if not GAME_NAME_RE.fullmatch(candidate):
        raise PackageRouteError(
            "PACKAGE_INVALID_GAME_NAME",
            "pass --game-name or set a portable game_name in the title manifest",
        )
    return candidate


def _ensure_untracked_output(output_dir: Path) -> None:
    root = ROOT.resolve()
    if output_dir == root or output_dir == root / "build" or root.is_relative_to(output_dir):
        raise PackageRouteError(
            "PACKAGE_OUTPUT_NOT_DEDICATED",
            "output-dir must be a dedicated package directory and must not contain the repository",
        )
    if output_dir.is_relative_to(root):
        relative = output_dir.relative_to(root).as_posix()
        ignored = subprocess.run(
            ["git", "check-ignore", "--quiet", "--", relative],
            cwd=ROOT,
            check=False,
        )
        if ignored.returncode != 0:
            raise PackageRouteError(
                "PACKAGE_OUTPUT_NOT_UNTRACKED",
                "output-dir is inside the repository but is not ignored by Git",
            )


def _version_identity(executable: str, label: str) -> tuple[str, str]:
    try:
        completed = subprocess.run(
            [executable, "--version"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as exc:
        raise PackageRouteError("PACKAGE_TOOLCHAIN_MISSING", f"{label}: {exc}") from exc
    output = completed.stdout or completed.stderr
    first_line = next((line.strip() for line in output.splitlines() if line.strip()), "")
    if completed.returncode != 0 or not first_line:
        raise PackageRouteError(
            "PACKAGE_TOOLCHAIN_VERSION_FAILED",
            f"could not identify {label} (exit {completed.returncode})",
        )
    return Path(executable.replace("\\", "/")).name, first_line


def _selected_guest_modules(
    manifest: dict[str, Any], selected_optional: set[str]
) -> list[dict[str, Any]]:
    return [
        module
        for module in manifest["modules"]
        if module["role"] == "guest-prx"
        or (module["role"] == "optional-guest-prx" and module["name"] in selected_optional)
    ]


def _make_input_images(
    manifest: dict[str, Any],
    executable_path: Path,
    module_dir: Path | None,
    psp_header: Path | None,
    selected_optional: set[str],
) -> tuple[list[dict[str, Any]], dict[str, Any], list[dict[str, Any]], list[dict[str, str]]]:
    """Validate source ELF inputs and collect deterministic analysis/import counts."""
    try:
        import analyze
        import import_audit
        from hle_manifest import build_manifest as build_hle_manifest
        from imports import _import_model
        from types import SimpleNamespace

        normalized = title_manifest.validate_manifest(manifest)
        selected_modules = _selected_guest_modules(normalized, selected_optional)
        sources: list[dict[str, Any]] = [{
            "name": "executable",
            "path": executable_path,
            "base": normalized["executable"]["base"],
            "module_name": None,
        }]
        for module in selected_modules:
            if module_dir is None:
                raise PackageRouteError(
                    "PACKAGE_MODULE_DIR_REQUIRED",
                    f"module_dir is required for guest PRX {module['name']}",
                )
            path = module_dir / module["name"]
            _require_file(path, f"guest PRX {module['name']}")
            sources.append({
                "name": module["name"],
                "path": path,
                "base": module["load_address"],
                "module_name": module["name"],
            })

        registry = build_hle_manifest()
        analysis_summary = {"analyzed_functions": 0, "executable_regions": 0}
        unsupported_imports: list[dict[str, Any]] = []
        diagnostics: list[dict[str, str]] = []
        extra_spans = [
            (span["start"], span["end"])
            for span in normalized["executable"]["extra_executable_spans"]
        ]
        if psp_header is not None:
            _require_file(psp_header, "PSP header")

        for source in sources:
            base = source["base"]
            elf = analyze.Elf(str(source["path"]), base=base)
            starts, ranges = analyze.analyze(
                elf,
                extra_spans=extra_spans if source["name"] == "executable" else None,
            )
            if ranges == [(0, 0)]:
                raise PackageRouteError(
                    "PACKAGE_NO_EXECUTABLE_REGIONS",
                    f"{source['name']} has no analyzer-owned executable region",
                )
            source["elf"] = elf
            source["ranges"] = ranges
            source["analyzed_functions"] = len(starts)
            analysis_summary["analyzed_functions"] += len(starts)
            analysis_summary["executable_regions"] += len(ranges)

            try:
                stubs, findings = _import_model(elf)
            except (ValueError, RuntimeError) as exc:
                raise PackageRouteError(
                    "PACKAGE_INVALID_IMPORT_TABLE",
                    f"{source['name']}: {exc}",
                ) from exc
            for finding in findings:
                diagnostics.append({"module": source["name"], "message": str(finding)})
            functions = [
                SimpleNamespace(stub_addr=addr, library=lib, nid=nid)
                for addr, (lib, nid) in sorted(stubs.items())
            ]
            classified = import_audit.classify_imports(
                functions, registry, with_addresses=True, findings=findings
            )
            for row in classified["imports"]:
                if row["classification"] != "dedicated":
                    unsupported_imports.append({
                        "boundary": f"PSP import {row['lib']}:{row['nid']}",
                        "module": source["name"],
                        "library": row["lib"],
                        "nid": row["nid"],
                        "name": row["name"],
                        "classification": row["classification"],
                        "status": "in the works",
                        "tracking_issue": 71,
                    })
        unsupported_imports.sort(
            key=lambda row: (row["module"], row["library"], row["nid"])
        )
        return sources, analysis_summary, unsupported_imports, diagnostics
    except PackageRouteError:
        raise
    except (OSError, ValueError, RuntimeError) as exc:
        raise PackageRouteError("PACKAGE_INVALID_ELF", str(exc)) from exc


def _read_codegen_fallbacks(
    stub_report: Path, sources: list[dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    regions: list[dict[str, Any]] = []
    instructions: list[dict[str, Any]] = []
    for line in stub_report.read_text(encoding="ascii").splitlines():
        if not line.strip():
            continue
        address_text, _, reason = line.partition(" ")
        try:
            entry = int(address_text, 16)
        except ValueError as exc:
            raise PackageRouteError(
                "PACKAGE_INVALID_CODEGEN_REPORT", "malformed codegen fallback address"
            ) from exc
        owner = next(
            (
                source
                for source in sources
                if any(lo <= entry < hi for lo, hi in source["ranges"])
            ),
            None,
        )
        module = owner["name"] if owner is not None else "executable region"
        region = {
            "boundary": f"AOT function at 0x{entry:08x}",
            "module": module,
            "entry_address": f"0x{entry:08x}",
            "reason": reason,
            "status": "in the works",
            "tracking_issue": 118,
        }
        regions.append(region)

        location = re.search(r"\bat (0x[0-9a-fA-F]{8})\b", reason)
        if location is None:
            continue
        instruction_address = int(location.group(1), 16)
        instruction_owner = next(
            (
                source
                for source in sources
                if any(lo <= instruction_address < hi for lo, hi in source["ranges"])
            ),
            owner,
        )
        word = (
            instruction_owner["elf"].read_at_vaddr(instruction_address, 4)
            if instruction_owner is not None
            else None
        )
        instructions.append({
            "boundary": f"Allegrex instruction at 0x{instruction_address:08x}",
            "module": instruction_owner["name"] if instruction_owner is not None else module,
            "address": f"0x{instruction_address:08x}",
            "word": f"0x{int.from_bytes(word, 'little'):08x}" if word and len(word) == 4 else None,
            "reason": reason,
            "status": "in the works",
            "tracking_issue": 118,
        })
    regions.sort(key=lambda row: (row["module"], row["entry_address"]))
    instructions.sort(key=lambda row: (row["module"], row["address"]))
    return regions, instructions


def _required_local_assets(
    manifest: dict[str, Any],
    selected_optional: set[str],
    module_hashes: dict[str, str],
    output_dir: Path,
) -> list[dict[str, Any]]:
    filesystem = manifest["filesystem"]
    assets: list[dict[str, Any]] = [{
        "kind": "title-data-root",
        "path": filesystem["data_root"],
        "required": True,
        "provisioning": "local title data supplied by the user",
    }]
    contract = manifest.get("runtime_contract")
    if contract and contract["resources"]["mode"] == "manifest-filesystem":
        for locator in contract["resources"]["locators"]:
            assets.append({
                "kind": "runtime-resource-locator",
                "path": locator,
                "required": True,
                "provisioning": "local title data supplied by the user",
            })
    for module in sorted(manifest["modules"], key=lambda item: item["name"]):
        if module["role"] not in {"guest-prx", "optional-guest-prx"}:
            continue
        selected = (
            module["role"] == "guest-prx"
            or module["name"] in selected_optional
        )
        asset: dict[str, Any] = {
            "kind": "guest-prx",
            "name": module["name"],
            "load_address": _hex(module["load_address"]),
            "required": bool(module["required"]),
            "included_in_aot": selected,
            "provisioning": "local title module supplied by the user",
        }
        if module["name"] in module_hashes:
            asset["sha256"] = module_hashes[module["name"]]
        assets.append(asset)

    if os.name == "nt":
        assets.extend((
            {
                "kind": "host-runtime-library",
                "name": "SDL3.dll",
                "required": True,
                "bundled": (output_dir / "SDL3.dll").is_file(),
                "resolution": "package directory or host PATH",
            },
            {
                "kind": "host-runtime-library",
                "name": "vulkan-1.dll",
                "required": True,
                "bundled": (output_dir / "vulkan-1.dll").is_file(),
                "resolution": "system Vulkan loader or package directory",
            },
        ))
    else:
        assets.extend((
            {
                "kind": "host-runtime-library",
                "name": "SDL3 shared library",
                "required": True,
                "resolution": "host runtime library search path",
            },
            {
                "kind": "host-runtime-library",
                "name": "Vulkan loader",
                "required": True,
                "resolution": "host runtime library search path",
            },
        ))
    return sorted(assets, key=lambda item: (item["kind"], item.get("name", item.get("path", ""))))


def _hash_package_inputs(
    manifest_path: Path,
    game_elf: Path,
    selected_modules: list[dict[str, Any]],
    module_input_paths: dict[str, Path],
    psp_header: Path | None,
) -> dict[str, Any]:
    module_addresses = {module["name"]: module["load_address"] for module in selected_modules}
    return {
        "manifest": {"sha256": _sha256_file(manifest_path)},
        "executable": {"sha256": _sha256_file(game_elf)},
        "modules": [
            {
                "name": name,
                "load_address": _hex(module_addresses[name]),
                "sha256": _sha256_file(path),
            }
            for name, path in sorted(module_input_paths.items())
        ],
        "psp_header": {"sha256": _sha256_file(psp_header)} if psp_header is not None else None,
    }


def build_package(
    manifest: dict[str, Any],
    *,
    manifest_path: Path,
    game_elf: Path,
    output_dir: Path,
    game_name: str | None = None,
    module_dir: Path | None = None,
    psp_header: Path | None = None,
    codegen_profile: str | None = None,
    include_optional_modules: set[str] | None = None,
    funcs_per_chunk: int = 2000,
    python_command: str = "python",
    make_command: str | None = None,
    public_safe: bool = False,
) -> dict[str, Any]:
    """Run the canonical two-phase Make build and emit package/report JSON."""
    try:
        normalized = title_manifest.validate_manifest(manifest)
    except ValueError as exc:
        raise PackageRouteError("PACKAGE_INVALID_MANIFEST", str(exc)) from exc

    selected_optional = include_optional_modules or set()
    selected_name = _package_game_name(normalized, game_name)
    # The output directory is written by the Make recipes, so it must itself be
    # Make-safe. Inputs are staged into it when their own paths are not.
    output_dir = _absolute_path(output_dir, "output-dir")
    _ensure_untracked_output(output_dir)
    stage_dir = output_dir / "staged-inputs"
    manifest_path = _absolute_path(manifest_path, "manifest path", make_safe=False)
    game_elf = _absolute_path(game_elf, "executable ELF path", make_safe=False)
    module_dir = (_absolute_path(module_dir, "module-dir", make_safe=False)
                  if module_dir is not None else None)
    psp_header = (_absolute_path(psp_header, "PSP-header path", make_safe=False)
                  if psp_header is not None else None)
    _require_file(manifest_path, "title manifest")
    _require_file(game_elf, "executable ELF")
    manifest_path = _stage_for_make(manifest_path, stage_dir, "title manifest")
    game_elf = _stage_for_make(game_elf, stage_dir, "executable ELF")
    if psp_header is not None:
        psp_header = _stage_for_make(psp_header, stage_dir, "PSP header")
    if module_dir is not None and _make_unsafe(module_dir.as_posix()):
        staged_modules = stage_dir / "modules"
        for module in _selected_guest_modules(normalized, selected_optional):
            if _make_unsafe(module["name"]):
                raise PackageRouteError(
                    "PACKAGE_UNSUPPORTED_PATH",
                    f"guest PRX name {module['name']!r} is not usable in a Make path",
                )
            source = module_dir / module["name"]
            _require_file(source, f"guest PRX {module['name']}")
            staged_modules.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, staged_modules / module["name"])
        module_dir = staged_modules
    if normalized["executable"]["bss_metadata_source"] == "psp-header" and psp_header is None:
        raise PackageRouteError(
            "PACKAGE_PSP_HEADER_REQUIRED",
            "the manifest requires --psp-header for BSS metadata",
        )

    try:
        plan = build_plan(
            normalized,
            game_name=selected_name,
            game_elf=game_elf,
            build_dir=output_dir,
            module_dir=module_dir,
            psp_header=psp_header,
            codegen_profile=codegen_profile,
            include_optional_modules=selected_optional,
            funcs_per_chunk=funcs_per_chunk,
            python_command=python_command,
        )
    except (OSError, ValueError) as exc:
        raise PackageRouteError("PACKAGE_INVALID_PLAN", str(exc)) from exc

    selected_modules = _selected_guest_modules(normalized, selected_optional)
    module_input_paths: dict[str, Path] = {}
    for module in selected_modules:
        if module_dir is None:
            raise PackageRouteError(
                "PACKAGE_MODULE_DIR_REQUIRED",
                f"module_dir is required for guest PRX {module['name']}",
            )
        module_path = _absolute_path(module_dir / module["name"], f"guest PRX {module['name']}")
        _require_file(module_path, f"guest PRX {module['name']}")
        module_input_paths[module["name"]] = module_path
    if psp_header is not None:
        _require_file(psp_header, "PSP header")

    try:
        sources, analysis_summary, unsupported_imports, diagnostics = _make_input_images(
            normalized,
            game_elf,
            module_dir,
            psp_header,
            selected_optional,
        )
    except PackageRouteError:
        raise
    except (OSError, ValueError, RuntimeError) as exc:
        raise PackageRouteError("PACKAGE_INVALID_ELF", str(exc)) from exc

    input_hashes = _hash_package_inputs(
        manifest_path, game_elf, selected_modules, module_input_paths, psp_header
    )
    package_path = output_dir / "package.json"
    report_path = output_dir / "build-report.json"
    if package_path.exists():
        try:
            previous_package = json.loads(package_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise PackageRouteError(
                "PACKAGE_OUTPUT_CONFLICT", "existing package.json is not a valid package"
            ) from exc
        if previous_package.get("inputs") != input_hashes:
            raise PackageRouteError(
                "PACKAGE_OUTPUT_CONFLICT",
                "output-dir already contains a package built from different inputs",
            )
    elif report_path.exists():
        raise PackageRouteError(
            "PACKAGE_OUTPUT_CONFLICT",
            "output-dir contains build-report.json without its matching package.json",
        )

    make_name = make_command or ("mingw32-make" if os.name == "nt" else "make")
    make_executable = shutil.which(make_name)
    if make_executable is None:
        raise PackageRouteError(
            "PACKAGE_TOOLCHAIN_MISSING", f"{make_name} is not available on PATH"
        )
    cc_name = os.environ.get("CC", "gcc")
    cc_executable = shutil.which(cc_name)
    if cc_executable is None:
        raise PackageRouteError(
            "PACKAGE_TOOLCHAIN_MISSING", f"CC={cc_name!r} is not available on PATH"
        )
    make_display_name, make_identity = _version_identity(make_executable, "Make")
    cc_display_name, cc_identity = _version_identity(cc_executable, "C compiler")

    extra_elf_specs = [
        arg.split("=", 1)[1]
        for arg in plan["commands"]["codegen"]
        if arg.startswith("--extra-elf=")
    ]
    link_map = output_dir / f"{selected_name}.map"
    build_environment = os.environ.copy()
    build_environment.update({
        "PYTHON": python_command,
        "GAME_NAME": selected_name,
        "GAME_ELF": game_elf.as_posix(),
        "GAME_BASE": plan["environment"]["GAME_BASE"],
        "GAME_ENTRY": plan["environment"]["GAME_ENTRY"],
        "GAME_EXTRA_ELFS": " ".join(extra_elf_specs),
        "GAME_PSP_HEADER": psp_header.as_posix() if psp_header is not None else "",
        "TITLE_EXTRA_SPANS": plan["environment"]["TITLE_EXTRA_SPANS"],
        "HST_EXTRA_SPANS": "",
        "TITLE_MANIFEST": manifest_path.as_posix(),
        "BUILD_DIR": output_dir.as_posix(),
        "FUNCS_PER_CHUNK": str(funcs_per_chunk),
        "CODEGEN_PROFILE_ARG": (
            f"--profile={plan['codegen_profile']}"
            if plan["codegen_profile"] != "none"
            else ""
        ),
        "CODEGEN_USER_ARGS": "",
        "LINK_MAP": link_map.as_posix(),
        "PUBLIC_SAFE": "1" if public_safe else "0",
    })
    try:
        built = subprocess.run(
            [make_executable, "--no-print-directory", "all"],
            cwd=ROOT,
            env=build_environment,
            check=False,
        )
    except OSError as exc:
        raise PackageRouteError("PACKAGE_BUILD_FAILED", str(exc)) from exc
    if built.returncode != 0:
        raise PackageRouteError(
            "PACKAGE_BUILD_FAILED", f"{make_display_name} exited with status {built.returncode}"
        )

    current_input_hashes = _hash_package_inputs(
        manifest_path, game_elf, selected_modules, module_input_paths, psp_header
    )
    if current_input_hashes != input_hashes:
        raise PackageRouteError(
            "PACKAGE_INPUT_CHANGED",
            "a package input changed while the build was running; rerun with a stable input set",
        )

    executable_suffix = ".exe" if os.name == "nt" else ""
    executable_name = f"{selected_name}{executable_suffix}"
    compiled_executable = output_dir / executable_name
    image_path = output_dir / f"{selected_name}_image.bin"
    funcs_header = output_dir / f"{selected_name}_recomp_funcs.h"
    stub_report = output_dir / f"{selected_name}_recomp_stubs.txt"
    required_outputs = (compiled_executable, image_path, funcs_header, stub_report, link_map)
    missing_outputs = [path.name for path in required_outputs if not path.is_file()]
    if missing_outputs:
        raise PackageRouteError(
            "PACKAGE_BUILD_INCOMPLETE", "missing build outputs: " + ", ".join(missing_outputs)
        )

    entry_symbols = re.findall(
        r"(?m)^\s*void\s+([fr])_[0-9a-fA-F]+\s*\(",
        funcs_header.read_text(encoding="ascii"),
    )
    aot_callables = sum(1 for prefix in entry_symbols if prefix == "f")
    aot_resumes = sum(1 for prefix in entry_symbols if prefix == "r")
    unsupported_regions, unsupported_instructions = _read_codegen_fallbacks(stub_report, sources)
    object_files = sorted(
        (path for path in output_dir.rglob("*.o") if path.is_file()),
        key=lambda path: path.relative_to(output_dir).as_posix(),
    )
    generated_objects = [
        {
            "path": path.relative_to(output_dir).as_posix(),
            "sha256": _sha256_file(path),
        }
        for path in object_files
    ]
    executable_sha256 = _sha256_file(compiled_executable)
    module_hashes = {row["name"]: row["sha256"] for row in input_hashes["modules"]}
    required_assets = _required_local_assets(
        normalized, selected_optional, module_hashes, output_dir
    )
    runtime_header_hash = _sha256_file(ROOT / "src" / "rt" / "recomp.h")
    tools = {
        "python": platform.python_version(),
        "planner": {"sha256": _sha256_file(Path(__file__).resolve())},
        "analyzer": {"sha256": _sha256_file(ROOT / "tools" / "analyze.py")},
        "codegen": {"sha256": _sha256_file(ROOT / "tools" / "codegen.py")},
        "make": {"command": make_display_name, "identity": make_identity},
        "compiler": {"command": cc_display_name, "identity": cc_identity},
    }
    unsupported = {
        "imports": unsupported_imports,
        "instructions": unsupported_instructions,
        "regions": unsupported_regions,
    }
    coverage = {
        **analysis_summary,
        "aot_entries": len(entry_symbols),
        "aot_callable_entries": aot_callables,
        "aot_resume_entries": aot_resumes,
        "fallback_entries": len(unsupported_regions),
        "unsupported_import_count": len(unsupported_imports),
        "unsupported_instruction_count": len(unsupported_instructions),
        "unsupported_region_count": len(unsupported_regions),
    }
    report = {
        "format": BUILD_REPORT_FORMAT,
        "schema_version": PACKAGE_SCHEMA_VERSION,
        "title_id": normalized["id"],
        "runtime_abi": {"name": "CpuState", "version": codegen_abi_version()},
        "input_hashes": input_hashes,
        "tools": tools,
        "coverage": coverage,
        "unsupported": unsupported,
        "analysis_diagnostics": sorted(
            diagnostics, key=lambda row: (row["module"], row["message"])
        ),
        "artifacts": {
            "executable": executable_name,
            "link_map": link_map.name,
        },
    }
    package = {
        "format": PACKAGE_FORMAT,
        "schema_version": PACKAGE_SCHEMA_VERSION,
        "title": {
            "id": normalized["id"],
            "display_name": normalized["display_name"],
            "kind": normalized["kind"],
            "manifest_sha256": input_hashes["manifest"]["sha256"],
            "protected_digest": compute_protected_digest(normalized),
        },
        "inputs": input_hashes,
        "runtime": {
            "abi": "CpuState",
            "abi_version": codegen_abi_version(),
            "abi_header_sha256": runtime_header_hash,
            "run_entry": _hex(_resolve_run_entry(normalized)),
            "runtime_contract": normalized.get("runtime_contract"),
            "runtime_bindings": normalized.get("runtime_bindings", {}),
            "required_runtime_bindings": normalized.get("required_runtime_bindings", []),
        },
        "executable": {
            "path": executable_name,
            "sha256": executable_sha256,
            "guest_entry": _hex(normalized["executable"]["entry"]),
        },
        "generated_objects": generated_objects,
        "required_local_assets": required_assets,
        "build_report": "build-report.json",
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    report_path.write_text(canonical_json(report), encoding="utf-8", newline="\n")
    package_path.write_text(canonical_json(package), encoding="utf-8", newline="\n")
    return package


def codegen_abi_version() -> int:
    """Return the version embedded in generated code and checked by recomp.h."""
    import codegen

    return int(codegen.CPU_STATE_ABI_VERSION)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--game-name")
    parser.add_argument("--game-elf", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--package", action="store_true", help="build and describe a runnable AOT package")
    parser.add_argument("--output-dir", type=Path, help="dedicated untracked package/build directory")
    parser.add_argument("--module-dir", type=Path)
    parser.add_argument("--psp-header", type=Path)
    parser.add_argument("--make-command", help="Make executable (default: mingw32-make on Windows, make elsewhere)")
    parser.add_argument("--public-safe", action="store_true", help="build with the public-safe runtime backends")
    parser.add_argument(
        "--profile",
        dest="codegen_profile",
        choices=("none", "hst"),
    )
    parser.add_argument(
        "--manager-plan",
        action="store_true",
        help="emit the bounded manager/build configuration instead of command vectors",
    )
    parser.add_argument(
        "--print-protected-digest",
        action="store_true",
        help="print the manifest's protected-semantics digest and exit (no plan is emitted)",
    )
    parser.add_argument("--include-optional-module", action="append", default=[])
    parser.add_argument("--funcs-per-chunk", type=int, default=2000)
    parser.add_argument("--python-command", default="python")
    args = parser.parse_args(argv)
    try:
        manifest = title_manifest.load_manifest(args.manifest)
    except (OSError, ValueError) as exc:
        prefix = "PACKAGE_INVALID_MANIFEST" if args.package else "ERROR"
        print(f"{prefix}: {exc}", file=sys.stderr)
        return 2
    try:
        if args.package:
            if args.print_protected_digest or args.manager_plan:
                parser.error("--package cannot be combined with --print-protected-digest or --manager-plan")
            if args.build_dir is not None:
                parser.error("use --output-dir with --package; --build-dir is for plan output")
            missing = [
                flag
                for flag, value in (("--game-elf", args.game_elf), ("--output-dir", args.output_dir))
                if not value
            ]
            if missing:
                parser.error(f"--package requires {', '.join(missing)}")
            build_package(
                manifest,
                manifest_path=args.manifest,
                game_elf=args.game_elf,
                output_dir=args.output_dir,
                game_name=args.game_name,
                module_dir=args.module_dir,
                psp_header=args.psp_header,
                codegen_profile=args.codegen_profile,
                include_optional_modules=set(args.include_optional_module),
                funcs_per_chunk=args.funcs_per_chunk,
                python_command=args.python_command,
                make_command=args.make_command,
                public_safe=args.public_safe,
            )
            return 0
        if args.output_dir is not None or args.make_command is not None or args.public_safe:
            parser.error("--output-dir, --make-command, and --public-safe require --package")
        if args.print_protected_digest:
            print(compute_protected_digest(manifest))
            return 0
        missing = [
            flag
            for flag, value in (
                ("--game-name", args.game_name),
                ("--game-elf", args.game_elf),
                ("--build-dir", args.build_dir),
            )
            if not value
        ]
        if missing:
            parser.error(f"the following arguments are required: {', '.join(missing)}")
        plan_builder = build_manager_plan if args.manager_plan else build_plan
        plan = plan_builder(
            manifest,
            game_name=args.game_name,
            game_elf=args.game_elf,
            build_dir=args.build_dir,
            module_dir=args.module_dir,
            psp_header=args.psp_header,
            codegen_profile=args.codegen_profile,
            include_optional_modules=set(args.include_optional_module),
            funcs_per_chunk=args.funcs_per_chunk,
            python_command=args.python_command,
        )
        print(canonical_json(plan), end="")
        return 0
    except (OSError, ValueError) as exc:
        prefix = "PACKAGE_ROUTE_FAILED" if args.package else "ERROR"
        print(f"{prefix}: {exc}", file=sys.stderr)
        return 2
    except PackageRouteError as exc:
        print(f"{exc.code}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
