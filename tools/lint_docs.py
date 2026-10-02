#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Lightweight documentation freshness and staleness linter.

Detects deterministic classes of staleness that should not require network access:
- ephemeral CI-run wording in the evergreen README;
- volatile ``as of`` status claims in the evergreen README;
- obsolete private/public repository-topology statements;
- known retired public URLs outside explicitly historical evidence records;
- repository-relative Markdown links whose targets are absent or escape the tree;
- false "checked-in"/"Git-ignored" claims about the publication-excluded retail
  title manifest while it is untracked;
- links to repository commits unavailable in public history;
- frozen hand-written HLE census numbers that must come from the generator;
- a hand-written HLE registration / NID / fake-success count presented as live
  state on a CURRENT page (the census is a generated artifact);
- title-manifest README drift from the actual public manifest inventory;
- a missing non-CURRENT status marker on the dated toolchain baseline;
- a capability-disposition row with no valid status, a PASS/PARTIAL row that
  names no repository test path that exists, or an UNBUILT/DROPPED row that
  names no tracking issue.

Live GitHub object existence/state is intentionally handled by ``audit_public_issue_links.py``.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
from collections.abc import Iterator
from urllib.parse import unquote

try:
    from .nk_core.git_isolation import isolated_git_env
except ImportError:
    from nk_core.git_isolation import isolated_git_env

ROOT = pathlib.Path(__file__).resolve().parent.parent

EPHEMERAL_RUN_PATTERNS = [
    re.compile(
        r"latest (?:full |successful )?(?:hosted )?run (?:was |is )?(?:run )?`?\d+`?",
        re.IGNORECASE,
    ),
    re.compile(r"run `\d{8,}`", re.IGNORECASE),
]

README_DATED_STATUS_PATTERNS = [
    re.compile(
        r"\bas of (?:january|february|march|april|may|june|july|august|september|october|november|december|\d{4}-\d{2}-\d{2})\b",
        re.IGNORECASE,
    ),
]

OBSOLETE_TOPOLOGY_PATTERNS = [
    re.compile(r"(?:The|This) repository is \*\*(?:still )?private\*\*", re.IGNORECASE),
    re.compile(r"Keep this repository private", re.IGNORECASE),
    re.compile(r"create a \*\*(?:new )?public repository", re.IGNORECASE),
    re.compile(r"For the \*\*new public repository\*\*", re.IGNORECASE),
]

# Regression denylist for URLs known to have belonged to pre-export/private-era tracking.
# This is not a general proof of object existence; the networked auditor owns that question.
#
# EXPIRY.  GitHub numbers issues and pull requests from one sequence, so the public
# repository steadily REALLOCATES these numbers to real objects of its own.  A denylist
# entry is only meaningful while the public repository has not yet reached it; once it
# has, the URL resolves to a live public object and flagging it is a false positive that
# blocks legitimate work -- which is how this was found: citing the (real, new) public
# issue 98 failed this lint.
#
# An offline check that fires on live objects is worse than no check at all. Retiring an
# entry loses only an offline regression check; leaving one in place past its number
# breaks the build. The guard below refuses any entry at or below the frontier, so the
# two can never drift apart silently.
#
# The frontier is a LOWER BOUND on what the public repository has allocated, and it only
# ever moves up. State it as a bound rather than as an exact count: an exact count is stale
# the moment the next object is opened, and the dangerous direction is a frontier that is
# too LOW, because that is what lets a soon-to-be-live number sit in the denylist unnoticed.
# Raise it whenever this file is touched during a sweep. The public sequence had allocated
# at least through 410 as of 2026-09-23 (issues #278-#400 and PRs through #410), so every
# number up to 410 is a live public object.
PUBLIC_ISSUE_NUMBER_FRONTIER = 410

# Every former private-era number is now at or below the frontier, i.e. reallocated to a
# live public issue or PR, so none may be flagged any more.
RETIRED_PRIVATE_ISSUE_NUMBERS: tuple[int, ...] = ()

_reallocated = sorted(n for n in RETIRED_PRIVATE_ISSUE_NUMBERS
                      if n <= PUBLIC_ISSUE_NUMBER_FRONTIER)
