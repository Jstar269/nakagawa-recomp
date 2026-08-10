"use client";

import { Zap, Sparkles, Timer, History, type LucideIcon } from "lucide-react";
import { useStudio } from "./studio-context";
import { PRESETS } from "@/lib/recompiler/presets";
import { cn } from "@/lib/utils";
import { useState } from "react";
import { useToast } from "@/hooks/use-toast";

const ACCENTS: Record<string, { ring: string; glow: string; text: string; icon: LucideIcon }> = {
  ball: { ring: "border-primary/40", glow: "shadow-[0_0_22px_-6px] shadow-primary/60", text: "text-primary", icon: Zap },
  violet: { ring: "border-fuchsia-400/40", glow: "shadow-[0_0_22px_-6px] shadow-fuchsia-500/50", text: "text-fuchsia-300", icon: Sparkles },
  amber: { ring: "border-amber-400/40", glow: "shadow-[0_0_22px_-6px] shadow-amber-500/50", text: "text-amber-300", icon: Timer },
  sky: { ring: "border-sky-400/40", glow: "shadow-[0_0_22px_-6px] shadow-sky-500/50", text: "text-sky-300", icon: History },
};

export function PresetBar() {
  const { config, update } = useStudio();
  const { toast } = useToast();
  const [active, setActive] = useState<string | null>(null);

  // Detect which preset matches the current config (best-effort).
  function detectActive(): string | null {
    for (const p of PRESETS) {
      const pc = p.apply();
      if (
        pc.graphics.resolutionPreset === config.graphics.resolutionPreset &&
        pc.graphics.frameRateCap === config.graphics.frameRateCap &&
        pc.minimizeStrategy === config.minimizeStrategy
      ) {
        return p.id;
      }
    }
    return null;
  }

  const detected = detectActive();

  return (
    <div className="rounded-xl border border-border/60 bg-card/40 glass p-3">
      <div className="flex items-center justify-between mb-2">
        <span className="text-[11px] font-semibold uppercase tracking-wide text-muted-foreground">
          Enhancement design presets
        </span>
        <span className="text-[10px] text-muted-foreground">
          preview only · not wired to the native runtime
        </span>
      </div>
      <div className="grid grid-cols-2 sm:grid-cols-4 gap-2">
        {PRESETS.map((p) => {
          const a = ACCENTS[p.accent];
          const Icon = a.icon;
          const isActive = (active ?? detected) === p.id;
          return (
            <button
              key={p.id}
              onClick={() => {
                const cfg = p.apply();
                // Replace the whole config.
                update("graphics", cfg.graphics);
                update("performance", cfg.performance);
                update("limitations", cfg.limitations);
                update("controllers", cfg.controllers);
                update("patches", cfg.patches);
                update("minimizeStrategy", cfg.minimizeStrategy);
                update("profileName", cfg.profileName);
                setActive(p.id);
                toast({
                  title: `${p.name} design selected`,
                  description: "Preview values changed; the current native runtime is unchanged.",
                });
              }}
              className={cn(
                "group relative rounded-lg border p-2.5 text-left transition-all hover:-translate-y-0.5",
                isActive
                  ? `${a.ring} bg-card/70 ${a.glow}`
                  : "border-border/60 bg-background/30 hover:border-border",
              )}
            >
              <div className="flex items-center gap-1.5 mb-1">
                <Icon className={cn("size-3.5", isActive ? a.text : "text-muted-foreground")} />
                <span className={cn("text-xs font-semibold", isActive ? a.text : "")}>
                  {p.name}
                </span>
              </div>
              <p className="text-[10px] text-muted-foreground font-mono leading-tight">{p.tagline}</p>
            </button>
          );
        })}
      </div>
    </div>
  );
}
