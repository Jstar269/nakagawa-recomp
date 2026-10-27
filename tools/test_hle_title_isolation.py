# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Title-isolation for compat overrides retired from generic hle.c (issue #98).

Generic PSP handlers must not call HST addresses, write HST latches or install
HST callback addresses when running a neutral/second title. The display bring-up,
runtime-sync and frame-latch groups were typed title configuration until #363 retired
them; this module proves the generic/title separation, that a manifest still naming one
of those groups fails with the named HLE_TITLE_ASSIST_RETIRED error, and the retired
libfont field migration.

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

# HST's 16 retired addresses (inventoried as retired title configuration, not generic)
HST_MIGRATED = {
    0x00000bcc, 0x0029a8bc, 0x0001dc00, 0x0031fcc0, 0x00311140, 0x002d0738,
    0x00333138, 0x002bdf38, 0x000823f0, 0x00082438, 0x00082474, 0x0008249c,
    0x000824c0, 0x000824e8, 0x002d132c, 0x00331b80,
}

# HST's runtime-sync wrapper pairs (mode, enter, leave) and frame-ready latch
HST_WRAPPERS = ((0, 0x000823f0, 0x00082438), (1, 0x00082474, 0x0008249c),
                (2, 0x000824c0, 0x000824e8))
HST_FRAME_LATCH = 0x00331b80

# The assists retired under #363, with the shapes they had while live. The values are
# synthetic: they show that a once-valid manifest is refused, not a malformed one.
RETIRED_ASSISTS = {
    "frame_ready_latch_addr": 0x08905004,
    "display_bringup": {
        "malloc_entry": 0x08901000,
        "vblank_device_init_entry": 0x08901010,
        "render_context_init_entry": 0x08901020,
        "render_context_magic_addr": 0x08902000,
        "render_table_ready_flag_addr": 0x08902004,
        "render_context_word_addr": 0x08902008,
    },
    "runtime_sync": {
        "config_base": 0x08903000,
        "sema_name_ptr": 0x08903020,
        "wrappers": [
            {"mode": 0, "enter": 0x08904000, "leave": 0x08904004},
            {"mode": 1, "enter": 0x08904010, "leave": 0x08904014},
            {"mode": 2, "enter": 0x08904020, "leave": 0x08904024},
        ],
    },
}

# Macro and validity-bit fragments the retired assists used to put in the generated
# header; neither the header nor the C that reads it may carry them any more.
RETIRED_TOKENS = ("FRAME_READY_LATCH", "FRAME_LATCH", "DISPLAY_BRINGUP", "RUNTIME_SYNC")


def retired_tokens_in(text: str) -> list[str]:
    return [token for token in RETIRED_TOKENS if token in text]


def synthetic_bindings(shift: int = 0) -> dict:
    """A positive configuration made of the bindings that remain (disjoint from HST).

    ``shift`` moves every address, which gives a second, distinct synthetic title.
    """
    return {
        "fallback_entry": 0x08910000 + shift,
        "worker_thread_entry": 0x08910010 + shift,
        "launcher_thread_entry": 0x08910020 + shift,
        "dispatch_aliases": [{"from": 0x08911000 + shift, "to": 0x08911010 + shift}],
        "callback_terminators": [
            {"sentinel": 0, "pc": 0x08912000 + shift, "ra": 0x08912010 + shift},
        ],
    }


def synthetic_addresses(bindings: dict) -> set[int]:
    """Every guest address a binding set names (a terminator sentinel is a value)."""
    found = {bindings[name] for name in title_manifest.RUNTIME_BINDING_FIELDS if name in bindings}
    for alias in bindings.get("dispatch_aliases", []):
        found |= {alias["from"], alias["to"]}
    for entry in bindings.get("callback_terminators", []):
        found |= {entry[field] for field in ("pc", "ra") if field in entry}
    return found


def synthetic_manifest_configured(shift: int = 0) -> dict:
    base = json.loads(FIXTURE_B.read_text(encoding="utf-8"))
    base["runtime_bindings"] = {"schema_version": 1, **synthetic_bindings(shift)}
    return base


