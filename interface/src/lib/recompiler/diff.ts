import type { RecompilerConfig } from "./types";

export interface DiffEntry {
  path: string;
  label: string;
  from: string;
  to: string;
  changed: boolean;
  category: "graphics" | "performance" | "limitations" | "controllers" | "patches" | "bundle";
}

const RES_LABEL: Record<string, string> = {
  native: "480×272",
  x2: "960×544",
  x3: "1440×816",
  x4: "1080p (1920×1088)",
  x6: "1440p (2880×1632)",
  x8: "4K (3840×2176)",
  custom: "custom",
};

function resLabel(c: RecompilerConfig): string {
  const g = c.graphics;
  if (g.resolutionPreset === "custom") return `${g.customWidth}×${g.customHeight}`;
  return RES_LABEL[g.resolutionPreset] ?? g.resolutionPreset;
}

// Compare two configs and return a list of diff entries.
export function diffConfigs(
  current: RecompilerConfig,
  baseline: RecompilerConfig,
): DiffEntry[] {
  const entries: DiffEntry[] = [];

  // Graphics
  const cg = current.graphics;
  const bg = baseline.graphics;
  entries.push({
    path: "graphics.resolution",
    label: "Resolution",
    from: resLabel(baseline),
    to: resLabel(current),
    changed: cg.resolutionPreset !== bg.resolutionPreset || cg.customWidth !== bg.customWidth || cg.customHeight !== bg.customHeight,
    category: "graphics",
  });
  entries.push({
    path: "graphics.fps",
    label: "Frame rate cap",
    from: bg.frameRateCap === "native" ? "30 fps" : `${bg.frameRateCap} fps`,
    to: cg.frameRateCap === "native" ? "30 fps" : `${cg.frameRateCap} fps`,
    changed: cg.frameRateCap !== bg.frameRateCap,
    category: "graphics",
  });
  entries.push({
    path: "graphics.aspect",
    label: "Aspect ratio",
    from: bg.aspectRatio,
    to: cg.aspectRatio,
    changed: cg.aspectRatio !== bg.aspectRatio,
    category: "graphics",
  });
  entries.push({
    path: "graphics.msaa",
    label: "MSAA",
    from: bg.msaa,
    to: cg.msaa,
    changed: cg.msaa !== bg.msaa,
    category: "graphics",
  });
  entries.push({
    path: "graphics.anisotropy",
    label: "Anisotropic",
    from: bg.anisotropy,
    to: cg.anisotropy,
    changed: cg.anisotropy !== bg.anisotropy,
    category: "graphics",
  });
  entries.push({
    path: "graphics.textureFilter",
    label: "Texture filter",
    from: bg.textureFilter,
    to: cg.textureFilter,
    changed: cg.textureFilter !== bg.textureFilter,
    category: "graphics",
  });
  entries.push({
    path: "graphics.textureUpscale",
    label: "Texture upscale",
    from: bg.textureUpscale,
    to: cg.textureUpscale,
    changed: cg.textureUpscale !== bg.textureUpscale,
    category: "graphics",
  });
  entries.push({
    path: "graphics.widescreenHack",
    label: "Widescreen hack",
    from: bg.widescreenHack ? "on" : "off",
    to: cg.widescreenHack ? "on" : "off",
    changed: cg.widescreenHack !== bg.widescreenHack,
    category: "graphics",
  });

  // Performance
  const cp = current.performance;
  const bp = baseline.performance;
  entries.push({
    path: "performance.cpuClock",
    label: "CPU clock",
    from: bp.cpuClockMode,
    to: cp.cpuClockMode,
    changed: cp.cpuClockMode !== bp.cpuClockMode,
    category: "performance",
  });
  entries.push({
    path: "performance.threadAffinity",
    label: "Thread affinity",
    from: bp.threadAffinity,
    to: cp.threadAffinity,
    changed: cp.threadAffinity !== bp.threadAffinity,
    category: "performance",
  });
  entries.push({
    path: "performance.jitCache",
    label: "JIT cache",
    from: bp.jitCache ? "on" : "off",
    to: cp.jitCache ? "on" : "off",
    changed: cp.jitCache !== bp.jitCache,
    category: "performance",
  });
  entries.push({
    path: "performance.fastMemory",
    label: "Fast memory",
    from: bp.fastMemory ? "on" : "off",
    to: cp.fastMemory ? "on" : "off",
    changed: cp.fastMemory !== bp.fastMemory,
    category: "performance",
  });

  // Limitations (count only)
  const curRemoved = Object.values(current.limitations.removed).filter(Boolean).length;
  const baseRemoved = Object.values(baseline.limitations.removed).filter(Boolean).length;
  entries.push({
    path: "limitations.removed",
    label: "PSP limits removed",
    from: `${baseRemoved} / 10`,
    to: `${curRemoved} / 10`,
    changed: curRemoved !== baseRemoved,
    category: "limitations",
  });

  // Patches (count only)
  const curPatches = Object.values(current.patches.enabled).filter(Boolean).length;
  const basePatches = Object.values(baseline.patches.enabled).filter(Boolean).length;
  entries.push({
    path: "patches.enabled",
    label: "Game patches enabled",
    from: `${basePatches} / 10`,
    to: `${curPatches} / 10`,
    changed: curPatches !== basePatches,
    category: "patches",
  });

  // Controllers
  entries.push({
    path: "controllers.device",
    label: "Controller device",
    from: baseline.controllers.device,
    to: current.controllers.device,
    changed: current.controllers.device !== baseline.controllers.device,
    category: "controllers",
  });

  // Bundle
  entries.push({
    path: "bundle.strategy",
    label: "Bundle strategy",
    from: baseline.minimizeStrategy,
    to: current.minimizeStrategy,
    changed: current.minimizeStrategy !== baseline.minimizeStrategy,
    category: "bundle",
  });

  return entries;
}

export const CATEGORY_LABELS: Record<DiffEntry["category"], string> = {
  graphics: "Graphics",
  performance: "Performance",
  limitations: "PSP Limits",
  controllers: "Controllers",
  patches: "Patches",
  bundle: "Bundle",
};
