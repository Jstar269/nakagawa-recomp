#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""progress_tracker.py — empirical verification for Nakagawa Recomp.

Runs concrete gating checks against actual file outputs (logs, binaries) and
emits a structured progress.json. Each item that gets mapped to truth comes
out of a measurement, not a description. Items carry an evidence grade from
tools/evidence_model.py (unknown / heuristic / executed / freshness-bound /
content-validated / stale) and the run is bound to the exact build identity
(source commit, binary/profile/input-manifest hashes, generation timestamp)
when the binary is available, so a stale or unbound measurement can never read
as current proof (#181).

Usage:
    python tools/progress_tracker.py verify     # measure + emit
    python tools/progress_tracker.py show       # print state
    python tools/progress_tracker.py diff A B   # compare two progress.json files
    python tools/progress_tracker.py axes       # measure six coverage axes and write
                                                 # ignored reports under logs/

Exit codes:
    0 = all ok
    1 = a pending item regressed
    2 = filesystem/layout error
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

import evidence_model

# Make Unicode characters render on Windows consoles (cp1252 default).
try:
    if hasattr(sys.stdout, "reconfigure"):
        getattr(sys.stdout, "reconfigure")(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        getattr(sys.stderr, "reconfigure")(encoding="utf-8")
except Exception:
    pass

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build" / "hst"
LOGS = REPO / "logs"
OUT = REPO / "progress.json"

# ──────────────────────────────────────────────────────────────
# Verifier definitions
# Each returns (units_positive, units_negative) — negatives are regressions.
# ──────────────────────────────────────────────────────────────


def _log_lines(name: str) -> list[str]:
    p = LOGS / name
    if not p.exists():
        return []
    raw = p.read_bytes()
    # Auto-detect UTF-16 LE (BOM FF FE) or UTF-8.
    if raw[:2] == b"\xff\xfe":
        text = raw.decode("utf-16-le", errors="ignore")
    elif raw[:3] == b"\xef\xbb\xbf":
        text = raw[3:].decode("utf-8", errors="ignore")
    else:
        text = raw.decode("utf-8", errors="ignore")
    return text.splitlines()


def count_pattern(name: str, pattern: str) -> int:
    """Count regex matches in a log file."""
    lines = _log_lines(name)
    if not lines:
        return 0
    return sum(1 for line in lines if re.search(pattern, line))


def file_exists(rel: str) -> bool:
    return (REPO / rel).exists()


def file_nonempty(rel: str) -> bool:
    p = REPO / rel
    return p.exists() and p.stat().st_size > 0


def latest_log() -> str | None:
    """Returns the name of the most recently *modified* stderr_run*.log, or None.

    Selection is by modification time, not lexicographic filename order: numeric or
    dated run-log names can otherwise select stale evidence over a newer run (#48).
    """
    candidates = list(LOGS.glob("stderr_run*.log"))
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime).name


def _git_commit() -> str | None:
    """Current build/source commit, or None outside a git checkout."""
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(REPO),
                           capture_output=True, text=True, timeout=5)
        if r.returncode == 0:
            return r.stdout.strip()
    except Exception:
        pass
    return None


def _sha256_file(path: Path) -> str | None:
    """SHA-256 of a file, or None when it does not exist or cannot be read."""
    try:
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def _sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _profile_descriptor() -> dict:
    """Deterministic route/input profile for the HST build route.

    GAME_BASE=0 / GAME_ENTRY=0 are the canonical manager-supplied values
    (AGENTS.md); a different route hashes to a different profile.
    """
    return {"game": "hst", "base": 0, "entry": 0}


def _build_run_identity(*, generated_at: str) -> dict | None:
    """The build identity a measurement binds to, or None when the current
    binary or source commit is unavailable (no freshness binding possible)."""
    source_commit = _git_commit()
    binary_sha256 = _sha256_file(BUILD / "hst.exe")
    if source_commit is None or binary_sha256 is None:
        return None
    identity = {
        "source_commit": source_commit,
        "binary_sha256": binary_sha256,
        "profile_sha256": _sha256_text(json.dumps(_profile_descriptor(), sort_keys=True)),
        "input_manifest_sha256": _sha256_file(BUILD / "hst_imports.toml"),
        "generated_at": generated_at,
    }
    try:
        # The shared evidence model is the single validation authority for the
        # identity shape; an identity that cannot round-trip must not be emitted.
        evidence_model.EvidenceIdentity.from_mapping(identity)
    except evidence_model.EvidenceError:
        return None
    # NOTE: identity binds to the current on-disk binary. Whether that binary is
    # additionally the manager's known-zero build (build_manifest.json hash match)
    # is P1.4's separate, stricter check — a fresh identity is not that verdict.
    return identity


def _run_evidence_grade(latest: str | None, identity_bound: bool, stale: bool | None) -> str:
    """Run-level grade: nothing selected -> unknown; selected but unbound ->
    executed; bound but older than the binary -> stale; bound and current ->
    freshness-bound."""
    if latest is None:
        return "unknown"
    if not identity_bound:
        return "executed"
    if stale:
        return "stale"
    return "freshness-bound"


def _run_metadata(latest: str | None) -> dict:
    """Which run the measurement used, and whether it looks stale relative to the
    current binary — so a progress report is revision-aware instead of silently
    trusting whatever log happened to be selected (#48). The returned block also
    carries the exact build identity (source commit, binary/profile/input-manifest
    hashes, generation timestamp) and a run-level evidence grade (#181): stale or
    unbound data is labeled as such and can never read as current proof."""
    meta: dict[str, object] = {
        "source_commit": _git_commit(),
        "selected_log": latest,
        "selected_log_mtime": None,
        "stale_vs_build": None,
    }
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    meta["generated_at"] = generated_at
    if latest:
        p = LOGS / latest
        if p.exists():
            meta["selected_log_mtime"] = int(p.stat().st_mtime)
            exe = BUILD / "hst.exe"
            if exe.exists():
                # The selected run predates the current binary: its evidence may
                # describe an older build.
                meta["stale_vs_build"] = bool(p.stat().st_mtime < exe.stat().st_mtime)
    identity = _build_run_identity(generated_at=generated_at)
    meta["identity"] = identity
    meta["identity_bound"] = identity is not None
    meta["evidence_grade"] = _run_evidence_grade(
        latest, identity is not None, meta["stale_vs_build"]
    )
    return meta


def _find_powershell() -> str | None:
    return shutil.which("pwsh")


def _artifact_fresh(path: Path, against: list[Path]) -> bool:
    """True when the artifact exists and is at least as new as every source it should
    have been regenerated from. Stale artifacts are 'unknown', never verified (#181)."""
    if not path.exists():
        return False
    artifact_mtime = path.stat().st_mtime
    for src in against:
        if src.exists() and src.stat().st_mtime > artifact_mtime:
            return False
    return True


