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

    def _resolve_image(self, exe_path: Path, title_id: str) -> Optional[Path]:
        """Locate the runtime image, in the same order as find_candidate_image."""
        candidates = [
            exe_path.with_name(exe_path.stem + "_image.bin"),
            exe_path.parent / "hst_image.bin",
            self.repo_root / "build" / "hst" / "hst_image.bin",
            self.repo_root / "runtime" / "hst_image.bin",
        ]
        if title_id:
            candidates.append(
                self.repo_root / "build" / title_id / f"{title_id}_image.bin"
            )
        for cand in candidates:
            if cand.is_file():
                return cand
        return None

    def _resolve_addresses(self, title_id: str, disc_id: str) -> Tuple[int, int]:
        """Base and entry for the title, from the canonical registry.

        Fail closed rather than guessing: a wrong entry point does not produce a
        readable error, it produces a runtime that executes the wrong bytes.
        """
        from .title_registry import get_default_registry

        registry = get_default_registry()
        profile = None
        if disc_id:
            profile = registry.lookup_by_disc_id(disc_id)
        if profile is None and title_id:
            profile = registry.lookup_by_id(title_id)
        if profile is None:
            raise RuntimeLaunchError(
                f"No title profile for disc '{disc_id}' / title '{title_id}', so the "
                "runtime base and entry addresses are unknown."
            )
        return profile.executable_base, profile.executable_entry

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

        # Resolve binary path. Both the extensioned and extensionless names are
        # probed: the runtime has no .exe suffix on Linux or macOS.
        exe_candidates: List[Path] = []
        for stem in (
            self.repo_root / "build" / "hst" / "hst",
            self.repo_root / "build" / title_id / title_id,
            self.repo_root / "hst",
        ):
            exe_candidates.append(stem.with_name(stem.name + ".exe"))
            exe_candidates.append(stem)

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

        # The runtime's image mode needs --image, the image path, the base and
        # entry addresses and two trace placeholders; src/rt/driver.c exits
        # through its argc < 4 usage path without them. A plan consisting of
        # the executable alone was therefore never runnable, whatever else it
        # resolved correctly. The native launcher in src/core/nk_launch.c
        # builds exactly this argv, and the two must not drift apart.
        image_path = self._resolve_image(exe_path, title_id)
        if image_path is None:
            raise RuntimeLaunchError(
                f"Runtime image not found for '{title_id}'. Looked beside "
                f"{exe_path} and under build/. Build the runtime image first; "
                "returning a command that exits on usage would not be a launch plan."
            )

        base, entry = self._resolve_addresses(title_id, disc_id)

        cmd = [
            str(exe_path),
            "--image",
            str(image_path),
            f"0x{base:x}",
            f"0x{entry:08x}",
            "none",
            "none",
            "--sched" if no_gui else "--gui",
        ]

        return cmd, env
