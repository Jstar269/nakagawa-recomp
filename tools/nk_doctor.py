#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Fail-closed workspace diagnostics for Nakagawa Recomp.

Canonical successor to tools/hst_doctor.py (which is now a deprecated forwarding
wrapper). Identical behaviour; updated to import from nk_doctor_core and
nk_doctor_checks instead of the legacy hst_* modules.

This command validates the host toolchain, private game-input layout, runtime
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
from pathlib import Path
import sys
from typing import Sequence

from nk_doctor_core import Report, _parse_elf, _validate_iso, _validate_pe_x64
from nk_doctor_checks import (
    check_agent_identity,
    check_build_products,
    check_build_profile,
    check_platform,
    check_private_inputs,
    check_repository_contract,
    check_runtime_dependencies,
    check_save_root,
    check_toolchain,
    check_vfpu_assets,
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


def render_json(report: Report, strict: bool, *, tool_name: str = "nk_doctor") -> str:
    payload = {
        "schema_version": 1,
        "tool": tool_name,
        "root": str(report.root),
        "scope": report.scope,
        "strict": strict,
        "counts": report.counts(),
        "exit_code": report.exit_code(strict),
        "results": [asdict(result) for result in report.results],
    }
    return json.dumps(payload, indent=2, sort_keys=True)


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
    parser.add_argument(
        "--msys-path",
        type=Path,
        default=Path(r"C:\msys64\ucrt64\bin"),
        help="MSYS2 UCRT64 bin directory",
    )
    parser.add_argument(
        "--vulkan-sdk",
        type=Path,
        default=None,
        help="explicit Vulkan SDK directory (otherwise use VULKAN_SDK, then the newest valid C:\\VulkanSDK installation)",
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
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument("--strict", action="store_true", help="make warnings produce exit status 2")
    return parser


def main(
    argv: Sequence[str] | None = None,
    *,
    tool_name: str = "nk_doctor",
    legacy_default_title: bool = False,
) -> int:
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
    elif not legacy_default_title:
        # nk.ps1 and nk_doctor default to the public synthetic title.  The
        # deprecated HST wrapper opts into its own compatibility default.
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
        legacy_default=legacy_default_title,
    )

    if args.scope in {"repo", "all"}:
        check_repository_contract(report)
        check_agent_identity(report)
    if args.scope in {"build", "run", "all"}:
        check_platform(report)
    if args.scope in {"build", "all"}:
        check_toolchain(report, args.msys_path, args.vulkan_sdk, root)
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

    output = render_json(report, args.strict, tool_name=tool_name) if args.json else render_text(report)
    print(output)
    return report.exit_code(args.strict)


if __name__ == "__main__":
    raise SystemExit(main())
