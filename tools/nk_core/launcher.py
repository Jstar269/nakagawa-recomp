# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Runtime execution builder and session binder for Nakagawa Recomp."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
from typing import Any, Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Generic launch-resolution candidate contract (#366) -- the single source of
# truth for BOTH launch planners.
#
# tools/title_catalog_codegen.py projects these constants into
# src/core/generated/nk_title_catalog.h so src/core/nk_launch.c consumes the
# same ordered candidate contract (title_catalog_codegen.py --verify fails
# closed on drift), and tools/test_nk_core.py proves machine parity: native
# and Python must select the same title/runtime/image/base/entry outcome for
# identical source-owned fixtures.
#
# Candidates are pure layout patterns keyed ONLY by the selected title's
# validated identity (its registry/catalog game_name and title id). No retail
# title name, disc id, or title-specific path may appear here: generic
# resolution has no default title and no wrong-title rescue path. A selected
# title launches its own validated runtime/package or fails closed.
# ---------------------------------------------------------------------------

#: Identity sources consulted in order: the manager-selected build name
#: first, then the versioned title id. Both come from validated title data.
NAME_SOURCES: Tuple[str, ...] = ("game_name", "title_id")

#: Runtime executable layouts, probed for EACH name source in NAME_SOURCES
#: order. The Windows spelling is probed before the extensionless one.
EXE_CANDIDATES: Tuple[str, ...] = (
    "build/{name}/{name}.exe",
    "build/{name}/{name}",
)

#: Runtime image layouts, probed for each name source AFTER the sibling image
#: of the resolved executable.
IMAGE_CANDIDATES: Tuple[str, ...] = (
    "build/{name}/{name}_image.bin",
)

#: Sibling image name transform: <executable stem> + IMAGE_SUFFIX.
IMAGE_SUFFIX: str = "_image.bin"

#: Portable build-name shape for every identity-derived path component.
_BUILD_NAME_RE = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")


class RuntimeLaunchError(RuntimeError):
    """Raised when runtime parameters or binaries are invalid."""


class RuntimeLauncher:
    """Constructs explicit, fail-closed runtime launch sessions.

    Identity rule: title_id/disc_id come only from the validated session
    manifest and the canonical title registry, and when both are present they
    must agree on exactly one validated profile. Missing, malformed, or
    disagreeing identity is an actionable validation error -- there is no
    default identity and no retail-title fallback of any kind.
    """

    def __init__(
        self,
        repo_root: Optional[Path] = None,
        registry: Optional[Any] = None,
    ) -> None:
        self.repo_root = Path(repo_root or Path.cwd()).resolve()
        # An explicit registry injection keeps private-title routes (and
        # tests) from mutating the process-wide default registry; the
        # default remains the canonical public registry.
        self._registry = registry

    def _get_registry(self) -> Any:
        if self._registry is not None:
            return self._registry
        from .title_registry import get_default_registry

        return get_default_registry()

    def _identity_names(self, title_profile: Any) -> Dict[str, str]:
        """Identity-derived path components, validated as portable names."""
        names = {
            "game_name": title_profile.game_name or "",
            "title_id": title_profile.id or "",
        }
        for source in NAME_SOURCES:
            name = names.get(source) or ""
            if name and not _BUILD_NAME_RE.fullmatch(name):
                raise RuntimeLaunchError(
                    f"Game name '{name}' is not a portable build identifier."
                )
        return names

    def _resolve_identity(self, manifest: Dict[str, Any]) -> Any:
        """Bind the session manifest to exactly one validated title profile.

        Fail closed rather than guessing: a wrong or missing identity does not
        produce a readable error, it produces a runtime paired with another
        title's session data.
        """
        title_id = manifest.get("title_id")
        disc_id = manifest.get("disc_id")
        for field, value in (("title_id", title_id), ("disc_id", disc_id)):
            if value is not None and not isinstance(value, str):
                raise RuntimeLaunchError(
                    f"Manifest identity field '{field}' must be a string."
                )
        title = title_id.strip() if isinstance(title_id, str) else ""
        disc = disc_id.strip() if isinstance(disc_id, str) else ""

        if not title and not disc:
            raise RuntimeLaunchError(
                "Manifest declares no title identity: 'title_id' and/or "
                "'disc_id' is required. Generic launch resolution derives the "
                "runtime, image, base and entry only from validated title "
                "identity and has no default title."
            )

        registry = self._get_registry()
        by_disc = registry.lookup_by_disc_id(disc) if disc else None
        by_title = registry.lookup_by_id(title) if title else None

        if disc and title:
            if by_disc is not None and by_title is not None:
                if by_disc.id != by_title.id:
                    raise RuntimeLaunchError(
                        f"Session identity disagreement: disc_id '{disc}' "
                        f"resolves to title '{by_disc.id}' but title_id "
                        f"'{title}' resolves to title '{by_title.id}'."
                    )
                return by_disc
            if by_disc is not None:
                raise RuntimeLaunchError(
                    f"Session identity disagreement: disc_id '{disc}' resolves "
                    f"to title '{by_disc.id}' but title_id '{title}' is not a "
                    "validated title."
                )
            if by_title is not None:
                raise RuntimeLaunchError(
                    f"Session identity disagreement: disc_id '{disc}' is not a "
                    f"validated disc for title '{by_title.id}' declared by "
                    f"title_id '{title}'."
                )
            raise RuntimeLaunchError(
                f"No title profile for disc '{disc}' / title '{title}', so the "
                "runtime base and entry addresses are unknown."
            )
        if disc:
            if by_disc is None:
                raise RuntimeLaunchError(
                    f"No title profile for disc '{disc}', so the runtime base "
                    "and entry addresses are unknown."
                )
            return by_disc
        if by_title is None:
            raise RuntimeLaunchError(
                f"No title profile for title '{title}', so the runtime base "
                "and entry addresses are unknown."
            )
        return by_title

    def _exe_candidates(self, names: Dict[str, str]) -> List[Path]:
        """Executable candidates from the shared contract, identity-keyed."""
        candidates: List[Path] = []
        for source in NAME_SOURCES:
            name = names.get(source) or ""
            if not name:
                continue
            for pattern in EXE_CANDIDATES:
                candidates.append(self.repo_root / pattern.format(name=name))
        return candidates

    def _resolve_image(
        self, exe_path: Path, names: Dict[str, str]
    ) -> Optional[Path]:
        """Locate the runtime image, in the same order as nk_launch.c.

        The sibling image of the resolved executable comes first, then the
        shared build/<name> image layouts. There is no sibling-title or
        title-specific image fallback: a selected title without its own image
        fails closed.
        """
        candidates = [exe_path.with_name(exe_path.stem + IMAGE_SUFFIX)]
        for source in NAME_SOURCES:
            name = names.get(source) or ""
            if not name:
                continue
            for pattern in IMAGE_CANDIDATES:
                candidates.append(self.repo_root / pattern.format(name=name))
        for cand in candidates:
            if cand.is_file():
                return cand
        return None

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
        if not isinstance(manifest, dict):
            raise RuntimeLaunchError(
                f"Game manifest must be a JSON object: {manifest_file}"
            )

        # Identity first: title_id/disc_id from the manifest, bound to exactly
        # one validated registry profile. No default identity exists, so a
        # malformed or underspecified manifest fails closed here -- before any
        # path is probed -- instead of inheriting some other title.
        title_profile = self._resolve_identity(manifest)
        title_id = title_profile.id
        names = self._identity_names(title_profile)

        # The session manifest may only restate the validated build name;
        # a disagreement means the package and the selected title differ.
        declared_name = manifest.get("game_name")
        if declared_name not in (None, ""):
            if declared_name != names["game_name"]:
                raise RuntimeLaunchError(
                    f"Session identity disagreement: manifest game_name "
                    f"'{declared_name}' does not match the validated title "
                    f"game name '{names['game_name']}' for '{title_id}'."
                )

        iso_path = manifest.get("iso_path", "")
        if iso_path is None:
            iso_path = ""
        if iso_path and not isinstance(iso_path, str):
            raise RuntimeLaunchError("Manifest field 'iso_path' must be a string.")

        # Resolve binary path from the shared, identity-keyed contract. Both
        # the extensioned and extensionless names are probed: the runtime has
        # no .exe suffix on Linux or macOS.
        exe_candidates = self._exe_candidates(names)

        exe_path: Optional[Path] = None
        for cand in exe_candidates:
            if cand.is_file():
                exe_path = cand
                break

        if not exe_path:
            looked = [str(c) for c in exe_candidates] or ["(no identity-derived candidates)"]
            raise RuntimeLaunchError(
                f"Runtime binary not found for title '{title_id}'. "
                f"Looked in: {looked}. Build the runtime executable first."
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
        image_path = self._resolve_image(exe_path, names)
        if image_path is None:
            raise RuntimeLaunchError(
                f"Runtime image not found for '{title_id}'. Looked beside "
                f"{exe_path} and under build/. Build the runtime image first; "
                "returning a command that exits on usage would not be a launch plan."
            )

        # Addresses come from the same validated profile that supplied the
        # identity: a wrong entry point does not produce a readable error, it
        # produces a runtime that executes the wrong bytes.
        base = title_profile.executable_base
        entry = title_profile.executable_entry

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