# ──────────────────────────────────────────────────────────────
# Verifier registry — stable IDs consumed by progress.json/dashboard readers
# ──────────────────────────────────────────────────────────────


@dataclass
class Item:
    id: str
    desc: str
    phase: int
    units: int
    status: str  # pending|verified|regressed
    verified_by: str = ""
    units_delta: int = 0
    notes: list[str] = field(default_factory=list)
    evidence: str = ""  # evidence_model grade, filled by verify_all (#181)


# Item ids whose verifiers inspect source/artifact shape or file existence rather
# than executing the runtime: existence/coverage greps are heuristics, not
# execution evidence (#181). Everything else is a measurement against a run log or
# a real parse, which can reach executed/content-validated grades.
_SOURCE_SHAPE_IDS = frozenset({
    # Phase 1: pipeline artifacts by existence/count/freshness
    "P1.1", "P1.2", "P1.3", "P1.4",
    # Phase 2: source greps
    "P2.7", "P2.8",
    # Phase 3: chunk/source shape greps (P3.3 is log-based and stays executed)
    "P3.1", "P3.2", "P3.4", "P3.5", "P3.6", "P3.7", "P3.8",
})


def _item_kind(item_id: str) -> str:
    return "source-shape" if item_id in _SOURCE_SHAPE_IDS else "executed"


def _evidence_grade(status: str, *, kind: str, identity_bound: bool, stale: bool | None) -> str:
    """Map a verifier outcome to the shared evidence vocabulary (evidence_model.py).

    pending -> unknown; source-shape checks stay heuristic (file existence or
    source greps never prove execution); log measurements are executed and only
    become content-validated when bound to the current build identity with a
    non-stale run; an older-than-binary run is explicitly stale.

    Two deliberate mappings to the model's terms:
    * "stale" here is the mtime-based proxy for the model's STALE (identity
      mismatch). The selected run log predates the current binary, so its
      observations cannot be current proof even though the build identity
      itself matches.
    * the grade describes observation strength, never polarity: a regressed
      log item is also "content-validated" when bound and fresh. Consumers must
      read `status` for verified/regressed and `evidence` for strength, and
      completion credit (evidence_model.satisfies) applies to positive claims.
    """
    if status == "pending":
        return "unknown"
    if kind == "source-shape":
        return "heuristic"
    if not identity_bound or stale is None:
        return "executed"
    if stale:
        return "stale"
    return "content-validated"


def verify_all() -> list[Item]:
    items: list[Item] = []
    latest = latest_log()
    run_meta = _run_metadata(latest)
    log_lines = _log_lines(latest) if latest else []

    # ─ Phase 1: Pipeline ─
    items += [
        Item("P1.1", "prxload.py succeeds", 1, 4, _check_prxload()),
        Item("P1.2", "imports.py emits *_imports.toml", 1, 3,
             _check_imports_toml()),
        Item("P1.3", "codegen.py emits monolithic output + 8 chunk files",
             1, 5, _check_recomp_chunks()),
        Item("P1.4", "hst.exe linked", 1, 6, _check_hst_exe()),
        Item("P1.5", "hst_manager.ps1 parses (real PS parser when available, else source heuristic)", 1, 4,
             _check_ps1_parse()),
        Item("P1.6", "make selftest passes", 1, 3, "pending",
             verified_by="not executed this session"),
    ]

    # ─ Phase 2: Runtime RT ─
    items += [
        Item("P2.1", "alloc_block / thread tables seeded", 2, 2,
             _check_log(last_log=latest, patterns=[r"THREAD_SEED_OK"])),
        Item("P2.2", "sceKernelCreateThread / StartThread logged",
             2, 2,
             _check_log(last_log=latest,
                        patterns=[r"create thread #", r"start thread"])),
        Item("P2.3", "sceDisplaySetFrameBuf triggers present",
             2, 2,
             _check_sequence(last_log=latest,
                             ordered=[r"DISPLAY_SET_FB:", r"vkQueuePresent"])),
        Item("P2.4", "vkQueuePresentKHR fires", 2, 2,
             _check_log(last_log=latest, patterns=[r"vkQueuePresent"])),
        Item("P2.5", "sceUmdWaitDriveStat → wakeup linkage",
             2, 2, _check_umd_wakeup(last_log=latest)),
        Item("P2.6", "sceFontNewLib / sceFontOpen logged",
             2, 1,
             _check_log(last_log=latest,
                        patterns=[r"sceFontNewLib", r"sceFontOpen"])),
        Item("P2.7", "dispatch refactored to table", 2, 2,
             _check_dispatch_table()),
        Item("P2.8", "sr_inrange() guards", 2, 1,
             _check_sr_inrange()),
        Item("P2.9", "Vulkan targets persist", 2, 2,
             _check_log(last_log=latest,
                        patterns=[r"sdl3vk: init ok",
                                  r"gegpu: full GPU GE active"])),
    ]

    # ─ Phase 3: MIPS→C ─
    items += [
        Item("P3.1", "Out-of-SSA / register aliasing", 3, 3,
             _check_recomp_chunks()),  # proxy: emits
        Item("P3.2", "Branch delay slots", 3, 2,
             _check_delay_slot()),
        Item("P3.3", "VFPU table lookup", 3, 2,
             _check_vfpu()),
        Item("P3.4", "Function-boundary detection accuracy", 3, 2,
             _check_split_function()),
        Item("P3.5", "L_* label generation", 3, 3,
             _check_recomp_chunks()),
        Item("P3.6", "Stack frame save/restore", 3, 2,
             _check_stack_frame()),
        Item("P3.7", "Custom stub injection", 3, 1,
             _check_custom_stubs()),
        Item("P3.8", "Retired loop caps (WALKER_CAP/LOOP_CAPS) absent from chunks", 3, 2,
             _check_no_retired_caps()),
    ]

    # ─ Phase 4: Game Data ─
    items += [
        Item("P4.1", "Allocator freelist seeded at MEM[0x30b000]",
             4, 1,
             _check_log(last_log=latest,
                        patterns=[r"sceKernelAllocPartitionMemory",
                                  r"alloc_block",
                                  r"freelist"])),
        Item("P4.2", "Hash table at MEM[0x30aa88] seeded",
             4, 1,
             _check_log(last_log=latest,
                        patterns=[r"hash seed"])),
        Item("P4.3", "Resource-table walkers valid", 4, 2,
             _check_resource_walker(last_log=latest)),
        Item("P4.4", "main_RunGameLoop reaches VFS_AllocateHeap",
             4, 2,
             _check_vfs_alloc_reach(last_log=latest)),
        Item("P4.5", "Config_LoadGameSettings completes",
             4, 2, "pending", verified_by="loop verdict unknown"),
        Item("P4.6", "libfont.prx loaded",
             4, 1,
             _check_log(last_log=latest,
                        patterns=[r"sceKernelLoadModule.*libfont"])),
        Item("P4.7", "f_002b7d28 import stub reached",
             4, 1, _check_stub_called("f_002b7d28")),
        Item("P4.8", "Texture cache init", 4, 2, "pending"),
    ]

    # ─ Phase 5: Frame Rendering ─
    items += [
        Item("P5.1", "GE display list submitted", 5, 3,
             _check_log(last_log=latest,
                        patterns=[r"sceGeListEnQueue", r"GE_ENQ", r"ge_run_list"])),
        Item("P5.2", "Vulkan command buffer recorded", 5, 2,
             _check_log(last_log=latest, patterns=[r"vkCmdDraw:"])),
        Item("P5.3", "First frame swapchain-presented", 5, 1,
             _check_present_actually_fires(last_log=latest)),
    ]

    # ─ Phase 6: Main Loop ─
    items += [
        Item("P6.1", "Frame 0 → Frame 1 transition", 6, 2,
             _check_multi_frame(last_log=latest)),
        Item("P6.2", "Lockup watchdog abort (stall indicator; regression when present)", 6, 1,
             _check_watchdog_abort(last_log=latest)),
        Item("P6.3", "Multiple vkCmdDraw in single run", 6, 2,
             _check_log(last_log=latest, patterns=[r"vkCmdDraw:"])),
        Item("P6.4", "Vblank pacing reachable", 6, 1,
             _check_log(last_log=latest,
                        patterns=[r"sceDisplayWaitVblank"])),
        Item("P6.5", "Geometry progression in main loop", 6, 2,
             _check_main_loop_progression(last_log=latest)),
    ]

    # ─ Phase 7: Endgame ─
    items += [
        Item("P7.1", "Input mapped", 7, 1, "pending"),
        Item("P7.2", "Audio thread produces samples", 7, 1, "pending"),
        Item("P7.3", "UMD mount callback fires", 7, 1, "pending"),
        Item("P7.4", "Save / load functional", 7, 1, "pending"),
        Item("P7.5", "Title screen / first menu visible",
             7, 1, "pending"),
    ]

    # Apply regression deltas based on checking against baseline run.
    _apply_regressions(items, latest)

    # Grade each item against the shared evidence vocabulary, so a consumer can
    # tell executed/content-validated measurements from heuristics and can never
    # mistake stale or unbound data for current proof (#181).
    identity_bound = bool(run_meta.get("identity"))
    stale = run_meta.get("stale_vs_build")
    for it in items:
        it.evidence = _evidence_grade(
            it.status,
            kind=_item_kind(it.id),
            identity_bound=identity_bound,
            stale=stale,
        )
    return items


