# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Run the production-HLE oracle executable and capture only its stdout stream.

GNU Make on Windows may select cmd.exe for recipes; shell redirection of a relative
MSYS-style executable path is not portable there.  This small launcher keeps the
target platform-neutral without generating or transforming any oracle record.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("args", nargs=argparse.REMAINDER)
    parsed = parser.parse_args()
    child_args = parsed.args[1:] if parsed.args[:1] == ["--"] else parsed.args
    result = subprocess.run(
        [parsed.executable, *child_args],
        stdout=subprocess.PIPE,
        check=False,
    )
    output = Path(parsed.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result.stdout)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
