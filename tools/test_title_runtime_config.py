#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for the runtime title configuration path.

Two separable claims are covered here:

* the manifest's ``runtime_bindings`` block fails closed on every malformed shape, and
* the generic runtime source no longer contains the guest addresses it used to hardcode.

The second is deliberately a source-shape (tier-4) check. The executable proof that a
binding acts only where it is configured lives in the three-configuration
``make sched-selftest`` matrix, which builds the same scheduler source against a
generic, a fixture-A, and a fixture-B configuration.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import compat_overrides  # noqa: E402
import title_manifest  # noqa: E402
import title_runtime_config  # noqa: E402

FIXTURE_A = ROOT / "assets" / "titles" / "pspdev-phase5.json"
FIXTURE_B = ROOT / "assets" / "titles" / "synthetic.json"

#: Guest addresses the generic runtime hardcoded before this configuration path existed.
#: They are title data and must not reappear in generic runtime sources.
RETIRED_RUNTIME_ADDRESSES = {
    "fallback entry": ("0x0029a060", "0x29a060"),
    "worker thread entry": ("0x000468c8", "0x468c8"),
    "launcher thread entry": ("0x0029a174", "0x29a174"),
    "vblank frame counter": ("0x0031101c", "0x31101c"),
    "vblank vsync counter": ("0x0031105c", "0x31105c"),
}

#: Guest addresses that generic DISPATCH hardcoded before the typed collections existed.
#: Migrated 2026-08-21 into runtime_bindings.dispatch_aliases / .callback_terminators.
RETIRED_DISPATCH_ADDRESSES = {
    "dispatch alias source": ("0x00030950", "0x30950"),
    "dispatch alias target": ("0x00030948", "0x30948"),
    "null callback terminator ra": ("0x0003e06c", "0x3e06c"),
    "minus-one terminator pc": ("0x00292fa0", "0x292fa0"),
    "minus-one terminator ra": ("0x00047a0c", "0x47a0c"),
}

#: Generic runtime sources that must no longer name any of the addresses above.
#:
#: src/rt/dispatch_isolation_selftest.c is deliberately NOT here: it is the test that
#: proves those addresses are inert, and it can only state that claim by naming them.
#: DispatchAddressCensus asserts both halves -- absent from the sources below, and still
#: present in the selftest that proves inertness.
GENERIC_RUNTIME_SOURCES = ("src/rt/driver.c", "src/rt/sched.c", "src/rt/recomp.c",
                           "src/rt/title_config.c", "src/rt/title_config.h")


def base_manifest(**bindings: int) -> dict:
    manifest = json.loads(FIXTURE_A.read_text(encoding="utf-8"))
    if bindings:
        manifest["runtime_bindings"] = {"schema_version": 1, **bindings}
    else:
        manifest.pop("runtime_bindings", None)
    return manifest


