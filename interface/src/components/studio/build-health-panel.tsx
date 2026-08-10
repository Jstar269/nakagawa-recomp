"use client";

import React, { useState, useEffect, useMemo } from "react";
import {
  Activity,
  Plus,
  Trash2,
  Check,
  CheckCircle2,
  XCircle,
  AlertTriangle,
  RefreshCw,
  Save,
  Sliders,
  Settings,
  ShieldAlert,
  SlidersHorizontal,
  FolderLock
} from "lucide-react";
import {
  ResponsiveContainer,
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  Legend
} from "recharts";
import { Panel, SectionHeader } from "./ui-bits";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { useToast } from "@/hooks/use-toast";

interface Watchpoint {
  start: number;
  end: number;
  label: string;
}

interface DebugProfile {
  id: string;
  name: string;
  watchpoints: string; // JSON string of Watchpoint[]
  debugMask: number;
  isActive: boolean;
  createdAt: string;
  updatedAt: string;
}

interface SVMismatch {
  pc: string;
  register: string;
  activeState: string;
  expectedLatticeState: string;
}

interface TelemetryRun {
  id: string;
  timestamp: string;
  totalUnits: number;
  unitsEarned: number;
  unitsRegressed: number;
  completionPct: number;
  totalFunctions: number;
  matchedFunctions: number;
  totalBytes: number;
  matchedBytes: number;
  byteCompletionPct: number;
  svMismatchesCount: number;
  svMismatchesJson: string;
  fuzzTotalTrials: number;
  fuzzPassedTrials: number;
  fuzzFailedTrials: number;
  fuzzCoveragePct: number;
  vrTotalFrames: number;
  vrPassedFrames: number;
  vrFailedFrames: number;
  vrPassRate: number;
}

const DEBUG_CATEGORIES = [
  { bit: 0x01, label: "MEM (Memory Trap / OOR)", desc: "Memory access logging" },
  { bit: 0x02, label: "HLE (Syscalls)", desc: "HLE syscall dispatch tracing" },
  { bit: 0x04, label: "SCHED (Scheduler)", desc: "Thread scheduling events" },
  { bit: 0x08, label: "GE (Graphics Engine)", desc: "GE command processing" },
  { bit: 0x10, label: "INPUT (Controller)", desc: "Input state changes" },
  { bit: 0x20, label: "FS (Filesystem)", desc: "Filesystem / IO operations" },
  { bit: 0x40, label: "VIDEO (VBLANK)", desc: "Display, framebuffer, vblank" },
  { bit: 0x80, label: "MISC (Fonts/Audio)", desc: "Fonts, callbacks, etc." },
];

