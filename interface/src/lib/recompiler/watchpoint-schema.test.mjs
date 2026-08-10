import assert from "node:assert/strict";
import test from "node:test";
import {
  WATCHPOINT_SCHEMA_VERSION,
  MAX_WATCHPOINTS,
  MAX_WATCHPOINT_SPAN,
  MAX_LABEL_BYTES,
  MAX_DEBUG_MASK,
  parseWatchAddress,
  normalizeWatchpoint,
  validateWatchpointList,
  parseStoredWatchpoints,
  parseDebugMask,
  parseStrictBoolean,
  parseDebugProfileName,
} from "./watchpoint-schema.mjs";

const U32 = 0xffffffff;

// ---- Exact full-string numeric parsing -----------------------------------

test("parseWatchAddress accepts only documented full decimal and 0x-hex spellings", () => {
  assert.equal(parseWatchAddress("0").value, 0);
  assert.equal(parseWatchAddress("4096").value, 4096);
  assert.equal(parseWatchAddress("0x1000").value, 0x1000);
  assert.equal(parseWatchAddress("0X1F").value, 0x1f);
  assert.equal(parseWatchAddress("+0x1F").value, 0x1f);
  assert.equal(parseWatchAddress("+5").value, 5);
  assert.equal(parseWatchAddress(String(U32)).value, U32);
  assert.equal(parseWatchAddress(" 0x10 ").value, 0x10);
});

test("parseWatchAddress rejects partial strings, negatives and overflows", () => {
  for (const bad of ["0x100junk", "123junk", "junk", "", "  ", "0x", "0xG", "-5", "-0x10", "0x1.5", "1e3", "0b101", "0o17", "1_000", "4294967296", "0x100000000", "0xfffffffff", String(U32 + 1), "1.5", null, undefined, 42, {}, [], true]) {
    assert.equal(parseWatchAddress(bad).ok, false, `must reject ${JSON.stringify(bad)}`);
  }
});

// ---- Single watchpoint normalization -------------------------------------

test("normalizeWatchpoint accepts numbers and strings with canonical output", () => {
  const fromNumbers = normalizeWatchpoint({ start: 0x1000, end: 0x1010, label: "Font" });
  assert.equal(fromNumbers.ok, true);
  assert.deepEqual(fromNumbers.value, { start: 0x1000, end: 0x1010, label: "Font" });

  const fromStrings = normalizeWatchpoint({ start: "0x1000", end: "10100", label: "  Font  " });
  assert.equal(fromStrings.ok, true);
  assert.deepEqual(fromStrings.value, { start: 0x1000, end: 10100, label: "Font" });
});

test("normalizeWatchpoint enforces 0 <= start < end <= UINT32_MAX", () => {
  const cases = [
    { start: -1, end: 10 },
    { start: 0, end: 0 },          // zero span: rejected (end-exclusive contract)
    { start: 10, end: 10 },
    { start: 20, end: 10 },        // start > end
    { start: 0, end: U32 + 1 },
    { start: U32, end: U32 },      // zero span at top
    { start: 0x80000000, end: 0x80000000 + 1 }, // exceeds 16 MiB span? no, span=1
  ];
  for (const wp of cases) {
    assert.equal(normalizeWatchpoint(wp).ok, false, `must reject ${JSON.stringify(wp)}`);
  }
  // [0, X) is legal: start-inclusive, end-exclusive with start=0.
  assert.equal(normalizeWatchpoint({ start: 0, end: 0x100, label: "low" }).ok, true);
  // End may equal UINT32_MAX (covers the last byte).
  assert.equal(normalizeWatchpoint({ start: U32 - 1, end: U32, label: "top" }).ok, true);
});

test("normalizeWatchpoint enforces the maximum span", () => {
  assert.equal(normalizeWatchpoint({ start: 0, end: MAX_WATCHPOINT_SPAN, label: "ok" }).ok, true);
  assert.equal(normalizeWatchpoint({ start: 0, end: MAX_WATCHPOINT_SPAN + 1, label: "wide" }).ok, false);
});

test("normalizeWatchpoint rejects non-finite and unsafe integers", () => {
  for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, 0.5, 2 ** 53, Number.MAX_SAFE_INTEGER]) {
    assert.equal(normalizeWatchpoint({ start: bad, end: 0x2000, label: "x" }).ok, false, `start=${bad}`);
    assert.equal(normalizeWatchpoint({ start: 0x1000, end: bad, label: "x" }).ok, false, `end=${bad}`);
  }
});

// ---- Label policy --------------------------------------------------------

test("labels: non-empty, byte-limited, canonical character set", () => {
  assert.equal(normalizeWatchpoint({ start: 1, end: 2, label: "" }).ok, false);
  assert.equal(normalizeWatchpoint({ start: 1, end: 2, label: "   " }).ok, false);
  assert.equal(normalizeWatchpoint({ start: 1, end: 2, label: 42 }).ok, false);
  assert.equal(normalizeWatchpoint({ start: 1, end: 2, label: "A-Z a_z.9" }).ok, true);
  assert.equal(normalizeWatchpoint({ start: 1, end: 2, label: "quote\"inject" }).ok, false);
  assert.equal(normalizeWatchpoint({ start: 1, end: 2, label: "slash/bar" }).ok, false);
  assert.equal(normalizeWatchpoint({ start: 1, end: 2, label: "café" }).ok, false);
  assert.equal(normalizeWatchpoint({ start: 1, end: 2, label: "x".repeat(MAX_LABEL_BYTES) }).ok, true);
  assert.equal(normalizeWatchpoint({ start: 1, end: 2, label: "x".repeat(MAX_LABEL_BYTES + 1) }).ok, false);
});

