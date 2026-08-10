export const CONFIG_SCHEMA_VERSION: number;
export const PROFILE_NAME_MAX_BYTES: number;
export const MAX_CONFIG_DEPTH: number;
export const MAX_CONFIG_JSON_BYTES: number;
export const MAX_BINDINGS: number;
export const MAX_BINDING_FIELD_BYTES: number;

export const RESOLUTION_PRESETS: readonly string[];
export const FRAME_RATE_CAPS: readonly string[];
export const ASPECT_RATIOS: readonly string[];
export const TEXTURE_FILTERS: readonly string[];
export const MSAA_LEVELS: readonly string[];
export const ANISOTROPY_LEVELS: readonly string[];
export const TEXTURE_UPSCALES: readonly string[];
export const MOTION_BLUR_MODES: readonly string[];
export const DEPTH_OF_FIELD_MODES: readonly string[];
export const CPU_CLOCK_MODES: readonly string[];
export const THREAD_AFFINITIES: readonly string[];
export const FRAME_SKIPS: readonly string[];
export const MINIMIZE_STRATEGIES: readonly string[];
export const CONTROLLER_DEVICES: readonly string[];
export const PSP_LIMITS: readonly string[];
export const PATCH_IDS: readonly string[];
export const CONTROLLER_BINDING_KEYS: readonly string[];

export interface SchemaError {
  path: string;
  code: string;
  message: string;
}

export interface ValidationResult {
  ok: boolean;
  errors: SchemaError[];
  value: unknown;
}

export interface NameValidationResult {
  ok: boolean;
  value: string | null;
  error: { code: string; message: string } | null;
}

export interface StoredConfigStatus {
  status: "ok" | "corrupt" | "unsupported-version";
  errors: SchemaError[];
  value: unknown;
}

export function validateProfileName(raw: unknown): NameValidationResult;
export function validateConfig(input: unknown): ValidationResult;
export function checkStoredConfig(value: unknown, schemaVersion: number | undefined): StoredConfigStatus;
export function parseStoredConfig(
  configJson: string,
  schemaVersion: number | undefined,
): StoredConfigStatus;
export function summarizeConfig(config: unknown): {
  resolution: string;
  fps: string;
  limitsRemoved: number;
  patches: number;
  strategy: string;
};
