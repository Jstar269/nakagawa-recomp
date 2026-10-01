# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Game library manager and persistent storage for Nakagawa Recomp."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
import json
from pathlib import Path
import tempfile
import time
from typing import Any, Dict, List, Optional

from .package_cache import (
    BoundedJsonError,
    MAX_CACHE_JSON_BYTES,
    MAX_CACHE_JSON_DEPTH,
    MAX_CACHE_JSON_ITEMS,
    MAX_CACHE_JSON_MEMBERS,
    MAX_CACHE_JSON_NODES,
    bounded_echo,
    bounded_echo_fields,
    read_bounded_json,
)


LIBRARY_SCHEMA_VERSION = 1

# Externally supplied library.json is one bounded JSON artifact (#319): the
# byte ceiling is enforced with a bounded read (never a whole-file read), the
# structural ceilings run before json.loads, and record-level limits bound the
# decoded list. Byte, structure, and record limits apply independently;
# nothing is ever truncated silently.
MAX_LIBRARY_JSON_BYTES = MAX_CACHE_JSON_BYTES
MAX_LIBRARY_JSON_DEPTH = MAX_CACHE_JSON_DEPTH
MAX_LIBRARY_GAMES = 4096

_GAME_RECORD_FIELDS = frozenset({
    "disc_id", "title_name", "iso_path", "prepared_root", "is_prepared",
    "assets_staged", "extracted_asset_count", "extracted_audio_count",
    "extracted_visual_count", "extracted_layout_count", "disc_version",
    "last_played", "play_count", "settings_override",
})
_LIBRARY_ROOT_FIELDS = frozenset({"schema_version", "updated_at", "games"})

# A saved record emits every dataclass field, even defaults. The package-cache
# structural budgets would reject an ordinary saved library around 1,170 games.
# Give this route budgets derived from its own record schema; the independent
# 1 MiB byte limit still applies, including to large settings overrides.
MAX_LIBRARY_JSON_MEMBERS = max(
    MAX_CACHE_JSON_MEMBERS,
    MAX_LIBRARY_GAMES * len(_GAME_RECORD_FIELDS) + len(_LIBRARY_ROOT_FIELDS),
)
MAX_LIBRARY_JSON_ITEMS = max(
    MAX_CACHE_JSON_ITEMS, MAX_LIBRARY_JSON_MEMBERS + MAX_LIBRARY_GAMES,
)
MAX_LIBRARY_JSON_NODES = max(
    MAX_CACHE_JSON_NODES,
    MAX_LIBRARY_GAMES * (2 * len(_GAME_RECORD_FIELDS) + 1)
    + 2 * len(_LIBRARY_ROOT_FIELDS) + 2,
)