class RuntimeBindingValidation(unittest.TestCase):
    def assert_rejected(self, manifest: dict, fragment: str) -> None:
        with self.assertRaises(title_manifest.TitleManifestError) as caught:
            title_manifest.validate_manifest(manifest)
        self.assertIn(fragment, str(caught.exception))

    def test_public_fixtures_validate_and_carry_distinct_bindings(self) -> None:
        a = title_manifest.validate_manifest(json.loads(FIXTURE_A.read_text(encoding="utf-8")))
        b = title_manifest.validate_manifest(json.loads(FIXTURE_B.read_text(encoding="utf-8")))
        left = a["runtime_bindings"]
        right = b["runtime_bindings"]
        self.assertEqual(set(left), set(right))
        for field in title_manifest.RUNTIME_BINDING_FIELDS:
            self.assertNotEqual(
                left[field], right[field],
                f"the two public fixtures must not share {field}; multi-title behavior "
                "cannot be distinguished from a single parameterization otherwise",
            )

    def test_public_fixtures_use_no_retired_address(self) -> None:
        """A public fixture that reused a retired address would make every isolation
        assertion ambiguous: a match could mean "configured correctly" or "the old
        hardcoded value survived". Covers the scalar fields AND both collections."""
        retired = {int(spellings[0], 16)
                   for spellings in (*RETIRED_RUNTIME_ADDRESSES.values(),
                                     *RETIRED_DISPATCH_ADDRESSES.values())}
        for fixture in (FIXTURE_A, FIXTURE_B):
            bindings = title_manifest.validate_manifest(
                json.loads(fixture.read_text(encoding="utf-8"))
            )["runtime_bindings"]
            used = {value for value in bindings.values() if isinstance(value, int)}
            for alias in bindings.get("dispatch_aliases", []):
                used |= {alias["from"], alias["to"]}
            for entry in bindings.get("callback_terminators", []):
                # The SENTINEL is deliberately excluded: 0 and 0xFFFFFFFF are generic
                # guest vocabulary that every title shares. Only the site is an address.
                used |= {entry[field] for field in ("pc", "ra") if field in entry}
            overlap = used & retired
            self.assertEqual(
                overlap, set(),
                f"{fixture.name} reuses retired address(es): "
                f"{sorted(hex(a) for a in overlap)}",
            )

    def test_manifest_without_the_block_is_valid_and_configures_nothing(self) -> None:
        normalized = title_manifest.validate_manifest(base_manifest())
        self.assertNotIn("runtime_bindings", normalized)
        config = title_runtime_config.bindings_from_manifest(normalized)
        self.assertEqual(config["bindings"], {})

    def test_unknown_field_is_rejected(self) -> None:
        manifest = base_manifest(worker_thread_entry=0x08804200)
        manifest["runtime_bindings"]["turbo_mode"] = True
        self.assert_rejected(manifest, "unknown field(s): turbo_mode")

    def test_missing_or_wrong_schema_version_is_rejected(self) -> None:
        manifest = base_manifest(worker_thread_entry=0x08804200)
        del manifest["runtime_bindings"]["schema_version"]
        self.assert_rejected(manifest, "missing required field(s): schema_version")
        manifest = base_manifest(worker_thread_entry=0x08804200)
        manifest["runtime_bindings"]["schema_version"] = 2
        self.assert_rejected(manifest, "only runtime-binding schema version 1 is supported")

    def test_empty_block_is_rejected(self) -> None:
        manifest = base_manifest(worker_thread_entry=0x08804200)
        manifest["runtime_bindings"] = {"schema_version": 1}
        self.assert_rejected(manifest, "must configure at least one binding")

    def test_malformed_addresses_are_rejected(self) -> None:
        for value, fragment in (
            ("0x08804200", "must be an integer"),
            (True, "must be an integer"),
            (0x08804201, "must be 4-byte aligned"),
            (0, "must not be zero"),
            (-4, "must be in range"),
            (0x1_0000_0000, "must be in range"),
        ):
            with self.subTest(value=value):
                self.assert_rejected(base_manifest(worker_thread_entry=value), fragment)

    def test_partially_specified_vblank_pair_is_rejected(self) -> None:
        self.assert_rejected(
            base_manifest(vblank_frame_counter_addr=0x08820000),
            "is paired with vblank_vsync_counter_addr",
        )
        self.assert_rejected(
            base_manifest(vblank_vsync_counter_addr=0x08820004),
            "is paired with vblank_frame_counter_addr",
        )

    def test_identical_vblank_pair_is_rejected(self) -> None:
        self.assert_rejected(
            base_manifest(
                vblank_frame_counter_addr=0x08820000,
                vblank_vsync_counter_addr=0x08820000,
            ),
            "must be distinct addresses",
        )

    def test_identical_worker_and_launcher_roles_are_rejected(self) -> None:
        self.assert_rejected(
            base_manifest(
                worker_thread_entry=0x08804200,
                launcher_thread_entry=0x08804200,
            ),
            "must be distinct roles",
        )

    def test_a_single_binding_is_accepted(self) -> None:
        normalized = title_manifest.validate_manifest(base_manifest(fallback_entry=0x08804100))
        self.assertEqual(
            normalized["runtime_bindings"],
            {"schema_version": 1, "fallback_entry": 0x08804100},
        )

    def test_json_schema_publishes_the_same_binding_vocabulary(self) -> None:
        schema = json.loads(
            (ROOT / "assets" / "title_manifest.schema.json").read_text(encoding="utf-8")
        )
        block = schema["$defs"]["runtimeBindings"]
        self.assertFalse(block["additionalProperties"])
        published = set(block["properties"]) - {"schema_version"}
        self.assertEqual(
            published,
            set(title_manifest.RUNTIME_BINDING_FIELDS)
            | set(title_manifest.RUNTIME_BINDING_COLLECTIONS),
            "the schema binding vocabulary drifted from the Python validator",
        )
        self.assertIn("runtime_bindings", schema["properties"])

    def test_json_schema_publishes_the_collection_shapes(self) -> None:
        schema = json.loads(
            (ROOT / "assets" / "title_manifest.schema.json").read_text(encoding="utf-8")
        )
        defs = schema["$defs"]
        block = schema["$defs"]["runtimeBindings"]["properties"]
        for field, ceiling in (
            ("dispatch_aliases", title_manifest.MAX_DISPATCH_ALIASES),
            ("callback_terminators", title_manifest.MAX_CALLBACK_TERMINATORS),
        ):
            with self.subTest(field=field):
                published = block[field]
                self.assertEqual(published["type"], "array")
                self.assertEqual(published["minItems"], 1, "an empty collection is rejected")
                self.assertEqual(
                    published["maxItems"], ceiling,
                    "the published ceiling drifted from the Python validator",
                )
        alias = defs["dispatchAlias"]
        self.assertFalse(alias["additionalProperties"])
        self.assertEqual(set(alias["required"]), {"from", "to"})
        terminator = defs["callbackTerminator"]
        self.assertFalse(terminator["additionalProperties"])
        self.assertEqual(terminator["required"], ["sentinel"])
        # A sentinel is a raw target value: 0 and 0xFFFFFFFF are the real ones, so it must
        # NOT be published as a guestAddress (which forbids zero and demands alignment).
        sentinel_range = terminator["properties"]["sentinel"]["allOf"][0]
        self.assertEqual(sentinel_range["minimum"], 0)
        self.assertEqual(sentinel_range["maximum"], 0xFFFFFFFF)
        self.assertNotIn("$ref", json.dumps(sentinel_range),
                         "a sentinel must not be published as a guestAddress")
        # At least one context constraint, or the sentinel would terminate everywhere.
        self.assertEqual(
            terminator["anyOf"], [{"required": ["pc"]}, {"required": ["ra"]}],
            "the schema must publish the at-least-one-of-pc/ra rule",
        )
        # The core dispatch reservation, published with the same bounds the validator
        # enforces, and applied to exactly the two fields compared against a target.
        reserved = defs["coreReservedDispatchTarget"]
        self.assertEqual(reserved["minimum"], title_manifest.SR_DISPATCH_VFPU_TAG)
        self.assertEqual(
            reserved["maximum"],
            title_manifest.SR_DISPATCH_VFPU_TAG | (~title_manifest.SR_DISPATCH_VFPU_MASK
                                                   & 0xFFFFFFFF),
        )
        excluded = {"not": {"$ref": "#/$defs/coreReservedDispatchTarget"}}
        self.assertIn(excluded, alias["properties"]["from"]["allOf"],
                      "an alias source must publish the core reservation")
        self.assertIn(excluded, terminator["properties"]["sentinel"]["allOf"],
                      "a sentinel must publish the core reservation")
        self.assertNotIn("allOf", alias["properties"]["to"],
                         "an alias destination is not a dispatch target and must stay free")

    def test_the_core_reservation_mirrors_the_runtime_header(self) -> None:
        """The reservation only means anything if it names the same window dispatch()
        actually claims. src/rt/recomp.h is the authority; this reads the two macros
        back out of it so the Python mirror cannot drift from the C."""
        header = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")
        found = dict(re.findall(
            r"^#define\s+(SR_DISPATCH_VFPU_(?:TAG|MASK))\s+0x([0-9A-Fa-f]+)u?\s*$",
            header, re.MULTILINE))
        self.assertEqual(set(found), {"SR_DISPATCH_VFPU_TAG", "SR_DISPATCH_VFPU_MASK"},
                         "src/rt/recomp.h no longer defines the dispatch VFPU macros the "
                         "manifest validator mirrors")
        self.assertEqual(int(found["SR_DISPATCH_VFPU_TAG"], 16),
                         title_manifest.SR_DISPATCH_VFPU_TAG)
        self.assertEqual(int(found["SR_DISPATCH_VFPU_MASK"], 16),
                         title_manifest.SR_DISPATCH_VFPU_MASK)


