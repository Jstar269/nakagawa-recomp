// watchpoint-store.ts — canonical persistence + derived-file publication for
// watchpoints and debug profiles (issue #188).
//
// The database is the canonical store. `watchpoints.json` is a derived runtime
// artifact carrying format/version/profileId/contentHash; it is produced with
// atomic publication and is reported as stale/corrupt when it no longer
// matches the DB (never silently regenerated on GET).
//
// Concurrency: every read-modify-write on the DB path runs inside a Prisma
// interactive transaction, which better-sqlite3 fully serializes (verified
// empirically in PR #285) — so two concurrent watchpoint mutations cannot
// silently overwrite each other. The direct-file mode (no active debug
// profile) has no DB to serialize on, so its read-modify-write is serialized
// by an in-process async mutex. Timing is never used as a correctness
// mechanism.

import { db as globalDb } from "@/lib/db";
import type { PrismaClient } from "@/generated/prisma/client";
import path from "node:path";
import { findRepoRoot } from "./runner";
import {
  WATCHPOINT_SCHEMA_VERSION,
  MAX_WATCHPOINTS,
  normalizeWatchpoint,
  validateWatchpointList,
  parseStoredWatchpoints,
  parseDebugMask,
  parseStrictBoolean,
  parseDebugProfileName,
} from "./watchpoint-schema.mjs";
import {
  WATCHPOINTS_FILE_NAME,
  serializeWatchpointsFile,
  atomicWriteFileSync,
  readWatchpointsFile,
  fileMatchesWatchpoints,
  contentHash,
} from "./watchpoint-file.mjs";

export class WatchpointStoreError extends Error {
  code: string;
  status: number;
  fields: { path: string; code: string; message: string }[] | null;

  constructor(code: string, status: number, message: string) {
    super(message);
    this.name = "WatchpointStoreError";
    this.code = code;
    this.status = status;
    this.fields = null;
  }
}

// ---- Derived file location -----------------------------------------------

/**
 * Canonical derived-artifact path. `SR_WATCHPOINTS_FILE` overrides the default
 * repo-root location (mirrors the runtime's env-driven configuration; also
 * used by tests to keep the artifact out of the worktree).
 */
export function watchpointsFilePath(): string {
  const override = process.env.SR_WATCHPOINTS_FILE;
  if (override && override.trim().length > 0) return override;
  return path.join(findRepoRoot(), WATCHPOINTS_FILE_NAME);
}

// ---- In-process async mutex (direct-file mode serialization) -------------

let fileLockChain: Promise<unknown> = Promise.resolve();

async function withFileLock<T>(fn: () => Promise<T>): Promise<T> {
  const run = fileLockChain.then(fn, fn);
  fileLockChain = run.then(
    () => undefined,
    () => undefined,
  );
  return run;
}

// ---- Internal helpers ----------------------------------------------------

type Client = PrismaClient;

function publishedFileState(filePath: string): { ok: boolean; detail: string } {
  const state = readWatchpointsFile(filePath);
  switch (state.status) {
    case "ok":
      return { ok: true, detail: "synced" };
    case "missing":
      return { ok: false, detail: "missing" };
    case "corrupt":
      return { ok: false, detail: `corrupt: ${state.reason}` };
    case "unsupported-version":
      return { ok: false, detail: `unsupported-version: ${state.reason}` };
    case "hash-mismatch":
      return { ok: false, detail: `stale: ${state.reason}` };
  }
}

function parseStoredOrThrow(raw: string): { start: number; end: number; label: string }[] {
  const result = parseStoredWatchpoints(raw);
  if (!result.ok) {
    throw new WatchpointStoreError("corrupt", 500, `stored watchpoints are invalid: ${result.reason}`);
  }
  return result.value;
}

function writeDerivedFile(
  filePath: string,
  watchpoints: { start: number; end: number; label: string }[],
  profileId: string | null,
  source: "db" | "direct",
): { ok: boolean; detail: string } {
  const content = serializeWatchpointsFile({
    watchpoints,
    profileId,
    source,
    writtenAt: new Date().toISOString(),
  });
  try {
    atomicWriteFileSync(filePath, content);
    return { ok: true, detail: "synced" };
  } catch (err) {
    return { ok: false, detail: `write-failed: ${(err as Error).message}` };
  }
}

