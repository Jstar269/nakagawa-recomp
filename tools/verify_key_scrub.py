#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Check whether any PSP KIRK/amctrl constant is still reachable in Git history.

This contains no key material: it reads the constant *values* from the local key
file (see docs/PGD_KEYS.md) and searches every reachable commit for each value in
the textual encodings the tree has ever used -- contiguous hex, and C/Python byte
arrays with assorted spacing, case, and zero-padding. `git log -S <hex>` alone is
NOT sufficient, because it never matches the `{0x12,0x46,...}` byte-array form.

Exit 0  -> no constant found in any reachable commit (scrub verified / never present).
Exit 3  -> at least one constant is still reachable (scrub incomplete / not yet run).
Exit 2  -> could not run (missing key file, not a git repo, etc.).

Run it BEFORE a scrub to confirm the exposure, and AFTER to confirm it is gone.
Usage:
  python tools/verify_key_scrub.py            # uses keys/pgd_keys.txt or $SR_PGD_KEYS
  python tools/verify_key_scrub.py --keys PATH
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys


def key_values(path: str) -> dict[str, bytes]:
    """name -> 16 raw bytes, parsed from the `name = hex` key file."""
    values: dict[str, bytes] = {}
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, _, value = line.partition("=")
            name, value = name.strip(), value.strip()
            if len(value) == 32:
                try:
                    values[name] = bytes.fromhex(value)
                except ValueError:
                    pass
    return values


def encodings(raw: bytes) -> list[str]:
    """Every textual spelling of `raw` this codebase has plausibly used."""
    forms: set[str] = set()
    forms.add(raw.hex())                      # lowercase contiguous hex
    forms.add(raw.hex().upper())              # uppercase contiguous hex
    for sep in (",", ", "):                   # C / Python byte arrays
        for pad in (True, False):             # 0x0a vs 0xa (non-padded ints)
            for up in (False, True):          # 0xab vs 0xAB
                parts = []
                for b in raw:
                    h = f"{b:02x}" if pad else f"{b:x}"
                    if up:
                        h = h.upper()
                    parts.append("0x" + h)
                forms.add(sep.join(parts))
                # Python bytes([...]) also renders small values as bare ints (1, not 0x01).
    # Bare-int Python list form (bytes([0x27, 0x74, 1, 2, ...])): decimal for <16, hex else.
    forms.add(", ".join(str(b) if b < 16 else f"0x{b:02x}" for b in raw))
    forms.add(", ".join(str(b) if b < 16 else f"0x{b:02X}" for b in raw))
    return sorted(forms)


def search_history(needle: str) -> bool:
    """True if `needle` appears in the content of any reachable commit (pickaxe)."""
    result = subprocess.run(
        ["git", "log", "--all", "-S", needle, "--oneline", "--source"],
        capture_output=True, text=True, check=False,
    )
    return result.returncode == 0 and bool(result.stdout.strip())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keys", default=os.environ.get("SR_PGD_KEYS") or os.path.join("keys", "pgd_keys.txt"))
    args = parser.parse_args(argv)

    if not os.path.isfile(args.keys):
        print(f"key file not found: {args.keys} (see docs/PGD_KEYS.md)", file=sys.stderr)
        return 2

    values = key_values(args.keys)
    if not values:
        print(f"no usable constants parsed from {args.keys}", file=sys.stderr)
        return 2

    exposed: list[str] = []
    for name, raw in values.items():
        hit = next((form for form in encodings(raw) if search_history(form)), None)
        status = "REACHABLE in history" if hit else "clean"
        print(f"  {name}: {status}")
        if hit:
            exposed.append(name)

    if exposed:
        print(f"\n{len(exposed)} constant(s) still reachable in Git history: {', '.join(exposed)}")
        print("History still exposes the keys. See docs/KEY_HISTORY_SCRUB.md.")
        return 3
    print("\nNo PSP constant is reachable in any commit. History is clean.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
