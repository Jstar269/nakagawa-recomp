"use client";

import { useCallback, useEffect, useMemo, useState } from "react";
import {
  Activity,
  AlertCircle,
  AlertTriangle,
  CheckCircle2,
  ChevronDown,
  ChevronRight,
  Info,
  RefreshCw,
  Search,
  Wrench,
} from "lucide-react";
import { Panel } from "./ui-bits";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Input } from "@/components/ui/input";
import { cn } from "@/lib/utils";
import type { DoctorReport, DoctorResult, DoctorStatus } from "@/lib/recompiler/doctor";

type CategoryFilter = "all" | "toolchain" | "inputs" | "products" | "runtime" | "repo";
type StatusFilter = "all" | "failing" | "warning" | "passing";

function categorizeCheck(code: string): CategoryFilter {
  if (
    code.startsWith("POWERSHELL_") ||
    code.startsWith("HOST_") ||
    code.startsWith("PYTHON_") ||
    code.startsWith("MSYS2_") ||
    code.startsWith("TOOL_") ||
    code.startsWith("SDL3_IMPORT") ||
    code.startsWith("VULKAN_") ||
    code.startsWith("GLSLC") ||
    code.startsWith("SHADER_")
  ) {
    return "toolchain";
  }
  if (code.startsWith("INPUT_") || code === "SAVE_ROOT") {
    return "inputs";
  }
  if (code.startsWith("BUILD_")) {
    return "products";
  }
  if (code.startsWith("RUNTIME_") || code.startsWith("VFPU_")) {
    return "runtime";
  }
  if (code.startsWith("REPO_") || code.startsWith("LICENSE_") || code.startsWith("NOTICE_")) {
    return "repo";
  }
  return "all";
}

interface PreflightCardProps {
  onReportLoaded?: (report: DoctorReport | null) => void;
  className?: string;
  defaultCollapsed?: boolean;
}

