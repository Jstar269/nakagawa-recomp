import type { ControllerBinding, IsoMeta, PatchId, PspLimit } from "./types";

// Hot Shots Tennis: Get a Grip (Everybody's Tennis / Minna no Tennis)
// PSP, Clap Hanz / Sony Computer Entertainment, 2010 (JP), 2011 (EU/US).
export const GAME_PROFILE = {
  title: "Hot Shots Tennis: Get a Grip",
  altTitles: ["Everybody's Tennis", "Minna no Tennis Portable"],
  developer: "Clap Hanz",
  publisher: "Sony Computer Entertainment",
  platform: "PlayStation Portable (PSP-1000/2000/3000)",
  release: "2010-02-25 (JP) / 2011 (EU/US)",
  genre: "Sports / Tennis",
  cpu: "MIPS R4000 Allegrex @ 333 MHz (capped at 222 MHz by Sony SDK)",
  gpu: "PSP custom GPU, 2 MB VRAM, 1664x1664 tex",
  memory: "32 MB main RAM (+ 4 MB embedded)",
  nativeResolution: "480 x 272",
  nativeFrameRate: "30 fps (logic-locked)",
  disc: "UMD 1.8 GB (ISO9660 + PSP_EXTENSIONS)",
  gameCodes: ["ULJS-00338", "UCES-01420", "UCUS-98767"],
  regionCodes: {
    ULJS: "Japan",
    UCES: "Europe",
    UCUS: "North America",
  },
} as const;

export const LIMIT_INFO: Record<
  PspLimit,
  { label: string; native: string; removed: string; risk: "safe" | "moderate" | "advanced" }
> = {
  memory32: {
    label: "32 MB Main RAM Cap",
    native: "32 MB user RAM",
    removed: "Up to 96 MB available",
    risk: "moderate",
  },
  vram2mb: {
    label: "2 MB VRAM Budget",
    native: "2 MB VRAM / 512 KB framebuffer",
    removed: "Host GPU VRAM (unlimited)",
    risk: "safe",
  },
  drawCalls: {
    label: "Per-Frame Draw Call Quota",
    native: "~6,000 DC/frame hard cap",
    removed: "Unlimited (host GPU)",
    risk: "safe",
  },
  saveSize1mb: {
    label: "1 MB Save Data Limit",
    native: "1 MB save partition",
    removed: "Unbounded host save",
    risk: "safe",
  },
  adHocLimit: {
    label: "Ad-Hoc Multiplayer Lockout",
    native: "Ad-hoc only (no online)",
    removed: "Netcode stub (offline build)",
    risk: "advanced",
  },
  cpu222cap: {
    label: "222 MHz CPU Clock Cap",
    native: "222 MHz enforced by SDK",
    removed: "Full 333 MHz / OC 444 MHz",
    risk: "moderate",
  },
  meCoreLock: {
    label: "Media Engine Core Lock",
    native: "ME core reserved for codecs",
    removed: "ME core available for game logic",
    risk: "advanced",
  },
  textureSwizzle: {
    label: "Texture Swizzle / 512 Edge",
    native: "Swizzled, max 512x512",
    removed: "Linear + up to 8192x8192",
    risk: "safe",
  },
  depthBuffer16: {
    label: "16-bit Depth Buffer",
    native: "16-bit Z (z-fighting on hills)",
    removed: "24/32-bit float Z",
    risk: "safe",
  },
  audio44khz: {
    label: "44.1 kHz Audio Ceiling",
    native: "44.1 kHz SAC cap",
    removed: "48 / 96 kHz host mix",
    risk: "safe",
  },
};

export const PATCH_INFO: Record<
  PatchId,
  { label: string; description: string; risk: "safe" | "moderate" | "advanced"; stage: string }
> = {
  courtHD: {
    label: "HD Court Textures",
    description: "Design target: re-encode court surfaces for higher-resolution host sampling; not implemented.",
    risk: "safe",
    stage: "assets",
  },
  ballTrail: {
    label: "Ball Trail Physics",
    description: "Design target: add a sub-frame trail buffer for high-frame-rate rendering; not implemented.",
    risk: "moderate",
    stage: "code",
  },
  hudReposition: {
    label: "HUD Repositioning",
    description: "Design target: re-anchor score/UI elements for widescreen output; not implemented.",
    risk: "safe",
    stage: "code",
  },
  cameraFree: {
    label: "Free Camera",
    description: "Design target: expose the broadcast camera matrix to host controls; not implemented.",
    risk: "advanced",
    stage: "code",
  },
  skipIntroLogos: {
    label: "Skip Intro Logos",
    description: "Design target: identify and bypass publisher-logo sequences; not implemented.",
    risk: "safe",
    stage: "code",
  },
  unlockAllCourts: {
    label: "Unlock All Courts",
    description: "Design target: alter court-unlock state in a user-controlled save patch; not implemented.",
    risk: "moderate",
    stage: "save",
  },
  proAiRebalance: {
    label: "Pro AI Rebalance",
    description: "Design target: expose AI difficulty constants for optional tuning; not implemented.",
    risk: "moderate",
    stage: "code",
  },
  widescreenFix: {
    label: "Widescreen Projection Fix",
    description: "Design target: patch the projection matrix for widescreen output; not implemented.",
    risk: "moderate",
    stage: "code",
  },
  audioRemaster: {
    label: "Audio Remaster",
    description: "Design target: experiment with replacement audio assets and stream metadata; not implemented.",
    risk: "safe",
    stage: "assets",
  },
  shadowSoftening: {
    label: "Shadow Softening",
    description: "Design target: investigate filtered host-side shadows; not implemented.",
    risk: "safe",
    stage: "code",
  },
};

