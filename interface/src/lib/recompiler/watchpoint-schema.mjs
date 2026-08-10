// watchpoint-schema.mjs — runtime validation and normalization for watchpoints
// and debug profiles (issue #188).
//
// The native runtime consumes watchpoints from `watchpoints.json` and from
// `SR_WATCH_N` environment variables (see src/rt/debug.c and
// src/rt/driver.c). The range semantics are canonical:
//
//   match = guest_addr >= start && guest_addr < end     (start-inclusive,
//                                                       end-exclusive)
//
// so a watchpoint must satisfy `0 <= start < end <= 0xFFFFFFFF`. The runtime
// silently drops all-zero (start==0 && end==0) entries and deduplicates exact
// (start, end) pairs, so this module rejects zero-length and exact-duplicate
// ranges up front. Overlapping-but-distinct ranges are permitted and documented
// (the runtime treats each watch independently).
//
// Labels are canonicalized to the same character set the runtime's environment
// path already enforces ([A-Za-z0-9_. -], capped length) so the file and the
// env representations can never diverge.

export const WATCHPOINT_SCHEMA_VERSION = 1;

// Maximum number of watchpoints per profile (matches SR_MAX_MEM_WATCHES = 16).
export const MAX_WATCHPOINTS = 16;

// Maximum inclusive span end - start (16 MiB). Ranges wider than this are
// rejected as oversized.
export const MAX_WATCHPOINT_SPAN = 1 << 24;

// Maximum UTF-8 byte length of a watchpoint label.
export const MAX_LABEL_BYTES = 64;

// Canonical label character set: letters, digits, underscore, dot, space, dash.
const LABEL_RE = /^[A-Za-z0-9_. -]+$/;

// Max UTF-8 bytes for a debug profile name.
export const DEBUG_PROFILE_NAME_MAX_BYTES = 100;

// Debug masks are validated as bounded unsigned 32-bit values.
export const MAX_DEBUG_MASK = 0xffffffff;

// ---- Numeric parsing -----------------------------------------------------

const HEX_RE = /^[+]?0[xX][0-9a-fA-F]+$/;
const DEC_RE = /^[+]?[0-9]+$/;

/**
 * Parse an address as a full string. Accepts plain decimal or 0x-hex with an
 * optional leading `+`. Rejects partial strings ("0x100junk", "123junk"),
 * empty strings, whitespace, negatives and anything > 0xFFFFFFFF.
 * Returns { ok, value } where value is a safe integer.
 */
export function parseWatchAddress(raw) {
  if (typeof raw !== "string") {
    return { ok: false, value: null, reason: "address must be a string" };
  }
  const trimmed = raw.trim();
  if (trimmed.length === 0) {
    return { ok: false, value: null, reason: "address must not be empty" };
  }
  let value;
  if (HEX_RE.test(trimmed)) {
    value = Number.parseInt(trimmed.replace(/^[+]/, "").slice(2), 16);
  } else if (DEC_RE.test(trimmed)) {
    value = Number.parseInt(trimmed, 10);
  } else {
    return { ok: false, value: null, reason: `"${raw}" is not a full decimal or 0x-hex address` };
  }
  if (!Number.isSafeInteger(value) || value < 0 || value > 0xffffffff) {
    return { ok: false, value: null, reason: `address out of range: ${raw}` };
  }
  return { ok: true, value, reason: null };
}

function labelError(label) {
  if (typeof label !== "string" || label.trim().length === 0) {
    return "label must be a non-empty string";
  }
  const trimmed = label.trim();
  if (!LABEL_RE.test(trimmed)) {
    return `label may only contain letters, digits, "_", ".", " " and "-"`;
  }
  if (Buffer.byteLength(trimmed, "utf8") > MAX_LABEL_BYTES) {
    return `label exceeds ${MAX_LABEL_BYTES} bytes`;
  }
  return null;
}

/**
 * Normalize a single watchpoint from a request body. `start`/`end` may be
 * numbers or strings (the UI sends strings). Returns
 * { ok, value: {start, end, label}, reason }.
 */