@dataclass
class LibraryGameRecord:
    disc_id: str
    title_name: str
    iso_path: str
    prepared_root: str = ""
    is_prepared: bool = False
    assets_staged: bool = False
    extracted_asset_count: int = 0
    extracted_audio_count: int = 0
    extracted_visual_count: int = 0
    extracted_layout_count: int = 0
    disc_version: str = "1.00"
    last_played: Optional[float] = None
    play_count: int = 0
    settings_override: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "LibraryGameRecord":
        """Validate one externally supplied game record before field access.

        Every malformed shape raises ``ValueError`` with a named diagnosis;
        substitution of a scalar, list, or null for a required record object
        never escapes as ``AttributeError``/``TypeError``.
        """
        if not isinstance(data, dict):
            raise ValueError("game record must be a JSON object")
        unknown = set(data) - _GAME_RECORD_FIELDS
        if unknown:
            raise ValueError(
                "game record has unsupported fields: " + bounded_echo_fields(unknown)
            )

        disc_id = data.get("disc_id")
        if not isinstance(disc_id, str) or not disc_id.strip():
            raise ValueError("game record disc_id must be a non-empty string")
        title_name = data.get("title_name", "Unknown Title")
        if not isinstance(title_name, str) or len(title_name) > 256:
            raise ValueError("game record title_name must be a string of at most 256 characters")
        iso_path = data.get("iso_path", "")
        if not isinstance(iso_path, str):
            raise ValueError("game record iso_path must be a string")
        prepared_root = data.get("prepared_root", "")
        if not isinstance(prepared_root, str):
            raise ValueError("game record prepared_root must be a string")
        is_prepared = data.get("is_prepared", False)
        if not isinstance(is_prepared, bool):
            raise ValueError("game record is_prepared must be a boolean")
        assets_staged = data.get("assets_staged", False)
        if not isinstance(assets_staged, bool):
            raise ValueError("game record assets_staged must be a boolean")
        counts: Dict[str, int] = {}
        for field_name in (
            "extracted_asset_count", "extracted_audio_count",
            "extracted_visual_count", "extracted_layout_count", "play_count",
        ):
            value = data.get(field_name, 0)
            # type() rejects bool (an int subclass) and float on purpose.
            if type(value) is not int or value < 0:
                raise ValueError(f"game record {field_name} must be a non-negative integer")
            counts[field_name] = value
        disc_version = data.get("disc_version", "1.00")
        if not isinstance(disc_version, str) or len(disc_version) > 32:
            raise ValueError("game record disc_version must be a string of at most 32 characters")
        last_played = data.get("last_played")
        if last_played is not None and type(last_played) not in (int, float):
            raise ValueError("game record last_played must be a timestamp or null")
        settings_override = data.get("settings_override", {})
        if not isinstance(settings_override, dict) or any(
            not isinstance(key, str) for key in settings_override
        ):
            raise ValueError("game record settings_override must be an object with string keys")

        return cls(
            disc_id=disc_id.strip(),
            title_name=title_name,
            iso_path=iso_path,
            prepared_root=prepared_root,
            is_prepared=is_prepared,
            assets_staged=assets_staged,
            extracted_asset_count=counts["extracted_asset_count"],
            extracted_audio_count=counts["extracted_audio_count"],
            extracted_visual_count=counts["extracted_visual_count"],
            extracted_layout_count=counts["extracted_layout_count"],
            disc_version=disc_version,
            last_played=last_played,
            play_count=counts["play_count"],
            settings_override=dict(settings_override),
        )

    def is_source_available(self) -> bool:
        """Check whether the backing ISO file still exists on disk."""
        if not self.iso_path:
            return False
        return Path(self.iso_path).is_file()


