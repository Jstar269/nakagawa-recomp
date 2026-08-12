import { NextRequest, NextResponse } from "next/server";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { parsePerfProfiler } from "@/lib/recompiler/telemetry";
import { db } from "@/lib/db";

export const runtime = "nodejs";

export async function GET(req: NextRequest) {
  try {
    const repoRoot = findRepoRoot();
    const live = parsePerfProfiler(repoRoot);

    // Fetch the 10 most recent telemetry runs to plot trends
    const runs = await db.telemetryRun.findMany({
      orderBy: { timestamp: "desc" },
      take: 10,
    });

    const reversedRuns = [...runs].reverse();
    const trend: any[] = [];

    // Extract optimization trend data
    reversedRuns.forEach((run, index) => {
      if (run.rawJson) {
        try {
          const parsed = JSON.parse(run.rawJson);
          if (parsed.perfProfile && parsed.perfProfile.functions) {
            const topFns = parsed.perfProfile.functions.slice(0, 5);
            const dataPoint: any = {
              build: `B${run.id.substring(0, 4)}`,
              timestamp: run.timestamp,
            };
            topFns.forEach((fn: any) => {
              dataPoint[fn.pc] = Math.round(fn.durationNs / 1000000 * 100) / 100; // ms
            });
            trend.push(dataPoint);
          }
        } catch (e) {
          // Ignore parse errors on individual runs
        }
      }
    });

    return NextResponse.json({
      live,
      trend,
    });
  } catch (e) {
    return NextResponse.json({ error: "Failed to load performance telemetry", detail: String(e) }, { status: 500 });
  }
}
