"use client";

import { useEffect, useState, useCallback } from "react";
import {
  Eye,
  Activity,
  CheckCircle2,
  XCircle,
  RefreshCw,
  Image as ImageIcon,
  Sliders,
  AlertCircle,
  Sparkles
} from "lucide-react";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Panel, SectionHeader, StatPill } from "./ui-bits";
import { cn } from "@/lib/utils";
import { ResponsiveContainer, BarChart, Bar, XAxis, YAxis, Tooltip, Legend } from "recharts";

interface FrameDiff {
  filename: string;
  width: number;
  height: number;
  total_pixels: number;
  diff_pixels: number;
  diff_pct: number;
  big_diff_pixels: number;
  big_diff_pct: number;
  max_delta: number;
  status: "pass" | "fail";
}

interface RegressionReport {
  timestamp: number;
  threshold: number;
  summary: {
    total_frames: number;
    passed_frames: number;
    failed_frames: number;
    pass_rate: number;
  };
  frames: FrameDiff[];
}

export function VisualRegressionPanel() {
  const [report, setReport] = useState<RegressionReport | null>(null);
  const [loading, setLoading] = useState(true);
  const [selectedFrame, setSelectedFrame] = useState<FrameDiff | null>(null);
  const [sliderVal, setSliderVal] = useState(50);
  const [refreshInterval, setRefreshInterval] = useState<NodeJS.Timeout | null>(null);

  const [showDiffMask, setShowDiffMask] = useState(false);
  const [varianceData, setVarianceData] = useState<any>(null);
  const [varianceLoading, setVarianceLoading] = useState(false);
  const [activeChannels, setActiveChannels] = useState<{ R: boolean; G: boolean; B: boolean; A: boolean }>({
    R: true,
    G: true,
    B: true,
    A: true
  });

  const fetchVariance = useCallback(async (filename: string) => {
    setVarianceLoading(true);
    try {
      const res = await fetch(`/api/recompiler/visual-regression/variance?file=${filename}`);
      if (res.ok) {
        const data = await res.json();
        setVarianceData(data);
      } else {
        setVarianceData(null);
      }
    } catch (e) {
      console.error("Failed to load variance details", e);
      setVarianceData(null);
    } finally {
      setVarianceLoading(false);
    }
  }, []);

  useEffect(() => {
    if (selectedFrame) {
      fetchVariance(selectedFrame.filename);
    } else {
      setVarianceData(null);
    }
  }, [selectedFrame, fetchVariance]);

  const fetchReport = useCallback(async () => {
    try {
      const res = await fetch("/api/recompiler/visual-regression/report");
      if (res.ok) {
        const data: RegressionReport = await res.json();
        setReport(data);

        // Auto-select first frame if none is selected
        if (data.frames.length > 0 && !selectedFrame) {
          setSelectedFrame(data.frames[0]);
        } else if (data.frames.length > 0 && selectedFrame) {
          // Update selected frame data from fresh report
          const updated = data.frames.find(f => f.filename === selectedFrame.filename);
          if (updated) setSelectedFrame(updated);
        }
      }
    } catch (e) {
      console.error("Failed to load visual regression report", e);
    } finally {
      setLoading(false);
    }
  }, [selectedFrame]);

  useEffect(() => {
    fetchReport();

    // Set up polling interval to fetch report updates every 2 seconds
    const interval = setInterval(fetchReport, 2000);
    setRefreshInterval(interval);

    return () => {
      if (interval) clearInterval(interval);
    };
  }, [fetchReport]);

  const selectFrame = (frame: FrameDiff) => {
    setSelectedFrame(frame);
    setSliderVal(50); // Reset slider to center
  };

  if (loading && !report) {
    return (
      <div className="space-y-4">
        <SectionHeader
          icon={<Eye className="size-4.5" />}
          title="Visual Checks & Regression"
          subtitle="Pixel-by-pixel framebuffer accuracy comparisons between Vulkan GPU rendering and reference oracle outputs."
        />
        <div className="flex items-center gap-2 py-12 justify-center text-muted-foreground text-xs">
          <Activity className="size-4 animate-pulse text-primary" /> Loading visual regression stats…
        </div>
      </div>
    );
  }

  const summary = report?.summary ?? { total_frames: 0, passed_frames: 0, failed_frames: 0, pass_rate: 0 };
  const frames = report?.frames ?? [];

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<Eye className="size-4.5" />}
        title="Visual Checks & Regression"
        subtitle="Compare recompiler framebuffer outputs directly against reference golden frames to catch shader regressions."
        right={
          <Button size="sm" variant="outline" className="h-8 gap-1.5" onClick={fetchReport}>
            <RefreshCw className="size-3.5" /> Refresh
          </Button>
        }
      />

      {/* Aggregate metrics */}
      <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
        <StatPill label="Total snapshots" value={String(summary.total_frames)} />
        <StatPill label="Passed frames" value={String(summary.passed_frames)} accent={summary.passed_frames > 0} />
        <StatPill label="Failed frames" value={String(summary.failed_frames)} accent={summary.failed_frames > 0} />
        <StatPill
          label="Match rate"
          value={`${summary.pass_rate}%`}
          accent={summary.pass_rate === 100}
        />
      </div>

      {frames.length === 0 ? (
        <Panel title="No Snapshots Recorded" icon={<ImageIcon className="size-4" />}>
          <div className="flex flex-col items-center justify-center py-10 text-center">
            <div className="size-12 rounded-xl bg-muted/30 border border-border grid place-items-center mb-3">
              <ImageIcon className="size-6 text-muted-foreground" />
            </div>
            <p className="text-sm font-medium">No Framebuffer Snapshots Found</p>
            <p className="text-xs text-muted-foreground max-w-md mt-1 leading-normal">
              Use <strong>Recompile → Capture 15s snapshots</strong> for a bounded headless software-render run.
              Native captures are PPM files; the image endpoint converts them to PNG only for browser display.
            </p>
          </div>
        </Panel>
      ) : (
        <div className="grid grid-cols-1 lg:grid-cols-[320px_1fr] gap-4">

          {/* Snapshots list */}
          <div className="space-y-3">
            <Panel title="Frames list" icon={<ImageIcon className="size-4" />} className="h-[480px] overflow-hidden flex flex-col">
              <div className="overflow-y-auto max-h-[420px] thin-scroll space-y-1.5 pr-1">
                {frames.map((f) => {
                  const active = selectedFrame?.filename === f.filename;
                  const passed = f.status === "pass";
                  return (
                    <button
                      key={f.filename}
                      onClick={() => selectFrame(f)}
                      className={cn(
                        "w-full flex items-center justify-between rounded-lg p-2.5 text-left border transition-all duration-150",
                        active
                          ? "bg-primary/15 border-primary/40 text-foreground"
                          : "bg-background/20 border-border/40 text-muted-foreground hover:text-foreground hover:bg-accent/30"
                      )}
                    >
                      <div className="min-w-0 flex-1">
                        <div className="text-xs font-semibold font-mono truncate">{f.filename}</div>
                        <div className="text-[10px] text-muted-foreground/80 mt-0.5 font-mono">
                          diff: {f.big_diff_pixels}px ({f.big_diff_pct}%)
                        </div>
                      </div>

                      <div className="flex items-center gap-1.5 shrink-0 pl-2">
                        {passed ? (
                          <Badge variant="outline" className="h-5 px-1.5 text-[10px] bg-emerald-500/10 border-emerald-500/30 text-emerald-300 gap-1">
                            <CheckCircle2 className="size-3" /> Pass
                          </Badge>
                        ) : (
                          <Badge variant="outline" className="h-5 px-1.5 text-[10px] bg-destructive/10 border-destructive/30 text-destructive gap-1">
                            <XCircle className="size-3" /> Fail
                          </Badge>
                        )}
                      </div>
                    </button>
                  );
                })}
              </div>
            </Panel>
          </div>

          {/* Slider comparison workspace */}
          <div className="space-y-4">
            {selectedFrame ? (
              <Panel
                title={`Visual comparison workspace — ${selectedFrame.filename}`}
                icon={<Sliders className="size-4" />}
                description="Interactive overlay comparison. Slide to wipe between reference oracle and active recompiler."
              >
                <div className="flex flex-col items-center gap-4">
                  {/* Workspace Controls */}
                  <div className="flex flex-col sm:flex-row justify-between items-center w-full max-w-[540px] gap-2 mb-1">
                    {/* Channel Selector Pills */}
                    <div className="flex items-center gap-1 bg-muted/40 p-0.5 rounded-lg border border-border/40 text-[10px]">
                      {(["R", "G", "B", "A"] as const).map((ch) => {
                        const active = activeChannels[ch];
                        return (
                          <button
                            key={ch}
                            onClick={() => setActiveChannels((prev) => ({ ...prev, [ch]: !prev[ch] }))}
                            className={cn(
                              "px-2.5 py-0.5 rounded transition-all font-semibold border border-transparent",
                              active
                                ? ch === "R"
                                  ? "bg-red-500/20 text-red-400 border-red-500/40"
                                  : ch === "G"
                                  ? "bg-emerald-500/20 text-emerald-400 border-emerald-500/40"
                                  : ch === "B"
                                  ? "bg-blue-500/20 text-blue-400 border-blue-500/40"
                                  : "bg-purple-500/20 text-purple-400 border-purple-500/40"
                                : "text-muted-foreground hover:text-foreground"
                            )}
                          >
                            Channel {ch}
                          </button>
                        );
                      })}
                    </div>

                    <Button
                      size="sm"
                      variant={showDiffMask ? "default" : "outline"}
                      className="h-8 gap-1.5 text-xs"
                      onClick={() => setShowDiffMask(!showDiffMask)}
                    >
                      <Sparkles className="size-3.5" />
                      {showDiffMask ? "Hide Difference Overlay" : "Overlay Difference Mask"}
                    </Button>
                  </div>

                  {/* Slider comparison window */}
                  <div className="relative w-full max-w-[540px] aspect-[480/272] select-none overflow-hidden rounded-xl border-2 border-border/80 bg-black shadow-2xl">

                    {/* Golden Reference (Oracle) - Underneath */}
                    <img
                      src={`/api/recompiler/visual-regression/image?type=golden&file=${selectedFrame.filename}`}
                      alt="Oracle Reference"
                      className="w-full h-full object-cover pointer-events-none"
                      draggable={false}
                    />

                    {/* Active Render (Recompiler) - Clipped Overlay */}
                    <div
                      className="absolute inset-0 pointer-events-none"
                      style={{ clipPath: `polygon(0 0, ${sliderVal}% 0, ${sliderVal}% 100%, 0 100%)` }}
                    >
                      <img
                        src={`/api/recompiler/visual-regression/image?type=snapshot&file=${selectedFrame.filename}`}
                        alt="Recompiler Active Render"
                        className="w-full h-full object-cover pointer-events-none"
                        draggable={false}
                      />
                    </div>

                    {/* Glowing Separator Line */}
                    <div
                      className="absolute top-0 bottom-0 w-0.5 bg-primary shadow-[0_0_12px_#00E5FF] pointer-events-none"
                      style={{ left: `${sliderVal}%` }}
                    />

                    {/* Drag Handle Badge */}
                    <div
                      className="absolute top-1/2 -translate-y-1/2 -translate-x-1/2 size-8 rounded-full bg-card/95 border-2 border-primary shadow-2xl flex items-center justify-center pointer-events-none cursor-ew-resize"
                      style={{ left: `${sliderVal}%` }}
                    >
                      <Sliders className="size-4 text-primary" />
                    </div>

                    {/* Diff Highlight Mask Overlay */}
                    {showDiffMask && (
                      <img
                        src={`/api/recompiler/visual-regression/image?type=diff&file=${selectedFrame.filename}&channels=${Object.keys(activeChannels).filter(k => activeChannels[k as keyof typeof activeChannels]).join(",")}`}
                        alt="Diff Highlight"
                        className="absolute inset-0 w-full h-full object-cover pointer-events-none mix-blend-screen z-10 animate-pulse"
                        style={{ opacity: 0.85 }}
                        draggable={false}
                      />
                    )}

                    {/* Labels */}
                    <div className="absolute left-3 top-3 px-2 py-0.5 rounded bg-black/60 border border-white/10 text-[9px] uppercase tracking-wide text-white select-none pointer-events-none z-20">
                      Reference Oracle (Left)
                    </div>
                    <div className="absolute right-3 top-3 px-2 py-0.5 rounded bg-primary/80 border border-primary/20 text-[9px] uppercase tracking-wide text-black select-none pointer-events-none font-semibold z-20">
                      Active Recompiler (Right)
                    </div>

                    {/* Transparent Range Input Overlay for Drag control */}
                    <input
                      type="range"
                      min="0"
                      max="100"
                      value={sliderVal}
                      onChange={(e) => setSliderVal(Number(e.target.value))}
                      className="absolute inset-0 w-full h-full opacity-0 cursor-ew-resize z-30"
                    />
                  </div>

                  {/* Slider Helper Controls */}
                  <div className="flex items-center gap-4 w-full max-w-[540px] text-xs justify-between">
                    <button
                      onClick={() => setSliderVal(0)}
                      className="text-muted-foreground hover:text-foreground hover:underline transition-colors font-medium"
                    >
                      100% Oracle Reference
                    </button>
                    <span className="text-[10px] text-muted-foreground/80 font-mono">
                      Split position: {sliderVal}%
                    </span>
                    <button
                      onClick={() => setSliderVal(100)}
                      className="text-primary hover:underline transition-colors font-medium"
                    >
                      100% Recompiler Output
                    </button>
                  </div>

                  {/* Selected Frame Metrics Details */}
                  <div className="w-full grid grid-cols-1 sm:grid-cols-2 gap-4 mt-2 pt-4 border-t border-border/50">
                    <div className="space-y-2">
                      <div className="text-[11px] uppercase tracking-wide text-muted-foreground font-semibold">
                        Comparison Statistics
                      </div>
                      <div className="rounded-lg border border-border/40 bg-background/30 p-3 space-y-1.5 font-mono text-xs">
                        <div className="flex justify-between">
                          <span className="text-muted-foreground">Dimensions:</span>
                          <span className="text-foreground">{selectedFrame.width} × {selectedFrame.height} px</span>
                        </div>
                        <div className="flex justify-between">
                          <span className="text-muted-foreground">Total Pixels:</span>
                          <span className="text-foreground">{selectedFrame.total_pixels.toLocaleString()}</span>
                        </div>
                        <div className="flex justify-between">
                          <span className="text-muted-foreground">Any-Diff Pixels:</span>
                          <span className="text-foreground">{selectedFrame.diff_pixels.toLocaleString()} ({selectedFrame.diff_pct}%)</span>
                        </div>
                        <div className="flex justify-between">
                          <span className="text-muted-foreground">Big-Diff (&gt;{report?.threshold ?? 3}/255):</span>
                          <span className={cn(selectedFrame.big_diff_pixels > 0 ? "text-destructive font-semibold" : "text-emerald-400")}>
                            {selectedFrame.big_diff_pixels.toLocaleString()} ({selectedFrame.big_diff_pct}%)
                          </span>
                        </div>
                        <div className="flex justify-between">
                          <span className="text-muted-foreground">Max Channel Delta:</span>
                          <span className={cn(selectedFrame.max_delta > (report?.threshold ?? 3) ? "text-destructive font-semibold" : "text-emerald-400")}>
                            {selectedFrame.max_delta}/255
                          </span>
                        </div>
                      </div>
                    </div>

                    <div className="space-y-2.5">
                      <div className="text-[11px] uppercase tracking-wide text-muted-foreground font-semibold">
                        Shader Pipeline Status
                      </div>

                      {selectedFrame.status === "pass" ? (
                        <div className="rounded-lg border border-emerald-500/20 bg-emerald-500/5 p-3 flex gap-2.5">
                          <CheckCircle2 className="size-5 text-emerald-400 shrink-0 mt-0.5" />
                          <div className="text-xs leading-normal">
                            <span className="font-semibold text-emerald-300 block mb-0.5">Reference Correct</span>
                            Zero pixels exceed the strict color variance threshold. This frame compiles and renders identically to the oracle.
                          </div>
                        </div>
                      ) : (
                        <div className="rounded-lg border border-destructive/20 bg-destructive/5 p-3 flex gap-2.5">
                          <AlertCircle className="size-5 text-destructive shrink-0 mt-0.5" />
                          <div className="text-xs leading-normal">
                            <span className="font-semibold text-destructive block mb-0.5">Pixel Drift Detected</span>
                            A total of {selectedFrame.big_diff_pixels.toLocaleString()} pixels deviate from the golden frame.
                            This typically suggests a Vulkan pipeline math regression, vertex cache mismatch, or an unhandled GE state transition.
                          </div>
                        </div>
                      )}
                    </div>
                  </div>

                  {/* Variance Details & Color Histograms */}
                  {varianceData && (
                    <div className="w-full space-y-5 mt-4 pt-4 border-t border-border/50">
                      <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
                        {/* Channel Delta Histograms */}
                        <div className="space-y-2">
                          <div className="text-[11px] uppercase tracking-wide text-muted-foreground font-semibold">
                            Color Channel Variance Histograms
                          </div>
                          <div className="rounded-lg border border-border/40 bg-background/20 p-3 h-[220px]">
                            <ResponsiveContainer width="100%" height="100%">
                              <BarChart data={varianceData.histogram} margin={{ top: 5, right: 5, left: -20, bottom: 5 }}>
                                <XAxis dataKey="range" tick={{ fontSize: 9 }} stroke="#888" />
                                <YAxis tick={{ fontSize: 9 }} stroke="#888" allowDecimals={false} />
                                <Tooltip contentStyle={{ background: "#222", border: "1px solid #444", fontSize: 10 }} />
                                <Legend wrapperStyle={{ fontSize: 10 }} />
                                {activeChannels.R && <Bar dataKey="R" fill="#ff4d4d" radius={[2, 2, 0, 0]} />}
                                {activeChannels.G && <Bar dataKey="G" fill="#4dff4d" radius={[2, 2, 0, 0]} />}
                                {activeChannels.B && <Bar dataKey="B" fill="#4d88ff" radius={[2, 2, 0, 0]} />}
                                {activeChannels.A && <Bar dataKey="A" fill="#a855f7" radius={[2, 2, 0, 0]} />}
                              </BarChart>
                            </ResponsiveContainer>
                          </div>
                        </div>

                        {/* Spatial Variance Heatmap */}
                        <div className="space-y-2">
                          <div className="text-[11px] uppercase tracking-wide text-muted-foreground font-semibold">
                            Spatial Variance Heatmap (Downscaled Grid)
                          </div>
                          <div className="rounded-lg border border-border/40 bg-background/20 p-3 flex flex-col justify-center items-center">
                            <div className="grid grid-cols-[repeat(30,minmax(0,1fr))] gap-[1px] w-full aspect-[480/272] bg-card/60 p-1 rounded border border-border/30">
                              {varianceData.grid.cells.map((cell: any, idx: number) => {
                                let totalDiff = 0;
                                let activeCount = 0;
                                if (activeChannels.R) { totalDiff += cell.r; activeCount++; }
                                if (activeChannels.G) { totalDiff += cell.g; activeCount++; }
                                if (activeChannels.B) { totalDiff += cell.b; activeCount++; }
                                if (activeChannels.A) { totalDiff += cell.a; activeCount++; }

                                const avg = activeCount > 0 ? totalDiff / activeCount : 0;
                                const intensity = Math.min(avg / 15, 1.0);
                                return (
                                  <div
                                    key={idx}
                                    className="w-full aspect-square transition-all duration-150"
                                    style={{
                                      backgroundColor: intensity > 0 ? `rgba(255, 0, 128, ${intensity})` : "rgba(255, 255, 255, 0.03)"
                                    }}
                                    title={`Cell ${idx}: dR=${cell.r}, dG=${cell.g}, dB=${cell.b}, dA=${cell.a}`}
                                  />
                                );
                              })}
                            </div>
                            <div className="flex justify-between w-full text-[9px] text-muted-foreground mt-2">
                              <span>Left</span>
                              <div className="flex items-center gap-1">
                                <span>No Drift</span>
                                <div className="w-12 h-2 rounded bg-gradient-to-r from-muted/30 to-[#ff0080]" />
                                <span>High Drift</span>
                              </div>
                              <span>Right</span>
                            </div>
                          </div>
                        </div>
                      </div>
                    </div>
                  )}
                </div>
              </Panel>
            ) : null}
          </div>
        </div>
      )}
    </div>
  );
}
