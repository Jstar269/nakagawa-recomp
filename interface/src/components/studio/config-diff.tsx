"use client";

import { useCallback, useEffect, useState } from "react";
import { GitCompare, Layers } from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel } from "./ui-bits";
import { diffConfigs, CATEGORY_LABELS, type DiffEntry } from "@/lib/recompiler/diff";
import type { RecompilerConfig } from "@/lib/recompiler/types";
import { cn } from "@/lib/utils";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";

type BaselineId = "native" | string;

export function ConfigDiff() {
  const { config, native, profiles } = useStudio();
  const [baselineId, setBaselineId] = useState<BaselineId>("native");
  const [baselineConfig, setBaselineConfig] = useState<RecompilerConfig | null>(null);
  const [loading, setLoading] = useState(false);

  // Fetch the selected profile's config when baselineId changes (non-native).
  const fetchBaseline = useCallback(async (id: BaselineId) => {
    if (id === "native") {
      setBaselineConfig(null);
      return;
    }
    setLoading(true);
    try {
      const res = await fetch(`/api/recompiler/profiles/${id}`);
      if (res.ok) {
        const data = await res.json();
        setBaselineConfig(data.config as RecompilerConfig);
      }
    } catch {
      /* ignore */
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    setTimeout(() => {
      fetchBaseline(baselineId);
    }, 0);
  }, [baselineId, fetchBaseline]);

  const baseline = baselineId === "native" ? native : baselineConfig;
  const baselineLabel =
    baselineId === "native"
      ? "PSP native"
      : profiles.find((p) => p.id === baselineId)?.name ?? "profile";

  const entries = baseline ? diffConfigs(config, baseline) : [];
  const changed = entries.filter((e) => e.changed);
  const byCategory = entries.reduce<Record<string, DiffEntry[]>>((acc, e) => {
    (acc[e.category] ??= []).push(e);
    return acc;
  }, {});

  return (
    <Panel
      title="Config diff"
      description={`Compare current config against a baseline`}
      icon={<GitCompare className="size-4" />}
      right={
        <Select value={baselineId} onValueChange={(v) => setBaselineId(v as BaselineId)}>
          <SelectTrigger className="h-7 w-[150px] text-[11px]">
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            <SelectItem value="native" className="text-xs">
              PSP native
            </SelectItem>
            {profiles.map((p) => (
              <SelectItem key={p.id} value={p.id} className="text-xs">
                {p.name}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
      }
    >
      {loading ? (
        <div className="flex items-center gap-2 rounded-lg bg-muted/20 border border-border/50 px-3 py-2.5">
          <Layers className="size-3.5 text-muted-foreground animate-pulse" />
          <span className="text-xs text-muted-foreground">Loading baseline…</span>
        </div>
      ) : changed.length === 0 ? (
        <div className="flex items-center gap-2 rounded-lg bg-emerald-500/10 border border-emerald-500/20 px-3 py-2.5">
          <span className="text-xs text-emerald-300">
            Identical — current config matches {baselineLabel}.
          </span>
        </div>
      ) : (
        <>
          <div className="text-[10px] text-muted-foreground mb-2">
            <span className="font-semibold text-ball">{changed.length}</span> setting
            {changed.length === 1 ? "" : "s"} differ from{" "}
            <span className="font-medium">{baselineLabel}</span>
          </div>
          <div className="space-y-3">
            {Object.entries(byCategory).map(([cat, list]) => {
              const catChanged = list.filter((e) => e.changed);
              if (catChanged.length === 0) return null;
              return (
                <div key={cat}>
                  <div className="text-[10px] uppercase tracking-wide text-muted-foreground mb-1.5 font-semibold">
                    {CATEGORY_LABELS[cat as DiffEntry["category"]]}
                  </div>
                  <div className="space-y-1">
                    {list.map((e) => (
                      <div
                        key={e.path}
                        className={cn(
                          "flex items-center gap-2 text-[11px] rounded-md px-2 py-1.5 border transition-colors",
                          e.changed
                            ? "bg-primary/5 border-primary/20"
                            : "bg-background/20 border-border/40 opacity-50",
                        )}
                      >
                        <span className="text-muted-foreground w-28 shrink-0 truncate">
                          {e.label}
                        </span>
                        <span className="font-mono text-muted-foreground/70 line-through truncate flex-1 text-right">
                          {e.from}
                        </span>
                        <span className="text-muted-foreground/50 shrink-0">→</span>
                        <span
                          className={cn(
                            "font-mono truncate flex-1 shrink-0",
                            e.changed ? "text-ball" : "",
                          )}
                        >
                          {e.to}
                        </span>
                      </div>
                    ))}
                  </div>
                </div>
              );
            })}
          </div>
        </>
      )}
    </Panel>
  );
}
