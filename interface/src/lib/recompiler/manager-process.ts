import { ChildProcess, spawn, spawnSync } from "node:child_process";
import { createWriteStream, type WriteStream } from "node:fs";
import path from "node:path";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { findRepoRoot as realFindRepoRoot, spawnPowerShell as realSpawnPowerShell } from "./runner";
import {
  managerPowerShellParameters,
  type DashboardManagerAction,
  type ManagerLaunchRequest,
} from "./manager-contract";

export type ProcessMessage = {
  type: "stdout" | "stderr" | "close" | "error";
  /** Generation that produced this event; stale generations are ignored. */
  runId?: number;
  text?: string;
  code?: number;
  message?: string;
};

type ManagerAction = DashboardManagerAction | "Fuzz";

/**
 * Explicit process lifecycle phases (issue #186).  A run moves through these
 * deterministically; terminal phases are recorded on the run's tombstone so a
 * stale callback can never resurrect or overwrite a newer generation.
 */
export type ProcessPhase =
  | "starting"
  | "running"
  | "stopping"
  | "exited"
  | "failed-to-spawn";

export type RunRecord = {
  runId: number;
  child: ChildProcess;
  snapshotWatcher: ChildProcess | null;
  action: ManagerAction;
  phase: ProcessPhase;
  lastExitCode: number | null;
  startedAt: number;
  spawned: boolean;
  terminalEmitted: boolean;
};

export interface FuzzLaunchOptions {
  trials: number;
  seed: string;
  constraint: "none" | "integer" | "fpu" | "vfpu";
}

// --- Bounded log history ----------------------------------------------------
// Retained history is bounded by total UTF-8 bytes AND line count; individual
// chunks are truncated.  This replaces the unbounded 5000-chunk array so a
// verbose child can never retain hundreds of megabytes in the dashboard.
const MAX_LOG_BYTES = 4 * 1024 * 1024;   // 4 MiB retained
const MAX_LOG_LINES = 50_000;            // line cap
const MAX_CHUNK_BYTES = 64 * 1024;       // per-chunk truncation

/**
 * Truncate a UTF-8 string to at most `maxBytes` bytes at a character boundary.
 * Walks back over continuation bytes so a multi-byte sequence is never split
 * and the result never exceeds the byte budget (no U+FFFD padding past it).
 */
export function truncateUtf8(text: string, maxBytes: number): string {
  if (Buffer.byteLength(text, "utf8") <= maxBytes) return text;
  const buf = Buffer.from(text, "utf8");
  let end = Math.min(buf.length, maxBytes);
  // If the byte at `end` is a continuation byte, its lead byte is inside the
  // window and the sequence would be split: back up to before that sequence.
  while (end > 0 && end < buf.length && (buf[end] & 0xc0) === 0x80) {
    end -= 1;
  }
  return buf.subarray(0, end).toString("utf8");
}

export function pushBoundedLog(
  logs: string[],
  bytes: { total: number; lines: number },
  text: string,
): void {
  if (text.length === 0) return;
  let chunk = text;
  if (Buffer.byteLength(chunk, "utf8") > MAX_CHUNK_BYTES) {
    chunk = truncateUtf8(chunk, MAX_CHUNK_BYTES) + "\n…[chunk truncated]\n";
  }
  const chunkBytes = Buffer.byteLength(chunk, "utf8");
  const chunkLines = chunk.split(/\r?\n/).length;
  logs.push(chunk);
  bytes.total += chunkBytes;
  bytes.lines += chunkLines;
  while (logs.length > 0 &&
         (bytes.total > MAX_LOG_BYTES || bytes.lines > MAX_LOG_LINES)) {
    const dropped = logs.shift();
    if (dropped === undefined) break;
    bytes.total -= Buffer.byteLength(dropped, "utf8");
    bytes.lines -= dropped.split(/\r?\n/).length;
  }
}

// --- Bounded fuzz log writer ------------------------------------------------
// Replaces the per-chunk synchronous appendFileSync() with an asynchronous
// write stream carrying an explicit byte-size policy and end-of-stream close.
const MAX_FUZZ_LOG_BYTES = 64 * 1024 * 1024;  // 64 MiB mirror cap

