// SPDX-License-Identifier: GPL-3.0-or-later
import { NextResponse } from "next/server";
import { findRepoRoot, routeError } from "@/lib/recompiler/runner";
import { parsePerfProfiler } from "@/lib/recompiler/telemetry";
import { db } from "@/lib/db";

export const runtime = "nodejs";

export async function GET() {
  try {
    const repoRoot = findRepoRoot();
    const live = parsePerfProfiler(repoRoot);

    // Fetch the 10 most recent telemetry runs to plot trends
    const runs = await db.telemetryRun.findMany({
      orderBy: { timestamp: "desc" },
      take: 10,
    });

    const reversedRuns = [...runs].reverse();
    const trend: Record<string, string | number | Date>[] = [];

    // Extract optimization trend data
    reversedRuns.forEach((run) => {
      if (run.rawJson) {
        try {
          const parsed = JSON.parse(run.rawJson);
          if (parsed.perfProfile && parsed.perfProfile.functions) {
            const topFns = parsed.perfProfile.functions.slice(0, 5);
            const dataPoint: Record<string, string | number | Date> = {
              build: `B${run.id.substring(0, 4)}`,
              timestamp: run.timestamp,
            };
            topFns.forEach((fn: { pc: string; durationNs: number }) => {
              dataPoint[fn.pc] = Math.round(fn.durationNs / 1000000 * 100) / 100; // ms
            });
            trend.push(dataPoint);
          }
        } catch {
          // Ignore parse errors on individual runs
        }
      }
    });

    return NextResponse.json({
      live,
      trend,
    });
  } catch (e) {
    return routeError("Failed to load performance telemetry", e, 500);
  }
}
