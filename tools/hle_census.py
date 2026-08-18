#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Public-safe HLE semantic capability census.

Consumes the canonical registration extraction from tools/hle_manifest.py
(the single source of truth for src/rt/hle.c registrations) and enriches each
registration with public-source-derived fields. Nothing here is a claim of
PSP support: registration presence, handler presence and semantic status are
kept as separate fields, and every behavioral signal is labeled SOURCE_SHAPE
(static text/structure evidence only).

Per-registration fields:
  module                best-effort PSP module bucket derived from the name
  nid, name, handler    identity (from hle_manifest extraction)
  handler_class         classification (dedicated / fake_success / controlled_unsupported)
  status                curated implementation status (hle_registry_meta)
  semantic_annotation   curated notes (float-return, alias rules, issue links)
  guest_span_behavior   static scan of the handler body for guest-span /
                        guest-memory helper calls (SOURCE_SHAPE, not proof)
  scheduler_interaction static scan for scheduler/callback/VBLANK helper calls
                        (SOURCE_SHAPE, not proof)
  tests                 public test files referencing the NID or registered name
  evidence_links        public issue links and docs references (where represented)
  title_use_evidence    always "none_public" (private title evidence is never
                        published into the public tree)

The census also computes a triage ranking of the top-N UNREVIEWED
registrations by generic PSP relevance (module frequency, public call-site /
test references, scheduler/memory risk signals) so reviewers can prioritize by
common-PSP value rather than raw count or HST need.

CLI:
    python tools/hle_census.py [--out build/hle_census.json] [--top 30]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hle_manifest  # noqa: E402
import hle_registry_meta as meta  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HLE_C = ROOT / "src" / "rt" / "hle.c"
CENSUS_SCHEMA = 1

# --------------------------------------------------------------------------
# Module derivation (best effort, public-name based). A registered name such
# as sceKernelCreateThread maps to the PSP module bucket sceKernel; names that
# do not start with sce (newlib / __sceSas* / sas_ok loop entries) fall into
# an explicit bucket.
# --------------------------------------------------------------------------
_MODULE_RE = re.compile(r"^(?:__)?(sce[A-Z][a-z0-9]*)")


def derive_module(name: str) -> str:
    m = _MODULE_RE.match(name)
    if m:
        return m.group(1)
    if name.startswith("newlib") or "newlib" in name:
        return "newlib"
    if name.startswith("__sceSas"):
        return "sceSas"
    return "other"


# --------------------------------------------------------------------------
# Handler body extraction (comment/#if0-stripped view from hle_manifest).
# --------------------------------------------------------------------------
_DEF_RE = re.compile(
    r"(?m)^\s*(?:static\s+)?uint32_t\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*CpuState\s*\*"
)
_MACRO_USE_RE = re.compile(r"^UTILITY_DIALOG_HANDLERS\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,", re.MULTILINE)


