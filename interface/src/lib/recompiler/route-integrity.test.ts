// route-integrity.test.ts — DB-backed tests that exercise the real API route
// handlers (config / profiles / profiles/[id] / profiles/[id]/export) against
// a throwaway SQLite database. Run with `npm run test:db`.
//
// These lock the route wiring: status codes, response shapes and the
// corrupt/unsupported-version classification behavior the UI depends on.

import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { execSync } from "node:child_process";
import { rmSync, existsSync } from "node:fs";
import { randomUUID } from "node:crypto";
import { NextRequest } from "next/server";

const DB_NAME = `test-route-${randomUUID()}.db`;
process.env.DATABASE_URL = `file:./prisma/.test/${DB_NAME}`;

execSync(`npx prisma db push --url "file:./prisma/.test/${DB_NAME}"`, { stdio: "ignore" });

type RouteModule<T> = T;

let configRoute: RouteModule<typeof import("@/app/api/recompiler/config/route")>;
let profilesRoute: RouteModule<typeof import("@/app/api/recompiler/profiles/route")>;
let idRoute: RouteModule<typeof import("@/app/api/recompiler/profiles/[id]/route")>;
let exportRoute: RouteModule<typeof import("@/app/api/recompiler/profiles/[id]/export/route")>;
import type { MinimizeStrategy } from "./types";
let defaultConfig: (strategy?: MinimizeStrategy) => unknown;

before(async () => {
  configRoute = await import("@/app/api/recompiler/config/route");
  profilesRoute = await import("@/app/api/recompiler/profiles/route");
  idRoute = await import("@/app/api/recompiler/profiles/[id]/route");
  exportRoute = await import("@/app/api/recompiler/profiles/[id]/export/route");
  const defaults = await import("@/lib/recompiler/defaults");
  defaultConfig = defaults.defaultConfig;
});

after(async () => {
  const { db } = await import("@/lib/db");
  await db.$disconnect();
  const base = `prisma/.test/${DB_NAME}`;
  for (const suffix of ["", "-journal", "-wal", "-shm"]) {
    if (existsSync(base + suffix)) rmSync(base + suffix, { force: true });
  }
});

function post(url: string, body: unknown): NextRequest {
  return new NextRequest(url, {
    method: "POST",
    body: JSON.stringify(body),
    headers: { "content-type": "application/json" },
  });
}

function patch(url: string, body: unknown, id: string): NextRequest {
  return new NextRequest(url, { method: "PATCH", body: JSON.stringify(body), headers: { "content-type": "application/json" } });
}

async function bodyOf(res: Response): Promise<Record<string, unknown>> {
  return (await res.json()) as Record<string, unknown>;
}

test("config GET returns a default only when nothing is saved", async () => {
  const res = await configRoute.GET();
  const body = await bodyOf(res);
  assert.equal(res.status, 200);
  assert.equal(body.status, "none");
  assert.equal(body.id, null);
});

test("config POST rejects invalid configs with field-level 400 before persisting", async () => {
  const bad = defaultConfig("minimal") as { graphics: { framePacing: unknown } };
  bad.graphics.framePacing = "yes";
  const res = await configRoute.POST(post("http://localhost/api/recompiler/config", { config: bad }));
  const body = await bodyOf(res);
  assert.equal(res.status, 400);
  assert.equal(body.error, "validation-failed");
  assert.ok(Array.isArray(body.fields), "field-level errors must be returned");
});

test("config POST persists a valid config and GET returns it", async () => {
  const cfg = defaultConfig("minimal") as { profileName: string };
  const res = await configRoute.POST(post("http://localhost/api/recompiler/config", { config: cfg }));
  const body = await bodyOf(res);
  assert.equal(res.status, 200);
  assert.equal(body.status, "ok");
  assert.equal(body.schemaVersion, 1);

  const get = await configRoute.GET();
  const got = await bodyOf(get);
  assert.equal(get.status, 200);
  assert.equal(got.status, "ok");
  assert.equal((got.config as { profileName: string }).profileName, cfg.profileName);
});

