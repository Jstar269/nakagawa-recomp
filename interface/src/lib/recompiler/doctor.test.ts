import { test } from "node:test";
import assert from "node:assert/strict";
import { NextRequest } from "next/server";
import { parseDoctorScope, runDoctor, DOCTOR_SCOPES } from "./doctor";
import { findRepoRoot } from "./runner";
import { GET as doctorGet } from "@/app/api/recompiler/doctor/route";

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
