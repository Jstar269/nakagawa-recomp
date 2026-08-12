import { spawn } from "node:child_process";
import { NextRequest, NextResponse } from "next/server";
import { findRepoRoot, readProgressJson } from "@/lib/recompiler/runner";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";

export const runtime = "nodejs";

// GET /api/recompiler/progress → { total, earned, regressed, percent, phases, ... }
export async function GET() {
  try {
    const repoRoot = findRepoRoot();
    const snap = readProgressJson(repoRoot);
    if (!snap) {
      return NextResponse.json({ error: "progress-missing", detail: `no progress.json at repo root (${repoRoot})` }, { status: 404 });
    }
    return NextResponse.json({ ...snap });
  } catch (e) {
    return NextResponse.json({ error: "progress-read-failed", detail: String(e) }, { status: 500 });
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
    const child = spawn("python", ["tools/progress_tracker.py", action], { cwd: repoRoot });
    let stdout = "";
    let stderr = "";
    child.stdout?.on("data", (b: Buffer) => {
      stdout += b.toString("utf8");
    });
    child.stderr?.on("data", (b: Buffer) => {
      stderr += b.toString("utf8");
    });
    await new Promise<void>((res) => {
      child.on("exit", () => res());
      child.on("error", () => res());
    });
    return NextResponse.json({ action, stdout, stderr, ok: child.exitCode === 0 });
  } catch (e) {
    return NextResponse.json({ error: "progress-action-failed", detail: String(e) }, { status: 500 });
  }
}