function applyMutation(
  list: { start: number; end: number; label: string }[],
  op: "add" | "delete-label" | "delete-start" | "clear",
  arg: { label?: string; start?: number; watchpoint?: { start: number; end: number; label: string } },
): { list: { start: number; end: number; label: string }[]; changed: boolean } {
  switch (op) {
    case "add": {
      const wp = arg.watchpoint!;
      if (list.length >= MAX_WATCHPOINTS) {
        throw new WatchpointStoreError("limit-exceeded", 400, `Maximum of ${MAX_WATCHPOINTS} watchpoints can be active simultaneously`);
      }
      if (list.some((w) => w.start === wp.start && w.end === wp.end)) {
        throw new WatchpointStoreError("duplicate-watchpoint", 400, "A watchpoint with the same start/end range already exists");
      }
      return { list: [...list, wp], changed: true };
    }
    case "delete-label": {
      const filtered = list.filter((w) => w.label !== arg.label);
      return { list: filtered, changed: filtered.length !== list.length };
    }
    case "delete-start": {
      const filtered = list.filter((w) => w.start !== arg.start);
      return { list: filtered, changed: filtered.length !== list.length };
    }
    case "clear":
      return { list: [], changed: list.length > 0 };
  }
}

// ---- Read paths ----------------------------------------------------------

export type WatchpointReadResult = {
  watchpoints: { start: number; end: number; label: string }[];
  source: "profile" | "file";
  fileState: { ok: boolean; detail: string };
  profileId: string | null;
};

/**
 * Read the effective watchpoint set. When an active debug profile exists, the
 * DB is canonical; otherwise the file is the direct store. The file state is
 * always reported so a stale derived artifact is visible, never silently
 * regenerated.
 */
export async function getWatchpoints(
  client: Client = globalDb,
  filePath: string = watchpointsFilePath(),
): Promise<WatchpointReadResult> {
  const active = await client.debugProfile.findFirst({ where: { isActive: true } });
  if (active) {
    if (typeof active.schemaVersion !== "number" || active.schemaVersion !== WATCHPOINT_SCHEMA_VERSION) {
      throw new WatchpointStoreError("unsupported-version", 500, `Active debug profile uses unsupported schema version ${String(active.schemaVersion)}`);
    }
    const watchpoints = parseStoredOrThrow(active.watchpoints);
    const synced = fileMatchesWatchpoints(filePath, watchpoints);
    if (synced) {
      return { watchpoints, source: "profile", profileId: active.id, fileState: { ok: true, detail: "synced" } };
    }
    const fileState = publishedFileState(filePath);
    return {
      watchpoints,
      source: "profile",
      profileId: active.id,
      fileState: fileState.ok ? { ok: false, detail: "stale: file hash does not match the database" } : fileState,
    };
  }
  const state = readWatchpointsFile(filePath);
  if (state.status === "ok") {
    return { watchpoints: state.watchpoints, source: "file", fileState: { ok: true, detail: "direct" }, profileId: state.meta.profileId };
  }
  if (state.status === "missing") {
    return { watchpoints: [], source: "file", fileState: { ok: true, detail: "missing" }, profileId: null };
  }
  throw new WatchpointStoreError("corrupt", 500, `watchpoints.json is ${state.status}: ${state.reason}`);
}

// ---- Mutations -----------------------------------------------------------

export interface MutationResult {
  watchpoints: { start: number; end: number; label: string }[];
  added?: { start: number; end: number; label: string };
  source: "profile" | "file";
  fileState: { ok: boolean; detail: string };
}

/**
 * Mutate the effective watchpoint set. With an active profile the mutation
 * runs inside an interactive transaction (read + validate + apply + write are
 * serialized against concurrent requests); in direct mode it runs under the
 * file lock. The derived file is then republished.
 */
