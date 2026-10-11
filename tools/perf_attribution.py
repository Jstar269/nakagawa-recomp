#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Present cadence and cost attribution for one SR_PERF run, or a diff of two runs.

A run is read from two files that the same run produced:

- the perf summary written through ``SR_PERF_JSON`` (schema ``nakagawa-perf-v1``, written by
  ``write_summary`` in ``src/rt/perf.c``). It is loaded and validated by
  ``perf_summary_diff.load_summary``, so an unknown schema or a malformed field is refused;
- the stderr log, captured with ``SR_PERF=1`` and ``SR_PRESENT_TRACE=1``. ``SR_PERF`` prints the
  one-second ``PERF`` and ``PERF_ATTRIB`` lines (``report_if_due`` in ``src/rt/perf.c``).
  ``SR_PRESENT_TRACE`` prints one ``HOST_PRESENT_SUBMITTED`` line per frame the presenter
  accepted (``display_present_frame`` in ``src/rt/hle.c``). Other lines are ignored and counted.

Definitions
- A present is one ``HOST_PRESENT_SUBMITTED`` line. ``f`` is the vblank counter when it was
  printed and ``buf`` is the framebuffer address the guest displayed.
- A new frame (flip) is a present whose ``buf`` differs from the previous present's ``buf``.
  The first present is a new frame. It sets the baseline and has no interval.
- A re-present is a present whose ``buf`` equals the previous present's ``buf``. A game that
  redraws into one buffer without changing its address is counted as re-presents, so the new
  frame count is a lower bound on distinct frames.
- A vblank interval is the difference in ``f`` between consecutive new frames: the number of
  vblanks from one new frame to the next. ``s_vcount`` only increases within a run, so a
  decrease in ``f`` is reported as non-monotonic. That interval is excluded rather than clamped,
  and the lower ``f`` becomes the new baseline.
- The median is the mean of the two middle values for an even count. p95 and p99 are
  nearest-rank: the value at 1-based rank ``ceil(p * n / 100)``. Fewer than two intervals makes
  every interval statistic ``n/a``.
- Implied frames per second are ``59.94 / median`` and ``59.94 / mean`` vblanks per new frame.
  Either is ``n/a`` when its statistic is zero or missing.
- A cost share is ``cost_ms / wall_ms`` over the complete one-second intervals. A ``PERF`` line
  and a ``PERF_ATTRIB`` line are paired by order. ``cpu`` is ``cpu_ms`` from ``PERF``; ``aot`` is
  ``aot_ms`` and ``ge_cpu`` is ``ge_cpu_ms`` from ``PERF_ATTRIB``. The three shares overlap and
  must not be added: ``aot`` and ``cpu`` are both measured while the guest runs, ``cpu`` is guest
  time minus GE waits, presenter time and guest idle, and ``ge_cpu`` wraps ``ge_run_list``, which
  the GE HLE handlers in ``src/rt/hle.c`` call. The final partial second is never printed to
  stderr, so the telemetry shares cover whole seconds. The whole-run shares come from the summary.

Malformed lines
A line that carries the ``HOST_PRESENT_SUBMITTED`` tag, or starts with a telemetry tag, but does
not match the emitter's format is counted as malformed and listed with its line number. It is
never skipped silently. A line longer than ``MAX_LINE_CHARS`` is read in bounded pieces; it is
malformed when its head carries a tag and ignored otherwise.

Exit status: 0 when both inputs were read and the report was written, even when the report
carries warnings. 2 with an ``error:`` message when an input is missing, unreadable, not UTF-8
or refused by the summary validator.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from collections.abc import Iterator
from pathlib import Path
from typing import Any, TextIO

try:
    from perf_summary_diff import PerfSummaryError, load_summary
except ModuleNotFoundError:
    from tools.perf_summary_diff import PerfSummaryError, load_summary

SCHEMA = "nakagawa-perf-attribution-v1"
DIFF_SCHEMA = "nakagawa-perf-attribution-diff-v1"
VBLANK_HZ = 59.94
MIN_INTERVALS = 2
MAX_LINE_CHARS = 4096
MAX_EXAMPLES = 5
UINT32_MAX = 0xFFFFFFFF
PRESENT_TAG = "HOST_PRESENT_SUBMITTED"
PERF_TAG = "PERF vblank_total="
ATTRIB_TAG = "PERF_ATTRIB aot_ms="