if _reallocated:
    # A raise, not an assert: `python -O` strips asserts, and this guard exists to stop
    # the linter flagging live public objects.
    raise ValueError(
        "denylisted issue number(s) already reallocated by the public repository "
        f"(frontier {PUBLIC_ISSUE_NUMBER_FRONTIER}): {_reallocated}; remove them "
        "rather than flagging a live object"
    )

def _retired_issue_url_pattern(numbers: tuple[int, ...]) -> re.Pattern[str]:
    # An empty alternation would match every issue URL, so an empty denylist must
    # compile to a pattern that never matches.
    if not numbers:
        return re.compile(r"(?!)")
    return re.compile(
        r"github\.com/Jstar269/nakagawa-recomp/(?:issues|pull)/(?:"
        + "|".join(str(n) for n in numbers)
        + r")\b"
    )


RETIRED_PRIVATE_ISSUE_URLS = _retired_issue_url_pattern(RETIRED_PRIVATE_ISSUE_NUMBERS)

HISTORICAL_EVIDENCE_DOCS = {
    "docs/STATUS_HISTORY.md",
    "docs/ROADMAP.md",
    "docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md",
    "docs/COVERAGE_LEDGER.md",
}

# Frozen document-truth snapshots are evidence of a past sweep, not maintained
# prose. They deliberately quote the stale claims they record, so every
# mutable-fact check below skips them. Later sweeps add a new dated snapshot
# rather than editing these files.
PROJECT_TRUTH_SNAPSHOT_PREFIX = "docs/research/project-truth/"

# The retail HST manifest is publication-excluded and never checked in (with a
# .gitignore accident guard). While it is untracked, no maintained document may
# describe it as checked in or conflate publication exclusion with ignoring.
# Negated forms ("intentionally not checked in", "never checked in") are the
# correct wording and must not match.
HST_CHECKED_IN_MANIFEST_PAT = re.compile(
    r"(?<!not )(?<!never )checked-in HST(?: retail)? manifest", re.IGNORECASE
)
HST_GITIGNORED_MANIFEST_PAT = re.compile(
    r"Git-ignored (?:HST(?: retail| title)? |title )?manifest|local ignored manifest",
    re.IGNORECASE,
)

# Frozen hand-written HLE census strings. The live census is generated by
# tools/test_compat_manifest.py from tools/compat_overrides.py; prose must cite
# the gate, never restate these numbers.
FROZEN_HLE_CENSUS_PATS = [
    re.compile(r"46 title guest addresses across 59 sites"),
]

# --- HLE census doc guard (#341 / #363 Phase 0 Step 0.3) ---------------------------
#
# The semantic census is a tool artifact, not prose. `tools/hle_manifest.py`
# emits it twice from one source: `--census-json` (machine-readable) and
# `--census-markdown` (the block embedded in docs/HLE_AND_WORKAROUND_INVENTORY.md
# and pinned against the generator by tools/test_hle_manifest.py). Nothing
# regenerates or checks a number typed into a paragraph, so a hand-written
# census count is stale by construction -- and a stale count is exactly what a
# consumer reads as live HLE status. This guard therefore rejects ANY hard-coded
# census count in a CURRENT document, not a denylist of the numbers that have
# already drifted, so the next drift is caught before it is published.
#
# Three escapes, each checked rather than assumed:
#   1. docs/README.md classifies the page as ARCHIVED / HISTORICAL / REFERENCE /
#      SUPERSEDED / DRAFT, or it lives under docs/archive/;
#   2. the line is inside the generated census block, whose numbers come from
#      the generator and are drift-checked there;
#   3. the line itself labels the number historical / capture-time, which is how
#      an unavoidable example from a past audit stays honestly labelled.
#
# An unindexed document counts as CURRENT. The taxonomy in docs/README.md is
# maintained, so a missing row must fail closed instead of buying a free pass.
HLE_CENSUS_BLOCK_BEGIN = "<!-- BEGIN GENERATED HLE STATUS CENSUS -->"
HLE_CENSUS_BLOCK_END = "<!-- END GENERATED HLE STATUS CENSUS -->"

