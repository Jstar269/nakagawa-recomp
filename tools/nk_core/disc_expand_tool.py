# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Compressed disc images (.cso, .zso, .chd) for the Python tools.

The player expands a compressed disc image once at import into a plain .iso in
the per-user data folder (``src/core/nk_disc_expand.c``). The Python tools reach
the same C code through the ``nk_disc_expand`` command-line wrapper
(``src/core/nk_disc_expand_main.c``), built and cached under ``build/`` the way
``decrypt_tool`` builds ``nk_decrypt``. There is no second decoder in Python:
this module only recognises the magic, runs the wrapper and reads its one JSON
line.

``ensure_plain_iso`` returns a plain .iso path for any disc image: the path
itself for an .iso, or the expansion (made now, or reused when the same
unchanged file was expanded before) for a compressed image.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from typing import Optional, Union

ROOT = Path(__file__).resolve().parents[2]

MAGICS = {b"CISO": "cso", b"ZISO": "zso", b"MComprHD": "chd"}

CORE_SOURCES = (
    "src/core/nk_disc_expand_main.c",
    "src/core/nk_disc_expand.c",
    "src/core/nk_disc_chd.c",
    "src/core/nk_iso.c",
    "src/core/nk_psp_inflate.c",
    "src/core/nk_psp_sha1.c",
    "src/core/generated/nk_title_catalog.c",
)
CORE_HEADERS = (
    "src/core/nk_disc_expand.h",
    "src/core/nk_disc_chd.h",
    "src/core/nk_iso.h",
    "src/core/nk_platform.h",
    "src/core/nk_psp_inflate.h",
    "src/core/nk_psp_sha1.h",
    "src/core/nk_types.h",
    "src/core/generated/nk_title_catalog.h",
)
LIBCHDR_DIR = "third_party/libchdr"
LIBCHDR_SOURCES = tuple(
    f"{LIBCHDR_DIR}/src/{name}" for name in (
        "libchdr_bitstream.c", "libchdr_cdrom.c", "libchdr_chd.c", "libchdr_codec_avhuff.c",
        "libchdr_codec_cdfl.c", "libchdr_codec_cdlz.c", "libchdr_codec_cdzl.c",
        "libchdr_codec_cdzs.c", "libchdr_codec_flac.c", "libchdr_codec_huff.c",
        "libchdr_codec_lzma.c", "libchdr_codec_zlib.c", "libchdr_codec_zstd.c",
        "libchdr_flac.c", "libchdr_huffman.c",
    )
) + (
    f"{LIBCHDR_DIR}/deps/lzma-26.02/src/LzmaDec.c",
    f"{LIBCHDR_DIR}/deps/miniz-3.1.2/miniz.c",
    f"{LIBCHDR_DIR}/deps/zstd-1.5.7/zstddeclib.c",
)
# Upstream's default configuration, as the Makefile builds it.
LIBCHDR_FLAGS = (
    "-std=c99", "-O2",
    f"-I{LIBCHDR_DIR}/include",
    f"-I{LIBCHDR_DIR}/deps/lzma-26.02/include",
    f"-I{LIBCHDR_DIR}/deps/miniz-3.1.2",
    f"-I{LIBCHDR_DIR}/deps/zstd-1.5.7",
    "-DMINIZ_NO_ARCHIVE_APIS", "-DMINIZ_NO_DEFLATE_APIS", "-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME",
)
# A refusal the wrapper names, and the exit codes it uses.
EXIT_REFUSED = 2


class DiscImageRefused(ValueError):
    """The wrapper refused the image; ``code`` is its named boundary."""

    def __init__(self, message: str, code: str) -> None:
        super().__init__(message)
        self.code = code


def compressed_format(path: Union[Path, str]) -> Optional[str]:
    """Return "cso", "zso" or "chd" when the file starts with that magic, else None."""
    try:
        with open(path, "rb") as stream:
            head = stream.read(8)
    except OSError:
        return None
    for magic, name in MAGICS.items():
        if head[: len(magic)] == magic:
            return name
    return None


