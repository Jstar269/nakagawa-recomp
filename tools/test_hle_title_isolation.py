# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Title-isolation for compat overrides retired from generic hle.c (issue #98).

Generic PSP handlers must not call HST addresses, write HST latches or install
HST callback addresses when running a neutral/second title. The four EXPLICIT
groups (display_setmode, runtime_sync, libfont, frame latch) are now typed
title configuration; this module proves the generic/title separation.

Evidence tiers used here are SOURCE_SHAPE (text/manifest/header shape) and
PRODUCTION_DISPATCH-level via generated-header assertions. No private HST
bytes are read.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0, str(ROOT / "tools"))

import title_manifest
import title_runtime_config
import compat_overrides
from test_compat_manifest import extract_hle_guest_addresses, HLE_C

FIXTURE_A = ROOT / "assets" / "titles" / "pspdev-phase5.json"
FIXTURE_B = ROOT / "assets" / "titles" / "synthetic.json"

# HST's 16 retired addresses (now title-configured, not generic)
HST_MIGRATED = {
    0x00000bcc, 0x0029a8bc, 0x0001dc00, 0x0031fcc0, 0x00311140, 0x002d0738,
    0x00333138, 0x002bdf38, 0x000823f0, 0x00082438, 0x00082474, 0x0008249c,
    0x000824c0, 0x000824e8, 0x002d132c, 0x00331b80,
}

# Synthetic positive-config addresses (disjoint from HST and from each other)
SYNTH_DISPLAY = {
    "malloc_entry": 0x08901000,
    "vblank_device_init_entry": 0x08901010,
    "render_context_init_entry": 0x08901020,
    "render_context_magic_addr": 0x08902000,
    "render_table_ready_flag_addr": 0x08902004,
    "render_context_word_addr": 0x08902008,
}
SYNTH_RUNTIME_SYNC = {
    "config_base": 0x08903000,
    "sema_name_ptr": 0x08903020,
    "wrappers": [
        {"mode": 0, "enter": 0x08904000, "leave": 0x08904004},
        {"mode": 1, "enter": 0x08904010, "leave": 0x08904014},
        {"mode": 2, "enter": 0x08904020, "leave": 0x08904024},
    ],
}
SYNTH_LIBFONT = 0x08905000
SYNTH_FRAME = 0x08905004


def synthetic_manifest_with_migrated() -> dict:
    import copy
    base = json.loads(FIXTURE_B.read_text(encoding="utf-8"))
    base["runtime_bindings"].update({
        "display_bringup": dict(SYNTH_DISPLAY),
        "runtime_sync": copy.deepcopy(SYNTH_RUNTIME_SYNC),
        "libfont_ready_flag_addr": SYNTH_LIBFONT,
        "frame_ready_latch_addr": SYNTH_FRAME,
    })
    return base


