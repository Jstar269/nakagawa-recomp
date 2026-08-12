# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Permanent, self-contained regression suite for the fail-closed publication gate.

Every fixture here is **synthetic**. No real PGF outline bytes, no real PGD
implementation, no key material, no game data. The suite reproduces the *path and
policy conditions* of the 2026-08-11 boundary breach without redistributing any of
the questioned content, so it keeps working after the public repository is
replaced and the incident commit no longer exists anywhere public.

The exact-incident regression against the real tree is deliberately **not** here:
it depends on preserved private evidence. See
``test_incident_regression_private.py``, which skips when that evidence is absent.

What each group proves:

* ``TestIncidentPathsRejected`` -- all fifteen excluded paths are rejected, in the
  default mode as well as under ``--public-scope``. The original gate caught six
  under ``--public-scope`` and zero by default.
* ``TestUnknownPathsRejected`` -- a new, unlisted file under ``src/`` or ``tools/``
  is rejected rather than assumed publishable. This is the architectural class of
  failure, not just the fifteen known names.
* ``TestPolicyIntegrity`` -- contradictory, stale, or unsupported policy states
  fail closed.
* ``TestManifestReconciliation`` -- policy and release manifest may not disagree.
* ``TestTreeBinding`` -- an audit of tree A cannot be presented as clearance for
  tree B.
* ``TestPositiveControls`` -- known-safe, explicitly classified files still pass,
  so the gate is not merely failing everything.
