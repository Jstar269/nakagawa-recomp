import assert from "node:assert/strict";
import test from "node:test";
import { readFileSync } from "node:fs";
import {
  CONFIG_SCHEMA_VERSION,
  validateConfig,
  validateProfileName,
  checkStoredConfig,
  parseStoredConfig,
  summarizeConfig,
  RESOLUTION_PRESETS,
  FRAME_RATE_CAPS,
  ASPECT_RATIOS,
  TEXTURE_FILTERS,
  MSAA_LEVELS,
  ANISOTROPY_LEVELS,
  TEXTURE_UPSCALES,
  MOTION_BLUR_MODES,
  DEPTH_OF_FIELD_MODES,
  CPU_CLOCK_MODES,
  THREAD_AFFINITIES,
  FRAME_SKIPS,
  MINIMIZE_STRATEGIES,
  CONTROLLER_DEVICES,
  PSP_LIMITS,
  PATCH_IDS,
  CONTROLLER_BINDING_KEYS,
  PROFILE_NAME_MAX_BYTES,
} from "./config-schema.mjs";

// ---- Fixture: a fully valid config (mirrors defaults.ts shapes) ----------

function validConfig(overrides = {}) {
  const cfg = {
    profileName: "Test Profile",
    graphics: {
      resolutionPreset: "x4",
      customWidth: 1920,
      customHeight: 1088,
      renderToTextureScale: 1,
      frameRateCap: "60",
      framePacing: true,
      vsync: true,
      tripleBuffering: true,
      aspectRatio: "16:9",
      widescreenHack: true,
      textureFilter: "xbrz",
      textureUpscale: "x4",
      msaa: "4x",
      anisotropy: "16x",
      motionBlur: "enhanced",
      depthOfField: "enhanced",
      hudScale: 1.1,
      sharpness: 35,
    },
    performance: {
      cpuClockMode: "333 unlocked",
      threadAffinity: "all",
      fastMemory: true,
      blockLinking: true,
      jitCache: true,
      vertexCache: true,
      shaderCache: true,
      frameSkip: "off",
      ioThreading: true,
      fastForwardUnthrottled: false,
    },
    limitations: {
      removed: {
        memory32: true, vram2mb: true, drawCalls: true, saveSize1mb: true,
        adHocLimit: false, cpu222cap: true, meCoreLock: false,
        textureSwizzle: true, depthBuffer16: true, audio44khz: true,
      },
    },
    controllers: {
      device: "dualsense",
      deadzone: 12,
      triggerSensitivity: 60,
      rumble: true,
      adaptiveTriggers: true,
      gyroAim: false,
      bindings: [
        { pspAction: "CIRCLE", label: "Top Spin", defaultPsp: "CIRCLE", mappedTo: "CIRCLE" },
      ],
    },
    patches: {
      enabled: {
        courtHD: false, ballTrail: false, hudReposition: true, cameraFree: false,
        skipIntroLogos: true, unlockAllCourts: false, proAiRebalance: false,
        widescreenFix: true, audioRemaster: false, shadowSoftening: false,
      },
    },
    minimizeStrategy: "minimal",
    updatedAt: "2026-08-05T00:00:00.000Z",
  };
  return { ...cfg, ...overrides };
}

function errorsFor(input) {
  const result = validateConfig(input);
  assert.equal(result.ok, false, `expected validation failure, got ok for ${JSON.stringify(input).slice(0, 80)}`);
  return result.errors;
}

function codesFor(input) {
  return errorsFor(input).map((e) => e.code);
}

// ---- Acceptance of valid input -------------------------------------------

test("accepts a fully valid config and returns a defensive copy", () => {
  const cfg = validConfig();
  const result = validateConfig(cfg);
  assert.equal(result.ok, true);
  assert.deepEqual(result.errors, []);
  assert.deepEqual(result.value, cfg);
  // Caller mutation must not affect the returned copy.
  cfg.graphics.sharpness = 999;
  assert.equal(result.value.graphics.sharpness, 35);
});

