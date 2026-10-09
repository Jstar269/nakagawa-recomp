# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Project-authored; the layout model is tools/imports.py (psp_import_table layout).

"""Import-stub census: every import-table stub of an image must reach the HLE.

For each image (an executable or a guest module) the census lists every import-table
stub slot, using imports.function_import_model (the analyzer's own pairing layout), and
checks three things per slot:

  (a) the analyzer owns the stub address as a function entry;
  (b) codegen emits a body for it. Codegen keeps an analyzed entry only when it lies in
      the owned ranges (codegen.build_entry_catalog); a runtime-placed module filters by
      its named code sections alone, which is reported separately as runtime_missing;
  (c) the NID reaches an HLE handler (an sr_hle_register call in src/rt) or the named
      unknown-NID trap in sr_syscall (src/rt/hle.c). The trap is fail-closed, so every
      NID reaches one of the two. (c) is counted, not gated.

A slot failing (a) or (b) is MISSING and carries one reason:

  outside every range       not in any executable range the analyzer owns or codegen emits
  dropped by a late pass    a direct call reached the stub, but the late discovery passes
                            did not keep it
  overlapping a function    the stub address lies inside another function's traced body
  variable import           the image declares variable imports, which the analyzer refuses
  other                     anything else, including a refused or unanalysable image

Usage:
  import_stub_census.py --image PATH [--image PATH ...] [--corpus DIR] [--out OUT.jsonl]
                        [--markdown OUT.md] [--check [--allow-reason REASON ...]]
"""

import argparse
import glob
import json
import os
import re
import sys
import time
from collections import Counter

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import analyze  # noqa: E402
import codegen  # noqa: E402  (build_entry_catalog is the emission filter)
import imports  # noqa: E402
from imports import ImportTableError  # noqa: E402

REASON_OUTSIDE = "outside every range"
REASON_LATE = "dropped by a late pass"
REASON_OVERLAP = "overlapping a function"
REASON_VARIABLE = "variable import"
REASON_OTHER = "other"
REASONS = (REASON_OUTSIDE, REASON_LATE, REASON_OVERLAP, REASON_VARIABLE, REASON_OTHER)

VARIABLE_IMPORTS_CODE = "ANALYZER_VARIABLE_IMPORTS_UNSUPPORTED"
HLE_REGISTRATION = re.compile(r"sr_hle_register\(\s*0x([0-9a-fA-F]+)u?")


def handled_nids(repo_root):
    """Return the NIDs with an HLE handler: every sr_hle_register(0x...) call in src/rt."""
    nids = set()
    for path in glob.glob(os.path.join(repo_root, "src", "rt", "*.c")):
        with open(path, encoding="utf-8", errors="replace") as handle:
            nids.update(int(m.group(1), 16) for m in HLE_REGISTRATION.finditer(handle.read()))
    return frozenset(nids)


def default_base(path):
    """EBOOTs are linked at their load address (no rebase); guest modules are rebased to 0."""
    return None if os.path.basename(path).lower() == "eboot.elf" else 0


def _has_import_table(elf):
    """Mirror the analyzer's gate: it parses imports only for a module-info or a stub section."""
    if elf.sec(".rodata.sceModuleInfo") is not None:
        return True
    return elf.sec(".sceStub.text") is not None and elf.reloc is not None


def _missing(addr, library, nid, reason, detail=""):
    return {"addr": addr, "library": library, "nid": nid, "reason": reason, "detail": detail}


def _image_level(rec, detail):
    """Record one missing finding that names no slot (the image cannot list its slots)."""
    rec["missing"].append(_missing(None, None, None, REASON_OTHER, detail))
    rec["reasons"][REASON_OTHER] = rec["reasons"].get(REASON_OTHER, 0) + 1


def _window_slots(elf):
    """Return {stub address: (library, NID)} read straight from the import windows.

    Used only when the layout model refuses the table. A NID the image does not map is
    None; the slot is still listed, so it is accounted for.
    """
    try:
        windows, _variables = imports.function_import_windows(elf)
    except Exception:  # the window walk itself is unreadable: the caller records it
        return {}
    slots = {}
    for window in windows:
        for index in range(window.count):
            blob = elf.read_at_vaddr(window.nid_data + 4 * index, 4)
            nid = int.from_bytes(blob, "little") if blob is not None and len(blob) == 4 else None
            slots[window.first_sym + 8 * index] = (window.library, nid)
    return slots