class DispatchAliasValidation(unittest.TestCase):
    """The alias collection fails closed on every shape the runtime cannot honour."""

    def assert_rejected(self, aliases: list, fragment: str) -> None:
        manifest = base_manifest(worker_thread_entry=0x08804200)
        manifest["runtime_bindings"]["dispatch_aliases"] = aliases
        with self.assertRaises(title_manifest.TitleManifestError) as caught:
            title_manifest.validate_manifest(manifest)
        self.assertIn(fragment, str(caught.exception))

    def test_a_well_formed_alias_is_accepted_and_normalized(self) -> None:
        manifest = base_manifest(worker_thread_entry=0x08804200)
        # Deliberately out of order: normalization must be canonical so two manifests
        # that mean the same thing produce the same digest.
        manifest["runtime_bindings"]["dispatch_aliases"] = [
            {"from": 0x08809000, "to": 0x08809100},
            {"from": 0x08808000, "to": 0x08808100},
        ]
        normalized = title_manifest.validate_manifest(manifest)["runtime_bindings"]
        self.assertEqual(
            normalized["dispatch_aliases"],
            [{"from": 0x08808000, "to": 0x08808100},
             {"from": 0x08809000, "to": 0x08809100}],
        )

    def test_empty_collection_is_rejected(self) -> None:
        self.assert_rejected([], "must not be empty; omit the field instead")

    def test_self_alias_is_rejected(self) -> None:
        self.assert_rejected([{"from": 0x08808000, "to": 0x08808000}],
                             "an alias to itself redirects nothing")

    def test_duplicate_source_is_rejected(self) -> None:
        self.assert_rejected(
            [{"from": 0x08808000, "to": 0x08808100},
             {"from": 0x08808000, "to": 0x08808200}],
            "one source cannot redirect to two bodies",
        )

    def test_chained_alias_is_rejected(self) -> None:
        """The runtime resolves ONE step; a chain would silently stop short."""
        self.assert_rejected(
            [{"from": 0x08808000, "to": 0x08808100},
             {"from": 0x08808100, "to": 0x08808200}],
            "the runtime resolves one step",
        )

    def test_malformed_alias_addresses_are_rejected(self) -> None:
        for alias, fragment in (
            ({"from": 0x08808001, "to": 0x08808100}, "must be 4-byte aligned"),
            ({"from": 0, "to": 0x08808100}, "must not be zero"),
            ({"from": 0x08808000, "to": 0}, "must not be zero"),
            ({"from": "0x08808000", "to": 0x08808100}, "must be an integer"),
            ({"from": 0x08808000}, "missing required field(s): to"),
            ({"to": 0x08808100}, "missing required field(s): from"),
            ({"from": 0x08808000, "to": 0x08808100, "why": 1}, "unknown field(s): why"),
        ):
            with self.subTest(alias=alias):
                self.assert_rejected([alias], fragment)

    def test_the_ceiling_is_enforced(self) -> None:
        over = [{"from": 0x08808000 + 8 * i, "to": 0x08808004 + 8 * i}
                for i in range(title_manifest.MAX_DISPATCH_ALIASES + 1)]
        self.assert_rejected(over, "maximum is 32")

    def test_a_source_inside_the_core_dispatch_reservation_is_rejected(self) -> None:
        """dispatch() reads a target matching the VFPU tag as an instruction address
        before it consults any title binding, so an alias source in that window could
        never fire. Accepting one produces a build whose configuration silently does
        nothing, so it is a manifest error, not a runtime surprise."""
        for source in (title_manifest.SR_DISPATCH_VFPU_TAG,
                       title_manifest.SR_DISPATCH_VFPU_TAG + 0x1000,
                       0x43FFFFFC):
            with self.subTest(source=source):
                self.assert_rejected(
                    [{"from": source, "to": 0x08808100}],
                    "core VFPU dispatch-target range",
                )

    def test_the_reservation_is_exactly_the_window_dispatch_claims(self) -> None:
        """Both neighbours of the reserved window are ordinary, usable sources; only the
        window itself is refused. A rule one address too wide would silently forbid a
        legitimate binding."""
        for source in (title_manifest.SR_DISPATCH_VFPU_TAG - 4, 0x43FFFFFC + 4):
            with self.subTest(source=source):
                manifest = base_manifest(worker_thread_entry=0x08804200)
                manifest["runtime_bindings"]["dispatch_aliases"] = [
                    {"from": source, "to": 0x08808100}]
                normalized = title_manifest.validate_manifest(manifest)["runtime_bindings"]
                self.assertEqual(normalized["dispatch_aliases"],
                                 [{"from": source, "to": 0x08808100}])

    def test_a_destination_inside_the_reservation_is_accepted(self) -> None:
        """`to` is only ever handed to sr_lookup(); it is never compared against a
        dispatch target, so the core reservation must not apply to it."""
        manifest = base_manifest(worker_thread_entry=0x08804200)
        manifest["runtime_bindings"]["dispatch_aliases"] = [
            {"from": 0x08808000, "to": title_manifest.SR_DISPATCH_VFPU_TAG + 0x1000}]
        normalized = title_manifest.validate_manifest(manifest)["runtime_bindings"]
        self.assertEqual(
            normalized["dispatch_aliases"],
            [{"from": 0x08808000, "to": title_manifest.SR_DISPATCH_VFPU_TAG + 0x1000}],
        )


