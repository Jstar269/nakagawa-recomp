// SPDX-License-Identifier: GPL-3.0-or-later
import { NextRequest, NextResponse } from "next/server";
import { findRepoRoot, routeError, runSubprocess } from "@/lib/recompiler/runner";
import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import path from "node:path";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";

export const runtime = "nodejs";

/** Parse a canonical <game>_imports.toml into its [[import]] entries. */
function parseImportsToml(text: string): Array<{ nid: string; lib: string; stub: string }> {
  const entries: Array<{ nid: string; lib: string; stub: string }> = [];
  let current: Record<string, string> | null = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (line.startsWith("[[import]]")) {
      if (current) entries.push(current as { nid: string; lib: string; stub: string });
      current = {};
    } else if (current && line.includes("=")) {
      const eq = line.indexOf("=");
      const key = line.slice(0, eq).trim();
      const value = line.slice(eq + 1).trim().replace(/^"|"$/g, "");
      current[key] = value;
    }
  }
  if (current) entries.push(current as { nid: string; lib: string; stub: string });
  return entries;
}

/**
 * NID coverage from the AUTHORITATIVE hle_manifest.py classifications, joined with the
 * actual imported NIDs. Manifest statuses are preserved (unreviewed / partial /
 * compatibility / stub / controlled_unsupported / complete) instead of being collapsed
 * into a single "resolved" bucket, so a dedicated-but-unreviewed handler never reads as
 * implementation coverage (#181).
 */

/** Newest build/<game>/<game>_imports.toml, or "" when none exists yet. */
function findLatestImportsToml(repoRoot: string): string {
  const buildDir = path.join(repoRoot, "build");
  try {
    const found: { file: string; mtime: number }[] = [];
    for (const entry of readdirSync(buildDir, { withFileTypes: true })) {
      if (!entry.isDirectory()) continue;
      const candidate = path.join(buildDir, entry.name, `${entry.name}_imports.toml`);
      try {
        const st = statSync(candidate);
        if (st.isFile()) found.push({ file: candidate, mtime: st.mtimeMs });
      } catch {
        // Title directory without an imports file; ignore.
      }
    }
    found.sort((left, right) => right.mtime - left.mtime);
    if (found.length > 0) return found[0].file;
  } catch {
    // No build directory yet.
  }
  return "";
}

export async function GET(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req, { mutating: true });
  if (rejection) return rejection;
  try {
    const repoRoot = findRepoRoot();
    const manifestScript = path.join(repoRoot, "tools", "hle_manifest.py");
    const manifestPath = path.join(repoRoot, "build", "hle_manifest.json");
    // The pipeline writes build/<game>/<game>_imports.toml (Makefile convention);
    // audit the most recently generated one instead of assuming a title.
    const importsPath = findLatestImportsToml(repoRoot);
    const pythonCmd = process.platform === "win32" ? "python" : "python3";

    await runSubprocess(pythonCmd, [manifestScript, "--out", manifestPath], {
      cwd: repoRoot,
      timeoutMs: 30_000,
      maxBuffer: 4 * 1024 * 1024,
      signal: req.signal,
    });

    if (!existsSync(manifestPath) || !existsSync(importsPath)) {
      return NextResponse.json(
        { error: "missing-inputs", detail: `manifest=${existsSync(manifestPath)} imports=${existsSync(importsPath)}` },
        { status: 500 }
      );
    }

    const manifest = JSON.parse(readFileSync(manifestPath, "utf8")) as {
      registrations?: Array<{
        nid: string;
        name: string;
        handler: string;
        classification: string;
        status: string;
      }>;
    };
    const byNid = new Map<string, NonNullable<typeof manifest.registrations>[number]>();
    for (const reg of manifest.registrations ?? []) {
      const normalized = reg.nid.toLowerCase();
      if (!byNid.has(normalized)) byNid.set(normalized, reg);
    }

    const imports = parseImportsToml(readFileSync(importsPath, "utf8"));
    const summary = {
      total_imports: imports.length,
      resolved: 0,
      stubbed: 0,
      unmapped: 0,
      unreviewed: 0,
      partial: 0,
      compatibility: 0,
      controlled_unsupported: 0,
      coverage_pct: 0,
      implemented_pct: 0,
    };
    const moduleStats = new Map<string, Record<string, number>>();
    const nids: unknown[] = [];

    for (const imp of imports) {
      const nidHex = imp.nid.toLowerCase();
      const reg = byNid.get(nidHex);
      let status: string;
      if (!reg) {
        status = "unmapped";
      } else if (reg.classification === "fake_success" || reg.status === "stub") {
        status = "stubbed";
      } else {
        status = reg.status; // complete | partial | compatibility | controlled_unsupported | unreviewed
      }
      if (status === "complete") summary.resolved += 1;
      else if (status === "stubbed") summary.stubbed += 1;
      else if (status === "unmapped") summary.unmapped += 1;
      else if (status === "unreviewed") summary.unreviewed += 1;
      else if (status === "partial") summary.partial += 1;
      else if (status === "compatibility") summary.compatibility += 1;
      else if (status === "controlled_unsupported") summary.controlled_unsupported += 1;

      const lib = imp.lib || "unknown";
      const stats = moduleStats.get(lib) ?? { total: 0, resolved: 0, stubbed: 0, unmapped: 0, unreviewed: 0, partial: 0, compatibility: 0, controlled_unsupported: 0 };
      stats.total += 1;
      if (status in stats) stats[status as keyof typeof stats] += 1;
      moduleStats.set(lib, stats);

      nids.push({
        nid_hex: imp.nid,
        lib,
        stub: imp.stub,
        status,
        name: reg?.name ?? `NID_0x${nidHex.replace("0x", "")}`,
        handler: reg?.handler ?? null,
        classification: reg?.classification ?? null,
        manifest_status: reg?.status ?? null,
        evidence: reg
          ? "hle-manifest-classified"
          : "no-registration",
      });
    }

    if (summary.total_imports > 0) {
      summary.coverage_pct = Math.round((summary.resolved / summary.total_imports) * 10000) / 100;
      summary.implemented_pct = Math.round(((summary.resolved + summary.stubbed) / summary.total_imports) * 10000) / 100;
    }

    return NextResponse.json({
      source: `tools/hle_manifest.py + ${path.relative(repoRoot, importsPath).replace(/\\/g, "/")}`,
      summary,
      status_breakdown: {
        complete: summary.resolved,
        stubbed: summary.stubbed,
        unmapped: summary.unmapped,
        unreviewed: summary.unreviewed,
        partial: summary.partial,
        compatibility: summary.compatibility,
        controlled_unsupported: summary.controlled_unsupported,
      },
      modules: [...moduleStats.entries()].map(([name, stats]) => ({ name, ...stats })),
      nids,
    });
  } catch (e) {
    return routeError("audit-failed", e, 500);
  }
}
