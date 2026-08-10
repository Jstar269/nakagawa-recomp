# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Run the VFPU oracle host harness and capture only its stdout stream.

The harness hashes its own running executable, so the digest cannot be pointed at
a file that was never executed.  This launcher exists because GNU Make on Windows
may select cmd.exe for recipes, where redirecting a relative MSYS-style path is
not portable.  It generates and transforms nothing.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("args", nargs=argparse.REMAINDER)
    parsed = parser.parse_args()
    child = parsed.args[1:] if parsed.args[:1] == ["--"] else parsed.args
    exe = Path(parsed.executable)
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    result = subprocess.run(
        [str(exe), *child, "--artifact-sha256", digest],
        stdout=subprocess.PIPE,
        check=False,
    )
    out = Path(parsed.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(result.stdout)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
