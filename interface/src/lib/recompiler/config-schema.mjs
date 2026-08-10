// config-schema.mjs — runtime schema validation for the recompiler configuration.
//
// Issue #188: dashboard configuration is persisted and later cast back into
// trusted application types without validation. This module is the single
// authoritative runtime validator for a `RecompilerConfig` object and for the
// profile-name field. API routes reject invalid request bodies with field-level
// errors BEFORE persistence, and classify stored rows on read as ok / corrupt /
// unsupported-version instead of silently substituting defaults.
//
// This module is intentionally pure (no DB, no fs, no crypto) so it can be
// unit-tested with `node --test` like the other `*.mjs` dashboard modules.
//
// NOTE: the string-typed unions in `types.ts` are the compile-time view of the
// same domain. A source-shape test (`config-schema.test.mjs`) locks this
// module's value sets to the literals declared in `types.ts` so the two cannot
// drift apart silently.

// Version of the persisted configuration schema. Stored rows carry this value
// (RecompilerProfile.schemaVersion); readers report `unsupported-version` for
// any other value and never guess.
export const CONFIG_SCHEMA_VERSION = 1;

// ---- Size / depth limits (reject excessive input before persistence) -----

// Maximum UTF-8 byte length of a profile name.
export const PROFILE_NAME_MAX_BYTES = 100;

// Maximum JSON nesting depth of a config object.
export const MAX_CONFIG_DEPTH = 12;

// Maximum serialized size (UTF-8 bytes) of a config object.
export const MAX_CONFIG_JSON_BYTES = 128 * 1024;

// Maximum number of controller bindings.
export const MAX_BINDINGS = 32;

// Maximum UTF-8 byte length of a single controller-binding string field.
export const MAX_BINDING_FIELD_BYTES = 200;

// ---- Enum value sets (mirrored by the unions in types.ts) ----------------

export const RESOLUTION_PRESETS = [
  "native", "x2", "x3", "x4", "x6", "x8", "custom",
];
export const FRAME_RATE_CAPS = ["native", "60", "120", "unlocked"];
export const ASPECT_RATIOS = ["native", "16:9", "16:10", "21:9", "4:3", "stretch"];
export const TEXTURE_FILTERS = ["native", "bilinear", "bicubic", "xbrz", "hybrid"];
export const MSAA_LEVELS = ["off", "2x", "4x", "8x"];
export const ANISOTROPY_LEVELS = ["off", "2x", "4x", "8x", "16x"];
export const TEXTURE_UPSCALES = ["off", "x2", "x4", "x6"];
export const MOTION_BLUR_MODES = ["native", "off", "enhanced"];
export const DEPTH_OF_FIELD_MODES = ["native", "off", "enhanced"];
export const CPU_CLOCK_MODES = ["native", "max333", "333 unlocked", "oc444"];
export const THREAD_AFFINITIES = ["single", "dual", "all"];
export const FRAME_SKIPS = ["off", "auto", "1", "2", "3"];
export const MINIMIZE_STRATEGIES = ["portable", "minimal", "ultra"];
export const CONTROLLER_DEVICES = [
  "dualsense", "xbox-series", "switch-pro", "xbox-elite", "generic-xinput",
];
export const PSP_LIMITS = [
  "memory32", "vram2mb", "drawCalls", "saveSize1mb", "adHocLimit",
  "cpu222cap", "meCoreLock", "textureSwizzle", "depthBuffer16", "audio44khz",
];
export const PATCH_IDS = [
  "courtHD", "ballTrail", "hudReposition", "cameraFree", "skipIntroLogos",
  "unlockAllCourts", "proAiRebalance", "widescreenFix", "audioRemaster",
  "shadowSoftening",
];
export const CONTROLLER_BINDING_KEYS = ["pspAction", "label", "defaultPsp", "mappedTo"];

