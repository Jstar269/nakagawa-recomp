# SPDX-License-Identifier: GPL-2.0-or-later
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


def _is_dashboard(path: str) -> bool:
    return path == "interface" or path.startswith("interface/")


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
        "package.json",
        "package-lock.json",
        "interface/package.json",
        "interface/package-lock.json",
    } or path.startswith("requirements/")


def _is_security_publication(path: str) -> bool:
    name = PurePosixPath(path).name
    return (
        path.startswith(".github/ISSUE_TEMPLATE/")
        or name in {"SECURITY.md", "SECURITY.txt", "NOTICE", "NOTICE.md", "LICENSE", "LICENSE.md"}
        or path.startswith("docs/PUBLICATION")
        or path.startswith("docs/LEGAL")
        or path == "docs/provenance/MODIFIED_FILE_NOTICES.json"
        or path in {
            "tools/publish_audit.py",
            "tools/generate_sbom.py",
            "tools/verify_key_scrub.py",
            "tools/modified_file_notice_audit.py",
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
        or logical_path.startswith("tools/pspdev_")
        or name.endswith(".mk")
    )


def _is_native_runtime(path: str) -> bool:
    suffix = PurePosixPath(path).suffix.lower()
    return (
        path.startswith("src/")
        or path.startswith("include/")
        or path.startswith("assets/vfpu/")
        or path.startswith("assets/shaders/")
        or (suffix in {".c", ".cc", ".cpp", ".h", ".hpp"} and not path.startswith("interface/"))
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
    # in gates the change cannot affect (notably the dashboard, whose lint is
    # independently blocked by #248). The Python gate is the one that actually
    # validates these files -- test_hle_manifest asserts the baseline is current
    # and reproducible -- so classifying them here neither skips nor weakens a
    # check that was doing real work.
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
        }
    )


def _is_recognised(path: str) -> bool:
    return any(
        predicate(path)
        for predicate in (
            _is_docs,
            _is_dashboard,
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


def _changed_files_from_git(event_name: str, event: Mapping[str, object]) -> list[str]:
    if event_name == "workflow_dispatch":
        return []

    sha = os.environ.get("GITHUB_SHA", "HEAD")
    if event_name == "pull_request":
        pull_request = event.get("pull_request")
        base_sha = pull_request.get("base", {}).get("sha") if isinstance(pull_request, dict) else None
        left = str(base_sha or "HEAD^")
    else:
        left = os.environ.get("GITHUB_EVENT_BEFORE", "") or str(event.get("before") or "")
        if not left or set(left) == {"0"}:
            result = subprocess.run(
                ["git", "diff-tree", "--root", "--no-commit-id", "--name-status", "-r", sha],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            )
            return _parse_changed_paths(result.stdout)

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


def classify(paths: Iterable[str], *, event_name: str = "pull_request", draft: bool = False) -> dict[str, str]:
    files = sorted({_normalise(path) for path in paths if path.strip()})
    unknown_paths = any(not _is_recognised(path) for path in files)
    force_full = event_name == "workflow_dispatch" or not files or "<history-unavailable>" in files or unknown_paths
    docs_only = bool(files) and all(_is_docs(path) for path in files)
    workflow_ci = force_full or any(_is_workflow_ci(path) for path in files)
    dashboard = force_full or any(_is_dashboard(path) for path in files)
    dependency_metadata = force_full or any(_is_dependency_metadata(path) for path in files)
    security_publication = force_full or any(_is_security_publication(path) for path in files)
    public_surface = force_full or any(_is_public_surface(path) for path in files)
    manager_powershell = force_full or any(_is_manager(path) for path in files)
    title_manifest = force_full or any(_is_title_manifest(path) for path in files)
    build_system = force_full or any(_is_build_system(path) for path in files)
    native_runtime = force_full or any(_is_native_runtime(path) for path in files)
    python_tools = force_full or any(_is_python_tool(path) for path in files)
    native_tool = force_full or any(_is_native_tool(path) for path in files)
    markdown = force_full or any(_is_markdown(path) for path in files)

    run_native = native_runtime or build_system or manager_powershell or title_manifest or native_tool or workflow_ci
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
    run_python = python_tools or run_native or workflow_ci or security_publication
    run_windows = run_native
    run_dashboard = dashboard or workflow_ci
    # A normal main push is already covered by its PR. Workflow changes are
    # exceptional: validate the new workflow itself on the default branch too.
    is_main_push = event_name == "push" and os.environ.get("GITHUB_REF") == "refs/heads/main"
    allow_substantive = event_name == "workflow_dispatch" or (not draft and (not is_main_push or workflow_ci))
    run_main_smoke = is_main_push or event_name == "workflow_dispatch"

    return {
        "docs_only": str(docs_only).lower(),
        "python_tools": str(python_tools).lower(),
        "native_runtime": str(native_runtime).lower(),
        "build_system": str(build_system).lower(),
        "manager_powershell": str(manager_powershell).lower(),
        "title_manifest": str(title_manifest).lower(),
        "dashboard": str(dashboard).lower(),
        "workflow_ci": str(workflow_ci).lower(),
        "dependency_metadata": str(dependency_metadata).lower(),
        "security_publication": str(security_publication).lower(),
        "public_surface": str(public_surface).lower(),
        "markdown": str(markdown).lower(),
        "run_python": str(run_python).lower(),
        "run_native": str(run_native).lower(),
        "run_windows": str(run_windows).lower(),
        "run_dashboard": str(run_dashboard).lower(),
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
    outputs = classify(paths, event_name=args.event_name, draft=_event_draft(args.event_name, event))
    _write_outputs(outputs, args.github_output)
    print(f"event={args.event_name}")
    print(f"changed_files={len(paths)}")
    if paths:
        print("changed_paths=" + ",".join(sorted(paths)))
    for key, value in outputs.items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
