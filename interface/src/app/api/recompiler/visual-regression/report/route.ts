import { NextRequest, NextResponse } from "next/server";
import { readFileSync, existsSync } from "node:fs";
import { findRepoRoot } from "@/lib/recompiler/runner";
import path from "node:path";

export const runtime = "nodejs";

export async function GET(req: NextRequest) {
  try {
    const repoRoot = findRepoRoot();
    const p = path.join(repoRoot, "visual_regression_report.json");
    if (!existsSync(p)) {
      return NextResponse.json({
        timestamp: Date.now() / 1000,
        threshold: 3,
        summary: { total_frames: 0, passed_frames: 0, failed_frames: 0, pass_rate: 0.0 },
        frames: []
      });
    }
    const reportStr = readFileSync(p, "utf8");
    const report = JSON.parse(reportStr);
    return NextResponse.json(report);
  } catch (e) {
    return NextResponse.json({ error: "report-read-failed", detail: String(e) }, { status: 500 });
  }
}