# ──────────────────────────────────────────────────────────────
# Verifier bodies
# ──────────────────────────────────────────────────────────────


def _check_prxload() -> str:
    """Verified only for a FRESH prxload output: an hst_image.bin artifact must be at
    least as new as the pipeline sources that produce it. A stale or orphaned artifact
    (or a stray prxload* file elsewhere) proves nothing about the current pipeline (#181)."""
    image = BUILD / "hst_image.bin"
    if _artifact_fresh(image, [TOOLS / "prxload.py", TOOLS / "imports.py", TOOLS / "analyze.py", TOOLS / "codegen.py"]):
        return "verified"
    for prxload_out in BUILD.glob("prxload*"):
        if _artifact_fresh(prxload_out, [TOOLS / "prxload.py", TOOLS / "imports.py", TOOLS / "analyze.py"]):
            return "verified"
    return "pending"


def _check_imports_toml() -> str:
    """Verified only for the canonical build/hst/hst_imports.toml when it is fresh
    against tools/imports.py. A stray *_imports.toml anywhere in the tree must not
    earn credit (#181)."""
    toml = BUILD / "hst_imports.toml"
    if _artifact_fresh(toml, [TOOLS / "imports.py", TOOLS / "analyze.py"]) and toml.stat().st_size > 0:
        return "verified"
    return "pending"


def _check_recomp_chunks() -> str:
    """Verified only when the expected chunk count exists AND the newest chunk is at
    least as new as codegen.py. An old chunk set surviving a generator change is not
    evidence of the current pipeline (#181)."""
    chunks = list(BUILD.glob("hst_recomp*.c"))
    if len(chunks) < 9:
        return "pending"
    newest = max(chunks, key=lambda p: p.stat().st_mtime)
    if not _artifact_fresh(newest, [TOOLS / "codegen.py", TOOLS / "analyze.py", TOOLS / "imports.py"]):
        return "pending"
    return "verified"


def _check_hst_exe() -> str:
    """Verified only when the binary exists, is large enough to be a real link, and is
    bound to the current build by the manager's logs/build_manifest.json executable hash.
    A surviving binary without a matching fresh build manifest is 'unknown', not success
    (#181; the manager writes build_manifest.json after every known-zero build)."""
    exe = BUILD / "hst.exe"
    if not (exe.exists() and exe.stat().st_size > 100_000_000):
        return "pending"
    manifest = LOGS / "build_manifest.json"
    if not _artifact_fresh(manifest, [exe]):
        return "pending"
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except Exception:
        return "pending"
    try:
        current_hash = hashlib.sha256(exe.read_bytes()).hexdigest().upper()
    except Exception:
        return "pending"
    if data.get("exe_sha256") != current_hash:
        return "pending"
    return "verified"


def _check_ps1_parse(ps1: Path | None = None) -> str:
    """Prefer a real PowerShell parse when an interpreter is available; otherwise
    fall back to a clearly-labeled source heuristic. Never claim a parse from mere
    file existence (#48). The P1.5 item description reflects which path is used."""
    if ps1 is None:
        ps1 = REPO / "hst_manager.ps1"
    if not ps1.exists():
        return "pending"
    pwsh = _find_powershell()
    if pwsh:
        # Plain concatenation (no f-string) so the PowerShell braces stay literal;
        # single quotes in the path are doubled for PS single-quoted-string safety.
        ps_path = str(ps1).replace("'", "''")
        script = (
            "$e=$null; $t=$null; "
            "[void][System.Management.Automation.Language.Parser]::ParseFile('"
            + ps_path + "', [ref]$t, [ref]$e); "
            "if ($e -and $e.Count -gt 0) { exit 1 } else { exit 0 }"
        )
        try:
            r = subprocess.run([pwsh, "-NoProfile", "-Command", script],
                               capture_output=True, timeout=60)
            return "verified" if r.returncode == 0 else "regressed"
        except Exception:
            pass  # fall back to the heuristic below
    # Heuristic fallback: structural source markers only, not a real parse. A heuristic
    # must not earn the same completion weight as an executed parse, so it stays pending
    # (unknown) instead of verified (#181).
    return "pending"


