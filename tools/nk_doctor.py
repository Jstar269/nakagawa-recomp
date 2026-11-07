#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Fail-closed workspace diagnostics for Nakagawa Recomp.

This command validates the host toolchain, manifest-declared game inputs, runtime
assets, build products, and a small set of publication-facing repository
contracts. It never copies, decrypts, extracts, or uploads private material.

The output is designed for both humans and wrappers such as nk.ps1. Exit
status is zero only when no FAIL result is present (and, with --strict, when no
WARN result is present).
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
import json
import os
from pathlib import Path
import sys
from typing import Sequence

from nk_doctor_core import Report
from nk_doctor_checks import (
    TitleDiagnosticContext,
    check_agent_identity,
    check_build_products,
    check_build_profile,
    check_data_directory,
    check_long_paths,
    check_platform,
    check_private_inputs,
    check_repository_contract,
    check_runtime_dependencies,
    check_runtime_package_cache,
    check_typography_runtime,
    check_save_root,
    check_toolchain,
    check_vfpu_assets,
    check_worktree_accumulation,
    title_diagnostic_context,
)


def render_text(report: Report) -> str:
    lines = [
        f"Nakagawa Recomp Doctor -- scope={report.scope}",
        f"root={report.root}",
        "",
    ]
    for result in report.results:
        location = f" [{result.path}]" if result.path else ""
        display_status = {"FAIL": "ERROR", "WARN": "WARNING"}.get(result.status, result.status)
        lines.append(f"[{display_status}] {result.code}: {result.summary}{location}")
        if result.detail:
            lines.append(f"       {result.detail}")
        if result.remediation:
            lines.append(f"       Fix: {result.remediation}")
    counts = report.counts()
    lines.extend(
        [
            "",
            "Summary: " + " ".join(f"{key}={counts[key]}" for key in ("PASS", "WARN", "FAIL", "INFO")),
        ]
    )
    return "\n".join(lines)


def render_json(report: Report, strict: bool) -> str:
    payload = {
        "schema_version": 1,
        "tool": "nk_doctor",
        "root": str(report.root),
        "scope": report.scope,
        "strict": strict,
        "counts": report.counts(),
        "exit_code": report.exit_code(strict),
        "results": [asdict(result) for result in report.results],
    }
    return json.dumps(payload, indent=2, sort_keys=True)


