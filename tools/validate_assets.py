#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""
Automated Asset Validation & Regression Script (validate_assets.py)
==================================================================
Cross-examines extracted textures, sounds, and layouts against golden references,
validates GIM palettes, and checks SGD sound stream links.
"""

import os
import sys
import json
import hashlib
import struct
import re
import argparse
from typing import Dict, Any, List, Tuple

def compute_md5(fpath: str) -> str:
    """Compute MD5 hash of a file."""
    hasher = hashlib.md5()
    with open(fpath, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest()

def check_gim_palette(gim_path: str) -> Tuple[bool, str]:
    """Parse GIM file blocks to check if indexed textures have valid non-empty palettes."""
    try:
        with open(gim_path, "rb") as f:
            data = f.read()
    except Exception as e:
        return False, f"Failed to read file: {e}"

    if len(data) < 16 or data[0:11] != b"MIG.00.1PSP":
        return True, "Skipped (not a valid GIM header)"

    has_image = False
    has_palette = False
    fmt = None
    palette_size = 0

    def scan_blocks(offset: int, limit: int):
        nonlocal has_image, has_palette, fmt, palette_size
        while offset + 16 <= limit:
            block_id = struct.unpack_from("<H", data, offset)[0]
            block_size = struct.unpack_from("<I", data, offset + 4)[0]
            hdr_size = struct.unpack_from("<I", data, offset + 12)[0]
            if block_size == 0 or offset + block_size > limit:
                break
            content = offset + hdr_size
            if block_id == 0x0004:
                fmt = struct.unpack_from("<H", data, content + 4)[0]
                has_image = True
            elif block_id == 0x0005:
                has_palette = True
                p_off = struct.unpack_from("<I", data, content + 28)[0]
                p_end = struct.unpack_from("<I", data, content + 32)[0]
                palette_size = p_end - p_off
            elif block_id in (0x0002, 0x0003):
                scan_blocks(content, offset + block_size)
            offset += block_size

    try:
        scan_blocks(16, len(data))
    except Exception as e:
        return False, f"Corrupted GIM block structure: {e}"

    if fmt in (4, 5):  # T4 or T8 indexed formats
        if not has_palette:
            return False, f"Indexed format T{4 if fmt == 4 else 8} requires a palette, but none found"
        if palette_size == 0:
            return False, "Indexed format has empty (0-byte) palette block"
        return True, f"Valid T{4 if fmt == 4 else 8} palette ({palette_size} bytes)"

    return True, f"Direct color format ({fmt})"

def _normalize_asset_path(p: str) -> str:
    """Normalize a path for cross-folder comparison: lowercase, forward-slashed,
    and with the leading `disc0:`, `PSP_GAME/USRDIR` (and drive) prefixes stripped."""
    norm = p.lower().replace("\\", "/")
    for prefix in ("disc0:/", "disc0:", "psp_game/usrdir/", "psp_game/usrdir"):
        if norm.startswith(prefix):
            norm = norm[len(prefix):]
    return norm.lstrip("/")


def check_sgd_stream_links(sgd_path: str, local_files_lower: set, local_full_lower: set) -> Tuple[bool, str]:
    """Scan SGD binary file for references to stream files (.sgb, .vag, .at3, .wav) and verify they exist.

    Matching prefers the FULL normalized relative path (so two files with the same
    basename in different folders are not confused). Only when a full-path match is
    absent does it fall back to a basename-only match, which is reported as an
    explicit "ambiguous" warning.
    """
    try:
        with open(sgd_path, "rb") as f:
            data = f.read()
    except Exception as e:
        return False, f"Failed to read file: {e}"

    # Extract printable ASCII paths/filenames that could be stream targets
    strings = re.findall(b"[a-zA-Z0-9_./\\\\-]{4,128}", data)
    checked_links = []
    missing_links = []
    ambiguous = []

    for s in strings:
        try:
            s_str = s.decode("ascii").lower()
            if s_str.endswith((".sgb", ".vag", ".at3", ".wav")):
                checked_links.append(s_str)
                norm = _normalize_asset_path(s_str)
                if norm in local_full_lower:
                    continue
                fn = os.path.basename(s_str)
                if fn in local_files_lower:
                    ambiguous.append(s_str)
                else:
                    missing_links.append(s_str)
        except Exception:
            pass

    if missing_links:
        msg = f"Missing target stream files: {', '.join(missing_links)}"
        if ambiguous:
            msg += f" | ambiguous basename matches (kept): {', '.join(ambiguous)}"
        return False, msg
    if ambiguous:
        return True, f"OK (verified {len(checked_links)} stream references; ambiguous basename matches: {', '.join(ambiguous)})"
    return True, f"OK (verified {len(checked_links)} stream references)"

def main():
    parser = argparse.ArgumentParser(description="HST Asset Regression and Verification Suite")
    parser.add_argument("--dir", default="place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted", help="Directory of extracted assets containing inventory_map.json files")
    parser.add_argument("--reference", default="tools/reference_hashes.json", help="Path to golden reference hashes file")
    parser.add_argument("--bootstrap", action="store_true", help="Generate/overwrite reference_hashes.json from current directory state")
    parser.add_argument("--strict", action="store_true", help="Fail the run if any extracted asset is missing from the golden reference hashes")
    args = parser.parse_args()

    extracted_dir = os.path.abspath(args.dir)
    ref_path = os.path.abspath(args.reference)

    if not os.path.isdir(extracted_dir):
        print(f"Error: Target directory does not exist: {extracted_dir}")
        sys.exit(1)

    print(f"Scanning target: {extracted_dir}")
    inventories = []
    for root, _, files in os.walk(extracted_dir):
        if "inventory_map.json" in files:
            inventories.append(os.path.join(root, "inventory_map.json"))

    if not inventories:
        print("No inventory_map.json indices found. Please extract assets first.")
        sys.exit(1)

    print(f"Found {len(inventories)} archive inventory index maps.")

    # Build local files set once for stream link checks
    print("Building cached index of extracted filenames for link resolution...")
    local_files_lower = set()
    local_full_lower = set()
    for root, _, files in os.walk(extracted_dir):
        rel = os.path.relpath(root, extracted_dir).replace(os.sep, "/")
        for fn in files:
            local_files_lower.add(fn.lower())
            full = _normalize_asset_path(rel + "/" + fn.lower())
            local_full_lower.add(full)
    print(f"Index built: {len(local_files_lower)} files registered.")

    # Load reference hashes if not bootstrapping
    ref_hashes = {}
    if not args.bootstrap:
        if os.path.isfile(ref_path):
            try:
                with open(ref_path, "r", encoding="utf-8") as f:
                    ref_hashes = json.load(f)
                print(f"Loaded {len(ref_hashes)} golden reference hashes from {os.path.basename(ref_path)}.")
            except Exception as e:
                print(f"Error reading reference hashes: {e}")
                sys.exit(1)
        else:
            print(
                f"Reference file {os.path.basename(ref_path)} not found. "
                "Use --bootstrap explicitly to create a private game-derived manifest."
            )
            sys.exit(2)

    new_ref_hashes = {}
    mismatches = []
    palette_failures = []
    stream_failures = []
    missing_from_golden = []
    total_checked = 0

    for inv_path in inventories:
        inv_dir = os.path.dirname(inv_path)
        rel_inv_dir = os.path.relpath(inv_dir, extracted_dir)
        try:
            with open(inv_path, "r", encoding="utf-8") as f:
                inv = json.load(f)
        except Exception as e:
            print(f"Error reading index {inv_path}: {e}")
            continue

        # Items are categorized in: textures, sounds, scene_graphs, other
        categories = ["textures", "sounds", "scene_graphs", "other"]
        for cat in categories:
            items = inv.get(cat, [])
            for item in items:
                rel_path = item.get("path")
                if not rel_path:
                    continue

                full_path = os.path.join(inv_dir, rel_path)
                if not os.path.isfile(full_path):
                    mismatches.append((rel_path, "File missing on disk"))
                    continue

                # Compute current hash
                h = compute_md5(full_path)
                # Store in bootstrap database
                db_key = os.path.join(rel_inv_dir, rel_path).replace("\\", "/")
                new_ref_hashes[db_key] = h
                total_checked += 1

                # 1. Compare hashes
                if not args.bootstrap:
                    ref_h = ref_hashes.get(db_key)
                    if not ref_h:
                        # Present on disk but absent from the golden reference.
                        # Record it (warn later); only hard-fail under --strict.
                        missing_from_golden.append(db_key)
                    elif ref_h != h:
                        mismatches.append((db_key, f"Hash mismatch (Got: {h}, Expected: {ref_h})"))

                # 2. Check texture palette for GIMs
                if cat == "textures" and rel_path.lower().endswith(".gim"):
                    ok, msg = check_gim_palette(full_path)
                    if not ok:
                        palette_failures.append((db_key, msg))

                # 3. Check sound stream link for SGDs
                if cat == "sounds" and rel_path.lower().endswith(".sgd"):
                    ok, msg = check_sgd_stream_links(full_path, local_files_lower, local_full_lower)
                    if not ok:
                        stream_failures.append((db_key, msg))

    # Summary and execution outcomes
    print("\n--- Asset Validation Summary ---")
    print(f"Total files analyzed: {total_checked}")
    print(f"Hash mismatches:      {len(mismatches)}")
    print(f"Palette failures:     {len(palette_failures)}")
    print(f"Sound link failures:  {len(stream_failures)}")
    print(f"Missing from golden:  {len(missing_from_golden)}")

    if args.bootstrap:
        try:
            with open(ref_path, "w", encoding="utf-8") as f:
                json.dump(new_ref_hashes, f, indent=4)
            print(f"\nBootstrapped/updated reference hashes file: {ref_path} ({len(new_ref_hashes)} files logged)")
        except Exception as e:
            print(f"Failed to write reference hashes: {e}")
            sys.exit(1)

    # Report detailed failures
    success = True
    if mismatches:
        success = False
        print("\n[!] Hash Corruptions / Mismatches:")
        for path, err in mismatches[:15]:
            print(f"  - {path}: {err}")
        if len(mismatches) > 15:
            print(f"  ...and {len(mismatches) - 15} more.")

    if palette_failures:
        success = False
        print("\n[!] Palette Integrity Failures:")
        for path, err in palette_failures[:15]:
            print(f"  - {path}: {err}")
        if len(palette_failures) > 15:
            print(f"  ...and {len(palette_failures) - 15} more.")

    if stream_failures:
        success = False
        print("\n[!] Missing Sound Streams / Broken links:")
        for path, err in stream_failures[:15]:
            print(f"  - {path}: {err}")
        if len(stream_failures) > 15:
            print(f"  ...and {len(stream_failures) - 15} more.")

    if missing_from_golden:
        print("\n[!] WARNING: Extracted assets missing from golden reference hashes:")
        for path in missing_from_golden[:15]:
            print(f"  - {path}")
        if len(missing_from_golden) > 15:
            print(f"  ...and {len(missing_from_golden) - 15} more.")
        if args.strict:
            success = False
            print("    (--strict set: treating missing-from-golden as failure)")

    if success:
        print("\n[+] Verification SUCCESS: All asset validations passed successfully.")
        sys.exit(0)
    else:
        print("\n[!] Verification FAILED: Diagnostics reported regressions.")
        sys.exit(1)

if __name__ == "__main__":
    main()