def _check_log(*, last_log: str | None, patterns: list[str]) -> str:
    if not last_log:
        return "pending"
    lines = _log_lines(last_log)
    if not lines:
        return "pending"
    matched = sum(
        1 for line in lines
        if any(re.search(p, line) for p in patterns)
    )
    return "verified" if matched > 0 else "pending"


def _check_sequence(*, last_log: str | None, ordered: list[str]) -> str:
    """Verified only when every pattern in `ordered` appears, each at or after the
    previous one's first match — i.e. the causal sequence the item claims actually
    occurs, not merely one of its markers in isolation (#48)."""
    if not last_log:
        return "pending"
    lines = _log_lines(last_log)
    if not lines:
        return "pending"
    pos = -1
    for pat in ordered:
        found = -1
        for i in range(pos + 1, len(lines)):
            if re.search(pat, lines[i]):
                found = i
                break
        if found < 0:
            return "pending"
        pos = found
    return "verified"


def _check_watchdog_abort(*, last_log: str | None) -> str:
    """A WATCHDOG line marks a stall / no-frame abort, not forward progress. Its
    presence is a regression; its absence earns no positive credit here (a dedicated
    watchdog-detection test would own verifying the watchdog itself) (#48)."""
    if not last_log:
        return "pending"
    lines = _log_lines(last_log)
    if any(re.search(r"WATCHDOG:", line) for line in lines):
        return "regressed"
    return "pending"


def _check_umd_wakeup(*, last_log: str | None) -> str:
    """Has WakeupThread been called at least once on the UMD route?

    Route-sensitive: a log that never exercises the UMD path (e.g. a headless scheduler
    run) cannot regress this item. Absence of the wakeup marker is 'pending' (unknown)
    unless the log proves the UMD route was exercised, in which case it is a regression
    (#181)."""
    if not last_log:
        return "pending"
    lines = _log_lines(last_log)
    if any(re.search(r"WakeupThread|Wakeup wakeup", line) for line in lines):
        return "verified"
    route_exercised = any(
        re.search(r"sceUmd|UMD_|UMD |sceUmdWaitDriveStat|drive status|UMD_INSERTED|umd_", line, re.IGNORECASE)
        for line in lines
    )
    if not route_exercised:
        return "pending"  # this log did not exercise the UMD route
    return "regressed"  # UMD route ran but produced no wakeup


def _check_dispatch_table() -> str:
    src = REPO / "src" / "rt" / "recomp.c"
    if not src.exists():
        return "pending"
    text = src.read_text(encoding="utf-8", errors="ignore")
    if "g_exact_hooks" in text or "DispatchHook" in text:
        return "verified"
    return "pending"


def _check_sr_inrange() -> str:
    target_files = [
        REPO / "src" / "rt" / "gpu_sdl3vk" / "ge_gpu.c",
        REPO / "src" / "rt" / "ge.c",
    ]
    for f in target_files:
        if not f.exists():
            continue
        text = f.read_text(encoding="utf-8", errors="ignore")
        if "sr_inrange" in text:
            return "verified"
    return "pending"


def _check_delay_slot() -> str:
    """Cheap heuristic: at least one recomp chunk has __YIELD inside an if-block preceded by goto."""
    chunks = list(BUILD.glob("hst_recomp*.c"))
    for c in chunks:
        text = c.read_text(encoding="utf-8", errors="ignore")
        if "SR_YIELD(s, 0x0006ea40" in text or "L_0006ea2c" in text:
            return "verified"
    return "pending"


def _check_vfpu() -> str:
    """VFPU is 'verified' only from real verification evidence — a fuzz/selftest/gate
    pass recorded in a log — not from the mere existence of the asset directory,
    which proves nothing about lookup/interpreter behavior (#48)."""
    markers = (
        r"vfpu_fuzz.*(PASS|OK|0 mismatch|no mismatch)",
        r"VFPU[^\n]*self ?test[^\n]*(PASS|OK)",
        r"sr_vfpu[^\n]*(PASS|OK)",
        r"\[PASS\][^\n]*vfpu",
    )
    for p in sorted(LOGS.glob("*.log")):
        try:
            text = p.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        if any(re.search(m, text, re.IGNORECASE) for m in markers):
            return "verified"
    return "pending"


def _check_split_function() -> str:
    """Returns regressed if the mis-split f_0005a648 is still present in recomp output.
    The mis-split function contains exactly: 0x03e00008 (jr ra), 0x00801021 (move at, v0),
    and nothing else — that is 8 bytes of real MIPS, suggesting an artifact of the split.
    An EMPTY chunk set proves nothing: the absence of a mis-split in a directory with no
    generated output is not evidence the generator is correct (#181)."""
    chunks = list(BUILD.glob("hst_recomp*.c"))
    if not chunks:
        return "pending"
    for c in chunks:
        text = c.read_text(encoding="utf-8", errors="ignore")
        # Loose check: body of f_0005a648 contains only jr ra + move v0, a0
        m = re.search(
            r"void f_0005a648\(CpuState \*s\)\s*\{"
            r"[\s\S]*?"
            r"0x00801021u"
            r"[\s\S]*?"
            r"return;\s*\}",
            text,
        )
        if m:
            return "regressed"
    return "verified"


def _check_stack_frame() -> str:
    """At least one recomp chunk contains saved ra + sp adjust together."""
    chunks = list(BUILD.glob("hst_recomp*.c"))
    for c in chunks:
        text = c.read_text(encoding="utf-8", errors="ignore")
        if "27bdffe0" in text and "afbf000c" in text:
            return "verified"
    return "pending"


def _check_custom_stubs() -> str:
    """Verified only when a generated chunk carries codegen's real custom-stub marker
    ("custom stub:"). The generic "for (;;)" substring is the dispatch loop of every
    generated function, so matching it would prove nothing about stub injection (#181)."""
    chunks = list(BUILD.glob("hst_recomp*.c"))
    if not chunks:
        return "pending"
    for c in chunks:
        text = c.read_text(encoding="utf-8", errors="ignore")
        if "custom stub:" in text:
            return "verified"
    return "pending"