// PSP face buttons -> recompiled virtual actions.
export const PSP_ACTIONS = [
  { psp: "CIRCLE", label: "Top Spin / Confirm", defaultPsp: "CIRCLE" },
  { psp: "CROSS", label: "Slice / Cancel", defaultPsp: "CROSS" },
  { psp: "SQUARE", label: "Lob", defaultPsp: "SQUARE" },
  { psp: "TRIANGLE", label: "Smash / Menu", defaultPsp: "TRIANGLE" },
  { psp: "L", label: "Left Trigger / Charge", defaultPsp: "L" },
  { psp: "R", label: "Right Trigger / Charge", defaultPsp: "R" },
  { psp: "DPAD_UP", label: "Move / Aim Up", defaultPsp: "DPAD_UP" },
  { psp: "DPAD_DOWN", label: "Move / Aim Down", defaultPsp: "DPAD_DOWN" },
  { psp: "DPAD_LEFT", label: "Move / Aim Left", defaultPsp: "DPAD_LEFT" },
  { psp: "DPAD_RIGHT", label: "Move / Aim Right", defaultPsp: "DPAD_RIGHT" },
  { psp: "ANALOG", label: "Player Movement", defaultPsp: "ANALOG" },
  { psp: "START", label: "Pause", defaultPsp: "START" },
  { psp: "SELECT", label: "Scorecard", defaultPsp: "SELECT" },
] as const;

export const CONTROLLER_SCHEMES: Record<
  string,
  { label: string; glyph: string; buttons: string[]; supportsAdaptive: boolean; supportsGyro: boolean }
> = {
  dualsense: {
    label: "DualSense (PS5)",
    glyph: "PS",
    buttons: [
      "CIRCLE", "CROSS", "SQUARE", "TRIANGLE",
      "L1", "L2", "R1", "R2",
      "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
      "LEFT_STICK", "RIGHT_STICK", "L3", "R3",
      "TOUCHPAD", "CREATE", "OPTIONS", "MUTE",
    ],
    supportsAdaptive: true,
    supportsGyro: true,
  },
  "xbox-series": {
    label: "Xbox Series Controller",
    glyph: "XB",
    buttons: [
      "B", "A", "X", "Y",
      "LB", "LT", "RB", "RT",
      "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
      "LEFT_STICK", "RIGHT_STICK", "LSB", "RSB",
      "VIEW", "MENU", "SHARE",
    ],
    supportsAdaptive: false,
    supportsGyro: false,
  },
  "switch-pro": {
    label: "Switch Pro Controller",
    glyph: "SW",
    buttons: [
      "A", "B", "X", "Y",
      "L", "ZL", "R", "ZR",
      "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
      "LEFT_STICK", "RIGHT_STICK", "LSB", "RSB",
      "MINUS", "PLUS", "HOME", "CAPTURE",
    ],
    supportsAdaptive: false,
    supportsGyro: true,
  },
  "xbox-elite": {
    label: "Xbox Elite Series 2",
    glyph: "EL",
    buttons: [
      "B", "A", "X", "Y",
      "LB", "LT", "RB", "RT",
      "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
      "LEFT_STICK", "RIGHT_STICK", "LSB", "RSB",
      "VIEW", "MENU", "P1", "P2", "P3", "P4",
    ],
    supportsAdaptive: false,
    supportsGyro: false,
  },
  "generic-xinput": {
    label: "Generic XInput Pad",
    glyph: "XI",
    buttons: [
      "B", "A", "X", "Y",
      "LB", "LT", "RB", "RT",
      "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
      "LEFT_STICK", "RIGHT_STICK", "LSB", "RSB",
      "BACK", "START",
    ],
    supportsAdaptive: false,
    supportsGyro: false,
  },
};

export function defaultBindings(): ControllerBinding[] {
  const map: Record<string, string> = {
    CIRCLE: "CIRCLE",
    CROSS: "CROSS",
    SQUARE: "SQUARE",
    TRIANGLE: "TRIANGLE",
    L: "L2",
    R: "R2",
    DPAD_UP: "DPAD_UP",
    DPAD_DOWN: "DPAD_DOWN",
    DPAD_LEFT: "DPAD_LEFT",
    DPAD_RIGHT: "DPAD_RIGHT",
    ANALOG: "LEFT_STICK",
    START: "OPTIONS",
    SELECT: "CREATE",
  };
  return PSP_ACTIONS.map((a) => ({
    pspAction: a.psp,
    label: a.label,
    defaultPsp: a.defaultPsp,
    mappedTo: map[a.psp] ?? a.psp,
  }));
}

// Given an ISO volume id + parsed SFO-ish code, try to recognize the game.
export function matchGame(gameCode: string | null): {
  matchedTitle: string | null;
  region: string | null;
} {
  if (!gameCode) return { matchedTitle: null, region: null };
  const prefix = gameCode.slice(0, 4).toUpperCase();
  const region = GAME_PROFILE.regionCodes[prefix as keyof typeof GAME_PROFILE.regionCodes] ?? null;
  const matched = GAME_PROFILE.gameCodes.includes(gameCode.toUpperCase() as never)
    ? GAME_PROFILE.title
    : null;
  return { matchedTitle: matched, region };
}

export function emptyIsoMeta(): IsoMeta {
  return {
    fileName: "",
    sizeBytes: 0,
    volumeId: "",
    systemId: "",
    application: "",
    publisher: "",
    creationDate: "",
    fileCount: 0,
    gameCode: null,
    region: null,
    matchedTitle: null,
  };
}
