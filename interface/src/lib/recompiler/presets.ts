import type { RecompilerConfig } from "./types";
import {
  defaultConfig,
  defaultGraphics,
  defaultPerformance,
  defaultLimitations,
  defaultControllers,
  defaultPatches,
} from "./defaults";

export interface Preset {
  id: string;
  name: string;
  tagline: string;
  description: string;
  accent: "ball" | "amber" | "violet" | "sky";
  apply: () => RecompilerConfig;
}

export const PRESETS: Preset[] = [
  {
    id: "balanced",
    name: "Balanced",
    tagline: "Concept · 60 fps · 1080p",
    accent: "ball",
    description:
      "Unimplemented preview target for a conservative 1080p/60 configuration. Applying it only changes dashboard design-state; it does not alter the native build.",
    apply: () => {
      const c = defaultConfig("minimal");
      c.profileName = "Balanced";
      c.graphics = {
        ...defaultGraphics(),
        resolutionPreset: "x4",
        customWidth: 1920,
        customHeight: 1088,
        frameRateCap: "60",
        msaa: "4x",
        anisotropy: "16x",
        textureUpscale: "x4",
      };
      c.performance = { ...defaultPerformance(), cpuClockMode: "333 unlocked" };
      c.limitations = defaultLimitations();
      c.patches = {
        ...defaultPatches(),
        enabled: {
          ...defaultPatches().enabled,
          widescreenFix: true,
          skipIntroLogos: true,
          hudReposition: true,
          shadowSoftening: true,
        },
      };
      c.minimizeStrategy = "minimal";
      c.updatedAt = new Date().toISOString();
      return c;
    },
  },
  {
    id: "max-quality",
    name: "Max Quality",
    tagline: "Concept · 120 fps · 4K",
    accent: "violet",
    description:
      "Unimplemented preview target combining proposed 4K, 120-fps, limit-removal, asset, and visual-patch work. It does not alter the native build.",
    apply: () => {
      const c = defaultConfig("minimal");
      c.profileName = "Max Quality";
      c.graphics = {
        ...defaultGraphics(),
        resolutionPreset: "x8",
        customWidth: 3840,
        customHeight: 2176,
        renderToTextureScale: 1.5,
        frameRateCap: "120",
        msaa: "8x",
        anisotropy: "16x",
        textureUpscale: "x6",
        textureFilter: "hybrid",
        motionBlur: "enhanced",
        depthOfField: "enhanced",
        sharpness: 45,
        hudScale: 1.2,
      };
      c.performance = {
        ...defaultPerformance(),
        cpuClockMode: "oc444",
        threadAffinity: "all",
        fastForwardUnthrottled: true,
      };
      const removed = { ...defaultLimitations().removed };
      (Object.keys(removed) as Array<keyof typeof removed>).forEach((k) => (removed[k] = true));
      c.limitations = { removed };
      c.patches = {
        ...defaultPatches(),
        enabled: {
          ...defaultPatches().enabled,
          courtHD: true,
          audioRemaster: true,
          ballTrail: true,
          widescreenFix: true,
          skipIntroLogos: true,
          hudReposition: true,
          shadowSoftening: true,
          proAiRebalance: true,
          cameraFree: true,
          unlockAllCourts: true,
        },
      };
      c.minimizeStrategy = "minimal";
      c.updatedAt = new Date().toISOString();
      return c;
    },
  },
  {
    id: "speedrun",
    name: "Speedrun",
    tagline: "Concept · unlocked · 720p",
    accent: "amber",
    description:
      "Unimplemented low-latency preview target. Its frame-rate, patch, and adaptive-trigger values are dashboard design-state only.",
    apply: () => {
      const c = defaultConfig("minimal");
      c.profileName = "Speedrun";
      c.graphics = {
        ...defaultGraphics(),
        resolutionPreset: "x2",
        customWidth: 960,
        customHeight: 544,
        frameRateCap: "unlocked",
        framePacing: false,
        vsync: false,
        tripleBuffering: false,
        msaa: "off",
        anisotropy: "off",
        textureUpscale: "off",
        motionBlur: "off",
        depthOfField: "off",
        sharpness: 0,
        hudScale: 1,
      };
      c.performance = {
        ...defaultPerformance(),
        cpuClockMode: "oc444",
        threadAffinity: "all",
        fastForwardUnthrottled: true,
      };
      c.patches = {
        ...defaultPatches(),
        enabled: {
          ...defaultPatches().enabled,
          skipIntroLogos: true,
          widescreenFix: true,
          hudReposition: true,
          unlockAllCourts: true,
        },
      };
      c.controllers = {
        ...defaultControllers(),
        adaptiveTriggers: true,
        deadzone: 8,
        triggerSensitivity: 80,
      };
      c.minimizeStrategy = "ultra";
      c.updatedAt = new Date().toISOString();
      return c;
    },
  },
  {
    id: "purist",
    name: "Purist",
    tagline: "Concept · native PSP profile",
    accent: "sky",
    description:
      "Preview target that documents a native-style configuration. Applying it does not change the renderer or runtime behavior.",
    apply: () => {
      const c = defaultConfig("portable");
      c.profileName = "Purist (native PSP)";
      c.graphics = {
        ...defaultGraphics(),
        resolutionPreset: "native",
        customWidth: 480,
        customHeight: 272,
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
      };
      c.performance = {
        ...defaultPerformance(),
        cpuClockMode: "native",
        threadAffinity: "single",
        fastMemory: false,
        blockLinking: false,
        jitCache: false,
        vertexCache: false,
        shaderCache: false,
        ioThreading: false,
      };
      const removed = { ...defaultLimitations().removed };
      (Object.keys(removed) as Array<keyof typeof removed>).forEach((k) => (removed[k] = false));
      c.limitations = { removed };
      const enabled = { ...defaultPatches().enabled };
      (Object.keys(enabled) as Array<keyof typeof enabled>).forEach((k) => (enabled[k] = false));
      c.patches = { enabled };
      c.minimizeStrategy = "portable";
      c.updatedAt = new Date().toISOString();
      return c;
    },
  },
];

export function getPreset(id: string): Preset | undefined {
  return PRESETS.find((p) => p.id === id);
}
