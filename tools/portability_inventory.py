#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Public-safe portability inventory of the native runtime and build.

Scans tracked source areas for explicit Win32/host-specific constructs
(_WIN32, windows.h, HANDLE/HWND, Win32 fibers, Create*/Find*/Get*/Load*
families, QueryPerformance*, timeBeginPeriod, Sleep, Media Foundation,
MinGW/MSYS2/PowerShell references, drive-letter paths, Win32 CRT aliases)
and classifies every hit so a future host-services extraction knows where the
platform seams are and which ones sit inside PSP semantic core.

Classification (per the wave-1 portability lane contract):
  SEMANTIC_CORE_CONTAMINATION  Win32 reachable from PSP semantic behavior
                               (scheduler, HLE, GE, memory, analysis surfaces)
  BACKEND_EXPECTED             host-services/backend code (GPU, audio, media,
                               OSK, input, driver, capture, third-party decoders)
  BUILD_TOOL_ONLY              Make/tooling/CI/dashboard code
  TEST_ONLY                    selftest/unit-test code
  PRIVATE_MANAGER_ONLY         PowerShell title-manager orchestration

The scan is structural text evidence (SOURCE_SHAPE): a hit reports the line,
not an interpretation. A guarded `#ifdef _WIN32` block that compiles to
nothing on POSIX is still recorded; portability probes (tools/build_manifest
lane) decide what actually blocks.

CLI:
    python tools/portability_inventory.py [--out build/portability_inventory.json]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 1

# --------------------------------------------------------------------------
# Scan areas and their default classification.
# --------------------------------------------------------------------------
CORE_FILES = {
    "recomp.c", "hle.c", "sched.c", "sr_coro.c", "mpeg.c", "savedata.c",
    "debug.c", "watchpoints_file.c", "guest_printf.c", "perf.c", "ge.c",
    "vfpu_tables.c", "vfpu_interp.c", "vfpu_fuzz.c", "vfpu_oracle_host.c",
    "ge_replay.c",
}
CORE_HEADERS = {
    "recomp.h", "sr_coro.h", "dispatch_table.h", "nid_names.h", "vfs_path.h",
    "asset_index.h", "evf.h", "intr_conformance.h", "fp_convert.h",
    "ge_shared.h", "sdkver.h", "sr_h264.h",
    "fbcap_policy.h", "watchpoints_file.h", "debug.h", "perf.h",
}
BACKEND_HEADERS = {"pgd_api.h", "pgf_api.h"}
BACKEND_FILES = {
    "gui.c", "osk_win.c", "h264_mf.c", "h264_null.c", "driver.c",
    "iso_unavailable.c", "iso.c", "pgf_unavailable.c", "pgf.c",
    "pgd_unavailable.c", "pgd.c", "audio_unavailable.c", "audio.c",
    "fbcap_policy.c", "ge_capture.c", "atrac3p_bridge.c",
}
TEST_FILES_RE = re.compile(r"selftest|_selftest\.c$")


