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

      // If no process is running, we can close the stream immediately
      if (!managerProcess.child) {
        sendEvent("close", { code: managerProcess.lastExitCode ?? 0 });
        controller.close();
        return;
      }

      // Register listener for live events
      const listener = (event: { type: "stdout" | "stderr" | "close" | "error"; runId?: number; text?: string; code?: number; message?: string }) => {
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
// Non-blocking trigger to start hst_manager.ps1 with the requested action.
export async function POST(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req, { mutating: true }) ?? rejectUnsupportedProcessHost();
  if (rejection) return rejection;

  const body = await req.json().catch(() => ({}));
  let launch;
  try {
    launch = parseManagerLaunchRequest(body);
  } catch (error) {
    return NextResponse.json(
      { error: "invalid-manager-request", detail: String(error), supported: DASHBOARD_MANAGER_ACTIONS },
      { status: 400 },
    );
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
    return NextResponse.json({ error: "manager-failed", detail: String(e) }, { status: 500 });
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
