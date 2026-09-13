import { NextRequest, NextResponse } from "next/server";
import { findRepoRoot, readProgressJson } from "@/lib/recompiler/runner";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";
import { runSubprocess } from "@/lib/recompiler/child-process";
import { routeError } from "@/lib/recompiler/error-response";

export const runtime = "nodejs";

// GET /api/recompiler/progress → { total, earned, regressed, percent, phases, ... }
export async function GET() {
  try {
    const repoRoot = findRepoRoot();
    const snap = readProgressJson(repoRoot);
    if (!snap) {
      return NextResponse.json({ error: "progress-missing" }, { status: 404 });
    }
    return NextResponse.json({ ...snap });
  } catch (e) {
    return routeError("progress-read-failed", e, 500);
  }
}

// POST /api/recompiler/progress { action: "verify" | "show" } → shells to progress_tracker.py
export async function POST(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req, { mutating: true });
  if (rejection) return rejection;
  const body = await req.json().catch(() => ({}));
  const action = body?.action;
  if (action !== "verify" && action !== "show") {
    return NextResponse.json({ error: "unsupported action", supported: ["verify", "show"] }, { status: 400 });
  }
  try {
    const repoRoot = findRepoRoot();
    const pythonCmd = process.platform === "win32" ? "python" : "python3";
    const result = await runSubprocess(pythonCmd, ["tools/progress_tracker.py", action], {
      cwd: repoRoot,
      timeoutMs: 30_000,
      maxBuffer: 4 * 1024 * 1024,
      signal: req.signal,
      allowNonZeroExit: true,
    });
    return NextResponse.json({ action, stdout: result.stdout, stderr: result.stderr, ok: result.exitCode === 0 });
  } catch (e) {
    return routeError("progress-action-failed", e, 500);
  }
}