def census_image(path, base, *, label, extra_spans=None, handled=frozenset()):
    """Census one image and return its record. Never raises for a bad image; it is recorded."""
    started = time.time()
    rec = {
        "image": label, "status": "ok", "refusal": None, "detail": "",
        "stubs": 0, "handled": 0, "trapped": 0,
        "missing": [], "runtime_missing": [], "reasons": {},
    }
    try:
        elf = analyze.Elf(path, base=base)
    except Exception as exc:  # an unreadable image is a finding, not a crash
        rec.update(status="error", detail=f"{type(exc).__name__}: {str(exc)[:200]}")
        _image_level(rec, f"image unreadable ({type(exc).__name__}): its slots cannot be listed")
        rec["seconds"] = round(time.time() - started, 2)
        return rec

    named = analyze.exec_ranges(elf, extra_spans=extra_spans)
    file_exec = analyze._file_backed_exec_ranges(elf)
    is_module = os.path.basename(path).lower() != "eboot.elf"

    stubs, variable_tables = {}, ()
    if _has_import_table(elf):
        try:
            stubs, _findings, variable_tables = imports.function_import_model(elf)
        except (ImportTableError, imports.LayoutImportError) as exc:
            # LayoutImportError is the layout model's own boundary (the same class the
            # analyzer converts); its code names the refusal when it has one.
            rec.update(status="refused", refusal=getattr(exc, "code", None)
                       or "ANALYZER_IMPORT_TABLE_INVALID", detail=str(exc)[:200])
        except Exception as exc:  # parity with the analyzer's own import-table wrapper
            rec.update(status="refused", refusal="ANALYZER_IMPORT_TABLE_INVALID",
                       detail=f"{type(exc).__name__}: {str(exc)[:200]}")
    if rec["status"] == "refused" and not stubs:
        # The layout model refused the table. The loader still reads each window directly
        # (slot firstSym + 8*i takes nidData[i]), so name every function slot from its
        # window, with the refusal as its reason. A table no window can be read from is
        # one image-level finding instead.
        stubs = _window_slots(elf)
        if not stubs:
            _image_level(rec, f"import table refused: {rec['refusal']}")

    functions, owned, report = set(), [], {}
    analyzed_ok = False
    try:
        functions, owned = analyze.analyze(elf, extra_spans=extra_spans, report=report)
        analyzed_ok = True
    except ImportTableError as exc:
        rec.update(status="refused", refusal=exc.code, detail=str(exc)[:200])
    except Exception as exc:  # an analyzer fault is recorded against every slot
        rec.update(status="error", detail=f"{type(exc).__name__}: {str(exc)[:200]}")

    catalog = set(codegen.build_entry_catalog(functions, owned)) if analyzed_ok else set()

    for addr, (library, nid) in sorted(stubs.items()):
        rec["stubs"] += 1
        if nid in handled:
            rec["handled"] += 1
        else:
            rec["trapped"] += 1
        reason, detail = None, ""
        in_named = analyze.in_ranges(addr, named)
        if not analyzed_ok:
            if rec["refusal"] == VARIABLE_IMPORTS_CODE:
                reason = REASON_VARIABLE
            else:
                reason, detail = REASON_OTHER, f"analysis refused: {rec['refusal'] or rec['status']}"
        elif addr & 3:
            reason, detail = REASON_OTHER, "misaligned stub slot"
        elif addr not in functions:
            if not (in_named or analyze.in_ranges(addr, file_exec)):
                reason = REASON_OUTSIDE
            elif addr in report.get("calls", ()):
                reason = REASON_LATE
            elif addr in report.get("covered", ()):
                reason = REASON_OVERLAP
            else:
                reason, detail = REASON_OTHER, "executable bytes, never reached"
        elif addr not in catalog:
            reason, detail = REASON_OUTSIDE, "entry outside the owned ranges codegen emits from"
        if reason is not None:
            rec["missing"].append(_missing(addr, library, nid, reason, detail))
            rec["reasons"][reason] = rec["reasons"].get(reason, 0) + 1
        elif is_module and not in_named:
            # A guest module placed at runtime keeps only its named code sections
            # (codegen's runtime-module path), so this stub would lose its body there.
            rec["runtime_missing"].append(_missing(addr, library, nid, REASON_OUTSIDE,
                                                   "runtime module filter (named sections only)"))

    rec["seconds"] = round(time.time() - started, 2)
    return rec


def discover_corpus(corpus):
    """Return (path, label, base) for every staged executable and guest module under corpus."""
    images = []
    for path in sorted(glob.glob(os.path.join(corpus, "*", "decrypted", "*"))):
        name = os.path.basename(path).lower()
        if name == "eboot.elf" or name.endswith(".prx"):
            title = os.path.basename(os.path.dirname(os.path.dirname(path)))
            images.append((path, f"{title}/{os.path.basename(path)}", default_base(path)))
    return images


def summarize(records):
    """Totals over census records: images, stubs, missing by reason, runtime-only misses."""
    totals = Counter()
    reasons = Counter()
    for rec in records:
        totals["images"] += 1
        totals["stubs"] += rec["stubs"]
        totals["handled"] += rec["handled"]
        totals["trapped"] += rec["trapped"]
        totals["missing"] += len(rec["missing"])
        totals["runtime_missing"] += len(rec["runtime_missing"])
        if rec["status"] != "ok":
            totals["images_not_ok"] += 1
        for reason, count in rec["reasons"].items():
            reasons[reason] += count
    return {"totals": dict(totals), "reasons": dict(reasons)}


