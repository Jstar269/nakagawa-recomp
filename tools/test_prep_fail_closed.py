# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Fail-closed preparation regressions for issue #374.

Every fixture is generated in-process from synthetic bytes: no retail disc,
archive, module, texture, name, or hash appears in this file.

The suite pins the claims the Python preparation route previously got wrong:

* a ``claphanz_xb`` profile whose required archive stage performs no work is
  never promoted as a READY installation (the failing-before false success);
* hostile XB archive bytes -- truncated FST, unsafe member path, decode
  failure -- and an XB-parser import failure cannot convert preparation into
  success, and a previous install survives every failure untouched;
* required-output completeness is checked before atomic promotion: a manifest
  that is not attributable to the selected disc, a staging write failure, or a
  promotion rename failure all fail the transaction with the previous install
  intact;
* the documented ``python tools/nk_cli.py prepare ...`` invocation fails
  closed instead of reporting success for blocked or unsupported work.
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
    ProgressEvent,
    TitleProfile,
    TitleRegistry,
)
from test_extract_xb_security import _make_raw_archive  # noqa: E402
from test_nk_core import _create_mock_iso  # noqa: E402
from test_xb_probe import _make_archive  # noqa: E402
from xb_probe import XBCompression  # noqa: E402


TOOLS_DIR = Path(__file__).resolve().parent
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

    def test_cli_prepare_never_reports_success_for_claphanz_profile(self) -> None:
        """The documented invocation shape, with a claphanz_xb profile registered.

        The CLI loads the default registry, which holds no XB profile, so the
        test seeds one through ``sitecustomize`` on ``PYTHONPATH``. The command
        under test remains a literal ``python tools/nk_cli.py prepare ...``.
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

        iso_file = self.temp_dir / "claphanz.iso"
        _create_mock_iso(iso_file, disc_id=CLAPHANZ_ISO)

        existing_pythonpath = os.environ.get("PYTHONPATH")
        pythonpath = str(site_dir)
        if existing_pythonpath:
            pythonpath = pythonpath + os.pathsep + existing_pythonpath

        proc = self._run_cli(
            "prepare",
            str(iso_file),
            "--dest",
            str(self.dest_root),
            env_extra={"PYTHONPATH": pythonpath},
        )

        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("PREPARATION_ROUTE_UNSUPPORTED", proc.stderr)
        self.assertNotIn("Preparation successful", proc.stdout)
        self.assertFalse((self.dest_root / CLAPHANZ_ISO).exists())
        self.assertEqual(list(self.dest_root.glob(".staging_*")), [])


if __name__ == "__main__":
    unittest.main()
