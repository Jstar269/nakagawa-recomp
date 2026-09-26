# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Build helper for the ``nk_decrypt`` command-line boundary (issue #295).

Compiles the portable C99 decryption engine under ``src/core`` into a
cached helper binary under ``build/`` that the Python tooling and the
test suite invoke.  The binary embeds no key material: callers pass a
user-supplied local key file at run time, and a missing entry fails
closed naming the exact entry the container needs.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
from typing import Optional, Union

SOURCES = (
    "src/core/nk_json.c",
    "src/core/nk_psp_crypto.c",
    "src/core/nk_psp_keystore.c",
    "src/core/nk_psp_aes.c",
    "src/core/nk_psp_sha1.c",
    "src/core/nk_psp_ec.c",
    "src/core/nk_psp_kirk.c",
    "src/core/nk_psp_prx.c",
    "src/core/nk_psp_kle.c",
    "src/core/nk_psp_inflate.c",
    "src/core/nk_psp_container.c",
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
            "failed to build nk_decrypt (issue #295 boundary):\n" + proc.stdout + proc.stderr
        )
    stamp.write_text(key, encoding="utf-8")
    return out
