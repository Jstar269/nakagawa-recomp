"use client";

import { useCallback, useEffect, useMemo, useState } from "react";
import { AlertTriangle, FileText, RefreshCw, Search, ShieldCheck } from "lucide-react";
import { Panel, SectionHeader } from "./ui-bits";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";

type IssuesResponse = {
  source: string;
  content: string;
  updatedAt: string;
};

export function TroubleshootingPanel() {
  const [issues, setIssues] = useState<IssuesResponse | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [query, setQuery] = useState("");
  const [loading, setLoading] = useState(true);

  const refresh = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const response = await fetch("/api/recompiler/issues", { cache: "no-store" });
      const data = await response.json();
      if (!response.ok) throw new Error(data?.detail ?? data?.error ?? "failed to read ISSUES.md");
      setIssues(data as IssuesResponse);
    } catch (reason) {
      setError(String(reason));
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const visibleText = useMemo(() => {
    if (!issues || !query.trim()) return issues?.content ?? "";
    const needle = query.trim().toLowerCase();
    return issues.content
      .split(/\r?\n/)
      .filter((line) => line.toLowerCase().includes(needle))
      .join("\n");
  }, [issues, query]);

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<AlertTriangle className="size-4.5" />}
        title="Troubleshooting"
        subtitle="Live view of the repository's authoritative ISSUES.md; no duplicated fix order or cached root-cause narrative."
        right={
          <Button size="sm" variant="outline" className="h-8 gap-1.5" onClick={() => void refresh()} disabled={loading}>
            <RefreshCw className={loading ? "size-3.5 animate-spin" : "size-3.5"} /> Refresh
          </Button>
        }
      />

      <Panel
        title="Repository issue tracker"
        description={issues ? `${issues.source} · updated ${new Date(issues.updatedAt).toLocaleString()}` : "Reading ISSUES.md"}
        icon={<FileText className="size-4" />}
        right={
          <div className="relative">
            <Search className="absolute left-2 top-1/2 size-3.5 -translate-y-1/2 text-muted-foreground" />
            <Input
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              placeholder="Filter lines…"
              className="h-7 w-40 pl-7 text-[11px]"
            />
          </div>
        }
      >
        {error ? (
          <div className="rounded-lg border border-destructive/40 bg-destructive/10 p-3 text-xs text-destructive">
            {error}
          </div>
        ) : (
          <pre className="max-h-[34rem] overflow-auto whitespace-pre-wrap rounded-lg border border-border/60 bg-black/40 p-3 font-mono text-[10px] leading-relaxed text-foreground/85 thin-scroll">
            {loading && !issues ? "Loading ISSUES.md…" : visibleText || "No matching lines."}
          </pre>
        )}
      </Panel>

      <Panel title="Debugging guardrails" icon={<ShieldCheck className="size-4" />}>
        <ul className="space-y-2 text-[11px] text-muted-foreground">
          <li>Use the native manager profiles for ordinary runs; the dashboard does not accept arbitrary environment overrides.</li>
          <li><code className="font-mono text-foreground">SR_*</code> diagnostic switches are presence-based, so a value of <code className="font-mono text-foreground">0</code> may still enable a switch.</li>
          <li>Native framebuffer captures are PPM files. The dashboard converts them to PNG only for browser display.</li>
          <li>Generated <code className="font-mono text-foreground">build/hst/hst_recomp_*.c</code> chunks must be regenerated through the pipeline, never edited by hand.</li>
        </ul>
      </Panel>
    </div>
  );
}
