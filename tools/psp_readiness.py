# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Report local verification and PSP-oracle readiness without touching inputs.

The command is intentionally observational.  It does not install PSPDEV,
start USB services, launch the game, mutate the lock, or inspect private
payloads.  Missing PSPDEV/PSPLINK is an explicit optional/hardware status;
software readiness is evaluated separately.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, asdict
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
# The PSPDEV build toolchain and the PSPLINK PC clients need not live in the same
# place. On Windows the supported route is PSPDEV under WSL (pspdev.github.io) with
# the clients running natively, because WSL2 has no USB passthrough without usbipd.
# Each tool is therefore satisfied by any one of its sources.
BUILD_TOOLS = ("psp-config", "psp-gcc", "psp-cmake", "psp-addr2line", "psp-gdb")
CLIENT_TOOLS = ("usbhostfs_pc", "pspsh")
EXPECTED_PSP_TOOLS = BUILD_TOOLS + CLIENT_TOOLS

# Operator-supplied locations; kept out of the tracked tree so no user-specific
# path is published. See docs/HARDWARE_ORACLE.md for the public boundary.
ENV_CLIENT_DIR = "PSP_ORACLE_PSPLINK_PC_DIR"
ENV_PPSSPP_HEADLESS = "PSP_ORACLE_PPSSPP_HEADLESS"


@dataclass
class Check:
    name: str
    status: str
    detail: str