// Numeric field bounds. These are deliberately wider than the current UI
// sliders so legitimate dashboard output can never be rejected, while still
// blocking garbage (negatives, absurd magnitudes, non-finite values).
const NUMERIC_BOUNDS = {
  customWidth: { min: 1, max: 16384, integer: true },
  customHeight: { min: 1, max: 16384, integer: true },
  renderToTextureScale: { min: 0.25, max: 8, integer: false },
  hudScale: { min: 0.5, max: 4, integer: false },
  sharpness: { min: 0, max: 100, integer: true },
  deadzone: { min: 0, max: 100, integer: true },
  triggerSensitivity: { min: 0, max: 100, integer: true },
};

// ---- Validation helpers --------------------------------------------------

function addError(errors, path, code, message) {
  errors.push({ path, code, message });
}

function isPlainObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function checkKnownKeys(obj, known, path, errors) {
  for (const key of Object.keys(obj)) {
    if (!known.includes(key)) {
      addError(errors, `${path}.${key}`, "unknown-key", `Unknown key "${key}"`);
    }
  }
}

function checkDepth(value, depth) {
  if (depth > MAX_CONFIG_DEPTH) return false;
  if (isPlainObject(value)) {
    for (const key of Object.keys(value)) {
      if (!checkDepth(value[key], depth + 1)) return false;
    }
  } else if (Array.isArray(value)) {
    for (const item of value) {
      if (!checkDepth(item, depth + 1)) return false;
    }
  }
  return true;
}

function checkNumberField(value, path, errors, { min, max, integer }) {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    addError(errors, path, "invalid-number", `Expected a finite number, got ${describe(value)}`);
    return false;
  }
  if (integer && !Number.isInteger(value)) {
    addError(errors, path, "non-integer", `Expected an integer, got ${value}`);
    return false;
  }
  if (value < min || value > max) {
    addError(errors, path, "out-of-range", `Value ${value} is outside [${min}, ${max}]`);
    return false;
  }
  return true;
}

function checkBooleanField(value, path, errors) {
  if (typeof value !== "boolean") {
    addError(errors, path, "invalid-boolean", `Expected a boolean, got ${describe(value)}`);
    return false;
  }
  return true;
}

function checkEnumField(value, allowed, path, errors) {
  if (!allowed.includes(value)) {
    addError(errors, path, "invalid-enum", `Expected one of ${allowed.map((v) => JSON.stringify(v)).join(", ")}, got ${describe(value)}`);
    return false;
  }
  return true;
}

function describe(value) {
  if (value === null) return "null";
  if (value === undefined) return "undefined";
  if (typeof value === "string") return `string "${value.slice(0, 40)}"`;
  if (typeof value === "number") return `number ${value}`;
  if (Array.isArray(value)) return "array";
  return typeof value;
}

function utf8Length(value) {
  return Buffer.byteLength(value, "utf8");
}

// ---- Public validators ---------------------------------------------------

/** Validate a profile name. Returns { ok, value, error }. */
export function validateProfileName(raw) {
  if (typeof raw !== "string") {
    return { ok: false, value: null, error: { code: "invalid-name", message: "Profile name must be a string" } };
  }
  const name = raw.trim();
  if (name.length === 0) {
    return { ok: false, value: null, error: { code: "empty-name", message: "Profile name must not be empty" } };
  }
  if (/[\u0000-\u001f\u007f]/.test(name)) {
    return { ok: false, value: null, error: { code: "control-chars", message: "Profile name must not contain control characters" } };
  }
  if (utf8Length(name) > PROFILE_NAME_MAX_BYTES) {
    return { ok: false, value: null, error: { code: "name-too-long", message: `Profile name exceeds ${PROFILE_NAME_MAX_BYTES} bytes` } };
  }
  return { ok: true, value: name, error: null };
}

/**
 * Deep-validate a RecompilerConfig-like input. Returns
 * { ok, errors, value } where `value` is a normalized copy (the caller object
 * is never mutated). `errors` is a field-level list; each entry is
 * { path, code, message }.
 */
