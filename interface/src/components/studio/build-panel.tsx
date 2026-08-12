"use client";

import React, { useEffect, useRef, useState } from "react";
import { CircleCheck, Hammer, Loader2, Play, Server, Square, Terminal } from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel, SectionHeader } from "./ui-bits";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { cn } from "@/lib/utils";

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

export function BuildPanel() {
  const { buildStatus, setBuild, buildRequestNonce } = useStudio();
  const [realStatus, setRealStatus] = useState<RealStatus>("idle");
  const [realLogTail, setRealLogTail] = useState<string[]>([]);
  const [inspectStatus, setInspectStatus] = useState("not checked");
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

  async function realBuild(action: ManagerAction, runOptions?: RunOptions) {
    eventSourceRef.current?.close();
    eventSourceRef.current = null;

    setRealStatus("running");
    if (action !== "Run") setBuild({ buildStatus: "running" });
    setRealLogTail([
      `[${new Date().toISOString()}] starting hst_manager.ps1 -Action ${action}`,
    ]);

    try {
      const body = action === "Run"
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
            // Keep the generic transport error.
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

  async function refreshInspect() {
    setInspectStatus("checking…");
    try {
      const response = await fetch("/api/recompiler/run", { cache: "no-store" });
      const data = await response.json();
      if (!response.ok) throw new Error(data?.detail ?? data?.error ?? "status request failed");
      const binary = data.exists
        ? `${(data.sizeBytes / 1024 / 1024).toFixed(2)} MB · built ${new Date(data.mtime).toLocaleString()}`
        : "missing — run BuildFull";
      setInspectStatus(`${data.hstExePath.split(/[\\/]/).pop()} · ${binary}`);
    } catch (error) {
      setInspectStatus(`unreachable: ${String(error)}`);
    }
  }

  async function stopManagerTask() {
    try {
      const response = await fetch("/api/recompiler/manager", { method: "DELETE" });
      const data = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(data?.detail ?? data?.error ?? "stop failed");
      setRealLogTail((previous) => [...previous, data.stopped ? "[ok] process tree stopped" : "[ok] no active process"]);
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

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<Hammer className="size-4.5" />}
        title="Recompile"
        subtitle="Build, test, run, and inspect the actual native project through hst_manager.ps1."
        right={
          <Badge
            variant="outline"
            className={cn(
              "h-6",
              running && "border-primary/40 text-primary bg-primary/10",
              !running && realStatus === "done" && "border-emerald-500/40 text-emerald-300 bg-emerald-500/10",
            )}
          >
            {running ? <Loader2 className="size-3 mr-1 animate-spin" /> : realStatus === "done" ? <CircleCheck className="size-3 mr-1" /> : null}
            {running ? "Running" : realStatus === "done" ? "Complete" : realStatus === "failed" ? "Failed" : "Idle"}
          </Badge>
        }
      />

      <Panel
        title="Native pipeline"
        description="Runs the repository's real manager and streams its real stdout/stderr. No generated demo artifacts or simulated stages."
        icon={<Server className="size-4" />}
        right={
          <Button size="sm" variant="outline" className="h-7 gap-1.5" onClick={() => void refreshInspect()}>
            hst.exe status
          </Button>
        }
      >
        <div className="flex items-center gap-2 text-[11px] text-muted-foreground mb-3">
          <Server className="size-3.5" />
          <span className="font-mono">{inspectStatus}</span>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <Button size="sm" variant="outline" className="gap-1.5" onClick={() => void realBuild("BuildFast")} disabled={running}>
            <Play className="size-3.5" /> BuildFast
          </Button>
          <Button size="sm" variant="outline" className="gap-1.5" onClick={() => void realBuild("BuildFull")} disabled={running}>
            <Play className="size-3.5" /> BuildFull
          </Button>
          <Button size="sm" variant="outline" className="gap-1.5" onClick={() => void realBuild("Test")} disabled={running}>
            <Play className="size-3.5" /> Selftest
          </Button>
          <Button size="sm" variant="outline" className="gap-1.5" onClick={() => void realBuild("Run")} disabled={running}>
            <Play className="size-3.5" /> Run Standard GUI
          </Button>
          <Button
            size="sm"
            variant="outline"
            className="gap-1.5"
            onClick={() => void realBuild("Run", {
              profile: "Software",
              durationSeconds: 15,
              noGui: true,
              softwareRender: true,
              snapshotInterval: 20,
            })}
            disabled={running}
          >
            <Play className="size-3.5" /> Capture 15s snapshots
          </Button>
          {running ? (
            <Button size="sm" variant="destructive" className="gap-1.5" onClick={() => void stopManagerTask()}>
              <Square className="size-3.5" /> Stop process tree
            </Button>
          ) : null}
          <Button size="sm" variant="ghost" className="gap-1.5" onClick={() => void followRunLog()}>
            <Terminal className="size-3.5" /> Tail run log
          </Button>
        </div>

        <WatchpointAlertsSection />

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
    const match = line.match(/MEM_WATCH\[([^\]]+)\]:\s+(WRITE|READ)\s+addr=(0x[0-9a-fA-F]+)\s+val=(0x[0-9a-fA-F]+)\s+pc=(0x[0-9a-fA-F]+)/);
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
    const existing = existingRaw ? JSON.parse(existingRaw) as WatchpointAlert[] : [];
    localStorage.setItem("hst_watchpoint_alerts", JSON.stringify([...existing, ...alerts].slice(-100)));
    window.dispatchEvent(new Event("storage"));
  } catch {
    // Local alert persistence is optional.
  }
}

function realLogColor(line: string): string {
  if (line.includes("MEM_WATCH[")) return line.includes("WRITE") ? "text-rose-400 font-semibold" : "text-amber-400 font-semibold";
  if (line.startsWith("[error]") || line.startsWith("[fail]")) return "text-destructive";
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
            <span className="text-rose-300 font-semibold">{alert.label} ({alert.type})</span>
            <span className="text-muted-foreground">{alert.addr} · {alert.val} · PC {alert.pc}</span>
          </div>
        ))}
      </div>
    </div>
  );
}
