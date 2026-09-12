# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Title registry dynamically loaded from validated assets/titles/*.json manifests."""

from __future__ import annotations

import json
from pathlib import Path
import sys
from typing import Any, Dict, List, Optional, Sequence

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import publication_policy
import title_manifest
from . import synthetic_disc_ids
from .types import TitleProfile

# Map synthetic titles to test disc IDs for synthetic ISO test harnesses
# Re-exported from the single home so this module and
# tools/title_catalog_codegen.py cannot drift apart again.
SYNTHETIC_DISC_ID_MAP = synthetic_disc_ids.SYNTHETIC_DISC_IDS


def title_profile_from_manifest(manifest: Dict[str, Any]) -> TitleProfile:
    """Derive TitleProfile directly from validated manifest data."""
    t_id = manifest["id"]
    display_name = manifest["display_name"]
    kind = manifest["kind"]

    if kind == "retail":
        disc_info = manifest["disc"]
        disc_ids = [disc_info["id"]] + disc_info.get("compatible_revisions", [])
        regions = [disc_info.get("region", "UNKNOWN")]
    elif kind == "synthetic":
        # A public synthetic title has an assigned disc id; a PRIVATE overlay
        # loaded from outside this repository has none, and that is not an error
        # -- it simply has no disc id and is reachable by title id alone. What
        # must not happen is the old behaviour of substituting a shared
        # "TEST00000" for every unassigned title, which made any two of them
        # collide with each other and with the public catalog. The strict
        # title_manifest.synthetic_disc_id() is for catalog generation, where an
        # unassigned PUBLIC manifest really is a mistake.
        assigned = synthetic_disc_ids.SYNTHETIC_DISC_IDS.get(t_id)
        disc_ids = [assigned] if assigned else []
        regions = ["TEST"]
    else:
        disc_ids = []
        regions = ["HOMEBREW"]

    modules = manifest.get("modules", [])
    required_modules = [m["name"] for m in modules if m.get("required")]

    fs = manifest.get("filesystem", {})
    data_root = fs.get("data_root", "")

    # Identify archive format from title attributes or filesystem
    archive_format = "claphanz_xb" if "xbdata" in data_root else "raw"

    return TitleProfile(
        id=t_id,
        name=display_name,
        disc_ids=disc_ids,
        regions=regions,
        executable_base=manifest["executable"]["base"],
        executable_entry=manifest["executable"]["entry"],
        fallback_entry=f"0x{manifest['executable']['entry']:08x}",
        required_modules=required_modules,
        archive_format=archive_format,
        archive_relpath=data_root,
        save_namespace=disc_ids[0] if disc_ids else t_id,
        runtime_profile=manifest.get("hle_profile", "standard"),
        codegen_profile=manifest.get("codegen_profile", "none"),
    )


class TitleRegistry:
    """Thread-safe registry loaded directly from validated manifest files."""

    def __init__(self, titles_dir: Optional[Path] = None, include_defaults: bool = True) -> None:
        self._profiles: Dict[str, TitleProfile] = {}
        self._disc_index: Dict[str, str] = {}
        if include_defaults:
            t_dir = titles_dir or (ROOT / "assets" / "titles")
            if t_dir.is_dir():
                self.load_from_directory(t_dir)

    def load_from_directory(self, directory: Path, enforce_public_policy: bool = True) -> None:
        policy = None
        if enforce_public_policy:
            policy_path = ROOT / "assets" / "public_source_profile.json"
            if policy_path.is_file():
                policy = publication_policy.load_policy(policy_path)

        for json_file in sorted(directory.glob("*.json")):
            if policy is not None:
                try:
                    rel = json_file.resolve().relative_to(ROOT).as_posix()
                except ValueError:
                    # Same hole as tools/title_catalog_codegen.py had: naming
                    # the file assets/titles/<name> lent an untracked manifest
                    # the inclusion decision recorded for the tracked file of
                    # that name. A path outside ROOT has no policy identity, so
                    # it is not public-eligible. A caller that means to load an
                    # external directory passes enforce_public_policy=False.
                    continue
                res = policy.resolve(rel)
                if res.is_excluded or res.disposition != publication_policy.INCLUDED:
                    continue

            try:
                raw = title_manifest.load_manifest(json_file)
                validated = title_manifest.validate_manifest(raw)
                profile = title_profile_from_manifest(validated)
                self.register(profile)
            except Exception as exc:
                # Malformed manifests are rejected at load
                raise RuntimeError(f"Failed loading manifest {json_file}: {exc}") from exc

    def load_private_manifest(self, manifest_file: Path, allow_override: bool = False) -> TitleProfile:
        """Explicitly load an external/local private title manifest (e.g. for private testing)."""
        raw = title_manifest.load_manifest(manifest_file)
        validated = title_manifest.validate_manifest(raw)
        profile = title_profile_from_manifest(validated)

        # Collision check against existing public profiles
        collision = (profile.id in self._profiles)
        if not collision:
            for disc_id in profile.disc_ids:
                norm = self._norm_disc(disc_id)
                if norm in self._disc_index:
                    collision = True
                    break

        if collision:
            if not allow_override:
                raise RuntimeError(
                    f"Overlay identity '{profile.id}' conflicts with public canonical catalog; "
                    f"override forbidden in standard mode"
                )
            else:
                print(
                    f"[WARNING] Overriding canonical public title definition for '{profile.id}' "
                    f"with external private manifest!",
                    file=sys.stderr
                )

        self.register(profile)
        return profile

    def register(self, profile: TitleProfile) -> None:
        self._profiles[profile.id] = profile
        for disc_id in profile.disc_ids:
            norm = self._norm_disc(disc_id)
            self._disc_index[norm] = profile.id

    def lookup_by_disc_id(self, disc_id: str) -> Optional[TitleProfile]:
        norm = self._norm_disc(disc_id)
        profile_id = self._disc_index.get(norm)
        if profile_id:
            return self._profiles.get(profile_id)
        return None

    def find_by_disc_id(self, disc_id: str) -> Optional[TitleProfile]:
        """Alias for lookup_by_disc_id matching C API naming."""
        return self.lookup_by_disc_id(disc_id)

    def get(self, profile_id: str) -> Optional[TitleProfile]:
        return self._profiles.get(profile_id)

    def lookup_by_id(self, profile_id: str) -> Optional[TitleProfile]:
        return self._profiles.get(profile_id)

    def find_by_id(self, profile_id: str) -> Optional[TitleProfile]:
        """Alias for lookup_by_id matching C API naming."""
        return self.lookup_by_id(profile_id)

    def list_all(self) -> List[TitleProfile]:
        return list(self._profiles.values())

    def all_profiles(self) -> List[TitleProfile]:
        return list(self._profiles.values())

    def _norm_disc(self, disc_id: str) -> str:
        return "".join(c for c in disc_id if c not in "-_ ").upper()


_DEFAULT_REGISTRY: Optional[TitleRegistry] = None


def get_default_registry() -> TitleRegistry:
    global _DEFAULT_REGISTRY
    if _DEFAULT_REGISTRY is None:
        _DEFAULT_REGISTRY = TitleRegistry(include_defaults=True)
    return _DEFAULT_REGISTRY
