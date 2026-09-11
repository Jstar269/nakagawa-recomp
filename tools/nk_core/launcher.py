# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Runtime execution builder and session binder for Nakagawa Recomp."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


class RuntimeLaunchError(RuntimeError):
    """Raised when runtime parameters or binaries are invalid."""


class RuntimeLauncher:
    """Constructs explicit, fail-closed runtime launch sessions."""

    def __init__(self, repo_root: Optional[Path] = None) -> None:
        self.repo_root = Path(repo_root or Path.cwd()).resolve()

    def build_launch_plan(
        self,
        game_dir: Path | str,
        profile: str = "Standard",
        fps_cap: int = 30,
        gpu_ge: bool = True,
        software_render: bool = False,
        no_gui: bool = False,
    ) -> Tuple[List[str], Dict[str, str]]:
        """Construct deterministic argv and env dictionary for runtime execution."""
        g_dir = Path(game_dir).resolve()
        manifest_file = g_dir / "manifest.json"
        if not manifest_file.is_file():
            raise RuntimeLaunchError(f"Missing game manifest: {manifest_file}")

        with open(manifest_file, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        title_id = manifest.get("title_id", "hst")
        disc_id = manifest.get("disc_id", "UCUS98701")
        iso_path = manifest.get("iso_path", "")

        # Resolve binary path
        exe_candidates = [
            self.repo_root / "build" / "hst" / "hst.exe",
            self.repo_root / "build" / title_id / f"{title_id}.exe",
            self.repo_root / "hst.exe",
        ]
        exe_path: Optional[Path] = None
        for cand in exe_candidates:
            if cand.is_file():
                exe_path = cand
                break

        if not exe_path:
            raise RuntimeLaunchError(
                f"Runtime binary not found. Looked in: {[str(c) for c in exe_candidates]}. "
                "Build the runtime executable first."
            )

        cmd = [str(exe_path)]

        # Environment variables configured strictly for this execution session
        env = dict(os.environ)
        if iso_path:
            iso_file = Path(iso_path)
            if not iso_file.is_file():
                # Check fallback in prepared directory: disc/game.iso
                fallback_iso = g_dir / "disc" / "game.iso"
                if fallback_iso.is_file():
                    iso_path = str(fallback_iso)
                else:
                    raise RuntimeLaunchError(
                        f"Game source ISO not found at '{iso_path}' and no local fallback exists. "
                        "The ISO file may have been moved, renamed, or deleted. Please re-locate it."
                    )
            env["PSP_ISO"] = iso_path
        env["SR_FPS_CAP"] = str(fps_cap)
        env["SR_GPU_GE"] = "0" if software_render else ("1" if gpu_ge else "0")
        env["SR_DATAROOT"] = str(g_dir / "extracted")

        if profile == "Performance":
            env["SR_DEBUG"] = "0"
        elif profile == "Benchmark":
            env["SR_DEBUG"] = "0x20"  # perf telemetry
        elif profile == "Diagnostics":
            env["SR_DEBUG"] = "0xFF"

        return cmd, env
