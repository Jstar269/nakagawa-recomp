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

The strict C compile builds each changed source with the include paths and
defines the Makefile compiles it with (``STRICT_C_UNITS``), plus ``-Werror``.
A source whose SDK header is not installed is ``SKIP``; one that includes a
header only a build step writes, when that step has not run, is ``NOT_RUN``.

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
from dataclasses import dataclass
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
    from .vulkan_sdk import VulkanSdkError, discover_vulkan_sdk  # noqa: E402
except ImportError:  # pragma: no cover - direct script execution
    import ci_paths  # noqa: E402
    from vulkan_sdk import VulkanSdkError, discover_vulkan_sdk  # noqa: E402

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
    # Every include directory below is one the Makefile puts on the compile line
    # of the sources that need it: CFLAGS (`src/rt`, `src/core`), PLAYER_INCLUDES
    # (`src/player`, `src/player/ui`, `src/ui`, `third_party/clay`,
    # `src/core/generated`), RT_NK_UI_FLAGS (`src/rt/overlay`), the Ed25519
    # selftest (`third_party/monocypher`) and HLE_INCLUDES (the ATRAC3+ subset).
    # A `tests/native/*.c` unit lives outside `src/`, so a bare include of a
    # player or UI header resolves only with that directory on the path.
    "-Isrc/rt", "-Isrc/core",
    "-Isrc/player", "-Isrc/player/ui", "-Isrc/ui", "-Isrc/rt/overlay",
    "-Isrc/core/generated", "-Ithird_party/clay", "-Ithird_party/monocypher",
    "-Isrc/rt/atrac3p", "-Isrc/rt/atrac3p/libavcodec", "-Isrc/rt/atrac3p/libavutil",
)


@dataclass(frozen=True)
class GeneratedHeader:
    """A header a source includes that only a build step writes.

    *directory* is where the Makefile writes it, spelled with ``{build_root}``
    and ``{build_dir}`` (the Makefile's ``BUILD_ROOT`` and ``BUILD_DIR``
    defaults, overridable from the environment the way make reads them).
    *include* is the ``-I`` spelling on the recipe that compiles the source, and
    *producer* is what a contributor runs to create the header.
    """

    name: str
    directory: str
    include: str
    producer: str


@dataclass(frozen=True)
class StrictUnit:
    """The extra flags the Makefile compiles one source with.

    *recipes* name the Makefile rules (their target text) whose compile line
    these flags come from; ``tools/test_contrib_check.py`` reads those recipes
    and fails if a define, include or warning listed here is not on them, so
    this table cannot drift from the build.
    """

    recipes: tuple[str, ...]
    defines: tuple[str, ...] = ()
    includes: tuple[str, ...] = ()
    #: ``-Wno-*`` suppressions the Makefile owns, with their reason, for this
    #: source's warning-audited build.  Only the ones this source needs.
    warnings: tuple[str, ...] = ()
    generated: tuple[GeneratedHeader, ...] = ()


