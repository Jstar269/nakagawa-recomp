# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Safe, bounded ISO9660 and PARAM.SFO inspector for PSP disc images."""

from __future__ import annotations

import os
from pathlib import Path
import struct
from typing import Dict, Optional, Tuple

from .title_registry import TitleRegistry, get_default_registry
from .types import IsoMetadata, TitleProfile


SECTOR_SIZE = 2048
PVD_SECTOR = 16
ISO_MAGIC = b"\x01CD001\x01"
SFO_MAGIC = b"\x00PSF\x01\x01\x00\x00"


class IsoInspectionError(ValueError):
    """Raised when an ISO image is unreadable or malformed."""


def parse_param_sfo(data: bytes) -> Dict[str, str]:
    """Parse Sony PSP PARAM.SFO key/value pairs safely."""
    if len(data) < 20 or data[0:8] != SFO_MAGIC:
        return {}

    key_table_start = struct.unpack_from("<I", data, 8)[0]
    data_table_start = struct.unpack_from("<I", data, 12)[0]
    entry_count = struct.unpack_from("<I", data, 16)[0]

    if entry_count > 256 or key_table_start >= len(data) or data_table_start >= len(data):
        return {}

    result: Dict[str, str] = {}
    for i in range(entry_count):
        entry_off = 20 + (i * 16)
        if entry_off + 16 > key_table_start:
            break
        key_offset, param_fmt, param_len, max_len, data_offset = struct.unpack_from(
            "<HHIII", data, entry_off
        )
        # Read key name
        k_start = key_table_start + key_offset
        if k_start >= len(data):
            continue
        k_end = data.find(b"\0", k_start)
        if k_end == -1:
            k_end = min(len(data), k_start + 64)
        key_name = data[k_start:k_end].decode("utf-8", errors="replace")

        # Read data
        d_start = data_table_start + data_offset
        d_end = d_start + param_len
        if d_end > len(data):
            continue
        raw_val = data[d_start:d_end]

        if param_fmt in (0x0204, 0x0004):  # UTF-8 string
            val_str = raw_val.rstrip(b"\0").decode("utf-8", errors="replace")
            result[key_name] = val_str
        elif param_fmt == 0x0404:  # Integer
            if len(raw_val) >= 4:
                result[key_name] = str(struct.unpack_from("<I", raw_val, 0)[0])

    return result


def inspect_iso(
    iso_path: Path | str,
    registry: Optional[TitleRegistry] = None,
) -> IsoMetadata:
    """Inspect a PSP ISO image and match it against supported title profiles."""
    path = Path(iso_path)
    if not path.is_file():
        raise IsoInspectionError(f"ISO file does not exist: {path}")

    size_bytes = path.stat().st_size
    if size_bytes < 1024 * 1024:  # Under 1 MiB is not a valid PSP UMD image
        raise IsoInspectionError(f"File is too small to be a valid PSP ISO: {size_bytes} bytes")

    reg = registry or get_default_registry()

    with open(path, "rb") as f:
        # 1. Read Primary Volume Descriptor (PVD)
        f.seek(PVD_SECTOR * SECTOR_SIZE)
        pvd_data = f.read(SECTOR_SIZE)
        if len(pvd_data) < SECTOR_SIZE or pvd_data[0:7] != ISO_MAGIC:
            raise IsoInspectionError("Not a valid ISO9660 image (missing PVD descriptor)")

        volume_id = pvd_data[40:72].decode("latin-1", errors="replace").strip()

        # 2. Search for PARAM.SFO or scan for DISC_ID
        # Fast scan: scan first 64 MiB for SFO magic or Disc ID pattern
        scan_limit = min(size_bytes, 64 * 1024 * 1024)
        f.seek(0)
        chunk = f.read(scan_limit)

        sfo_dict: Dict[str, str] = {}
        sfo_idx = chunk.find(SFO_MAGIC)
        if sfo_idx != -1:
            # Found PARAM.SFO in the initial sector buffer
            sfo_data = chunk[sfo_idx : sfo_idx + 16384]
            sfo_dict = parse_param_sfo(sfo_data)

        disc_id = sfo_dict.get("DISC_ID", "")
        title = sfo_dict.get("TITLE", "")
        version = sfo_dict.get("DISC_VERSION", "1.00")

        # Fallback if SFO was compressed or not in the first 64 MiB: check known signatures
        if not disc_id:
            for profile in reg.all_profiles():
                for cand in profile.disc_ids:
                    cand_bytes = cand.replace("-", "").encode("ascii")
                    cand_dash = cand.encode("ascii")
                    if cand_bytes in chunk or cand_dash in chunk:
                        disc_id = cand
                        title = profile.name
                        break
                if disc_id:
                    break

        if not disc_id:
            import re
            m = re.search(rb"(UCUS|ULUS|UCES|ULES|UCJS|ULJS|UCAS|ULAS)[-_]?([0-9]{5})", chunk)
            if m:
                prefix = m.group(1).decode("ascii")
                number = m.group(2).decode("ascii")
                disc_id = f"{prefix}{number}"
                title = f"PSP Title ({disc_id})"

        if not disc_id:
            disc_id = volume_id or "UNKNOWN"
            title = volume_id or "Unknown PSP Title"

        # Determine region from disc ID prefix
        region = "UNKNOWN"
        upper_disc = disc_id.upper()
        if upper_disc.startswith("UCUS") or upper_disc.startswith("ULUS"):
            region = "NA"
        elif upper_disc.startswith("UCES") or upper_disc.startswith("ULES"):
            region = "EU"
        elif upper_disc.startswith("UCJS") or upper_disc.startswith("ULJS"):
            region = "JP"
        elif upper_disc.startswith("UCAS") or upper_disc.startswith("ULAS"):
            region = "ASIA"

        matched = reg.lookup_by_disc_id(disc_id)

        return IsoMetadata(
            disc_id=disc_id,
            title=title,
            version=version,
            region=region,
            volume_id=volume_id,
            size_bytes=size_bytes,
            matched_profile=matched,
        )
