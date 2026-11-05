#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Measure how stable GE draw identity is across consecutive guest frames.

Render-state interpolation (the motion lane's design for in-between frames and FSR
motion vectors) pairs each draw of frame B with the same draw in frame A. Whether that
works on a title depends on how stable the draws are from frame to frame, and this
tool measures it from the trace the runtime already has. Run a title with
``SR_GEWATCH=1 SR_GEWATCH_AFTER=<first vblank> SR_GEWATCH_LIMIT=<lines>`` and pass the
run's stderr log. Three kinds of line are read, all printed by ``src/rt/``:

* ``GEWATCH frame=<vblank> xform type= count= vtype= idx= vaddr= iaddr=``, one per
  transform-mode PRIM (``ge.c``);
* ``GELIST f=<vblank> list= fbp= prims=``, one per finished display list (``ge.c``);
* ``PRESENT f=<vblank> buf=``, one per immediate display-buffer change
  (``hle_display.c``).

A PRIM's key is ``(type, count, vtype, index format, vaddr, iaddr)``; its signature is
the key without the two addresses. For each pair of consecutive frames the tool
reports, as shares of frame B's PRIMs:

``exact``
    PRIMs whose full key also occurs in A (counted as a multiset).
``exact2``
    the same against the frame before A, which catches lists double-buffered by
    frame parity.
``ordinal``
    positions where A and B have the same signature.
``matched``
    PRIMs the design's matcher pairs: full keys unique in both frames are anchors (the
    longest run in the same order), then equal signatures pair in order between
    anchors (a longest common subsequence).
``vmass``
    the share of B's vertex references (the sum of ``count``) in matched PRIMs.

Frames are grouped either by the vblank that the lines carry (``--group vblank``) or
by guest frame (``--group fbp``, the default). A title that renders at 30 Hz spreads
one guest frame over two vblanks, so grouping by vblank would split it. ``fbp``
grouping starts a new guest frame whenever a list draws into a display buffer other
than the one the previous display list drew into. Each PRIM belongs to the list that
the next ``GELIST`` line closes, and a list into any other framebuffer (a render
target) belongs to the guest frame of the next display list. The display buffers are the ``PRESENT`` buffers when
the log has them, otherwise the two framebuffers that receive the most PRIMs.