"""

from __future__ import annotations

import json
import hashlib
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
AUDIT = TOOLS / "publish_audit.py"
CANONICAL_POLICY = ROOT / "assets" / "public_source_profile.json"

sys.path.insert(0, str(TOOLS))
import publication_policy  # noqa: E402
from publication_policy import PolicyError, canonical_digest, load_policy  # noqa: E402
from public_export import build_document, write_document  # noqa: E402

#: The fifteen paths introduced by ee3985619879f9048344fada20c2d9a64471058d.
#: Reproduced as names only. The fixtures below carry synthetic content.
INCIDENT_PATHS = (
    "font/jpn0.pgf",
    "font/kr0.pgf",
    "font/ltn0.pgf",
    "font/ltn8.pgf",
    "src/rt/pgd.c",
    "src/rt/pgd.h",
    "src/rt/pgf.c",
    "src/rt/pgf.h",
    "tools/pgd_decrypt.py",
    "tools/pgd_e2e_harness.c",
    "tools/pgd_test_keys.py",
    "tools/test_pgd_c.py",
    "tools/test_pgd_decrypt.py",
    "tools/test_pgd_hardening.py",
    "tools/test_pgd_malformed.py",
)

#: A syntactically valid PGF header: 4-byte header word then the "PGF0"
#: signature, matching the real on-disk layout. The remainder is filler. This
#: exercises magic detection without carrying a single real glyph outline.
SYNTHETIC_PGF = b"\x00\x00\x88\x01PGF0" + b"\x00" * 64 + b"SYNTHETIC FIXTURE - NOT A FONT" + b"\x00" * 64

SYNTHETIC_C = (
    "// SPDX-License-Identifier: GPL-2.0-or-later\n"
    "/* synthetic fixture - not a real implementation */\n"
    "int synthetic_stub(void) { return 0; }\n"
)
SYNTHETIC_PY = (
    "# SPDX-License-Identifier: GPL-2.0-or-later\n"
    "# synthetic fixture - not a real implementation\n"
    "def synthetic_stub():\n    return 0\n"
)

REQUIRED_FILES = {
    "LICENSE": "synthetic fixture license placeholder\n",
    "NOTICE.md": "# Notices\n\nSynthetic fixture.\n",
    "README.md": "# Synthetic fixture\n",
    "AGENTS.md": "# Synthetic fixture\n",
}


class TestGeneratedDocumentBytes(unittest.TestCase):
    def test_json_writer_uses_lf_bytes_on_every_host(self) -> None:
        directory = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        output = directory / "generated.json"
        write_document(output, {"first": 1, "second": 2})
        raw = output.read_bytes()
        self.assertTrue(raw.endswith(b"\n"))
        self.assertNotIn(b"\r\n", raw)


def content_for(path: str) -> bytes:
    if path.endswith(".pgf"):
        return SYNTHETIC_PGF
    if path.endswith((".c", ".h")):
        return SYNTHETIC_C.encode()
    if path.endswith(".py"):
        return SYNTHETIC_PY.encode()
    return b"synthetic fixture\n"


def run_audit(candidate: Path, *extra: str, policy: Path | None = None) -> subprocess.CompletedProcess:
    argv = [sys.executable, str(AUDIT), "--candidate-root", str(candidate), *extra]
    if policy is not None:
        argv += ["--policy", str(policy)]
    ledger = candidate / "assets" / "public_provenance_ledger.json"
    if ledger.is_file() and "--provenance-ledger" not in extra:
        argv += ["--provenance-ledger", str(ledger)]
    return subprocess.run(argv, cwd=ROOT, capture_output=True, text=True)


def findings_of(result: subprocess.CompletedProcess) -> list[tuple[str, str]]:
    """Parse ``CODE: path: detail`` lines into (code, path) pairs."""
    parsed: list[tuple[str, str]] = []
    for line in result.stderr.splitlines():
        if ":" not in line:
            continue
        code, _, rest = line.partition(":")
        code = code.strip()
        if not code or not code.replace("_", "").isupper():
            continue
        path = rest.split(":", 1)[0].strip()
        parsed.append((code, path))
    return parsed


def codes_for(result: subprocess.CompletedProcess, path: str) -> set[str]:
    return {code for code, found in findings_of(result) if found == path}


class FixtureMixin:
    """Builds a minimal synthetic candidate tree plus a matching policy."""

    def build_candidate(self, extra_files: dict[str, bytes] | None = None) -> Path:
        candidate = Path(self.enterContext(__import__("tempfile").TemporaryDirectory())) / "candidate"
        candidate.mkdir()
        for name, text in REQUIRED_FILES.items():
            (candidate / name).write_text(text, encoding="utf-8")
        (candidate / "src" / "rt").mkdir(parents=True)
        (candidate / "src" / "rt" / "safe.c").write_bytes(SYNTHETIC_C.encode())
        (candidate / "tools").mkdir(exist_ok=True)
        (candidate / "tools" / "safe.py").write_bytes(SYNTHETIC_PY.encode())
        # A well-formed candidate carries a release manifest that accounts for every
        # policy-excluded path. Tests that exercise reconciliation failures overwrite it.
        (candidate / "assets").mkdir(exist_ok=True)
        (candidate / "assets" / "release_manifest.json").write_text(
            json.dumps({
                "name": "synthetic",
                "components": [
                    {"id": p.replace("/", "-").replace(".", "-"), "source_path": p,
                     "type": "source", "presence": "excluded_from_public_profile",
                     "license": "NOASSERTION", "disposition": "excluded_pending_qualified_review",
                     "public_scope_included": False, "optional": True}
                    for p in INCIDENT_PATHS
                ],
            }, indent=2),
            encoding="utf-8",
        )
        for rel, blob in (extra_files or {}).items():
            target = candidate / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(blob)
        return candidate

    def build_policy(self, candidate: Path, **overrides) -> Path:
        include = sorted(
            {p.relative_to(candidate).as_posix() for p in candidate.rglob("*") if p.is_file()}
            # PUBLIC_EXPORT.json is written below, after the enumeration, so name it
            # explicitly rather than letting the clean candidate trip its own gate.
            | {"PUBLIC_EXPORT.json", "assets/public_source_profile.json",
               "assets/public_provenance_ledger.json"}
        )
        document = {
            "name": "synthetic-test-profile",
            "profile_version": "2.0.0",
            "min_tool_version": "0.4.0",
            "build_mode": "PUBLIC_SAFE=1",
            "default_disposition": "REJECT",
            "exclude_prefixes": [],
            "exclude_globs": ["font/*.pgf"],
            "exclude_paths": [p for p in INCIDENT_PATHS if not p.endswith(".pgf")],
            "include_paths": [p for p in include if p not in INCIDENT_PATHS and not p.endswith(".pgf")],
        }
        document.update(overrides)
        policy_path = candidate.parent / "synthetic_policy.json"
        policy_path.write_text(json.dumps(document, indent=2), encoding="utf-8")
        try:
            policy_obj = load_policy(policy_path)
        except PolicyError:
            # Negative policy-integrity tests deliberately construct an invalid
            # profile; the auditor must be the component that reports it.
            return policy_path
        (candidate / "assets" / "public_source_profile.json").write_text(
            json.dumps(document, indent=2), encoding="utf-8"
        )
        ledger_entries = []
        for rel in document["include_paths"]:
            if rel == "assets/public_provenance_ledger.json":
                continue
            raw = (candidate / rel).read_bytes() if (candidate / rel).is_file() else b""
            ledger_entries.append({
                "path": rel,
                "classification": "synthetic_fixture",
                "evidence": {"source": "self-contained synthetic publication fixture"},
                "sha256": hashlib.sha256(raw).hexdigest(),
            })
        ledger_entries.append({
            "path": "assets/public_provenance_ledger.json",
            "classification": "reviewed_configuration",
            "evidence": {"source": "self-contained synthetic publication fixture"},
        })
        if not any(e["path"] == "PUBLIC_EXPORT.json" for e in ledger_entries):
            ledger_entries.append({
                "path": "PUBLIC_EXPORT.json",
                "classification": "generated_from_public_source",
                "evidence": {"source": "self-contained synthetic export generator"},
            })
        ledger = {"schema_version": 1, "entries": ledger_entries}
        ledger_path = candidate / "assets" / "public_provenance_ledger.json"
        ledger_path.write_text(json.dumps(ledger, indent=2), encoding="utf-8")
        files = [(p.relative_to(candidate).as_posix(), p.read_bytes())
                 for p in candidate.rglob("*") if p.is_file()]
        files.append(("PUBLIC_EXPORT.json", b""))
        export_document = build_document(
            policy_obj, files, provenance_ledger=ledger_path.read_bytes(),
            manifest=(candidate / "assets" / "release_manifest.json").read_bytes(),
        )
        write_document(candidate / "PUBLIC_EXPORT.json", export_document)
        return policy_path


class TestIncidentPathsRejected(FixtureMixin, unittest.TestCase):
    """The complete 2026-08-11 path set must be rejected, in every mode."""

    def _run_with_all_incident_paths(self, *extra: str) -> subprocess.CompletedProcess:
        candidate = self.build_candidate({p: content_for(p) for p in INCIDENT_PATHS})
        policy = self.build_policy(candidate)
        return run_audit(candidate, *extra, policy=policy)

    def test_all_fifteen_rejected_in_default_mode(self):
        """Regression for the original defect: default mode reported OK on this tree."""
        result = self._run_with_all_incident_paths()
        self.assertEqual(result.returncode, 1, "audit must fail on the incident path set")
        missed = [p for p in INCIDENT_PATHS if "POLICY_EXCLUDED_PRESENT" not in codes_for(result, p)]
        self.assertEqual(missed, [], f"excluded paths not rejected in default mode: {missed}")

    def test_all_fifteen_rejected_under_public_scope(self):
        result = self._run_with_all_incident_paths("--public-scope")
        self.assertEqual(result.returncode, 1)
        missed = [p for p in INCIDENT_PATHS if "POLICY_EXCLUDED_PRESENT" not in codes_for(result, p)]
        self.assertEqual(missed, [], f"excluded paths not rejected under --public-scope: {missed}")

    def test_zero_missed_excluded_paths(self):
        """The count itself is the contract: 15 of 15, not 6 of 15."""
        result = self._run_with_all_incident_paths()
        rejected = {p for code, p in findings_of(result) if code == "POLICY_EXCLUDED_PRESENT"}
        self.assertEqual(len(rejected & set(INCIDENT_PATHS)), len(INCIDENT_PATHS))

    def test_single_excluded_path_is_enough_to_fail(self):
        for path in INCIDENT_PATHS:
            with self.subTest(path=path):
                candidate = self.build_candidate({path: content_for(path)})
                policy = self.build_policy(candidate)
                result = run_audit(candidate, policy=policy)
                self.assertEqual(result.returncode, 1, f"{path} alone must fail the audit")
                self.assertIn("POLICY_EXCLUDED_PRESENT", codes_for(result, path))


class TestUnknownPathsRejected(FixtureMixin, unittest.TestCase):
    """Unknown means reject. Living under src/ or tools/ grants nothing."""

    UNKNOWN = {
        "src/rt/newthing.h": SYNTHETIC_C.encode(),
        "src/rt/newthing.c": SYNTHETIC_C.encode(),
        "tools/newthing.py": SYNTHETIC_PY.encode(),
        "assets/newthing.dat": b"\x01\x02\x03\x04synthetic\x00\x00",
    }

    def test_unlisted_paths_are_rejected(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)  # built before the unknown files exist
        for rel, blob in self.UNKNOWN.items():
            target = candidate / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(blob)
        result = run_audit(candidate, policy=policy)
        self.assertEqual(result.returncode, 1, "unknown paths must fail the audit")
        for rel in self.UNKNOWN:
            with self.subTest(path=rel):
                self.assertIn("POLICY_UNCLASSIFIED", codes_for(result, rel))

    def test_pgf_magic_at_an_unlisted_path_is_detected(self):
        """A font blob renamed into an innocuous path must not slip through."""
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)
        (candidate / "assets" / "ui").mkdir(parents=True, exist_ok=True)
        (candidate / "assets" / "ui" / "theme.dat").write_bytes(SYNTHETIC_PGF)
        result = run_audit(candidate, policy=policy)
        self.assertEqual(result.returncode, 1)
        codes = codes_for(result, "assets/ui/theme.dat")
        self.assertIn("POLICY_UNCLASSIFIED", codes)

    def test_pgf_magic_is_recognised_at_the_real_offset(self):
        """The signature sits at offset 4, after a header word."""
        from publish_audit import _magic_kind

        self.assertEqual(_magic_kind(SYNTHETIC_PGF, "x.dat"), "PSP PGF font")

    def test_magic_check_survives_explicit_inclusion(self):
        """Defence in depth: an included path with font magic is still reported."""
        from publish_audit import _magic_kind

        candidate = self.build_candidate({"assets/theme.dat": SYNTHETIC_PGF})
        policy = self.build_policy(candidate)  # includes assets/theme.dat
        self.assertEqual(_magic_kind(SYNTHETIC_PGF, "assets/theme.dat"), "PSP PGF font")
        result = run_audit(candidate, policy=policy)
        self.assertIn("MAGIC", codes_for(result, "assets/theme.dat"))
        self.assertEqual(result.returncode, 1)


class TestPolicyIntegrity(FixtureMixin, unittest.TestCase):
    """Contradictory, stale or unsupported policy states fail closed."""

    def test_unsupported_profile_version_fails(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate, profile_version="99.0.0")
        result = run_audit(candidate, policy=policy)
        self.assertEqual(result.returncode, 1)
        self.assertIn("POLICY_VERSION_UNSUPPORTED", {c for c, _ in findings_of(result)})

    def test_tool_older_than_min_tool_version_fails(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate, min_tool_version="99.0.0")
        result = run_audit(candidate, policy=policy)
        self.assertEqual(result.returncode, 1)
        self.assertIn("POLICY_VERSION_UNSUPPORTED", {c for c, _ in findings_of(result)})

    def test_non_reject_default_disposition_is_refused(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate, default_disposition="included")
        with self.assertRaises(PolicyError):
            load_policy(policy)
        result = run_audit(candidate, policy=policy)
        self.assertEqual(result.returncode, 1)
        self.assertIn("POLICY_UNREADABLE", {c for c, _ in findings_of(result)})

    def test_path_in_both_lists_is_refused(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)
        document = json.loads(policy.read_text(encoding="utf-8"))
        document["include_paths"].append("src/rt/pgd.c")
        policy.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaises(PolicyError):
            load_policy(policy)

    def test_include_path_shadowed_by_exclude_glob_is_refused(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)
        document = json.loads(policy.read_text(encoding="utf-8"))
        document["include_paths"].append("font/newfont.pgf")
        policy.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaises(PolicyError):
            load_policy(policy)

    def test_missing_policy_file_fails_closed(self):
        candidate = self.build_candidate()
        result = run_audit(candidate, policy=candidate.parent / "does_not_exist.json")
        self.assertEqual(result.returncode, 1)
        self.assertIn("POLICY_UNREADABLE", {c for c, _ in findings_of(result)})

    def test_stale_export_digest_fails(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)
        (candidate / "PUBLIC_EXPORT.json").write_text(
            json.dumps({"profile": "synthetic-test-profile", "policy_sha256": "0" * 64}),
            encoding="utf-8",
        )
        result = run_audit(candidate, policy=policy)
        self.assertEqual(result.returncode, 1)
        self.assertIn("POLICY_EXPORT_STALE", {c for c, _ in findings_of(result)})

    def test_policy_edited_after_export_generation_fails(self):
        """Regenerating nothing, then editing the policy, must invalidate the export."""
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)
        document = json.loads(policy.read_text(encoding="utf-8"))
        document["exclude_paths"].append("src/rt/something_new.c")
        policy.write_text(json.dumps(document), encoding="utf-8")
        result = run_audit(candidate, policy=policy)
        self.assertIn("POLICY_EXPORT_STALE", {c for c, _ in findings_of(result)})

    def test_missing_export_fails(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)
        (candidate / "PUBLIC_EXPORT.json").unlink()
        result = run_audit(candidate, policy=policy)
        self.assertEqual(result.returncode, 1)
        self.assertIn("POLICY_EXPORT_MISSING", {c for c, _ in findings_of(result)})

    def test_digest_ignores_formatting_but_not_content(self):
        base = {"a": 1, "paths": ["x", "y"]}
        self.assertEqual(canonical_digest(base), canonical_digest({"paths": ["x", "y"], "a": 1}))
        self.assertNotEqual(canonical_digest(base), canonical_digest({"a": 1, "paths": ["x", "z"]}))

    def test_candidate_supplied_policy_is_not_trusted(self):
        """The tree under audit must not be able to supply the rules it is gated by."""
        candidate = self.build_candidate({p: content_for(p) for p in INCIDENT_PATHS})
        policy = self.build_policy(candidate)
        # Plant a permissive policy inside the candidate, as a materialized export
        # legitimately carries one.
        weakened = json.loads(policy.read_text(encoding="utf-8"))
        weakened["exclude_paths"] = []
        weakened["exclude_globs"] = []
        weakened["include_paths"] = sorted(
            p.relative_to(candidate).as_posix() for p in candidate.rglob("*") if p.is_file()
        )
        (candidate / "assets").mkdir(exist_ok=True)
        (candidate / "assets" / "public_source_profile.json").write_text(
            json.dumps(weakened), encoding="utf-8"
        )
        result = run_audit(candidate, policy=policy)
        self.assertEqual(result.returncode, 1, "the planted permissive policy must not weaken the gate")
        missed = [p for p in INCIDENT_PATHS if "POLICY_EXCLUDED_PRESENT" not in codes_for(result, p)]
        self.assertEqual(missed, [])

    def test_candidate_cannot_request_a_relaxed_private_mode(self):
        candidate = self.build_candidate({"src/rt/secret.bin": b"private\n"})
        policy = self.build_policy(candidate)
        result = run_audit(candidate, "--private-development-tree", policy=policy)
        self.assertEqual(result.returncode, 2)
        self.assertIn("unrecognized arguments", result.stderr)

    def test_candidate_hook_configuration_cannot_determine_server_clearance(self):
        candidate = self.build_candidate({
            "src/rt/private.bin": b"private\n",
            "src/rt/pgd.c": SYNTHETIC_C.encode(),
            ".pre-commit-config.yaml": b"entry: python tools/publish_audit.py --private-development-tree\n",
        })
        policy = self.build_policy(candidate)
        result = run_audit(candidate, policy=policy)
        self.assertEqual(result.returncode, 1)
        self.assertIn("POLICY_EXCLUDED_PRESENT", {c for c, _ in findings_of(result)})

    def test_policy_inclusion_cannot_self_authorize_provenance(self):
        """Sol's mutation: add a source, include it, refresh export, omit ledger evidence."""
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)
        trusted_ledger = candidate.parent / "trusted-ledger.json"
        trusted_ledger.write_bytes((candidate / "assets" / "public_provenance_ledger.json").read_bytes())
        new_path = candidate / "src" / "rt" / "new_source.c"
        new_path.write_bytes(SYNTHETIC_C.encode())
        document = json.loads(policy.read_text(encoding="utf-8"))
        document["include_paths"].append("src/rt/new_source.c")
        policy.write_text(json.dumps(document, indent=2), encoding="utf-8")
        (candidate / "assets" / "public_source_profile.json").write_text(
            json.dumps(document, indent=2), encoding="utf-8"
        )
        policy_obj = load_policy(policy)
        files = [(p.relative_to(candidate).as_posix(), p.read_bytes())
                 for p in candidate.rglob("*") if p.is_file() and p.name != "PUBLIC_EXPORT.json"]
        files.append(("PUBLIC_EXPORT.json", b""))
        write_document(candidate / "PUBLIC_EXPORT.json", build_document(policy_obj, files,
            provenance_ledger=(candidate / "assets" / "public_provenance_ledger.json").read_bytes(),
            manifest=(candidate / "assets" / "release_manifest.json").read_bytes()))
        result = run_audit(candidate, "--provenance-ledger", str(trusted_ledger), policy=policy)
        self.assertEqual(result.returncode, 1)
        self.assertIn("PROVENANCE_MISSING", {c for c, _ in findings_of(result)})


