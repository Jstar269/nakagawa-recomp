"use client";

import { useEffect, useState, useCallback, useMemo } from "react";
import {
  Flame,
  RefreshCw,
  Cpu,
  Activity,
  Play,
  Search,
  TrendingDown,
  HelpCircle,
  Gauge
} from "lucide-react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Panel, SectionHeader, StatPill } from "./ui-bits";
import {
  ResponsiveContainer,
  BarChart,
  Bar,
  XAxis,
  YAxis,
  Tooltip,
  Legend,
  LineChart,
  Line
} from "recharts";

interface ProfileFunction {
  pc: string;
  calls: number;
  durationNs: number;
  avgDurationNs: number;
  readHits?: number;
  writeHits?: number;
}

interface ProfileBlock {
  pc: string;
  count: number;
}

interface WatchpointStat {
  label: string;
  reads: number;
  writes: number;
  hits: number;
}

interface ProfilerResponse {
  live: {
    functions: ProfileFunction[];
    blocks: ProfileBlock[];
    timestamp: number | null;
    watchpointStats?: WatchpointStat[];
  };
  trend: any[];
}

function formatBytes(bytes: number) {
  if (bytes === 0) return "0 B";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(2)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

export function ProfilerPanel() {
  const [data, setData] = useState<ProfilerResponse | null>(null);
  const [loading, setLoading] = useState(true);
  const [funcSearch, setFuncSearch] = useState("");
  const [blockSearch, setBlockSearch] = useState("");
  const [activeTab, setActiveTab] = useState<"functions" | "latency" | "blocks" | "trend">("functions");

  const fetchProfiler = useCallback(async () => {
    try {
      const res = await fetch("/api/recompiler/telemetry/profiler");
      if (res.ok) {
        const json = await res.json();
        setData(json);
      }
    } catch (e) {
      console.error("Failed to fetch profiler stats", e);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    fetchProfiler();
    const interval = setInterval(fetchProfiler, 3000);
    return () => clearInterval(interval);
  }, [fetchProfiler]);

  // Derived metrics
  const stats = useMemo(() => {
    if (!data || !data.live) {
      return {
        totalFuncs: 0,
        totalTimeMs: 0,
        totalBlocks: 0,
        hotFunc: null,
      };
    }

    const funcs = data.live.functions;
    const blocks = data.live.blocks;

    const totalFuncs = funcs.length;
    const totalTimeNs = funcs.reduce((acc, f) => acc + f.durationNs, 0);
    const totalTimeMs = Math.round(totalTimeNs / 1000000);
    const totalBlocks = blocks.reduce((acc, b) => acc + b.count, 0);

    const hotFunc = funcs.length > 0 ? funcs[0] : null;
    const hotPct = (hotFunc && totalTimeNs > 0)
      ? Math.round((hotFunc.durationNs / totalTimeNs) * 1000) / 10
      : 0;

    return {
      totalFuncs,
      totalTimeMs,
      totalBlocks,
      hotFunc: hotFunc ? { ...hotFunc, pct: hotPct } : null,
    };
  }, [data]);

  // Filtered lists
  const filteredFuncs = useMemo(() => {
    if (!data?.live?.functions) return [];
    return data.live.functions.filter(
      f => f.pc.toLowerCase().includes(funcSearch.toLowerCase())
    );
  }, [data, funcSearch]);

  const filteredBlocks = useMemo(() => {
    if (!data?.live?.blocks) return [];
    return data.live.blocks.filter(
      b => b.pc.toLowerCase().includes(blockSearch.toLowerCase())
    );
  }, [data, blockSearch]);

  // Top 15 Chart Data
  const chartFuncData = useMemo(() => {
    if (!data?.live?.functions) return [];
    return data.live.functions.slice(0, 15).map(f => ({
      name: `f_${f.pc.substring(2)}`,
      "Duration (ms)": Math.round(f.durationNs / 1000000 * 100) / 100,
      calls: f.calls,
      "Read Hits": f.readHits || 0,
      "Write Hits": f.writeHits || 0,
    }));
  }, [data]);

  const chartLatencyData = useMemo(() => {
    if (!data?.live?.functions) return [];
    // Sort descending by average latency
    const sorted = [...data.live.functions].sort((a, b) => b.avgDurationNs - a.avgDurationNs);
    return sorted.slice(0, 15).map(f => ({
      name: `f_${f.pc.substring(2)}`,
      "Avg Latency (μs)": Math.round(f.avgDurationNs / 1000 * 10) / 10,
    }));
  }, [data]);

  // Block Heatmap Grid Cells (Top 100)
  const heatmapCells = useMemo(() => {
    if (!data?.live?.blocks || data.live.blocks.length === 0) return [];
    const top100 = data.live.blocks.slice(0, 100);
    const maxVal = Math.max(...top100.map(b => b.count));
    return top100.map(b => ({
      pc: b.pc,
      count: b.count,
      intensity: maxVal > 0 ? b.count / maxVal : 0,
    }));
  }, [data]);

  // Historical trend lines
  const trendLineNames = useMemo(() => {
    if (!data?.trend || data.trend.length === 0) return [];
    const keys = new Set<string>();
    data.trend.forEach(point => {
      Object.keys(point).forEach(k => {
        if (k !== "build" && k !== "timestamp") keys.add(k);
      });
    });
    return Array.from(keys);
  }, [data]);

  return (
    <div className="space-y-4">
      <div className="flex flex-col sm:flex-row sm:items-center justify-between gap-3">
        <SectionHeader
          title="Hot-Path Performance Profiler"
          subtitle="Isolate hotspots, evaluate dynamic loop latencies, and track performance optimization trends."
        />
        <Button
          variant="outline"
          size="sm"
          onClick={fetchProfiler}
          disabled={loading}
          className="h-8 gap-1.5 self-start sm:self-auto"
        >
          <RefreshCw className={`size-3.5 ${loading ? 'animate-spin' : ''}`} />
          Refresh Stats
        </Button>
      </div>

      {/* Profiler Status Bar & Guide */}
      <div className="rounded-xl border border-primary/20 bg-primary/5 p-4 flex flex-col sm:flex-row items-start sm:items-center gap-3.5">
        <div className="size-9 rounded-lg bg-primary/10 border border-primary/30 flex items-center justify-center shrink-0">
          <Flame className="size-5 text-primary" />
        </div>
        <div className="space-y-0.5 min-w-0 flex-1">
          <div className="text-sm font-semibold text-foreground flex items-center gap-2">
            Profiler Engine Status:
            {stats.totalFuncs > 0 ? (
              <span className="text-emerald-400 font-bold flex items-center gap-1.5">
                <span className="relative flex size-2">
                  <span className="animate-ping absolute inline-flex h-full w-full rounded-full bg-emerald-400 opacity-75"></span>
                  <span className="relative inline-flex rounded-full size-2 bg-emerald-500"></span>
                </span>
                ACTIVE (Collecting traces)
              </span>
            ) : (
              <span className="text-amber-400 font-bold">READY (Awaiting logs)</span>
            )}
          </div>
          <div className="text-xs text-muted-foreground leading-relaxed">
            To generate execution telemetry, start your game build with <code className="text-primary font-mono bg-primary/10 px-1 rounded">SR_PROFILE=1</code> or run traces through the debugger.
          </div>
        </div>
      </div>

      {/* Stat Pills Grid */}
      <div className="grid grid-cols-2 lg:grid-cols-4 gap-3">
        <StatPill
          label="Profiled Functions"
          value={stats.totalFuncs.toString()}
        />
        <StatPill
          label="Total CPU Execution"
          value={`${stats.totalTimeMs.toLocaleString()} ms`}
          accent
        />
        <StatPill
          label="Basic Block Loops"
          value={stats.totalBlocks.toLocaleString()}
        />
        <StatPill
          label="Hottest Hotspot"
          value={stats.hotFunc ? `f_${stats.hotFunc.pc.substring(2)}` : "None"}
          accent={!!stats.hotFunc}
        />
      </div>

      {/* Interactive Telemetry Section */}
      <div className="grid grid-cols-1 xl:grid-cols-3 gap-4">
        {/* Workspace Chart Panel */}
        <div className="xl:col-span-2 space-y-4">
          <div className="rounded-xl border border-border bg-card p-4 space-y-4">
            <div className="flex flex-wrap items-center justify-between gap-3 border-b border-border/50 pb-3">
              <div className="flex items-center gap-2">
                <Gauge className="size-4 text-primary" />
                <span className="text-sm font-semibold">Real-Time Optimization charts</span>
              </div>
              <div className="flex bg-muted/40 p-0.5 rounded-lg border border-border/40 text-xs">
                <button
                  onClick={() => setActiveTab("functions")}
                  className={`px-3 py-1 rounded-md transition-all font-medium ${
                    activeTab === "functions" ? "bg-card shadow-sm text-foreground" : "text-muted-foreground hover:text-foreground"
                  }`}
                >
                  Hot Functions
                </button>
                <button
                  onClick={() => setActiveTab("latency")}
                  className={`px-3 py-1 rounded-md transition-all font-medium ${
                    activeTab === "latency" ? "bg-card shadow-sm text-foreground" : "text-muted-foreground hover:text-foreground"
                  }`}
                >
                  Latency (μs/call)
                </button>
                <button
                  onClick={() => setActiveTab("blocks")}
                  className={`px-3 py-1 rounded-md transition-all font-medium ${
                    activeTab === "blocks" ? "bg-card shadow-sm text-foreground" : "text-muted-foreground hover:text-foreground"
                  }`}
                >
                  Block Heatmap
                </button>
                <button
                  onClick={() => setActiveTab("trend")}
                  className={`px-3 py-1 rounded-md transition-all font-medium ${
                    activeTab === "trend" ? "bg-card shadow-sm text-foreground" : "text-muted-foreground hover:text-foreground"
                  }`}
                >
                  Trends
                </button>
              </div>
            </div>

            {/* Render selected workspace view */}
            <div className="h-[300px]">
              {activeTab === "functions" && (
                chartFuncData.length > 0 ? (
                  <ResponsiveContainer width="100%" height="100%">
                    <BarChart data={chartFuncData} margin={{ top: 10, right: 20, left: -20, bottom: 5 }}>
                      <XAxis dataKey="name" stroke="#666" tick={{ fontSize: 9 }} />
                      <YAxis yAxisId="left" stroke="#ff4d4d" tick={{ fontSize: 9 }} />
                      <YAxis yAxisId="right" orientation="right" stroke="#00E5FF" tick={{ fontSize: 9 }} />
                      <Tooltip contentStyle={{ background: "#1a1a1a", border: "1px solid #333", fontSize: 11 }} />
                      <Legend wrapperStyle={{ fontSize: 10 }} />
                      <Bar yAxisId="left" dataKey="Duration (ms)" fill="#ff4d4d" radius={[4, 4, 0, 0]} />
                      <Bar yAxisId="right" dataKey="Read Hits" fill="#00E5FF" radius={[4, 4, 0, 0]} opacity={0.8} />
                      <Bar yAxisId="right" dataKey="Write Hits" fill="#E040FB" radius={[4, 4, 0, 0]} opacity={0.8} />
                    </BarChart>
                  </ResponsiveContainer>
                ) : (
                  <div className="w-full h-full flex flex-col items-center justify-center text-xs text-muted-foreground">
                    No inclusive telemetry. Start game with profiling logs active.
                  </div>
                )
              )}

              {activeTab === "latency" && (
                chartLatencyData.length > 0 ? (
                  <ResponsiveContainer width="100%" height="100%">
                    <BarChart data={chartLatencyData} margin={{ top: 10, right: 10, left: -20, bottom: 5 }}>
                      <XAxis dataKey="name" stroke="#666" tick={{ fontSize: 9 }} />
                      <YAxis stroke="#666" tick={{ fontSize: 9 }} />
                      <Tooltip contentStyle={{ background: "#1a1a1a", border: "1px solid #333", fontSize: 11 }} />
                      <Legend wrapperStyle={{ fontSize: 10 }} />
                      <Bar dataKey="Avg Latency (μs)" fill="#ff9900" radius={[4, 4, 0, 0]} />
                    </BarChart>
                  </ResponsiveContainer>
                ) : (
                  <div className="w-full h-full flex flex-col items-center justify-center text-xs text-muted-foreground">
                    No inclusive telemetry. Start game with profiling logs active.
                  </div>
                )
              )}

              {activeTab === "blocks" && (
                heatmapCells.length > 0 ? (
                  <div className="w-full h-full flex flex-col justify-center">
                    <div className="grid grid-cols-10 gap-1.5 max-w-[480px] mx-auto p-2 bg-card border border-border/40 rounded-xl">
                      {heatmapCells.map((cell, idx) => (
                        <div
                          key={idx}
                          className="aspect-square w-full rounded transition-all duration-150 relative hover:scale-110 shadow"
                          style={{
                            backgroundColor: `rgba(239, 68, 68, ${Math.max(cell.intensity, 0.08)})`
                          }}
                          title={`Block ${cell.pc}: ${cell.count.toLocaleString()} iterations`}
                        />
                      ))}
                    </div>
                    <div className="flex justify-between max-w-[480px] mx-auto w-full text-[9px] text-muted-foreground mt-3 px-1">
                      <span>Top 1 Loop</span>
                      <div className="flex items-center gap-1.5">
                        <span>Low Loop Freq</span>
                        <div className="w-16 h-2 rounded bg-gradient-to-r from-red-500/10 to-red-500" />
                        <span>High Loop Freq</span>
                      </div>
                      <span>Top 100 Loop</span>
                    </div>
                  </div>
                ) : (
                  <div className="w-full h-full flex flex-col items-center justify-center text-xs text-muted-foreground">
                    No basic block yield iterations. Ensure SR_YIELD loop hooks are executing.
                  </div>
                )
              )}

              {activeTab === "trend" && (
                data?.trend && data.trend.length > 0 ? (
                  <ResponsiveContainer width="100%" height="100%">
                    <LineChart data={data.trend} margin={{ top: 10, right: 10, left: -20, bottom: 5 }}>
                      <XAxis dataKey="build" stroke="#666" tick={{ fontSize: 9 }} />
                      <YAxis stroke="#666" tick={{ fontSize: 9 }} />
                      <Tooltip contentStyle={{ background: "#1a1a1a", border: "1px solid #333", fontSize: 11 }} />
                      <Legend wrapperStyle={{ fontSize: 10 }} />
                      {trendLineNames.map((pc, idx) => {
                        const colors = ["#ff4d4d", "#33cc33", "#3399ff", "#ff9900", "#cc33ff"];
                        return (
                          <Line
                            key={pc}
                            type="monotone"
                            dataKey={pc}
                            name={`f_${pc.substring(2)}`}
                            stroke={colors[idx % colors.length]}
                            strokeWidth={2}
                            dot={{ r: 3 }}
                            activeDot={{ r: 5 }}
                          />
                        );
                      })}
                    </LineChart>
                  </ResponsiveContainer>
                ) : (
                  <div className="w-full h-full flex flex-col items-center justify-center text-xs text-muted-foreground">
                    No historical telemetry runs found. Commit builds and log telemetry.
                  </div>
                )
              )}
            </div>
          </div>
        </div>

        {/* Live Search Stats Lists */}
        <div className="space-y-4">
          {/* Top Profiled Functions Table */}
          <div className="rounded-xl border border-border bg-card p-4 flex flex-col h-[380px]">
            <div className="flex items-center justify-between gap-3 mb-2 border-b border-border/40 pb-2">
              <span className="text-xs font-semibold uppercase tracking-wider text-muted-foreground">
                Hottest Function Index
              </span>
              <div className="relative w-36">
                <Search className="absolute left-2 top-1/2 -translate-y-1/2 size-3 text-muted-foreground" />
                <Input
                  size={10}
                  className="pl-7 h-6 text-[10px]"
                  placeholder="Search PC..."
                  value={funcSearch}
                  onChange={(e) => setFuncSearch(e.target.value)}
                />
              </div>
            </div>

            <div className="flex-1 overflow-y-auto space-y-1.5 thin-scroll pr-1 text-xs">
              {filteredFuncs.length > 0 ? (
                filteredFuncs.map(f => (
                  <div
                    key={f.pc}
                    className="flex items-center justify-between p-2 rounded-lg border border-border/30 bg-background/25 font-mono text-[11px]"
                  >
                    <div className="flex flex-col gap-0.5">
                      <span className="text-primary font-bold">f_{f.pc.substring(2)}</span>
                      <span className="text-[9px] text-muted-foreground">{f.calls.toLocaleString()} calls</span>
                    </div>
                    <div className="text-right flex flex-col gap-0.5">
                      <span className="text-foreground">{Math.round(f.durationNs / 1000000 * 10) / 10} ms</span>
                      <span className="text-[9px] text-muted-foreground">{(f.avgDurationNs / 1000).toFixed(1)} μs/call</span>
                    </div>
                  </div>
                ))
              ) : (
                <div className="h-full flex items-center justify-center text-muted-foreground text-xs">
                  No functions match search.
                </div>
              )}
            </div>
          </div>
        </div>
      </div>

      {/* Loop Block Iterations Table */}
      <div className="rounded-xl border border-border bg-card p-4 space-y-3">
        <div className="flex items-center justify-between gap-3 border-b border-border/40 pb-2">
          <div className="flex items-center gap-2">
            <Activity className="size-4 text-amber-400" />
            <span className="text-xs font-bold uppercase tracking-wider text-muted-foreground">
              Loop Yield & basic Block yields
            </span>
          </div>
          <div className="relative w-48">
            <Search className="absolute left-2.5 top-1/2 -translate-y-1/2 size-3.5 text-muted-foreground" />
            <Input
              className="pl-8 h-8 text-xs"
              placeholder="Search basic block PC..."
              value={blockSearch}
              onChange={(e) => setBlockSearch(e.target.value)}
            />
          </div>
        </div>

        <div className="max-h-[220px] overflow-y-auto thin-scroll pr-1">
          <table className="w-full text-left text-xs font-mono">
            <thead>
              <tr className="border-b border-border/50 text-muted-foreground text-[10px] uppercase">
                <th className="py-2 pl-2">Basic Block PC</th>
                <th className="py-2">Generated Symbol</th>
                <th className="py-2 text-right pr-2">Execution Count</th>
              </tr>
            </thead>
            <tbody className="divide-y divide-border/20">
              {filteredBlocks.length > 0 ? (
                filteredBlocks.map(b => (
                  <tr key={b.pc} className="hover:bg-muted/10 transition-colors">
                    <td className="py-2 pl-2 text-foreground font-semibold">{b.pc}</td>
                    <td className="py-2 text-primary">f_{b.pc.substring(2)}</td>
                    <td className="py-2 text-right pr-2 font-bold text-amber-400">{b.count.toLocaleString()}</td>
                  </tr>
                ))
              ) : (
                <tr>
                  <td colSpan={3} className="py-8 text-center text-muted-foreground">
                    No basic block yield hooks match search.
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </div>

      {/* Memory Watchpoint Access Frequencies & Bandwidth */}
      <div className="rounded-xl border border-border bg-card p-4 space-y-3">
        <div className="flex items-center gap-2 border-b border-border/40 pb-2">
          <Cpu className="size-4 text-cyan-400" />
          <span className="text-xs font-bold uppercase tracking-wider text-muted-foreground">
            Active Memory Watchpoint Bandwidth & Access Frequencies
          </span>
        </div>

        <div className="overflow-x-auto">
          <table className="w-full text-left text-xs font-mono">
            <thead>
              <tr className="border-b border-border/50 text-muted-foreground text-[10px] uppercase">
                <th className="py-2 pl-2">Watchpoint Label</th>
                <th className="py-2 text-right">Read Hits</th>
                <th className="py-2 text-right">Write Hits</th>
                <th className="py-2 text-right">Total Hits</th>
                <th className="py-2 text-right pr-2">Est. Bandwidth</th>
              </tr>
            </thead>
            <tbody className="divide-y divide-border/20">
              {data?.live?.watchpointStats && data.live.watchpointStats.length > 0 ? (
                data.live.watchpointStats.map((wp) => (
                  <tr key={wp.label} className="hover:bg-muted/10 transition-colors">
                    <td className="py-2 pl-2 text-foreground font-semibold">{wp.label}</td>
                    <td className="py-2 text-right text-cyan-300">{wp.reads.toLocaleString()}</td>
                    <td className="py-2 text-right text-purple-300">{wp.writes.toLocaleString()}</td>
                    <td className="py-2 text-right text-foreground font-bold">{wp.hits.toLocaleString()}</td>
                    <td className="py-2 text-right pr-2 text-emerald-400 font-semibold">{formatBytes(wp.hits * 4)}</td>
                  </tr>
                ))
              ) : (
                <tr>
                  <td colSpan={5} className="py-8 text-center text-muted-foreground">
                    No active memory watchpoint hits detected in the latest run log.
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  );
}
