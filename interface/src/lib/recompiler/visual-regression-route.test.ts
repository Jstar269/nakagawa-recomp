// visual-regression-route.test.ts — route-level tests for the shared bounded
// P6 PPM loader (issue #174).  Exercises the real image and variance GET
// handlers against temp repo roots containing generated PPM fixtures.  Run
// with `npm run test:db`.
//
// The temp root satisfies findRepoRoot's anchor set via HST_DASHBOARD_REPO_ROOT,
// so no real repo or game files are touched.

import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";

let repoRoot: string;
let snapDir: string;
let goldDir: string;

function writePpm(dir: string, name: string, w: number, h: number, payload?: Buffer) {
  const header = `P6\n${w} ${h}\n255\n`;
  const pixels = payload ?? Buffer.alloc(w * h * 3, 0x80);
  const file = path.join(dir, name);
  writeFileSync(file, Buffer.concat([Buffer.from(header, "ascii"), pixels]));
  return file;
}

function makeCanonicalRoot() {
  const root = mkdtempSync(path.join(tmpdir(), "vr-route-"));
  for (const anchor of ["hst_manager.ps1", "AGENTS.md", "Makefile"]) {
    writeFileSync(path.join(root, anchor), "# test anchor\n");
  }
  mkdirSync(path.join(root, "build", "snapshots"), { recursive: true });
  mkdirSync(path.join(root, "build", "golden"), { recursive: true });
  return root;
}

before(() => {
  repoRoot = makeCanonicalRoot();
  snapDir = path.join(repoRoot, "build", "snapshots");
  goldDir = path.join(repoRoot, "build", "golden");
  process.env.HST_DASHBOARD_REPO_ROOT = repoRoot;
});

after(() => {
  delete process.env.HST_DASHBOARD_REPO_ROOT;
  rmSync(repoRoot, { recursive: true, force: true });
});

function imageUrl(file: string, type: string, extra = "") {
  return `http://localhost/api/recompiler/visual-regression/image?type=${type}&file=${file}${extra}`;
}

function varianceUrl(file: string) {
  return `http://localhost/api/recompiler/visual-regression/variance?file=${file}`;
}

async function imageGet(url: string) {
  const mod = await import("@/app/api/recompiler/visual-regression/image/route");
  const req = new Request(url);
  return mod.GET(req as never);
}

async function varianceGet(url: string) {
  const mod = await import("@/app/api/recompiler/visual-regression/variance/route");
  const req = new Request(url);
  return mod.GET(req as never);
}

