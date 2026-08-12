/* Real runner layer for the repository's native scripts and generated output.
 *
 * Design: this module assumes it is being executed on the same machine as the
 * Nakagawa Recomp project. Resolve REPO_ROOT from cwd; bail with a clear
 * error otherwise. PowerShell is required for hst_manager.ps1. There is no
 * simulated build or runtime fallback. */
import { spawn, spawnSync, ChildProcess } from "node:child_process";
import { closeSync, existsSync, openSync, readFileSync, readSync, readdirSync, realpathSync, statSync } from "node:fs";
import path from "node:path";
import { buildPowerShellArgs } from "./powershell-args.mjs";

// ---- Repo layout discovery ----------------------------------------------

/** Immutable anchors that must coexist at the repository root. */
const REPO_ANCHORS = ["hst_manager.ps1", "AGENTS.md", "Makefile"] as const;

function isRepoRoot(dir: string): boolean {
  // Issue #186: require multiple immutable anchors so a stray hst_manager.ps1
  // elsewhere cannot grant process-control routes access to the wrong tree.
  const present = REPO_ANCHORS.filter((anchor) =>
    existsSync(/* turbopackIgnore: true */ path.join(/* turbopackIgnore: true */ dir, anchor)));
  return present.length === REPO_ANCHORS.length;
}

/**
 * Resolve the canonical repository root.  Issue #186: the result is realpath-
 * canonicalized, and ambiguous or symlink/reparse-escaped roots are refused.
 * An explicit HST_DASHBOARD_REPO_ROOT environment override is honored as
 * explicit configuration and is itself validated against the anchors.
 */
export function findRepoRoot(): string {
  const override = process.env.HST_DASHBOARD_REPO_ROOT;
  if (override?.trim()) {
    const resolved = resolveCanonicalRoot(override.trim());
    if (resolved) return resolved;
    throw new Error(`repo-root-invalid: HST_DASHBOARD_REPO_ROOT=${override} is not a Nakagawa Recomp root`);
  }

  // Walk up from cwd looking for the anchor set.
  let dir = process.cwd();
  for (let i = 0; i < 8; i++) {
    const resolved = resolveCanonicalRoot(dir);
    if (resolved) return resolved;
    const parent = path.dirname(/* turbopackIgnore: true */ dir);
    if (parent === dir) break;
    dir = parent;
  }
  // Fallback: relative to the dashboard cwd.
  const studioSibling = path.resolve(/* turbopackIgnore: true */ process.cwd(), "..");
  const siblingResolved = resolveCanonicalRoot(studioSibling);
  if (siblingResolved) return siblingResolved;
  throw new Error("repo-root-not-found: hst_manager.ps1/AGENTS.md/Makefile are not on the path; run the dashboard from the Nakagawa Recomp project tree.");
}

export function resolveCanonicalRoot(candidate: string): string | null {
  if (!existsSync(/* turbopackIgnore: true */ candidate)) return null;
  let canonical = candidate;
  try {
    canonical = realpathSync(/* turbopackIgnore: true */ candidate);
  } catch {
    // Keep the input path if realpath fails (e.g. drive-root edge cases); the
    // anchor check below still applies to the canonical candidate.
  }
  // A symlink/reparse-escaped root would resolve to a place where the anchors
  // do not coexist; refuse it instead of trusting the original spelling.
  return isRepoRoot(canonical) ? canonical : null;
}

export function repoPath(...parts: string[]): string {
  return path.join(/* turbopackIgnore: true */ findRepoRoot(), ...parts);
}

// ---- Hst.exe inspector ---------------------------------------------------

export interface HstExecutableStatus {
  hstExePath: string;
  exists: boolean;
  sizeBytes: number;
  mtime: number;
  vulkanSdkFoundAt: string | null;
}

function isUsableVulkanSdk(sdkPath: string): boolean {
  const headerCandidates = [
    path.join(sdkPath, "Include", "vulkan", "vulkan.h"),
    path.join(sdkPath, "include", "vulkan", "vulkan.h"),
  ];
  const libraryCandidates = [
    path.join(sdkPath, "Lib", "vulkan-1.lib"),
    path.join(sdkPath, "lib", "vulkan-1.lib"),
  ];
  return headerCandidates.some((candidate) => existsSync(candidate)) &&
    libraryCandidates.some((candidate) => existsSync(candidate));
}

function parseSdkVersion(name: string): number[] | null {
  const parts = name.split(".");
  if (parts.length < 2 || parts.some((part) => !/^\d+$/.test(part))) return null;
  return parts.map((part) => Number(part));
}

