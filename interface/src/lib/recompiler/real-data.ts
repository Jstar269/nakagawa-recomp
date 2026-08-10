// Maintained architecture data for dashboard panels. Source and ISSUES.md are authoritative.

export interface PipelineStage { id: number; script: string; input: string; output: string; purpose: string; }
export const PIPELINE_STAGES: PipelineStage[] = [
  { id: 1, script: "prxload.py", input: "decrypted ELF", output: "*_image.bin", purpose: "Apply PSP relocations and emit the flat guest memory image." },
  { id: 2, script: "imports.py", input: "decrypted ELF", output: "*_imports.toml", purpose: "Parse PRX import stubs and map addresses to libraries/NIDs." },
  { id: 3, script: "analyze.py + codegen.py", input: "ELF + extra PRXs", output: "*_recomp.c + 8 chunks", purpose: "Discover entries and translate MIPS blocks to C. HST-specific replacements live in tools/host_stubs.py; LOOP_CAPS are retired." },
  { id: 4, script: "Makefile / hst_manager.ps1", input: "generated C + src/rt", output: "hst.exe", purpose: "Compile generated chunks at O0, runtime at O2, then link SDL3, Vulkan, and Windows media/system libraries." },
];

export interface RuntimeSubsystem { id: string; name: string; file: string; purpose: string; status: "complete" | "in-progress" | "partial"; }
export const RUNTIME_SUBSYSTEMS: RuntimeSubsystem[] = [
  { id: "recomp", name: "Dispatch / CpuState", file: "recomp.h / recomp.c", purpose: "Guest state ABI, memory helpers, function dispatch, HLE trap, and late-import resolution.", status: "in-progress" },
  { id: "sched", name: "Fiber scheduler", file: "sched.c", purpose: "Cooperative PSP-thread scheduling over Windows fibers; lower PSP priority numbers run first.", status: "in-progress" },
  { id: "hle", name: "HLE", file: "hle.c", purpose: "Kernel, thread, I/O, display, font, utility, and media syscall handlers registered in s_hle[].", status: "in-progress" },
  { id: "ge", name: "Software GE", file: "ge.c", purpose: "Reference software rasterizer and PSP framebuffer semantics.", status: "partial" },
  { id: "vulkan", name: "SDL3/Vulkan", file: "gpu_sdl3vk/", purpose: "Window/input/presentation plus optional GPU GE capture and rasterization.", status: "partial" },
  { id: "vfpu", name: "VFPU", file: "vfpu_interp.c", purpose: "Fallback interpreter and PPSSPP-origin lookup tables for HST's covered VFPU words.", status: "partial" },
  { id: "audio", name: "Audio", file: "audio.c", purpose: "SDL3 audio stream backend; higher-level ATRAC behavior remains incomplete.", status: "partial" },
  { id: "iso", name: "ISO/VFS", file: "iso.c / iso.h", purpose: "ISO9660, Joliet, Rock Ridge, multi-extent, and host/extracted-asset paths.", status: "in-progress" },
  { id: "mpeg", name: "MPEG/PSMF", file: "mpeg.c / h264_*.c", purpose: "Player bookkeeping and Windows Media Foundation decode; data getters remain blocked.", status: "partial" },
  { id: "savedata", name: "Savedata/dialogs", file: "savedata.c / osk_win.c", purpose: "Host memstick storage and partial utility-dialog behavior.", status: "partial" },
  { id: "pgf", name: "PGF fonts", file: "pgf.c / pgf.h", purpose: "Parsed metrics and glyph rasterization using PPSSPP replacement fonts.", status: "complete" },
];

export interface LoopCap { address: string; name: string; limit: number; action: string; note: string; }
export const LOOP_CAPS: LoopCap[] = [];

export interface ProgressPhase { id: string; name: string; description: string; earned: number; total: number; regressed: number; pending: number; }
export const PROGRESS_PHASES: ProgressPhase[] = [
  { id: "P1", name: "Pipeline", description: "ELF load, import analysis, code generation, and native build.", earned: 0, total: 0, regressed: 0, pending: 0 },
  { id: "P2", name: "Runtime", description: "Dispatch, HLE, scheduler, filesystems, and platform integration.", earned: 0, total: 0, regressed: 0, pending: 0 },
  { id: "P3", name: "Translation", description: "MIPS/VFPU correctness and differential verification.", earned: 0, total: 0, regressed: 0, pending: 0 },
  { id: "P4", name: "Game data", description: "Resource, text, font, and asset loading.", earned: 0, total: 0, regressed: 0, pending: 0 },
  { id: "P5", name: "Rendering", description: "Software/Vulkan frame production and presentation.", earned: 0, total: 0, regressed: 0, pending: 0 },
  { id: "P6", name: "Main loop", description: "Sustained frame progression and input/audio behavior.", earned: 0, total: 0, regressed: 0, pending: 0 },
  { id: "P7", name: "Release", description: "Playable flows, packaging, portability, and polish.", earned: 0, total: 0, regressed: 0, pending: 0 },
];
export const PROGRESS_TOTAL = { earned: 0, regressed: 0, total: 0, pct: 0 };

