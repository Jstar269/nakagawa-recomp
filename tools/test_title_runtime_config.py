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
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

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

#: Generic runtime sources that must no longer name any of the addresses above.
GENERIC_RUNTIME_SOURCES = ("src/rt/driver.c", "src/rt/sched.c", "src/rt/title_config.c",
                           "src/rt/title_config.h")


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
        for fixture in (FIXTURE_A, FIXTURE_B):
            bindings = title_manifest.validate_manifest(
                json.loads(fixture.read_text(encoding="utf-8"))
            )["runtime_bindings"]
            for label, spellings in RETIRED_RUNTIME_ADDRESSES.items():
                retired = int(spellings[0], 16)
                self.assertNotIn(
                    retired, set(bindings.values()),
                    f"{fixture.name} must not reuse the retired {label} address",
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
            published, set(title_manifest.RUNTIME_BINDING_FIELDS),
            "the schema binding vocabulary drifted from the Python validator",
        )
        self.assertIn("runtime_bindings", schema["properties"])


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
            self.assertIn("SR_TITLE_CONFIG_SCHEMA_VERSION 1", out.read_text(encoding="utf-8"))

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
