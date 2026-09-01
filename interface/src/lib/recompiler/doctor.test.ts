import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import * as os from "node:os";
import * as path from "node:path";
import { NextRequest } from "next/server";
import {
  DOCTOR_SCOPES,
  isDoctorReport,
  parseDoctorScope,
  runDoctor,
} from "./doctor";
import type { DoctorReport } from "./doctor";
import { findRepoRoot } from "./runner";
import { GET as doctorGet } from "@/app/api/recompiler/doctor/route";
import { summarizeDoctorReport } from "@/components/studio/summary-rail";

async function withSyntheticDoctor<T>(script: string, callback: (root: string) => Promise<T>): Promise<T> {
  const root = await mkdtemp(path.join(os.tmpdir(), "nakagawa-doctor-test-"));
  await mkdir(path.join(root, "tools"), { recursive: true });
  await writeFile(path.join(root, "tools", "hst_doctor.py"), script, "utf8");
  try {
    return await callback(root);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}

const syntheticZeroFailureReport: DoctorReport = {
  schema_version: 1,
  tool: "hst_doctor",
  root: "synthetic",
  scope: "repo",
  strict: false,
  counts: { PASS: 1, WARN: 0, FAIL: 0, INFO: 0 },
  exit_code: 0,
  results: [
    {
      status: "PASS",
      code: "SYNTHETIC_PASS",
      summary: "synthetic pass",
      path: null,
      detail: null,
      remediation: null,
    },
  ],
};

const syntheticFailureReport: DoctorReport = {
  ...syntheticZeroFailureReport,
  scope: "all",
  counts: { PASS: 0, WARN: 0, FAIL: 1, INFO: 0 },
  exit_code: 1,
  results: [
    {
      status: "FAIL",
      code: "SYNTHETIC_FAILURE",
      summary: "synthetic failure",
      path: null,
      detail: null,
      remediation: null,
    },
  ],
};

test("parseDoctorScope: accepts all valid scopes and defaults to all", () => {
  assert.equal(parseDoctorScope(undefined), "all");
  assert.equal(parseDoctorScope(null), "all");
  assert.equal(parseDoctorScope(""), "all");

  for (const scope of DOCTOR_SCOPES) {
    assert.equal(parseDoctorScope(scope), scope);
  }
});

test("parseDoctorScope: rejects invalid scopes", () => {
  assert.throws(() => parseDoctorScope("invalid"), /invalid scope/);
  assert.throws(() => parseDoctorScope("ALL"), /invalid scope/);
  assert.throws(() => parseDoctorScope(123), /invalid scope/);
  assert.throws(() => parseDoctorScope(true), /invalid scope/);
});

test("summary Doctor state never treats loading or unavailable as ALL PASS", () => {
  const loading = summarizeDoctorReport(null, "loading");
  const unavailable = summarizeDoctorReport(null, "unavailable");
  const success = summarizeDoctorReport(syntheticZeroFailureReport, "ready");
  const malformed = summarizeDoctorReport(
    { ...syntheticZeroFailureReport, counts: undefined } as unknown as DoctorReport,
    "ready",
  );

  assert.equal(loading.label, "CHECKING…");
  assert.equal(unavailable.label, "UNAVAILABLE");
  assert.notEqual(loading.label, "ALL PASS");
  assert.notEqual(unavailable.label, "ALL PASS");
  assert.equal(isDoctorReport(syntheticZeroFailureReport), true);
  assert.equal(success.label, "ALL PASS");
  assert.equal(malformed.label, "UNAVAILABLE");
});

test("runDoctor: rejects malformed synthetic output", async () => {
  await withSyntheticDoctor(`print('{"tool": "hst_doctor"}')\n`, async (root) => {
    await assert.rejects(runDoctor(root));
  });
});

test("runDoctor: rejects a nonzero synthetic subprocess without a report", async () => {
  await withSyntheticDoctor("import sys\nsys.stderr.write(\"synthetic failure\\n\")\nsys.exit(7)\n", async (root) => {
    await assert.rejects(runDoctor(root));
  });
});

test("runDoctor: preserves a structured nonzero Doctor report", async () => {
  const payload = JSON.stringify(syntheticFailureReport);
  await withSyntheticDoctor(`import sys\nprint(${JSON.stringify(payload)})\nsys.exit(1)\n`, async (root) => {
    const report = await runDoctor(root);
    assert.equal(report.exit_code, 1);
    assert.equal(report.counts.FAIL, 1);
  });
});

test("runDoctor: rejects a timed-out synthetic subprocess", async () => {
  await withSyntheticDoctor("import time\ntime.sleep(2)\n", async (root) => {
    await assert.rejects(runDoctor(root, { timeoutMs: 250 }));
  });
});

test("runDoctor: executes hst_doctor.py and produces structured report", async () => {
  const repoRoot = findRepoRoot();
  const report = await runDoctor(repoRoot, { scope: "repo" });

  assert.equal(report.schema_version, 1);
  assert.equal(report.tool, "hst_doctor");
  assert.equal(report.scope, "repo");
  assert.ok(typeof report.counts === "object");
  assert.ok(typeof report.counts.PASS === "number");
  assert.ok(typeof report.counts.FAIL === "number");
  assert.ok(typeof report.counts.WARN === "number");
  assert.ok(typeof report.counts.INFO === "number");
  assert.ok(Array.isArray(report.results));
  assert.ok(report.results.length > 0);

  for (const result of report.results) {
    assert.ok(["PASS", "WARN", "FAIL", "INFO"].includes(result.status));
    assert.ok(typeof result.code === "string");
    assert.ok(typeof result.summary === "string");
  }
});

test("GET /api/recompiler/doctor route: returns 400 on invalid scope", async () => {
  const req = new NextRequest("http://127.0.0.1:3000/api/recompiler/doctor?scope=badscope", {
    headers: { host: "127.0.0.1:3000" },
  });
  const res = await doctorGet(req);
  assert.equal(res.status, 400);
  const data = (await res.json()) as Record<string, unknown>;
  assert.equal(data.error, "invalid-doctor-scope");
});

test("GET /api/recompiler/doctor route: returns valid report for scope=repo", async () => {
  const req = new NextRequest("http://127.0.0.1:3000/api/recompiler/doctor?scope=repo", {
    headers: { host: "127.0.0.1:3000" },
  });
  const res = await doctorGet(req);
  assert.equal(res.status, 200);
  assert.equal(res.headers.get("cache-control"), "no-store");
  const data = (await res.json()) as { tool: string; scope: string; results: unknown[] };
  assert.equal(data.tool, "hst_doctor");
  assert.equal(data.scope, "repo");
  assert.ok(Array.isArray(data.results));
});

test("GET /api/recompiler/doctor route: rejects non-local host", async () => {
  const req = new NextRequest("http://example.com/api/recompiler/doctor?scope=repo", {
    headers: { host: "example.com" },
  });
  const res = await doctorGet(req);
  assert.equal(res.status, 403);
});
