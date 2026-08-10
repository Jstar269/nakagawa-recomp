import type { NormalizedWatchpoint } from "./watchpoint-schema.mjs";

export const WATCHPOINTS_FILE_FORMAT: string;
export const WATCHPOINTS_FILE_VERSION: number;
export const WATCHPOINTS_FILE_MAX_BYTES: number;
export const WATCHPOINTS_FILE_NAME: string;

export function canonicalWatchpointsJson(watchpoints: NormalizedWatchpoint[]): string;
export function contentHash(watchpoints: NormalizedWatchpoint[]): string;

export interface SerializeOptions {
  watchpoints: NormalizedWatchpoint[];
  profileId?: string | null;
  source?: "db" | "direct" | "legacy" | null;
  writtenAt: string;
}

export function serializeWatchpointsFile(options: SerializeOptions): string;
export function atomicWriteFileSync(filePath: string, content: string): void;

export interface WatchpointsFileMeta {
  format: string;
  version: number | null;
  profileId: string | null;
  source: string | null;
  writtenAt: string | null;
  contentHash: string | null;
}

export type WatchpointsFileState =
  | { status: "missing" }
  | { status: "ok"; watchpoints: NormalizedWatchpoint[]; meta: WatchpointsFileMeta }
  | { status: "corrupt"; reason: string }
  | { status: "unsupported-version"; reason: string }
  | { status: "hash-mismatch"; reason: string; meta: WatchpointsFileMeta };

export function readWatchpointsFile(filePath: string): WatchpointsFileState;
export function fileMatchesWatchpoints(
  filePath: string,
  watchpoints: NormalizedWatchpoint[],
): boolean;
