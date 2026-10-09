# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Fail-closed preparation regressions, and the archive-disc staging route.

Every fixture is generated in-process from synthetic bytes: no retail disc,
archive, module, texture, name, or hash appears in this file.

The suite pins the claims the Python preparation route previously got wrong:

* a ``claphanz_xb`` profile is never staged by Python: without the native
  player's staging transaction it is refused before any filesystem mutation,
  and with it the disc is handed to that transaction, never promoted as READY
  on the strength of a Python-side no-op;
* hostile XB archive bytes -- truncated FST, unsafe member path, decode
  failure -- and an XB-parser import failure cannot convert preparation into
  success, and a previous install survives every failure untouched;
* required-output completeness is checked before atomic promotion: a manifest
  that is not attributable to the selected disc, a staging write failure, or a
  promotion rename failure all fail the transaction with the previous install
  intact;
* the documented ``python tools/nk_cli.py prepare ...`` invocation fails
  closed instead of reporting success for blocked or unsupported work, and
  sets up an archive disc through the player's staging transaction (the same
  one the setup wizard runs) when the player is built.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

from nk_core import (  # noqa: E402
    PreparationEngine,
    PreparationResult,
    ProgressEvent,
    TitleProfile,
    TitleRegistry,
)
from nk_core.prep_engine import NativeTitleStager, PLAYER_EXECUTABLE  # noqa: E402
from test_extract_xb_security import _make_raw_archive  # noqa: E402
from test_nk_core import _create_mock_iso  # noqa: E402
from test_xb_probe import _make_archive  # noqa: E402
from xb_probe import XBCompression  # noqa: E402


TOOLS_DIR = Path(__file__).resolve().parent
BUILT_PLAYER = TOOLS_DIR.parent / "build" / PLAYER_EXECUTABLE
CLAPHANZ_ISO = "TEST90001"
RAW_ISO = "TEST00001"
UNSUPPORTED_ISO = "ULUS12345"


def _claphanz_registry() -> TitleRegistry:
    registry = TitleRegistry(include_defaults=True)
    registry.register(
        TitleProfile(
            id="claphanz-synthetic-v1",
            name="ClapHanz Synthetic",
            disc_ids=[CLAPHANZ_ISO],
            regions=["TEST"],
            archive_format="claphanz_xb",
            archive_relpath="UMD_DATA/xbdata",
            save_namespace=CLAPHANZ_ISO,
        )
    )
    return registry


def _iso_with_payload(path: Path, payload: bytes, disc_id: str) -> None:
    """Build a synthetic ISO that additionally carries ``payload`` bytes.

    The appended bytes stand in for archive content present on the disc. No ISO
    directory extraction exists on the Python route, so the payload must never
    become evidence of a completed archive stage.
    """
    _create_mock_iso(path, disc_id=disc_id)
    with open(path, "ab") as handle:
        handle.write(payload)


def _hostile_xb_payloads() -> dict[str, bytes]:
    valid = _make_archive([("data/ok.bin", b"synthetic member", XBCompression.NONE)])
    return {
        "truncated_fst": valid[:24],
        "unsafe_path": _make_archive(
            [("../escape.bin", b"synthetic escape attempt", XBCompression.NONE)]
        ),
        "decode_failure": _make_raw_archive(
            [("data/bad.bin", 64, XBCompression.LZS, b"\xff\xfe not an LZS stream")]
        ),
    }


class _ImportBlocker:
    """A meta-path finder that refuses one module name, for import-failure tests."""

    def __init__(self, name: str) -> None:
        self.name = name
        self.attempts: list[str] = []

    def find_spec(self, fullname, path=None, target=None):
        if fullname == self.name or fullname.startswith(self.name + "."):
            self.attempts.append(fullname)
            raise ImportError(f"{fullname} must not be imported during preparation")
        return None


