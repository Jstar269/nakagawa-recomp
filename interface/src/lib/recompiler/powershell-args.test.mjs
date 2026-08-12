import assert from "node:assert/strict";
import test from "node:test";
import { readFileSync } from "node:fs";
import { buildPowerShellArgs } from "./powershell-args.mjs";

const runnerSource = readFileSync(new URL("./runner.ts", import.meta.url), "utf8");

test("emits each PowerShell parameter as a separate argv token", () => {
  assert.deepEqual(buildPowerShellArgs("C:\\repo\\hst_manager.ps1", { Action: "BuildFast" }), [
    "-NoProfile",
    "-NonInteractive",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    "C:\\repo\\hst_manager.ps1",
    "-Action",
    "BuildFast",
  ]);
});

test("preserves spaces and quotes inside one value token", () => {
  const args = buildPowerShellArgs("manager.ps1", {
    DiffTarget: 'C:\\trace files\\oracle "retail".trace',
  });
  assert.deepEqual(args.slice(-2), ["-DiffTarget", 'C:\\trace files\\oracle "retail".trace']);
});

test("emits true switches and omits false or empty optional values", () => {
  const args = buildPowerShellArgs("manager.ps1", {
    NoGui: true,
    SoftwareRender: false,
    Missing: undefined,
    Empty: null,
  });
  assert.deepEqual(args.slice(-1), ["-NoGui"]);
});

test("keeps numeric zero as an explicit value", () => {
  const args = buildPowerShellArgs("manager.ps1", { Duration: 0 });
  assert.deepEqual(args.slice(-2), ["-Duration", "0"]);
});

test("rejects parameter names that could become PowerShell syntax", () => {
  assert.throws(
    () => buildPowerShellArgs("manager.ps1", { "Action;Write-Host": "Run" }),
    /invalid PowerShell parameter name/,
  );
});

test("runner uses only PowerShell 7 and capability-based Vulkan discovery", () => {
  assert.doesNotMatch(runnerSource, /powershell\.exe/i);
  assert.match(runnerSource, /const cmdPath = "pwsh"/g);
  assert.doesNotMatch(runnerSource, /1\.4\.350\.0|1\.3\.290\.0/);
  assert.match(runnerSource, /vulkan\.h/);
  assert.match(runnerSource, /vulkan-1\.lib/);
});
