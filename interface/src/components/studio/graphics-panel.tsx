"use client";

import { MonitorPlay, Maximize, Gauge, Layers, Sparkles } from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel, SectionHeader, Field } from "./ui-bits";
import { CourtPreview } from "./court-preview";
import { VramViewerPanel } from "./vram-viewer";
import { Switch } from "@/components/ui/switch";
import { Slider } from "@/components/ui/slider";
import { Input } from "@/components/ui/input";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { ToggleGroup, ToggleGroupItem } from "@/components/ui/toggle-group";
import { Label } from "@/components/ui/label";
import { cn } from "@/lib/utils";
import { RESOLUTION_PRESETS } from "@/lib/recompiler/defaults";
import type {
  AspectRatio,
  FrameRateCap,
  GraphicsConfig,
  MsaaLevel,
  ResolutionPreset,
  TextureFilter,
} from "@/lib/recompiler/types";

export function GraphicsPanel() {
  const { config, updateGraphics } = useStudio();
  const g = config.graphics;

  const setRes = (preset: ResolutionPreset) => {
    if (preset === "custom") {
      updateGraphics({ resolutionPreset: "custom" });
    } else {
      const p = RESOLUTION_PRESETS[preset];
      updateGraphics({ resolutionPreset: preset, customWidth: p.w, customHeight: p.h });
    }
  };

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<MonitorPlay className="size-4.5" />}
        title="Graphics"
        subtitle="Push the PSP renderer past its 480×272 / 30fps ceiling. Internal render targets scale independently of the output resolution."
      />

      <CourtPreview />

      <VramViewerPanel />

      <Panel title="Resolution scaling" icon={<Maximize className="size-4" />}>
        <div className="grid grid-cols-2 sm:grid-cols-3 gap-2">
          {(Object.keys(RESOLUTION_PRESETS) as ResolutionPreset[])
            .filter((k) => k !== "custom")
            .map((k) => {
              const p = RESOLUTION_PRESETS[k];
              const active = g.resolutionPreset === k;
              return (
                <button
                  key={k}
                  onClick={() => setRes(k)}
                  className={cn(
                    "rounded-lg border px-3 py-2 text-left transition-colors",
                    active
                      ? "border-primary/50 bg-primary/10"
                      : "border-border/60 bg-background/30 hover:border-border",
                  )}
                >
                  <div className={cn("text-xs font-semibold", active ? "text-primary" : "")}>
                    {p.label}
                  </div>
                  <div className="text-[10px] font-mono text-muted-foreground mt-0.5">{p.note}</div>
                </button>
              );
            })}
          <button
            onClick={() => setRes("custom")}
            className={cn(
              "rounded-lg border px-3 py-2 text-left transition-colors",
              g.resolutionPreset === "custom"
                ? "border-primary/50 bg-primary/10"
                : "border-border/60 bg-background/30 hover:border-border",
            )}
          >
            <div className={cn("text-xs font-semibold", g.resolutionPreset === "custom" ? "text-primary" : "")}>
              Custom
            </div>
            <div className="text-[10px] font-mono text-muted-foreground mt-0.5">your own size</div>
          </button>
        </div>

        {g.resolutionPreset === "custom" ? (
          <div className="grid grid-cols-2 gap-2 mt-3">
            <Field label="Width" hint="px">
              <Input
                type="number"
                value={g.customWidth}
                min={480}
                max={7680}
                onChange={(e) => updateGraphics({ customWidth: Number(e.target.value) || 480 })}
                className="font-mono h-8"
              />
            </Field>
            <Field label="Height" hint="px">
              <Input
                type="number"
                value={g.customHeight}
                min={272}
                max={4320}
                onChange={(e) => updateGraphics({ customHeight: Number(e.target.value) || 272 })}
                className="font-mono h-8"
              />
            </Field>
          </div>
        ) : null}

        <div className="mt-3">
          <Field
            label="Internal render-to-texture scale"
            hint={`${g.renderToTextureScale.toFixed(2)}x`}
          >
            <Slider
              value={[g.renderToTextureScale]}
              min={0.5}
              max={2}
              step={0.05}
              onValueChange={(v) => updateGraphics({ renderToTextureScale: v[0] })}
            />
          </Field>
          <p className="text-[10px] text-muted-foreground mt-1">
            Scales the game&apos;s internal framebuffer before upscale. Higher = sharper UI, costlier.
          </p>
        </div>
      </Panel>

      <Panel title="Frame rate" icon={<Gauge className="size-4" />}>
        <Field label="Frame rate cap" hint={`${g.frameRateCap}`}>
          <ToggleGroup
            type="single"
            value={g.frameRateCap}
            onValueChange={(v) => v && updateGraphics({ frameRateCap: v as FrameRateCap })}
            className="grid grid-cols-4 gap-1.5 w-full"
          >
            {(
              [
                ["native", "30 fps", "native"],
                ["60", "60 fps", "doubled"],
                ["120", "120 fps", "interpolated"],
                ["unlocked", "Unlocked", "host"],
              ] as const
            ).map(([val, label, hint]) => (
              <ToggleGroupItem
                key={val}
                value={val}
                className="flex-col h-auto py-2 data-[state=on]:bg-primary/15 data-[state=on]:text-primary data-[state=on]:border-primary/30"
              >
                <span className="text-xs font-semibold">{label}</span>
                <span className="text-[9px] text-muted-foreground">{hint}</span>
              </ToggleGroupItem>
            ))}
          </ToggleGroup>
        </Field>

        <div className="grid sm:grid-cols-3 gap-3 mt-3">
          <ToggleRow
            label="Frame pacing"
            desc="Even frame intervals"
            checked={g.framePacing}
            onChange={(v) => updateGraphics({ framePacing: v })}
          />
          <ToggleRow
            label="V-Sync"
            desc="Tear-free output"
            checked={g.vsync}
            onChange={(v) => updateGraphics({ vsync: v })}
          />
          <ToggleRow
            label="Triple buffering"
            desc="Lower input lag w/ vsync"
            checked={g.tripleBuffering}
            onChange={(v) => updateGraphics({ tripleBuffering: v })}
          />
        </div>
      </Panel>

      <Panel title="Aspect & widescreen" icon={<Maximize className="size-4" />}>
        <div className="grid sm:grid-cols-2 gap-3">
          <Field label="Output aspect ratio">
            <Select
              value={g.aspectRatio}
              onValueChange={(v) => updateGraphics({ aspectRatio: v as AspectRatio })}
            >
              <SelectTrigger className="h-8">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="native">Native (PSP 4:3-ish)</SelectItem>
                <SelectItem value="16:9">16:9 Widescreen</SelectItem>
                <SelectItem value="16:10">16:10</SelectItem>
                <SelectItem value="21:9">21:9 Ultrawide</SelectItem>
                <SelectItem value="4:3">4:3</SelectItem>
                <SelectItem value="stretch">Stretch to fill</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <ToggleRow
            label="Widescreen hack"
            desc="Patch 4:3 → 16:9 projection"
            checked={g.widescreenHack}
            onChange={(v) => updateGraphics({ widescreenHack: v })}
          />
        </div>
      </Panel>

      <Panel title="Texture & filtering" icon={<Layers className="size-4" />}>
        <div className="grid sm:grid-cols-2 gap-3">
          <Field label="Texture filter">
            <Select
              value={g.textureFilter}
              onValueChange={(v) => updateGraphics({ textureFilter: v as TextureFilter })}
            >
              <SelectTrigger className="h-8">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="native">Native (nearest)</SelectItem>
                <SelectItem value="bilinear">Bilinear</SelectItem>
                <SelectItem value="bicubic">Bicubic</SelectItem>
                <SelectItem value="xbrz">xBRZ (retro sharp)</SelectItem>
                <SelectItem value="hybrid">Hybrid (recommended)</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field label="Texture upscale">
            <Select
              value={g.textureUpscale}
              onValueChange={(v) =>
                updateGraphics({ textureUpscale: v as GraphicsConfig["textureUpscale"] })
              }
            >
              <SelectTrigger className="h-8">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="off">Off</SelectItem>
                <SelectItem value="x2">2x</SelectItem>
                <SelectItem value="x4">4x</SelectItem>
                <SelectItem value="x6">6x</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field label="MSAA">
            <Select
              value={g.msaa}
              onValueChange={(v) => updateGraphics({ msaa: v as MsaaLevel })}
            >
              <SelectTrigger className="h-8">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="off">Off</SelectItem>
                <SelectItem value="2x">2x</SelectItem>
                <SelectItem value="4x">4x</SelectItem>
                <SelectItem value="8x">8x</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field label="Anisotropic filtering">
            <Select
              value={g.anisotropy}
              onValueChange={(v) =>
                updateGraphics({ anisotropy: v as GraphicsConfig["anisotropy"] })
              }
            >
              <SelectTrigger className="h-8">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="off">Off</SelectItem>
                <SelectItem value="2x">2x</SelectItem>
                <SelectItem value="4x">4x</SelectItem>
                <SelectItem value="8x">8x</SelectItem>
                <SelectItem value="16x">16x</SelectItem>
              </SelectContent>
            </Select>
          </Field>
        </div>
      </Panel>

      <Panel title="Post-processing" icon={<Sparkles className="size-4" />}>
        <div className="grid sm:grid-cols-2 gap-3">
          <Field label="Motion blur">
            <Select
              value={g.motionBlur}
              onValueChange={(v) =>
                updateGraphics({ motionBlur: v as GraphicsConfig["motionBlur"] })
              }
            >
              <SelectTrigger className="h-8">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="native">Native</SelectItem>
                <SelectItem value="off">Off</SelectItem>
                <SelectItem value="enhanced">Enhanced (velocity)</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field label="Depth of field">
            <Select
              value={g.depthOfField}
              onValueChange={(v) =>
                updateGraphics({ depthOfField: v as GraphicsConfig["depthOfField"] })
              }
            >
              <SelectTrigger className="h-8">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="native">Native</SelectItem>
                <SelectItem value="off">Off</SelectItem>
                <SelectItem value="enhanced">Enhanced (bokeh)</SelectItem>
              </SelectContent>
            </Select>
          </Field>
        </div>
        <div className="grid sm:grid-cols-2 gap-4 mt-3">
          <Field label="HUD scale" hint={`${g.hudScale.toFixed(2)}x`}>
            <Slider
              value={[g.hudScale]}
              min={0.75}
              max={2}
              step={0.05}
              onValueChange={(v) => updateGraphics({ hudScale: v[0] })}
            />
          </Field>
          <Field label="Sharpening" hint={`${g.sharpness}`}>
            <Slider
              value={[g.sharpness]}
              min={0}
              max={100}
              step={1}
              onValueChange={(v) => updateGraphics({ sharpness: v[0] })}
            />
          </Field>
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
