import { NextRequest, NextResponse } from "next/server";
import { readFileSync, statSync } from "node:fs";
import { findLatestRunLog, findRepoRoot } from "@/lib/recompiler/runner";

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
      const buf = readFileSync(tail.path);
      const slice = buf.subarray(since).toString("utf8");
      lines = slice.split(/\r?\n/).filter(Boolean);
      advanced = since + buf.subarray(since).length;
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
    return NextResponse.json({ error: "log-read-failed", detail: String(e) }, { status: 500 });
  }
}