/** Resolve the dashboard's read-only SDK status using the manager's precedence. */
export function findVulkanSdk(): string | null {
  const environmentCandidate = process.env.VULKAN_SDK;
  if (environmentCandidate?.trim()) return isUsableVulkanSdk(environmentCandidate) ? environmentCandidate : null;

  // Keep the installation root runtime-derived so Next's standalone tracer does not
  // attempt to copy an entire developer SDK into the dashboard bundle.
  const installRoot = path.join(process.env.SystemDrive ?? "C:", "VulkanSDK");
  if (!existsSync(installRoot)) return null;
  let candidates: { path: string; version: number[] }[] = [];
  try {
    candidates = readdirSync(installRoot, { withFileTypes: true })
      .filter((entry) => entry.isDirectory())
      .map((entry) => ({ path: path.join(installRoot, entry.name), version: parseSdkVersion(entry.name) }))
      .filter((candidate): candidate is { path: string; version: number[] } => candidate.version !== null)
      .sort((left, right) => {
        const length = Math.max(left.version.length, right.version.length);
        for (let index = 0; index < length; index += 1) {
          const delta = (right.version[index] ?? 0) - (left.version[index] ?? 0);
          if (delta !== 0) return delta;
        }
        return 0;
      });
  } catch {
    return null;
  }
  return candidates.find((candidate) => isUsableVulkanSdk(candidate.path))?.path ?? null;
}

export function inspectHst(repoRoot: string): HstExecutableStatus {
  const hstExePath = path.join(/* turbopackIgnore: true */ repoRoot, "build", "hst", "hst.exe");
  let exists = false, sizeBytes = 0, mtime = 0;
  if (existsSync(hstExePath)) {
    exists = true;
    const st = statSync(hstExePath);
    sizeBytes = st.size;
    mtime = st.mtimeMs;
  }
  const vulkanSdkFoundAt = findVulkanSdk();
  return { hstExePath, exists, sizeBytes, mtime, vulkanSdkFoundAt };
}

// ---- PowerShell wrapper --------------------------------------------------

export interface PowerShellCallOptions {
  cwd?: string;
  env?: NodeJS.ProcessEnv;
  timeoutMs?: number;
  captureStream?: (chunk: Buffer) => void;
}

export interface PowerShellCallResult {
  ok: boolean;
  status: number | null;
  stdout: string;
  stderr: string;
  signal: NodeJS.Signals | null;
  timedOut: boolean;
}

export function callPowerShell(
  command: string,
  parameters: Record<string, string | number | boolean> = {},
  opts: PowerShellCallOptions = {},
): PowerShellCallResult {
  const cmdPath = "pwsh";

  const psArgs = buildPowerShellArgs(command, parameters);
  const sync = spawnSync(cmdPath, psArgs, {
    cwd: opts.cwd ?? process.cwd(),
    env: opts.env ?? process.env,
    encoding: "utf8",
    timeout: opts.timeoutMs ?? 5 * 60 * 1000,
    maxBuffer: 32 * 1024 * 1024,
  });
  return {
    ok: sync.status === 0 && !sync.error,
    status: sync.status,
    stdout: sync.stdout ?? "",
    stderr: sync.stderr ?? "",
    signal: sync.signal,
    timedOut: !!sync.signal && sync.signal === "SIGTERM",
  };
}

// ---- BuildFull / BuildFast drivers --------------------------------------

export interface BuildInvocation {
  action: "BuildFull" | "BuildFast" | "Test" | "Clean";
  startedAt: number;
  finishedAt: number | null;
  status: "queued" | "running" | "completed" | "failed";
  outputTail: string;
  result: PowerShellCallResult | null;
}

export function invokeHstManager(
  repoRoot: string,
  action: BuildInvocation["action"],
  opts: { timeoutMs?: number } = {},
): BuildInvocation {
  const inv: BuildInvocation = {
    action,
    startedAt: Date.now(),
    finishedAt: null,
    status: "running",
    outputTail: "",
    result: null,
  };
  const ps = path.join(/* turbopackIgnore: true */ repoRoot, "hst_manager.ps1");
  if (!existsSync(ps)) {
    inv.status = "failed";
    inv.finishedAt = Date.now();
    inv.result = {
      ok: false,
      status: null,
      stdout: "",
      stderr: `hst_manager.ps1 not found at ${ps}`,
      signal: null,
      timedOut: false,
    };
    return inv;
  }
  const r = callPowerShell(ps, { Action: action }, { cwd: repoRoot, timeoutMs: opts.timeoutMs });
  inv.finishedAt = Date.now();
  inv.status = r.ok ? "completed" : "failed";
  inv.result = r;
  inv.outputTail = (r.stdout + "\n" + r.stderr).slice(-4000);
  return inv;
}

// ---- Log tailing ---------------------------------------------------------

/** Upper bound on the number of bytes read from a log for a tail request. */
export const MAX_LOG_TAIL_BYTES = 256 * 1024;

export interface LogTail {
  found: boolean;
  path: string | null;
  sizeBytes: number;
  lastLines: string[];
  allLogs: string[];
}

/**
 * Read only the last `maxBytes` bytes of a file and return the final lines.
 * Issue #186: a large/corrupt log must never be loaded whole into memory.
 */
