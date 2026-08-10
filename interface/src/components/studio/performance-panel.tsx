"use client";

import { Gauge, Cpu, Layers, Zap } from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel, SectionHeader, Field } from "./ui-bits";
import { Switch } from "@/components/ui/switch";
import { Label } from "@/components/ui/label";
import { ToggleGroup, ToggleGroupItem } from "@/components/ui/toggle-group";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import type { CpuClockMode, PerformanceConfig } from "@/lib/recompiler/types";

export function PerformancePanel() {
  const { config, updatePerformance } = useStudio();
  const p = config.performance;

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<Gauge className="size-4.5" />}
        title="Performance"
        subtitle="Recompile the Allegrex CPU faster than the real silicon ever ran, and lift the SDK's own throttles."
      />

      <Panel title="CPU clock" icon={<Cpu className="size-4" />}>
        <Field label="Clock mode" hint="PSP caps games at 222 MHz by SDK policy">
          <Select
            value={p.cpuClockMode}
            onValueChange={(v) => updatePerformance({ cpuClockMode: v as CpuClockMode })}
          >
            <SelectTrigger className="h-8">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="native">Native (222 MHz, capped)</SelectItem>
              <SelectItem value="max333">Max (333 MHz, Sony max)</SelectItem>
              <SelectItem value="333 unlocked">333 MHz unlocked (recommended)</SelectItem>
              <SelectItem value="oc444">Overclock 444 MHz (hot)</SelectItem>
            </SelectContent>
          </Select>
        </Field>
        <div className="mt-3 grid grid-cols-3 gap-2 text-center">
          <ClockMeter label="222" active={p.cpuClockMode === "native"} color="bg-muted-foreground" />
          <ClockMeter label="333" active={p.cpuClockMode === "max333" || p.cpuClockMode === "333 unlocked"} color="bg-emerald-500" />
          <ClockMeter label="444" active={p.cpuClockMode === "oc444"} color="bg-amber-500" />
        </div>
      </Panel>

      <Panel title="Recompiler engine" icon={<Layers className="size-4" />}>
        <div className="grid sm:grid-cols-2 gap-3">
          <ToggleRow
            label="JIT cache"
            desc="Compile blocks once, reuse"
            checked={p.jitCache}
            onChange={(v) => updatePerformance({ jitCache: v })}
          />
          <ToggleRow
            label="Block linking"
            desc="Skip dispatcher between blocks"
            checked={p.blockLinking}
            onChange={(v) => updatePerformance({ blockLinking: v })}
          />
          <ToggleRow
            label="Fast memory"
            desc="Skip MMU checks (safe on host)"
            checked={p.fastMemory}
            onChange={(v) => updatePerformance({ fastMemory: v })}
          />
          <ToggleRow
            label="Vertex cache"
            desc="Cache decoded vertex data"
            checked={p.vertexCache}
            onChange={(v) => updatePerformance({ vertexCache: v })}
          />
          <ToggleRow
            label="Shader cache"
            desc="Persist SPIR-V / DXIL on disk"
            checked={p.shaderCache}
            onChange={(v) => updatePerformance({ shaderCache: v })}
          />
          <ToggleRow
            label="I/O threading"
            desc="Async UMD reads"
            checked={p.ioThreading}
            onChange={(v) => updatePerformance({ ioThreading: v })}
          />
        </div>
      </Panel>

      <Panel title="Threading & scheduling" icon={<Zap className="size-4" />}>
        <Field label="Host thread affinity">
          <ToggleGroup
            type="single"
            value={p.threadAffinity}
            onValueChange={(v) =>
              v && updatePerformance({ threadAffinity: v as PerformanceConfig["threadAffinity"] })
            }
            className="grid grid-cols-3 gap-1.5 w-full"
          >
            <ToggleGroupItem
              value="single"
              className="flex-col h-auto py-2 data-[state=on]:bg-primary/15 data-[state=on]:text-primary data-[state=on]:border-primary/30"
            >
              <span className="text-xs font-semibold">Single</span>
              <span className="text-[9px] text-muted-foreground">1 core</span>
            </ToggleGroupItem>
            <ToggleGroupItem
              value="dual"
              className="flex-col h-auto py-2 data-[state=on]:bg-primary/15 data-[state=on]:text-primary data-[state=on]:border-primary/30"
            >
              <span className="text-xs font-semibold">Dual</span>
              <span className="text-[9px] text-muted-foreground">main + ME</span>
            </ToggleGroupItem>
            <ToggleGroupItem
              value="all"
              className="flex-col h-auto py-2 data-[state=on]:bg-primary/15 data-[state=on]:text-primary data-[state=on]:border-primary/30"
            >
              <span className="text-xs font-semibold">All</span>
              <span className="text-[9px] text-muted-foreground">spread</span>
            </ToggleGroupItem>
          </ToggleGroup>
        </Field>

        <div className="mt-3">
          <Field label="Frame skip" hint={p.frameSkip}>
            <Select
              value={p.frameSkip}
              onValueChange={(v) =>
                updatePerformance({ frameSkip: v as PerformanceConfig["frameSkip"] })
              }
            >
              <SelectTrigger className="h-8">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="off">Off</SelectItem>
                <SelectItem value="auto">Auto (hold 60)</SelectItem>
                <SelectItem value="1">Skip 1</SelectItem>
                <SelectItem value="2">Skip 2</SelectItem>
                <SelectItem value="3">Skip 3</SelectItem>
              </SelectContent>
            </Select>
          </Field>
        </div>

        <div className="mt-3">
          <ToggleRow
            label="Unthrottled fast-forward"
            desc="Remove speed cap when fast-forwarding"
            checked={p.fastForwardUnthrottled}
            onChange={(v) => updatePerformance({ fastForwardUnthrottled: v })}
          />
        </div>
      </Panel>
    </div>
  );
}

function ClockMeter({
  label,
  active,
  color,
}: {
  label: string;
  active: boolean;
  color: string;
}) {
  return (
    <div
      className={`rounded-lg border px-2 py-2 transition-colors ${
        active ? "border-primary/40 bg-primary/5" : "border-border/50 bg-background/30"
      }`}
    >
      <div className={`mx-auto h-1.5 w-10 rounded-full ${active ? color : "bg-border"}`} />
      <div className={`text-xs font-mono mt-1 ${active ? "text-foreground" : "text-muted-foreground"}`}>
        {label} MHz
      </div>
    </div>
  );
}

function ToggleRow({
  label,
  desc,
  checked,
  onChange,
}: {
  label: string;
  desc?: string;
  checked: boolean;
  onChange: (v: boolean) => void;
}) {
  return (
    <div className="flex items-center justify-between rounded-lg border border-border/60 bg-background/30 px-3 py-2">
      <div className="min-w-0">
        <Label className="text-xs font-medium cursor-pointer">{label}</Label>
        {desc ? <p className="text-[10px] text-muted-foreground">{desc}</p> : null}
      </div>
      <Switch checked={checked} onCheckedChange={onChange} />
    </div>
  );
}