test("accepts every legal enum value for each enum field", () => {
  const base = validConfig();
  const cases = [
    ["graphics.resolutionPreset", RESOLUTION_PRESETS],
    ["graphics.frameRateCap", FRAME_RATE_CAPS],
    ["graphics.aspectRatio", ASPECT_RATIOS],
    ["graphics.textureFilter", TEXTURE_FILTERS],
    ["graphics.msaa", MSAA_LEVELS],
    ["graphics.anisotropy", ANISOTROPY_LEVELS],
    ["graphics.textureUpscale", TEXTURE_UPSCALES],
    ["graphics.motionBlur", MOTION_BLUR_MODES],
    ["graphics.depthOfField", DEPTH_OF_FIELD_MODES],
    ["performance.cpuClockMode", CPU_CLOCK_MODES],
    ["performance.threadAffinity", THREAD_AFFINITIES],
    ["performance.frameSkip", FRAME_SKIPS],
    ["controllers.device", CONTROLLER_DEVICES],
    ["minimizeStrategy", MINIMIZE_STRATEGIES],
  ];
  for (const [path, values] of cases) {
    for (const value of values) {
      const cfg = validConfig();
      const parts = path.split(".");
      let target = cfg;
      for (const part of parts.slice(0, -1)) target = target[part];
      target[parts[parts.length - 1]] = value;
      const result = validateConfig(cfg);
      assert.equal(result.ok, true, `expected ${path} = ${JSON.stringify(value)} to be accepted`);
    }
  }
});

test("accepts every limit and patch key", () => {
  for (const limit of PSP_LIMITS) {
    const cfg = validConfig();
    cfg.limitations.removed[limit] = true;
    assert.equal(validateConfig(cfg).ok, true, `limit ${limit}`);
  }
  for (const patch of PATCH_IDS) {
    const cfg = validConfig();
    cfg.patches.enabled[patch] = true;
    assert.equal(validateConfig(cfg).ok, true, `patch ${patch}`);
  }
});

// ---- Unknown keys --------------------------------------------------------

test("rejects unknown top-level keys", () => {
  const codes = codesFor(validConfig({ evilExtra: 1 }));
  assert.ok(codes.includes("unknown-key"));
});

test("rejects unknown nested keys at every depth", () => {
  const cases = [
    ["graphics", "evil"],
    ["performance", "turbo"],
    ["limitations", "extra"],
    ["limitations.removed", "memory999"],
    ["controllers", "rumbleHz"],
    ["controllers.bindings[0]", "macro"],
    ["patches", "extra"],
    ["patches.enabled", "notAPatch"],
  ];
  for (const [path, key] of cases) {
    const cfg = validConfig();
    const parts = path.split(".");
    let target = cfg;
    for (const part of parts) {
      if (/^[^[]+\[\d+\]$/.test(part)) {
        const [name, index] = part.split(/\[|\]/);
        target = target[name][Number(index)];
      } else {
        target = target[part];
      }
    }
    target[key] = true;
    const codes = codesFor(cfg);
    assert.ok(codes.includes("unknown-key"), `expected unknown-key at ${path}`);
  }
});

// ---- Wrong types ---------------------------------------------------------

test("rejects wrong types", () => {
  const cfg = validConfig({ profileName: 42 });
  assert.ok(codesFor(cfg).includes("invalid-type"));
  const g = validConfig();
  g.graphics.framePacing = "yes"; // truthy string, not a boolean
  assert.ok(codesFor(g).includes("invalid-boolean"));
  const p = validConfig();
  p.performance.frameSkip = true;
  assert.ok(codesFor(p).includes("invalid-enum"));
  const b = validConfig();
  b.controllers.bindings[0].mappedTo = 7;
  assert.ok(codesFor(b).includes("invalid-type"));
});

test("rejects string-boolean coercion targets for stored config", () => {
  const g = validConfig();
  g.graphics.vsync = "true"; // a stored row could contain this
  assert.equal(validateConfig(g).ok, false);
});

// ---- Non-finite / out-of-range numbers -----------------------------------

test("rejects NaN, Infinity and -Infinity in numeric fields", () => {
  for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
    for (const path of ["graphics.sharpness", "graphics.hudScale", "controllers.deadzone"]) {
      const cfg = validConfig();
      const parts = path.split(".");
      cfg[parts[0]][parts[1]] = bad;
      const codes = codesFor(cfg);
      assert.ok(codes.includes("invalid-number"), `${path} with ${bad}`);
    }
  }
});

