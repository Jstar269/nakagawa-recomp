# SPDX-License-Identifier: GPL-2.0-or-later

"""Sync + invariant tests for the fail-closed VFPU table loader (issue #187).

The runtime embeds its own copy of the table manifest (name, byte length,
SHA-256) in src/rt/vfpu_tables.c so it never has to trust a JSON parser at
load time. This test keeps that embedded copy honest:

  * every embedded entry matches assets/vfpu/PROVENANCE.json (the committed,
    human-auditable manifest);
  * the embedded SHA-256 matches the actual committed file bytes;
  * the genuine tables satisfy the value-domain invariants the C validators
    enforce (asin indices below the deltas entry count; sin interval-derived
    lo/hi within the exceptions allocation, inclusive upper bound);
  * the old trusted-by-size-only loader is gone from recomp.c and the
    defense-in-depth checked accesses are present;
  * the native selftest and Makefile target exist.
"""

import hashlib
import json
import re
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets" / "vfpu"
TABLES_C = ROOT / "src" / "rt" / "vfpu_tables.c"
RECOMP_C = ROOT / "src" / "rt" / "recomp.c"
SELFTEST_C = ROOT / "src" / "rt" / "vfpu_tables_selftest.c"
MAKEFILE = ROOT / "Makefile"

# Genuine sizes, cross-checked against the manifest and PROVENANCE.json.
ASIN_INDICES_ENTRIES = 798916 // 2
ASIN_DELTAS_ENTRIES = 517448 // 8
SIN_INTERVAL_ENTRIES = 131074 // 2
SIN_EXCEPTIONS_BYTES = 86938


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


TYPE_WIDTH = {
    "int8": 1, "uint8": 1, "int16": 2, "uint16": 2,
    "int32": 4, "uint32": 4, "int64": 8, "uint64": 8,
}


def type_width(table_type: str) -> int:
    base = re.match(r"(u?int(?:8|16|32|64))(?:\[|$)", table_type)
    assert base, f"unparseable table type {table_type!r}"
    return TYPE_WIDTH[base.group(1)]


def parse_embedded_manifest() -> dict[str, dict]:
    """Parse the SrVfpuTableSpec array out of vfpu_tables.c."""
    source = TABLES_C.read_text(encoding="utf-8")
    body = source[source.index("static const SrVfpuTableSpec SR_VFPU_TABLES[]"):]
    body = body[: body.index("};")]
    manifest: dict[str, dict] = {}
    for match in re.finditer(
        r'\{\s*"([^"]+)"\s*,\s*(\d+)u?\s*,\s*(\d+)u?\s*,\s*"([0-9a-f]{64})"\s*\}',
        body,
    ):
        name, bytes_, elem, digest = match.groups()
        manifest[name] = {
            "bytes": int(bytes_),
            "elem": int(elem),
            "sha256": digest,
        }
    return manifest


