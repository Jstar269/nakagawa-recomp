import { NextRequest, NextResponse } from "next/server";
import { defaultConfig } from "@/lib/recompiler/defaults";
import { getActiveProfile, upsertActiveProfile, ProfileStoreError } from "@/lib/recompiler/profile-store";

export const runtime = "nodejs";

// GET /api/recompiler/config -> returns the active saved profile, or a freshly
// generated default if nothing has been persisted yet. A stored row that fails
// validation is reported explicitly (corrupt / unsupported-version) and is
// NEVER silently replaced with a default (#188).
export async function GET() {
  try {
    const detail = await getActiveProfile();
    if (!detail) {
      const config = defaultConfig("minimal");
      return NextResponse.json({ id: null, name: config.profileName, config, updatedAt: null, status: "none" });
    }
    return NextResponse.json({
      id: detail.row.id,
      name: detail.row.name,
      config: detail.config,
      schemaVersion: detail.row.schemaVersion,
      updatedAt: detail.row.updatedAt,
      status: "ok",
    });
  } catch (e) {
    if (e instanceof ProfileStoreError) {
      return NextResponse.json({ error: e.code, message: e.message, fields: e.fields }, { status: e.status });
    }
    return NextResponse.json({ error: "failed-to-load", message: String(e) }, { status: 500 });
  }
}

// POST /api/recompiler/config -> upsert the active profile. The request body
// config is schema-validated before persistence; the create-or-update happens
// inside one transaction so the one-default invariant holds under concurrency.
export async function POST(req: NextRequest) {
  let body: { config?: unknown; name?: unknown };
  try {
    body = await req.json();
  } catch {
    return NextResponse.json({ error: "invalid-json", message: "Invalid JSON body" }, { status: 400 });
  }
  try {
    const { row, config } = await upsertActiveProfile({ config: body.config, name: body.name });
    return NextResponse.json({
      id: row.id,
      name: row.name,
      config,
      schemaVersion: row.schemaVersion,
      updatedAt: row.updatedAt,
      status: "ok",
    });
  } catch (e) {
    if (e instanceof ProfileStoreError) {
      return NextResponse.json({ error: e.code, message: e.message, fields: e.fields }, { status: e.status });
    }
    return NextResponse.json({ error: "failed-to-save", message: String(e) }, { status: 500 });
  }
}