export async function mutateWatchpoints(
  op: "add" | "delete-label" | "delete-start" | "clear",
  arg: { label?: string; start?: number; watchpoint?: { start: number; end: number; label: string } },
  client: Client = globalDb,
  filePath: string = watchpointsFilePath(),
): Promise<MutationResult> {
  const active = await client.debugProfile.findFirst({ where: { isActive: true } });

  if (active) {
    const updated = await client.$transaction(async (tx) => {
      const current = await tx.debugProfile.findFirst({ where: { isActive: true } });
      if (!current) {
        // The active profile disappeared between our read and the transaction.
        throw new WatchpointStoreError("conflict", 409, "Active debug profile changed during the request; retry");
      }
      const list = parseStoredOrThrow(current.watchpoints);
      const result = applyMutation(list, op, arg);
      if (result.changed) {
        await tx.debugProfile.update({
          where: { id: current.id },
          data: { watchpoints: JSON.stringify(result.list) },
        });
      }
      return { list: result.list, id: current.id };
    });
    const fileState = writeDerivedFile(filePath, updated.list, updated.id, "db");
    return {
      watchpoints: updated.list,
      added: op === "add" ? arg.watchpoint : undefined,
      source: "profile",
      fileState,
    };
  }

  // Direct file mode: serialize the read-modify-write under the in-process lock.
  const result = await withFileLock(async () => {
    const state = readWatchpointsFile(filePath);
    let list: { start: number; end: number; label: string }[];
    if (state.status === "ok") {
      list = state.watchpoints;
    } else if (state.status === "missing") {
      list = [];
    } else {
      throw new WatchpointStoreError("corrupt", 500, `watchpoints.json is ${state.status}: ${state.reason}`);
    }
    const mutated = applyMutation(list, op, arg);
    let fileState = { ok: true, detail: "unchanged" };
    if (mutated.changed) {
      fileState = writeDerivedFile(filePath, mutated.list, null, "direct");
    }
    return { list: mutated.list, fileState };
  });
  return {
    watchpoints: result.list,
    added: op === "add" ? arg.watchpoint : undefined,
    source: "file",
    fileState: result.fileState,
  };
}

// ---- Debug profiles ------------------------------------------------------

export interface DebugProfileView {
  id: string;
  name: string;
  watchpoints: { start: number; end: number; label: string }[];
  debugMask: number;
  isActive: boolean;
  schemaVersion: number;
  createdAt: Date;
  updatedAt: Date;
}

function profileView(row: {
  id: string;
  name: string;
  watchpoints: string;
  debugMask: number;
  isActive: boolean;
  schemaVersion: number;
  createdAt: Date;
  updatedAt: Date;
}): DebugProfileView {
  return {
    id: row.id,
    name: row.name,
    watchpoints: parseStoredOrThrow(row.watchpoints),
    debugMask: row.debugMask,
    isActive: row.isActive,
    schemaVersion: row.schemaVersion,
    createdAt: row.createdAt,
    updatedAt: row.updatedAt,
  };
}

export async function listDebugProfiles(client: Client = globalDb): Promise<{
  profiles: DebugProfileView[];
  corrupt: { id: string; name: string; reason: string }[];
}> {
  const rows = await client.debugProfile.findMany({ orderBy: { createdAt: "desc" } });
  const profiles: DebugProfileView[] = [];
  const corrupt: { id: string; name: string; reason: string }[] = [];
  for (const row of rows) {
    if (typeof row.schemaVersion !== "number" || row.schemaVersion !== WATCHPOINT_SCHEMA_VERSION) {
      corrupt.push({ id: row.id, name: row.name, reason: `unsupported schema version ${String(row.schemaVersion)}` });
      continue;
    }
    try {
      profiles.push(profileView(row));
    } catch (err) {
      corrupt.push({ id: row.id, name: row.name, reason: (err as Error).message });
    }
  }
  return { profiles, corrupt };
}

export async function createDebugProfile(
  input: { name: unknown; watchpoints?: unknown; debugMask?: unknown },
  client: Client = globalDb,
): Promise<DebugProfileView> {
  const nameResult = parseDebugProfileName(input.name);
  if (!nameResult.ok) throw new WatchpointStoreError("invalid-name", 400, nameResult.reason);
  // Explicit null is invalid (consistent with PUT); only an omitted field
  // defaults to an empty list.
  const listResult =
    typeof input.watchpoints === "string"
      ? parseStoredWatchpoints(input.watchpoints)
      : input.watchpoints === undefined
        ? validateWatchpointList([])
        : validateWatchpointList(input.watchpoints);
  if (!listResult.ok) throw new WatchpointStoreError("invalid-watchpoints", 400, listResult.reason);
  const maskResult = parseDebugMask(input.debugMask ?? 0);
  if (!maskResult.ok) throw new WatchpointStoreError("invalid-debug-mask", 400, maskResult.reason);

  const row = await client.$transaction(async (tx) => {
    const existing = await tx.debugProfile.findUnique({ where: { name: nameResult.value } });
    if (existing) throw new WatchpointStoreError("duplicate-name", 400, "A profile with this name already exists");
    return tx.debugProfile.create({
      data: {
        name: nameResult.value,
        watchpoints: JSON.stringify(listResult.value),
        debugMask: maskResult.value,
        isActive: false,
        schemaVersion: WATCHPOINT_SCHEMA_VERSION,
      },
    });
  });
  return profileView(row);
}

