# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Game library manager and persistent storage for Nakagawa Recomp."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
import json
import os
from pathlib import Path
import time
from typing import Any, Dict, List, Optional


LIBRARY_SCHEMA_VERSION = 1


@dataclass
class LibraryGameRecord:
    disc_id: str
    title_name: str
    iso_path: str
    prepared_root: str = ""
    is_prepared: bool = False
    disc_version: str = "1.00"
    last_played: Optional[float] = None
    play_count: int = 0
    settings_override: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> LibraryGameRecord:
        return cls(
            disc_id=str(data.get("disc_id", "")),
            title_name=str(data.get("title_name", "Unknown Title")),
            iso_path=str(data.get("iso_path", "")),
            prepared_root=str(data.get("prepared_root", "")),
            is_prepared=bool(data.get("is_prepared", False)),
            disc_version=str(data.get("disc_version", "1.00")),
            last_played=data.get("last_played"),
            play_count=int(data.get("play_count", 0)),
            settings_override=dict(data.get("settings_override", {})),
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

        out_file.parent.mkdir(parents=True, exist_ok=True)
        tmp_file = out_file.with_suffix(".tmp")

        payload = {
            "schema_version": LIBRARY_SCHEMA_VERSION,
            "updated_at": time.time(),
            "games": [rec.to_dict() for rec in self._games.values()],
        }

        with open(tmp_file, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, ensure_ascii=False)

        # Atomic replacement
        tmp_file.replace(out_file)

    @classmethod
    def load(cls, file_path: Path | str) -> GameLibrary:
        path = Path(file_path).resolve()
        lib = cls(storage_file=path)
        if not path.is_file():
            return lib

        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)

        # Fail closed on a schema this loader does not understand, the way the
        # native loader in src/core/nk_library.c already does. Interpreting the
        # familiar-looking fields of a newer file and then rewriting it as
        # schema 1 on the next save silently discards whatever the newer schema
        # added -- a data-losing "success" rather than a refusal.
        if not isinstance(data, dict):
            raise ValueError(f"{path}: library root must be a JSON object")
        schema = data.get("schema_version")
        if schema != LIBRARY_SCHEMA_VERSION:
            raise ValueError(
                f"{path}: unsupported library schema_version {schema!r}; "
                f"this build reads version {LIBRARY_SCHEMA_VERSION}"
            )

        games_list = data.get("games", [])
        if not isinstance(games_list, list):
            raise ValueError(f"{path}: 'games' must be a list")
        for g_dict in games_list:
            rec = LibraryGameRecord.from_dict(g_dict)
            lib.add_or_update_game(rec)

        return lib