test("profiles POST creates a non-default profile and duplicate-from-missing 400s", async () => {
  const res = await profilesRoute.POST(post("http://localhost/api/recompiler/profiles", {
    name: "Second",
    config: defaultConfig("portable"),
  }));
  const body = await bodyOf(res);
  assert.equal(res.status, 201);
  assert.equal(body.isDefault, false);

  const dup = await profilesRoute.POST(post("http://localhost/api/recompiler/profiles", {
    name: "Copy",
    duplicateFrom: "missing",
  }));
  const dupBody = await bodyOf(dup);
  assert.equal(dup.status, 400);
  assert.equal(dupBody.error, "duplicate-source-not-found");
});

test("PATCH activate switches the active profile transactionally", async () => {
  const list = await bodyOf(await profilesRoute.GET());
  const second = (list.profiles as { id: string; name: string }[]).find((p) => p.name === "Second")!;
  const res = await idRoute.PATCH(
    patch("http://localhost/api/recompiler/profiles/x", { activate: true }, second.id),
    { params: Promise.resolve({ id: second.id }) },
  );
  const body = await bodyOf(res);
  assert.equal(res.status, 200);
  assert.equal(body.isDefault, true);

  const got = await bodyOf(await configRoute.GET());
  assert.equal(got.status, "ok");
  assert.equal((got.config as { profileName: string }).profileName, "Second");
});

test("profiles list returns valid profiles and an empty corrupt array", async () => {
  const res = await profilesRoute.GET();
  const body = await bodyOf(res);
  assert.equal(res.status, 200);
  assert.equal((body.profiles as unknown[]).length, 2);
  assert.deepEqual(body.corrupt, []);
});

test("DELETE refuses the active profile and allows the inactive one", async () => {
  const list = await bodyOf(await profilesRoute.GET());
  const second = (list.profiles as { id: string; name: string }[]).find((p) => p.name === "Second")!;
  const first = (list.profiles as { id: string; name: string }[]).find((p) => p.name !== "Second")!;

  const delActive = await idRoute.DELETE(new NextRequest("http://localhost/api/recompiler/profiles/x", { method: "DELETE" }), {
    params: Promise.resolve({ id: second.id }),
  });
  assert.equal(delActive.status, 409);

  const delInactive = await idRoute.DELETE(new NextRequest("http://localhost/api/recompiler/profiles/x", { method: "DELETE" }), {
    params: Promise.resolve({ id: first.id }),
  });
  assert.equal(delInactive.status, 200);
});

test("export includes schemaVersion and a valid config", async () => {
  const list = await bodyOf(await profilesRoute.GET());
  const second = (list.profiles as { id: string; name: string }[]).find((p) => p.name === "Second")!;
  const res = await exportRoute.GET(new NextRequest("http://localhost/api/recompiler/profiles/x"), {
    params: Promise.resolve({ id: second.id }),
  });
  assert.equal(res.status, 200);
  const payload = JSON.parse(await res.text()) as { schemaVersion: number; config: unknown };
  assert.equal(payload.schemaVersion, 1);
  assert.ok(payload.config);
});

test("no-op PATCH on a corrupt profile still returns metadata", async () => {
  const list = await bodyOf(await profilesRoute.GET());
  const second = (list.profiles as { id: string; name: string }[]).find((p) => p.name === "Second")!;
  // Corrupt the stored config behind the API's back.
  const { db } = await import("@/lib/db");
  await db.recompilerProfile.update({ where: { id: second.id }, data: { configJson: "[]" } });

  const res = await idRoute.PATCH(patch("http://localhost/api/recompiler/profiles/x", {}, second.id), {
    params: Promise.resolve({ id: second.id }),
  });
  assert.equal(res.status, 200, "a metadata no-op must not require a valid stored config");

  // Reading it as a profile must report corruption, not serve garbage.
  const get = await idRoute.GET(new NextRequest("http://localhost/api/recompiler/profiles/x"), {
    params: Promise.resolve({ id: second.id }),
  });
  const getBody = await bodyOf(get);
  assert.equal(get.status, 500);
  assert.equal(getBody.error, "corrupt");
});
