import type {
  GraphicsConfig,
  LimitationsConfig,
  MinimizeStrategy,
  PatchId,
  PspLimit,
  RecompilerConfig,
  ControllersConfig,
  PerformanceConfig,
  PatchesConfig,
} from "./types";
import { defaultBindings, LIMIT_INFO, PATCH_INFO } from "./profiles";

export const RESOLUTION_PRESETS: Record<
  string,
  { w: number; h: number; label: string; note: string }
> = {
  native: { w: 480, h: 272, label: "Native PSP", note: "480 x 272 (CRT look)" },
  x2: { w: 960, h: 544, label: "2x / Vita", note: "960 x 544" },
  x3: { w: 1440, h: 816, label: "3x", note: "1440 x 816" },
  x4: { w: 1920, h: 1088, label: "4x / 1080p", note: "1920 x 1088" },
  x6: { w: 2880, h: 1632, label: "6x / 1440p", note: "2880 x 1632" },
  x8: { w: 3840, h: 2176, label: "8x / 4K", note: "3840 x 2176" },
};

export const allLimits: PspLimit[] = Object.keys(LIMIT_INFO) as PspLimit[];
export const allPatches: PatchId[] = Object.keys(PATCH_INFO) as PatchId[];

export function defaultGraphics(): GraphicsConfig {
  return {
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
  };
}

export function defaultPerformance(): PerformanceConfig {
  return {
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
  };
}

export function defaultLimitations(): LimitationsConfig {
  const removed = {} as Record<PspLimit, boolean>;
  for (const k of allLimits) removed[k] = true;
  // Ad-hoc stays native by default (advanced risk).
  removed.adHocLimit = false;
  removed.meCoreLock = false;
  return { removed };
}

export function defaultControllers(): ControllersConfig {
  return {
    device: "dualsense",
    deadzone: 12,
    triggerSensitivity: 60,
    rumble: true,
    adaptiveTriggers: true,
    gyroAim: false,
    bindings: defaultBindings(),
  };
}

export function defaultPatches(): PatchesConfig {
  const enabled = {} as Record<PatchId, boolean>;
  for (const k of allPatches) enabled[k] = false;
  enabled.widescreenFix = true;
  enabled.skipIntroLogos = true;
  enabled.hudReposition = true;
  return { enabled };
}

export function defaultConfig(
  strategy: MinimizeStrategy = "minimal",
): RecompilerConfig {
  return {
    profileName: "Hot Shots Tennis — Get a Grip (Default)",
    graphics: defaultGraphics(),
    performance: defaultPerformance(),
    limitations: defaultLimitations(),
    controllers: defaultControllers(),
    patches: defaultPatches(),
    minimizeStrategy: strategy,
    updatedAt: new Date().toISOString(),
  };
}

// A "pure native" config used to compute the diff shown in the UI.
export function nativeConfig(): RecompilerConfig {
  const removed = {} as Record<PspLimit, boolean>;
  for (const k of allLimits) removed[k] = false;
  const enabled = {} as Record<PatchId, boolean>;
  for (const k of allPatches) enabled[k] = false;
  return {
    profileName: "PSP Native (reference)",
    graphics: {
      resolutionPreset: "native",
      customWidth: 480,
      customHeight: 272,
      renderToTextureScale: 1,
      frameRateCap: "native",
      framePacing: false,
      vsync: false,
      tripleBuffering: false,
      aspectRatio: "native",
      widescreenHack: false,
      textureFilter: "native",
      textureUpscale: "off",
      msaa: "off",
      anisotropy: "off",
      motionBlur: "native",
      depthOfField: "native",
      hudScale: 1,
      sharpness: 0,
    },
    performance: {
      cpuClockMode: "native",
      threadAffinity: "single",
      fastMemory: false,
      blockLinking: false,
      jitCache: false,
      vertexCache: false,
      shaderCache: false,
      frameSkip: "off",
      ioThreading: false,
      fastForwardUnthrottled: false,
    },
    limitations: { removed },
    controllers: defaultControllers(),
    patches: { enabled },
    minimizeStrategy: "portable",
    updatedAt: new Date().toISOString(),
  };
}