The trace logs no per-draw target, no command address and only large or indexed
through-mode sprites, so this is a lower bound on what the matcher sees, not a model
of it. When the trace hits ``SR_GEWATCH_LIMIT`` the transform lines stop while the
list lines go on; the tool reports that and drops the frames after the last
transform line rather than counting them as empty.
"""

from __future__ import annotations

import argparse
import bisect
import difflib
import json
import re
import statistics
import sys
from collections import Counter
from dataclasses import dataclass, field

_XFORM = re.compile(
    r"GEWATCH frame=(\d+) xform type=(\d+) count=(\d+) vtype=0x([0-9a-fA-F]+) idx=(\d+) "
    r"vaddr=0x([0-9a-fA-F]+) iaddr=0x([0-9a-fA-F]+)")
_LIST = re.compile(r"GELIST f=(\d+) list=0x([0-9a-fA-F]+) fbp=0x([0-9a-fA-F]+) prims=(\d+)")
_PRESENT = re.compile(r"PRESENT f=(\d+) buf=0x([0-9a-fA-F]+)")

METRICS = ("exact", "exact2", "ordinal", "matched", "vmass")


@dataclass(frozen=True)
class Prim:
    """One transform-mode PRIM from a ``GEWATCH ... xform`` line."""

    vblank: int
    type: int
    count: int
    vtype: int
    idx: int
    vaddr: int
    iaddr: int

    @property
    def key(self) -> tuple[int, int, int, int, int, int]:
        return (self.type, self.count, self.vtype, self.idx, self.vaddr, self.iaddr)

    @property
    def sig(self) -> tuple[int, int, int, int]:
        return (self.type, self.count, self.vtype, self.idx)


@dataclass
class DrawList:
    """One finished display list and the transform PRIMs logged before it closed."""

    vblank: int
    addr: int
    fbp: int
    prims: list[Prim] = field(default_factory=list)


@dataclass
class Trace:
    lists: list[DrawList]
    presents: list[tuple[int, int]]
    trailing: list[Prim]
    last_xform_vblank: int | None
    last_list_vblank: int | None


def parse(lines) -> Trace:
    """Read the three line kinds out of a mixed runtime log; other lines are ignored."""
    lists: list[DrawList] = []
    presents: list[tuple[int, int]] = []
    pending: list[Prim] = []
    last_x = last_l = None
    for line in lines:
        m = _XFORM.search(line)
        if m:
            f, t, c, vt, ix, va, ia = m.groups()
            pending.append(Prim(int(f), int(t), int(c), int(vt, 16), int(ix), int(va, 16), int(ia, 16)))
            last_x = int(f)
            continue
        m = _LIST.search(line)
        if m:
            f, addr, fbp, _ = m.groups()
            lists.append(DrawList(int(f), int(addr, 16), int(fbp, 16), pending))
            pending = []
            last_l = int(f)
            continue
        m = _PRESENT.search(line)
        if m:
            presents.append((int(m.group(1)), int(m.group(2), 16)))
    return Trace(lists, presents, pending, last_x, last_l)


def display_buffers(trace: Trace) -> set[int]:
    if trace.presents:
        return {buf for _, buf in trace.presents}
    weight: Counter[int] = Counter()
    for dl in trace.lists:
        weight[dl.fbp] += len(dl.prims)
    return {fbp for fbp, _ in weight.most_common(2)}


def group_frames(trace: Trace, mode: str) -> list[list[Prim]]:
    """Return the transform PRIMs of each frame, in draw order; frames without any are dropped."""
    lists = trace.lists
    if trace.last_xform_vblank is not None:
        lists = [dl for dl in lists if dl.vblank <= trace.last_xform_vblank]
    frames: list[list[Prim]] = []
    if mode == "vblank":
        by: dict[int, list[Prim]] = {}
        for dl in lists:
            for p in dl.prims:
                by.setdefault(p.vblank, []).append(p)
        frames = [by[k] for k in sorted(by)]
    elif mode == "fbp":
        shown = display_buffers(trace)
        cur: list[Prim] = []
        offscreen: list[Prim] = []  # lists into other targets wait for the next display list
        cur_fbp = None
        for dl in lists:
            if dl.fbp not in shown:
                offscreen.extend(dl.prims)
                continue
            if cur_fbp is not None and dl.fbp != cur_fbp:
                frames.append(cur)
                cur = []
            cur_fbp = dl.fbp
            cur.extend(offscreen)
            offscreen = []
            cur.extend(dl.prims)
        cur.extend(offscreen)
        if cur:
            frames.append(cur)
    else:
        raise ValueError(f"unknown grouping {mode!r}")
    return [f for f in frames if f]


def _longest_increasing(pairs: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Longest run of pairs (sorted by A position) whose B positions strictly increase."""
    tails: list[int] = []
    tails_i: list[int] = []
    prev = [-1] * len(pairs)
    for i, (_, b) in enumerate(pairs):
        j = bisect.bisect_left(tails, b)
        if j == len(tails):
            tails.append(b)
            tails_i.append(i)
        else:
            tails[j] = b
            tails_i[j] = i
        prev[i] = tails_i[j - 1] if j else -1
    out = []
    k = tails_i[-1] if tails_i else -1
    while k >= 0:
        out.append(pairs[k])
        k = prev[k]
    return out[::-1]


def match(a: list[Prim], b: list[Prim]) -> dict[int, int]:
    """Pair B's PRIMs with A's: unique full keys as ordered anchors, then signatures between them.

    Returns ``{b index: a index}``.
    """
    ka = Counter(p.key for p in a)
    kb = Counter(p.key for p in b)
    pos_a = {p.key: i for i, p in enumerate(a) if ka[p.key] == 1}
    anchors = sorted((pos_a[p.key], j) for j, p in enumerate(b) if kb[p.key] == 1 and p.key in pos_a)
    anchors = _longest_increasing(anchors)
    pairs = {j: i for i, j in anchors}
    bounds = [(-1, -1)] + anchors + [(len(a), len(b))]
    for (a0, b0), (a1, b1) in zip(bounds, bounds[1:], strict=False):
        sa = [p.sig for p in a[a0 + 1:a1]]
        sb = [p.sig for p in b[b0 + 1:b1]]
        if not sa or not sb:
            continue
        sm = difflib.SequenceMatcher(None, sa, sb, autojunk=False)
        for blk in sm.get_matching_blocks():
            for k in range(blk.size):
                pairs[b0 + 1 + blk.b + k] = a0 + 1 + blk.a + k
    return pairs


def _multiset_share(ref: list[Prim], b: list[Prim]) -> float:
    left = Counter(p.key for p in ref)
    hit = 0
    for p in b:
        if left[p.key] > 0:
            left[p.key] -= 1
            hit += 1
    return hit / len(b)


def pair_rows(frames: list[list[Prim]]) -> list[dict]:
    rows = []
    for i in range(1, len(frames)):
        a, b = frames[i - 1], frames[i]
        n = min(len(a), len(b))
        pairs = match(a, b)
        verts = sum(p.count for p in b)
        rows.append({
            "a_vblank": a[0].vblank, "b_vblank": b[0].vblank, "a_prims": len(a), "b_prims": len(b),
            "b_verts": verts,
            "exact": _multiset_share(a, b),
            "exact2": _multiset_share(frames[i - 2], b) if i >= 2 else None,
            "ordinal": sum(1 for k in range(n) if a[k].sig == b[k].sig) / len(b),
            "matched": len(pairs) / len(b),
            "vmass": (sum(b[j].count for j in pairs) / verts) if verts else 1.0,
        })
    return rows