# The nouns the generated census uses for its counts. Generic stubs, dedicated
# handlers, refusals and NIDs are one census, so a hand-written count of any of
# them is the same kind of rot.
HLE_CENSUS_COUNT_PAT = re.compile(
    r"\b\d{1,4}\s+(?:(?:are|were|is|remain|remains|left)\s+)?(?:import\s+)?"
    r"(?:registrations?|NIDs?|NID\s+registrations?"
    r"|fake[-\s]success(?:\s+(?:stubs?|registrations?|NIDs?|handlers?|entries))?"
    r"|dedicated\s+handlers?|handlers?"
    r"|controlled[-\s]unsupported(?:\s+registrations?)?)\b",
    re.IGNORECASE,
)

# An explicit historical label on the same line. This is deliberately a list of
# the words a reviewer actually writes when preserving a past figure, not a
# licence: the count still has to read as past evidence on its own line. A bare
# past-tense verb is only accepted *directly before* the count, with at most a
# quantity qualifier between ("the census was 380 registrations", "it fell to
# about 51"), and never when the clause goes on to describe a change ("there were
# 380 registrations added"), because "were added" is not a capture-time label.
# Once one count on a line is past evidence, later counts on that line share its
# context ("was 380 registrations and 51 stubs") unless the line also speaks of
# the present.
HLE_CENSUS_HISTORICAL_LABEL_PAT = re.compile(
    r"histor|capture[- ]time|\bthen\b|previous|former|\bas of\b|snapshot"
    r"|supersed|no longer|\b[0-9a-f]{7,40}\b",
    re.IGNORECASE,
)
HLE_CENSUS_PAST_COUNT_PAT = re.compile(
    r"\b(?:was|were|had|remained|reached|fell|rose)"
    r"(?:\s+(?:to|at|about|around|roughly|approximately|nearly|almost|only|just))*"
    r"\s+$",
    re.IGNORECASE,
)
# A change verb anywhere later in the count's clause turns "there were N ... added" into a
# statement about what happened, not a captured figure.
HLE_CENSUS_CHANGE_AFTER_COUNT_PAT = re.compile(
    r"^[^.;:]*?\b(?:added|removed|registered|implemented|retired|dropped)\b",
    re.IGNORECASE,
)
HLE_CENSUS_PRESENT_PAT = re.compile(r"\b(?:now|currently|today|is|are)\b", re.IGNORECASE)

NON_CURRENT_DOC_STATUSES = frozenset(
    {"ARCHIVED", "HISTORICAL", "REFERENCE", "SUPERSEDED", "DRAFT"}
)
ARCHIVE_DOC_PREFIX = "docs/archive/"

REPO_COMMIT_URL_PAT = re.compile(
    r"github\.com/Jstar269/nakagawa-recomp/(?:tree|blob|commit)/([0-9a-f]{7,40})\b"
)

TOOLCHAIN_BASELINE_DOC = "docs/archive/TOOLCHAIN_BASELINE_2026-08.md"
TOOLCHAIN_BASELINE_STATUS_PAT = re.compile(
    r"^STATUS\s*=\s*(HISTORICAL|REFERENCE)\b", re.MULTILINE
)

DOCS_README = "docs/README.md"

TITLES_DIR = "assets/titles"
TITLES_README = "assets/titles/README.md"

# --- capability disposition table ---------------------------------------------------
#
# The native player is the product's only UI, so every capability the retired
# localhost dashboard used to provide must carry an explicit disposition: what it
# is now, which surface owns it, and either a repository test path that exists or
# the tracking issue for what is not built. A row that cannot name its evidence is
# the failure mode this table exists to prevent -- an unnamed boundary reads as a
# working feature.
#
# FAIL CLOSED. An absent, duplicated or inverted marker pair is an error rather
# than an empty section, and so is a section with no data rows: an empty inventory
# would silently agree with "nothing is broken" while consumers read the silence as
# a verdict. The same reasoning makes the status vocabulary closed -- `IN_PROGRESS`,
# `NOT_SUPPORTED` and `PASS (something)` are the older vocabularies this replaces,
# and accepting them would let a row escape both the evidence and the issue rules.
CAPABILITY_MATRIX_DOC = "docs/NATIVE_UI_REGRESSION_MATRIX.md"
CAPABILITY_TABLE_BEGIN = "<!-- capability-disposition:begin -->"
CAPABILITY_TABLE_END = "<!-- capability-disposition:end -->"
CAPABILITY_STATUSES = ("PASS", "PARTIAL", "UNBUILT", "DROPPED")
CAPABILITY_COLUMN_COUNT = 5
CAPABILITY_HEADER_CELLS = ("ID", "CAPABILITY", "STATUS", "SURFACE", "OWNING EVIDENCE")

