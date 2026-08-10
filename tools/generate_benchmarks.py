#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
"""
AutomTelemetry Benchmark Generator for Nakagawa Recomp
======================================================
Connects to the portable SQLite database (dev.db), extracts chronological
telemetry records, and generates Markdown, PDF, or HTML reports.
"""

import os
import sys
import sqlite3
import json
import argparse
import uuid
import hashlib
import tempfile
import html
from urllib.parse import quote
from datetime import datetime

# Issue #189 contract:
# * Report generation is STRICTLY read-only.  The DB is only ever opened for
#   reading; syncing/importing historical rows is an explicit --sync action and
#   never an implicit side effect of generating a report.
# * SQL is bounded: only the requested row count is fetched, with deterministic
#   ordering, and an explicit maximum is enforced.
# * Every rendered value is validated/escaped; numeric fields must be finite
#   and in-range; HTML/SVG/Markdown/PDF text is escaped.
# * Output is produced atomically: write to an exclusive temp sibling, validate
#   format + byte budget, hash, then rename into place.  Partial output never
#   looks like a completed report.

MAX_REPORT_BYTES = 32 * 1024 * 1024  # hard ceiling for any generated report
MAX_RUN_LIMIT = 1000                  # explicit maximum for --limit
MAX_RAW_JSON_BYTES = 4 * 1024 * 1024  # per-row rawJson blob ceiling

# Issue #189: reports must carry an explicit source/evidence classification so
# local self-reported telemetry can never be silently conflated with emulator,
# private, or PSP-hardware evidence.  The generated reports are derived from the
# local dev.db telemetry database only.
SOURCE_CLASSIFICATION = (
    "Data source: local dev.db telemetry (self-reported pipeline metrics). "
    "Not emulator, private-input, or PSP-hardware evidence."
)