class GenericIsolationTests(unittest.TestCase):
    """Generic build must not carry HST's compat behavior."""

    def test_generic_hle_has_no_migrated_addresses(self):
        found = set(extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8")))
        overlap = found & HST_MIGRATED
        self.assertEqual(overlap, set(),
                         f"generic hle.c still contains HST compat address(es): "
                         f"{sorted(hex(a) for a in overlap)}")

    def test_generic_title_config_has_no_migrated_bindings(self):
        cfg = title_runtime_config.bindings_from_manifest(None)
        self.assertEqual(cfg["bindings"], {})
        header = title_runtime_config.render_header(cfg)
        self.assertIn("#define SR_TITLE_CONFIG_VALID (0u)", header)
        self.assertIn("#define SR_TITLE_CONFIG_DISPLAY_BRINGUP_MALLOC_ENTRY 0x00000000u", header)
        self.assertIn("#define SR_TITLE_CONFIG_RUNTIME_SYNC_WRAPPER_COUNT 0", header)
        self.assertIn("#define SR_TITLE_CONFIG_LIBFONT_READY_FLAG_ADDR 0x00000000u", header)
        self.assertIn("#define SR_TITLE_CONFIG_FRAME_READY_LATCH_ADDR 0x00000000u", header)

    def test_public_fixtures_remain_generic_for_migrated_groups(self):
        # Existing public fixtures predate #98; they do not configure the migrated
        # compat groups, so a build against them must stay generic for those.
        for path in (FIXTURE_A, FIXTURE_B):
            with self.subTest(fixture=path.name):
                manifest = json.loads(path.read_text(encoding="utf-8"))
                normalized = title_manifest.validate_manifest(manifest)
                bindings = normalized.get("runtime_bindings", {})
                for key in ("display_bringup", "runtime_sync",
                            "libfont_ready_flag_addr", "frame_ready_latch_addr"):
                    self.assertNotIn(key, bindings,
                                     f"{path.name} unexpectedly configures {key}")

    def test_migrated_addresses_are_absent_from_fixture_headers(self):
        # The headers emitted for the two public fixtures must not mention HST's
        # addresses (they use disjoint synthetic ones or none at all).
        hst_hex = {f"{a:08x}" for a in HST_MIGRATED}
        for path in (FIXTURE_A, FIXTURE_B):
            manifest = json.loads(path.read_text(encoding="utf-8"))
            cfg = title_runtime_config.bindings_from_manifest(manifest)
            header = title_runtime_config.render_header(cfg).lower()
            for hx in hst_hex:
                self.assertNotIn(hx, header,
                                 f"{path.name} header contains HST address {hx}")


class PositiveConfiguredTests(unittest.TestCase):
    """A synthetic title that explicitly configures the compat must validate and emit."""

    def test_positive_synthetic_validates_and_emits(self):
        manifest = synthetic_manifest_with_migrated()
        normalized = title_manifest.validate_manifest(manifest)
        self.assertIn("display_bringup", normalized["runtime_bindings"])
        self.assertIn("runtime_sync", normalized["runtime_bindings"])
        cfg = title_runtime_config.bindings_from_manifest(manifest)
        header = title_runtime_config.render_header(cfg)
        # Every synthetic address must appear in the header
        for addr in list(SYNTH_DISPLAY.values()) + [SYNTH_LIBFONT, SYNTH_FRAME,
                    SYNTH_RUNTIME_SYNC["config_base"], SYNTH_RUNTIME_SYNC["sema_name_ptr"]] + \
                    [w["enter"] for w in SYNTH_RUNTIME_SYNC["wrappers"]] + \
                    [w["leave"] for w in SYNTH_RUNTIME_SYNC["wrappers"]]:
            self.assertIn(f"0x{addr:08x}", header.lower())
        # HST's addresses must NOT appear
        for addr in HST_MIGRATED:
            self.assertNotIn(f"0x{addr:08x}", header.lower())
        # Validity bits must be set
        self.assertIn("SR_TITLE_CFG_DISPLAY_BRINGUP", header)
        self.assertIn("SR_TITLE_CFG_RUNTIME_SYNC", header)
        self.assertIn("SR_TITLE_CFG_LIBFONT_READY", header)
        self.assertIn("SR_TITLE_CFG_FRAME_LATCH", header)

    def test_two_configured_titles_have_disjoint_compat_addresses(self):
        # Synthetic vs HST must be disjoint; also synthetic vs itself with altered addrs
        manifest2 = synthetic_manifest_with_migrated()
        # Shift every synth address by +0x100 to get a second distinct title
        shifted = json.loads(json.dumps(manifest2))
        for key in SYNTH_DISPLAY:
            shifted["runtime_bindings"]["display_bringup"][key] += 0x100
        shifted["runtime_bindings"]["runtime_sync"]["config_base"] += 0x100
        shifted["runtime_bindings"]["runtime_sync"]["sema_name_ptr"] += 0x100
        for w in shifted["runtime_bindings"]["runtime_sync"]["wrappers"]:
            w["enter"] += 0x100
            w["leave"] += 0x100
        shifted["runtime_bindings"]["libfont_ready_flag_addr"] += 0x100
        shifted["runtime_bindings"]["frame_ready_latch_addr"] += 0x100
        a = set(SYNTH_DISPLAY.values()) | {SYNTH_LIBFONT, SYNTH_FRAME,
               SYNTH_RUNTIME_SYNC["config_base"], SYNTH_RUNTIME_SYNC["sema_name_ptr"]} | \
            {w["enter"] for w in SYNTH_RUNTIME_SYNC["wrappers"]} | \
            {w["leave"] for w in SYNTH_RUNTIME_SYNC["wrappers"]}
        b = set(shifted["runtime_bindings"]["display_bringup"].values()) | \
            {shifted["runtime_bindings"]["libfont_ready_flag_addr"],
             shifted["runtime_bindings"]["frame_ready_latch_addr"],
             shifted["runtime_bindings"]["runtime_sync"]["config_base"],
             shifted["runtime_bindings"]["runtime_sync"]["sema_name_ptr"]} | \
            {w["enter"] for w in shifted["runtime_bindings"]["runtime_sync"]["wrappers"]} | \
            {w["leave"] for w in shifted["runtime_bindings"]["runtime_sync"]["wrappers"]}
        self.assertEqual(a & b, set(), "two synthetic titles must have disjoint compat addresses")
        self.assertEqual(a & HST_MIGRATED, set())
        self.assertEqual(b & HST_MIGRATED, set())


class WrongTitleTests(unittest.TestCase):
    """Wrong-title safety: generic/synthetic must not inherit HST's mapping."""

    def test_wrong_title_does_not_get_hst_wrappers(self):
        # Generic config has no wrappers
        generic = title_runtime_config.bindings_from_manifest(None)
        self.assertNotIn("runtime_sync", generic["bindings"])
        # Synthetic configured title has synthetic wrappers only
        synth = synthetic_manifest_with_migrated()
        cfg = title_runtime_config.bindings_from_manifest(synth)
        wrappers = {(w["mode"], w["enter"], w["leave"]) for w in cfg["bindings"]["runtime_sync"]["wrappers"]}
        hst_wrappers = {(0, 0x000823f0, 0x00082438), (1, 0x00082474, 0x0008249c), (2, 0x000824c0, 0x000824e8)}
        self.assertEqual(wrappers & hst_wrappers, set(),
                         "synthetic title must not contain HST's wrapper addresses")

    def test_wrong_title_does_not_write_hst_latches(self):
        # The latch addresses are distinct per title
        self.assertNotEqual(SYNTH_FRAME, 0x00331b80)
        self.assertNotEqual(SYNTH_LIBFONT, 0x002d132c)
        # And generic has no latch to write at all
        hdr = title_runtime_config.render_header(title_runtime_config.bindings_from_manifest(None))
        self.assertIn("FRAME_READY_LATCH_ADDR 0x00000000u", hdr)
        self.assertIn("LIBFONT_READY_FLAG_ADDR 0x00000000u", hdr)


class InvalidAddressTests(unittest.TestCase):
    """Malformed compat addresses must fail closed."""

    def test_unaligned_and_zero_rejected(self):
        for bad in (0, 0x08901001, 0x08902002):
            with self.subTest(bad=hex(bad) if bad else bad):
                m = synthetic_manifest_with_migrated()
                m["runtime_bindings"]["libfont_ready_flag_addr"] = bad
                with self.assertRaises(title_manifest.TitleManifestError):
                    title_manifest.validate_manifest(m)

    def test_core_reserved_dispatch_targets_rejected(self):
        reserved = 0x40000000  # inside VFPU window
        m = synthetic_manifest_with_migrated()
        m["runtime_bindings"]["display_bringup"]["malloc_entry"] = reserved
        with self.assertRaises(title_manifest.TitleManifestError) as cm:
            title_manifest.validate_manifest(m)
        self.assertIn("core VFPU", str(cm.exception))
        m = synthetic_manifest_with_migrated()
        m["runtime_bindings"]["runtime_sync"]["wrappers"][0]["enter"] = reserved
        with self.assertRaises(title_manifest.TitleManifestError):
            title_manifest.validate_manifest(m)

    def test_duplicate_mode_rejected(self):
        m = synthetic_manifest_with_migrated()
        # Replace wrappers with a duplicate-mode set within max (2 entries, both mode 0)
        m["runtime_bindings"]["runtime_sync"]["wrappers"] = [
            {"mode": 0, "enter": 0x08904000, "leave": 0x08904004},
            {"mode": 0, "enter": 0x08904010, "leave": 0x08904014},
        ]
        with self.assertRaises(title_manifest.TitleManifestError) as cm:
            title_manifest.validate_manifest(m)
        self.assertIn("duplicate mode", str(cm.exception).lower())

    def test_empty_wrappers_rejected(self):
        m = synthetic_manifest_with_migrated()
        m["runtime_bindings"]["runtime_sync"]["wrappers"] = []
        with self.assertRaises(title_manifest.TitleManifestError):
            title_manifest.validate_manifest(m)


class DisabledProfileTests(unittest.TestCase):
    def test_disabled_display_does_no_guest_calls(self):
        # Manifest without display_bringup => header has no bringup, accessor will be 0
        m = json.loads(FIXTURE_B.read_text(encoding="utf-8"))
        cfg = title_runtime_config.bindings_from_manifest(m)
        self.assertNotIn("display_bringup", cfg["bindings"])
        hdr = title_runtime_config.render_header(cfg)
        self.assertIn("SR_TITLE_CONFIG_VALID (", hdr)
        self.assertNotIn("SR_TITLE_CFG_DISPLAY_BRINGUP", hdr)

    def test_disabled_runtime_sync_installs_nothing(self):
        m = json.loads(FIXTURE_B.read_text(encoding="utf-8"))
        cfg = title_runtime_config.bindings_from_manifest(m)
        self.assertNotIn("runtime_sync", cfg["bindings"])

    def test_enabling_one_compat_does_not_enable_others(self):
        m = synthetic_manifest_with_migrated()
        # Remove frame latch, keep others
        del m["runtime_bindings"]["frame_ready_latch_addr"]
        normalized = title_manifest.validate_manifest(m)
        self.assertIn("display_bringup", normalized["runtime_bindings"])
        self.assertNotIn("frame_ready_latch_addr", normalized["runtime_bindings"])


class MutationTests(unittest.TestCase):
    """Mutations must be caught."""

    def test_new_hardcoded_hst_address_in_hle_fails(self):
        # Simulate a developer re-adding a hardcoded HST address to hle.c
        mutated = HLE_C.read_text(encoding="utf-8") + "\n    MEM_W32(0x002d132cu, 1u);\n"
        found = extract_hle_guest_addresses(mutated)
        self.assertIn(0x002d132c, found)
        missing = set(found) - {a for group in compat_overrides.HLE_GUEST_ADDRESS_GROUPS for a in group["addresses"]}
        # The newly added address is not in the live inventory, so it must be missing
        self.assertIn(0x002d132c, missing)

    def test_removing_inventory_entry_fails(self):
        live = {a for group in compat_overrides.HLE_GUEST_ADDRESS_GROUPS for a in group["addresses"]}
        # Drop one diagnostic address from the live inventory; hle.c still has it
        thinned = live - {0x0030a000}
        found = set(extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8")))
        self.assertEqual(found - thinned, {0x0030a000})

    def test_title_configured_inventory_is_not_live(self):
        live = {a for group in compat_overrides.HLE_GUEST_ADDRESS_GROUPS for a in group["addresses"]}
        configured = {a for group in compat_overrides.HLE_TITLE_CONFIGURED_COMPAT for a in group["addresses"]}
        self.assertEqual(live & configured, set(),
                         "live and title-configured inventories must be disjoint")
        self.assertEqual(configured, HST_MIGRATED)


if __name__ == "__main__":
    unittest.main()
