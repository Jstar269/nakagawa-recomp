# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Transactional preparation engine for PSP game runtime environments."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
from typing import Callable, Optional

from .iso_inspect import IsoInspectionError, inspect_iso, select_boot_executable_source
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
PREP_ENGINE_VERSION = "0.3.0"

# Archive formats this Python route promotes itself. Every other format
# (notably claphanz_xb) needs the disc's archives extracted, which only the
# native player's staging transaction does; prepare_game hands those discs to
# a NativeTitleStager, or refuses before any filesystem mutation when none is
# configured.
SUPPORTED_ARCHIVE_FORMATS = frozenset({"raw"})

PLAYER_EXECUTABLE = "nakagawa_player.exe" if os.name == "nt" else "nakagawa_player"

# (percent, "files done/total") from the native staging transaction.
NativeProgress = Callable[[int, str], None]
NativeStager = Callable[[Path, str, NativeProgress], PreparationResult]

_STAGE_BOUNDARY = re.compile(r"\[(STAGE_[A-Z_]+)\]")


def player_candidates(source_root: Path) -> list[Path]:
    """Where the player sits relative to a source tree.

    A checkout builds it into ``build/``; the release layout keeps ``bin/``
    beside the ``source/`` folder that carries ``tools/``.
    """
    return [
        source_root / "build" / PLAYER_EXECUTABLE,
        source_root.parent / "bin" / PLAYER_EXECUTABLE,
    ]


def _player_fields(text: str) -> dict[str, str]:
    return dict(part.split("=", 1) for part in text.split()[1:] if "=" in part)