def check_failures(records, allow_reasons=()):
    """Return the records that fail the census gate. Allowed reasons are named decisions."""
    failures = []
    for rec in records:
        blocking = [m for m in rec["missing"] if m["reason"] not in allow_reasons]
        if blocking or rec["runtime_missing"] or rec["status"] == "error":
            failures.append(rec)
    return failures


def render_markdown(records, title="Import-stub census"):
    """Render totals, per-image missing counts and missing entries grouped by reason."""
    summary = summarize(records)
    totals = summary["totals"]
    lines = [f"# {title}", "",
             f"Images: {totals.get('images', 0)}; stubs: {totals.get('stubs', 0)}; "
             f"missing: {totals.get('missing', 0)}; runtime-only misses: "
             f"{totals.get('runtime_missing', 0)}; images not ok: {totals.get('images_not_ok', 0)}.",
             f"HLE handler: {totals.get('handled', 0)}; unknown-NID trap: {totals.get('trapped', 0)}.",
             "", "## Missing by reason", "", "| reason | count |", "| --- | ---: |"]
    for reason in REASONS:
        lines.append(f"| {reason} | {summary['reasons'].get(reason, 0)} |")
    lines += ["", "## Per-image missing counts", "",
              "| image | stubs | missing | runtime-only | status |", "| --- | ---: | ---: | ---: | --- |"]
    for rec in records:
        if rec["missing"] or rec["runtime_missing"] or rec["status"] != "ok":
            lines.append(f"| {rec['image']} | {rec['stubs']} | {len(rec['missing'])} | "
                         f"{len(rec['runtime_missing'])} | {rec['status']}"
                         f"{(': ' + rec['refusal']) if rec['refusal'] else ''} |")
    lines += ["", "## Missing entries by reason", ""]
    for reason in REASONS:
        entries = [(rec["image"], m) for rec in records for m in rec["missing"]
                   if m["reason"] == reason]
        if not entries:
            continue
        lines += [f"### {reason} ({len(entries)})", "",
                  "| image | stub | library | NID | detail |", "| --- | --- | --- | --- | --- |"]
        for image, m in entries:
            addr = "-" if m["addr"] is None else f"0x{m['addr']:08x}"
            nid = "-" if m["nid"] is None else f"0x{m['nid']:08x}"
            lines.append(f"| {image} | {addr} | {m['library'] or '-'} | {nid} | {m['detail']} |")
        lines.append("")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--image", action="append", default=[],
                        help="an executable or guest module to census (repeatable)")
    parser.add_argument("--corpus", help="a directory of <title>/decrypted/ staged images")
    parser.add_argument("--out", help="write one JSON record per image to this file")
    parser.add_argument("--markdown", help="write the rendered census to this file")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 on any missing stub, runtime-only miss, or analyzer fault")
    parser.add_argument("--allow-reason", action="append", default=[], choices=REASONS,
                        help="a named reason a decision still owns; does not fail --check")
    args = parser.parse_args(argv)

    images = [(p, os.path.basename(p), default_base(p)) for p in args.image]
    if args.corpus:
        images += discover_corpus(args.corpus)
    if not images:
        parser.error("no images: pass --image or --corpus")

    repo_root = os.path.dirname(TOOLS_DIR)
    handled = handled_nids(repo_root)
    records = []
    out = open(args.out, "w", encoding="utf-8") if args.out else None
    try:
        for path, label, base in images:
            try:
                rec = census_image(path, base, label=label, handled=handled)
            except Exception as exc:  # a census fault is a recorded finding, never a lost run
                rec = {"image": label, "status": "error", "refusal": None,
                       "detail": f"{type(exc).__name__}: {str(exc)[:200]}", "stubs": 0,
                       "handled": 0, "trapped": 0, "missing": [], "runtime_missing": [],
                       "reasons": {}}
            records.append(rec)
            if out is not None:
                out.write(json.dumps(rec, sort_keys=True) + "\n")
                out.flush()
            print(f"{label} {rec['status']} stubs={rec['stubs']} missing={len(rec['missing'])} "
                  f"runtime={len(rec['runtime_missing'])} {rec.get('seconds', 0)}s", flush=True)
    finally:
        if out is not None:
            out.close()

    summary = summarize(records)
    print(json.dumps(summary, sort_keys=True))
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(render_markdown(records))
    if args.check:
        failures = check_failures(records, tuple(args.allow_reason))
        for rec in failures:
            sys.stderr.write(f"import-stub census: {rec['image']}: "
                             f"{len(rec['missing'])} missing, {len(rec['runtime_missing'])} runtime-only, "
                             f"status {rec['status']}\n")
        return 1 if failures else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