test("rejects out-of-range numbers (negatives, absurd magnitudes, non-integers where required)", () => {
  const cases = [
    ["graphics.customWidth", -5],
    ["graphics.customHeight", 100000],
    ["graphics.sharpness", 101],
    ["graphics.sharpness", 12.5], // integer-required field
    ["controllers.deadzone", -1],
    ["controllers.triggerSensitivity", 1000],
  ];
  for (const [path, value] of cases) {
    const cfg = validConfig();
    const parts = path.split(".");
    cfg[parts[0]][parts[1]] = value;
    const codes = codesFor(cfg);
    assert.ok(codes.includes("invalid-number") || codes.includes("non-integer") || codes.includes("out-of-range"), `${path} = ${value}`);
  }
});

test("accepts fractional slider values that the UI legitimately produces", () => {
  const cfg = validConfig();
  cfg.graphics.hudScale = 0.75;
  cfg.graphics.renderToTextureScale = 0.5;
  assert.equal(validateConfig(cfg).ok, true);
});

// ---- Depth / size limits -------------------------------------------------

test("rejects excessive nesting depth", () => {
  const cfg = validConfig();
  let node = cfg;
  for (let i = 0; i < 15; i++) {
    node.extra = {};
    node = node.extra;
  }
  assert.ok(codesFor(cfg).includes("too-deep"));
});

test("rejects oversized serialized configs", () => {
  const cfg = validConfig();
  cfg.profileName = "x".repeat(200 * 1024);
  const codes = codesFor(cfg);
  assert.ok(codes.includes("config-too-large") || codes.includes("invalid-type"), JSON.stringify(codes));
});

// ---- Profile name --------------------------------------------------------

test("validates profile names: trim, non-empty, no control chars, byte limit", () => {
  assert.equal(validateProfileName("  Hello  ").value, "Hello");
  assert.equal(validateProfileName("").ok, false);
  assert.equal(validateProfileName("   ").ok, false);
  assert.equal(validateProfileName("a\u0001b").ok, false);
  assert.equal(validateProfileName(42).ok, false);
  assert.equal(validateProfileName("x".repeat(PROFILE_NAME_MAX_BYTES + 1)).ok, false);
  // Multi-byte characters count by bytes, not code points.
  assert.equal(validateProfileName("é".repeat(60)).ok, false); // 120 bytes
  assert.equal(validateProfileName("é".repeat(40)).ok, true); // 80 bytes
});

// ---- Stored-row classification -------------------------------------------

test("classifies stored rows: ok / corrupt / unsupported-version", () => {
  const good = validConfig();
  assert.equal(checkStoredConfig(good, CONFIG_SCHEMA_VERSION).status, "ok");
  assert.equal(checkStoredConfig(good, 2).status, "unsupported-version");
  assert.equal(checkStoredConfig(good, "1").status, "unsupported-version"); // version must be numeric
  assert.equal(checkStoredConfig(good, undefined).status, "unsupported-version");
  assert.equal(checkStoredConfig(null, CONFIG_SCHEMA_VERSION).status, "corrupt");
  assert.equal(checkStoredConfig({ profileName: "only-name" }, CONFIG_SCHEMA_VERSION).status, "corrupt");
});

test("parseStoredConfig never throws on malformed JSON", () => {
  assert.equal(parseStoredConfig("{not json", CONFIG_SCHEMA_VERSION).status, "corrupt");
  assert.equal(parseStoredConfig("[]", CONFIG_SCHEMA_VERSION).status, "corrupt");
  assert.equal(parseStoredConfig(JSON.stringify(validConfig()), CONFIG_SCHEMA_VERSION).status, "ok");
});

// ---- summarize never throws ----------------------------------------------

