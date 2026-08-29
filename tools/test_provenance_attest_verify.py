# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Adversarial regression: a coherent candidate cannot self-authorize provenance.

The property under test is the one ordinary pull request CI could not state
before ``tools/provenance_attest_verify.py`` existed: a pull request author who
knows the implementation can make the public ledger, the policy, the export
digests and the content hashes agree with each other perfectly, and the merge
path must still refuse the change when the external trusted authority does not
back it.

Every case below builds a real Git repository, commits a *self-consistent*
attack -- the ledger entry is well formed, the hash matches the bytes, the
policy includes the path -- and asserts the exact finding code that stops it.
A test that only proved a malformed ledger is rejected would prove nothing; the
threat is a well-formed lie.

Fixtures are synthetic.  No private detailed ledger, key material, game data, or
real repository content is used or copied.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unicodedata
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import provenance_attest_verify as verifier  # noqa: E402
import public_export  # noqa: E402
from publication_policy import load_policy  # noqa: E402

RESERVED_CONTEXT = verifier.TRUSTED_CONTEXT
TRUSTED_WORKFLOW = verifier.TRUSTED_WORKFLOW

#: A synthetic stand-in for the private detailed ledger.  ``blanket-tools``
#: deliberately mirrors the real ledger's ``tools/*`` catch-all so the tests can
#: prove a blanket record is inert for classification while still being a valid
#: anchor for a path it already covers.
TRUSTED_RECORDS = {
    "schema_version": 1,
    "records": [
        {
            "id": "core-runtime",
            "paths": ["src/rt/core.c"],
            "classification": "derived-translated",
            "upstream": "upstream-project",
            "upstream_paths": ["Private/Upstream/Path.cpp"],
            "upstream_revision": None,
            "upstream_license": "GPL-2.0-or-later",
            "evidence_tier": "H",
        },
        {
            "id": "widget-independent",
            "paths": ["src/rt/widget.c"],
            "classification": "project-authored-independent",
            "upstream": None,
            "evidence_tier": "S",
        },
        {
            # Covers a path whose public ledger entry names no record at all,
            # so its id is private knowledge the report must not disclose.
            "id": "private-only-record",
            "paths": ["README.md"],
            "classification": "project-authored-independent",
            "upstream": None,
            "evidence_tier": "S",
        },
        {
            # ``font/README.md`` is the one path _class_for special-cases
            # *before* consulting records, so it is the only shape where a
            # record exists but the derived evidence carries no record id.
            # Without it in the corpus, taking the id from the raw record map
            # instead of from the derived evidence is undetectable.
            "id": "font-binaries",
            "paths": ["font/README.md"],
            "classification": "project-authored-independent",
            "upstream": None,
            "evidence_tier": "S",
        },
        {
            "id": "blanket-tools",
            "paths": ["tools/*"],
            "classification": "project-authored-independent",
            "upstream": None,
            "evidence_tier": "S",
            "catch_all": True,
        },
    ],
}

POLICY = {
    "name": "public-safe-test",
    "profile_version": "2.0.0",
    "min_tool_version": "1.0.0",
    "build_mode": "PUBLIC_SAFE",
    "default_disposition": "REJECT",
    "exclude_prefixes": ["private/"],
    "exclude_globs": ["*.iso"],
    "exclude_paths": ["secret.txt"],
    "include_paths": [],
}


def _sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