def _long_path_candidates(
    root: Path,
    title_context: TitleDiagnosticContext,
    user_data_root: Path | None,
) -> tuple[tuple[str, Path], ...]:
    """Name expected input, build, and output paths without opening those paths."""
    candidates: list[tuple[str, Path]] = []

    def add(label: str, path: Path | None) -> None:
        if path is not None:
            candidates.append((label, path))

    if title_context.has_declared_executable_path and title_context.game_elf_candidates:
        add("configured executable input", title_context.game_elf_candidates[0])
    elif title_context.game_elf_candidates:
        for path in title_context.game_elf_candidates:
            add("executable input candidate", path)
    add("configured PSP header input", title_context.psp_header_path)
    add("configured disc image input", title_context.disc_image)
    add("manifest asset input root", title_context.data_root)
    if title_context.module_dir is not None:
        if title_context.required_modules:
            for name in title_context.required_modules:
                add("required PRX input", title_context.module_dir / name)
        else:
            add("module input root", title_context.module_dir)

    sr_dataroot = os.environ.get("SR_DATAROOT")
    if sr_dataroot:
        path = Path(sr_dataroot)
        if path.is_absolute():
            add("configured asset input root", path)

    build_dir_value = os.environ.get("BUILD_DIR")
    if build_dir_value:
        build_dir = Path(build_dir_value)
        if not build_dir.is_absolute():
            build_dir = root / build_dir
    else:
        build_root_value = os.environ.get("NK_BUILD_ROOT") or os.environ.get("BUILD_ROOT")
        if build_root_value:
            build_root = Path(build_root_value)
            if not build_root.is_absolute():
                build_root = root / build_root
            build_dir = build_root / title_context.game_name
        else:
            build_dir = root / "build" / title_context.game_name
    add("build executable output", build_dir / f"{title_context.game_name}.exe")
    add("build image output", build_dir / f"{title_context.game_name}_image.bin")

    memstick_value = os.environ.get("SR_MEMSTICK")
    if memstick_value:
        memstick_root = Path(memstick_value)
        if memstick_root.is_absolute():
            add("configured save output root", memstick_root)
    else:
        add("save output root", root / "memstick")
    log_root_value = os.environ.get("LOG_DIR")
    log_root = Path(log_root_value) if log_root_value else root / "logs"
    if not log_root.is_absolute():
        log_root = root / log_root
    for log_name in ("stdout_run.log", "stderr_run.log", "build_out_recomp.log"):
        add("run log output", log_root / log_name)
    for label, relative in (
        ("screenshot output root", Path("docs/ui-baseline")),
        ("oracle evidence root", Path("oracle")),
        ("oracle fixture output root", Path("fixtures/psp_oracle/build")),
        ("compile database output", Path("compile_commands.json")),
        ("tool scratch output root", Path("scratch")),
        ("temporary test output root", Path("tmp")),
        ("agent state output root", Path(".codex-remote-attachments")),
    ):
        add(label, root / relative)

    if user_data_root is not None:
        path = user_data_root if user_data_root.is_absolute() else Path.cwd() / user_data_root
        add("per-user package output root", path)

    return tuple(candidates)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root (default: parent of tools/)",
    )
    parser.add_argument(
        "--scope",
        choices=("repo", "inputs", "build", "products", "run", "all"),
        default="all",
        help="checks to run",
    )
    default_msys = os.environ.get("MSYS_PATH")
    if not default_msys:
        default_msys = r"C:\msys64\ucrt64\bin"
    parser.add_argument(
        "--msys-path",
        type=Path,
        default=Path(default_msys),
        help="MSYS2 UCRT64 bin directory (default: MSYS_PATH or C:\\msys64\\ucrt64\\bin)",
    )
    parser.add_argument(
        "--vulkan-sdk",
        type=Path,
        default=None,
        help="explicit Vulkan SDK directory (otherwise use VULKAN_SDK, then the newest valid C:\\VulkanSDK installation)",
    )
    parser.add_argument(
        "--sdl3-path",
        type=Path,
        default=None,
        help="explicit SDL3 directory (otherwise use SDL3_DIR, then the MSYS2 UCRT64 toolchain)",
    )
    parser.add_argument(
        "--title-manifest",
        type=Path,
        default=None,
        help="optional title manifest path (validates title identity against manifest.disc.id)",
    )
    parser.add_argument(
        "--game-name",
        default=None,
        help="optional portable build target name (otherwise use the selected manifest)",
    )
    parser.add_argument(
        "--user-data-root",
        type=Path,
        help="per-user data root containing packages/<DISC_ID>",
    )
    parser.add_argument(
        "--disc-id",
        help="validate the private runtime package cache for this disc ID",
    )
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument("--strict", action="store_true", help="make warnings produce exit status 2")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except OSError:
            pass
    if hasattr(sys.stderr, "reconfigure"):
        try:
            sys.stderr.reconfigure(encoding="utf-8", errors="replace")
        except OSError:
            pass
    args = build_parser().parse_args(argv)
    root = args.root.resolve()
    report = Report(root, args.scope)
    check_data_directory(report)

    manifest_arg = args.title_manifest
    manifest_data: dict[str, object] | None = None
    if manifest_arg is not None:
        manifest_path = manifest_arg if manifest_arg.is_absolute() else (root / manifest_arg)
        if not manifest_path.is_file():
            report.fail(
                "INPUT_TITLE_MANIFEST",
                f"Configured title manifest was not found: {manifest_arg}",
                path=manifest_path,
                remediation="Provide a path to an existing title manifest.",
            )
            manifest_arg = None
        else:
            try:
                import title_manifest
                manifest_data = title_manifest.load_manifest(manifest_path)
            except Exception as exc:
                report.fail(
                    "INPUT_TITLE_MANIFEST",
                    f"Invalid title manifest: {exc}",
                    path=manifest_path,
                    remediation="Fix the title manifest syntax or schema.",
                )
    else:
        default_path = root / "assets" / "titles" / "synthetic.json"
        if default_path.is_file():
            try:
                import title_manifest
                manifest_data = title_manifest.load_manifest(default_path)
            except Exception as exc:
                report.fail(
                    "INPUT_TITLE_MANIFEST",
                    f"Invalid default synthetic title manifest: {exc}",
                    path=default_path,
                    remediation="Fix the default synthetic title manifest syntax or schema.",
                )

    title_context = title_diagnostic_context(
        root,
        manifest_data,
        game_name=args.game_name,
    )

    check_long_paths(
        report,
        root,
        extra_paths=_long_path_candidates(root, title_context, args.user_data_root),
    )

    if args.scope in {"repo", "all"}:
        check_repository_contract(report)
        check_agent_identity(report)
        check_worktree_accumulation(report, root)
    if args.scope in {"build", "run", "all"}:
        check_platform(report)
    if args.scope in {"build", "all"}:
        check_toolchain(report, args.msys_path, args.vulkan_sdk, root, sdl3_path=args.sdl3_path)
    if args.scope in {"inputs", "build", "all"}:
        check_private_inputs(
            report,
            need_iso=args.scope in {"inputs", "all"},
            need_assets=args.scope in {"inputs", "all"},
            title_manifest=manifest_data,
            title_context=title_context,
        )
    if args.scope in {"inputs", "run", "all"}:
        check_save_root(report, root)
    if args.scope == "run":
        check_private_inputs(
            report,
            need_iso=True,
            need_assets=True,
            title_manifest=manifest_data,
            title_context=title_context,
        )
    if args.scope in {"products", "run", "all"}:
        check_build_products(report, title_context.game_name)
        check_build_profile(report, root, title_context.game_name)
    if args.scope in {"run", "all"}:
        check_vfpu_assets(report)
        check_runtime_dependencies(report, args.msys_path, title_context.game_name)
        check_typography_runtime(report, args.msys_path)
    if args.disc_id:
        if args.user_data_root is None:
            report.fail(
                "RUNTIME_PACKAGE_CACHE",
                "--disc-id requires --user-data-root for private package validation",
                remediation="Pass the per-user data root and rerun Doctor.",
            )
        elif args.scope in {"products", "run", "all"}:
            check_runtime_package_cache(report, args.user_data_root, args.disc_id.upper())

    output = render_json(report, args.strict) if args.json else render_text(report)
    print(output)
    returned: int = report.exit_code(args.strict)
    return returned


if __name__ == "__main__":
    raise SystemExit(main())