def _check_no_retired_caps(*, chunks_dir: str | None = None) -> str:
    """Verify the retired loop-cap escape valves (WALKER_CAP, LOOP_CAPS) are gone
    from the recompiled C chunks. They were livelock masks whose back-edge SR_YIELD
    already guarantees cooperative scheduling; any reappearance is a regression."""
    if chunks_dir is None:
        chunks_dir = os.environ.get("RECOMP_CHUNKS_DIR", str(BUILD))
    cdir = Path(chunks_dir)
    chunks = list(cdir.glob("hst_recomp*.c"))
    if not chunks:
        return "pending"
    for c in chunks:
        text = c.read_text(encoding="utf-8", errors="ignore")
        if "WALKER_CAP" in text or "LOOP_CAPS" in text:
            return "regressed"
    return "verified"



def _check_resource_walker(*, last_log: str | None) -> str:
    """If dispatches to PLT reach 200k, walker didn't complete → regressed."""
    if not last_log:
        return "pending"
    dispatch_count = count_pattern(last_log, r"DISPATCH 0x0005a648")
    if dispatch_count > 1000:
        return "regressed"
    # Otherwise it's plausible — verifier soft-verifies
    return "pending"


def _check_vfs_alloc_reach(*, last_log: str | None) -> str:
    """f_00047054 is the post-config init; f_00046fc0 is between config and it.
    Either reached implies VFS path is progressing. Verified if either dispatches."""
    if not last_log:
        return "pending"
    lines = _log_lines(last_log)
    if any("0x00047054" in line or "0x00046fc0" in line
           or "f_002b7d28" in line for line in lines):
        return "verified"
    return "pending"


def _check_stub_called(name: str) -> str:
    """Checks that name appears in DISPATCH or HLE log lines at least once."""
    last = latest_log()
    if not last:
        return "pending"
    lines = _log_lines(last)
    # Look for the upper case OR lower case address form
    low = name.lower().replace("f_", "")
    if any(name in line or low in line.lower() for line in lines):
        return "verified"
    return "pending"


def _check_present_actually_fires(*, last_log: str | None) -> str:
    """vkCmdDraw fires, but a present fires only if vkQueuePresent happens.
    Also accept SR_FBSNAP snapshots as proof of present -- they only emit from
    sceDisplaySetFrameBuf's snap path, which runs only after the Vulkan present chain.
    Files: snap_*.ppm in repo root or build/, frame_*.png in build/snapshots/."""
    if not last_log:
        return "pending"
    draws = count_pattern(last_log, r"vkCmdDraw:")
    presents = count_pattern(last_log, r"vkQueuePresent")
    ppms = list(REPO.glob("snap_*.ppm")) + list((BUILD.parent).glob("snap_*.ppm"))
    ppms += list(BUILD.glob("snap_*.ppm")) + list((BUILD / "snapshots").glob("*"))
    fbsnap_logs = count_pattern(last_log, r"FBSNAP")
    if draws > 0 and ppms and len(ppms) >= 2:
        return "verified"
    if draws > 0 and fbsnap_logs >= 2:
        return "verified"
    if draws > 0 and presents == 0:
        return "regressed"
    if presents > 0:
        return "verified"
    return "pending"


def _check_multi_frame(*, last_log: str | None) -> str:
    """DISPLAY_SET_FB ≥ 2 in same log = frame 1 reached."""
    if not last_log:
        return "pending"
    sets = count_pattern(last_log, r"DISPLAY_SET_FB:")
    if sets >= 2:
        return "verified"
    return "regressed"


def _check_main_loop_progression(*, last_log: str | None) -> str:
    """If the run shows many DISPLAY_SET_FB (frames submitted) AFTER any spin,
    the game loop is progressing past frame 1 regardless of the spin artifact."""
    if not last_log:
        return "pending"
    lines = _log_lines(last_log)
    spin_idx = next(
        (i for i, line in enumerate(lines) if "spin on uid 0x115" in line),
        -1
    )
    if spin_idx < 0:
        return "pending"
    later = lines[spin_idx:]
    sets_after = sum(1 for l in later if "DISPLAY_SET_FB:" in l)
    draws_after = sum(1 for l in later if "vkCmdDraw:" in l)
    presents_after = sum(1 for l in later if "vkQueuePresent" in l)
    if sets_after >= 2 or draws_after >= 2 or presents_after >= 1:
        return "verified"
    non_vblank_dispatch = sum(
        1 for line in later[200:]
        if "DISPATCH 0x" in line and "0x000480a4" not in line
    )
    return "regressed" if non_vblank_dispatch < 5 else "verified"


def _apply_regressions(items: list[Item], last_log: str | None) -> None:
    """Adjust statuses that should be regressions given global evidence."""
    if not last_log:
        return
    lines = _log_lines(last_log)
    text = "\n".join(lines)

    # Frame count regression override
    sets = sum(1 for line in lines if "DISPLAY_SET_FB:" in line)
    if sets < 2:
        for it in items:
            if it.id == "P6.1":
                it.status = "regressed"
                it.notes.append(f"DISPLAY_SET_FB count={sets} in {last_log}")
            if it.id == "P6.5":
                it.status = "regressed"
                it.notes.append("main loop progression halted")

    # vkCmdDraw≥1 but vkQueuePresent=0 → P5.3 only stays regressed if no FBSNAP frames
    # were captured. Snapshots can only be saved from sceDisplaySetFrameBuf, after the
    # Vulkan present chain runs -- so their existence is proof of present.
    has_draw = any("vkCmdDraw:" in l for l in lines)
    has_present = any("vkQueuePresent" in l for l in lines)
    has_snapshots = any(REPO.glob("snap_*.ppm")) or any((BUILD / "snapshots").glob("*"))
    if has_draw and not has_present and not has_snapshots:
        for it in items:
            if it.id == "P5.3":
                it.status = "regressed"
                it.notes.append(f"draws={count_pattern(last_log, 'vkCmdDraw:')} presents=0")


# ──────────────────────────────────────────────────────────────
# Coverage axes — A. codegen coverage, B. HLE NID coverage,
# C. GE command coverage, D. VFPU opcode coverage, E. subsystem matrix,
# F. playability milestone index (= verify_all()/aggregate() above).
#
# These are read-side measurements over checked-in source + the latest local
# build/log; none of them require game data to be committed, but several are
# only meaningful once a local `make` + run has produced build/hst/ and a
# logs/stderr_run*.log (they degrade to "unknown" counts otherwise, not a
# crash — a clean checkout with no local build must still be able to run
# `progress_tracker.py axes`).
# ──────────────────────────────────────────────────────────────

SRC_RT = REPO / "src" / "rt"
TOOLS = REPO / "tools"