export function validateConfig(input) {
  const errors = [];
  if (!isPlainObject(input)) {
    return { ok: false, errors: [{ path: "", code: "not-object", message: "Config must be a JSON object" }], value: null };
  }
  if (utf8Length(JSON.stringify(input)) > MAX_CONFIG_JSON_BYTES) {
    return { ok: false, errors: [{ path: "", code: "config-too-large", message: `Config exceeds ${MAX_CONFIG_JSON_BYTES} bytes` }], value: null };
  }
  if (!checkDepth(input, 0)) {
    return { ok: false, errors: [{ path: "", code: "too-deep", message: `Config nesting exceeds ${MAX_CONFIG_DEPTH} levels` }], value: null };
  }

  checkKnownKeys(input, ["profileName", "graphics", "performance", "limitations", "controllers", "patches", "minimizeStrategy", "updatedAt"], "", errors);

  if (typeof input.profileName !== "string") {
    addError(errors, "profileName", "invalid-type", `Expected a string, got ${describe(input.profileName)}`);
  }

  // graphics
  const g = input.graphics;
  if (!isPlainObject(g)) {
    addError(errors, "graphics", "invalid-type", "Expected an object");
  } else {
    checkKnownKeys(g, [
      "resolutionPreset", "customWidth", "customHeight", "renderToTextureScale",
      "frameRateCap", "framePacing", "vsync", "tripleBuffering", "aspectRatio",
      "widescreenHack", "textureFilter", "textureUpscale", "msaa", "anisotropy",
      "motionBlur", "depthOfField", "hudScale", "sharpness",
    ], "graphics", errors);
    checkEnumField(g.resolutionPreset, RESOLUTION_PRESETS, "graphics.resolutionPreset", errors);
    checkNumberField(g.customWidth, "graphics.customWidth", errors, NUMERIC_BOUNDS.customWidth);
    checkNumberField(g.customHeight, "graphics.customHeight", errors, NUMERIC_BOUNDS.customHeight);
    checkNumberField(g.renderToTextureScale, "graphics.renderToTextureScale", errors, NUMERIC_BOUNDS.renderToTextureScale);
    checkEnumField(g.frameRateCap, FRAME_RATE_CAPS, "graphics.frameRateCap", errors);
    checkBooleanField(g.framePacing, "graphics.framePacing", errors);
    checkBooleanField(g.vsync, "graphics.vsync", errors);
    checkBooleanField(g.tripleBuffering, "graphics.tripleBuffering", errors);
    checkEnumField(g.aspectRatio, ASPECT_RATIOS, "graphics.aspectRatio", errors);
    checkBooleanField(g.widescreenHack, "graphics.widescreenHack", errors);
    checkEnumField(g.textureFilter, TEXTURE_FILTERS, "graphics.textureFilter", errors);
    checkEnumField(g.textureUpscale, TEXTURE_UPSCALES, "graphics.textureUpscale", errors);
    checkEnumField(g.msaa, MSAA_LEVELS, "graphics.msaa", errors);
    checkEnumField(g.anisotropy, ANISOTROPY_LEVELS, "graphics.anisotropy", errors);
    checkEnumField(g.motionBlur, MOTION_BLUR_MODES, "graphics.motionBlur", errors);
    checkEnumField(g.depthOfField, DEPTH_OF_FIELD_MODES, "graphics.depthOfField", errors);
    checkNumberField(g.hudScale, "graphics.hudScale", errors, NUMERIC_BOUNDS.hudScale);
    checkNumberField(g.sharpness, "graphics.sharpness", errors, NUMERIC_BOUNDS.sharpness);
  }

  // performance
  const p = input.performance;
  if (!isPlainObject(p)) {
    addError(errors, "performance", "invalid-type", "Expected an object");
  } else {
    checkKnownKeys(p, [
      "cpuClockMode", "threadAffinity", "fastMemory", "blockLinking", "jitCache",
      "vertexCache", "shaderCache", "frameSkip", "ioThreading", "fastForwardUnthrottled",
    ], "performance", errors);
    checkEnumField(p.cpuClockMode, CPU_CLOCK_MODES, "performance.cpuClockMode", errors);
    checkEnumField(p.threadAffinity, THREAD_AFFINITIES, "performance.threadAffinity", errors);
    for (const key of ["fastMemory", "blockLinking", "jitCache", "vertexCache", "shaderCache", "ioThreading", "fastForwardUnthrottled"]) {
      checkBooleanField(p[key], `performance.${key}`, errors);
    }
    checkEnumField(p.frameSkip, FRAME_SKIPS, "performance.frameSkip", errors);
  }

  // limitations.removed
  const lim = input.limitations;
  if (!isPlainObject(lim)) {
    addError(errors, "limitations", "invalid-type", "Expected an object");
  } else {
    checkKnownKeys(lim, ["removed"], "limitations", errors);
    const removed = lim.removed;
    if (!isPlainObject(removed)) {
      addError(errors, "limitations.removed", "invalid-type", "Expected an object");
    } else {
      checkKnownKeys(removed, PSP_LIMITS, "limitations.removed", errors);
      for (const limit of PSP_LIMITS) {
        checkBooleanField(removed[limit], `limitations.removed.${limit}`, errors);
      }
    }
  }

  // controllers
  const c = input.controllers;
  if (!isPlainObject(c)) {
    addError(errors, "controllers", "invalid-type", "Expected an object");
  } else {
    checkKnownKeys(c, ["device", "deadzone", "triggerSensitivity", "rumble", "adaptiveTriggers", "gyroAim", "bindings"], "controllers", errors);
    checkEnumField(c.device, CONTROLLER_DEVICES, "controllers.device", errors);
    checkNumberField(c.deadzone, "controllers.deadzone", errors, NUMERIC_BOUNDS.deadzone);
    checkNumberField(c.triggerSensitivity, "controllers.triggerSensitivity", errors, NUMERIC_BOUNDS.triggerSensitivity);
    for (const key of ["rumble", "adaptiveTriggers", "gyroAim"]) {
      checkBooleanField(c[key], `controllers.${key}`, errors);
    }
    if (!Array.isArray(c.bindings)) {
      addError(errors, "controllers.bindings", "invalid-type", "Expected an array");
    } else if (c.bindings.length > MAX_BINDINGS) {
      addError(errors, "controllers.bindings", "too-many", `At most ${MAX_BINDINGS} bindings are allowed`);
    } else {
      c.bindings.forEach((binding, index) => {
        const path = `controllers.bindings[${index}]`;
        if (!isPlainObject(binding)) {
          addError(errors, path, "invalid-type", "Expected an object");
          return;
        }
        checkKnownKeys(binding, CONTROLLER_BINDING_KEYS, path, errors);
        for (const key of CONTROLLER_BINDING_KEYS) {
          const value = binding[key];
          if (typeof value !== "string") {
            addError(errors, `${path}.${key}`, "invalid-type", "Expected a string");
          } else if (utf8Length(value) > MAX_BINDING_FIELD_BYTES) {
            addError(errors, `${path}.${key}`, "too-long", `Exceeds ${MAX_BINDING_FIELD_BYTES} bytes`);
          }
        }
      });
    }
  }

  // patches.enabled
  const pat = input.patches;
  if (!isPlainObject(pat)) {
    addError(errors, "patches", "invalid-type", "Expected an object");
  } else {
    checkKnownKeys(pat, ["enabled"], "patches", errors);
    const enabled = pat.enabled;
    if (!isPlainObject(enabled)) {
      addError(errors, "patches.enabled", "invalid-type", "Expected an object");
    } else {
      checkKnownKeys(enabled, PATCH_IDS, "patches.enabled", errors);
      for (const patch of PATCH_IDS) {
        checkBooleanField(enabled[patch], `patches.enabled.${patch}`, errors);
      }
    }
  }

  checkEnumField(input.minimizeStrategy, MINIMIZE_STRATEGIES, "minimizeStrategy", errors);

  // updatedAt is server-managed on write; any value is tolerated here so the
  // stored timestamp is always authoritative (see profile-store).
  if (input.updatedAt !== undefined && typeof input.updatedAt !== "string" && typeof input.updatedAt !== "number") {
    addError(errors, "updatedAt", "invalid-type", "Expected a string or number");
  }

  if (errors.length > 0) {
    return { ok: false, errors, value: null };
  }

  // Return a defensive deep copy so callers cannot mutate the validator's view.
  return { ok: true, errors: [], value: structuredClone(input) };
}

