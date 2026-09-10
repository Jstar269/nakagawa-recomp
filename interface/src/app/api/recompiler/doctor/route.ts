import { NextRequest, NextResponse } from "next/server";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { classifyDoctorFailure, parseDoctorScope, runDoctor } from "@/lib/recompiler/doctor";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";

export const runtime = "nodejs";

// GET /api/recompiler/doctor?scope=(repo|inputs|build|products|run|all)&strict=(true|false)
export async function GET(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req);
  if (rejection) return rejection;

  const url = new URL(req.url);
  const rawScope = url.searchParams.get("scope");
  const rawStrict = url.searchParams.get("strict");

  let scope;
  try {
    scope = parseDoctorScope(rawScope ?? undefined);
  } catch (error) {
    return NextResponse.json({ error: "invalid-doctor-scope", detail: String(error) }, { status: 400 });
  }

  const strict = rawStrict === "true" || rawStrict === "1";

  try {
    const repoRoot = findRepoRoot();
    const report = await runDoctor(repoRoot, { scope, strict });
    return NextResponse.json(report, {
      headers: { "Cache-Control": "no-store" },
    });
  } catch (error) {
    // Classify route-level failures (the diagnostic never produced a report)
    // so clients can show plain-language remediation. The raw diagnostic is
    // preserved verbatim for a developer-facing detail view.
    const rawDetail = String(error);
    const classified = classifyDoctorFailure(rawDetail);
    return NextResponse.json(
      {
        error: "doctor-failed",
        reason: classified.reason,
        title: classified.title,
        explanation: classified.explanation,
        nextAction: classified.nextAction,
        detail: rawDetail,
      },
      { status: 500 },
    );
  }
}
