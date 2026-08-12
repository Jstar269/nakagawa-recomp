import assert from "node:assert/strict";
import test from "node:test";
import { parseDebugConsoleRequest } from "./debug-console-contract.mjs";

test("accepts a bounded read-only memory request", () => {
  assert.deepEqual(parseDebugConsoleRequest({ action: "read_mem", args: ["0x08800000", "64", "words"] }), {
    action: "read_mem",
    args: ["0x08800000", "64", "words"],
    mutating: false,
  });
});

test("marks process and memory mutations", () => {
  assert.equal(parseDebugConsoleRequest({ action: "pause" }).mutating, true);
  assert.equal(parseDebugConsoleRequest({ action: "write_mem", args: ["0x08800000", "12 34"] }).mutating, true);
  assert.equal(parseDebugConsoleRequest({ action: "write_cpu", args: ["pc", "0x08804000"] }).mutating, true);
});

test("rejects unknown actions, fields, and extra keys", () => {
  assert.throws(() => parseDebugConsoleRequest({ action: "--simulate" }), /action must be one of/);
  assert.throws(() => parseDebugConsoleRequest({ action: "status", args: ["--simulate"] }), /expects 0 argument/);
  assert.throws(() => parseDebugConsoleRequest({ action: "status", extra: true }), /unsupported field/);
});

test("bounds memory reads and validates their format", () => {
  assert.throws(
    () => parseDebugConsoleRequest({ action: "read_mem", args: ["0x08800000", "4097"] }),
    /1 to 4096/,
  );
  assert.throws(
    () => parseDebugConsoleRequest({ action: "read_mem", args: ["0x08800000", "16", "binary"] }),
    /format must be one of/,
  );
});

test("validates write payloads and CpuState fields", () => {
  assert.throws(
    () => parseDebugConsoleRequest({ action: "write_mem", args: ["0x08800000", "123"] }),
    /complete bytes/,
  );
  assert.throws(
    () => parseDebugConsoleRequest({ action: "write_cpu", args: ["constructor", "1"] }),
    /supported CpuState register/,
  );
});
