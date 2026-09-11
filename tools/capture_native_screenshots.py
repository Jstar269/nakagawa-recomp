#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

import argparse
import os
import subprocess
import sys
from pathlib import Path
from PIL import Image


def default_player_path(repo_root: Path) -> Path:
    """The player binary for THIS host.

    The Makefile emits `build/nakagawa_player` with no extension on Linux and
    macOS. Probing only the .exe name made this capture utility exit before
    taking a single screenshot everywhere except Windows, which made the
    screenshot matrix a Windows-only artifact of a cross-platform player.
    """
    suffix = ".exe" if os.name == "nt" else ""
    return repo_root / "build" / f"nakagawa_player{suffix}"


def capture_matrix(player_exe: Path | None = None):
    repo_root = Path(__file__).resolve().parent.parent
    player_exe = Path(player_exe) if player_exe else default_player_path(repo_root)
    out_dir = repo_root / "docs" / "ui-baseline"
    out_dir.mkdir(parents=True, exist_ok=True)

    if not player_exe.exists():
        print(f"ERROR: {player_exe} not found. Please compile it first.")
        sys.exit(1)

    views = [
        ("native_01_empty_library.png", ["--view=empty", "--empty", "--width=1280", "--height=720"]),
        ("native_02_add_game.png", ["--view=empty", "--empty", "--width=1280", "--height=720"]),
        ("native_03_iso_inspecting.png", ["--view=inspecting", "--width=1280", "--height=720"]),
        ("native_04_supported_game.png", ["--view=supported", "--width=1280", "--height=720"]),
        ("native_05_unsupported_game.png", ["--view=unsupported", "--width=1280", "--height=720"]),
        ("native_06_preparing.png", ["--view=preparing", "--width=1280", "--height=720"]),
        ("native_07_ready_library.png", ["--view=library", "--width=1280", "--height=720"]),
        ("native_08_settings.png", ["--view=settings", "--width=1280", "--height=720"]),
        ("native_09_missing_source_error.png", ["--view=error", "--width=1280", "--height=720"]),
        ("native_10_library_1080p.png", ["--view=library", "--width=1920", "--height=1080"]),
        ("native_11_settings_1080p.png", ["--view=settings", "--width=1920", "--height=1080"]),
    ]

    temp_bmp = repo_root / "build" / "temp_screen.bmp"

    print("============================================================")
    print("CAPTURING NATIVE PLAYER SCREENSHOT MATRIX")
    print("============================================================")

    results = []
    for filename, args in views:
        if temp_bmp.exists():
            temp_bmp.unlink()

        cmd = [str(player_exe), f"--screenshot={temp_bmp}"] + args
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"FAILED: {' '.join(cmd)}")
            print("STDERR:", proc.stderr)
            results.append((filename, False, "process returned error"))
            continue

        if not temp_bmp.exists():
            print(f"FAILED: {temp_bmp} was not generated.")
            results.append((filename, False, "no bmp generated"))
            continue

        out_png = out_dir / filename
        with Image.open(temp_bmp) as im:
            im.save(out_png, "PNG")
            w, h = im.size

        if temp_bmp.exists():
            temp_bmp.unlink()

        size_kb = out_png.stat().st_size / 1024.0
        print(f"CAPTURED: {filename} ({w}x{h}, {size_kb:.1f} KB)")
        results.append((filename, True, f"{w}x{h} ({size_kb:.1f} KB)"))

    print("\nSummary:")
    for name, ok, detail in results:
        status = "OK" if ok else "FAIL"
        print(f"  [{status}] {name:35s} -> {detail}")

    all_ok = all(r[1] for r in results)
    return 0 if all_ok else 1

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--player",
        type=Path,
        default=None,
        help="path to the player binary (default: build/nakagawa_player[.exe] for this host)",
    )
    args = parser.parse_args()
    return capture_matrix(args.player)


if __name__ == "__main__":
    sys.exit(main())
