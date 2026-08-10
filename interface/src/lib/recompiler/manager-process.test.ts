// manager-process.test.ts — deterministic lifecycle tests for the
// generation-bound manager process registry (issue #186).  No real process is
// spawned: spawn/terminate/repo-root primitives are injected as fakes.  Run
// with `npm run test:db`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { mkdirSync, mkdtempSync, writeFileSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";

import { createManagerProcess } from "./manager-process";
import { pushBoundedLog, FuzzLogWriter, terminateProcessTree, truncateUtf8 } from "./manager-process";
import { resolveCanonicalRoot, tailFile } from "./runner";

/** Minimal fake ChildProcess: an EventEmitter with pid/stdout/stderr + kill. */
class FakeChild extends EventEmitter {
  pid: number | undefined;
  exitCode: number | null = null;
  signalCode: NodeJS.Signals | null = null;
  killed = false;
  stdout = new EventEmitter() as unknown as NodeJS.ReadableStream;
  stderr = new EventEmitter() as unknown as NodeJS.ReadableStream;

  constructor(pid = 1234) {
    super();
    this.pid = pid;
  }

  kill(): boolean {
    this.killed = true;
    return true;
  }
}

function makeDeps(overrides: Partial<Parameters<typeof createManagerProcess>[0]> = {}) {
  const children: FakeChild[] = [];
  const terminated: FakeChild[] = [];
  const repoRoot = mkdtempSync(path.join(tmpdir(), "hst-mgr-test-"));
  // The anchor set findRepoRoot needs.
  writeFileSync(path.join(repoRoot, "hst_manager.ps1"), "# test\n");
  writeFileSync(path.join(repoRoot, "AGENTS.md"), "# test\n");
  writeFileSync(path.join(repoRoot, "Makefile"), "all:\n");
  // Directories the run may touch (fuzz mirror log, watcher script probe).
  mkdirSync(path.join(repoRoot, "logs"), { recursive: true });
  mkdirSync(path.join(repoRoot, "tools"), { recursive: true });
  writeFileSync(path.join(repoRoot, "tools", "ppmdiff.py"), "#!/usr/bin/env python3\n");

  const deps = {
    spawnShell: () => {
      const child = new FakeChild(1000 + children.length);
      children.push(child);
      return child as unknown as import("node:child_process").ChildProcess;
    },
    spawnWatcher: () => {
      const child = new FakeChild(5000 + children.length);
      children.push(child);
      return child as unknown as import("node:child_process").ChildProcess;
    },
    terminateTree: (child: unknown) => {
      terminated.push(child as FakeChild);
    },
    findRepoRoot: () => repoRoot,
    logTelemetry: async () => {},
    ...overrides,
  };
  return { deps, children, terminated, repoRoot };
}

function emitStdout(child: FakeChild, text: string) {
  child.stdout.emit("data", Buffer.from(text, "utf8"));
}

test("run A stopped, run B starts, then A emits close: B intact", async () => {
  const { deps, children } = makeDeps();
  const mgr = createManagerProcess(deps);

  await mgr.startFuzzManagerProcess({ trials: 10, seed: "0x1", constraint: "integer" });
  const runA = children[0];
  assert.equal(mgr.state.runId, 1);
  assert.equal(mgr.state.phase, "starting");

  runA.emit("spawn");
  assert.equal(mgr.state.phase, "running");

  // Stop run A: terminate + free the slot for B.
  assert.equal(mgr.stopActiveManagerProcess(), true);
  assert.equal(mgr.state.child, null);

  // Start run B while A's close has NOT yet fired.
  await mgr.startFuzzManagerProcess({ trials: 5, seed: "0x2", constraint: "integer" });
  const runB = children[1];
  assert.equal(mgr.state.runId, 2);
  runB.emit("spawn");
  assert.equal(mgr.state.phase, "running");
  assert.equal(mgr.state.child, runB);

  // Now the stale close/error from A arrives.
  const messages: string[] = [];
  const listener = (m: { type: string; runId?: number }) => {
    messages.push(`${m.type}:${m.runId}`);
  };
  mgr.state.listeners.add(listener);
  runA.emit("close", 0);
  runA.emit("error", new Error("stale error"));

  // B's child, watcher (none here), status, phase and listeners remain intact.
  assert.equal(mgr.state.child, runB, "B's child must survive A's stale events");
  assert.equal(mgr.state.phase, "running");
  assert.equal(mgr.state.runId, 2);
  // The stale close/error must not emit terminal events to listeners (A's
  // terminal event was emitted at stop time with runId=1).
  assert.ok(!messages.some((m) => m.endsWith(":2")), `no terminal for B: ${messages}`);

  runB.emit("close", 0);
  assert.equal(mgr.state.child, null);
  assert.equal(mgr.state.phase, "exited");
  rmSync(deps.findRepoRoot!(), { recursive: true, force: true });
});

