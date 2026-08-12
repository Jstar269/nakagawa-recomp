"use client";

import { Unlock, TriangleAlert, Check, X } from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel, SectionHeader, RiskBadge } from "./ui-bits";
import { Switch } from "@/components/ui/switch";
import { Label } from "@/components/ui/label";
import { LIMIT_INFO } from "@/lib/recompiler/profiles";
import { allLimits } from "@/lib/recompiler/defaults";
import type { PspLimit } from "@/lib/recompiler/types";
import { cn } from "@/lib/utils";

export function LimitationsPanel() {
  const { config, updateLimitations } = useStudio();
  const removed = config.limitations.removed;

  const count = Object.values(removed).filter(Boolean).length;

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<Unlock className="size-4.5" />}
        title="Remove PSP Limitations"
        subtitle="The PSP shipped with hard SDK caps on memory, VRAM, clock and draw calls. The recompiler re-points each to the host and removes the cap entirely."
        right={
          <div className="text-right">
            <div className="text-2xl font-bold text-ball font-mono leading-none">{count}</div>
            <div className="text-[10px] text-muted-foreground uppercase">removed</div>
          </div>
        }
      />

      <div className="grid sm:grid-cols-2 gap-3">
        {allLimits.map((key) => {
          const info = LIMIT_INFO[key];
          const isRemoved = removed[key];
          return (
            <Panel key={key} className="p-0">
              <div className="p-3.5">
                <div className="flex items-start justify-between gap-2 mb-2">
                  <div className="flex items-center gap-2 min-w-0">
                    <div
                      className={cn(
                        "size-7 rounded-md grid place-items-center border shrink-0",
                        isRemoved
                          ? "bg-primary/15 border-primary/30 text-primary"
                          : "bg-muted/40 border-border text-muted-foreground",
                      )}
                    >
                      {isRemoved ? <Check className="size-3.5" /> : <X className="size-3.5" />}
                    </div>
                    <Label className="text-xs font-semibold cursor-pointer leading-tight">
                      {info.label}
                    </Label>
                  </div>
                  <RiskBadge risk={info.risk} />
                </div>

                <div className="grid grid-cols-2 gap-1.5 mb-3">
                  <div
                    className={cn(
                      "rounded-md px-2 py-1.5 border",
                      !isRemoved
                        ? "bg-background/50 border-border/60"
                        : "bg-muted/20 border-border/40 opacity-60",
                    )}
                  >
                    <div className="text-[9px] uppercase text-muted-foreground">Native</div>
                    <div className="text-[11px] font-mono line-through decoration-muted-foreground/50">
                      {info.native}
                    </div>
                  </div>
                  <div
                    className={cn(
                      "rounded-md px-2 py-1.5 border",
                      isRemoved
                        ? "bg-primary/10 border-primary/30"
                        : "bg-muted/20 border-border/40 opacity-60",
                    )}
                  >
                    <div className="text-[9px] uppercase text-muted-foreground">Removed</div>
                    <div className={cn("text-[11px] font-mono", isRemoved ? "text-ball" : "")}>
                      {info.removed}
                    </div>
                  </div>
                </div>

                <div className="flex items-center justify-between">
                  <span className="text-[10px] text-muted-foreground">
                    {isRemoved ? "Cap removed" : "Capped (native)"}
                  </span>
                  <Switch
                    checked={isRemoved}
                    onCheckedChange={(v) => updateLimitations({ [key]: v } as Record<PspLimit, boolean>)}
                  />
                </div>

                {isRemoved && info.risk === "advanced" ? (
                  <div className="mt-2 flex items-start gap-1.5 rounded-md bg-amber-500/10 border border-amber-500/20 px-2 py-1.5">
                    <TriangleAlert className="size-3 text-amber-300 mt-0.5 shrink-0" />
                    <p className="text-[10px] text-amber-200/90 leading-snug">
                      Advanced: may affect game logic that assumes this constraint.
                    </p>
                  </div>
                ) : null}
              </div>
            </Panel>
          );
        })}
      </div>
    </div>
  );
}
