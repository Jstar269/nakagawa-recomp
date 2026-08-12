import assert from "node:assert/strict";
import test from "node:test";
import { parseProgressSnapshot } from "./progress-snapshot.mjs";

const NOW_MS = 1_000_000_000_000;
const FRESH_MTIME_MS = NOW_MS - 10_000; // 10s ago → fresh
const STALE_MTIME_MS = NOW_MS - 120_000; // 2m ago → not fresh

test("degrades a legacy pre-#181 snapshot to the old shape without provenance", () => {
  const snap = parseProgressSnapshot(
    {
      total_units: 100,
      units_earned: 40,
      units_regressed: 2,
      phases: [{ id: 1, title: "Pipeline", earned: 10, pending: 5, regressed: 0 }],
    },
    { nowMs: NOW_MS, fileMtimeMs: FRESH_MTIME_MS },
  );
  assert.equal(snap.total, 100);
  assert.equal(snap.earned, 40);
  assert.equal(snap.regressed, 2);
  assert.equal(snap.percent, 38); // (40 - 2) / 100
  assert.equal(snap.phases.length, 1);
  assert.equal(snap.phases[0].id, "1");
  assert.equal(snap.fresh, true);
  assert.equal(snap.runIdentity, null);
  assert.equal(snap.evidenceGrade, null);
  assert.equal(snap.itemsCarryEvidence, false);
  assert.deepEqual(snap.evidenceSummary, { contentValidated: 0, executed: 0, heuristic: 0, unknown: 0, stale: 0 });
});

test("binds run identity and carries run-level evidence grade", () => {
  const snap = parseProgressSnapshot(
    {
      total_units: 10,
      units_earned: 10,
      run: {
        evidence_grade: "content-validated",
        stale_vs_build: false,
        identity: {
          source_commit: "21af483abc",
          binary_sha256: "aa".repeat(32),
          profile_sha256: "bb".repeat(32),
          generated_at: "2026-08-05T00:00:00Z",
        },
      },
    },
    { nowMs: NOW_MS, fileMtimeMs: FRESH_MTIME_MS },
  );
  assert.deepEqual(snap.runIdentity, {
    sourceCommit: "21af483abc",
    binarySha256: "aa".repeat(32),
    profileSha256: "bb".repeat(32),
    generatedAt: "2026-08-05T00:00:00Z",
  });
  assert.equal(snap.evidenceGrade, "content-validated");
  assert.equal(snap.staleVsBuild, false);
});

test("surfaces stale_vs_build so the dashboard can refuse stale proof", () => {
  const snap = parseProgressSnapshot(
    { run: { stale_vs_build: true, evidence_grade: "stale" } },
    { nowMs: NOW_MS, fileMtimeMs: STALE_MTIME_MS },
  );
  assert.equal(snap.staleVsBuild, true);
  assert.equal(snap.evidenceGrade, "stale");
  assert.equal(snap.fresh, false);
});

test("sums per-item evidence grades and ignores unknown labels", () => {
  const snap = parseProgressSnapshot(
    {
      items: [
        { id: "P1.1", evidence: "content-validated" },
        { id: "P1.2", evidence: "executed" },
        { id: "P1.3", evidence: "heuristic" },
        { id: "P1.4", evidence: "unknown" },
        { id: "P1.5", evidence: "stale" },
        { id: "P1.6", evidence: "future-grade-name" }, // must not crash or count
        { id: "P1.7" }, // no evidence field at all
      ],
    },
    { nowMs: NOW_MS, fileMtimeMs: FRESH_MTIME_MS },
  );
  assert.equal(snap.itemsCarryEvidence, true);
  assert.deepEqual(snap.evidenceSummary, {
    contentValidated: 1,
    executed: 1,
    heuristic: 1,
    unknown: 1,
    stale: 1,
  });
});

test("is fresh only when mtime is within the window", () => {
  const base = { total_units: 5, units_earned: 5 };
  assert.equal(parseProgressSnapshot(base, { nowMs: NOW_MS, fileMtimeMs: FRESH_MTIME_MS }).fresh, true);
  assert.equal(parseProgressSnapshot(base, { nowMs: NOW_MS, fileMtimeMs: STALE_MTIME_MS }).fresh, false);
  assert.equal(parseProgressSnapshot(base, { nowMs: NOW_MS, fileMtimeMs: 0 }).fresh, false);
});

test("rejects non-object run/identity blocks instead of throwing", () => {
  const snap = parseProgressSnapshot(
    { run: "not-an-object", items: "also-not-an-array" },
    { nowMs: NOW_MS, fileMtimeMs: FRESH_MTIME_MS },
  );
  assert.equal(snap.runIdentity, null);
  assert.equal(snap.evidenceGrade, null);
  assert.equal(snap.itemsCarryEvidence, false);
  assert.equal(snap.phases.length, 0);
});

test("handles the by_phase object form", () => {
  const snap = parseProgressSnapshot(
    {
      by_phase: { P1: { earned: 6, total: 10, regressed: 1 } },
    },
    { nowMs: NOW_MS, fileMtimeMs: FRESH_MTIME_MS },
  );
  assert.equal(snap.phases.length, 1);
  assert.equal(snap.phases[0].id, "P1");
  assert.equal(snap.phases[0].earned, 6);
  assert.equal(snap.phases[0].pending, 3);
  assert.equal(snap.phases[0].regressed, 1);
});

test("computes percent when completion_pct is absent and total is zero", () => {
  assert.equal(parseProgressSnapshot({}, { nowMs: NOW_MS, fileMtimeMs: FRESH_MTIME_MS }).percent, 0);
});

test("degrades garbage numeric strings to finite zeros instead of NaN", () => {
  const snap = parseProgressSnapshot(
    {
      total_units: "abc",
      units_earned: "def",
      completion_pct: "nope",
      phases: [{ id: 1, earned: "x", pending: "y", regressed: "z" }],
    },
    { nowMs: NOW_MS, fileMtimeMs: FRESH_MTIME_MS },
  );
  assert.equal(snap.total, 0);
  assert.equal(snap.earned, 0);
  assert.equal(snap.percent, 0);
  assert.ok(Number.isFinite(snap.percent));
  assert.equal(snap.phases[0].earned, 0);
  assert.equal(snap.phases[0].pending, 0);
  assert.equal(snap.phases[0].regressed, 0);
});

test("clamps a non-finite completion_pct instead of surfacing it", () => {
  const snap = parseProgressSnapshot(
    { total_units: 10, units_earned: 5, completion_pct: Number.POSITIVE_INFINITY },
    { nowMs: NOW_MS, fileMtimeMs: FRESH_MTIME_MS },
  );
  assert.equal(snap.percent, 0);
  assert.ok(Number.isFinite(snap.percent));
});
