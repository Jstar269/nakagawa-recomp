# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import pspsdk_compare
import pspsdk_source
import pspsdk_sync

PIN = "314b2083f2e1eaf145fc5de342736336fe1f0148"

IMPORT_SOURCE = r'''
    .set noreorder
    #include "pspimport.s"
    #ifdef F_ThreadManForUser_0000
        IMPORT_START "ThreadManForUser",0x40010000
    #endif
    #ifdef F_ThreadManForUser_0001
        IMPORT_FUNC "ThreadManForUser",0xE81CAF8F,sceKernelCreateCallback
    #endif
    /* IMPORT_FUNC "ThreadManForUser",0xDEADBEEF,commentedOut */
'''

HEADER_SOURCE = r'''
#ifndef SYNTH_H
#define SYNTH_H
extern SceUID sceKernelCreateCallback(
    const char *name,
    SceKernelCallbackFunction func,
    void *arg
);
#endif
'''


def make_tree(root: Path) -> None:
    (root / "src" / "user").mkdir(parents=True)
    (root / "src" / "user" / "ThreadManForUser.S").write_text(
        IMPORT_SOURCE, encoding="utf-8", newline="\n"
    )
    (root / "src" / "user" / "pspthreadman.h").write_text(
        HEADER_SOURCE, encoding="utf-8", newline="\n"
    )