def header_for(manifest: dict | None) -> str:
    return title_runtime_config.render_header(title_runtime_config.bindings_from_manifest(manifest))


class GenericIsolationTests(unittest.TestCase):
    """Generic build must not carry HST's compat behavior."""

    def test_generic_hle_has_no_migrated_addresses(self):
        found = set(extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8")))
        overlap = found & HST_MIGRATED
        self.assertEqual(overlap, set(),
                         f"generic hle.c still contains HST compat address(es): "
                         f"{sorted(hex(a) for a in overlap)}")

    def test_generic_runtime_sources_do_not_consume_retired_bindings(self):
        for relative in ("src/rt/hle.c", "src/rt/title_config.c", "src/rt/title_config.h"):
            with self.subTest(source=relative):
                source = (ROOT / relative).read_text(encoding="utf-8")
                self.assertEqual(retired_tokens_in(source), [],
                                 f"{relative} still reads a retired title binding")

    def test_generic_title_config_has_no_migrated_bindings(self):
        cfg = title_runtime_config.bindings_from_manifest(None)
        self.assertEqual(cfg["bindings"], {})
        header = title_runtime_config.render_header(cfg)
        self.assertIn("#define SR_TITLE_CONFIG_VALID (0u)", header)
        # Every binding that remains is emitted disabled; nothing retired is emitted.
        for name in title_runtime_config.FIELD_BITS:
            self.assertIn(f"#define SR_TITLE_CONFIG_{name.upper()} 0x00000000u", header)
        self.assertEqual(retired_tokens_in(header), [])

    def test_public_fixtures_remain_generic_for_migrated_groups(self):
        # Existing public fixtures predate #98; they do not configure the migrated
        # compat groups, so a build against them must stay generic for those.
        for path in (FIXTURE_A, FIXTURE_B):
            with self.subTest(fixture=path.name):
                manifest = json.loads(path.read_text(encoding="utf-8"))
                normalized = title_manifest.validate_manifest(manifest)
                bindings = normalized.get("runtime_bindings", {})
                for key in RETIRED_ASSISTS:
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


class RetiredAssistTests(unittest.TestCase):
    """The display bring-up, runtime-sync and frame-latch assists are retired (#363)."""

    def test_retired_fields_are_refused_with_the_named_error(self):
        self.assertEqual(set(RETIRED_ASSISTS), set(title_manifest.RETIRED_HLE_ASSIST_BINDINGS))
        for field, value in RETIRED_ASSISTS.items():
            with self.subTest(field=field):
                configured = synthetic_manifest_configured()
                configured["runtime_bindings"][field] = value
                declared = synthetic_manifest_configured()
                declared["required_runtime_bindings"] = [field]
                for manifest, path in ((configured, f"runtime_bindings.{field}"),
                                       (declared, "required_runtime_bindings[0]")):
                    with self.assertRaises(title_manifest.TitleManifestError) as cm:
                        title_manifest.validate_manifest(manifest)
                    self.assertIn(path, str(cm.exception))
                    self.assertIn(title_manifest.HLE_ASSIST_RETIRED_MESSAGE, str(cm.exception))

    def test_no_generated_header_carries_a_retired_macro_or_bit(self):
        for label, manifest in (
                ("generic", None),
                (FIXTURE_A.name, json.loads(FIXTURE_A.read_text(encoding="utf-8"))),
                (FIXTURE_B.name, json.loads(FIXTURE_B.read_text(encoding="utf-8"))),
                ("synthetic configured", synthetic_manifest_configured())):
            with self.subTest(config=label):
                self.assertEqual(retired_tokens_in(header_for(manifest)), [])


class PositiveConfiguredTests(unittest.TestCase):
    """A synthetic title that explicitly configures the compat must validate and emit."""

    def test_positive_synthetic_validates_and_emits(self):
        manifest = synthetic_manifest_configured()
        normalized = title_manifest.validate_manifest(manifest)
        for name in (*title_manifest.RUNTIME_BINDING_FIELDS,
                     *title_manifest.RUNTIME_BINDING_COLLECTIONS):
            self.assertIn(name, normalized["runtime_bindings"])
        header = header_for(manifest).lower()
        # Every synthetic address must appear in the header
        for addr in synthetic_addresses(synthetic_bindings()):
            self.assertIn(f"0x{addr:08x}", header)
        # HST's addresses must NOT appear
        for addr in HST_MIGRATED:
            self.assertNotIn(f"0x{addr:08x}", header)
        # Validity bits must be set for everything that was configured
        for bit in (*title_runtime_config.FIELD_BITS.values(),
                    *title_runtime_config.COLLECTION_BITS.values()):
            self.assertIn(bit.lower(), header)

    def test_two_configured_titles_have_disjoint_compat_addresses(self):
        # A second synthetic title, with every address shifted, must validate on its own
        # and share no address with the first or with HST.
        shifted = synthetic_manifest_configured(shift=0x100)
        title_manifest.validate_manifest(shifted)
        a = synthetic_addresses(synthetic_bindings())
        b = synthetic_addresses(synthetic_bindings(shift=0x100))
        self.assertEqual(a & b, set(), "two synthetic titles must have disjoint compat addresses")
        self.assertEqual(a & HST_MIGRATED, set())
        self.assertEqual(b & HST_MIGRATED, set())
        self.assertNotEqual(
            title_runtime_config.config_digest(
                title_runtime_config.bindings_from_manifest(synthetic_manifest_configured())),
            title_runtime_config.config_digest(title_runtime_config.bindings_from_manifest(shifted)))


class WrongTitleTests(unittest.TestCase):
    """Wrong-title safety: generic/synthetic must not inherit HST's mapping."""

    def headers(self) -> dict[str, str]:
        return {"generic": header_for(None), "synthetic": header_for(synthetic_manifest_configured())}

    def test_wrong_title_does_not_get_hst_wrappers(self):
        # No runtime-sync mechanism remains to install wrappers; neither a generic nor a
        # configured title's header may name HST's.
        wrapper_addresses = {a for _mode, enter, leave in HST_WRAPPERS for a in (enter, leave)}
        for label, header in self.headers().items():
            with self.subTest(config=label):
                for address in wrapper_addresses:
                    self.assertNotIn(f"{address:08x}", header.lower())
        found = set(extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8")))
        self.assertEqual(found & wrapper_addresses, set())

    def test_wrong_title_does_not_write_hst_latches(self):
        # There is no latch binding to configure and no assist to write one
        for label, header in self.headers().items():
            with self.subTest(config=label):
                self.assertNotIn(f"{HST_FRAME_LATCH:08x}", header.lower())
                self.assertNotIn("FRAME_READY_LATCH_ADDR", header)
                self.assertNotIn("LIBFONT_READY_FLAG_ADDR", header)
        found = set(extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8")))
        self.assertNotIn(HST_FRAME_LATCH, found)


class InvalidAddressTests(unittest.TestCase):
    """Malformed compat addresses must fail closed."""

    def test_unaligned_and_zero_rejected(self):
        for bad in (0, 0x08901001, 0x08902002):
            with self.subTest(bad=hex(bad) if bad else bad):
                m = synthetic_manifest_configured()
                m["runtime_bindings"]["fallback_entry"] = bad
                with self.assertRaises(title_manifest.TitleManifestError):
                    title_manifest.validate_manifest(m)

    def test_legacy_libfont_ready_binding_is_rejected_by_name(self):
        manifest = synthetic_manifest_configured()
        manifest["runtime_bindings"]["libfont_ready_flag_addr"] = 0x08905000
        with self.assertRaisesRegex(
            title_manifest.TitleManifestError,
            "LIBFONT_READY_FLAG_RETIRED: host readiness injection is not supported yet",
        ):
            title_manifest.validate_manifest(manifest)

    def test_core_reserved_dispatch_targets_rejected(self):
        reserved = 0x40000000  # inside VFPU window
        m = synthetic_manifest_configured()
        m["runtime_bindings"]["dispatch_aliases"][0]["from"] = reserved
        with self.assertRaises(title_manifest.TitleManifestError) as cm:
            title_manifest.validate_manifest(m)
        self.assertIn("core VFPU", str(cm.exception))
        m = synthetic_manifest_configured()
        m["runtime_bindings"]["callback_terminators"][0]["sentinel"] = reserved
        with self.assertRaises(title_manifest.TitleManifestError) as cm:
            title_manifest.validate_manifest(m)
        self.assertIn("core VFPU", str(cm.exception))


class DisabledProfileTests(unittest.TestCase):
    def test_enabling_one_compat_does_not_enable_others(self):
        for kept in title_manifest.RUNTIME_BINDING_FIELDS:
            with self.subTest(kept=kept):
                m = synthetic_manifest_configured()
                bindings = m["runtime_bindings"]
                for name in title_manifest.RUNTIME_BINDING_FIELDS:
                    if name != kept:
                        del bindings[name]
                normalized = title_manifest.validate_manifest(m)
                configured = set(normalized["runtime_bindings"]) & set(
                    title_manifest.RUNTIME_BINDING_FIELDS)
                self.assertEqual(configured, {kept})
                header = header_for(m)
                for name, bit in title_runtime_config.FIELD_BITS.items():
                    macro = f"#define SR_TITLE_CONFIG_{name.upper()} 0x00000000u"
                    if name == kept:
                        self.assertIn(bit, header)
                        self.assertNotIn(macro, header)
                    else:
                        self.assertNotIn(bit, header)
                        self.assertIn(macro, header)


class MutationTests(unittest.TestCase):
    """Mutations must be caught."""

    def test_new_hardcoded_hst_address_in_hle_fails(self):
        # Simulate a developer re-adding a hardcoded HST address to hle.c
        mutated = HLE_C.read_text(encoding="utf-8") + "\n    MEM_W32(0x002d132cu, 1u);\n"
        found = extract_hle_guest_addresses(mutated)
        self.assertIn(0x002d132c, found)
        retired = {a for group in compat_overrides.RETIRED_HLE_DIAGNOSTIC_GROUPS
                   for a in group["addresses"]}
        configured = {a for group in compat_overrides.HLE_TITLE_CONFIGURED_COMPAT
                      for a in group["addresses"]}
        missing = set(found) & (retired | configured)
        self.assertIn(0x002d132c, missing)

    def test_removing_inventory_entry_fails(self):
        retired = {a for group in compat_overrides.RETIRED_HLE_DIAGNOSTIC_GROUPS
                   for a in group["addresses"]}
        thinned = retired - {0x0030a000}
        found = set(extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8")))
        self.assertEqual(found & thinned, set())
        self.assertNotIn(0x0030a000, found)

    def test_title_configured_inventory_is_not_live(self):
        live = {a for group in compat_overrides.HLE_GUEST_ADDRESS_GROUPS for a in group["addresses"]}
        configured = {a for group in compat_overrides.HLE_TITLE_CONFIGURED_COMPAT for a in group["addresses"]}
        self.assertEqual(live & configured, set(),
                         "live and title-configured inventories must be disjoint")
        self.assertEqual(configured, HST_MIGRATED)

    def test_hst_reent_addresses_live_only_in_title_configuration(self):
        addresses = {int(item["address"]) for item in
                     compat_overrides.TITLE_CONFIGURED_SCHEDULER_COMPAT}
        self.assertEqual(addresses, {0x002cf338, 0x0030aa88})
        title_config = (ROOT / "src" / "rt" / "title_config.c").read_text(encoding="utf-8")
        self.assertTrue(all(f"0x{address:08x}u" in title_config for address in addresses))
        for relative in ("src/rt/hle.c", "src/rt/sched.c", "src/rt/recomp.c"):
            source = (ROOT / relative).read_text(encoding="utf-8").lower()
            for address in addresses:
                self.assertNotIn(f"0x{address:08x}", source,
                                 f"{relative} contains HST-only reent address {address:#x}")


if __name__ == "__main__":
    unittest.main()