class CallbackTerminatorValidation(unittest.TestCase):
    """The terminator collection fails closed, and can never become address-global."""

    def assert_rejected(self, terminators: list, fragment: str) -> None:
        manifest = base_manifest(worker_thread_entry=0x08804200)
        manifest["runtime_bindings"]["callback_terminators"] = terminators
        with self.assertRaises(title_manifest.TitleManifestError) as caught:
            title_manifest.validate_manifest(manifest)
        self.assertIn(fragment, str(caught.exception))

    def test_the_real_sentinel_values_are_accepted(self) -> None:
        """0 and 0xFFFFFFFF are the sentinels guests actually use. Neither is a guest
        address -- zero is forbidden as one, and 0xFFFFFFFF is not 4-byte aligned -- so
        this asserts the sentinel is validated as a raw target value instead."""
        manifest = base_manifest(worker_thread_entry=0x08804200)
        manifest["runtime_bindings"]["callback_terminators"] = [
            {"sentinel": 0, "ra": 0x08808000},
            {"sentinel": 0xFFFFFFFF, "pc": 0x08808100, "ra": 0x08808200},
        ]
        normalized = title_manifest.validate_manifest(manifest)["runtime_bindings"]
        self.assertEqual(
            normalized["callback_terminators"],
            [{"sentinel": 0, "ra": 0x08808000},
             {"sentinel": 0xFFFFFFFF, "pc": 0x08808100, "ra": 0x08808200}],
        )

    def test_an_unconstrained_terminator_is_rejected(self) -> None:
        """This is the load-bearing rule: without a pc or an ra, the sentinel would
        terminate at every call site in the program."""
        self.assert_rejected([{"sentinel": 0}], "must constrain at least one of pc/ra")

    def test_empty_collection_is_rejected(self) -> None:
        self.assert_rejected([], "must not be empty; omit the field instead")

    def test_duplicate_terminator_is_rejected(self) -> None:
        self.assert_rejected(
            [{"sentinel": 0, "ra": 0x08808000}, {"sentinel": 0, "ra": 0x08808000}],
            "duplicates the terminator already declared",
        )

    def test_a_terminator_subsumed_by_a_broader_one_is_rejected(self) -> None:
        """A narrower entry behind a broader one can never decide anything; that is an
        authoring error, not a preference, so it fails rather than being ignored."""
        self.assert_rejected(
            [{"sentinel": 0, "ra": 0x08808000},
             {"sentinel": 0, "pc": 0x08808100, "ra": 0x08808000}],
            "is unreachable behind the broader terminator",
        )

    def test_distinct_sites_for_one_sentinel_are_accepted(self) -> None:
        """The same sentinel at genuinely different sites is normal, not a conflict."""
        manifest = base_manifest(worker_thread_entry=0x08804200)
        manifest["runtime_bindings"]["callback_terminators"] = [
            {"sentinel": 0, "ra": 0x08808000},
            {"sentinel": 0, "ra": 0x08808004},
        ]
        normalized = title_manifest.validate_manifest(manifest)["runtime_bindings"]
        self.assertEqual(len(normalized["callback_terminators"]), 2)

    def test_malformed_terminator_fields_are_rejected(self) -> None:
        for entry, fragment in (
            ({"sentinel": 0x1_0000_0000, "ra": 0x08808000}, "must be in range"),
            ({"sentinel": -1, "ra": 0x08808000}, "must be in range"),
            ({"sentinel": "0", "ra": 0x08808000}, "must be an integer"),
            ({"sentinel": 0, "ra": 0}, "must not be zero"),
            ({"sentinel": 0, "pc": 0x08808001}, "must be 4-byte aligned"),
            ({"ra": 0x08808000}, "missing required field(s): sentinel"),
            ({"sentinel": 0, "ra": 0x08808000, "uid": 1}, "unknown field(s): uid"),
        ):
            with self.subTest(entry=entry):
                self.assert_rejected([entry], fragment)

    def test_a_sentinel_inside_the_core_dispatch_reservation_is_rejected(self) -> None:
        """Same reservation as the alias source, and for the same reason: the sentinel
        is compared against a dispatch target, and dispatch() has already claimed that
        encoding for the per-instruction VFPU fallback."""
        for sentinel in (title_manifest.SR_DISPATCH_VFPU_TAG,
                         title_manifest.SR_DISPATCH_VFPU_TAG + 0x2000,
                         0x43FFFFFF):
            with self.subTest(sentinel=sentinel):
                self.assert_rejected(
                    [{"sentinel": sentinel, "ra": 0x08808000}],
                    "core VFPU dispatch-target range",
                )

    def test_a_context_field_inside_the_reservation_is_accepted(self) -> None:
        """`pc` and `ra` are compared against CpuState fields at the call site, never
        against a dispatch target, so the reservation must not reach them."""
        manifest = base_manifest(worker_thread_entry=0x08804200)
        manifest["runtime_bindings"]["callback_terminators"] = [
            {"sentinel": 0, "pc": title_manifest.SR_DISPATCH_VFPU_TAG + 0x1000,
             "ra": title_manifest.SR_DISPATCH_VFPU_TAG + 0x2000}]
        normalized = title_manifest.validate_manifest(manifest)["runtime_bindings"]
        self.assertEqual(
            normalized["callback_terminators"],
            [{"sentinel": 0, "pc": title_manifest.SR_DISPATCH_VFPU_TAG + 0x1000,
              "ra": title_manifest.SR_DISPATCH_VFPU_TAG + 0x2000}],
        )

    def test_the_ceiling_is_enforced(self) -> None:
        over = [{"sentinel": 0, "ra": 0x08808000 + 4 * i}
                for i in range(title_manifest.MAX_CALLBACK_TERMINATORS + 1)]
        self.assert_rejected(over, "maximum is 32")


