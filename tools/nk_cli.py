#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Headless CLI interface for Nakagawa Recomp title inspection, preparation, and launch."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from nk_core import (
    PreparationEngine,
    ProgressEvent,
    RuntimeLauncher,
    get_default_registry,
    inspect_iso,
)


def print_progress(event: ProgressEvent) -> None:
    pct_str = f"{event.percentage:.1f}%" if event.percentage is not None else "..."
    sys.stdout.write(f"[{event.stage.value}] {pct_str} {event.operation} - {event.message}\n")
    sys.stdout.flush()


def cmd_inspect(args: argparse.Namespace) -> int:
    try:
        meta = inspect_iso(args.iso)
    except Exception as exc:
        sys.stderr.write(f"Error inspecting ISO: {exc}\n")
        return 1

    payload = {
        "disc_id": meta.disc_id,
        "title": meta.title,
        "version": meta.version,
        "region": meta.region,
        "volume_id": meta.volume_id,
        "size_bytes": meta.size_bytes,
        "supported": meta.is_supported,
        "matched_profile": meta.matched_profile.id if meta.matched_profile else None,
    }
    if args.json:
        print(json.dumps(payload, indent=2))
    else:
        print(f"Disc ID:    {meta.disc_id}")
        print(f"Title:      {meta.title}")
        print(f"Region:     {meta.region}")
        print(f"Supported:  {'YES' if meta.is_supported else 'NO'}")
        if meta.matched_profile:
            print(f"Profile:    {meta.matched_profile.name} ({meta.matched_profile.id})")
    return 0 if meta.is_supported else 2


def cmd_prepare(args: argparse.Namespace) -> int:
    engine = PreparationEngine()
    result = engine.prepare_game(
        args.iso,
        on_progress=print_progress,
        destination_root=Path(args.dest) if args.dest else None,
    )
    if result.success:
        print(f"\nPreparation successful! Manifest written to: {result.manifest_path}")
        return 0
    else:
        sys.stderr.write(f"\nPreparation failed [{result.error_code}]: {result.error_message}\n")
        return 1


def cmd_launch(args: argparse.Namespace) -> int:
    launcher = RuntimeLauncher()
    try:
        cmd, env = launcher.build_launch_plan(
            args.game_dir,
            profile=args.profile,
            fps_cap=args.fps_cap,
            software_render=args.software,
        )
        print("Launch plan prepared:")
        print(f"Executable: {cmd[0]}")
        print(f"PSP_ISO:    {env.get('PSP_ISO')}")
        print(f"DATAROOT:   {env.get('SR_DATAROOT')}")
        print(f"FPS_CAP:    {env.get('SR_FPS_CAP')}")
        return 0
    except Exception as exc:
        sys.stderr.write(f"Launch planning error: {exc}\n")
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Nakagawa Recomp Headless CLI")
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    p_inspect = subparsers.add_parser("inspect", help="Inspect a PSP ISO image")
    p_inspect.add_argument("iso", help="Path to PSP ISO image")
    p_inspect.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    p_inspect.set_defaults(func=cmd_inspect)

    p_prep = subparsers.add_parser("prepare", help="Prepare an ISO for native execution")
    p_prep.add_argument("iso", help="Path to PSP ISO image")
    p_prep.add_argument("--dest", help="Optional destination games directory")
    p_prep.set_defaults(func=cmd_prepare)

    p_launch = subparsers.add_parser("launch", help="Plan launch arguments for a prepared game")
    p_launch.add_argument("game_dir", help="Path to prepared game directory (containing manifest.json)")
    p_launch.add_argument("--profile", default="Standard", choices=["Standard", "Performance", "Benchmark", "Diagnostics"])
    p_launch.add_argument("--fps-cap", type=int, default=30)
    p_launch.add_argument("--software", action="store_true", help="Use software GE rasterizer")
    p_launch.set_defaults(func=cmd_launch)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
