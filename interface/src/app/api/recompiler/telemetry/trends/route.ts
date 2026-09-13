import { NextRequest, NextResponse } from "next/server";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { existsSync, mkdtempSync, rmSync, readFileSync, statSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";
import { runSubprocess, SubprocessError } from "@/lib/recompiler/child-process";
import { routeError } from "@/lib/recompiler/error-response";

export const runtime = "nodejs";

// Issue #189 contract for the export surface:
// * The report is produced into an exclusive per-request temp directory
//   (mkdtemp) so concurrent requests can never collide on a shared filename
//   and a stale sibling can never be mistaken for a fresh report.
// * The child process has a hard timeout and its stdout/stderr are consumed
//   so a stalled generator cannot hang the route or wedge on a full pipe.
// * The generated file is read with a byte budget; a report larger than the
//   ceiling is treated as a failure, never streamed unbounded.
// * Client disconnect aborts the child; cleanup happens in all paths.
// * The generator itself is read-only (see tools/generate_benchmarks.py); this
//   route only triggers report generation and never mutates telemetry state.

const CHILD_TIMEOUT_MS = 30_000;
const MAX_REPORT_BYTES = 32 * 1024 * 1024;

export async function GET(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req, { mutating: true });
  if (rejection) return rejection;
  let tempDir: string | null = null;
  try {
    const url = new URL(req.url);
    const format = url.searchParams.get("format") || "html";

    if (format !== "pdf" && format !== "html") {
      return NextResponse.json({ error: "invalid-format", message: "format must be 'pdf' or 'html'" }, { status: 400 });
    }

    const repoRoot = findRepoRoot();
    const scriptPath = path.join(repoRoot, "tools", "generate_benchmarks.py");
    const dbPath = path.join(repoRoot, "interface", "prisma", "dev.db");

    // Exclusive per-request temp directory (mkdtemp) — never a predictable name.
    tempDir = mkdtempSync(path.join(os.tmpdir(), "hst-telemetry-"));
    const reportPath = path.join(tempDir, `report.${format}`);
    const pythonCmd = process.platform === "win32" ? "python" : "python3";

    const childArgs = [
      scriptPath,
      "--db", dbPath,
      "--limit", "15",
    ];

    if (format === "pdf") {
      childArgs.push("--pdf", reportPath);
    } else {
      childArgs.push("--html", reportPath);
    }

    const abortSignal = req.signal;

    await runSubprocess(pythonCmd, childArgs, {
      cwd: repoRoot,
      timeoutMs: CHILD_TIMEOUT_MS,
      signal: req.signal,
    });

    if (!existsSync(reportPath)) {
      return NextResponse.json({ error: "export-failed", message: "Generated file was not created" }, { status: 500 });
    }

    const stat = statSync(reportPath);
    if (stat.size > MAX_REPORT_BYTES) {
      return NextResponse.json({ error: "export-failed", message: "Generated report exceeds the byte budget" }, { status: 500 });
    }

    const fileBytes = readFileSync(reportPath);

    const contentType = format === "pdf" ? "application/pdf" : "text/html";
    const filename = `hst-telemetry-report.${format}`;

    return new Response(fileBytes, {
      headers: {
        "Content-Type": contentType,
        "Content-Disposition": `attachment; filename=${filename}`,
        "Content-Length": String(fileBytes.length),
      },
    });
  } catch (e) {
    if (e instanceof SubprocessError && e.aborted) {
      return NextResponse.json({ error: "aborted", message: "request cancelled" }, { status: 499 });
    }
    return routeError("export-failed", e, 500);
  } finally {
    if (tempDir) {
      try {
        rmSync(tempDir, { recursive: true, force: true });
      } catch {
        // Best-effort cleanup; the temp dir is exclusive and contained in tmp.
      }
    }
  }
}
