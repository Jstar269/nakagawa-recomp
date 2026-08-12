"use client";

import { Wand2, Check } from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel, SectionHeader, RiskBadge } from "./ui-bits";
import { Switch } from "@/components/ui/switch";
import { Label } from "@/components/ui/label";
import { Badge } from "@/components/ui/badge";
import { PATCH_INFO } from "@/lib/recompiler/profiles";
import { allPatches } from "@/lib/recompiler/defaults";
import type { PatchId } from "@/lib/recompiler/types";
import { cn } from "@/lib/utils";

const STAGE_LABEL: Record<string, string> = {
  code: "Code patch",
  assets: "Asset patch",
  save: "Save patch",
};

export function PatchesPanel() {
  const { config, updatePatches } = useStudio();
  const enabled = config.patches.enabled;
  const count = Object.values(enabled).filter(Boolean).length;

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<Wand2 className="size-4.5" />}
        title="Game-Specific Patches"
        subtitle="Unimplemented design concepts for possible game-specific patches. These switches are previews and do not alter generated code, assets, saves, or the native runtime."
        right={
          <div className="text-right">
            <div className="text-2xl font-bold text-ball font-mono leading-none">{count}</div>
            <div className="text-[10px] text-muted-foreground uppercase">enabled</div>
          </div>
        }
      />

      <div className="grid sm:grid-cols-2 gap-3">
        {allPatches.map((key) => {
          const info = PATCH_INFO[key];
          const on = enabled[key];
          return (
            <Panel key={key} className="p-0">
              <div
                role="button"
                tabIndex={0}
                onClick={() => updatePatches({ [key]: !on } as Record<PatchId, boolean>)}
                onKeyDown={(e) => {
                  if (e.key === "Enter" || e.key === " ") {
                    e.preventDefault();
                    updatePatches({ [key]: !on } as Record<PatchId, boolean>);
                  }
                }}
                className={cn(
                  "w-full text-left p-3.5 transition-colors cursor-pointer outline-none focus-visible:ring-2 focus-visible:ring-ring/50",
                  on ? "bg-primary/5" : "",
                )}
              >
                <div className="flex items-start gap-3">
                  <div
                    className={cn(
                      "size-8 rounded-md grid place-items-center border shrink-0 mt-0.5 transition-colors",
                      on
                        ? "bg-primary/15 border-primary/40 text-primary"
                        : "bg-muted/40 border-border text-muted-foreground",
                    )}
                  >
                    {on ? <Check className="size-4" /> : <Wand2 className="size-4" />}
                  </div>
                  <div className="min-w-0 flex-1">
                    <div className="flex items-center gap-2 flex-wrap">
                      <Label className="text-xs font-semibold cursor-pointer leading-tight">
                        {info.label}
                      </Label>
                      <Badge
                        variant="outline"
                        className="text-[9px] h-4 px-1 font-mono text-muted-foreground"
                      >
                        {STAGE_LABEL[info.stage]}
                      </Badge>
                      <RiskBadge risk={info.risk} />
                    </div>
                    <p className="text-[11px] text-muted-foreground mt-1 leading-snug">
                      {info.description}
                    </p>
                  </div>
                  <Switch
                    checked={on}
                    onCheckedChange={(v) => updatePatches({ [key]: v } as Record<PatchId, boolean>)}
                  />
                </div>
              </div>
            </Panel>
          );
        })}
      </div>
    </div>
  );
}