class TestManifestReconciliation(FixtureMixin, unittest.TestCase):
    """Policy and release manifest may not contradict each other."""

    def _manifest(self, candidate: Path, components: list[dict]) -> Path:
        path = candidate / "assets" / "release_manifest.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"name": "synthetic", "components": components}, indent=2),
                        encoding="utf-8")
        return path

    def test_excluded_path_missing_from_manifest_fails(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)
        manifest = self._manifest(candidate, [])
        result = run_audit(candidate, "--manifest", str(manifest), policy=policy)
        self.assertEqual(result.returncode, 1)
        self.assertIn("POLICY_MANIFEST_MISSING", {c for c, _ in findings_of(result)})

    def test_manifest_claiming_public_scope_for_excluded_path_fails(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)
        components = [
            {"id": p.replace("/", "-"), "source_path": p, "type": "source",
             "public_scope_included": True, "disposition": "included"}
            for p in INCIDENT_PATHS if not p.endswith(".pgf")
        ]
        manifest = self._manifest(candidate, components)
        result = run_audit(candidate, "--manifest", str(manifest), policy=policy)
        self.assertEqual(result.returncode, 1)
        self.assertIn("POLICY_MANIFEST_CONFLICT", {c for c, _ in findings_of(result)})


class TestTreeBinding(unittest.TestCase):
    """An audit of one tree must not be presentable as clearance for another."""

    def test_mismatched_expected_tree_fails(self):
        result = subprocess.run(
            [sys.executable, str(AUDIT), "--tracked-only", "--expect-tree", "0" * 40],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("POLICY_TREE_MISMATCH", {c for c, _ in findings_of(result)})

    def test_matching_expected_tree_passes(self):
        tree = subprocess.run(["git", "write-tree"], cwd=ROOT,
                              capture_output=True, text=True, check=True).stdout.strip()
        result = subprocess.run(
            [sys.executable, str(AUDIT), "--tracked-only", "--expect-tree", tree],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertNotIn("POLICY_TREE_MISMATCH", {c for c, _ in findings_of(result)})

    def _scratch_repo(self) -> Path:
        root = Path(self.enterContext(__import__("tempfile").TemporaryDirectory())) / "repo"
        root.mkdir()
        for argv in (("init", "-q", "."), ("config", "user.email", "t@example.invalid"),
                     ("config", "user.name", "test")):
            subprocess.run(["git", *argv], cwd=root, check=True, capture_output=True)
        (root / "a.txt").write_text("a\n", encoding="utf-8")
        subprocess.run(["git", "add", "a.txt"], cwd=root, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "one"], cwd=root, check=True, capture_output=True)
        return root

    def _git(self, root: Path, *argv: str) -> str:
        return subprocess.run(["git", *argv], cwd=root, check=True,
                              capture_output=True, text=True).stdout.strip()

    def _bind(self, root: Path, tree: str, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(AUDIT), "--repo-root", str(root), "--tracked-only", "--expect-tree", tree, *extra],
            cwd=root, capture_output=True, text=True,
        )

    def test_safe_head_unsafe_index_is_not_cleared_by_head(self):
        root = self._scratch_repo()
        head_tree = self._git(root, "rev-parse", "HEAD^{tree}")
        (root / "smuggled.bin").write_bytes(b"\x00\x00\x88\x01PGF0")
        subprocess.run(["git", "add", "smuggled.bin"], cwd=root, check=True, capture_output=True)
        index_tree = self._git(root, "write-tree")
        self.assertNotEqual(head_tree, index_tree)
        result = self._bind(root, head_tree)
        self.assertIn("POLICY_TREE_MISMATCH", {c for c, _ in findings_of(result)})

    def test_unsafe_head_safe_index_binds_to_the_index(self):
        root = self._scratch_repo()
        (root / "smuggled.bin").write_bytes(b"\x00\x00\x88\x01PGF0")
        subprocess.run(["git", "add", "smuggled.bin"], cwd=root, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "two"], cwd=root, check=True, capture_output=True)
        head_tree = self._git(root, "rev-parse", "HEAD^{tree}")
        subprocess.run(["git", "rm", "-q", "--cached", "smuggled.bin"], cwd=root, check=True, capture_output=True)
        index_tree = self._git(root, "write-tree")
        self.assertNotIn("POLICY_TREE_MISMATCH", {c for c, _ in findings_of(self._bind(root, index_tree))})
        self.assertIn("POLICY_TREE_MISMATCH", {c for c, _ in findings_of(self._bind(root, head_tree))})

    def test_worktree_audit_cannot_be_tree_bound(self):
        root = self._scratch_repo()
        index_tree = self._git(root, "write-tree")
        (root / "a.txt").write_text("modified on disk only\n", encoding="utf-8")
        result = self._bind(root, index_tree, "--worktree")
        self.assertIn("POLICY_TREE_UNBINDABLE", {c for c, _ in findings_of(result)})


