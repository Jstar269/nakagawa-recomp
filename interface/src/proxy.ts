import { NextRequest, NextResponse } from "next/server";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";

export function proxy(request: NextRequest) {
  const mutating = !new Set(["GET", "HEAD", "OPTIONS"]).has(request.method);
  return rejectNonLocalControlRequest(request, { mutating }) ?? NextResponse.next();
}

export const config = {
  matcher: "/api/recompiler/:path*",
};
