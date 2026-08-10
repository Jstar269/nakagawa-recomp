// profile-store.test.ts — DB-backed integration tests for the transactional
// profile store (issue #188). Runs against a throwaway SQLite file; the schema
// is pushed with `prisma db push` in the setup hook. Use `npm run test:db`.
//
// Each test file runs in its own process under node --test, so this file owns
// its DATABASE_URL / Prisma singleton entirely.

import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { execSync } from "node:child_process";
import { rmSync, existsSync } from "node:fs";
import { randomUUID } from "node:crypto";

const DB_NAME = `test-profile-${randomUUID()}.db`;
const DB_URL = `file:./prisma/.test/${DB_NAME}`;

// Must be set before the store module (and its @/lib/db import) is evaluated.
process.env.DATABASE_URL = DB_URL;

type ProfileStoreErrorT = import("./profile-store").ProfileStoreError;

let store: typeof import("./profile-store");
let db: typeof import("@/lib/db")["db"];

before(() => {
  execSync(`npx prisma db push --url "${DB_URL}"`, { stdio: "ignore" });
  return import("./profile-store")
    .then((m) => {
      store = m;
      return import("@/lib/db");
    })
    .then((m) => {
      db = m.db;
    });
});

after(async () => {
  if (db) await db.$disconnect();
  const base = `prisma/.test/${DB_NAME}`;
  for (const suffix of ["", "-journal", "-wal", "-shm"]) {
    if (existsSync(base + suffix)) rmSync(base + suffix, { force: true });
  }
});

import { defaultConfig } from "./defaults";

function validConfig(name = "Test Profile") {
  const cfg = defaultConfig("minimal");
  cfg.profileName = name;
  return cfg;
}

async function expectStoreError(promise: Promise<unknown>, code: string): Promise<void> {
  let caught: unknown = null;
  try {
    await promise;
  } catch (err) {
    caught = err;
  }
  assert.ok(caught !== null, `expected ProfileStoreError with code ${code}, but operation succeeded`);
  assert.ok(caught instanceof store.ProfileStoreError, `expected ProfileStoreError, got ${String(caught)}`);
  assert.equal((caught as ProfileStoreErrorT).code, code);
}

async function seedCorruptRow(name: string, configJson: string, schemaVersion = 1) {
  await db.recompilerProfile.create({
    data: { name, configJson, isDefault: false, schemaVersion },
  });
}

// ---- Upsert / get active -------------------------------------------------

test("upsertActiveProfile creates the first active profile", async () => {
  const { row, config } = await store.upsertActiveProfile({ config: validConfig("First") });
  assert.equal(row.name, "First");
  assert.equal(row.isDefault, true);
  assert.equal(row.schemaVersion, 1);
  assert.equal((config as { profileName: string }).profileName, "First");

  const active = await store.getActiveProfile();
  assert.equal(active?.row.id, row.id);
});

test("upsertActiveProfile updates the same row instead of creating a second default", async () => {
  await store.upsertActiveProfile({ config: validConfig("One") });
  await store.upsertActiveProfile({ config: validConfig("Two") });
  const rows = await db.recompilerProfile.findMany();
  assert.equal(rows.length, 1);
  assert.equal(rows[0].name, "Two");
  const defaults = await db.recompilerProfile.count({ where: { isDefault: true } });
  assert.equal(defaults, 1);
});

test("upsertActiveProfile rejects invalid configs before touching the database", async () => {
  const before = await db.recompilerProfile.count();
  const bad = validConfig();
  (bad.graphics as unknown as { framePacing: string }).framePacing = "yes";
  await expectStoreError(store.upsertActiveProfile({ config: bad }), "validation-failed");
  const after = await db.recompilerProfile.count();
  assert.equal(after, before, "no row may be written for an invalid config");
});

test("upsertActiveProfile rejects invalid names", async () => {
  await expectStoreError(store.upsertActiveProfile({ config: validConfig(), name: "   " }), "empty-name");
  await expectStoreError(store.upsertActiveProfile({ config: validConfig(), name: 42 }), "invalid-name");
});

// ---- Corrupt / unsupported stored rows -----------------------------------

test("getActiveProfile reports a corrupt stored row instead of returning a default", async () => {
  await store.upsertActiveProfile({ config: validConfig("Good") });
  await db.recompilerProfile.updateMany({
    where: { isDefault: true },
    data: { configJson: "{not json" },
  });
  await expectStoreError(store.getActiveProfile(), "corrupt");
  // Cleanup: repair the row so later tests see a valid active profile.
  await db.recompilerProfile.updateMany({
    where: { isDefault: true },
    data: { configJson: JSON.stringify(validConfig("Repaired")), schemaVersion: 1 },
  });
});

