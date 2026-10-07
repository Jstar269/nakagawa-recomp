# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Classify a change for the path-gated public CI workflow.

The classifier deliberately errs toward running a gate.  It has no game-input
dependencies and can be exercised with ``--files`` in unit tests.  In GitHub
Actions it reads the event payload and the checked-out commit history, then
writes boolean outputs to ``GITHUB_OUTPUT``.

Its one repository dependency is the publication policy.  Whether a path is
published is a fact the policy already owns, so ``_is_public_surface`` asks the
policy rather than maintaining a second, drifting list; it fails closed to "in
the surface" when the policy cannot be read.  It is intentionally not used to
widen ``run_python`` here: every tracked file is published, so that would make
the Python gate unconditional and buy nothing, because the publication audit
already runs ungated in ``hygiene`` on every event.  The output is exported so a
local readiness check can route the same decision, where no ungated equivalent
runs.
"""

from __future__ import annotations

import argparse
from functools import lru_cache
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
from typing import Iterable, Mapping, Sequence


ROOT = Path(__file__).resolve().parents[1]


def _normalise(path: str) -> str:
    normalised = path.replace("\\", "/")
    while normalised.startswith("./"):
        normalised = normalised[2:]
    return normalised


def _is_markdown(path: str) -> bool:
    return PurePosixPath(path).suffix.lower() in {".md", ".mdx", ".markdown"}


def _is_docs(path: str) -> bool:
    return (
        path.startswith("docs/")
        or path.startswith(".github/ISSUE_TEMPLATE/")
        or path.startswith(".github/PULL_REQUEST_TEMPLATE")
        or path in {"README.md", "ISSUES.md", "AGENTS.md", ".github/copilot-instructions.md"}
        or _is_markdown(path)
    )


def _is_workflow_ci(path: str) -> bool:
    logical_path = _logical_tool_path(path) or path
    return (
        logical_path.startswith(".github/workflows/")
        or logical_path.startswith(".github/actions/")
        or logical_path
        in {
            "tools/ci_paths.py",
            "tools/test_ci_paths.py",
            "tools/ci_required.py",
            "tools/test_ci_required.py",
            "tools/ci_test_shards.py",
            "tools/test_ci_test_shards.py",
        }
    )


def _is_dependency_metadata(path: str) -> bool:
    return path in {
        ".github/dependabot.yml",
        ".pre-commit-config.yaml",
        "pyproject.toml",
        "requirements.txt",
        "requirements-dev.txt",
        "Pipfile",
        "Pipfile.lock",
        "poetry.lock",
    } or path.startswith("requirements/")


GENERATED_PUBLIC_METADATA = frozenset(
    {
        "PUBLIC_EXPORT.json",
        "assets/public_provenance_ledger.json",
        "assets/public_source_profile.json",
    }
)


def _is_generated_public_metadata(path: str) -> bool:
    """True for the derived public metadata the publication gate exists to protect.

    These files are regenerated from the tracked tree by nearly every publish, so
    treating them as unknown paths made ``force_full`` fire on 106 of 111 commits and
    saturated the routing the classifier computes.  They are publication artifacts,
    not an unrecognised file class: recognising them routes the publication gate while
    leaving the fail-closed rule intact for genuinely unknown paths.
    """
    return path in GENERATED_PUBLIC_METADATA


def _is_security_publication(path: str) -> bool:
    logical_path = _logical_tool_path(path) or path
    name = PurePosixPath(logical_path).name
    return (
        logical_path.startswith(".github/ISSUE_TEMPLATE/")
        or name in {"SECURITY.md", "SECURITY.txt", "NOTICE", "NOTICE.md", "LICENSE", "LICENSE.md"}
        or logical_path.startswith("docs/PUBLICATION")
        or logical_path.startswith("docs/LEGAL")
        or logical_path.startswith("docs/provenance/")
        or logical_path == "docs/INDEPENDENCE_CAMPAIGN.md"
        or _is_generated_public_metadata(logical_path)
        or logical_path in {
            "tools/publish_audit.py",
            "tools/generate_sbom.py",
            "tools/verify_key_scrub.py",
            "tools/modified_file_notice_audit.py",
            "tools/provenance_record_gap.py",
        }
    )


@lru_cache(maxsize=1)
def _public_policy() -> object | None:
    """Load the publication policy once, or ``None`` when it is unreadable.

    Imported lazily so that importing this module never depends on the policy
    parsing cleanly; a broken policy must still let the classifier run and route
    *everything*, which is what ``_is_public_surface`` does on ``None``.
    """
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import publication_policy

        return publication_policy.load_policy(ROOT / "assets" / "public_source_profile.json")
    except Exception:
        return None


def _is_public_surface(path: str) -> bool:
    """True when the policy publishes this path.

    Any change to a published path can invalidate the public provenance ledger
    and ``PUBLIC_EXPORT.json``, so it must route the publication and provenance
    integrity gates.  Asking the policy keeps this in step with the surface
    automatically; an unreadable policy fails closed.
    """
    policy = _public_policy()
    if policy is None:
        return True
    try:
        return policy.resolve(path).disposition == "included"
    except Exception:
        return True


def _is_policy_included(path: str) -> bool:
    """True when the publication policy explicitly includes this path.

    Fails closed (returns False) when the policy is unreadable or raises an error,
    so an unreadable policy forces full validation via unknown paths rather than
    silently classifying everything as recognized.
    """
    policy = _public_policy()
    if policy is None:
        return False
    try:
        return policy.resolve(path).disposition == "included"
    except Exception:
        return False


def _is_manager(path: str) -> bool:
    return PurePosixPath(path).suffix.lower() in {".ps1", ".psm1"}


def _is_title_manifest(path: str) -> bool:
    logical_path = _logical_tool_path(path) or path
    return (
        logical_path.startswith("assets/titles/")
        or logical_path == "assets/title_manifest.schema.json"
        or logical_path.startswith("tools/title_")
    )


def _is_build_system(path: str) -> bool:
    logical_path = _logical_tool_path(path) or path
    name = PurePosixPath(logical_path).name
    return (
        logical_path in {"Makefile", "GNUmakefile", "CMakeLists.txt"}
        or logical_path.startswith("mk/")
        or logical_path.startswith("cmake/")
        or logical_path.startswith("tools/build")
        or logical_path.startswith("tools/hst_")
        or logical_path in {
            "tools/nk_doctor.py",
            "tools/nk_doctor_checks.py",
            "tools/nk_doctor_core.py",
            "tools/nk_safety.ps1",
        }
        or logical_path.startswith("tools/pspdev_")
        or name.endswith(".mk")
    )


# ---- Makefile recipe-only routing (#702) ------------------------------------------
#
# A change to the top-level Makefile normally classifies as ``build_system``, which
# routes every gate.  That is right for anything that can change how a shared object,
# flag or rule is built, and wrong for the common edit that only rewrites the recipe
# of one phony tool or test target (``player-ui-regressions``, ``contrib-check``):
# such an edit changes what that target runs and nothing that any other recipe or
# compile step sees.
#
# ``makefile_recipe_only_targets`` proves that narrower claim structurally or returns
# None.  It parses both revisions into per-line kinds, diffs them line by line, and
# accepts the change only when
#
# * every removed and added line is a comment, a blank line, or a recipe line of an
#   explicit rule whose targets are all literal names declared ``.PHONY``;
# * every unchanged line keeps exactly the same kind and owning targets, so an edit
#   cannot reshape its neighbours (a dropped line continuation, a recipe that now
#   belongs to a different header); and
# * neither revision uses a construct the parser does not model
#   (``.RECIPEPREFIX``, an unterminated ``define``, a recipe line outside a rule).
#
# Everything else -- a variable or flag assignment, a rule header or prerequisite, a
# pattern or static-pattern rule, a file-producing rule's recipe, a conditional, an
# include, a ``.PHONY`` list -- returns None, and the path keeps the full
# ``build_system`` routing.  None is also the answer whenever either revision cannot
# be read.

_MAKE_CONDITIONALS = frozenset({"ifeq", "ifneq", "ifdef", "ifndef", "else", "endif"})
_MAKE_DIRECTIVES = frozenset(
    {"include", "-include", "sinclude", "export", "unexport", "override", "private", "vpath", "undefine"}
)
_MAKE_ASSIGNMENT = re.compile(
    r"^(?P<prefix>(?:(?:override|export|private)\s+)*)(?P<name>[^\s:#=]+)\s*(?P<op>:{1,3}=|\?=|\+=|!=|=)"
)
_MAKE_RULE_HEADER = re.compile(r"^(?P<targets>[^:=#]+?)\s*(?P<colons>::?)(?![=:])(?P<rest>.*)$")
_MAKE_VARIABLE_REF = re.compile(r"\$[({]([A-Za-z0-9_.-]+)[)}]")


class _MakefileUnsupported(ValueError):
    """The Makefile uses a construct this structural classifier does not model."""


def _strip_make_comment(text: str) -> str:
    """Drop an unescaped ``#`` comment from a non-recipe logical line."""

    index = 0
    while True:
        index = text.find("#", index)
        if index < 0:
            return text
        backslashes = len(text[:index]) - len(text[:index].rstrip("\\"))
        if backslashes % 2 == 0:
            return text[:index]
        index += 1


