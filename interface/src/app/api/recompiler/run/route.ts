import { NextRequest, NextResponse } from "next/server";
import { findRepoRoot, inspectHst } from "@/lib/recompiler/runner";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";

export const runtime = "nodejs";

// GET /api/recompiler/run → { repoRoot, hstExe, vulkanSdk, configs }
export async function GET(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req);
  if (rejection) return rejection;
  try {
    const repoRoot = findRepoRoot();
    const insp = inspectHst(repoRoot);
    return NextResponse.json({ repoRoot, ...insp });
  } catch (e) {
    return NextResponse.json({ error: "studio-not-anchored", detail: String(e) }, { status: 503 });
  }
}
