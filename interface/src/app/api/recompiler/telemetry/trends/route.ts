import { NextRequest, NextResponse } from "next/server";
import { spawn } from "node:child_process";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { existsSync, mkdtempSync, rmSync, readFileSync, statSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";

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

    await new Promise<void>((resolve, reject) => {
      const child = spawn(pythonCmd, childArgs, { cwd: repoRoot });
      let settled = false;

      // Drain child output so the pipe can never fill and deadlock the child.
      child.stdout?.on("data", () => {});
      child.stderr?.on("data", () => {});

      const timeout = setTimeout(() => {
        if (!settled) {
          settled = true;
          child.kill();
          reject(new Error("generate_benchmarks.py timed out"));
        }
      }, CHILD_TIMEOUT_MS);

      const abortHandler = () => {
        if (!settled) {
          settled = true;
          clearTimeout(timeout);
          child.kill();
          reject(new Error("request aborted"));
        }
      };
      abortSignal.addEventListener("abort", abortHandler, { once: true });

      child.on("close", (code) => {
        if (settled) return;
        settled = true;
        clearTimeout(timeout);
        abortSignal.removeEventListener("abort", abortHandler);
        if (code === 0) resolve();
        else reject(new Error(`generate_benchmarks.py exited with code ${code}`));
      });
      child.on("error", (err) => {
        if (settled) return;
        settled = true;
        clearTimeout(timeout);
        abortSignal.removeEventListener("abort", abortHandler);
        reject(err);
      });
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
    if (e instanceof Error && e.message === "request aborted") {
      return NextResponse.json({ error: "aborted", message: "request cancelled" }, { status: 499 });
    }
    return NextResponse.json({ error: "export-failed", detail: String(e) }, { status: 500 });
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
