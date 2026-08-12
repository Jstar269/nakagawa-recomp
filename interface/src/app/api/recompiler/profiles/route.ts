import { NextRequest, NextResponse } from "next/server";
import { listProfiles, createProfile, ProfileStoreError } from "@/lib/recompiler/profile-store";

export const runtime = "nodejs";

// GET /api/recompiler/profiles -> list all saved profiles, active first.
// Corrupt rows are returned in a separate `corrupt` array so one malformed row
// cannot take the whole list endpoint down or crash the UI (#188).
export async function GET() {
  try {
    const { profiles, corrupt } = await listProfiles();
    return NextResponse.json({ profiles, corrupt });
  } catch (e) {
    if (e instanceof ProfileStoreError) {
      return NextResponse.json({ error: e.code, message: e.message, fields: e.fields }, { status: e.status });
    }
    return NextResponse.json({ error: "db-error", message: String(e) }, { status: 500 });
  }
}

// POST /api/recompiler/profiles -> create a new named profile (or duplicate).
// Body: { name, config?, duplicateFrom? }
// A missing or corrupt duplicate source is a hard error, not a silent fallback.
export async function POST(req: NextRequest) {
  let body: { name?: unknown; config?: unknown; duplicateFrom?: string };
  try {
    body = await req.json();
  } catch {
    return NextResponse.json({ error: "invalid-json", message: "Invalid JSON body" }, { status: 400 });
  }
  try {
    const { row, config } = await createProfile(body);
    return NextResponse.json(
      {
        id: row.id,
        name: row.name,
        config,
        isDefault: false,
        schemaVersion: row.schemaVersion,
        updatedAt: row.updatedAt,
      },
      { status: 201 },
    );
  } catch (e) {
    if (e instanceof ProfileStoreError) {
      return NextResponse.json({ error: e.code, message: e.message, fields: e.fields }, { status: e.status });
    }
    return NextResponse.json({ error: "failed-to-create", message: String(e) }, { status: 500 });
  }
}
