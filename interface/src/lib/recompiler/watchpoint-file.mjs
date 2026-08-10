// watchpoint-file.mjs — the derived `watchpoints.json` runtime artifact
// (issue #188).
//
// The database is the canonical store for watchpoints. This module writes the
// on-disk runtime artifact as a versioned envelope so the artifact is
// self-describing, bounded, deterministic and verifiable:
//
//   {
//     "format": "hst-watchpoints",
//     "version": 1,
//     "profileId": null | "<cuid>",
//     "source": "db" | "direct",
//     "writtenAt": "<ISO-8601>",
//     "contentHash": "<sha256 of the canonical watchpoints array JSON>",
//     "watchpoints": [ { "start": ..., "end": ..., "label": "..." }, ... ]
//   }
//
// The native runtime parser (src/rt/watchpoints_file.c) accepts exactly this
// envelope AND the legacy bare-array form, so old files keep working while new
// files carry identity. Publication is atomic: content is written to a unique
// temporary sibling and renamed into place; a failure never leaves a partial
// file that looks current. Readers never confuse a `*.tmp-*` sibling with the
// canonical file (the canonical name is exact).

import { createHash, randomBytes } from "node:crypto";
import {
  readFileSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
  existsSync,
} from "node:fs";
import path from "node:path";
import { validateWatchpointList } from "./watchpoint-schema.mjs";

export const WATCHPOINTS_FILE_FORMAT = "hst-watchpoints";
export const WATCHPOINTS_FILE_VERSION = 1;
export const WATCHPOINTS_FILE_MAX_BYTES = 64 * 1024;
export const WATCHPOINTS_FILE_NAME = "watchpoints.json";

/** Deterministic canonical JSON of a normalized watchpoint array. */
export function canonicalWatchpointsJson(watchpoints) {
  return JSON.stringify(watchpoints);
}

/** sha256 hex of the canonical watchpoint array JSON. */
export function contentHash(watchpoints) {
  return createHash("sha256").update(canonicalWatchpointsJson(watchpoints), "utf8").digest("hex");
}

/**
 * Serialize the derived envelope. `writtenAt` is an explicit parameter so
 * output is deterministic for tests; callers pass a fresh timestamp.
 */
export function serializeWatchpointsFile({ watchpoints, profileId, source, writtenAt }) {
  const envelope = {
    format: WATCHPOINTS_FILE_FORMAT,
    version: WATCHPOINTS_FILE_VERSION,
    profileId: profileId ?? null,
    source: source ?? "direct",
    writtenAt,
    contentHash: contentHash(watchpoints),
    watchpoints,
  };
  return JSON.stringify(envelope, null, 2);
}

/**
 * Atomic file publication:
 *  1. size-validate the content;
 *  2. write to a unique temporary sibling (`<name>.tmp-<pid>-<random>`);
 *  3. rename over the canonical path.
 * On any failure the temporary is removed and the original file is untouched.
 */
export function atomicWriteFileSync(filePath, content) {
  const bytes = Buffer.byteLength(content, "utf8");
  if (bytes > WATCHPOINTS_FILE_MAX_BYTES) {
    throw new Error(`derived watchpoint file exceeds ${WATCHPOINTS_FILE_MAX_BYTES} bytes (${bytes})`);
  }
  const dir = path.dirname(filePath);
  const base = path.basename(filePath);
  const temp = path.join(dir, `${base}.tmp-${process.pid}-${randomBytes(6).toString("hex")}`);
  try {
    writeFileSync(temp, content, "utf8");
    renameSync(temp, filePath);
  } catch (err) {
    try {
      if (existsSync(temp)) rmSync(temp, { force: true });
    } catch {
      /* best-effort cleanup */
    }
    throw err;
  }
}

function isEnvelope(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

/**
 * Read and classify the derived file. Never throws.
 * Returns:
 *   { status: "missing" }
 *   { status: "ok", watchpoints, meta }       meta = envelope identity fields
 *   { status: "corrupt", reason }             malformed JSON / bad content
 *   { status: "unsupported-version", reason } version != 1
 *   { status: "hash-mismatch", reason, meta } declared hash != computed hash
 */
export function readWatchpointsFile(filePath) {
  if (!existsSync(filePath)) return { status: "missing" };
  let raw;
  try {
    const size = statSync(filePath).size;
    if (size > WATCHPOINTS_FILE_MAX_BYTES) {
      return { status: "corrupt", reason: `file exceeds ${WATCHPOINTS_FILE_MAX_BYTES} bytes` };
    }
    raw = readFileSync(filePath, "utf8");
  } catch (err) {
    return { status: "corrupt", reason: `unreadable file: ${err.message}` };
  }
  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch (err) {
    return { status: "corrupt", reason: `not valid JSON: ${err.message}` };
  }

  // Legacy bare-array files (pre-#188 writer output) remain readable.
  if (Array.isArray(parsed)) {
    const result = validateWatchpointList(parsed);
    if (!result.ok) return { status: "corrupt", reason: result.reason };
    return {
      status: "ok",
      watchpoints: result.value,
      meta: { format: "legacy-array", version: null, profileId: null, source: "legacy", writtenAt: null, contentHash: null },
    };
  }

  if (!isEnvelope(parsed) || parsed.format !== WATCHPOINTS_FILE_FORMAT) {
    return { status: "corrupt", reason: "file is neither an hst-watchpoints envelope nor a watchpoint array" };
  }
  if (typeof parsed.version !== "number" || parsed.version !== WATCHPOINTS_FILE_VERSION) {
    return { status: "unsupported-version", reason: `unsupported watchpoints file version ${String(parsed.version)}` };
  }
  const meta = {
    format: parsed.format,
    version: parsed.version,
    profileId: typeof parsed.profileId === "string" ? parsed.profileId : null,
    source: typeof parsed.source === "string" ? parsed.source : null,
    writtenAt: typeof parsed.writtenAt === "string" ? parsed.writtenAt : null,
    contentHash: typeof parsed.contentHash === "string" ? parsed.contentHash : null,
  };
  if (typeof parsed.contentHash !== "string" || !/^[0-9a-f]{64}$/.test(parsed.contentHash)) {
    return { status: "corrupt", reason: "contentHash is missing or malformed" };
  }
  const result = validateWatchpointList(parsed.watchpoints);
  if (!result.ok) return { status: "corrupt", reason: `watchpoints: ${result.reason}` };
  const computed = contentHash(result.value);
  if (computed !== parsed.contentHash) {
    return { status: "hash-mismatch", reason: "declared contentHash does not match the watchpoint array", meta };
  }
  return { status: "ok", watchpoints: result.value, meta };
}

/**
 * True when the file is present, valid and its content hash matches the given
 * canonical watchpoint set — i.e. the derived artifact is not stale.
 */
export function fileMatchesWatchpoints(filePath, watchpoints) {
  const state = readWatchpointsFile(filePath);
  return state.status === "ok" && state.meta.contentHash === contentHash(watchpoints);
}