export function PreflightCard({ onReportLoaded, className, defaultCollapsed = false }: PreflightCardProps) {
  const [report, setReport] = useState<DoctorReport | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [category, setCategory] = useState<CategoryFilter>("all");
  const [statusFilter, setStatusFilter] = useState<StatusFilter>("all");
  const [query, setQuery] = useState("");
  const [isCollapsed, setIsCollapsed] = useState(defaultCollapsed);

  const fetchDoctor = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const res = await fetch("/api/recompiler/doctor?scope=all", { cache: "no-store" });
      const data = await res.json();
      if (!res.ok) {
        throw new Error(data?.detail ?? data?.error ?? "doctor diagnostic query failed");
      }
      setReport(data as DoctorReport);
      onReportLoaded?.(data as DoctorReport);
    } catch (err) {
      setError(String(err));
      onReportLoaded?.(null);
    } finally {
      setLoading(false);
    }
  }, [onReportLoaded]);

  useEffect(() => {
    void fetchDoctor();
  }, [fetchDoctor]);

  const filteredResults = useMemo(() => {
    if (!report) return [];
    return report.results.filter((item) => {
      if (category !== "all" && categorizeCheck(item.code) !== category) {
        return false;
      }
      if (statusFilter === "failing" && item.status !== "FAIL") return false;
      if (statusFilter === "warning" && item.status !== "WARN") return false;
      if (statusFilter === "passing" && item.status !== "PASS") return false;

      if (query.trim()) {
        const needle = query.trim().toLowerCase();
        const matchesCode = item.code.toLowerCase().includes(needle);
        const matchesSummary = item.summary.toLowerCase().includes(needle);
        const matchesDetail = item.detail?.toLowerCase().includes(needle) ?? false;
        const matchesRemediation = item.remediation?.toLowerCase().includes(needle) ?? false;
        const matchesPath = item.path?.toLowerCase().includes(needle) ?? false;
        return matchesCode || matchesSummary || matchesDetail || matchesRemediation || matchesPath;
      }
      return true;
    });
  }, [report, category, statusFilter, query]);

  const counts = report?.counts ?? { PASS: 0, WARN: 0, FAIL: 0, INFO: 0 };
  const hasFailures = counts.FAIL > 0;
  const hasWarnings = counts.WARN > 0;

  return (
    <Panel
      title="Workspace Doctor Preflight"
      description="Fail-closed preflight diagnostics for host toolchain, private game inputs, build products, and runtime dependencies."
      icon={<Wrench className="size-4" />}
      className={className}
      right={
        <div className="flex items-center gap-2">
          {report && (
            <Badge
              variant="outline"
              className={cn(
                "h-6 gap-1 text-[11px] font-medium",
                hasFailures
                  ? "border-rose-500/40 text-rose-300 bg-rose-500/10"
                  : hasWarnings
                  ? "border-amber-500/40 text-amber-300 bg-amber-500/10"
                  : "border-emerald-500/40 text-emerald-300 bg-emerald-500/10",
              )}
            >
              {hasFailures ? (
                <AlertCircle className="size-3 text-rose-400" />
              ) : hasWarnings ? (
                <AlertTriangle className="size-3 text-amber-400" />
              ) : (
                <CheckCircle2 className="size-3 text-emerald-400" />
              )}
              {hasFailures
                ? `${counts.FAIL} Action Items`
                : hasWarnings
                ? `${counts.WARN} Warnings`
                : "Preflight Passed"}
            </Badge>
          )}
          <Button
            size="sm"
            variant="outline"
            className="h-7 gap-1 text-xs"
            onClick={() => void fetchDoctor()}
            disabled={loading}
          >
            <RefreshCw className={cn("size-3", loading && "animate-spin")} />
            Refresh
          </Button>
          <Button
            size="sm"
            variant="ghost"
            className="size-7 p-0"
            onClick={() => setIsCollapsed(!isCollapsed)}
            aria-label={isCollapsed ? "Expand Preflight" : "Collapse Preflight"}
          >
            {isCollapsed ? <ChevronRight className="size-4" /> : <ChevronDown className="size-4" />}
          </Button>
        </div>
      }
    >
      {/* Counts Bar */}
      <div className="grid grid-cols-2 sm:grid-cols-4 gap-2 mb-3">
        <button
          type="button"
          onClick={() => setStatusFilter(statusFilter === "passing" ? "all" : "passing")}
          className={cn(
            "rounded-lg border p-2 text-left transition-colors",
            statusFilter === "passing"
              ? "border-emerald-500 bg-emerald-500/15"
              : "border-border/60 bg-background/40 hover:border-emerald-500/50",
          )}
        >
          <div className="text-[10px] uppercase tracking-wide text-muted-foreground flex items-center gap-1">
            <CheckCircle2 className="size-3 text-emerald-400" /> Passing
          </div>
          <div className="text-sm font-mono font-bold text-emerald-300 mt-0.5">{counts.PASS}</div>
        </button>

        <button
          type="button"
          onClick={() => setStatusFilter(statusFilter === "failing" ? "all" : "failing")}
          className={cn(
            "rounded-lg border p-2 text-left transition-colors",
            statusFilter === "failing"
              ? "border-rose-500 bg-rose-500/15"
              : "border-border/60 bg-background/40 hover:border-rose-500/50",
          )}
        >
          <div className="text-[10px] uppercase tracking-wide text-muted-foreground flex items-center gap-1">
            <AlertCircle className="size-3 text-rose-400" /> Action Required
          </div>
          <div className="text-sm font-mono font-bold text-rose-300 mt-0.5">{counts.FAIL}</div>
        </button>

        <button
          type="button"
          onClick={() => setStatusFilter(statusFilter === "warning" ? "all" : "warning")}
          className={cn(
            "rounded-lg border p-2 text-left transition-colors",
            statusFilter === "warning"
              ? "border-amber-500 bg-amber-500/15"
              : "border-border/60 bg-background/40 hover:border-amber-500/50",
          )}
        >
          <div className="text-[10px] uppercase tracking-wide text-muted-foreground flex items-center gap-1">
            <AlertTriangle className="size-3 text-amber-400" /> Warnings
          </div>
          <div className="text-sm font-mono font-bold text-amber-300 mt-0.5">{counts.WARN}</div>
        </button>

        <div className="rounded-lg border border-border/60 bg-background/40 p-2">
          <div className="text-[10px] uppercase tracking-wide text-muted-foreground flex items-center gap-1">
            <Info className="size-3 text-cyan-400" /> Detected Info
          </div>
          <div className="text-sm font-mono font-bold text-cyan-300 mt-0.5">{counts.INFO}</div>
        </div>
      </div>

      {!isCollapsed && (
        <div className="space-y-3 pt-1">
          {/* Category Tabs & Search */}
          <div className="flex flex-col sm:flex-row sm:items-center justify-between gap-2 border-b border-border/40 pb-2">
            <div className="flex flex-wrap gap-1">
              {(
                [
                  { id: "all", label: "All" },
                  { id: "toolchain", label: "Toolchain" },
                  { id: "inputs", label: "Game Inputs" },
                  { id: "products", label: "Build Output" },
                  { id: "runtime", label: "Runtime" },
                  { id: "repo", label: "Repository" },
                ] as const
              ).map((tab) => (
                <button
                  key={tab.id}
                  onClick={() => setCategory(tab.id)}
                  className={cn(
                    "px-2 py-1 rounded text-[11px] font-medium transition-colors",
                    category === tab.id
                      ? "bg-primary/20 text-primary border border-primary/30"
                      : "text-muted-foreground hover:text-foreground hover:bg-accent/40",
                  )}
                >
                  {tab.label}
                </button>
              ))}
            </div>
            <div className="relative">
              <Search className="absolute left-2 top-1/2 size-3 -translate-y-1/2 text-muted-foreground" />
              <Input
                value={query}
                onChange={(e) => setQuery(e.target.value)}
                placeholder="Filter checks…"
                className="h-7 w-36 sm:w-44 pl-6 text-[11px]"
              />
            </div>
          </div>

          {/* Results List */}
          {error ? (
            <div className="rounded-lg border border-rose-500/40 bg-rose-950/20 p-3 text-xs text-rose-300">
              {error}
            </div>
          ) : loading && !report ? (
            <div className="text-center py-6 text-xs text-muted-foreground flex items-center justify-center gap-2">
              <RefreshCw className="size-3.5 animate-spin" />
              Running workspace diagnostics (tools/hst_doctor.py)...
            </div>
          ) : filteredResults.length === 0 ? (
            <div className="text-center py-6 text-xs text-muted-foreground">
              No diagnostic checks match the current filter.
            </div>
          ) : (
            <div className="space-y-2 max-h-80 overflow-y-auto thin-scroll pr-1">
              {filteredResults.map((item, idx) => (
                <CheckResultItem key={`${item.code}-${idx}`} result={item} />
              ))}
            </div>
          )}
        </div>
      )}
    </Panel>
  );
}