def _makefile_line_kinds(text: str) -> tuple[list[tuple[object, ...]], frozenset[str]]:
    """Return one kind tuple per physical line, and the literal ``.PHONY`` names.

    Kinds are ``("blank",)``, ``("comment",)``, ``("recipe", targets, explicit)``
    and ``("other", detail)``.  ``targets`` is the owning rule's target tuple and
    ``explicit`` is False for pattern, static-pattern and computed-name rules.
    """

    lines = text.split("\n")
    kinds: list[tuple[object, ...]] = []
    context: tuple[tuple[str, ...], bool] | None = None
    phony_words: list[str] = []
    simple_values: dict[str, list[str]] = {}
    assignment_counts: dict[str, int] = {}
    in_define = False
    depth = 0
    index = 0
    while index < len(lines):
        start = index
        logical = lines[index]
        while logical.endswith("\\") and index + 1 < len(lines):
            index += 1
            logical = logical[:-1] + " " + lines[index]
        span = index - start + 1
        index += 1
        raw = lines[start]

        if in_define:
            kinds.extend([("other", "define")] * span)
            if logical.strip().split(None, 1)[:1] == ["endef"]:
                in_define = False
            continue
        if raw.startswith("\t"):
            if context is None:
                raise _MakefileUnsupported(f"line {start + 1}: recipe line outside a rule")
            kinds.extend([("recipe", context[0], context[1])] * span)
            continue
        stripped = logical.strip()
        if not stripped:
            kinds.extend([("blank",)] * span)
            continue
        if stripped.startswith("#"):
            kinds.extend([("comment",)] * span)
            continue
        if ".RECIPEPREFIX" in stripped:
            raise _MakefileUnsupported(f"line {start + 1}: .RECIPEPREFIX is not modelled")
        code = _strip_make_comment(stripped).strip()
        first = code.split(None, 1)[0] if code else ""
        if first in _MAKE_CONDITIONALS:
            # A conditional may sit inside a recipe; it does not end the rule context.
            if first in {"ifeq", "ifneq", "ifdef", "ifndef"}:
                depth += 1
            elif first == "endif":
                depth -= 1
            kinds.extend([("other", "conditional")] * span)
            continue
        if first == "define" or (first in {"override", "export", "private"} and " define " in f" {code} "):
            in_define = True
            kinds.extend([("other", "define")] * span)
            if code.split()[-1:] == ["endef"]:
                in_define = False
            continue
        context = None
        assignment = _MAKE_ASSIGNMENT.match(code)
        header = _MAKE_RULE_HEADER.match(code)
        if assignment:
            name = assignment.group("name")
            assignment_counts[name] = assignment_counts.get(name, 0) + 1
            if depth == 0 and not assignment.group("prefix") and assignment.group("op") in {"=", ":=", "::="}:
                simple_values[name] = code[assignment.end():].split()
            kinds.extend([("other", "assignment")] * span)
            continue
        if first in _MAKE_DIRECTIVES:
            kinds.extend([("other", "directive")] * span)
            continue
        if header is None:
            kinds.extend([("other", "unparsed")] * span)
            continue
        targets = tuple(header.group("targets").split())
        rest = header.group("rest")
        recipe_part = ""
        if ";" in rest:
            rest, recipe_part = rest.split(";", 1)
        if _MAKE_ASSIGNMENT.match(rest.strip()):
            # ``target: VAR = value`` is a target-specific variable, not a rule; a
            # tab line after it is not modelled as anyone's recipe.
            kinds.extend([("other", "target-variable")] * span)
            continue
        explicit = (
            bool(targets)
            and ":" not in rest
            and not any("%" in target or "$" in target for target in targets)
        )
        if targets == (".PHONY",) and depth == 0:
            # A conditionally declared phony target is not phony in every
            # configuration, so only unconditional declarations count.
            phony_words.extend(rest.split())
        context = (targets, explicit)
        kinds.extend([("other", "rule-header", targets, recipe_part.strip())] * span)

    if in_define:
        raise _MakefileUnsupported("unterminated define")

    phony: set[str] = set()
    for word in phony_words:
        reference = _MAKE_VARIABLE_REF.fullmatch(word)
        if reference is None:
            if "$" not in word:
                phony.add(word)
            continue
        name = reference.group(1)
        # Only a name assigned exactly once, recursively or simply, to literal words
        # is expanded; anything appended, conditional or computed stays unresolved,
        # which keeps its targets out of the phony set and therefore out of the
        # narrow route.
        values = simple_values.get(name)
        if assignment_counts.get(name) == 1 and values is not None and not any("$" in v for v in values):
            phony.update(values)
    return kinds, frozenset(phony)


