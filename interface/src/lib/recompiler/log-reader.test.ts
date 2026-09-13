import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { writeFileSync, appendFileSync, rmSync, existsSync } from "node:fs";
import path from "node:path";
import { readLogSince } from "./runner";

const TEST_LOG = path.join(process.cwd(), "prisma", ".test", "test-log-reader-" + Date.now() + ".log");

before(() => {
  writeFileSync(TEST_LOG, "line 1\nline 2\nline 3\n", "utf8");
});

after(() => {
  if (existsSync(TEST_LOG)) {
    rmSync(TEST_LOG, { force: true });
  }
});

test("readLogSince: reads from byte 0 and returns lines with cursor", () => {
  const result = readLogSince(TEST_LOG, 0);
  assert.equal(result.lines.length, 3);
  assert.equal(result.lines[0], "line 1");
  assert.equal(result.lines[1], "line 2");
  assert.equal(result.lines[2], "line 3");
  assert.ok(result.cursor > 0);
});

test("readLogSince: subsequent read with cursor returns only new lines", () => {
  const first = readLogSince(TEST_LOG, 0);
  const cursor1 = first.cursor;

  // Append new content
  appendFileSync(TEST_LOG, "line 4\nline 5\n", "utf8");

  const second = readLogSince(TEST_LOG, cursor1);
  assert.equal(second.lines.length, 2);
  assert.equal(second.lines[0], "line 4");
  assert.equal(second.lines[1], "line 5");
  assert.ok(second.cursor > cursor1);

  // Read again without appending
  const third = readLogSince(TEST_LOG, second.cursor);
  assert.deepEqual(third.lines, []);
  assert.equal(third.cursor, second.cursor);
});

test("readLogSince: respects maxBytes budget", () => {
  // Read at most 10 bytes
  const bounded = readLogSince(TEST_LOG, 0, 10);
  assert.equal(bounded.cursor, 10);
  assert.ok(bounded.lines.length >= 1);
});

test("readLogSince: handles invalid or negative sinceByte gracefully", () => {
  const result = readLogSince(TEST_LOG, -100);
  assert.ok(result.lines.length > 0);
  assert.ok(result.cursor > 0);
});
