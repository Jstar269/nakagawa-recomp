export const WATCHPOINT_SCHEMA_VERSION: number;
export const MAX_WATCHPOINTS: number;
export const MAX_WATCHPOINT_SPAN: number;
export const MAX_LABEL_BYTES: number;
export const DEBUG_PROFILE_NAME_MAX_BYTES: number;
export const MAX_DEBUG_MASK: number;

export interface NormalizedWatchpoint {
  start: number;
  end: number;
  label: string;
}

export interface OkResult<T> {
  ok: true;
  value: T;
  reason: null;
}

export interface ErrResult {
  ok: false;
  value: null;
  reason: string;
}

export type Result<T> = OkResult<T> | ErrResult;

export function parseWatchAddress(raw: unknown): Result<number>;
export function normalizeWatchpoint(input: unknown): Result<NormalizedWatchpoint>;
export function validateWatchpointList(input: unknown): Result<NormalizedWatchpoint[]>;
export function parseStoredWatchpoints(raw: unknown): Result<NormalizedWatchpoint[]>;
export function parseDebugMask(raw: unknown): Result<number>;
export function parseStrictBoolean(raw: unknown): Result<boolean>;
export function parseDebugProfileName(raw: unknown): Result<string>;
