"use client";

import { useEffect, useState, useMemo } from "react";
import { BarChart3, TrendingUp, TrendingDown, Clock, Target, RefreshCcw, Loader2 } from "lucide-react";
import { AreaChart, Area, XAxis, YAxis, CartesianGrid, Tooltip, Legend, ResponsiveContainer } from "recharts";
import { Panel, SectionHeader } from "./ui-bits";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { PROGRESS_PHASES, PROGRESS_TOTAL } from "@/lib/recompiler/real-data";
import { cn } from "@/lib/utils";

/* LiveProgress shape: what /api/recompiler/progress returns.
 * The real progress.json stores phases keyed by a numeric id; PROGRESS_PHASES
 * uses string keys ("P1"..). We map between the two by sorting instead of
 * joining — the API returns whatever the file has, and we display them regardless. */
type LiveProgress = {
  total: number;
  earned: number;
  regressed: number;
  percent: number;
  phases: { id: number | string; title: string; earned: number; pending: number; regressed: number }[];
  fresh: boolean;
  generatedAt: number;
  /* #181: provenance surfaced by progress_tracker.py via progress.json. */
  evidenceGrade?: string | null;
  staleVsBuild?: boolean | null;
  runIdentity?: { sourceCommit: string; binarySha256: string; profileSha256: string; generatedAt: string } | null;
  evidenceSummary?: { contentValidated: number; executed: number; heuristic: number; unknown: number; stale: number };
  itemsCarryEvidence?: boolean;
};

const EVIDENCE_GRADE_STYLES: Record<string, string> = {
  "content-validated": "border-emerald-500/30 text-emerald-300 bg-emerald-500/10",
  executed: "border-sky-500/30 text-sky-300 bg-sky-500/10",
  heuristic: "border-violet-500/30 text-violet-300 bg-violet-500/10",
  unknown: "border-muted-foreground/30 text-muted-foreground bg-muted/10",
  stale: "border-red-500/30 text-red-300 bg-red-500/10",
};