def makefile_recipe_only_targets(old_text: str, new_text: str) -> tuple[str, ...] | None:
    """Return the phony targets whose recipes alone changed, or None.

    An empty tuple means only comments or blank lines changed.  None means the change
    is not provably recipe-only and must keep the full ``build_system`` routing.
    """

    import difflib

    try:
        old_kinds, old_phony = _makefile_line_kinds(old_text)
        new_kinds, new_phony = _makefile_line_kinds(new_text)
    except (_MakefileUnsupported, RecursionError):
        return None
    if old_phony != new_phony:
        return None

    def narrow(kind: tuple[object, ...], phony: frozenset[str]) -> bool:
        if kind[0] in {"blank", "comment"}:
            return True
        if kind[0] != "recipe" or not kind[2]:
            return False
        return all(target in phony for target in kind[1])  # type: ignore[union-attr]

    old_lines = old_text.split("\n")
    new_lines = new_text.split("\n")
    targets: set[str] = set()
    matcher = difflib.SequenceMatcher(None, old_lines, new_lines, autojunk=False)
    for tag, old_start, old_end, new_start, new_end in matcher.get_opcodes():
        if tag == "equal":
            if old_kinds[old_start:old_end] != new_kinds[new_start:new_end]:
                return None
            continue
        for kinds, phony, start, end in (
            (old_kinds, old_phony, old_start, old_end),
            (new_kinds, new_phony, new_start, new_end),
        ):
            for kind in kinds[start:end]:
                if not narrow(kind, phony):
                    return None
                if kind[0] == "recipe":
                    targets.update(kind[1])  # type: ignore[arg-type]
    return tuple(sorted(targets))


