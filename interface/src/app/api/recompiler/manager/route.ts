// SPDX-License-Identifier: GPL-3.0-or-later
import { routeError } from "@/lib/recompiler/runner";
import { NextRequest, NextResponse } from "next/server";
import { startManagerProcess, stopActiveManagerProcess, managerProcess } from "@/lib/recompiler/manager-process";
import { parseManagerLaunchRequest, DASHBOARD_MANAGER_ACTIONS } from "@/lib/recompiler/manager-contract";
import { rejectNonLocalControlRequest, rejectUnsupportedProcessHost } from "@/lib/recompiler/local-request";

export const runtime = "nodejs";

// GET /api/recompiler/manager
// Streams live stdout/stderr of the active process as Server-Sent Events (SSE).
export async function GET(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req);
  if (rejection) return rejection;
  const encoder = new TextEncoder();    const stream = new ReadableStream({
    start(controller) {
      const sendEvent = (event: string, data: unknown) => {
        try {
          controller.enqueue(encoder.encode(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`));
        } catch {
          // controller might already be closed
        }
      };

      // Send log history so client has immediate context upon connection/tab switches
      for (const logText of managerProcess.logs) {
        sendEvent("stdout", { text: logText });
      }

      // Bind this stream to the run that is active at connect time (#186): a
      // stale close/error from a previous generation must never close a stream
      // that is following the current run.
      const trackedRunId = managerProcess.runId;

      // Issue O-08: an idle manager no longer closes the stream. Idle streams
      // stay open and receive lifecycle "status" events, so dashboard panels
      // can refresh on real state changes instead of polling every few
      // seconds. The stream still closes on a terminal close/error event of
      // the run it follows.
      if (!managerProcess.child) {
        sendEvent("status", {
          phase: managerProcess.phase ?? "exited",
          action: null,
          code: managerProcess.lastExitCode ?? 0,
        });
      }

      // Register listener for live events
      const listener = (event: { type: "stdout" | "stderr" | "close" | "error" | "status"; runId?: number; text?: string; code?: number; message?: string; phase?: string; action?: string | null }) => {
        if (event.type === "status") {
          // Status events are lifecycle-wide; forward them regardless of the
          // generation bound at connect time.
          sendEvent("status", { phase: event.phase, action: event.action, code: event.code });
          return;
        }
        if (trackedRunId !== null && event.runId !== undefined && event.runId !== trackedRunId) {
          return; // event from a superseded generation; ignore
        }
        if (event.type === "stdout" || event.type === "stderr") {
          sendEvent(event.type, { text: event.text });
        } else if (event.type === "close") {
          sendEvent("close", { code: event.code });
          controller.close();
          managerProcess.listeners.delete(listener);
        } else if (event.type === "error") {
          sendEvent("error", { message: event.message });
          controller.close();
          managerProcess.listeners.delete(listener);
        }
      };

      managerProcess.listeners.add(listener);

      // Clean up when client disconnects
      req.signal.addEventListener("abort", () => {
        managerProcess.listeners.delete(listener);
      });
    },
  });

  return new Response(stream, {
    headers: {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache, no-transform",
      "Connection": "keep-alive",
    },
  });
}

// POST /api/recompiler/manager
// Non-blocking trigger to start nk_manager.ps1 with the requested action.
export async function POST(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req, { mutating: true }) ?? rejectUnsupportedProcessHost();
  if (rejection) return rejection;

  const body = await req.json().catch(() => ({}));
  let launch;
  try {
    launch = parseManagerLaunchRequest(body);
  } catch (error) {
    return routeError("invalid-manager-request", error, 400, {
      detail: error instanceof Error ? error.message : "Invalid manager request",
      supported: DASHBOARD_MANAGER_ACTIONS,
    });
  }

  // Prevent multiple overlapping tasks
  if (managerProcess.child) {
    return NextResponse.json({
      error: "process-active",
      message: `A background process (${managerProcess.action}) is already running. Please stop it first.`,
    }, { status: 409 });
  }

  try {
    await startManagerProcess(launch);
    return NextResponse.json({
      ok: true,
      status: "running",
      action: launch.action,
    });
  } catch (e) {
    return routeError("manager-failed", e, 500);
  }
}

// DELETE /api/recompiler/manager
// Kills the active rebuild or program execution process.
export async function DELETE(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req, { mutating: true }) ?? rejectUnsupportedProcessHost();
  if (rejection) return rejection;
  const action = managerProcess.action;
  const stopped = stopActiveManagerProcess();
  return NextResponse.json({
    ok: true,
    stopped,
    stoppedAction: action,
  });
}