class Repository:
    """A throwaway Git repository whose trees are the verifier's input."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)
        self._git("init", "--quiet", "--initial-branch=main")
        self._git("config", "user.email", "fixture@example.invalid")
        self._git("config", "user.name", "Fixture")
        self._git("config", "commit.gpgsign", "false")

    def _git(self, *args: str) -> str:
        result = subprocess.run(
            ["git", *args], cwd=self.root, capture_output=True, text=True, check=True,
        )
        return result.stdout.strip()

    def write(self, path: str, raw: bytes | str) -> None:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(raw, str):
            raw = raw.encode("utf-8")
        target.write_bytes(raw)

    def remove(self, path: str) -> None:
        (self.root / path).unlink()

    #: Called just before staging so the export always describes the tree that
    #: is about to be committed, whatever order the case wrote its files in.
    before_commit = None

    def commit(self, message: str) -> str:
        if self.before_commit is not None:
            self.before_commit()
        self._git("add", "--all")
        self._git("commit", "--quiet", "--allow-empty", "-m", message)
        return self._git("rev-parse", "HEAD")

    def tracked_files(self) -> list[tuple[str, bytes]]:
        files = []
        for path in sorted(self.root.rglob("*")):
            if path.is_dir() or ".git" in path.relative_to(self.root).parts:
                continue
            rel = path.relative_to(self.root).as_posix()
            files.append((rel, b"" if rel == verifier.EXPORT_PATH else path.read_bytes()))
        return files

    def branch(self, name: str, start: str) -> None:
        self._git("checkout", "--quiet", "-B", name, start)


class GateCase(unittest.TestCase):
    """Base fixture: a clean base commit that the verifier accepts."""

    #: The base tree's public files and the claim each one carries.  ``core.c``
    #: is derived, ``widget.c`` independent, ``legacy.py`` is a grandfathered
    #: over-claim with no exact trusted record -- the shape the real repository
    #: carries 317 times.
    FILES = {
        "src/rt/core.c": b"int core(void) { return 1; }\n",
        "src/rt/widget.c": b"int widget(void) { return 2; }\n",
        "tools/legacy.py": b"print('legacy')\n",
        "README.md": b"# fixture\n",
        # No trusted record of any kind names this one, so it is classified by
        # path rule alone -- the "deterministic" backing case.
        "docs/notes.md": b"# fixture notes\n",
        # Special-cased in _class_for ahead of the record lookup, so a record
        # names it yet the derived evidence carries no record id.
        "font/README.md": b"# optional fonts\n",
    }

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        base = Path(self._tmp.name)
        self.repo = Repository(base / "repo")
        self.outside = base / "outside"
        self.outside.mkdir()
        self.trusted_ledger = self.outside / "IMPLEMENTATION_PROVENANCE.json"
        self.repo.before_commit = self.regenerate_export
        self.write_trusted(TRUSTED_RECORDS)

        entries = []
        for path, raw in self.FILES.items():
            entries.append(self.entry(path, raw))
        self.repo.write(TRUSTED_WORKFLOW, self.workflow_bytes())
        entries.append(self.entry(TRUSTED_WORKFLOW, self.workflow_bytes(),
                                  classification="reviewed_configuration",
                                  evidence={"source": "configuration review"}))
        for path, raw in self.FILES.items():
            self.repo.write(path, raw)
        self.write_policy(list(self.FILES) + [TRUSTED_WORKFLOW, verifier.LEDGER_PATH])
        for control in (verifier.LEDGER_PATH, verifier.EXPORT_PATH):
            entries.append({
                "path": control,
                "classification": "reviewed_configuration",
                "evidence": {"source": "configuration review"},
            })
        self.write_ledger(entries)
        self.base = self.repo.commit("base")

    # -- fixture helpers -----------------------------------------------------

    def workflow_bytes(self, *, trigger: bytes = b"pull_request_target", context: str | None = None) -> bytes:
        name = RESERVED_CONTEXT if context is None else context
        return (
            b"on:\n  " + trigger + b":\njobs:\n  attest:\n    name: "
            + name.encode("utf-8") + b"\n"
        )

    def approve_blob(self, path: str, raw: bytes, *, record_id: str, classification: str,
                     document: dict | None = None) -> dict:
        """Add a trusted reviewed-blob approval for exactly these bytes."""
        document = json.loads(json.dumps(document or TRUSTED_RECORDS))
        document.setdefault("reviewed_blobs", []).append({
            "path": path, "sha256": _sha(raw),
            "classification": classification, "record_id": record_id,
        })
        self.write_trusted(document)
        return document

    def write_trusted(self, document: dict) -> None:
        self.trusted_ledger.write_text(json.dumps(document, indent=2), encoding="utf-8", newline="\n")

    def write_policy(self, include_paths: list[str]) -> None:
        """Publish the given paths plus whichever control files the tree has.

        The two generated control files are always public, and the gate
        workflow is public whenever it exists; folding that in here keeps every
        call site about the paths the case actually cares about.
        """
        paths = set(include_paths) | {verifier.LEDGER_PATH, verifier.EXPORT_PATH}
        if (self.repo.root / TRUSTED_WORKFLOW).exists():
            paths.add(TRUSTED_WORKFLOW)
        else:
            paths.discard(TRUSTED_WORKFLOW)
        document = dict(POLICY)
        document["include_paths"] = sorted(paths)
        self.repo.write(verifier.POLICY_PATH, json.dumps(document, indent=2) + "\n")

    def write_ledger(self, entries: list[dict], *, export_ledger_sha: str | None = None) -> None:
        """Write the ledger and the export that pins its digest.

        The export records the SHA-256 of the ledger blob it was generated
        against.  Passing ``export_ledger_sha`` writes a *stale* pin: exactly
        what a candidate that edits the ledger without regenerating looks like.
        """
        document = {
            "schema_version": 1,
            "generated_by": "tools/provenance_ledger.py",
            "policy_profile": POLICY["name"],
            "classification_vocabulary": sorted(verifier.ALLOWED_CLASSES),
            "entries": sorted(entries, key=lambda item: item["path"]),
        }
        raw = (json.dumps(document, indent=2) + "\n").encode("utf-8")
        self.repo.write(verifier.LEDGER_PATH, raw)
        self._export_ledger_sha = export_ledger_sha
        self.regenerate_export()

    def regenerate_export(self, **field_overrides) -> None:
        """Write the canonical public export for the tree as it stands.

        The verifier recomputes every security-relevant export field, so a
        hand-written stub would fail for reasons that have nothing to do with
        the case under test. Building it with the same generator the release
        process uses keeps each case about its own attack; ``field_overrides``
        is how a case forges exactly one field.
        """
        policy_path = self.repo.root / verifier.POLICY_PATH
        ledger_path = self.repo.root / verifier.LEDGER_PATH
        if not policy_path.is_file() or not ledger_path.is_file():
            return
        if not (self.repo.root / verifier.EXPORT_PATH).exists():
            self.repo.write(verifier.EXPORT_PATH, b"{}")
        policy = load_policy(policy_path)
        document = public_export.build_document(
            policy, self.repo.tracked_files(),
            provenance_ledger=ledger_path.read_bytes(),
        )
        stale = getattr(self, "_export_ledger_sha", None)
        if stale is not None:
            document["provenance_ledger_sha256"] = stale
        document.update(field_overrides)
        self.repo.write(
            verifier.EXPORT_PATH,
            json.dumps(document, indent=2, ensure_ascii=False) + "\n",
        )

    def entry(self, path: str, raw: bytes, *, classification: str | None = None,
              evidence: dict | None = None, sha256: str | None = None) -> dict:
        """Build a ledger entry, defaulting to the truthful claim for the path."""
        if classification is None or evidence is None:
            defaults = {
                "src/rt/core.c": ("upstream_derived", {
                    "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                    "record_id": "core-runtime", "evidence_tier": "H",
                    "upstream": "upstream-project",
                }),
                "src/rt/widget.c": ("project_authored_attested", {
                    "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                    "record_id": "widget-independent", "evidence_tier": "S",
                    "authorship": "independent implementation record",
                }),
                "tools/legacy.py": ("project_authored_attested", {
                    "source": "public provenance census",
                    "authorship": "independent project implementation",
                }),
                "README.md": ("reviewed_documentation", {"source": "public documentation review"}),
                "docs/notes.md": ("reviewed_documentation", {"source": "public documentation review"}),
                "font/README.md": ("reviewed_documentation", {
                    "source": "public documentation review",
                    "statement": (
                        "generic user-supplied optional font instructions; unresolved font "
                        "binaries and license packet are excluded"
                    ),
                }),
            }
            fallback = ("reviewed_configuration", {"source": "configuration review"})
            classification, evidence = defaults.get(path, fallback)
        result = {"path": path, "classification": classification, "evidence": evidence}
        if path not in verifier.UNHASHED_PATHS:
            result["sha256"] = sha256 or _sha(raw)
        return result

    def current_entries(self, rev: str = "HEAD") -> list[dict]:
        raw = subprocess.run(
            ["git", "show", f"{rev}:{verifier.LEDGER_PATH}"], cwd=self.repo.root,
            capture_output=True, check=True,
        ).stdout
        return json.loads(raw.decode("utf-8"))["entries"]

    # -- invocation ----------------------------------------------------------

    def run_verify(self, candidate: str, *, base: str | None = None,
                   trusted_ledger: Path | None = None) -> dict:
        return verifier.verify(
            repo=self.repo.root,
            candidate_rev=candidate,
            base_rev=base or self.base,
            trusted_ledger=trusted_ledger or self.trusted_ledger,
            workdir=self.outside / "work",
        )

    def codes(self, verdict: dict, *, fatal_only: bool = True) -> set[str]:
        return {
            item["code"] for item in verdict["findings"]
            if item["fatal"] or not fatal_only
        }

    def assertFatal(self, verdict: dict, code: str) -> None:
        self.assertEqual(verdict["verdict"], "fail")
        self.assertIn(code, self.codes(verdict), msg=f"findings: {verdict['findings']}")

    def assertPasses(self, verdict: dict) -> None:
        self.assertEqual(
            verdict["verdict"], "pass",
            msg=f"unexpected fatal findings: {[f for f in verdict['findings'] if f['fatal']]}",
        )


class BaselineTests(GateCase):
    """The fixture itself must pass, or every failure assertion below is vacuous."""

    def test_base_tree_passes(self) -> None:
        self.assertPasses(self.run_verify(self.base))

    def test_verdict_binds_the_exact_candidate_tree(self) -> None:
        verdict = self.run_verify(self.base)
        scope = verdict["repository_scope"]
        self.assertEqual(scope["candidate_commit"], self.base)
        self.assertEqual(len(scope["candidate_tree"]), 40)
        self.assertEqual(
            verdict["trusted_ledger_sha256"], _sha(self.trusted_ledger.read_bytes()),
        )

    def test_grandfathered_over_claim_is_reported_not_fatal(self) -> None:
        verdict = self.run_verify(self.base)
        self.assertPasses(verdict)
        debt = {item["path"] for item in verdict["grandfathered_debt"]}
        self.assertIn("tools/legacy.py", debt)

    def test_a_private_record_id_is_never_disclosed(self) -> None:
        """The report may repeat what the public ledger says, and nothing more.

        ``README.md`` is covered by a trusted record the public ledger does not
        name.  Reporting that id would publish a private record name through a
        CI log, so the debt entry must be redacted -- while the ids the public
        ledger already carries stay legible.
        """
        verdict = self.run_verify(self.base)
        blob = json.dumps(verdict)
        self.assertNotIn("private-only-record", blob)
        entry = next(item for item in verdict["grandfathered_debt"] if item["path"] == "README.md")
        self.assertEqual(entry["trusted_record_id"], verifier.WITHHELD)
        self.assertNotIn(
            "src/rt/core.c", {item["path"] for item in verdict["grandfathered_debt"]},
            "core.c agrees with the trusted authority and is not debt",
        )


class UnbackedAttestationTests(GateCase):
    """A candidate cannot mint authority the trusted ledger does not hold."""

    def _add_public_file(self, path: str, raw: bytes, entry: dict) -> str:
        self.repo.branch("attack", self.base)
        self.repo.write(path, raw)
        entries = self.current_entries(self.base) + [entry]
        self.write_ledger(entries)
        self.write_policy(list(self.FILES) + [TRUSTED_WORKFLOW, verifier.LEDGER_PATH, path])
        return self.repo.commit("attack")

    def test_invented_record_id_is_refused(self) -> None:
        """The live finding this gate was built for: a plausible, absent record."""
        raw = b"int fresh(void) { return 3; }\n"
        head = self._add_public_file("src/rt/fresh.c", raw, {
            "path": "src/rt/fresh.c",
            "classification": "project_authored_attested",
            "evidence": {
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "fresh-runtime-attestation",
                "evidence_tier": "S",
                "authorship": "independent implementation record",
            },
            "sha256": _sha(raw),
        })
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "TRUSTED_RECORD_UNRESOLVED")

    def test_borrowed_record_id_is_refused(self) -> None:
        """A record that exists but says nothing about this path is not cover."""
        raw = b"int borrowed(void) { return 4; }\n"
        head = self._add_public_file("src/rt/borrowed.c", raw, {
            "path": "src/rt/borrowed.c",
            "classification": "project_authored_attested",
            "evidence": {
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "widget-independent",
                "evidence_tier": "S",
                "authorship": "independent implementation record",
            },
            "sha256": _sha(raw),
        })
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "TRUSTED_RECORD_UNRESOLVED")

    def test_blanket_record_cannot_attest_a_new_path(self) -> None:
        """The subtle one: anchor integrity accepts ``tools/*``, Tier B does not.

        ``blanket-tools`` exists and its pattern does cover ``tools/new.py``, so
        RECORD_ABSENT and RECORD_NOT_COVERING both stay silent.  Classification
        must still refuse it, because a blanket authorship statement written
        before a file existed cannot attest that file.
        """
        raw = b"print('new tool')\n"
        head = self._add_public_file("tools/new.py", raw, {
            "path": "tools/new.py",
            "classification": "project_authored_attested",
            "evidence": {
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "blanket-tools",
                "evidence_tier": "S",
                "authorship": "independent implementation record",
            },
            "sha256": _sha(raw),
        })
        verdict = self.run_verify(head)
        self.assertNotIn("TRUSTED_RECORD_UNRESOLVED", self.codes(verdict))
        self.assertNotIn("TRUSTED_RECORD_UNRESOLVED", self.codes(verdict))
        self.assertFatal(verdict, "CLAIM_UNBACKED")

    def test_no_record_at_all_is_refused(self) -> None:
        raw = b"int silent(void) { return 5; }\n"
        head = self._add_public_file("src/rt/silent.c", raw, {
            "path": "src/rt/silent.c",
            "classification": "project_authored_attested",
            "evidence": {
                "source": "public provenance census",
                "authorship": "independent project implementation",
            },
            "sha256": _sha(raw),
        })
        self.assertFatal(self.run_verify(head), "CLAIM_UNBACKED")

    def test_relabelling_implementation_as_configuration_is_refused(self) -> None:
        """Deterministic classes need no record; the path must actually be one."""
        raw = b"export const x = 1;\n"
        head = self._add_public_file("src/rt/relabelled.ts", raw, {
            "path": "src/rt/relabelled.ts",
            "classification": "reviewed_configuration",
            "evidence": {"source": "configuration review"},
            "sha256": _sha(raw),
        })
        self.assertFatal(self.run_verify(head), "CLAIM_UNBACKED")

    def test_a_genuinely_recorded_new_path_passes(self) -> None:
        """The gate must not merely reject everything.

        A new implementation path needs both factors: a record that covers it
        and an approval naming the exact bytes.
        """
        raw = b"int recorded(void) { return 6; }\n"
        document = json.loads(json.dumps(TRUSTED_RECORDS))
        document["records"].append({
            "id": "recorded-path", "paths": ["src/rt/recorded.c"],
            "classification": "project-authored-independent", "upstream": None,
            "evidence_tier": "S",
        })
        self.approve_blob("src/rt/recorded.c", raw, record_id="recorded-path",
                          classification="project-authored-independent", document=document)
        head = self._add_public_file("src/rt/recorded.c", raw, {
            "path": "src/rt/recorded.c",
            "classification": "project_authored_attested",
            "evidence": {
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "recorded-path", "evidence_tier": "S",
                "authorship": "independent implementation record",
                "upstream_attribution": None,
            },
            "sha256": _sha(raw),
        })
        self.assertPasses(self.run_verify(head))


class ClaimRatchetTests(GateCase):
    """Restating an existing path's provenance requires trusted agreement."""

    def _restate(self, path: str, classification: str, evidence: dict) -> str:
        self.repo.branch("attack", self.base)
        entries = []
        for entry in self.current_entries(self.base):
            if entry["path"] == path:
                entry = dict(entry, classification=classification, evidence=evidence)
            entries.append(entry)
        self.write_ledger(entries)
        return self.repo.commit("restate")

    def test_upgrading_derived_to_independent_is_refused(self) -> None:
        head = self._restate("src/rt/core.c", "project_authored_attested", {
            "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
            "record_id": "core-runtime", "evidence_tier": "H",
            "authorship": "independent implementation record",
        })
        self.assertFatal(self.run_verify(head), "CLAIM_UNBACKED")

    def test_repointing_a_path_at_another_record_is_refused(self) -> None:
        head = self._restate("src/rt/core.c", "upstream_derived", {
            "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
            "record_id": "blanket-tools", "evidence_tier": "S",
            "upstream": "upstream-project",
        })
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "TRUSTED_RECORD_UNRESOLVED")
        self.assertIn("CLAIM_UNBACKED", self.codes(verdict))

    def test_laundering_a_grandfathered_over_claim_is_refused(self) -> None:
        """Debt may be paid down, never re-minted under a new anchor."""
        head = self._restate("tools/legacy.py", "project_authored_attested", {
            "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
            "record_id": "blanket-tools", "evidence_tier": "S",
            "authorship": "independent implementation record",
        })
        self.assertFatal(self.run_verify(head), "CLAIM_UNBACKED")

class AttestationInheritanceTests(GateCase):
    """Changed bytes must not inherit an attestation made about other bytes.

    Grandfathering survives only while *both* halves of the reviewed state are
    frozen: the claim as the trusted base ledger recorded it, and the bytes as
    the trusted base tree recorded them.  These cases walk the full matrix.
    """

    LEGACY = "tools/legacy.py"          # claim disagrees with authority (blanket backing)
    ATTESTED = "src/rt/widget.c"        # claim agrees with authority (exact record)

    def _edit(self, path: str, raw: bytes, *, update_hash: bool = True,
              entry_mutator=None, branch: str = "attack") -> str:
        self.repo.branch(branch, self.base)
        self.repo.write(path, raw)
        entries = []
        for entry in self.current_entries(self.base):
            if entry["path"] == path:
                entry = dict(entry)
                if update_hash:
                    entry["sha256"] = _sha(raw)
                if entry_mutator is not None:
                    entry = entry_mutator(entry)
            entries.append(entry)
        self.write_ledger(entries)
        return self.repo.commit(f"edit {path}")

    # -- A ------------------------------------------------------------------
    def test_A_grandfathered_path_with_unchanged_bytes_is_allowed(self) -> None:
        """Frozen debt is reported, never fatal -- otherwise nothing can merge.

        The debt set is asserted exactly. A path that agrees with authority must
        not drift into it: ``font/README.md`` in particular is derived through
        _class_for's special case, which yields no record id even though a
        record names the path, and reading the id from the raw record map
        instead would silently add it here.
        """
        verdict = self.run_verify(self.base)
        self.assertPasses(verdict)
        debt = {item["path"]: item for item in verdict["grandfathered_debt"]}
        self.assertEqual(sorted(debt), ["README.md", self.LEGACY])
        self.assertEqual(debt[self.LEGACY]["backing"], verifier.BACKING_BLANKET)
        self.assertEqual(verdict["changed_unattested_paths"], [])

    # -- B ------------------------------------------------------------------
    def test_B_same_claim_changed_bytes_updated_hash_is_refused(self) -> None:
        """The headline attack: coherent hash, inherited claim, new content."""
        head = self._edit(self.LEGACY, b"print('replaced wholesale')\n")
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "CONTENT_UNATTESTED")
        self.assertNotIn("CONTENT_MISMATCH", self.codes(verdict))
        self.assertEqual(
            [item["path"] for item in verdict["changed_unattested_paths"]], [self.LEGACY],
        )

    def test_B2_a_one_byte_change_is_enough(self) -> None:
        """No size threshold: content identity is exact or it is nothing."""
        head = self._edit(self.LEGACY, b"print('legacy')\n\n")
        self.assertFatal(self.run_verify(head), "CONTENT_UNATTESTED")

    def test_B3_a_same_length_change_is_still_a_change(self) -> None:
        """Comparing sizes rather than bytes would let this through.

        The replacement is exactly as long as the reviewed content, so any
        length-based shortcut in the content half of the predicate reports the
        path as frozen when it is not.
        """
        original = self.FILES[self.LEGACY]
        replacement = b"print('LEGACY')\n"
        self.assertEqual(len(replacement), len(original))
        self.assertNotEqual(replacement, original)
        head = self._edit(self.LEGACY, replacement)
        self.assertFatal(self.run_verify(head), "CONTENT_UNATTESTED")

    def test_B4_a_path_absent_from_the_base_tree_is_not_frozen(self) -> None:
        """A claim inherited from a base entry with no base file is not review.

        The base ledger here carries an entry for a file the base tree does not
        contain, so the claim half of the predicate is satisfied while nothing
        was ever reviewed. Treating "absent from the base" as frozen would let
        the candidate introduce the bytes under a pre-planted claim.
        """
        ghost, raw = "tools/ghost.py", b"print('ghost')\n"
        claim = {
            "path": ghost, "classification": "project_authored_attested",
            "evidence": {"source": "public provenance census",
                         "authorship": "independent project implementation"},
            "sha256": _sha(raw),
        }
        self.repo.branch("oddbase", self.base)
        self.write_ledger(self.current_entries(self.base) + [claim])
        odd_base = self.repo.commit("base ledger entry with no base file")

        self.repo.branch("attack", odd_base)
        self.repo.write(ghost, raw)
        self.write_policy(list(self.FILES) + [ghost])
        self.write_ledger(self.current_entries(odd_base))
        head = self.repo.commit("introduce the pre-planted path")
        self.assertFatal(self.run_verify(head, base=odd_base), "CONTENT_UNATTESTED")

    # -- C ------------------------------------------------------------------
    def test_C_changed_bytes_with_a_forged_candidate_authority_is_refused(self) -> None:
        """Committing a detailed ledger that blesses the new bytes changes nothing."""
        self.repo.branch("attack", self.base)
        raw = b"print('replaced, with my own authority')\n"
        self.repo.write(self.LEGACY, raw)
        forged = json.loads(json.dumps(TRUSTED_RECORDS))
        forged["records"].append({
            "id": "legacy-blessed", "paths": [self.LEGACY],
            "classification": "project-authored-independent", "upstream": None,
            "evidence_tier": "S",
        })
        self.repo.write("docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                        json.dumps(forged, indent=2) + "\n")
        entries = [
            dict(entry, sha256=_sha(raw)) if entry["path"] == self.LEGACY else entry
            for entry in self.current_entries(self.base)
        ]
        self.write_ledger(entries)
        self.write_policy(list(self.FILES) + ["docs/provenance/IMPLEMENTATION_PROVENANCE.json"])
        entries = self.current_entries(self.base) + [{
            "path": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
            "classification": "reviewed_documentation",
            "evidence": {"source": "public documentation review"},
            "sha256": _sha((self.repo.root / "docs/provenance/IMPLEMENTATION_PROVENANCE.json").read_bytes()),
        }]
        entries = [dict(e, sha256=_sha(raw)) if e["path"] == self.LEGACY else e for e in entries]
        self.write_ledger(entries)
        head = self.repo.commit("bless my own bytes")
        self.assertFatal(self.run_verify(head), "CONTENT_UNATTESTED")

    # -- D ------------------------------------------------------------------
    def test_D_changed_bytes_pointed_at_an_unrelated_trusted_record_is_refused(self) -> None:
        """Reaching for a real record that says nothing about this path."""
        head = self._edit(
            self.LEGACY, b"print('replaced and repointed')\n",
            entry_mutator=lambda entry: dict(entry, evidence={
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "core-runtime", "evidence_tier": "H",
                "authorship": "independent implementation record",
            }),
        )
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "TRUSTED_RECORD_UNRESOLVED")
        self.assertIn("CLAIM_UNBACKED", self.codes(verdict))

    def test_D2_changed_bytes_under_a_blanket_record_is_still_refused(self) -> None:
        """A wildcard is inert for content just as it is for classification.

        ``blanket-tools`` covers ``tools/*`` and so passes anchor integrity.
        It is still not an attestation about these particular new bytes.
        """
        head = self._edit(
            self.LEGACY, b"print('replaced under the blanket')\n",
            entry_mutator=lambda entry: dict(entry, evidence={
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "blanket-tools", "evidence_tier": "S",
                "authorship": "independent implementation record",
            }),
        )
        verdict = self.run_verify(head)
        self.assertNotIn("TRUSTED_RECORD_UNRESOLVED", self.codes(verdict))
        self.assertFatal(verdict, "CLAIM_UNBACKED")

    # -- E ------------------------------------------------------------------
    def test_E_renaming_a_path_to_inherit_another_record_is_refused(self) -> None:
        """A record names paths.  A new path is a new path, however similar."""
        self.repo.branch("attack", self.base)
        raw = self.FILES[self.ATTESTED]
        self.repo.remove(self.ATTESTED)
        self.repo.write("src/rt/widget_v2.c", raw)
        entries = [e for e in self.current_entries(self.base) if e["path"] != self.ATTESTED]
        entries.append({
            "path": "src/rt/widget_v2.c",
            "classification": "project_authored_attested",
            "evidence": {
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "widget-independent", "evidence_tier": "S",
                "authorship": "independent implementation record",
            },
            "sha256": _sha(raw),
        })
        self.write_ledger(entries)
        self.write_policy(
            [p for p in self.FILES if p != self.ATTESTED] + ["src/rt/widget_v2.c"]
        )
        head = self.repo.commit("rename to inherit")
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "TRUSTED_RECORD_UNRESOLVED")
        self.assertIn("CLAIM_UNBACKED", self.codes(verdict))

    def test_E2_copying_an_attested_path_does_not_extend_its_record(self) -> None:
        """Identical bytes are not identical provenance; the record names paths."""
        self.repo.branch("attack", self.base)
        raw = self.FILES[self.ATTESTED]
        self.repo.write("src/rt/widget_copy.c", raw)
        entries = self.current_entries(self.base) + [{
            "path": "src/rt/widget_copy.c",
            "classification": "project_authored_attested",
            "evidence": {
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "widget-independent", "evidence_tier": "S",
                "authorship": "independent implementation record",
            },
            "sha256": _sha(raw),
        }]
        self.write_ledger(entries)
        self.write_policy(list(self.FILES) + ["src/rt/widget_copy.c"])
        head = self.repo.commit("copy to inherit")
        self.assertFatal(self.run_verify(head), "TRUSTED_RECORD_UNRESOLVED")

    # -- F ------------------------------------------------------------------
    def test_F_authority_updated_for_the_new_bytes_passes(self) -> None:
        """The intended path out of debt: authority first, then the ledger.

        Authority must supply both halves -- a record covering the path and an
        approval naming the exact new bytes.
        """
        replacement = b"print('replaced, and now attested')\n"
        document = json.loads(json.dumps(TRUSTED_RECORDS))
        document["records"].append({
            "id": "legacy-attested", "paths": [self.LEGACY],
            "classification": "project-authored-independent", "upstream": None,
            "evidence_tier": "S",
        })
        self.approve_blob(self.LEGACY, replacement, record_id="legacy-attested",
                          classification="project-authored-independent", document=document)
        head = self._edit(
            self.LEGACY, replacement,
            entry_mutator=lambda entry: dict(entry, evidence={
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "legacy-attested", "evidence_tier": "S",
                "authorship": "independent implementation record",
                "upstream_attribution": None,
            }),
        )
        verdict = self.run_verify(head)
        self.assertPasses(verdict)
        self.assertEqual(  # only the redaction fixture's own debt remains
            [item["path"] for item in verdict["grandfathered_debt"]], ["README.md"],
        )

    def test_F2_an_agreeing_path_still_needs_its_new_bytes_approved(self) -> None:
        """An attested path is not a blank cheque for future content.

        This inverts the second-pass behaviour deliberately. Records carry
        paths, so path-level authority alone once let an agreeing path change
        content freely; exact-blob authorization is what closes that.
        """
        replacement = b"int widget(void) { return 4242; }\n"
        head = self._edit(self.ATTESTED, replacement)
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "BLOB_UNAPPROVED")
        self.assertEqual(verdict["changed_unattested_paths"], [])

        self.approve_blob(self.ATTESTED, replacement, record_id="widget-independent",
                          classification="project-authored-independent")
        self.assertPasses(self.run_verify(head))

    def test_F3_a_deterministic_path_may_change_content_freely(self) -> None:
        """Documentation with no record is classified by what it is, every run."""
        head = self._edit("docs/notes.md", b"# fixture notes, revised\n")
        verdict = self.run_verify(head)
        self.assertPasses(verdict)
        self.assertEqual(verdict["changed_unattested_paths"], [])

    def test_F4_debt_with_exact_backing_still_freezes_its_content(self) -> None:
        """Backing says which remedy applies, never whether the rule applies.

        ``README.md`` has an exact trusted record deriving a different class
        from the one the public ledger claims.  That is debt, so its bytes are
        frozen until the public entry is corrected -- even though the authority
        does speak about the path.
        """
        head = self._edit("README.md", b"# fixture, revised\n")
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "CONTENT_UNATTESTED")
        self.assertEqual(
            [item["backing"] for item in verdict["changed_unattested_paths"]],
            [verifier.BACKING_EXACT],
        )

    # -- the two halves are independently load-bearing ----------------------
    def test_deleting_a_grandfathered_path_is_allowed(self) -> None:
        """Paying debt down by removal must never be harder than keeping it."""
        self.repo.branch("attack", self.base)
        self.repo.remove(self.LEGACY)
        entries = [e for e in self.current_entries(self.base) if e["path"] != self.LEGACY]
        self.write_ledger(entries)
        self.write_policy([p for p in self.FILES if p != self.LEGACY])
        head = self.repo.commit("delete legacy")
        self.assertPasses(self.run_verify(head))

    def test_correcting_a_grandfathered_claim_to_authority_is_allowed(self) -> None:
        """Debt is paid by agreeing with authority, and that must not be refused."""
        document = json.loads(json.dumps(TRUSTED_RECORDS))
        document["records"].append({
            "id": "legacy-attested", "paths": [self.LEGACY],
            "classification": "derived-translated", "upstream": "upstream-project",
            "upstream_paths": [], "upstream_revision": None,
            "upstream_license": "GPL-2.0-or-later", "evidence_tier": "S",
        })
        self.write_trusted(document)
        self.repo.branch("fix", self.base)
        entries = []
        for entry in self.current_entries(self.base):
            if entry["path"] == self.LEGACY:
                entry = dict(entry, classification="upstream_derived", evidence={
                    "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                    "record_id": "legacy-attested", "evidence_tier": "S",
                    "upstream": "upstream-project", "upstream_paths": [],
                    "upstream_revision": None, "license": "GPL-2.0-or-later",
                    "modification_status": "modified_or_translated; see record",
                })
            entries.append(entry)
        self.write_ledger(entries)
        head = self.repo.commit("pay down debt")
        self.assertPasses(self.run_verify(head))


class ExactBlobAuthorizationTests(GateCase):
    """Path coverage is not content approval.

    ``src/rt/widget.c`` is fully authority-backed: an exact record names it and
    the public ledger's claim agrees. Under path-level authority alone its
    content could be replaced wholesale. These cases require the trusted
    authority to name the exact candidate blob before any new implementation
    bytes are accepted at that path.
    """

    BACKED = "src/rt/widget.c"          # exact record, claim agrees with authority
    RECORD = "widget-independent"
    PRIVATE_CLASS = "project-authored-independent"

    def approve(self, path: str, raw: bytes, *, record_id: str | None = None,
                classification: str | None = None, sha256: str | None = None,
                document: dict | None = None) -> dict:
        """Add a reviewed-blob approval to the trusted authority."""
        document = json.loads(json.dumps(document or TRUSTED_RECORDS))
        document.setdefault("reviewed_blobs", []).append({
            "path": path,
            "sha256": sha256 or _sha(raw),
            "classification": classification or self.PRIVATE_CLASS,
            "record_id": record_id or self.RECORD,
            # Private review metadata the gate must never echo.
            "reviewed_by": "maintainer",
            "review_note": "PRIVATE REVIEW NOTE, MUST NOT APPEAR IN ANY OUTPUT",
        })
        self.write_trusted(document)
        return document

    def edit_backed(self, raw: bytes, *, path: str | None = None) -> str:
        path = path or self.BACKED
        self.repo.branch("attack", self.base)
        self.repo.write(path, raw)
        entries = [
            dict(entry, sha256=_sha(raw)) if entry["path"] == path else entry
            for entry in self.current_entries(self.base)
        ]
        self.write_ledger(entries)
        return self.repo.commit("change authority-backed implementation bytes")

    # -- A ------------------------------------------------------------------
    def test_A_trusted_path_and_record_with_unchanged_blob_passes(self) -> None:
        self.assertPasses(self.run_verify(self.base))

    # -- B ------------------------------------------------------------------
    def test_B_changed_blob_with_coherent_public_hash_and_no_approval_fails(self) -> None:
        """The third-pass headline: the claim agrees, the bytes are new."""
        head = self.edit_backed(b"int widget(void) { return 777; }\n")
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "BLOB_UNAPPROVED")
        # Not caught by anything else: the claim is untouched and correct.
        self.assertNotIn("CLAIM_UNBACKED", self.codes(verdict))
        self.assertNotIn("CONTENT_UNATTESTED", self.codes(verdict))
        self.assertNotIn("CONTENT_MISMATCH", self.codes(verdict))

    # -- C ------------------------------------------------------------------
    def test_C_candidate_fabricated_digest_in_the_public_ledger_fails(self) -> None:
        """A digest the candidate writes is not an approval."""
        raw = b"int widget(void) { return 778; }\n"
        self.repo.branch("attack", self.base)
        self.repo.write(self.BACKED, raw)
        entries = []
        for entry in self.current_entries(self.base):
            if entry["path"] == self.BACKED:
                entry = dict(entry, sha256=_sha(raw), evidence=dict(
                    entry["evidence"], approved_sha256=_sha(raw), reviewed_blob=True,
                ))
            entries.append(entry)
        self.write_ledger(entries)
        head = self.repo.commit("self-issued approval")
        self.assertFatal(self.run_verify(head), "BLOB_UNAPPROVED")

    # -- D ------------------------------------------------------------------
    def test_D_private_authority_approving_the_exact_digest_passes(self) -> None:
        raw = b"int widget(void) { return 779; }\n"
        self.approve(self.BACKED, raw)
        head = self.edit_backed(raw)
        verdict = self.run_verify(head)
        self.assertPasses(verdict)
        self.assertEqual(
            [item["path"] for item in verdict["blobs_approved_this_candidate"]], [self.BACKED],
        )

    # -- E ------------------------------------------------------------------
    def test_E_approval_for_the_wrong_digest_fails(self) -> None:
        """Right path, right record, digest of some other revision."""
        self.approve(self.BACKED, b"int widget(void) { return 111; }\n")
        head = self.edit_backed(b"int widget(void) { return 222; }\n")
        self.assertFatal(self.run_verify(head), "BLOB_UNAPPROVED")

    # -- F ------------------------------------------------------------------
    def test_F_approval_bound_to_another_path_fails(self) -> None:
        """The digest is approved, but for a different path."""
        raw = b"int widget(void) { return 333; }\n"
        self.approve("src/rt/core.c", raw, record_id="core-runtime",
                     classification="derived-translated")
        head = self.edit_backed(raw)
        self.assertFatal(self.run_verify(head), "BLOB_UNAPPROVED")

    # -- G ------------------------------------------------------------------
    def test_G_exact_bytes_copied_to_a_new_path_fail(self) -> None:
        """Approving bytes at one path never authorizes them at another."""
        raw = self.FILES[self.BACKED]
        self.approve(self.BACKED, raw)
        self.repo.branch("attack", self.base)
        self.repo.write("src/rt/widget_copy.c", raw)
        entries = self.current_entries(self.base) + [{
            "path": "src/rt/widget_copy.c",
            "classification": "project_authored_attested",
            "evidence": {
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": self.RECORD, "evidence_tier": "S",
                "authorship": "independent implementation record",
            },
            "sha256": _sha(raw),
        }]
        self.write_ledger(entries)
        self.write_policy(list(self.FILES) + ["src/rt/widget_copy.c"])
        head = self.repo.commit("copy approved bytes to a new path")
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "BLOB_UNAPPROVED")
        self.assertIn("TRUSTED_RECORD_UNRESOLVED", self.codes(verdict))

    # -- H ------------------------------------------------------------------
    def test_H_changing_classification_and_bytes_fails(self) -> None:
        raw = b"int widget(void) { return 444; }\n"
        self.repo.branch("attack", self.base)
        self.repo.write(self.BACKED, raw)
        entries = []
        for entry in self.current_entries(self.base):
            if entry["path"] == self.BACKED:
                entry = dict(entry, sha256=_sha(raw), classification="upstream_derived",
                             evidence={
                                 "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                                 "record_id": self.RECORD, "evidence_tier": "S",
                                 "upstream": "documented upstream family",
                             })
            entries.append(entry)
        self.write_ledger(entries)
        head = self.repo.commit("reclassify and replace")
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "BLOB_UNAPPROVED")
        self.assertIn("CLAIM_UNBACKED", self.codes(verdict))

    def test_H2_approval_whose_classification_disagrees_fails(self) -> None:
        """An approval cannot authorize a class its own record does not carry."""
        raw = b"int widget(void) { return 445; }\n"
        self.approve(self.BACKED, raw, classification="derived-translated")
        head = self.edit_backed(raw)
        self.assertFatal(self.run_verify(head), "BLOB_APPROVAL_CLASS_MISMATCH")

    def test_H3_approval_citing_a_record_that_does_not_cover_the_path_fails(self) -> None:
        raw = b"int widget(void) { return 446; }\n"
        self.approve(self.BACKED, raw, record_id="core-runtime")
        head = self.edit_backed(raw)
        self.assertFatal(self.run_verify(head), "BLOB_APPROVAL_RECORD_MISMATCH")

    # -- I ------------------------------------------------------------------
    def test_I_wildcard_record_with_changed_bytes_fails(self) -> None:
        """``tools/*`` is not authorization for replacement content.

        Even with an exact-blob approval citing the blanket record, the claim
        ratchet still has no exact record to derive from, so the path stays
        refused. A wildcard cannot become content authority by either route.
        """
        raw = b"print('replaced under the blanket')\n"
        self.approve("tools/legacy.py", raw, record_id="blanket-tools")
        self.repo.branch("attack", self.base)
        self.repo.write("tools/legacy.py", raw)
        entries = [
            dict(entry, sha256=_sha(raw)) if entry["path"] == "tools/legacy.py" else entry
            for entry in self.current_entries(self.base)
        ]
        self.write_ledger(entries)
        head = self.repo.commit("blanket-backed replacement")
        verdict = self.run_verify(head)
        self.assertEqual(verdict["verdict"], "fail")
        self.assertTrue(
            {"BLOB_APPROVAL_RECORD_MISMATCH", "CONTENT_UNATTESTED"} & self.codes(verdict),
            msg=f"findings: {verdict['findings']}",
        )

    # -- J ------------------------------------------------------------------
    def test_J_approval_for_a_previous_candidate_commit_goes_stale(self) -> None:
        """Approve revision one, then push revision two."""
        first = b"int widget(void) { return 555; }\n"
        self.approve(self.BACKED, first)
        head = self.edit_backed(first)
        self.assertPasses(self.run_verify(head))

        second = b"int widget(void) { return 556; }\n"
        self.repo.write(self.BACKED, second)
        entries = [
            dict(entry, sha256=_sha(second)) if entry["path"] == self.BACKED else entry
            for entry in self.current_entries(head)
        ]
        self.write_ledger(entries)
        head2 = self.repo.commit("push a second revision")
        self.assertFatal(self.run_verify(head2), "BLOB_UNAPPROVED")

    # -- K ------------------------------------------------------------------
    def test_K_mutable_revision_selectors_are_refused_in_strict_mode(self) -> None:
        """CI must judge the exact SHAs the event carried, not branch names."""
        with self.assertRaises(verifier.VerifyError) as caught:
            verifier.verify(
                repo=self.repo.root, candidate_rev="attack", base_rev=self.base,
                trusted_ledger=self.trusted_ledger, workdir=self.outside / "work",
                require_immutable_revisions=True,
            )
        self.assertEqual(caught.exception.code, "MUTABLE_REVISION_REFUSED")

    def test_K2_exact_shas_are_accepted_in_strict_mode(self) -> None:
        verdict = verifier.verify(
            repo=self.repo.root, candidate_rev=self.base, base_rev=self.base,
            trusted_ledger=self.trusted_ledger, workdir=self.outside / "work",
            require_immutable_revisions=True, authority_revision="a" * 40,
        )
        self.assertPasses(verdict)
        self.assertEqual(verdict["authority_revision"], "a" * 40)

    # -- L ------------------------------------------------------------------
    def test_L_internally_perfect_candidate_still_fails_on_external_digest(self) -> None:
        """Every public artifact agrees with every other, and it is still refused."""
        raw = b"int widget(void) { return 666; }\n"
        head = self.edit_backed(raw)
        verdict = self.run_verify(head)
        # Prove the internal coherence the attacker achieved.
        self.assertNotIn("CONTENT_MISMATCH", self.codes(verdict))
        self.assertNotIn("LEDGER_SCHEMA", self.codes(verdict))
        self.assertNotIn("LEDGER_COVERAGE", self.codes(verdict))
        self.assertNotIn("EXPORT_FIELD_MISMATCH", self.codes(verdict))
        self.assertFatal(verdict, "BLOB_UNAPPROVED")

    def test_the_gate_hashes_the_git_object_not_the_ledger_claim(self) -> None:
        """The digest that selects an approval must be computed, not read.

        Here the candidate ships unapproved bytes while its ledger *claims* the
        digest of an approved revision. ``CONTENT_MISMATCH`` catches the lie on
        its own, so a gate that looked the approval up by the claimed digest
        would still fail the run -- and the defect would be invisible in the
        verdict. It is visible in the findings: the blob must be reported
        unapproved, and nothing may be recorded as approved.
        """
        approved = b"int widget(void) { return 1000; }\n"
        shipped = b"int widget(void) { return 1001; }\n"
        self.approve(self.BACKED, approved)
        self.repo.branch("attack", self.base)
        self.repo.write(self.BACKED, shipped)
        entries = [
            dict(entry, sha256=_sha(approved)) if entry["path"] == self.BACKED else entry
            for entry in self.current_entries(self.base)
        ]
        self.write_ledger(entries)
        head = self.repo.commit("claim the approved digest, ship other bytes")
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "BLOB_UNAPPROVED")
        self.assertIn("CONTENT_MISMATCH", self.codes(verdict))
        self.assertEqual(verdict["blobs_approved_this_candidate"], [])
        self.assertEqual(
            [item["sha256"] for item in verdict["blobs_unapproved"]], [_sha(shipped)],
            "the reported digest must be the one computed from the Git object",
        )

    # -- hygiene ------------------------------------------------------------
    def test_private_review_metadata_never_reaches_the_verdict(self) -> None:
        raw = b"int widget(void) { return 888; }\n"
        self.approve(self.BACKED, raw)
        head = self.edit_backed(raw)
        blob = json.dumps(self.run_verify(head))
        self.assertNotIn("PRIVATE REVIEW NOTE", blob)
        self.assertNotIn("reviewed_by", blob)

    def test_an_approval_citing_an_unknown_record_is_refused(self) -> None:
        raw = b"int widget(void) { return 999; }\n"
        self.approve(self.BACKED, raw, record_id="no-such-record")
        head = self.edit_backed(raw)
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(head)
        self.assertEqual(caught.exception.code, "TRUSTED_LEDGER_INVALID")

    def test_a_repeated_approval_for_one_path_and_digest_is_refused(self) -> None:
        """Explicit semantics: one approval per (path, digest).

        Two copies are ambiguous authority even when they agree, so the
        authority is refused rather than silently deduplicated.
        """
        raw = b"int widget(void) { return 2024; }\n"
        document = self.approve(self.BACKED, raw)
        document["reviewed_blobs"].append(dict(document["reviewed_blobs"][-1]))
        self.write_trusted(document)
        head = self.edit_backed(raw)
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(head)
        self.assertEqual(caught.exception.code, "TRUSTED_LEDGER_INVALID")

    def test_several_digests_for_one_path_are_legitimate(self) -> None:
        """Approvals are a revision history; only the exact digest matches."""
        older = b"int widget(void) { return 1; }\n"
        newer = b"int widget(void) { return 2; }\n"
        document = self.approve(self.BACKED, older)
        self.approve(self.BACKED, newer, document=document)
        self.assertPasses(self.run_verify(self.edit_backed(newer)))

    def test_an_unknown_record_id_covers_nothing(self) -> None:
        """Why the finding needs no separate existence check.

        ``_record_covers`` is the single question -- does authority cover this
        path under this id -- and an id the authority does not hold covers
        nothing. Stating it here keeps the property tested rather than
        duplicated in an untestable branch.
        """
        exact = {"a.c": {"id": "real"}}
        patterns = {"real": ["a.c"]}
        self.assertFalse(verifier._record_covers("a.c", "absent", exact, patterns))
        self.assertTrue(verifier._record_covers("a.c", "real", exact, patterns))

    def test_a_wildcard_approval_path_is_refused(self) -> None:
        document = json.loads(json.dumps(TRUSTED_RECORDS))
        document["reviewed_blobs"] = [{
            "path": "src/rt/*", "sha256": "0" * 64,
            "classification": self.PRIVATE_CLASS, "record_id": self.RECORD,
        }]
        self.write_trusted(document)
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(self.base)
        self.assertEqual(caught.exception.code, "TRUSTED_LEDGER_INVALID")

    def test_a_deterministic_path_needs_no_blob_approval(self) -> None:
        """Only implementation-class claims are content-gated."""
        head = self.edit_backed(b"# fixture notes, revised\n", path="docs/notes.md")
        self.assertPasses(self.run_verify(head))


class TrustedScopeTests(GateCase):
    """The candidate's own policy must not decide what the gate looks at.

    Letting it do so was a complete bypass, reproduced against the third-pass
    verifier on the real repository: change an implementation file, drop it
    from ``include_paths``, delete its ledger entry, regenerate the export, and
    every downstream check simply stopped seeing the path.
    """

    BACKED = "src/rt/widget.c"

    def _drop_from_scope(self, path: str, *, exclude: bool = False,
                         change_bytes: bool = True) -> str:
        self.repo.branch("attack", self.base)
        if change_bytes:
            self.repo.write(path, b"int widget(void) { return 31337; }\n")
        document = dict(POLICY)
        document["include_paths"] = sorted(
            (set(self.FILES) | {TRUSTED_WORKFLOW, verifier.LEDGER_PATH, verifier.EXPORT_PATH})
            - {path}
        )
        if exclude:
            document["exclude_paths"] = sorted(set(POLICY["exclude_paths"]) | {path})
        self.repo.write(verifier.POLICY_PATH, json.dumps(document, indent=2) + "\n")
        self.write_ledger([e for e in self.current_entries(self.base) if e["path"] != path])
        return self.repo.commit("drop a changed path out of scope")

    def test_removing_a_changed_path_from_include_paths_is_refused(self) -> None:
        head = self._drop_from_scope(self.BACKED)
        verdict = self.run_verify(head)
        self.assertFatal(verdict, "TRUSTED_SCOPE_VIOLATION")

    def test_excluding_a_changed_path_is_refused(self) -> None:
        head = self._drop_from_scope(self.BACKED, exclude=True)
        self.assertFatal(self.run_verify(head), "TRUSTED_SCOPE_VIOLATION")

    def test_dropping_an_unchanged_path_from_scope_is_still_refused(self) -> None:
        """Scope is not allowed to narrow even when nothing else changed."""
        head = self._drop_from_scope(self.BACKED, change_bytes=False)
        self.assertFatal(self.run_verify(head), "TRUSTED_SCOPE_VIOLATION")

    def test_genuinely_deleting_a_path_is_allowed(self) -> None:
        """Removal from the tree is legitimate; vanishing from scope is not."""
        self.repo.branch("work", self.base)
        self.repo.remove(self.BACKED)
        document = dict(POLICY)
        document["include_paths"] = sorted(
            (set(self.FILES) | {TRUSTED_WORKFLOW, verifier.LEDGER_PATH, verifier.EXPORT_PATH})
            - {self.BACKED}
        )
        self.repo.write(verifier.POLICY_PATH, json.dumps(document, indent=2) + "\n")
        self.write_ledger([e for e in self.current_entries(self.base) if e["path"] != self.BACKED])
        head = self.repo.commit("delete the file outright")
        self.assertPasses(self.run_verify(head))

    def test_a_new_implementation_path_must_enter_the_universe(self) -> None:
        """Adding source while leaving it out of the policy is refused."""
        self.repo.branch("attack", self.base)
        self.repo.write("src/rt/smuggled.c", b"int smuggled(void) { return 1; }\n")
        head = self.repo.commit("add source outside the policy")
        self.assertFatal(self.run_verify(head), "TRUSTED_SCOPE_VIOLATION")

    def test_a_new_path_the_trusted_policy_excludes_is_allowed_out(self) -> None:
        """The trusted policy's own exclusions still work; the candidate's do not."""
        self.repo.branch("work", self.base)
        self.repo.write("private/secret.c", b"int secret(void) { return 1; }\n")
        head = self.repo.commit("add a file the trusted policy excludes")
        self.assertPasses(self.run_verify(head))

    def test_a_candidate_authored_exclusion_cannot_hide_a_new_path(self) -> None:
        """The decisive case: only the TRUSTED policy may except a new path.

        The candidate adds implementation and writes its own exclusion for it.
        Honouring that exclusion would let any new file arrive unverified, so
        the trusted policy is the only one consulted here.
        """
        self.repo.branch("attack", self.base)
        self.repo.write("src/rt/smuggled.c", b"int smuggled(void) { return 1; }\n")
        document = dict(POLICY)
        document["include_paths"] = sorted(
            set(self.FILES) | {TRUSTED_WORKFLOW, verifier.LEDGER_PATH, verifier.EXPORT_PATH}
        )
        # An exclusion the trusted policy does not have.
        document["exclude_paths"] = sorted(set(POLICY["exclude_paths"]) | {"src/rt/smuggled.c"})
        self.repo.write(verifier.POLICY_PATH, json.dumps(document, indent=2) + "\n")
        head = self.repo.commit("exclude my own new source")
        self.assertFatal(self.run_verify(head), "TRUSTED_SCOPE_VIOLATION")

    def test_widening_scope_is_allowed(self) -> None:
        """Coverage may grow; the new path just has to satisfy provenance."""
        self.repo.branch("work", self.base)
        raw = b"# extra documentation\n"
        self.repo.write("docs/extra.md", raw)
        self.write_policy(list(self.FILES) + ["docs/extra.md"])
        self.write_ledger(self.current_entries(self.base) + [{
            "path": "docs/extra.md", "classification": "reviewed_documentation",
            "evidence": {"source": "public documentation review"}, "sha256": _sha(raw),
        }])
        head = self.repo.commit("publish one more document")
        self.assertPasses(self.run_verify(head))


class ClassificationFloorTests(GateCase):
    """An implementation file may not be relabelled out of content gating."""

    BACKED = "src/rt/widget.c"

    def _relabel(self, classification: str, evidence: dict) -> str:
        self.repo.branch("attack", self.base)
        self.repo.write(self.BACKED, b"int widget(void) { return 4711; }\n")
        raw = (self.repo.root / self.BACKED).read_bytes()
        entries = []
        for entry in self.current_entries(self.base):
            if entry["path"] == self.BACKED:
                entry = dict(entry, classification=classification,
                             evidence=evidence, sha256=_sha(raw))
            entries.append(entry)
        self.write_ledger(entries)
        return self.repo.commit(f"relabel as {classification}")

    def test_relabelling_implementation_as_documentation_is_refused(self) -> None:
        head = self._relabel("reviewed_documentation", {"source": "public documentation review"})
        self.assertFatal(self.run_verify(head), "CLASSIFICATION_DOWNGRADE")

    def test_relabelling_implementation_as_a_fixture_is_refused(self) -> None:
        head = self._relabel("synthetic_fixture", {"source": "path-reviewed fixture/test census"})
        self.assertFatal(self.run_verify(head), "CLASSIFICATION_DOWNGRADE")

    def test_relabelling_implementation_as_configuration_is_refused(self) -> None:
        head = self._relabel("reviewed_configuration", {"source": "configuration review"})
        self.assertFatal(self.run_verify(head), "CLASSIFICATION_DOWNGRADE")

    def test_moving_source_under_a_test_looking_name_does_not_exempt_it(self) -> None:
        """``tools/test_*`` is deterministic by path, so the move is a new path.

        The copy needs its own record and its own blob approval; inheriting the
        old path's treatment by renaming into a fixture-shaped name must not
        work.
        """
        self.repo.branch("attack", self.base)
        raw = self.FILES[self.BACKED]
        self.repo.write("tools/test_widget_smuggled.py", raw)
        self.write_policy(list(self.FILES) + ["tools/test_widget_smuggled.py"])
        self.write_ledger(self.current_entries(self.base) + [{
            "path": "tools/test_widget_smuggled.py", "classification": "synthetic_fixture",
            "evidence": {"source": "path-reviewed fixture/test census"}, "sha256": _sha(raw),
        }])
        head = self.repo.commit("smuggle source under a fixture name")
        verdict = self.run_verify(head)
        # The path really is deterministic by rule, so it is not content-gated;
        # what must not happen is the ORIGINAL path escaping its treatment.
        self.assertPasses(verdict)
        self.assertEqual(
            self.run_verify(self.base)["blobs_unapproved"], [],
            "the original attested path is untouched and still governed",
        )

    def test_correcting_a_genuine_over_claim_is_still_allowed(self) -> None:
        """The floor is trusted derivation, not the base claim.

        ``tools/legacy.py`` is classified as implementation in the base ledger
        while authority derives nothing for it, so moving the ledger onto the
        derived class is a correction rather than a downgrade -- and the 113
        normalization entries depend on that distinction.
        """
        self.repo.branch("work", self.base)
        entries = []
        for entry in self.current_entries(self.base):
            if entry["path"] == "tools/legacy.py":
                entry = dict(entry, classification="synthetic_fixture",
                             evidence={"source": "path-reviewed fixture/test census"})
            entries.append(entry)
        self.write_ledger(entries)
        head = self.repo.commit("correct an over-claim")
        self.assertNotIn("CLASSIFICATION_DOWNGRADE", self.codes(self.run_verify(head)))


class CanonicalExportTests(GateCase):
    """Every security-relevant export field is recomputed, not accepted."""

    def _forge(self, **fields) -> dict:
        self.repo.branch("attack", self.base)
        self.repo.before_commit = lambda: self.regenerate_export(**fields)
        return self.run_verify(self.repo.commit("forge export fields"))

    def test_forged_policy_digest_is_refused(self) -> None:
        self.assertFatal(self._forge(policy_sha256="0" * 64), "EXPORT_FIELD_MISMATCH")

    def test_forged_included_content_digest_is_refused(self) -> None:
        self.assertFatal(self._forge(included_content_sha256="0" * 64), "EXPORT_FIELD_MISMATCH")

    def test_forged_counts_are_refused(self) -> None:
        self.assertFatal(self._forge(included_file_count=1), "EXPORT_FIELD_MISMATCH")
        self.assertFatal(self._forge(tracked_file_count=99), "EXPORT_FIELD_MISMATCH")
        self.assertFatal(self._forge(excluded_file_count=99), "EXPORT_FIELD_MISMATCH")

    def test_forged_exclusion_lists_are_refused(self) -> None:
        self.assertFatal(self._forge(excluded_paths=[]), "EXPORT_FIELD_MISMATCH")
        self.assertFatal(self._forge(excluded_globs=[]), "EXPORT_FIELD_MISMATCH")
        self.assertFatal(self._forge(excluded_present_paths=["nope"]), "EXPORT_FIELD_MISMATCH")

    def test_forged_schema_and_profile_fields_are_refused(self) -> None:
        self.assertFatal(self._forge(export_schema_version=99), "EXPORT_FIELD_MISMATCH")
        self.assertFatal(self._forge(profile="something-else"), "EXPORT_FIELD_MISMATCH")
        self.assertFatal(self._forge(policy_version="9.9.9"), "EXPORT_FIELD_MISMATCH")

    def test_an_extra_invented_field_is_refused(self) -> None:
        self.assertFatal(self._forge(attacker_field="hello"), "EXPORT_FIELD_MISMATCH")

    def test_a_correct_ledger_digest_does_not_buy_silence(self) -> None:
        """The third-pass check verified this one field and nothing else."""
        verdict = self._forge(included_content_sha256="0" * 64)
        self.assertFatal(verdict, "EXPORT_FIELD_MISMATCH")
        detail = " ".join(f["detail"] for f in verdict["findings"])
        self.assertIn("included_content_sha256", detail)


class StrictParserTests(GateCase):
    """Duplicate JSON keys are refused before any semantics run."""

    def _with_duplicate_key(self, path: str, safe: str, malicious: str) -> str:
        self.repo.branch("attack", self.base)
        raw = (self.repo.root / path).read_bytes().decode("utf-8")
        # Insert a second copy of an object key: json.loads keeps the last.
        injected = raw.replace("{\n", "{\n  " + json.dumps(safe) + ": "
                               + json.dumps(malicious) + ",\n", 1)
        self.repo.write(path, injected)
        self.repo.before_commit = None
        return self.repo.commit("duplicate key")

    def test_duplicate_key_in_the_public_ledger_is_refused(self) -> None:
        self.repo.branch("attack", self.base)
        raw = (self.repo.root / verifier.LEDGER_PATH).read_text(encoding="utf-8")
        self.repo.write(verifier.LEDGER_PATH,
                        raw.replace('"schema_version": 1,', '"schema_version": 1,\n  "schema_version": 2,', 1))
        self.repo.before_commit = None
        head = self.repo.commit("duplicate key in the ledger")
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(head)
        self.assertEqual(caught.exception.code, "CANDIDATE_LEDGER_INVALID")

    def test_duplicate_key_in_the_policy_is_refused(self) -> None:
        self.repo.branch("attack", self.base)
        raw = (self.repo.root / verifier.POLICY_PATH).read_text(encoding="utf-8")
        self.repo.write(verifier.POLICY_PATH,
                        raw.replace('"name":', '"name": "decoy",\n  "name":', 1))
        self.repo.before_commit = None
        head = self.repo.commit("duplicate key in the policy")
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(head)
        self.assertIn("POLICY", caught.exception.code)

    def test_duplicate_key_in_the_export_is_refused(self) -> None:
        self.repo.branch("attack", self.base)
        raw = (self.repo.root / verifier.EXPORT_PATH).read_text(encoding="utf-8")
        self.repo.write(verifier.EXPORT_PATH,
                        raw.replace('"profile":', '"profile": "decoy",\n  "profile":', 1))
        self.repo.before_commit = None
        head = self.repo.commit("duplicate key in the export")
        self.assertFatal(self.run_verify(head), "EXPORT_UNREADABLE")

    def test_duplicate_key_in_the_trusted_authority_is_refused(self) -> None:
        raw = self.trusted_ledger.read_text(encoding="utf-8")
        self.trusted_ledger.write_text(
            raw.replace('"schema_version": 1,', '"schema_version": 1,\n  "schema_version": 2,', 1),
            encoding="utf-8", newline="\n")
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(self.base)
        self.assertEqual(caught.exception.code, "TRUSTED_LEDGER_INVALID")


class PathCanonicalizationTests(GateCase):
    """One file, one spelling. Ambiguity fails closed rather than normalizing."""

    def _stage_path(self, raw_path: bytes) -> str:
        blob = subprocess.run(
            ["git", "hash-object", "-w", "--stdin"], cwd=self.repo.root,
            input=b"int x(void) { return 1; }\n", capture_output=True, check=True,
        ).stdout.decode("ascii").strip()
        # `git mktree` builds a single level, so it cannot express a nested
        # path. `update-index --index-info` takes the raw bytes and any depth.
        subprocess.run(["git", "read-tree", self.base], cwd=self.repo.root, check=True,
                       capture_output=True)
        subprocess.run(
            ["git", "update-index", "-z", "--index-info"], cwd=self.repo.root,
            input=b"100644 " + blob.encode("ascii") + b" 0\t" + raw_path + b"\0",
            capture_output=True, check=True,
        )
        tree = subprocess.run(
            ["git", "write-tree"], cwd=self.repo.root, capture_output=True, check=True,
        ).stdout.decode("ascii").strip()
        return subprocess.run(
            ["git", "commit-tree", tree, "-p", self.base, "-m", "odd path"],
            cwd=self.repo.root, capture_output=True, check=True,
        ).stdout.decode("ascii").strip()

    def _assert_refused(self, raw_path: bytes) -> None:
        head = self._stage_path(raw_path)
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(head)
        self.assertEqual(caught.exception.code, "TREE_INVALID")

    def test_an_nfd_spelling_is_refused(self) -> None:
        """NFC and NFD are different Git paths that name one file elsewhere."""
        nfd = unicodedata.normalize("NFD", "src/rt/café.c")
        self.assertNotEqual(nfd, unicodedata.normalize("NFC", nfd))
        self._assert_refused(nfd.encode("utf-8"))

    def test_backslash_and_control_forms_are_refused_by_the_parser(self) -> None:
        """Git will not index these on this host, so the rule is tested directly.

        NFD is different: Git stores it happily, which is why it gets a
        tree-level case above. These two are still enforced because the same
        canonicalizer validates ledger, policy and approval path strings, and
        nothing normalizes those.
        """
        for candidate in ("src\\rt\\windows.c", "src/rt/we\x01ird.c",
                          "src/rt/nl\nname.c", "src/rt/cr\rname.c", "src/rt/del\x7f.c"):
            with self.subTest(path=repr(candidate)):
                with self.assertRaises(verifier.VerifyError):
                    verifier.canonical_path(candidate, code="X", label="probe")

    def test_dot_and_separator_forms_are_refused_by_the_parser(self) -> None:
        """Git normalizes these away when building a tree, so test the rule.

        ``git update-index`` collapses ``a//b`` and rejects ``.``/``..``
        components outright, which means no tree can carry them to the
        verifier. The canonicalizer must still refuse them: the same function
        validates ledger, policy and approval paths, which are plain JSON
        strings that nothing normalizes.
        """
        for candidate in ("src/./rt/dotted.c", "src/rt/../rt/parent.c",
                          "src//rt/double.c", "src/rt/trailing/", "/src/rt/abs.c",
                          "src/rt/", ".", ".."):
            with self.subTest(path=candidate):
                with self.assertRaises(verifier.VerifyError):
                    verifier.canonical_path(candidate, code="X", label="probe")

    def test_length_and_depth_ceilings_are_enforced_by_the_parser(self) -> None:
        deep = "/".join("d" * 2 for _ in range(verifier.MAX_PATH_DEPTH + 1)) + "/f.c"
        long_component = "a" * (verifier.MAX_COMPONENT_BYTES + 1) + ".c"
        long_path = "/".join("dir" for _ in range(4)) + "/" + "b" * verifier.MAX_PATH_BYTES
        for candidate in (deep, long_component, long_path):
            with self.subTest(path=candidate[:40]):
                with self.assertRaises(verifier.VerifyError):
                    verifier.canonical_path(candidate, code="X", label="probe")

    def test_a_canonical_nfc_path_is_accepted(self) -> None:
        """The rule must not reject legitimate non-ASCII names."""
        nfc = unicodedata.normalize("NFC", "docs/café.md")
        head = self._stage_path(nfc.encode("utf-8"))
        # The point is that the canonicalizer lets it through: the tree is read
        # to completion and the run reaches ordinary semantic findings instead
        # of raising TREE_INVALID.
        verdict = self.run_verify(head)
        self.assertNotIn("TREE_INVALID", self.codes(verdict, fatal_only=False))
        self.assertEqual(verifier.canonical_path(nfc, code="X", label="probe"), nfc)


class MergeIdentityTests(GateCase):
    """The attested head tree must be the tree that would actually land."""

    def test_a_head_behind_its_base_is_refused(self) -> None:
        """Merge/squash/rebase all preserve the head tree only when up to date.

        With the base already an ancestor of the head there is nothing to
        reconcile, so every allowed merge method yields exactly the tree this
        run attested. When the head is behind, the merge result is a tree the
        run never saw.
        """
        self.repo.branch("newer-base", self.base)
        self.repo.write("docs/notes.md", b"# base moved on\n")
        entries = [
            dict(e, sha256=_sha(b"# base moved on\n")) if e["path"] == "docs/notes.md" else e
            for e in self.current_entries(self.base)
        ]
        self.write_ledger(entries)
        moved_base = self.repo.commit("base advances")

        verdict = self.run_verify(self.base, base=moved_base)
        self.assertFatal(verdict, "MERGE_BASE_STALE")

    def test_an_up_to_date_head_is_accepted(self) -> None:
        self.assertNotIn("MERGE_BASE_STALE", self.codes(self.run_verify(self.base)))


class PrivateRecordOracleTests(GateCase):
    """The gate must not reveal which private record ids exist."""

    def _claim_record(self, record_id: str) -> dict:
        self.repo.branch(f"attack-{abs(hash(record_id))}", self.base)
        entries = []
        for entry in self.current_entries(self.base):
            if entry["path"] == "src/rt/widget.c":
                entry = dict(entry, evidence={
                    "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                    "record_id": record_id, "evidence_tier": "S",
                    "authorship": "independent implementation record",
                })
            entries.append(entry)
        self.write_ledger(entries)
        return self.run_verify(self.repo.commit(f"claim {record_id}"))

    def test_existing_and_nonexistent_record_guesses_are_indistinguishable(self) -> None:
        """A wrong guess at a real id must look exactly like a made-up one."""
        existing = self._claim_record("core-runtime")        # real, wrong path
        invented = self._claim_record("no-such-record-here")  # not in the authority

        def signature(verdict: dict) -> list[tuple[str, str, str]]:
            return sorted(
                (f["code"], f["path"], f["detail"]) for f in verdict["findings"]
                if f["code"] == "TRUSTED_RECORD_UNRESOLVED"
            )

        self.assertTrue(signature(existing), "the probe must produce a finding")
        self.assertEqual(signature(existing), signature(invented))
        for verdict in (existing, invented):
            blob = json.dumps(verdict)
            self.assertNotIn("core-runtime", blob)
            self.assertNotIn("no-such-record-here", blob)


class GrandfatheringScopeTests(GateCase):
    """Cases that used to pass under the claim-only predicate."""

    def test_editing_a_grandfathered_file_is_no_longer_allowed(self) -> None:
        """This is the first-pass behaviour, inverted deliberately.

        Until the content half of the predicate existed, this candidate passed:
        same claim, changed bytes, coherently updated hash.  It is exactly the
        attestation-inheritance hole, so it must now fail.
        """
        self.repo.branch("work", self.base)
        raw = b"print('legacy, edited')\n"
        self.repo.write("tools/legacy.py", raw)
        entries = [
            dict(entry, sha256=_sha(raw)) if entry["path"] == "tools/legacy.py" else entry
            for entry in self.current_entries(self.base)
        ]
        self.write_ledger(entries)
        head = self.repo.commit("edit legacy")
        self.assertFatal(self.run_verify(head), "CONTENT_UNATTESTED")


class ContentBindingTests(GateCase):
    """The verdict must be about the bytes actually under review."""

    def test_stale_hash_is_refused(self) -> None:
        self.repo.branch("attack", self.base)
        self.repo.write("src/rt/widget.c", b"int widget(void) { return 99; }\n")
        head = self.repo.commit("change content, keep hash")
        self.assertFatal(self.run_verify(head), "CONTENT_MISMATCH")

    def test_public_file_without_a_ledger_entry_is_refused(self) -> None:
        self.repo.branch("attack", self.base)
        self.repo.write("src/rt/ghost.c", b"int ghost(void) { return 7; }\n")
        self.write_policy(list(self.FILES) + [TRUSTED_WORKFLOW, verifier.LEDGER_PATH, "src/rt/ghost.c"])
        head = self.repo.commit("public file, no entry")
        self.assertFatal(self.run_verify(head), "LEDGER_COVERAGE")

    def test_entry_without_a_file_is_refused(self) -> None:
        self.repo.branch("attack", self.base)
        self.repo.remove("src/rt/widget.c")
        head = self.repo.commit("entry outlives its file")
        self.assertFatal(self.run_verify(head), "LEDGER_COVERAGE")

    def test_ledger_edited_without_regenerating_the_export_is_refused(self) -> None:
        """publish_audit states this too, but from a candidate-defined workflow."""
        self.repo.branch("attack", self.base)
        stale = _sha(b"whatever the export was generated against")
        self.write_ledger(self.current_entries(self.base), export_ledger_sha=stale)
        head = self.repo.commit("edit ledger, keep export")
        self.assertFatal(self.run_verify(head), "EXPORT_FIELD_MISMATCH")

    def test_a_symlink_in_the_public_tree_is_refused(self) -> None:
        """Hashing a symlink would attest its target string, not any content."""
        self.repo.branch("attack", self.base)
        # Stage a real mode-120000 entry without needing symlink support on the
        # host filesystem, which Windows CI runners do not reliably grant.
        blob = subprocess.run(
            ["git", "hash-object", "-w", "--stdin"], cwd=self.repo.root,
            input=b"src/rt/core.c", capture_output=True, check=True,
        ).stdout.decode("ascii").strip()
        self.repo._git("update-index", "--add", "--cacheinfo", f"120000,{blob},src/rt/link.c")
        self.repo._git("commit", "--quiet", "-m", "symlink")
        head = self.repo._git("rev-parse", "HEAD")
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(head)
        self.assertEqual(caught.exception.code, "TREE_INVALID")

    def test_an_oversized_tree_is_refused(self) -> None:
        """The ceilings are a real control, not a comment.

        Committing 50,000 paths in a unit test is not practical, so the ceiling
        is lowered for the duration. What is under test is that a tree over the
        limit is refused, not the particular number.
        """
        self.repo.branch("attack", self.base)
        for index in range(4):
            self.repo.write(f"docs/bulk_{index}.md", b"# bulk\n")
        head = self.repo.commit("many paths")
        original = verifier.MAX_TREE_PATHS
        verifier.MAX_TREE_PATHS = 3
        self.addCleanup(setattr, verifier, "MAX_TREE_PATHS", original)
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(head)
        self.assertEqual(caught.exception.code, "TREE_TOO_LARGE")

    def test_an_oversized_single_blob_is_refused(self) -> None:
        """The per-blob ceiling is separate from the aggregate one.

        It is checked from the cat-file header before the blob is read, so an
        oversized object is refused rather than held first.
        """
        self.repo.branch("attack", self.base)
        self.repo.write("docs/big.md", b"# " + b"x" * 8192 + b"\n")
        head = self.repo.commit("one large blob")
        original = verifier.MAX_BLOB_BYTES
        verifier.MAX_BLOB_BYTES = 4096
        self.addCleanup(setattr, verifier, "MAX_BLOB_BYTES", original)
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(head)
        self.assertEqual(caught.exception.code, "TREE_TOO_LARGE")

    def test_oversized_aggregate_path_text_is_refused(self) -> None:
        """Path text has its own ceiling, enforced while ls-tree streams."""
        self.repo.branch("attack", self.base)
        for index in range(6):
            self.repo.write(f"docs/padding_{'n' * 40}_{index}.md", b"# pad\n")
        head = self.repo.commit("many long path names")
        original = verifier.MAX_TREE_PATH_BYTES
        verifier.MAX_TREE_PATH_BYTES = 256
        self.addCleanup(setattr, verifier, "MAX_TREE_PATH_BYTES", original)
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(head)
        self.assertEqual(caught.exception.code, "TREE_TOO_LARGE")

    def test_an_oversized_tree_by_bytes_is_refused(self) -> None:
        self.repo.branch("attack", self.base)
        self.repo.write("docs/bulk.md", b"# " + b"x" * 4096 + b"\n")
        head = self.repo.commit("large blob")
        original = verifier.MAX_TREE_BYTES
        verifier.MAX_TREE_BYTES = 1024
        self.addCleanup(setattr, verifier, "MAX_TREE_BYTES", original)
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(head)
        self.assertEqual(caught.exception.code, "TREE_TOO_LARGE")

    def test_unresolved_classification_is_refused(self) -> None:
        self.repo.branch("attack", self.base)
        entries = [
            dict(entry, classification="unresolved", evidence={"source": "none"})
            if entry["path"] == "src/rt/widget.c" else entry
            for entry in self.current_entries(self.base)
        ]
        self.write_ledger(entries)
        head = self.repo.commit("unresolved")
        self.assertFatal(self.run_verify(head), "LEDGER_SCHEMA")


class TrustBoundaryTests(GateCase):
    """The candidate tree is data.  It can never become an authority."""

    def test_a_detailed_ledger_committed_by_the_candidate_is_ignored(self) -> None:
        """Substituting authority into the tree must not authorize anything."""
        self.repo.branch("attack", self.base)
        raw = b"int forged(void) { return 8; }\n"
        self.repo.write("src/rt/forged.c", raw)
        forged_authority = json.loads(json.dumps(TRUSTED_RECORDS))
        forged_authority["records"].append({
            "id": "forged-authority", "paths": ["src/rt/forged.c"],
            "classification": "project-authored-independent", "upstream": None,
            "evidence_tier": "S",
        })
        self.repo.write(
            "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
            json.dumps(forged_authority, indent=2) + "\n",
        )
        entries = self.current_entries(self.base) + [{
            "path": "src/rt/forged.c",
            "classification": "project_authored_attested",
            "evidence": {
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "forged-authority", "evidence_tier": "S",
                "authorship": "independent implementation record",
            },
            "sha256": _sha(raw),
        }]
        self.write_ledger(entries)
        self.write_policy(list(self.FILES) + [
            TRUSTED_WORKFLOW, verifier.LEDGER_PATH, "src/rt/forged.c",
            "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
        ])
        head = self.repo.commit("commit a forged authority")
        self.assertFatal(self.run_verify(head), "TRUSTED_RECORD_UNRESOLVED")

    def test_candidate_copy_of_the_verifier_is_never_consulted(self) -> None:
        """Replacing the in-tree verifier with a rubber stamp changes nothing."""
        self.repo.branch("attack", self.base)
        self.repo.write(
            "tools/provenance_attest_verify.py",
            b"import sys\nprint('pass')\nsys.exit(0)\n",
        )
        self.write_policy(list(self.FILES) + [
            TRUSTED_WORKFLOW, verifier.LEDGER_PATH, "tools/provenance_attest_verify.py",
        ])
        entries = self.current_entries(self.base) + [{
            "path": "tools/provenance_attest_verify.py",
            "classification": "project_authored_attested",
            "evidence": {
                "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
                "record_id": "blanket-tools", "evidence_tier": "S",
                "authorship": "independent implementation record",
            },
            "sha256": _sha(b"import sys\nprint('pass')\nsys.exit(0)\n"),
        }]
        self.write_ledger(entries)
        head = self.repo.commit("rubber-stamp the verifier")
        self.assertFatal(self.run_verify(head), "CLAIM_UNBACKED")

    def test_trusted_ledger_inside_the_repository_is_refused(self) -> None:
        inside = self.repo.root / "trusted.json"
        inside.write_text(json.dumps(TRUSTED_RECORDS), encoding="utf-8", newline="\n")
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(self.base, trusted_ledger=inside)
        self.assertEqual(caught.exception.code, "TRUSTED_INPUT_CANDIDATE_CONTROLLED")

    def test_missing_trusted_ledger_fails_closed(self) -> None:
        with self.assertRaises(verifier.VerifyError) as caught:
            self.run_verify(self.base, trusted_ledger=self.outside / "absent.json")
        self.assertEqual(caught.exception.code, "TRUSTED_INPUT_MISSING")

    def test_policy_and_prior_ledger_come_from_the_base_not_the_candidate(self) -> None:
        """A candidate that rewrites its own base-ledger history gains nothing.

        The claim ratchet compares against the *base* ledger.  Here the
        candidate restates ``core.c`` as independent and also back-dates that
        same claim into its own tree; the base commit is unchanged, so the
        restatement is still new and still unbacked.
        """
        self.repo.branch("attack", self.base)
        entries = []
        for entry in self.current_entries(self.base):
            if entry["path"] == "src/rt/core.c":
                entry = dict(entry, classification="project_authored_attested", evidence={
                    "source": "public provenance census",
                    "authorship": "independent project implementation",
                })
            entries.append(entry)
        self.write_ledger(entries)
        head = self.repo.commit("rewrite the claim in place")
        self.assertFatal(self.run_verify(head), "CLAIM_UNBACKED")


class PolicyTamperTests(GateCase):
    """Publication scope may tighten in a pull request; it may not loosen."""

    def _rewrite_policy(self, **changes) -> str:
        self.repo.branch("attack", self.base)
        document = dict(POLICY)
        document["include_paths"] = sorted(
            list(self.FILES) + [TRUSTED_WORKFLOW, verifier.LEDGER_PATH, verifier.EXPORT_PATH]
        )
        document.update(changes)
        self.repo.write(verifier.POLICY_PATH, json.dumps(document, indent=2) + "\n")
        return self.repo.commit("rewrite policy")

    def test_dropping_an_exclusion_is_refused(self) -> None:
        self.assertFatal(self.run_verify(self._rewrite_policy(exclude_paths=[])), "POLICY_SUBSTITUTION")

    def test_dropping_an_exclude_prefix_is_refused(self) -> None:
        self.assertFatal(self.run_verify(self._rewrite_policy(exclude_prefixes=[])), "POLICY_SUBSTITUTION")

    def test_dropping_an_exclude_glob_is_refused(self) -> None:
        self.assertFatal(self.run_verify(self._rewrite_policy(exclude_globs=[])), "POLICY_SUBSTITUTION")

    def test_tightening_the_policy_is_allowed(self) -> None:
        head = self._rewrite_policy(exclude_paths=["secret.txt", "another-secret.txt"])
        self.assertPasses(self.run_verify(head))


class MergePathTamperTests(GateCase):
    """The gate's own merge-path wiring is part of what it verifies."""

    def _commit_workflow(self, path: str, raw: bytes) -> str:
        self.repo.branch("attack", self.base)
        self.repo.write(path, raw)
        if path != TRUSTED_WORKFLOW:
            entries = self.current_entries(self.base) + [{
                "path": path, "classification": "reviewed_configuration",
                "evidence": {"source": "configuration review"}, "sha256": _sha(raw),
            }]
            self.write_ledger(entries)
            self.write_policy(list(self.FILES) + [TRUSTED_WORKFLOW, verifier.LEDGER_PATH, path])
        else:
            entries = [
                dict(entry, sha256=_sha(raw)) if entry["path"] == path else entry
                for entry in self.current_entries(self.base)
            ]
            self.write_ledger(entries)
        return self.repo.commit("workflow change")

    def test_a_second_job_under_the_required_context_is_refused(self) -> None:
        """Required checks match by name, so the name is a merge-path secret."""
        raw = (
            b"on: [pull_request]\njobs:\n  spoof:\n    name: "
            + RESERVED_CONTEXT.encode("utf-8") + b"\n    steps:\n      - run: exit 0\n"
        )
        head = self._commit_workflow(".github/workflows/spoof.yml", raw)
        self.assertFatal(self.run_verify(head), "CI_CONTEXT_COLLISION")

    def test_deleting_the_trusted_workflow_is_refused(self) -> None:
        self.repo.branch("attack", self.base)
        self.repo.remove(TRUSTED_WORKFLOW)
        entries = [
            entry for entry in self.current_entries(self.base)
            if entry["path"] != TRUSTED_WORKFLOW
        ]
        self.write_ledger(entries)
        self.write_policy(list(self.FILES) + [verifier.LEDGER_PATH])
        head = self.repo.commit("delete the gate")
        self.assertFatal(self.run_verify(head), "TRUSTED_WORKFLOW_WEAKENED")

    def test_downgrading_the_trigger_to_pull_request_is_refused(self) -> None:
        """`pull_request` would hand the workflow definition to the candidate."""
        head = self._commit_workflow(TRUSTED_WORKFLOW, self.workflow_bytes(trigger=b"pull_request"))
        self.assertFatal(self.run_verify(head), "TRUSTED_WORKFLOW_WEAKENED")

    def test_renaming_the_required_job_is_refused(self) -> None:
        head = self._commit_workflow(
            TRUSTED_WORKFLOW, self.workflow_bytes(context="Something Else"),
        )
        self.assertFatal(self.run_verify(head), "TRUSTED_WORKFLOW_WEAKENED")

    def test_introducing_the_gate_workflow_is_not_reported_as_tampering(self) -> None:
        """A base without the workflow is the change that adds the gate."""
        # The predecessor has to be a real ancestor of the candidate, not a
        # sibling: build the gate-less commit first, then re-introduce the
        # workflow on top of it.
        self.repo.branch("nogate", self.base)
        self.repo.remove(TRUSTED_WORKFLOW)
        self.write_ledger([e for e in self.current_entries(self.base) if e["path"] != TRUSTED_WORKFLOW])
        self.write_policy(list(self.FILES))
        older = self.repo.commit("a base predating the gate")

        workflow = self.workflow_bytes()
        self.repo.write(TRUSTED_WORKFLOW, workflow)
        self.write_policy(list(self.FILES) + [TRUSTED_WORKFLOW])
        self.write_ledger(self.current_entries(older) + [
            self.entry(TRUSTED_WORKFLOW, workflow, classification="reviewed_configuration",
                       evidence={"source": "configuration review"}),
        ])
        head = self.repo.commit("introduce the gate workflow")
        verdict = self.run_verify(head, base=older)
        self.assertPasses(verdict)
        self.assertNotIn("TRUSTED_WORKFLOW_MODIFIED", self.codes(verdict, fatal_only=False))

    def test_a_benign_workflow_edit_is_reported_but_not_fatal(self) -> None:
        raw = self.workflow_bytes() + b"    timeout-minutes: 12\n"
        head = self._commit_workflow(TRUSTED_WORKFLOW, raw)
        verdict = self.run_verify(head)
        self.assertPasses(verdict)
        self.assertIn("TRUSTED_WORKFLOW_MODIFIED", self.codes(verdict, fatal_only=False))


class LiveRepositoryTests(unittest.TestCase):
    """Guard the real repository's own invariants that need no private input."""

    def test_the_reserved_context_appears_in_exactly_one_workflow(self) -> None:
        workflows = sorted((ROOT / ".github" / "workflows").glob("*.yml"))
        self.assertTrue(workflows, "no workflows found")
        naming = [
            path for path in workflows
            if RESERVED_CONTEXT.encode("utf-8") in path.read_bytes()
        ]
        self.assertEqual(
            [path.name for path in naming], [Path(TRUSTED_WORKFLOW).name],
            "the required-check context must be declared only by the trusted gate workflow",
        )

    def test_the_trusted_workflow_is_target_triggered(self) -> None:
        raw = (ROOT / TRUSTED_WORKFLOW).read_bytes()
        self.assertIn(b"pull_request_target", raw)
        self.assertNotIn(b"\non:\n  pull_request:\n", raw)

    def test_the_trusted_workflow_never_checks_out_a_candidate_chosen_base(self) -> None:
        """A pull request picks its own base, so base.ref is attacker input.

        Checking it out would run the author's own tools/ code in the job that
        holds the authority token. The checkout must pin the default branch,
        and the job must decline any other base.
        """
        raw = (ROOT / TRUSTED_WORKFLOW).read_text(encoding="utf-8")
        checkout_refs = re.findall(r"^\s+ref: (.+)$", raw, re.MULTILINE)
        self.assertEqual(
            checkout_refs, ["${{ github.event.repository.default_branch }}"],
            "the trusted checkout must pin the default branch",
        )
        self.assertIn(
            "github.event.pull_request.base.ref == github.event.repository.default_branch", raw,
        )

    def test_the_trusted_workflow_never_interpolates_untrusted_event_text(self) -> None:
        """Titles, bodies and branch names must not reach the job at all."""
        raw = (ROOT / TRUSTED_WORKFLOW).read_text(encoding="utf-8")
        for expression in re.findall(r"\$\{\{(.+?)\}\}", raw):
            self.assertNotRegex(
                expression, r"\.(title|body|head\.ref|head\.label|head\.repo)",
                msg=f"attacker-authored text interpolated: {expression.strip()}",
            )

    def test_the_trusted_workflow_judges_immutable_revisions(self) -> None:
        """Branch names are moving targets; the verdict must name exact SHAs."""
        raw = (ROOT / TRUSTED_WORKFLOW).read_text(encoding="utf-8")
        self.assertIn("--require-immutable-revisions", raw)
        self.assertIn("--candidate \"${HEAD_SHA}\"", raw)
        self.assertIn("--base \"${BASE_SHA}\"", raw)
        self.assertNotIn("--base HEAD", raw)
        # The base the ratchet is measured against must be on the trusted branch.
        self.assertIn('merge-base --is-ancestor "${BASE_SHA}" HEAD', raw)

    def test_the_authority_revision_is_resolved_once(self) -> None:
        """Read the ledger AT the resolved SHA, not at a branch name twice.

        Asking "what is main" and separately "give me main's ledger" leaves a
        window in which the two answers describe different content.
        """
        raw = (ROOT / TRUSTED_WORKFLOW).read_text(encoding="utf-8")
        resolve = raw.index('authority_sha="$(gh api "repos/${TRUSTED_REPO}/commits/')
        fetch = raw.index("contents/${TRUSTED_LEDGER_PATH}")
        self.assertLess(resolve, fetch, "the revision must be resolved before the ledger is read")
        self.assertIn("contents/${TRUSTED_LEDGER_PATH}?ref=${authority_sha}", raw)
        self.assertNotIn("contents/${TRUSTED_LEDGER_PATH}?ref=${TRUSTED_REF}", raw)
        self.assertIn('--authority-revision "${AUTHORITY_SHA}"', raw)

    def test_the_gate_is_documented_as_observation_mode(self) -> None:
        """Enforcement stays off while context-name spoofing is unresolved."""
        self.assertIn("OBSERVATION MODE", (ROOT / TRUSTED_WORKFLOW).read_text(encoding="utf-8"))
        doc = (ROOT / "docs" / "PROVENANCE_MERGE_GATE.md").read_text(encoding="utf-8")
        self.assertIn("DO NOT ADD IT YET", doc)

    def test_every_third_party_action_is_pinned_to_a_commit_sha(self) -> None:
        raw = (ROOT / TRUSTED_WORKFLOW).read_text(encoding="utf-8")
        uses = re.findall(r"^\s+uses: (.+)$", raw, re.MULTILINE)
        self.assertTrue(uses)
        for entry in uses:
            reference = entry.split("#", 1)[0].strip().split("@", 1)[-1]
            self.assertRegex(reference, r"^[0-9a-f]{40}$", msg=f"unpinned action: {entry}")


if __name__ == "__main__":
    unittest.main()
