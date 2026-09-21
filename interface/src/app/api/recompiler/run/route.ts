// SPDX-License-Identifier: GPL-3.0-or-later
import { NextRequest, NextResponse } from "next/server";
import { findRepoRoot, inspectBinary, routeError } from "@/lib/recompiler/runner";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";

export const runtime = "nodejs";

// GET /api/recompiler/run → { repoRoot, exePath, vulkanSdk, configs }
export async function GET(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req);
  if (rejection) return rejection;
  try {
    const repoRoot = findRepoRoot();
    const insp = inspectBinary(repoRoot);
    return NextResponse.json({ repoRoot, ...insp });
  } catch (e) {
    return routeError("studio-not-anchored", e, 503);
  }
}