def _compiler() -> Optional[str]:
    return os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")


def _platform_source() -> str:
    return "src/core/nk_platform_win32.c" if os.name == "nt" else "src/core/nk_platform_posix.c"


def find_or_build_tool(root: Union[Path, str] = ROOT) -> Path:
    """Return the built ``nk_disc_expand`` binary, building it when its sources change."""
    root = Path(root)
    cc = _compiler()
    if not cc:
        raise RuntimeError("no C compiler found (set CC or put gcc on PATH)")
    suffix = ".exe" if os.name == "nt" else ""
    out = root / "build" / f"nk_disc_expand{suffix}"
    stamp = root / "build" / f"nk_disc_expand{suffix}.stamp"
    core = CORE_SOURCES + (_platform_source(),)

    digest = hashlib.sha256()
    digest.update(cc.encode("utf-8"))
    for rel in core + CORE_HEADERS + LIBCHDR_SOURCES:
        digest.update(rel.encode("utf-8"))
        digest.update((root / rel).read_bytes())
    key = digest.hexdigest()
    if out.is_file() and stamp.is_file() and stamp.read_text(encoding="utf-8") == key:
        return out

    objects = root / "build" / "nk_disc_expand_objects"
    objects.mkdir(parents=True, exist_ok=True)
    object_paths = []
    for rel in LIBCHDR_SOURCES:
        obj = objects / (Path(rel).stem + ".o")
        cmd = [cc, *LIBCHDR_FLAGS, "-c", rel, "-o", str(obj)]
        proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"failed to build libchdr ({rel}):\n{proc.stdout}{proc.stderr}")
        object_paths.append(str(obj))
    cmd = [cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Isrc/core", "-Isrc/core/generated",
           f"-I{LIBCHDR_DIR}/include", *core, *object_paths, "-o", str(out)]
    if os.name == "nt":
        cmd.extend(["-lshell32", "-lole32", "-luuid"])
    proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("failed to build nk_disc_expand:\n" + proc.stdout + proc.stderr)
    stamp.write_text(key, encoding="utf-8")
    return out


def run_tool(*args: str, root: Union[Path, str] = ROOT) -> dict:
    """Run the wrapper and return its JSON result; a refusal raises DiscImageRefused."""
    tool = find_or_build_tool(root)
    proc = subprocess.run([str(tool), *args], capture_output=True, text=True, encoding="utf-8")
    try:
        result = json.loads(proc.stdout.strip().splitlines()[-1]) if proc.stdout.strip() else None
    except (ValueError, IndexError):
        result = None
    if not isinstance(result, dict):
        raise RuntimeError(
            f"nk_disc_expand gave no result (exit {proc.returncode}): {proc.stderr.strip()}"
        )
    if result.get("status") == "OK" and proc.returncode == 0:
        return result
    if result.get("status") == "REFUSED" and proc.returncode == EXIT_REFUSED:
        raise DiscImageRefused(str(result.get("message", "")), str(result.get("code", "")))
    raise RuntimeError(f"nk_disc_expand failed (exit {proc.returncode}): {proc.stdout.strip()}")


def import_image(path: Union[Path, str], data_root: Union[Path, str],
                 *, root: Union[Path, str] = ROOT) -> dict:
    """Expand a compressed image into data_root once (reusing an earlier expansion)."""
    return run_tool("import", "--data-root", str(data_root), str(path), root=root)


def ensure_plain_iso(path: Union[Path, str], data_root: Union[Path, str],
                     *, root: Union[Path, str] = ROOT) -> Path:
    """Return a plain .iso for any disc image: the path itself, or its expansion."""
    path = Path(path)
    if compressed_format(path) is None:
        return path
    result = import_image(path, data_root, root=root)
    iso = Path(str(result["iso_path"]))
    if not iso.is_file():
        raise RuntimeError(f"nk_disc_expand reported an expansion that is missing: {iso}")
    return iso
