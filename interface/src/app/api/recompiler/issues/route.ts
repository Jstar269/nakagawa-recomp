import { NextRequest, NextResponse } from "next/server";
import { existsSync, readFileSync, statSync } from "node:fs";
import path from "node:path";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";

export const runtime = "nodejs";

export async function GET(request: NextRequest) {
  const rejection = rejectNonLocalControlRequest(request);
  if (rejection) return rejection;

  try {
    const issuesPath = path.join(findRepoRoot(), "ISSUES.md");
    if (!existsSync(issuesPath)) {
      return NextResponse.json({ error: "issues-file-missing" }, { status: 404 });
    }
    return NextResponse.json(
      {
        source: "ISSUES.md",
        content: readFileSync(issuesPath, "utf8"),
        updatedAt: statSync(issuesPath).mtime.toISOString(),
      },
      { headers: { "Cache-Control": "no-store" } },
    );
  } catch (error) {
    return NextResponse.json({ error: "issues-read-failed", detail: String(error) }, { status: 500 });
  }
}