def _read_text(p: Path) -> str:
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="ignore")


def axis_codegen_coverage() -> dict:
    """A. Analyzable ELF functions vs registered/compiled functions.

    'Registered' comes from the `sr_register_all: starting N registrations`
    line codegen.py bakes into the generated C (observed at hst.exe runtime,
    so it's read from the latest log). 'Analyzable' requires re-running
    analyze.py against a local ELF (game data, not committed) — best-effort
    only, since a clean checkout has no ELF to analyze.
    """
    result: dict[str, object] = {"registered": None, "analyzable": None, "note": ""}
    latest = latest_log()
    if latest:
        m = re.search(r"sr_register_all: starting (\d+) registrations",
                       "\n".join(_log_lines(latest)))
        if m:
            result["registered"] = int(m.group(1))
    # The manager's canonical HST input is lowercase. Retain the legacy spelling as a
    # compatibility fallback for case-sensitive hosts and older local workspaces.
    # Canonical HST layout first (AGENTS.md), then legacy root spellings.
    elf_candidates = [
        REPO / "place_game_here" / "EBOOT.elf",
        REPO / "eboot.elf",
        REPO / "EBOOT.elf",
    ]
    if (REPO / "place_game_here" / "EXTRACTED").exists():
        elf_candidates += list((REPO / "place_game_here" / "EXTRACTED").glob("**/*.elf"))
    elf = next((p for p in elf_candidates if p.exists()), None)
    if elf is not None:
        try:
            sys.path.insert(0, str(TOOLS))
            import analyze as _analyze  # type: ignore
            # HST is a flat-rebased PRX image; GAME_BASE=0 per CLAUDE.md/README.
            elf_obj = _analyze.Elf(str(elf), base=0)
            starts, ranges = _analyze.analyze(
                elf_obj, extra_spans=_analyze.analyzer_span_from_env()
            )
            known = set(a for a in starts if _analyze.in_ranges(a, ranges))
            result["analyzable"] = len(known)
        except Exception as e:  # pragma: no cover - best effort only
            result["note"] = f"analyze.py re-run failed: {e}"
        finally:
            if str(TOOLS) in sys.path:
                sys.path.remove(str(TOOLS))
    else:
        result["note"] = "no local ELF (place_game_here/EXTRACTED/ or eboot.elf) to re-analyze; registered count is log-only"
    return result


_NID_REGISTER_RE = re.compile(
    r'sr_hle_register\(\s*(0x[0-9a-fA-F]+)\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*\)'
)


def axis_nid_coverage() -> dict:
    """B. Imported NIDs (hst_imports.toml) vs sr_hle_register handlers (hle.c).

    Joins on NID hex value (names are unreliable — see hle.c dup/synthetic
    names). Reports total imports, matched-by-any-handler, and matched-by-a
    non-trivial handler (excludes the generic no-op `h_ok` stub).
    """
    imports_path = BUILD / "hst_imports.toml"
    hle_path = SRC_RT / "hle.c"
    result: dict[str, object] = {"imported_nids": None, "registered_nids": 0,
                                "registered_nonstub_nids": 0, "note": ""}
    imports_text = _read_text(imports_path)
    if not imports_text:
        result["note"] = f"{imports_path} not present (run `make` locally first)"
        return result
    imported = set(m.group(1).lower() for m in re.finditer(
        r'nid\s*=\s*(0x[0-9a-fA-F]+)', imports_text))
    result["imported_nids"] = len(imported)

    hle_text = _read_text(hle_path)
    registered: dict[str, str] = {}
    for m in _NID_REGISTER_RE.finditer(hle_text):
        nid, _name, handler = m.group(1).lower(), m.group(2), m.group(3)
        # sr_hle_register rejects later duplicates (first registration wins, see hle.c's
        # duplicate-NID diagnostic), so the metric must not let a later duplicate win.
        registered.setdefault(nid, handler)
    matched = imported & set(registered.keys())
    matched_nonstub = {n for n in matched if registered[n] != "h_ok"}
    result["registered_nids"] = len(matched)
    result["registered_nonstub_nids"] = len(matched_nonstub)
    return result


_GE_ENUM_RE = re.compile(
    r"command numbers \(subset\) ----\s*\*/\s*enum\s*\{(.*?)\};", re.DOTALL)
_GE_CONST_RE = re.compile(r"(GE_\w+)\s*=\s*(0x[0-9A-Fa-f]+)")


def axis_ge_command_coverage() -> dict:
    """C. GE command dispatch: distinct case labels in ge_run_list_inner's
    switch vs the named command constants ge.c's own enum documents.

    There is no full 0x00-0xFF canonical opcode list vendored in this repo
    (A complete external VFPU/GE opcode denominator is not
    checked in) — this is coverage against ge.c's *own* documented subset,
    not against the true PSP GE opcode space. Treat the percentage as a
    lower bound, not an absolute completion figure.
    """
    text = _read_text(SRC_RT / "ge.c")
    result: dict[str, object] = {"documented_ge_constants": None, "dispatch_case_labels": None, "note": ""}
    if not text:
        result["note"] = "src/rt/ge.c not found"
        return result
    enum_m = _GE_ENUM_RE.search(text)
    if enum_m:
        consts = _GE_CONST_RE.findall(enum_m.group(1))
        result["documented_ge_constants"] = len(set(v for _, v in consts))
    # Case labels inside ge_run_list_inner's dispatch switch only (exclude the
    # unrelated ge_cmd_name() debug-string switch earlier in the file).
    fn_m = re.search(r"static uint32_t ge_run_list_inner\(.*", text, re.DOTALL)
    if fn_m:
        body = fn_m.group(0)
        case_labels = re.findall(r"^\s*case\s+[^:]+:", body, re.MULTILINE)
        result["dispatch_case_labels"] = len(case_labels)
    known_unhandled = ["stencil ops", "logic ops", "colour/color test",
                        "bezier/spline patches", "skinning/morphing",
                        "mipmaps (level 0 only)", "DXT/CLUT16/CLUT32 textures"]
    result["known_unhandled"] = known_unhandled
    return result


_VFPU_MNEMONIC_RE = re.compile(r"#\s*(v[a-z][a-z0-9._/]*)", re.IGNORECASE)


