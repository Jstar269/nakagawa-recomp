import { NextRequest, NextResponse } from "next/server";
import { db } from "@/lib/db";
import { logTelemetry } from "@/lib/recompiler/telemetry";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";

export const runtime = "nodejs";

// Issue #189: the telemetry list is bounded.  Only the most-recent window is
// fetched so a large dev.db can never be serialized whole into the response;
// the dashboard chart/table needs at most the recent window.
const MAX_TELEMETRY_RUNS = 200;

// GET /api/recompiler/telemetry -> Get chronological list of telemetry runs
export async function GET() {
  try {
    const runs = await db.telemetryRun.findMany({
      orderBy: { timestamp: "desc" },
      take: MAX_TELEMETRY_RUNS,
      // Omit heavy payloads the panels never consume; they remain in the DB.
      select: {
        id: true,
        timestamp: true,
        totalUnits: true,
        unitsEarned: true,
        unitsRegressed: true,
        completionPct: true,
        totalFunctions: true,
        matchedFunctions: true,
        totalBytes: true,
        matchedBytes: true,
        byteCompletionPct: true,
        svMismatchesCount: true,
        svMismatchesJson: true,
        fuzzTotalTrials: true,
        fuzzPassedTrials: true,
        fuzzFailedTrials: true,
        fuzzCoveragePct: true,
        vrTotalFrames: true,
        vrPassedFrames: true,
        vrFailedFrames: true,
        vrPassRate: true,
      },
    });
    const chronological = [...runs].reverse();
    return NextResponse.json({ telemetry: chronological });
  } catch (e) {
    return NextResponse.json({ error: "db-error", detail: String(e) }, { status: 500 });
  }
}

// POST /api/recompiler/telemetry -> Parse progress.json and save to SQLite.
// This is the explicit mutation path: it writes rows to dev.db, so it is gated
// to local control requests exactly like the other mutating routes (#189).
export async function POST(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req, { mutating: true });
  if (rejection) return rejection;
  try {
    const currentRun = await logTelemetry();
    if (!currentRun) {
      return NextResponse.json({ error: "telemetry-log-failed" }, { status: 500 });
    }
    return NextResponse.json({
      success: true,
      currentRun,
    });
  } catch (e) {
    return NextResponse.json({ error: "telemetry-log-failed", detail: String(e) }, { status: 500 });
  }
}