class PreparationFailClosedCase(unittest.TestCase):
    """Shared synthetic ISO, previous-install, and staging assertions."""

    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="test_prep_fail_closed_"))
        self.dest_root = self.temp_dir / "installed_games"

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def _engine(self, claphanz: bool) -> PreparationEngine:
        registry = _claphanz_registry() if claphanz else TitleRegistry(include_defaults=True)
        return PreparationEngine(base_dir=self.temp_dir, registry=registry)

    def _seed_previous_install(self, disc_id: str) -> tuple[bytes, bytes]:
        target = self.dest_root / disc_id
        target.mkdir(parents=True, exist_ok=True)
        marker = b"synthetic previous install payload"
        (target / "marker.bin").write_bytes(marker)
        manifest_bytes = json.dumps(
            {
                "schema_version": 1,
                "engine_version": "0.2.0",
                "title_id": "previous-install",
                "disc_id": disc_id,
                "iso_path": "",
            },
            indent=2,
        ).encode("utf-8")
        (target / "manifest.json").write_bytes(manifest_bytes)
        return marker, manifest_bytes

    def _assert_previous_install_intact(
        self, disc_id: str, marker: bytes, manifest_bytes: bytes
    ) -> None:
        target = self.dest_root / disc_id
        self.assertEqual((target / "marker.bin").read_bytes(), marker)
        self.assertEqual((target / "manifest.json").read_bytes(), manifest_bytes)
        self.assertEqual(list(self.dest_root.glob(".staging_*")), [])
        self.assertEqual(list(self.dest_root.glob(f"{disc_id}.retired-*")), [])


class RouteBlockedTests(PreparationFailClosedCase):
    """A claphanz_xb profile whose archive stage cannot run is never READY."""

    def test_zero_xb_preparation_is_not_promoted_as_ready(self) -> None:
        """Failing-before: the no-XB synthetic ISO used to become a success.

        On origin/main a claphanz_xb profile staged only the ISO hardlink,
        found zero ``*.xb*`` candidates, swallowed any parser failure, wrote a
        manifest, promoted the tree, emitted READY, and returned
        ``success=True``.
        """
        iso_file = self.temp_dir / "claphanz.iso"
        _create_mock_iso(iso_file, disc_id=CLAPHANZ_ISO)

        events: list[ProgressEvent] = []
        result = self._engine(claphanz=True).prepare_game(
            iso_file,
            on_progress=events.append,
            destination_root=self.dest_root,
        )

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "PREPARATION_ROUTE_UNSUPPORTED")
        self.assertEqual(result.disc_id, CLAPHANZ_ISO)
        self.assertIsNone(result.prepared_root)
        self.assertIsNone(result.manifest_path)

        stages = [event.stage.value for event in events]
        self.assertNotIn("READY", stages)
        self.assertIn("FAILED", stages)

        self.assertFalse(self.dest_root.exists())

    def test_blocked_route_leaves_previous_install_untouched(self) -> None:
        marker, manifest_bytes = self._seed_previous_install(CLAPHANZ_ISO)
        iso_file = self.temp_dir / "claphanz.iso"
        _create_mock_iso(iso_file, disc_id=CLAPHANZ_ISO)

        result = self._engine(claphanz=True).prepare_game(
            iso_file,
            destination_root=self.dest_root,
        )

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "PREPARATION_ROUTE_UNSUPPORTED")
        self._assert_previous_install_intact(CLAPHANZ_ISO, marker, manifest_bytes)

    def test_hostile_xb_iso_fixtures_fail_closed_and_preserve_previous_install(
        self,
    ) -> None:
        """Truncated FST, unsafe path, and decode-failure discs all refuse."""
        for variant, payload in _hostile_xb_payloads().items():
            with self.subTest(variant=variant):
                case_dir = self.temp_dir / variant
                case_dir.mkdir()
                dest_root = case_dir / "installed_games"
                target = dest_root / CLAPHANZ_ISO
                target.mkdir(parents=True)
                marker = f"previous install for {variant}".encode("utf-8")
                (target / "marker.bin").write_bytes(marker)
                manifest_bytes = json.dumps(
                    {"schema_version": 1, "disc_id": CLAPHANZ_ISO}
                ).encode("utf-8")
                (target / "manifest.json").write_bytes(manifest_bytes)

                iso_file = case_dir / "hostile.iso"
                _iso_with_payload(iso_file, payload, CLAPHANZ_ISO)

                events: list[ProgressEvent] = []
                result = PreparationEngine(
                    base_dir=case_dir, registry=_claphanz_registry()
                ).prepare_game(
                    iso_file,
                    on_progress=events.append,
                    destination_root=dest_root,
                )

                self.assertFalse(result.success)
                self.assertEqual(result.error_code, "PREPARATION_ROUTE_UNSUPPORTED")
                self.assertNotIn(
                    "READY", [event.stage.value for event in events]
                )
                self.assertEqual((target / "marker.bin").read_bytes(), marker)
                self.assertEqual(
                    (target / "manifest.json").read_bytes(), manifest_bytes
                )
                self.assertEqual(list(dest_root.glob(".staging_*")), [])
                self.assertEqual(
                    list(dest_root.glob(f"{CLAPHANZ_ISO}.retired-*")), []
                )

    def test_xb_probe_import_failure_cannot_become_success(self) -> None:
        """No import problem may be converted to success, on either route."""
        blocker = _ImportBlocker("xb_probe")
        sys.meta_path.insert(0, blocker)
        try:
            claphanz_iso = self.temp_dir / "claphanz.iso"
            _create_mock_iso(claphanz_iso, disc_id=CLAPHANZ_ISO)
            blocked = self._engine(claphanz=True).prepare_game(
                claphanz_iso,
                destination_root=self.dest_root,
            )

            raw_iso = self.temp_dir / "raw.iso"
            _create_mock_iso(raw_iso, disc_id=RAW_ISO)
            raw = self._engine(claphanz=False).prepare_game(
                raw_iso,
                destination_root=self.dest_root,
            )
        finally:
            sys.meta_path.remove(blocker)

        self.assertFalse(blocked.success)
        self.assertEqual(blocked.error_code, "PREPARATION_ROUTE_UNSUPPORTED")
        self.assertTrue(raw.success)
        self.assertTrue(raw.prepared_root.is_dir())