def find_repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def parse_progress_json(progress_path):
    if not os.path.exists(progress_path):
        return None
    try:
        with open(progress_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:
        print(f"Error reading progress.json: {e}", file=sys.stderr)
        return None

def sync_database_from_progress(db_path, progress_path):
    """Seed/Sync TelemetryRun table using progress.json history if missing."""
    progress_data = parse_progress_json(progress_path)
    if not progress_data:
        return 0

    total_units = int(progress_data.get("total_units", progress_data.get("total", 0)))
    units_earned = int(progress_data.get("units_earned", progress_data.get("earned", 0)))
    units_regressed = int(progress_data.get("units_regressed", progress_data.get("regressed", 0)))
    completion_pct = float(progress_data.get("completion_pct", 0.0))
    if not completion_pct and total_units:
        completion_pct = round(((units_earned - units_regressed) / total_units) * 100.0, 2)

    opengrip_progress = progress_data.get("opengrip_progress", [])
    if not opengrip_progress:
        return 0

    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS "TelemetryRun" (
            "id" TEXT NOT NULL PRIMARY KEY,
            "timestamp" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
            "totalUnits" INTEGER NOT NULL,
            "unitsEarned" INTEGER NOT NULL,
            "unitsRegressed" INTEGER NOT NULL,
            "completionPct" REAL NOT NULL,
            "totalFunctions" INTEGER NOT NULL DEFAULT 0,
            "matchedFunctions" INTEGER NOT NULL DEFAULT 0,
            "totalBytes" INTEGER NOT NULL DEFAULT 0,
            "matchedBytes" INTEGER NOT NULL DEFAULT 0,
            "byteCompletionPct" REAL NOT NULL DEFAULT 0.0,
            "svMismatchesCount" INTEGER NOT NULL DEFAULT 0,
            "svMismatchesJson" TEXT NOT NULL DEFAULT '[]',
            "fuzzTotalTrials" INTEGER NOT NULL DEFAULT 0,
            "fuzzPassedTrials" INTEGER NOT NULL DEFAULT 0,
            "fuzzFailedTrials" INTEGER NOT NULL DEFAULT 0,
            "fuzzCoveragePct" REAL NOT NULL DEFAULT 0.0,
            "fuzzCurveJson" TEXT NOT NULL DEFAULT '[]',
            "vrTotalFrames" INTEGER NOT NULL DEFAULT 0,
            "vrPassedFrames" INTEGER NOT NULL DEFAULT 0,
            "vrFailedFrames" INTEGER NOT NULL DEFAULT 0,
            "vrPassRate" REAL NOT NULL DEFAULT 0.0,
            "rawJson" TEXT
        );
    """)
    conn.commit()

    inserted_count = 0
    for entry in opengrip_progress:
        ts_str = entry.get("timestamp")
        if not ts_str:
            continue

        try:
            dt = datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
            ts_formatted = dt.strftime("%Y-%m-%d %H:%M:%S")
        except Exception:
            ts_formatted = ts_str

        cursor.execute('SELECT COUNT(*) FROM "TelemetryRun" WHERE "timestamp" = ? OR "timestamp" LIKE ?', (ts_formatted, ts_formatted + "%"))
        if cursor.fetchone()[0] > 0:
            continue

        run_id = f"cli_{uuid.uuid4().hex[:12]}"
        total_funcs = int(entry.get("total_functions", 0))
        matched_funcs = int(entry.get("matched_functions", 0))
        total_bytes = int(entry.get("total_bytes", 0))
        matched_bytes = int(entry.get("matched_bytes", 0))
        byte_pct = float(entry.get("byte_completion_pct", 0.0))

        cursor.execute("""
            INSERT INTO "TelemetryRun" (
                id, timestamp, totalUnits, unitsEarned, unitsRegressed, completionPct,
                totalFunctions, matchedFunctions, totalBytes, matchedBytes, byteCompletionPct,
                svMismatchesCount, svMismatchesJson, fuzzTotalTrials, fuzzPassedTrials, fuzzFailedTrials,
                fuzzCoveragePct, fuzzCurveJson, vrTotalFrames, vrPassedFrames, vrFailedFrames, vrPassRate,
                rawJson
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, '[]', 0, 0, 0, 0.0, '[]', 0, 0, 0, 0.0, ?)
        """, (
            run_id, ts_formatted, total_units, units_earned, units_regressed, completion_pct,
            total_funcs, matched_funcs, total_bytes, matched_bytes, byte_pct,
            json.dumps({"by_phase": progress_data.get("by_phase", {})})
        ))
        inserted_count += 1

    conn.commit()
    conn.close()
    return inserted_count

# --- ASCII Chart Logic ---
def make_ascii_chart(title, x_labels, y_values, is_percentage=False, y_unit=""):
    if not y_values:
        return "No data points to plot.\n"

    height = 8
    width = 55
    y_min = min(y_values)
    y_max = max(y_values)

    if y_min == y_max:
        y_min -= 1.0 if y_min > 0 else 1.0
        y_max += 1.0

    y_range = y_max - y_min
    y_min_adj = max(0.0, y_min - (y_range * 0.1))
    y_max_adj = y_max + (y_range * 0.1)

    if is_percentage:
        y_max_adj = min(100.0 if y_max > 1.0 else 1.0, y_max_adj)

    grid = [[" " for _ in range(width)] for _ in range(height)]
    num_points = len(y_values)

    for i, val in enumerate(y_values):
        if num_points > 1:
            x_col = int((i / (num_points - 1)) * (width - 1))
        else:
            x_col = width // 2

        y_scaled = (val - y_min_adj) / (y_max_adj - y_min_adj)
        y_row = height - 1 - int(y_scaled * (height - 1))
        y_row = max(0, min(height - 1, y_row))

        grid[y_row][x_col] = "*"

        if i > 0:
            prev_x_col = int(((i - 1) / (num_points - 1)) * (width - 1))
            prev_y_scaled = (y_values[i - 1] - y_min_adj) / (y_max_adj - y_min_adj)
            prev_y_row = height - 1 - int(prev_y_scaled * (height - 1))
            prev_y_row = max(0, min(height - 1, prev_y_row))

            steps = max(abs(x_col - prev_x_col), abs(y_row - prev_y_row))
            for s in range(1, steps):
                t = s / steps
                curr_x = int(prev_x_col + (x_col - prev_x_col) * t)
                curr_y = int(prev_y_row + (y_row - prev_y_row) * t)
                if grid[curr_y][curr_x] == " ":
                    grid[curr_y][curr_x] = "."

    lines = []
    for r in range(height):
        val_at_row = y_max_adj - (r / (height - 1)) * (y_max_adj - y_min_adj)
        if is_percentage:
            lbl = f"{val_at_row * 100:.1f}%" if y_max <= 1.0 else f"{val_at_row:.1f}%"
        else:
            lbl = f"{val_at_row:.1f}{y_unit}"

        row_str = "".join(grid[r])
        lines.append(f"{lbl:>10} | {row_str}")

    lines.append(" " * 11 + "+" + "-" * width)

    x_label_row = " " * 12
    if num_points > 1:
        first_lbl = str(x_labels[0])
        last_lbl = str(x_labels[-1])
        mid_idx = num_points // 2
        mid_lbl = str(x_labels[mid_idx])

        first_pos = 0
        last_pos = width - len(last_lbl)
        mid_pos = int((mid_idx / (num_points - 1)) * (width - 1)) - (len(mid_lbl) // 2)
        mid_pos = max(first_pos + len(first_lbl) + 1, min(last_pos - len(mid_lbl) - 1, mid_pos))

        x_label_row += first_lbl
        x_label_row += " " * (mid_pos - len(first_lbl)) + mid_lbl
        x_label_row += " " * (last_pos - mid_pos - len(mid_lbl)) + last_lbl
    elif num_points == 1:
        x_label_row += str(x_labels[0])

    lines.append(x_label_row)
    return "\n".join(lines)

# --- Pure Python PDF Exporter ---
class PDFDocument:
    def __init__(self):
        self.objects = []

    def add_object(self, content_bytes):
        obj_id = len(self.objects) + 1
        self.objects.append((obj_id, content_bytes))
        return obj_id

    def build(self):
        header = b"%PDF-1.4\n"
        body = bytearray(header)
        offsets = {}

        for obj_id, content in self.objects:
            offsets[obj_id] = len(body)
            body.extend(f"{obj_id} 0 obj\n".encode("ascii"))
            body.extend(content)
            body.extend(b"\nendobj\n")

        xref_offset = len(body)
        body.extend(b"xref\n")
        body.extend(f"0 {len(self.objects) + 1}\n".encode("ascii"))
        body.extend(b"0000000000 65535 f \n")
        for obj_id in range(1, len(self.objects) + 1):
            offset = offsets[obj_id]
            body.extend(f"{offset:010d} 00000 n \n".encode("ascii"))

        body.extend(b"trailer\n")
        body.extend(f"<< /Size {len(self.objects) + 1} /Root 1 0 R /Info {len(self.objects)} 0 R >>\n".encode("ascii"))
        body.extend(b"startxref\n")
        body.extend(f"{xref_offset}\n".encode("ascii"))
        body.extend(b"%%EOF\n")
        return bytes(body)

def generate_pdf_report(runs, latencies, output_path):
    pdf = PDFDocument()

    # 1. Catalog
    pdf.add_object(b"<< /Type /Catalog /Pages 2 0 R >>")
    # 2. Pages
    pdf.add_object(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")

    # Stream content drawing
    stream = []

    def draw_rect(x, y, w, h, fill=None, stroke=None, line_width=1):
        stream.append(f"{line_width} w")
        if fill:
            stream.append(f"{fill[0]} {fill[1]} {fill[2]} rg")
            stream.append(f"{x} {y} {w} {h} re f")
        if stroke:
            stream.append(f"{stroke[0]} {stroke[1]} {stroke[2]} RG")
            stream.append(f"{x} {y} {w} {h} re S")

    def draw_line(x1, y1, x2, y2, color=(0, 0, 0), width: float = 1, dash=False):
        stream.append(f"{width} w")
        stream.append(f"{color[0]} {color[1]} {color[2]} RG")
        if dash:
            stream.append("[3 3] 0 d")
        else:
            stream.append("[] 0 d")
        stream.append(f"{x1} {y1} m {x2} {y2} l S")
        stream.append("[] 0 d") # reset dash

    def escape_str(s):
        # PDF literal-string escaping + ASCII-only guard: non-ASCII bytes from a
        # corrupt row would otherwise crash the .encode("ascii") below, so they
        # are mapped to '?' (fail-closed rendering, never a partial PDF).
        escaped = str(s).replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        return "".join(ch if ord(ch) < 128 else "?" for ch in escaped)

    def draw_text(text, x, y, font="F1", size=10, color=(0, 0, 0)):
        stream.append("BT")
        stream.append(f"/{font} {size} Tf")
        stream.append(f"{color[0]} {color[1]} {color[2]} rg")
        stream.append(f"{x} {y} Td ({escape_str(text)}) Tj")
        stream.append("ET")

    # Dark background header block
    draw_rect(0, 770, 596, 72, fill=(0.08, 0.15, 0.1), stroke=None) # Dark forest / tennis green
    draw_rect(0, 767, 596, 3, fill=(0.73, 0.94, 0.25), stroke=None) # Lime primary line
    draw_text("NAKAGAWA RECOMP OPTIMIZATION LAB", 40, 810, font="F2", size=16, color=(1, 1, 1))
    draw_text("High-Fidelity Telemetry Trend Report", 40, 792, font="F1", size=10, color=(0.7, 0.7, 0.7))

    # Generated Date + source classification (PDF text is escaped by draw_text)
    date_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    draw_text(f"Generated: {date_str}", 440, 810, font="F1", size=8, color=(0.8, 0.8, 0.8))
    draw_text(SOURCE_CLASSIFICATION, 40, 20, font="F1", size=6, color=(0.5, 0.5, 0.5))

    latest = runs[-1]
    lat = latencies[-1]

    # Latest Run Cards
    draw_text("CURRENT COMPLIANCE STATUS", 40, 740, font="F2", size=11, color=(0.1, 0.1, 0.1))

    cards = [
        ("Compile Rate", f"{latest['completionPct']:.2f}%", (0.05, 0.5, 0.9)),
        ("Byte Accuracy", f"{latest['byteCompletionPct']*100.0:.3f}%", (0.1, 0.7, 0.4)),
        ("VR Pass Rate", f"{latest['vrPassRate']:.1f}%", (0.9, 0.6, 0.1)),
        ("Avg Latency", f"{lat:.2f} us" if lat > 0 else "N/A", (0.5, 0.3, 0.8))
    ]

    for idx, (label, val, col) in enumerate(cards):
        cx = 40 + idx * 133
        draw_rect(cx, 680, 118, 50, fill=(0.96, 0.97, 0.96), stroke=(0.85, 0.85, 0.85))
        draw_text(label, cx + 8, 718, font="F1", size=8, color=(0.4, 0.4, 0.4))
        draw_text(val, cx + 8, 694, font="F2", size=15, color=col)

    # Historical runs table
    draw_text("HISTORICAL OPTIMIZATION RUNS", 40, 650, font="F2", size=11, color=(0.1, 0.1, 0.1))

    table_y = 630
    col_x = [40, 110, 240, 320, 400, 480]
    headers = ["Run ID", "Timestamp", "Compile Rate", "Byte Match", "VR Pass", "Latency"]

    # Draw headers
    draw_rect(40, table_y - 18, 516, 18, fill=(0.9, 0.92, 0.9), stroke=(0.8, 0.8, 0.8))
    for x, h in zip(col_x, headers):
        draw_text(h, x + 4, table_y - 13, font="F2", size=8, color=(0.2, 0.2, 0.2))

    table_y -= 18
    # Render last 8 runs in table
    table_runs = runs[-8:]
    table_lats = latencies[-8:]
    for idx, (r, lt) in enumerate(zip(table_runs, table_lats)):
        bg = (0.98, 0.98, 0.98) if idx % 2 == 0 else (1.0, 1.0, 1.0)
        draw_rect(40, table_y - 16, 516, 16, fill=bg, stroke=(0.88, 0.88, 0.88))

        r_id = r["id"][:8]
        ts = r["timestamp"]
        c_rate = f"{r['completionPct']:.1f}%"
        b_match = f"{r['byteCompletionPct']*100.0:.3f}%"
        vr = f"{r['vrPassRate']:.1f}%"
        lt_str = f"{lt:.2f} us" if lt > 0 else "N/A"

        vals = [r_id, ts, c_rate, b_match, vr, lt_str]
        for x, v in zip(col_x, vals):
            draw_text(v, x + 4, table_y - 12, font="F1", size=8, color=(0.3, 0.3, 0.3))
        table_y -= 16

    # Charts section
    draw_text("VISUAL OPTIMIZATION TRENDS", 40, table_y - 20, font="F2", size=11, color=(0.1, 0.1, 0.1))

    # Draw line chart for Compile Rate
    chart_y = table_y - 150
    chart_h = 100
    chart_w = 230

    def draw_trend_chart(x_offset, title, values, y_max_val=100.0, is_pct=True):
        if y_max_val <= 0.0:
            y_max_val = 1.0
        draw_rect(x_offset, chart_y, chart_w, chart_h, fill=(0.98, 0.98, 0.98), stroke=(0.85, 0.85, 0.85))
        draw_text(title, x_offset, chart_y + chart_h + 6, font="F2", size=9, color=(0.1, 0.1, 0.1))

        # Grid lines (y)
        for grid_idx in range(5):
            gy = chart_y + (grid_idx * (chart_h // 4))
            draw_line(x_offset, gy, x_offset + chart_w, gy, color=(0.9, 0.9, 0.9), width=0.5)
            # Label
            val_lbl = (grid_idx / 4.0) * y_max_val
            lbl_str = f"{val_lbl:.1f}%" if is_pct else f"{val_lbl:.1f}"
            draw_text(lbl_str, x_offset - 30, gy - 2, font="F1", size=6, color=(0.5, 0.5, 0.5))

        # Draw line
        num_v = len(values)
        if num_v > 1:
            points = []
            for v_idx, val in enumerate(values):
                vx = x_offset + (v_idx / (num_v - 1)) * chart_w
                vy = chart_y + (val / y_max_val) * chart_h
                vy = max(chart_y, min(chart_y + chart_h, vy))
                points.append((vx, vy))

            # Stroke lines
            for pt_idx in range(len(points) - 1):
                p1 = points[pt_idx]
                p2 = points[pt_idx + 1]
                draw_line(p1[0], p1[1], p2[0], p2[1], color=(0.1, 0.6, 0.3), width=1.5)

            # Draw dots
            for px, py in points:
                # Simple dot: cross lines
                draw_line(px - 2, py, px + 2, py, color=(0.8, 0.2, 0.1), width=1.5)
                draw_line(px, py - 2, px, py + 2, color=(0.8, 0.2, 0.1), width=1.5)

    # Render two side-by-side charts
    # Chart 1: Compile Rate
    draw_trend_chart(50, "Compile Rate (%)", compile_rates := [r["completionPct"] for r in runs[-15:]])
    # Chart 2: Byte Match
    draw_trend_chart(320, "Byte Match (%)", [r["byteCompletionPct"] * 100.0 for r in runs[-15:]])

    # 3. Page Catalog Link
    page_content = "\n".join(stream).encode("ascii")

    # 5. Contents Stream Object
    contents_str = f"<< /Length {len(page_content)} >> stream\n".encode("ascii") + page_content + b"\nendstream"
    contents_id = pdf.add_object(contents_str)

    # 3. Page Object
    pdf.add_object(f"<< /Type /Page /Parent 2 0 R /Resources 4 0 R /MediaBox [0 0 595.28 841.89] /Contents {contents_id} 0 R >>".encode("ascii"))

    # 4. Resources
    pdf.add_object(b"<< /Font << /F1 6 0 R /F2 7 0 R >> >>")

    # 6. Fonts
    pdf.add_object(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /MacRomanEncoding >>")
    pdf.add_object(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /MacRomanEncoding >>")

    # 8. Info
    pdf.add_object(f"<< /Title (Nakagawa Recomp Telemetry Benchmark Report) /Creator (Nakagawa Recomp) /CreationDate (D:{datetime.now().strftime('%Y%m%d%H%M%S')}) >>".encode("ascii"))

    pdf_bytes = pdf.build()
    atomic_write_bytes(output_path, pdf_bytes)

# --- Beautiful HTML Report Exporter ---
def generate_html_report(runs, latencies, output_path):
    latest = runs[-1]
    lat = latencies[-1]
    date_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    # Generate SVG charts data
    def make_svg_chart(values, is_percentage=True, color="#10b981", y_max_val=100.0):
        if not values:
            return ""
        if y_max_val <= 0.0:
            y_max_val = 1.0

        # Grid sizes
        svg_w = 500
        svg_h = 180
        pad_x = 40
        pad_y = 20
        chart_w = svg_w - pad_x * 2
        chart_h = svg_h - pad_y * 2

        # Grid lines (y)
        grid_y = []
        for g_idx in range(5):
            val = (g_idx / 4.0) * y_max_val
            label = f"{val:.1f}%" if is_percentage else f"{val:.1f}"
            y_pos = svg_h - pad_y - (g_idx * (chart_h / 4.0))
            grid_y.append(f'<line x1="{pad_x}" y1="{y_pos}" x2="{svg_w - pad_x}" y2="{y_pos}" stroke="#2a2a2a" stroke-width="0.5" />')
            grid_y.append(f'<text x="{pad_x - 10}" y="{y_pos + 4}" fill="#737373" font-size="9" text-anchor="end">{label}</text>')

        num_v = len(values)
        points = []
        path_d = ""
        area_d = ""
        dots = []

        for idx, val in enumerate(values):
            x = pad_x + (idx / max(1, num_v - 1)) * chart_w
            y = svg_h - pad_y - (val / y_max_val) * chart_h
            y = max(pad_y, min(svg_h - pad_y, y))
            points.append((x, y))

            if idx == 0:
                path_d = f"M {x} {y}"
                area_d = f"M {x} {svg_h - pad_y} L {x} {y}"
            else:
                path_d += f" L {x} {y}"
                area_d += f" L {x} {y}"

            dots.append(
                f'<circle cx="{x}" cy="{y}" r="4" fill="#ff7e47" class="chart-dot" cursor-pointer>'
                f'<title>Run {idx+1}: {val:.2f}%</title>'
                f'</circle>'
            )

        if points:
            area_d += f" L {points[-1][0]} {svg_h - pad_y} Z"

        svg_chart = f"""
        <svg viewBox="0 0 {svg_w} {svg_h}" width="100%" height="100%">
            <!-- Grid lines -->
            {"".join(grid_y)}
            <!-- Shaded Area -->
            <path d="{area_d}" fill="{color}" fill-opacity="0.08" />
            <!-- Line Path -->
            <path d="{path_d}" fill="none" stroke="{color}" stroke-width="2" stroke-linecap="round" />
            <!-- Hover Dots -->
            {"".join(dots)}
        </svg>
        """
        return svg_chart

    # Generate SVGs
    svg_compile = make_svg_chart([r["completionPct"] for r in runs[-15:]])
    svg_bytes = make_svg_chart([r["byteCompletionPct"] * 100.0 for r in runs[-15:]])
    svg_vr = make_svg_chart([r["vrPassRate"] for r in runs[-15:]])
    svg_latency = make_svg_chart([l for l in latencies[-15:]], is_percentage=False, color="#a855f7", y_max_val=max(latencies[-15:] or [1.0]))

    # Historical runs rows.  Every interpolated string is HTML-escaped (#189);
    # numeric fields were validated finite/in-range by validate_run_row().
    def esc(value) -> str:
        return html.escape(str(value), quote=True)

    rows = []
    for r, lt in zip(runs[-15:], latencies[-15:]):
        lat_str = f"{lt:.2f} &mu;s" if lt > 0 else "N/A"
        row_html = f"""
        <tr class="border-b border-neutral-800 hover:bg-neutral-900/40 font-mono text-xs">
            <td class="px-4 py-3 text-neutral-400"><code>{esc(r['id'][:8])}</code></td>
            <td class="px-4 py-3 text-neutral-300">{esc(r['timestamp'])}</td>
            <td class="px-4 py-3 text-center text-sky-400 font-semibold">{r['completionPct']:.2f}%</td>
            <td class="px-4 py-3 text-center text-emerald-400 font-semibold">{r['byteCompletionPct']*100.0:.4f}%</td>
            <td class="px-4 py-3 text-center text-amber-400 font-semibold">{r['vrPassRate']:.1f}%</td>
            <td class="px-4 py-3 text-center text-purple-400">{lat_str}</td>
        </tr>
        """
        rows.append(row_html)

    html_content = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Nakagawa Recomp Optimization Laboratory Report</title>
    <style>
        body {{
            background-color: #0d0d0d;
            color: #e5e5e5;
            font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
            margin: 0;
            padding: 0;
        }}
        .container {{
            max-width: 1200px;
            margin: 0 auto;
            padding: 2rem 1.5rem;
        }}
        header {{
            border-bottom: 2px solid #73f43f; /* Tennis ball yellow-green */
            padding-bottom: 1.5rem;
            margin-bottom: 2rem;
            display: flex;
            justify-content: space-between;
            align-items: flex-end;
            background: linear-gradient(135deg, #132415 0%, #0d0d0d 100%);
            padding: 1.5rem;
            border-radius: 12px;
        }}
        h1 {{
            color: #ffffff;
            margin: 0;
            font-size: 1.8rem;
            letter-spacing: -0.025em;
            font-weight: 800;
        }}
        .subtitle {{
            color: #a3a3a3;
            margin: 0.25rem 0 0 0;
            font-size: 0.95rem;
        }}
        .grid {{
            display: grid;
            grid-template-cols: repeat(auto-fit, minmax(260px, 1fr));
            gap: 1.5rem;
            margin-bottom: 2rem;
        }}
        .card {{
            background-color: #171717;
            border: 1px solid #262626;
            border-radius: 12px;
            padding: 1.25rem;
            box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1), 0 2px 4px -1px rgba(0, 0, 0, 0.06);
            transition: transform 0.2s, border-color 0.2s;
        }}
        .card:hover {{
            transform: translateY(-2px);
            border-color: #3b3b3b;
        }}
        .card-label {{
            font-size: 0.75rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
            color: #a3a3a3;
            font-weight: 600;
        }}
        .card-value {{
            font-size: 2rem;
            font-weight: 700;
            margin: 0.5rem 0 0.25rem 0;
            font-family: monospace;
        }}
        .text-blue {{ color: #0284c7; }}
        .text-green {{ color: #10b981; }}
        .text-amber {{ color: #f59e0b; }}
        .text-purple {{ color: #a855f7; }}

        .charts-container {{
            display: grid;
            grid-template-cols: repeat(auto-fit, minmax(500px, 1fr));
            gap: 1.5rem;
            margin-bottom: 3rem;
        }}
        .chart-box {{
            background-color: #171717;
            border: 1px solid #262626;
            border-radius: 12px;
            padding: 1.5rem;
        }}
        .chart-title {{
            font-size: 1rem;
            font-weight: 700;
            margin: 0 0 1rem 0;
            color: #ffffff;
        }}
        .table-box {{
            background-color: #171717;
            border: 1px solid #262626;
            border-radius: 12px;
            overflow-x: auto;
            margin-bottom: 2rem;
        }}
        table {{
            width: 100%;
            border-collapse: collapse;
            text-align: left;
        }}
        th {{
            background-color: #202020;
            color: #a3a3a3;
            font-size: 0.75rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
            padding: 0.75rem 1rem;
            font-weight: 700;
        }}
        td {{
            padding: 0.75rem 1rem;
        }}
        .chart-dot:hover {{
            fill: #73f43f !important;
            r: 6px !important;
        }}
    </style>
</head>
<body>
    <div class="container">
        <header>
            <div>
                <h1>Nakagawa Recomp Optimization Report</h1>
                <p class="subtitle">Active Execution & Telemetry Laboratory Trends</p>
            </div>
            <div style="font-size: 0.8rem; color: #737373;">
                Report Generated: <b>{date_str}</b>
                <div style="margin-top: 0.35rem; max-width: 34rem;">{esc(SOURCE_CLASSIFICATION)}</div>
            </div>
        </header>

        <!-- KPI Cards -->
        <div class="grid">
            <div class="card">
                <div class="card-label">Overall Compile Rate</div>
                <div class="card-value text-blue">{latest['completionPct']:.2f}%</div>
                <div style="font-size: 0.8rem; color: #737373;">{latest['unitsEarned']} of {latest['totalUnits']} blocks</div>
            </div>
            <div class="card">
                <div class="card-label">MIPS Byte Accuracy</div>
                <div class="card-value text-green">{latest['byteCompletionPct']*100.0:.4f}%</div>
                <div style="font-size: 0.8rem; color: #737373;">{latest['matchedBytes']:,} matched bytes</div>
            </div>
            <div class="card">
                <div class="card-label">VR Pass Rate</div>
                <div class="card-value text-amber">{latest['vrPassRate']:.2f}%</div>
                <div style="font-size: 0.8rem; color: #737373;">{latest['vrPassedFrames']} of {latest['vrTotalFrames']} frames</div>
            </div>
            <div class="card">
                <div class="card-label">Avg Execution Latency</div>
                <div class="card-value text-purple">{lat:.2f} &mu;s</div>
                <div style="font-size: 0.8rem; color: #737373;">per hot block call</div>
            </div>
        </div>

        <!-- Trends Section -->
        <h2 style="font-size: 1.3rem; margin-bottom: 1rem; color: #ffffff;">Optimization & Performance Trends</h2>
        <div class="charts-container">
            <div class="chart-box">
                <h3 class="chart-title">Compile Rate Over Time (%)</h3>
                {svg_compile}
            </div>
            <div class="chart-box">
                <h3 class="chart-title">Byte-Decompilation Accuracy (%)</h3>
                {svg_bytes}
            </div>
            <div class="chart-box">
                <h3 class="chart-title">Synthetic Framebuffer-Memory Parity Pass Rate (%)</h3>
                {svg_vr}
            </div>
            <div class="chart-box">
                <h3 class="chart-title">Average Block Latency (&mu;s/call)</h3>
                {svg_latency}
            </div>
        </div>

        <!-- Historical Table -->
        <h2 style="font-size: 1.3rem; margin-bottom: 1rem; color: #ffffff;">Historical Progress Log</h2>
        <div class="table-box">
            <table>
                <thead>
                    <tr>
                        <th style="padding-left: 1.25rem;">Run ID</th>
                        <th>Timestamp</th>
                        <th style="text-align: center;">Compile Rate</th>
                        <th style="text-align: center;">Byte Match</th>
                        <th style="text-align: center;">VR Pass Rate</th>
                        <th style="text-align: center;">Avg Latency</th>
                    </tr>
                </thead>
                <tbody>
                    {"".join(rows)}
                </tbody>
            </table>
        </div>
    </div>
</body>
</html>
"""
    atomic_write_text(output_path, html_content)

# --- Markdown & Control Logic ---
def validate_run_row(row):
    """Validate a TelemetryRun row against the report schema (issue #189).

    Returns a cleaned row or None.  Non-finite/out-of-range numeric fields,
    malformed timestamps, and unsafe IDs are rejected; a valid row never
    carries NaN/Infinity into a report.
    """
    required_ints = (
        "totalUnits", "unitsEarned", "unitsRegressed", "totalFunctions",
        "matchedFunctions", "totalBytes", "matchedBytes", "fuzzTotalTrials",
        "fuzzPassedTrials", "fuzzFailedTrials", "vrTotalFrames",
        "vrPassedFrames", "vrFailedFrames",
    )
    for field in required_ints:
        val = row.get(field)
        if not isinstance(val, int) or isinstance(val, bool):
            return None
        if val < 0 or val > 0x7FFFFFFFFFFFFFFF:
            return None
    required_floats = ("completionPct", "byteCompletionPct", "vrPassRate", "fuzzCoveragePct")
    for field in required_floats:
        val = row.get(field)
        if not isinstance(val, (int, float)) or isinstance(val, bool):
            return None
        if not (val == val) or val in (float("inf"), float("-inf")):
            return None  # NaN / Infinity rejected
        if not (0.0 <= val <= 100.0):
            return None  # percentages must be in-range
    rid = row.get("id")
    if not isinstance(rid, str) or not rid or len(rid) > 128:
        return None
    if not all(c.isalnum() or c in "-_" for c in rid):
        return None
    ts = row.get("timestamp")
    if not isinstance(ts, str) or not ts:
        return None
    # Bounded blobs: oversized optional JSON payloads are rejected rather than
    # loaded into memory and parsed (#189).
    for blob_field in ("rawJson", "svMismatchesJson", "fuzzCurveJson"):
        blob = row.get(blob_field)
        if blob is not None:
            if not isinstance(blob, str) or len(blob) > MAX_RAW_JSON_BYTES:
                return None
    return dict(row)


def atomic_write_bytes(path, data):
    """Write report bytes atomically: exclusive temp sibling + rename (#189).

    The temp file is created in the destination directory (same filesystem, so
    os.replace is atomic), the byte budget is enforced before any write, and a
    failure always removes the temp sibling so partial output never looks like a
    completed report.  Returns the SHA-256 of the published bytes.
    """
    if len(data) > MAX_REPORT_BYTES:
        raise ValueError(f"report exceeds the {MAX_REPORT_BYTES}-byte budget")
    directory = os.path.dirname(os.path.abspath(path)) or "."
    fd, tmp_path = tempfile.mkstemp(prefix=".report-tmp-", suffix=".part", dir=directory)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_path, path)
    except Exception:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise
    return hashlib.sha256(data).hexdigest()