export function ProgressPanel() {
  const [live, setLive] = useState<LiveProgress | null>(null);
  const [refreshState, setRefreshState] = useState<"idle" | "fetching" | "error" | "unanchored">("idle");
  const [verifyState, setVerifyState] = useState<string | null>(null);
  const [telemetry, setTelemetry] = useState<any[]>([]);
  const [mounted, setMounted] = useState(false);

  useEffect(() => {
    setMounted(true);
    refresh();
    fetchTelemetry();
  }, []);

  async function fetchTelemetry() {
    try {
      const res = await fetch("/api/recompiler/telemetry");
      if (res.ok) {
        const d = await res.json();
        setTelemetry(d.telemetry || []);
        // Issue #189: no implicit database synchronization merely because data
        // is absent.  Recording a telemetry snapshot is an explicit user action
        // (the "Record Telemetry Snapshot" button in the build-health panel),
        // never an automatic side effect of rendering this panel.
      }
    } catch (e) {
      /* ignore */
    }
  }

  const chartData = useMemo(() => {
    return telemetry.map(t => {
      const d = new Date(t.timestamp);
      return {
        date: d.toLocaleDateString(undefined, { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" }),
        pipelinePct: t.completionPct,
        decompPct: t.byteCompletionPct ? t.byteCompletionPct * 100 : 0,
      };
    });
  }, [telemetry]);

  async function refresh() {
    setRefreshState("fetching");
    try {
      const r = await fetch("/api/recompiler/progress");
      const d = await r.json();
      if (!r.ok) { setRefreshState(d?.error === "progress-missing" ? "unanchored" : "error"); setLive(null); return; }
      setLive(d);
      setRefreshState("idle");
    } catch (e) {
      setRefreshState("error");
    }
  }

  async function runVerify() {
    setVerifyState("running python tools/progress_tracker.py verify…");
    try {
      const r = await fetch("/api/recompiler/progress", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ action: "verify" }) });
      const d = await r.json();
      setVerifyState(`exit=${d.ok ? 0 : 1}\n${(d.stdout || d.stderr || "").slice(-2000)}`);
      await refresh();
    } catch (e) {
      setVerifyState(`error: ${String(e)}`);
    }
  }

  /* Use live when present, else fall back to baked PROGRESS_TOTAL so the layout
   * still renders cleanly when the studio isn't anchored to the repo. */
  const total = live
    ? { pct: live.percent, earned: live.earned, regressed: live.regressed, total: live.total }
    : null;
  const totalPct = total?.pct ?? PROGRESS_TOTAL.pct;
  const earned = total?.earned ?? PROGRESS_TOTAL.earned;
  const regressed = total?.regressed ?? PROGRESS_TOTAL.regressed;
  const totalN = total?.total ?? PROGRESS_TOTAL.total;

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<BarChart3 className="size-4.5" />}
        title="Progress Tracker"
        subtitle="Live measurements from progress.json when available; otherwise phases are shown as unmeasured."
        right={
          <div className="flex items-center gap-2">
            {live?.fresh === false ? (
              <Badge variant="outline" className="text-[10px] h-6 text-amber-300 border-amber-500/30">
                progress.json stale
              </Badge>
            ) : live ? (
              <Badge variant="outline" className="text-[10px] h-6 text-emerald-300 border-emerald-500/30">
                live · progress.json
              </Badge>
            ) : (
              <Badge variant="outline" className="text-[10px] h-6 text-muted-foreground">
                {refreshState === "unanchored" ? "not anchored" : refreshState === "error" ? "fetch error" : "baked snapshot"}
              </Badge>
            )}
            <Button size="sm" variant="outline" className="h-7 gap-1.5" onClick={refresh}>
              {refreshState === "fetching" ? <Loader2 className="size-3.5 animate-spin" /> : <RefreshCcw className="size-3.5" />}
              Refresh
            </Button>
            <Button size="sm" variant="outline" className="h-7 gap-1.5" onClick={runVerify} disabled={!!verifyState}>
              <RefreshCcw className="size-3.5" /> Run verify
            </Button>
          </div>
        }
      />

      <Panel
        title="Overall score"
        description={`${earned} earned − ${regressed} regressed = ${earned - regressed} net / ${totalN} total${live ? ` · read ${new Date(live.generatedAt).toLocaleTimeString()}` : " · baked"}`}
        icon={<Target className="size-4" />}
      >
        {verifyState ? (
          <div className="rounded-lg border border-border/60 bg-black/40 p-3 font-mono text-[11px] leading-relaxed mb-3 max-h-44 overflow-y-auto thin-scroll">
            {verifyState.split("\n").map((l, i) => <div key={i}>{l}</div>)}
          </div>
        ) : null}
        <div className="flex items-baseline gap-4 mb-3">
          <div>
            <div className="text-4xl font-bold text-ball font-mono leading-none">
              {totalPct.toFixed(2)}%
            </div>
            <div className="text-[10px] text-muted-foreground uppercase tracking-wide mt-1">
              complete
            </div>
          </div>
          <div className="flex gap-3 ml-auto text-right">
            <div>
              <div className="text-lg font-semibold font-mono text-emerald-400">{earned}</div>
              <div className="text-[9px] text-muted-foreground uppercase">earned</div>
            </div>
            <div>
              <div className="text-lg font-semibold font-mono text-amber-400">−{regressed}</div>
              <div className="text-[9px] text-muted-foreground uppercase">regressed</div>
            </div>
            <div>
              <div className="text-lg font-semibold font-mono">{totalN}</div>
              <div className="text-[9px] text-muted-foreground uppercase">total</div>
            </div>
          </div>
        </div>

        <div className="relative h-3 rounded-full bg-muted/30 overflow-hidden">
          <div
            className="absolute inset-y-0 left-0 bg-emerald-500/60"
            style={{ width: `${(earned / totalN) * 100}%` }}
          />
          <div
            className="absolute inset-y-0 bg-amber-500/50"
            style={{
              left: `${(earned / totalN) * 100}%`,
              width: `${(regressed / totalN) * 100}%`,
            }}
          />
        </div>
        <div className="flex justify-between text-[9px] font-mono text-muted-foreground mt-1">
          <span className="text-emerald-400">earned</span>
          <span className="text-amber-400">regressed</span>
          <span>pending</span>
        </div>
      </Panel>

      {/* #181: run-level provenance — only rendered when progress.json carries it. */}
      {live && (live.evidenceGrade || live.runIdentity || live.staleVsBuild === true) && (
        <Panel
          title="Run evidence"
          description="Revision/run binding recorded by progress_tracker.py at emit time."
          icon={<Clock className="size-4" />}
        >
          <div className="space-y-2.5 text-[11px]">
            {live.evidenceGrade && (
              <div className="flex items-center gap-2">
                <span className="text-[10px] text-muted-foreground uppercase tracking-wide w-24 shrink-0">grade</span>
                <Badge
                  variant="outline"
                  className={cn(
                    "text-[9px] h-5 px-1.5 font-mono shrink-0",
                    EVIDENCE_GRADE_STYLES[live.evidenceGrade] ?? EVIDENCE_GRADE_STYLES.unknown,
                  )}
                >
                  {live.evidenceGrade}
                </Badge>
                {live.staleVsBuild === true && (
                  <Badge variant="outline" className="text-[9px] h-5 px-1.5 font-mono shrink-0 border-red-500/30 text-red-300 bg-red-500/10">
                    stale vs build — not current proof
                  </Badge>
                )}
              </div>
            )}
            {live.runIdentity && (
              <div className="flex flex-wrap gap-x-5 gap-y-1 font-mono text-[10px] text-muted-foreground">
                {live.runIdentity.sourceCommit && (
                  <span>source <span className="text-foreground">{live.runIdentity.sourceCommit.slice(0, 10)}</span></span>
                )}
                {live.runIdentity.binarySha256 && (
                  <span>binary <span className="text-foreground">{live.runIdentity.binarySha256.slice(0, 12)}…</span></span>
                )}
                {live.runIdentity.generatedAt && (
                  <span>emitted <span className="text-foreground">{live.runIdentity.generatedAt}</span></span>
                )}
              </div>
            )}
            {live.evidenceSummary && live.itemsCarryEvidence === true && (
              <div className="flex flex-wrap gap-x-4 gap-y-1 text-[10px]">
                <span className="text-muted-foreground">per-item evidence:</span>
                <span className="text-emerald-400 font-mono">content-validated {live.evidenceSummary.contentValidated}</span>
                <span className="text-sky-400 font-mono">executed {live.evidenceSummary.executed}</span>
                <span className="text-violet-400 font-mono">heuristic {live.evidenceSummary.heuristic}</span>
                <span className="text-muted-foreground font-mono">unknown {live.evidenceSummary.unknown}</span>
                {live.evidenceSummary.stale > 0 && (
                  <span className="text-red-400 font-mono">stale {live.evidenceSummary.stale}</span>
                )}
              </div>
            )}
            {live.itemsCarryEvidence === false && live.evidenceGrade && (
              <p className="text-[10px] text-muted-foreground leading-snug">
                Run-level grade is bound, but individual units in this snapshot do not carry per-item evidence
                grades yet — treat unit counts as ungraded observations.
              </p>
            )}
          </div>
        </Panel>
      )}

      {/* Chronological Telemetry & Performance Charts */}
      {mounted && telemetry.length > 0 && (
        <Panel
          title="Chronological Telemetry & Performance Charts"
          description="Chronological analysis of the static compiler pipeline and MIPS decompilation byte-matching progress over time."
          icon={<TrendingUp className="size-4" />}
        >
          {telemetry.length < 2 ? (
            <div className="flex flex-col items-center justify-center p-8 text-center text-muted-foreground text-xs italic">
              Awaiting more telemetry data points to chart chronological progress. Run compilation builds or verify tests to log data points.
            </div>
          ) : (
            <div className="h-64 w-full text-xs mt-2">
              <ResponsiveContainer width="100%" height="100%">
                <AreaChart data={chartData} margin={{ top: 10, right: 10, left: -20, bottom: 0 }}>
                  <defs>
                    <linearGradient id="colorPipeline" x1="0" y1="0" x2="0" y2="1">
                      <stop offset="5%" stopColor="#10b981" stopOpacity={0.2}/>
                      <stop offset="95%" stopColor="#10b981" stopOpacity={0}/>
                    </linearGradient>
                    <linearGradient id="colorDecomp" x1="0" y1="0" x2="0" y2="1">
                      <stop offset="5%" stopColor="#0ea5e9" stopOpacity={0.2}/>
                      <stop offset="95%" stopColor="#0ea5e9" stopOpacity={0}/>
                    </linearGradient>
                  </defs>
                  <CartesianGrid strokeDasharray="3 3" stroke="#2a2a2a" />
                  <XAxis dataKey="date" stroke="#888" tickLine={false} />
                  <YAxis stroke="#888" domain={[0, 100]} tickLine={false} />
                  <Tooltip
                    contentStyle={{ backgroundColor: "#151515", borderColor: "#333", color: "#fff" }}
                    labelStyle={{ color: "#aaa" }}
                  />
                  <Legend />
                  <Area type="monotone" name="Pipeline Complete %" dataKey="pipelinePct" stroke="#10b981" fillOpacity={1} fill="url(#colorPipeline)" strokeWidth={2} />
                  <Area type="monotone" name="Decompilation Complete %" dataKey="decompPct" stroke="#0ea5e9" fillOpacity={1} fill="url(#colorDecomp)" strokeWidth={2} />
                </AreaChart>
              </ResponsiveContainer>
            </div>
          )}
        </Panel>
      )}

      <Panel
        title="Phase breakdown"
        description="7 phases from pipeline to endgame"
        icon={<BarChart3 className="size-4" />}
      >
        <div className="space-y-3">
          {/* Phase names are baked; counts come only from a live progress.json. */}
          {PROGRESS_PHASES.map((phase) => {
            const phaseIdInt = parseInt(String(phase.id).replace("P", ""), 10);
            const phaseLive = Number.isFinite(phaseIdInt)
              ? live?.phases?.find((p) => Number(p.id) === phaseIdInt)
              : undefined;
            const earnedN = phaseLive?.earned ?? phase.earned;
            const regressedN = phaseLive?.regressed ?? phase.regressed;
            const denom = phaseLive ? Math.max(1, phase.total) : 0;
            const pct = denom > 0 ? Math.min(100, (earnedN / denom) * 100) : 0;
            const isRegressed = regressedN > 0;
            return (
              <div key={phase.id}>
                <div className="flex items-center gap-2 mb-1">
                  <Badge
                    variant="outline"
                    className={cn(
                      "text-[9px] h-5 px-1.5 font-mono font-bold shrink-0",
                      isRegressed
                        ? "border-amber-500/30 text-amber-300 bg-amber-500/10"
                        : (phase.total - earnedN) > 0
                          ? "border-amber-500/30 text-amber-300 bg-amber-500/10"
                          : "border-emerald-500/30 text-emerald-300 bg-emerald-500/10",
                    )}
                  >
                    {phase.id}
                  </Badge>
                  <span className="text-xs font-semibold">{phase.name}</span>
                  <span className="text-[10px] font-mono text-muted-foreground ml-auto">
                    {phaseLive ? `${earnedN}/${denom}` : "unmeasured"}
                  </span>
                  {isRegressed ? (
                    <span className="inline-flex items-center gap-0.5 text-[9px] text-amber-400 font-mono">
                      <TrendingDown className="size-2.5" />−{regressedN}
                    </span>
                  ) : null}
                  {phaseLive ? (
                    <span className="text-[8px] text-emerald-300/70 font-mono">live</span>
                  ) : null}
                </div>
                <p className="text-[10px] text-muted-foreground mb-1.5 leading-snug">
                  {phase.description}
                </p>
                <div className="h-1.5 rounded-full bg-muted/30 overflow-hidden">
                  <div
                    className={cn(
                      "h-full rounded-full transition-all",
                      isRegressed ? "bg-amber-500/50" : "bg-emerald-500/60",
                    )}
                    style={{ width: `${pct}%` }}
                  />
                </div>
              </div>
            );
          })}

          {live?.phases?.filter((p) => {
            const intId = Number(p.id);
            return !PROGRESS_PHASES.some((pp) => {
              const ppId = parseInt(String(pp.id).replace("P", ""), 10);
              return Number.isFinite(ppId) && ppId === intId && false;
            });
          }).map((phase) => {
            const denom = Math.max(1, phase.earned + phase.pending + phase.regressed);
            const pct = (phase.earned / denom) * 100;
            const isRegressed = phase.regressed > 0;
            return (
              <div key={`live-${String(phase.id)}`}>
                <div className="flex items-center gap-2 mb-1">
                  <Badge variant="outline" className="text-[9px] h-5 px-1.5 font-mono font-bold shrink-0 border-emerald-500/30 text-emerald-300 bg-emerald-500/10">
                    P{phase.id}
                  </Badge>
                  <span className="text-xs font-semibold">{phase.title}</span>
                  <span className="text-[10px] font-mono text-muted-foreground ml-auto">
                    {phase.earned}/{denom}
                  </span>
                  {isRegressed ? (
                    <span className="inline-flex items-center gap-0.5 text-[9px] text-amber-400 font-mono">
                      <TrendingDown className="size-2.5" />−{phase.regressed}
                    </span>
                  ) : null}
                  <span className="text-[8px] text-emerald-300/70 font-mono">live</span>
                </div>
                <div className="h-1.5 rounded-full bg-muted/30 overflow-hidden">
                  <div className={cn("h-full rounded-full transition-all", isRegressed ? "bg-amber-500/50" : "bg-emerald-500/60")} style={{ width: `${pct}%` }} />
                </div>
              </div>
            );
          })}
        </div>
      </Panel>

      <Panel
        title="Methodology"
        description="What counts as verified / regressed / pending"
        icon={<Clock className="size-4" />}
      >
        <div className="space-y-2 text-[11px]">
          <div className="flex items-start gap-2">
            <Badge variant="outline" className="text-[8px] h-4 px-1 border-emerald-500/30 text-emerald-300 bg-emerald-500/10 shrink-0 mt-0.5">
              verified
            </Badge>
            <span className="text-muted-foreground">
              Conferred only when a measurement exists in a runtime log. Speculation gets <code className="font-mono text-foreground">pending</code>.
            </span>
          </div>
          <div className="flex items-start gap-2">
            <Badge variant="outline" className="text-[8px] h-4 px-1 border-amber-500/30 text-amber-300 bg-amber-500/10 shrink-0 mt-0.5">
              regressed
            </Badge>
            <span className="text-muted-foreground">
              Prior state was nearer to a working outcome than current, OR running the current build produces a logically worse output than a frozen-sane baseline.
            </span>
          </div>
          <div className="flex items-start gap-2">
            <Badge variant="outline" className="text-[8px] h-4 px-1 border-amber-500/30 text-amber-300 bg-amber-500/10 shrink-0 mt-0.5">
              pending
            </Badge>
            <span className="text-muted-foreground">
              Assessment blocked by an upstream gate. A known unknown, awaiting evidence — not a guess.
            </span>
          </div>
        </div>
        <div className="mt-3 rounded-lg bg-primary/5 border border-primary/20 px-2.5 py-2">
          <p className="text-[10px] text-muted-foreground leading-snug">
            <span className="inline-flex items-center gap-1 text-primary font-medium">
              <TrendingUp className="size-3" /> Notable:
            </span>{" "}
            WALKER_CAP fix (permanent → per-call reset) reduced spurious dispatches to stub
            <code className="font-mono"> f_0005a648</code> from ~200,000 to zero. PLT_MISS cascade
            dropped from 5–9/run to 0.
          </p>
        </div>
      </Panel>
    </div>
  );
}
