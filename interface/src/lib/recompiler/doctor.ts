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
const STATUS_SET = new Set<DoctorStatus>(["PASS", "WARN", "FAIL", "INFO"]);
const COUNT_KEYS = ["PASS", "WARN", "FAIL", "INFO"] as const;

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

export function isDoctorReport(value: unknown): value is DoctorReport {
  if (!isRecord(value)) return false;
  if (
    value.schema_version !== 1 ||
    value.tool !== "hst_doctor" ||
    typeof value.root !== "string" ||
    typeof value.scope !== "string" ||
    !SCOPE_SET.has(value.scope) ||
    typeof value.strict !== "boolean" ||
    typeof value.exit_code !== "number" ||
    !Number.isInteger(value.exit_code) ||
    ![0, 1, 2].includes(value.exit_code) ||
    !isRecord(value.counts) ||
    !Array.isArray(value.results)
  ) {
    return false;
  }

  const counts = value.counts;
  const actualCounts: Record<DoctorStatus, number> = { PASS: 0, WARN: 0, FAIL: 0, INFO: 0 };
  for (const key of COUNT_KEYS) {
    const count = counts[key];
    if (typeof count !== "number" || !Number.isInteger(count) || count < 0) return false;
  }

  for (const result of value.results) {
    if (
      !isRecord(result) ||
      typeof result.status !== "string" ||
      !STATUS_SET.has(result.status as DoctorStatus) ||
      typeof result.code !== "string" ||
      typeof result.summary !== "string"
    ) {
      return false;
    }
    actualCounts[result.status as DoctorStatus] += 1;
  }

  if (!COUNT_KEYS.every((key) => counts[key] === actualCounts[key])) return false;
  const failCount = counts.FAIL as number;
  const warnCount = counts.WARN as number;
  const expectedExitCode = failCount > 0 ? 1 : value.strict && warnCount > 0 ? 2 : 0;
  return value.exit_code === expectedExitCode;
}

/**
 * Reason a Doctor run could not produce a report at all. These are route-level
 * failures (the diagnostic subprocess never ran or never reported), distinct
 * from a report whose checks contain FAIL results.
 */
export type DoctorFailureReason =
  | "python-missing"
  | "repo-root-missing"
  | "doctor-script-missing"
  | "timeout"
  | "unknown";

export interface ClassifiedDoctorFailure {
  reason: DoctorFailureReason;
  /** One-line, plain-language statement of what went wrong. */
  title: string;
  /** Why this happens, in terms a non-expert can act on. */
  explanation: string;
  /** The concrete next action that resolves the failure. */
  nextAction: string;
  /** Raw diagnostic text, preserved for a developer-facing detail toggle. */
  diagnostic: string | null;
}

/**
 * Map a raw failure string from runDoctor (or a transport error) to a known
 * failure class with plain-language remediation. The raw string is always
 * preserved in `diagnostic` so no information is discarded for developers.
 */
export function classifyDoctorFailure(rawDetail: string | null | undefined): ClassifiedDoctorFailure {
  const detail = typeof rawDetail === "string" ? rawDetail : "";
  const lower = detail.toLowerCase();

  if (lower.includes("enoent") && lower.includes("python")) {
    return {
      reason: "python-missing",
      title: "Python is not installed or not on PATH",
      explanation:
        "The workspace preflight runs the project's diagnostic script, which needs Python. Windows could not start a python process.",
      nextAction:
        "Install Python 3 from python.org (or run 'winget install Python.Python.3.12') and choose 'Add python.exe to PATH' during setup, then reload this dashboard.",
      diagnostic: detail || null,
    };
  }
  if (lower.includes("repo-root-not-found")) {
    return {
      reason: "repo-root-missing",
      title: "The Nakagawa Recomp project folder was not found",
      explanation:
        "The dashboard locates the project by finding hst_manager.ps1, AGENTS.md and Makefile together. The folder it started in does not contain them.",
      nextAction:
        "Start the dashboard from the Nakagawa Recomp checkout (run 'npm run dev' inside its interface folder), or set HST_DASHBOARD_REPO_ROOT to the project folder and reload.",
      diagnostic: detail || null,
    };
  }
  if (lower.includes("hst_doctor.py not found")) {
    return {
      reason: "doctor-script-missing",
      title: "The preflight diagnostic script is missing",
      explanation:
        "The dashboard could not find the project's diagnostic script (tools/hst_doctor.py) inside the Nakagawa Recomp project.",
      nextAction:
        "Verify this is a complete Nakagawa Recomp checkout; tools/hst_doctor.py must exist in the project folder.",
      diagnostic: detail || null,
    };
  }
  if (lower.includes("timed out") || lower.includes("timeout") || lower.includes("etimedout")) {
    return {
      reason: "timeout",
      title: "The preflight check took too long",
      explanation:
        "The diagnostic script did not finish within its time limit. A first run can be slow while every tool is probed.",
      nextAction:
        "Use Refresh to try again. If it keeps timing out, run 'python tools/hst_doctor.py --json --scope all' in a terminal to see where it stalls.",
      diagnostic: detail || null,
    };
  }
  return {
    reason: "unknown",
    title: "The preflight check could not run",
    explanation: "The diagnostic script failed before it could produce a report.",
    nextAction:
      "Expand the technical detail below. If it names a missing tool, install that tool and refresh.",
    diagnostic: detail || null,
  };
}

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

    const parsed = JSON.parse(output) as unknown;
    if (!isDoctorReport(parsed)) {
      throw new Error("invalid doctor output payload");
    }

    return parsed;
  } catch (error: unknown) {
    if (error && typeof error === "object" && "stdout" in error) {
      const execError = error as {
        stdout?: string;
        stderr?: string;
        message?: string;
        code?: number | string;
        killed?: boolean;
        signal?: string | null;
      };
      const raw = String(execError.stdout || "").trim();
      if (raw.startsWith("{") && raw.endsWith("}")) {
        try {
          const parsed = JSON.parse(raw) as unknown;
          const processExitCode = Number(execError.code);
          if (
            isDoctorReport(parsed) &&
            parsed.exit_code !== 0 &&
            !execError.killed &&
            !execError.signal &&
            processExitCode === parsed.exit_code
          ) {
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