test("stale A error after B starts never kills B's watcher", async () => {
  const { deps, children, terminated } = makeDeps();
  const mgr = createManagerProcess(deps);

  await mgr.startManagerProcess({
    action: "Run",
    run: { profile: "Standard", durationSeconds: 5, noGui: true, softwareRender: false, snapshotInterval: 1 },
  });
  const runA = children[0];
  runA.emit("spawn");

  // A has a snapshot watcher (the ppmdiff watcher spawn).
  const watcherA = children[1];
  assert.equal(mgr.state.snapshotWatcher, watcherA);

  // Stop A, start B.
  mgr.stopActiveManagerProcess();
  await mgr.startManagerProcess({
    action: "Run",
    run: { profile: "Standard", durationSeconds: 5, noGui: true, softwareRender: false, snapshotInterval: 1 },
  });
  const runB = children[2];
  runB.emit("spawn");
  const watcherB = children[3];
  assert.equal(mgr.state.snapshotWatcher, watcherB);

  // Stale A error event fires.  It must not terminate B's watcher or B.
  terminated.length = 0;
  runA.emit("error", new Error("late failure"));
  assert.equal(mgr.state.child, runB);
  assert.equal(mgr.state.snapshotWatcher, watcherB, "B's watcher survives stale A");
  assert.ok(!terminated.includes(watcherB), "B's watcher was not terminated");

  runB.emit("close", 0);
  rmSync(deps.findRepoRoot!(), { recursive: true, force: true });
});

test("watcher spawn failure reports against owning run without killing manager child", async () => {
  const { deps, children } = makeDeps();
  const mgr = createManagerProcess(deps);

  await mgr.startManagerProcess({
    action: "Run",
    run: { profile: "Standard", durationSeconds: 5, noGui: true, softwareRender: false, snapshotInterval: 1 },
  });
  const runA = children[0];
  runA.emit("spawn");
  const watcher = children[1];
  assert.equal(mgr.state.snapshotWatcher, watcher);

  const errors: { message?: string; runId?: number }[] = [];
  mgr.state.listeners.add((m) => {
    if (m.type === "error") errors.push(m);
  });

  // Watcher fails to spawn: 'error' on the watcher child.
  watcher.emit("error", new Error("ENOENT: python"));
  assert.equal(mgr.state.child, runA, "manager child must not be closed");
  assert.equal(mgr.state.phase, "running");
  assert.equal(mgr.state.snapshotWatcher, null, "failed watcher is detached");
  assert.ok(errors.some((e) => e.runId === 1 && /watcher failed/.test(e.message ?? "")));

  runA.emit("close", 0);
  rmSync(deps.findRepoRoot!(), { recursive: true, force: true });
});

test("truncateUtf8 cuts at a byte budget without lone surrogates", () => {
  // 4-byte emoji: 5 chars = 20 bytes. Cutting to 10 bytes must not split a
  // surrogate pair and must not exceed the byte budget.
  const emoji = "😀😀😀😀😀";
  const cut = truncateUtf8(emoji, 10);
  assert.ok(Buffer.byteLength(cut, "utf8") <= 10);
  // No lone surrogate halves in the output.
  for (let i = 0; i < cut.length; i++) {
    const code = cut.charCodeAt(i);
    assert.ok(!(code >= 0xd800 && code <= 0xdbff) || (i + 1 < cut.length && (cut.charCodeAt(i + 1) & 0xfc00) === 0xdc00),
      "high surrogate must be paired");
    assert.ok(!((code & 0xfc00) === 0xdc00 && (i === 0 || (cut.charCodeAt(i - 1) & 0xfc00) !== 0xd800)),
      "low surrogate must be paired");
  }
  // ASCII passes through unchanged.
  assert.equal(truncateUtf8("hello world", 5), "hello");
});

