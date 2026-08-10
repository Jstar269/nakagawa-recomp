// profile-store.ts — transactional persistence for recompiler profiles.
//
// Issue #188: activating/deactivating profiles previously ran as multiple
// non-transactional statements, so a failure between them could leave zero or
// several "default" rows, and check-then-act sequences (delete) raced with
// concurrent requests. This store makes every multi-step invariant atomic via
// Prisma interactive transactions (supported on SQLite with the better-sqlite3
// driver adapter — verified empirically) and validates every config before it
// is persisted or returned.
//
// Stored rows are classified on read as ok / corrupt / unsupported-version;
// corrupt rows are surfaced explicitly and are never silently replaced with
// defaults.

import { db as globalDb } from "@/lib/db";
import type { PrismaClient } from "@/generated/prisma/client";
import {
  CONFIG_SCHEMA_VERSION,
  validateConfig,
  validateProfileName,
  parseStoredConfig,
  summarizeConfig,
} from "./config-schema.mjs";

// ---- Errors --------------------------------------------------------------

export class ProfileStoreError extends Error {
  code: string;
  status: number;
  fields: { path: string; code: string; message: string }[] | null;

  constructor(
    code: string,
    status: number,
    message: string,
    fields: { path: string; code: string; message: string }[] | null = null,
  ) {
    super(message);
    this.name = "ProfileStoreError";
    this.code = code;
    this.status = status;
    this.fields = fields;
  }
}

const notFound = (id: string) =>
  new ProfileStoreError("profile-not-found", 404, `Profile ${id} does not exist`);
const conflict = (code: string, message: string) => new ProfileStoreError(code, 409, message);

// ---- Types ---------------------------------------------------------------

export interface StoredProfileView {
  id: string;
  name: string;
  isDefault: boolean;
  schemaVersion: number;
  createdAt: Date;
  updatedAt: Date;
}

export interface ProfileListEntry extends StoredProfileView {
  status: "ok" | "corrupt" | "unsupported-version";
  summary: ReturnType<typeof summarizeConfig> | null;
  corruptReason: string | null;
}

export interface ProfileDetail {
  row: StoredProfileView;
  config: unknown;
  status: "ok" | "corrupt" | "unsupported-version";
  errors: { path: string; code: string; message: string }[];
}

// ---- Helpers -------------------------------------------------------------

function rowToView(row: {
  id: string;
  name: string;
  isDefault: boolean;
  schemaVersion: number;
  createdAt: Date;
  updatedAt: Date;
}): StoredProfileView {
  return {
    id: row.id,
    name: row.name,
    isDefault: row.isDefault,
    schemaVersion: row.schemaVersion,
    createdAt: row.createdAt,
    updatedAt: row.updatedAt,
  };
}

function classify(row: {
  configJson: string;
  schemaVersion: number;
}): {
  status: "ok" | "corrupt" | "unsupported-version";
  config: unknown;
  errors: { path: string; code: string; message: string }[];
} {
  const result = parseStoredConfig(row.configJson, row.schemaVersion);
  return { status: result.status, config: result.value, errors: result.errors };
}

function validateNameOrThrow(name: unknown): string {
  const result = validateProfileName(name);
  if (!result.ok) {
    throw new ProfileStoreError(result.error!.code, 400, result.error!.message);
  }
  return result.value!;
}

function validateConfigOrThrow(input: unknown): Record<string, unknown> {
  const result = validateConfig(input);
  if (!result.ok) {
    throw new ProfileStoreError("validation-failed", 400, "Config failed validation", result.errors);
  }
  return result.value as Record<string, unknown>;
}

// ---- Store operations ----------------------------------------------------

/**
 * Read the active (default) profile. Returns null when none is saved.
 * Throws ProfileStoreError with a corrupt/unsupported-version status when the
 * stored active row cannot be trusted — it is never silently replaced.
 */
export async function getActiveProfile(client: PrismaClient = globalDb): Promise<ProfileDetail | null> {
  const row = await client.recompilerProfile.findFirst({
    where: { isDefault: true },
    orderBy: { updatedAt: "desc" },
  });
  if (!row) return null;
  const { status, config, errors } = classify(row);
  if (status !== "ok") {
    throw new ProfileStoreError(status, 500, `Active profile is ${status === "corrupt" ? "corrupt" : "from an unsupported schema version"}`, errors);
  }
  return { row: rowToView(row), config, status, errors };
}

/**
 * Create or update the active profile. The deactivate/activate and
 * create-if-missing transitions run inside one transaction so the
 * one-default invariant cannot be violated by concurrent requests.
 */
export async function upsertActiveProfile(
  input: { name?: unknown; config?: unknown },
  client: PrismaClient = globalDb,
): Promise<{ row: StoredProfileView; config: unknown }> {
  const config = validateConfigOrThrow(input.config);
  const name = validateNameOrThrow(input.name ?? (config.profileName as string) ?? "Untitled Profile");
  config.profileName = name;
  config.updatedAt = new Date().toISOString();

  const row = await client.$transaction(async (tx) => {
    const existing = await tx.recompilerProfile.findFirst({ where: { isDefault: true } });
    if (existing) {
      const updated = await tx.recompilerProfile.update({
        where: { id: existing.id },
        data: { name, configJson: JSON.stringify(config), schemaVersion: CONFIG_SCHEMA_VERSION },
      });
      // Heal legacy data: a pre-#188 database could contain more than one
      // default row; leave exactly one after every write.
      await tx.recompilerProfile.updateMany({
        where: { isDefault: true, id: { not: existing.id } },
        data: { isDefault: false },
      });
      return updated;
    }
    return tx.recompilerProfile.create({
      data: {
        name,
        configJson: JSON.stringify(config),
        isDefault: true,
        schemaVersion: CONFIG_SCHEMA_VERSION,
      },
    });
  });
  return { row: rowToView(row), config };
}