class GameLibrary:
    """Persistent storage for registered and installed PSP games."""

    def __init__(self, storage_file: Optional[Path | str] = None) -> None:
        self.storage_file = Path(storage_file).resolve() if storage_file else None
        self._games: Dict[str, LibraryGameRecord] = {}

    def add_or_update_game(self, record: LibraryGameRecord) -> None:
        key = record.disc_id.upper().strip()
        self._games[key] = record

    def get_game(self, disc_id: str) -> Optional[LibraryGameRecord]:
        return self._games.get(disc_id.upper().strip())

    def remove_game(self, disc_id: str) -> bool:
        key = disc_id.upper().strip()
        if key in self._games:
            del self._games[key]
            return True
        return False

    def list_games(self) -> List[LibraryGameRecord]:
        return list(self._games.values())

    def count(self) -> int:
        return len(self._games)

    def verify_sources(self) -> Dict[str, bool]:
        """Verify availability of source ISO files for all registered games."""
        return {disc_id: rec.is_source_available() for disc_id, rec in self._games.items()}

    def save(self, target_file: Optional[Path | str] = None) -> None:
        out_file = Path(target_file).resolve() if target_file else self.storage_file
        if not out_file:
            raise ValueError("No storage file path specified for GameLibrary.")
        if len(self._games) > MAX_LIBRARY_GAMES:
            raise ValueError(
                f"library holds {len(self._games)} games, over the {MAX_LIBRARY_GAMES}-record limit"
            )

        out_file.parent.mkdir(parents=True, exist_ok=True)
        tmp_file: Optional[Path] = None

        payload = {
            "schema_version": LIBRARY_SCHEMA_VERSION,
            "updated_at": time.time(),
            "games": [rec.to_dict() for rec in self._games.values()],
        }

        try:
            # Each writer owns its temporary file. Overlapping saves must not
            # overwrite or unlink another writer's in-flight output.
            with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", dir=out_file.parent,
                prefix=f".{out_file.name}.", suffix=".tmp", delete=False,
            ) as f:
                tmp_file = Path(f.name)
                json.dump(payload, f, indent=2, ensure_ascii=False)
            # Validate with the exact reader before replacing a working library.
            # Oversized settings or invalid in-memory records must never install
            # a file that this same loader cannot read back.
            GameLibrary.load(tmp_file)
            tmp_file.replace(out_file)
        finally:
            if tmp_file is not None:
                tmp_file.unlink(missing_ok=True)

    @classmethod
    def load(cls, file_path: Path | str) -> "GameLibrary":
        """Load one externally supplied library.json under explicit ceilings.

        The byte ceiling is enforced with a bounded read before any wholesale
        read, UTF-8 decoding and structural ceilings run through the shared
        bounded-JSON helper, and every malformed shape raises ``ValueError``
        with a named diagnosis rather than a parser-implementation exception.
        """
        path = Path(file_path).resolve()
        lib = cls(storage_file=path)
        if not path.is_file():
            return lib

        try:
            data = read_bounded_json(
                path,
                max_bytes=MAX_LIBRARY_JSON_BYTES,
                max_depth=MAX_LIBRARY_JSON_DEPTH,
                max_members=MAX_LIBRARY_JSON_MEMBERS,
                max_items=MAX_LIBRARY_JSON_ITEMS,
                max_nodes=MAX_LIBRARY_JSON_NODES,
            )
        except (OSError, BoundedJsonError) as exc:
            raise ValueError(
                f"{path}: library JSON is unreadable or exceeds a bounded-input limit: {exc}"
            ) from exc

        # Fail closed on a schema this loader does not understand, the way the
        # native loader in src/core/nk_library.c already does. Interpreting the
        # familiar-looking fields of a newer file and then rewriting it as
        # schema 1 on the next save silently discards whatever the newer schema
        # added -- a data-losing "success" rather than a refusal.
        if not isinstance(data, dict):
            raise ValueError(f"{path}: library root must be a JSON object")
        unknown = set(data) - _LIBRARY_ROOT_FIELDS
        if unknown:
            raise ValueError(
                f"{path}: library root has unsupported fields: " + bounded_echo_fields(unknown)
            )
        schema = data.get("schema_version")
        if type(schema) is not int or schema != LIBRARY_SCHEMA_VERSION:
            raise ValueError(
                f"{path}: unsupported library schema_version {bounded_echo(schema)}; "
                f"this build reads version {LIBRARY_SCHEMA_VERSION}"
            )
        updated_at = data.get("updated_at")
        if updated_at is not None and type(updated_at) not in (int, float):
            raise ValueError(f"{path}: library updated_at must be a timestamp or null")
        if "games" not in data:
            raise ValueError(f"{path}: library is missing the games field")
        games_list = data["games"]
        if not isinstance(games_list, list):
            raise ValueError(f"{path}: 'games' must be a list")
        if len(games_list) > MAX_LIBRARY_GAMES:
            raise ValueError(
                f"{path}: 'games' holds {len(games_list)} records, "
                f"over the {MAX_LIBRARY_GAMES}-record limit"
            )
        for index, g_dict in enumerate(games_list):
            try:
                rec = LibraryGameRecord.from_dict(g_dict)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"{path}: game record {index} is invalid: {exc}") from exc
            key = rec.disc_id.upper().strip()
            if key in lib._games:
                raise ValueError(
                    f"{path}: game record {index} repeats disc_id {bounded_echo(rec.disc_id)}"
                )
            lib.add_or_update_game(rec)

        return lib
