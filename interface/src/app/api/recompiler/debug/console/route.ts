import { NextRequest, NextResponse } from "next/server";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { findRepoRoot } from "@/lib/recompiler/runner";
import path from "node:path";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";
import {
  DEBUG_CONSOLE_ACTIONS,
  parseDebugConsoleRequest,
} from "@/lib/recompiler/debug-console-contract.mjs";

export const runtime = "nodejs";

const execFileAsync = promisify(execFile);
const LIVE_CONTROL_ENABLED = process.env.HST_DASHBOARD_LIVE_CONTROL === "1";

export async function POST(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req, { mutating: true });
  if (rejection) return rejection;

  let command;
  try {
    const body = await req.json().catch(() => ({}));
    command = parseDebugConsoleRequest(body);
  } catch (error) {
    return NextResponse.json(
      {
        error: "invalid-debug-console-request",
        detail: String(error),
        supported: DEBUG_CONSOLE_ACTIONS,
      },
      { status: 400 },
    );
  }

  if (command.mutating && !LIVE_CONTROL_ENABLED) {
    return NextResponse.json(
      {
        error: "live-control-disabled",
        detail: "Set HST_DASHBOARD_LIVE_CONTROL=1 before starting the dashboard to enable process mutations.",
      },
      { status: 403 },
    );
  }

  try {
    const repoRoot = findRepoRoot();
    const scriptPath = path.join(repoRoot, "tools", "mem_debug.py");
    const pythonCmd = process.platform === "win32" ? "python" : "python3";
    // mem_debug.py is read-only by default (#180): mutating actions require the
    // explicit --mutate flag, which the dashboard only forwards when live
    // control is enabled (already gated above by HST_DASHBOARD_LIVE_CONTROL).
    const toolArgs = command.mutating && LIVE_CONTROL_ENABLED ? ["--mutate"] : [];
    const childArgs = [scriptPath, ...toolArgs, command.action, ...command.args];

    const { stdout } = await execFileAsync(pythonCmd, childArgs, {
      cwd: repoRoot,
      encoding: "utf8",
      maxBuffer: 4 * 1024 * 1024,
      timeout: 10_000,
      windowsHide: true,
    });

    const parsed: unknown = JSON.parse(stdout);
    if (command.action === "status" && parsed !== null && typeof parsed === "object" && !Array.isArray(parsed)) {
      return NextResponse.json({
        ...parsed,
        capabilities: { liveControl: LIVE_CONTROL_ENABLED },
      });
    }
    return NextResponse.json(parsed);
  } catch (e) {
    return NextResponse.json({ error: "command-execution-failed", detail: String(e) }, { status: 500 });
  }
}