export interface EnvVar { name: string; values: string; purpose: string; category: "gpu" | "trace" | "runtime" | "path"; }
export const ENV_VARS: EnvVar[] = [
  { name: "SR_DEBUG", values: "bitmask", purpose: "Central debug categories; 0xFF enables all.", category: "trace" },
  { name: "SR_GPU_GE", values: "0/1", purpose: "Enable GPU GE path; 0 keeps software rasterization.", category: "gpu" },
  { name: "SR_GPU_SCALE", values: "1..4", purpose: "Internal GPU rendering scale.", category: "gpu" },
  { name: "SR_VIDEO", values: "gdi", purpose: "Select the Win32/GDI fallback presenter.", category: "gpu" },
  { name: "SR_WATCHDOG_EXIT", values: "vblanks", purpose: "Abort after this many vblanks without a new frame.", category: "runtime" },
  { name: "SR_HLELOG / SR_THLOG / SR_IOLOG", values: "0/1", purpose: "Subsystem-specific traces.", category: "trace" },
  { name: "PSP_VFPU_TABLES", values: "path", purpose: "VFPU lookup-table directory.", category: "path" },
  { name: "PSP_ISO", values: "path", purpose: "User-supplied game ISO.", category: "path" },
  { name: "SR_FSDIR", values: "path", purpose: "Host memstick/save root.", category: "path" },
  { name: "SR_DATAROOT", values: "path", purpose: "Override extracted game-data root.", category: "path" },
];

export interface PortingStep { step: number; title: string; detail: string; }
export const PORTING_STEPS: PortingStep[] = [
  { step: 1, title: "Obtain an authorized decrypted ELF", detail: "Keep all game files local and ignored." },
  { step: 2, title: "Determine load variables", detail: "Read the ELF headers and pass GAME_NAME, GAME_ELF, GAME_BASE, and GAME_ENTRY explicitly." },
  { step: 3, title: "Identify extra PRXs", detail: "Add required modules with verified, non-overlapping load bases." },
  { step: 4, title: "Build in two phases", detail: "Use the Makefile all target so chunk discovery happens after pipeline generation." },
  { step: 5, title: "Trace startup", detail: "Enable targeted SR_DEBUG categories and inspect missing imports, dispatch targets, and filesystem paths." },
  { step: 6, title: "Implement missing HLE", detail: "Add verified NID mappings/handlers; do not rely on nonexistent permissive variables or linker wrapping." },
  { step: 7, title: "Verify translation", detail: "Use selftest, static verification, and authorized external oracle traces." },
  { step: 8, title: "Validate graphics and lifecycle", detail: "Compare software/GPU output and exercise repeated frames, input, audio, saves, and clean exit." },
];

export interface FunctionMapEntry { address: string; name: string; size: string; role: string; }
export const FUNCTION_MAP: FunctionMapEntry[] = [
  { address: "0x0029a060", name: "HST entry", size: "—", role: "Runtime-resolved guest entry used after loading the flat image." },
  { address: "0x000468c8", name: "main_RunGameLoop", size: "—", role: "Guest worker/main-loop path." },
  { address: "0x00015fb4", name: "text parser loop", size: "—", role: "Current worker samples occur here/near 0x0019660c." },
  { address: "0x00065104", name: "resource accessor", size: "—", role: "Former zero-root frontier; root construction is now retained." },
];

export interface ThreadMapEntry { uid: string; entry: string; state: string; role: string; }
export const THREAD_MAP: ThreadMapEntry[] = [
  { uid: "run-dependent", entry: "0x0029a060", state: "startup", role: "Initial guest entry thread." },
  { uid: "run-dependent", entry: "0x000468c8", state: "worker", role: "Game-loop worker; current parser spin occurs on this path." },
];