test("normalizeWatchpoint rejects unknown keys, wrong types and non-objects", () => {
  assert.equal(normalizeWatchpoint(null).ok, false);
  assert.equal(normalizeWatchpoint([1, 2]).ok, false);
  assert.equal(normalizeWatchpoint("wp").ok, false);
  assert.equal(normalizeWatchpoint({ start: 1, end: 2, label: "x", extra: true }).ok, false, "unknown key must be rejected");
  assert.equal(normalizeWatchpoint({ start: "1", end: "2" }).ok, false, "missing label");
  assert.equal(normalizeWatchpoint({ start: "1", end: "2", label: "x", start2: 9 }).ok, false);
});

// ---- Collection policy ---------------------------------------------------

function wp(start, end, label = "L") {
  return { start, end, label };
}

test("validateWatchpointList: empty is valid; max+1 is rejected", () => {
  assert.deepEqual(validateWatchpointList([]).value, []);
  const atMax = Array.from({ length: MAX_WATCHPOINTS }, (_, i) => wp(0x1000 + i * 0x100, 0x1000 + i * 0x100 + 0x10));
  assert.equal(validateWatchpointList(atMax).ok, true);
  const over = Array.from({ length: MAX_WATCHPOINTS + 1 }, (_, i) => wp(0x1000 + i * 0x100, 0x1000 + i * 0x100 + 0x10));
  assert.equal(validateWatchpointList(over).ok, false);
  assert.equal(validateWatchpointList("nope").ok, false);
  assert.equal(validateWatchpointList({}).ok, false);
});

test("validateWatchpointList rejects exact duplicates and accepts distinct overlaps", () => {
  const dup = [wp(1, 10, "a"), wp(1, 10, "a")];
  assert.equal(validateWatchpointList(dup).ok, false, "exact duplicate must be rejected");
  const dupSameRangeDiffLabel = [wp(1, 10, "a"), wp(1, 10, "b")];
  assert.equal(validateWatchpointList(dupSameRangeDiffLabel).ok, false, "same range is a duplicate regardless of label");
  // Overlapping-but-distinct ranges are the documented policy: allowed.
  const overlap = [wp(1, 10, "a"), wp(5, 20, "b")];
  assert.equal(validateWatchpointList(overlap).ok, true);
  const adjacent = [wp(1, 10, "a"), wp(10, 20, "b")];
  assert.equal(validateWatchpointList(adjacent).ok, true);
});

test("validateWatchpointList rejects any invalid entry and reports its index", () => {
  const withBad = [wp(1, 10), { start: "0x100junk", end: 20, label: "bad" }];
  const result = validateWatchpointList(withBad);
  assert.equal(result.ok, false);
  assert.match(result.reason, /watchpoints\[1\]/);
});

// ---- Stored watchpoints JSON ---------------------------------------------

test("parseStoredWatchpoints never throws and classifies malformed JSON", () => {
  assert.equal(parseStoredWatchpoints("{bad").ok, false);
  assert.equal(parseStoredWatchpoints("[]").ok, true);
  assert.equal(parseStoredWatchpoints("[{\"start\":1,\"end\":2,\"label\":\"x\"}]").ok, true);
  assert.equal(parseStoredWatchpoints(42).ok, false);
  assert.equal(parseStoredWatchpoints("null").ok, false);
});

// ---- Debug mask ----------------------------------------------------------

test("parseDebugMask requires a bounded unsigned 32-bit integer", () => {
  assert.equal(parseDebugMask(0).value, 0);
  assert.equal(parseDebugMask(0xff).value, 0xff);
  assert.equal(parseDebugMask(MAX_DEBUG_MASK).value, MAX_DEBUG_MASK);
  assert.equal(parseDebugMask("0").value, 0);
  assert.equal(parseDebugMask("255").value, 255);
  for (const bad of [-1, 1.5, MAX_DEBUG_MASK + 1, Number.NaN, Number.POSITIVE_INFINITY, "0xABC", "junk", "", true, null, "1.5", " 12 ", 2 ** 53]) {
    assert.equal(parseDebugMask(bad).ok, false, `must reject ${JSON.stringify(bad)}`);
  }
});

// ---- Strict booleans -----------------------------------------------------

test("parseStrictBoolean rejects truthy coercions", () => {
  assert.equal(parseStrictBoolean(true).value, true);
  assert.equal(parseStrictBoolean(false).value, false);
  for (const bad of ["false", "true", "yes", 1, 0, "0", null, undefined, [], {}]) {
    assert.equal(parseStrictBoolean(bad).ok, false, `must reject ${JSON.stringify(bad)}`);
  }
});

// ---- Debug profile names -------------------------------------------------

test("parseDebugProfileName validates bounded names", () => {
  assert.equal(parseDebugProfileName("  Debug  ").value, "Debug");
  assert.equal(parseDebugProfileName("").ok, false);
  assert.equal(parseDebugProfileName("a\u0001b").ok, false);
  assert.equal(parseDebugProfileName("x".repeat(101)).ok, false);
  assert.equal(parseDebugProfileName("é".repeat(60)).ok, false);
  assert.equal(parseDebugProfileName(7).ok, false);
});

test("schema version constant is 1", () => {
  assert.equal(WATCHPOINT_SCHEMA_VERSION, 1);
});
