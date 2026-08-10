// Core configuration types for the Hot Shots Tennis: Get a Grip recompiler.

export type ResolutionPreset =
  | "native" // 480x272 (PSP native)
  | "x2" // 960x544
  | "x3" // 1440x816
  | "x4" // 1920x1088 (~1080p)
  | "x6" // 2880x1632 (~1440p)
  | "x8" // 3840x2176 (~4K)
  | "custom";

export type FrameRateCap = "native" | "60" | "120" | "unlocked";

export type AspectRatio = "native" | "16:9" | "16:10" | "21:9" | "4:3" | "stretch";

export type TextureFilter = "native" | "bilinear" | "bicubic" | "xbrz" | "hybrid";

export type MsaaLevel = "off" | "2x" | "4x" | "8x";

export type AnisotropyLevel = "off" | "2x" | "4x" | "8x" | "16x";

export type CpuClockMode = "native" | "max333" | "333 unlocked" | "oc444";

export type MinimizeStrategy = "portable" | "minimal" | "ultra";

export type ControllerDevice =
  | "dualsense"
  | "xbox-series"
  | "switch-pro"
  | "xbox-elite"
  | "generic-xinput";

export type PspLimit =
  | "memory32"
  | "vram2mb"
  | "drawCalls"
  | "saveSize1mb"
  | "adHocLimit"
  | "cpu222cap"
  | "meCoreLock"
  | "textureSwizzle"
  | "depthBuffer16"
  | "audio44khz";

export interface GraphicsConfig {
  resolutionPreset: ResolutionPreset;
  customWidth: number;
  customHeight: number;
  renderToTextureScale: number;
  frameRateCap: FrameRateCap;
  framePacing: boolean;
  vsync: boolean;
  tripleBuffering: boolean;
  aspectRatio: AspectRatio;
  widescreenHack: boolean;
  textureFilter: TextureFilter;
  textureUpscale: "off" | "x2" | "x4" | "x6";
  msaa: MsaaLevel;
  anisotropy: AnisotropyLevel;
  motionBlur: "native" | "off" | "enhanced";
  depthOfField: "native" | "off" | "enhanced";
  hudScale: number;
  sharpness: number;
}

export interface PerformanceConfig {
  cpuClockMode: CpuClockMode;
  threadAffinity: "single" | "dual" | "all";
  fastMemory: boolean;
  blockLinking: boolean;
  jitCache: boolean;
  vertexCache: boolean;
  shaderCache: boolean;
  frameSkip: "off" | "auto" | "1" | "2" | "3";
  ioThreading: boolean;
  fastForwardUnthrottled: boolean;
}

export interface LimitationsConfig {
  removed: Record<PspLimit, boolean>;
}

export interface ControllerBinding {
  pspAction: string;
  label: string;
  defaultPsp: string;
  mappedTo: string;
}

export interface ControllersConfig {
  device: ControllerDevice;
  deadzone: number;
  triggerSensitivity: number;
  rumble: boolean;
  adaptiveTriggers: boolean;
  gyroAim: boolean;
  bindings: ControllerBinding[];
}

export type PatchId =
  | "courtHD"
  | "ballTrail"
  | "hudReposition"
  | "cameraFree"
  | "skipIntroLogos"
  | "unlockAllCourts"
  | "proAiRebalance"
  | "widescreenFix"
  | "audioRemaster"
  | "shadowSoftening";

export interface PatchesConfig {
  enabled: Record<PatchId, boolean>;
}

export interface IsoTreeNode {
  name: string;
  isDir: boolean;
  size: number;
  lba: number;
  children?: IsoTreeNode[];
}

export interface IsoMeta {
  fileName: string;
  sizeBytes: number;
  volumeId: string;
  systemId: string;
  application: string;
  publisher: string;
  creationDate: string;
  fileCount: number;
  gameCode: string | null;
  region: string | null;
  matchedTitle: string | null;
  tree?: IsoTreeNode[];
}

export interface RecompilerConfig {
  profileName: string;
  graphics: GraphicsConfig;
  performance: PerformanceConfig;
  limitations: LimitationsConfig;
  controllers: ControllersConfig;
  patches: PatchesConfig;
  minimizeStrategy: MinimizeStrategy;
  updatedAt: string;
}
