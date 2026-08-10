import { NextRequest, NextResponse } from "next/server";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { existsSync, mkdirSync, readFileSync } from "node:fs";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import path from "node:path";
import { rejectNonLocalControlRequest } from "@/lib/recompiler/local-request";

const execFileAsync = promisify(execFile);
const allowedGroups = new Set(["integer", "fpu", "vfpu"]);
export const runtime = "nodejs";

// POST /api/recompiler/tests/microtest
// Triggers on-demand microtest generation based on instruction groups or hex opcodes
export async function POST(req: NextRequest) {
  const rejection = rejectNonLocalControlRequest(req, { mutating: true });
  if (rejection) return rejection;
  try {
    const body = await req.json().catch(() => ({}));
    const extra = Number(body.extra ?? 6);
    const groups = Array.isArray(body.groups) ? body.groups : [];
    const opcodes = Array.isArray(body.opcodes) ? body.opcodes : [];

    if (!Number.isInteger(extra) || extra < 0 || extra > 64) {
      return NextResponse.json({ error: "invalid-extra", detail: "extra must be an integer from 0 to 64" }, { status: 400 });
    }
    if (groups.length > allowedGroups.size || groups.some((group: unknown) => typeof group !== "string" || !allowedGroups.has(group))) {
      return NextResponse.json({ error: "invalid-groups", detail: "groups may contain integer, fpu, and vfpu" }, { status: 400 });
    }
    if (opcodes.length > 256 || opcodes.some((opcode: unknown) => typeof opcode !== "string" || !/^(?:0x)?[0-9a-fA-F]{8}$/.test(opcode))) {
      return NextResponse.json({ error: "invalid-opcodes", detail: "opcodes must contain at most 256 eight-digit hex words" }, { status: 400 });
    }

    const repoRoot = findRepoRoot();
    const destDir = path.join(repoRoot, "build", "hst");
    mkdirSync(destDir, { recursive: true });
    const destFile = path.join(destDir, "microtest_gen.c");

    const pythonCmd = process.platform === "win32" ? "python" : "python3";
    const scriptPath = path.join(repoRoot, "tools", "gen_microtest.py");

    const childArgs = [scriptPath, destFile, String(extra)];
    if (opcodes.length > 0) {
      childArgs.push("--opcodes", opcodes.join(","));
    } else if (groups.length > 0) {
      childArgs.push("--groups", groups.join(","));
    }

    const { stdout, stderr } = await execFileAsync(pythonCmd, childArgs, {
      cwd: repoRoot,
      maxBuffer: 8 * 1024 * 1024,
      windowsHide: true,
    });

    if (existsSync(destFile)) {
      const generatedCode = readFileSync(destFile, "utf8");
      return NextResponse.json({
        success: true,
        destFile,
        stdout,
        stderr,
        code: generatedCode,
      });
    } else {
      return NextResponse.json({
        success: false,
        error: "generation-failed",
        detail: stderr || stdout || "Output file was not created",
      }, { status: 500 });
    }
  } catch (e) {
    return NextResponse.json({ error: "microtest-gen-failed", detail: String(e) }, { status: 500 });
  }
}