class _RecordingStager:
    """Stands in for the player's staging transaction and records each call."""

    def __init__(self, result: PreparationResult, progress: tuple[int, ...] = ()) -> None:
        self.result = result
        self.progress = progress
        self.calls: list[tuple[Path, str]] = []

    def __call__(self, iso: Path, disc_id: str, progress) -> PreparationResult:
        self.calls.append((iso, disc_id))
        for percent in self.progress:
            progress(percent, f"{percent}/100")
        return self.result


class NativeStagingRouteTests(PreparationFailClosedCase):
    """An archive disc is handed to the native staging transaction, never to Python."""

    def _native_engine(self, stager) -> PreparationEngine:
        return PreparationEngine(
            base_dir=self.temp_dir, registry=_claphanz_registry(), native_stager=stager
        )

    def test_archive_disc_is_staged_by_the_native_transaction(self) -> None:
        """Failing-before: the route refused every archive disc it was given."""
        iso_file = self.temp_dir / "claphanz.iso"
        _create_mock_iso(iso_file, disc_id=CLAPHANZ_ISO)
        prepared = self.temp_dir / "user" / "games" / CLAPHANZ_ISO
        stager = _RecordingStager(
            PreparationResult(success=True, disc_id=CLAPHANZ_ISO, prepared_root=prepared),
            progress=(25, 100),
        )

        events: list[ProgressEvent] = []
        result = self._native_engine(stager).prepare_game(iso_file, on_progress=events.append)

        self.assertTrue(result.success, result.error_message)
        self.assertEqual(result.prepared_root, prepared)
        self.assertIsNone(result.manifest_path)
        self.assertEqual(stager.calls, [(iso_file.resolve(), CLAPHANZ_ISO)])
        stages = [event.stage.value for event in events]
        self.assertIn("EXTRACTING_ARCHIVES", stages)
        self.assertEqual(stages[-1], "READY")
        self.assertEqual(
            [event.completed for event in events if event.current_item], [25, 100]
        )
        # Python wrote nothing of its own: no games folder, no staging tree.
        self.assertFalse((self.temp_dir / "games").exists())

    def test_native_failure_is_reported_with_its_boundary_and_never_ready(self) -> None:
        iso_file = self.temp_dir / "claphanz.iso"
        _create_mock_iso(iso_file, disc_id=CLAPHANZ_ISO)
        message = "[STAGE_DATA_FOLDER_MISSING] This disc image has no 'xbdata' folder."
        stager = _RecordingStager(PreparationResult(
            success=False, disc_id=CLAPHANZ_ISO,
            error_code="STAGE_DATA_FOLDER_MISSING", error_message=message,
        ))

        events: list[ProgressEvent] = []
        result = self._native_engine(stager).prepare_game(iso_file, on_progress=events.append)

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "STAGE_DATA_FOLDER_MISSING")
        self.assertEqual(result.error_message, message)
        stages = [event.stage.value for event in events]
        self.assertNotIn("READY", stages)
        self.assertEqual(stages[-1], "FAILED")

    def test_destination_is_refused_for_archive_discs(self) -> None:
        """--dest cannot move archive staging out of the per-user data root."""
        iso_file = self.temp_dir / "claphanz.iso"
        _create_mock_iso(iso_file, disc_id=CLAPHANZ_ISO)
        stager = _RecordingStager(PreparationResult(success=True, disc_id=CLAPHANZ_ISO))

        result = self._native_engine(stager).prepare_game(
            iso_file, destination_root=self.dest_root
        )

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "PREPARATION_DESTINATION_UNSUPPORTED")
        self.assertIn("--user-data-root", result.error_message)
        self.assertEqual(stager.calls, [])
        self.assertFalse(self.dest_root.exists())

    def test_missing_player_is_named_with_the_next_step(self) -> None:
        iso_file = self.temp_dir / "claphanz.iso"
        _create_mock_iso(iso_file, disc_id=CLAPHANZ_ISO)
        user_root = self.temp_dir / "user"
        missing = self.temp_dir / "no-player-here" / PLAYER_EXECUTABLE
        stager = NativeTitleStager(user_root, player=missing)

        result = self._native_engine(stager).prepare_game(iso_file)

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "PLAYER_NOT_FOUND")
        self.assertIn(str(missing), result.error_message)
        self.assertIn("--player", result.error_message)
        self.assertFalse(user_root.exists())

    def test_release_layout_player_is_found_beside_the_source_folder(self) -> None:
        source_root = self.temp_dir / "release" / "source"
        stager = NativeTitleStager(self.temp_dir / "user", source_root=source_root)
        self.assertEqual(
            stager.candidates(),
            [source_root / "build" / PLAYER_EXECUTABLE,
             self.temp_dir / "release" / "bin" / PLAYER_EXECUTABLE],
        )


