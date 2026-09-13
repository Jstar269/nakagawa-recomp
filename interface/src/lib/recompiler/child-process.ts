import { spawn, ChildProcess } from "node:child_process";

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