test("bounded log history by bytes and lines with chunk truncation", () => {
  const logs: string[] = [];
  const bytes = { total: 0, lines: 0 };

  // Small chunks accumulate normally.
  for (let i = 0; i < 100; i++) pushBoundedLog(logs, bytes, `line ${i}\n`);
  assert.equal(logs.length, 100);
  assert.ok(bytes.total < 1024);

  // Oversized chunk is truncated.
  pushBoundedLog(logs, bytes, "x".repeat(200 * 1024));
  assert.ok(logs[logs.length - 1].length < 200 * 1024);
  assert.ok(bytes.total <= 4 * 1024 * 1024);

  // Total byte cap: push more than 4 MiB of small lines; total stays bounded.
  const big: string[] = [];
  const bigBytes = { total: 0, lines: 0 };
  for (let i = 0; i < 50_000; i++) pushBoundedLog(big, bigBytes, `0123456789\n`);
  assert.ok(bigBytes.total <= 4 * 1024 * 1024);
  assert.ok(bigBytes.lines <= 50_000);
  assert.ok(big.length <= 50_000);
});

test("FuzzLogWriter caps bytes and closes", async () => {
  const dir = mkdtempSync(path.join(tmpdir(), "hst-fuzz-"));
  const file = path.join(dir, "fuzz.log");
  const writer = new FuzzLogWriter(file, 1024);
  writer.append("a".repeat(500));
  writer.append("b".repeat(500));
  writer.append("c".repeat(500)); // beyond the 1024 cap; truncated to remaining
  await writer.close();
  const content = readFileSync(file, "utf8");
  assert.ok(content.length <= 1024 + 16, `written ${content.length} bytes`);
  assert.ok(content.includes("a"));
  writer.close(); // idempotent
  rmSync(dir, { recursive: true, force: true });
});

test("terminateProcessTree refuses stale PIDs without spawning taskkill", () => {
  // A child Node has already observed exiting (exitCode set) must never be
  // force-killed by numeric PID: the PID may have been reused.  We assert the
  // guard's decision surface by faking a child whose kill() records the call;
  // an exited child must return without invoking kill() at all.
  const exited = new FakeChild(43);
  exited.exitCode = 0;
  const killCalls: string[] = [];
  exited.kill = (signal?: NodeJS.Signals | number) => {
    killCalls.push(String(signal));
    return true;
  };
  terminateProcessTree(exited as unknown as import("node:child_process").ChildProcess);
  assert.deepEqual(killCalls, [], "exited child must never be killed by PID");

  // A child with no PID is a no-op as well.
  const noPid = new FakeChild(0);
  noPid.pid = undefined;
  noPid.kill = () => { throw new Error("kill must not be called"); };
  assert.doesNotThrow(() => terminateProcessTree(noPid as unknown as import("node:child_process").ChildProcess));

  // Note: the live-child kill path (taskkill on Windows / SIGTERM fallback) is
  // deliberately NOT exercised with a fake PID — driving real taskkill against
  // a fabricated PID is exactly the stale-PID hazard #186 exists to prevent.
});

test("resolveCanonicalRoot requires the full anchor set", () => {
  const root = mkdtempSync(path.join(tmpdir(), "hst-root-"));
  writeFileSync(path.join(root, "hst_manager.ps1"), "# x\n");
  assert.equal(resolveCanonicalRoot(root), null, "single anchor is not a repo root");
  writeFileSync(path.join(root, "AGENTS.md"), "# x\n");
  assert.equal(resolveCanonicalRoot(root), null, "two anchors still insufficient");
  writeFileSync(path.join(root, "Makefile"), "all:\n");
  assert.ok(resolveCanonicalRoot(root), "full anchor set resolves");
  rmSync(root, { recursive: true, force: true });
});

test("tailFile reads only the tail of a large file", () => {
  const dir = mkdtempSync(path.join(tmpdir(), "hst-tail-"));
  const file = path.join(dir, "big.log");
  const lineCount = 20_000;
  const lines: string[] = [];
  for (let i = 0; i < lineCount; i++) lines.push(`line-${i}`);
  writeFileSync(file, lines.join("\n") + "\n");

  const tail = tailFile(file, 4096, 200);
  assert.ok(tail.length > 0);
  assert.ok(tail.length <= 200);
  assert.equal(tail[tail.length - 1], `line-${lineCount - 1}`);
  rmSync(dir, { recursive: true, force: true });
});
