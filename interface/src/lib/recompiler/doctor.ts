import { execFile } from "node:child_process";
import { promisify } from "node:util";
import path from "node:path";
import { existsSync } from "node:fs";

const execFileAsync = promisify(execFile);

export const DOCTOR_SCOPES = ["repo", "inputs", "build", "products", "run", "all"] as const;
export type DoctorScope = (typeof DOCTOR_SCOPES)[number];

export type DoctorStatus = "PASS" | "WARN" | "FAIL" | "INFO";

export interface DoctorResult {
  status: DoctorStatus;
  code: string;
  summary: string;
  path: string | null;
  detail: string | null;
  remediation: string | null;
  metadata?: Record<string, unknown>;
}

export interface DoctorCounts {
  PASS: number;
  WARN: number;
  FAIL: number;
  INFO: number;
}

export interface DoctorReport {
  schema_version: number;
  tool: string;
  root: string;
  scope: DoctorScope;
  strict: boolean;
  counts: DoctorCounts;
  exit_code: number;
  results: DoctorResult[];
}

const SCOPE_SET = new Set<string>(DOCTOR_SCOPES);

export function parseDoctorScope(value: unknown): DoctorScope {
  if (value === undefined || value === null || value === "") {
    return "all";
  }
  if (typeof value !== "string" || !SCOPE_SET.has(value)) {
    throw new Error(`invalid scope: expected one of ${DOCTOR_SCOPES.join(", ")}, got ${String(value)}`);
  }
  return value as DoctorScope;
}

export interface RunDoctorOptions {
  scope?: DoctorScope;
  strict?: boolean;
  msysPath?: string;
  vulkanSdk?: string;
  timeoutMs?: number;
}

export async function runDoctor(
  repoRoot: string,
  options: RunDoctorOptions = {},
): Promise<DoctorReport> {
  const scope = options.scope ?? "all";
  const strict = Boolean(options.strict);
  const doctorScript = path.join(repoRoot, "tools", "hst_doctor.py");

  if (!existsSync(doctorScript)) {
    throw new Error(`hst_doctor.py not found at ${doctorScript}`);
  }

  const pythonCmd = process.platform === "win32" ? "python" : "python3";
  const args = [doctorScript, "--json", "--scope", scope];

  if (strict) {
    args.push("--strict");
  }
  if (options.msysPath) {
    args.push("--msys-path", options.msysPath);
  }
  if (options.vulkanSdk) {
    args.push("--vulkan-sdk", options.vulkanSdk);
  }

  try {
    const { stdout, stderr } = await execFileAsync(pythonCmd, args, {
      cwd: repoRoot,
      maxBuffer: 16 * 1024 * 1024,
      timeout: options.timeoutMs ?? 20000,
      windowsHide: true,
    });

    const output = stdout.trim() || stderr.trim();
    if (!output) {
      throw new Error("hst_doctor.py produced no output");
    }

    const parsed = JSON.parse(output) as DoctorReport;
    if (!parsed || typeof parsed !== "object" || parsed.tool !== "hst_doctor") {
      throw new Error("invalid doctor output payload");
    }

    return parsed;
  } catch (error: unknown) {
    if (error && typeof error === "object" && "stdout" in error) {
      const execError = error as { stdout?: string; stderr?: string; message?: string };
      const raw = String(execError.stdout || "").trim();
      if (raw.startsWith("{") && raw.endsWith("}")) {
        try {
          const parsed = JSON.parse(raw) as DoctorReport;
          if (parsed && parsed.tool === "hst_doctor") {
            return parsed;
          }
        } catch {
          // fall through
        }
      }
      throw new Error(execError.stderr || execError.message || String(error));
    }
    throw error;
  }
}