test("getActiveProfile reports unsupported-version for a future schemaVersion", async () => {
  await store.upsertActiveProfile({ config: validConfig("Old") });
  await db.recompilerProfile.updateMany({
    where: { isDefault: true },
    data: { schemaVersion: 2 },
  });
  await expectStoreError(store.getActiveProfile(), "unsupported-version");
  await db.recompilerProfile.updateMany({
    where: { isDefault: true },
    data: { schemaVersion: 1 },
  });
});

test("listProfiles reports corrupt rows separately instead of failing the list", async () => {
  await store.upsertActiveProfile({ config: validConfig("Healthy") });
  await seedCorruptRow("Broken", "{bad json");
  await seedCorruptRow("Future", JSON.stringify(validConfig("Future")), 9);
  const { profiles, corrupt } = await store.listProfiles();
  const names = profiles.map((p) => p.name);
  assert.ok(names.includes("Healthy"));
  assert.ok(!names.includes("Broken"));
  assert.ok(!names.includes("Future"));
  const corruptNames = corrupt.map((c) => c.name).sort();
  assert.deepEqual(corruptNames, ["Broken", "Future"]);
  assert.equal(profiles.find((p) => p.name === "Healthy")?.summary?.resolution, "1080p");
});

// ---- Create / duplicate --------------------------------------------------

test("createProfile makes a non-default profile", async () => {
  await store.upsertActiveProfile({ config: validConfig("Active") });
  const { row } = await store.createProfile({ name: "Saved", config: validConfig("Saved") });
  assert.equal(row.isDefault, false);
  const defaults = await db.recompilerProfile.count({ where: { isDefault: true } });
  assert.equal(defaults, 1, "creating a profile must not disturb the active one");
});

test("duplicateFrom with a missing source is a hard error, not a silent fallback", async () => {
  await expectStoreError(
    store.createProfile({ name: "Copy", duplicateFrom: "does-not-exist" }),
    "duplicate-source-not-found",
  );
});

test("duplicateFrom with a corrupt source is rejected", async () => {
  const corrupted = await db.recompilerProfile.create({
    data: { name: "BrokenSrc", configJson: "[]", isDefault: false, schemaVersion: 1 },
  });
  await expectStoreError(
    store.createProfile({ name: "Copy", duplicateFrom: corrupted.id }),
    "duplicate-source-corrupt",
  );
});

test("duplicateFrom copies a valid source config", async () => {
  const src = await store.createProfile({ name: "Original", config: validConfig("Original") });
  const dup = await store.createProfile({ name: "Original (copy)", duplicateFrom: src.row.id });
  const srcConfig = (await store.getProfileById(src.row.id)).config as Record<string, unknown>;
  const dupConfig = dup.config as Record<string, unknown>;
  assert.equal(srcConfig.minimizeStrategy, dupConfig.minimizeStrategy);
  assert.equal(dup.row.name, "Original (copy)");
  assert.equal((dupConfig as { profileName: string }).profileName, "Original (copy)");
});

// ---- Transactional activation --------------------------------------------

test("concurrent activations all succeed and leave exactly one active profile", async () => {
  await db.recompilerProfile.deleteMany({});
  const a = await store.createProfile({ name: "A", config: validConfig("A") });
  const b = await store.createProfile({ name: "B", config: validConfig("B") });
  const c = await store.createProfile({ name: "C", config: validConfig("C") });

  const outcomes = await Promise.allSettled([
    store.activateProfile(a.row.id),
    store.activateProfile(b.row.id),
    store.activateProfile(c.row.id),
  ]);
  const failures = outcomes.filter((o) => o.status === "rejected");
  assert.deepEqual(failures, [], `every activation must succeed, got ${failures.length} failures`);

  const defaults = await db.recompilerProfile.count({ where: { isDefault: true } });
  assert.equal(defaults, 1, "concurrent activations must leave exactly one default");
  await db.recompilerProfile.deleteMany({});
});

test("concurrent upserts with no existing default leave exactly one row and one default", async () => {
  await db.recompilerProfile.deleteMany({});
  await Promise.allSettled([
    store.upsertActiveProfile({ config: validConfig("A") }),
    store.upsertActiveProfile({ config: validConfig("B") }),
    store.upsertActiveProfile({ config: validConfig("C") }),
  ]);
  const rows = await db.recompilerProfile.findMany();
  assert.equal(rows.length, 1, "concurrent upserts must not create duplicate rows");
  const defaults = await db.recompilerProfile.count({ where: { isDefault: true } });
  assert.equal(defaults, 1);
  await db.recompilerProfile.deleteMany({});
});

test("upsert heals a legacy multi-default database to exactly one default", async () => {
  await db.recompilerProfile.deleteMany({});
  // Simulate a pre-#188 database with two default rows.
  await db.recompilerProfile.create({
    data: { name: "LegacyA", configJson: JSON.stringify(validConfig("LegacyA")), isDefault: true, schemaVersion: 1 },
  });
  await db.recompilerProfile.create({
    data: { name: "LegacyB", configJson: JSON.stringify(validConfig("LegacyB")), isDefault: true, schemaVersion: 1 },
  });
  await store.upsertActiveProfile({ config: validConfig("New") });
  const defaults = await db.recompilerProfile.findMany({ where: { isDefault: true } });
  assert.equal(defaults.length, 1, "only one default may remain after an upsert");
  assert.equal(defaults[0].name, "New", "the upsert target must be the surviving default");
  await db.recompilerProfile.deleteMany({});
});

