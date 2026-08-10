"use client";

import { Compass, Terminal, Settings, AlertTriangle, CheckCircle2 } from "lucide-react";
import { Panel, SectionHeader } from "./ui-bits";
import { Badge } from "@/components/ui/badge";
import { PORTING_STEPS, ENV_VARS } from "@/lib/recompiler/real-data";
import { cn } from "@/lib/utils";

const CATEGORY_STYLES: Record<string, string> = {
  gpu: "border-fuchsia-500/30 text-fuchsia-300 bg-fuchsia-500/10",
  trace: "border-sky-500/30 text-sky-300 bg-sky-500/10",
  runtime: "border-amber-500/30 text-amber-300 bg-amber-500/10",
  path: "border-emerald-500/30 text-emerald-300 bg-emerald-500/10",
};

export function PortingPanel() {
  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<Compass className="size-4.5" />}
        title="Porting Guide"
        subtitle="The recompiler is game-agnostic by design. The game-specific parts are: input files, build variables, and HLE stubs. Everything else (runtime, codegen, GPU backend) is shared."
      />

      {/* Porting steps */}
      <Panel
        title="8-step porting process"
        description="From decrypted ELF to running native exe"
        icon={<Compass className="size-4" />}
      >
        <div className="space-y-2">
          {PORTING_STEPS.map((step, i) => (
            <div key={step.step} className="flex items-start gap-3">
              <div className="flex flex-col items-center shrink-0">
                <div className="size-6 rounded-full bg-primary/15 border border-primary/30 text-primary text-[10px] font-mono font-bold grid place-items-center">
                  {step.step}
                </div>
                {i < PORTING_STEPS.length - 1 ? (
                  <div className="w-px h-full bg-border/60 mt-1 min-h-4" />
                ) : null}
              </div>
              <div className="flex-1 pb-2">
                <div className="text-xs font-semibold">{step.title}</div>
                <p className="text-[11px] text-muted-foreground mt-0.5 leading-snug">{step.detail}</p>
              </div>
            </div>
          ))}
        </div>
      </Panel>

      {/* Build variables */}
      <Panel
        title="Build variables"
        description="Makefile overrides — pass on the make command line"
        icon={<Settings className="size-4" />}
      >
        <div className="rounded-lg border border-border/60 divide-y divide-border/50">
          <div className="grid grid-cols-[120px_1fr_1fr] gap-2 px-2.5 py-1.5 bg-background/40 text-[9px] uppercase tracking-wide text-muted-foreground">
            <span>Variable</span>
            <span>Default</span>
            <span>Purpose</span>
          </div>
          {[
            ["GAME_NAME", "hst", "Short game ID, used in build dir names"],
            ["GAME_ELF", "eboot.elf", "Decrypted ELF filename"],
            ["GAME_BASE", "0", "ELF load base (MUST be 0 for flat-PRX, not 0x08804000)"],
            ["GAME_ENTRY", "0x0029a060", "Entry point address (first PC)"],
            ["VULKAN_SDK", "auto-detected (or set an explicit path)", "Current Vulkan SDK path"],
            ["GAME_EXTRA_ELFS", "(empty)", "Additional PRXs as path@loadaddress"],
          ].map(([name, def, purpose]) => (
            <div
              key={name}
              className="grid grid-cols-[120px_1fr_1fr] gap-2 px-2.5 py-1.5 items-center text-[10px] hover:bg-accent/20"
            >
              <span className="font-mono text-primary/80">{name}</span>
              <span className="font-mono text-muted-foreground">{def}</span>
              <span className="text-muted-foreground">{purpose}</span>
            </div>
          ))}
        </div>
      </Panel>

      {/* Environment variables */}
      <Panel
        title="Environment variables"
        description="Runtime instrumentation + configuration flags"
        icon={<Terminal className="size-4" />}
      >
        <div className="rounded-lg border border-border/60 divide-y divide-border/50 max-h-96 overflow-y-auto thin-scroll">
          <div className="grid grid-cols-[140px_60px_1fr_80px] gap-2 px-2.5 py-1.5 bg-background/40 text-[9px] uppercase tracking-wide text-muted-foreground sticky top-0">
            <span>Variable</span>
            <span>Values</span>
            <span>Purpose</span>
            <span>Category</span>
          </div>
          {ENV_VARS.map((v) => (
            <div
              key={v.name}
              className="grid grid-cols-[140px_60px_1fr_80px] gap-2 px-2.5 py-1.5 items-center text-[10px] hover:bg-accent/20"
            >
              <span className="font-mono text-primary/80">{v.name}</span>
              <span className="font-mono text-muted-foreground">{v.values}</span>
              <span className="text-muted-foreground">{v.purpose}</span>
              <Badge
                variant="outline"
                className={cn("text-[8px] h-4 px-1 capitalize justify-self-start", CATEGORY_STYLES[v.category])}
              >
                {v.category}
              </Badge>
            </div>
          ))}
        </div>
      </Panel>

      {/* Run profiles */}
      <Panel
        title="Run profiles"
        description="hst_manager.ps1 -Profile presets"
        icon={<Settings className="size-4" />}
      >
        <div className="grid grid-cols-2 sm:grid-cols-4 gap-2">
          {[
            { name: "Standard", desc: "Bounded compatibility logging, GPU enabled" },
            { name: "Performance", desc: "Log-free visual/audio smoke testing" },
            { name: "Benchmark", desc: "1 Hz frame/GPU timing plus perf.csv" },
            { name: "Diagnostics", desc: "Everything verbose, GE dump on" },
            { name: "Software", desc: "GPU off, software rasterizer only" },
          ].map((p) => (
            <div
              key={p.name}
              className="rounded-lg border border-border/60 bg-background/30 px-2.5 py-2"
            >
              <div className="text-xs font-semibold text-primary">{p.name}</div>
              <p className="text-[10px] text-muted-foreground mt-0.5 leading-snug">{p.desc}</p>
            </div>
          ))}
        </div>
      </Panel>

      {/* Troubleshooting */}
      <Panel
        title="Troubleshooting"
        description="Common symptoms and their fixes"
        icon={<AlertTriangle className="size-4" />}
      >
        <div className="space-y-1.5">
          {[
            ["Linker failure", "Check the first unresolved symbol plus UCRT64 SDL3 and the Vulkan SDK library path"],
            ["VULKAN_SDK not found", "Set VULKAN_SDK to your install path, or install the SDK"],
            ["SDL3.dll not found", "Ensure SDL3.dll is next to hst.exe (Makefile copies it)"],
            ["VFPU tables not loaded", "Set PSP_VFPU_TABLES=assets/vfpu"],
            ["Unknown NID crash", "Use SR_HLELOG=1, verify the NID mapping, then add a real HLE handler"],
            ["Infinite loop", "Inspect the sampled guest PC and generated stubs; LOOP_CAPS are retired"],
            ["No frame rendered", "Check GE/display logs and the current P0 in ISSUES.md"],
            ["Thread deadlock", "SR_THLOG=1, check thread states in sched.c"],
          ].map(([symptom, fix]) => (
            <div
              key={symptom}
              className="flex items-start gap-2 text-[11px] rounded-md px-2 py-1.5 bg-background/30 border border-border/40"
            >
              <AlertTriangle className="size-3 text-amber-400 shrink-0 mt-0.5" />
              <span className="font-medium text-foreground/90 w-36 shrink-0">{symptom}</span>
              <span className="text-muted-foreground">{fix}</span>
            </div>
          ))}
        </div>
      </Panel>

      {/* Don'ts */}
      <Panel
        title="Don'ts"
        description="Hard-won operational traps"
        icon={<CheckCircle2 className="size-4" />}
      >
        <div className="space-y-1.5">
          {[
            "Don't edit generated files in build/ (*_recomp*.c)",
            "Don't change GAME_BASE from 0 (Makefile default 0x08804000 is WRONG for flat-PRX)",
            "Don't raise optimization on hst_recomp_*.c — gcc OOMs at -O1+",
            "Don't reintroduce retired LOOP_CAPS; fix the underlying control flow or data path",
            "Don't assume GAME_NAME=mygame works without overrides",
            "Don't edit assets/vfpu/*.dat — copy from PPSSPP",
          ].map((dont) => (
            <div
              key={dont}
              className="flex items-start gap-2 text-[11px] rounded-md px-2 py-1.5 bg-amber-500/5 border border-amber-500/20"
            >
              <span className="text-amber-400 shrink-0">✗</span>
              <span className="text-muted-foreground">{dont}</span>
            </div>
          ))}
        </div>
      </Panel>
    </div>
  );
}
