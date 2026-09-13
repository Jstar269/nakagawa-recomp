import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, writeFileSync, rmSync, existsSync } from "node:fs";
import path from "node:path";
import { safeWalkDirectory } from "./fs-walker";

const TEST_DIR = path.join(process.cwd(), "prisma", ".test", "fs-walker-test-" + Date.now());

before(() => {
  mkdirSync(path.join(TEST_DIR, "a", "b", "c"), { recursive: true });
  mkdirSync(path.join(TEST_DIR, "x"), { recursive: true });

  writeFileSync(path.join(TEST_DIR, "root.txt"), "root");
  writeFileSync(path.join(TEST_DIR, "target.json"), "target-1");
  writeFileSync(path.join(TEST_DIR, "a", "file_a.txt"), "a");
  writeFileSync(path.join(TEST_DIR, "a", "target.json"), "target-2");
  writeFileSync(path.join(TEST_DIR, "a", "b", "file_b.txt"), "b");
  writeFileSync(path.join(TEST_DIR, "a", "b", "c", "file_c.txt"), "c");
});

after(() => {
  if (existsSync(TEST_DIR)) {
    rmSync(TEST_DIR, { recursive: true, force: true });
  }
});

test("safeWalkDirectory: collects all files recursively", () => {
  const files = safeWalkDirectory(TEST_DIR);
  assert.equal(files.length, 6);
  assert.ok(files.some((f) => f.endsWith("root.txt")));
  assert.ok(files.some((f) => f.endsWith("file_c.txt")));
});

test("safeWalkDirectory: filters by targetFileName", () => {
  const files = safeWalkDirectory(TEST_DIR, { targetFileName: "target.json" });
  assert.equal(files.length, 2);
  for (const f of files) {
    assert.ok(f.endsWith("target.json"));
  }
});

test("safeWalkDirectory: respects maxFiles budget", () => {
  const files = safeWalkDirectory(TEST_DIR, { maxFiles: 3 });
  assert.equal(files.length, 3);
});

test("safeWalkDirectory: respects maxDepth budget", () => {
  // depth 0 = root dir only, depth 1 = a and x
  const filesDepth0 = safeWalkDirectory(TEST_DIR, { maxDepth: 0 });
  // root.txt and target.json are at depth 0
  assert.equal(filesDepth0.length, 2);

  const filesDepth1 = safeWalkDirectory(TEST_DIR, { maxDepth: 1 });
  // root.txt, target.json, file_a.txt, a/target.json
  assert.equal(filesDepth1.length, 4);
});

test("safeWalkDirectory: returns empty array for non-existent root", () => {
  const files = safeWalkDirectory(path.join(TEST_DIR, "does-not-exist"));
  assert.deepEqual(files, []);
});
