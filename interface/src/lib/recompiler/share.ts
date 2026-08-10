import type { RecompilerConfig } from "./types";
import { defaultConfig } from "./defaults";

// Encode a config into a compact URL-safe base64 string for sharing via URL hash.
// We strip the profileName + updatedAt (not meaningful for a shared setup) and
// use a minimal JSON encoding before base64url.

export function encodeConfigToHash(config: RecompilerConfig): string {
  // Minimal payload: drop transient fields.
  const payload = {
    g: config.graphics,
    p: config.performance,
    l: config.limitations.removed,
    c: {
      d: config.controllers.device,
      dz: config.controllers.deadzone,
      ts: config.controllers.triggerSensitivity,
      r: config.controllers.rumble,
      at: config.controllers.adaptiveTriggers,
      gy: config.controllers.gyroAim,
      b: config.controllers.bindings.map((b) => [b.pspAction, b.mappedTo]),
    },
    pa: config.patches.enabled,
    ms: config.minimizeStrategy,
  };
  const json = JSON.stringify(payload);
  // base64url encode
  const b64 = typeof window !== "undefined"
    ? btoa(unescape(encodeURIComponent(json)))
    : Buffer.from(json, "utf-8").toString("base64");
  return b64.replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function decodeConfigFromHash(hash: string): RecompilerConfig | null {
  try {
    // base64url decode
    let b64 = hash.replace(/-/g, "+").replace(/_/g, "/");
    while (b64.length % 4) b64 += "=";
    const json = typeof window !== "undefined"
      ? decodeURIComponent(escape(atob(b64)))
      : Buffer.from(b64, "base64").toString("utf-8");
    const p = JSON.parse(json);
    if (!p || !p.g || !p.p) return null;
    // Reconstruct a full config.
    const base = defaultConfig(p.ms ?? "minimal");
    base.graphics = { ...base.graphics, ...p.g };
    base.performance = { ...base.performance, ...p.p };
    if (p.l) base.limitations = { removed: { ...base.limitations.removed, ...p.l } };
    if (p.c) {
      base.controllers = {
        ...base.controllers,
        device: p.c.d ?? base.controllers.device,
        deadzone: p.c.dz ?? base.controllers.deadzone,
        triggerSensitivity: p.c.ts ?? base.controllers.triggerSensitivity,
        rumble: p.c.r ?? base.controllers.rumble,
        adaptiveTriggers: p.c.at ?? base.controllers.adaptiveTriggers,
        gyroAim: p.c.gy ?? base.controllers.gyroAim,
        bindings: Array.isArray(p.c.b)
          ? p.c.b.map((pair: [string, string], i: number) => ({
              pspAction: pair[0],
              label: base.controllers.bindings[i]?.label ?? pair[0],
              defaultPsp: base.controllers.bindings[i]?.defaultPsp ?? pair[0],
              mappedTo: pair[1],
            }))
          : base.controllers.bindings,
      };
    }
    if (p.pa) base.patches = { enabled: { ...base.patches.enabled, ...p.pa } };
    base.minimizeStrategy = p.ms ?? base.minimizeStrategy;
    base.profileName = "Shared config";
    base.updatedAt = new Date().toISOString();
    return base;
  } catch {
    return null;
  }
}

export function buildShareUrl(config: RecompilerConfig): string {
  const encoded = encodeConfigToHash(config);
  if (typeof window === "undefined") return `#cfg=${encoded}`;
  return `${window.location.origin}${window.location.pathname}#cfg=${encoded}`;
}
