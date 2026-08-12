import { NextRequest, NextResponse } from "next/server";
import {
  getProfileById,
  getProfileMeta,
  activateProfile,
  renameProfile,
  deleteProfile,
  ProfileStoreError,
} from "@/lib/recompiler/profile-store";

export const runtime = "nodejs";

interface Params {
  params: Promise<{ id: string }>;
}

function errorResponse(e: unknown) {
  if (e instanceof ProfileStoreError) {
    return NextResponse.json({ error: e.code, message: e.message, fields: e.fields }, { status: e.status });
  }
  return NextResponse.json({ error: "db-error", message: String(e) }, { status: 500 });
}

// GET /api/recompiler/profiles/[id] -> load a specific profile's full config.
// A corrupt or unsupported-version stored row is reported explicitly (#188).
export async function GET(_req: NextRequest, { params }: Params) {
  const { id } = await params;
  try {
    const detail = await getProfileById(id);
    return NextResponse.json({
      id: detail.row.id,
      name: detail.row.name,
      isDefault: detail.row.isDefault,
      schemaVersion: detail.row.schemaVersion,
      config: detail.config,
      updatedAt: detail.row.updatedAt,
      status: "ok",
    });
  } catch (e) {
    return errorResponse(e);
  }
}

// PATCH /api/recompiler/profiles/[id] -> rename and/or activate (set as default).
// Body: { name?, activate?: boolean }
// Activation runs inside a single transaction (deactivate-all + activate-one)
// so concurrent activations cannot leave multiple defaults (#188).
export async function PATCH(req: NextRequest, { params }: Params) {
  const { id } = await params;
  let body: { name?: unknown; activate?: boolean };
  try {
    body = await req.json();
  } catch {
    return NextResponse.json({ error: "invalid-json", message: "Invalid JSON body" }, { status: 400 });
  }
  try {
    if (body.activate === true) {
      const row = await activateProfile(id);
      return NextResponse.json({
        id: row.id,
        name: row.name,
        isDefault: row.isDefault,
        updatedAt: row.updatedAt,
      });
    }
    if (body.activate !== undefined && typeof body.activate !== "boolean") {
      return NextResponse.json({ error: "invalid-activate", message: "activate must be a boolean" }, { status: 400 });
    }
    if (body.name !== undefined) {
      const row = await renameProfile(id, body.name);
      return NextResponse.json({
        id: row.id,
        name: row.name,
        isDefault: row.isDefault,
        updatedAt: row.updatedAt,
      });
    }
    // No-op PATCH (no name, no activation): return the row metadata WITHOUT
    // validating the stored config, so corrupt profiles can still be managed.
    const row = await getProfileMeta(id);
    return NextResponse.json({
      id: row.id,
      name: row.name,
      isDefault: row.isDefault,
      updatedAt: row.updatedAt,
    });
  } catch (e) {
    return errorResponse(e);
  }
}

// DELETE /api/recompiler/profiles/[id] -> delete a profile (refuse if it's the
// only one left or if it's currently active). All checks and the delete run in
// one transaction (#188).
export async function DELETE(_req: NextRequest, { params }: Params) {
  const { id } = await params;
  try {
    await deleteProfile(id);
    return NextResponse.json({ ok: true });
  } catch (e) {
    return errorResponse(e);
  }
}
