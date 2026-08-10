"use client";

import React, { useState, useEffect } from "react";
import {
  Play,
  Square,
  Download,
  Trash2,
  Plus,
  Code,
  ShieldAlert,
  CheckCircle2,
  XCircle,
  Settings2,
  ListFilter,
  Activity
} from "lucide-react";
import { AreaChart, Area, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from "recharts";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Panel } from "./ui-bits";
import { useToast } from "@/hooks/use-toast";
import { ExecutionConsole } from "./execution-console";
import { NidAuditPanel } from "./nid-audit-panel";

interface Watchpoint {
  start: number;
  end: number;
  label: string;
}

interface AlertHit {
  id: string;
  label: string;
  type: string;
  addr: string;
  val: string;
  pc: string;
  timestamp: string;
}

interface FuzzProgressData {
  caseIdx: number;
  passed: number;
  failed: number;
  total: number;
  op: string;
}

export function TestLabPanel() {
  const { toast } = useToast();
  const [activeTab, setActiveTab] = useState<"console" | "nid" | "fuzz">("console");

  // Microtest state
  const [selectedGroups, setSelectedGroups] = useState<string[]>(["integer", "fpu"]);
  const [customOpcodes, setCustomOpcodes] = useState("");
  const [extraOps, setExtraOps] = useState(6);
  const [generatedCode, setGeneratedCode] = useState<string | null>(null);
  const [genLoading, setGenLoading] = useState(false);

  // Fuzzer state
  const [fuzzTrials, setFuzzTrials] = useState(200);
  const [fuzzSeed, setFuzzSeed] = useState("0x12345678");
  const [fuzzConstraint, setFuzzConstraint] = useState("none");
  const [fuzzRunning, setFuzzRunning] = useState(false);
  const [fuzzData, setFuzzData] = useState<FuzzProgressData[]>([]);
  const [fuzzSummary, setFuzzSummary] = useState<string | null>(null);

  // Watchpoints state
  const [watchpoints, setWatchpoints] = useState<Watchpoint[]>([]);
  const [watchLabel, setWatchLabel] = useState("");
  const [watchStart, setWatchStart] = useState("");
  const [watchEnd, setWatchEnd] = useState("");

  // Alerts state - lazily initialized from localStorage to avoid calling setState inside useEffect
  const [alerts, setAlerts] = useState<AlertHit[]>(() => {
    if (typeof window !== "undefined") {
      const storedAlerts = localStorage.getItem("hst_watchpoint_alerts");
      if (storedAlerts) {
        try {
          return JSON.parse(storedAlerts);
        } catch (e) {
          localStorage.removeItem("hst_watchpoint_alerts");
        }
      }
    }
    return [];
  });

  // Export state
  const [exporting, setExporting] = useState(false);

  // EventSource reference
  const eventSourceRef = React.useRef<EventSource | null>(null);

  // Helper functions declared before useEffect to satisfy hoisting and lint rules
  const fetchWatchpoints = React.useCallback(async () => {
    try {
      const r = await fetch("/api/recompiler/watchpoints");
      const d = await r.json();
      if (d.success) setWatchpoints(d.watchpoints);
    } catch {}
  }, []);

  const connectFuzzStream = React.useCallback(() => {
    if (eventSourceRef.current) return;

    const es = new EventSource("/api/recompiler/manager");
    eventSourceRef.current = es;

    es.addEventListener("stdout", (event) => {
      const data = JSON.parse(event.data);
      const lines = data.text.split(/\r?\n/).filter(Boolean);

      lines.forEach((l: string) => {
        if (l.startsWith("FUZZ_PROGRESS")) {
          const match = l.match(/FUZZ_PROGRESS case=(\d+) total=(\d+) passed=(\d+) failed=(\d+) op=(0x[0-9a-fA-F]+)/);
          if (match) {
            const [_, caseIdx, total, passed, failed, op] = match;
            setFuzzData(prev => {
              if (prev.some(d => d.caseIdx === parseInt(caseIdx))) return prev;
              return [
                ...prev,
                {
                  caseIdx: parseInt(caseIdx),
                  passed: parseInt(passed),
                  failed: parseInt(failed),
                  total: parseInt(total),
                  op,
                }
              ];
            });
          }
        } else if (l.includes("vfpu_fuzz:")) {
          setFuzzSummary(l);
        }
      });
    });

    es.addEventListener("close", () => {
      setFuzzRunning(false);
      es.close();
      eventSourceRef.current = null;
    });

    es.addEventListener("error", () => {
      setFuzzRunning(false);
      es.close();
      eventSourceRef.current = null;
    });
  }, []);

  const checkFuzzerStatus = React.useCallback(async () => {
    try {
      const r = await fetch("/api/recompiler/tests/fuzz");
      const d = await r.json();
      if (d.isRunning) {
        setFuzzRunning(true);
        connectFuzzStream();
      } else if (fuzzRunning) {
        setFuzzRunning(false);
      }
    } catch {}
  }, [fuzzRunning, connectFuzzStream]);

  // Load watchpoints and alerts at mount
  useEffect(() => {
    setTimeout(() => {
      fetchWatchpoints();
      checkFuzzerStatus();
    }, 0);

    // Poll fuzzer status while running
    let interval: ReturnType<typeof setInterval>;
    if (fuzzRunning) {
      interval = setInterval(checkFuzzerStatus, 1500);
    }
    return () => clearInterval(interval);
  }, [fuzzRunning, fetchWatchpoints, checkFuzzerStatus]);

  // Listen to storage events to keep watchpoint hits synchronised in real time
  useEffect(() => {
    const handleStorageChange = () => {
      const storedAlerts = localStorage.getItem("hst_watchpoint_alerts");
      if (storedAlerts) {
        try {
          setAlerts(JSON.parse(storedAlerts));
        } catch (e) {}
      } else {
        setAlerts([]);
      }
    };
    window.addEventListener("storage", handleStorageChange);
    return () => window.removeEventListener("storage", handleStorageChange);
  }, []);

  // Microtest generation trigger
  const handleGenerateMicrotest = async () => {
    setGenLoading(true);
    setGeneratedCode(null);
    try {
      const res = await fetch("/api/recompiler/tests/microtest", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          extra: extraOps,
          groups: customOpcodes ? [] : selectedGroups,
          opcodes: customOpcodes ? customOpcodes.split(",").map(o => o.trim()) : [],
        }),
      });
      const d = await res.json();
      if (res.ok && d.success) {
        setGeneratedCode(d.code);
        toast({
          title: "Microtest Generated",
          description: `Wrote test code to ${d.destFile.split(/[\\/]/).pop()}`,
        });
      } else {
        toast({
          variant: "destructive",
          title: "Generation Failed",
          description: d.detail || "Unable to generate microtest module",
        });
      }
    } catch (e) {
      toast({
        variant: "destructive",
        title: "Network Error",
        description: String(e),
      });
    } finally {
      setGenLoading(false);
    }
  };

  // Start fuzz run
  const handleStartFuzzer = async () => {
    setFuzzSummary(null);
    setFuzzData([]);
    setFuzzRunning(true);
    try {
      const res = await fetch("/api/recompiler/tests/fuzz", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          trials: fuzzTrials,
          seed: fuzzSeed,
          constraint: fuzzConstraint,
        }),
      });
      const d = await res.json();
      if (!res.ok) {
        throw new Error(d.message || "Failed to start fuzzer");
      }
      connectFuzzStream();
    } catch (e) {
      setFuzzRunning(false);
      toast({
        variant: "destructive",
        title: "Fuzzer Error",
        description: String(e),
      });
    }
  };

  const handleStopFuzzer = async () => {
    try {
      await fetch("/api/recompiler/manager", { method: "DELETE" });
      setFuzzRunning(false);
      toast({
        title: "Fuzzer Stopped",
        description: "Fuzzer process was killed by user request.",
      });
    } catch {}
  };

  // Watchpoint CRUD
  const handleAddWatchpoint = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!watchLabel || !watchStart || !watchEnd) return;
    try {
      const res = await fetch("/api/recompiler/watchpoints", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          start: watchStart,
          end: watchEnd,
          label: watchLabel,
        }),
      });
      const d = await res.json();
      if (res.ok && d.success) {
        setWatchpoints(d.watchpoints);
        setWatchLabel("");
        setWatchStart("");
        setWatchEnd("");
        // #188: the DB is canonical; if the derived runtime artifact could not
        // be published, say so — never show a state the runtime cannot consume.
        if (d.fileState && d.fileState.ok === false) {
          toast({
            variant: "destructive",
            title: "Runtime Artifact Not Updated",
            description: `watchpoints.json: ${d.fileState.detail}. The runtime may still use the previous set.`,
          });
        } else {
          toast({ title: "Watchpoint Added", description: `Active range registered: ${watchLabel}` });
        }
      } else {
        toast({ variant: "destructive", title: "Failed to Add", description: d.message });
      }
    } catch (e) {
      toast({ variant: "destructive", title: "Error", description: String(e) });
    }
  };

  const handleDeleteWatchpoint = async (start: number) => {
    try {
      const res = await fetch(`/api/recompiler/watchpoints?start=0x${start.toString(16)}`, {
        method: "DELETE",
      });
      const d = await res.json();
      if (res.ok && d.success) {
        setWatchpoints(d.watchpoints);
        if (d.fileState && d.fileState.ok === false) {
          toast({
            variant: "destructive",
            title: "Runtime Artifact Not Updated",
            description: `watchpoints.json: ${d.fileState.detail}.`,
          });
        } else {
          toast({ title: "Watchpoint Cleared" });
        }
      }
    } catch {}
  };

  const handleClearAllWatchpoints = async () => {
    try {
      const res = await fetch("/api/recompiler/watchpoints", { method: "DELETE" });
      const d = await res.json();
      if (res.ok && d.success) {
        setWatchpoints([]);
        if (d.fileState && d.fileState.ok === false) {
          toast({
            variant: "destructive",
            title: "Runtime Artifact Not Updated",
            description: `watchpoints.json: ${d.fileState.detail}.`,
          });
        } else {
          toast({ title: "All Watchpoints Cleared" });
        }
      }
    } catch {}
  };

  const handleClearAlerts = () => {
    setAlerts([]);
    localStorage.removeItem("hst_watchpoint_alerts");
  };

  // Export test suite bundle
  const handleExportSuite = async () => {
    setExporting(true);
    try {
      const res = await fetch("/api/recompiler/telemetry/export");
      if (!res.ok) throw new Error("Export download failed");

      const blob = await res.blob();
      const url = window.URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = "hst-test-suite-export.zip";
      document.body.appendChild(a);
      a.click();
      window.URL.revokeObjectURL(url);
      document.body.removeChild(a);

      toast({
        title: "Test Suite Exported",
        description: "Aggregated bundle downloaded successfully.",
      });
    } catch (e) {
      toast({
        variant: "destructive",
        title: "Export Failed",
        description: String(e),
      });
    } finally {
      setExporting(false);
    }
  };

  const toggleGroup = (g: string) => {
    setSelectedGroups(prev =>
      prev.includes(g) ? prev.filter(x => x !== g) : [...prev, g]
    );
  };

  const toHex = (n: number) => `0x${n.toString(16).toUpperCase()}`;

  return (
    <div className="space-y-4">
      {/* Premium Sliding Tab Selector */}
      <div className="flex border-b border-border/40 pb-px gap-2 overflow-x-auto thin-scroll">
        <button
          onClick={() => setActiveTab("console")}
          className={`px-4 py-2 text-xs font-mono font-bold border-b-2 transition-colors shrink-0 ${
            activeTab === "console"
              ? "border-primary text-primary"
              : "border-transparent text-muted-foreground hover:text-foreground"
          }`}
        >
          INTERACTIVE CONSOLE (REPL)
        </button>
        <button
          onClick={() => setActiveTab("nid")}
          className={`px-4 py-2 text-xs font-mono font-bold border-b-2 transition-colors shrink-0 ${
            activeTab === "nid"
              ? "border-primary text-primary"
              : "border-transparent text-muted-foreground hover:text-foreground"
          }`}
        >
          NID COMPLIANCE AUDITOR
        </button>
        <button
          onClick={() => setActiveTab("fuzz")}
          className={`px-4 py-2 text-xs font-mono font-bold border-b-2 transition-colors shrink-0 ${
            activeTab === "fuzz"
              ? "border-primary text-primary"
              : "border-transparent text-muted-foreground hover:text-foreground"
          }`}
        >
          TEST ASSEMBLER & FUZZER
        </button>
      </div>

      {activeTab === "console" && <ExecutionConsole />}
      {activeTab === "nid" && <NidAuditPanel />}
      {activeTab === "fuzz" && (
        <div className="grid grid-cols-1 xl:grid-cols-2 gap-4">
          {/* LEFT COLUMN: Test Lab Suite & Fuzzer */}
      <div className="space-y-4">
        {/* Microtest Lab */}
        <Panel title="Microtest Assembler Lab" icon={<Code className="size-4" />}>
          <div className="space-y-3">
            <p className="text-xs text-muted-foreground">
              Select target MIPS ISA instruction groups or input raw hex opcode words.
              On-demand compiles a custom Allegrex static test module.
            </p>

            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="block text-[10px] uppercase font-semibold text-muted-foreground mb-1.5">
                  ISA Instruction Groups
                </label>
                <div className="flex flex-col gap-1.5 p-2 rounded-lg border border-border/40 bg-black/20">
                  {["integer", "fpu", "vfpu"].map((g) => (
                    <label key={g} className="flex items-center gap-2 text-xs font-mono cursor-pointer capitalize">
                      <input
                        type="checkbox"
                        checked={selectedGroups.includes(g)}
                        onChange={() => toggleGroup(g)}
                        disabled={!!customOpcodes}
                        className="rounded border-border bg-background text-primary focus:ring-primary size-3.5"
                      />
                      {g}
                    </label>
                  ))}
                </div>
              </div>

              <div>
                <label className="block text-[10px] uppercase font-semibold text-muted-foreground mb-1.5">
                  Seeded Operands Count
                </label>
                <Input
                  type="number"
                  value={extraOps}
                  onChange={(e) => setExtraOps(parseInt(e.target.value) || 0)}
                  disabled={!!customOpcodes}
                  className="h-8 text-xs font-mono bg-black/20"
                  min="0"
                  max="100"
                />

                <label className="block text-[10px] uppercase font-semibold text-muted-foreground mt-2 mb-1.5">
                  Explicit Hex Opcodes (Comma-separated)
                </label>
                <Input
                  placeholder="e.g. 0x00000000, 0x014B4820"
                  value={customOpcodes}
                  onChange={(e) => setCustomOpcodes(e.target.value)}
                  className="h-8 text-xs font-mono bg-black/20"
                />
              </div>
            </div>

            <Button
              onClick={handleGenerateMicrotest}
              disabled={genLoading || (!customOpcodes && selectedGroups.length === 0)}
              className="w-full h-8 text-xs"
            >
              <Code className="size-3.5 mr-1.5" />
              {genLoading ? "Generating..." : "Assemble Test Module"}
            </Button>

            {generatedCode && (
              <div className="rounded-lg border border-border/60 bg-black/60 p-2.5 max-h-48 overflow-y-auto thin-scroll">
                <pre className="font-mono text-[10px] leading-relaxed text-emerald-400 select-all whitespace-pre">
                  {generatedCode}
                </pre>
              </div>
            )}
          </div>
        </Panel>

        {/* VFPU Fuzzer */}
        <Panel title="VFPU Differential Fuzzer Orchestration" icon={<Activity className="size-4" />}>
          <div className="space-y-4">
            <div className="grid grid-cols-3 gap-2">
              <div>
                <label className="block text-[10px] uppercase font-semibold text-muted-foreground mb-1">
                  Trials per opcode
                </label>
                <Input
                  type="number"
                  value={fuzzTrials}
                  onChange={(e) => setFuzzTrials(parseInt(e.target.value) || 200)}
                  disabled={fuzzRunning}
                  className="h-8 text-xs font-mono bg-black/20"
                />
              </div>

              <div>
                <label className="block text-[10px] uppercase font-semibold text-muted-foreground mb-1">
                  RNG Seed
                </label>
                <Input
                  value={fuzzSeed}
                  onChange={(e) => setFuzzSeed(e.target.value)}
                  disabled={fuzzRunning}
                  className="h-8 text-xs font-mono bg-black/20"
                />
              </div>

              <div>
                <label className="block text-[10px] uppercase font-semibold text-muted-foreground mb-1">
                  Operand Bounds
                </label>
                <select
                  value={fuzzConstraint}
                  onChange={(e) => setFuzzConstraint(e.target.value)}
                  disabled={fuzzRunning}
                  className="h-8 w-full text-xs font-mono rounded-md border border-input bg-black/20 px-2 py-1"
                >
                  <option value="none">none (standard)</option>
                  <option value="allow_nan_inf">allow nan / inf</option>
                  <option value="positive">positive only</option>
                  <option value="no_zero">non-zero only</option>
                </select>
              </div>
            </div>

            <div className="flex gap-2">
              <Button
                onClick={handleStartFuzzer}
                disabled={fuzzRunning}
                className="flex-1 h-8 text-xs"
                variant="default"
              >
                <Play className="size-3.5 mr-1.5" />
                Start Differential Loop
              </Button>

              {fuzzRunning && (
                <Button
                  onClick={handleStopFuzzer}
                  className="h-8 text-xs"
                  variant="destructive"
                >
                  <Square className="size-3.5 mr-1.5" />
                  Abort
                </Button>
              )}
            </div>

            {/* Trial progress curves */}
            {fuzzData.length > 0 && (
              <div className="space-y-2">
                <div className="flex items-center justify-between">
                  <span className="text-[10px] font-semibold uppercase text-muted-foreground">
                    Real-time Trial success curve
                  </span>
                  <span className="text-[10px] font-mono text-primary animate-pulse">
                    Streaming: case {fuzzData[fuzzData.length - 1].caseIdx + 1} ({toHex(parseInt(fuzzData[fuzzData.length - 1].op))})
                  </span>
                </div>

                <div className="h-40 w-full bg-black/20 rounded-lg p-1.5 border border-border/30">
                  <ResponsiveContainer width="100%" height="100%">
                    <AreaChart data={fuzzData}>
                      <defs>
                        <linearGradient id="passGrad" x1="0" y1="0" x2="0" y2="1">
                          <stop offset="5%" stopColor="#10b981" stopOpacity={0.2}/>
                          <stop offset="95%" stopColor="#10b981" stopOpacity={0}/>
                        </linearGradient>
                        <linearGradient id="failGrad" x1="0" y1="0" x2="0" y2="1">
                          <stop offset="5%" stopColor="#ef4444" stopOpacity={0.2}/>
                          <stop offset="95%" stopColor="#ef4444" stopOpacity={0}/>
                        </linearGradient>
                      </defs>
                      <CartesianGrid strokeDasharray="3 3" stroke="#2a2a2a" />
                      <XAxis dataKey="caseIdx" stroke="#525252" fontSize={8} />
                      <YAxis stroke="#525252" fontSize={8} />
                      <Tooltip
                        contentStyle={{ backgroundColor: "#171717", borderColor: "#2e2e2e", fontSize: "10px" }}
                        labelFormatter={(label) => `Case #${label}`}
                      />
                      <Area type="monotone" dataKey="passed" stroke="#10b981" fillOpacity={1} fill="url(#passGrad)" name="Passed" />
                      <Area type="monotone" dataKey="failed" stroke="#ef4444" fillOpacity={1} fill="url(#failGrad)" name="Failed" />
                    </AreaChart>
                  </ResponsiveContainer>
                </div>
              </div>
            )}

            {fuzzSummary && (
              <div className="p-2 bg-black/40 rounded-lg border border-border/40 font-mono text-[10px] flex items-center gap-2">
                {fuzzSummary.includes("0 words diverge") ? (
                  <CheckCircle2 className="size-3.5 text-emerald-400 shrink-0" />
                ) : (
                  <XCircle className="size-3.5 text-destructive shrink-0" />
                )}
                <span className={fuzzSummary.includes("0 words diverge") ? "text-emerald-400" : "text-destructive"}>
                  {fuzzSummary}
                </span>
              </div>
            )}
          </div>
        </Panel>
      </div>

      {/* RIGHT COLUMN: Memory Watchpoint Injection Layer */}
      <div className="space-y-4">
        {/* Watchpoint manager */}
        <Panel title="Live Memory Watchpoint manager" icon={<Settings2 className="size-4" />}>
          <div className="space-y-3">
            <p className="text-xs text-muted-foreground">
              Inject guest memory address ranges to monitor reads and writes. Watchpoints serialize to
              <code className="bg-black/40 px-1 rounded mx-1 text-primary">watchpoints.json</code> and feed to the emulator.
            </p>

            <form onSubmit={handleAddWatchpoint} className="grid grid-cols-3 gap-2">
              <div>
                <label className="block text-[10px] uppercase font-semibold text-muted-foreground mb-1">
                  Name / label
                </label>
                <Input
                  required
                  placeholder="AssetBucket"
                  value={watchLabel}
                  onChange={(e) => setWatchLabel(e.target.value)}
                  className="h-8 text-xs font-mono bg-black/20"
                />
              </div>

              <div>
                <label className="block text-[10px] uppercase font-semibold text-muted-foreground mb-1">
                  Start address
                </label>
                <Input
                  required
                  placeholder="0x08800000"
                  value={watchStart}
                  onChange={(e) => setWatchStart(e.target.value)}
                  className="h-8 text-xs font-mono bg-black/20"
                />
              </div>

              <div>
                <label className="block text-[10px] uppercase font-semibold text-muted-foreground mb-1">
                  End address
                </label>
                <div className="flex gap-1.5">
                  <Input
                    required
                    placeholder="0x08801000"
                    value={watchEnd}
                    onChange={(e) => setWatchEnd(e.target.value)}
                    className="h-8 text-xs font-mono bg-black/20 flex-1"
                  />
                  <Button type="submit" size="icon" className="h-8 w-8 shrink-0">
                    <Plus className="size-4" />
                  </Button>
                </div>
              </div>
            </form>

            <div className="space-y-2">
              <div className="flex items-center justify-between pt-1">
                <span className="text-[10px] font-semibold uppercase text-muted-foreground">
                  Active watchpoints ({watchpoints.length} / 16)
                </span>
                {watchpoints.length > 0 && (
                  <button
                    onClick={handleClearAllWatchpoints}
                    className="text-[10px] text-destructive hover:underline inline-flex items-center gap-1"
                  >
                    <Trash2 className="size-3" /> Clear all
                  </button>
                )}
              </div>

              <div className="rounded-lg border border-border/40 bg-black/20 overflow-hidden text-xs">
                {watchpoints.length === 0 ? (
                  <div className="p-4 text-center text-muted-foreground/60 italic font-mono text-[11px]">
                    No active watches registered.
                  </div>
                ) : (
                  <div className="max-h-36 overflow-y-auto thin-scroll">
                    <table className="w-full text-left font-mono text-[10px]">
                      <thead>
                        <tr className="border-b border-border/30 bg-black/35 text-muted-foreground">
                          <th className="px-2 py-1.5 font-semibold">label</th>
                          <th className="px-2 py-1.5 font-semibold">Start</th>
                          <th className="px-2 py-1.5 font-semibold">End</th>
                          <th className="px-2 py-1.5 w-8"></th>
                        </tr>
                      </thead>
                      <tbody>
                        {watchpoints.map((w, idx) => (
                          <tr key={idx} className="border-b border-border/20 hover:bg-black/10">
                            <td className="px-2 py-1.5 text-primary font-semibold">{w.label}</td>
                            <td className="px-2 py-1.5">{toHex(w.start)}</td>
                            <td className="px-2 py-1.5">{toHex(w.end)}</td>
                            <td className="px-2 py-1.5 text-right">
                              <button
                                onClick={() => handleDeleteWatchpoint(w.start)}
                                className="text-muted-foreground hover:text-destructive transition-colors"
                              >
                                <Trash2 className="size-3.5" />
                              </button>
                            </td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                )}
              </div>
            </div>
          </div>
        </Panel>

        {/* Watchpoint Pinned Alerts */}
        <Panel title="Pinned Watchpoint Alerts" icon={<ShieldAlert className="size-4" />}>
          <div className="space-y-2">
            <div className="flex items-center justify-between">
              <span className="text-[10px] font-semibold uppercase text-muted-foreground">
                Divergent read/write hits ({alerts.length})
              </span>
              {alerts.length > 0 && (
                <button
                  onClick={handleClearAlerts}
                  className="text-[10px] text-destructive hover:underline inline-flex items-center gap-1"
                >
                  <Trash2 className="size-3" /> Clear log
                </button>
              )}
            </div>

            <div className="rounded-lg border border-border/40 bg-black/20 overflow-hidden text-xs">
              {alerts.length === 0 ? (
                <div className="p-8 text-center text-muted-foreground/60 italic font-mono text-[11px]">
                  No hits logged. Launch emulator with watches configured.
                </div>
              ) : (
                <div className="max-h-48 overflow-y-auto thin-scroll space-y-1 p-2">
                  {alerts.slice(-40).map((a) => (
                    <div
                      key={a.id}
                      className={`p-2 rounded border font-mono text-[10px] flex justify-between items-center ${
                        a.type === "WRITE"
                          ? "bg-rose-950/20 border-rose-800/40 text-rose-300"
                          : "bg-amber-950/20 border-amber-800/40 text-amber-300"
                      }`}
                    >
                      <div className="space-y-0.5">
                        <div className="flex items-center gap-1.5">
                          <span className="font-bold uppercase tracking-wider text-[9px] bg-black/40 px-1 rounded">
                            {a.type}
                          </span>
                          <span className="font-semibold text-foreground">{a.label}</span>
                        </div>
                        <div>
                          Addr: <span className="font-semibold">{a.addr}</span> ·
                          val: <span className="font-semibold">{a.val}</span> ·
                          PC: <span className="font-semibold">{a.pc}</span>
                        </div>
                      </div>
                      <span className="text-[9px] text-muted-foreground/80 shrink-0 select-none">
                        {a.timestamp}
                      </span>
                    </div>
                  ))}
                </div>
              )}
            </div>
          </div>
        </Panel>

        {/* Test Suite Export Engine */}
        <Panel title="Test Suite Export Engine" icon={<ListFilter className="size-4" />}>
          <div className="space-y-3">
            <p className="text-xs text-muted-foreground">
              Bundle the static SQLite DB, chronological Recharts telemetry runs, GOLDEN visual regression diff states,
              and extracted inventories mapping tree into a single structured report for offline validation.
            </p>

            <Button
              onClick={handleExportSuite}
              disabled={exporting}
              className="w-full h-8 text-xs font-semibold"
              variant="outline"
            >
              <Download className="size-3.5 mr-1.5" />
              {exporting ? "Generating ZIP Archive..." : "Export Validation Suite (.zip)"}
            </Button>
          </div>
        </Panel>
      </div>
    </div>
  )}
</div>
);
}
