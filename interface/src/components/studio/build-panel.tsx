"use client";

import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  AlertCircle,
  AlertTriangle,
  CheckCircle2,
  CircleCheck,
  Cpu,
  FlaskConical,
  Hammer,
  HelpCircle,
  Info,
  Loader2,
  Play,
  Server,
  Square,
  Terminal,
  Zap,
} from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel, SectionHeader } from "./ui-bits";
import { PreflightCard } from "./preflight-card";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { cn } from "@/lib/utils";
import type { DoctorReport } from "@/lib/recompiler/doctor";

type ManagerAction = "BuildFull" | "BuildFast" | "Test" | "Run";
type RealStatus = "idle" | "running" | "done" | "failed";
type RunOptions = {
  profile: "Standard" | "Diagnostics" | "Software";
  durationSeconds: number;
  noGui: boolean;
  softwareRender: boolean;
  snapshotInterval: number | null;
};

interface WatchpointAlert {
  id: string;
  label: string;
  type: string;
  addr: string;
  val: string;
  pc: string;
  timestamp: string;
}

interface BinaryInfo {
  exists: boolean;
  sizeBytes: number;
  mtime: number;
  hstExePath: string;
  vulkanSdkFoundAt: string | null;
}

export function BuildPanel() {
  const { buildStatus, setBuild, buildRequestNonce } = useStudio();
  const [realStatus, setRealStatus] = useState<RealStatus>("idle");
  const [realLogTail, setRealLogTail] = useState<string[]>([]);
  const [inspectStatus, setInspectStatus] = useState("checking…");
  const [binaryInfo, setBinaryInfo] = useState<BinaryInfo | null>(null);
  const [doctorReport, setDoctorReport] = useState<DoctorReport | null>(null);
  const [showExplanation, setShowExplanation] = useState(false);
  const [activeActionNote, setActiveActionNote] = useState<string | null>(null);

  const eventSourceRef = useRef<EventSource | null>(null);
  const startBuildRef = useRef<() => void>(() => {});

  useEffect(() => {
    startBuildRef.current = () => void realBuild("BuildFull");
  });

  const lastNonceRef = useRef(0);
  useEffect(() => {
    if (buildRequestNonce > 0 && buildRequestNonce !== lastNonceRef.current) {
      lastNonceRef.current = buildRequestNonce;
      startBuildRef.current();
    }
  }, [buildRequestNonce]);

  useEffect(() => () => eventSourceRef.current?.close(), []);

  const refreshInspect = useCallback(async () => {
    setInspectStatus("checking…");
    try {
      const response = await fetch("/api/recompiler/run", { cache: "no-store" });
      const data = await response.json();
      if (!response.ok) throw new Error(data?.detail ?? data?.error ?? "status request failed");
      setBinaryInfo(data as BinaryInfo);
      const binary = data.exists
        ? `${(data.sizeBytes / 1024 / 1024).toFixed(2)} MB · built ${new Date(data.mtime).toLocaleString()}`
        : "missing — run BuildFull";
      setInspectStatus(`${data.hstExePath.split(/[\\/]/).pop()} · ${binary}`);
    } catch (error) {
      setInspectStatus(`unreachable: ${String(error)}`);
    }
  }, []);

  useEffect(() => {
    void refreshInspect();
  }, [refreshInspect]);

  // Evaluate toolchain and input prerequisites from Doctor report
  const buildPrereqs = useMemo(() => {
    if (!doctorReport) return { ready: true, missing: [] as string[] };
    const missing: string[] = [];
    for (const res of doctorReport.results) {
      if (res.status === "FAIL") {
        if (
          res.code.startsWith("POWERSHELL_") ||
          res.code.startsWith("HOST_") ||
          res.code.startsWith("PYTHON_") ||
          res.code.startsWith("MSYS2_") ||
          res.code.startsWith("TOOL_") ||
          res.code.startsWith("SDL3_IMPORT") ||
          res.code.startsWith("VULKAN_")
        ) {
          missing.push(`Toolchain: ${res.summary}`);
        } else if (res.code.startsWith("INPUT_") || res.code === "SAVE_ROOT") {
          missing.push(`Game Input: ${res.summary}`);
        }
      }
    }
    return {
      ready: missing.length === 0,
      missing,
    };
  }, [doctorReport]);

  async function realBuild(action: ManagerAction, runOptions?: RunOptions) {
    eventSourceRef.current?.close();
    eventSourceRef.current = null;

    setRealStatus("running");
    if (action !== "Run") setBuild({ buildStatus: "running" });
    setRealLogTail([
      `[${new Date().toISOString()}] starting hst_manager.ps1 -Action ${action}`,
    ]);

    try {
      const body =
        action === "Run"
          ? {
              action,
              run: runOptions ?? {
                profile: "Standard",
                durationSeconds: 0,
                noGui: false,
                softwareRender: false,
                snapshotInterval: null,
              },
            }
          : { action };
      const response = await fetch("/api/recompiler/manager", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const payload = await response.json().catch(() => ({}));
      if (!response.ok) {
        throw new Error(payload?.detail ?? payload?.message ?? payload?.error ?? "failed to start manager task");
      }

      const source = new EventSource("/api/recompiler/manager");
      eventSourceRef.current = source;

      const appendEventLines = (event: MessageEvent<string>) => {
        try {
          const data = JSON.parse(event.data) as { text?: string };
          const lines = String(data.text ?? "").split(/\r?\n/).filter(Boolean);
          if (lines.length === 0) return;
          setRealLogTail((previous) => [...previous, ...lines]);
          persistWatchpointAlerts(lines);
        } catch {
          setRealLogTail((previous) => [...previous, "[error] malformed manager stream event"]);
        }
      };

      source.addEventListener("stdout", appendEventLines as EventListener);
      source.addEventListener("stderr", appendEventLines as EventListener);
      source.addEventListener("error", ((event: MessageEvent<string> | Event) => {
        let message = "manager stream disconnected";
        if ("data" in event && event.data) {
          try {
            message = JSON.parse(event.data)?.message ?? message;
          } catch {
            // Keep generic transport error.
          }
        }
        setRealLogTail((previous) => [...previous, `[error] ${message}`]);
        setRealStatus("failed");
        source.close();
        if (eventSourceRef.current === source) eventSourceRef.current = null;
      }) as EventListener);
      source.addEventListener("close", ((event: MessageEvent<string>) => {
        const data = JSON.parse(event.data || "{}") as { code?: number };
        const exitCode = data.code ?? 1;
        setRealLogTail((previous) => [...previous, `[summary] process finished with code ${exitCode}`]);
        setRealStatus(exitCode === 0 ? "done" : "failed");
        if (action !== "Run") {
          setBuild({ buildStatus: exitCode === 0 ? "completed" : "failed" });
        }
        source.close();
        if (eventSourceRef.current === source) eventSourceRef.current = null;
        void refreshInspect();
      }) as EventListener);
    } catch (error) {
      setRealLogTail((previous) => [...previous, `[error] ${String(error)}`]);
      setRealStatus("failed");
      if (action !== "Run") setBuild({ buildStatus: "failed" });
    }
  }

  async function stopManagerTask() {
    try {
      const response = await fetch("/api/recompiler/manager", { method: "DELETE" });
      const data = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(data?.detail ?? data?.error ?? "stop failed");
      setRealLogTail((previous) => [
        ...previous,
        data.stopped ? "[ok] process tree stopped" : "[ok] no active process",
      ]);
    } catch (error) {
      setRealLogTail((previous) => [...previous, `[error] ${String(error)}`]);
    }
  }

  async function followRunLog() {
    try {
      const response = await fetch("/api/recompiler/log", { cache: "no-store" });
      const data = await response.json();
      if (!response.ok || !data.found) {
        setRealLogTail((previous) => [...previous, "[log] no native run log found"]);
        return;
      }
      setRealLogTail((previous) => [...previous, `[tail] ${data.path}`, ...data.lines.slice(-40)]);
    } catch (error) {
      setRealLogTail((previous) => [...previous, `[error] ${String(error)}`]);
    }
  }

  const running = realStatus === "running";
  const completed = buildStatus === "completed";
  const binaryExists = Boolean(binaryInfo?.exists);

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<Hammer className="size-4.5" />}
        title="Recompile & Run"
        subtitle="Manage the native compilation pipeline, verify prerequisites, and execute hst.exe via hst_manager.ps1."
        right={
          <Badge
            variant="outline"
            className={cn(
              "h-6",
              running && "border-primary/40 text-primary bg-primary/10",
              !running && realStatus === "done" && "border-emerald-500/40 text-emerald-300 bg-emerald-500/10",
              !running && realStatus === "failed" && "border-rose-500/40 text-rose-300 bg-rose-500/10",
            )}
          >
            {running ? (
              <Loader2 className="size-3 mr-1 animate-spin" />
            ) : realStatus === "done" ? (
              <CircleCheck className="size-3 mr-1" />
            ) : null}
            {running ? "Running" : realStatus === "done" ? "Complete" : realStatus === "failed" ? "Failed" : "Idle"}
          </Badge>
        }
      />

      {/* Preflight Diagnostics Card */}
      <PreflightCard onReportLoaded={setDoctorReport} />

      {/* Pipeline Explanations Accordion / Panel */}
      <Panel
        title="Build & Run Architecture"
        description="Truthful explanation of the native pipeline stages and execution modes."
        icon={<Info className="size-4" />}
        right={
          <Button
            size="sm"
            variant="ghost"
            className="h-7 text-xs gap-1"
            onClick={() => setShowExplanation(!showExplanation)}
          >
            <HelpCircle className="size-3.5" />
            {showExplanation ? "Hide Details" : "Show Details"}
          </Button>
        }
      >
        <div className="grid grid-cols-1 md:grid-cols-2 gap-3 text-xs">
          <div className="rounded-lg border border-border/50 bg-background/30 p-3 space-y-1.5">
            <div className="flex items-center gap-1.5 font-semibold text-foreground">
              <Zap className="size-3.5 text-primary" />
              <span>BuildFull (Full Pipeline)</span>
            </div>
            <p className="text-[11px] text-muted-foreground leading-relaxed">
              Analyzes decrypted MIPS binaries (<code className="font-mono text-foreground">EBOOT.elf</code> and PRXs) from{" "}
              <code className="font-mono text-foreground">place_game_here/</code>, runs the full Python code generator (
              <code className="font-mono text-foreground">tools/codegen.py</code>), outputs translation chunks (
              <code className="font-mono text-foreground">build/hst/hst_recomp_*.c</code>) and{" "}
              <code className="font-mono text-foreground">hst_image.bin</code>, then compiles the executable.
            </p>
            <div className="text-[10px] text-muted-foreground font-mono bg-accent/30 p-1 rounded">
              Use when: Initial build, changing game inputs, modifying analyzer logic, or after compiler updates.
            </div>
          </div>

          <div className="rounded-lg border border-border/50 bg-background/30 p-3 space-y-1.5">
            <div className="flex items-center gap-1.5 font-semibold text-foreground">
              <Cpu className="size-3.5 text-ball" />
              <span>BuildFast (Incremental Build)</span>
            </div>
            <p className="text-[11px] text-muted-foreground leading-relaxed">
              Incremental build that preserves reusable generated and compiled work. Make automatically regenerates or
              recompiles stale or missing outputs when needed.
            </p>
            <div className="text-[10px] text-muted-foreground font-mono bg-accent/30 p-1 rounded">
              Use when: Iterating on runtime C source, HLE syscalls, GPU Vulkan backend, or scheduler logic.
            </div>
          </div>
        </div>

        {showExplanation && (
          <div className="mt-3 pt-3 border-t border-border/40 grid grid-cols-1 sm:grid-cols-3 gap-2 text-xs">
            <div className="p-2 rounded bg-background/20 border border-border/30">
              <span className="font-semibold block mb-0.5 text-foreground flex items-center gap-1">
                <FlaskConical className="size-3 text-cyan-400" /> Selftest
              </span>
              <span className="text-[10px] text-muted-foreground leading-snug block">
                Executes native unit tests, cosimulation parity gates, dispatch boundary checks, and scheduler invariants.
              </span>
            </div>
            <div className="p-2 rounded bg-background/20 border border-border/30">
              <span className="font-semibold block mb-0.5 text-foreground flex items-center gap-1">
                <Play className="size-3 text-emerald-400" /> Run Standard GUI
              </span>
              <span className="text-[10px] text-muted-foreground leading-snug block">
                Launches <code className="font-mono">build/hst/hst.exe</code> with interactive window and Vulkan graphics renderer.
              </span>
            </div>
            <div className="p-2 rounded bg-background/20 border border-border/30">
              <span className="font-semibold block mb-0.5 text-foreground flex items-center gap-1">
                <Terminal className="size-3 text-amber-400" /> 15s Snapshots
              </span>
              <span className="text-[10px] text-muted-foreground leading-snug block">
                Runs headless with software rendering for 15s, capturing frame PPMs to <code className="font-mono">build/snapshots/</code>.
              </span>
            </div>
          </div>
        )}
      </Panel>

      {/* Main Native Pipeline Panel */}
      <Panel
        title="Native Execution Controls"
        description="Invokes hst_manager.ps1 with strict prerequisite validation and live stdout/stderr SSE streaming."
        icon={<Server className="size-4" />}
        right={
          <Button size="sm" variant="outline" className="h-7 gap-1.5 text-xs" onClick={() => void refreshInspect()}>
            Refresh hst.exe
          </Button>
        }
      >
        {/* Binary Status Indicator */}
        <div className="flex flex-col sm:flex-row sm:items-center justify-between gap-2 rounded-lg border border-border/60 bg-background/30 p-2.5 mb-3 text-xs font-mono">
          <div className="flex items-center gap-2 min-w-0">
            <Server className="size-4 text-primary shrink-0" />
            <span className="truncate">{inspectStatus}</span>
          </div>
          <Badge
            variant="outline"
            className={cn(
              "h-5 text-[10px] shrink-0",
              binaryExists
                ? "border-emerald-500/40 text-emerald-300 bg-emerald-500/10"
                : "border-amber-500/40 text-amber-300 bg-amber-500/10",
            )}
          >
            {binaryExists ? "Binary Ready" : "Executable Missing"}
          </Badge>
        </div>

        {/* Prerequisite Warnings */}
        {!buildPrereqs.ready && (
          <div className="rounded-lg border border-amber-900/40 bg-amber-950/20 p-3 mb-3 text-xs space-y-1.5">
            <div className="flex items-center gap-1.5 font-semibold text-amber-300">
              <AlertTriangle className="size-4 text-amber-400 shrink-0" />
              <span>Build Prerequisites Action Items Detected</span>
            </div>
            <p className="text-[11px] text-amber-200/80 leading-relaxed">
              Some prerequisites validated by Workspace Doctor are missing or incomplete. Builds may fail if required tools or game inputs are absent:
            </p>
            <ul className="list-disc list-inside space-y-0.5 text-[11px] text-amber-300/90 font-mono">
              {buildPrereqs.missing.slice(0, 4).map((item, idx) => (
                <li key={idx} className="truncate">{item}</li>
              ))}
              {buildPrereqs.missing.length > 4 && (
                <li>…and {buildPrereqs.missing.length - 4} more (see Preflight above)</li>
              )}
            </ul>
          </div>
        )}

        {/* Executable Guard Warning if user wants to run without binary */}
        {!binaryExists && (
          <div className="rounded-lg border border-border/60 bg-card/20 p-2.5 mb-3 text-xs text-muted-foreground flex items-center gap-2">
            <Info className="size-4 text-ball shrink-0" />
            <span>
              <code className="font-mono text-foreground font-semibold">hst.exe</code> has not been built yet.
              Run <strong className="text-foreground">BuildFull</strong> (or <strong className="text-foreground">BuildFast</strong>) before launching a run profile.
            </span>
          </div>
        )}

        {/* Action Buttons */}
        <div className="space-y-2">
          <div className="text-[11px] font-semibold uppercase tracking-wider text-muted-foreground">
            Compilation & Testing
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <Button
              size="sm"
              variant="outline"
              className="gap-1.5"
              onClick={() => void realBuild("BuildFast")}
              disabled={running}
              title="Incremental build that preserves reusable generated and compiled work"
            >
              <Cpu className="size-3.5 text-ball" /> BuildFast
            </Button>
            <Button
              size="sm"
              variant="outline"
              className="gap-1.5 bg-primary/10 border-primary/30 hover:bg-primary/20 text-primary"
              onClick={() => void realBuild("BuildFull")}
              disabled={running}
              title="Full pipeline: MIPS codegen, chunk creation, and binary compilation"
            >
              <Zap className="size-3.5" /> BuildFull (Codegen + Build)
            </Button>
            <Button
              size="sm"
              variant="outline"
              className="gap-1.5"
              onClick={() => void realBuild("Test")}
              disabled={running}
              title="Run native selftest and regression verification"
            >
              <FlaskConical className="size-3.5 text-cyan-400" /> Selftest
            </Button>
          </div>

          <div className="text-[11px] font-semibold uppercase tracking-wider text-muted-foreground pt-2">
            Executable Execution (Executable Guarded)
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <Button
              size="sm"
              variant={binaryExists ? "default" : "outline"}
              className={cn("gap-1.5", !binaryExists && "opacity-50 cursor-not-allowed")}
              onClick={() => {
                if (!binaryExists) return;
                void realBuild("Run");
              }}
              disabled={running || !binaryExists}
              title={binaryExists ? "Launch hst.exe with GUI" : "Requires built hst.exe"}
            >
              <Play className="size-3.5" /> Run Standard GUI
            </Button>
            <Button
              size="sm"
              variant="outline"
              className={cn("gap-1.5", !binaryExists && "opacity-50 cursor-not-allowed")}
              onClick={() => {
                if (!binaryExists) return;
                void realBuild("Run", {
                  profile: "Software",
                  durationSeconds: 15,
                  noGui: true,
                  softwareRender: true,
                  snapshotInterval: 20,
                });
              }}
              disabled={running || !binaryExists}
              title={binaryExists ? "Capture 15s snapshots headlessly" : "Requires built hst.exe"}
            >
              <Play className="size-3.5" /> Capture 15s snapshots
            </Button>

            {running ? (
              <Button size="sm" variant="destructive" className="gap-1.5" onClick={() => void stopManagerTask()}>
                <Square className="size-3.5" /> Stop process tree
              </Button>
            ) : null}
            <Button size="sm" variant="ghost" className="gap-1.5 text-muted-foreground" onClick={() => void followRunLog()}>
              <Terminal className="size-3.5" /> Tail run log
            </Button>
          </div>
        </div>

        <WatchpointAlertsSection />

        {/* Live Output Console */}
        {realLogTail.length > 0 ? (
          <div className="rounded-lg border border-border/60 bg-black/40 scanline p-3 font-mono text-[11px] leading-relaxed mt-3 max-h-60 overflow-y-auto thin-scroll">
            {realLogTail.slice(-120).map((line, index) => (
              <div key={`${index}-${line}`} className="whitespace-pre-wrap break-all">
                <span className="text-muted-foreground/50 select-none">{String(index + 1).padStart(3, "0")}</span>{" "}
                <span className={realLogColor(line)}>{line}</span>
              </div>
            ))}
          </div>
        ) : null}
      </Panel>

      {completed ? (
        <p className="text-[11px] text-muted-foreground">
          The native build completed. Runtime files remain in <code className="font-mono text-foreground">build/hst/</code>; this dashboard does not manufacture or package replacement binaries.
        </p>
      ) : null}
    </div>
  );
}

