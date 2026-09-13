import { NextResponse } from "next/server";

/**
 * Standardized error responder for dashboard API routes:
 * - Logs the full error to the server console (console.error) for diagnosis.
 * - Returns a fixed coded JSON error response to the client with appropriate HTTP status.
 * - Guarantees that internal exception strings, stack traces, and local host paths are not leaked to API clients.
 */
export function routeError(
  code: string,
  err: unknown,
  status: number = 500,
  extra?: Record<string, unknown>,
): NextResponse {
  if (err !== undefined) {
    console.error(`[API Error: ${code}]`, err);
  }
  return NextResponse.json(
    {
      error: code,
      ...extra,
    },
    { status },
  );
}