def _is_native_runtime(path: str) -> bool:
    suffix = PurePosixPath(path).suffix.lower()
    return (
        path.startswith("src/")
        or path.startswith("include/")
        or path.startswith("assets/vfpu/")
        or path.startswith("assets/shaders/")
        or suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}
        or path in {"driver.c", "recomp.h"}
    )


def _is_native_tool(path: str) -> bool:
    """Return true for Python tools whose output/semantics feed native gates."""

    logical_path = _logical_tool_path(path)
    if logical_path is None:
        return False
    name = PurePosixPath(logical_path).name
    prefixes = (
        "analyze",
        "boot_gate",
        "codegen",
        "gen_microtest",
        "hle_",
        "host_stubs",
        "import_audit",
        "imports",
        "microtest",
        "native_",
        "prxload",
        "psp_import",
        "pspdev",
        "ref_",
        "savedata_",
        "sched_",
        "shader_embed",
        "title_",
        "verify_gates",
        "vfpu_",
    )
    return name.startswith(prefixes) or name.endswith("_c.py")


def _logical_tool_path(path: str) -> str | None:
    """Map one ``tools/test_<subject>.py`` path to its logical tool subject.

    The mapping is intentionally one-way and non-recursive: subsystem
    predicates inspect the subject as if it were the implementation file, so a
    test cannot accidentally classify itself through a second test prefix.
    """

    normalised = _normalise(path)
    pure_path = PurePosixPath(normalised)
    if not normalised.startswith("tools/") or pure_path.suffix.lower() != ".py":
        return None
    if not pure_path.name.startswith("test_"):
        return normalised
    return str(pure_path.with_name(pure_path.name[5:]))


