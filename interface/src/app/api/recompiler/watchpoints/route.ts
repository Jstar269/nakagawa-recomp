import { NextRequest, NextResponse } from "next/server";
import {
  getWatchpoints,
  mutateWatchpoints,
  WatchpointStoreError,
} from "@/lib/recompiler/watchpoint-store";
import { normalizeWatchpoint, parseWatchAddress } from "@/lib/recompiler/watchpoint-schema.mjs";

export const runtime = "nodejs";

function errorResponse(e: unknown) {
  if (e instanceof WatchpointStoreError) {
    return NextResponse.json({ error: e.code, message: e.message }, { status: e.status });
  }
  return NextResponse.json({ error: "failed-to-load", message: String(e) }, { status: 500 });
}

function watchpointsPayload(result: { watchpoints: unknown[]; source: string; fileState: unknown; added?: unknown }) {
  return {
    success: true,
    watchpoints: result.watchpoints,
    source: result.source,
    fileState: result.fileState,
    ...(result.added !== undefined ? { added: result.added } : {}),
  };
}

// GET /api/recompiler/watchpoints
// Returns the currently effective watchpoints plus the derived-file state
// (synced / stale / corrupt / direct / missing). A stale artifact is reported,
// never silently regenerated.
export async function GET() {
  try {
    const result = await getWatchpoints();
    return NextResponse.json(watchpointsPayload(result));
  } catch (e) {
    return errorResponse(e);
  }
}

// POST /api/recompiler/watchpoints
// Registers a new watchpoint. Strict full-string numeric parsing, bounded
// ranges/span/labels, a shared 16-watchpoint limit, duplicate rejection, and a
// transactional read-modify-write (#188).
export async function POST(req: NextRequest) {
  let body: Record<string, unknown>;
  try {
    body = await req.json().catch(() => ({}));
  } catch {
    return NextResponse.json({ error: "invalid-json", message: "Invalid JSON body" }, { status: 400 });
  }
  const normalized = normalizeWatchpoint({
    start: body.start,
    end: body.end,
    label: body.label,
  });
  if (!normalized.ok) {
    return NextResponse.json({ error: "invalid-parameters", message: normalized.reason }, { status: 400 });
  }
  try {
    const result = await mutateWatchpoints("add", { watchpoint: normalized.value });
    return NextResponse.json(watchpointsPayload(result));
  } catch (e) {
    return errorResponse(e);
  }
}

// DELETE /api/recompiler/watchpoints
// Clears a specific watchpoint (by label or start address) or all of them.
export async function DELETE(req: NextRequest) {
  try {
    const url = new URL(req.url);
    const label = url.searchParams.get("label");
    const startStr = url.searchParams.get("start");

    if (label) {
      const result = await mutateWatchpoints("delete-label", { label });
      return NextResponse.json(watchpointsPayload(result));
    }
    if (startStr) {
      const parsed = parseWatchAddress(startStr);
      if (!parsed.ok) {
        return NextResponse.json({ error: "invalid-parameters", message: `start: ${parsed.reason}` }, { status: 400 });
      }
      const result = await mutateWatchpoints("delete-start", { start: parsed.value });
      return NextResponse.json(watchpointsPayload(result));
    }
    const result = await mutateWatchpoints("clear", {});
    return NextResponse.json(watchpointsPayload(result));
  } catch (e) {
    return errorResponse(e);
  }
}