/**
 * Classify a stored config value read from the database.
 * Returns { status: "ok"|"corrupt"|"unsupported-version", errors?, value? }.
 * `schemaVersion` comes from the row metadata (RecompilerProfile.schemaVersion).
 */
export function checkStoredConfig(value, schemaVersion) {
  if (typeof schemaVersion !== "number" || schemaVersion !== CONFIG_SCHEMA_VERSION) {
    const seen = typeof schemaVersion === "number" ? schemaVersion : String(schemaVersion);
    return { status: "unsupported-version", errors: [{ path: "", code: "unsupported-version", message: `Schema version ${seen} is not supported (expected ${CONFIG_SCHEMA_VERSION})` }], value: null };
  }
  const result = validateConfig(value);
  if (!result.ok) {
    return { status: "corrupt", errors: result.errors, value: null };
  }
  return { status: "ok", errors: [], value: result.value };
}

/**
 * Parse + classify a stored `configJson` string. Never throws.
 * Returns { status: "ok"|"corrupt"|"unsupported-version", errors?, value? }.
 */
export function parseStoredConfig(configJson, schemaVersion) {
  let parsed;
  try {
    parsed = JSON.parse(configJson);
  } catch (err) {
    return { status: "corrupt", errors: [{ path: "", code: "json-parse", message: `Stored config is not valid JSON: ${err.message}` }], value: null };
  }
  return checkStoredConfig(parsed, schemaVersion);
}

