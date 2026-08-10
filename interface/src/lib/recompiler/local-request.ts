import { NextRequest, NextResponse } from "next/server";

const LOCAL_HOSTS = new Set(["localhost", "127.0.0.1", "::1"]);

function normalizeHostname(hostname: string): string {
  const lower = hostname.toLowerCase();
  return lower.startsWith("[") && lower.endsWith("]") ? lower.slice(1, -1) : lower;
}

export function isLocalAuthority(authority: string | null): boolean {
  if (!authority) return false;
  if (authority.includes("@") || authority.includes("/") || authority.includes("\\")) return false;
  try {
    const hostname = normalizeHostname(new URL(`http://${authority}`).hostname);
    return LOCAL_HOSTS.has(hostname);
  } catch {
    return false;
  }
}

function isLocalOrigin(origin: string): boolean {
  try {
    const url = new URL(origin);
    return (
      !url.username &&
      !url.password &&
      (url.protocol === "http:" || url.protocol === "https:") &&
      LOCAL_HOSTS.has(normalizeHostname(url.hostname))
    );
  } catch {
    return false;
  }
}

export function rejectNonLocalControlRequest(
  request: NextRequest,
  options: { mutating?: boolean } = {},
): NextResponse | null {
  if (!isLocalAuthority(request.headers.get("host"))) {
    return NextResponse.json({ error: "local-host-required" }, { status: 403 });
  }

  if (request.headers.get("sec-fetch-site") === "cross-site") {
    return NextResponse.json({ error: "cross-site-request-rejected" }, { status: 403 });
  }

  if (options.mutating) {
    const origin = request.headers.get("origin");
    if (origin !== null && !isLocalOrigin(origin)) {
      return NextResponse.json({ error: "local-origin-required" }, { status: 403 });
    }
  }

  return null;
}

export function rejectUnsupportedProcessHost(): NextResponse | null {
  if (process.platform === "win32") return null;
  return NextResponse.json(
    { error: "windows-host-required", detail: "Local process control uses hst_manager.ps1 and hst.exe." },
    { status: 501 },
  );
}