def classify_file(path: str) -> tuple[str, str]:
    """Return (area, default classification). `path` must be repo-relative."""
    p = Path(path)
    parts = p.parts
    name = p.name
    if name in BACKEND_HEADERS:
        return "BACKEND", "BACKEND_EXPECTED"
    if name in CORE_FILES or name in CORE_HEADERS:
        return "CORE", "SEMANTIC_CORE_CONTAMINATION"
    if name in BACKEND_FILES:
        return "BACKEND", "BACKEND_EXPECTED"
    if TEST_FILES_RE.search(name):
        return "TESTS", "TEST_ONLY"
    if parts[:2] == ("src", "rt") and parts[2:3] == ("atrac3p",):
        return "BACKEND", "BACKEND_EXPECTED"
    if parts[:2] == ("src", "rt") and parts[2:3] == ("gpu_sdl3vk",):
        return "BACKEND", "BACKEND_EXPECTED"
    if parts[:2] == ("src", "rt"):
        return "BACKEND", "BACKEND_EXPECTED"
    if parts[:2] == ("src", "ref"):
        return "REF", "TEST_ONLY" if name == "selftest.cpp" else "BACKEND_EXPECTED"
    if parts[0] == "tools" and name.endswith(".ps1"):
        return "MANAGER", "PRIVATE_MANAGER_ONLY" if name.startswith("hst") else "BUILD_TOOL_ONLY"
    if parts[0] == "tools":
        return "TOOLS", "BUILD_TOOL_ONLY"
    if p.name in ("hst_manager.ps1", "hst.ps1"):
        return "MANAGER", "PRIVATE_MANAGER_ONLY"
    if p.name in ("Makefile", "copy_build_assets.ps1"):
        return "TOOLS", "BUILD_TOOL_ONLY"
    if parts[:2] == ("mk",):
        return "TOOLS", "BUILD_TOOL_ONLY"
    if parts[:2] == (".github",):
        return "TOOLS", "BUILD_TOOL_ONLY"
    if parts[0] == "interface":
        return "DASH", "BUILD_TOOL_ONLY"
    return "OTHER", "BUILD_TOOL_ONLY"


# --------------------------------------------------------------------------
# Patterns. Each entry: (label, regex). The label is the inventory category.
# --------------------------------------------------------------------------
PATTERNS: list[tuple[str, re.Pattern]] = [
    ("win32_define", re.compile(r"\b_WIN32\b")),
    ("windows_header", re.compile(r"[<\"]windows\.h[>\"]", re.IGNORECASE)),
    ("handle_type", re.compile(r"(?<![\w])HANDLE(?![\w])")),
    ("hwnd", re.compile(r"\bHWND\b")),
    ("win32_fiber", re.compile(
        r"\b(ConvertThreadToFiber|CreateFiberEx|SwitchToFiber|DeleteFiber|"
        r"IsThreadAFiber|GetCurrentFiber|CreateFiber)\b")),
    ("win32_create_family", re.compile(
        r"\b(CreateFileA|CreateFileW|CreateThread|CreateMutexA|CreateMutexW|"
        r"CreateEventA|CreateEventW|CreateProcessA|CreateProcessW|"
        r"CreateDirectoryA|CreateDirectoryW|CreateWindowA|CreateWindowW|"
        r"CreateSemaphoreA|CreateSemaphoreW|CreateFileMappingA|CreateFileMappingW)\b")),
    ("win32_find_close", re.compile(
        r"\b(FindFirstFileA|FindFirstFileW|FindNextFileA|FindNextFileW|"
        r"FindClose|CloseHandle|GetLastError|GetActiveWindow|"
        r"SetUnhandledExceptionFilter|SetThreadPriority|SetPriorityClass|"
        r"GetPriorityClass|GetCurrentProcessId|GetModuleFileNameA|GetModuleFileNameW)\b")),
    ("queryperformance", re.compile(r"\bQueryPerformance(Counter|Frequency)")),
    ("timebeginperiod", re.compile(r"\btime(Begin|End)Period\b")),
    ("sleep", re.compile(r"\bSleep\s*\(")),
    ("win32_dynload", re.compile(r"\b(LoadLibraryA|LoadLibraryW|GetProcAddress|FreeLibrary)\b")),
    ("media_foundation", re.compile(r"\b(CoInitializeEx|MFStartup|MFCreate[A-Za-z]+|mfplat|MF_VERSION)\b")),
    ("declspec", re.compile(r"__declspec")),
    ("win32_crt_alias", re.compile(
        r"\b(_stricmp|_strnicmp|_snprintf|_snwprintf|_fstat|_fstat64|"
        r"_getpid|_getcwd|_mkdir|_chdir|_exit|_wfindfirst|_wfindnext|"
        r"_wrename|_wremove|_wmkdir|_wcsicmp|_wcsnicmp)\b")),
    ("drive_letter_path", re.compile(r"\b[A-Za-z]:[\\/][A-Za-z0-9_.-]")),
    ("powershell", re.compile(r"\b(powershell|PowerShell|pwsh|PowerShell\.exe)\b")),
    ("msys_mingw", re.compile(r"\b(MSYS2?|msys64|MINGW|ucrt64|mingw32-make)\b")),
    ("win32_lib", re.compile(r"\b(gdi32|ole32|winmm|mfplat|dxguid|dinput8|ws2_32)\b")),
    ("gettickcount", re.compile(r"\b(GetTickCount|GetSystemTimeAsFileTime|GetSystemTime)\b")),
    ("device_path", re.compile(r"\\\\\.\\\\")),
    ("ps_cmdlet", re.compile(
        r"\b(Write-Host|Get-Process|Start-Process|Join-Path|Invoke-Expression|"
        r"Get-CimInstance|Test-Path|New-Item|Copy-Item|Remove-Item|Measure-Command)\b")),
]


