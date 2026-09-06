# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Creates clean local integration slice branches cut from origin/main using temporary git index."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent

SLICES = [
    {
        "name": "slice/title-catalog-isolation",
        "msg": "titles, core: isolate public catalog and harden manifest parser with strict AST validation",
        "files": [
            "src/core/nk_title_manifest.h",
            "src/core/nk_title_manifest.c",
            "src/core/generated/nk_title_catalog.h",
            "src/core/generated/nk_title_catalog.c",
            "tools/nk_core/title_registry.py",
            "tools/test_public_title_isolation.py",
            "tests/native/test_manifest_parser.c",
            "tools/test_title_manifest_parity.py",
        ],
    },
    {
        "name": "slice/platform-win32-process",
        "msg": "platform: implement robust Win32 CreateProcessW spawning, quoting, and Unicode environment",
        "files": [
            "src/core/nk_platform.h",
            "src/core/nk_platform_win32.c",
            "src/core/nk_platform_posix.c",
            "tests/native/argv_echo_helper.c",
            "tests/native/test_win32_process.c",
        ],
    },
    {
        "name": "slice/iso-sfo-library-durability",
        "msg": "core, iso: enforce ECMA-119 boundary semantics, duplicate SFO policy, and library save durability",
        "files": [
            "src/core/nk_types.h",
            "src/core/nk_iso.h",
            "src/core/nk_iso.c",
            "src/core/nk_library.h",
            "src/core/nk_library.c",
            "tools/nk_core/iso_inspect.py",
            "tests/native/test_parsers_hostile.c",
            "tools/test_iso_parity.py",
        ],
    },
    {
        "name": "slice/title-manifest-overlay-session",
        "msg": "launch: implement typed launch session builder, binary resolution, and overlay policy",
        "files": [
            "src/core/nk_launch.h",
            "src/core/nk_launch.c",
            "tests/native/test_core_catalog.c",
        ],
    },
    {
        "name": "slice/native-player-shell",
        "msg": "player: implement cross-platform SDL3 player shell, UI renderer, and runtime failure surfacing",
        "files": [
            "src/player/main.c",
            "src/player/player_state.h",
            "src/player/player_state.c",
            "src/player/iso_reader.h",
            "src/player/iso_reader.c",
            "src/player/ui_renderer.h",
            "src/player/ui_renderer.c",
        ],
    },
    {
        "name": "slice/cmake-parity-bootstrap",
        "msg": "build: add CMake build system with SDL3 bootstrap and machine-checkable Make/CMake parity",
        "files": [
            "CMakeLists.txt",
            "Makefile",
            "tools/test_build_system_parity.py",
        ],
    },
]


def main() -> None:
    build_dir = ROOT / "build"
    build_dir.mkdir(exist_ok=True)
    temp_index = build_dir / "temp_slice_index"
    env = dict(os.environ)
    env["GIT_INDEX_FILE"] = str(temp_index)

    base_sha = subprocess.check_output(
        ["git", "rev-parse", "origin/main"], cwd=ROOT, text=True
    ).strip()
    print(f"[SLICE] Base commit (origin/main): {base_sha}")

    for s in SLICES:
        branch = s["name"]
        msg = s["msg"]
        files = s["files"]

        if temp_index.exists():
            temp_index.unlink()

        # 1. Initialize temporary index with tree of origin/main
        subprocess.check_call(["git", "read-tree", "origin/main"], cwd=ROOT, env=env)

        # 2. Add files for this slice
        subprocess.check_call(["git", "add"] + files, cwd=ROOT, env=env)

        # 3. Write tree
        tree_sha = subprocess.check_output(["git", "write-tree"], cwd=ROOT, env=env, text=True).strip()

        # 4. Commit tree on top of origin/main
        commit_sha = subprocess.check_output(
            ["git", "commit-tree", tree_sha, "-p", base_sha, "-m", msg],
            cwd=ROOT,
            env=env,
            text=True,
        ).strip()

        # 5. Create or update local branch
        subprocess.check_call(["git", "branch", "-f", branch, commit_sha], cwd=ROOT)
        print(f"[SLICE] Created {branch} -> commit {commit_sha[:10]}")

    if temp_index.exists():
        temp_index.unlink()

    print("[SLICE] All local integration slice branches created successfully!")


if __name__ == "__main__":
    main()