export function normalizeWatchpoint(input) {
  if (input === null || typeof input !== "object" || Array.isArray(input)) {
    return { ok: false, value: null, reason: "watchpoint must be an object" };
  }
  for (const key of Object.keys(input)) {
    if (key !== "start" && key !== "end" && key !== "label") {
      return { ok: false, value: null, reason: `unknown watchpoint key "${key}"` };
    }
  }
  const { start, end, label } = input;
  const parsedStart =
    typeof start === "string" ? parseWatchAddress(start) : { ok: true, value: start, reason: null };
  const parsedEnd =
    typeof end === "string" ? parseWatchAddress(end) : { ok: true, value: end, reason: null };
  if (!parsedStart.ok) return { ok: false, value: null, reason: `start: ${parsedStart.reason}` };
  if (!parsedEnd.ok) return { ok: false, value: null, reason: `end: ${parsedEnd.reason}` };
  const s = parsedStart.value;
  const e = parsedEnd.value;
  if (!Number.isSafeInteger(s) || s < 0 || s > 0xffffffff) {
    return { ok: false, value: null, reason: "start must be an integer in [0, 0xffffffff]" };
  }
  if (!Number.isSafeInteger(e) || e < 0 || e > 0xffffffff) {
    return { ok: false, value: null, reason: "end must be an integer in [0, 0xffffffff]" };
  }
  if (s >= e) {
    return { ok: false, value: null, reason: `start (0x${s.toString(16)}) must be < end (0x${e.toString(16)})` };
  }
  const span = e - s;
  if (span > MAX_WATCHPOINT_SPAN) {
    return { ok: false, value: null, reason: `range span 0x${span.toString(16)} exceeds the maximum 0x${MAX_WATCHPOINT_SPAN.toString(16)}` };
  }
  const labelErr = labelError(label);
  if (labelErr) return { ok: false, value: null, reason: labelErr };
  return { ok: true, value: { start: s, end: e, label: label.trim() }, reason: null };
}

/**
 * Validate + normalize a whole watchpoint list. Rejects: non-arrays, more than
 * MAX_WATCHPOINTS entries, any invalid entry, and exact duplicates (same
 * start/end pair). Overlapping distinct ranges are allowed (documented policy).
 * Returns { ok, value, reason }.
 */
export function validateWatchpointList(input) {
  if (!Array.isArray(input)) {
    return { ok: false, value: null, reason: "watchpoints must be an array" };
  }
  if (input.length > MAX_WATCHPOINTS) {
    return { ok: false, value: null, reason: `at most ${MAX_WATCHPOINTS} watchpoints are allowed` };
  }
  const seen = new Set();
  const out = [];
  for (let i = 0; i < input.length; i++) {
    const result = normalizeWatchpoint(input[i]);
    if (!result.ok) {
      return { ok: false, value: null, reason: `watchpoints[${i}]: ${result.reason}` };
    }
    const key = `${result.value.start}:${result.value.end}`;
    if (seen.has(key)) {
      return { ok: false, value: null, reason: `duplicate watchpoint at index ${i} (same start/end as an earlier entry)` };
    }
    seen.add(key);
    out.push(result.value);
  }
  return { ok: true, value: out, reason: null };
}

/**
 * Parse + validate a stored debug-profile `watchpoints` JSON string.
 * Returns { ok, value, reason } — never throws.
 */
export function parseStoredWatchpoints(raw) {
  if (typeof raw !== "string") {
    return { ok: false, value: null, reason: "stored watchpoints must be a JSON string" };
  }
  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch (err) {
    return { ok: false, value: null, reason: `stored watchpoints are not valid JSON: ${err.message}` };
  }
  return validateWatchpointList(parsed);
}

/**
 * Validate a debug mask: must be a finite, safe, unsigned 32-bit integer.
 * Accepts numbers and numeric strings; rejects negatives, floats, strings like
 * "0xABC", booleans and anything > 0xFFFFFFFF.
 */
export function parseDebugMask(raw) {
  if (typeof raw === "string" && /^-?\d+$/.test(raw)) {
    const value = Number(raw);
    if (Number.isSafeInteger(value) && value >= 0 && value <= MAX_DEBUG_MASK) {
      return { ok: true, value, reason: null };
    }
    return { ok: false, value: null, reason: "debugMask must be an unsigned 32-bit integer" };
  }
  if (typeof raw === "number" && Number.isSafeInteger(raw) && raw >= 0 && raw <= MAX_DEBUG_MASK) {
    return { ok: true, value: raw, reason: null };
  }
  return { ok: false, value: null, reason: "debugMask must be an unsigned 32-bit integer" };
}

/**
 * Validate a strict boolean. Rejects truthy coercions: "false", 1, "yes" etc.
 */
export function parseStrictBoolean(raw) {
  if (typeof raw === "boolean") {
    return { ok: true, value: raw, reason: null };
  }
  return { ok: false, value: null, reason: "expected an actual boolean (true/false), not a truthy value" };
}

/**
 * Validate a debug profile name (byte-limited, no control chars).
 */
export function parseDebugProfileName(raw) {
  if (typeof raw !== "string") {
    return { ok: false, value: null, reason: "profile name must be a string" };
  }
  const name = raw.trim();
  if (name.length === 0) return { ok: false, value: null, reason: "profile name must not be empty" };
  if (/[\u0000-\u001f\u007f]/.test(name)) {
    return { ok: false, value: null, reason: "profile name must not contain control characters" };
  }
  if (Buffer.byteLength(name, "utf8") > DEBUG_PROFILE_NAME_MAX_BYTES) {
    return { ok: false, value: null, reason: `profile name exceeds ${DEBUG_PROFILE_NAME_MAX_BYTES} bytes` };
  }
  return { ok: true, value: name, reason: null };
}
