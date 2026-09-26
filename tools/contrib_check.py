#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Run only the local gates that apply to the files you changed.

``make contrib-check`` is the local fast path for a pull request.  The full
``make check`` and ``make readiness`` remain the authoritative gates; this
target exists because a contributor who must run the whole native and tooling
matrix to find out whether a one-line documentation fix is well formed gives up
instead of opening the pull request.

It does not decide what to run with a second, private copy of the rules.  Path
routing comes from ``tools/ci_paths.py``, the same classifier the hosted
workflow uses, so the local run and the hosted run cannot drift apart.  Each
gate reports one of ``PASS``, ``FAIL``, ``SKIP`` (the tool is not installed) or
``NOT_RUN`` (the change does not touch that surface).  A gate that cannot run is
never reported as a pass.

The publication-safety gate is the one place this script interprets output.
The publication audit's provenance self-consistency leg compares the working
tree against the checked-in ledger, which only a maintainer holding the private
trusted ledger can regenerate.  Those findings are therefore reported as
``MAINTAINER-SIDE`` and do not fail a local run; every other publication
finding does.  Both lists are always printed, so nothing is hidden behind the
split.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    from . import ci_paths  # noqa: E402
except ImportError:  # pragma: no cover - direct script execution
    import ci_paths  # noqa: E402

#: Findings the publication audit raises only because the generated controls
#: have not been regenerated yet.  The ledger is maintained outside the
#: repository, so a contributor cannot clear these and must not be asked to.
MAINTAINER_SIDE_CODES = (
    "PROVENANCE_CONTENT_MISMATCH",
    "PROVENANCE_UNVERIFIED",
    "POLICY_EXPORT_STALE",
)

STRICT_C_FLAGS = (
    "-fsyntax-only", "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-Isrc/rt", "-Isrc/core",
    "-Isrc/rt/atrac3p", "-Isrc/rt/atrac3p/libavcodec", "-Isrc/rt/atrac3p/libavutil",
)

#: A shared tool is exercised by more than the module named after it.  These
#: extra modules are the ones whose failures the hosted Python gate attributes
#: to a change in the shared tool, so the local run must too.
SHARED_TOOL_TESTS = {
    "tools/public_export.py": ("tools.test_public_export", "tools.test_publish_audit"),
    "tools/build_public_export.py": ("tools.test_public_export",),
    "tools/publish_audit.py": ("tools.test_publish_audit",),
    "tools/policy_sync.py": ("tools.test_publication_policy_gate", "tools.test_publish_audit"),
    "tools/provenance_ledger.py": ("tools.test_provenance_ledger",),
    "tools/provenance_attest_verify.py": (
        "tools.test_provenance_attest_verify", "tools.test_provenance_attestation_gate",
    ),
    "tools/provenance_refresh.py": ("tools.test_provenance_ledger",),
    "tools/publication_policy.py": ("tools.test_publication_policy_gate",),
    "tools/ci_paths.py": ("tools.test_ci_paths", "tools.test_ci_required"),
    "tools/contrib_check.py": ("tools.test_contrib_check",),
    "tools/provenance_record_gap.py": ("tools.test_provenance_record_gap",),
}

PASS, FAIL, SKIP, NOT_RUN, MAINTAINER_SIDE = "PASS", "FAIL", "SKIP", "NOT_RUN", "MAINTAINER-SIDE"

#: The interpreter that runs the sub-gates. It defaults to the one running
#: this script and is passed explicitly by the make target, so a repository
#: whose default ``python`` is not the interpreter holding the project's tools
#: can say so once instead of failing gate by gate.
GATE_PYTHON = os.environ.get("CONTRIB_PYTHON") or sys.executable


def find_tool(name: str) -> str | None:
    """Locate *name* on PATH, tolerating the mingw32-make PATH shape.

    ``mingw32-make`` hands a Windows interpreter the MSYS-style PATH it was
    given (``/c/Program Files/nodejs``), which ``shutil.which`` cannot use: a
    Windows ``CreateProcess`` does not understand a leading slash-drive path.
    Without this, the Windows target silently reports SKIP for every tool while
    the same command works from the contributor's shell -- a gate that reports
    its own environment wrongly is worse than no gate.
    """
    found = shutil.which(name)
    if found:
        return found
    if os.name != "nt":
        return None
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        match = re.fullmatch(r"/([A-Za-z])/(.+?)/?", entry.strip())
        if not match:
            continue
        windows = f"{match.group(1).upper()}:\\{match.group(2).replace('/', os.sep)}"
        for suffix in (".exe", ".cmd", ".bat", ""):
            candidate = Path(windows) / f"{name}{suffix}"
            if candidate.is_file():
                return str(candidate)
    return None


class Gate:
    __slots__ = ("name", "status", "detail")

    def __init__(self, name: str, status: str, detail: str = "") -> None:
        self.name = name
        self.status = status
        self.detail = detail