class ImportParserTests(unittest.TestCase):
    def test_extracts_start_and_function_with_provenance(self) -> None:
        functions, libraries = pspsdk_source.parse_import_assembly(
            IMPORT_SOURCE, "src/user/ThreadManForUser.S"
        )
        self.assertEqual(libraries, {"ThreadManForUser": "0x40010000"})
        self.assertEqual(
            functions,
            [
                {
                    "library": "ThreadManForUser",
                    "nid": "0xe81caf8f",
                    "symbol": "sceKernelCreateCallback",
                    "source_file": "src/user/ThreadManForUser.S",
                    "source_line": 8,
                }
            ],
        )

    def test_comments_do_not_create_imports(self) -> None:
        functions, _ = pspsdk_source.parse_import_assembly(
            IMPORT_SOURCE, "src/user/ThreadManForUser.S"
        )
        self.assertNotIn("commentedOut", {item["symbol"] for item in functions})

    def test_unknown_import_spelling_fails_closed(self) -> None:
        bad = IMPORT_SOURCE.replace(
            'IMPORT_FUNC "ThreadManForUser",0xE81CAF8F,sceKernelCreateCallback',
            'IMPORT_FUNC("ThreadManForUser",0xE81CAF8F,sceKernelCreateCallback)',
        )
        with self.assertRaises(pspsdk_source.SyncError) as ctx:
            pspsdk_source.parse_import_assembly(bad, "bad.S")
        self.assertIn("unrecognized IMPORT_FUNC", str(ctx.exception))

    def test_aliased_import_keeps_both_names(self) -> None:
        # PSPSDK's InitForKernel.S and sceSasCore.S use this macro; the sync failed on it.
        source = (
            'IMPORT_START "sceSasCore",0x00090000\n'
            'IMPORT_FUNC_WITH_ALIAS\t"sceSasCore",0xA232CBE6,'
            "__sceSasSetTrianglarWave,__sceSasSetTriangularWave\n"
        )
        functions, _ = pspsdk_source.parse_import_assembly(source, "src/sascore/sceSasCore.S")
        self.assertEqual(
            functions,
            [
                {
                    "library": "sceSasCore",
                    "nid": "0xa232cbe6",
                    "symbol": "__sceSasSetTrianglarWave",
                    "alias": "__sceSasSetTriangularWave",
                    "source_file": "src/sascore/sceSasCore.S",
                    "source_line": 2,
                }
            ],
        )

    def test_aliased_import_without_alias_fails_closed(self) -> None:
        source = (
            'IMPORT_START "L",0x00090000\n'
            'IMPORT_FUNC_WITH_ALIAS "L",0x00000001,onlyOneName\n'
        )
        with self.assertRaises(pspsdk_source.SyncError) as ctx:
            pspsdk_source.parse_import_assembly(source, "bad.S")
        self.assertIn("unrecognized IMPORT_FUNC_WITH_ALIAS", str(ctx.exception))

    def test_function_without_matching_start_fails(self) -> None:
        source = 'IMPORT_FUNC "Ghost",0x00000001,sceGhost\n'
        with self.assertRaises(pspsdk_source.SyncError) as ctx:
            pspsdk_source.parse_import_assembly(source, "ghost.S")
        self.assertIn("without IMPORT_START", str(ctx.exception))

    def test_conflicting_library_flags_fail(self) -> None:
        source = (
            'IMPORT_START "L",0x00000001\n'
            'IMPORT_START "L",0x00000002\n'
        )
        with self.assertRaises(pspsdk_source.SyncError) as ctx:
            pspsdk_source.parse_import_assembly(source, "flags.S")
        self.assertIn("conflicting flags", str(ctx.exception))

    def test_exact_duplicate_across_files_is_kept_once_and_named(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_tree(root)
            (root / "src" / "user" / "duplicate.S").write_text(
                IMPORT_SOURCE, encoding="utf-8"
            )
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                functions, _ = pspsdk_source.scan_imports(root)
            self.assertEqual(
                [(f["nid"], f["symbol"]) for f in functions],
                [("0xe81caf8f", "sceKernelCreateCallback")],
            )
            lines = stderr.getvalue().splitlines()
            self.assertEqual(len(lines), 1)
            self.assertIn("duplicate upstream import ThreadManForUser 0xe81caf8f", lines[0])
            self.assertIn("sceKernelCreateCallback", lines[0])
            self.assertIn("kept once", lines[0])

    def test_same_nid_with_different_names_fails_with_both_locations(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_tree(root)
            (root / "src" / "user" / "renamed.S").write_text(
                IMPORT_SOURCE.replace("sceKernelCreateCallback", "otherName"),
                encoding="utf-8",
            )
            with self.assertRaises(pspsdk_source.SyncError) as ctx:
                pspsdk_source.scan_imports(root)
            message = str(ctx.exception)
            self.assertIn("duplicate upstream import ThreadManForUser 0xe81caf8f", message)
            self.assertIn("src/user/ThreadManForUser.S:8", message)
            self.assertIn("src/user/renamed.S:8", message)

    NAND_START = 'IMPORT_START "sceNand_driver",0x00010000\n'

    def _scan_nand(self, *names: str) -> tuple[list[dict[str, object]], str]:
        # The shape of PSPSDK 71fb45a's src/nand/sceNand_driver.S: one NID declared twice.
        source = self.NAND_START + "".join(
            f'IMPORT_FUNC "sceNand_driver",0xC478C1DE,{name}\n' for name in names
        )
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "src" / "nand").mkdir(parents=True)
            (root / "src" / "nand" / "sceNand_driver.S").write_text(source, encoding="utf-8")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                functions, _ = pspsdk_source.scan_imports(root)
            return functions, stderr.getvalue()

    def test_placeholder_and_real_name_resolve_to_the_real_name(self) -> None:
        for names, lines in (
            (("sceNand_driver_C478C1DE", "sceNandReadPagesRawAll"), (2, 3)),
            (("sceNandReadPagesRawAll", "sceNand_driver_C478C1DE"), (3, 2)),
        ):
            with self.subTest(order=names):
                functions, stderr = self._scan_nand(*names)
                self.assertEqual(
                    [(f["nid"], f["symbol"], f["source_line"]) for f in functions],
                    [("0xc478c1de", "sceNandReadPagesRawAll", lines[1])],
                )
                printed = stderr.splitlines()
                self.assertEqual(len(printed), 1)
                self.assertIn("sceNand_driver 0xc478c1de", printed[0])
                self.assertIn(
                    f"placeholder sceNand_driver_C478C1DE at src/nand/sceNand_driver.S:{lines[0]}",
                    printed[0],
                )
                self.assertIn(
                    f"sceNandReadPagesRawAll at src/nand/sceNand_driver.S:{lines[1]}", printed[0]
                )

    def test_two_real_names_for_one_nid_still_fail(self) -> None:
        with self.assertRaises(pspsdk_source.SyncError) as ctx:
            self._scan_nand("sceNandReadPagesRawAll", "sceNandReadPagesRawExtra")
        self.assertIn("duplicate upstream import sceNand_driver 0xc478c1de", str(ctx.exception))

    def test_near_placeholders_still_fail(self) -> None:
        for name in (
            "sceOther_driver_C478C1DE",  # another library's form
            "sceNand_driver_DEADBEEF",  # another NID
            "sceNand_driver_c478c1de",  # not PSPSDK's upper-case form
            "sceNand_driver_C478C1DE_x",  # a prefix match only
        ):
            with self.subTest(name=name):
                with self.assertRaises(pspsdk_source.SyncError) as ctx:
                    self._scan_nand(name, "sceNandReadPagesRawAll")
                self.assertIn("duplicate upstream import", str(ctx.exception))

    def test_same_name_with_different_nids_fails_with_both_locations(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_tree(root)
            (root / "src" / "user" / "renumbered.S").write_text(
                IMPORT_SOURCE.replace("0xE81CAF8F", "0x12345678"),
                encoding="utf-8",
            )
            with self.assertRaises(pspsdk_source.SyncError) as ctx:
                pspsdk_source.scan_imports(root)
            message = str(ctx.exception)
            self.assertIn("sceKernelCreateCallback has two NIDs", message)
            self.assertIn("src/user/ThreadManForUser.S:8", message)
            self.assertIn("src/user/renumbered.S:8", message)


class PrototypeParserTests(unittest.TestCase):
    def test_extracts_multiline_prototype_and_parameters(self) -> None:
        found = pspsdk_source.parse_prototypes(
            HEADER_SOURCE,
            {"sceKernelCreateCallback"},
            "src/user/pspthreadman.h",
        )
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["return_type"], "SceUID")
        self.assertEqual(
            found[0]["parameters"],
            ["const char *name", "SceKernelCallbackFunction func", "void *arg"],
        )
        self.assertEqual(found[0]["source_line"], 4)

    def test_function_pointer_parameter_is_not_split_at_inner_comma(self) -> None:
        source = "int sceX(int (*fn)(int, int), void *arg);\n"
        found = pspsdk_source.parse_prototypes(source, {"sceX"}, "x.h")
        self.assertEqual(
            found[0]["parameters"], ["int (*fn)(int, int)", "void *arg"]
        )

    def test_function_definition_and_call_are_ignored(self) -> None:
        source = (
            "int sceX(int a) { return a; }\n"
            "static int y(void) { return sceX(1); }\n"
            "static void z(void) { (void)sceX(2); }\n"
        )
        self.assertEqual(
            pspsdk_source.parse_prototypes(source, {"sceX"}, "x.h"), []
        )

    def test_typedef_function_pointer_is_ignored(self) -> None:
        source = "typedef int (*sceX)(int);\n"
        self.assertEqual(
            pspsdk_source.parse_prototypes(source, {"sceX"}, "x.h"), []
        )

    def test_conflicting_prototypes_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_tree(root)
            (root / "src" / "user" / "other.h").write_text(
                "int sceKernelCreateCallback(int x);\n", encoding="utf-8"
            )
            prototypes, conflicts = pspsdk_source.scan_prototypes(
                root, {"sceKernelCreateCallback"}
            )
            self.assertEqual(len(prototypes), 2)
            self.assertEqual(len(conflicts), 1)
            self.assertEqual(conflicts[0]["symbol"], "sceKernelCreateCallback")


class SourceIdentityTests(unittest.TestCase):
    def test_unverified_source_requires_explicit_matching_assertion(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runner = mock.Mock(
                return_value=mock.Mock(
                    returncode=1, stdout=b"", stderr=b"not git"
                )
            )
            with self.assertRaises(pspsdk_source.SyncError):
                pspsdk_source.verify_source_identity(
                    root, PIN, git_runner=runner
                )
            identity = pspsdk_source.verify_source_identity(
                root,
                PIN,
                asserted_commit=PIN,
                allow_unverified_source=True,
                git_runner=runner,
            )
            self.assertEqual(identity["proof"], "caller-asserted")

    def test_git_head_must_match_lock(self) -> None:
        runner = mock.Mock(
            side_effect=[
                mock.Mock(
                    returncode=0,
                    stdout=("0" * 40 + "\n").encode(),
                    stderr=b"",
                )
            ]
        )
        with self.assertRaises(pspsdk_source.SyncError) as ctx:
            pspsdk_source.verify_source_identity(
                Path("."), PIN, git_runner=runner
            )
        self.assertIn("does not match lock", str(ctx.exception))

    def test_untracked_or_tracked_changes_fail_closed(self) -> None:
        runner = mock.Mock(
            side_effect=[
                mock.Mock(
                    returncode=0,
                    stdout=(PIN + "\n").encode(),
                    stderr=b"",
                ),
                mock.Mock(
                    returncode=0,
                    stdout=b"?? src/user/injected.S\n",
                    stderr=b"",
                ),
            ]
        )
        with self.assertRaises(pspsdk_source.SyncError) as ctx:
            pspsdk_source.verify_source_identity(
                Path("."), PIN, git_runner=runner
            )
        self.assertIn("modification", str(ctx.exception))


class ManifestAndComparisonTests(unittest.TestCase):
    def test_build_manifest_is_deterministic_and_carries_prototypes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_tree(root)
            identity = {
                "proof": "caller-asserted",
                "commit": PIN,
                "tracked_dirty": None,
                "tracked_dirty_count": None,
            }
            a = pspsdk_source.build_upstream_manifest(root, PIN, identity)
            b = pspsdk_source.build_upstream_manifest(root, PIN, identity)
            self.assertEqual(a, b)
            self.assertEqual(a["statistics"]["imports"], 1)
            function = a["libraries"][0]["functions"][0]
            self.assertEqual(function["symbol"], "sceKernelCreateCallback")
            self.assertEqual(len(function["prototypes"]), 1)

    def test_comparison_preserves_categories_and_does_not_claim_bug(self) -> None:
        upstream = {
            "schema": 1,
            "upstream": {"commit": PIN},
            "libraries": [
                {
                    "name": "L",
                    "flags": "0x00000000",
                    "functions": [
                        {
                            "library": "L",
                            "nid": "0x00000001",
                            "symbol": "same",
                        },
                        {
                            "library": "L",
                            "nid": "0x00000002",
                            "symbol": "upstreamName",
                        },
                        {
                            "library": "L",
                            "nid": "0x00000004",
                            "symbol": "symbolMoved",
                        },
                        {
                            "library": "L",
                            "nid": "0x00000005",
                            "symbol": "upstreamOnly",
                        },
                    ],
                }
            ],
        }
        nakagawa = {
            "schema": 1,
            "registrations": [
                {
                    "nid": "0x00000001",
                    "name": "same",
                    "handler": "h_same",
                    "classification": "dedicated",
                    "status": "implemented",
                },
                {
                    "nid": "0x00000002",
                    "name": "different",
                    "handler": "h_different",
                    "classification": "fake_success",
                    "status": "stub",
                },
                {
                    "nid": "0x00000003",
                    "name": "symbolMoved",
                    "handler": "h_moved",
                    "classification": "controlled_unsupported",
                    "status": "controlled_unsupported",
                },
                {
                    "nid": "0x00000006",
                    "name": "nakagawaOnly",
                    "handler": "h_only",
                    "classification": "dedicated",
                    "status": "unreviewed",
                },
            ],
        }
        result = pspsdk_compare.compare_with_nakagawa(upstream, nakagawa)
        self.assertEqual(
            {row["category"] for row in result["findings"]},
            {
                "exact_pair",
                "same_nid_conflicting_symbol",
                "same_symbol_conflicting_nid",
                "nakagawa_only",
                "pspsdk_only",
            },
        )
        row = next(
            item
            for item in result["findings"]
            if item.get("nakagawa_symbol") == "different"
        )
        self.assertEqual(row["nakagawa_status"], "stub")
        self.assertIn("not automatically", result["classification_rule"])

    def test_registration_under_either_aliased_name_is_an_exact_pair(self) -> None:
        upstream = {
            "schema": 1,
            "upstream": {"commit": PIN},
            "libraries": [
                {
                    "name": "L",
                    "flags": "0x00000000",
                    "functions": [
                        {
                            "library": "L",
                            "nid": "0x00000001",
                            "symbol": "primaryName",
                            "alias": "aliasName",
                        },
                    ],
                }
            ],
        }
        for name in ("primaryName", "aliasName"):
            with self.subTest(name=name):
                nakagawa = {
                    "schema": 1,
                    "registrations": [
                        {
                            "nid": "0x00000001",
                            "name": name,
                            "handler": "h",
                            "classification": "dedicated",
                            "status": "implemented",
                        }
                    ],
                }
                result = pspsdk_compare.compare_with_nakagawa(upstream, nakagawa)
                self.assertEqual(result["counts"], {"exact_pair": 1})

    def test_markdown_escapes_table_control_characters(self) -> None:
        upstream = {
            "upstream": {
                "commit": PIN,
                "identity": {"proof": "caller-asserted"},
            },
            "statistics": {
                "libraries": 1,
                "imports": 1,
                "symbols_with_prototypes": 0,
                "prototype_conflicts": 0,
            },
        }
        comparison = {
            "counts": {"nakagawa_only": 1},
            "findings": [
                {
                    "category": "nakagawa_only",
                    "nid": "0x00000001",
                    "nakagawa_symbol": "bad|name\nnext",
                    "upstream": [],
                }
            ],
        }
        text = pspsdk_compare.render_markdown(upstream, comparison)
        self.assertNotIn("bad|name\nnext", text)
        self.assertIn("bad\\|name next", text)

    def test_cli_synthetic_no_compare_writes_only_requested_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            source = temp / "pspsdk"
            make_tree(source)
            output = temp / "manifest.json"
            rc = pspsdk_sync.main(
                [
                    "--pspsdk-root",
                    str(source),
                    "--source-commit",
                    PIN,
                    "--allow-unverified-source",
                    "--manifest-out",
                    str(output),
                    "--no-compare",
                ]
            )
            self.assertEqual(rc, 0)
            manifest = json.loads(output.read_text(encoding="ascii"))
            self.assertEqual(manifest["statistics"]["imports"], 1)
            self.assertEqual(
                sorted(path.name for path in temp.iterdir()),
                ["manifest.json", "pspsdk"],
            )


class BoundaryTests(unittest.TestCase):
    def test_symlink_source_file_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            root = temp / "root"
            outside = temp / "outside.S"
            outside.write_text(IMPORT_SOURCE, encoding="utf-8")
            (root / "src").mkdir(parents=True)
            try:
                (root / "src" / "escape.S").symlink_to(outside)
            except (OSError, NotImplementedError):
                self.skipTest("symlink creation unavailable")
            with self.assertRaises(pspsdk_source.SyncError) as ctx:
                pspsdk_source.scan_imports(root)
            self.assertIn("symlink", str(ctx.exception))

    def test_oversized_source_fails_before_decode(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "big.S"
            path.write_bytes(b"x" * 9)
            with self.assertRaises(pspsdk_source.SyncError):
                pspsdk_source._read_bounded(path, max_bytes=8)


if __name__ == "__main__":
    unittest.main()