def _is_python_tool(path: str) -> bool:
    # Data artefacts under tools/ are inputs and outputs of the Python pipeline,
    # not a separate surface: tools/import_audit_baseline.json is regenerated by
    # hle_manifest.py --write-baseline, and tools/psp_oracle/manifest.json is
    # consumed by the oracle tooling. Leaving them unclassified made every HLE
    # registration change an `unknown_paths` hit, which sets force_full and drags
    # in gates the change cannot affect. The Python gate is the one that actually
    # validates these files -- test_hle_manifest asserts the baseline is current
    # and reproducible -- so classifying them here neither skips nor weakens a
    # check that was doing real work.
    #
    # `tools/README.md` is Markdown, not Python, but it is the *subject* of a
    # Python regression: `tools/test_lint_docs.py` compares its module index
    # against the tracked `tools/` module set and fails closed on drift. Routing
    # the file whose content can break that test is what makes the check enforced
    # rather than advisory -- otherwise the hosted `python_tests` job is skipped
    # for exactly the edit that breaks it, and the check only runs in the change
    # that introduces it. It is named individually instead of through the
    # `markdown` flag so every other Markdown-only edit keeps skipping the Python
    # matrix.
    return (
        path.startswith("tools/")
        and (path.endswith(".py") or path.endswith(".json") or path.endswith(".toml"))
        or path
        in {
            "pyproject.toml",
            "requirements.txt",
            "requirements-dev.txt",
            "Pipfile",
            "Pipfile.lock",
            "poetry.lock",
            "tools/README.md",
        }
    )


def _is_recognised(path: str) -> bool:
    return any(
        predicate(path)
        for predicate in (
            _is_docs,
            _is_workflow_ci,
            _is_dependency_metadata,
            _is_security_publication,
            _is_manager,
            _is_title_manifest,
            _is_build_system,
            _is_native_runtime,
            _is_python_tool,
        )
    )


def _parse_changed_paths(output: str) -> list[str]:
    """Parse ``git diff --name-status`` while retaining rename old/new paths."""

    paths: list[str] = []
    for line in output.splitlines():
        if not line.strip():
            continue
        fields = line.split("\t")
        status = fields[0]
        if re.fullmatch(r"[RC](?:100|0?[0-9]{1,2})", status):
            if len(fields) != 3 or not fields[1] or not fields[2]:
                return ["<history-unavailable>"]
            paths.extend((_normalise(fields[1]), _normalise(fields[2])))
        elif re.fullmatch(r"[ACDMTUXB]", status):
            if len(fields) != 2 or not fields[1]:
                return ["<history-unavailable>"]
            paths.append(_normalise(fields[1]))
        else:
            # Never salvage a path from an unknown or structurally malformed
            # record. One bad record makes the complete classification fail
            # closed to the full matrix.
            return ["<history-unavailable>"]
    return paths


def _diff_endpoints(event_name: str, event: Mapping[str, object]) -> tuple[str, str] | None:
    """Return the ``(left, right)`` revisions this event's change spans, or None.

    None means there is no left revision to diff against: a manual dispatch, or a
    push whose ``before`` is absent or all zeros (a new ref).
    """

    if event_name == "workflow_dispatch":
        return None
    sha = os.environ.get("GITHUB_SHA", "HEAD")
    if event_name == "pull_request":
        pull_request = event.get("pull_request")
        base_sha = pull_request.get("base", {}).get("sha") if isinstance(pull_request, dict) else None
        return str(base_sha or "HEAD^"), sha
    left = os.environ.get("GITHUB_EVENT_BEFORE", "") or str(event.get("before") or "")
    if not left or set(left) == {"0"}:
        return None
    return left, sha