export class FuzzLogWriter {
  private stream: WriteStream | null = null;
  private bytesWritten = 0;
  private readonly maxBytes: number;
  private readonly path: string;
  private closed = false;

  constructor(filePath: string, maxBytes = MAX_FUZZ_LOG_BYTES) {
    this.path = filePath;
    this.maxBytes = maxBytes;
  }

  append(text: string): void {
    if (this.closed) return;
    if (this.bytesWritten >= this.maxBytes) {
      // Explicit file-size policy: once the mirror cap is reached, stop writing
      // and note the truncation once so the policy is observable.
      if (!this.truncationNoted) {
        this.truncationNoted = true;
        const notice = "\n…[fuzz log truncated at the dashboard byte cap]\n";
        if (this.bytesWritten + Buffer.byteLength(notice, "utf8") <= this.maxBytes * 2) {
          this.writeRaw(notice);
        }
      }
      return;
    }
    let slice = text;
    const remaining = this.maxBytes - this.bytesWritten;
    if (Buffer.byteLength(slice, "utf8") > remaining) {
      slice = truncateUtf8(slice, remaining);
    }
    this.writeRaw(slice);
  }

  private writeRaw(text: string): void {
    if (!this.stream) {
      this.stream = createWriteStream(this.path, { flags: "a", encoding: "utf8" });
    }
    this.stream.write(text);
    this.bytesWritten += Buffer.byteLength(text, "utf8");
  }

  async close(): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    const stream = this.stream;
    this.stream = null;
    if (stream) {
      await new Promise<void>((resolve) => {
        stream.end(() => resolve());
      });
    }
  }

  private truncationNoted = false;
}

// --- Process-tree termination ----------------------------------------------
/**
 * Terminate a child's process tree.  Issue #186: never act on a stale PID.  If
 * Node has already observed this ChildProcess exiting (`exitCode`/`signalCode`
 * set), the PID may have been reused; skip force-killing it entirely.  The
 * ChildProcess object itself (not a bare numeric PID) is the identity anchor,
 * and the owning run's `startedAt` is recorded for evidence.
 */
export function terminateProcessTree(child: ChildProcess): void {
  if (!child.pid) return;
  if (child.exitCode !== null || child.signalCode !== null) return;
  if (process.platform === "win32") {
    const result = spawnSync("taskkill.exe", ["/PID", String(child.pid), "/T", "/F"], {
      windowsHide: true,
      stdio: "ignore",
    });
    if (result.status === 0) return;
  }
  try {
    child.kill("SIGTERM");
  } catch {
    // The process may already have exited.
  }
}

// --- Manager process registry (factory + production singleton) -------------

export interface ManagerProcessApi {
  state: ManagerProcessState;
  startManagerProcess(request: ManagerLaunchRequest): Promise<void>;
  startFuzzManagerProcess(options: FuzzLaunchOptions): Promise<void>;
  stopActiveManagerProcess(): boolean;
}

export type ManagerProcessState = {
  nextRunId: number;
  active: RunRecord | null;
  runs: Map<number, RunRecord>;
  logs: string[];
  logBytes: { total: number; lines: number };
  listeners: Set<(data: ProcessMessage) => void>;
  /** Terminal state of the most recently finalized run (survives stop/exit). */
  lastTerminal: { runId: number; phase: ProcessPhase; lastExitCode: number } | null;
  readonly child: ChildProcess | null;
  readonly snapshotWatcher: ChildProcess | null;
  readonly action: ManagerAction | null;
  readonly lastExitCode: number | null;
  readonly phase: ProcessPhase | null;
  readonly runId: number | null;
};

/** Test seam: injectable spawn/terminate/repo-root primitives. */
export interface ManagerProcessDeps {
  spawnShell: (command: string, parameters: Record<string, string | number | boolean>, opts: { cwd?: string; env?: NodeJS.ProcessEnv }) => ChildProcess;
  spawnWatcher: (command: string, args: string[], opts: { cwd?: string; env?: NodeJS.ProcessEnv; windowsHide: boolean }) => ChildProcess;
  terminateTree?: (child: ChildProcess) => void;
  findRepoRoot?: () => string;
  logTelemetry?: () => Promise<void>;
}