class VfpuTableManifestTests(unittest.TestCase):
    maxDiff = None

    def test_embedded_manifest_matches_provenance_json(self):
        prov = json.loads((ASSETS / "PROVENANCE.json").read_text(encoding="utf-8"))
        embedded = parse_embedded_manifest()

        self.assertEqual(
            len(embedded),
            len(prov["files"]),
            "embedded manifest entry count must match PROVENANCE.json",
        )
        self.assertEqual(
            prov.get("endianness"),
            "little-endian",
            "PROVENANCE.json must declare the table endianness",
        )
        for entry in prov["files"]:
            name = entry["path"]
            self.assertIn(name, embedded, f"{name} missing from embedded manifest")
            self.assertEqual(
                embedded[name]["bytes"],
                entry["bytes"],
                f"{name}: embedded byte length differs from PROVENANCE.json",
            )
            self.assertIn(
                "sha256", entry, f"{name}: PROVENANCE.json missing sha256 field"
            )
            self.assertEqual(
                embedded[name]["sha256"],
                entry["sha256"],
                f"{name}: embedded sha256 differs from PROVENANCE.json",
            )
            self.assertIn(
                "type", entry, f"{name}: PROVENANCE.json missing element type"
            )
            self.assertEqual(
                embedded[name]["elem"],
                type_width(entry["type"]),
                f"{name}: embedded element width differs from declared type "
                f"{entry['type']}",
            )

    def test_embedded_sha256_matches_committed_bytes(self):
        embedded = parse_embedded_manifest()
        for name, spec in embedded.items():
            path = ASSETS / name
            self.assertTrue(path.is_file(), f"{name} is not a committed asset")
            data = path.read_bytes()
            self.assertEqual(
                len(data),
                spec["bytes"],
                f"{name}: on-disk size differs from embedded manifest",
            )
            self.assertEqual(
                sha256_bytes(data),
                spec["sha256"],
                f"{name}: on-disk bytes do not match the embedded SHA-256",
            )
            self.assertEqual(spec["bytes"] % spec["elem"], 0)

    def test_genuine_asin_indices_respect_deltas_domain(self):
        data = (ASSETS / "vfpu_asin_lut_indices.dat").read_bytes()
        self.assertEqual(len(data), ASIN_INDICES_ENTRIES * 2)
        indices = struct.unpack("<" + "H" * ASIN_INDICES_ENTRIES, data)
        self.assertLess(max(indices), ASIN_DELTAS_ENTRIES)
        # The C validator rejects any index >= deltas entry count.
        self.assertEqual(len([i for i in indices if i >= ASIN_DELTAS_ENTRIES]), 0)

    def test_genuine_sin_intervals_stay_within_exceptions_allocation(self):
        data = (ASSETS / "vfpu_sin_lut_interval_delta.dat").read_bytes()
        self.assertEqual(len(data), SIN_INTERVAL_ENTRIES * 2)
        delta = struct.unpack("<" + "h" * SIN_INTERVAL_ENTRIES, data)
        max_m = 0
        for k in range(len(delta) - 1):
            lo = ((169 * k) >> 7) + delta[k] + 16384
            hi = ((169 * (k + 1)) >> 7) + delta[k + 1] + 16384
            # The C validator's invariant: 0 <= lo <= hi <= exception_count
            # (inclusive upper bound; m = (lo+hi)/2 < hi keeps reads in bounds).
            self.assertLessEqual(lo, hi)
            self.assertLessEqual(hi, SIN_EXCEPTIONS_BYTES)
            if lo < hi:
                max_m = max(max_m, (lo + hi) // 2)
        self.assertLess(max_m, SIN_EXCEPTIONS_BYTES)

    def test_old_size_only_loader_removed_from_recomp_c(self):
        source = RECOMP_C.read_text(encoding="utf-8")
        self.assertNotIn("sr_load_raw", source)
        self.assertNotIn("static int sr_vfpu_loaded", source)
        self.assertIn('#include "vfpu_tables.h"', source)
        # Defense-in-depth checked access must exist in both hot paths.
        self.assertIn("SR_VFPU_ASIN_DELTAS_ENTRIES", source)
        self.assertIn("SR_VFPU_SIN_EXCEPTIONS_BYTES", source)
        # Little-endian host contract is enforced in the loader.
        loader = TABLES_C.read_text(encoding="utf-8")
        self.assertIn("little-endian host is required", loader)
        # Evidence must report whether PSP_VFPU_TABLES is REALLY set, so a
        # default run never masquerades as an override run (and vice versa).
        self.assertNotIn("env_root != NULL", loader)
        self.assertIn("sr_vfpu_evidence(effective, env_overridden)", loader)

    def test_selftest_and_makefile_target_present(self):
        self.assertTrue(SELFTEST_C.is_file(), "vfpu_tables_selftest.c missing")
        source = SELFTEST_C.read_text(encoding="utf-8")
        for marker in (
            "test_sha256_known_answers",
            "test_asin_index_validator",
            "test_sin_interval_validator",
            "test_loader_rejects_corrupt_roots",
            "test_concurrent_first_use",
        ):
            self.assertIn(marker, source, f"selftest missing {marker}")
        make = MAKEFILE.read_text(encoding="utf-8")
        self.assertIn("vfpu-tables-selftest:", make)
        self.assertIn("src/rt/vfpu_tables.c \\", make)  # in RT_SRCS
        manager = (ROOT / "hst_manager.ps1").read_text(encoding="utf-8")
        self.assertIn("vfpu-tables-selftest", manager)


if __name__ == "__main__":
    unittest.main()