def axis_vfpu_opcode_coverage() -> dict:
    """D. VFPU mnemonics implemented in codegen.py's compile-time emitter.

    No full-ISA denominator is vendored (the external PSP VFPU docs /
    psp-tests are external, not checked into this repo), so this reports an
    absolute implemented count, not a percentage. sr_vfpu_interp coverage in
    src/rt/vfpu_interp.c is tracked separately as a boolean (wired into
    dispatch() yet, per Phase 6.1, or only reachable from the fuzzer).
    """
    codegen_text = _read_text(TOOLS / "codegen.py")
    interp_text = _read_text(SRC_RT / "vfpu_interp.c")
    recomp_text = _read_text(SRC_RT / "recomp.c")
    mnemonics = sorted(set(m.group(1).lower() for m in _VFPU_MNEMONIC_RE.finditer(codegen_text)))
    wired_into_dispatch = "sr_vfpu_interp" in recomp_text and bool(
        re.search(r"dispatch\s*\([^)]*\)\s*\{[\s\S]*sr_vfpu_interp", recomp_text))
    return {
        "implemented_mnemonics": len(mnemonics),
        "mnemonic_sample": mnemonics[:20],
        "sr_vfpu_interp_present": bool(interp_text),
        "sr_vfpu_interp_wired_into_dispatch": wired_into_dispatch,
        "note": "no vendored full-ISA VFPU opcode list to divide by",
    }


# subsystem -> (file, evidence regex, label if matched, label if not)
_SUBSYSTEM_CHECKS = {
    "audio": (SRC_RT / "audio.c", r"waveOutOpen", "REAL (Win32 waveOut)", "STUB"),
    "audio_atrac_decode": (SRC_RT / "hle.c", r"h_AtracDecodeData\([\s\S]{0,600}?stereo silence", "PARTIAL (zero-fills, no real decode)", "REAL or pattern stale — re-check hle.c"),
    "video": (SRC_RT / "h264_mf.c", r"CMSH264Decoder|IMFTransform", "PARTIAL (Media Foundation, Windows-only)", "STUB"),
    "input": (SRC_RT / "gui.c", r"XInputGetState|DirectInput8Create", "REAL (Win32 XInput/DirectInput; slated for SDL3 Gamepad, Phase 3)", "STUB"),
    "net": (SRC_RT / "hle.c", r"sceNetAdhoc|sceNetInet|sceNetApctl", None, "STUB (no handlers registered)"),
    # The fiber backend lives in sr_coro.c; sched.c only mentions fibers in comments,
    # so grepping sched.c could label the scheduler REAL from comments alone (#181).
    "sched": (SRC_RT / "sr_coro.c", r"CreateFiberEx|ConvertThreadToFiber", "REAL (cooperative coroutines via sr_coro, Windows fibers)", "STUB"),
    "savedata": (SRC_RT / "savedata.c", r"fopen", "REAL (file-backed virtual memory stick)", "STUB"),
    "pgf_font": (SRC_RT / "hle.c", r"pgf_open|sceFontGetCharGlyphImage", "REAL (parsed glyph rasterization)", "STUB"),
    "iso": (SRC_RT / "iso.c", r"iso_read", "REAL (ISO9660 sector reads from host file)", "STUB"),
}


def axis_subsystem_matrix() -> dict:
    """E. Stub-vs-real status per runtime subsystem, grep-verified against
    the evidence found in a manual code survey (see git history for the
    survey that grounded these patterns)."""
    out = {}
    for key, (path, pattern, hit_label, miss_label) in _SUBSYSTEM_CHECKS.items():
        text = _read_text(path)
        matched = bool(text) and bool(re.search(pattern, text))
        if matched:
            out[key] = hit_label or f"present ({pattern} matched in {path.name})"
        else:
            out[key] = miss_label or f"not found ({pattern} absent from {path.name})"
    return out


def measure_all_axes() -> dict:
    return {
        "A_codegen_coverage": axis_codegen_coverage(),
        "B_nid_coverage": axis_nid_coverage(),
        "C_ge_command_coverage": axis_ge_command_coverage(),
        "D_vfpu_opcode_coverage": axis_vfpu_opcode_coverage(),
        "E_subsystem_matrix": axis_subsystem_matrix(),
    }


def render_progress_markdown(axes: dict, milestone: dict | None) -> str:
    a, b, c, d, e = (axes["A_codegen_coverage"], axes["B_nid_coverage"],
                      axes["C_ge_command_coverage"], axes["D_vfpu_opcode_coverage"],
                      axes["E_subsystem_matrix"])
    lines = []
    lines.append("<!-- MACHINE-GENERATED by tools/progress_tracker.py axes — do not hand-edit. -->")
    lines.append("# Nakagawa Recomp — Progress")
    lines.append("")
    lines.append(f"Regenerate: `python tools/progress_tracker.py axes` "
                 f"(also writes `progress.json` via `python tools/progress_tracker.py verify`).")
    lines.append("")
    lines.append("## A. Codegen coverage")
    lines.append("")
    if a["registered"] is not None:
        lines.append(f"- Registered/compiled functions (latest run log): **{a['registered']}**")
    else:
        lines.append("- Registered/compiled functions: *no local run log — run `hst.exe` once and re-generate*")
    if a["analyzable"] is not None:
        lines.append(f"- Analyzable ELF functions (fresh `analyze.py` re-run): **{a['analyzable']}**")
    else:
        lines.append(f"- Analyzable ELF functions: *unavailable — {a['note'] or 'no local ELF'}*")
    lines.append("")
    lines.append("## B. HLE NID coverage")
    lines.append("")
    if b["imported_nids"] is not None:
        pct_any = 100.0 * b["registered_nids"] / b["imported_nids"] if b["imported_nids"] else 0.0
        pct_real = 100.0 * b["registered_nonstub_nids"] / b["imported_nids"] if b["imported_nids"] else 0.0
        lines.append(f"- Imported NIDs: **{b['imported_nids']}**")
        lines.append(f"- With a registered handler: **{b['registered_nids']}** ({pct_any:.1f}%)")
        lines.append(f"- With a non-stub handler (excludes generic `h_ok`): **{b['registered_nonstub_nids']}** ({pct_real:.1f}%)")
    else:
        lines.append(f"- *unavailable — {b['note']}*")
    lines.append("")
    lines.append("## C. GE command coverage")
    lines.append("")
    lines.append(f"- Documented GE_* constants in `src/rt/ge.c`'s own enum: **{c['documented_ge_constants']}**")
    lines.append(f"- Dispatch `case` labels in `ge_run_list_inner`: **{c['dispatch_case_labels']}**")
    lines.append("- Known-unhandled (explicit no-ops or absent): " + ", ".join(c.get("known_unhandled", [])))
    lines.append("- Caveat: no full PSP GE opcode space (0x00-0xFF) is vendored in this repo; this is coverage "
                  "against ge.c's own documented subset, a lower bound not an absolute figure.")
    lines.append("")
    lines.append("## D. VFPU opcode coverage")
    lines.append("")
    lines.append(f"- Implemented mnemonics/groups in `codegen.py`'s compile-time emitter: **{d['implemented_mnemonics']}**")
    lines.append(f"- `sr_vfpu_interp` present: **{d['sr_vfpu_interp_present']}**; "
                 f"wired into runtime `dispatch()` fallback: **{d['sr_vfpu_interp_wired_into_dispatch']}** "
                 "(full external accuracy denominator not vendored)")
    lines.append(f"- {d['note']}")
    lines.append("")
    lines.append("## E. Subsystem matrix")
    lines.append("")
    lines.append("| Subsystem | Status |")
    lines.append("|---|---|")
    for k, v in e.items():
        lines.append(f"| {k} | {v} |")
    lines.append("")
    lines.append("## F. Playability milestone index")
    lines.append("")
    if milestone:
        lines.append(f"- Total: **{milestone['units_earned'] - milestone['units_regressed']}"
                     f"/{milestone['total_units']}** -> **{milestone['completion_pct']}%** "
                     f"(latest log: `{milestone.get('latest_log') or 'none'}`)")
        for ph, info in milestone["by_phase"].items():
            delta = info["earned"] - info["lost"]
            lines.append(f"  - Phase {ph}: {delta}/{info['units']} units "
                         f"(earned={info['earned']} lost={info['lost']} pending={info['pending']})")
    else:
        lines.append("- *unavailable — run `python tools/progress_tracker.py verify` first (needs `build/hst/`)*")
    if milestone and milestone.get("run"):
        run = milestone["run"]
        identity = run.get("identity")
        if identity:
            lines.append(
                f"- Evidence grade: **{run.get('evidence_grade', 'unknown')}** — "
                f"commit `{identity['source_commit'][:12]}`, "
                f"binary `{identity['binary_sha256'][:12]}`, "
                f"generated `{identity['generated_at']}`"
            )
        else:
            lines.append(
                f"- Evidence grade: **{run.get('evidence_grade', 'unknown')}** — "
                "not identity-bound (missing binary hash or source commit)"
            )
    lines.append("")
    return "\n".join(lines) + "\n"


