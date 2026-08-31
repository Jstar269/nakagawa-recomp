"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import { Activity, Binary, CircleCheck, CircleX, RefreshCw, Wrench } from "lucide-react";
import { Panel } from "./ui-bits";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { useStudio } from "./studio-context";
import type { DoctorReport } from "@/lib/recompiler/doctor";

type DoctorLoadState = "loading" | "ready" | "unavailable";
type DoctorSummaryKind = "loading" | "unavailable" | "failure" | "warning" | "success";
type DoctorSummary = { kind: DoctorSummaryKind; label: string; detail: string };

const DOCTOR_STATUSES = new Set(["PASS", "WARN", "FAIL", "INFO"]);
const DOCTOR_SCOPES = new Set(["repo", "inputs", "build", "products", "run", "all"]);
const DOCTOR_COUNT_KEYS = ["PASS", "WARN", "FAIL", "INFO"] as const;

function isDoctorReportPayload(value: unknown): value is DoctorReport {
  if (value === null || typeof value !== "object" || Array.isArray(value)) return false;
  const payload = value as Record<string, unknown>;
  const counts = payload.counts;
  const results = payload.results;
  if (
    payload.schema_version !== 1 ||
    payload.tool !== "hst_doctor" ||
    typeof payload.root !== "string" ||
    typeof payload.scope !== "string" ||
    !DOCTOR_SCOPES.has(payload.scope) ||
    typeof payload.strict !== "boolean" ||
    typeof payload.exit_code !== "number" ||
    !Number.isInteger(payload.exit_code) ||
    ![0, 1, 2].includes(payload.exit_code) ||
    counts === null ||
    typeof counts !== "object" ||
    Array.isArray(counts) ||
    !Array.isArray(results)
  ) {
    return false;
  }

  const actualCounts = { PASS: 0, WARN: 0, FAIL: 0, INFO: 0 };
  for (const key of DOCTOR_COUNT_KEYS) {
    const count = (counts as Record<string, unknown>)[key];
    if (typeof count !== "number" || !Number.isInteger(count) || count < 0) return false;
  }
  for (const result of results) {
    if (
      result === null ||
      typeof result !== "object" ||
      Array.isArray(result) ||
      typeof (result as Record<string, unknown>).status !== "string" ||
      !DOCTOR_STATUSES.has((result as Record<string, unknown>).status as string) ||
      typeof (result as Record<string, unknown>).code !== "string" ||
      typeof (result as Record<string, unknown>).summary !== "string"
    ) {
      return false;
    }
    actualCounts[(result as Record<string, unknown>).status as keyof typeof actualCounts] += 1;
  }
  const countsRecord = counts as Record<string, unknown>;
  const expectedExitCode = (countsRecord.FAIL as number) > 0
    ? 1
    : payload.strict && (countsRecord.WARN as number) > 0
    ? 2
    : 0;
  return DOCTOR_COUNT_KEYS.every(
    (key) => (counts as Record<string, unknown>)[key] === actualCounts[key],
  ) && payload.exit_code === expectedExitCode;
}

export function summarizeDoctorReport(report: DoctorReport | null, state: DoctorLoadState): DoctorSummary {
  if (state === "loading") {
    return { kind: "loading", label: "CHECKING…", detail: "Doctor check in progress" };
  }
  if (state !== "ready" || !report || !isDoctorReportPayload(report)) {
    return { kind: "unavailable", label: "UNAVAILABLE", detail: "Doctor unavailable — retrying" };
  }
  if (report.counts.FAIL > 0) {
    return {
      kind: "failure",
      label: `${report.counts.FAIL} FAILURES`,
      detail: `${report.counts.PASS} pass · ${report.counts.FAIL} fail`,
    };
  }
  if (report.counts.WARN > 0) {
    return {
      kind: "warning",
      label: `${report.counts.WARN} WARNINGS`,
      detail: `${report.counts.PASS} pass · ${report.counts.FAIL} fail`,
    };
  }
  return {
    kind: "success",
    label: "ALL PASS",
    detail: `${report.counts.PASS} pass · ${report.counts.FAIL} fail`,
  };
}

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
  const [doctorState, setDoctorState] = useState<DoctorLoadState>("loading");
  const refreshSequence = useRef(0);

  const refresh = useCallback(async () => {
    const requestId = ++refreshSequence.current;
    setDoctor(null);
    setDoctorState("loading");
    const [bootResponse, binaryResponse, doctorResponse] = await Promise.all([
      fetch("/api/recompiler/boot", { cache: "no-store" }).catch(() => null),
      fetch("/api/recompiler/run", { cache: "no-store" }).catch(() => null),
      fetch("/api/recompiler/doctor?scope=all", { cache: "no-store" }).catch(() => null),
    ]);
    if (requestId !== refreshSequence.current) return;
    if (doctorResponse?.ok) {
      try {
        const data = await doctorResponse.json();
        if (!isDoctorReportPayload(data)) throw new Error("invalid doctor response payload");
        setDoctor(data);
        setDoctorState("ready");
      } catch {
        setDoctorState("unavailable");
      }
    } else {
      setDoctorState("unavailable");
    }
    if (bootResponse?.ok) setBoot(await bootResponse.json());
    if (binaryResponse?.ok) setBinary(await binaryResponse.json());
  }, []);

  useEffect(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), 4000);
    return () => window.clearInterval(timer);
  }, [refresh]);

  const milestones = ["image_loaded", "runtime_registered", "window_ready", "guest_start", "display_flip"];
  const doctorSummary = summarizeDoctorReport(doctor, doctorState);
  const doctorBadgeClass =
    doctorSummary.kind === "failure" || doctorSummary.kind === "unavailable"
      ? "border-rose-500/40 text-rose-400 bg-rose-500/10 text-[10px]"
      : doctorSummary.kind === "warning" || doctorSummary.kind === "loading"
      ? "border-amber-500/40 text-amber-400 bg-amber-500/10 text-[10px]"
      : "border-emerald-500/40 text-emerald-400 bg-emerald-500/10 text-[10px]";

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
            className={doctorBadgeClass}
          >
            {doctorSummary.label}
          </Badge>
          <span className="text-[10px] font-mono text-muted-foreground">
            {doctorSummary.detail}
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