/**
 * Compute a safe human-readable summary of a validated config. This must never
 * throw for any input; the list route uses it so one malformed row cannot take
 * the whole profile list down.
 */
export function summarizeConfig(config) {
  const fallback = { resolution: "unknown", fps: "unknown", limitsRemoved: 0, patches: 0, strategy: "unknown" };
  if (!isPlainObject(config)) return fallback;
  const resMap = {
    native: "480x272", x2: "960x544", x3: "1440x816", x4: "1080p", x6: "1440p", x8: "4K",
  };
  const fpsMap = { native: "30", 60: "60", 120: "120", unlocked: "∞" };
  const graphics = config.graphics;
  const limitations = config.limitations;
  const patches = config.patches;
  if (!isPlainObject(graphics) || !isPlainObject(limitations) || !isPlainObject(patches)) {
    return fallback;
  }
  let resolution;
  if (graphics.resolutionPreset === "custom") {
    resolution = `${graphics.customWidth ?? "?"}x${graphics.customHeight ?? "?"}`;
  } else {
    resolution = resMap[graphics.resolutionPreset] ?? "480x272";
  }
  const removed = isPlainObject(limitations.removed)
    ? Object.values(limitations.removed).filter((v) => v === true).length
    : 0;
  const enabled = isPlainObject(patches.enabled)
    ? Object.values(patches.enabled).filter((v) => v === true).length
    : 0;
  return {
    resolution,
    fps: fpsMap[graphics.frameRateCap] ?? "30",
    limitsRemoved: removed,
    patches: enabled,
    strategy: config.minimizeStrategy ?? "unknown",
  };
}