_MS = r"[0-9]+\.[0-9]{3}"
_COUNT = r"[0-9]+"

# (key, value shape) in the order report_if_due prints them (src/rt/perf.c).
PERF_FIELDS: tuple[tuple[str, str], ...] = (
    ("vblank_total", _COUNT), ("wall_ms", _MS), ("fps", _MS), ("frame_ms", _MS),
    ("vblank_hz", _MS), ("cpu_ms", _MS), ("ge_wait_ms", _MS), ("present_ms", _MS),
    ("idle_ms", _MS), ("submits", _COUNT), ("ge_submits", _COUNT),
    ("present_submits", _COUNT), ("waits", _COUNT), ("readback_waits", _COUNT),
    ("present_skips", _COUNT), ("present_wait_ms", _MS), ("target30", "yes|no"),
)
ATTRIB_FIELDS: tuple[tuple[str, str], ...] = (
    ("aot_ms", _MS), ("aot_calls", _COUNT), ("aot_instructions", _COUNT), ("interp_ms", _MS),
    ("interp_calls", _COUNT), ("interp_instructions", _COUNT), ("aot_to_interp", _COUNT),
    ("interp_to_aot", _COUNT), ("vfpu_ms", _MS), ("ge_cpu_ms", _MS), ("vk_submit_ms", _MS),
    ("vk_wait_ms", _MS), ("texture_decode_ms", _MS), ("iso_read_ms", _MS), ("vfs_read_ms", _MS),
    ("h264_ms", _MS), ("atrac_ms", _MS), ("mix_ms", _MS), ("output_ms", _MS),
)


def _line_pattern(head: str, fields: tuple[tuple[str, str], ...]) -> re.Pattern[str]:
    body = " ".join(f"{key}=(?P<{key}>{shape})" for key, shape in fields)
    return re.compile(f"{re.escape(head)} {body}")


# src/rt/hle.c, display_present_frame: "HOST_PRESENT_SUBMITTED f=%u buf=0x%08x fmt=%d stride=%d".
PRESENT_PATTERN = re.compile(
    r"HOST_PRESENT_SUBMITTED f=(?P<f>[0-9]+) buf=0x(?P<buf>[0-9a-fA-F]{8}) "
    r"fmt=(?P<fmt>-?[0-9]+) stride=(?P<stride>-?[0-9]+)"
)
PERF_PATTERN = _line_pattern("PERF", PERF_FIELDS)
ATTRIB_PATTERN = _line_pattern("PERF_ATTRIB", ATTRIB_FIELDS)

# Metric rows shared by the Markdown tables and the diff: (section, label, key, kind).
METRICS: tuple[tuple[str, str, str, str], ...] = (
    ("Presents", "Log lines read", "log.lines", "count"),
    ("Presents", "Lines not of a read kind (ignored)", "log.ignored_lines", "count"),
    ("Presents", "HOST_PRESENT_SUBMITTED lines (well formed)", "presents.well_formed", "count"),
    ("Presents", "New frames (flips; the first present counts)", "presents.new_frames", "count"),
    ("Presents", "Re-presents (same buf as the previous present)", "presents.re_presents", "count"),
    ("Presents", "Malformed HOST_PRESENT_SUBMITTED lines", "presents.malformed_lines", "count"),
    ("Presents", "Presents with f below the previous present", "presents.non_monotonic", "count"),
    ("Presents", "Vblank intervals excluded for a decrease in f", "presents.excluded_intervals", "count"),
    ("Vblanks per new frame", "Intervals", "vblank_intervals.intervals", "count"),
    ("Vblanks per new frame", "Median (p50)", "vblank_intervals.median", "float"),
    ("Vblanks per new frame", "p95 (nearest rank)", "vblank_intervals.p95", "float"),
    ("Vblanks per new frame", "p99 (nearest rank)", "vblank_intervals.p99", "float"),
    ("Vblanks per new frame", "Mean", "vblank_intervals.mean", "float"),
    ("Vblanks per new frame", "Implied fps, 59.94 / median", "vblank_intervals.implied_fps_median", "float"),
    ("Vblanks per new frame", "Implied fps, 59.94 / mean", "vblank_intervals.implied_fps_mean", "float"),
    ("Cost share, one-second intervals", "Complete intervals (PERF and PERF_ATTRIB paired)",
     "telemetry.intervals", "count"),
    ("Cost share, one-second intervals", "Wall ms over those intervals", "telemetry.wall_ms", "ms"),
    ("Cost share, one-second intervals", "cpu_ms, share of wall", "cost_share.cpu.share", "percent"),
    ("Cost share, one-second intervals", "ge_cpu_ms, share of wall", "cost_share.ge_cpu.share", "percent"),
    ("Cost share, one-second intervals", "aot_ms, share of wall", "cost_share.aot.share", "percent"),
    ("Cost share, one-second intervals", "Unpaired PERF or PERF_ATTRIB lines", "telemetry.unpaired_lines", "count"),
    ("Cost share, one-second intervals", "Malformed PERF or PERF_ATTRIB lines", "telemetry.malformed_lines", "count"),
    ("Whole run, from the perf summary", "wall_ns, in ms", "summary.wall_ms", "ms"),
    ("Whole run, from the perf summary", "vblanks.count", "summary.vblanks", "count"),
    ("Whole run, from the perf summary", "vblanks.presents", "summary.presents", "count"),
    ("Whole run, from the perf summary", "guest.ns, share of wall", "summary.guest_share", "percent"),
    ("Whole run, from the perf summary", "guest.aot_ns, share of wall", "summary.aot_share", "percent"),
    ("Whole run, from the perf summary", "ge.cpu_ns, share of wall", "summary.ge_cpu_share", "percent"),
)