# A backticked token containing at least one slash is a repository-relative path.
# A bare `mingw32-make target`, a `SR_HUD`-style switch or a `<angle>` placeholder
# contains none, so documenting a command or an env var never satisfies the rule.
CAPABILITY_PATH_TOKEN_PAT = re.compile(r"`([A-Za-z0-9_.+-]+(?:/[A-Za-z0-9_.+-]+)+)`")

# An issue or pull request number, either as a bare `#522` or inside a repository
# URL. Both are cited in practice, so both are accepted.
CAPABILITY_ISSUE_REF_PAT = re.compile(r"(?:issues|pull)/(\d+)\b|#(\d+)\b")

MARKDOWN_LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)\)")
EXTERNAL_LINK_PREFIXES = ("http://", "https://", "mailto:", "tel:", "data:")


def get_tracked_markdown_files(repo_root: pathlib.Path = ROOT) -> list[pathlib.Path]:
    """Return tracked Markdown files, using Git as authority when available."""
    try:
        res = subprocess.run(
            ["git", "ls-files", "*.md"],
            cwd=repo_root,
            env=isolated_git_env(root=repo_root),
            capture_output=True,
            text=True,
            check=True,
        )
        files = [
            repo_root / line for line in res.stdout.splitlines()
            if line.strip() and (repo_root / line).is_file()
        ]
        if files:
            return sorted(files)
    except (OSError, subprocess.SubprocessError):
        pass

    return sorted(
        path
        for path in repo_root.rglob("*.md")
        if ".git" not in path.relative_to(repo_root).parts
    )


def lint_readme(readme_path: pathlib.Path) -> list[str]:
    errors: list[str] = []
    if not readme_path.is_file():
        return errors

    text = readme_path.read_text(encoding="utf-8")
    for idx, line in enumerate(text.splitlines(), 1):
        for pattern in EPHEMERAL_RUN_PATTERNS:
            if pattern.search(line):
                errors.append(
                    f"README.md:{idx}: contains ephemeral CI run ID reference ('{line.strip()}')"
                )
        for pattern in README_DATED_STATUS_PATTERNS:
            if pattern.search(line):
                errors.append(
                    f"README.md:{idx}: contains volatile dated status claim ('{line.strip()}'); "
                    "move it to the current project status summary or a dated private record"
                )
    return errors


def lint_doc_links_and_topology(
    doc_path: pathlib.Path, repo_root: pathlib.Path = ROOT
) -> list[str]:
    errors: list[str] = []
    if not doc_path.is_file():
        return errors

    try:
        rel_path = doc_path.relative_to(repo_root).as_posix()
    except ValueError:
        rel_path = doc_path.name

    if _is_snapshot_doc(rel_path):
        # Frozen audit snapshots quote the stale claims they record.
        return errors

    is_historical_evidence = rel_path in HISTORICAL_EVIDENCE_DOCS
    text = doc_path.read_text(encoding="utf-8")

    in_fence = False
    root_resolved = repo_root.resolve()
    for idx, line in enumerate(text.splitlines(), 1):
        stripped = line.lstrip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            continue
        if not is_historical_evidence and RETIRED_PRIVATE_ISSUE_URLS.search(line):
            errors.append(f"{rel_path}:{idx}: contains dead private-era issue URL")
        for pattern in OBSOLETE_TOPOLOGY_PATTERNS:
            if pattern.search(line):
                errors.append(
                    f"{rel_path}:{idx}: contains obsolete private-repository topology statement"
                )
        if in_fence:
            continue
        for match in MARKDOWN_LINK_RE.finditer(line):
            raw_target = match.group(1).strip("<>")
            if raw_target.startswith("#") or raw_target.lower().startswith(
                EXTERNAL_LINK_PREFIXES
            ):
                continue
            target = unquote(raw_target.split("#", 1)[0])
            if not target:
                continue
            resolved = (doc_path.parent / target).resolve()
            try:
                resolved.relative_to(root_resolved)
            except ValueError:
                errors.append(
                    f"{rel_path}:{idx}: repository-relative link escapes the tree: {raw_target}"
                )
                continue
            if not resolved.exists():
                errors.append(
                    f"{rel_path}:{idx}: missing repository-relative link target: {raw_target}"
                )

    return errors