function persistWatchpointAlerts(lines: string[]) {
  const alerts: WatchpointAlert[] = [];
  for (const line of lines) {
    const match = line.match(
      /MEM_WATCH\[([^\]]+)\]:\s+(WRITE|READ)\s+addr=(0x[0-9a-fA-F]+)\s+val=(0x[0-9a-fA-F]+)\s+pc=(0x[0-9a-fA-F]+)/,
    );
    if (!match) continue;
    const [, label, type, addr, val, pc] = match;
    alerts.push({
      id: `${Date.now()}-${Math.random()}`,
      label,
      type,
      addr,
      val,
      pc,
      timestamp: new Date().toLocaleTimeString(),
    });
  }
  if (alerts.length === 0) return;
  try {
    const existingRaw = localStorage.getItem("hst_watchpoint_alerts");
    const existing = existingRaw ? (JSON.parse(existingRaw) as WatchpointAlert[]) : [];
    localStorage.setItem("hst_watchpoint_alerts", JSON.stringify([...existing, ...alerts].slice(-100)));
    window.dispatchEvent(new Event("storage"));
  } catch {
    // Local alert persistence is optional.
  }
}

function realLogColor(line: string): string {
  if (line.includes("MEM_WATCH[")) {
    return line.includes("WRITE") ? "text-rose-400 font-semibold" : "text-amber-400 font-semibold";
  }
  if (line.startsWith("[error]") || line.startsWith("[fail]")) return "text-destructive font-semibold";
  if (line.startsWith("[ok]") || line.startsWith("[summary]")) return "text-emerald-300";
  if (line.startsWith("[tail]")) return "text-primary font-semibold";
  return "text-foreground/80";
}