def address_reuse(frames: list[list[Prim]]) -> tuple[int, int]:
    """Count B PRIMs whose signature occurs in A, and how many of those reuse an A address pair."""
    same = total = 0
    for a, b in zip(frames, frames[1:], strict=False):
        seen: dict[tuple, set] = {}
        for p in a:
            seen.setdefault(p.sig, set()).add((p.vaddr, p.iaddr))
        for p in b:
            if p.sig in seen:
                total += 1
                same += (p.vaddr, p.iaddr) in seen[p.sig]
    return same, total


def _quantile(values: list[float], q: float) -> float:
    s = sorted(values)
    return s[min(len(s) - 1, int(q * (len(s) - 1)))]


def summarize(trace: Trace, mode: str) -> dict:
    frames = group_frames(trace, mode)
    out: dict = {"grouping": mode, "frames": len(frames), "pairs": 0}
    if trace.trailing:
        out["unclosed_prims"] = len(trace.trailing)
    if (trace.last_xform_vblank is not None and trace.last_list_vblank is not None
            and trace.last_list_vblank > trace.last_xform_vblank + 1):
        out["truncated_after_vblank"] = trace.last_xform_vblank
    if len(frames) < 2:
        return out
    rows = pair_rows(frames)
    out["pairs"] = len(rows)
    out["prims_per_frame"] = {"median": statistics.median(len(f) for f in frames),
                              "min": min(len(f) for f in frames), "max": max(len(f) for f in frames)}
    out["verts_per_frame_median"] = statistics.median(sum(p.count for p in f) for f in frames)
    out["equal_prim_count_pairs"] = sum(1 for r in rows if r["a_prims"] == r["b_prims"])
    for name in METRICS:
        v = [r[name] for r in rows if r[name] is not None]
        if v:
            out[name] = {"median": statistics.median(v), "p10": _quantile(v, 0.10), "min": min(v)}
    same, total = address_reuse(frames)
    if total:
        out["address_reuse"] = same / total
    out["lowest_vmass"] = [
        {k: r[k] for k in ("a_vblank", "b_vblank", "a_prims", "b_prims", "vmass")}
        for r in sorted(rows, key=lambda r: r["vmass"])[:5]]
    return out


def _print(label: str, s: dict) -> None:
    print(f"== {label} (grouped by {s['grouping']})")
    print(f"frames {s['frames']}, pairs {s['pairs']}")
    if "truncated_after_vblank" in s:
        print(f"WARNING: transform lines stop at vblank {s['truncated_after_vblank']} while lists go on "
              f"(SR_GEWATCH_LIMIT reached?); later frames dropped")
    if "unclosed_prims" in s:
        print(f"note: {s['unclosed_prims']} PRIMs after the last GELIST line were not counted")
    if not s["pairs"]:
        print("too few frames with transform PRIMs")
        return
    ppf = s["prims_per_frame"]
    print(f"PRIMs/frame median {ppf['median']} (min {ppf['min']}, max {ppf['max']}); "
          f"vertex refs/frame median {s['verts_per_frame_median']}")
    print(f"pairs with equal PRIM count: {s['equal_prim_count_pairs']}/{s['pairs']}")
    for name in METRICS:
        if name in s:
            m = s[name]
            print(f"{name:8s} median {m['median']:.3f}  p10 {m['p10']:.3f}  min {m['min']:.3f}")
    if "address_reuse" in s:
        print(f"same-signature PRIMs reusing their (vaddr, iaddr): {s['address_reuse']:.3f}")
    print("lowest vmass: " + ", ".join(
        f"{r['a_vblank']}->{r['b_vblank']} {r['vmass']:.2f} ({r['a_prims']}->{r['b_prims']})"
        for r in s["lowest_vmass"]))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log", help="runtime stderr log from a run with SR_GEWATCH=1")
    ap.add_argument("--label", help="name printed in the report (default: the log path)")
    ap.add_argument("--group", choices=("fbp", "vblank", "both"), default="both",
                    help="how lines are grouped into frames (default: both)")
    ap.add_argument("--json", action="store_true", help="print the summaries as JSON")
    args = ap.parse_args(argv)
    with open(args.log, encoding="utf-8", errors="replace") as fh:
        trace = parse(fh)
    modes = ("fbp", "vblank") if args.group == "both" else (args.group,)
    sums = [summarize(trace, m) for m in modes]
    if args.json:
        print(json.dumps({"label": args.label or args.log, "summaries": sums}, indent=2, sort_keys=True))
    else:
        for s in sums:
            _print(args.label or args.log, s)
    return 0 if any(s["pairs"] for s in sums) else 1


if __name__ == "__main__":
    sys.exit(main())
