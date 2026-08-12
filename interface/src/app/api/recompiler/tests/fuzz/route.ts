import { NextRequest, NextResponse } from "next/server";
import { startFuzzManagerProcess, managerProcess } from "@/lib/recompiler/manager-process";
import { rejectNonLocalControlRequest, rejectUnsupportedProcessHost } from "@/lib/recompiler/local-request";

export const runtime = "nodejs";

// GET /api/recompiler/tests/fuzz
// Returns status of the fuzzer process
export async function GET() {
  const isRunning = managerProcess.action === "Fuzz" && !!managerProcess.child;
  return NextResponse.json({
    isRunning,
    action: managerProcess.action,
    logCount: managerProcess.logs.length,
  });
}

// POST /api/recompiler/tests/fuzz
// Starts the fuzzer run with configuration parameters
export async function POST(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req, { mutating: true }) ?? rejectUnsupportedProcessHost();
  if (rejection) return rejection;
  if (managerProcess.child) {
    return NextResponse.json({
      error: "process-active",
      message: `A background process (${managerProcess.action}) is already running.`,
    }, { status: 409 });
  }

  try {
    const body = await req.json().catch(() => ({}));
    const trials = Number(body.trials ?? 200);
    const seed = String(body.seed ?? "0x12345678");
    const constraint = String(body.constraint ?? "none");

    if (!Number.isSafeInteger(trials) || trials < 1 || trials > 100_000) {
      return NextResponse.json({ error: "invalid-trials", detail: "trials must be an integer from 1 to 100000" }, { status: 400 });
    }
    if (!/^(?:0x[0-9a-f]{1,8}|[0-9]{1,10})$/i.test(seed)) {
      return NextResponse.json({ error: "invalid-seed", detail: "seed must be a 32-bit decimal or hexadecimal integer" }, { status: 400 });
    }
    if (!new Set(["none", "integer", "fpu", "vfpu"]).has(constraint)) {
      return NextResponse.json({ error: "invalid-constraint" }, { status: 400 });
    }

    await startFuzzManagerProcess({
      trials,
      seed,
      constraint: constraint as "none" | "integer" | "fpu" | "vfpu",
    });

    return NextResponse.json({
      success: true,
      status: "running",
      config: { trials, seed, constraint },
    });
  } catch (e) {
    return NextResponse.json({ error: "fuzz-start-failed", detail: String(e) }, { status: 500 });
  }
}