def atomic_write_text(path, text):
    """Write report text atomically (#189); UTF-8 encoded."""
    return atomic_write_bytes(path, text.encode("utf-8"))


def readonly_uri(db_path):
    """Build the read-only SQLite `file:` URI for db_path (issue #189).

    The absolute path is percent-encoded so Windows paths containing '#', ';'
    or '?' can never be parsed as URI fragments/options.  `mode=ro` refuses any
    write attempt; `immutable=1` is deliberately NOT set (it would silently
    hide WAL-mode updates from a live dev.db).  Note: `mode=ro` fails closed on
    a WAL-mode DB whose -wal/-shm sidecars are unreadable (the connection is
    rejected) - that is intentional; do not "fix" it back to immutable=1.
    """
    return f"file:{quote(os.path.abspath(db_path))}?mode=ro"


def generate_report(db_path, output_path, html_path=None, pdf_path=None, limit=10):
    if not os.path.exists(db_path):
        print(f"Error: Database file not found at {db_path}", file=sys.stderr)
        return False

    limit = max(1, min(int(limit), MAX_RUN_LIMIT))

    conn = sqlite3.connect(readonly_uri(db_path), uri=True)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()

    try:
        # Bounded SQL: fetch only the most-recent requested window, deterministic
        # ordering.  A large DB never defeats the --limit intent (#189).
        cursor.execute(
            'SELECT * FROM "TelemetryRun" ORDER BY "timestamp" DESC, "id" DESC LIMIT ?',
            (limit,),
        )
        all_runs = [dict(row) for row in cursor.fetchall()]
    except Exception as e:
        print(f"Error reading from TelemetryRun table: {e}", file=sys.stderr)
        conn.close()
        return False

    conn.close()

    if not all_runs:
        print("No telemetry runs found in database to generate report.", file=sys.stderr)
        return False

    # Validate every row; invalid rows are rejected, never rendered.
    all_runs = [r for r in (validate_run_row(r) for r in all_runs) if r is not None]
    if not all_runs:
        print("No valid telemetry runs found to generate report.", file=sys.stderr)
        return False

    runs = list(reversed(all_runs))  # chronological again for charts/tables
    latest = all_runs[0]  # most recent (all_runs is DESC)

    x_labels = []
    compile_rates = []
    byte_scores = []
    vr_rates = []
    latencies = []

    for i, r in enumerate(runs):
        try:
            dt = datetime.fromisoformat(r["timestamp"].replace("Z", "+00:00"))
            lbl = dt.strftime("%m-%d")
        except Exception:
            lbl = f"R{i+1}"
        x_labels.append(lbl)

        compile_rates.append(r["completionPct"])
        byte_scores.append(r["byteCompletionPct"])
        vr_rates.append(r["vrPassRate"])

        latency_val = 0.0
        if r["rawJson"]:
            try:
                raw = json.loads(r["rawJson"])
                perf = raw.get("perfProfile", {})
                funcs = perf.get("functions", [])
                if funcs:
                    total_dur = sum(f.get("durationNs", 0) for f in funcs)
                    total_calls = sum(f.get("calls", 0) for f in funcs)
                    if total_calls > 0:
                        latency_val = (total_dur / total_calls) / 1000.0
            except Exception:
                pass
        latencies.append(latency_val)

    # 1. Output Markdown if specified
    if output_path:
        chart_compile = make_ascii_chart("Compile Rate Trend", x_labels, compile_rates, is_percentage=True)
        chart_bytes = make_ascii_chart("MIPS Byte Match Trend", x_labels, byte_scores, is_percentage=True)
        chart_vr = make_ascii_chart("Synthetic Framebuffer-Memory Parity Trend", x_labels, vr_rates, is_percentage=True)
        chart_latency = make_ascii_chart("Avg Execution Latency Trend", x_labels, latencies, y_unit=" us")

        md = [
            "# Nakagawa Recomp Performance & Telemetry Progress Report",
            f"*Generated automatically on {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}*",
            "",
            f"> {SOURCE_CLASSIFICATION}",
            "",
            "## Current Status Snapshot",
            f"- **Total Compilable Units**: {latest['totalUnits']}",
            f"- **Units Earned**: {latest['unitsEarned']} / **Regressed**: {latest['unitsRegressed']}",
            f"- **Overall Pipeline Compile Rate**: {latest['completionPct']:.2f}%",
            f"- **MIPS Byte Decompilation Score**: {latest['byteCompletionPct'] * 100.0:.4f}% ({latest['matchedBytes']:,} / {latest['totalBytes']:,} bytes)",
            f"- **Synthetic Framebuffer-Memory Parity Pass Rate**: {latest['vrPassRate']:.2f}% ({latest['vrPassedFrames']} / {latest['vrTotalFrames']} frames)"
        ]
        if latencies and latencies[-1] > 0:
            md.append(f"- **Average Hot-Path Latency**: {latencies[-1]:.3f} μs / call")
        else:
            md.append("- **Average Hot-Path Latency**: N/A")
        md.append("")

        md.append("## Historical Runs Table")
        md.append("| Run ID | Timestamp | Compile Rate | Byte Match | VR Pass Rate | Avg Latency |")
        md.append("| :--- | :--- | :---: | :---: | :---: | :---: |")
        for r, lat in zip(runs, latencies):
            lat_str = f"{lat:.2f} μs" if lat > 0 else "N/A"
            # IDs are validated [A-Za-z0-9_-] by validate_run_row(); timestamps
            # may contain Markdown-significant characters, so they are escaped.
            ts_esc = r["timestamp"].replace("|", "\\|").replace("`", "\\`")
            md.append(f"| `{r['id'][:8]}` | {ts_esc} | {r['completionPct']:.1f}% | {r['byteCompletionPct']*100.0:.3f}% | {r['vrPassRate']:.1f}% | {lat_str} |")
        md.append("")

        md.append("## Optimization Progress Trends")
        md.append("### Pipeline Compile Rate (%)")
        md.append("```text\n" + chart_compile + "\n```\n")
        md.append("### MIPS Decompilation Byte-Matching Score (%)\n```text\n" + chart_bytes + "\n```\n")
        md.append("### Synthetic Framebuffer-Memory Parity Pass Rate (%)\n```text\n" + chart_vr + "\n```\n")
        md.append("### Average Basic-Block Execution Latency (μs/call)\n```text\n" + chart_latency + "\n```\n")

        atomic_write_text(output_path, "\n".join(md))
        print(f"Benchmark report generated successfully at: {output_path}")

    # 2. Output HTML if specified
    if html_path:
        generate_html_report(runs, latencies, html_path)
        print(f"Interactive HTML dashboard generated successfully at: {html_path}")

    # 3. Output PDF if specified
    if pdf_path:
        generate_pdf_report(runs, latencies, pdf_path)
        print(f"High-Fidelity PDF datasheet generated successfully at: {pdf_path}")

    return True

