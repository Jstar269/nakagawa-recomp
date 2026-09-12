# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Nakagawa Core: Title-agnostic preparation, inspection, and execution library."""

from .types import (
    CancellationToken,
    EventSeverity,
    IsoMetadata,
    PreparationResult,
    PrepStage,
    ProgressEvent,
    TitleProfile,
)
from .title_registry import TitleRegistry, get_default_registry
from .iso_inspect import inspect_iso, IsoInspectionError
from .prep_engine import PreparationEngine
from .launcher import RuntimeLauncher, RuntimeLaunchError
from .library import GameLibrary, LibraryGameRecord

__all__ = [
    "CancellationToken",
    "EventSeverity",
    "GameLibrary",
    "IsoMetadata",
    "IsoInspectionError",
    "LibraryGameRecord",
    "PreparationEngine",
    "PreparationResult",
    "PrepStage",
    "ProgressEvent",
    "RuntimeLauncher",
    "RuntimeLaunchError",
    "TitleProfile",
    "TitleRegistry",
    "get_default_registry",
    "inspect_iso",
]
