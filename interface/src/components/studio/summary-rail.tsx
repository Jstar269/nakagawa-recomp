"use client";

import { useCallback, useEffect, useState } from "react";
import { Activity, Binary, CircleCheck, CircleX, RefreshCw, Wrench } from "lucide-react";
import { Panel } from "./ui-bits";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { useStudio } from "./studio-context";
import type { DoctorReport } from "@/lib/recompiler/doctor";

type BootState = {
  ok?: boolean;
  status?: string;
  lastPhase?: string;
  reached?: Record<string, boolean>;
  nonzeroPixels?: number;
  malformedHostPaths?: number;
};

type BinaryState = {
  exists?: boolean;
  sizeBytes?: number;
  mtime?: number;
  hstExePath?: string;
};

export function SummaryRail() {
  const { setSection } = useStudio();
  const [boot, setBoot] = useState<BootState>({ status: "loading" });
  const [binary, setBinary] = useState<BinaryState>({});
  const [doctor, setDoctor] = useState<DoctorReport | null>(null);

  const refresh = useCallback(async () => {
    const [bootResponse, binaryResponse, doctorResponse] = await Promise.all([
      fetch("/api/recompiler/boot", { cache: "no-store" }).catch(() => null),
      fetch("/api/recompiler/run", { cache: "no-store" }).catch(() => null),
      fetch("/api/recompiler/doctor?scope=all", { cache: "no-store" }).catch(() => null),
    ]);
    if (bootResponse?.ok) setBoot(await bootResponse.json());
    if (binaryResponse?.ok) setBinary(await binaryResponse.json());
    if (doctorResponse?.ok) setDoctor(await doctorResponse.json());
  }, []);

  useEffect(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), 4000);
    return () => window.clearInterval(timer);
  }, [refresh]);

  const milestones = ["image_loaded", "runtime_registered", "window_ready", "guest_start", "display_flip"];
  const docCounts = doctor?.counts ?? { PASS: 0, WARN: 0, FAIL: 0, INFO: 0 };
  const hasFailures = docCounts.FAIL > 0;
  const hasWarnings = docCounts.WARN > 0;

  return (
    <div className="flex flex-col gap-3">
      {/* Workspace Preflight Summary */}
      <Panel
        title="Workspace Preflight"
        description="Doctor diagnostics status"
        icon={<Wrench className="size-4" />}
        right={
          <Button
            variant="ghost"
            size="sm"
            className="h-6 px-1.5 text-[10px] text-muted-foreground hover:text-foreground"
            onClick={() => setSection("build")}
          >
            Details →
          </Button>
        }
      >
        <div className="flex items-center justify-between mb-2">
          <Badge
            variant="outline"
            className={
              hasFailures
                ? "border-rose-500/40 text-rose-400 bg-rose-500/10 text-[10px]"
                : hasWarnings
                ? "border-amber-500/40 text-amber-400 bg-amber-500/10 text-[10px]"
                : "border-emerald-500/40 text-emerald-400 bg-emerald-500/10 text-[10px]"
            }
          >
            {hasFailures ? `${docCounts.FAIL} FAILURES` : hasWarnings ? `${docCounts.WARN} WARNINGS` : "ALL PASS"}
          </Badge>
          <span className="text-[10px] font-mono text-muted-foreground">
            {docCounts.PASS} pass · {docCounts.FAIL} fail
          </span>
        </div>
      </Panel>

      {/* Native Boot Health */}
      <Panel
        title="Native boot health"
        description="Parsed from BOOT_EVENT milestones"
        icon={<Activity className="size-4" />}
        right={
          <Button
            variant="ghost"
            size="sm"
            className="size-7 p-0"
            onClick={() => void refresh()}
            aria-label="Refresh native status"
          >
            <RefreshCw className="size-3.5" />
          </Button>
        }
      >
        <div className="flex items-center justify-between mb-3">
          <Badge
            variant="outline"
            className={
              boot.ok
                ? "border-emerald-500/40 text-emerald-700 dark:text-emerald-300"
                : "border-amber-500/40 text-amber-700 dark:text-amber-300"
            }
          >
            {boot.ok ? "FRAME PRESENTED" : (boot.status ?? "UNKNOWN").toUpperCase()}
          </Badge>
          <span className="text-[10px] font-mono text-muted-foreground">
            {boot.status ?? boot.lastPhase ?? "not-started"}
          </span>
        </div>
        <div className="space-y-1.5">
          {milestones.map((phase) => {
            const reached = Boolean(boot.reached?.[phase]);
            return (
              <div key={phase} className="flex items-center gap-2 text-[10px] font-mono">
                {reached ? (
                  <CircleCheck className="size-3.5 text-emerald-600 dark:text-emerald-400" />
                ) : (
                  <CircleX className="size-3.5 text-muted-foreground/40" />
                )}
                <span className={reached ? "text-foreground" : "text-muted-foreground"}>{phase}</span>
              </div>
            );
          })}
        </div>
        <div className="grid grid-cols-2 gap-2 mt-3">
          <div className="rounded border border-border/50 p-2">
            <div className="text-[9px] uppercase text-muted-foreground">Nonzero pixels</div>
            <div className="font-mono text-xs">{boot.nonzeroPixels ?? 0}</div>
          </div>
          <div className="rounded border border-border/50 p-2">
            <div className="text-[9px] uppercase text-muted-foreground">Bad host paths</div>
            <div className="font-mono text-xs">{boot.malformedHostPaths ?? 0}</div>
          </div>
        </div>
      </Panel>

      {/* Native Binary Status */}
      <Panel title="Native binary" description="Actual build/hst/hst.exe" icon={<Binary className="size-4" />}>
        <div className="flex items-center justify-between">
          <span className="text-xs font-medium">{binary.exists ? "Built" : "Missing"}</span>
          <span className="text-xs font-mono text-ball">
            {binary.sizeBytes ? `${(binary.sizeBytes / 1024 / 1024).toFixed(1)} MiB` : "—"}
          </span>
        </div>
        <p className="mt-2 text-[9px] font-mono text-muted-foreground break-all">
          {binary.hstExePath ?? "Project unavailable"}
        </p>
        {binary.mtime ? (
          <p className="mt-1 text-[9px] text-muted-foreground">Built {new Date(binary.mtime).toLocaleString()}</p>
        ) : null}
      </Panel>
    </div>
  );
}