def _is_snapshot_doc(rel_path: str) -> bool:
    return rel_path.startswith(PROJECT_TRUTH_SNAPSHOT_PREFIX)


def docs_index_statuses(repo_root: pathlib.Path = ROOT) -> dict[str, str]:
    """Return docs-relative document name -> taxonomy status from docs/README.md."""
    readme = repo_root / DOCS_README
    if not readme.is_file():
        return {}
    statuses: dict[str, str] = {}
    for line in readme.read_text(encoding="utf-8").splitlines():
        if not line.startswith("| `"):
            continue
        cells = [cell.strip() for cell in line.split("|")]
        if len(cells) < 3:
            continue
        statuses[cells[1].strip("`")] = cells[2].upper()
    return statuses


def _is_current_doc(rel_path: str, index_statuses: dict[str, str]) -> bool:
    """Whether a document is a maintained (CURRENT) page rather than dated evidence."""
    if rel_path.startswith(ARCHIVE_DOC_PREFIX):
        return False
    name = rel_path[len("docs/"):] if rel_path.startswith("docs/") else rel_path
    # An unindexed document is CURRENT: the taxonomy is maintained, so a missing
    # row must not silently turn a live page into exempt evidence.
    return index_statuses.get(name, "CURRENT") not in NON_CURRENT_DOC_STATUSES


def _census_prose_lines(text: str) -> Iterator[tuple[int, str]]:
    """Yield (line number, line) for every line outside the generated census block."""
    in_block = False
    for idx, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped == HLE_CENSUS_BLOCK_BEGIN:
            in_block = True
            continue
        if stripped == HLE_CENSUS_BLOCK_END:
            in_block = False
            continue
        if in_block:
            continue
        yield idx, line


def lint_hle_census_counts(
    doc_path: pathlib.Path,
    repo_root: pathlib.Path = ROOT,
    index_statuses: dict[str, str] | None = None,
) -> list[str]:
    """No CURRENT page may present a hand-written HLE census count as live state."""
    errors: list[str] = []
    if not doc_path.is_file():
        return errors

    try:
        rel_path = doc_path.relative_to(repo_root).as_posix()
    except ValueError:
        rel_path = doc_path.name

    if _is_snapshot_doc(rel_path):
        return errors
    if index_statuses is None:
        index_statuses = docs_index_statuses(repo_root)
    if not _is_current_doc(rel_path, index_statuses):
        return errors

    for idx, line in _census_prose_lines(doc_path.read_text(encoding="utf-8")):
        if HLE_CENSUS_HISTORICAL_LABEL_PAT.search(line):
            continue
        past_context = False
        for match in HLE_CENSUS_COUNT_PAT.finditer(line):
            if (HLE_CENSUS_PAST_COUNT_PAT.search(line[: match.start()])
                    and not HLE_CENSUS_CHANGE_AFTER_COUNT_PAT.search(line[match.end():])):
                past_context = not HLE_CENSUS_PRESENT_PAT.search(line)
                continue
            if past_context:
                continue
            errors.append(
                f"{rel_path}:{idx}: hand-codes an HLE census count "
                f"('{match.group(0).strip()}') as live state; label it historical/"
                "capture-time or generate it with tools/hle_manifest.py --census-markdown "
                "(docs/README.md, Mutable facts policy)"
            )
    return errors