class NativeTitleStager:
    """Set up one disc's files through the player's staging transaction.

    Runs ``nakagawa_player --user-data-root=<root> --iso=<iso> --stage-only``:
    the same transaction the player's setup wizard runs (extract into
    ``games/.staging_<disc>``, validate, promote to ``games/<disc>``, record it
    in the library), so the command line never keeps its own copy of the
    extraction. The player's calm boundary message is passed through as the
    error message, with its ``[STAGE_...]`` code as the error code.
    """

    def __init__(
        self,
        user_data_root: Path,
        player: Optional[Path] = None,
        *,
        source_root: Optional[Path] = None,
    ) -> None:
        self.user_data_root = Path(user_data_root)
        self.player = Path(player) if player is not None else None
        self.source_root = Path(source_root) if source_root is not None else (
            Path(__file__).resolve().parents[2]
        )
        self.reused = False

    def candidates(self) -> list[Path]:
        if self.player is not None:
            return [self.player]
        return player_candidates(self.source_root)

    def __call__(self, iso: Path, disc_id: str, progress: NativeProgress) -> PreparationResult:
        candidates = self.candidates()
        player = next((path for path in candidates if path.is_file()), None)
        if player is None:
            looked = ", ".join(str(path) for path in candidates)
            return PreparationResult(
                success=False,
                disc_id=disc_id,
                error_code="PLAYER_NOT_FOUND",
                error_message=(
                    "This game's data is packed in archives on the disc, and the Nakagawa "
                    "player sets those files up, but the player was not found (looked for "
                    f"{looked}). Build it with `mingw32-make player` (`make player` on "
                    "Linux), or pass --player with its location, then run prepare again."
                ),
            )
        command = [
            str(player),
            f"--user-data-root={self.user_data_root}",
            f"--iso={iso}",
            "--stage-only",
        ]
        try:
            process = subprocess.Popen(
                command,
                cwd=str(player.parent),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
        except OSError as exc:
            return PreparationResult(
                success=False,
                disc_id=disc_id,
                error_code="PLAYER_START_FAILED",
                error_message=(
                    f"The Nakagawa player at {player} could not be started ({exc}). "
                    "Rebuild it, then run prepare again."
                ),
            )

        status: Optional[str] = None
        prepared_root: Optional[Path] = None
        notes: list[str] = []
        try:
            assert process.stdout is not None
            for raw_line in process.stdout:
                line = raw_line.rstrip("\r\n")
                text = line[len("[PLAYER] "):] if line.startswith("[PLAYER] ") else line
                if text.startswith("STAGING_PROGRESS "):
                    fields = _player_fields(text)
                    try:
                        progress(int(fields.get("percent", "0")), fields.get("files", ""))
                    except ValueError:
                        pass
                elif text.startswith("STAGED_ROOT "):
                    prepared_root = Path(text[len("STAGED_ROOT "):])
                elif text.startswith("STAGING_RESULT "):
                    fields = _player_fields(text)
                    status = fields.get("status")
                    self.reused = fields.get("reused") == "1"
                elif text:
                    notes.append(text)
            returncode = process.wait()
        except BaseException:
            process.kill()
            process.wait()
            raise

        if returncode == 0 and status == "PASS" and prepared_root is not None:
            return PreparationResult(success=True, disc_id=disc_id, prepared_root=prepared_root)
        boundary = next((note for note in notes if _STAGE_BOUNDARY.search(note)), None)
        if boundary is not None:
            match = _STAGE_BOUNDARY.search(boundary)
            assert match is not None
            code = match.group(1)
            message = boundary[match.start():]
        elif status == "INCOMPLETE":
            code = "STAGE_DATA_ROOT_MISSING"
            message = next(
                (note for note in notes if "data folder" in note),
                "The game's files were set up, but its data folder is still missing.",
            )
        else:
            code = "NATIVE_STAGING_FAILED"
            detail = notes[-1] if notes else f"exit status {returncode}"
            message = (
                "The Nakagawa player could not set up this game's files. "
                f"Details: {detail}"
            )
        return PreparationResult(
            success=False, disc_id=disc_id, error_code=code, error_message=message
        )


class PreparationOutputError(RuntimeError):
    """Staged preparation outputs failed validation before atomic promotion."""


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
        native_stager: Optional[NativeStager] = None,
    ) -> None:
        self.base_dir = Path(base_dir or Path.cwd()).resolve()
        self.registry = registry or get_default_registry()
        self.native_stager = native_stager

    @staticmethod
    def _validate_staged_outputs(
        staging_dir: Path,
        source_iso: Path,
        profile: TitleProfile,
        disc_id: str,
    ) -> None:
        """Refuse promotion unless every required staged output is present.

        A parsed manifest alone is not evidence of preparation (#374): the
        disc tree, an ISO reference that resolves to the inspected source,
        and a manifest attributable to this disc/profile/source must all be
        verified on disk before atomic promotion.
        """
        disc_dir = staging_dir / "disc"
        if not disc_dir.is_dir():
            raise PreparationOutputError("staged disc directory is missing")

        game_iso = disc_dir / "game.iso"
        iso_pointer = disc_dir / "iso_path.txt"
        references_source = False
        if game_iso.exists():
            try:
                references_source = game_iso.samefile(source_iso)
            except OSError:
                references_source = False
        if not references_source and iso_pointer.is_file():
            try:
                pointed = Path(iso_pointer.read_text(encoding="utf-8").strip())
                references_source = pointed.resolve() == source_iso
            except (OSError, ValueError):
                references_source = False
        if not references_source:
            raise PreparationOutputError(
                "staged ISO reference does not resolve to the inspected source image"
            )

        manifest_path = staging_dir / "manifest.json"
        try:
            with open(manifest_path, "r", encoding="utf-8") as f:
                staged = json.load(f)
        except (OSError, json.JSONDecodeError) as exc:
            raise PreparationOutputError(f"staged manifest is unreadable: {exc}") from exc

        if staged.get("schema_version") != MANIFEST_SCHEMA_VERSION:
            raise PreparationOutputError("staged manifest schema_version mismatch")
        if staged.get("disc_id") != disc_id:
            raise PreparationOutputError("staged manifest disc_id is not attributable to this disc")
        if staged.get("title_id") != profile.id:
            raise PreparationOutputError("staged manifest title_id does not match the profile")
        if staged.get("archive_format") not in SUPPORTED_ARCHIVE_FORMATS:
            raise PreparationOutputError("staged manifest archive_format is not supported")
        if staged.get("iso_path") != str(source_iso):
            raise PreparationOutputError("staged manifest iso_path does not match the source image")
        try:
            staged_size = staged.get("iso_size")
            actual_size = source_iso.stat().st_size
        except OSError as exc:
            raise PreparationOutputError(f"cannot stat source image: {exc}") from exc
        if staged_size != actual_size:
            raise PreparationOutputError("staged manifest iso_size does not match the source image")

    def _prepare_with_native_staging(
        self,
        iso: Path,
        disc_id: str,
        profile: TitleProfile,
        destination_root: Optional[Path],
        emit: Callable[..., None],
        start_time: float,
    ) -> PreparationResult:
        def refuse(code: str, message: str) -> PreparationResult:
            emit(PrepStage.FAILED, message, severity=EventSeverity.ERROR)
            return PreparationResult(
                success=False, disc_id=disc_id, error_code=code, error_message=message
            )

        if self.native_stager is None:
            return refuse(
                "PREPARATION_ROUTE_UNSUPPORTED",
                f"This game's data is packed in '{profile.archive_format}' archives on the "
                "disc. Only the Nakagawa player's staging can set those files up, and none "
                "was provided to this preparation. Run `python tools/nk_cli.py prepare` "
                "with the player built, or add the disc in the player.",
            )
        if destination_root is not None:
            return refuse(
                "PREPARATION_DESTINATION_UNSUPPORTED",
                "This game's files are set up inside Nakagawa's data folder, where the "
                "player and Play find them, so --dest cannot place them elsewhere. Leave "
                "out --dest, or choose another data folder with --user-data-root.",
            )

        operation = "Setting up the game's files"
        emit(PrepStage.EXTRACTING_ARCHIVES, operation, completed=0, total=100,
             unit="percent", message="A large disc can take a few minutes.")

        def on_progress(percent: int, files: str) -> None:
            emit(PrepStage.EXTRACTING_ARCHIVES, operation,
                 completed=max(0, min(percent, 100)), total=100, unit="percent",
                 current_item=files, message=f"{files} files" if files else "")

        result = self.native_stager(iso, disc_id, on_progress)
        result.elapsed_ms = int((time.monotonic() - start_time) * 1000)
        if result.success:
            emit(PrepStage.READY, "The game's files are in place.", completed=100,
                 total=100, unit="percent")
        else:
            emit(PrepStage.FAILED,
                 result.error_message or "Setting up the game's files failed.",
                 severity=EventSeverity.ERROR)
        return result

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
        disc_id = "UNKNOWN"
        try:
            # Stage 1: Inspect ISO
            emit(PrepStage.INSPECTING_ISO, "Inspecting disc image", completed=0, total=100)
            cancel_token.check()

            try:
                iso_meta = inspect_iso(iso, self.registry, user_data_root=self.base_dir)
            except IsoInspectionError as exc:
                if exc.boundary_code is None:
                    raise
                return PreparationResult(
                    success=False,
                    disc_id=disc_id,
                    error_code=exc.boundary_code,
                    error_message=str(exc),
                )
            if iso_meta.qualification_error:
                return PreparationResult(
                    success=False,
                    disc_id=iso_meta.disc_id,
                    error_code="ISO_UNQUALIFIED_REVISION",
                    error_message=iso_meta.qualification_error,
                )
            if not iso_meta.is_supported or not iso_meta.matched_profile:
                return PreparationResult(
                    success=False,
                    disc_id=iso_meta.disc_id,
                    error_code="ISO_UNSUPPORTED_TITLE",
                    error_message=f"Title with disc ID '{iso_meta.disc_id}' is not currently supported.",
                )

            profile = iso_meta.matched_profile
            boot_executable_source = select_boot_executable_source(iso)
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

            # Route gate: a disc whose data ships in archives is set up by the
            # native staging transaction, never by a Python copy of it. Without
            # a stager the route refuses before any filesystem mutation.
            if profile.archive_format not in SUPPORTED_ARCHIVE_FORMATS:
                return self._prepare_with_native_staging(
                    iso, disc_id, profile, destination_root, emit, start_time
                )

            games_root = destination_root or (self.base_dir / "games")
            target_dir = games_root / disc_id

            # A one-second timestamp is not a unique name. Two preparations of
            # the same disc started within the same second computed the same
            # staging path, and the second one deleted the first one's live
            # staging tree before both interleaved writes into it. mkdtemp
            # creates the directory atomically and fails rather than colliding,
            # so concurrent preparations cannot share one.
            games_root.mkdir(parents=True, exist_ok=True)
            staging_dir = Path(
                tempfile.mkdtemp(prefix=f".staging_{disc_id}_", dir=games_root)
            )

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

            # Stage 3: no Python-side XB archive staging. Only "raw" reaches
            # this point (route-gated above); the empty extracted/ tree is
            # retained as the runtime's expected data-root layout.
            (staging_dir / "extracted").mkdir(parents=True, exist_ok=True)

            cancel_token.check()

            # Stage 4: Verification
            emit(PrepStage.VERIFYING_OUTPUT, "Verifying preparation manifest", completed=4, total=5)
            manifest = {
                "schema_version": MANIFEST_SCHEMA_VERSION,
                "engine_version": PREP_ENGINE_VERSION,
                "title_id": profile.id,
                "game_name": profile.game_name or profile.id,
                "disc_id": disc_id,
                "title_name": profile.name,
                "iso_path": str(iso),
                "iso_size": iso_meta.size_bytes,
                "created_at": time.time(),
                "runtime_profile": profile.runtime_profile,
                "archive_format": profile.archive_format,
                "save_namespace": profile.save_namespace,
                "boot_executable": boot_executable_source or "",
            }
            manifest_path = staging_dir / "manifest.json"
            with open(manifest_path, "w", encoding="utf-8") as f:
                json.dump(manifest, f, indent=2)

            cancel_token.check()

            # A parsed manifest is not sufficient evidence of preparation
            # (#374). Verify the required staged outputs before promotion.
            self._validate_staged_outputs(staging_dir, iso, profile, disc_id)

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
                disc_id=disc_id,
                error_code="PREPARATION_CANCELLED",
                error_message="Preparation was cancelled by user.",
            )
        except PreparationOutputError as exc:
            if staging_dir and staging_dir.exists():
                shutil.rmtree(staging_dir, ignore_errors=True)
            emit(PrepStage.FAILED, f"Preparation failed: {exc}", severity=EventSeverity.ERROR)
            return PreparationResult(
                success=False,
                disc_id=disc_id,
                error_code="PREPARATION_OUTPUT_INVALID",
                error_message=str(exc),
            )
        except Exception as exc:
            if staging_dir and staging_dir.exists():
                shutil.rmtree(staging_dir, ignore_errors=True)
            emit(PrepStage.FAILED, f"Preparation failed: {exc}", severity=EventSeverity.ERROR)
            return PreparationResult(
                success=False,
                disc_id=disc_id,
                error_code="PREPARATION_FAILED",
                error_message=str(exc),
            )