class PrepPreflightSideEffectTests(PreparationFailClosedCase):
    """Compatibility selection must keep decrypt output out of the ISO folder."""

    def test_prepare_without_destination_does_not_write_beside_iso(self) -> None:
        from test_iso_parity import (
            build_plain_mips_elf,
            build_psp_container,
            create_test_iso_with_executables,
        )
        from nk_core.decrypt_boundary import BoundaryOutcome

        source_dir = self.temp_dir / "source"
        source_dir.mkdir()
        iso_file = source_dir / "encrypted.iso"
        create_test_iso_with_executables(
            iso_file, build_psp_container(), disc_id=RAW_ISO
        )
        synthetic_key = source_dir / "keys" / "psp-keyfile.json"
        synthetic_key.parent.mkdir()
        synthetic_key.write_text("synthetic test marker", encoding="utf-8")
        source_names = {path.name for path in source_dir.iterdir()}
        decrypt_destinations: list[Path] = []

        def synthetic_decrypt(_data, destination, *, user_data_root):
            destination = Path(destination)
            decrypt_destinations.append(destination)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(build_plain_mips_elf())
            return BoundaryOutcome("ok", key_path=str(synthetic_key))

        with mock.patch.dict(os.environ):
            os.environ.pop("NAKAGAWA_PSP_KEY_FILE", None)
            with mock.patch(
                "nk_core.iso_inspect.decrypt_bytes_to", side_effect=synthetic_decrypt
            ):
                result = self._engine(claphanz=False).prepare_game(
                    iso_file, destination_root=None
                )

        self.assertTrue(result.success, result.error_message)
        self.assertEqual(decrypt_destinations, [])
        self.assertEqual({path.name for path in source_dir.iterdir()}, source_names)
        self.assertEqual(list(synthetic_key.parent.iterdir()), [synthetic_key])
        manifest = json.loads(result.manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["boot_executable"], "EBOOT.BIN")