def _is_hst_manifest_tracked(repo_root: pathlib.Path = ROOT) -> bool:
    """Return whether the retail HST manifest is tracked (it must not be)."""
    try:
        res = subprocess.run(
            ["git", "ls-files", f"{TITLES_DIR}/hst-ucus98701.json"],
            cwd=repo_root,
            env=isolated_git_env(root=repo_root),
            capture_output=True,
            text=True,
            check=True,
        )
        return bool(res.stdout.strip())
    except (OSError, subprocess.SubprocessError):
        # Without Git there is no tracked-ness evidence; skip rather than guess.
        return True


def _commit_exists_in_history(sha: str, repo_root: pathlib.Path = ROOT) -> bool:
    try:
        res = subprocess.run(
            ["git", "cat-file", "-e", sha],
            cwd=repo_root,
            env=isolated_git_env(root=repo_root),
            capture_output=True,
            text=True,
        )
        if res.returncode == 0:
            return True
        stderr = (res.stderr or "").lower()
        if "not a git repository" in stderr or "not a repo" in stderr:
            # No repository readable here: no evidence either way, so skip
            # rather than flag.
            return True
        return False
    except OSError:
        return True


def lint_doc_truth(
    doc_path: pathlib.Path,
    repo_root: pathlib.Path = ROOT,
    hst_manifest_tracked: bool = True,
) -> list[str]:
    """Enforce high-value document-truth invariants (all offline)."""
    errors: list[str] = []
    if not doc_path.is_file():
        return errors

    try:
        rel_path = doc_path.relative_to(repo_root).as_posix()
    except ValueError:
        rel_path = doc_path.name

    if _is_snapshot_doc(rel_path):
        return errors

    text = doc_path.read_text(encoding="utf-8")
    for idx, line in enumerate(text.splitlines(), 1):
        if not hst_manifest_tracked:
            if HST_CHECKED_IN_MANIFEST_PAT.search(line):
                errors.append(
                    f"{rel_path}:{idx}: calls the retail HST manifest 'checked-in' "
                    "while assets/titles/hst-ucus98701.json is untracked and "
                    "publication-excluded"
                )
            if HST_GITIGNORED_MANIFEST_PAT.search(line):
                errors.append(
                    f"{rel_path}:{idx}: calls the retail HST manifest Git-ignored; "
                    "name the real publication-exclusion mechanism instead"
                )
        for pattern in FROZEN_HLE_CENSUS_PATS:
            if pattern.search(line):
                errors.append(
                    f"{rel_path}:{idx}: restates a frozen HLE census "
                    f"('{pattern.pattern}'); cite tools/test_compat_manifest.py "
                    "instead of hand-maintaining the number"
                )
        for match in REPO_COMMIT_URL_PAT.finditer(line):
            if rel_path in HISTORICAL_EVIDENCE_DOCS:
                continue
            sha = match.group(1)
            if not _commit_exists_in_history(sha, repo_root):
                errors.append(
                    f"{rel_path}:{idx}: links repository commit {sha} which is "
                    "unavailable in public history; name it as plain "
                    "pre-republication text instead of a link"
                )
    return errors


def lint_titles_readme(repo_root: pathlib.Path = ROOT) -> list[str]:
    """Every tracked public title manifest must be named in its README."""
    errors: list[str] = []
    readme = repo_root / TITLES_README
    if not readme.is_file():
        return errors
    try:
        res = subprocess.run(
            ["git", "ls-files", f"{TITLES_DIR}/*.json"],
            cwd=repo_root,
            env=isolated_git_env(root=repo_root),
            capture_output=True,
            text=True,
            check=True,
        )
        tracked = sorted(pathlib.PurePosixPath(line).name for line in res.stdout.splitlines() if line.strip())
    except (OSError, subprocess.SubprocessError):
        return errors
    if not tracked:
        return errors
    text = readme.read_text(encoding="utf-8")
    for name in tracked:
        if name not in text:
            errors.append(
                f"{TITLES_README}: tracked public manifest {name} is not named "
                "in the titles README fixture inventory"
            )
    return errors


def lint_toolchain_baseline_marker(repo_root: pathlib.Path = ROOT) -> list[str]:
    """The dated toolchain baseline must carry a non-CURRENT status marker."""
    errors: list[str] = []
    doc = repo_root / TOOLCHAIN_BASELINE_DOC
    if not doc.is_file():
        return errors
    text = doc.read_text(encoding="utf-8")
    if not TOOLCHAIN_BASELINE_STATUS_PAT.search(text):
        errors.append(
            f"{TOOLCHAIN_BASELINE_DOC}: dated baseline has no STATUS = "
            "HISTORICAL/REFERENCE marker and reads as current toolchain truth"
        )
    return errors


