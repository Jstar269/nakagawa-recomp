# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Build helper for the ``nk_decrypt`` command-line boundary (issue #295).

Compiles the portable C99 decryption engine under ``src/core`` into a
cached helper binary under ``build/`` that the Python tooling and the
test suite invoke.  The binary decrypts with the built-in game tables
(owner decision 2026-10-27); a test may replace them with a key file of
synthetic keys.  A missing entry fails closed naming the exact entry the
container needs, and firmware is refused before any key lookup.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from typing import Callable, Optional, Union

SOURCES = (
    "src/core/nk_json.c",
    "src/core/nk_psp_crypto.c",
    "src/core/nk_psp_keystore.c",
    "src/core/nk_psp_builtin_keys.c",
    "src/core/nk_psp_aes.c",
    "src/core/nk_psp_sha1.c",
    "src/core/nk_psp_ec.c",
    "src/core/nk_psp_kirk.c",
    "src/core/nk_psp_prx.c",
    "src/core/nk_psp_kle.c",
    "src/core/nk_psp_inflate.c",
    "src/core/nk_psp_container.c",
    "src/core/nk_psp_amctrl.c",
    "src/core/nk_psp_lzrc.c",
    "src/core/nk_psp_npumdimg.c",
    "src/core/nk_decrypt_main.c",
)


def _compiler() -> Optional[str]:
    return os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")


def find_or_build_tool(root: Union[Path, str]) -> Path:
    """Return the built ``nk_decrypt`` binary, building or refreshing it when sources change."""
    root = Path(root)
    cc = _compiler()
    if not cc:
        raise RuntimeError("no C compiler found (set CC or put gcc on PATH)")
    suffix = ".exe" if os.name == "nt" else ""
    out = root / "build" / f"nk_decrypt{suffix}"
    stamp = root / "build" / f"nk_decrypt{suffix}.stamp"

    digest = hashlib.sha256()
    digest.update(cc.encode("utf-8"))
    for rel in SOURCES:
        source = root / rel
        digest.update(rel.encode("utf-8"))
        digest.update(source.read_bytes())
    key = digest.hexdigest()

    if out.is_file() and stamp.is_file() and stamp.read_text(encoding="utf-8") == key:
        return out

    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-I", str(root / "src" / "core")]
    cmd.extend(str(root / rel) for rel in SOURCES)
    cmd.extend(["-o", str(out)])
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "failed to build nk_decrypt (built-in decryption boundary):\n" + proc.stdout + proc.stderr
        )
    stamp.write_text(key, encoding="utf-8")
    return out


def _run_tool(tool: Path, encrypted: Path, destination: Path) -> tuple[int, str]:
    proc = subprocess.run(
        [str(tool), "decrypt", "--in", str(encrypted), "--out", str(destination)],
        capture_output=True, text=True,
    )
    lines = (proc.stdout + proc.stderr).strip().splitlines()
    return proc.returncode, lines[-1][-300:] if lines else ""


def decrypt_disc(iso_path: Union[Path, str], user_data_root: Union[Path, str], disc_id: str,
                 tool: Union[Path, str, None] = None, *,
                 runner: Optional[Callable[[Path, Path], tuple[int, str]]] = None) -> list[dict]:
    """Decrypt a disc's encrypted executable and guest modules into its per-title folder.

    This is the player's disc check run from the tools, with the same engine: an encrypted
    ``EBOOT.BIN`` becomes ``titles/<disc>/decrypted/EBOOT.elf`` and each encrypted guest module
    keeps its own name there, where ``inspect_compatibility_preflight`` and the bring-up look
    for them. A decrypted copy already there is kept. Returns one row per encrypted file: the
    disc member, the name written, the engine's exit status (0 for a kept copy) and its last
    message line. A file the engine refuses (firmware is refused by name) stays encrypted and
    the bring-up names that boundary.
    """
    from . import iso_inspect as ii

    iso = Path(iso_path)
    out_dir = ii.decrypted_module_dir(user_data_root, disc_id)
    if out_dir is None:
        raise ValueError(f"{disc_id!r} is not a disc ID")
    if runner is None:
        if tool is None:
            raise ValueError("decrypt_disc needs the nk_decrypt tool or a runner")
        tool_path = Path(tool)

        def runner(encrypted: Path, destination: Path) -> tuple[int, str]:
            return _run_tool(tool_path, encrypted, destination)

    size = iso.stat().st_size
    items: list[tuple[tuple[str, ...], str]] = []
    with iso.open("rb") as stream:
        extent = ii._lookup_iso_file(stream, size, ("PSP_GAME", "SYSDIR", "EBOOT.BIN"))
        if extent and extent[1] >= 4 and \
                ii._read_iso_extent(stream, size, extent[0], extent[1], 0, 4) in (b"~PSP", b"~SCE"):
            items.append((("PSP_GAME", "SYSDIR", "EBOOT.BIN"), "EBOOT.elf"))
    rows: list[dict] = []
    try:
        items += [(tuple(candidate["members"][0]), candidate["name"])
                  for candidate in ii.list_disc_module_candidates(iso)
                  if candidate["kind"] == "encrypted"]
    except ii.IsoInspectionError as exc:
        # A module set the intake refuses (two modules of one name, say) keeps its own named
        # boundary; the executable is still decrypted, so the bring-up reaches that boundary.
        rows.append({"member": None, "name": "(guest modules)", "status": -1,
                     "message": str(exc)[:300]})
    if not items:
        return rows
    out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nk-decrypt-") as staging:
        for members, name in items:
            destination = out_dir / name
            row = {"member": "/".join(members), "name": name}
            if destination.is_file() and destination.stat().st_size > 0:
                rows.append({**row, "status": 0, "message": "kept the decrypted copy already there"})
                continue
            with iso.open("rb") as stream:
                lba, count = ii._lookup_iso_file(stream, size, members)
                blob = ii._read_iso_extent(stream, size, lba, count, 0, count)
            encrypted = Path(staging) / (name + ".enc")
            encrypted.write_bytes(blob)
            status, message = runner(encrypted, destination)
            encrypted.unlink(missing_ok=True)
            rows.append({**row, "status": status, "message": message})
    return rows