function CheckResultItem({ result }: { result: DoctorResult }) {
  const [expanded, setExpanded] = useState(result.status === "FAIL");

  const badgeStyle: Record<DoctorStatus, { badge: string; text: string; icon: React.ReactNode }> = {
    PASS: {
      badge: "border-emerald-500/30 text-emerald-400 bg-emerald-500/10",
      text: "text-foreground",
      icon: <CheckCircle2 className="size-3.5 text-emerald-400 shrink-0 mt-0.5" />,
    },
    WARN: {
      badge: "border-amber-500/30 text-amber-400 bg-amber-500/10",
      text: "text-amber-200",
      icon: <AlertTriangle className="size-3.5 text-amber-400 shrink-0 mt-0.5" />,
    },
    FAIL: {
      badge: "border-rose-500/30 text-rose-400 bg-rose-500/10",
      text: "text-rose-200",
      icon: <AlertCircle className="size-3.5 text-rose-400 shrink-0 mt-0.5" />,
    },
    INFO: {
      badge: "border-cyan-500/30 text-cyan-400 bg-cyan-500/10",
      text: "text-foreground/90",
      icon: <Info className="size-3.5 text-cyan-400 shrink-0 mt-0.5" />,
    },
  };

  const style = badgeStyle[result.status] ?? badgeStyle.INFO;
  const hasExtra = Boolean(result.detail || result.remediation || result.path);

  return (
    <div
      className={cn(
        "rounded-lg border p-2.5 text-xs transition-colors",
        result.status === "FAIL"
          ? "border-rose-900/40 bg-rose-950/15"
          : result.status === "WARN"
          ? "border-amber-900/40 bg-amber-950/15"
          : "border-border/30 bg-background/30",
      )}
    >
      <div
        className={cn("flex items-start justify-between gap-2", hasExtra && "cursor-pointer")}
        onClick={() => hasExtra && setExpanded(!expanded)}
      >
        <div className="flex items-start gap-2 min-w-0">
          {style.icon}
          <div className="min-w-0">
            <div className="flex items-center gap-1.5 flex-wrap">
              <span className="font-mono text-[10px] font-semibold text-muted-foreground bg-accent/40 px-1 rounded">
                {result.code}
              </span>
              <span className={cn("font-medium", style.text)}>{result.summary}</span>
            </div>
            {result.path && (
              <div className="font-mono text-[10px] text-muted-foreground mt-0.5 truncate">
                {result.path}
              </div>
            )}
          </div>
        </div>
        <div className="flex items-center gap-1 shrink-0">
          <Badge variant="outline" className={cn("text-[9px] px-1.5 py-0 h-4 font-mono", style.badge)}>
            {result.status}
          </Badge>
          {hasExtra && (
            <button type="button" className="text-muted-foreground hover:text-foreground">
              {expanded ? <ChevronDown className="size-3.5" /> : <ChevronRight className="size-3.5" />}
            </button>
          )}
        </div>
      </div>

      {expanded && hasExtra && (
        <div className="mt-2 pt-2 border-t border-border/30 space-y-1.5 text-[11px] font-mono leading-relaxed pl-5">
          {result.detail && (
            <div className="text-muted-foreground/90">
              <span className="text-muted-foreground font-semibold font-sans">Detail: </span>
              {result.detail}
            </div>
          )}
          {result.remediation && (
            <div className="rounded bg-primary/10 border border-primary/20 p-1.5 text-primary">
              <span className="font-bold font-sans">Fix: </span>
              {result.remediation}
            </div>
          )}
        </div>
      )}
    </div>
  );
}