class GeneratedConfiguration(unittest.TestCase):
    def test_generic_configuration_disables_every_binding(self) -> None:
        config = title_runtime_config.bindings_from_manifest(None)
        self.assertEqual(config["source_id"], "none")
        self.assertEqual(config["bindings"], {})
        header = title_runtime_config.render_header(config)
        self.assertIn("#define SR_TITLE_CONFIG_VALID (0u)", header)
        self.assertIn('#define SR_TITLE_CONFIG_SOURCE_ID "none"', header)
        for macro in (
            "SR_TITLE_CONFIG_FALLBACK_ENTRY",
            "SR_TITLE_CONFIG_WORKER_THREAD_ENTRY",
            "SR_TITLE_CONFIG_LAUNCHER_THREAD_ENTRY",
            "SR_TITLE_CONFIG_VBLANK_FRAME_COUNTER_ADDR",
            "SR_TITLE_CONFIG_VBLANK_VSYNC_COUNTER_ADDR",
        ):
            self.assertIn(f"#define {macro} 0x00000000u", header)

    def test_every_emitted_field_has_a_validity_bit(self) -> None:
        self.assertEqual(
            set(title_runtime_config.FIELD_BITS),
            set(title_manifest.RUNTIME_BINDING_FIELDS),
            "a manifest binding without a runtime representation would be silently dropped",
        )

    def test_paired_counters_share_one_validity_bit(self) -> None:
        for left, right in title_manifest.RUNTIME_BINDING_PAIRS:
            self.assertEqual(
                title_runtime_config.FIELD_BITS[left],
                title_runtime_config.FIELD_BITS[right],
                "a paired binding must be all-or-nothing at the C boundary too",
            )

    def test_digest_and_header_are_deterministic_and_configuration_specific(self) -> None:
        digests = {}
        headers = {}
        for label, path in (("none", None), ("a", FIXTURE_A), ("b", FIXTURE_B)):
            manifest = None if path is None else json.loads(path.read_text(encoding="utf-8"))
            config = title_runtime_config.bindings_from_manifest(manifest)
            digests[label] = title_runtime_config.config_digest(config)
            headers[label] = title_runtime_config.render_header(config)
            # Determinism: a second pass over the same input reproduces both exactly.
            again = title_runtime_config.bindings_from_manifest(manifest)
            self.assertEqual(digests[label], title_runtime_config.config_digest(again))
            self.assertEqual(headers[label], title_runtime_config.render_header(again))
        self.assertEqual(len(set(digests.values())), 3, "digests must separate configurations")
        self.assertEqual(len(set(headers.values())), 3, "headers must separate configurations")

    def test_generator_needs_no_game_input(self) -> None:
        """The generic artifact is producible from the tool alone."""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "sr_title_config.h"
            code = title_runtime_config.main(["--output", str(out)])
            self.assertEqual(code, 0)
            self.assertIn(
                f"SR_TITLE_CONFIG_SCHEMA_VERSION "
                f"{title_runtime_config.GENERATED_SCHEMA_VERSION}",
                out.read_text(encoding="utf-8"),
            )

    def test_the_runtime_refuses_a_different_generated_schema_version(self) -> None:
        """title_config.c must fail the BUILD on a stale artifact rather than compile
        against a macro contract it no longer understands."""
        source = (ROOT / "src" / "rt" / "title_config.c").read_text(encoding="utf-8")
        self.assertIn(
            f"#if SR_TITLE_CONFIG_SCHEMA_VERSION != "
            f"{title_runtime_config.GENERATED_SCHEMA_VERSION}",
            source,
            "the runtime schema guard drifted from the generator",
        )
        self.assertIn("#error", source)

    def _compile_title_config(self, header_text: str) -> subprocess.CompletedProcess:
        """Compile src/rt/title_config.c against a supplied generated artifact."""
        compiler = shutil.which(os.environ.get("CC") or "") or shutil.which("gcc") or shutil.which("cc")
        if not compiler:
            self.skipTest("no C compiler on PATH; the compile-time contract cannot be exercised")
        with tempfile.TemporaryDirectory() as tmp:
            gen = Path(tmp) / "sr_title_config.h"
            gen.write_text(header_text, encoding="utf-8")
            return subprocess.run(
                [compiler, "-c", "-std=c11", "-I", str(ROOT / "src" / "rt"), "-I", tmp,
                 str(ROOT / "src" / "rt" / "title_config.c"), "-o", str(Path(tmp) / "tc.o")],
                capture_output=True, text=True,
            )

    def test_a_generated_list_shorter_than_its_count_fails_the_build(self) -> None:
        """The count is the runtime's authority for both collections, so a generated
        artifact whose count exceeds its list would zero-pad the arrays -- and a
        zero-filled TERMINATOR entry reads as {sentinel 0, no pc, no ra}, exactly the
        program-wide match the manifest validator refuses to accept.

        title_config.c sizes both arrays from the list, so _Static_assert turns that
        disagreement into a compile error. This asserts the honest build compiles AND
        that the mutated one does not -- a green result here has to be earned."""
        good = title_runtime_config.render_header(
            title_runtime_config.load_config(None))
        ok = self._compile_title_config(good)
        self.assertEqual(ok.returncode, 0,
                         "the generic generated artifact must compile: " + ok.stderr)

        for macro in ("SR_TITLE_CONFIG_DISPATCH_ALIAS_COUNT",
                      "SR_TITLE_CONFIG_CALLBACK_TERMINATOR_COUNT"):
            with self.subTest(macro=macro):
                mutated = good.replace(f"#define {macro} 0", f"#define {macro} 1")
                self.assertNotEqual(mutated, good, "the count macro moved")
                bad = self._compile_title_config(mutated)
                self.assertNotEqual(
                    bad.returncode, 0,
                    f"{macro} declaring more entries than the list supplies must fail "
                    "the build, but it compiled")
                self.assertIn("does not match its declared count", bad.stderr)

    def test_rewrite_is_a_no_op_when_the_configuration_is_unchanged(self) -> None:
        config = title_runtime_config.bindings_from_manifest(
            json.loads(FIXTURE_A.read_text(encoding="utf-8"))
        )
        rendered = title_runtime_config.render_header(config)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "sr_title_config.h"
            self.assertTrue(title_runtime_config.write_if_changed(out, rendered))
            self.assertFalse(title_runtime_config.write_if_changed(out, rendered))

    def test_cli_rejects_a_malformed_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.json"
            manifest = base_manifest(worker_thread_entry=0x08804201)
            bad.write_text(json.dumps(manifest), encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "title_runtime_config.py"),
                 "--manifest", str(bad), "--output", str(Path(tmp) / "out.h")],
                capture_output=True, text=True, cwd=ROOT,
            )
            self.assertEqual(proc.returncode, 2)
            self.assertIn("4-byte aligned", proc.stderr)
            self.assertFalse((Path(tmp) / "out.h").exists())


