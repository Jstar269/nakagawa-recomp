import assert from "node:assert/strict";
import test from "node:test";
import { mkdtempSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import {
  WATCHPOINTS_FILE_FORMAT,
  WATCHPOINTS_FILE_VERSION,
  WATCHPOINTS_FILE_MAX_BYTES,
  serializeWatchpointsFile,
  atomicWriteFileSync,
  readWatchpointsFile,
  fileMatchesWatchpoints,
  contentHash,
} from "./watchpoint-file.mjs";
import { validateWatchpointList } from "./watchpoint-schema.mjs";

const WATCHES = [
  { start: 0x08001000, end: 0x08001100, label: "Font Engine" },
  { start: 0x08800000, end: 0x08800100, label: "Vertex Pool" },
];
const T = "2026-08-06T00:00:00.000Z";

function tempDir(t) {
  const dir = mkdtempSync(path.join(tmpdir(), "wpf-"));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function validEnvelope(overrides = {}) {
  return serializeWatchpointsFile({
    watchpoints: WATCHES,
    profileId: "prof-1",
    source: "db",
    writtenAt: T,
    ...overrides,
  });
}

// ---- Serialization -------------------------------------------------------

test("serializeWatchpointsFile is deterministic and self-describing", () => {
  const a = validEnvelope();
  const b = validEnvelope();
  assert.equal(a, b, "same inputs must produce identical bytes");
  const parsed = JSON.parse(a);
  assert.equal(parsed.format, WATCHPOINTS_FILE_FORMAT);
  assert.equal(parsed.version, WATCHPOINTS_FILE_VERSION);
  assert.equal(parsed.contentHash, contentHash(WATCHES));
});

test("contentHash is stable across serializations", () => {
  assert.equal(contentHash(WATCHES), contentHash([...WATCHES]));
  assert.notEqual(contentHash(WATCHES), contentHash([WATCHES[0]]));
});

// ---- Atomic write / read -------------------------------------------------

test("round-trips an envelope through an atomic write", (t) => {
  const dir = tempDir(t);
  const file = path.join(dir, "watchpoints.json");
  atomicWriteFileSync(file, validEnvelope());
  const state = readWatchpointsFile(file);
  assert.equal(state.status, "ok");
  assert.deepEqual(state.watchpoints, WATCHES);
  assert.equal(state.meta.profileId, "prof-1");
  assert.equal(state.meta.source, "db");
});

test("write failure removes the temporary and never leaves a partial file", (t) => {
  const dir = tempDir(t);
  const file = path.join(dir, "missing-subdir", "watchpoints.json"); // dir does not exist
  assert.throws(() => atomicWriteFileSync(file, validEnvelope()));
  assert.equal(existsSync(file), false);
  const leftovers = readdirs(dir);
  assert.equal(leftovers.length, 0, "no temp or partial file may remain");
});

test("rename failure keeps the original untouched and cleans the temporary", (t) => {
  const dir = tempDir(t);
  // Make the destination a directory so the rename cannot replace it.
  mkdirSync(path.join(dir, "watchpoints.json"));
  assert.throws(() => atomicWriteFileSync(path.join(dir, "watchpoints.json"), validEnvelope()));
  const leftovers = readdirs(dir);
  assert.deepEqual(leftovers, ["watchpoints.json"], "only the blocking directory remains");
});

test("stale temporary siblings are never read as current", (t) => {
  const dir = tempDir(t);
  const file = path.join(dir, "watchpoints.json");
  // A stale temp from a crashed writer.
  writeFileSync(path.join(dir, "watchpoints.json.tmp-999-deadbeef"), "garbage that is not current");
  atomicWriteFileSync(file, validEnvelope());
  const state = readWatchpointsFile(file);
  assert.equal(state.status, "ok");
  assert.deepEqual(state.watchpoints, WATCHES);
});

test("missing file is reported as missing, not corrupt", (t) => {
  const dir = tempDir(t);
  assert.equal(readWatchpointsFile(path.join(dir, "watchpoints.json")).status, "missing");
});

// ---- Classification ------------------------------------------------------

test("classifies corrupt, unsupported-version and hash-mismatch states", (t) => {
  const dir = tempDir(t);
  const file = path.join(dir, "watchpoints.json");

  writeFileSync(file, "{not json");
  assert.equal(readWatchpointsFile(file).status, "corrupt");

  const badVersion = JSON.parse(validEnvelope());
  badVersion.version = 2;
  writeFileSync(file, JSON.stringify(badVersion));
  assert.equal(readWatchpointsFile(file).status, "unsupported-version");

  const badHash = JSON.parse(validEnvelope());
  badHash.contentHash = "0".repeat(64);
  writeFileSync(file, JSON.stringify(badHash));
  assert.equal(readWatchpointsFile(file).status, "hash-mismatch");

  const noHash = JSON.parse(validEnvelope());
  delete noHash.contentHash;
  writeFileSync(file, JSON.stringify(noHash));
  assert.equal(readWatchpointsFile(file).status, "corrupt");

  const wrongFormat = JSON.parse(validEnvelope());
  wrongFormat.format = "something-else";
  writeFileSync(file, JSON.stringify(wrongFormat));
  assert.equal(readWatchpointsFile(file).status, "corrupt");
});

test("classifies invalid watchpoint content inside the envelope", (t) => {
  const dir = tempDir(t);
  const file = path.join(dir, "watchpoints.json");
  const envelope = JSON.parse(validEnvelope());
  envelope.watchpoints = [{ start: 20, end: 10, label: "reversed" }];
  envelope.contentHash = contentHash([{ start: 20, end: 10, label: "reversed" }]);
  writeFileSync(file, JSON.stringify(envelope));
  assert.equal(readWatchpointsFile(file).status, "corrupt");
});

test("legacy bare-array files remain readable", (t) => {
  const dir = tempDir(t);
  const file = path.join(dir, "watchpoints.json");
  writeFileSync(file, JSON.stringify(WATCHES, null, 2), "utf8"); // pre-#188 layout
  const state = readWatchpointsFile(file);
  assert.equal(state.status, "ok");
  assert.deepEqual(state.watchpoints, WATCHES);
  assert.equal(state.meta.source, "legacy");
});

test("oversized files are rejected as corrupt", (t) => {
  const dir = tempDir(t);
  const file = path.join(dir, "watchpoints.json");
  const huge = JSON.stringify({ junk: "x".repeat(WATCHPOINTS_FILE_MAX_BYTES + 1) });
  writeFileSync(file, huge);
  assert.equal(readWatchpointsFile(file).status, "corrupt");
});

// ---- Staleness -----------------------------------------------------------

test("fileMatchesWatchpoints detects stale derived artifacts", (t) => {
  const dir = tempDir(t);
  const file = path.join(dir, "watchpoints.json");
  atomicWriteFileSync(file, validEnvelope());
  assert.equal(fileMatchesWatchpoints(file, WATCHES), true);
  assert.equal(fileMatchesWatchpoints(file, [WATCHES[0]]), false, "DB no longer matches the file");
  writeFileSync(file, JSON.stringify(WATCHES), "utf8"); // legacy, no hash
  assert.equal(fileMatchesWatchpoints(file, WATCHES), false, "legacy files cannot be proven in sync");
});

test("atomic write enforces the size cap", (t) => {
  const dir = tempDir(t);
  const file = path.join(dir, "watchpoints.json");
  assert.throws(() => atomicWriteFileSync(file, "x".repeat(WATCHPOINTS_FILE_MAX_BYTES + 1)));
});

// ---- Determinism of the writer used by the native fixture -----------------

test("writer output matches the embedded native-parser fixture format", () => {
  // The native selftest (src/rt/watchpoints_file_selftest.c) embeds a byte
  // fixture produced by this writer for a fixed watchpoint set. Lock the
  // grammar here so a writer change cannot silently break the runtime parser.
  const envelope = JSON.parse(validEnvelope({ profileId: null, source: "direct" }));
  assert.deepEqual(Object.keys(envelope), [
    "format", "version", "profileId", "source", "writtenAt", "contentHash", "watchpoints",
  ]);
  for (const watch of envelope.watchpoints) {
    assert.deepEqual(Object.keys(watch), ["start", "end", "label"]);
    assert.equal(typeof watch.start, "number");
    assert.equal(typeof watch.end, "number");
    assert.equal(typeof watch.label, "string");
  }
});

function readdirs(dir) {
  return readdirSync(dir).sort();
}