class TransactionHardeningTests(PreparationFailClosedCase):
    """Required outputs are validated before promotion; failures preserve the install."""

    def test_unattributable_manifest_is_not_promoted(self) -> None:
        """Failing-before: a manifest for the wrong disc used to be promoted.

        The validation gate must reject a staged manifest that is not
        attributable to the disc and profile actually prepared; origin/main
        wrote the manifest straight through to a successful promotion.
        """
        marker, manifest_bytes = self._seed_previous_install(RAW_ISO)
        iso_file = self.temp_dir / "raw.iso"
        _create_mock_iso(iso_file, disc_id=RAW_ISO)

        real_dump = json.dump

        def corrupt_manifest(obj, fp, **kwargs):
            obj = dict(obj)
            obj["disc_id"] = "TEST90002"
            real_dump(obj, fp, **kwargs)

        with mock.patch(
            "nk_core.prep_engine.json.dump", side_effect=corrupt_manifest
        ):
            result = self._engine(claphanz=False).prepare_game(
                iso_file,
                destination_root=self.dest_root,
            )

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "PREPARATION_OUTPUT_INVALID")
        self._assert_previous_install_intact(RAW_ISO, marker, manifest_bytes)

    def test_manifest_write_failure_fails_and_preserves_previous_install(
        self,
    ) -> None:
        marker, manifest_bytes = self._seed_previous_install(RAW_ISO)
        iso_file = self.temp_dir / "raw.iso"
        _create_mock_iso(iso_file, disc_id=RAW_ISO)

        with mock.patch(
            "nk_core.prep_engine.json.dump",
            side_effect=OSError("synthetic disk-full write failure"),
        ):
            result = self._engine(claphanz=False).prepare_game(
                iso_file,
                destination_root=self.dest_root,
            )

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "PREPARATION_FAILED")
        self.assertIn("synthetic disk-full write failure", result.error_message)
        self._assert_previous_install_intact(RAW_ISO, marker, manifest_bytes)

    def test_promotion_rename_failure_restores_previous_install(self) -> None:
        marker, manifest_bytes = self._seed_previous_install(RAW_ISO)
        iso_file = self.temp_dir / "raw.iso"
        _create_mock_iso(iso_file, disc_id=RAW_ISO)

        original_rename = Path.rename

        def fail_staging_promotion(self, target):
            if self.name.startswith(".staging_"):
                raise OSError("synthetic promotion failure")
            return original_rename(self, target)

        with mock.patch.object(Path, "rename", fail_staging_promotion):
            result = self._engine(claphanz=False).prepare_game(
                iso_file,
                destination_root=self.dest_root,
            )

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "PREPARATION_FAILED")
        self._assert_previous_install_intact(RAW_ISO, marker, manifest_bytes)

    def test_raw_preparation_completes_with_verified_required_outputs(
        self,
    ) -> None:
        """The supported route still succeeds, with outputs beyond the manifest."""
        iso_file = self.temp_dir / "raw.iso"
        _create_mock_iso(iso_file, disc_id=RAW_ISO)

        events: list[ProgressEvent] = []
        result = self._engine(claphanz=False).prepare_game(
            iso_file,
            on_progress=events.append,
            destination_root=self.dest_root,
        )

        self.assertTrue(result.success)
        self.assertIsNotNone(result.prepared_root)
        assert result.prepared_root is not None
        self.assertEqual(result.prepared_root.name, RAW_ISO)

        disc_dir = result.prepared_root / "disc"
        self.assertTrue(disc_dir.is_dir())
        game_iso = disc_dir / "game.iso"
        iso_pointer = disc_dir / "iso_path.txt"
        self.assertTrue(game_iso.exists() or iso_pointer.is_file())
        if iso_pointer.is_file():
            self.assertEqual(
                Path(iso_pointer.read_text(encoding="utf-8").strip()), iso_file
            )

        self.assertIsNotNone(result.manifest_path)
        manifest = json.loads(result.manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["disc_id"], RAW_ISO)
        self.assertEqual(manifest["title_id"], "synthetic-allegrex-v1")
        self.assertEqual(manifest["archive_format"], "raw")
        self.assertEqual(manifest["iso_path"], str(iso_file))
        self.assertEqual(manifest["iso_size"], iso_file.stat().st_size)

        self.assertIn("READY", [event.stage.value for event in events])
        self.assertEqual(list(self.dest_root.glob(".staging_*")), [])