function WatchpointAlertsSection() {
  const [alerts, setAlerts] = useState<WatchpointAlert[]>([]);

  useEffect(() => {
    const load = () => {
      const raw = localStorage.getItem("hst_watchpoint_alerts");
      if (!raw) return setAlerts([]);
      try {
        setAlerts(JSON.parse(raw) as WatchpointAlert[]);
      } catch {
        setAlerts([]);
      }
    };
    load();
    window.addEventListener("storage", load);
    return () => window.removeEventListener("storage", load);
  }, []);

  if (alerts.length === 0) return null;
  return (
    <div className="mt-3 rounded-lg border border-rose-800/40 bg-rose-950/15 p-3 space-y-2">
      <div className="flex items-center justify-between">
        <span className="text-[10px] font-semibold uppercase text-rose-400 tracking-wider">
          Watchpoint hits ({alerts.length})
        </span>
        <button
          onClick={() => {
            localStorage.removeItem("hst_watchpoint_alerts");
            setAlerts([]);
          }}
          className="text-[10px] text-muted-foreground hover:text-rose-400 underline font-mono"
        >
          Clear
        </button>
      </div>
      <div className="max-h-24 overflow-y-auto thin-scroll space-y-1">
        {alerts.slice(-10).map((alert) => (
          <div key={alert.id} className="flex justify-between items-center text-[10px] font-mono py-0.5 border-b border-border/20">
            <span className="text-rose-300 font-semibold">
              {alert.label} ({alert.type})
            </span>
            <span className="text-muted-foreground">
              {alert.addr} · {alert.val} · PC {alert.pc}
            </span>
          </div>
        ))}
      </div>
    </div>
  );
}