async function defaultTelemetry(): Promise<void> {
  // Lazy import so importing this module never forces @/lib/db (tests and
  // dashboard startup stay free of a DATABASE_URL dependency unless a run ends).
  try {
    const { logTelemetry } = await import("./telemetry");
    await logTelemetry();
  } catch {
    // Telemetry is best-effort after a run ends.
  }
}

export function createManagerProcess(deps: ManagerProcessDeps): ManagerProcessApi {
  const state = createState();
  const terminateTree = deps.terminateTree ?? terminateProcessTree;
  const findRepoRoot = deps.findRepoRoot ?? realFindRepoRoot;
  const telemetry = deps.logTelemetry ?? defaultTelemetry;

  function emit(message: ProcessMessage): void {
    for (const listener of state.listeners) {
      try {
        listener(message);
      } catch {
        // A disconnected SSE consumer must not affect the managed process.
      }
    }
  }

  /**
   * Finalize a run.  A run may be finalized exactly once for terminal events;
   * its tombstone (phase/lastExitCode) is preserved so a late callback from a
   * superseded generation can never mutate the active run's child, watcher, or
   * emit a close/error that would terminate the newer generation.
   */
  function finalizeRun(run: RunRecord, code: number | null, phase: ProcessPhase, message?: string): void {
    run.phase = phase;
    run.lastExitCode = code ?? 1;
    // Only a run at least as new as the last recorded terminal may update it;
    // a delayed close from an older generation must not clobber a newer run's
    // terminal state (issue #186).
    if (run.runId >= (state.lastTerminal?.runId ?? 0)) {
      state.lastTerminal = { runId: run.runId, phase, lastExitCode: run.lastExitCode };
    }
    const ownsActive = state.active?.runId === run.runId;
    if (!run.terminalEmitted) {
      run.terminalEmitted = true;
      emit({
        type: phase === "failed-to-spawn" ? "error" : "close",
        code: code ?? 1,
        message,
        runId: run.runId,
      });
    }
    if (ownsActive) {
      // Terminate THIS run's watcher only; never a newer run's watcher.
      if (run.snapshotWatcher) {
        terminateTree(run.snapshotWatcher);
        run.snapshotWatcher = null;
      }
      state.active = null;
    }
    // Bound the tombstone map to the most recent runs.
    if (state.runs.size > 32) {
      const oldest = state.runs.keys().next().value;
      if (oldest !== undefined) state.runs.delete(oldest);
    }
  }

  function stopActiveManagerProcess(): boolean {
    const run = state.active;
    if (!run) return false;
    run.phase = "stopping";
    terminateTree(run.child);
    if (run.snapshotWatcher) terminateTree(run.snapshotWatcher);
    // Emit the terminal close for the stopping run NOW (with its runId) so SSE
    // consumers close their streams, then free the slot for a replacement run.
    // The child's delayed close/error will be ignored because this run no longer
    // owns the active generation and its terminal event was already emitted.
    finalizeRun(run, 1, "exited");
    return true;
  }

  function cleanManagerEnvironment(additions: Record<string, string> = {}): NodeJS.ProcessEnv {
    const env: NodeJS.ProcessEnv = { ...process.env };
    // SR_* switches are presence-based in the runtime. Never inherit a caller's
    // shell diagnostics into a dashboard task; hst_manager.ps1 sets the selected
    // profile explicitly. Fuzzer inputs are added only by the validated endpoint.
    for (const key of Object.keys(env)) {
      if (key.startsWith("SR_") || key.startsWith("FUZZ_")) delete env[key];
    }
    return { ...env, ...additions };
  }

  async function activeDebugEnvironment(): Promise<Record<string, string>> {
    try {
      const { db } = await import("@/lib/db");
      const profile = await db.debugProfile.findFirst({ where: { isActive: true } });
      if (!profile) return {};
      // #188: profiles from an unsupported schema version must not feed the
      // runtime environment.
      if (typeof profile.schemaVersion !== "number" || profile.schemaVersion !== 1) return {};

      const env: Record<string, string> = {};
      if (Number.isSafeInteger(profile.debugMask) && profile.debugMask > 0) {
        env.SR_DEBUG = String(profile.debugMask >>> 0);
      }

      const parsed = JSON.parse(profile.watchpoints) as unknown;
      if (!Array.isArray(parsed)) return env;
      for (const [index, value] of parsed.slice(0, 16).entries()) {
        if (!value || typeof value !== "object") continue;
        const watch = value as Record<string, unknown>;
        const start = Number(watch.start);
        const end = Number(watch.end);
        if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start < 0 || end < start || end > 0xffffffff) continue;
        const label = String(watch.label ?? `watch-${index}`).replace(/[^A-Za-z0-9_. -]/g, "_").slice(0, 48);
        env[`SR_WATCH_${index}`] = `0x${start.toString(16)},0x${end.toString(16)},${label}`;
      }
      return env;
    } catch (error) {
      console.error("Failed to load active debug profile:", error);
      return {};
    }
  }

  async function startManagedPowerShell(
    action: ManagerAction,
    parameters: Record<string, string | number | boolean>,
    envAdditions: Record<string, string> = {},
    options: { watchSnapshots?: boolean } = {},
  ): Promise<void> {
    if (state.active) {
      throw new Error(`a background process (${state.active.action}) is already running`);
    }

    const repoRoot = findRepoRoot();
    const psScript = path.join(repoRoot, "hst_manager.ps1");
    if (!existsSync(psScript)) throw new Error(`hst_manager.ps1 not found at ${psScript}`);

    const fuzzLogPath = path.join(repoRoot, "logs", "vfpu_fuzz_latest.log");
    const fuzzWriter = action === "Fuzz" ? new FuzzLogWriter(fuzzLogPath) : null;
    if (fuzzWriter) writeFileSync(fuzzLogPath, "", "utf8");

    const runId = state.nextRunId++;
    const child = deps.spawnShell(psScript, parameters, {
      cwd: repoRoot,
      env: cleanManagerEnvironment(envAdditions),
    });
    const run: RunRecord = {
      runId,
      child,
      snapshotWatcher: null,
      action,
      phase: "starting",
      lastExitCode: null,
      startedAt: Date.now(),
      spawned: false,
      terminalEmitted: false,
    };
    state.runs.set(runId, run);
    state.active = run;

    const handleData = (type: "stdout" | "stderr", chunk: Buffer) => {
      if (state.active?.runId !== runId) return; // stale generation
      const text = chunk.toString("utf8");
      pushBoundedLog(state.logs, state.logBytes, text);
      if (fuzzWriter) fuzzWriter.append(text);
      emit({ type, text, runId });
    };

    child.stdout?.on("data", (chunk: Buffer) => handleData("stdout", chunk));
    child.stderr?.on("data", (chunk: Buffer) => handleData("stderr", chunk));

    child.on("spawn", () => {
      run.spawned = true;
      if (state.active?.runId === runId) run.phase = "running";
    });

    child.on("error", (error) => {
      // A spawn failure ('ENOENT' etc.) surfaces here before/without 'close'.
      finalizeRun(run, 1, run.spawned ? "exited" : "failed-to-spawn", error.message);
      fuzzWriter?.close();
      void telemetry().catch(() => {});
    });

    child.on("close", (code) => {
      finalizeRun(run, code ?? 1, "exited");
      fuzzWriter?.close();
      void telemetry().catch(() => {});
    });

    if (options.watchSnapshots) {
      const watcherScript = path.join(repoRoot, "tools", "ppmdiff.py");
      if (existsSync(watcherScript)) {
        const snapshots = path.join(repoRoot, "build", "snapshots");
        const golden = path.join(repoRoot, "build", "golden");
        mkdirSync(snapshots, { recursive: true });
        mkdirSync(golden, { recursive: true });
        const python = process.platform === "win32" ? "python" : "python3";
        const watcher = deps.spawnWatcher(python, [watcherScript, "--watch", snapshots, golden], {
          cwd: repoRoot,
          env: cleanManagerEnvironment(),
          windowsHide: true,
        });
        run.snapshotWatcher = watcher;
        watcher.stdout?.on("data", (chunk: Buffer) => handleData("stdout", chunk));
        watcher.stderr?.on("data", (chunk: Buffer) => handleData("stderr", chunk));
        // Issue #186: watcher failure is reported against its OWNING run; it
        // must never crash the server or falsely close the manager child.
        watcher.on("error", (error) => {
          if (state.active?.runId !== runId) return;
          emit({ type: "error", message: `snapshot watcher failed: ${error.message}`, runId });
          run.snapshotWatcher = null;
        });
        watcher.on("close", (code) => {
          // A watcher exiting on its own must not stop the manager run.
          if (state.active?.runId !== runId) return;
          if (code !== 0 && code !== null) {
            emit({ type: "error", message: `snapshot watcher exited with code ${code}`, runId });
          }
          run.snapshotWatcher = null;
        });
      }
    }
  }

  async function startManagerProcess(request: ManagerLaunchRequest): Promise<void> {
    const env: Record<string, string> = {};
    if (request.action === "Run" && request.run?.snapshotInterval !== null && request.run?.snapshotInterval !== undefined) {
      env.SR_FBSNAP = String(request.run.snapshotInterval);
    }
    if (request.action === "Run" && request.run?.profile === "Diagnostics") {
      Object.assign(env, await activeDebugEnvironment());
    }
    return startManagedPowerShell(
      request.action,
      managerPowerShellParameters(request),
      env,
      { watchSnapshots: request.action === "Run" && request.run?.snapshotInterval !== null },
    );
  }

  async function startFuzzManagerProcess(options: FuzzLaunchOptions): Promise<void> {
    return startManagedPowerShell(
      "Fuzz",
      { Action: "Fuzz" },
      {
        FUZZ_TRIALS: String(options.trials),
        FUZZ_SEED: options.seed,
        FUZZ_CONSTRAINT: options.constraint,
      },
    );
  }

  return { state, startManagerProcess, startFuzzManagerProcess, stopActiveManagerProcess };
}