export function tailFile(pathName: string, maxBytes = MAX_LOG_TAIL_BYTES, maxLines = 200): string[] {
  const size = statSync(/* turbopackIgnore: true */ pathName).size;
  const readBytes = Math.min(size, maxBytes);
  const fd = openSync(/* turbopackIgnore: true */ pathName, "r");
  try {
    const buf = Buffer.alloc(readBytes);
    if (readBytes > 0) {
      readSync(fd, buf, 0, readBytes, size - readBytes);
    }
    const content = buf.toString("utf8");
    const lines = content.split(/\r?\n/);
    return lines.slice(-maxLines).filter((line) => line.length > 0);
  } finally {
    closeSync(fd);
  }
}

/**
 * Read only the first `maxBytes` bytes of a file and report whether the read
 * was truncated.  Used by event-scanning consumers (e.g. the boot route) so a
 * large/corrupt log can never exhaust the dashboard process (#186).  The boot
 * events this reader scans for are emitted at startup, i.e. at the file head.
 */
export function readLogPrefix(pathName: string, maxBytes = MAX_LOG_TAIL_BYTES): { content: string; truncated: boolean; sizeBytes: number } {
  const size = statSync(/* turbopackIgnore: true */ pathName).size;
  const readBytes = Math.min(size, maxBytes);
  const fd = openSync(/* turbopackIgnore: true */ pathName, "r");
  try {
    const buf = Buffer.alloc(readBytes);
    if (readBytes > 0) {
      readSync(fd, buf, 0, readBytes, 0);
    }
    return {
      content: buf.toString("utf8"),
      truncated: size > readBytes,
      sizeBytes: size,
    };
  } finally {
    closeSync(fd);
  }
}

/**
 * Read only the last `maxBytes` bytes of a file as a string.  Used by
 * telemetry parsers that scan for the most-recent section markers (e.g.
 * `--- PERF_PROFILE ---`), so a large/corrupt log can never be loaded whole
 * into memory (#189).
 */
export function readLogTailContent(pathName: string, maxBytes = MAX_LOG_TAIL_BYTES): { content: string; truncated: boolean; sizeBytes: number } {
  const size = statSync(/* turbopackIgnore: true */ pathName).size;
  const readBytes = Math.min(size, maxBytes);
  const fd = openSync(/* turbopackIgnore: true */ pathName, "r");
  try {
    const buf = Buffer.alloc(readBytes);
    if (readBytes > 0) {
      readSync(fd, buf, 0, readBytes, size - readBytes);
    }
    return {
      content: buf.toString("utf8"),
      truncated: size > readBytes,
      sizeBytes: size,
    };
  } finally {
    closeSync(fd);
  }
}

export function findLatestRunLog(repoRoot: string): LogTail {
  const dir = path.join(/* turbopackIgnore: true */ repoRoot, "logs");
  if (!existsSync(dir)) return { found: false, path: null, sizeBytes: 0, lastLines: [], allLogs: [] };
  const candidates = readdirSync(dir)
    .filter((f) => /^stderr_(?:run\d*|[a-z0-9]+)\.log$/i.test(f))
    .map((f) => ({ f, mtime: statSync(path.join(/* turbopackIgnore: true */ dir, f)).mtimeMs }))
    .sort((a, b) => b.mtime - a.mtime);
  if (candidates.length === 0) return { found: false, path: null, sizeBytes: 0, lastLines: [], allLogs: [] };
  const allLogs = candidates.map((c) => c.f).sort();
  const latest = path.join(/* turbopackIgnore: true */ dir, candidates[0].f);
  return {
    found: true,
    path: latest,
    sizeBytes: statSync(latest).size,
    lastLines: tailFile(latest),
    allLogs,
  };
}

// ---- progress.json reader ------------------------------------------------

import { parseProgressSnapshot } from "./progress-snapshot.mjs";
import type { ProgressSnapshot, RunIdentity, EvidenceSummary, ProgressPhase } from "./progress-snapshot.mjs";
export type { RunIdentity, EvidenceSummary, ProgressPhase } from "./progress-snapshot.mjs";

export function readProgressJson(repoRoot: string): ProgressSnapshot | null {
  const p = path.join(/* turbopackIgnore: true */ repoRoot, "progress.json");
  if (!existsSync(p)) return null;
  try {
    const raw = JSON.parse(readFileSync(p, "utf8"));
    // #181: the pure parser carries run identity + per-item evidence grades
    // from progress_tracker.py; legacy files degrade to the pre-#181 shape.
    return parseProgressSnapshot(raw, { fileMtimeMs: statSync(p).mtimeMs });
  } catch {
    return null;
  }
}

export function spawnPowerShell(
  command: string,
  parameters: Record<string, string | number | boolean> = {},
  opts: { cwd?: string; env?: NodeJS.ProcessEnv } = {},
): ChildProcess {
  const cmdPath = "pwsh";

  const psArgs = buildPowerShellArgs(command, parameters);

  return spawn(cmdPath, psArgs, {
    cwd: opts.cwd ?? process.cwd(),
    env: opts.env ?? process.env,
  });
}
