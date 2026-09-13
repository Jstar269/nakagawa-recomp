import { test } from "node:test";
import assert from "node:assert/strict";
import { runSubprocess, SubprocessError } from "./child-process";

test("runSubprocess: executes command and captures stdout/stderr", async () => {
  const isWin = process.platform === "win32";
  const cmd = isWin ? "cmd.exe" : "sh";
  const args = isWin ? ["/c", "echo hello-world"] : ["-c", "echo hello-world"];

  const res = await runSubprocess(cmd, args);
  assert.equal(res.exitCode, 0);
  assert.ok(res.stdout.includes("hello-world"));
});

test("runSubprocess: kills child on timeoutMs expiry", async () => {
  const isWin = process.platform === "win32";
  const cmd = isWin ? "powershell.exe" : "sleep";
  const args = isWin ? ["-NoProfile", "-Command", "Start-Sleep -Seconds 5"] : ["5"];

  await assert.rejects(
    async () => {
      await runSubprocess(cmd, args, { timeoutMs: 300 });
    },
    (err: unknown) => {
      assert.ok(err instanceof SubprocessError);
      assert.equal(err.code, "timeout");
      assert.equal(err.timedOut, true);
      assert.match(err.message, /timed out after 300ms/i);
      return true;
    }
  );
});

test("runSubprocess: throws when maxBuffer is exceeded", async () => {
  const isWin = process.platform === "win32";
  const cmd = isWin ? "cmd.exe" : "sh";
  // Generate > 100 bytes of output
  const args = isWin
    ? ["/c", "echo " + "A".repeat(200)]
    : ["-c", "printf '%0.sA' {1..200}"];

  await assert.rejects(
    async () => {
      await runSubprocess(cmd, args, { maxBuffer: 50 });
    },
    (err: unknown) => {
      assert.ok(err instanceof SubprocessError);
      assert.equal(err.code, "buffer-overflow");
      assert.match(err.message, /exceeded buffer limit of 50 bytes/i);
      return true;
    }
  );
});

test("runSubprocess: aborts child immediately on AbortSignal", async () => {
  const controller = new AbortController();
  const isWin = process.platform === "win32";
  const cmd = isWin ? "powershell.exe" : "sleep";
  const args = isWin ? ["-NoProfile", "-Command", "Start-Sleep -Seconds 5"] : ["5"];

  setTimeout(() => controller.abort(), 100);

  await assert.rejects(
    async () => {
      await runSubprocess(cmd, args, { signal: controller.signal });
    },
    (err: unknown) => {
      assert.ok(err instanceof SubprocessError);
      assert.equal(err.code, "aborted");
      assert.equal(err.aborted, true);
      return true;
    }
  );
});

test("runSubprocess: allowNonZeroExit returns exit code instead of throwing", async () => {
  const isWin = process.platform === "win32";
  const cmd = isWin ? "cmd.exe" : "sh";
  const args = isWin ? ["/c", "exit 42"] : ["-c", "exit 42"];

  const res = await runSubprocess(cmd, args, { allowNonZeroExit: true });
  assert.equal(res.exitCode, 42);
});

test("runSubprocess: drains large stdio without deadlock", async () => {
  const isWin = process.platform === "win32";
  const cmd = isWin ? "powershell.exe" : "sh";
  // Emit ~100KB of output
  const args = isWin
    ? ["-NoProfile", "-Command", "1..2000 | ForEach-Object { 'chunk-' + $_ }"]
    : ["-c", "for i in $(seq 1 2000); do echo chunk-$i; done"];

  const res = await runSubprocess(cmd, args, { maxBuffer: 1024 * 1024 });
  assert.equal(res.exitCode, 0);
  assert.ok(res.stdout.includes("chunk-2000"));
});
