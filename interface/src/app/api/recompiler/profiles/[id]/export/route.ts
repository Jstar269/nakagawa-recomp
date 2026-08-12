import { NextRequest, NextResponse } from "next/server";
import { getProfileById, ProfileStoreError } from "@/lib/recompiler/profile-store";

export const runtime = "nodejs";

// GET /api/recompiler/profiles/[id]/export -> returns the profile as a
// downloadable .json file (so configs can be shared between machines).
// Corrupt / unsupported-version stored rows are refused, not exported (#188).
export async function GET(
  _req: NextRequest,
  { params }: { params: Promise<{ id: string }> },
) {
  const { id } = await params;
  try {
    const detail = await getProfileById(id);
    const payload = {
      format: "hst-profile",
      version: 1,
      schemaVersion: detail.row.schemaVersion,
      name: detail.row.name,
      exportedAt: new Date().toISOString(),
      config: detail.config,
    };
    const body = JSON.stringify(payload, null, 2);
    const safeName = detail.row.name.replace(/[^a-z0-9-_]+/gi, "_").slice(0, 40) || "profile";
    return new NextResponse(body, {
      status: 200,
      headers: {
        "Content-Type": "application/json",
        "Content-Disposition": `attachment; filename="${safeName}.hst.json"`,
      },
    });
  } catch (e) {
    if (e instanceof ProfileStoreError) {
      return NextResponse.json({ error: e.code, message: e.message, fields: e.fields }, { status: e.status });
    }
    return NextResponse.json({ error: "db-error", message: String(e) }, { status: 500 });
  }
}
