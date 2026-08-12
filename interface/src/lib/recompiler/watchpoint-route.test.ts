// watchpoint-route.test.ts — DB-backed tests that exercise the real watchpoint
// API route handlers (watchpoints + watchpoints/profiles) against a throwaway
// SQLite database and a temp derived artifact (SR_WATCHPOINTS_FILE). Run with
// `npm run test:db`.

import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { execSync } from "node:child_process";
import { mkdtempSync, rmSync, existsSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { randomUUID } from "node:crypto";
import { NextRequest } from "next/server";

const DB_NAME = `test-wproute-${randomUUID()}.db`;
process.env.DATABASE_URL = `file:./prisma/.test/${DB_NAME}`;

let filePath: string;
let watchpointsRoute: typeof import("@/app/api/recompiler/watchpoints/route");
let profilesRoute: typeof import("@/app/api/recompiler/watchpoints/profiles/route");
let db: typeof import("@/lib/db")["db"];

before(() => {
  execSync(`npx prisma db push --url "file:./prisma/.test/${DB_NAME}"`, { stdio: "ignore" });
  const dir = mkdtempSync(path.join(tmpdir(), "wproute-"));
  filePath = path.join(dir, "watchpoints.json");
  process.env.SR_WATCHPOINTS_FILE = filePath;
  return Promise.all([
    import("@/app/api/recompiler/watchpoints/route"),
    import("@/app/api/recompiler/watchpoints/profiles/route"),
    import("@/lib/db"),
  ]).then(([w, p, m]) => {
    watchpointsRoute = w;
    profilesRoute = p;
    db = m.db;
  });
});

after(async () => {
  delete process.env.SR_WATCHPOINTS_FILE;
  if (db) await db.$disconnect();
  rmSync(path.dirname(filePath), { recursive: true, force: true });
  const base = `prisma/.test/${DB_NAME}`;
  for (const suffix of ["", "-journal", "-wal", "-shm"]) {
    if (existsSync(base + suffix)) rmSync(base + suffix, { force: true });
  }
});

function json(method: string, url: string, body?: unknown): NextRequest {
  return new NextRequest(url, {
    method,
    body: body === undefined ? undefined : JSON.stringify(body),
    headers: { "content-type": "application/json" },
  });
}

async function bodyOf(res: Response): Promise<Record<string, unknown>> {
  return (await res.json()) as Record<string, unknown>;
}

function wp(start: number, end: number, label: string) {
  return { start, end, label };
}

// ---- Direct-file mode through the real routes ----------------------------

test("watchpoint POST/DELETE/GET flow with strict parsing and shared limit", async () => {
  await db.debugProfile.deleteMany({});

  // Partial-string numeric input is rejected with 400.
  let res = await watchpointsRoute.POST(json("POST", "http://localhost/api/recompiler/watchpoints", {
    start: "0x100junk", end: "0x200", label: "bad",
  }));
  let body = await bodyOf(res);
  assert.equal(res.status, 400);
  assert.match(body.message as string, /start/);

  // Valid add.
  res = await watchpointsRoute.POST(json("POST", "http://localhost/api/recompiler/watchpoints", {
    start: "0x08001000", end: "0x08001100", label: "Font Engine",
  }));
  body = await bodyOf(res);
  assert.equal(res.status, 200);
  assert.equal(body.success, true);
  assert.equal((body.watchpoints as unknown[]).length, 1);
  assert.equal(body.source, "file");
  // Direct-file mode: the write succeeded, so the artifact is honestly synced.
  assert.equal((body.fileState as { detail: string }).detail, "synced");

  // Duplicate range rejected.
  res = await watchpointsRoute.POST(json("POST", "http://localhost/api/recompiler/watchpoints", {
    start: "0x08001000", end: "0x08001100", label: "other label",
  }));
  body = await bodyOf(res);
  assert.equal(res.status, 400);
  assert.equal(body.error, "duplicate-watchpoint");

  // Delete by start.
  res = await watchpointsRoute.DELETE(json("DELETE", "http://localhost/api/recompiler/watchpoints?start=0x08001000"));
  body = await bodyOf(res);
  assert.equal(res.status, 200);
  assert.equal((body.watchpoints as unknown[]).length, 0);

  // The derived file exists, is an envelope, and carries a hash.
  const file = JSON.parse(readFileSync(filePath, "utf8"));
  assert.equal(file.format, "hst-watchpoints");
  assert.match(file.contentHash as string, /^[0-9a-f]{64}$/);
});

test("watchpoint GET reports a stale artifact without regenerating it", async () => {
  await db.debugProfile.deleteMany({});
  // Active profile: DB is canonical, so a drifted file is reported, not fixed.
  const created = await db.debugProfile.create({
    data: { name: "Active", watchpoints: JSON.stringify([{ start: 0x1000, end: 0x1100, label: "A" }]), debugMask: 0, isActive: true, schemaVersion: 1 },
  });
  await watchpointsRoute.GET(); // publishes the derived file for the active profile
  const parsed = JSON.parse(readFileSync(filePath, "utf8")) as { contentHash: string };
  parsed.contentHash = "0".repeat(64);
  writeFileSync(filePath, JSON.stringify(parsed), "utf8");

  const res = await watchpointsRoute.GET();
  const body = await bodyOf(res);
  assert.equal(res.status, 200);
  assert.equal((body.fileState as { ok: boolean }).ok, false);
  assert.match((body.fileState as { detail: string }).detail, /stale|mismatch|corrupt/);
  assert.equal(readFileSync(filePath, "utf8"), JSON.stringify(parsed), "GET must not regenerate the file");
  await db.debugProfile.deleteMany({});
  void created;
});

// ---- Debug-profile routes -------------------------------------------------

test("debug profile create/activate/delete flow with strict validation", async () => {
  await db.debugProfile.deleteMany({});

  // String boolean coercion is rejected.
  let res: Response = await profilesRoute.PUT(json("PUT", "http://localhost/api/recompiler/watchpoints/profiles", {
    id: "anything", isActive: "true",
  }));
  let body = await bodyOf(res);
  assert.equal(res.status, 400);
  assert.equal(body.error, "invalid-is-active");

  // Create.
  res = await profilesRoute.POST(json("POST", "http://localhost/api/recompiler/watchpoints/profiles", {
    name: "Debug A", watchpoints: [wp(0x1000, 0x1100, "A")], debugMask: 0x1f,
  }));
  body = await bodyOf(res);
  assert.equal(res.status, 200);
  const profile = body.profile as { id: string; name: string; schemaVersion: number };
  assert.equal(profile.name, "Debug A");
  assert.equal(profile.schemaVersion, 1);

  // Duplicate name.
  res = await profilesRoute.POST(json("POST", "http://localhost/api/recompiler/watchpoints/profiles", { name: "Debug A" }));
  body = await bodyOf(res);
  assert.equal(res.status, 400);
  assert.equal(body.error, "duplicate-name");

  // Invalid mask.
  res = await profilesRoute.POST(json("POST", "http://localhost/api/recompiler/watchpoints/profiles", {
    name: "BadMask", debugMask: "0xABC",
  }));
  assert.equal(res.status, 400);

  // Activate.
  res = await profilesRoute.PUT(json("PUT", "http://localhost/api/recompiler/watchpoints/profiles", {
    id: profile.id, isActive: true,
  }));
  body = await bodyOf(res);
  assert.equal(res.status, 200);
  assert.equal((body.profile as { isActive: boolean }).isActive, true);
  assert.equal((body.fileState as { ok: boolean }).ok, true);

  // The derived file now carries the profile identity (DB -> file).
  const file = JSON.parse(readFileSync(filePath, "utf8"));
  assert.equal(file.profileId, profile.id);
  assert.equal(file.source, "db");

  // GET now reports profile source.
  const get = await watchpointsRoute.GET();
  const got = await bodyOf(get);
  assert.equal(got.source, "profile");
  assert.equal((got.fileState as { detail: string }).detail, "synced");

  // Delete the active profile -> empty file.
  res = await profilesRoute.DELETE(json("DELETE", `http://localhost/api/recompiler/watchpoints/profiles?id=${profile.id}`));
  body = await bodyOf(res);
  assert.equal(res.status, 200);
  const cleared = JSON.parse(readFileSync(filePath, "utf8"));
  assert.equal(cleared.profileId, null);
  assert.deepEqual(cleared.watchpoints, []);
});

test("corrupt debug profiles are reported separately and never crash the list", async () => {
  await db.debugProfile.deleteMany({});
  await profilesRoute.POST(json("POST", "http://localhost/api/recompiler/watchpoints/profiles", { name: "Good" }));
  await db.debugProfile.create({
    data: { name: "Broken", watchpoints: "{bad", debugMask: 0, isActive: false, schemaVersion: 1 },
  });
  const res = await profilesRoute.GET();
  const body = await bodyOf(res);
  assert.equal(res.status, 200);
  assert.equal((body.profiles as unknown[]).length, 1);
  assert.deepEqual((body.corrupt as { name: string }[]).map((c) => c.name), ["Broken"]);
});
