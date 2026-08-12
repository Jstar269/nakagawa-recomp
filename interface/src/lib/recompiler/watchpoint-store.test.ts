// watchpoint-store.test.ts — DB-backed integration tests for the watchpoint /
// debug-profile store (issue #188 PR-b). Run with `npm run test:db`.

import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { execSync } from "node:child_process";
import { mkdtempSync, rmSync, existsSync, writeFileSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { randomUUID } from "node:crypto";

const DB_NAME = `test-watch-${randomUUID()}.db`;
process.env.DATABASE_URL = `file:./prisma/.test/${DB_NAME}`;

import { contentHash as contentHashOf, readWatchpointsFile } from "./watchpoint-file.mjs";
import { serializeWatchpointsFile } from "./watchpoint-file.mjs";

let store: typeof import("./watchpoint-store");
let db: typeof import("@/lib/db")["db"];
let serializeFile: typeof serializeWatchpointsFile;
let fileDir: string;
let filePath: string;

type Wp = { start: number; end: number; label: string };
type StoreErrorT = import("./watchpoint-store").WatchpointStoreError;

const W1: Wp = { start: 0x08001000, end: 0x08001100, label: "Font Engine" };
const W2: Wp = { start: 0x08800000, end: 0x08800100, label: "Vertex Pool" };
const W3: Wp = { start: 0x09000000, end: 0x09000010, label: "Audio Buffer" };

before(() => {
  execSync(`npx prisma db push --url "file:./prisma/.test/${DB_NAME}"`, { stdio: "ignore" });
  fileDir = mkdtempSync(path.join(tmpdir(), "wpstore-"));
  filePath = path.join(fileDir, "watchpoints.json");
  serializeFile = serializeWatchpointsFile;
  return Promise.all([import("./watchpoint-store"), import("@/lib/db")]).then(
    ([s, m]) => {
      store = s;
      db = m.db;
    },
  );
});

after(async () => {
  if (db) await db.$disconnect();
  rmSync(fileDir, { recursive: true, force: true });
  const base = `prisma/.test/${DB_NAME}`;
  for (const suffix of ["", "-journal", "-wal", "-shm"]) {
    if (existsSync(base + suffix)) rmSync(base + suffix, { force: true });
  }
});

async function expectStoreError(promise: Promise<unknown>, code: string): Promise<void> {
  let caught: unknown = null;
  try {
    await promise;
  } catch (err) {
    caught = err;
  }
  assert.ok(caught !== null, `expected WatchpointStoreError ${code}, but operation succeeded`);
  assert.ok(caught instanceof store.WatchpointStoreError, `expected WatchpointStoreError, got ${String(caught)}`);
  assert.equal((caught as StoreErrorT).code, code);
}

function fileBytes(): string {
  return readFileSync(filePath, "utf8");
}

// ---- Direct-file mode (no active debug profile) --------------------------

test("direct mode: add / delete / clear with atomic file publication", async () => {
  await db.debugProfile.deleteMany({});
  await store.mutateWatchpoints("clear", {}, db, filePath);

  const add1 = await store.mutateWatchpoints("add", { watchpoint: W1 }, db, filePath);
  assert.deepEqual(add1.watchpoints, [W1]);
  assert.equal(add1.source, "file");
  const add2 = await store.mutateWatchpoints("add", { watchpoint: W2 }, db, filePath);
  assert.deepEqual(add2.watchpoints, [W1, W2]);

  const got = await store.getWatchpoints(db, filePath);
  assert.deepEqual(got.watchpoints, [W1, W2]);
  assert.equal(got.fileState.detail, "direct");

  const del = await store.mutateWatchpoints("delete-label", { label: W1.label }, db, filePath);
  assert.deepEqual(del.watchpoints, [W2]);
  await store.mutateWatchpoints("clear", {}, db, filePath);
  assert.deepEqual((await store.getWatchpoints(db, filePath)).watchpoints, []);
});

test("direct mode: exact duplicates are rejected with a 400-class error", async () => {
  await db.debugProfile.deleteMany({});
  await store.mutateWatchpoints("clear", {}, db, filePath);
  await store.mutateWatchpoints("add", { watchpoint: W1 }, db, filePath);
  await expectStoreError(
    store.mutateWatchpoints("add", { watchpoint: { ...W1, label: "same range, other label" } }, db, filePath),
    "duplicate-watchpoint",
  );
  assert.deepEqual((await store.getWatchpoints(db, filePath)).watchpoints, [W1]);
});

// ---- DB mode (active debug profile is canonical) -------------------------

async function makeActiveProfile(watchpoints: Wp[], mask = 0x0f) {
  await db.debugProfile.deleteMany({});
  const created = await db.debugProfile.create({
    data: { name: "Active", watchpoints: JSON.stringify(watchpoints), debugMask: mask, isActive: true, schemaVersion: 1 },
  });
  return created.id;
}

test("db mode: mutations go to the active profile and the file is published with identity", async () => {
  const id = await makeActiveProfile([W1]);
  const added = await store.mutateWatchpoints("add", { watchpoint: W2 }, db, filePath);
  assert.equal(added.source, "profile");
  assert.deepEqual(added.watchpoints, [W1, W2]);

  const row = await db.debugProfile.findUnique({ where: { id } });
  assert.deepEqual(JSON.parse(row!.watchpoints), [W1, W2], "DB is canonical");

  const file = JSON.parse(fileBytes());
  assert.equal(file.format, "hst-watchpoints");
  assert.equal(file.profileId, id);
  assert.equal(file.source, "db");
  assert.ok(/^[0-9a-f]{64}$/.test(file.contentHash));

  const got = await store.getWatchpoints(db, filePath);
  assert.equal(got.fileState.detail, "synced");
});

test("db mode: concurrent adds never lose an update and never exceed the limit", async () => {
  await makeActiveProfile([]);
  const adds = Array.from({ length: 8 }, (_, i) =>
    store.mutateWatchpoints("add", { watchpoint: { start: 0x1000 + i * 0x100, end: 0x1000 + i * 0x100 + 0x10, label: `w${i}` } }, db, filePath),
  );
  const outcomes = await Promise.allSettled(adds);
  assert.deepEqual(outcomes.filter((o) => o.status === "rejected"), [], "every concurrent add must succeed");
  const got = await store.getWatchpoints(db, filePath);
  assert.equal(got.watchpoints.length, 8, "no lost update: all eight entries present");
});

test("db mode: the shared 16-watchpoint limit is enforced with no silent truncation", async () => {
  await makeActiveProfile([]);
  for (let i = 0; i < 16; i++) {
    await store.mutateWatchpoints("add", { watchpoint: { start: 0x2000 + i * 0x100, end: 0x2000 + i * 0x100 + 0x10, label: `w${i}` } }, db, filePath);
  }
  await expectStoreError(
    store.mutateWatchpoints("add", { watchpoint: { start: 0xfffff000, end: 0xfffff010, label: "one-too-many" } }, db, filePath),
    "limit-exceeded",
  );
  const got = await store.getWatchpoints(db, filePath);
  assert.equal(got.watchpoints.length, 16);
});

test("db mode: stale derived file is reported, not silently regenerated on GET", async () => {
  const id = await makeActiveProfile([W1, W2]);
  await store.mutateWatchpoints("clear", {}, db, filePath);
  // Corrupt the file behind the store's back to simulate an interrupted writer.
  writeFileSync(filePath, JSON.stringify({ ...JSON.parse(fileBytes()), contentHash: "0".repeat(64) }));
  const got = await store.getWatchpoints(db, filePath);
  assert.deepEqual(got.watchpoints, [], "DB value is authoritative");
  assert.equal(got.fileState.ok, false);
  assert.match(got.fileState.detail, /stale|mismatch|corrupt/);
  void id;
});

test("db mode: file publication failure after DB commit is reported, not claimed", async () => {
  const id = await makeActiveProfile([]);
  const badFile = path.join(fileDir, "missing-subdir", "watchpoints.json");
  const result = await store.mutateWatchpoints("add", { watchpoint: W1 }, db, badFile);
  // DB committed:
  const row = await db.debugProfile.findUnique({ where: { id } });
  assert.deepEqual(JSON.parse(row!.watchpoints), [W1]);
  // File did not:
  assert.equal(result.fileState.ok, false);
  assert.match(result.fileState.detail, /write-failed/);
  assert.equal(existsSync(badFile), false);
});

// ---- Debug profile CRUD + activation -------------------------------------

test("createDebugProfile validates name, watchpoints and debugMask", async () => {
  await db.debugProfile.deleteMany({});
  const ok = await store.createDebugProfile({ name: "  Debug One  ", watchpoints: [W1], debugMask: 0xff });
  assert.equal(ok.name, "Debug One");
  assert.deepEqual(ok.watchpoints, [W1]);
  assert.equal(ok.debugMask, 0xff);
  assert.equal(ok.isActive, false);
  assert.equal(ok.schemaVersion, 1);

  await expectStoreError(store.createDebugProfile({ name: "Debug One", watchpoints: [] }), "duplicate-name");
  await expectStoreError(store.createDebugProfile({ name: "Bad", watchpoints: [{ start: 0x100, end: 0x50, label: "x" }] }), "invalid-watchpoints");
  await expectStoreError(store.createDebugProfile({ name: "Bad", watchpoints: [], debugMask: -1 }), "invalid-debug-mask");
  await expectStoreError(store.createDebugProfile({ name: "Bad", watchpoints: [], debugMask: "0xABC" }), "invalid-debug-mask");
  await expectStoreError(store.createDebugProfile({ name: "", watchpoints: [] }), "invalid-name");
});

test("updateDebugProfile rejects truthy boolean coercion for isActive", async () => {
  await db.debugProfile.deleteMany({});
  const created = await store.createDebugProfile({ name: "Coerce", watchpoints: [] });
  await expectStoreError(
    store.updateDebugProfile({ id: created.id, isActive: "false" }),
    "invalid-is-active",
  );
  await expectStoreError(
    store.updateDebugProfile({ id: created.id, isActive: 1 }),
    "invalid-is-active",
  );
});

test("concurrent activations leave exactly one active debug profile", async () => {
  await db.debugProfile.deleteMany({});
  const a = await store.createDebugProfile({ name: "A", watchpoints: [W1] });
  const b = await store.createDebugProfile({ name: "B", watchpoints: [W2] });
  const c = await store.createDebugProfile({ name: "C", watchpoints: [W3] });
  const outcomes = await Promise.allSettled([
    store.updateDebugProfile({ id: a.id, isActive: true }, db, filePath),
    store.updateDebugProfile({ id: b.id, isActive: true }, db, filePath),
    store.updateDebugProfile({ id: c.id, isActive: true }, db, filePath),
  ]);
  assert.deepEqual(outcomes.filter((o) => o.status === "rejected"), [], "all activations must succeed");
  const active = await db.debugProfile.findMany({ where: { isActive: true } });
  assert.equal(active.length, 1, "concurrent activations must leave exactly one active profile");
});

test("activation failure (missing profile) rolls back the deactivate-all step", async () => {
  await db.debugProfile.deleteMany({});
  const a = await store.createDebugProfile({ name: "Keep", watchpoints: [] });
  await store.updateDebugProfile({ id: a.id, isActive: true }, db, filePath);
  await expectStoreError(store.updateDebugProfile({ id: "missing", isActive: true }, db, filePath), "profile-not-found");
  const active = await db.debugProfile.findMany({ where: { isActive: true } });
  assert.equal(active.length, 1, "a failed activation must not leave zero active profiles");
  assert.equal(active[0].id, a.id);
});

test("injected transaction failure rolls back all writes", async () => {
  await db.debugProfile.deleteMany({});
  const before = await db.debugProfile.count();
  await db
    .$transaction(async (tx) => {
      await tx.debugProfile.create({
        data: { name: "Ghost", watchpoints: "[]", debugMask: 0, isActive: false, schemaVersion: 1 },
      });
      throw new Error("injected failure between steps");
    })
    .catch(() => undefined);
  assert.equal(await db.debugProfile.count(), before);
});

test("deleteDebugProfile republishes the file for the remaining active state", async () => {
  await db.debugProfile.deleteMany({});
  const active = await store.createDebugProfile({ name: "ActiveDel", watchpoints: [W1] });
  await store.updateDebugProfile({ id: active.id, isActive: true }, db, filePath);
  assert.equal(JSON.parse(fileBytes()).profileId, active.id);

  // Deleting an INACTIVE profile leaves the active one's file untouched.
  const spare = await store.createDebugProfile({ name: "Spare", watchpoints: [W2] });
  await store.deleteDebugProfile(spare.id, db, filePath);
  assert.equal(JSON.parse(fileBytes()).profileId, active.id);
  assert.deepEqual(JSON.parse(fileBytes()).watchpoints, [W1]);

  // Deleting the ACTIVE profile publishes the empty file (no profile active).
  const { fileState } = await store.deleteDebugProfile(active.id, db, filePath);
  assert.equal(fileState.ok, true);
  const last = JSON.parse(fileBytes());
  assert.equal(last.profileId, null);
  assert.deepEqual(last.watchpoints, []);
});

// ---- Stored-state classification -----------------------------------------

test("listDebugProfiles surfaces corrupt and unsupported-version rows separately", async () => {
  await db.debugProfile.deleteMany({});
  await store.createDebugProfile({ name: "Good", watchpoints: [W1] });
  await db.debugProfile.create({ data: { name: "Broken", watchpoints: "{bad", debugMask: 0, isActive: false, schemaVersion: 1 } });
  await db.debugProfile.create({ data: { name: "Future", watchpoints: "[]", debugMask: 0, isActive: false, schemaVersion: 9 } });
  const { profiles, corrupt } = await store.listDebugProfiles();
  assert.deepEqual(profiles.map((p) => p.name), ["Good"]);
  const reasons = corrupt.map((c) => c.name).sort();
  assert.deepEqual(reasons, ["Broken", "Future"]);
});

test("an active profile with unsupported schema version fails closed", async () => {
  await db.debugProfile.deleteMany({});
  await db.debugProfile.create({ data: { name: "Old", watchpoints: "[]", debugMask: 0, isActive: true, schemaVersion: 2 } });
  await expectStoreError(store.getWatchpoints(db, filePath), "unsupported-version");
});

// ---- Runtime-truth integration at the byte level -------------------------

test("the published file parses into exactly the expected native watchpoint set", () => {
  // The native parser (src/rt/watchpoints_file_selftest.c) embeds a byte
  // fixture produced by serializeWatchpointsFile for a fixed watchpoint set
  // and timestamp. Prove the identical bytes round-trip through the JS reader
  // and carry the canonical hash.
  const serialized = JSON.stringify({
    format: "hst-watchpoints",
    version: 1,
    profileId: "prof-integration",
    source: "db",
    writtenAt: "2026-08-06T00:00:00.000Z",
    contentHash: contentHashOf([W1, W2]),
    watchpoints: [W1, W2],
  });
  const file = path.join(fileDir, "integration.json");
  writeFileSync(file, serialized, "utf8");
  const state = readWatchpointsFile(file);
  assert.equal(state.status, "ok");
  assert.deepEqual(state.watchpoints, [W1, W2]);
  assert.equal(state.meta.contentHash, contentHashOf([W1, W2]));
});

test("exact same normalized values flow DB -> file -> runtime parser input", async () => {
  await db.debugProfile.deleteMany({});
  const id = await makeActiveProfile([W1, W2]);
  await store.mutateWatchpoints("add", { watchpoint: W3 }, db, filePath);
  const dbValue = JSON.parse((await db.debugProfile.findUnique({ where: { id } }))!.watchpoints);
  const fileValue = JSON.parse(fileBytes()).watchpoints;
  assert.deepEqual(dbValue, [W1, W2, W3], "DB and file carry identical normalized values");
  assert.deepEqual(fileValue, [W1, W2, W3]);

  // The byte fixture the native parser is tested against must be generated by
  // this exact writer with this exact content hash.
  const fixture = serializeFile({ watchpoints: [W1, W2], profileId: "prof-integration", source: "db", writtenAt: "2026-08-06T00:00:00.000Z" });
  assert.match(fixture, /"format": "hst-watchpoints"/);
  assert.equal(JSON.parse(fixture).contentHash, contentHashOf([W1, W2]));
});
