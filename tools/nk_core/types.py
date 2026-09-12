# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Portable types and event contracts for the Nakagawa core preparation & runtime system."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Sequence


class PrepStage(str, Enum):
    IDLE = "IDLE"
    INSPECTING_ISO = "INSPECTING_ISO"
    EXTRACTING_CONTAINERS = "EXTRACTING_CONTAINERS"
    EXTRACTING_ARCHIVES = "EXTRACTING_ARCHIVES"
    PREPARING_MODULES = "PREPARING_MODULES"
    VERIFYING_OUTPUT = "VERIFYING_OUTPUT"
    READY = "READY"
    FAILED = "FAILED"
    CANCELLED = "CANCELLED"


class EventSeverity(str, Enum):
    INFO = "INFO"
    WARN = "WARN"
    ERROR = "ERROR"


@dataclass(frozen=True)
class ProgressEvent:
    """Structured progress event emitted during preparation or runtime launch."""
    stage: PrepStage
    operation: str
    completed: int = 0
    total: int = 0
    unit: str = "items"
    current_item: str = ""
    elapsed_ms: int = 0
    message: str = ""
    severity: EventSeverity = EventSeverity.INFO
    cancellable: bool = True

    @property
    def percentage(self) -> Optional[float]:
        if self.total <= 0:
            return None
        pct = (self.completed / self.total) * 100.0
        return min(100.0, max(0.0, pct))

    def to_dict(self) -> Dict[str, Any]:
        return {
            "stage": self.stage.value,
            "operation": self.operation,
            "completed": self.completed,
            "total": self.total,
            "unit": self.unit,
            "current_item": self.current_item,
            "elapsed_ms": self.elapsed_ms,
            "message": self.message,
            "severity": self.severity.value,
            "cancellable": self.cancellable,
            "percentage": self.percentage,
        }


class CancellationToken:
    """Cooperative cancellation token for preparation tasks."""
    def __init__(self) -> None:
        self._cancelled = False

    def cancel(self) -> None:
        self._cancelled = True

    @property
    def is_cancelled(self) -> bool:
        return self._cancelled

    def check(self) -> None:
        if self._cancelled:
            raise InterruptedError("Operation cancelled by user")


@dataclass(frozen=True)
class TitleProfile:
    """Data-driven title profile describing a supported PSP game."""
    id: str
    name: str
    disc_ids: Sequence[str]
    regions: Sequence[str]
    executable_base: int = 0
    executable_entry: int = 0
    fallback_entry: Optional[str] = None
    required_modules: Sequence[str] = field(default_factory=list)
    archive_format: str = "raw"  # "raw", "claphanz_xb", etc.
    archive_relpath: str = ""
    save_namespace: str = ""
    runtime_profile: str = "standard"
    codegen_profile: str = "default"
    min_iso_bytes: int = 10 * 1024 * 1024  # 10 MB minimum

    def matches_disc_id(self, disc_id: str) -> bool:
        norm = disc_id.strip().upper().replace("-", "").replace("_", "")
        for candidate in self.disc_ids:
            if candidate.strip().upper().replace("-", "").replace("_", "") == norm:
                return True
        return False


@dataclass(frozen=True)
class IsoMetadata:
    """Metadata parsed from a PSP game image."""
    disc_id: str
    title: str
    version: str = "1.00"
    region: str = "UNKNOWN"
    volume_id: str = ""
    size_bytes: int = 0
    matched_profile: Optional[TitleProfile] = None

    @property
    def is_supported(self) -> bool:
        return self.matched_profile is not None


@dataclass
class PreparationResult:
    """Outcome of an ISO preparation run."""
    success: bool
    disc_id: str
    prepared_root: Optional[Path] = None
    manifest_path: Optional[Path] = None
    error_code: Optional[str] = None
    error_message: Optional[str] = None
    elapsed_ms: int = 0