/** List all profiles; corrupt rows are reported separately, never crash the list. */
export async function listProfiles(client: PrismaClient = globalDb): Promise<{
  profiles: ProfileListEntry[];
  corrupt: { id: string; name: string; reason: string }[];
}> {
  const rows = await client.recompilerProfile.findMany({
    orderBy: [{ isDefault: "desc" }, { updatedAt: "desc" }],
  });
  const profiles: ProfileListEntry[] = [];
  const corrupt: { id: string; name: string; reason: string }[] = [];
  for (const row of rows) {
    const { status, config, errors } = classify(row);
    const view = rowToView(row);
    if (status === "ok") {
      profiles.push({
        ...view,
        status,
        summary: summarizeConfig(config),
        corruptReason: null,
      });
    } else {
      corrupt.push({
        id: row.id,
        name: row.name,
        reason: errors[0]?.message ?? status,
      });
    }
  }
  return { profiles, corrupt };
}

/**
 * Read one profile. Throws ProfileStoreError 404 when missing and 500 with a
 * corrupt/unsupported-version status when the stored config cannot be trusted.
 */
export async function getProfileById(
  id: string,
  client: PrismaClient = globalDb,
): Promise<ProfileDetail> {
  const row = await client.recompilerProfile.findUnique({ where: { id } });
  if (!row) throw notFound(id);
  const { status, config, errors } = classify(row);
  if (status !== "ok") {
    throw new ProfileStoreError(status, 500, `Profile ${id} is ${status === "corrupt" ? "corrupt" : "from an unsupported schema version"}`, errors);
  }
  return { row: rowToView(row), config, status, errors };
}

/**
 * Create a profile (optionally duplicating another). A missing or corrupt
 * duplicate source is a hard error — the caller is told the duplicate was not
 * performed instead of silently falling back to a default.
 */
export async function createProfile(
  input: { name?: unknown; config?: unknown; duplicateFrom?: string },
  client: PrismaClient = globalDb,
): Promise<{ row: StoredProfileView; config: unknown }> {
  let config: Record<string, unknown>;
  let name: string;

  if (input.duplicateFrom) {
    const src = await client.recompilerProfile.findUnique({ where: { id: input.duplicateFrom } });
    if (!src) {
      throw new ProfileStoreError("duplicate-source-not-found", 400, `Source profile ${input.duplicateFrom} does not exist`);
    }
    const { status, config: srcConfig, errors } = classify(src);
    if (status !== "ok") {
      throw new ProfileStoreError("duplicate-source-corrupt", 400, "Source profile failed validation", errors);
    }
    config = validateConfigOrThrow(srcConfig);
    name = validateNameOrThrow(input.name ?? `${src.name} (copy)`);
  } else {
    config = validateConfigOrThrow(input.config);
    name = validateNameOrThrow(input.name ?? (config.profileName as string) ?? "Untitled Profile");
  }

  config.profileName = name;
  config.updatedAt = new Date().toISOString();

  const row = await client.recompilerProfile.create({
    data: {
      name,
      configJson: JSON.stringify(config),
      isDefault: false,
      schemaVersion: CONFIG_SCHEMA_VERSION,
    },
  });
  return { row: rowToView(row), config };
}

/**
 * Activate a profile atomically: deactivate everything, then set the target,
 * inside a single transaction. Concurrent activations cannot leave multiple
 * defaults.
 */
export async function activateProfile(
  id: string,
  client: PrismaClient = globalDb,
): Promise<StoredProfileView> {
  const row = await client.$transaction(async (tx) => {
    const target = await tx.recompilerProfile.findUnique({ where: { id } });
    if (!target) throw notFound(id);
    await tx.recompilerProfile.updateMany({
      where: { isDefault: true },
      data: { isDefault: false },
    });
    return tx.recompilerProfile.update({
      where: { id },
      data: { isDefault: true },
    });
  });
  return rowToView(row);
}

/**
 * Read a profile's row metadata WITHOUT validating its stored config. Used by
 * rename/no-op PATCH paths so a corrupt config never blocks a metadata update.
 */
export async function getProfileMeta(
  id: string,
  client: PrismaClient = globalDb,
): Promise<StoredProfileView> {
  const row = await client.recompilerProfile.findUnique({ where: { id } });
  if (!row) throw notFound(id);
  return rowToView(row);
}

/** Rename a profile (validated). */
export async function renameProfile(
  id: string,
  name: unknown,
  client: PrismaClient = globalDb,
): Promise<StoredProfileView> {
  const clean = validateNameOrThrow(name);
  const existing = await client.recompilerProfile.findUnique({ where: { id } });
  if (!existing) throw notFound(id);
  const row = await client.recompilerProfile.update({ where: { id }, data: { name: clean } });
  return rowToView(row);
}

/**
 * Delete a profile. The existence check, active check, last-profile check and
 * delete run inside one transaction so concurrent requests cannot delete the
 * last profile or race the count check.
 */
export async function deleteProfile(id: string, client: PrismaClient = globalDb): Promise<void> {
  await client.$transaction(async (tx) => {
    const row = await tx.recompilerProfile.findUnique({ where: { id } });
    if (!row) throw notFound(id);
    if (row.isDefault) {
      throw conflict("cannot-delete-active", "Cannot delete the active profile. Activate another first.");
    }
    const count = await tx.recompilerProfile.count();
    if (count <= 1) {
      throw conflict("cannot-delete-last", "Cannot delete the last remaining profile.");
    }
    await tx.recompilerProfile.delete({ where: { id } });
  });
}