DEFINITIONS: tuple[str, ...] = (
    "New frame: a present whose `buf` differs from the previous present's `buf`. The first present counts.",
    "Re-present: a present whose `buf` equals the previous present's `buf`.",
    "Vblanks per new frame: the difference in `f` between consecutive new frames. "
    "n/a below two intervals.",
    "Median: mean of the two middle values for an even count. p95 and p99: nearest-rank, "
    "the value at rank ceil(p * n / 100).",
    f"Implied fps: {VBLANK_HZ} divided by the median or the mean of vblanks per new frame.",
    "Cost share: cost_ms / wall_ms over paired one-second PERF and PERF_ATTRIB lines. "
    "The shares overlap and must not be added.",
    "Whole-run shares: the perf summary's guest, aot and ge.cpu times over wall_ns.",
)


class PerfAttributionError(ValueError):
    """An input could not be read or does not match the format this tool reads."""


def _read_line(handle: TextIO, limit: int, path: Path, line_no: int) -> str:
    try:
        return handle.readline(limit)
    except UnicodeDecodeError as exc:
        raise PerfAttributionError(f"{path}: line {line_no} is not valid UTF-8") from exc


def _read_lines(handle: TextIO, path: Path) -> Iterator[tuple[int, str, bool]]:
    """Yield (line number, text without its newline, overlong) for each physical line.

    An overlong line is yielded as its first MAX_LINE_CHARS + 1 characters with ``overlong``
    set. The rest of that line is read in bounded pieces and discarded.
    """
    limit = MAX_LINE_CHARS + 1
    line_no = 0
    while True:
        line_no += 1
        chunk = _read_line(handle, limit, path, line_no)
        if not chunk:
            return
        if len(chunk) == limit and not chunk.endswith("\n"):
            head = chunk
            while chunk and not chunk.endswith("\n"):
                chunk = _read_line(handle, limit, path, line_no)
            yield line_no, head, True
        else:
            yield line_no, chunk.removesuffix("\n"), False


def _note(bucket: list[str], line_no: int, text: str) -> None:
    if len(bucket) < MAX_EXAMPLES:
        bucket.append(f"line {line_no}: {text}")


