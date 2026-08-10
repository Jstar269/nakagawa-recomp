"use client";

import { Gamepad2, Sliders, Activity, ArrowLeftRight, Crosshair } from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel, SectionHeader, Field } from "./ui-bits";
import { GamepadDetector } from "./gamepad-detector";
import { Switch } from "@/components/ui/switch";
import { Slider } from "@/components/ui/slider";
import { Label } from "@/components/ui/label";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { CONTROLLER_SCHEMES } from "@/lib/recompiler/profiles";
import type { ControllerDevice } from "@/lib/recompiler/types";
import { cn } from "@/lib/utils";

export function ControllersPanel() {
  const { config, updateControllers, captureTarget, startCapture } = useStudio();
  const c = config.controllers;
  const scheme = CONTROLLER_SCHEMES[c.device];

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<Gamepad2 className="size-4.5" />}
        title="Modern Controllers"
        subtitle="Preview mappings for the PSP's virtual inputs. Native SDL gamepad input works; the adaptive-trigger and gyro switches below are unimplemented design concepts."
      />

      <Panel title="Device" icon={<Gamepad2 className="size-4" />}>
        <div className="grid grid-cols-2 sm:grid-cols-3 gap-2">
          {(Object.keys(CONTROLLER_SCHEMES) as ControllerDevice[]).map((d) => {
            const s = CONTROLLER_SCHEMES[d];
            const active = c.device === d;
            return (
              <button
                key={d}
                onClick={() => updateControllers({ device: d })}
                className={cn(
                  "rounded-lg border px-3 py-2.5 text-left transition-colors",
                  active
                    ? "border-primary/50 bg-primary/10"
                    : "border-border/60 bg-background/30 hover:border-border",
                )}
              >
                <div className="flex items-center gap-2">
                  <span
                    className={cn(
                      "size-7 rounded-md grid place-items-center text-[10px] font-bold font-mono border",
                      active
                        ? "bg-primary/20 border-primary/40 text-primary"
                        : "bg-muted/40 border-border text-muted-foreground",
                    )}
                  >
                    {s.glyph}
                  </span>
                  <div className="min-w-0">
                    <div className={cn("text-xs font-semibold leading-tight", active ? "text-primary" : "")}>
                      {s.label.split(" (")[0]}
                    </div>
                    <div className="text-[9px] text-muted-foreground">{s.buttons.length} btns</div>
                  </div>
                </div>
                <div className="flex gap-1 mt-2">
                  {s.supportsAdaptive ? (
                    <Badge variant="outline" className="text-[8px] h-4 px-1 text-emerald-300 border-emerald-500/30">
                      adaptive
                    </Badge>
                  ) : null}
                  {s.supportsGyro ? (
                    <Badge variant="outline" className="text-[8px] h-4 px-1 text-emerald-300 border-emerald-500/30">
                      gyro
                    </Badge>
                  ) : null}
                </div>
              </button>
            );
          })}
        </div>
      </Panel>

      <GamepadDetector />

      <Panel title="Feel & response" icon={<Sliders className="size-4" />}>
        <div className="grid sm:grid-cols-2 gap-4">
          <Field label="Stick deadzone" hint={`${c.deadzone}%`}>
            <Slider
              value={[c.deadzone]}
              min={0}
              max={40}
              step={1}
              onValueChange={(v) => updateControllers({ deadzone: v[0] })}
            />
          </Field>
          <Field label="Trigger sensitivity" hint={`${c.triggerSensitivity}%`}>
            <Slider
              value={[c.triggerSensitivity]}
              min={10}
              max={100}
              step={1}
              onValueChange={(v) => updateControllers({ triggerSensitivity: v[0] })}
            />
          </Field>
        </div>

        <div className="grid sm:grid-cols-3 gap-3 mt-3">
          <ToggleRow
            label="Rumble"
            desc="Haptic on hit / serve"
            checked={c.rumble}
            onChange={(v) => updateControllers({ rumble: v })}
          />
          <ToggleRow
            label="Adaptive triggers"
            desc="Charge shot resistance"
            checked={c.adaptiveTriggers}
            disabled={!scheme.supportsAdaptive}
            onChange={(v) => updateControllers({ adaptiveTriggers: v })}
          />
          <ToggleRow
            label="Gyro aim"
            desc="Tilt to aim serves"
            checked={c.gyroAim}
            disabled={!scheme.supportsGyro}
            onChange={(v) => updateControllers({ gyroAim: v })}
          />
        </div>
        {!scheme.supportsAdaptive || !scheme.supportsGyro ? (
          <p className="text-[10px] text-muted-foreground mt-2">
            Some features are dimmed because <span className="font-mono">{scheme.label}</span>{" "}
            doesn&apos;t expose them.
          </p>
        ) : null}
      </Panel>

      <Panel
        title="Button bindings"
        description={`PSP action → ${scheme.label} · click capture then press a pad button`}
        icon={<ArrowLeftRight className="size-4" />}
      >
        <div className="rounded-lg border border-border/60 divide-y divide-border/50">
          <div className="grid grid-cols-[1fr_auto] sm:grid-cols-[1.4fr_1fr_auto_auto] gap-2 px-3 py-2 bg-background/40 text-[10px] uppercase tracking-wide text-muted-foreground">
            <span>PSP action</span>
            <span className="hidden sm:block">Purpose</span>
            <span className="text-right">Mapped to</span>
            <span className="text-right w-16">Capture</span>
          </div>
          {c.bindings.map((b, i) => {
            const isCapturing = captureTarget === b.pspAction;
            return (
              <div
                key={b.pspAction}
                className={cn(
                  "grid grid-cols-[1fr_auto] sm:grid-cols-[1.4fr_1fr_auto_auto] gap-2 px-3 py-2 items-center transition-colors",
                  isCapturing && "bg-primary/5",
                )}
              >
                <div className="flex items-center gap-2 min-w-0">
                  <span className="size-6 rounded bg-primary/10 border border-primary/20 text-primary text-[9px] font-mono grid place-items-center shrink-0">
                    {b.pspAction.slice(0, 4)}
                  </span>
                  <span className="text-[11px] font-mono truncate">{b.pspAction}</span>
                </div>
                <span className="hidden sm:block text-[11px] text-muted-foreground truncate">
                  {b.label}
                </span>
                <Select
                  value={b.mappedTo}
                  onValueChange={(v) => {
                    const next = [...c.bindings];
                    next[i] = { ...b, mappedTo: v };
                    updateControllers({ bindings: next });
                  }}
                >
                  <SelectTrigger className="h-7 w-[140px] sm:ml-auto font-mono text-[11px]">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {scheme.buttons.map((btn) => (
                      <SelectItem key={btn} value={btn} className="font-mono text-[11px]">
                        {btn}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
                <Button
                  size="sm"
                  variant={isCapturing ? "default" : "outline"}
                  className="h-7 w-16 gap-1 text-[10px] shrink-0"
                  onClick={() => startCapture(b.pspAction, 0)}
                  disabled={!!captureTarget && !isCapturing}
                >
                  <Crosshair className={cn("size-3", isCapturing && "animate-pulse")} />
                  {isCapturing ? "…" : "cap"}
                </Button>
              </div>
            );
          })}
        </div>
      </Panel>
    </div>
  );
}

function ToggleRow({
  label,
  desc,
  checked,
  onChange,
  disabled,
}: {
  label: string;
  desc?: string;
  checked: boolean;
  onChange: (v: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <div
      className={cn(
        "flex items-center justify-between rounded-lg border border-border/60 bg-background/30 px-3 py-2",
        disabled && "opacity-50",
      )}
    >
      <div className="min-w-0">
        <Label className="text-xs font-medium cursor-pointer">{label}</Label>
        {desc ? <p className="text-[10px] text-muted-foreground">{desc}</p> : null}
      </div>
      <Switch checked={checked} onCheckedChange={onChange} disabled={disabled} />
    </div>
  );
}
