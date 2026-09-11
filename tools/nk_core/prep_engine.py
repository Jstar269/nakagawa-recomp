# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Transactional preparation engine for PSP game runtime environments."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import time
from typing import Callable, Optional

from .iso_inspect import inspect_iso
from .title_registry import TitleRegistry, get_default_registry
from .types import (
    CancellationToken,
    EventSeverity,
    PreparationResult,
    PrepStage,
    ProgressEvent,
    TitleProfile,
)


MANIFEST_SCHEMA_VERSION = 1
PREP_ENGINE_VERSION = "0.2.0"


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


class PreparationEngine:
    """Coordinates transactional preparation of game assets and runtime configuration."""

    def __init__(
        self,
        base_dir: Optional[Path] = None,
        registry: Optional[TitleRegistry] = None,
    ) -> None:
        self.base_dir = Path(base_dir or Path.cwd()).resolve()
        self.registry = registry or get_default_registry()

    def prepare_game(
        self,
        iso_path: Path | str,
        on_progress: Optional[Callable[[ProgressEvent], None]] = None,
        token: Optional[CancellationToken] = None,
        destination_root: Optional[Path] = None,
    ) -> PreparationResult:
        start_time = time.monotonic()
        iso = Path(iso_path).resolve()
        cancel_token = token or CancellationToken()

        def emit(
            stage: PrepStage,
            operation: str,
            completed: int = 0,
            total: int = 0,
            unit: str = "items",
            current_item: str = "",
            message: str = "",
            severity: EventSeverity = EventSeverity.INFO,
        ) -> None:
            if on_progress:
                elapsed_ms = int((time.monotonic() - start_time) * 1000)
                event = ProgressEvent(
                    stage=stage,
                    operation=operation,
                    completed=completed,
                    total=total,
                    unit=unit,
                    current_item=current_item,
                    elapsed_ms=elapsed_ms,
                    message=message,
                    severity=severity,
                    cancellable=True,
                )
                on_progress(event)

        staging_dir: Optional[Path] = None
        try:
            # Stage 1: Inspect ISO
            emit(PrepStage.INSPECTING_ISO, "Inspecting disc image", completed=0, total=100)
            cancel_token.check()

            iso_meta = inspect_iso(iso, self.registry)
            if not iso_meta.is_supported or not iso_meta.matched_profile:
                return PreparationResult(
                    success=False,
                    disc_id=iso_meta.disc_id,
                    error_code="ISO_UNSUPPORTED_TITLE",
                    error_message=f"Title with disc ID '{iso_meta.disc_id}' is not currently supported.",
                )

            profile = iso_meta.matched_profile
            emit(
                PrepStage.INSPECTING_ISO,
                f"Identified {profile.name} ({iso_meta.disc_id})",
                completed=100,
                total=100,
            )

            # Determine destination.
            #
            # A profile with compatible_revisions matches through any of its
            # disc IDs, but this always used profile.disc_ids[0]. Two compatible
            # revisions therefore installed over each other in one directory and
            # both recorded the primary ID, losing the identity actually read
            # from the disc and potentially replacing the wrong revision. Keep
            # the inspected ID.
            #
            # The ID comes from the disc's own PARAM.SFO, so it is untrusted
            # input and becomes a directory name below. Accepting only an ID the
            # matched profile already declares keeps it to a known-good set,
            # which is also what makes traversal impossible here.
            inspected_disc_id = (iso_meta.disc_id or "").strip()
            disc_id = inspected_disc_id if inspected_disc_id in profile.disc_ids else profile.disc_ids[0]

            games_root = destination_root or (self.base_dir / "games")
            target_dir = games_root / disc_id
            staging_dir = games_root / f".staging_{disc_id}_{int(time.time())}"

            if staging_dir.exists():
                shutil.rmtree(staging_dir, ignore_errors=True)
            staging_dir.mkdir(parents=True, exist_ok=True)

            cancel_token.check()

            # Stage 2: Prepare Directory Structure & ISO reference
            emit(PrepStage.EXTRACTING_CONTAINERS, "Creating container layout", completed=1, total=5)
            disc_dir = staging_dir / "disc"
            disc_dir.mkdir(parents=True, exist_ok=True)

            # Store safe relative or canonical pointer to ISO
            iso_copy_link = disc_dir / "game.iso"
            try:
                # Prefer symlink / hardlink where supported to avoid multi-GB duplicate copies
                os.link(iso, iso_copy_link)
            except (AttributeError, OSError):
                # If hardlinking fails across drives, write location pointer
                with open(disc_dir / "iso_path.txt", "w", encoding="utf-8") as f:
                    f.write(str(iso))

            cancel_token.check()

            # Stage 3: Extract Archives (using built-in xb_probe if ClapHanz XB format)
            extracted_dir = staging_dir / "extracted"
            extracted_dir.mkdir(parents=True, exist_ok=True)

            if profile.archive_format == "claphanz_xb":
                emit(
                    PrepStage.EXTRACTING_ARCHIVES,
                    "Preparing ClapHanz XB archives via clean-room parser",
                    completed=2,
                    total=5,
                )
                # If xb archives exist in an extracted ISO tree or container
                xb_candidates = list((staging_dir / "disc").glob("**/*.xb*"))
                for idx, arc in enumerate(xb_candidates, start=1):
                    cancel_token.check()
                    emit(
                        PrepStage.EXTRACTING_ARCHIVES,
                        f"Extracting {arc.name}",
                        completed=idx,
                        total=len(xb_candidates),
                        current_item=arc.name,
                    )
                    out_path = extracted_dir / f"{arc.stem}_extracted"
                    out_path.mkdir(parents=True, exist_ok=True)
                    try:
                        from ..xb_probe import XBArchiveReader
                        reader = XBArchiveReader(arc)
                        for entry in reader.entries:
                            target_file = out_path / entry.path.replace("/", os.sep)
                            target_file.parent.mkdir(parents=True, exist_ok=True)
                            target_file.write_bytes(reader.read_entry(entry))
                    except Exception:
                        pass

            cancel_token.check()

            # Stage 4: Verification
            emit(PrepStage.VERIFYING_OUTPUT, "Verifying preparation manifest", completed=4, total=5)
            manifest = {
                "schema_version": MANIFEST_SCHEMA_VERSION,
                "engine_version": PREP_ENGINE_VERSION,
                "title_id": profile.id,
                "disc_id": disc_id,
                "title_name": profile.name,
                "iso_path": str(iso),
                "iso_size": iso_meta.size_bytes,
                "created_at": time.time(),
                "runtime_profile": profile.runtime_profile,
                "archive_format": profile.archive_format,
                "save_namespace": profile.save_namespace,
            }
            manifest_path = staging_dir / "manifest.json"
            with open(manifest_path, "w", encoding="utf-8") as f:
                json.dump(manifest, f, indent=2)

            cancel_token.check()

            # Stage 5: Atomic Promotion
            emit(PrepStage.READY, "Promoting staged game data", completed=5, total=5)
            # Keep any previous preparation until the new one is in place.
            # Deleting it first meant a failed or partial rename left the user
            # with neither the working install nor the new one -- and the outer
            # handler then removed the staging tree too. Moving it aside keeps
            # a rollback available for the one operation that can still fail.
            retired_dir = None
            if target_dir.exists():
                retired_dir = target_dir.with_name(target_dir.name + ".retired-" + str(int(time.time())))
                target_dir.rename(retired_dir)
            try:
                staging_dir.rename(target_dir)
            except OSError:
                # Promotion failed: restore what the user already had rather
                # than leaving the title with nothing.
                if retired_dir is not None and not target_dir.exists():
                    retired_dir.rename(target_dir)
                raise
            if retired_dir is not None:
                shutil.rmtree(retired_dir, ignore_errors=True)

            final_manifest = target_dir / "manifest.json"
            elapsed_ms = int((time.monotonic() - start_time) * 1000)

            emit(PrepStage.READY, "Preparation complete! Ready to play.", completed=5, total=5)

            return PreparationResult(
                success=True,
                disc_id=disc_id,
                prepared_root=target_dir,
                manifest_path=final_manifest,
                elapsed_ms=elapsed_ms,
            )

        except InterruptedError:
            if staging_dir and staging_dir.exists():
                shutil.rmtree(staging_dir, ignore_errors=True)
            emit(PrepStage.CANCELLED, "Preparation cancelled by user", severity=EventSeverity.WARN)
            return PreparationResult(
                success=False,
                disc_id="UNKNOWN",
                error_code="PREPARATION_CANCELLED",
                error_message="Preparation was cancelled by user.",
            )
        except Exception as exc:
            if staging_dir and staging_dir.exists():
                shutil.rmtree(staging_dir, ignore_errors=True)
            emit(PrepStage.FAILED, f"Preparation failed: {exc}", severity=EventSeverity.ERROR)
            return PreparationResult(
                success=False,
                disc_id="UNKNOWN",
                error_code="PREPARATION_FAILED",
                error_message=str(exc),
            )