def _find_brace_block(source: str, start: int) -> str:
    """Return the brace-delimited block starting at source[start] == '{'."""
    depth = 0
    i = start
    n = len(source)
    in_str = None
    while i < n:
        c = source[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == in_str:
                in_str = None
        elif c in ('"', "'"):
            in_str = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
        i += 1
    return source[start:i]


def extract_handler_bodies(source: str) -> dict[str, str]:
    """Map handler name -> body text (SOURCE_SHAPE scans)."""
    bodies: dict[str, str] = {}
    for m in _DEF_RE.finditer(source):
        name = m.group(1)
        open_brace = source.find("{", m.end())
        if open_brace < 0:
            continue
        bodies[name] = _find_brace_block(source, open_brace)
    for m in _MACRO_USE_RE.finditer(source):
        prefix = m.group(1)
        open_brace = source.find("{", m.end())
        body = _find_brace_block(source, open_brace) if open_brace >= 0 else ""
        for suffix in ("Init", "Update", "Status", "Shutdown"):
            bodies.setdefault(prefix + suffix, body)
    return bodies


# --------------------------------------------------------------------------
# Static behavioral signals (SOURCE_SHAPE only).
# --------------------------------------------------------------------------
SPAN_HELPERS = (
    "sr_guest_span_writable",
    "sr_guest_span_readable",
    "sr_guest_span",
    "sr_inrange",
    "sr_vram",
)
SCHED_HELPERS = (
    # sr_* scheduler/clock API
    "sr_yield",
    "sr_timeslice",
    "sr_coro_switch",
    "sr_coro_create",
    "sr_coro_destroy",
    "sr_thread_has_pending_callbacks",
    "sr_thread_dispatch_callbacks",
    "sr_callback_unregister_owner",
    "sr_vblank",
    "sr_sched_on",
    "sr_hle_advance_time",
    "sr_hle_refresh",
    "sr_wait",
    "sr_wake",
    # sched_* scheduler API (representative families; any sched_ token counts
    # as a scheduler-interaction signal, classification by keyword below)
    "sched_block_on",
    "sched_block_on_timeout",
    "sched_delay_current",
    "sched_thread_sleep",
    "sched_thread_sleep_cb",
    "sched_thread_wakeup",
    "sched_thread_cancel_wakeup",
    "sched_preempt",
    "sched_exit_current",
    "sched_create_thread",
    "sched_start_thread",
    "sched_terminate_thread",
    "sched_delete_thread",
    "sched_set_priority",
    "sched_resume_dispatch",
    "sched_suspend_dispatch",
    "sched_resume_interrupts",
    "sched_suspend_interrupts",
    "sched_raise_interrupt",
    "sched_wait_vblank",
    "sched_display_is_vblank",
    "sched_vtime_deadline_after",
    "sched_vtime_refresh",
    "sched_vtime_us",
    "sched_set_current_cb_wait",
    "sched_set_current_join_target",
    "sched_take_current_join_result",
    "sched_current_has_pending_wakeup",
)


def _helpers_used(body: str, names: tuple[str, ...]) -> list[str]:
    found = {n for n in names if n in body}
    # any remaining sched_* call not named above is still a scheduler signal
    if body and "sched_" in body:
        for m in re.finditer(r"\bsched_[a-z_]+\(", body):
            found.add(m.group(0)[:-1])
    return sorted(found)


def scheduler_class(body: str) -> str:
    used = _helpers_used(body, SCHED_HELPERS)
    if not used:
        return "none_static"
    classes = []
    if any("vblank" in h for h in used):
        classes.append("vblank")
    if any(("callback" in h) or ("cb_wait" in h) for h in used):
        classes.append("callback")
    if any(
        ("block" in h) or ("sleep" in h) or ("wait" in h) or ("wake" in h)
        or ("delay" in h) or ("preempt" in h) or ("yield" in h)
        or ("timeslice" in h) or ("join" in h) or ("coro_switch" in h)
        for h in used
    ):
        classes.append("wait_or_switch")
    if any(("interrupt" in h) or ("dispatch" in h) for h in used):
        classes.append("interrupts_or_dispatch")
    if any(
        ("exit" in h) or ("delete" in h) or ("terminate" in h)
        or ("create" in h) or ("start" in h) or ("priority" in h)
        for h in used
    ):
        classes.append("thread_lifecycle")
    if any(("vtime" in h) or ("advance" in h) or ("refresh" in h) or ("sched_on" in h) for h in used):
        classes.append("time_state")
    return "+".join(classes) or "none_static"


# --------------------------------------------------------------------------
# Public test / docs reference index.
# --------------------------------------------------------------------------
_TEST_FILES = sorted(
    [str(p.relative_to(ROOT)) for p in (ROOT / "tools").glob("test_*.py")]
    + [str(p.relative_to(ROOT)) for p in (ROOT / "src" / "rt").glob("*selftest*.c")]
)
_DOC_FILES = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "docs").glob("*.md"))