def _run(command: list[str], *, timeout: float = 5.0) -> tuple[int | None, str]:
    try:
        completed = subprocess.run(
            command,
            cwd=ROOT,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
            shell=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, type(exc).__name__
    return completed.returncode, completed.stdout[-2000:].replace("\r", "").replace("\n", " ").strip()


def _git_dirty() -> bool:
    code, output = _run(["git", "status", "--porcelain", "--untracked-files=no"])
    return code != 0 or bool(output)


def _add(checks: list[Check], name: str, status: str, detail: str) -> None:
    checks.append(Check(name, status, detail))


def _wsl_pspdev_tools() -> set[str]:
    """Names of PSPDEV tools reachable through the default WSL distribution.

    Ubuntu's stock .bashrc returns early for non-interactive shells, so the
    PSPDEV exports there are not visible to ``bash -lc``; the environment is set
    explicitly instead. Returns an empty set when WSL or PSPDEV is unavailable.
    """

    if not (shutil.which("wsl.exe") or shutil.which("wsl")):
        return set()
    probe = (
        'export PSPDEV="${PSPDEV:-$HOME/pspdev}"; export PATH="$PATH:$PSPDEV/bin"; '
        "for t in " + " ".join(EXPECTED_PSP_TOOLS) + '; do command -v "$t" >/dev/null 2>&1 && echo "$t"; done'
    )
    code, output = _run(["wsl.exe", "-e", "bash", "-c", probe], timeout=45.0)
    if code != 0 or not output:
        return set()
    return {token for token in output.replace("\r", " ").split() if token in set(EXPECTED_PSP_TOOLS)}


def _client_dir_tools() -> set[str]:
    """PSPLINK PC clients found in an operator-configured directory."""

    configured = os.environ.get(ENV_CLIENT_DIR, "").strip()
    if not configured:
        return set()
    directory = Path(configured)
    if not directory.is_dir():
        return set()
    found: set[str] = set()
    for tool in CLIENT_TOOLS:
        if (directory / f"{tool}.exe").is_file() or (directory / tool).is_file():
            found.add(tool)
    return found


def _psplink_device() -> tuple[bool, str]:
    """Detect the PSPLINK USB endpoint (VID 054C, PID 01C9).

    A PSP sitting in USB mass-storage mode presents PID 02D2 instead and is
    deliberately NOT accepted: it cannot answer a probe. Absence of a detector on
    this platform is reported as not-connected rather than assumed present.
    """

    if not sys.platform.startswith("win"):
        return False, "no device detector on this platform; prove the link with a host0 round trip"
    # `@(...)` is required: powershell.exe is Windows PowerShell 5.1, where a
    # single pipeline object has no .Count property and the expression yields
    # $null. Without the array subexpression this reports "not connected" even
    # when the endpoint is present -- a false negative that is easy to mistake
    # for a correct answer, because the common case is genuinely disconnected.
    code, output = _run(
        ["powershell.exe", "-NoProfile", "-Command",
         "@(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | "
         "Where-Object { $_.InstanceId -match 'VID_054C&PID_01C9' }).Count"],
        timeout=45.0,
    )
    if code != 0:
        return False, "device query failed; treat as not connected"
    if output.strip().isdigit() and int(output.strip()) > 0:
        return True, "PSPLINK USB endpoint present (VID_054C&PID_01C9)"
    return False, "no PSPLINK USB endpoint; PSP not running PSPLINK (mass-storage mode does not count)"


def _ppsspp_headless() -> Path | None:
    """Locate a PPSSPP headless build usable as a pre-hardware smoke target."""

    configured = os.environ.get(ENV_PPSSPP_HEADLESS, "").strip()
    if configured:
        candidate = Path(configured)
        return candidate if candidate.is_file() else None
    for pattern in (
        "third_party/ppsspp-src/headless/*/Release/PPSSPPHeadless.exe",
        "third_party/ppsspp-src/headless/PPSSPPHeadless",
    ):
        for candidate in sorted(ROOT.glob(pattern)):
            if candidate.is_file():
                return candidate
    return None


def collect(*, run_focused: bool = False) -> dict[str, Any]:
    checks: list[Check] = []
    dirty = _git_dirty()
    _add(checks, "source_state", "FAIL" if dirty else "PASS",
         "tracked source changes are present; review/commit before hardware evidence" if dirty else "clean tracked tree")

    for tool in ("python", "git", "gcc", "mingw32-make"):
        resolved = shutil.which(tool)
        _add(checks, f"host_tool:{tool}", "PASS" if resolved else "FAIL", "available" if resolved else "missing")
    # The console script is often absent from PATH even when the module is
    # installed; ``python -m pre_commit`` is the form the repository actually uses.
    pre_commit = shutil.which("pre-commit")
    if pre_commit:
        _add(checks, "host_tool:pre-commit", "PASS", "console script available")
    else:
        code, _output = _run([sys.executable, "-m", "pre_commit", "--version"], timeout=30.0)
        _add(checks, "host_tool:pre-commit", "PASS" if code == 0 else "OPTIONAL_MISSING",
             "module available via `python -m pre_commit`" if code == 0 else "not installed on this host")
    for sanitizer in ("libasan.a", "libubsan.a"):
        code, output = _run(["gcc", "-print-file-name=" + sanitizer])
        available = code == 0 and bool(output) and output.strip().lower() != sanitizer.lower()
        _add(checks, f"sanitizer:{sanitizer}", "PASS" if available else "OPTIONAL_MISSING",
             "GCC runtime available" if available else "GCC runtime unavailable")

    private_root = ROOT / "place_game_here"
    private_shape = all(
        path.exists()
        for path in (
            private_root,
            private_root / "EBOOT.elf",
            private_root / "EXTRACTED" / "decrypted",
        )
    )
    _add(checks, "private_workspace", "PASS" if private_shape else "OPTIONAL_MISSING",
         "canonical private workspace shape is present" if private_shape else "canonical private inputs are unavailable")

    lock_ok = False
    try:
        sys.path.insert(0, str(ROOT / "tools"))
        import pspdev_lock  # type: ignore

        _data, pending = pspdev_lock.load_lock(ROOT / "assets" / "upstream" / "pspdev.lock.json")
        lock_ok = True
        _add(checks, "pspdev_lock", "PASS", "schema and pinned source/distribution lock validate")
        if pending:
            _add(checks, "pspdev_lock_pending", "OPTIONAL_MISSING", f"{len(pending)} optional local evidence item(s) pending")
    except Exception as exc:  # fail closed without exposing paths or payloads
        _add(checks, "pspdev_lock", "FAIL", type(exc).__name__)

    binary = ROOT / "build" / "hst" / "hst.exe"
    chunks = sorted((ROOT / "build" / "hst").glob("hst_recomp_*.c")) if (ROOT / "build" / "hst").exists() else []
    _add(checks, "hst_buildfull_artifact", "PASS" if binary.is_file() and chunks else "FAIL",
         f"linked executable and {len(chunks)} generated chunks present" if binary.is_file() and chunks else "BuildFull artifact is absent")

    fixture_root = ROOT / "fixtures" / "psp_oracle"
    fixture_ok = (fixture_root / "probe.c").is_file() and (fixture_root / "Makefile").is_file()
    _add(checks, "source_owned_probe", "PASS" if fixture_ok else "FAIL",
         "probe source and PSPDEV Makefile present" if fixture_ok else "probe source is incomplete")
    manifest_ok = (ROOT / "tools" / "psp_oracle" / "manifest.json").is_file()
    _add(checks, "probe_manifest", "PASS" if manifest_ok else "FAIL", "issue-linked manifest present" if manifest_ok else "manifest missing")

    try:
        from psp_oracle.protocol import parse_output

        parse_output(
            "NAKAGAWA_PSP_META schema=1 source=psp model=synthetic firmware=test "
            "binary_sha256=" + "0" * 64 + " source_commit=" + "0" * 40 + "\n"
            "NAKAGAWA_PSP_TEST schema=1 test_id=READINESS case_id=1 status=PASS result=0x1\n"
        )
        _add(checks, "result_protocol", "PASS", "strict parser accepts canonical synthetic record")
    except Exception as exc:
        _add(checks, "result_protocol", "FAIL", type(exc).__name__)

    wsl = shutil.which("wsl.exe") or shutil.which("wsl")
    _add(checks, "wsl", "PASS" if wsl else "OPTIONAL_MISSING", "available" if wsl else "WSL not installed or not on PATH")

    wsl_tools = _wsl_pspdev_tools()
    client_tools = _client_dir_tools()
    _add(checks, "pspdev_via_wsl", "PASS" if wsl_tools else "OPTIONAL_MISSING",
         f"{len(wsl_tools)} PSPDEV tool(s) reachable in WSL" if wsl_tools else "no PSPDEV toolchain found in WSL")
    _add(checks, "psplink_client_dir", "PASS" if client_tools else "OPTIONAL_MISSING",
         f"{len(client_tools)} client(s) in ${ENV_CLIENT_DIR}" if client_tools
         else f"${ENV_CLIENT_DIR} unset or has no usbhostfs_pc/pspsh")

    for tool in EXPECTED_PSP_TOOLS:
        if shutil.which(tool):
            source = "host PATH"
        elif tool in wsl_tools:
            source = "WSL"
        elif tool in client_tools:
            source = f"${ENV_CLIENT_DIR}"
        else:
            source = ""
        if source:
            status, detail = "PASS", f"available via {source}"
        elif tool in CLIENT_TOOLS:
            status, detail = "HARDWARE_NOT_CONNECTED", "not detected on PATH or in the configured client directory"
        else:
            status, detail = "OPTIONAL_MISSING", "not detected on PATH or in WSL"
        _add(checks, f"pspdev:{tool}", status, detail)

    headless = _ppsspp_headless()
    _add(checks, "ppsspp_headless", "PASS" if headless else "OPTIONAL_MISSING",
         "emulator smoke target available" if headless else "no PPSSPP headless build found")

    # Tool presence is not device presence. PSPLINK exposes its own USB endpoint
    # (Sony VID 054C, PID 01C9) that appears only while PSPLINK is running on the
    # PSP, so this is the one check that distinguishes "installed" from
    # "reachable". Anything weaker would let a toolchain-only host report ready.
    link, link_detail = _psplink_device()
    _add(checks, "pspdev:link", "PASS" if link else "HARDWARE_NOT_CONNECTED", link_detail)

    if run_focused:
        code, detail = _run([sys.executable, "-m", "unittest", "tools.test_psp_oracle", "tools.test_psp_issue_matrix"], timeout=120.0)
        _add(checks, "focused_tests", "PASS" if code == 0 else "FAIL", detail or "completed")

    software_failures = [check.name for check in checks if check.status == "FAIL" and not check.name.startswith("pspdev:")]
    hardware_missing = [check.name for check in checks if check.status in {"OPTIONAL_MISSING", "HARDWARE_NOT_CONNECTED"} and check.name.startswith("pspdev:")]
    return {
        "schema": 1,
        "software_ready": not software_failures,
        "hardware_ready": not hardware_missing and not software_failures,
        "checks": [asdict(check) for check in checks],
        "software_failures": software_failures,
        "hardware_missing": hardware_missing,
        "lock_valid": lock_ok,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument("--run-focused", action="store_true", help="run the small protocol/matrix test subset")
    args = parser.parse_args(argv)
    report = collect(run_focused=args.run_focused)
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        for check in report["checks"]:
            print(f"[{check['status']}] {check['name']}: {check['detail']}")
        print(f"software_ready={report['software_ready']} hardware_ready={report['hardware_ready']}")
    return 0 if report["software_ready"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
