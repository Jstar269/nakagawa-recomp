"use client";

import React, { useEffect, useState } from "react";
import {
  Play,
  Disc3,
  Monitor,
  Gamepad2,
  Volume2,
  Sparkles,
  CheckCircle2,
  AlertCircle,
  FolderOpen,
  Settings2,
  Wrench,
  Loader2,
  Flame,
  ShieldCheck,
  Maximize,
} from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel } from "./ui-bits";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Switch } from "@/components/ui/switch";
import { Slider } from "@/components/ui/slider";
import { cn } from "@/lib/utils";
import { useToast } from "@/hooks/use-toast";
import { RESOLUTION_PRESETS } from "@/lib/recompiler/defaults";
import type { ResolutionPreset } from "@/lib/recompiler/types";

export function LauncherPanel() {
  const { config, updateGraphics, setSection, isoMeta } = useStudio();
  const { toast } = useToast();
  const [binaryReady, setBinaryReady] = useState<boolean | null>(null);
  const [launching, setLaunching] = useState(false);
  const [checking, setChecking] = useState(true);

  // Check if hst.exe exists on host
  useEffect(() => {
    async function checkBinary() {
      try {
        const res = await fetch("/api/recompiler/run", { cache: "no-store" });
        const data = await res.json();
        setBinaryReady(Boolean(data?.exists));
      } catch {
        setBinaryReady(false);
      } finally {
        setChecking(false);
      }
    }
    void checkBinary();
  }, []);

  async function handleLaunch() {
    setLaunching(true);
    try {
      const res = await fetch("/api/recompiler/manager", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          action: "Run",
          run: {
            profile: "Standard",
            durationSeconds: 0,
            noGui: false,
            softwareRender: false,
            snapshotInterval: null,
          },
        }),
      });
      const data = await res.json().catch(() => ({}));
      if (!res.ok) {
        throw new Error(data?.detail ?? data?.message ?? data?.error ?? "Launch failed");
      }
      toast({
        title: "Starting Game",
        description: "Hot Shots Tennis is launching in SDL3 + Vulkan...",
      });
    } catch (e) {
      toast({
        title: "Launch Error",
        description: String(e),
        variant: "destructive",
      });
    } finally {
      setLaunching(false);
    }
  }

  const g = config.graphics;
  const currentRes = g.resolutionPreset;

  return (
    <div className="space-y-6">
      {/* Hero Banner */}
      <div className="relative overflow-hidden rounded-2xl border border-primary/30 bg-gradient-to-br from-emerald-950/40 via-card/80 to-background p-6 lg:p-8 shadow-2xl">
        <div className="absolute -right-12 -top-12 size-64 rounded-full bg-emerald-500/10 blur-3xl pointer-events-none" />
        <div className="relative z-10 flex flex-col md:flex-row md:items-center justify-between gap-6">
          <div className="space-y-2 max-w-xl">
            <div className="flex items-center gap-2">
              <Badge className="bg-emerald-500/20 text-emerald-400 border-emerald-500/30 hover:bg-emerald-500/20 px-2.5 py-0.5">
                <Sparkles className="size-3 mr-1" /> PC Native Recompilation
              </Badge>
              {binaryReady ? (
                <Badge variant="outline" className="text-emerald-300 border-emerald-500/30 bg-emerald-950/30">
                  <CheckCircle2 className="size-3 mr-1" /> Ready to Play
                </Badge>
              ) : (
                <Badge variant="outline" className="text-amber-300 border-amber-500/30 bg-amber-950/30">
                  <AlertCircle className="size-3 mr-1" /> Setup Needed
                </Badge>
              )}
            </div>

            <h1 className="text-3xl lg:text-4xl font-black tracking-tight text-foreground font-sans">
              Hot Shots Tennis
              <span className="text-primary block text-2xl lg:text-3xl font-semibold opacity-90 mt-0.5">
                Get a Grip!
              </span>
            </h1>

            <p className="text-sm text-muted-foreground leading-relaxed">
              Experience the PlayStation Portable classic rebuilt natively for PC. Enjoy 4K resolution scaling,
              uncapped high-refresh gameplay, low-latency Vulkan graphics, and modern controller haptics.
            </p>
          </div>

          {/* Big Action Button */}
          <div className="flex flex-col items-stretch md:items-end gap-3 shrink-0">
            {checking ? (
              <Button size="lg" disabled className="h-14 px-8 rounded-xl text-base gap-2">
                <Loader2 className="size-5 animate-spin" /> Checking Status...
              </Button>
            ) : binaryReady ? (
              <Button
                size="lg"
                onClick={handleLaunch}
                disabled={launching}
                className="h-14 px-8 rounded-xl text-base font-bold bg-emerald-500 hover:bg-emerald-400 text-emerald-950 shadow-lg shadow-emerald-500/25 gap-3 transition-all hover:scale-[1.02] active:scale-[0.98]"
              >
                {launching ? <Loader2 className="size-5 animate-spin" /> : <Play className="size-5 fill-current" />}
                PLAY GAME
              </Button>
            ) : (
              <Button
                size="lg"
                onClick={() => setSection("build")}
                className="h-14 px-8 rounded-xl text-base font-bold bg-primary hover:bg-primary/90 text-primary-foreground shadow-lg shadow-primary/25 gap-3"
              >
                <Wrench className="size-5" /> Start Setup Wizard
              </Button>
            )}

            <div className="text-xs text-muted-foreground flex items-center gap-2">
              <span className="size-2 rounded-full bg-emerald-400 animate-pulse" />
              <span>Vulkan · 60 FPS Cap · Controller Ready</span>
            </div>
          </div>
        </div>
      </div>

      {/* Quick Player Settings Grid */}
      <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
        {/* Graphics & Resolution Card */}
        <Panel
          title="Display & Resolution"
          icon={<Monitor className="size-4 text-primary" />}
          className="p-4 space-y-3"
        >
          <p className="text-xs text-muted-foreground">
            Internal render scaling pushes crisp geometric clarity far beyond the PSP original.
          </p>

          <div className="grid grid-cols-2 gap-1.5 pt-1">
            {(["native", "x2", "x4", "x8"] as ResolutionPreset[]).map((resKey) => {
              const preset = RESOLUTION_PRESETS[resKey];
              const active = currentRes === resKey;
              return (
                <button
                  key={resKey}
                  onClick={() => updateGraphics({ resolutionPreset: resKey })}
                  className={cn(
                    "px-3 py-2 rounded-lg border text-left text-xs transition-colors",
                    active
                      ? "border-primary bg-primary/10 text-primary font-semibold"
                      : "border-border/60 hover:border-border text-muted-foreground hover:text-foreground",
                  )}
                >
                  <div>{preset.label}</div>
                  <div className="text-[10px] opacity-70">{preset.note}</div>
                </button>
              );
            })}
          </div>

          <div className="pt-2 border-t border-border/40 flex items-center justify-between text-xs">
            <span className="text-muted-foreground">Widescreen 16:9</span>
            <Badge variant="outline" className="text-[10px] text-emerald-400 border-emerald-500/30">
              Active
            </Badge>
          </div>
        </Panel>

        {/* Controller & Input Card */}
        <Panel
          title="Controller Setup"
          icon={<Gamepad2 className="size-4 text-primary" />}
          className="p-4 space-y-3"
        >
          <p className="text-xs text-muted-foreground">
            Native gamepad support with automatic button mappings for tennis swings.
          </p>

          <div className="rounded-lg bg-background/50 border border-border/60 p-3 flex items-center gap-3">
            <div className="size-10 rounded-lg bg-primary/10 border border-primary/20 grid place-items-center shrink-0">
              <Gamepad2 className="size-5 text-primary" />
            </div>
            <div className="min-w-0">
              <div className="text-xs font-semibold">DualSense / Xbox Pad</div>
              <div className="text-[10px] text-muted-foreground">SDL3 Gamepad Subsystem</div>
            </div>
            <Badge variant="outline" className="ml-auto text-[10px] text-emerald-400 border-emerald-500/30">
              Connected
            </Badge>
          </div>

          <div className="pt-2 border-t border-border/40 flex items-center justify-between">
            <Button
              variant="outline"
              size="sm"
              onClick={() => setSection("controllers")}
              className="w-full text-xs h-8 gap-1.5"
            >
              <Settings2 className="size-3.5" /> Customize Button Mappings
            </Button>
          </div>
        </Panel>

        {/* Game Assets & Saves Card */}
        <Panel
          title="Game Ingestion & Saves"
          icon={<Disc3 className="size-4 text-primary" />}
          className="p-4 space-y-3"
        >
          <p className="text-xs text-muted-foreground">
            Manage your lawful game files, tournaments, and memory stick progress.
          </p>

          <div className="space-y-2 text-xs">
            <div className="flex items-center justify-between p-2 rounded-lg bg-background/40 border border-border/40">
              <span className="text-muted-foreground">Game Disc (ISO)</span>
              {isoMeta.fileName ? (
                <span className="font-mono text-emerald-400 text-[11px] truncate max-w-[140px]">
                  {isoMeta.fileName}
                </span>
              ) : (
                <span className="text-amber-300 text-[11px]">Not Loaded</span>
              )}
            </div>

            <div className="flex items-center justify-between p-2 rounded-lg bg-background/40 border border-border/40">
              <span className="text-muted-foreground">Savedata Profile</span>
              <span className="text-foreground text-[11px]">Slot 1 (Auto-Save)</span>
            </div>
          </div>

          <div className="pt-2 border-t border-border/40 flex gap-2">
            <Button
              variant="outline"
              size="sm"
              onClick={() => setSection("iso")}
              className="flex-1 text-xs h-8 gap-1"
            >
              <FolderOpen className="size-3.5" /> ISO Details
            </Button>
            <Button
              variant="outline"
              size="sm"
              onClick={() => setSection("build")}
              className="flex-1 text-xs h-8 gap-1"
            >
              <Wrench className="size-3.5" /> Diagnostics
            </Button>
          </div>
        </Panel>
      </div>

      {/* Switch to Developer Studio Mode Callout */}
      <div className="rounded-xl border border-border/70 bg-card/40 p-4 flex flex-col sm:flex-row items-start sm:items-center justify-between gap-4">
        <div className="flex items-center gap-3">
          <div className="size-9 rounded-lg bg-muted/60 border border-border/70 grid place-items-center shrink-0">
            <Flame className="size-4.5 text-muted-foreground" />
          </div>
          <div>
            <h3 className="text-xs font-semibold">Looking for deep engine internals & telemetry?</h3>
            <p className="text-[11px] text-muted-foreground mt-0.5">
              Access the NID auditor, VRAM viewer, CPU profiler flamegraphs, shader visual checks, and compiler logs.
            </p>
          </div>
        </div>
        <Button
          variant="secondary"
          size="sm"
          onClick={() => setSection("build")}
          className="text-xs shrink-0 gap-1.5"
        >
          <Settings2 className="size-3.5" /> Open Studio Mode
        </Button>
      </div>
    </div>
  );
}