def build_reference_index() -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    """NID/name -> referencing test files and docs files."""
    from collections import defaultdict

    by_nid: dict[str, list[str]] = defaultdict(list)
    by_name: dict[str, list[str]] = defaultdict(list)
    for path in _TEST_FILES + _DOC_FILES:
        try:
            text = Path(ROOT, path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        hexes = set(re.findall(r"0x([0-9a-fA-F]{8})\b", text))
        for h in hexes:
            by_nid["0x" + h.lower()].append(path)
        names = set(re.findall(r"\b(sce[A-Za-z0-9_]+|newlib[A-Za-z0-9_]+|__sceSas[A-Za-z0-9_]+)\b", text))
        for n in names:
            by_name[n].append(path)
    for k in by_nid:
        by_nid[k] = sorted(set(by_nid[k]))
    for k in by_name:
        by_name[k] = sorted(set(by_name[k]))
    return by_nid, by_name


# --------------------------------------------------------------------------
# Generic PSP relevance scoring for the UNREVIEWED triage.
# --------------------------------------------------------------------------
# Curated per-module generic-relevance weight. Higher = a generic (non-HST)
# PSP title is more likely to call this module family and the cost of a wrong
# stub is higher (kernel/I/O/display/power/GE are universally hit).
MODULE_WEIGHTS = {
    "sceKernel": 3.0,
    "sceIo": 3.0,
    "sceDisplay": 3.0,
    "scePower": 3.0,
    "sceGe": 3.0,
    "sceCtrl": 2.5,
    "sceAudio": 2.5,
    "sceDmac": 2.5,
    "sceRtc": 2.0,
    "sceUtility": 2.0,
    "sceFont": 2.0,
    "sceMpeg": 2.0,
    "scePsmf": 2.0,
    "sceAtrac": 2.0,
    "sceSas": 2.0,
    "sceUmd": 1.5,
    "sceUsb": 1.5,
    "sceHprm": 1.5,
    "sceImpose": 1.5,
    "sceNet": 2.0,
    "sceWlanDrv": 1.0,
    "sceReg": 1.5,
    "sceSsl": 1.5,
    "sceHttp": 1.5,
    "sceMp4": 1.5,
    "sceVaudio": 1.5,
    "sceAudiocodec": 1.5,
    "sceOpenPSID": 1.5,
    "sceNand": 1.5,
    "sceChkreg": 1.0,
    "sceVerp": 1.0,
    "sceHpremote": 1.0,
    "sceMaint": 1.0,
    "scePspNp": 1.5,
    "sceMemab": 1.5,
    "sceDdr": 1.5,
    "sceSysreg": 1.5,
    "sceGpio": 1.5,
    "sceMsstor": 1.5,
    "sceSdio": 1.5,
    "sceUart": 1.5,
}


def module_weight(module: str) -> float:
    if module in MODULE_WEIGHTS:
        return MODULE_WEIGHTS[module]
    # prefix match for families like sceNetAdhoc / sceUsbGps / sceMpegbase
    for prefix, w in MODULE_WEIGHTS.items():
        if module.startswith(prefix):
            return w
    return 1.0


# --------------------------------------------------------------------------
# Census assembly.
# --------------------------------------------------------------------------
def build_census(source: str, top: int) -> dict:
    regs = hle_manifest.extract_registrations(source)
    hle_manifest.validate_meta(regs)
    bodies = extract_handler_bodies(hle_manifest.active_source(source))
    by_nid, by_name = build_reference_index()

    nid_names = {r["nid"]: r["name"] for r in regs}
    entries = []
    for r in regs:
        nid = f"0x{r['nid']:08x}"
        handler = r["handler"]
        body = bodies.get(handler, "")
        classification, status = hle_manifest.classify(handler)

        # semantic annotation: curated notes where present
        notes: list[str] = []
        if nid in meta.KNOWN_NID_ISSUES:
            notes.append(f"tracking:{meta.KNOWN_NID_ISSUES[nid]}")
        if nid in meta.FLOAT_RETURN_NIDS:
            notes.append("float_return_f0")
        for rule in meta.ALIAS_RULES:
            if r["name"].startswith(rule["name_prefix"]):
                notes.append(f"alias_rule:{rule['why']}")
        if status in ("complete", "partial"):
            notes.append(f"curated:{status}")

        span_helpers = _helpers_used(body, SPAN_HELPERS)
        sched_helpers = _helpers_used(body, SCHED_HELPERS)
        test_refs = sorted(set(by_nid.get(nid, []) + by_name.get(r["name"], [])))
        doc_refs = [f for f in by_name.get(r["name"], []) if f.startswith("docs/")]
        issue_links = sorted({meta.KNOWN_NID_ISSUES[nid]} if nid in meta.KNOWN_NID_ISSUES else set())
        if nid in meta.FLOAT_RETURN_NIDS:
            issue_links.append(meta.FLOAT_RETURN_NIDS[nid]["issue"])
        issue_links = sorted(set(issue_links))

        entries.append(
            {
                "module": derive_module(r["name"]),
                "nid": nid,
                "name": r["name"],
                "handler": handler,
                "handler_class": classification,
                "status": status,
                "semantic_annotation": notes,
                "guest_span_behavior": {
                    "class": "span_checked_static" if span_helpers else "no_static_span_check",
                    "helpers": span_helpers,
                    "evidence": "SOURCE_SHAPE",
                },
                "scheduler_interaction": {
                    "class": scheduler_class(body),
                    "helpers": sched_helpers,
                    "evidence": "SOURCE_SHAPE",
                },
                "tests": test_refs,
                "evidence_links": {
                    "issues": issue_links,
                    "docs": doc_refs,
                },
                "title_use_evidence": "none_public",
            }
        )

    # --- UNREVIEWED triage -------------------------------------------------
    module_counts: dict[str, int] = {}
    for e in entries:
        module_counts[e["module"]] = module_counts.get(e["module"], 0) + 1

    triage_candidates = []
    for e in entries:
        if e["status"] != "unreviewed":
            continue
        mw = module_weight(e["module"])
        freq_norm = min(module_counts.get(e["module"], 0) / 20.0, 1.0)
        test_bonus = min(len(e["tests"]), 3) * 0.5
        risk = len(e["guest_span_behavior"]["helpers"]) + len(e["scheduler_interaction"]["helpers"])
        risk_bonus = min(risk, 4) * 0.25
        score = mw * (1.0 + 0.5 * freq_norm) + test_bonus + risk_bonus
        triage_candidates.append(
            {
                "rank": 0,
                "nid": e["nid"],
                "name": e["name"],
                "handler": e["handler"],
                "module": e["module"],
                "score": round(score, 3),
                "score_components": {
                    "module_weight": mw,
                    "module_frequency": module_counts.get(e["module"], 0),
                    "frequency_norm": round(freq_norm, 3),
                    "test_bonus": test_bonus,
                    "risk_bonus": risk_bonus,
                    "risk_signals": {
                        "span_helpers": e["guest_span_behavior"]["helpers"],
                        "sched_helpers": e["scheduler_interaction"]["helpers"],
                    },
                },
                "tests": e["tests"],
            }
        )
    triage_candidates.sort(key=lambda t: (-t["score"], t["nid"]))
    for i, t in enumerate(triage_candidates[:top], start=1):
        t["rank"] = i

    counts = {"total": 0, "complete": 0, "partial": 0, "stub": 0,
              "controlled_unsupported": 0, "unreviewed": 0}
    for e in entries:
        counts["total"] += 1
        counts[e["status"]] = counts.get(e["status"], 0) + 1

    return {
        "schema": CENSUS_SCHEMA,
        "source": "src/rt/hle.c",
        "counts": counts,
        "module_counts": dict(sorted(module_counts.items(), key=lambda kv: (-kv[1], kv[0]))),
        "registrations": entries,
        "unreviewed_triage_top": triage_candidates[:top],
        "method": (
            "registration/handler extraction: tools/hle_manifest.py; "
            "behavioral signals are static SOURCE_SHAPE scans of handler bodies; "
            "tests/docs references are public file scans; title_use_evidence is "
            "always none_public by policy."
        ),
    }


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "hle_census.json")
    ap.add_argument("--top", type=int, default=30)
    args = ap.parse_args(argv)
    source = HLE_C.read_text(encoding="utf-8")
    census = build_census(source, top=args.top)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", encoding="ascii", newline="\n") as f:
        json.dump(census, f, indent=2, sort_keys=True)
        f.write("\n")
    c = census["counts"]
    print(
        f"hle_census: {c['total']} registrations "
        f"(complete={c['complete']} partial={c['partial']} stub={c['stub']} "
        f"controlled_unsupported={c['controlled_unsupported']} unreviewed={c['unreviewed']}) "
        f"-> {args.out}"
    )
    print(f"hle_census: top-{min(args.top, len(census['unreviewed_triage_top']))} unreviewed triage written")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