def _changed_files_from_git(event_name: str, event: Mapping[str, object]) -> list[str]:
    if event_name == "workflow_dispatch":
        return []

    endpoints = _diff_endpoints(event_name, event)
    if endpoints is None:
        sha = os.environ.get("GITHUB_SHA", "HEAD")
        result = subprocess.run(
            ["git", "diff-tree", "--root", "--no-commit-id", "--name-status", "-r", sha],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        return _parse_changed_paths(result.stdout)
    left, sha = endpoints

    try:
        result = subprocess.run(
            # Name-status keeps deletions and type changes, while retaining
            # both sides of a rename lets a native source renamed into a
            # documentation tree remain native-relevant.
            ["git", "diff", "--name-status", "--find-renames", left, sha],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        # A shallow or unusual event checkout should fail closed.  The workflow
        # uses fetch-depth: 0, but treating the current tree as fully relevant is
        # safer than silently skipping a gate when history is unavailable.
        return ["<history-unavailable>"]
    return _parse_changed_paths(result.stdout)


#: Python shards for a change whose only build-system edit is a recipe-only Makefile
#: edit. The Makefile-coupled subset (tools/ci_test_shards.py --select makefile) is
#: about nine tenths of the suite by measured weight, because most heavy modules run
#: make, so it keeps two runners rather than one; at that width the Python job is
#: about as long as the native and Windows gates the same change still requires.
PYTHON_SHARDS_FULL = 4
PYTHON_SHARDS_MAKEFILE = 2


def _makefile_revisions(left: str, right: str) -> tuple[str, str] | None:
    texts: list[str] = []
    for revision in (left, right):
        try:
            result = subprocess.run(
                ["git", "show", f"{revision}:Makefile"],
                cwd=ROOT,
                check=True,
                capture_output=True,
            )
            texts.append(result.stdout.decode("utf-8"))
        except (OSError, subprocess.CalledProcessError, UnicodeDecodeError):
            return None
    return texts[0], texts[1]


def makefile_change_from_git(left: str, right: str) -> tuple[str, ...] | None:
    """Return ``makefile_recipe_only_targets`` for the Makefile between two revisions.

    None when either revision lacks a readable Makefile, so an added, deleted or
    unreadable Makefile keeps the full build-system routing.
    """

    revisions = _makefile_revisions(left, right)
    if revisions is None:
        return None
    return makefile_recipe_only_targets(*revisions)


def classify(
    paths: Iterable[str],
    *,
    event_name: str = "pull_request",
    draft: bool = False,
    makefile_change: tuple[str, ...] | None = None,
) -> dict[str, str]:
    """Classify ``paths`` into gate decisions.

    ``makefile_change`` is the result of ``makefile_recipe_only_targets`` for this
    change's Makefile edit, or None when there is none or it is not provably
    recipe-only.  Only a non-None value narrows anything, so a caller that cannot
    compute it keeps the full ``build_system`` routing.
    """

    files = sorted({_normalise(path) for path in paths if path.strip()})
    unknown_paths = any(not _is_recognised(path) for path in files)
    force_full = event_name == "workflow_dispatch" or not files or "<history-unavailable>" in files or unknown_paths
    docs_only = bool(files) and all(
        _is_docs(path) or _is_generated_public_metadata(path) for path in files
    )
    workflow_ci = force_full or any(_is_workflow_ci(path) for path in files)
    dependency_metadata = force_full or any(_is_dependency_metadata(path) for path in files)
    security_publication = force_full or any(_is_security_publication(path) for path in files)
    public_surface = force_full or any(_is_public_surface(path) for path in files)
    manager_powershell = force_full or any(_is_manager(path) for path in files)
    title_manifest = force_full or any(_is_title_manifest(path) for path in files)
    makefile_recipe_only = (
        not force_full and makefile_change is not None and "Makefile" in files
    )
    build_system = force_full or any(
        _is_build_system(path) and not (makefile_recipe_only and path == "Makefile") for path in files
    )
    native_runtime = force_full or any(_is_native_runtime(path) for path in files)
    python_tools = force_full or any(_is_python_tool(path) for path in files)
    native_tool = force_full or any(_is_native_tool(path) for path in files)
    markdown = force_full or any(_is_markdown(path) for path in files)

    native_without_recipe = (
        native_runtime or build_system or manager_powershell or title_manifest or native_tool or workflow_ci
    )
    # A recipe-only Makefile edit still runs every native and Windows gate: those are
    # the jobs that execute recipes, and the edited target may be one of them.
    run_native = native_without_recipe or makefile_recipe_only
    # ``security_publication`` was computed and exported but fed no decision at
    # all, so a change to the publication contract itself -- docs/PUBLICATION*,
    # tools/publish_audit.py, the notice audit -- routed no Python gate.  It now
    # routes one.
    #
    # ``public_surface`` deliberately does NOT widen this.  Every tracked file in
    # this repository is published, so routing on it would make ``run_python``
    # unconditional and buy nothing: the publication audit that protects the
    # generated ledger and export runs in the ungated ``hygiene`` job on every
    # event (see ``PublicationCoverageInvariantTests``).  The output is exported
    # so a local readiness check can route the same decision, where no ungated
    # equivalent runs.
    python_full = python_tools or native_without_recipe or workflow_ci or security_publication
    run_python = python_full or makefile_recipe_only
    # #702: when the recipe-only Makefile edit is the only reason to run Python, run
    # just the modules that can observe it, on fewer runners. Anything else that
    # routes Python restores the complete suite.
    python_scope = "all" if python_full or not makefile_recipe_only else "makefile"
    python_num_shards = PYTHON_SHARDS_FULL if python_scope == "all" else PYTHON_SHARDS_MAKEFILE
    run_windows = run_native
    # A normal main push is already covered by its PR. Workflow changes are
    # exceptional: validate the new workflow itself on the default branch too.
    # Draft pull requests are not a separate validation mode: they get the
    # same path-applicable gates as ready pull requests, so progress does not
    # require a manual ready-for-review transition or workflow dispatch.
    is_main_push = event_name == "push" and os.environ.get("GITHUB_REF") == "refs/heads/main"
    allow_substantive = event_name == "workflow_dispatch" or (not is_main_push or workflow_ci)
    run_main_smoke = is_main_push or event_name == "workflow_dispatch"

    return {
        "docs_only": str(docs_only).lower(),
        "python_tools": str(python_tools).lower(),
        "native_runtime": str(native_runtime).lower(),
        "build_system": str(build_system).lower(),
        "manager_powershell": str(manager_powershell).lower(),
        "title_manifest": str(title_manifest).lower(),
        "workflow_ci": str(workflow_ci).lower(),
        "dependency_metadata": str(dependency_metadata).lower(),
        "security_publication": str(security_publication).lower(),
        "public_surface": str(public_surface).lower(),
        "markdown": str(markdown).lower(),
        "makefile_recipe_only": str(makefile_recipe_only).lower(),
        "run_python": str(run_python).lower(),
        "python_scope": python_scope,
        "python_num_shards": str(python_num_shards),
        "python_shards": json.dumps(list(range(python_num_shards)), separators=(",", ":")),
        "run_native": str(run_native).lower(),
        "run_windows": str(run_windows).lower(),
        "run_markdown": str(markdown).lower(),
        "run_main_smoke": str(run_main_smoke).lower(),
        "allow_substantive": str(allow_substantive).lower(),
        "draft": str(draft).lower(),
    }


def _read_event(path: str | None) -> dict[str, object]:
    if not path:
        return {}
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def _event_draft(event_name: str, event: Mapping[str, object]) -> bool:
    if event_name != "pull_request":
        return False
    pull_request = event.get("pull_request")
    return bool(pull_request.get("draft")) if isinstance(pull_request, dict) else False


def _write_outputs(outputs: Mapping[str, str], output_path: str | None) -> None:
    if not output_path:
        return
    with Path(output_path).open("a", encoding="utf-8", newline="\n") as handle:
        for key, value in outputs.items():
            handle.write(f"{key}={value}\n")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--files", nargs="*", help="changed paths (for local tests)")
    parser.add_argument("--event-name", default=os.environ.get("GITHUB_EVENT_NAME", "pull_request"))
    parser.add_argument("--event-path", default=os.environ.get("GITHUB_EVENT_PATH"))
    parser.add_argument("--github-output", default=os.environ.get("GITHUB_OUTPUT"))
    args = parser.parse_args(argv)

    event = _read_event(args.event_path)
    if args.files is not None:
        paths = args.files
    else:
        paths = _changed_files_from_git(args.event_name, event)
    makefile_change: tuple[str, ...] | None = None
    if args.files is None and "Makefile" in {_normalise(path) for path in paths}:
        endpoints = _diff_endpoints(args.event_name, event)
        if endpoints is not None:
            makefile_change = makefile_change_from_git(*endpoints)
    outputs = classify(
        paths,
        event_name=args.event_name,
        draft=_event_draft(args.event_name, event),
        makefile_change=makefile_change,
    )
    _write_outputs(outputs, args.github_output)
    print(f"event={args.event_name}")
    print(f"changed_files={len(paths)}")
    if paths:
        print("changed_paths=" + ",".join(sorted(paths)))
    if makefile_change is not None:
        print("makefile_recipe_targets=" + ",".join(makefile_change))
    for key, value in outputs.items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