export interface UpdateDebugProfileResult {
  profile: DebugProfileView;
  fileState: { ok: boolean; detail: string };
}

export async function updateDebugProfile(
  input: { id: string; name?: unknown; watchpoints?: unknown; debugMask?: unknown; isActive?: unknown },
  client: Client = globalDb,
  filePath: string = watchpointsFilePath(),
): Promise<UpdateDebugProfileResult> {
  const id = input.id;
  let name: string | undefined;
  if (input.name !== undefined) {
    const nameResult = parseDebugProfileName(input.name);
    if (!nameResult.ok) throw new WatchpointStoreError("invalid-name", 400, nameResult.reason);
    name = nameResult.value;
  }
  let watchpoints: { start: number; end: number; label: string }[] | undefined;
  if (input.watchpoints !== undefined) {
    const listResult =
      typeof input.watchpoints === "string"
        ? parseStoredWatchpoints(input.watchpoints)
        : validateWatchpointList(input.watchpoints);
    if (!listResult.ok) throw new WatchpointStoreError("invalid-watchpoints", 400, listResult.reason);
    watchpoints = listResult.value;
  }
  let debugMask: number | undefined;
  if (input.debugMask !== undefined) {
    const maskResult = parseDebugMask(input.debugMask);
    if (!maskResult.ok) throw new WatchpointStoreError("invalid-debug-mask", 400, maskResult.reason);
    debugMask = maskResult.value;
  }
  // isActive must be an actual boolean — never a truthy coercion.
  let activate: boolean | undefined;
  if (input.isActive !== undefined) {
    const boolResult = parseStrictBoolean(input.isActive);
    if (!boolResult.ok) throw new WatchpointStoreError("invalid-is-active", 400, boolResult.reason);
    activate = boolResult.value;
  }

  const row = await client.$transaction(async (tx) => {
    const existing = await tx.debugProfile.findUnique({ where: { id } });
    if (!existing) throw new WatchpointStoreError("profile-not-found", 404, "Debug profile not found");
    const data: Record<string, unknown> = {};
    if (name !== undefined) data.name = name;
    if (watchpoints !== undefined) data.watchpoints = JSON.stringify(watchpoints);
    if (debugMask !== undefined) data.debugMask = debugMask;
    if (activate === true) {
      // Deactivate all others, activate this one — atomically.
      await tx.debugProfile.updateMany({ where: { id: { not: id } }, data: { isActive: false } });
      data.isActive = true;
    } else if (activate === false) {
      // Deactivating the active profile with no replacement is an explicit
      // user choice: the runtime falls back to the (republished, empty) file
      // until another profile is activated. This is a coherent contract, not
      // an error path.
      data.isActive = false;
    }
    return tx.debugProfile.update({ where: { id }, data });
  });

  // Republish the derived file for the new active state.
  const active = await client.debugProfile.findFirst({ where: { isActive: true } });
  if (active) {
    const list = parseStoredOrThrow(active.watchpoints);
    const fileState = writeDerivedFile(filePath, list, active.id, "db");
    return { profile: profileView(row), fileState };
  }
  const fileState = writeDerivedFile(filePath, [], null, "direct");
  return { profile: profileView(row), fileState };
}

export async function deleteDebugProfile(
  id: string,
  client: Client = globalDb,
  filePath: string = watchpointsFilePath(),
): Promise<{ fileState: { ok: boolean; detail: string } }> {
  const wasActive = await client.$transaction(async (tx) => {
    const existing = await tx.debugProfile.findUnique({ where: { id } });
    if (!existing) throw new WatchpointStoreError("profile-not-found", 404, "Debug profile not found");
    await tx.debugProfile.delete({ where: { id } });
    return existing.isActive;
  });
  if (wasActive) {
    const stillActive = await client.debugProfile.findFirst({ where: { isActive: true } });
    const fileState = stillActive
      ? writeDerivedFile(filePath, parseStoredOrThrow(stillActive.watchpoints), stillActive.id, "db")
      : writeDerivedFile(filePath, [], null, "direct");
    return { fileState };
  }
  return { fileState: { ok: true, detail: "unchanged" } };
}