def _git(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args], cwd=ROOT, capture_output=True, text=True, check=check,
    )


def changed_paths(base: str) -> list[str]:
    """Committed changes against *base* plus anything still uncommitted."""
    merge_base = _git("merge-base", base, "HEAD").stdout.strip()
    if not merge_base:
        raise SystemExit(f"contrib-check: cannot resolve a merge base with {base}")
    # NUL-separated output: paths are never quoted or split on spaces.
    committed = _git("diff", "--name-only", "-z", "--find-renames", merge_base, "HEAD").stdout.split("\0")
    status = _git("status", "--porcelain=v1", "-z", "--untracked-files=all", check=False).stdout
    working: list[str] = []
    entries = status.split("\0")
    index = 0
    while index < len(entries):
        entry = entries[index]
        index += 1
        if len(entry) < 4:
            continue
        code, name = entry[:2], entry[3:]
        if code[0] in "RC":  # a rename or copy: the next entry is the source path
            index += 1
        if "D" in code and code.strip() == "D":
            continue
        working.append(name)
    return sorted({path for path in committed + working if path})


def python_test_modules(paths: list[str]) -> list[str]:
    """Map the changed tools/ modules onto the test modules that cover them."""
    modules: set[str] = set()
    for path in paths:
        name = PurePosixPath(path).name
        if not path.startswith("tools/") or not name.endswith(".py"):
            continue
        if name.startswith("test_"):
            modules.add("tools." + name[:-3])
            continue
        modules.update(SHARED_TOOL_TESTS.get(path, ()))
        if (ROOT / f"tools/test_{name}").is_file():
            modules.add(f"tools.test_{name[:-3]}")
    return sorted(modules)


def _run(label: str, command: list[str], *, timeout: int) -> Gate:
    try:
        result = subprocess.run(
            command, cwd=ROOT, capture_output=True, text=True, timeout=timeout,
        )
    except FileNotFoundError:
        return Gate(label, SKIP, f"{command[0]} is not installed")
    except subprocess.TimeoutExpired:
        return Gate(label, FAIL, f"timed out after {timeout}s: {' '.join(command)}")
    if result.returncode == 0:
        return Gate(label, PASS)
    output = (result.stdout + result.stderr).strip().splitlines()
    return Gate(label, FAIL, " | ".join(output[-6:]) or f"exit {result.returncode}")


def _python_tool(tool: str, arguments: list[str], *, timeout: int) -> Gate:
    """Run *tool* as a module of the gate interpreter, or report it missing."""
    executable = find_tool(tool)
    if executable:
        return _run(tool, [executable, *arguments], timeout=timeout)
    gate = _run(tool, [GATE_PYTHON, "-m", tool, *arguments], timeout=timeout)
    if gate.status == FAIL and "No module named" in gate.detail:
        return Gate(
            tool, SKIP,
            f"{tool} is not installed for {GATE_PYTHON}; install it or set CONTRIB_PYTHON "
            f"to the interpreter that has it",
        )
    return gate


def ruff_gate(paths: list[str]) -> Gate:
    if not paths:
        return Gate("ruff", NOT_RUN, "no changed Python")
    return _python_tool("ruff", ["check", *paths], timeout=300)


def unit_gate(modules: list[str]) -> Gate:
    if not modules:
        return Gate("python-unittest", NOT_RUN, "no changed tools/ module")
    return _run("python-unittest", [GATE_PYTHON, "-m", "unittest", *modules], timeout=1800)


def markdown_gate(paths: list[str]) -> Gate:
    markdown = [p for p in paths if PurePosixPath(p).suffix.lower() in {".md", ".markdown"}]
    if not markdown:
        return Gate("markdownlint", NOT_RUN, "no changed Markdown")
    npx = find_tool("npx")
    if npx is None:
        return Gate("markdownlint", SKIP, "npx is not installed")
    return _run(
        "markdownlint",
        [npx, "--no-install", "markdownlint-cli2", "--no-globs", *markdown],
        timeout=600,
    )


def c_gate(paths: list[str]) -> Gate:
    sources = [p for p in paths if PurePosixPath(p).suffix.lower() == ".c"]
    if not sources:
        return Gate("strict-c-compile", NOT_RUN, "no changed C source")
    compiler = os.environ.get("CC") or find_tool("gcc") or find_tool("cc")
    if compiler is None:
        return Gate("strict-c-compile", SKIP, "no C compiler on PATH (set CC)")
    failures = []
    for source in sources:
        gate = _run(f"cc {source}", [compiler, *STRICT_C_FLAGS, source], timeout=600)
        if gate.status != PASS:
            failures.append(f"{source}: {gate.detail}")
    if failures:
        return Gate("strict-c-compile", FAIL, " || ".join(failures))
    return Gate("strict-c-compile", PASS, f"{len(sources)} file(s)")