test("image: canonical PSP-sized PPM renders as PNG", async () => {
  writePpm(snapDir, "frame_0001.ppm", 480, 272);
  const res = await imageGet(imageUrl("frame_0001.ppm", "snapshot"));
  assert.equal(res.status, 200);
  const body = Buffer.from(await res.arrayBuffer());
  assert.deepEqual([...body.subarray(0, 8)], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
});

test("image: malformed PPM (truncated payload) is a deterministic 400", async () => {
  const header = "P6\n4 4\n255\n";
  const truncated = Buffer.concat([
    Buffer.from(header, "ascii"),
    Buffer.alloc(4 * 4 * 3 - 3, 0x80),
  ]);
  writeFileSync(path.join(snapDir, "trunc.ppm"), truncated);
  const res = await imageGet(imageUrl("trunc.ppm", "snapshot"));
  assert.equal(res.status, 400);
  const text = await res.text();
  assert.match(text, /Malformed PPM/);
});

test("image: huge-dimension PPM (tiny payload) rejected before allocation", async () => {
  // Header claims 30000x30000 (≈2.7 GB RGB) with a tiny payload.  The old
  // parseInt path would feed this to sharp/Buffer; the shared loader rejects
  // the dimension limit deterministically.
  const header = "P6\n30000 30000\n255\n";
  const fake = Buffer.concat([Buffer.from(header, "ascii"), Buffer.alloc(64, 0)]);
  writeFileSync(path.join(snapDir, "huge.ppm"), fake);
  const res = await imageGet(imageUrl("huge.ppm", "snapshot"));
  assert.equal(res.status, 400);
  assert.match(await res.text(), /Malformed PPM/);
});

test("image: NaN-equivalent dimension (0x10junk) rejected", async () => {
  const header = "P6\n0x10junk 8\n255\n";
  writeFileSync(
    path.join(snapDir, "junk.ppm"),
    Buffer.concat([Buffer.from(header, "ascii"), Buffer.alloc(8 * 8 * 3, 0)]),
  );
  const res = await imageGet(imageUrl("junk.ppm", "snapshot"));
  assert.equal(res.status, 400);
});

test("image: diff of matching dimensions returns PNG; mismatch is 400", async () => {
  writePpm(snapDir, "frame_0002.ppm", 16, 16, Buffer.alloc(16 * 16 * 3, 0x10));
  writePpm(goldDir, "frame_0002.ppm", 16, 16, Buffer.alloc(16 * 16 * 3, 0x20));
  const res = await imageGet(imageUrl("frame_0002.ppm", "diff"));
  assert.equal(res.status, 200);
  const body = Buffer.from(await res.arrayBuffer());
  assert.deepEqual([...body.subarray(0, 8)], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

  // Mismatched dimensions must be rejected before any diff allocation.
  writePpm(goldDir, "frame_0002.ppm", 17, 16, Buffer.alloc(17 * 16 * 3, 0x20));
  const res2 = await imageGet(imageUrl("frame_0002.ppm", "diff"));
  assert.equal(res2.status, 400);
});

test("variance: canonical PSP-sized PPM produces 30x17 grid + histogram", async () => {
  writePpm(snapDir, "frame_0003.ppm", 480, 272);
  writePpm(goldDir, "frame_0003.ppm", 480, 272, Buffer.alloc(480 * 272 * 3, 0x60));
  const res = await varianceGet(varianceUrl("frame_0003.ppm"));
  assert.equal(res.status, 200);
  const json = await res.json();
  assert.equal(json.width, 480);
  assert.equal(json.height, 272);
  assert.equal(json.grid.rows, 17);
  assert.equal(json.grid.cols, 30);
  assert.equal(json.grid.cells.length, 17 * 30);
  assert.equal(json.histogram.length, 16);
});

test("variance: tiny dimensions below 30x17 are safe (no zero cell size)", async () => {
  writePpm(snapDir, "tiny.ppm", 10, 10, Buffer.alloc(10 * 10 * 3, 0x11));
  writePpm(goldDir, "tiny.ppm", 10, 10, Buffer.alloc(10 * 10 * 3, 0x22));
  const res = await varianceGet(varianceUrl("tiny.ppm"));
  assert.equal(res.status, 200);
  const json = await res.json();
  // Every grid cell must be present and numeric — no NaN from zero cells.
  assert.equal(json.grid.cells.length, 17 * 30);
  for (const cell of json.grid.cells) {
    for (const k of ["r", "g", "b", "a"]) {
      assert.ok(Number.isFinite(cell[k]), `cell.${k} must be finite`);
    }
  }
});

test("variance: malformed PPM is a deterministic 400, not a crash", async () => {
  const header = "P6\n8 8\n255\n";
  writeFileSync(
    path.join(snapDir, "bad.ppm"),
    Buffer.concat([Buffer.from(header, "ascii"), Buffer.alloc(8 * 8 * 3 - 5, 0x80)]),
  );
  writePpm(goldDir, "bad.ppm", 8, 8);
  const res = await varianceGet(varianceUrl("bad.ppm"));
  assert.equal(res.status, 400);
});

test("image: unsafe-integer dimensions cannot reach allocation", async () => {
  // 2^60 x 2 would be rejected as bad-header by the strict token parser.
  const header = "P6\n1152921504606846976 2\n255\n";
  writeFileSync(
    path.join(snapDir, "overflow.ppm"),
    Buffer.concat([Buffer.from(header, "ascii"), Buffer.alloc(16, 0)]),
  );
  const res = await imageGet(imageUrl("overflow.ppm", "snapshot"));
  assert.equal(res.status, 400);
});