export function BuildHealthPanel() {
  const { toast } = useToast();
  const [activeTab, setActiveTab] = useState<"matrix" | "violations" | "profiles">("matrix");

  // Telemetry & DB state
  const [telemetry, setTelemetry] = useState<TelemetryRun[]>([]);
  const [loadingTelemetry, setLoadingTelemetry] = useState(false);
  const [selectedRunId, setSelectedRunId] = useState<string | null>(null);

  // Profile Manager state
  const [profiles, setProfiles] = useState<DebugProfile[]>([]);
  const [loadingProfiles, setLoadingProfiles] = useState(false);
  const [selectedProfileId, setSelectedProfileId] = useState<string | null>(null);

  // New profile form
  const [newProfileName, setNewProfileName] = useState("");

  // Edit profile form
  const [editName, setEditName] = useState("");
  const [editMask, setEditMask] = useState(0);
  const [editWatches, setEditWatches] = useState<Watchpoint[]>([]);

  // Add watchpoint fields
  const [watchStart, setWatchStart] = useState("");
  const [watchEnd, setWatchEnd] = useState("");
  const [watchLabel, setWatchLabel] = useState("");

  // Fetch telemetry runs
  const fetchTelemetry = async () => {
    setLoadingTelemetry(true);
    try {
      const res = await fetch("/api/recompiler/telemetry");
      if (res.ok) {
        const d = await res.json();
        const runs = d.telemetry || [];
        setTelemetry(runs);
        if (runs.length > 0 && !selectedRunId) {
          setSelectedRunId(runs[runs.length - 1].id);
        }
      }
    } catch (e) {
      console.error("Telemetry fetch error:", e);
    } finally {
      setLoadingTelemetry(false);
    }
  };

  // Trigger telemetry snapshot aggregation
  const handleSnapshotTelemetry = async () => {
    setLoadingTelemetry(true);
    try {
      const res = await fetch("/api/recompiler/telemetry", { method: "POST" });
      if (res.ok) {
        toast({
          title: "Telemetry Snapshot Logged",
          description: "All test and validation pipelines were successfully aggregated into dev.db.",
        });
        await fetchTelemetry();
      } else {
        toast({
          variant: "destructive",
          title: "Snapshot Failed",
          description: "Unable to aggregate telemetry pipelines.",
        });
      }
    } catch (err) {
      toast({
        variant: "destructive",
        title: "Network Error",
        description: String(err),
      });
    } finally {
      setLoadingTelemetry(false);
    }
  };

  // Fetch Debug Profiles
  const fetchProfiles = async () => {
    setLoadingProfiles(true);
    try {
      const res = await fetch("/api/recompiler/watchpoints/profiles");
      if (res.ok) {
        const d = await res.json();
        setProfiles(d.profiles || []);
      }
    } catch (e) {
      console.error("Profiles fetch error:", e);
    } finally {
      setLoadingProfiles(false);
    }
  };

  // Initialize profile form
  const handleSelectProfile = (p: DebugProfile) => {
    setSelectedProfileId(p.id);
    setEditName(p.name);
    setEditMask(p.debugMask);
    try {
      setEditWatches(JSON.parse(p.watchpoints));
    } catch {
      setEditWatches([]);
    }
    setWatchStart("");
    setWatchEnd("");
    setWatchLabel("");
  };

  // Create Profile
  const handleCreateProfile = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!newProfileName.trim()) return;
    try {
      const res = await fetch("/api/recompiler/watchpoints/profiles", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name: newProfileName }),
      });
      const d = await res.json();
      if (res.ok && d.success) {
        toast({ title: "Profile Created", description: `Created profile "${newProfileName}"` });
        setNewProfileName("");
        await fetchProfiles();
        handleSelectProfile(d.profile);
      } else {
        toast({ variant: "destructive", title: "Create Failed", description: d.message || "Duplicate profile name" });
      }
    } catch (err) {
      toast({ variant: "destructive", title: "Error", description: String(err) });
    }
  };

  // Save profile changes
  const handleSaveProfile = async () => {
    if (!selectedProfileId) return;
    try {
      const res = await fetch("/api/recompiler/watchpoints/profiles", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          id: selectedProfileId,
          name: editName,
          debugMask: editMask,
          watchpoints: editWatches,
        }),
      });
      const d = await res.json();
      if (res.ok && d.success) {
        toast({ title: "Profile Saved", description: "Changes synced to database." });
        await fetchProfiles();
      } else {
        toast({ variant: "destructive", title: "Save Failed", description: d.message });
      }
    } catch (err) {
      toast({ variant: "destructive", title: "Error", description: String(err) });
    }
  };

  // Make profile active
  const handleMakeActiveProfile = async (id: string) => {
    try {
      const res = await fetch("/api/recompiler/watchpoints/profiles", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ id, isActive: true }),
      });
      const d = await res.json();
      if (res.ok && d.success) {
        toast({
          title: "Profile Activated",
          description: "Watchpoints synced to watchpoints.json. Bitmask injected as environment payload.",
        });
        await fetchProfiles();
        // If we are currently editing the activated profile, sync its local state
        const updated = d.profile as DebugProfile;
        if (selectedProfileId === updated.id) {
          setEditMask(updated.debugMask);
        }
      }
    } catch (err) {
      toast({ variant: "destructive", title: "Activation Failed", description: String(err) });
    }
  };

  // Delete profile
  const handleDeleteProfile = async (id: string) => {
    try {
      const res = await fetch(`/api/recompiler/watchpoints/profiles?id=${id}`, { method: "DELETE" });
      if (res.ok) {
        toast({ title: "Profile Deleted", description: "The debugging profile was purged." });
        if (selectedProfileId === id) {
          setSelectedProfileId(null);
        }
        await fetchProfiles();
      }
    } catch (err) {
      toast({ variant: "destructive", title: "Delete Failed", description: String(err) });
    }
  };

  // Watchpoint management within profile
  const handleAddWatchpoint = () => {
    if (!watchStart || !watchEnd || !watchLabel) return;
    const startNum = watchStart.toLowerCase().startsWith("0x") ? parseInt(watchStart, 16) : parseInt(watchStart, 10);
    const endNum = watchEnd.toLowerCase().startsWith("0x") ? parseInt(watchEnd, 16) : parseInt(watchEnd, 10);

    if (isNaN(startNum) || isNaN(endNum)) {
      toast({ variant: "destructive", title: "Invalid Bounds", description: "Start and End must be valid integers/hex." });
      return;
    }

    if (editWatches.length >= 16) {
      toast({ variant: "destructive", title: "Limit Exceeded", description: "Maximum of 16 watchpoints allowed per profile." });
      return;
    }

    const newWatch: Watchpoint = { start: startNum, end: endNum, label: watchLabel };
    setEditWatches([...editWatches, newWatch]);
    setWatchStart("");
    setWatchEnd("");
    setWatchLabel("");
  };

  const handleRemoveWatchpoint = (idx: number) => {
    setEditWatches(editWatches.filter((_, i) => i !== idx));
  };

  // Toggle debug category mask bits
  const handleToggleCategory = (bit: number) => {
    setEditMask(prev => (prev & bit) ? (prev & ~bit) : (prev | bit));
  };

  // Load data on mount
  useEffect(() => {
    fetchTelemetry();
    fetchProfiles();
  }, []);

  // Compute active run details
  const selectedRun = useMemo(() => {
    return telemetry.find(r => r.id === selectedRunId) || null;
  }, [telemetry, selectedRunId]);

  const selectedRunMismatches = useMemo<SVMismatch[]>(() => {
    if (!selectedRun) return [];
    try {
      return JSON.parse(selectedRun.svMismatchesJson);
    } catch {
      return [];
    }
  }, [selectedRun]);

  // Compute graph data
  const chartData = useMemo(() => {
    return telemetry.map(t => {
      const d = new Date(t.timestamp);
      return {
        timestamp: d.toLocaleDateString(undefined, {
          month: "short",
          day: "numeric",
          hour: "2-digit",
          minute: "2-digit"
        }),
        "Recompile Completion %": t.completionPct,
        "Fuzz Pass Rate %": t.fuzzTotalTrials > 0 ? Math.round(((t.fuzzPassedTrials) / t.fuzzTotalTrials) * 10000) / 100 : 0.0,
        "Visual Regression Pass %": t.vrPassRate,
      };
    });
  }, [telemetry]);

  const activeProfile = profiles.find(p => p.isActive);

  return (
    <div className="space-y-4">
      <SectionHeader
        title="Local CI/CD Build Health Matrix"
        subtitle="Unified telemetry dashboard displaying differential fuzzer coverage, static analysis mismatches, and visual shader regressions."
        right={
          <div className="flex gap-2">
            <Button variant="outline" size="sm" className="h-8 gap-1.5" onClick={() => { fetchTelemetry(); fetchProfiles(); }}>
              <RefreshCw className="size-3.5" />
              Refresh
            </Button>
            <Button size="sm" className="h-8 gap-1.5 bg-primary hover:bg-primary/95 text-primary-foreground" onClick={handleSnapshotTelemetry}>
              <Activity className="size-3.5" />
              Record Telemetry Snapshot
            </Button>
          </div>
        }
      />

      {/* Tabs */}
      <div className="flex border-b border-border/60">
        <button
          onClick={() => setActiveTab("matrix")}
          className={`px-4 py-2 text-xs font-semibold uppercase tracking-wider border-b-2 -mb-px transition-colors ${
            activeTab === "matrix" ? "border-primary text-primary" : "border-transparent text-muted-foreground hover:text-foreground"
          }`}
        >
          Telemetry Matrix
        </button>
        <button
          onClick={() => setActiveTab("violations")}
          className={`px-4 py-2 text-xs font-semibold uppercase tracking-wider border-b-2 -mb-px transition-colors ${
            activeTab === "violations" ? "border-primary text-primary" : "border-transparent text-muted-foreground hover:text-foreground"
          }`}
        >
          Static Violations ({selectedRun ? selectedRunMismatches.length : 0})
        </button>
        <button
          onClick={() => setActiveTab("profiles")}
          className={`px-4 py-2 text-xs font-semibold uppercase tracking-wider border-b-2 -mb-px transition-colors ${
            activeTab === "profiles" ? "border-primary text-primary" : "border-transparent text-muted-foreground hover:text-foreground"
          }`}
        >
          Debug Profiles Manager
        </button>
      </div>

      {/* Active profile bar */}
      <div className="flex items-center justify-between rounded-lg border border-border/40 bg-card/15 p-2.5 px-4 text-xs">
        <div className="flex items-center gap-2">
          <Settings className="size-4 text-primary shrink-0" />
          <span className="text-muted-foreground">Active Debug Profile:</span>
          {activeProfile ? (
            <span className="font-semibold text-foreground bg-primary/10 border border-primary/20 rounded px-1.5 py-0.5">
              {activeProfile.name}
            </span>
          ) : (
            <span className="italic text-muted-foreground">None selected (Direct watchpoints.json loaded)</span>
          )}
        </div>
        {activeProfile && (
          <div className="flex items-center gap-4 text-[11px] text-muted-foreground font-mono">
            <span>SR_DEBUG: 0x{activeProfile.debugMask.toString(16).toUpperCase().padStart(2, "0")}</span>
            <span>Watches: {JSON.parse(activeProfile.watchpoints).length} / 16</span>
          </div>
        )}
      </div>

      {/* Matrix Tab */}
      {activeTab === "matrix" && (
        <div className="space-y-4">
          {/* Quick Metrics */}
          {selectedRun && (
            <div className="grid grid-cols-1 md:grid-cols-3 gap-4">
              <Panel className="space-y-2">
                <div className="flex items-center justify-between">
                  <span className="text-[11px] font-semibold text-muted-foreground uppercase">Recompile Pipeline</span>
                  <Activity className="size-4 text-cyan-400" />
                </div>
                <div className="flex items-baseline gap-2">
                  <span className="text-2xl font-bold tracking-tight">{selectedRun.completionPct}%</span>
                  <span className="text-[10px] text-muted-foreground font-mono">
                    {selectedRun.unitsEarned - selectedRun.unitsRegressed} / {selectedRun.totalUnits} units
                  </span>
                </div>
                <p className="text-[10px] text-muted-foreground">Compile-time structural lifting progress.</p>
              </Panel>

              <Panel className="space-y-2">
                <div className="flex items-center justify-between">
                  <span className="text-[11px] font-semibold text-muted-foreground uppercase">Differential Fuzzing</span>
                  <ShieldAlert className="size-4 text-amber-400" />
                </div>
                <div className="flex items-baseline gap-2">
                  <span className="text-2xl font-bold tracking-tight">
                    {selectedRun.fuzzTotalTrials > 0
                      ? `${Math.round((selectedRun.fuzzPassedTrials / selectedRun.fuzzTotalTrials) * 100)}%`
                      : "0%"}
                  </span>
                  <span className="text-[10px] text-muted-foreground font-mono">
                    {selectedRun.fuzzPassedTrials} / {selectedRun.fuzzTotalTrials} trials
                  </span>
                </div>
                <div className="flex justify-between items-center text-[10px] text-muted-foreground font-mono">
                  <span>Coverage: {selectedRun.fuzzCoveragePct.toFixed(1)}%</span>
                  <span>Fails: {selectedRun.fuzzFailedTrials}</span>
                </div>
              </Panel>

              <Panel className="space-y-2">
                <div className="flex items-center justify-between">
                  <span className="text-[11px] font-semibold text-muted-foreground uppercase">Visual Regression</span>
                  <CheckCircle2 className="size-4 text-emerald-400" />
                </div>
                <div className="flex items-baseline gap-2">
                  <span className="text-2xl font-bold tracking-tight">{selectedRun.vrPassRate}%</span>
                  <span className="text-[10px] text-muted-foreground font-mono">
                    {selectedRun.vrPassedFrames} / {selectedRun.vrTotalFrames} frames
                  </span>
                </div>
                <p className="text-[10px] text-muted-foreground">Identical matches against visual goldens.</p>
              </Panel>
            </div>
          )}

          {/* Chronological Graph */}
          <Panel className="p-4 space-y-3">
            <h3 className="text-xs font-semibold text-foreground uppercase tracking-wide">Orchestration Timeline Matrix</h3>
            <div className="h-[280px] w-full">
              {chartData.length > 0 ? (
                <ResponsiveContainer width="100%" height="100%">
                  <LineChart data={chartData} margin={{ top: 5, right: 10, left: -20, bottom: 5 }}>
                    <CartesianGrid strokeDasharray="3 3" stroke="#27272a" />
                    <XAxis dataKey="timestamp" stroke="#71717a" fontSize={9} />
                    <YAxis stroke="#71717a" fontSize={9} domain={[0, 100]} />
                    <Tooltip contentStyle={{ backgroundColor: "#09090b", borderColor: "#27272a" }} labelStyle={{ fontSize: "10px", fontWeight: "bold" }} itemStyle={{ fontSize: "11px" }} />
                    <Legend wrapperStyle={{ fontSize: "10px" }} />
                    <Line type="monotone" dataKey="Recompile Completion %" stroke="#22d3ee" strokeWidth={2} dot={{ r: 2 }} />
                    <Line type="monotone" dataKey="Fuzz Pass Rate %" stroke="#fbbf24" strokeWidth={2} dot={{ r: 2 }} />
                    <Line type="monotone" dataKey="Visual Regression Pass %" stroke="#10b981" strokeWidth={2} dot={{ r: 2 }} />
                  </LineChart>
                </ResponsiveContainer>
              ) : (
                <div className="flex h-full items-center justify-center text-xs text-muted-foreground italic">
                  No telemetry snapshots recorded in dev.db. Run your tests to gather data.
                </div>
              )}
            </div>
          </Panel>
        </div>
      )}

      {/* Violations Tab */}
      {activeTab === "violations" && (
        <div className="grid grid-cols-1 lg:grid-cols-[250px_minmax(0,1fr)] gap-4">
          {/* Timeline runs */}
          <div className="space-y-2">
            <h4 className="text-[10px] font-semibold text-muted-foreground uppercase tracking-wider">Runs with Mismatches</h4>
            <div className="space-y-1.5 max-h-[400px] overflow-y-auto thin-scroll border border-border/40 rounded-lg p-1.5 bg-card/10">
              {telemetry.filter(t => t.svMismatchesCount > 0).length === 0 ? (
                <p className="text-[10px] text-muted-foreground italic p-2 text-center">No structural mismatches detected.</p>
              ) : (
                telemetry.filter(t => t.svMismatchesCount > 0).map(t => {
                  const d = new Date(t.timestamp);
                  const active = t.id === selectedRunId;
                  return (
                    <button
                      key={t.id}
                      onClick={() => setSelectedRunId(t.id)}
                      className={`w-full text-left p-2 rounded text-xs transition-colors flex justify-between items-center ${
                        active ? "bg-rose-950/20 text-rose-400 border border-rose-900/40" : "hover:bg-accent/40 text-muted-foreground border border-transparent"
                      }`}
                    >
                      <span className="font-medium truncate">
                        {d.toLocaleTimeString()} - {d.toLocaleDateString(undefined, { month: "short", day: "numeric" })}
                      </span>
                      <span className="font-mono bg-rose-950/40 text-rose-500 rounded px-1 text-[9px] shrink-0 border border-rose-900/30">
                        {t.svMismatchesCount} err
                      </span>
                    </button>
                  );
                })
              )}
            </div>
          </div>

          {/* Mismatch inspector */}
          <div className="space-y-4">
            <div className="border-b border-border/60 pb-2">
              <h3 className="text-xs font-semibold text-foreground uppercase tracking-wide">Abstract Interpretation Verification</h3>
              <p className="text-[10px] text-muted-foreground mt-0.5">
                Every compilation block assert checks compile-time constants (ALU folds / materializations) against runtime register states.
              </p>
            </div>

            {selectedRunMismatches.length === 0 ? (
              <div className="rounded-lg border border-emerald-900/30 bg-emerald-950/10 p-8 text-center text-xs text-emerald-400 flex flex-col items-center justify-center gap-2">
                <CheckCircle2 className="size-6 text-emerald-500" />
                <div>
                  <span className="font-bold block">No Static Verification Violations</span>
                  <span className="text-muted-foreground text-[10px] block mt-0.5">All lifter register models match predicted lattice states.</span>
                </div>
              </div>
            ) : (
              <div className="space-y-3">
                {selectedRunMismatches.map((m, idx) => (
                  <div key={idx} className="bg-zinc-950/80 border border-zinc-800 rounded-lg p-3 font-mono text-xs leading-relaxed overflow-x-auto shadow-inner">
                    <div className="flex justify-between items-center pb-2 border-b border-zinc-900/60 mb-2 text-[10px] text-zinc-400">
                      <span className="flex items-center gap-1.5">
                        <AlertTriangle className="size-3 text-amber-500" />
                        PC: <span className="text-amber-500 font-bold">{m.pc}</span>
                      </span>
                      <span>
                        Register: <span className="text-sky-500 font-bold">{m.register}</span>
                      </span>
                    </div>
                    <div className="space-y-1">
                      <div className="flex items-center text-rose-400 bg-rose-950/20 px-2 py-0.5 rounded">
                        <span className="text-rose-500 font-semibold mr-3 w-28 shrink-0">- Expected (Lattice):</span>
                        <span className="font-bold">{m.expectedLatticeState}</span>
                      </div>
                      <div className="flex items-center text-emerald-400 bg-emerald-950/20 px-2 py-0.5 rounded">
                        <span className="text-emerald-500 font-semibold mr-3 w-28 shrink-0">+ Active (Host):    </span>
                        <span className="font-bold">{m.activeState}</span>
                      </div>
                    </div>
                  </div>
                ))}
              </div>
            )}
          </div>
        </div>
      )}

      {/* Profiles Tab */}
      {activeTab === "profiles" && (
        <div className="grid grid-cols-1 lg:grid-cols-[280px_minmax(0,1fr)] gap-4">
          {/* Profiles lists */}
          <div className="space-y-4">
            <Panel className="p-3 space-y-3">
              <h4 className="text-[10px] font-semibold text-muted-foreground uppercase tracking-wider">Create Debug Profile</h4>
              <form onSubmit={handleCreateProfile} className="flex gap-2">
                <Input
                  className="h-8 text-xs bg-background/50 border-border/60"
                  placeholder="Profile Name..."
                  value={newProfileName}
                  onChange={e => setNewProfileName(e.target.value)}
                />
                <Button type="submit" size="sm" className="h-8 px-2 shrink-0">
                  <Plus className="size-3.5" />
                </Button>
              </form>
            </Panel>

            <Panel className="p-3 space-y-2">
              <h4 className="text-[10px] font-semibold text-muted-foreground uppercase tracking-wider">Available Profiles</h4>
              {loadingProfiles ? (
                <p className="text-[10px] text-muted-foreground italic text-center p-2">Loading...</p>
              ) : profiles.length === 0 ? (
                <p className="text-[10px] text-muted-foreground italic text-center p-2">No custom profiles yet.</p>
              ) : (
                <div className="space-y-1.5 max-h-[300px] overflow-y-auto thin-scroll">
                  {profiles.map(p => {
                    const active = p.id === selectedProfileId;
                    return (
                      <div
                        key={p.id}
                        onClick={() => handleSelectProfile(p)}
                        className={`group w-full text-left p-2 rounded text-xs transition-colors flex justify-between items-center cursor-pointer border ${
                          active
                            ? "bg-primary/10 text-primary border-primary/40"
                            : "hover:bg-accent/40 text-muted-foreground border-transparent"
                        }`}
                      >
                        <span className="font-medium truncate flex-1">{p.name}</span>
                        <div className="flex items-center gap-1.5 opacity-80 group-hover:opacity-100 shrink-0">
                          {p.isActive ? (
                            <span className="bg-primary/20 text-primary rounded px-1 py-0.5 text-[8px] uppercase font-bold tracking-wider">
                              Active
                            </span>
                          ) : (
                            <button
                              onClick={(e) => {
                                e.stopPropagation();
                                handleMakeActiveProfile(p.id);
                              }}
                              className="text-[8px] uppercase tracking-wider bg-accent hover:bg-primary hover:text-primary-foreground text-muted-foreground font-bold px-1.5 py-0.5 rounded transition-colors"
                            >
                              Activate
                            </button>
                          )}
                          <button
                            onClick={(e) => {
                              e.stopPropagation();
                              handleDeleteProfile(p.id);
                            }}
                            className="text-muted-foreground hover:text-rose-500 p-0.5 rounded transition-colors"
                          >
                            <Trash2 className="size-3" />
                          </button>
                        </div>
                      </div>
                    );
                  })}
                </div>
              )}
            </Panel>
          </div>

          {/* Profile Editor Workspace */}
          <div>
            {!selectedProfileId ? (
              <div className="rounded-lg border border-dashed border-border/60 bg-card/5 p-12 text-center text-xs text-muted-foreground flex flex-col items-center justify-center gap-2">
                <Sliders className="size-6 text-muted-foreground/60" />
                <div>
                  <span className="font-bold block">No Profile Selected</span>
                  <span className="text-[10px] block mt-0.5">Select or create a debug profile to begin editing watchpoints.</span>
                </div>
              </div>
            ) : (
              <Panel className="p-4 space-y-4">
                <div className="flex justify-between items-center border-b border-border/60 pb-3">
                  <div className="space-y-1 flex-1 mr-4">
                    <Input
                      className="text-sm font-semibold h-8 bg-transparent border-0 hover:bg-accent/40 focus:bg-background px-1 focus:ring-1 focus:ring-primary w-full"
                      value={editName}
                      onChange={e => setEditName(e.target.value)}
                    />
                    <p className="text-[10px] text-muted-foreground leading-normal">
                      Configure debugger target environment bitmask categories and address ranges.
                    </p>
                  </div>
                  <Button size="sm" className="h-8 gap-1.5 shrink-0" onClick={handleSaveProfile}>
                    <Save className="size-3.5" />
                    Save Profile
                  </Button>
                </div>

                {/* Sub-panels */}
                <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
                  {/* Category Bitmask Panel */}
                  <div className="space-y-2">
                    <div className="flex justify-between items-center">
                      <h4 className="text-[10px] font-semibold text-muted-foreground uppercase tracking-wider">Debug Categories (SR_DEBUG)</h4>
                      <span className="text-[11px] font-mono text-amber-500 font-bold">
                        Payload: 0x{editMask.toString(16).toUpperCase().padStart(2, "0")}
                      </span>
                    </div>

                    <div className="space-y-1.5 rounded-lg border border-border/60 bg-card/10 p-2.5">
                      {DEBUG_CATEGORIES.map(cat => {
                        const checked = (editMask & cat.bit) !== 0;
                        return (
                          <div
                            key={cat.bit}
                            onClick={() => handleToggleCategory(cat.bit)}
                            className={`flex items-center justify-between p-1.5 rounded cursor-pointer transition-colors text-xs ${
                              checked ? "bg-primary/5 hover:bg-primary/10" : "hover:bg-accent/30 text-muted-foreground"
                            }`}
                          >
                            <div className="space-y-0.5">
                              <span className={`font-semibold ${checked ? "text-primary" : "text-foreground"}`}>
                                {cat.label}
                              </span>
                              <span className="block text-[9px] text-muted-foreground/80 leading-tight">
                                {cat.desc}
                              </span>
                            </div>
                            <div className={`size-3.5 rounded border flex items-center justify-center transition-colors shrink-0 ${
                              checked ? "bg-primary border-primary text-primary-foreground" : "border-border/60"
                            }`}>
                              {checked && <Check className="size-2.5 stroke-[3]" />}
                            </div>
                          </div>
                        );
                      })}
                    </div>
                  </div>

                  {/* Watchpoints List Panel */}
                  <div className="space-y-2">
                    <div className="flex justify-between items-center">
                      <h4 className="text-[10px] font-semibold text-muted-foreground uppercase tracking-wider">Memory Watchpoints</h4>
                      <span className="text-[10px] font-mono text-muted-foreground">
                        {editWatches.length} / 16 active
                      </span>
                    </div>

                    {/* Add watchpoint form */}
                    <div className="flex flex-col gap-1.5 rounded-lg border border-border/60 bg-card/10 p-2.5">
                      <div className="grid grid-cols-2 gap-1.5">
                        <Input
                          className="h-7 text-[10px] bg-background/50"
                          placeholder="Start (e.g. 0x08001000)"
                          value={watchStart}
                          onChange={e => setWatchStart(e.target.value)}
                        />
                        <Input
                          className="h-7 text-[10px] bg-background/50"
                          placeholder="End (e.g. 0x08001010)"
                          value={watchEnd}
                          onChange={e => setWatchEnd(e.target.value)}
                        />
                      </div>
                      <div className="flex gap-2">
                        <Input
                          className="h-7 text-[10px] bg-background/50 flex-1"
                          placeholder="Label (e.g. Font Engine)"
                          value={watchLabel}
                          onChange={e => setWatchLabel(e.target.value)}
                        />
                        <Button type="button" size="sm" className="h-7 text-[10px] px-2 shrink-0 bg-primary/20 hover:bg-primary/30 text-primary border border-primary/30" onClick={handleAddWatchpoint}>
                          Add Watch
                        </Button>
                      </div>
                    </div>

                    {/* Watchpoints list scroll */}
                    <div className="space-y-1.5 max-h-[190px] overflow-y-auto thin-scroll border border-border/40 rounded-lg p-1.5 bg-card/5">
                      {editWatches.length === 0 ? (
                        <p className="text-[10px] text-muted-foreground italic p-4 text-center">No active watchpoints mapped.</p>
                      ) : (
                        editWatches.map((w, idx) => (
                          <div key={idx} className="flex justify-between items-center p-1.5 rounded border border-border/30 bg-card/20 text-[10px] font-mono">
                            <div className="truncate flex-1 pr-2">
                              <span className="font-semibold text-foreground bg-accent/40 px-1 py-0.5 rounded mr-1.5 font-sans">
                                {w.label}
                              </span>
                              <span className="text-muted-foreground">
                                0x{w.start.toString(16).toUpperCase()}..0x{w.end.toString(16).toUpperCase()}
                              </span>
                            </div>
                            <button
                              type="button"
                              onClick={() => handleRemoveWatchpoint(idx)}
                              className="text-muted-foreground hover:text-rose-500 shrink-0"
                            >
                              <Trash2 className="size-3.5" />
                            </button>
                          </div>
                        ))
                      )}
                    </div>
                  </div>
                </div>
              </Panel>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
