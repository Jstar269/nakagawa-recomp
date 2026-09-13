/* Real runner layer for the repository's native scripts and generated output.
 *
 * Design: this module assumes it is being executed on the same machine as the
 * Nakagawa Recomp project. Resolve REPO_ROOT from cwd; bail with a clear
 * error otherwise. PowerShell is required for hst_manager.ps1. There is no
 * simulated build or runtime fallback. */
import { spawn, spawnSync, ChildProcess } from "node:child_process";
import { closeSync, existsSync, lstatSync, openSync, readFileSync, readSync, readdirSync, realpathSync, statSync } from "node:fs";
import path from "node:path";
import { NextResponse } from "next/server";
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

/**
 * Incrementally read new lines appended to a log file since a given byte offset.
 * Bounded to maxBytes (default MAX_LOG_TAIL_BYTES = 256KB) so a large/corrupt log
 * is never loaded whole into memory (Issue #187 Finding 3).
 * Returns the parsed lines and the updated byte offset cursor.
 */
export function readLogSince(
  pathName: string,
  sinceByte: number,
  maxBytes = MAX_LOG_TAIL_BYTES,
): { lines: string[]; cursor: number } {
  const size = statSync(/* turbopackIgnore: true */ pathName).size;
  if (!Number.isFinite(sinceByte) || sinceByte < 0) {
    sinceByte = 0;
  }
  if (sinceByte >= size) {
    return { lines: [], cursor: size };
  }

  const bytesToRead = Math.min(size - sinceByte, maxBytes);
  const fd = openSync(/* turbopackIgnore: true */ pathName, "r");
  try {
    const buf = Buffer.alloc(bytesToRead);
    readSync(fd, buf, 0, bytesToRead, sinceByte);
    const content = buf.toString("utf8");
    const lines = content.split(/\r?\n/).filter((line) => line.length > 0);
    return {
      lines,
      cursor: sinceByte + bytesToRead,
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

// ---- Subprocess execution hygiene (#188 Finding 6) -------------------------

export interface SubprocessOptions {
  cwd?: string;
  env?: NodeJS.ProcessEnv;
  timeoutMs?: number;
  maxBuffer?: number;
  signal?: AbortSignal;
  windowsHide?: boolean;
  allowNonZeroExit?: boolean;
}

export interface SubprocessResult {
  stdout: string;
  stderr: string;
  exitCode: number;
}

export class SubprocessError extends Error {
  readonly code: string;
  readonly exitCode: number | null;
  readonly stdout: string;
  readonly stderr: string;
  readonly timedOut: boolean;
  readonly aborted: boolean;

  constructor(message: string, details: {
    code: string;
    exitCode?: number | null;
    stdout?: string;
    stderr?: string;
    timedOut?: boolean;
    aborted?: boolean;
  }) {
    super(message);
    this.name = "SubprocessError";
    this.code = details.code;
    this.exitCode = details.exitCode ?? null;
    this.stdout = details.stdout ?? "";
    this.stderr = details.stderr ?? "";
    this.timedOut = !!details.timedOut;
    this.aborted = !!details.aborted;
  }
}

/**
 * Execute a child process with fail-closed safety guarantees:
 * - Hard timeout with child.kill() to prevent hung/stalled processes.
 * - Both stdout and stderr pipes are actively drained with listeners to prevent pipe-buffer deadlocks.
 * - Bounded buffer limits (maxBuffer) to prevent out-of-memory crashes.
 * - AbortSignal listener to terminate processes on client disconnection.
 */
export function runSubprocess(
  command: string,
  args: string[] = [],
  options: SubprocessOptions = {},
): Promise<SubprocessResult> {
  const timeoutMs = options.timeoutMs ?? 30_000;
  const maxBuffer = options.maxBuffer ?? 8 * 1024 * 1024;
  const windowsHide = options.windowsHide ?? true;
  const allowNonZeroExit = options.allowNonZeroExit ?? false;

  return new Promise((resolve, reject) => {
    let child: ChildProcess;
    try {
      child = spawn(command, args, {
        cwd: options.cwd,
        env: options.env,
        windowsHide,
      });
    } catch (err) {
      return reject(new SubprocessError(`Failed to spawn ${command}`, {
        code: "spawn-failed",
        stderr: err instanceof Error ? err.message : String(err),
      }));
    }

    let stdout = "";
    let stderr = "";
    let settled = false;
    let timedOut = false;
    let aborted = false;

    const killProcess = () => {
      try {
        child.kill();
      } catch {
        // ignore kill failure
      }
    };

    let timeoutTimer: NodeJS.Timeout | null = null;
    let abortHandler: (() => void) | null = null;

    const cleanup = () => {
      if (timeoutTimer) {
        clearTimeout(timeoutTimer);
        timeoutTimer = null;
      }
      if (options.signal && abortHandler) {
        options.signal.removeEventListener("abort", abortHandler);
        abortHandler = null;
      }
    };

    if (timeoutMs > 0 && timeoutMs !== Infinity) {
      timeoutTimer = setTimeout(() => {
        if (!settled) {
          settled = true;
          timedOut = true;
          cleanup();
          killProcess();
          reject(new SubprocessError(`${command} timed out after ${timeoutMs}ms`, {
            code: "timeout",
            stdout,
            stderr,
            timedOut: true,
          }));
        }
      }, timeoutMs);
    }

    if (options.signal) {
      if (options.signal.aborted) {
        settled = true;
        aborted = true;
        cleanup();
        killProcess();
        return reject(new SubprocessError(`${command} aborted before start`, {
          code: "aborted",
          aborted: true,
        }));
      }
      abortHandler = () => {
        if (!settled) {
          settled = true;
          aborted = true;
          cleanup();
          killProcess();
          reject(new SubprocessError(`${command} request aborted`, {
            code: "aborted",
            stdout,
            stderr,
            aborted: true,
          }));
        }
      };
      options.signal.addEventListener("abort", abortHandler, { once: true });
    }

    child.stdout?.on("data", (chunk: Buffer) => {
      if (settled) return;
      const str = chunk.toString("utf8");
      if (stdout.length + str.length > maxBuffer) {
        settled = true;
        cleanup();
        killProcess();
        reject(new SubprocessError(`${command} stdout exceeded buffer limit of ${maxBuffer} bytes`, {
          code: "buffer-overflow",
          stdout: stdout.slice(0, maxBuffer),
          stderr,
        }));
        return;
      }
      stdout += str;
    });

    child.stderr?.on("data", (chunk: Buffer) => {
      if (settled) return;
      const str = chunk.toString("utf8");
      if (stderr.length + str.length > maxBuffer) {
        settled = true;
        cleanup();
        killProcess();
        reject(new SubprocessError(`${command} stderr exceeded buffer limit of ${maxBuffer} bytes`, {
          code: "buffer-overflow",
          stdout,
          stderr: stderr.slice(0, maxBuffer),
        }));
        return;
      }
      stderr += str;
    });

    child.on("error", (err: Error) => {
      if (settled) return;
      settled = true;
      cleanup();
      reject(new SubprocessError(`${command} error: ${err.message}`, {
        code: "process-error",
        stdout,
        stderr,
      }));
    });

    child.on("close", (code: number | null) => {
      if (settled) return;
      settled = true;
      cleanup();
      const exitCode = code ?? 0;
      if (exitCode !== 0 && !allowNonZeroExit) {
        reject(new SubprocessError(`${command} exited with code ${exitCode}`, {
          code: "nonzero-exit",
          exitCode,
          stdout,
          stderr,
        }));
      } else {
        resolve({
          stdout,
          stderr,
          exitCode,
        });
      }
    });
  });
}

// ---- Safe API error responses (#188 Finding 6) -----------------------------

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

// ---- Path-traversal resistant shared directory walker (#187 Finding 4) -----

export interface SafeWalkOptions {
  maxDepth?: number;
  maxFiles?: number;
  targetFileName?: string;
}

/**
 * Traverses a directory tree fail-closed with path-traversal resistance:
 * - Uses path.relative containment check (both lexical and realpath) rather than string prefix.
 * - Tracks visited real directory paths to prevent recursion loops from junctions or symlinks.
 * - Enforces hard depth and total file count caps.
 */
export function safeWalkDirectory(
  rootDir: string,
  options: SafeWalkOptions = {},
): string[] {
  const maxDepth = options.maxDepth ?? 16;
  const maxFiles = options.maxFiles ?? 5000;
  const targetFileName = options.targetFileName;

  if (!existsSync(rootDir)) return [];

  const normalizedRoot = path.resolve(rootDir);
  let realRoot: string;
  try {
    realRoot = realpathSync.native(normalizedRoot);
  } catch {
    return [];
  }

  const results: string[] = [];
  const visitedDirs = new Set<string>();

  function walk(currentDir: string, currentDepth: number): void {
    if (currentDepth > maxDepth || results.length >= maxFiles) return;

    let realCurrent: string;
    try {
      realCurrent = realpathSync.native(currentDir);
    } catch {
      return;
    }

    // Check directory containment relative to realRoot
    const rel = path.relative(realRoot, realCurrent);
    if (rel !== "" && (rel.startsWith("..") || path.isAbsolute(rel))) {
      return;
    }

    // Loop detection via canonical realpath
    if (visitedDirs.has(realCurrent)) return;
    visitedDirs.add(realCurrent);

    let entries: string[];
    try {
      entries = readdirSync(currentDir);
    } catch {
      return;
    }

    for (const entry of entries) {
      if (results.length >= maxFiles) break;

      const entryPath = path.join(currentDir, entry);
      try {
        const lst = lstatSync(entryPath);
        if (lst.isSymbolicLink()) continue;

        const realEntry = realpathSync.native(entryPath);
        const relEntry = path.relative(realRoot, realEntry);
        if (relEntry.startsWith("..") || path.isAbsolute(relEntry)) {
          continue;
        }

        const st = statSync(entryPath);
        if (st.isDirectory()) {
          walk(entryPath, currentDepth + 1);
        } else if (!targetFileName || entry === targetFileName) {
          results.push(entryPath);
        }
      } catch {
        // Skip inaccessible entries
      }
    }
  }

  walk(rootDir, 0);
  return results;
}
