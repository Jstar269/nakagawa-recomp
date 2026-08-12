import { NextResponse } from "next/server";

export async function GET() {
  return NextResponse.json({
    service: "Nakagawa Recomp dashboard",
    status: "ok",
    scope: "local-workspace",
  });
}