#: Sources the Makefile compiles with more than the common flags.  A selftest
#: defines the hooks it is built against (`SR_HLE_THREAD_SELFTEST`, ...), and a
#: header it includes is declared only under that define, so compiling it bare
#: reports implicit declarations that the real build never sees.
STRICT_C_UNITS: dict[str, StrictUnit] = {
    "src/rt/hle_thread_selftest.c": StrictUnit(
        recipes=("hle-thread-selftest-build", "$(PSP_ORACLE_SMOKE_EXE)"),
        defines=("SR_HLE_THREAD_SELFTEST", "SR_CORO_LIFECYCLE_TEST", "SR_SCHED_LIVENESS_TEST"),
        # The Makefile's audited suppression list for this source: the
        # intr_conformance.h IcProbe rows omit the trailing `skip` field on purpose.
        warnings=("-Wno-missing-field-initializers",),
    ),
    "src/rt/sched_selftest.c": StrictUnit(
        recipes=("sched-selftest-one",),
        defines=("SR_SCHED_LIVENESS_TEST",),
    ),
    "src/rt/gpu_coherence_selftest.c": StrictUnit(
        recipes=("$(GPU_COHERENCE_BIN)",),
        defines=("SR_GPU_COHERENCE_SELFTEST",),
    ),
    "src/rt/atrac3p_bridge_selftest.c": StrictUnit(
        recipes=("atrac3p-bridge-selftest",),
        includes=("tests/native",),
    ),
    "src/rt/vfpu_oracle_host.c": StrictUnit(
        recipes=("$(PSP_VFPU_ORACLE_EXE)",),
        includes=("fixtures/vfpu_oracle",),
    ),
    "src/rt/title_config.c": StrictUnit(
        recipes=("$(BUILD_DIR)/title_config.o",),
        generated=(GeneratedHeader(
            "sr_title_config.h", "{build_dir}", "-I$(TITLE_CONFIG_DIR)",
            "make {build_dir}/sr_title_config.h",
        ),),
    ),
    "src/rt/vfpu_interp_selftest.c": StrictUnit(
        recipes=("vfpu-interp-selftest",),
        defines=("SR_FLIGHT_RECORDER_STANDALONE",),
        generated=(GeneratedHeader(
            "vfpu_overlap_diff_cases.h", "{build_dir}", "-I$(BUILD_DIR)",
            "make vfpu-interp-selftest",
        ),),
    ),
    "src/rt/vfpu_fuzz.c": StrictUnit(
        recipes=("$(BUILD_DIR)/vfpu_fuzz.o",),
        defines=("SR_SDL3VK",),
        generated=(GeneratedHeader(
            "vfpu_fuzz_cases.h", "{build_dir}", "-I$(BUILD_DIR)", "make vfpu_fuzz",
        ),),
    ),
    "fixtures/cosim/cosim_selftest.c": StrictUnit(
        recipes=("cosim-selftest-run",),
        defines=("SR_INSTRUCTION_TRACE",),
        # cosim-selftest generates the fixture into $(BUILD_ROOT)/cosim and runs
        # cosim-selftest-run with BUILD_DIR=$(COSIM_DIR) and GAME_NAME=cosim.
        generated=(
            GeneratedHeader(
                "cosim_recomp_funcs.h", "{build_root}/cosim", "-I$(BUILD_DIR)",
                "make cosim-selftest",
            ),
            GeneratedHeader(
                "cosim_cells.h", "{build_root}/cosim/fixture", "-I$(COSIM_FIXTURE)",
                "make cosim-selftest",
            ),
            GeneratedHeader(
                "cosim_fpu_corpus.h", "{build_root}/cosim/fixture", "-I$(COSIM_FIXTURE)",
                "make cosim-selftest",
            ),
        ),
    ),
}

#: A missing header from an SDK the build discovers rather than vendors.  The
#: compile cannot run without it, so the file is SKIP (never PASS, never FAIL).
_MISSING_HEADER = re.compile(r"fatal error: ([^:\s]+): No such file or directory")
_SDK_HEADER_PREFIXES = {
    "vulkan/": "the Vulkan SDK",
    "vk_video/": "the Vulkan SDK",
    "SDL3/": "the SDL3 development headers",
    "SDL3_ttf/": "the SDL3_ttf development headers",
}

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
    # NUL-separated output: paths are never quoted or split on spaces. In
    # `status --porcelain=v1 -z` each entry is one field "XY path"; a rename or
    # copy is followed by one more field holding its source path.
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
        if "D" in code:  # deleted in the index or the worktree: nothing to check
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


def build_locations() -> dict[str, str]:
    """The Makefile's ``BUILD_ROOT`` and ``BUILD_DIR``, as make resolves them.

    ``BUILD_ROOT ?= build``, ``GAME_NAME ?= mygame`` and
    ``BUILD_DIR ?= $(BUILD_ROOT)/$(GAME_NAME)``; make takes each from the
    environment when it is set there, and so does this.
    """
    build_root = os.environ.get("BUILD_ROOT") or "build"
    game_name = os.environ.get("GAME_NAME") or "mygame"
    build_dir = os.environ.get("BUILD_DIR") or f"{build_root}/{game_name}"
    return {"build_root": build_root, "build_dir": build_dir}


def vulkan_include_flags() -> tuple[list[str], str]:
    """The Makefile's ``VULKAN_INC_FLAGS``, from the same SDK discovery.

    Returns the flags and, when no SDK was found, the discovery's own reason.
    On Linux the default is no SDK at all: the headers are system headers and
    the compiler finds them without a flag, exactly as the Makefile expects.
    """
    try:
        sdk = discover_vulkan_sdk()
    except VulkanSdkError as error:
        return [], str(error)
    root = sdk.as_posix()
    if os.name == "nt":
        return [f"-I{root}/Include", f"-I{root}/include"], ""
    return [f"-I{root}/include"], ""