class TestPositiveControls(FixtureMixin, unittest.TestCase):
    """The gate must pass known-safe trees, or it proves nothing by failing."""

    def test_clean_synthetic_candidate_passes(self):
        candidate = self.build_candidate()
        policy = self.build_policy(candidate)
        result = run_audit(candidate, policy=policy)
        self.assertEqual(result.returncode, 0,
                         f"clean candidate must pass; stderr:\n{result.stderr}")

    def test_explicitly_included_source_resolves_included(self):
        policy = load_policy(CANONICAL_POLICY)
        for path in ("src/rt/ge.c", "tools/publish_audit.py", "NOTICE.md", "LICENSE"):
            with self.subTest(path=path):
                self.assertEqual(policy.resolve(path).disposition, publication_policy.INCLUDED)

    def test_canonical_policy_loads_and_excludes_the_incident_set(self):
        policy = load_policy(CANONICAL_POLICY)
        for path in INCIDENT_PATHS:
            with self.subTest(path=path):
                self.assertTrue(policy.resolve(path).is_excluded,
                                f"{path} must be excluded by the canonical policy")

    def test_canonical_policy_rejects_unknown_paths(self):
        policy = load_policy(CANONICAL_POLICY)
        for path in ("src/rt/newthing.h", "tools/newthing.py", "docs/whatever_new.md"):
            with self.subTest(path=path):
                self.assertTrue(policy.resolve(path).is_unclassified)

    def test_private_development_tree_is_not_public_clearance(self):
        candidate = self.build_candidate({p: content_for(p) for p in INCIDENT_PATHS})
        policy = self.build_policy(candidate)
        for extra in ([], ["--public-scope"], ["--public-scope", "--worktree"]):
            with self.subTest(mode=" ".join(extra) or "default"):
                result = run_audit(candidate, *extra, policy=policy)
                if "--worktree" in extra:
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("cannot be combined", result.stderr)
                    continue
                self.assertEqual(result.returncode, 1,
                                 f"private tree must not pass public clearance; stderr:\n{result.stderr}")
                self.assertIn("POLICY_EXCLUDED_PRESENT", {c for c, _ in findings_of(result)})


if __name__ == "__main__":
    unittest.main()