test("activateProfile on a missing id throws not-found and persists nothing", async () => {
  await db.recompilerProfile.deleteMany({});
  await store.upsertActiveProfile({ config: validConfig("Keep") });
  await expectStoreError(store.activateProfile("nope"), "profile-not-found");
  const defaults = await db.recompilerProfile.count({ where: { isDefault: true } });
  assert.equal(defaults, 1, "a failed activation must not clear the existing default");
  await db.recompilerProfile.deleteMany({});
});

// ---- Delete --------------------------------------------------------------

test("deleteProfile refuses to delete the active or the last profile", async () => {
  await db.recompilerProfile.deleteMany({});
  const active = await store.upsertActiveProfile({ config: validConfig("Active") });
  await expectStoreError(store.deleteProfile(active.row.id), "cannot-delete-active");

  await store.createProfile({ name: "Other", config: validConfig("Other") });
  await db.recompilerProfile.updateMany({ where: { id: active.row.id }, data: { isDefault: false } });
  // Now the active one is no longer default; deleting it leaves "Other".
  await store.deleteProfile(active.row.id);
  const remaining = await db.recompilerProfile.findMany();
  assert.equal(remaining.length, 1);
  await expectStoreError(store.deleteProfile(remaining[0].id), "cannot-delete-last");
  await db.recompilerProfile.deleteMany({});
});

test("concurrent deletes of different profiles leave exactly one", async () => {
  await db.recompilerProfile.deleteMany({});
  const active = await store.upsertActiveProfile({ config: validConfig("Active") });
  const x = await store.createProfile({ name: "X", config: validConfig("X") });
  const y = await store.createProfile({ name: "Y", config: validConfig("Y") });
  await Promise.allSettled([store.deleteProfile(x.row.id), store.deleteProfile(y.row.id)]);
  const remaining = await db.recompilerProfile.findMany();
  assert.equal(remaining.length, 1);
  assert.equal(remaining[0].id, active.row.id);
  await db.recompilerProfile.deleteMany({});
});

// ---- Rename / read -------------------------------------------------------

test("renameProfile validates the name and persists it", async () => {
  await db.recompilerProfile.deleteMany({});
  const { row } = await store.upsertActiveProfile({ config: validConfig("Before") });
  const renamed = await store.renameProfile(row.id, "  After  ");
  assert.equal(renamed.name, "After");
  await expectStoreError(store.renameProfile(row.id, "x".repeat(200)), "name-too-long");
  await expectStoreError(store.renameProfile("missing", "NewName"), "profile-not-found");
  await db.recompilerProfile.deleteMany({});
});

test("getProfileById validates the stored config", async () => {
  await db.recompilerProfile.deleteMany({});
  const { row } = await store.upsertActiveProfile({ config: validConfig("Read") });
  const detail = await store.getProfileById(row.id);
  assert.equal(detail.status, "ok");
  await expectStoreError(store.getProfileById("missing"), "profile-not-found");
  await db.recompilerProfile.update({ where: { id: row.id }, data: { configJson: "[]" } });
  await expectStoreError(store.getProfileById(row.id), "corrupt");
  await db.recompilerProfile.update({ where: { id: row.id }, data: { schemaVersion: 7 } });
  await expectStoreError(store.getProfileById(row.id), "unsupported-version");
  await db.recompilerProfile.deleteMany({});
});

test("getProfileMeta returns metadata without validating the stored config", async () => {
  await db.recompilerProfile.deleteMany({});
  const { row } = await store.upsertActiveProfile({ config: validConfig("Meta") });
  await db.recompilerProfile.update({ where: { id: row.id }, data: { configJson: "garbage" } });
  const meta = await store.getProfileMeta(row.id);
  assert.equal(meta.id, row.id);
  await expectStoreError(store.getProfileMeta("missing"), "profile-not-found");
  await db.recompilerProfile.deleteMany({});
});

// ---- Rollback primitive --------------------------------------------------

test("a failure inside an interactive transaction rolls everything back", async () => {
  await db.recompilerProfile.deleteMany({});
  const before = await db.recompilerProfile.count();
  await db
    .$transaction(async (tx) => {
      await tx.recompilerProfile.create({
        data: { name: "Ghost", configJson: "{}", isDefault: false, schemaVersion: 1 },
      });
      throw new Error("injected failure between steps");
    })
    .catch(() => undefined);
  const after = await db.recompilerProfile.count();
  assert.equal(after, before, "the intermediate create must be rolled back");
});