def _marked_capability_section(
    doc_text: str, doc_label: str
) -> tuple[list[str], int, list[str]]:
    """Return the disposition section's lines, its 1-based first line, and errors."""
    errors: list[str] = []
    lines = doc_text.splitlines()
    begins = [i for i, line in enumerate(lines) if line.strip() == CAPABILITY_TABLE_BEGIN]
    ends = [i for i, line in enumerate(lines) if line.strip() == CAPABILITY_TABLE_END]
    if len(begins) != 1 or len(ends) != 1:
        errors.append(
            f"{doc_label}: the capability disposition table must be delimited by exactly one "
            f"'{CAPABILITY_TABLE_BEGIN}' / '{CAPABILITY_TABLE_END}' pair"
        )
        return [], 0, errors
    if ends[0] <= begins[0]:
        errors.append(
            f"{doc_label}: the capability disposition table end marker appears before its begin marker"
        )
        return [], 0, errors
    return lines[begins[0] + 1:ends[0]], begins[0] + 2, errors


def _capability_row_cells(line: str) -> list[str] | None:
    """Split a Markdown table row into cells, or return None if it is not one.

    A delimiter row (`| :--- |`) carries no capability, so it is reported as None
    rather than as a row whose status happens to be empty.
    """
    stripped = line.strip()
    if not stripped.startswith("|"):
        return None
    cells = [cell.strip() for cell in re.split(r"(?<!\\)\|", stripped)[1:-1]]
    if not cells or all(re.fullmatch(r"[-:\s]+", cell) for cell in cells):
        return None
    return cells


def _existing_capability_paths(evidence: str, repo_root: pathlib.Path) -> list[str]:
    """Return the backticked paths in an evidence cell that exist in this repository."""
    root_resolved = repo_root.resolve()
    found: list[str] = []
    for raw in CAPABILITY_PATH_TOKEN_PAT.findall(evidence):
        resolved = (repo_root / raw).resolve()
        try:
            resolved.relative_to(root_resolved)
        except ValueError:
            continue
        if resolved.exists():
            found.append(raw)
    return found


def lint_capability_disposition_table(repo_root: pathlib.Path = ROOT) -> list[str]:
    """Every capability-disposition row must declare a valid status and real evidence."""
    errors: list[str] = []
    doc = repo_root / CAPABILITY_MATRIX_DOC
    if not doc.is_file():
        return [
            f"{CAPABILITY_MATRIX_DOC}: is missing; the per-capability disposition of the "
            "retired web UI and diagnostic capabilities cannot be verified"
        ]

    section, first_line, section_errors = _marked_capability_section(
        doc.read_text(encoding="utf-8"), CAPABILITY_MATRIX_DOC
    )
    errors.extend(section_errors)
    if section_errors:
        return errors

    in_fence = False
    rows = 0
    header_seen = False
    for offset, line in enumerate(section):
        lineno = first_line + offset
        if line.strip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        cells = _capability_row_cells(line)
        if cells is None:
            continue
        if not header_seen:
            # Requiring the header, rather than skipping the first row whatever it is,
            # keeps the table self-describing: a rename cannot silently retire the
            # contract, and no data row can hide itself in the header position.
            header_seen = True
            if tuple(cell.upper() for cell in cells) != CAPABILITY_HEADER_CELLS:
                errors.append(
                    f"{CAPABILITY_MATRIX_DOC}:{lineno}: the capability disposition table must "
                    f"begin with the column header row {' | '.join(CAPABILITY_HEADER_CELLS)}"
                )
            continue
        rows += 1
        identifier = cells[0].strip().strip("`") or "<unnamed>"
        if len(cells) != CAPABILITY_COLUMN_COUNT:
            errors.append(
                f"{CAPABILITY_MATRIX_DOC}:{lineno}: disposition row {identifier} has "
                f"{len(cells)} columns, not {CAPABILITY_COLUMN_COUNT}"
            )
            continue
        status = cells[2].strip().strip("*_").strip()
        if status not in CAPABILITY_STATUSES:
            errors.append(
                f"{CAPABILITY_MATRIX_DOC}:{lineno}: disposition row {identifier} has status "
                f"'{cells[2]}'; use exactly one of {', '.join(CAPABILITY_STATUSES)}"
            )
            continue
        evidence = cells[4]
        if status in ("PASS", "PARTIAL"):
            if not CAPABILITY_PATH_TOKEN_PAT.search(evidence):
                errors.append(
                    f"{CAPABILITY_MATRIX_DOC}:{lineno}: {status} row {identifier} names no "
                    "backticked repository test path"
                )
            elif not _existing_capability_paths(evidence, repo_root):
                errors.append(
                    f"{CAPABILITY_MATRIX_DOC}:{lineno}: {status} row {identifier} names no "
                    "repository path that exists"
                )
        elif not CAPABILITY_ISSUE_REF_PAT.search(evidence):
            errors.append(
                f"{CAPABILITY_MATRIX_DOC}:{lineno}: {status} row {identifier} names no tracking "
                "issue; an unimplemented or dropped capability must carry its issue number"
            )

    if rows == 0:
        errors.append(
            f"{CAPABILITY_MATRIX_DOC}: the capability disposition table declares no rows; "
            "an empty table would silently agree that nothing is missing"
        )
    return errors