class GeneratedCollections(unittest.TestCase):
    """The emitted artifact states the collections, and states nothing when empty."""

    def config(self, path: Path | None) -> dict:
        manifest = None if path is None else json.loads(path.read_text(encoding="utf-8"))
        return title_runtime_config.bindings_from_manifest(manifest)

    def test_generic_configuration_emits_empty_collections(self) -> None:
        header = title_runtime_config.render_header(self.config(None))
        self.assertIn("#define SR_TITLE_CONFIG_DISPATCH_ALIAS_COUNT 0", header)
        self.assertIn("#define SR_TITLE_CONFIG_CALLBACK_TERMINATOR_COUNT 0", header)
        # An empty list macro must expand to nothing at all, not to a zero entry that
        # the runtime would then have to recognize as a non-entry.
        for macro in ("SR_TITLE_CONFIG_DISPATCH_ALIAS_LIST",
                      "SR_TITLE_CONFIG_CALLBACK_TERMINATOR_LIST"):
            self.assertIn(f"#define {macro}\n", header)
        self.assertNotIn("SR_TITLE_CFG_ALIAS(", header)
        self.assertNotIn("SR_TITLE_CFG_TERMINATOR(", header)
        self.assertIn("#define SR_TITLE_CONFIG_VALID (0u)", header)

    def test_every_collection_has_a_validity_bit(self) -> None:
        self.assertEqual(
            set(title_runtime_config.COLLECTION_BITS),
            set(title_manifest.RUNTIME_BINDING_COLLECTIONS),
            "a manifest collection with no runtime representation would be dropped",
        )

    def test_a_collection_without_a_runtime_representation_is_refused(self) -> None:
        """Fail-closed guard: a future manifest field must not reach the runtime with
        no C binding. The validator blocks it first, so this drives the generator."""
        with self.assertRaises(title_runtime_config.TitleRuntimeConfigError):
            title_runtime_config.render_header({
                "source_id": "x", "bindings": {"telemetry_hooks": [{"from": 4}]},
            })

    def test_configured_collections_reach_the_artifact(self) -> None:
        for path in (FIXTURE_A, FIXTURE_B):
            with self.subTest(fixture=path.name):
                config = self.config(path)
                header = title_runtime_config.render_header(config)
                aliases = config["bindings"]["dispatch_aliases"]
                terminators = config["bindings"]["callback_terminators"]
                self.assertIn(
                    f"#define SR_TITLE_CONFIG_DISPATCH_ALIAS_COUNT {len(aliases)}", header)
                self.assertIn(
                    "#define SR_TITLE_CONFIG_CALLBACK_TERMINATOR_COUNT "
                    f"{len(terminators)}", header)
                for alias in aliases:
                    self.assertIn(
                        f"SR_TITLE_CFG_ALIAS(0x{alias['from']:08x}u, "
                        f"0x{alias['to']:08x}u)", header)
                for entry in terminators:
                    self.assertIn(
                        f"SR_TITLE_CFG_TERMINATOR(0x{entry['sentinel']:08x}u, "
                        f"{1 if 'pc' in entry else 0}u, 0x{entry.get('pc', 0):08x}u, "
                        f"{1 if 'ra' in entry else 0}u, 0x{entry.get('ra', 0):08x}u)",
                        header)
                for bit in title_runtime_config.COLLECTION_BITS.values():
                    self.assertIn(bit, header, "a non-empty collection must set its bit")

    def test_the_two_fixtures_declare_disjoint_collection_addresses(self) -> None:
        """The matrix can only separate two titles if their addresses cannot coincide."""
        def sites(path: Path) -> set[int]:
            bindings = self.config(path)["bindings"]
            used: set[int] = set()
            for alias in bindings["dispatch_aliases"]:
                used |= {alias["from"], alias["to"]}
            for entry in bindings["callback_terminators"]:
                used |= {entry[f] for f in ("pc", "ra") if f in entry}
            return used

        a, b = sites(FIXTURE_A), sites(FIXTURE_B)
        self.assertTrue(a and b, "both fixtures must declare collection addresses")
        self.assertEqual(a & b, set(),
                         f"the fixtures share collection address(es): "
                         f"{sorted(hex(x) for x in a & b)}")

    def test_the_two_fixtures_share_their_sentinel_values(self) -> None:
        """Deliberate: a sentinel is generic guest vocabulary, so both fixtures use the
        SAME values at DIFFERENT sites. That is what makes 'same 0 elsewhere follows
        generic behavior' an assertion the matrix can actually fail."""
        def sentinels(path: Path) -> set[int]:
            return {t["sentinel"]
                    for t in self.config(path)["bindings"]["callback_terminators"]}
        self.assertEqual(sentinels(FIXTURE_A), sentinels(FIXTURE_B))
        self.assertEqual(sentinels(FIXTURE_A), {0, 0xFFFFFFFF})

    def test_a_changed_collection_changes_the_digest(self) -> None:
        """The build binds this digest into RUNTIME_PROFILE_HASH, so a changed binding
        must invalidate stale runtime objects instead of relinking silently."""
        manifest = json.loads(FIXTURE_A.read_text(encoding="utf-8"))
        before = title_runtime_config.config_digest(
            title_runtime_config.bindings_from_manifest(manifest))
        manifest["runtime_bindings"]["dispatch_aliases"] = [
            {"from": 0x08809000, "to": 0x08809100}]
        after = title_runtime_config.config_digest(
            title_runtime_config.bindings_from_manifest(manifest))
        self.assertNotEqual(before, after, "an alias change left the digest unchanged")

        manifest = json.loads(FIXTURE_A.read_text(encoding="utf-8"))
        manifest["runtime_bindings"]["callback_terminators"][0]["ra"] += 4
        moved = title_runtime_config.config_digest(
            title_runtime_config.bindings_from_manifest(manifest))
        self.assertNotEqual(before, moved, "a terminator move left the digest unchanged")

    def test_the_digest_is_order_independent(self) -> None:
        """Normalization is canonical, so reordering a collection is not a new build."""
        manifest = json.loads(FIXTURE_B.read_text(encoding="utf-8"))
        straight = title_runtime_config.config_digest(
            title_runtime_config.bindings_from_manifest(manifest))
        manifest["runtime_bindings"]["dispatch_aliases"].reverse()
        manifest["runtime_bindings"]["callback_terminators"].reverse()
        reversed_digest = title_runtime_config.config_digest(
            title_runtime_config.bindings_from_manifest(manifest))
        self.assertEqual(straight, reversed_digest)