def strict_compile_command(compiler: str, source: str, vulkan_flags: list[str]) -> tuple[list[str] | None, str]:
    """The strict compile line for *source*, or ``None`` and why it cannot run.

    It cannot run when the source includes a header that only a build step
    writes and that step has not run here.
    """
    unit = STRICT_C_UNITS.get(source)
    extra: list[str] = []
    if unit is not None:
        extra.extend(f"-D{define}" for define in unit.defines)
        extra.extend(f"-I{include}" for include in unit.includes)
        extra.extend(unit.warnings)
        locations = build_locations()
        for header in unit.generated:
            directory = header.directory.format(**locations)
            if not (ROOT / directory / header.name).is_file():
                producer = header.producer.format(**locations)
                return None, (
                    f"needs the generated header {directory}/{header.name}, which only "
                    f"`{producer}` writes"
                )
            flag = f"-I{directory}"
            if flag not in extra:
                extra.append(flag)
    return [compiler, *STRICT_C_FLAGS, *vulkan_flags, *extra, source], ""


def _missing_sdk(output: str) -> str | None:
    """The SDK a failed compile could not find, if that is why it failed."""
    match = _MISSING_HEADER.search(output)
    if not match:
        return None
    header = match.group(1)
    for prefix, sdk in _SDK_HEADER_PREFIXES.items():
        if header.startswith(prefix):
            return f"{sdk} ({header}) is not installed"
    return None


def c_gate(paths: list[str]) -> Gate:
    sources = [
        p for p in paths
        if PurePosixPath(p).suffix.lower() == ".c" and (ROOT / p).is_file()
    ]
    if not sources:
        return Gate("strict-c-compile", NOT_RUN, "no changed C source")
    compiler = os.environ.get("CC") or find_tool("gcc") or find_tool("cc")
    if compiler is None:
        return Gate("strict-c-compile", SKIP, "no C compiler on PATH (set CC)")
    vulkan_flags, vulkan_missing = vulkan_include_flags()
    results: list[tuple[str, str, str]] = []
    for source in sources:
        command, reason = strict_compile_command(compiler, source, vulkan_flags)
        if command is None:
            results.append((NOT_RUN, source, reason))
            continue
        gate = _run(f"cc {source}", command, timeout=600)
        if gate.status == FAIL:
            sdk = _missing_sdk(gate.detail)
            if sdk is not None:
                # A missing SDK header is the environment, not the change: the
                # file was not compiled, so it is a skip and never a pass.
                why = f"{sdk}; {vulkan_missing}" if vulkan_missing and "Vulkan" in sdk else sdk
                results.append((SKIP, source, why))
                continue
        results.append((gate.status, source, gate.detail))
    passed = sum(1 for status, _source, _detail in results if status == PASS)
    for status in (FAIL, SKIP, NOT_RUN):
        named = [f"{source}: {detail}" for found, source, detail in results if found == status]
        if not named:
            continue
        others = [
            f"{found} {source}: {detail}"
            for found, source, detail in results if found not in (PASS, status)
        ]
        summary = " || ".join(named + others)
        return Gate("strict-c-compile", status, f"{summary} || {passed} other file(s) passed"
                    if passed else summary)
    return Gate("strict-c-compile", PASS, f"{len(sources)} file(s)")


def docs_gate(paths: list[str]) -> Gate:
    if not any(PurePosixPath(p).suffix.lower() in {".md", ".markdown"} for p in paths):
        return Gate("documentation-freshness", NOT_RUN, "no changed Markdown")
    return _run("documentation-freshness", [GATE_PYTHON, "tools/lint_docs.py"], timeout=300)


def purity_gate(paths: list[str]) -> Gate:
    """Hosted hygiene runs tools/purity_gate.py on every change; locally, when it can matter."""
    if not any(p.startswith("src/rt/") or p == "tools/purity_gate.py" for p in paths):
        return Gate("core-purity", NOT_RUN, "no changed src/rt/ file")
    return _run("core-purity", [GATE_PYTHON, "tools/purity_gate.py"], timeout=300)


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
        purity_gate(paths),
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