def lint_docs_index_completeness(repo_root: pathlib.Path = ROOT) -> list[str]:
    """Every tracked markdown document under docs/ must be indexed in docs/README.md."""
    errors: list[str] = []
    readme = repo_root / DOCS_README
    if not readme.is_file():
        return errors

    try:
        res = subprocess.run(
            ["git", "ls-files", "docs/*.md", "docs/**/*.md"],
            cwd=repo_root,
            env=isolated_git_env(root=repo_root),
            capture_output=True,
            text=True,
            check=True,
        )
        tracked_docs = [
            line.strip().replace("\\", "/")
            for line in res.stdout.splitlines()
            if line.strip() and (repo_root / line.strip().replace("\\", "/")).is_file()
        ]
    except (OSError, subprocess.SubprocessError):
        tracked_docs = [
            p.relative_to(repo_root).as_posix()
            for p in (repo_root / "docs").rglob("*.md")
        ]

    target_docs = {
        p[len("docs/"):] for p in tracked_docs
        if p != DOCS_README and not p.startswith("docs/ui-baseline/")
    }

    text = readme.read_text(encoding="utf-8")
    table_entries = set()
    for line in text.splitlines():
        if line.startswith("| `"):
            entry = line.split("|")[1].strip().strip("`")
            table_entries.add(entry)

    missing = sorted(target_docs - table_entries)
    for doc in missing:
        errors.append(
            f"{DOCS_README}: tracked document docs/{doc} is not indexed in the document status taxonomy table"
        )
    return errors


def run_all_doc_lints(repo_root: pathlib.Path = ROOT) -> list[str]:
    errors = lint_readme(repo_root / "README.md")
    md_files = get_tracked_markdown_files(repo_root)
    hst_tracked = _is_hst_manifest_tracked(repo_root)
    index_statuses = docs_index_statuses(repo_root)
    for md_file in md_files:
        errors.extend(lint_doc_links_and_topology(md_file, repo_root))
        errors.extend(lint_doc_truth(md_file, repo_root, hst_tracked))
        errors.extend(lint_hle_census_counts(md_file, repo_root, index_statuses))
    errors.extend(lint_titles_readme(repo_root))
    errors.extend(lint_toolchain_baseline_marker(repo_root))
    errors.extend(lint_capability_disposition_table(repo_root))
    errors.extend(lint_docs_index_completeness(repo_root))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Documentation freshness and staleness linter")
    parser.add_argument("--repo-root", type=pathlib.Path, default=ROOT, help="Repository root path")
    args = parser.parse_args()

    errors = run_all_doc_lints(args.repo_root)
    if errors:
        print("Documentation Freshness Linter: FAIL", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    print("Documentation Freshness Linter: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