class _Scan:
    """One pass over a stderr log. Each line is counted in exactly one bucket."""

    def __init__(self) -> None:
        self.lines = 0
        self.ignored = 0
        self.presents = 0
        self.malformed_presents = 0
        self.malformed_telemetry = 0
        self.new_frames = 0
        self.re_presents = 0
        self.non_monotonic = 0
        self.excluded_intervals = 0
        self.first_f: int | None = None
        self.last_f: int | None = None
        self.intervals: Counter[int] = Counter()
        self.perf_rows: list[tuple[float, float]] = []    # (wall_ms, cpu_ms)
        self.attrib_rows: list[tuple[float, float]] = []  # (aot_ms, ge_cpu_ms)
        self.present_errors: list[str] = []
        self.telemetry_errors: list[str] = []
        self.monotonic_notes: list[str] = []
        self._prev_f: int | None = None
        self._prev_buf: int | None = None
        self._base_f: int | None = None

    def feed(self, line_no: int, text: str, overlong: bool) -> None:
        self.lines += 1
        if overlong:
            if PRESENT_TAG in text:
                self._reject_present(line_no, f"line is longer than {MAX_LINE_CHARS} characters")
            elif text.startswith((PERF_TAG, ATTRIB_TAG)):
                self._reject_telemetry(line_no, f"line is longer than {MAX_LINE_CHARS} characters")
            else:
                self.ignored += 1
        elif PRESENT_TAG in text:
            self._feed_present(line_no, text)
        elif text.startswith(PERF_TAG):
            match = PERF_PATTERN.fullmatch(text)
            if match is None:
                self._reject_telemetry(line_no, "PERF line does not match the emitter format")
            else:
                self.perf_rows.append((float(match["wall_ms"]), float(match["cpu_ms"])))
        elif text.startswith(ATTRIB_TAG):
            match = ATTRIB_PATTERN.fullmatch(text)
            if match is None:
                self._reject_telemetry(line_no, "PERF_ATTRIB line does not match the emitter format")
            else:
                self.attrib_rows.append((float(match["aot_ms"]), float(match["ge_cpu_ms"])))
        else:
            self.ignored += 1

    def _reject_present(self, line_no: int, reason: str) -> None:
        self.malformed_presents += 1
        _note(self.present_errors, line_no, reason)

    def _reject_telemetry(self, line_no: int, reason: str) -> None:
        self.malformed_telemetry += 1
        _note(self.telemetry_errors, line_no, reason)

    def _feed_present(self, line_no: int, text: str) -> None:
        match = PRESENT_PATTERN.fullmatch(text)
        if match is None:
            self._reject_present(line_no, "does not match the HOST_PRESENT_SUBMITTED format")
            return
        f = int(match["f"])
        buf = int(match["buf"], 16)
        fmt = int(match["fmt"])
        stride = int(match["stride"])
        # The emitter prints only frames display_host_span_valid accepted: a non-zero
        # address, fmt 0..3 and a positive stride. Any other value is not what it prints.
        if f > UINT32_MAX:
            self._reject_present(line_no, "f does not fit in 32 bits")
        elif buf == 0:
            self._reject_present(line_no, "buf is zero")
        elif not 0 <= fmt <= 3:
            self._reject_present(line_no, f"fmt={fmt} is outside 0..3")
        elif stride <= 0:
            self._reject_present(line_no, f"stride={stride} is not positive")
        else:
            self._present(line_no, f, buf)

    def _present(self, line_no: int, f: int, buf: int) -> None:
        self.presents += 1
        if self.first_f is None:
            self.first_f = f
        self.last_f = f
        if self._prev_f is not None and f < self._prev_f:
            self.non_monotonic += 1
            _note(self.monotonic_notes, line_no, f"f={f} is below the previous present's f={self._prev_f}")
        if self._prev_buf is None or buf != self._prev_buf:
            self.new_frames += 1
            if self._base_f is not None:
                if f >= self._base_f:
                    self.intervals[f - self._base_f] += 1
                else:
                    self.excluded_intervals += 1
                    _note(self.monotonic_notes, line_no,
                          f"new frame f={f} is below the previous new frame's f={self._base_f}; "
                          "interval excluded")
            self._base_f = f
        else:
            self.re_presents += 1
        self._prev_f = f
        self._prev_buf = buf


def scan_log(path: Path) -> _Scan:
    try:
        handle = path.open(encoding="utf-8")
    except OSError as exc:
        raise PerfAttributionError(f"{path}: cannot read stderr log: {exc.strerror or exc}") from exc
    scan = _Scan()
    with handle:
        for line_no, text, overlong in _read_lines(handle, path):
            scan.feed(line_no, text, overlong)
    return scan


