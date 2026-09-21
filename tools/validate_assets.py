#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""
Automated Asset Validation & Regression Script (validate_assets.py)
==================================================================
Cross-examines extracted textures, sounds, and layouts against golden references,
validates GIM palettes, and checks SGD sound stream links.

Fail-closed contract (issue #376): every inventory_map.json discovered under the
target tree must be readable, well-formed JSON, and schema-valid, and at least
one inventory must parse for the run to succeed. An inventory that cannot be
fully examined is a validation failure, never a silent skip: zero successfully
parsed inventories cannot report SUCCESS, one bad inventory among good ones
fails the whole run, and a .gim asset with an invalid or truncated GIM header is
reported as a corruption failure rather than a successful palette skip. GIM
block offsets and sizes are bounds-checked before every field read, with the
offending block kept in the diagnostic.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys

GIM_MAGIC = b"MIG.00.1PSP"
GIM_HEADER_SIZE = 16
GIM_BLOCK_HEADER_SIZE = 16
# Full block-local content header for 0x0004/0x0005 blocks, mirroring
# tools/extract_xb.py::decode_gim_data's `content_limit - content < 36` check.
GIM_CONTENT_HEADER_SIZE = 36
# Formats decode_gim_data actually decodes (bpp_map + its final else rejects).
GIM_SUPPORTED_IMAGE_FORMATS = frozenset((0, 1, 2, 3, 4, 5))
# Mirrors tools/extract_xb.py::GIM_MAX_NESTING exactly, including its boundary:
# the decoder rejects when depth > GIM_MAX_NESTING, so 32 container levels are
# legal and 33 are not.  The constant is mirrored rather than imported because
# this script must run under `python -I` (isolated mode strips the script
# directory, so a sibling import would fail) and importing extract_xb has
# import-time side effects (sys.path mutation).
GIM_MAX_SCAN_DEPTH = 32
# Mirrors tools/extract_xb.py::GIM_MAX_BLOCKS (same reasoning as above).
GIM_MAX_BLOCKS = 1 << 20

# The inventory schema written by tools/extract_xb.py process_extracted_directory.
INVENTORY_CATEGORIES = ("textures", "sounds", "scene_graphs", "other")
INVENTORY_ITEM_KEYS = ("name", "path", "size_bytes")


class InventoryError(Exception):
    """An inventory_map.json could not be read, parsed, or fails the schema."""


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


def _gim_u16(data: bytes, offset: int) -> int:
    if offset + 2 > len(data):
        raise IndexError(f"u16 read at {offset:#x} is past end of data ({len(data)} bytes)")
    return struct.unpack_from("<H", data, offset)[0]


def _gim_u32(data: bytes, offset: int) -> int:
    if offset + 4 > len(data):
        raise IndexError(f"u32 read at {offset:#x} is past end of data ({len(data)} bytes)")
    return struct.unpack_from("<I", data, offset)[0]


def check_gim_palette(gim_path: str) -> tuple[bool, str]:
    """Parse GIM file blocks to check if indexed textures have valid non-empty palettes.

    Parse GIM file blocks to check if indexed textures have valid non-empty
    palettes, using the same bounds contract as the repository decoder
    (tools/extract_xb.py::decode_gim_data): block sizes are validated before
    advancing, payload offsets are block-content-relative and must land inside
    their own block's content region, image/palette content headers must be
    complete, and an unsupported or missing image block is a failure.

    The caller only invokes this for .gim-classified assets, so a file without a
    readable GIM header is a corruption failure, not a successful skip.
    """
    try:
        with open(gim_path, "rb") as f:
            data = f.read()
    except Exception as e:
        return False, f"Failed to read file: {e}"

    if len(data) < GIM_HEADER_SIZE:
        return False, (
            f"Truncated GIM header: file is {len(data)} bytes, "
            f"a GIM header needs {GIM_HEADER_SIZE}"
        )
    if data[0:11] != GIM_MAGIC:
        return False, f"Invalid GIM header magic: expected MIG.00.1PSP, got {data[0:11]!r}"

    has_image = False
    has_palette = False
    fmt: int | None = None
    palette_size = 0
    blocks_seen = 0

    def scan_blocks(offset: int, limit: int, depth: int = 0) -> None:
        nonlocal has_image, has_palette, fmt, palette_size, blocks_seen
        if depth > GIM_MAX_SCAN_DEPTH:
            raise IndexError(f"container nesting deeper than {GIM_MAX_SCAN_DEPTH} levels")
        while offset < limit:
            if limit - offset < GIM_BLOCK_HEADER_SIZE:
                raise IndexError(
                    f"truncated block header: only {limit - offset} bytes remain at offset {offset:#x}"
                )
            blocks_seen += 1
            if blocks_seen > GIM_MAX_BLOCKS:
                raise IndexError(f"GIM declares more than {GIM_MAX_BLOCKS} blocks")
            block_id = _gim_u16(data, offset)
            block_size = _gim_u32(data, offset + 4)
            hdr_size = _gim_u32(data, offset + 12)
            if block_size < GIM_BLOCK_HEADER_SIZE:
                raise IndexError(
                    f"block at offset {offset:#x} declares size {block_size} below the block header size"
                )
            if offset > limit or block_size > limit - offset:
                raise IndexError(
                    f"block at offset {offset:#x} ({block_size} bytes) extends past end of data"
                )
            if hdr_size < GIM_BLOCK_HEADER_SIZE or hdr_size > block_size:
                raise IndexError(
                    f"block at offset {offset:#x} declares invalid header size {hdr_size}"
                )
            content = offset + hdr_size
            content_limit = offset + block_size
            try:
                if block_id == 0x0004:
                    if content_limit - content < GIM_CONTENT_HEADER_SIZE:
                        raise IndexError(
                            f"image block content region is {content_limit - content} bytes, "
                            f"needs the full {GIM_CONTENT_HEADER_SIZE}-byte content header"
                        )
                    # Dimension/pixel-budget caps (w, h, raw byte counts) are
                    # decode-side allocation bounds in decode_gim_data and are
                    # deliberately not mirrored here: they are not structural
                    # validity, and this check must not reject a GIM the
                    # decoder could structurally scan.
                    fmt = _gim_u16(data, content + 4)
                    d_off = _gim_u32(data, content + 28)
                    d_end = _gim_u32(data, content + 32)
                    if d_end < d_off:
                        raise IndexError(f"image data offsets inverted (start {d_off} > end {d_end})")
                    if d_end > content_limit - content:
                        raise IndexError(
                            f"image data end {d_end} extends past its block's content region"
                            f" ({content_limit - content} bytes)"
                        )
                    has_image = True
                elif block_id == 0x0005:
                    if content_limit - content < GIM_CONTENT_HEADER_SIZE:
                        raise IndexError(
                            f"palette block content region is {content_limit - content} bytes, "
                            f"needs the full {GIM_CONTENT_HEADER_SIZE}-byte content header"
                        )
                    has_palette = True
                    p_off = _gim_u32(data, content + 28)
                    p_end = _gim_u32(data, content + 32)
                    if p_end < p_off:
                        raise IndexError(f"palette offsets inverted (start {p_off} > end {p_end})")
                    if p_end > content_limit - content:
                        raise IndexError(
                            f"palette end {p_end} extends past its block's content region"
                            f" ({content_limit - content} bytes)"
                        )
                    palette_size = p_end - p_off
                elif block_id in (0x0002, 0x0003):
                    scan_blocks(content, content_limit, depth + 1)
            except IndexError as e:
                raise IndexError(f"{e} (in 0x{block_id:04x} block at offset {offset:#x})") from None
            offset = content_limit

    try:
        scan_blocks(GIM_HEADER_SIZE, len(data))
    except IndexError as e:
        return False, f"Corrupted GIM block structure: {e}"

    if not has_image:
        return False, (
            "No image block (0x0004) found: palette/container-only GIM is not a valid texture"
        )
    if fmt not in GIM_SUPPORTED_IMAGE_FORMATS:
        return False, (
            f"Unsupported GIM image format {fmt} (supported: "
            f"{', '.join(str(f) for f in sorted(GIM_SUPPORTED_IMAGE_FORMATS))})"
        )

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


def check_sgd_stream_links(sgd_path: str, local_files_lower: set, local_full_lower: set) -> tuple[bool, str]:
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


def _load_inventory(inv_path: str) -> dict:
    """Read and JSON-parse one inventory_map.json, failing closed."""
    try:
        with open(inv_path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        raise InventoryError(f"unreadable ({e})") from None
    except UnicodeDecodeError as e:
        raise InventoryError(f"not valid UTF-8 text ({e})") from None
    try:
        inv = json.loads(text)
    except json.JSONDecodeError as e:
        raise InventoryError(f"malformed JSON ({e})") from None
    if not isinstance(inv, dict):
        raise InventoryError(f"top-level JSON value must be an object, got {type(inv).__name__}")
    return inv


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _path_is_contained(rel_path: str) -> bool:
    """True for a relative, non-escaping asset path (no drive, root, or `..`)."""
    if rel_path[:1] in ("/", "\\") or re.match(r"^[A-Za-z]:", rel_path):
        return False
    return ".." not in rel_path.replace("\\", "/").split("/")


def validate_inventory_shape(inv: dict) -> list[str]:
    """Return the schema violations of one parsed inventory (empty list = valid).

    The schema is the one `tools/extract_xb.py process_extracted_directory`
    writes: the four category keys, each a list of objects carrying a string
    `name`, a relative string `path`, and a non-negative integer `size_bytes`.
    The optional `rejected` list (added by the extractor security work) is only
    loosely shape-checked so inventories from older extractions still validate.
    """
    problems: list[str] = []
    for cat in INVENTORY_CATEGORIES:
        if cat not in inv:
            problems.append(f"missing required category '{cat}'")
            continue
        items = inv[cat]
        if not isinstance(items, list):
            problems.append(f"category '{cat}' must be a list, got {type(items).__name__}")
            continue
        for idx, item in enumerate(items):
            if not isinstance(item, dict):
                problems.append(f"{cat}[{idx}]: item must be an object, got {type(item).__name__}")
                continue
            for key in INVENTORY_ITEM_KEYS:
                if key not in item:
                    problems.append(f"{cat}[{idx}]: missing required key '{key}'")
            name = item.get("name")
            if "name" in item and (not isinstance(name, str) or not name):
                problems.append(f"{cat}[{idx}]: 'name' must be a non-empty string")
            path = item.get("path")
            if "path" in item and (not isinstance(path, str) or not path):
                problems.append(f"{cat}[{idx}]: 'path' must be a non-empty string")
            elif isinstance(path, str) and not _path_is_contained(path):
                problems.append(f"{cat}[{idx}]: 'path' {path!r} escapes the inventory directory")
            size = item.get("size_bytes")
            if "size_bytes" in item and not (_is_int(size) and size >= 0):
                problems.append(f"{cat}[{idx}]: 'size_bytes' must be a non-negative integer")

    rejected = inv.get("rejected")
    if rejected is not None:
        if not isinstance(rejected, list):
            problems.append("category 'rejected' must be a list when present")
        else:
            for idx, item in enumerate(rejected):
                if not isinstance(item, dict) or not isinstance(item.get("reason"), str):
                    problems.append(f"rejected[{idx}]: must be an object with a string 'reason'")
    return problems


def _display_path(path: str, extracted_dir: str) -> str:
    """Show a discovered path relative to the target tree when it lives inside it."""
    rel = os.path.relpath(path, extracted_dir).replace(os.sep, "/")
    return path if rel.startswith("..") else rel


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
    inventory_failures = []
    inventories_discovered = len(inventories)
    inventories_parsed = 0
    total_checked = 0

    for inv_path in inventories:
        inv_dir = os.path.dirname(inv_path)
        rel_inv_dir = os.path.relpath(inv_dir, extracted_dir)
        try:
            inv = _load_inventory(inv_path)
        except InventoryError as e:
            print(f"Error reading index {inv_path}: {e}")
            inventory_failures.append((inv_path, str(e)))
            continue

        shape_problems = validate_inventory_shape(inv)
        if shape_problems:
            summary = "; ".join(shape_problems[:5])
            if len(shape_problems) > 5:
                summary += f" (and {len(shape_problems) - 5} more)"
            print(f"Error reading index {inv_path}: schema violations: {summary}")
            inventory_failures.append((inv_path, f"schema violations: {summary}"))
            continue

        inventories_parsed += 1

        # Items are categorized in: textures, sounds, scene_graphs, other
        for cat in INVENTORY_CATEGORIES:
            for item in inv[cat]:
                rel_path = item["path"]
                full_path = os.path.join(inv_dir, rel_path)
                if not os.path.isfile(full_path):
                    mismatches.append((rel_path, "File missing on disk"))
                    continue

                try:
                    h = compute_md5(full_path)
                except OSError as e:
                    mismatches.append((rel_path, f"Unreadable asset: {e}"))
                    continue

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
    print(f"Inventories discovered: {inventories_discovered}")
    print(f"Inventories parsed:     {inventories_parsed}")
    print(f"Inventories failed:     {len(inventory_failures)}")
    print(f"Total files analyzed:   {total_checked}")
    print(f"Hash mismatches:        {len(mismatches)}")
    print(f"Palette failures:       {len(palette_failures)}")
    print(f"Sound link failures:    {len(stream_failures)}")
    print(f"Missing from golden:    {len(missing_from_golden)}")

    if args.bootstrap and inventory_failures:
        print(
            "\n[!] Refusing to bootstrap a golden reference manifest: "
            "the target tree was not completely examined "
            f"({len(inventory_failures)} of {inventories_discovered} inventories failed)."
        )
    elif args.bootstrap:
        try:
            with open(ref_path, "w", encoding="utf-8") as f:
                json.dump(new_ref_hashes, f, indent=4)
            print(f"\nBootstrapped/updated reference hashes file: {ref_path} ({len(new_ref_hashes)} files logged)")
        except Exception as e:
            print(f"Failed to write reference hashes: {e}")
            sys.exit(1)

    # Report detailed failures
    success = True
    if inventory_failures:
        success = False
        print("\n[!] Inventory Index Failures (unreadable, malformed, or schema-invalid):")
        for path, err in inventory_failures[:15]:
            print(f"  - {_display_path(path, extracted_dir)}: {err}")
        if len(inventory_failures) > 15:
            print(f"  ...and {len(inventory_failures) - 15} more.")

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

    if inventories_discovered and not inventories_parsed:
        success = False
        print("\n[!] No inventory could be parsed; the target tree was not fully examined.")

    if success:
        print("\n[+] Verification SUCCESS: All asset validations passed successfully.")
        sys.exit(0)
    else:
        print("\n[!] Verification FAILED: Diagnostics reported regressions.")
        sys.exit(1)


if __name__ == "__main__":
    main()