def main():
    parser = argparse.ArgumentParser(description="Nakagawa Recomp Telemetry Benchmark Generator")
    parser.add_argument("--db", help="Path to dev.db file")
    parser.add_argument("--output", help="Path to write the Markdown report")
    parser.add_argument("--html", help="Path to write the HTML report")
    parser.add_argument("--pdf", help="Path to write the PDF report")
    parser.add_argument("--sync", action="store_true", help="Sync/import data from progress.json into DB before generation")
    parser.add_argument("--limit", type=int, default=10, help="Max runs to chart")
    args = parser.parse_args()

    repo_root = find_repo_root()
    db_path = args.db if args.db else os.path.join(repo_root, "interface", "prisma", "dev.db")
    output_path = args.output
    progress_path = os.path.join(repo_root, "progress.json")

    # If output_path is not specified and no format is provided, default to Markdown benchmarks_report.md
    if not output_path and not args.html and not args.pdf:
        output_path = os.path.join(repo_root, "benchmarks_report.md")

    # Issue #189: report generation is STRICTLY read-only.  Syncing/importing
    # historical rows is an explicit --sync maintenance action only; a missing
    # DB is reported, never auto-created as an implicit export side effect.
    if args.sync:
        print("Syncing database from progress.json historical log...")
        synced = sync_database_from_progress(db_path, progress_path)
        print(f"Synced {synced} historical telemetry run records.")

    success = generate_report(db_path, output_path, html_path=args.html, pdf_path=args.pdf, limit=args.limit)
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