class DispatchAddressCensus(unittest.TestCase):
    """Source-shape gate (tier 4) over the addresses generic dispatch used to hardcode.

    SCOPE, stated honestly: this is a literal-text census over a fixed list of numbers
    in a fixed list of files. It catches a reintroduced literal, which is the realistic
    regression, and it is what makes the executable matrix's zero-collision claim hold
    for the whole file rather than only the probes it runs. It is NOT a proof about
    arbitrary C: a value assembled at run time, or spelled some other way, would pass
    this check. The executable proof is `make dispatch-isolation-selftest`.
    """

    SELFTEST = "src/rt/dispatch_isolation_selftest.c"

    def test_generic_runtime_sources_name_no_retired_dispatch_address(self) -> None:
        for relative in GENERIC_RUNTIME_SOURCES:
            lines = (ROOT / relative).read_text(encoding="utf-8").splitlines()
            for label, spellings in RETIRED_DISPATCH_ADDRESSES.items():
                # Report the offending LINES, not the whole file: assertNotIn against a
                # source text prints the entire container and buries the finding.
                found = [
                    f"{relative}:{number}: {line.strip()}"
                    for number, line in enumerate(lines, 1)
                    if any(s.casefold() in line.casefold() for s in spellings)
                ]
                self.assertEqual(
                    found, [],
                    f"the retired {label} address is named in generic runtime code; it "
                    "belongs in a validated title manifest, not in generic dispatch:\n"
                    + "\n".join(found),
                )

    def test_the_census_would_catch_each_reintroduction(self) -> None:
        """Negative control, one per migrated binding: the check above is not vacuous.
        Each sample is the literal shape the migrated code actually had."""
        samples = {
            "dispatch alias source":
                "    { 0x00030950u, 0xFFFFFFFFu, \"TC30950\", hook_call_0x30948 },",
            "null callback terminator ra":
                "    if (target == 0u && s->r[31] == 0x0003e06cu) { s->r[2] = 1u; }",
            "minus-one terminator pc":
                "    if (target == UINT32_MAX && s->pc == 0x00292fa0u) { s->r[2] = 1u; }",
        }
        for label, sample in samples.items():
            with self.subTest(label=label):
                hits = {
                    found
                    for found, spellings in RETIRED_DISPATCH_ADDRESSES.items()
                    if any(s.casefold() in sample.casefold() for s in spellings)
                }
                self.assertIn(label, hits,
                              f"the census would not have noticed: {sample}")

    def test_the_isolation_selftest_still_proves_the_addresses_are_inert(self) -> None:
        """The other half of the census. Excluding the selftest from the scan above is
        only sound while the selftest actually exercises those addresses -- otherwise
        the numbers could quietly vanish from the tree along with the proof."""
        text = (ROOT / self.SELFTEST).read_text(encoding="utf-8").casefold()
        for label, spellings in RETIRED_DISPATCH_ADDRESSES.items():
            self.assertTrue(
                any(spelling.casefold() in text for spelling in spellings),
                f"{self.SELFTEST} no longer probes the retired {label} address, so "
                "nothing proves it is inert",
            )
        self.assertIn("test_retired_bindings_are_inert", text)

    def test_the_inventory_and_the_census_police_the_same_addresses(self) -> None:
        """Two independent hardcoded lists now name these addresses: the
        compatibility-override inventory in tools/compat_overrides.py, which states the
        remaining semantic debt and its retirement criterion, and RETIRED_DISPATCH_ADDRESSES
        above, which enforces their absence from generic runtime code.

        Nothing tied them together. An address could be dropped from the census while the
        inventory kept claiming a gate that no longer existed, or added to the inventory
        without ever being policed -- which is exactly the "moved a literal from one
        hardcoded table into another" failure this whole migration exists to avoid.
        """
        inventoried = {int(o["address"]) for o in compat_overrides.TITLE_CONFIGURED_DISPATCH}
        self.assertTrue(inventoried,
                        "TITLE_CONFIGURED_DISPATCH is empty; the inventory claims nothing "
                        "and every assertion below would be vacuous")
        policed = {int(spellings[0], 16) for spellings in RETIRED_DISPATCH_ADDRESSES.values()}
        self.assertEqual(
            inventoried - policed, set(),
            "tools/compat_overrides.py inventories title-configured dispatch address(es) "
            "that this census does not police, so nothing enforces their absence from "
            f"generic runtime code: {sorted(hex(a) for a in inventoried - policed)}")

    def test_every_inventoried_binding_names_a_live_collection_and_accessor(self) -> None:
        """An inventory entry is prose unless it points at something real. Each one must
        name a collection the validator actually accepts, and that collection's generic
        accessor must actually be called from dispatch()."""
        accessors = {
            "dispatch_aliases": "sr_title_config_dispatch_alias",
            "callback_terminators": "sr_title_config_is_callback_terminator",
        }
        self.assertEqual(set(accessors), set(title_manifest.RUNTIME_BINDING_COLLECTIONS),
                         "a runtime-binding collection has no accessor mapping here, so "
                         "entries naming it would go unchecked")
        recomp = (ROOT / "src" / "rt" / "recomp.c").read_text(encoding="utf-8")
        for entry in compat_overrides.TITLE_CONFIGURED_DISPATCH:
            source = entry.get("source", "")
            named = [c for c in accessors if c in source]
            self.assertEqual(
                len(named), 1,
                f"inventory entry {entry.get('name')!r} must name exactly one runtime "
                f"binding collection in its source field, got: {source!r}")
            self.assertIn(
                accessors[named[0]], recomp,
                f"inventory entry {entry.get('name')!r} claims to flow through "
                f"{named[0]}, but dispatch() does not call {accessors[named[0]]}")
            self.assertNotEqual(
                entry.get("test", "none"), "none",
                f"inventory entry {entry.get('name')!r} is title-configured but names no "
                "executable test; the isolation matrix is what makes the claim checkable")

    def test_dispatch_reads_the_collections_only_through_the_generic_accessors(self) -> None:
        recomp = (ROOT / "src" / "rt" / "recomp.c").read_text(encoding="utf-8")
        for accessor in ("sr_title_config_dispatch_alias",
                         "sr_title_config_is_callback_terminator"):
            self.assertIn(accessor, recomp,
                          f"recomp.c must consume {accessor} rather than a literal")
        self.assertNotIn(
            "sr_title_config.h", recomp,
            "recomp.c must not include the build-local generated artifact; only "
            "title_config.c may see it",
        )

    def test_the_selftest_matrix_covers_generic_and_two_distinct_fixtures(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("DISPATCH_ISO_CONFIGS := generic fixture-a fixture-b", makefile)
        self.assertRegex(makefile, r"(?m)^DISPATCH_ISO_MANIFEST_generic :=\s*$")
        fixtures = set(re.findall(
            r"(?m)^DISPATCH_ISO_MANIFEST_fixture-\w+ := (\S+)$", makefile))
        self.assertEqual(len(fixtures), 2, "the matrix must use two distinct fixtures")
        # Both matrices must exercise the same fixture pair, or one could silently stop
        # covering a configuration the other still claims is covered.
        sched_fixtures = set(re.findall(
            r"(?m)^SCHED_SELFTEST_MANIFEST_fixture-\w+ := (\S+)$", makefile))
        self.assertEqual(fixtures, sched_fixtures,
                         "the dispatch and scheduler matrices drifted apart")

    def test_targets_that_compile_dispatch_link_the_title_configuration(self) -> None:
        """recomp.c now calls the accessors, so every target that compiles it must link
        title_config.c against a generated artifact -- otherwise the target fails to
        link, or worse, silently drops the isolation boundary."""
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        for target in ("heap-selftest:", "profiler-selftest:", "vfpu-interp-selftest:",
                       "dispatch-isolation-selftest-one:"):
            recipe = makefile.split(target, 1)[1].split("\n\n", 1)[0]
            with self.subTest(target=target):
                self.assertIn("src/rt/title_config.c", recipe)
                self.assertRegex(recipe, r"-I\$\((GENERIC_TITLE_CONFIG_DIR|DISPATCH_ISO_DIR)\)")


class GenericRuntimeCarriesNoTitleAddress(unittest.TestCase):
    def test_generic_runtime_sources_name_no_retired_address(self) -> None:
        for relative in GENERIC_RUNTIME_SOURCES:
            text = (ROOT / relative).read_text(encoding="utf-8").casefold()
            for label, spellings in RETIRED_RUNTIME_ADDRESSES.items():
                for spelling in spellings:
                    self.assertNotIn(
                        spelling.casefold(), text,
                        f"{relative} still names the retired {label} address {spelling}",
                    )

    def test_the_guard_would_catch_a_reintroduced_literal(self) -> None:
        """A negative control: the check above is not vacuous."""
        sample = "    if (entry == 0x000468c8u) { claim_worker(); }".casefold()
        hits = [
            label
            for label, spellings in RETIRED_RUNTIME_ADDRESSES.items()
            if any(spelling.casefold() in sample for spelling in spellings)
        ]
        self.assertEqual(hits, ["worker thread entry"])

    def test_the_runtime_reads_the_bindings_only_through_the_generic_accessors(self) -> None:
        accessors = (
            "sr_title_config_fallback_entry",
            "sr_title_config_is_worker_entry",
            "sr_title_config_is_launcher_entry",
            "sr_title_config_vblank_counters",
        )
        header = (ROOT / "src" / "rt" / "title_config.h").read_text(encoding="utf-8")
        for name in accessors:
            self.assertIn(name, header)
        driver = (ROOT / "src" / "rt" / "driver.c").read_text(encoding="utf-8")
        sched = (ROOT / "src" / "rt" / "sched.c").read_text(encoding="utf-8")
        self.assertIn("sr_title_config_fallback_entry()", driver)
        for name in ("sr_title_config_is_worker_entry", "sr_title_config_is_launcher_entry",
                     "sr_title_config_vblank_counters"):
            self.assertIn(name, sched)
        # Only title_config.c may see the generated artifact.
        for relative in ("src/rt/driver.c", "src/rt/sched.c"):
            self.assertNotIn(
                "sr_title_config.h", (ROOT / relative).read_text(encoding="utf-8"),
                f"{relative} must not include the build-local generated artifact",
            )

    def test_makefile_binds_the_configuration_into_the_runtime_profile(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        hash_line = next(
            line for line in makefile.splitlines()
            if line.startswith("RUNTIME_PROFILE_HASH :=")
        )
        self.assertIn("TITLE_CONFIG_DIGEST=$(TITLE_CONFIG_DIGEST)", hash_line)
        record_line = next(
            line for line in makefile.splitlines()
            if "--section runtime" in line
        )
        self.assertIn("TITLE_CONFIG_DIGEST=$(TITLE_CONFIG_DIGEST)", record_line)
        # The generic build must not default to a title manifest.
        self.assertRegex(makefile, r"(?m)^TITLE_MANIFEST \?=\s*$")
        # The scheduler matrix must cover a generic build and two distinct fixtures.
        self.assertIn("SCHED_SELFTEST_CONFIGS := generic fixture-a fixture-b", makefile)
        self.assertRegex(makefile, r"(?m)^SCHED_SELFTEST_MANIFEST_generic :=\s*$")
        fixtures = set(re.findall(r"(?m)^SCHED_SELFTEST_MANIFEST_fixture-\w+ := (\S+)$", makefile))
        self.assertEqual(len(fixtures), 2, "the matrix must use two distinct fixtures")

    def test_an_explicit_hst_build_without_a_manifest_fails_closed(self):
        """GAME_NAME=hst with no title configuration must refuse, not build generically."""
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("TITLE_CONFIG_HST_UNBOUND := 1", makefile)
        # The refusal is attached to the generated artifact, so it fires exactly when a
        # runtime object would be produced -- never for diagnostics or cleanup.
        rule = makefile.split("$(TITLE_CONFIG_HEADER): ", 1)[1].split("\n\n", 1)[0]
        self.assertIn("ifeq ($(TITLE_CONFIG_HST_UNBOUND),1)", rule)
        self.assertIn("$(error", rule)
        self.assertIn("TITLE_MANIFEST=$(HST_TITLE_MANIFEST)", rule)
        # The game-input-free selftests must not be dragged into that requirement.
        self.assertIn("GENERIC_TITLE_CONFIG_HEADER", makefile)
        for target in ("hle-thread-selftest-build:", "$(PSP_ORACLE_SMOKE_EXE):"):
            recipe = makefile.split(target, 1)[1].split("\n\n", 1)[0]
            self.assertIn("$(GENERIC_TITLE_CONFIG_DIR)", recipe,
                          f"{target} must build against a title-neutral configuration")


class RoleUidsAreOutcomesNotConfiguration(unittest.TestCase):
    """The role UIDs must never be title configuration, nor a numeric default."""

    SCHED = ROOT / "src" / "rt" / "sched.c"
    HLE = ROOT / "src" / "rt" / "hle.c"

    def test_role_uids_are_not_manifest_fields(self):
        published = set(title_manifest.RUNTIME_BINDING_FIELDS)
        for forbidden in ("root_uid", "worker_uid", "launcher_uid", "root_thread_uid"):
            self.assertNotIn(
                forbidden, published,
                "a role UID is an outcome of allocation and must not become title configuration",
            )

    def test_role_globals_start_uncaptured(self):
        source = self.SCHED.read_text(encoding="utf-8")
        for role in ("g_root_uid", "g_worker_uid", "g_launcher_uid"):
            self.assertRegex(
                source, rf"(?m)^uint32_t\s+{role}\s*=\s*SR_ROLE_UID_NONE;",
                f"{role} must not be seeded with a UID the allocator can hand out",
            )
        for historical in ("0x110u", "0x111u", "0x114u"):
            self.assertNotRegex(
                source, rf"(?m)^uint32_t\s+g_\w+_uid\s*=\s*{historical};",
                "a historical allocation must not be a role default",
            )

    def test_the_uid_pool_never_produces_the_absent_marker(self):
        hle = self.HLE.read_text(encoding="utf-8")
        allocator = hle.split("uint32_t sr_alloc_uid(void) {", 1)[1].split("}", 1)[0]
        self.assertIn("SR_ROLE_UID_NONE", allocator)
        self.assertIn("s_uid == 0u", allocator)

    def test_the_headless_gate_stub_reports_no_role_and_agrees_on_the_marker(self):
        """The gate has no scheduler, so it cannot have earned a role identity."""
        stub = (ROOT / "tools" / "gate_stub.c").read_text(encoding="utf-8")
        header = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")
        marker = re.search(r"#define SR_ROLE_UID_NONE (\S+)", header).group(1)
        self.assertEqual(
            re.search(r"#define SR_ROLE_UID_NONE (\S+)", stub).group(1), marker,
            "gate_stub.c restates SR_ROLE_UID_NONE; it must match recomp.h",
        )
        for accessor in ("sched_root_uid", "sched_worker_uid", "sched_launcher_uid"):
            self.assertRegex(
                stub, rf"uint32_t {accessor}\(void\)\s*{{\s*return SR_ROLE_UID_NONE;",
                f"{accessor} in the headless gate must report an uncaptured role",
            )

    def test_role_questions_go_through_failclosed_predicates(self):
        header = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")
        for name in ("sched_uid_is_root", "sched_uid_is_worker", "sched_uid_is_launcher",
                     "sched_current_is_worker", "sched_current_is_launcher",
                     "sched_role_uid_captured", "SR_ROLE_UID_NONE"):
            self.assertIn(name, header)
        # UID 0 is PSP's "current thread" value; it must be rejected by the matcher, so a
        # role question asked with no current thread can never answer yes.
        matcher = self.SCHED.read_text(encoding="utf-8")
        matcher = matcher.split("static int role_uid_matches(", 1)[1].split("\n}", 1)[0]
        self.assertIn("uid == 0u", matcher)
        self.assertIn("role_uid == SR_ROLE_UID_NONE", matcher)


if __name__ == "__main__":
    unittest.main()