function createState(): ManagerProcessState {
  const state = {} as ManagerProcessState;
  state.nextRunId = 1;
  state.active = null;
  state.runs = new Map();
  state.logs = [];
  state.logBytes = { total: 0, lines: 0 };
  state.listeners = new Set();
  state.lastTerminal = null;
  Object.defineProperties(state, {
    child: { get: () => state.active?.child ?? null },
    snapshotWatcher: { get: () => state.active?.snapshotWatcher ?? null },
    action: { get: () => state.active?.action ?? null },
    lastExitCode: { get: () => state.active?.lastExitCode ?? state.lastTerminal?.lastExitCode ?? null },
    phase: { get: () => state.active?.phase ?? state.lastTerminal?.phase ?? null },
    runId: { get: () => state.active?.runId ?? state.lastTerminal?.runId ?? null },
  });
  return state;
}

// --- Production singleton ---------------------------------------------------

declare global {
  var activeManagerProcess: ManagerProcessApi | undefined;
}

const productionDeps: ManagerProcessDeps = {
  spawnShell: realSpawnPowerShell,
  spawnWatcher: (command, args, opts) => spawn(command, args, opts),
};

if (!globalThis.activeManagerProcess) {
  globalThis.activeManagerProcess = createManagerProcess(productionDeps);
}
export const managerProcess: ManagerProcessState = globalThis.activeManagerProcess.state;
export const startManagerProcess = globalThis.activeManagerProcess.startManagerProcess;
export const startFuzzManagerProcess = globalThis.activeManagerProcess.startFuzzManagerProcess;
export const stopActiveManagerProcess = globalThis.activeManagerProcess.stopActiveManagerProcess;
