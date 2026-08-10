"use client";

import { useEffect, useRef, useState } from "react";
import { MonitorPlay, Zap } from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel } from "./ui-bits";
import { cn } from "@/lib/utils";

const RES_MAP: Record<string, { w: number; h: number; label: string }> = {
  native: { w: 480, h: 272, label: "480×272" },
  x2: { w: 960, h: 544, label: "960×544" },
  x3: { w: 1440, h: 816, label: "1440×816" },
  x4: { w: 1920, h: 1088, label: "1920×1088" },
  x6: { w: 2880, h: 1632, label: "2880×1632" },
  x8: { w: 3840, h: 2176, label: "3840×2176" },
  custom: { w: 1920, h: 1088, label: "custom" },
};

const ASPECT_MAP: Record<string, number> = {
  native: 480 / 272,
  "16:9": 16 / 9,
  "16:10": 16 / 10,
  "21:9": 21 / 9,
  "4:3": 4 / 3,
  stretch: 16 / 9,
};

const FPS_MAP: Record<string, number> = {
  native: 30,
  "60": 60,
  "120": 120,
  unlocked: 144,
};

export function CourtPreview() {
  const { config } = useStudio();
  const g = config.graphics;
  const [frame, setFrame] = useState(0);
  const rafRef = useRef<number | null>(null);
  const lastTimeRef = useRef(0);

  const res =
    g.resolutionPreset === "custom"
      ? { w: g.customWidth, h: g.customHeight, label: `${g.customWidth}×${g.customHeight}` }
      : RES_MAP[g.resolutionPreset] ?? RES_MAP.x4;
  const aspect = ASPECT_MAP[g.aspectRatio] ?? 16 / 9;
  const fps = FPS_MAP[g.frameRateCap] ?? 60;
  const msaaOn = g.msaa !== "off";
  const filterSharp = g.textureFilter === "native";

  // Animate the ball at the selected FPS (capped to ~60 for display).
  const displayFps = Math.min(fps, 60);
  useEffect(() => {
    function loop(t: number) {
      if (!lastTimeRef.current) lastTimeRef.current = t;
      const dt = t - lastTimeRef.current;
      if (dt >= 1000 / displayFps) {
        setFrame((f) => (f + 1) % 1000);
        lastTimeRef.current = t;
      }
      rafRef.current = requestAnimationFrame(loop);
    }
    rafRef.current = requestAnimationFrame(loop);
    return () => {
      if (rafRef.current) cancelAnimationFrame(rafRef.current);
    };
  }, [displayFps]);

  // Rally simulation: the ball bounces between two players on opposite sides
  // of the net. Each rally cycle is 120 frames (2s at 60fps). The ball arcs
  // from one player to the other, and each player slides horizontally to
  // intercept.
  const rallyFrame = frame % 120;
  const rallyPhase = rallyFrame / 120; // 0..1
  // Ball travels left↔right; use a triangle wave for x position.
  const ballGoesRight = rallyFrame < 60;
  const halfPhase = ballGoesRight ? rallyFrame / 60 : (120 - rallyFrame) / 60; // 0..1
  const ballX = 18 + halfPhase * 64; // 18%..82%
  // Arc the ball with a parabolic height; peaks at mid-court.
  const ballY = 30 + Math.sin(halfPhase * Math.PI) * 38; // arc up then down
  // Player X positions: track the ball's target side. Player A (left) moves
  // when the ball is coming to them; Player B (right) moves otherwise.
  const playerAX = 16 + Math.sin(frame * 0.03) * 4; // slight idle drift
  const playerBX = 84 - Math.sin(frame * 0.03) * 4;
  // Ball trail (previous positions) for motion blur.
  const trailPhase = Math.max(0, halfPhase - 0.08);
  const trailX = 18 + trailPhase * 64;
  const trailY = 30 + Math.sin(trailPhase * Math.PI) * 38;

  // Resolution badge color: green for HD+, amber for low.
  const isHD = res.w >= 1280;

  return (
    <Panel
      title="Live preview"
      description="Mock court at your selected resolution & aspect ratio"
      icon={<MonitorPlay className="size-4" />}
    >
      <div className="grid sm:grid-cols-[1fr_auto] gap-3 items-start">
        {/* The preview frame */}
        <div className="relative w-full">
          <div
            className={cn(
              "relative w-full overflow-hidden rounded-lg border-2 transition-all",
              msaaOn ? "border-primary/40" : "border-border/60",
              filterSharp && "pixelated",
            )}
            style={{ aspectRatio: String(aspect) }}
          >
            {/* Court background */}
            <div className="absolute inset-0 bg-gradient-to-b from-emerald-700/60 to-emerald-900/70">
              {/* Court lines */}
              <svg
                viewBox="0 0 100 100"
                preserveAspectRatio="none"
                className="absolute inset-0 w-full h-full"
              >
                {/* Outer doubles boundary */}
                <rect
                  x="8"
                  y="12"
                  width="84"
                  height="76"
                  fill="none"
                  stroke="white"
                  strokeOpacity="0.7"
                  strokeWidth="0.4"
                />
                {/* Net */}
                <line
                  x1="50"
                  y1="12"
                  x2="50"
                  y2="88"
                  stroke="white"
                  strokeOpacity="0.9"
                  strokeWidth="0.6"
                />
                {/* Service lines */}
                <line
                  x1="8"
                  y1="35"
                  x2="92"
                  y2="35"
                  stroke="white"
                  strokeOpacity="0.6"
                  strokeWidth="0.3"
                />
                <line
                  x1="8"
                  y1="65"
                  x2="92"
                  y2="65"
                  stroke="white"
                  strokeOpacity="0.6"
                  strokeWidth="0.3"
                />
                {/* Center service line */}
                <line
                  x1="50"
                  y1="35"
                  x2="50"
                  y2="65"
                  stroke="white"
                  strokeOpacity="0.5"
                  strokeWidth="0.3"
                />
                {/* Singles boundary (inner) */}
                <rect
                  x="14"
                  y="12"
                  width="72"
                  height="76"
                  fill="none"
                  stroke="white"
                  strokeOpacity="0.5"
                  strokeWidth="0.3"
                />
              </svg>
            </div>

            {/* Player A (left, near side) */}
            <PlayerSprite x={playerAX} y={82} color="bg-sky-400" side="left" />

            {/* Player B (right, far side) */}
            <PlayerSprite x={playerBX} y={18} color="bg-rose-400" side="right" />

            {/* Ball trail (motion blur when enhanced) */}
            {g.motionBlur === "enhanced" ? (
              <div
                className="absolute size-2.5 rounded-full bg-yellow-300/30 blur-[3px]"
                style={{ left: `${trailX}%`, top: `${trailY}%`, transform: "translate(-50%, -50%)" }}
              />
            ) : null}
            <div
              className="absolute size-2.5 rounded-full bg-yellow-300 shadow-[0_0_8px_2px] shadow-yellow-300/60"
              style={{ left: `${ballX}%`, top: `${ballY}%`, transform: "translate(-50%, -50%)" }}
            />
            {/* Ball shadow on the court (tracks x, sits on court surface) */}
            <div
              className="absolute size-1.5 rounded-full bg-black/25 blur-[1px]"
              style={{ left: `${ballX}%`, top: `${ballGoesRight ? 82 : 18}%`, transform: "translate(-50%, -50%)" }}
            />

            {/* HUD overlay (scaled) */}
            <div
              className="absolute top-1 left-1 text-white/90 font-mono leading-none"
              style={{ fontSize: "0.55rem" }}
            >
              <div className="bg-black/40 rounded px-1 py-0.5">40-30</div>
            </div>
            <div
              className="absolute top-1 right-1 text-white/90 font-mono leading-none"
              style={{ fontSize: "0.55rem" }}
            >
              <div className="bg-black/40 rounded px-1 py-0.5">SET 1</div>
            </div>

            {/* FPS counter overlay */}
            <div className="absolute bottom-1 right-1 bg-black/50 rounded px-1 py-0.5 font-mono text-[0.5rem] text-emerald-300">
              {fps}fps
            </div>

            {/* DoF blur when enhanced */}
            {g.depthOfField === "enhanced" ? (
              <div className="absolute inset-0 bg-gradient-to-b from-transparent via-transparent to-black/10 pointer-events-none" />
            ) : null}
          </div>

          {/* Resolution + aspect badges below preview */}
          <div className="flex items-center gap-1.5 mt-2 flex-wrap">
            <span
              className={cn(
                "text-[10px] font-mono px-1.5 py-0.5 rounded border",
                isHD
                  ? "bg-emerald-500/10 border-emerald-500/30 text-emerald-300"
                  : "bg-amber-500/10 border-amber-500/30 text-amber-300",
              )}
            >
              {res.label}
            </span>
            <span className="text-[10px] font-mono px-1.5 py-0.5 rounded border bg-background/40 border-border/60 text-muted-foreground">
              {g.aspectRatio}
            </span>
            {msaaOn ? (
              <span className="text-[10px] font-mono px-1.5 py-0.5 rounded border bg-primary/10 border-primary/30 text-primary">
                MSAA {g.msaa}
              </span>
            ) : null}
            {g.anisotropy !== "off" ? (
              <span className="text-[10px] font-mono px-1.5 py-0.5 rounded border bg-primary/10 border-primary/30 text-primary">
                AF {g.anisotropy}
              </span>
            ) : null}
          </div>
        </div>

        {/* Stats sidebar */}
        <div className="flex sm:flex-col gap-2 sm:w-32">
          <StatBox label="Pixels" value={fmtPixels(res.w * res.h)} accent />
          <StatBox label="vs native" value={`${(((res.w * res.h) / (480 * 272)) * 100).toFixed(0)}%`} />
          <StatBox label="Frame time" value={`${(1000 / fps).toFixed(1)}ms`} />
          <StatBox
            label="HUD scale"
            value={`${g.hudScale.toFixed(2)}×`}
          />
        </div>
      </div>

      {/* Resolution comparison bar */}
      <div className="mt-3">
        <div className="flex items-center gap-1.5 mb-1">
          <Zap className="size-3 text-primary" />
          <span className="text-[10px] uppercase tracking-wide text-muted-foreground">
            Pixel budget vs native
          </span>
        </div>
        <div className="h-2 rounded-full bg-muted/30 overflow-hidden">
          <div
            className="h-full bg-gradient-to-r from-primary/60 to-primary transition-all"
            style={{
              width: `${Math.min(100, ((res.w * res.h) / (480 * 272)) * 4)}%`,
            }}
          />
        </div>
        <div className="flex justify-between text-[9px] font-mono text-muted-foreground mt-0.5">
          <span>480×272 (1×)</span>
          <span>{(((res.w * res.h) / (480 * 272)) * 100).toFixed(0)}% of native</span>
        </div>
      </div>
    </Panel>
  );
}