def _kth(ordered: list[tuple[int, int]], rank: int) -> int:
    """Return the value at 0-based ``rank`` of a sample given as sorted (value, count) pairs."""
    seen = 0
    for value, occurrences in ordered:
        seen += occurrences
        if rank < seen:
            return value
    raise PerfAttributionError(f"rank {rank} is outside the sample")


def _interval_stats(intervals: Counter[int]) -> dict[str, Any]:
    count = sum(intervals.values())
    stats: dict[str, Any] = {
        "intervals": count, "median": None, "p95": None, "p99": None, "mean": None,
        "implied_fps_median": None, "implied_fps_mean": None,
    }
    if count < MIN_INTERVALS:
        return stats
    ordered = sorted(intervals.items())
    if count % 2:
        median = float(_kth(ordered, count // 2))
    else:
        median = (_kth(ordered, count // 2 - 1) + _kth(ordered, count // 2)) / 2
    mean = sum(value * occurrences for value, occurrences in ordered) / count
    stats.update(
        median=median,
        # Nearest rank, 1-based: ceil(p * n / 100), computed in integers.
        p95=float(_kth(ordered, (95 * count + 99) // 100 - 1)),
        p99=float(_kth(ordered, (99 * count + 99) // 100 - 1)),
        mean=mean,
        implied_fps_median=VBLANK_HZ / median if median else None,
        implied_fps_mean=VBLANK_HZ / mean if mean else None,
    )
    return stats


def _share(part: float | None, whole: float | None) -> float | None:
    if part is None or not whole:
        return None
    return part / whole


def _cost_block(perf_rows: list[tuple[float, float]],
                attrib_rows: list[tuple[float, float]]) -> tuple[dict[str, Any], dict[str, Any]]:
    pairs = min(len(perf_rows), len(attrib_rows))
    wall = cpu = aot = ge_cpu = None
    if pairs:
        wall = sum(row[0] for row in perf_rows[:pairs])
        cpu = sum(row[1] for row in perf_rows[:pairs])
        aot = sum(row[0] for row in attrib_rows[:pairs])
        ge_cpu = sum(row[1] for row in attrib_rows[:pairs])
    telemetry = {
        "intervals": pairs,
        "unpaired_lines": abs(len(perf_rows) - len(attrib_rows)),
        "wall_ms": wall,
    }
    cost = {
        "cpu": {"ms": cpu, "share": _share(cpu, wall)},
        "ge_cpu": {"ms": ge_cpu, "share": _share(ge_cpu, wall)},
        "aot": {"ms": aot, "share": _share(aot, wall)},
    }
    return telemetry, cost


def _count(value: Any, path: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise PerfAttributionError(f"{path}: expected a non-negative integer")
    return value


def _summary_block(summary: dict[str, Any]) -> dict[str, Any]:
    # perf_summary_diff.validate_summary does not check every field this report reads
    # (vblanks.*, guest.ns), so each one is checked here rather than defaulted.
    wall_ns = _count(summary["wall_ns"], "$.wall_ns")
    guest_ns = _count(summary["guest"]["ns"], "$.guest.ns")
    aot_ns = _count(summary["guest"]["aot_ns"], "$.guest.aot_ns")
    ge_cpu_ns = _count(summary["ge"]["cpu_ns"], "$.ge.cpu_ns")
    return {
        "wall_ms": wall_ns / 1_000_000,
        "vblanks": _count(summary["vblanks"].get("count"), "$.vblanks.count"),
        "presents": _count(summary["vblanks"].get("presents"), "$.vblanks.presents"),
        "guest_share": _share(guest_ns, wall_ns),
        "aot_share": _share(aot_ns, wall_ns),
        "ge_cpu_share": _share(ge_cpu_ns, wall_ns),
    }


def _warnings(scan: _Scan, telemetry: dict[str, Any], intervals: dict[str, Any]) -> list[str]:
    warnings: list[str] = []
    if scan.presents + scan.malformed_presents == 0:
        warnings.append("no HOST_PRESENT_SUBMITTED lines: run with SR_PRESENT_TRACE=1")
    if scan.malformed_presents:
        warnings.append(f"{scan.malformed_presents} malformed HOST_PRESENT_SUBMITTED line(s), "
                        "not counted as presents: " + "; ".join(scan.present_errors))
    if scan.non_monotonic:
        warnings.append(f"{scan.non_monotonic} present(s) with f below the previous present. The "
                        "vblank counter only increases within one run, so the log holds out-of-order "
                        "lines or more than one run: " + "; ".join(scan.monotonic_notes))
    if scan.excluded_intervals:
        warnings.append(f"{scan.excluded_intervals} vblank interval(s) excluded because f fell "
                        "below the previous new frame")
    if scan.malformed_telemetry:
        warnings.append(f"{scan.malformed_telemetry} malformed PERF or PERF_ATTRIB line(s), not used "
                        "in the cost shares: " + "; ".join(scan.telemetry_errors))
    if not scan.perf_rows and not scan.attrib_rows:
        warnings.append("no PERF or PERF_ATTRIB lines: run with SR_PERF=1 to get cost shares")
    if telemetry["unpaired_lines"]:
        warnings.append(f"{telemetry['unpaired_lines']} unpaired PERF or PERF_ATTRIB line(s) left out "
                        "of the cost shares")
    if intervals["intervals"] < MIN_INTERVALS:
        warnings.append(f"{intervals['intervals']} vblank interval(s); the vblank statistics need "
                        f"{MIN_INTERVALS} and are n/a")
    return warnings


def analyze(perf_path: Path, stderr_path: Path) -> dict[str, Any]:
    """Read one run and return the attribution as a JSON-ready dict."""
    summary = _summary_block(load_summary(perf_path))
    scan = scan_log(stderr_path)
    intervals = _interval_stats(scan.intervals)
    telemetry, cost = _cost_block(scan.perf_rows, scan.attrib_rows)
    telemetry["malformed_lines"] = scan.malformed_telemetry
    result: dict[str, Any] = {
        "schema": SCHEMA,
        "sources": {"perf": str(perf_path), "stderr": str(stderr_path)},
        "vblank_hz": VBLANK_HZ,
        "log": {"lines": scan.lines, "ignored_lines": scan.ignored},
        "presents": {
            "well_formed": scan.presents,
            "new_frames": scan.new_frames,
            "re_presents": scan.re_presents,
            "malformed_lines": scan.malformed_presents,
            "non_monotonic": scan.non_monotonic,
            "excluded_intervals": scan.excluded_intervals,
            "first_f": scan.first_f,
            "last_f": scan.last_f,
        },
        "vblank_intervals": intervals,
        "telemetry": telemetry,
        "cost_share": cost,
        "summary": summary,
    }
    result["warnings"] = _warnings(scan, telemetry, intervals)
    return result


def diff_runs(perf_a: Path, stderr_a: Path, perf_b: Path, stderr_b: Path) -> dict[str, Any]:
    run_a = analyze(perf_a, stderr_a)
    run_b = analyze(perf_b, stderr_b)
    flat_a = _flatten(run_a)
    flat_b = _flatten(run_b)
    delta = {key: _delta(flat_a[key], flat_b[key]) for _, _, key, _ in METRICS}
    return {"schema": DIFF_SCHEMA, "a": run_a, "b": run_b, "delta": delta}


def _flatten(node: dict[str, Any], prefix: str = "") -> dict[str, Any]:
    flat: dict[str, Any] = {}
    for key, value in node.items():
        if isinstance(value, dict):
            flat.update(_flatten(value, f"{prefix}{key}."))
        else:
            flat[f"{prefix}{key}"] = value
    return flat


def _delta(before: Any, after: Any) -> Any:
    if before is None or after is None:
        return None
    return after - before


def _cell(value: Any, kind: str) -> str:
    if value is None:
        return "n/a"
    if kind == "count":
        return str(value)
    if kind == "percent":
        return f"{100.0 * value:.2f}%"
    if kind == "ms":
        return f"{value:.3f}"
    return f"{value:.2f}"


def _delta_cell(before: Any, after: Any, kind: str) -> str:
    delta = _delta(before, after)
    if delta is None:
        return "n/a"
    if kind == "count":
        return f"{delta:+d}"
    if kind == "percent":
        return f"{100.0 * delta:+.2f} pp"
    if kind == "ms":
        return f"{delta:+.3f}"
    return f"{delta:+.2f}"


def _table(header: list[str], rows: list[list[str]]) -> list[str]:
    lines = ["| " + " | ".join(header) + " |", "| " + " | ".join("---" for _ in header) + " |"]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return lines


def _sections() -> list[tuple[str, list[tuple[str, str, str]]]]:
    sections: dict[str, list[tuple[str, str, str]]] = {}
    for section, label, key, kind in METRICS:
        sections.setdefault(section, []).append((label, key, kind))
    return list(sections.items())


def _warning_lines(warnings: list[str]) -> list[str]:
    if not warnings:
        return []
    return ["", "## Warnings", ""] + [f"- {text}" for text in warnings]


def render_markdown(result: dict[str, Any]) -> str:
    flat = _flatten(result)
    lines = [
        "# SR_PERF present cadence and cost attribution",
        "",
        f"- Perf summary: `{result['sources']['perf']}`",
        f"- Stderr log: `{result['sources']['stderr']}`",
        f"- Vblank rate: {VBLANK_HZ} per second",
        "",
        "## Definitions",
        "",
    ]
    lines.extend(f"- {text}" for text in DEFINITIONS)
    for section, rows in _sections():
        lines.extend(["", f"## {section}", ""])
        lines.extend(_table(["Metric", "Value"],
                            [[label, _cell(flat[key], kind)] for label, key, kind in rows]))
    lines.extend(_warning_lines(result["warnings"]))
    return "\n".join(lines)


def render_diff_markdown(result: dict[str, Any]) -> str:
    flat_a = _flatten(result["a"])
    flat_b = _flatten(result["b"])
    lines = [
        "# SR_PERF present cadence: run A against run B",
        "",
        f"- Run A perf summary: `{result['a']['sources']['perf']}`",
        f"- Run A stderr log: `{result['a']['sources']['stderr']}`",
        f"- Run B perf summary: `{result['b']['sources']['perf']}`",
        f"- Run B stderr log: `{result['b']['sources']['stderr']}`",
        f"- Vblank rate: {VBLANK_HZ} per second",
        "",
        "## Definitions",
        "",
    ]
    lines.extend(f"- {text}" for text in DEFINITIONS)
    for section, rows in _sections():
        lines.extend(["", f"## {section}", ""])
        lines.extend(_table(
            ["Metric", "A", "B", "B - A"],
            [[label, _cell(flat_a[key], kind), _cell(flat_b[key], kind),
              _delta_cell(flat_a[key], flat_b[key], kind)] for label, key, kind in rows],
        ))
    lines.extend(_warning_lines([f"run A: {text}" for text in result["a"]["warnings"]]
                                + [f"run B: {text}" for text in result["b"]["warnings"]]))
    return "\n".join(lines)


def _all_warnings(result: dict[str, Any]) -> list[str]:
    if "delta" in result:
        return ([f"run A: {text}" for text in result["a"]["warnings"]]
                + [f"run B: {text}" for text in result["b"]["warnings"]])
    return list(result["warnings"])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Present cadence and cost attribution for one SR_PERF run, or a diff of two runs")
    parser.add_argument("--perf", type=Path, help="perf summary JSON written through SR_PERF_JSON")
    parser.add_argument("--stderr", type=Path,
                        help="stderr log of the same run, captured with SR_PERF=1 and SR_PRESENT_TRACE=1")
    parser.add_argument("--diff", nargs=4, type=Path, metavar=("PERF_A", "STDERR_A", "PERF_B", "STDERR_B"),
                        help="compare run A with run B")
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    args = parser.parse_args(argv)
    if args.diff is not None:
        if args.perf is not None or args.stderr is not None:
            parser.error("--diff takes four paths and cannot be combined with --perf or --stderr")
    elif args.perf is None or args.stderr is None:
        parser.error("--perf and --stderr are both required unless --diff is given")
    try:
        if args.diff is not None:
            result = diff_runs(args.diff[0], args.diff[1], args.diff[2], args.diff[3])
        else:
            result = analyze(args.perf, args.stderr)
    except (PerfAttributionError, PerfSummaryError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if args.format == "json":
        print(json.dumps(result, indent=2, sort_keys=True))
    elif "delta" in result:
        print(render_diff_markdown(result))
    else:
        print(render_markdown(result))
    for warning in _all_warnings(result):
        print(f"warning: {warning}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
