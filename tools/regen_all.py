#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regenerate every committed generated file, or check that none is stale.

Several committed files are produced by a generator and pinned by a test or a
gate that fails when the committed copy drifts from a fresh generation. Each of
those checks used to name its own generator, so a pull request that touched a
shared input failed one check per CI round trip. This tool runs every public
generator in a fixed order and reports which files changed:

    python tools/regen_all.py            # rewrite the stale files
    python tools/regen_all.py --check    # exit 1 if any file is stale; write nothing
    python tools/regen_all.py --list     # print the steps, their outputs and skips

Order matters only at the end: the provenance controls hash the staged tree, so
the refresh runs last, after the regenerated files are staged. The refresh is
``tools/contrib_refresh.py``; it reads the staged index, so stage your own
changes first (``git add``). It refuses a new or removed public path and a
changed publication profile, because those need maintainer authority
(``docs/PUBLICATION_READINESS.md``); this tool then reports the refusal as
``SKIP`` with the route to follow and does not fail on it.

A generator whose inputs are private (retail images, the trusted detailed
ledger, a telemetry database) is listed as skipped and never run. Exit codes:
0 nothing stale (or everything rewritten), 1 stale under ``--check`` or a step
failed, 2 a usage error.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Callable

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"

#: The one command every staleness check prints as its fix.
FIX_COMMAND = "python tools/regen_all.py"

HLE_INVENTORY = "docs/HLE_AND_WORKAROUND_INVENTORY.md"
HLE_CENSUS_BEGIN = "<!-- BEGIN GENERATED HLE STATUS CENSUS -->"
HLE_CENSUS_END = "<!-- END GENERATED HLE STATUS CENSUS -->"

#: contrib_refresh refusals that mean "a maintainer route applies", not "stale".
PROVENANCE_AUTHORITY_CODES = frozenset(
    {"NEW_PUBLIC_PATH", "REMOVED_PUBLIC_PATH", "CHANGED_PATH_NOT_PUBLIC", "UNRESOLVED_ENTRY", "POLICY_CHANGED"}
)


class StepError(RuntimeError):
    """A generator failed; the message is what it printed."""


@dataclasses.dataclass(frozen=True)
class Step:
    name: str
    command: str
    outputs: tuple[str, ...]
    #: Returns {repo-relative path: expected bytes}, generated without touching the tree.
    render: Callable[[Path], dict[str, bytes]] | None = None
    #: Set for a generator that is listed but never run here.
    skip: str = ""


