# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import hashlib
import os
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import publish_audit

LF = bytes([10])
CRLF = bytes([13, 10])
NUL = bytes([0])
BOM = bytes([0xEF, 0xBB, 0xBF])
UTF16LE = bytes([0xFF, 0xFE])
UTF16BE = bytes([0xFE, 0xFF])



def hermetic_policy(repo: Path, include_paths, exclude_paths=(), exclude_globs=()) -> tuple[Path, Path]:
    """Write a valid canonical policy + matching export for a hermetic fixture repo.

    The publication gate fails closed: it refuses to run without a policy, and it
    rejects any path the policy does not classify. Hermetic fixtures therefore have
    to declare their own tiny policy, exactly as the real repository does. Returns
    (policy_path, export_path) for passing to audit_entries().
    """
    import publication_policy

    document = {
        "name": "hermetic-test-profile",
        "profile_version": "2.0.0",
        "min_tool_version": "0.4.0",
        "build_mode": "PUBLIC_SAFE=1",
        "default_disposition": "REJECT",
        "exclude_prefixes": [],
        "exclude_globs": list(exclude_globs),
        "exclude_paths": list(exclude_paths),
        "include_paths": sorted(set(include_paths)),
    }
    policy_path = repo / "_hermetic_policy.json"
    policy_path.write_text(json.dumps(document), encoding="utf-8", newline="\n")
    export_path = repo / "_hermetic_export.json"
    export_path.write_text(
        json.dumps({"profile": document["name"],
                    "policy_sha256": publication_policy.canonical_digest(document)}) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return policy_path, export_path


class TestPublishAudit(unittest.TestCase):
    def test_forbidden_private_paths_and_formats(self):
        self.assertIsNotNone(publish_audit._forbidden_path("place_game_here/EBOOT.elf"))
        self.assertIsNotNone(publish_audit._forbidden_path("notes/EBOOT.BIN.dec.h"))
        self.assertIsNotNone(publish_audit._forbidden_path("build/hst/hst_recomp_0.c"))
        self.assertIsNotNone(
            publish_audit._forbidden_path("OpenGrip_For_Inspiration/functions.csv")
        )
        self.assertIsNotNone(publish_audit._forbidden_path("assets/game.iso"))
        self.assertIsNotNone(publish_audit._forbidden_path("tools/vfpu_words.txt"))
        self.assertIsNotNone(publish_audit._forbidden_path("tools/reference_hashes.json"))
        self.assertIsNone(publish_audit._forbidden_path("assets/vfpu/table.dat"))

    def test_direct_private_key_assignment_is_detected_without_storing_the_value(self):
        source = "VKEY = bytes.fromhex('0123456789abcdef' * 2)\n"
        self.assertEqual(publish_audit.private_key_assignment_lines(source), [])
        literal = "VKEY = bytes.fromhex('" + ("01" * 16) + "')\n"
        self.assertEqual(publish_audit.private_key_assignment_lines(literal), [1])

    def test_unrelated_fips_key_literal_is_allowed(self):
        source = "key = bytes.fromhex('" + ("01" * 16) + "')\n"
        self.assertEqual(publish_audit.private_key_assignment_lines(source), [])

    def test_action_pin_rule(self):
        self.assertIsNotNone(
            publish_audit.FULL_SHA_ACTION.fullmatch(
                "actions/checkout@de0fac2e4500dabe0009e67214ff5f5447ce83dd"
            )
        )
        self.assertIsNone(publish_audit.FULL_SHA_ACTION.fullmatch("actions/checkout@v6"))

    def test_filename_collisions_case_and_unicode(self):
        paths = ["docs/README.md", "docs/readme.md"]
        findings = publish_audit.check_collisions(paths)
        self.assertTrue(any(f.code == "COLLISION_CASE" for f in findings))

    def test_dangerous_unicode_path_characters(self):
        bidi_path = "docs/guide_\u202e_override.md"
        findings = publish_audit.check_collisions([bidi_path])
        self.assertTrue(any(f.code == "PATH_DANGEROUS_UNICODE" for f in findings))

    def test_windows_reserved_device_names_and_trailing_characters(self):
        reserved_path = "docs/CON.txt"
        findings = publish_audit.check_collisions([reserved_path])
        self.assertTrue(any(f.code == "FILENAME_RESERVED" for f in findings))

        trailing_dot_path = "docs/guide.md."
        findings_dot = publish_audit.check_collisions([trailing_dot_path])
        self.assertTrue(any(f.code == "FILENAME_TRAILING" for f in findings_dot))

    def test_cross_platform_local_paths(self):
        slash = chr(92)
        windows_text = "path = " + "C:" + slash + "Us" + "ers" + slash + "alice" + slash + "repo\n"
        self.assertTrue(bool(publish_audit.WINDOWS_USER_PATH.search(windows_text)))

        mac_text = "path = '/Us" + "ers/john/secret.txt'\n"
        self.assertTrue(bool(publish_audit.MAC_USER_PATH.search(mac_text)))

        wsl_text = "path = '/mnt/c/Us" + "ers/alice/repo'\n"
        self.assertTrue(bool(publish_audit.WSL_USER_PATH.search(wsl_text)))

        unc_text = "path = '\\\\" + "server\\share\\data'\n"
        self.assertTrue(bool(publish_audit.UNC_PATH.search(unc_text)))

    def test_onedrive_and_temp_local_paths(self):
        slash = chr(92)
        onedrive_text = "backup_dir = '/Us" + "ers/john/One" + "Drive - Company/data'\n"
        self.assertTrue(bool(publish_audit.ONEDRIVE_PATH.search(onedrive_text)))

        temp_text = "tmp = 'C:" + slash + "Windows" + slash + "Te" + "mp" + slash + "scratch.txt'\n"
        self.assertTrue(bool(publish_audit.TEMP_PATH.search(temp_text)))

    def test_audit_entries_and_manifest_report(self):
        entries = [
            publish_audit.GitEntry("100644", "abc1234", "0", "LICENSE", "file"),
            publish_audit.GitEntry("160000", "def5678", "0", "third_party/submodule", "gitlink"),
        ]
        findings = publish_audit.audit_entries(entries)
        report = publish_audit.generate_manifest_report(entries, findings)
        self.assertIn("status", report)
        self.assertEqual(report["total_files"], 2)
        self.assertIn("meta", report)
        self.assertIn("aggregate_manifest_sha256", report["meta"])
        self.assertTrue(any(f.code == "GITLINK" for f in findings))

    def test_csv_audit_manifest_export(self):
        entries = [
            publish_audit.GitEntry("100644", "abc1234", "0", "LICENSE", "file"),
        ]
        findings = publish_audit.audit_entries(entries)
        with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as tmp:
            tmp_path = Path(tmp.name)
        try:
            publish_audit.export_csv_manifest_report(entries, findings, tmp_path)
            text = tmp_path.read_text(encoding="utf-8")
            self.assertIn("path,mode,working_mode,index_sha,size,sha256,kind,text_binary,magic", text)
            self.assertIn("LICENSE", text)
        finally:
            tmp_path.unlink(missing_ok=True)

    def test_secret_scan_redaction_fixture(self):
        sentinel_secret = "SUPER_SECRET_SENTINEL_TOKEN_12345"
        leaks_data = [
            {
                "RuleID": "generic-api-key",
                "File": "config/secret.json",
                "StartLine": 4,
                "Secret": sentinel_secret,
            }
        ]
        with tempfile.NamedTemporaryFile(suffix=".json", mode="w", delete=False, encoding="utf-8") as tmp:
            json.dump(leaks_data, tmp)
            tmp_path = Path(tmp.name)
        try:
            findings = publish_audit.parse_secret_scan_report(tmp_path)
            self.assertEqual(len(findings), 1)
            self.assertEqual(findings[0].code, "SECRET_SCAN_LEAK")
            self.assertNotIn(sentinel_secret, findings[0].detail)
            self.assertNotIn(sentinel_secret[:10], findings[0].detail)

            entries = [publish_audit.GitEntry("100644", "abc", "0", "config/secret.json", "file")]
            report = publish_audit.generate_manifest_report(entries, findings, secret_scan_report=tmp_path)
            report_str = json.dumps(report)
            self.assertNotIn(sentinel_secret, report_str)

            with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as tmp_csv:
                csv_path = Path(tmp_csv.name)
            try:
                publish_audit.export_csv_manifest_report(entries, findings, csv_path)
                csv_text = csv_path.read_text(encoding="utf-8")
                self.assertNotIn(sentinel_secret, csv_text)
            finally:
                csv_path.unlink(missing_ok=True)
        finally:
            tmp_path.unlink(missing_ok=True)

    def test_symlink_containment_path_resolution(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            tmp_dir = Path(tmp_dir_raw).resolve()
            repo = tmp_dir / "repo"
            repo_private = tmp_dir / "repo-private"
            repo.mkdir()
            repo_private.mkdir()

            secret_file = repo_private / "secret.txt"
            secret_file.write_text("secret", encoding="utf-8", newline="\n")

            symlink_file = repo / "link.txt"
            try:
                symlink_file.symlink_to(secret_file)
            except (OSError, NotImplementedError):
                self.skipTest("Symlinks not supported on this platform/privilege level")

            entries = [publish_audit.GitEntry("120000", "sha", "0", "link.txt", "symlink")]
            findings = publish_audit.audit_entries(entries, repo_root=repo, is_candidate_root=True)
            self.assertTrue(any(f.code == "SYMLINK_ESCAPE" for f in findings))

    def test_symlink_relative_escape_rejection(self):
        entries = [publish_audit.GitEntry("120000", "sha", "0", "docs/link_escape.txt", "symlink")]
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            docs_dir = repo / "docs"
            docs_dir.mkdir()
            symlink_file = docs_dir / "link_escape.txt"
            try:
                symlink_file.symlink_to(Path("../../outside.txt"))
            except (OSError, NotImplementedError):
                self.skipTest("Symlinks not supported on this platform/privilege level")

            findings = publish_audit.audit_entries(entries, repo_root=repo, is_candidate_root=True)
            self.assertTrue(any(f.code == "SYMLINK_ESCAPE" for f in findings))

    def test_git_lfs_pointer_recognition_and_manifest_classification(self):
        lfs_pointer = (
            "version https://git-lfs.github.com/spec/v1\n"
            "oid sha256:d363cec75fecfafd1511e370e48c748c30199c694b83a4026be34d799b192917\n"
            "size 4316284\n"
        )
        self.assertTrue(publish_audit.is_git_lfs_pointer(lfs_pointer))

        regular_text = (
            "This file references version https://git-lfs.github.com/spec/v1 in documentation.\n"
            "It is not an LFS pointer.\n"
        )
        self.assertFalse(publish_audit.is_git_lfs_pointer(regular_text))

        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            (repo / "LICENSE").write_text("LICENSE", encoding="utf-8", newline="\n")
            (repo / "NOTICE.md").write_text("NOTICE", encoding="utf-8", newline="\n")
            (repo / "README.md").write_text("README", encoding="utf-8", newline="\n")
            (repo / "AGENTS.md").write_text("AGENTS", encoding="utf-8", newline="\n")

            lfs_file = repo / "asset.bin"
            lfs_file.write_text(lfs_pointer, encoding="utf-8", newline="\n")

            entries = [
                publish_audit.GitEntry("100644", "", "0", "asset.bin", "lfs_pointer")
            ]
            report = publish_audit.generate_manifest_report(entries, [], repo_root=repo, is_candidate_root=True)
            self.assertEqual(report["summary"]["lfs_pointers"], 1)

    def test_lfs_attribute_mismatch_detection(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            (repo / ".gitattributes").write_text("*.dat filter=lfs diff=lfs merge=lfs -text\n", encoding="utf-8", newline="\n")
            (repo / "data.dat").write_text("Not an LFS pointer content\n", encoding="utf-8", newline="\n")

            entries = [publish_audit.GitEntry("100644", "", "0", "data.dat", "file")]
            findings = publish_audit.audit_entries(entries, repo_root=repo, is_candidate_root=True)
            self.assertTrue(any(f.code == "LFS_MISMATCH" for f in findings))

    def test_public_scope_unresolved_asset_rejection(self):
        manifest_path = publish_audit.ROOT / "assets" / "release_manifest.json"
        entries = [
            publish_audit.GitEntry("100644", "hash1", "0", "font/jpn0.pgf", "file"),
        ]
        findings = publish_audit.audit_entries(entries, manifest_path=manifest_path, public_scope=True)
        self.assertTrue(any(f.code == "UNRESOLVED_PUBLIC" for f in findings))

    def test_spirv_and_magic_binary_detection(self):
        kind_spirv = publish_audit._magic_kind(b"\x07\x23\x02\x03\x00\x00\x00\x00")
        self.assertEqual(kind_spirv, "SPIR-V shader bytecode")

        kind_elf = publish_audit._magic_kind(b"\x7fELF\x02\x01\x01\x00")
        self.assertEqual(kind_elf, "ELF executable")

        kind_png = publish_audit._magic_kind(b"\x89PNG\r\n\x1a\n\x00\x00")
        self.assertEqual(kind_png, "PNG image")

    def test_unknown_renamed_binary_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            unknown_bin = repo / "data.unknown"
            unknown_bin.write_bytes(b"\x00\xfe\xdc\xba\x98\x76\x54\x32\x10\xff\xee\xdd")

            entries = [publish_audit.GitEntry("100644", "", "0", "data.unknown", "file")]
            findings = publish_audit.audit_entries(entries, repo_root=repo, exhaustive=True, is_candidate_root=True)
            self.assertTrue(any(f.code == "MAGIC_UNKNOWN" for f in findings))

    def test_manifest_ingestion_malformed_and_duplicate_path(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            malformed_json = repo / "bad_manifest.json"
            malformed_json.write_text("{ invalid json }", encoding="utf-8", newline="\n")

            findings = publish_audit.audit_entries([], manifest_path=malformed_json)
            self.assertTrue(any(f.code == "MANIFEST_ERROR" for f in findings))

            dup_json = repo / "dup_manifest.json"
            dup_data = {
                "components": [
                    {"id": "c1", "source_path": "src/rt/core.c", "license": "MIT"},
                    {"id": "c2", "source_path": "src/rt/core.c", "license": "GPL-2.0-or-later"},
                ]
            }
            dup_json.write_text(json.dumps(dup_data), encoding="utf-8", newline="\n")
            findings_dup = publish_audit.audit_entries([], manifest_path=dup_json)
            self.assertTrue(any(f.code == "MANIFEST_DUPLICATE_PATH" for f in findings_dup))

    def test_manifest_orphan_path_detection(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            orphan_json = repo / "orphan_manifest.json"
            orphan_data = {
                "components": [
                    {"id": "orphan1", "source_path": "non_existent_file.c", "license": "MIT"},
                ]
            }
            orphan_json.write_text(json.dumps(orphan_data), encoding="utf-8", newline="\n")

            entries = [publish_audit.GitEntry("100644", "", "0", "LICENSE", "file")]
            findings = publish_audit.audit_entries(entries, manifest_path=orphan_json, repo_root=repo, is_candidate_root=True)
            self.assertTrue(any(f.code == "MANIFEST_ORPHAN_PATH" for f in findings))

    def test_spdx_required_skips_byte_exact_import_trees(self):
        # Byte-exact third-party imports (FFmpeg n4.4 under src/rt/atrac3p/) must
        # not be forced to gain SPDX headers: editing them breaks the recorded
        # upstream blob identity (see PROVENANCE.md). The project-authored
        # wrapper and the rest of src/ still require SPDX.
        self.assertFalse(publish_audit._spdx_required("src/rt/atrac3p/libavcodec/atrac.c"))
        self.assertFalse(publish_audit._spdx_required("src/rt/atrac3p/libavutil/mem.c"))
        self.assertFalse(publish_audit._spdx_required("src/rt/atrac3p/libavcodec/vlc.h"))
        self.assertTrue(publish_audit._spdx_required("src/rt/atrac3p/atrac3p_api.c"))
        self.assertTrue(publish_audit._spdx_required("src/rt/atrac3p_selftest.c"))
        self.assertTrue(publish_audit._spdx_required("src/rt/recomp.c"))
        self.assertTrue(publish_audit._spdx_required("tools/codegen.py"))

    def test_deterministic_report_output_and_aggregate_sha(self):
        entries = [
            publish_audit.GitEntry("100644", "abc1234", "0", "LICENSE", "file"),
            publish_audit.GitEntry("100644", "def5678", "0", "README.md", "file"),
        ]
        findings = publish_audit.audit_entries(entries)
        report1 = publish_audit.generate_manifest_report(entries, findings)
        report2 = publish_audit.generate_manifest_report(entries, findings)

        self.assertEqual(
            report1["meta"]["aggregate_manifest_sha256"],
            report2["meta"]["aggregate_manifest_sha256"],
        )
        self.assertGreater(len(report1["meta"]["aggregate_manifest_sha256"]), 0)

    def test_publish_audit_and_test_suite_do_not_self_trigger(self):
        repo_root = publish_audit.ROOT
        entries = publish_audit._get_git_entries(tracked_only=True, repo_root=repo_root)
        pub_entries = [e for e in entries if e.path in ("tools/publish_audit.py", "tools/test_publish_audit.py")]
        self.assertEqual(len(pub_entries), 2)
        findings = publish_audit.audit_entries(pub_entries, repo_root=repo_root)
        self.assertEqual(findings, [], f"publish_audit self-triggered findings on its own source: {findings}")

    def test_synthetic_scanner_copy_with_injected_path_fails(self):
        repo_root = publish_audit.ROOT
        pub_code = (repo_root / "tools" / "publish_audit.py").read_text(encoding="utf-8")
        slash = chr(92)
        injected_code = pub_code + "\n# Injected private path literal\nBAD_PATH = 'C:" + slash + "Us" + "ers" + slash + "alice" + slash + "secret'\n"

        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            tmp_dir = Path(tmp_dir_raw).resolve()
            fake_scanner = tmp_dir / "publish_audit_copy.py"
            fake_scanner.write_text(injected_code, encoding="utf-8", newline="\n")

            entries = [publish_audit.GitEntry("100644", "", "0", "publish_audit_copy.py", "file")]
            findings = publish_audit.audit_entries(entries, repo_root=tmp_dir, is_candidate_root=True)
            self.assertTrue(any(f.code == "LOCAL_PATH" for f in findings))

    def test_candidate_scan_skips_vcs_metadata_dirs(self):
        # A materialized candidate may itself be a fresh Git repository (e.g.
        # build_public_export.py git-inits the export); its .git internals are
        # scaffolding and must not surface as unknown binaries.
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            (repo / "src").mkdir(parents=True)
            (repo / "src" / "core.c").write_text("# SPDX-License-Identifier: GPL-2.0-or-later\nint x;\n", encoding="utf-8", newline="\n")
            (repo / "LICENSE").write_text("LICENSE", encoding="utf-8", newline="\n")
            (repo / "NOTICE.md").write_text("NOTICE", encoding="utf-8", newline="\n")
            (repo / "README.md").write_text("README", encoding="utf-8", newline="\n")
            (repo / "AGENTS.md").write_text("AGENTS", encoding="utf-8", newline="\n")
            subprocess.run(["git", "init"], cwd=repo, check=True, capture_output=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True)
            subprocess.run(["git", "add", "."], cwd=repo, check=True, capture_output=True)
            subprocess.run(["git", "commit", "-m", "init"], cwd=repo, check=True, capture_output=True)

            entries = publish_audit._get_filesystem_entries(repo)
            paths = [e.path for e in entries]
            self.assertFalse(any(p.startswith(".git/") or p == ".git" for p in paths))
            findings = publish_audit.audit_entries(entries, repo_root=repo, is_candidate_root=True)
            self.assertFalse(any(f.code == "MAGIC_UNKNOWN" for f in findings))

    def test_candidate_scan_skips_gitignored_untracked_scaffolding(self):
        # Contributor tooling leaves gitignored scaffolding in a materialized
        # candidate (e.g. .ruff_cache/ after running pre-commit). The candidate
        # walk must honor the tree's own .gitignore for untracked files instead
        # of reporting their binary internals as unknown.
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            (repo / "src").mkdir(parents=True)
            (repo / "src" / "core.c").write_text("# SPDX-License-Identifier: GPL-2.0-or-later\nint x;\n", encoding="utf-8", newline="\n")
            (repo / "LICENSE").write_text("LICENSE", encoding="utf-8", newline="\n")
            (repo / "NOTICE.md").write_text("NOTICE", encoding="utf-8", newline="\n")
            (repo / "README.md").write_text("README", encoding="utf-8", newline="\n")
            (repo / "AGENTS.md").write_text("AGENTS", encoding="utf-8", newline="\n")
            (repo / ".gitignore").write_text(".ruff_cache/\n", encoding="utf-8", newline="\n")
            (repo / ".ruff_cache").mkdir()
            (repo / ".ruff_cache" / "bin.dat").write_bytes(b"\x00\x01\x02\x03binary-cache-data")
            subprocess.run(["git", "init"], cwd=repo, check=True, capture_output=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True)
            subprocess.run(["git", "add", "."], cwd=repo, check=True, capture_output=True)
            subprocess.run(["git", "commit", "-m", "init"], cwd=repo, check=True, capture_output=True)
            # .ruff_cache is untracked; a fresh run of the scan must prune it.
            self.assertNotIn(".gitignore", subprocess.run(["git", "status", "--porcelain"], cwd=repo, capture_output=True, text=True).stdout)

            entries = publish_audit._get_filesystem_entries(repo)
            paths = [e.path for e in entries]
            self.assertIn("src/core.c", paths)
            self.assertFalse(any(p.startswith(".ruff_cache/") for p in paths), f"gitignored cache leaked: {paths}")
            findings = publish_audit.audit_entries(entries, repo_root=repo, is_candidate_root=True)
            self.assertFalse(any(f.code == "MAGIC_UNKNOWN" for f in findings))

    def test_candidate_scan_keeps_gitignored_tracked_files(self):
        # A tracked file that happens to match a .gitignore pattern must never
        # be pruned from the candidate walk.
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            (repo / "LICENSE").write_text("LICENSE", encoding="utf-8", newline="\n")
            (repo / "NOTICE.md").write_text("NOTICE", encoding="utf-8", newline="\n")
            (repo / "README.md").write_text("README", encoding="utf-8", newline="\n")
            (repo / "AGENTS.md").write_text("AGENTS", encoding="utf-8", newline="\n")
            (repo / ".gitignore").write_text("*.tmp\n", encoding="utf-8", newline="\n")
            (repo / "note.tmp").write_text("tracked despite pattern", encoding="utf-8", newline="\n")
            subprocess.run(["git", "init"], cwd=repo, check=True, capture_output=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True)
            # git add . would skip note.tmp (matches *.tmp); a force-add is how
            # a tracked file that matches an ignore pattern legitimately exists.
            subprocess.run(["git", "add", "."], cwd=repo, check=True, capture_output=True)
            subprocess.run(["git", "add", "-f", "note.tmp"], cwd=repo, check=True, capture_output=True)
            subprocess.run(["git", "commit", "-m", "init"], cwd=repo, check=True, capture_output=True)

            entries = publish_audit._get_filesystem_entries(repo)
            paths = [e.path for e in entries]
            self.assertIn("note.tmp", paths)

    def test_manifest_orphan_skips_public_scope_excluded_components(self):
        # A manifest component declared public_scope_included: False is
        # legitimately absent from a public-scope candidate; absence must not
        # be reported as an orphan. Outside public-scope auditing it still is.
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            (repo / "LICENSE").write_text("LICENSE", encoding="utf-8", newline="\n")
            (repo / "NOTICE.md").write_text("NOTICE", encoding="utf-8", newline="\n")
            (repo / "README.md").write_text("README", encoding="utf-8", newline="\n")
            (repo / "AGENTS.md").write_text("AGENTS", encoding="utf-8", newline="\n")
            manifest = repo / "m.json"
            manifest.write_text(
                json.dumps(
                    {
                        "components": [
                            {
                                "id": "font",
                                "source_path": "font/jpn0.pgf",
                                "type": "asset",
                                "presence": "tracked_file",
                                "public_scope_included": False,
                            }
                        ]
                    }
                ),
                encoding="utf-8",
                newline="\n",
            )
            entries = [publish_audit.GitEntry("100644", "", "0", "LICENSE", "file")]
            # The manifest component is declared public_scope_included: False but the
            # policy here does NOT exclude it, so its absence is still an orphan
            # outside public-scope auditing. (A policy-excluded path is exempt in
            # every mode -- that case is covered by
            # test_manifest_orphan_skips_policy_excluded_components below.)
            policy_path, export_path = hermetic_policy(repo, ["LICENSE"])
            findings_public = publish_audit.audit_entries(
                entries, manifest_path=manifest, public_scope=True, repo_root=repo, is_candidate_root=True,
                policy_path=policy_path, export_path=export_path,
            )
            self.assertFalse(any(f.code == "MANIFEST_ORPHAN_PATH" for f in findings_public))
            findings_private = publish_audit.audit_entries(
                entries, manifest_path=manifest, public_scope=False, repo_root=repo, is_candidate_root=True,
                policy_path=policy_path, export_path=export_path,
            )
            self.assertTrue(any(f.code == "MANIFEST_ORPHAN_PATH" for f in findings_private))

            # A path the canonical policy excludes is expected to be absent in every
            # mode; its absence is compliance, not an orphan.
            excl_policy, excl_export = hermetic_policy(
                repo, ["LICENSE"], exclude_paths=["font/jpn0.pgf"]
            )
            findings_excluded = publish_audit.audit_entries(
                entries, manifest_path=manifest, public_scope=False, repo_root=repo, is_candidate_root=True,
                policy_path=excl_policy, export_path=excl_export,
            )
            self.assertFalse(any(f.code == "MANIFEST_ORPHAN_PATH" for f in findings_excluded))

    def test_hermetic_git_index_vs_working_tree_contract(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            subprocess.run(["git", "init"], cwd=repo, check=True, capture_output=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True)

            slash = chr(92)
            safe_content = "# SPDX-License-Identifier: GPL-2.0-or-later\n# Safe tracked file\n"
            unsafe_content = "# SPDX-License-Identifier: GPL-2.0-or-later\nSECRET_DIR = 'C:" + slash + "Us" + "ers" + slash + "alice" + slash + "data'\n"

            target_file = repo / "src" / "core.py"
            (repo / "src").mkdir(parents=True)
            (repo / "LICENSE").write_text("LICENSE", encoding="utf-8", newline="\n")
            (repo / "NOTICE.md").write_text("NOTICE", encoding="utf-8", newline="\n")
            (repo / "README.md").write_text("README", encoding="utf-8", newline="\n")
            (repo / "AGENTS.md").write_text("AGENTS", encoding="utf-8", newline="\n")

            # --- Case 1: Index is SAFE, Working tree on disk is UNSAFE (unstaged edit) ---
            target_file.write_text(safe_content, encoding="utf-8", newline="\n")
            subprocess.run(["git", "add", "src/core.py"], cwd=repo, check=True)
            safe_sha = subprocess.run(["git", "ls-files", "-s", "src/core.py"], cwd=repo, capture_output=True, text=True, check=True).stdout.split()[1]

            # Modify working tree to be UNSAFE without staging
            target_file.write_text(unsafe_content, encoding="utf-8", newline="\n")

            entries_git = publish_audit._get_git_entries(tracked_only=True, repo_root=repo)
            self.assertEqual(len(entries_git), 1)
            self.assertEqual(entries_git[0].sha, safe_sha)

            policy_path, export_path = hermetic_policy(repo, ["src/core.py"])
            findings_index = publish_audit.audit_entries(
                entries_git, repo_root=repo, policy_path=policy_path, export_path=export_path
            )
            self.assertEqual(findings_index, [], "Index audit must pass when indexed blob is safe, ignoring unstaged unsafe working tree edits")

            # --- Case 2: Index is UNSAFE, Working tree on disk is SAFE ---
            target_file.write_text(unsafe_content, encoding="utf-8", newline="\n")
            subprocess.run(["git", "add", "src/core.py"], cwd=repo, check=True)
            unsafe_sha = subprocess.run(["git", "ls-files", "-s", "src/core.py"], cwd=repo, capture_output=True, text=True, check=True).stdout.split()[1]

            # Revert working tree on disk to be SAFE (unstaged)
            target_file.write_text(safe_content, encoding="utf-8", newline="\n")

            entries_unsafe_index = publish_audit._get_git_entries(tracked_only=True, repo_root=repo)
            self.assertEqual(entries_unsafe_index[0].sha, unsafe_sha)

            findings_unsafe_index = publish_audit.audit_entries(entries_unsafe_index, repo_root=repo)
            self.assertTrue(any(f.code == "LOCAL_PATH" for f in findings_unsafe_index), "Index audit must fail when indexed blob is unsafe, even if working tree file was edited back to safe")

            # --- Case 3: Staged UNSAFE vs Unstaged SAFE ---
            # Verified above: staged object is unsafe_sha, working tree is safe, audit_entries catches unsafe_sha.

            # --- Case 4: Index-only / deleted working-tree file ---
            target_file.unlink()
            entries_deleted_disk = publish_audit._get_git_entries(tracked_only=True, repo_root=repo)
            self.assertEqual(entries_deleted_disk[0].sha, unsafe_sha)
            findings_deleted_disk = publish_audit.audit_entries(entries_deleted_disk, repo_root=repo)
            self.assertTrue(any(f.code == "LOCAL_PATH" for f in findings_deleted_disk), "Index audit reads deleted file blob cleanly via git cat-file")

            # --- Case 5: Materialized Candidate-Root Disk Semantics ---
            target_file.write_text(unsafe_content, encoding="utf-8", newline="\n")
            entries_candidate = publish_audit._get_filesystem_entries(repo)
            findings_candidate = publish_audit.audit_entries(entries_candidate, repo_root=repo, is_candidate_root=True)
            self.assertTrue(any(f.code == "LOCAL_PATH" for f in findings_candidate), "Candidate-root mode audits disk content directly")


# A tracked file whose *working-tree* bytes carry a publication finding while its staged
# blob stays clean. This is the exact shape that let `hst_manager.ps1 -Action Verify`
# report every gate PASS while the publication audit read different bytes: the audit
# enumerated paths from the Git index and then read each path's staged blob, so an
# unstaged edit was invisible to it.
SAFE_SOURCE = "# SPDX-License-Identifier: GPL-2.0-or-later\n# Safe tracked file\n"
UNSAFE_SOURCE = (
    "# SPDX-License-Identifier: GPL-2.0-or-later\n"
    "SECRET_DIR = 'C:" + chr(92) + "Us" + "ers" + chr(92) + "alice" + chr(92) + "data'\n"
)


def _make_publication_fixture_repo(repo: Path) -> Path:
    """Build a minimal hermetic repo that a clean publication audit passes."""
    subprocess.run(["git", "init"], cwd=repo, check=True, capture_output=True)
    subprocess.run(["git", "config", "user.name", "Test"], cwd=repo, check=True, capture_output=True)
    subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True, capture_output=True)

    for name, body in (
        ("LICENSE", "LICENSE"),
        ("NOTICE.md", "NOTICE"),
        ("README.md", "README"),
        ("AGENTS.md", "AGENTS"),
    ):
        # Final newline and explicit LF: the fixture claims to be a tree a clean
        # audit passes, and the text-hygiene check is part of "clean".
        (repo / name).write_text(body + "\n", encoding="utf-8", newline="\n")

    # main() defaults --manifest to <root>/assets/release_manifest.json and reports a
    # MANIFEST_ERROR when it is absent, so the fixture needs an empty but valid one.
    (repo / "assets").mkdir()
    (repo / "assets" / "release_manifest.json").write_text('{"components": []}\n', encoding="utf-8", newline="\n")

    target = repo / "src" / "core.py"
    (repo / "src").mkdir()
    target.write_text(SAFE_SOURCE, encoding="utf-8", newline="\n")

    # The gate fails closed without a canonical policy, and rejects any path the
    # policy does not classify, so a fixture that is meant to pass has to declare
    # its own policy and a matching generated export -- exactly as the real
    # repository does.
    import publication_policy

    tracked = ["LICENSE", "NOTICE.md", "README.md", "AGENTS.md",
               "assets/release_manifest.json", "src/core.py",
               "assets/public_source_profile.json", "PUBLIC_EXPORT.json"]
    document = {
        "name": "hermetic-test-profile",
        "profile_version": "2.0.0",
        "min_tool_version": "0.4.0",
        "build_mode": "PUBLIC_SAFE=1",
        "default_disposition": "REJECT",
        "exclude_prefixes": [],
        "exclude_globs": [],
        "exclude_paths": [],
        "include_paths": sorted(tracked),
    }
    (repo / "assets" / "public_source_profile.json").write_text(
        json.dumps(document) + "\n", encoding="utf-8", newline="\n")
    (repo / "PUBLIC_EXPORT.json").write_text(
        json.dumps({"profile": document["name"],
                    "policy_sha256": publication_policy.canonical_digest(document)}) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    subprocess.run(["git", "add", "-A"], cwd=repo, check=True, capture_output=True)
    return target


def _verify_suite_audit_invocations() -> list[list[str]]:
    """Extract the publish_audit argument lists that -Action Verify actually runs.

    Parsed out of hst_manager.ps1 rather than restated here, so that dropping or
    weakening a content source in the manager fails this test instead of leaving the
    regression asserting a command line the canonical gate no longer uses.
    """
    manager = (publish_audit.ROOT / "hst_manager.ps1").read_text(encoding="utf-8")
    start = manager.index("function Invoke-VerifySuite")
    rest = manager.find("\n    function ", start)
    body = manager[start : rest if rest != -1 else len(manager)]

    invocations = []
    for raw_args in re.findall(r"python\s+tools/publish_audit\.py([^\r\n|]*)", body):
        invocations.append(raw_args.split())
    return invocations


class TestVerifyWorktreeTruth(unittest.TestCase):
    def test_committed_tree_source_reads_materialized_commit_bytes(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            _make_publication_fixture_repo(repo)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=repo, check=True)

            materialized, tree = publish_audit._materialize_committed_tree("HEAD", repo)
            try:
                entries = publish_audit._get_filesystem_entries(materialized)
                findings = publish_audit.audit_entries(
                    entries,
                    manifest_path=materialized / "assets" / "release_manifest.json",
                    repo_root=materialized,
                    content_source=publish_audit.CONTENT_COMMITTED,
                    policy_path=materialized / "assets" / "public_source_profile.json",
                    export_path=materialized / "PUBLIC_EXPORT.json",
                    expected_tree_sha=tree,
                    committed_tree_ref="HEAD",
                    tree_repo_root=repo,
                )
                self.assertFalse(
                    [f for f in findings if f.code == "UNREADABLE"],
                    "committed-tree audits must read the materialized commit bytes",
                )
                self.assertFalse(
                    [f for f in findings if f.code.startswith("POLICY_TREE_")],
                    "committed-tree audit must bind to the exact commit tree",
                )
            finally:
                import shutil
                shutil.rmtree(materialized, ignore_errors=True)

    def test_worktree_source_sees_unstaged_edit_that_index_source_ignores(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            target = _make_publication_fixture_repo(repo)

            # Staged blob stays clean; only the bytes on disk carry the finding.
            target.write_text(UNSAFE_SOURCE, encoding="utf-8", newline="\n")

            index_entries = publish_audit._get_git_entries(
                tracked_only=True, repo_root=repo, content_source=publish_audit.CONTENT_INDEX
            )
            index_findings = publish_audit.audit_entries(
                index_entries, repo_root=repo, content_source=publish_audit.CONTENT_INDEX
            )
            self.assertEqual(
                [f for f in index_findings if f.code == "LOCAL_PATH"],
                [],
                "index content source must keep ignoring unstaged edits (pre-commit contract)",
            )

            worktree_entries = publish_audit._get_git_entries(
                tracked_only=True, repo_root=repo, content_source=publish_audit.CONTENT_WORKTREE
            )
            worktree_findings = publish_audit.audit_entries(
                worktree_entries, repo_root=repo, content_source=publish_audit.CONTENT_WORKTREE
            )
            self.assertTrue(
                any(f.code == "LOCAL_PATH" and f.path == "src/core.py" for f in worktree_findings),
                "worktree content source must read the bytes currently on disk",
            )

    def test_worktree_source_falls_back_to_index_blob_for_deleted_file(self):
        """Deleting a tracked file must not clear a finding its staged blob still carries."""
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            target = _make_publication_fixture_repo(repo)

            target.write_text(UNSAFE_SOURCE, encoding="utf-8", newline="\n")
            subprocess.run(["git", "add", "src/core.py"], cwd=repo, check=True, capture_output=True)
            target.unlink()

            entries = publish_audit._get_git_entries(
                tracked_only=True, repo_root=repo, content_source=publish_audit.CONTENT_WORKTREE
            )
            findings = publish_audit.audit_entries(
                entries, repo_root=repo, content_source=publish_audit.CONTENT_WORKTREE
            )
            self.assertTrue(
                any(f.code == "LOCAL_PATH" for f in findings),
                "worktree mode must fall back to the staged blob when the path is gone from disk",
            )

    def test_report_meta_records_which_bytes_were_audited(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            _make_publication_fixture_repo(repo)
            entries = publish_audit._get_git_entries(tracked_only=True, repo_root=repo)

            for source in (publish_audit.CONTENT_INDEX, publish_audit.CONTENT_WORKTREE):
                report = publish_audit.generate_manifest_report(
                    entries, [], repo_root=repo, content_source=source
                )
                self.assertEqual(report["meta"]["content_source"], source)

            candidate = publish_audit.generate_manifest_report(
                entries, [], repo_root=repo, is_candidate_root=True
            )
            self.assertEqual(candidate["meta"]["content_source"], publish_audit.CONTENT_CANDIDATE)

    def test_verify_suite_audit_invocations_cover_both_content_sources(self):
        invocations = _verify_suite_audit_invocations()
        self.assertTrue(invocations, "Invoke-VerifySuite must still run tools/publish_audit.py")
        self.assertTrue(
            any("--worktree" in args for args in invocations),
            "Verify must audit working-tree bytes, not only staged blobs",
        )
        self.assertTrue(
            any("--worktree" not in args for args in invocations),
            "Verify must still audit the staged blobs a commit would publish",
        )

    def test_canonical_verify_audit_fails_on_unstaged_publication_finding(self):
        """Run the manager's own audit command lines against a tree with an unstaged finding.

        Executes tools/publish_audit.py's main() the way -Action Verify invokes it, so a
        regression that drops --worktree from hst_manager.ps1 makes this fail rather than
        leaving a green Verify describing bytes nobody checked.
        """
        invocations = _verify_suite_audit_invocations()
        self.assertTrue(invocations)

        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            repo = Path(tmp_dir_raw).resolve()
            target = _make_publication_fixture_repo(repo)

            with mock.patch.object(publish_audit, "ROOT", repo):
                # A clean tree stays green on every content source Verify uses.
                clean_codes = [publish_audit.main(list(args)) for args in invocations]
                self.assertEqual(
                    clean_codes,
                    [0] * len(invocations),
                    f"clean fixture tree must pass every Verify audit invocation: {clean_codes}",
                )

                # Unstaged tracked edit carrying a publication finding.
                target.write_text(UNSAFE_SOURCE, encoding="utf-8", newline="\n")
                unstaged_codes = [publish_audit.main(list(args)) for args in invocations]
                self.assertTrue(
                    any(code != 0 for code in unstaged_codes),
                    "an unstaged publication finding must break the canonical Verify audit",
                )

                # Staging it keeps it caught: the normal commit-bound workflow is unchanged.
                subprocess.run(["git", "add", "src/core.py"], cwd=repo, check=True, capture_output=True)
                staged_codes = [publish_audit.main(list(args)) for args in invocations]
                self.assertTrue(
                    any(code != 0 for code in staged_codes),
                    "a staged publication finding must still break the canonical Verify audit",
                )

                # Reverting on disk *and* in the index returns the gate to green.
                target.write_text(SAFE_SOURCE, encoding="utf-8", newline="\n")
                subprocess.run(["git", "add", "src/core.py"], cwd=repo, check=True, capture_output=True)
                restored_codes = [publish_audit.main(list(args)) for args in invocations]
                self.assertEqual(
                    restored_codes,
                    [0] * len(invocations),
                    f"reverted tree must return to green: {restored_codes}",
                )

    def test_worktree_and_candidate_root_are_mutually_exclusive(self):
        self.assertEqual(publish_audit.main(["--worktree", "--candidate-root", str(publish_audit.ROOT)]), 2)


class TextHygieneTests(unittest.TestCase):
    """Encoding and line-ending drift must be named, not discovered as hash noise.

    `.gitattributes` normalises on commit, so the index is clean by construction
    and these findings fire almost entirely in worktree audits. That is the case
    that matters: a CRLF-polluted checkout makes every touched file's content
    hash disagree with the provenance ledger, and the resulting wall of
    PROVENANCE_CONTENT_MISMATCH says nothing about the actual cause. This
    happened, and cost real time, which is why the diagnosis is now a check.
    """

    def _codes(self, data: bytes, path: str = "sample.txt"):
        return [f.code for f in publish_audit.check_text_hygiene({path: (data, None)})]

    def test_clean_lf_text_is_silent(self):
        self.assertEqual(self._codes(b"line one" + LF + b"line two" + LF), [])

    def test_crlf_is_reported(self):
        self.assertEqual(self._codes(b"a" + CRLF + b"b" + CRLF), ["TEXT_LINE_ENDING_CRLF"])

    def test_utf8_bom_is_reported(self):
        self.assertEqual(self._codes(BOM + b"line" + LF), ["TEXT_ENCODING_BOM"])

    def test_utf16_both_endiannesses_are_reported(self):
        self.assertEqual(self._codes(UTF16LE + b"l" + NUL), ["TEXT_ENCODING_UTF16"])
        self.assertEqual(self._codes(UTF16BE + NUL + b"l"), ["TEXT_ENCODING_UTF16"])

    def test_missing_final_newline_is_reported(self):
        self.assertEqual(self._codes(b"one" + LF + b"two"), ["TEXT_FINAL_NEWLINE"])

    def test_binary_content_is_never_reported(self):
        # NUL-bearing data is binary; CRLF inside it is not a line-ending defect.
        self.assertEqual(self._codes(NUL + b"payload" + CRLF), [])

    def test_exempt_binary_suffix_is_skipped(self):
        self.assertEqual(self._codes(b"a" + CRLF + b"b", path="assets/x.dat"), [])

    def test_empty_file_is_silent(self):
        self.assertEqual(self._codes(b""), [])

    def test_real_tracked_binary_asset_is_not_flagged(self):
        raw = subprocess.run(
            ["git", "show", ":assets/vfpu/vfpu_log2_lut.dat"],
            cwd=publish_audit.ROOT, capture_output=True,
        ).stdout
        if not raw:
            self.skipTest("binary LUT unavailable in this checkout")
        self.assertEqual(self._codes(raw, path="assets/vfpu/vfpu_log2_lut.dat"), [])


class CanonicalWriterTests(unittest.TestCase):
    """Canonical generated text must be byte-identical on every host.

    Windows Python translates "
" to CRLF in text mode unless newline is given
    explicitly, so a writer without it produces different bytes -- and a
    different SHA-256 -- depending on which host an agent happens to run from.
    Every writer that emits canonical or hash-participating text therefore pins
    the newline.
    """

    CANONICAL_WRITERS = (
        "tools/public_export.py",
        "tools/provenance_attest_verify.py",
        "tools/shader_embed.py",
        "tools/generate_sbom.py",
        "tools/build_public_export.py",
        "tools/publish_audit.py",
    )

    def test_canonical_writers_pin_the_newline(self):
        pattern = re.compile(r'\.write_text\(((?:[^()]|\([^()]*\))*)\)', re.S)
        for rel in self.CANONICAL_WRITERS:
            source = (publish_audit.ROOT / rel).read_text(encoding="utf-8")
            for match in pattern.finditer(source):
                args = match.group(1)
                if "encoding=" not in args:
                    continue
                with self.subTest(path=rel, call=args[:60]):
                    self.assertIn("newline=", args,
                                  "%s writes canonical text without pinning newline; on Windows "
                                  "this silently emits CRLF and changes the SHA-256" % rel)

    def test_same_logical_text_yields_the_same_bytes_and_digest(self):
        # The host-independence invariant, exercised directly: an explicit LF
        # writer must produce identical bytes to an explicit binary writer, on
        # any platform this test runs on.
        text = "alpha" + chr(10) + "beta" + chr(10)
        with tempfile.TemporaryDirectory() as tmp:
            explicit = Path(tmp) / "explicit.txt"
            binary = Path(tmp) / "binary.txt"
            explicit.write_text(text, encoding="utf-8", newline=chr(10))
            binary.write_bytes(text.encode("utf-8"))
            self.assertEqual(explicit.read_bytes(), binary.read_bytes())
            self.assertEqual(
                hashlib.sha256(explicit.read_bytes()).hexdigest(),
                hashlib.sha256(binary.read_bytes()).hexdigest(),
            )
            self.assertNotIn(CRLF, explicit.read_bytes())

    def test_host_default_writer_would_differ_on_windows(self):
        # Pins *why* the rule exists rather than only that it is followed: on a
        # host whose linesep is CRLF, an unpinned writer diverges. Asserted
        # conditionally so the test states the same fact on every platform.
        with tempfile.TemporaryDirectory() as tmp:
            unpinned = Path(tmp) / "unpinned.txt"
            unpinned.write_text("a" + chr(10), encoding="utf-8")  # deliberately unpinned:
            # this call is the defect under test and must NOT be "fixed"
            produced = unpinned.read_bytes()
            if os.linesep == chr(13) + chr(10):
                self.assertIn(CRLF, produced,
                              "expected host newline translation to be observable here")
            else:
                self.assertEqual(produced, b"a" + LF)


if __name__ == "__main__":
    unittest.main()