def docs_gate(paths: list[str]) -> Gate:
    if not any(PurePosixPath(p).suffix.lower() in {".md", ".markdown"} for p in paths):
        return Gate("documentation-freshness", NOT_RUN, "no changed Markdown")
    return _run("documentation-freshness", [GATE_PYTHON, "tools/lint_docs.py"], timeout=300)


def publication_gate() -> tuple[Gate, Gate]:
    """Publication safety, split into contributor and maintainer obligations."""
    try:
        result = subprocess.run(
            [GATE_PYTHON, "tools/policy_sync.py"],
            cwd=ROOT, capture_output=True, text=True, timeout=600,
        )
    except subprocess.TimeoutExpired:
        return Gate("publication-policy", FAIL, "policy_sync timed out"), Gate(
            "publication-safety", NOT_RUN, "policy classification did not complete",
        )
    if result.returncode != 0:
        return Gate("publication-policy", FAIL, (result.stdout + result.stderr).strip()[-400:]), Gate(
            "publication-safety", NOT_RUN, "policy classification failed",
        )
    policy = Gate("publication-policy", PASS)

    try:
        audit = subprocess.run(
            [GATE_PYTHON, "tools/publish_audit.py", "--tracked-only", "--worktree",
             "--public-scope", "--provenance-self-consistency"],
            cwd=ROOT, capture_output=True, text=True, timeout=1800,
        )
    except subprocess.TimeoutExpired:
        return policy, Gate("publication-safety", FAIL, "publish_audit timed out")
    findings = [
        line.strip()
        for line in (audit.stdout + audit.stderr).splitlines()
        if line.strip() and ": " in line and not line.strip().startswith("publication audit:")
    ]
    maintainer = [line for line in findings if any(f"{code}:" in line for code in MAINTAINER_SIDE_CODES)]
    contributor = [line for line in findings if line not in maintainer]
    if contributor:
        return policy, Gate("publication-safety", FAIL, " || ".join(contributor[:6]))
    if maintainer:
        codes = sorted({line.split(":", 1)[0] for line in maintainer})
        return policy, Gate(
            "publication-safety", MAINTAINER_SIDE,
            f"{len(maintainer)} finding(s) a maintainer clears by regenerating the controls: "
            + ", ".join(codes),
        )
    return policy, Gate("publication-safety", PASS)


def run(base: str) -> list[Gate]:
    paths = changed_paths(base)
    routing = ci_paths.classify(paths)
    python_paths = [
        path for path in paths
        if path.endswith(".py") and path.startswith(("tools/", "fixtures/"))
    ]
    print(f"contrib-check: base {base}, {len(paths)} changed path(s)")
    for path in paths:
        print(f"  {path}")
    if not paths:
        return [Gate("changed-paths", NOT_RUN, "nothing changed against the base")]
    print("  routing: " + " ".join(
        f"{key}={routing[key]}"
        for key in ("docs_only", "python_tools", "native_runtime", "markdown")
    ))
    modules = python_test_modules(paths)
    if modules:
        print("  test modules: " + ", ".join(modules))
    gates = [
        ruff_gate(python_paths),
        unit_gate(modules),
        c_gate(paths),
        markdown_gate(paths),
        docs_gate(paths),
    ]
    gates.extend(publication_gate())
    return gates


def set_gate_python(interpreter: str) -> None:
    """Point the sub-gates at one interpreter for the whole run."""
    global GATE_PYTHON
    GATE_PYTHON = interpreter


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--base", default=os.environ.get("CONTRIB_BASE", "origin/main"),
        help="branch or commit the change is measured against (default: origin/main)",
    )
    parser.add_argument(
        "--python", default=GATE_PYTHON,
        help="interpreter that runs the sub-gates (default: CONTRIB_PYTHON, else this one)",
    )
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    set_gate_python(args.python)

    gates = run(args.base)
    print("")
    for gate in gates:
        detail = f" -- {gate.detail}" if gate.detail else ""
        print(f"  {gate.status:<14} {gate.name}{detail}")
    failed = [gate.name for gate in gates if gate.status == FAIL]
    maintainer = [gate.name for gate in gates if gate.status == MAINTAINER_SIDE]
    skipped = [gate.name for gate in gates if gate.status == SKIP]
    if maintainer:
        print("\n  a maintainer regenerates PUBLIC_EXPORT.json and the public ledger from the "
              "private trusted ledger (make provenance-refresh); a contributor cannot do that, and these findings are "
              "reported rather than counted as a local failure.")
    if skipped:
        print(f"  SKIPPED because the tool is not installed: {', '.join(skipped)} -- "
              "a skipped gate is not a pass.")
    print(f"\ncontrib-check: {'FAIL (' + ', '.join(failed) + ')' if failed else 'OK'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