def _run(argv: list[str]) -> None:
    result = subprocess.run(
        [sys.executable, *argv], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise StepError(f"{' '.join(argv)} exited {result.returncode}: {detail}")


def _text_bytes(path: Path) -> bytes:
    # Generators open their outputs in text mode; normalize to the LF bytes the tree stores.
    return path.read_text(encoding="utf-8").replace("\r\n", "\n").encode("utf-8")


def _single_out(tool: str, output: str, *extra: str) -> Callable[[Path], dict[str, bytes]]:
    def render(tmp: Path) -> dict[str, bytes]:
        out = tmp / Path(output).name
        _run([f"tools/{tool}", "--out", str(out), *extra])
        return {output: _text_bytes(out)}

    return render


def _refuse_baseline_downgrades(fresh: Path) -> None:
    """Never let a refresh absorb a coverage downgrade the import-audit gate exists to catch."""
    sys.path.insert(0, str(TOOLS))
    from import_audit_gate import DOWNGRADE_FROM, DOWNGRADE_TO

    committed_path = ROOT / "tools" / "import_audit_baseline.json"
    if not committed_path.is_file():
        return
    committed = json.loads(committed_path.read_text(encoding="ascii"))
    current = json.loads(fresh.read_text(encoding="ascii"))
    downgrades = sorted(
        f"{nid} {old['name']}: {old['classification']} -> {current[nid]['classification'] if nid in current else 'missing'}"
        for nid, old in committed.items()
        if old["classification"] in DOWNGRADE_FROM
        and (current[nid]["classification"] if nid in current else "missing") in DOWNGRADE_TO
    )
    if downgrades:
        raise StepError(
            "refusing to refresh tools/import_audit_baseline.json over a coverage downgrade "
            f"({'; '.join(downgrades)}); fix the handler, or review the downgrade and run "
            "python tools/hle_manifest.py --write-baseline yourself"
        )


def _render_hle(tmp: Path) -> dict[str, bytes]:
    baseline = tmp / "import_audit_baseline.json"
    census = tmp / "hle_census.md"
    _run([
        "tools/hle_manifest.py",
        "--out", str(tmp / "hle_manifest.json"),
        "--write-baseline", str(baseline),
        "--census-markdown", str(census),
        "--census-heading-level", "3",
    ])
    _refuse_baseline_downgrades(baseline)
    doc = (ROOT / HLE_INVENTORY).read_text(encoding="utf-8")
    if doc.count(HLE_CENSUS_BEGIN) != 1 or doc.count(HLE_CENSUS_END) != 1:
        raise StepError(f"{HLE_INVENTORY} needs exactly one generated census block")
    head, rest = doc.split(HLE_CENSUS_BEGIN, 1)
    _, tail = rest.split(HLE_CENSUS_END, 1)
    block = census.read_text(encoding="utf-8").strip("\r\n")
    spliced = f"{head}{HLE_CENSUS_BEGIN}\n{block}\n{HLE_CENSUS_END}{tail}"
    return {
        "tools/import_audit_baseline.json": _text_bytes(baseline),
        HLE_INVENTORY: spliced.encode("utf-8"),
    }


def _render_title_catalog(tmp: Path) -> dict[str, bytes]:
    _run(["tools/title_catalog_codegen.py", "--output-dir", str(tmp)])
    return {
        f"src/core/generated/{name}": _text_bytes(tmp / name)
        for name in ("nk_title_catalog.h", "nk_title_catalog.c")
    }


def _render_pgf_vectors(tmp: Path) -> dict[str, bytes]:
    # The header's generator lives in its test module (`--regenerate`), which has no --out.
    sys.path.insert(0, str(TOOLS))
    import test_pgf_validate_parity

    return {"tests/native/pgf_golden_vectors.h": test_pgf_validate_parity.golden_header_text().encode("utf-8")}


STEPS: tuple[Step, ...] = (
    Step(
        "recomp helper inventory",
        "python tools/recomp_helper_inventory.py",
        ("docs/RECOMP_HELPER_INVENTORY.md",),
        _single_out("recomp_helper_inventory.py", "docs/RECOMP_HELPER_INVENTORY.md"),
    ),
    Step(
        "NID name table",
        "python tools/gen_nidnames.py",
        ("src/rt/nid_names.h",),
        _single_out("gen_nidnames.py", "src/rt/nid_names.h"),
    ),
    Step(
        "HLE baseline and inventory census block",
        "python tools/hle_manifest.py --write-baseline --census-markdown <tmp> --census-heading-level 3",
        ("tools/import_audit_baseline.json", HLE_INVENTORY),
        _render_hle,
    ),
    Step(
        "waits census",
        "python tools/waits_census.py",
        ("docs/PSP_INTR_WAITS_CENSUS.md",),
        _single_out("waits_census.py", "docs/PSP_INTR_WAITS_CENSUS.md"),
    ),
    Step(
        "public title catalog",
        "python tools/title_catalog_codegen.py",
        ("src/core/generated/nk_title_catalog.h", "src/core/generated/nk_title_catalog.c"),
        _render_title_catalog,
    ),
    Step(
        "nk_ui embedded assets",
        "python tools/gen_ui_assets.py",
        ("src/ui/nk_ui_assets.c",),
        _single_out("gen_ui_assets.py", "src/ui/nk_ui_assets.c"),
    ),
    Step(
        "PGF golden vectors",
        "python tools/test_pgf_validate_parity.py --regenerate",
        ("tests/native/pgf_golden_vectors.h",),
        _render_pgf_vectors,
    ),
    Step(
        "embedded shaders",
        "mingw32-make shaders GLSLC=<glslc>",
        ("src/rt/gpu_sdl3vk/psp_vert.inc", "src/rt/gpu_sdl3vk/psp_frag.inc", "src/rt/gpu_sdl3vk/shader_manifest.json"),
        skip="needs the glslc toolchain, whose version the manifest records; `make shader-verify` checks it",
    ),
    Step(
        "profile-zero prebuilt digests",
        "see fixtures/profile_zero/README.md",
        ("fixtures/profile_zero/prebuilt/SHA256SUMS",),
        skip="needs the pinned PSPDEV toolchain; CI checks it with sha256sum --check",
    ),
    Step(
        "import-stub census",
        "python tools/import_stub_census.py --corpus <staged titles> --markdown <out>",
        (),
        skip="needs private inputs: decrypted title images; its output is never committed",
    ),
    Step(
        "benchmark report",
        "python tools/generate_benchmarks.py --db dev.db --output <out>",
        (),
        skip="needs private inputs: the local telemetry database; its output is never committed",
    ),
)

PROVENANCE = Step(
    "provenance controls",
    "python tools/contrib_refresh.py",
    ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"),
)


def _current(path: str) -> bytes | None:
    target = ROOT / path
    return target.read_bytes() if target.is_file() else None


def _git(*args: str) -> subprocess.CompletedProcess:
    sys.path.insert(0, str(TOOLS))
    from nk_core.git_isolation import isolated_git_env

    return subprocess.run(
        ["git", *args], cwd=ROOT, env=isolated_git_env(root=ROOT), capture_output=True, text=True
    )


def _without_admission(ledger: bytes | None) -> dict | None:
    if ledger is None:
        return None
    try:
        document = json.loads(ledger)
    except ValueError:
        return None
    document.pop("admission", None)
    return document


def _provenance(check: bool, base: str | None) -> tuple[str, list[str], str]:
    """Return (status, stale or changed paths, detail) for the provenance controls."""
    sys.path.insert(0, str(TOOLS))
    import contrib_refresh
    import provenance_attest_verify as verifier

    try:
        ledger_bytes, export_bytes, _ = contrib_refresh._generate(ROOT, base)
    except contrib_refresh.RefreshRefused as error:
        where = f" ({error.path})" if error.path else ""
        if error.code in PROVENANCE_AUTHORITY_CODES:
            return (
                "SKIP",
                [],
                f"{error.code}{where}: a new, removed or reclassified public path needs maintainer "
                "authority; follow docs/PUBLICATION_READINESS.md "
                "(step by step: a pull request that adds a public path)",
            )
        return "FAIL", [], f"{error.code}{where}: {error.detail}"
    except verifier.VerifyError as error:
        return "FAIL", [], f"{error.code}: {error}"

    stale = [
        path
        for path, expected in ((contrib_refresh.LEDGER, ledger_bytes), (contrib_refresh.EXPORT, export_bytes))
        if _current(path) != expected
    ]
    note = ""
    if stale == [contrib_refresh.LEDGER] and _without_admission(_current(contrib_refresh.LEDGER)) == _without_admission(
        ledger_bytes
    ):
        # Right after an admission merge the base ledger carries an `admission` audit block. The
        # trusted refresh drops it and contrib_refresh keeps it; the per-path entries agree.
        stale, note = [], "differs only by the base ledger's admission block, which the trusted refresh drops"
    if check or not stale:
        return ("STALE" if stale else "OK"), stale, note
    args = [str(TOOLS / "contrib_refresh.py")]
    if base is not None:
        args += ["--base", base]
    result = subprocess.run([sys.executable, *args], cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        return "FAIL", [], (result.stderr or result.stdout).strip()
    return "WROTE", stale, ""


def run(check: bool, base: str | None) -> int:
    failed = False
    stale_paths: list[str] = []
    written: list[str] = []
    with tempfile.TemporaryDirectory(prefix="regen-all-") as scratch:
        for step in STEPS:
            if step.skip:
                print(f"SKIP  {step.name}: {step.skip}")
                continue
            tmp = Path(scratch) / step.name.replace(" ", "_")
            tmp.mkdir()
            try:
                expected = step.render(tmp)
            except (StepError, OSError, UnicodeError) as error:
                print(f"FAIL  {step.name}: {error}")
                failed = True
                continue
            changed = [path for path, data in expected.items() if _current(path) != data]
            if not changed:
                print(f"OK    {step.name}")
                continue
            if check:
                print(f"STALE {step.name}: {', '.join(changed)}")
                stale_paths += changed
                continue
            for path in changed:
                (ROOT / path).write_bytes(expected[path])
            print(f"WROTE {step.name}: {', '.join(changed)}")
            written += changed

    if failed:
        print("regen_all: a generator failed; fix it before refreshing the provenance controls")
        return 1

    if written:
        # The provenance refresh hashes the index, so it must see the regenerated bytes.
        staged = _git("add", "--", *written)
        if staged.returncode:
            print(f"FAIL  staging the regenerated files: {staged.stderr.strip()}")
            return 1

    status, paths, detail = _provenance(check, base)
    label = f"{status:<5} {PROVENANCE.name}"
    print(f"{label}: {', '.join(paths) or detail}" if (paths or detail) else label)
    if status == "STALE":
        stale_paths += paths
    elif status == "WROTE":
        written += paths

    if check:
        if stale_paths:
            print(f"regen_all: {len(stale_paths)} generated file(s) are stale. Fix with: {FIX_COMMAND}")
            return 1
        if status == "FAIL":
            return 1
        print("regen_all: every generated file is current")
        return 0
    if status == "FAIL":
        return 1
    if written:
        print(f"regen_all: rewrote and staged {len(written)} file(s): {', '.join(written)}")
    else:
        print("regen_all: every generated file was already current")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="exit 1 if any generated file is stale; write nothing")
    mode.add_argument("--list", action="store_true", help="print the steps in order and exit")
    parser.add_argument(
        "--base", default=None, help="provenance base commit (default: merge-base(HEAD, origin/main))"
    )
    args = parser.parse_args(argv)
    if args.list:
        for step in (*STEPS, PROVENANCE):
            outputs = ", ".join(step.outputs) or "-"
            print(f"{step.name}: {step.command} -> {outputs}" + (f" [skipped: {step.skip}]" if step.skip else ""))
        return 0
    os.chdir(ROOT)
    return run(args.check, args.base)


if __name__ == "__main__":
    raise SystemExit(main())
