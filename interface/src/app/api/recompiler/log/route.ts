import { NextRequest, NextResponse } from "next/server";
import { findLatestRunLog, findRepoRoot, readLogSince, routeError } from "@/lib/recompiler/runner";

export const runtime = "nodejs";

// GET /api/recompiler/log → tail the latest stderr_run*.log
//   ?since=<bytes> → return content past N bytes (incremental polling)
export async function GET(req: NextRequest) {
  try {
    const repoRoot = findRepoRoot();
    const tail = findLatestRunLog(repoRoot);
    const sinceRaw = new URL(req.url).searchParams.get("since");
    const since = sinceRaw ? Number(sinceRaw) : 0;
    let lines = tail.lastLines;
    let advanced = 0;
    if (since > 0 && tail.path && since < tail.sizeBytes) {
      const result = readLogSince(tail.path, since);
      lines = result.lines;
      advanced = result.cursor;
    } else {
      advanced = tail.sizeBytes;
    }
    return NextResponse.json({
      found: tail.found,
      path: tail.path,
      sizeBytes: tail.sizeBytes,
      cursor: advanced,
      allLogs: tail.allLogs,
      lines,
    });
  } catch (e) {
    return routeError("log-read-failed", e, 500);
  }
}