class CliFailClosedTests(PreparationFailClosedCase):
    """``python tools/nk_cli.py prepare ...`` fails closed under the real invocation."""

    def _run_cli(self, *args: str, env_extra: dict[str, str] | None = None):
        env = dict(os.environ)
        # The CLI reads the per-user data root (its title manifests) on every
        # prepare; keep that inside this test's scratch folder.
        profile = self.temp_dir / "profile"
        env.update({
            "LOCALAPPDATA": str(profile / "localappdata"),
            "APPDATA": str(profile / "appdata"),
            "USERPROFILE": str(profile),
            "HOME": str(profile),
            "XDG_DATA_HOME": str(profile / "data"),
            "XDG_CONFIG_HOME": str(profile / "config"),
            "XDG_CACHE_HOME": str(profile / "cache"),
        })
        if env_extra:
            env.update(env_extra)
        return subprocess.run(
            [sys.executable, str(TOOLS_DIR / "nk_cli.py"), *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            cwd=str(self.temp_dir),
            timeout=120,
        )

    def test_cli_prepare_succeeds_for_raw_profile(self) -> None:
        iso_file = self.temp_dir / "raw.iso"
        _create_mock_iso(iso_file, disc_id=RAW_ISO)

        proc = self._run_cli(
            "prepare", str(iso_file), "--dest", str(self.dest_root)
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Preparation successful", proc.stdout)

        target = self.dest_root / RAW_ISO
        manifest = json.loads((target / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["disc_id"], RAW_ISO)
        self.assertEqual(manifest["archive_format"], "raw")
        self.assertTrue(
            (target / "disc" / "game.iso").exists()
            or (target / "disc" / "iso_path.txt").is_file()
        )
        self.assertEqual(list(self.dest_root.glob(".staging_*")), [])

    def test_cli_prepare_fails_closed_for_unsupported_disc(self) -> None:
        iso_file = self.temp_dir / "unsupported.iso"
        _create_mock_iso(iso_file, disc_id=UNSUPPORTED_ISO)

        proc = self._run_cli(
            "prepare", str(iso_file), "--dest", str(self.dest_root)
        )

        self.assertEqual(proc.returncode, 1)
        self.assertIn("ISO_UNSUPPORTED_TITLE", proc.stderr)
        self.assertFalse((self.dest_root / UNSUPPORTED_ISO).exists())

    def test_cli_prepare_fails_closed_for_malformed_iso(self) -> None:
        iso_file = self.temp_dir / "malformed.iso"
        iso_file.write_bytes(b"not an iso")

        proc = self._run_cli(
            "prepare", str(iso_file), "--dest", str(self.dest_root)
        )

        self.assertEqual(proc.returncode, 1)
        self.assertIn("Preparation failed", proc.stderr)
        self.assertFalse(self.dest_root.exists())

    def _claphanz_pythonpath(self) -> str:
        """Seed a claphanz_xb profile into the CLI's default registry.

        The default registry holds no XB profile, so it is added through
        ``sitecustomize`` on ``PYTHONPATH``; the command under test remains a
        literal ``python tools/nk_cli.py prepare ...``.
        """
        site_dir = self.temp_dir / "sitecustom"
        site_dir.mkdir()
        (site_dir / "sitecustomize.py").write_text(
            "import sys\n"
            f"sys.path.insert(0, {str(TOOLS_DIR)!r})\n"
            "import nk_core.title_registry as _tr\n"
            "from nk_core import TitleProfile, TitleRegistry\n"
            "_reg = TitleRegistry(include_defaults=True)\n"
            "_reg.register(TitleProfile(\n"
            "    id='claphanz-synthetic-v1',\n"
            "    name='ClapHanz Synthetic',\n"
            f"    disc_ids=['{CLAPHANZ_ISO}'],\n"
            "    regions=['TEST'],\n"
            "    archive_format='claphanz_xb',\n"
            "    archive_relpath='UMD_DATA/xbdata',\n"
            f"    save_namespace='{CLAPHANZ_ISO}',\n"
            "))\n"
            "_tr._DEFAULT_REGISTRY = _reg\n",
            encoding="utf-8",
        )
        existing_pythonpath = os.environ.get("PYTHONPATH")
        pythonpath = str(site_dir)
        if existing_pythonpath:
            pythonpath = pythonpath + os.pathsep + existing_pythonpath
        return pythonpath

    def test_cli_prepare_never_reports_success_for_claphanz_profile(self) -> None:
        """--dest cannot send an archive disc's files outside the data folder."""
        iso_file = self.temp_dir / "claphanz.iso"
        _create_mock_iso(iso_file, disc_id=CLAPHANZ_ISO)

        proc = self._run_cli(
            "prepare",
            str(iso_file),
            "--dest",
            str(self.dest_root),
            env_extra={"PYTHONPATH": self._claphanz_pythonpath()},
        )

        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("PREPARATION_DESTINATION_UNSUPPORTED", proc.stderr)
        self.assertNotIn("Preparation successful", proc.stdout)
        self.assertFalse((self.dest_root / CLAPHANZ_ISO).exists())
        self.assertEqual(list(self.dest_root.glob(".staging_*")), [])

    def test_cli_prepare_names_a_missing_player_calmly(self) -> None:
        iso_file = self.temp_dir / "claphanz.iso"
        _create_mock_iso(iso_file, disc_id=CLAPHANZ_ISO)
        user_root = self.temp_dir / "user-data"
        missing = self.temp_dir / "missing" / PLAYER_EXECUTABLE

        proc = self._run_cli(
            "prepare", str(iso_file), "--user-data-root", str(user_root),
            "--player", str(missing),
            env_extra={"PYTHONPATH": self._claphanz_pythonpath()},
        )

        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("PLAYER_NOT_FOUND", proc.stderr)
        self.assertIn("--player", proc.stderr)
        self.assertNotIn("Traceback", proc.stderr)
        self.assertFalse((user_root / "games").exists())

    def test_cli_prepare_stages_an_archive_disc_through_the_player(self) -> None:
        """The literal CLI command sets up an archive disc and Play can find it.

        Failing-before: the command refused every archive disc
        (PREPARATION_ROUTE_UNSUPPORTED). It now runs the player's staging
        transaction: the archive is extracted, promoted into the per-user data
        root, and recorded in the library; a second run reuses the files and
        clears what an interrupted run left behind.
        """
        if not BUILT_PLAYER.is_file():
            self.skipTest(
                f"{BUILT_PLAYER} is not built; run `mingw32-make player` (`make player` "
                "on Linux) to exercise the native staging route"
            )
        from test_iso_parity import (
            archive_title_manifest,
            build_plain_mips_elf,
            create_archive_title_iso,
        )

        disc_id = "ULUS99996"
        iso_file = self.temp_dir / "archive.iso"
        create_archive_title_iso(iso_file, disc_id=disc_id, title="Synthetic Archive",
                                 executable=build_plain_mips_elf())
        user_root = self.temp_dir / "user-data"
        (user_root / "manifests").mkdir(parents=True)
        (user_root / "manifests" / "archive.json").write_text(
            json.dumps(archive_title_manifest(disc_id, "archive-ulus99996")),
            encoding="utf-8",
        )

        proc = self._run_cli("prepare", str(iso_file), "--user-data-root", str(user_root))

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("Preparation successful", proc.stdout)
        self.assertIn(f"build-package {disc_id}", proc.stdout)
        game_root = user_root / "games" / disc_id
        member = game_root / "xbdata" / "menu" / "assets.xb.d" / "data" / "raw.bin"
        self.assertEqual(member.read_bytes(), b"synthetic archive member")
        library = json.loads((user_root / "library.json").read_text(encoding="utf-8"))
        [game] = library["games"]
        self.assertEqual(game["disc_id"], disc_id)
        self.assertTrue(game["assets_staged"])
        self.assertEqual(Path(game["prepared_root"]).resolve(), game_root.resolve())
        self.assertEqual(game["extracted_asset_count"], 2)

        # An interrupted earlier run left an unpromoted staging tree behind.
        leftover = user_root / "games" / f".staging_{disc_id}"
        leftover.mkdir()
        (leftover / "partial.bin").write_bytes(b"partial")
        again = self._run_cli("prepare", str(iso_file), "--user-data-root", str(user_root))

        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        self.assertIn("already in place", again.stdout)
        self.assertFalse(leftover.exists())
        self.assertEqual(member.read_bytes(), b"synthetic archive member")


if __name__ == "__main__":
    unittest.main()