test("summarizeConfig degrades gracefully on garbage input", () => {
  assert.deepEqual(summarizeConfig(null), { resolution: "unknown", fps: "unknown", limitsRemoved: 0, patches: 0, strategy: "unknown" });
  assert.deepEqual(summarizeConfig("nope"), { resolution: "unknown", fps: "unknown", limitsRemoved: 0, patches: 0, strategy: "unknown" });
  assert.equal(summarizeConfig(validConfig()).resolution, "1080p");
  const custom = validConfig();
  custom.graphics.resolutionPreset = "custom";
  custom.graphics.customWidth = 800;
  custom.graphics.customHeight = 480;
  assert.equal(summarizeConfig(custom).resolution, "800x480");
  // Missing nested objects must not throw.
  const partial = { graphics: {}, limitations: {}, patches: {} };
  assert.deepEqual(summarizeConfig(partial), { resolution: "480x272", fps: "30", limitsRemoved: 0, patches: 0, strategy: "unknown" });
});

// ---- Drift lock: value sets must match types.ts unions -------------------

const typesSource = readFileSync(new URL("./types.ts", import.meta.url), "utf8");

function literalsFrom(unionText) {
  const literals = unionText.match(/"([^"]+)"/g);
  return literals ? literals.map((l) => l.slice(1, -1)) : [];
}

function unionFrom(source, name) {
  const re = new RegExp(`export type ${name}\\s*=\\s*([^;]+);`);
  const match = source.match(re);
  assert.ok(match, `union ${name} not found in types.ts`);
  return literalsFrom(match[1]);
}

// Unions declared inline inside an interface (e.g. `frameSkip: "off" | ...;`).
function inlineUnionFrom(source, interfaceName, fieldName) {
  const re = new RegExp(`export interface ${interfaceName} \\{([^}]*)\\}`);
  const match = source.match(re);
  assert.ok(match, `interface ${interfaceName} not found in types.ts`);
  const field = match[1].match(new RegExp(`\\b${fieldName}\\s*:\\s*([^;]+);`));
  assert.ok(field, `${interfaceName}.${fieldName} not found in types.ts`);
  return literalsFrom(field[1]);
}

const MODULE_ARRAYS = {
  ResolutionPreset: RESOLUTION_PRESETS,
  FrameRateCap: FRAME_RATE_CAPS,
  AspectRatio: ASPECT_RATIOS,
  TextureFilter: TEXTURE_FILTERS,
  MsaaLevel: MSAA_LEVELS,
  AnisotropyLevel: ANISOTROPY_LEVELS,
  TextureUpscale: TEXTURE_UPSCALES,
  MotionBlur: MOTION_BLUR_MODES,
  DepthOfField: DEPTH_OF_FIELD_MODES,
  CpuClockMode: CPU_CLOCK_MODES,
  ThreadAffinity: THREAD_AFFINITIES,
  FrameSkip: FRAME_SKIPS,
  MinimizeStrategy: MINIMIZE_STRATEGIES,
  ControllerDevice: CONTROLLER_DEVICES,
  PspLimit: PSP_LIMITS,
  PatchId: PATCH_IDS,
};

// Unions declared inline inside an interface in types.ts.
const INLINE_UNIONS = {
  TextureUpscale: { interface: "GraphicsConfig", field: "textureUpscale" },
  MotionBlur: { interface: "GraphicsConfig", field: "motionBlur" },
  DepthOfField: { interface: "GraphicsConfig", field: "depthOfField" },
  ThreadAffinity: { interface: "PerformanceConfig", field: "threadAffinity" },
  FrameSkip: { interface: "PerformanceConfig", field: "frameSkip" },
};

test("value sets in config-schema.mjs match the unions in types.ts", () => {
  for (const [typeName, moduleArray] of Object.entries(MODULE_ARRAYS)) {
    const literals = INLINE_UNIONS[typeName]
      ? inlineUnionFrom(typesSource, INLINE_UNIONS[typeName].interface, INLINE_UNIONS[typeName].field)
      : unionFrom(typesSource, typeName);
    assert.deepEqual(
      [...new Set(moduleArray)].sort(),
      [...new Set(literals)].sort(),
      `drift between config-schema.mjs and types.ts ${typeName}`,
    );
  }
});

test("binding key set matches the ControllerBinding interface fields", () => {
  const m = typesSource.match(/export interface ControllerBinding \{([^}]+)\}/);
  assert.ok(m);
  const fields = [...m[1].matchAll(/^\s*([a-zA-Z]+)\s*:/gm)].map((x) => x[1]).sort();
  assert.deepEqual([...CONTROLLER_BINDING_KEYS].sort(), fields);
});
