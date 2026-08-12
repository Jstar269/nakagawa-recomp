import { NextResponse } from "next/server";
import { findLatestRunLog, findRepoRoot, readLogPrefix } from "@/lib/recompiler/runner";

export const runtime = "nodejs";

const expected = ["image_loaded", "runtime_registered", "window_ready", "guest_start", "display_flip", "first_frame"] as const;

export async function GET() {
  try {
    const latest = findLatestRunLog(findRepoRoot());
    if (!latest.found || !latest.path) {
      return NextResponse.json({ ok: false, status: "not-run", events: [], reached: {} });
    }
    // Issue #186: scan only a bounded prefix (boot events are emitted at
    // startup, i.e. the file head); a large/corrupt log is never loaded whole.
    const { content, truncated } = readLogPrefix(latest.path);
    const events = content.split(/\r?\n/).flatMap((line, index) => {
      const marker = line.match(/\bBOOT_EVENT\s+(.+)$/);
      if (!marker) return [];
      const event: Record<string, string | number> = { line: index + 1 };
      for (const pair of marker[1].matchAll(/([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|\S+)/g)) {
        event[pair[1]] = pair[2].replace(/^"|"$/g, "");
      }
      return [event];
    });
    const phases = events.map((event) => String(event.phase ?? "unknown"));
    const reached = Object.fromEntries(expected.map((phase) => [phase, phases.includes(phase)]));
    const frame = events.find((event) => event.phase === "first_frame");
    const nonzeroPixels = Number(frame?.nonzero_pixels ?? 0);
    const stalled = phases.includes("stalled");
    const presented = reached.display_flip;
    const malformedHostPaths = (content.match(/Open\(host0:(?:\)|[^\r\n]*�)/g) ?? []).length;
    return NextResponse.json({
      // The Vulkan path reports display_flip but does not pass through gui.c's
      // software-frame pixel counter. A later no-frame watchdog event can mean
      // an event-driven static menu, so it must not erase a proven presentation.
      ok: Boolean(presented),
      status: presented ? (stalled ? "idle-after-flip" : "presenting") : stalled ? "stalled" : phases.length ? "starting" : "unknown",
      lastPhase: phases.at(-1) ?? "not-started",
      reached,
      nonzeroPixels,
      malformedHostPaths,
      events,
      logPath: latest.path,
      sizeBytes: latest.sizeBytes,
      scanTruncated: truncated,
    });
  } catch (error) {
    return NextResponse.json({ error: "boot-status-failed", detail: String(error) }, { status: 500 });
  }
}
