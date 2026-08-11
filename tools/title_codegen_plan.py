#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Build a deterministic, read-only code-generation plan from a title manifest.

The public manifest supplies source-owned title configuration. Private executable,
module, and PSP-header paths remain explicit command-line bindings and are never
written back to the manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any

import title_manifest

GAME_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
MIN_FUNCS_PER_CHUNK = 1
MAX_FUNCS_PER_CHUNK = 100_000
MANAGER_PLAN_VERSION = 1


def compute_protected_digest(manifest: dict[str, Any]) -> str:
    """Deterministic digest of the protected title semantics.

    Covers the entire validated manifest except the free-text ``notes`` field.
    A manager adapter can therefore fail closed against one opaque constant
    (the digest of the checked-in manifest) instead of re-encoding every
    protected value, so a mutation anywhere in the title contract changes the
    digest and is rejected before Make runs.
    """
    normalized = title_manifest.validate_manifest(manifest)
    protected = {key: value for key, value in normalized.items() if key != "notes"}
    rendered = title_manifest.canonical_json(protected)
    return hashlib.sha256(rendered.encode("utf-8")).hexdigest()


class TitleCodegenPlanError(ValueError):
    """Fail-closed plan construction error."""


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
        "HST_EXTRA_SPANS": rendered,
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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--game-name")
    parser.add_argument("--game-elf", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--module-dir", type=Path)
    parser.add_argument("--psp-header", type=Path)
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
        help="print the protected-contract digest and exit (no plan is emitted)",
    )
    parser.add_argument("--include-optional-module", action="append", default=[])
    parser.add_argument("--funcs-per-chunk", type=int, default=2000)
    parser.add_argument("--python-command", default="python")
    args = parser.parse_args(argv)
    try:
        manifest = title_manifest.load_manifest(args.manifest)
        if args.print_protected_digest:
            print(compute_protected_digest(manifest))
            return 0
        if not args.game_name or not args.game_elf or not args.build_dir:
            parser.error("--game-name, --game-elf and --build-dir are required")
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
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
