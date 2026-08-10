"use client";

import { useEffect, useState, useRef } from "react";
import { Cpu, Wrench, GitBranch, Table, Terminal, Play, Pause, Trash2, FileText, AlertTriangle, ShieldAlert } from "lucide-react";
import { Panel, SectionHeader } from "./ui-bits";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import {
  PIPELINE_STAGES,
  RUNTIME_SUBSYSTEMS,
  LOOP_CAPS,
  FUNCTION_MAP,
  THREAD_MAP,
} from "@/lib/recompiler/real-data";
import { cn } from "@/lib/utils";

const STATUS_STYLES: Record<string, string> = {
  complete: "border-emerald-500/30 text-emerald-300 bg-emerald-500/10",
  "in-progress": "border-amber-500/30 text-amber-300 bg-amber-500/10",
  partial: "border-sky-500/30 text-sky-300 bg-sky-500/10",
};

interface CrashReport {
  exception?: string;
  fault?: string;
  hostRegs: Record<string, string>;
  guestRegs: Record<string, string>;
}

export function InternalsPanel() {
  const [activeTab, setActiveTab] = useState<"logs" | "registers">("logs");

  // Log Streamer States
  const [logs, setLogs] = useState<string[]>([]);
  const [isPlaying, setIsPlaying] = useState(true);
  const [cursor, setCursor] = useState(0);
  const [searchQuery, setSearchQuery] = useState("");
  const [selectedSubsystems, setSelectedSubsystems] = useState<string[]>([]);
  const [crashReport, setCrashReport] = useState<CrashReport | null>(null);

  const scrollRef = useRef<HTMLDivElement>(null);
  const isPlayingRef = useRef(isPlaying);
  isPlayingRef.current = isPlaying;

  const subsystems = ["MEM", "HLE", "SCHED", "GE", "INPUT", "FS", "VIDEO", "MISC"];

  // SSE Logs Streaming
  useEffect(() => {
    if (!isPlaying) return;

    const es = new EventSource("/api/recompiler/manager");

    const appendLogLines = (text: string) => {
      const newLines = text.split(/\r?\n/).filter(Boolean);
      if (newLines.length === 0) return;
      setLogs((prev) => {
        const nextLogs = [...prev, ...newLines];
        if (nextLogs.length > 5000) {
          nextLogs.splice(0, nextLogs.length - 5000);
        }
        detectCrashReports(nextLogs);
        return nextLogs;
      });
    };

    es.addEventListener("stdout", (event) => {
      const data = JSON.parse(event.data);
      appendLogLines(data.text);
    });

    es.addEventListener("stderr", (event) => {
      const data = JSON.parse(event.data);
      appendLogLines(data.text);
    });

    es.addEventListener("error", (event: any) => {
      // EventSource reconnects automatically on failure
    });

    es.addEventListener("close", (event: any) => {
      const data = JSON.parse(event.data || "{}");
      setLogs((prev) => [...prev, `[system] process finished with code ${data.code ?? 0}`]);
    });

    return () => {
      es.close();
    };
  }, [isPlaying]);

  // Autoscroll logs
  useEffect(() => {
    if (scrollRef.current) {
      scrollRef.current.scrollTop = scrollRef.current.scrollHeight;
    }
  }, [logs]);

  function detectCrashReports(allLogs: string[]) {
    let startIndex = -1;
    let endIndex = -1;
    for (let i = allLogs.length - 1; i >= 0; i--) {
      if (allLogs[i].includes("=== END CRASH REPORT ===")) {
        endIndex = i;
      }
      if (allLogs[i].includes("=== PSP RECOMPILER CRASH REPORT ===")) {
        startIndex = i;
        break;
      }
    }

    if (startIndex !== -1 && endIndex !== -1 && startIndex < endIndex) {
      const crashLines = allLogs.slice(startIndex, endIndex + 1);
      const parsed = parseCrashReport(crashLines);
      if (parsed) {
        setCrashReport(parsed);
      }
    }
  }

  function parseCrashReport(lines: string[]): CrashReport | null {
    let exception = "";
    let fault = "";
    const hostRegs: Record<string, string> = {};
    const guestRegs: Record<string, string> = {};

    for (const line of lines) {
      if (line.includes("Exception:")) {
        exception = line.trim();
      } else if (line.includes("Fault:")) {
        fault = line.trim();
      } else if (line.includes("=") && (line.includes("RIP") || line.includes("RAX") || line.includes("RCX") || line.includes("RSI") || line.includes("RBP") || line.includes("R9") || line.includes("R11") || line.includes("R13") || line.includes("R15"))) {
        const parts = line.trim().split(/\s+/);
        for (const part of parts) {
          const [k, v] = part.split("=");
          if (k && v) hostRegs[k] = v;
        }
      } else if (line.includes("=") && (line.includes("PC") || line.includes("r4") || line.includes("r8") || line.includes("r12") || line.includes("r16") || line.includes("r20") || line.includes("r24") || line.includes("r28") || line.includes("hi"))) {
        const parts = line.trim().split(/\s+/);
        for (const part of parts) {
          const [k, v] = part.split("=");
          if (k && v) guestRegs[k] = v;
        }
      }
    }

    if (Object.keys(guestRegs).length === 0) return null;
    return { exception, fault, hostRegs, guestRegs };
  }

  const toggleSubsystem = (sub: string) => {
    setSelectedSubsystems((prev) =>
      prev.includes(sub) ? prev.filter((s) => s !== sub) : [...prev, sub]
    );
  };

  const filteredLogs = logs.filter((line) => {
    // 1. Text filter
    if (searchQuery && !line.toLowerCase().includes(searchQuery.toLowerCase())) {
      return false;
    }
    // 2. Subsystem filter (if any are active, the line must match one of the active subsystems)
    if (selectedSubsystems.length > 0) {
      const match = line.match(/^\[(MEM|HLE|SCHED|GE|INPUT|FS|VIDEO|MISC)\]/);
      if (match) {
        const sub = match[1];
        if (!selectedSubsystems.includes(sub)) {
          return false;
        }
      } else {
        // Include non-categorized lines only if MISC is selected or no subsystems are selected
        if (!selectedSubsystems.includes("MISC")) {
          return false;
        }
      }
    }
    return true;
  });

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<Cpu className="size-4.5" />}
        title="Recompiler Internals & Telemetry"
        subtitle="Live logs streaming, CpuState inspector, and recompiler offline pipeline statistics."
      />

      {/* Tabs list */}
      <div className="flex border-b border-border/60">
        <button
          onClick={() => setActiveTab("logs")}
          className={cn(
            "px-4 py-2 text-xs font-semibold border-b-2 -mb-px transition-colors duration-200",
            activeTab === "logs"
              ? "border-primary text-primary"
              : "border-transparent text-muted-foreground hover:text-foreground"
          )}
        >
          <div className="flex items-center gap-1.5">
            <Terminal className="size-3.5" /> Live Log Streamer
          </div>
        </button>
        <button
          onClick={() => setActiveTab("registers")}
          className={cn(
            "px-4 py-2 text-xs font-semibold border-b-2 -mb-px transition-colors duration-200",
            activeTab === "registers"
              ? "border-primary text-primary"
              : "border-transparent text-muted-foreground hover:text-foreground"
          )}
        >
          <div className="flex items-center gap-1.5">
            <Cpu className="size-3.5" /> Memory & Registers
          </div>
        </button>
      </div>

      {/* Logs View */}
      {activeTab === "logs" && (
        <Panel
          title="Debug Log Streamer"
          description="Non-blocking real-time capture of stdout/stderr logs from the recompiler runtime."
          icon={<Terminal className="size-4" />}
        >
          <div className="flex flex-col gap-3">
            {/* Log Controls */}
            <div className="flex flex-wrap items-center justify-between gap-2">
              <div className="flex items-center gap-2">
                <Button
                  size="sm"
                  variant="outline"
                  onClick={() => setIsPlaying(!isPlaying)}
                  className="h-8 gap-1.5"
                >
                  {isPlaying ? (
                    <>
                      <Pause className="size-3.5 text-amber-400" /> Pause
                    </>
                  ) : (
                    <>
                      <Play className="size-3.5 text-emerald-400" /> Resume
                    </>
                  )}
                </Button>
                <Button
                  size="sm"
                  variant="outline"
                  onClick={() => {
                    setLogs([]);
                    setCrashReport(null);
                  }}
                  className="h-8 gap-1.5"
                >
                  <Trash2 className="size-3.5 text-rose-400" /> Clear
                </Button>
              </div>
              <input
                type="text"
                placeholder="Filter logs by text..."
                value={searchQuery}
                onChange={(e) => setSearchQuery(e.target.value)}
                className="h-8 px-2.5 rounded-md border border-border/60 bg-background/50 text-[11px] placeholder:text-muted-foreground focus:outline-none focus:border-primary/50 min-w-48 flex-1 sm:flex-initial"
              />
            </div>

            {/* Subsystem Toggles */}
            <div className="flex flex-wrap gap-1 items-center">
              <span className="text-[10px] text-muted-foreground uppercase mr-1">Categories:</span>
              {subsystems.map((sub) => {
                const isActive = selectedSubsystems.includes(sub);
                return (
                  <button
                    key={sub}
                    onClick={() => toggleSubsystem(sub)}
                    className={cn(
                      "text-[9px] px-2 py-0.5 rounded border font-mono transition-all",
                      isActive
                        ? "border-primary/60 text-primary bg-primary/10"
                        : "border-border/60 text-muted-foreground bg-transparent hover:border-muted-foreground/40"
                    )}
                  >
                    {sub}
                  </button>
                );
              })}
            </div>

            {/* Logs Console */}
            <div
              ref={scrollRef}
              className="h-96 rounded-lg border border-border/60 bg-black/50 p-3 font-mono text-[11px] leading-relaxed overflow-y-auto thin-scroll flex flex-col gap-0.5 text-emerald-400/90"
            >
              {filteredLogs.length === 0 ? (
                <div className="text-muted-foreground italic text-center my-auto">
                  {isPlaying ? "Awaiting log stream..." : "Stream paused. No logs match active filters."}
                </div>
              ) : (
                filteredLogs.map((line, idx) => {
                  const isCrashHeader = line.includes("=== PSP RECOMPILER CRASH REPORT ===");
                  const isCrashFooter = line.includes("=== END CRASH REPORT ===");
                  return (
                    <div
                      key={idx}
                      className={cn(
                        "whitespace-pre-wrap break-all",
                        isCrashHeader && "text-rose-400 font-bold mt-2",
                        isCrashFooter && "text-rose-400 font-bold mb-2",
                        line.startsWith("[HLE]") && "text-sky-300",
                        line.startsWith("[SCHED]") && "text-purple-300",
                        line.startsWith("[GE]") && "text-amber-300",
                        line.startsWith("[VIDEO]") && "text-indigo-300",
                        (line.includes("Exception:") || line.includes("Fault:")) && "text-rose-300 font-medium"
                      )}
                    >
                      {line}
                    </div>
                  );
                })
              )}
            </div>
          </div>
        </Panel>
      )}

      {/* Registers & Crash Inspector */}
      {activeTab === "registers" && (
        <Panel
          title="Memory & Register CPU Inspector"
          description="Decodes register snapshots from crash dumps to diagnose host faults (Access Violations) and guest exceptions."
          icon={<Cpu className="size-4" />}
        >
          <div className="space-y-4">
            {/* Simulation Layer controls */}
            <div className="rounded-xl border border-border/60 bg-card/40 p-4 space-y-3">
              <div className="flex items-center justify-between">
                <span className="text-[11px] font-semibold uppercase tracking-wide text-primary">
                  Trace & Crash Simulation Layer
                </span>
                <Badge variant="outline" className="text-[9px] h-4 px-1.5 border-amber-500/20 text-amber-400">
                  Interactive Debugging
                </Badge>
              </div>
              <p className="text-[10px] text-muted-foreground leading-snug">
                Select a pre-configured mock crash snapshot below or paste a raw crash dump into the text area to simulate guest-vs-host trace differential state analysis.
              </p>

              {/* Presets */}
              <div className="flex flex-wrap gap-2">
                <Button
                  size="sm"
                  variant="outline"
                  onClick={() => {
                    const dump = `=== PSP RECOMPILER CRASH REPORT ===
Exception: CPU Watchdog Timeout / Infinite Spin Loop in sceFont HLE!
Fault: Runaway loop at sceFont.c:__wrap_f_000650e0 obj=0x08b34000
Host CPU state (x64 Native):
RIP=00007ff78e12ab73 RAX=0000000000000001 RBX=0000000000000002 RCX=0000000008b34000 RDX=0000000000000000
RSI=0000000000000008 RDI=0000000004370000 RBP=0000003bc49fef50 RSP=0000003bc49fee10
R8=0000000000000000 R9=0000000000000001 R10=0000000000000002 R11=0000000000000246
R12=0000000000000000 R13=000000000030aa88 R14=0000021c3bdf0000 R15=0000000000000000
Guest CPU state (MIPS Virtualized):
PC=000650e8 hi=00000000 lo=00000000
r0=00000000 r1=00000001 r2=00000000 r3=00000001
r4=08b34008 r5=00000000 r6=00000000 r7=00000000
r8=00000008 r9=00000000 r10=00000000 r11=00000000
r12=00000000 r13=00000000 r14=00000000 r15=00000000
r16=08b34000 r17=00000008 r18=00000000 r19=00000000
r20=00000000 r21=00000000 r22=00000000 r23=00000000
r24=00000000 r25=00000000 r26=00000000 r27=00000000
r28=0030aa88 r29=04000000 r30=00000000 r31=00064c12
=== END CRASH REPORT ===`;
                    setLogs(prev => [...prev, ...dump.split("\n")]);
                    const parsed = parseCrashReport(dump.split("\n"));
                    if (parsed) setCrashReport(parsed);
                  }}
                  className="h-7 text-[10px] font-mono border-amber-500/30 text-amber-300 hover:bg-amber-500/10"
                >
                  Load Watchdog Crash (sceFont)
                </Button>
                <Button
                  size="sm"
                  variant="outline"
                  onClick={() => {
                    const dump = `=== PSP RECOMPILER CRASH REPORT ===
Exception: CPU Floating Point Division-by-Zero in ge_gpu!
Fault: Access violation reading command queue block 0x00000000
Host CPU state (x64 Native):
RIP=00007ff78e11a3b2 RAX=0000000000000000 RBX=000000021c3bdf10 RCX=0000000000000000 RDX=0000000000000000
RSI=0000021c3bef19c0 RDI=0000000004000000 RBP=0000003bc49fedb0 RSP=0000003bc49fec90
R8=0000000000000000 R9=0000000000000000 R10=0000000000000000 R11=0000000000000000
R12=0000000000000000 R13=0000000000000000 R14=0000000000000000 R15=0000000000000000
Guest CPU state (MIPS Virtualized):
PC=0004f6b4 hi=00000000 lo=00000000
r0=00000000 r1=00000000 r2=00000000 r3=00000000
r4=00000000 r5=00000000 r6=00000000 r7=00000000
r8=00000000 r9=00000000 r10=00000000 r11=00000000
r12=00000000 r13=00000000 r14=00000000 r15=00000000
r16=00000000 r17=00000000 r18=00000000 r19=00000000
r20=00000000 r21=00000000 r22=00000000 r23=00000000
r24=00000000 r25=00000000 r26=00000000 r27=00000000
r28=00000000 r29=04000000 r30=00000000 r31=0004ccb8
=== END CRASH REPORT ===`;
                    setLogs(prev => [...prev, ...dump.split("\n")]);
                    const parsed = parseCrashReport(dump.split("\n"));
                    if (parsed) setCrashReport(parsed);
                  }}
                  className="h-7 text-[10px] font-mono border-sky-500/30 text-sky-300 hover:bg-sky-500/10"
                >
                  Load Division-by-Zero (ge_gpu)
                </Button>
                <Button
                  size="sm"
                  variant="outline"
                  onClick={() => {
                    setCrashReport(null);
                  }}
                  className="h-7 text-[10px] gap-1.5"
                >
                  Clear Inspector State
                </Button>
              </div>

              {/* Paste Textarea */}
              <div className="space-y-1.5">
                <span className="text-[10px] font-mono text-muted-foreground uppercase">Paste custom register snapshot:</span>
                <div className="flex gap-2">
                  <textarea
                    placeholder="Paste crash report or register log lines here..."
                    onChange={(e) => {
                      const text = e.target.value;
                      const parsed = parseCrashReport(text.split("\n"));
                      if (parsed) {
                        setCrashReport(parsed);
                      }
                    }}
                    className="w-full h-16 p-2 rounded border border-border/60 bg-black/40 text-[10px] font-mono placeholder:text-muted-foreground focus:outline-none focus:border-primary/50 text-emerald-400"
                  />
                </div>
              </div>
            </div>

            {crashReport ? (
              <div className="space-y-4">
                {/* Fault Alert */}
                <div className="flex items-start gap-3 p-3 rounded-lg border border-rose-500/20 bg-rose-500/10 text-rose-200">
                  <ShieldAlert className="size-5 shrink-0 mt-0.5" />
                  <div>
                    <div className="text-xs font-semibold">{crashReport.exception}</div>
                    {crashReport.fault && (
                      <div className="text-[10px] font-mono text-rose-300/95 mt-1">{crashReport.fault}</div>
                    )}
                  </div>
                </div>

                {/* Grid layout */}
                <div className="grid md:grid-cols-2 gap-4">
                  {/* Guest Registers */}
                  <div>
                    <h4 className="text-[10px] font-semibold uppercase text-muted-foreground tracking-wide mb-2 flex items-center gap-1.5">
                      <Badge variant="outline" className="text-[9px] h-4 px-1.5 border-primary/20 text-primary">PSP</Badge> Guest MIPS State
                    </h4>
                    <div className="grid grid-cols-2 gap-x-3 gap-y-1.5 bg-black/30 border border-border/40 rounded-lg p-2.5 font-mono text-[11px]">
                      <div className="col-span-2 flex justify-between border-b border-border/20 pb-1 mb-1 text-primary">
                        <span>PC (Program Counter)</span>
                        <span className="font-bold text-foreground">{crashReport.guestRegs["PC"] ?? "0x00000000"}</span>
                      </div>
                      {Object.entries(crashReport.guestRegs)
                        .filter(([k]) => k !== "PC")
                        .map(([reg, val]) => {
                          const isSuspicious = val === "00000000" || val === "0030aa88" || val.startsWith("08");
                          return (
                            <div key={reg} className={cn(
                              "flex justify-between hover:bg-accent/10 rounded px-1 py-0.5 transition-colors",
                              isSuspicious && "bg-amber-500/5 text-amber-300"
                            )}>
                              <span className="text-muted-foreground">{reg}</span>
                              <span className="text-foreground">{val}</span>
                            </div>
                          );
                        })}
                    </div>
                  </div>

                  {/* Host Registers */}
                  <div>
                    <h4 className="text-[10px] font-semibold uppercase text-muted-foreground tracking-wide mb-2 flex items-center gap-1.5">
                      <Badge variant="outline" className="text-[9px] h-4 px-1.5 border-indigo-500/20 text-indigo-400">HOST</Badge> Native x64 State
                    </h4>
                    <div className="grid grid-cols-2 gap-x-3 gap-y-1.5 bg-black/30 border border-border/40 rounded-lg p-2.5 font-mono text-[11px]">
                      <div className="col-span-2 flex justify-between border-b border-border/20 pb-1 mb-1 text-indigo-300">
                        <span>RIP (Instruction Pointer)</span>
                        <span className="font-bold text-foreground">{crashReport.hostRegs["RIP"] ?? "0x0000000000000000"}</span>
                      </div>
                      {Object.entries(crashReport.hostRegs)
                        .filter(([k]) => k !== "RIP")
                        .map(([reg, val]) => {
                          const isZero = val === "0000000000000000";
                          return (
                            <div key={reg} className={cn(
                              "flex justify-between hover:bg-accent/10 rounded px-1 py-0.5 transition-colors",
                              isZero && "text-muted-foreground/80"
                            )}>
                              <span className="text-muted-foreground">{reg}</span>
                              <span className="text-foreground font-medium">{val}</span>
                            </div>
                          );
                        })}
                    </div>
                  </div>
                </div>
              </div>
            ) : (
              <div className="flex flex-col items-center justify-center p-8 border border-dashed border-border/80 rounded-lg text-center bg-black/10">
                <FileText className="size-8 text-muted-foreground mb-2" />
                <div className="text-xs font-semibold">No active crash reports loaded</div>
                <p className="text-[10px] text-muted-foreground mt-1 max-w-sm">
                  Registers are automatically inspected and extracted here when a recompiler runtime crash report is written to the live debug stream, or click a load button above to simulate.
                </p>
              </div>
            )}
          </div>
        </Panel>
      )}

      {/* Pipeline */}
      <Panel
        title="Build pipeline"
        description="5-stage offline compilation: ELF → flat image → imports → MIPS-to-C → native exe"
        icon={<GitBranch className="size-4" />}
      >
        <div className="space-y-2">
          {PIPELINE_STAGES.map((stage, i) => (
            <div key={stage.id} className="flex items-start gap-3">
              <div className="flex flex-col items-center shrink-0">
                <div className="size-7 rounded-full bg-primary/15 border border-primary/30 text-primary text-xs font-mono font-bold grid place-items-center">
                  {stage.id}
                </div>
                {i < PIPELINE_STAGES.length - 1 ? (
                  <div className="w-px h-full bg-border/60 mt-1 min-h-6" />
                ) : null}
              </div>
              <div className="flex-1 pb-3">
                <div className="flex items-center gap-2 flex-wrap">
                  <span className="text-xs font-mono font-semibold text-primary">{stage.script}</span>
                  <Badge variant="outline" className="text-[9px] h-4 px-1 font-mono">
                    {stage.input} → {stage.output}
                  </Badge>
                </div>
                <p className="text-[11px] text-muted-foreground mt-1 leading-snug">{stage.purpose}</p>
              </div>
            </div>
          ))}
        </div>
      </Panel>

      {/* Runtime subsystems */}
      <Panel
        title="Runtime subsystems"
        description="src/rt/ — compiled into hst.exe (the native runtime)"
        icon={<Wrench className="size-4" />}
      >
        <div className="grid sm:grid-cols-2 gap-2">
          {RUNTIME_SUBSYSTEMS.map((sub) => (
            <div
              key={sub.id}
              className="rounded-lg border border-border/60 bg-background/30 p-2.5"
            >
              <div className="flex items-center gap-2 mb-1">
                <span className="text-xs font-semibold">{sub.name}</span>
                <Badge
                  variant="outline"
                  className={cn("text-[8px] h-4 px-1 capitalize", STATUS_STYLES[sub.status])}
                >
                  {sub.status}
                </Badge>
              </div>
              <div className="text-[9px] font-mono text-muted-foreground mb-1">{sub.file}</div>
              <p className="text-[10px] text-muted-foreground leading-snug">{sub.purpose}</p>
            </div>
          ))}
        </div>
      </Panel>

      {/* Retired LOOP_CAPS */}
      <Panel
        title="Retired LOOP_CAPS"
        description="All loop-cap band-aids were removed after root-cause fixes"
        icon={<Table className="size-4" />}
      >
        <div className="rounded-lg border border-border/60 divide-y divide-border/50 max-h-80 overflow-y-auto thin-scroll">
          <div className="grid grid-cols-[80px_1fr_60px_1fr] gap-2 px-2.5 py-1.5 bg-background/40 text-[9px] uppercase tracking-wide text-muted-foreground">
            <span>Address</span>
            <span>Name</span>
            <span>Limit</span>
            <span>Action</span>
          </div>
          {LOOP_CAPS.length === 0 ? (
            <div className="px-2.5 py-3 text-[10px] text-muted-foreground">
              No active loop caps. Diagnose sampled PCs and fix the underlying path.
            </div>
          ) : null}
          {LOOP_CAPS.map((cap) => (
            <div
              key={cap.address}
              className="grid grid-cols-[80px_1fr_60px_1fr] gap-2 px-2.5 py-1.5 items-center text-[10px] hover:bg-accent/20"
            >
              <span className="font-mono text-primary/80">{cap.address}</span>
              <span className="font-mono font-medium truncate" title={cap.note}>{cap.name}</span>
              <span className="font-mono text-muted-foreground">{cap.limit.toLocaleString()}</span>
              <span className="font-mono text-muted-foreground truncate" title={cap.action}>{cap.action}</span>
            </div>
          ))}
        </div>
      </Panel>

      {/* Function map */}
      <Panel
        title="Key function map"
        description="Critical guest addresses (from functions.csv + Ghidra decompilation)"
        icon={<Terminal className="size-4" />}
      >
        <div className="space-y-1.5">
          {FUNCTION_MAP.map((fn) => (
            <div
              key={fn.address}
              className="flex items-center gap-2 text-[11px] rounded-md px-2 py-1.5 bg-background/30 border border-border/40"
            >
              <span className="font-mono text-primary/80 w-24 shrink-0">{fn.address}</span>
              <span className="font-mono font-medium w-44 shrink-0 truncate">{fn.name}</span>
              <span className="font-mono text-muted-foreground w-16 shrink-0">{fn.size}</span>
              <span className="text-muted-foreground truncate flex-1">{fn.role}</span>
            </div>
          ))}
        </div>
      </Panel>

      {/* Thread map */}
      <Panel
        title="Thread map"
        description="Guest threads observed in run logs (cooperative fiber scheduler)"
        icon={<Cpu className="size-4" />}
      >
        <div className="grid grid-cols-2 sm:grid-cols-4 gap-2">
          {THREAD_MAP.map((t) => (
            <div
              key={t.uid}
              className="rounded-lg border border-border/60 bg-background/30 px-2.5 py-2"
            >
              <div className="flex items-center gap-1.5">
                <span className="text-xs font-mono font-bold text-primary">{t.uid}</span>
                <Badge
                  variant="outline"
                  className={cn(
                    "text-[8px] h-4 px-1",
                    t.state === "DORMANT" && "border-muted-foreground/30 text-muted-foreground",
                    t.state === "EXIT" && "border-amber-500/30 text-amber-300 bg-amber-500/10",
                    t.state === "EXIT/dissolved" && "border-amber-500/30 text-amber-300 bg-amber-500/10",
                  )}
                >
                  {t.state}
                </Badge>
              </div>
              <div className="text-[9px] font-mono text-muted-foreground mt-1">entry {t.entry}</div>
              <div className="text-[10px] text-muted-foreground mt-0.5">{t.role}</div>
            </div>
          ))}
        </div>
      </Panel>
    </div>
  );
}