function fmtPixels(n: number): string {
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`;
  if (n >= 1_000) return `${(n / 1_000).toFixed(0)}K`;
  return String(n);
}

function StatBox({
  label,
  value,
  accent,
}: {
  label: string;
  value: string;
  accent?: boolean;
}) {
  return (
    <div className="rounded-lg border border-border/60 bg-background/30 px-2 py-1.5 flex-1">
      <div className="text-[9px] uppercase tracking-wide text-muted-foreground">{label}</div>
      <div className={cn("text-xs font-mono font-medium mt-0.5", accent ? "text-ball" : "")}>
        {value}
      </div>
    </div>
  );
}

function PlayerSprite({
  x,
  y,
  color,
  side,
}: {
  x: number;
  y: number;
  color: string;
  side: "left" | "right";
}) {
  return (
    <div
      className="absolute flex flex-col items-center"
      style={{ left: `${x}%`, top: `${y}%`, transform: "translate(-50%, -50%)" }}
    >
      {/* Head */}
      <div className={cn("size-2 rounded-full", color, "shadow-sm")} />
      {/* Body */}
      <div className={cn("w-2 h-3 rounded-sm", color, side === "left" ? "-rotate-3" : "rotate-3", "shadow-sm")} />
      {/* Racket hint */}
      <div
        className={cn("w-1 h-1 rounded-full", color, "opacity-80")}
        style={{ marginLeft: side === "left" ? "6px" : "-6px", marginTop: "-3px" }}
      />
    </div>
  );
}