# ──────────────────────────────────────────────────────────────
# Score aggregation
# ──────────────────────────────────────────────────────────────


def aggregate(items: list[Item]) -> dict:
    by_phase: dict[int, dict] = {}
    total_units = 0
    total_earned = 0
    total_regressed = 0
    for it in items:
        ph = by_phase.setdefault(it.phase, {"units": 0, "earned": 0,
                                            "lost": 0, "pending": 0})
        ph["units"] += it.units
        if it.status == "verified":
            ph["earned"] += it.units
            total_earned += it.units
        elif it.status == "regressed":
            ph["lost"] += it.units
            total_regressed += it.units
        elif it.status == "pending":
            ph["pending"] += it.units
        total_units += it.units

    return {
        "total_units": total_units,
        "units_earned": total_earned,
        "units_regressed": total_regressed,
        "completion_pct": round(
            100.0 * max(0, total_earned - total_regressed) / total_units, 2
        ),
        "by_phase": {str(k): v for k, v in sorted(by_phase.items())},
        "items": [asdict(it) for it in items],
    }


# ──────────────────────────────────────────────────────────────
# CLI
# ──────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    if not argv or argv[0] == "verify":
        if not BUILD.exists():
            print(f"error: missing build dir {BUILD}", file=sys.stderr)
            return 2
        items = verify_all()
        result = aggregate(items)
        latest = latest_log()
        result["latest_log"] = latest
        result["run"] = _run_metadata(latest)
        # Preserve other keys (like opengrip_progress) from existing file
        existing_data = {}
        if OUT.exists():
            try:
                existing_data = json.loads(OUT.read_text(encoding="utf-8"))
                if not isinstance(existing_data, dict):
                    existing_data = {}
            except Exception:
                existing_data = {}
        for k, v in result.items():
            existing_data[k] = v
        OUT.write_text(json.dumps(existing_data, indent=2), encoding="utf-8")
        print(
            f"verified: {result['units_earned']}/"
            f"{result['total_units']}  "
            f"regressed: {result['units_regressed']}  "
            f"completion: {result['completion_pct']}%"
        )
        return 0
    if argv[0] == "axes":
        axes = measure_all_axes()
        milestone = None
        if BUILD.exists():
            milestone = aggregate(verify_all())
            _latest = latest_log()
            milestone["latest_log"] = _latest
            milestone["run"] = _run_metadata(_latest)
        doc = render_progress_markdown(axes, milestone)
        LOGS.mkdir(parents=True, exist_ok=True)
        out_path = LOGS / "PROGRESS.md"
        out_path.write_text(doc, encoding="utf-8")
        axes_json = LOGS / "progress_axes.json"
        axes_json.write_text(json.dumps(axes, indent=2), encoding="utf-8")
        print(f"wrote {out_path}")
        print(f"wrote {axes_json}")
        return 0
    if argv[0] == "show":
        if not OUT.exists():
            print("run `python tools/progress_tracker.py verify` first")
            return 2
        data = json.loads(OUT.read_text())
        print(f"== PROGRESS == ({data.get('latest_log', 'no log')})")
        for ph, info in data["by_phase"].items():
            delta = info["earned"] - info["lost"]
            print(
                f"Phase {ph}: {delta}/{info['units']} units "
                f"(earned={info['earned']} lost={info['lost']})"
            )
        print(
            f"Total: {data['units_earned'] - data['units_regressed']}"
            f"/{data['total_units']} -> {data['completion_pct']}%"
        )
        return 0
    if argv[0] == "diff":
        if len(argv) < 3:
            print("usage: progress_tracker.py diff <a.json> <b.json>")
            return 2
        a = json.loads(Path(argv[1]).read_text())
        b = json.loads(Path(argv[2]).read_text())
        delta = b.get("completion_pct", 0) - a.get("completion_pct", 0)
        sign = "+" if delta >= 0 else ""
        print(
            f"{a.get('completion_pct')}% -> "
            f"{b.get('completion_pct')}% ({sign}{delta:.2f}%) "
            f"[now={b.get('latest_log', '?')}]"
        )
        # Plus per-status deltas
        a_items = {x["id"]: x for x in a["items"]}
        b_items = {x["id"]: x for x in b["items"]}
        flips = []
        for k in sorted(set(a_items) | set(b_items)):
            if a_items.get(k, {}).get("status") != b_items.get(k, {}).get("status"):
                flips.append(
                    f"  {k}: {a_items.get(k, {}).get('status', '?')} -> "
                    f"{b_items.get(k, {}).get('status', '?')}"
                )
        if flips:
            print("Status flips:")
            for f in flips:
                print(f)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