def scan_text(text: str, patterns: list[tuple[str, re.Pattern]]) -> list[dict]:
    hits = []
    for line_no, line in enumerate(text.splitlines(), start=1):
        for label, pat in patterns:
            for m in pat.finditer(line):
                # device_path pattern needs escaping in JSON display
                snippet = line.strip()
                if len(snippet) > 120:
                    snippet = snippet[:120] + "..."
                hits.append({"line": line_no, "label": label, "text": snippet})
    return hits


def scan_file(path: Path) -> dict | None:
    if path.suffix not in (".c", ".h", ".cpp", ".hpp", ".py", ".ps1", ".mk", ".yml", ".yaml"):
        if path.name not in ("Makefile",):
            return None
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    if not text:
        return None
    try:
        rel = str(path.relative_to(ROOT)).replace("\\", "/")
    except ValueError:
        rel = str(path).replace("\\", "/")
    area, default = classify_file(rel)
    hits = scan_text(text, PATTERNS)
    if not hits:
        return None
    # unique per (line, label)
    seen = set()
    uniq = []
    for h in hits:
        k = (h["line"], h["label"])
        if k in seen:
            continue
        seen.add(k)
        uniq.append(h)
    return {
        "path": rel,
        "area": area,
        "class": default,
        "hits": uniq,
    }


def scan() -> list[dict]:
    out: list[dict] = []
    roots = [
        ROOT / "src",
        ROOT / "tools",
        ROOT / "mk",
        ROOT / ".github",
    ]
    extra_files = [
        ROOT / "Makefile",
        ROOT / "hst_manager.ps1",
        ROOT / "hst.ps1",
        ROOT / "copy_build_assets.ps1",
    ]
    for root in roots:
        if not root.exists():
            continue
        for p in sorted(root.rglob("*")):
            if not p.is_file():
                continue
            if ".git" in p.parts:
                continue
            rec = scan_file(p)
            if rec:
                out.append(rec)
    for p in extra_files:
        if p.exists():
            rec = scan_file(p)
            if rec:
                out.append(rec)
    class_to_key = {
        "SEMANTIC_CORE_CONTAMINATION": "semantic_core",
        "BACKEND_EXPECTED": "backend",
        "BUILD_TOOL_ONLY": "build",
        "TEST_ONLY": "tests",
        "PRIVATE_MANAGER_ONLY": "manager",
    }
    for rec in out:
        rec["class"] = class_to_key.get(rec["class"], "other")
    out.sort(key=lambda r: r["path"])
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "portability_inventory.json")
    args = ap.parse_args(argv)
    files = scan()
    counts = {"semantic_core": 0, "backend": 0, "build": 0, "tests": 0, "manager": 0, "other": 0}
    hit_total = 0
    for rec in files:
        counts[rec["class"]] += 1
        hit_total += len(rec["hits"])
    doc = {
        "schema": SCHEMA,
        "method": "SOURCE_SHAPE text scan; classification by file area; see tools/portability_inventory.py",
        "counts": counts,
        "hit_total": hit_total,
        "files": files,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", encoding="ascii", newline="\n") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"portability_inventory: {len(files)} files, {hit_total} hits -> {args.out}")
    print("counts:", json.dumps(counts))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
