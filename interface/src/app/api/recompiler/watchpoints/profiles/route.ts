import { NextRequest, NextResponse } from "next/server";
import {
  listDebugProfiles,
  createDebugProfile,
  updateDebugProfile,
  deleteDebugProfile,
  WatchpointStoreError,
} from "@/lib/recompiler/watchpoint-store";

export const runtime = "nodejs";

function errorResponse(e: unknown) {
  if (e instanceof WatchpointStoreError) {
    return NextResponse.json({ error: e.code, message: e.message }, { status: e.status });
  }
  return NextResponse.json({ error: "failed", message: String(e) }, { status: 500 });
}

// GET /api/recompiler/watchpoints/profiles
// Lists all debug profiles. Corrupt / unsupported-version profiles are
// reported in a separate `corrupt` array so one bad row cannot break the list
// or crash the UI (#188).
export async function GET() {
  try {
    const { profiles, corrupt } = await listDebugProfiles();
    return NextResponse.json({ success: true, profiles, corrupt });
  } catch (e) {
    return errorResponse(e);
  }
}

// POST /api/recompiler/watchpoints/profiles
// Creates a new debug profile. Name, watchpoints JSON and debugMask are all
// validated before persistence; isActive is always false on create.
export async function POST(req: NextRequest) {
  let body: Record<string, unknown>;
  try {
    body = await req.json().catch(() => ({}));
  } catch {
    return NextResponse.json({ error: "invalid-json", message: "Invalid JSON body" }, { status: 400 });
  }
  try {
    const profile = await createDebugProfile({
      name: body.name,
      watchpoints: body.watchpoints,
      debugMask: body.debugMask ?? 0,
    });
    return NextResponse.json({ success: true, profile });
  } catch (e) {
    return errorResponse(e);
  }
}

// PUT /api/recompiler/watchpoints/profiles
// Updates a profile and/or activates it. `isActive` must be an actual JSON
// boolean; `debugMask` a bounded unsigned integer; activation is one atomic
// transaction (deactivate-all + activate-one). The derived file is republished
// and its state is reported — the API never claims the file was written when
// it was not (#188).
export async function PUT(req: NextRequest) {
  let body: Record<string, unknown>;
  try {
    body = await req.json().catch(() => ({}));
  } catch {
    return NextResponse.json({ error: "invalid-json", message: "Invalid JSON body" }, { status: 400 });
  }
  const id = body.id;
  if (typeof id !== "string" || id.length === 0) {
    return NextResponse.json({ error: "missing-id", message: "Profile ID is required" }, { status: 400 });
  }
  try {
    const { profile, fileState } = await updateDebugProfile({
      id,
      name: body.name,
      watchpoints: body.watchpoints,
      debugMask: body.debugMask,
      isActive: body.isActive,
    });
    return NextResponse.json({ success: true, profile, fileState });
  } catch (e) {
    return errorResponse(e);
  }
}

// DELETE /api/recompiler/watchpoints/profiles
// Deletes a debug profile (transactionally). If the deleted profile was the
// active one, the derived file is republished for the remaining active state.
export async function DELETE(req: NextRequest) {
  try {
    const url = new URL(req.url);
    const id = url.searchParams.get("id");
    if (!id) {
      return NextResponse.json({ error: "missing-id", message: "Profile ID is required in query params" }, { status: 400 });
    }
    const { fileState } = await deleteDebugProfile(id);
    return NextResponse.json({ success: true, message: "Profile deleted successfully", fileState });
  } catch (e) {
    return errorResponse(e);
  }
}
