"use client";

import React, { useState, useEffect } from "react";
import { CheckCircle2, ShieldAlert, XCircle, Search, RefreshCw, BarChart2, Compass, AlertCircle } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Panel } from "./ui-bits";

interface NidInfo {
  nid_hex: string;
  name: string;
  lib: string;
  status: "resolved" | "stubbed" | "unmapped";
  handler: string | null;
  stub: string;
  registers: string[];
  logging: string;
  flags: string[];
}

interface ModuleInfo {
  name: string;
  resolved: number;
  stubbed: number;
  unmapped: number;
  total: number;
  coverage_pct: number;
  implemented_pct: number;
}

interface AuditSummary {
  total_imports: number;
  resolved: number;
  stubbed: number;
  unmapped: number;
  coverage_pct: number;
  implemented_pct: number;
}

export function NidAuditPanel() {
  const [data, setData] = useState<{ summary: AuditSummary; modules: ModuleInfo[]; nids: NidInfo[] } | null>(null);
  const [loading, setLoading] = useState(true);
  const [search, setSearch] = useState("");
  const [statusFilter, setStatusFilter] = useState<"all" | "resolved" | "stubbed" | "unmapped">("all");
  const [selectedNid, setSelectedNid] = useState<NidInfo | null>(null);
  const [selectedModule, setSelectedModule] = useState<string | null>(null);

  const fetchAudit = async () => {
    setLoading(true);
    try {
      const res = await fetch("/api/recompiler/debug/nid-audit");
      const d = await res.json();
      setData(d);
      if (d.nids && d.nids.length > 0) {
        setSelectedNid(d.nids[0]);
      }
    } catch (e) {
      console.error(e);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    const initial = setTimeout(fetchAudit, 0);
    const interval = setInterval(fetchAudit, 3000);
    return () => {
      clearTimeout(initial);
      clearInterval(interval);
    };
  }, []);

  if (loading) {
    return (
      <div className="flex h-64 items-center justify-center font-mono text-sm text-muted-foreground animate-pulse">
        Running HLE kernel NID coverage audit...
      </div>
    );
  }

  if (!data) {
    return (
      <div className="p-8 text-center text-amber-500 font-mono text-xs border border-amber-500/20 rounded-xl bg-amber-500/5">
        Failed to execute NID compliance audit. Verify build/hst/hst_imports.toml exists.
      </div>
    );
  }

  const { summary, modules, nids } = data;

  // Filter modules
  const filteredNids = nids.filter((n) => {
    const matchesSearch =
      n.name.toLowerCase().includes(search.toLowerCase()) ||
      n.nid_hex.toLowerCase().includes(search.toLowerCase()) ||
      n.lib.toLowerCase().includes(search.toLowerCase()) ||
      (n.handler && n.handler.toLowerCase().includes(search.toLowerCase()));

    const matchesStatus = statusFilter === "all" || n.status === statusFilter;
    const matchesModule = !selectedModule || n.lib === selectedModule;

    return matchesSearch && matchesStatus && matchesModule;
  });

  const getStatusBadge = (status: "resolved" | "stubbed" | "unmapped") => {
    if (status === "resolved") {
      return (
        <span className="inline-flex items-center gap-1 text-[10px] px-2 py-0.5 rounded font-mono font-semibold bg-emerald-500/10 text-emerald-500 border border-emerald-500/25">
          <CheckCircle2 className="size-3" /> RESOLVED
        </span>
      );
    } else if (status === "stubbed") {
      return (
        <span className="inline-flex items-center gap-1 text-[10px] px-2 py-0.5 rounded font-mono font-semibold bg-amber-500/10 text-amber-500 border border-amber-500/25">
          <ShieldAlert className="size-3" /> STUBBED
        </span>
      );
    } else {
      return (
        <span className="inline-flex items-center gap-1 text-[10px] px-2 py-0.5 rounded font-mono font-semibold bg-neutral-500/10 text-neutral-400 border border-neutral-500/25">
          <XCircle className="size-3" /> UNMAPPED
        </span>
      );
    }
  };

  return (
    <div className="space-y-4">
      {/* 1. Global Compliance Metrics */}
      <Panel
        title="Automated NID Compliance Audit"
        icon={<BarChart2 className="size-4" />}
        right={
          <Button variant="outline" size="sm" className="h-7 px-2 gap-1 text-xs" onClick={fetchAudit}>
            <RefreshCw className="size-3" /> Run Audit
          </Button>
        }
      >
        <div className="grid grid-cols-1 md:grid-cols-4 gap-4">
          <div className="space-y-1">
            <span className="text-[10px] font-semibold text-muted-foreground uppercase tracking-wide">Total Imports Found</span>
            <div className="text-2xl font-bold font-mono">{summary.total_imports}</div>
            <p className="text-[10px] text-muted-foreground leading-normal">Registered in hst_imports.toml</p>
          </div>

          <div className="space-y-1">
            <span className="text-[10px] font-semibold text-muted-foreground uppercase tracking-wide">Coverage (Bit-Perfect)</span>
            <div className="text-2xl font-bold font-mono text-emerald-500">{summary.coverage_pct}%</div>
            <div className="w-full bg-neutral-800 h-1.5 rounded-full overflow-hidden">
              <div className="bg-emerald-500 h-full" style={{ width: `${summary.coverage_pct}%` }} />
            </div>
          </div>

          <div className="space-y-1">
            <span className="text-[10px] font-semibold text-muted-foreground uppercase tracking-wide">Total Implemented</span>
            <div className="text-2xl font-bold font-mono text-amber-500">{summary.implemented_pct}%</div>
            <div className="w-full bg-neutral-800 h-1.5 rounded-full overflow-hidden">
              <div className="bg-amber-500 h-full" style={{ width: `${summary.implemented_pct}%` }} />
            </div>
          </div>

          <div className="space-y-1">
            <span className="text-[10px] font-semibold text-muted-foreground uppercase tracking-wide">Resolved/Stub/Unmapped</span>
            <div className="text-sm font-bold font-mono pt-1 space-y-0.5">
              <div className="flex justify-between"><span className="text-emerald-500">Resolved:</span> <span>{summary.resolved}</span></div>
              <div className="flex justify-between"><span className="text-amber-500">Stubbed:</span> <span>{summary.stubbed}</span></div>
              <div className="flex justify-between"><span className="text-neutral-400">Unmapped:</span> <span>{summary.unmapped}</span></div>
            </div>
          </div>
        </div>
      </Panel>

      {/* 2. Main Content Grid */}
      <div className="grid grid-cols-1 xl:grid-cols-[300px_minmax(0,1fr)_320px] gap-4">
        {/* LEFT COLUMN: Modules Grid Checklist */}
        <div className="space-y-4">
          <Panel title="System Modules" icon={<Compass className="size-4" />}>
            <p className="text-[11px] text-muted-foreground mb-3 leading-relaxed">
              Select a PSP system library below to isolate NID coverage tables.
            </p>
            <div className="space-y-1.5 max-h-[420px] overflow-y-auto thin-scroll pr-1">
              <button
                onClick={() => setSelectedModule(null)}
                className={`w-full text-left text-xs font-semibold px-2.5 py-1.5 rounded-lg border transition-colors flex items-center justify-between ${
                  !selectedModule
                    ? "bg-primary/10 border-primary/30 text-primary"
                    : "bg-background/20 border-transparent text-muted-foreground hover:bg-neutral-800/40 hover:text-foreground"
                }`}
              >
                <span>ALL MODULES</span>
                <span className="font-mono text-[10px]">{nids.length}</span>
              </button>

              {modules.map((m) => {
                const active = selectedModule === m.name;
                return (
                  <button
                    key={m.name}
                    onClick={() => setSelectedModule(m.name)}
                    className={`w-full text-left px-2.5 py-2 rounded-lg border transition-colors space-y-1 ${
                      active
                        ? "bg-primary/10 border-primary/30 text-primary"
                        : "bg-background/20 border-transparent text-muted-foreground hover:bg-neutral-800/40 hover:text-foreground"
                    }`}
                  >
                    <div className="flex justify-between items-center text-xs font-mono font-semibold">
                      <span className="truncate max-w-[180px]">{m.name}</span>
                      <span>{m.total}</span>
                    </div>
                    <div className="flex items-center gap-1.5">
                      <div className="flex-1 bg-neutral-850 h-1 rounded-full overflow-hidden">
                        <div className="bg-emerald-500 h-full float-left" style={{ width: `${m.coverage_pct}%` }} />
                        <div className="bg-amber-500 h-full float-left" style={{ width: `${m.implemented_pct - m.coverage_pct}%` }} />
                      </div>
                      <span className="text-[9px] font-mono text-muted-foreground">{Math.round(m.coverage_pct)}%</span>
                    </div>
                  </button>
                );
              })}
            </div>
          </Panel>
        </div>

        {/* MIDDLE COLUMN: NIDs Grid/Table */}
        <div className="space-y-4">
          <Panel title={`${selectedModule || "All"} NID imports`} icon={<Search className="size-4" />}>
            <div className="space-y-3">
              {/* Search & Filters */}
              <div className="flex flex-col sm:flex-row gap-2">
                <div className="relative flex-1">
                  <Search className="absolute left-2.5 top-2.5 size-3.5 text-muted-foreground" />
                  <Input
                    placeholder="Search NID, name, or handler..."
                    value={search}
                    onChange={(e) => setSearch(e.target.value)}
                    className="h-9 pl-8 text-xs font-mono bg-black/20"
                  />
                </div>

                <div className="flex gap-1.5 shrink-0 overflow-x-auto">
                  {(["all", "resolved", "stubbed", "unmapped"] as const).map((filter) => (
                    <Button
                      key={filter}
                      size="sm"
                      variant={statusFilter === filter ? "default" : "outline"}
                      onClick={() => setStatusFilter(filter)}
                      className="h-9 text-[10px] uppercase font-mono px-3 shrink-0"
                    >
                      {filter}
                    </Button>
                  ))}
                </div>
              </div>

              {/* Master Grid Table */}
              <div className="rounded-xl border border-border/60 bg-black/20 overflow-hidden text-xs">
                <div className="max-h-[380px] overflow-y-auto thin-scroll">
                  <table className="w-full text-left border-collapse">
                    <thead>
                      <tr className="border-b border-border/30 bg-black/35 font-mono text-[10px] text-muted-foreground uppercase">
                        <th className="px-3 py-2 font-semibold">NID</th>
                        <th className="px-3 py-2 font-semibold">Stub Addr</th>
                        <th className="px-3 py-2 font-semibold">Name</th>
                        <th className="px-3 py-2 font-semibold">Handler</th>
                        <th className="px-3 py-2 font-semibold w-24">Status</th>
                      </tr>
                    </thead>
                    <tbody>
                      {filteredNids.length === 0 ? (
                        <tr>
                          <td colSpan={5} className="p-8 text-center text-muted-foreground italic font-mono text-xs">
                            No NID imports match criteria.
                          </td>
                        </tr>
                      ) : (
                        filteredNids.map((n, idx) => {
                          const isSelected = selectedNid?.nid_hex === n.nid_hex;
                          return (
                            <tr
                              key={idx}
                              onClick={() => setSelectedNid(n)}
                              className={`border-b border-border/10 cursor-pointer font-mono text-[11px] transition-colors ${
                                isSelected
                                  ? "bg-primary/10 hover:bg-primary/15"
                                  : "hover:bg-neutral-800/20"
                              }`}
                            >
                              <td className="px-3 py-2.5 font-semibold text-primary">{n.nid_hex}</td>
                              <td className="px-3 py-2.5 text-muted-foreground">{n.stub}</td>
                              <td className="px-3 py-2.5 max-w-[160px] truncate" title={n.name}>{n.name}</td>
                              <td className="px-3 py-2.5 max-w-[140px] truncate text-muted-foreground" title={n.handler || ""}>
                                {n.handler || "-"}
                              </td>
                              <td className="px-3 py-2.5">{getStatusBadge(n.status)}</td>
                            </tr>
                          );
                        })
                      )}
                    </tbody>
                  </table>
                </div>
              </div>

              <div className="text-[10px] text-muted-foreground font-mono">
                Showing {filteredNids.length} of {nids.filter(n => !selectedModule || n.lib === selectedModule).length} NIDs
              </div>
            </div>
          </Panel>
        </div>

        {/* RIGHT COLUMN: Scannable NID details Sidebar */}
        <div className="space-y-4">
          <Panel title="NID Details" icon={<AlertCircle className="size-4" />}>
            {selectedNid ? (
              <div className="space-y-4">
                <div className="space-y-1">
                  <span className="text-[9px] uppercase font-bold tracking-wider text-muted-foreground">Name</span>
                  <div className="text-sm font-bold font-mono break-all">{selectedNid.name}</div>
                </div>

                <div className="grid grid-cols-2 gap-3">
                  <div className="space-y-1">
                    <span className="text-[9px] uppercase font-bold tracking-wider text-muted-foreground">NID Value</span>
                    <div className="text-xs font-mono font-semibold text-primary">{selectedNid.nid_hex}</div>
                  </div>

                  <div className="space-y-1">
                    <span className="text-[9px] uppercase font-bold tracking-wider text-muted-foreground">Stub Address</span>
                    <div className="text-xs font-mono">{selectedNid.stub}</div>
                  </div>
                </div>

                <div className="space-y-1">
                  <span className="text-[9px] uppercase font-bold tracking-wider text-muted-foreground">Library Module</span>
                  <div className="text-xs font-mono font-semibold">{selectedNid.lib}</div>
                </div>

                <div className="space-y-1">
                  <span className="text-[9px] uppercase font-bold tracking-wider text-muted-foreground">HLE Handler</span>
                  <div className="text-xs font-mono bg-black/40 px-2 py-1 rounded border border-neutral-850 flex items-center justify-between">
                    <span>{selectedNid.handler || "Unmapped"}</span>
                    <span>{getStatusBadge(selectedNid.status)}</span>
                  </div>
                </div>

                <div className="space-y-1.5">
                  <span className="text-[9px] uppercase font-bold tracking-wider text-muted-foreground block">Signature Requirements</span>
                  <div className="flex flex-wrap gap-1">
                    {selectedNid.registers.length === 0 ? (
                      <span className="text-[10px] text-muted-foreground italic">No registers accessed (standard or unmapped)</span>
                    ) : (
                      selectedNid.registers.map((r, idx) => (
                        <span key={idx} className="text-[10px] font-mono px-2 py-0.5 rounded bg-neutral-900 border border-neutral-800 text-neutral-300">
                          {r}
                        </span>
                      ))
                    )}
                  </div>
                </div>

                <div className="space-y-1">
                  <span className="text-[9px] uppercase font-bold tracking-wider text-muted-foreground">Logging Behavior</span>
                  <div className="text-xs font-mono">
                    {selectedNid.logging === "Silent" && (
                      <span className="text-neutral-400">Silent (No debug traces)</span>
                    )}
                    {selectedNid.logging === "Logs on call" && (
                      <span className="text-purple-400 font-semibold">Logs details to stderr on call</span>
                    )}
                    {selectedNid.logging === "Logs warning" && (
                      <span className="text-amber-500 font-semibold">Logs unimplemented warning</span>
                    )}
                  </div>
                </div>

                <div className="space-y-1.5">
                  <span className="text-[9px] uppercase font-bold tracking-wider text-muted-foreground block">Assembly Call-out Flags</span>
                  <div className="flex flex-wrap gap-1">
                    {selectedNid.flags.length === 0 ? (
                      <span className="text-[10px] text-muted-foreground italic">None (safe leaf function)</span>
                    ) : (
                      selectedNid.flags.map((f, idx) => (
                        <span key={idx} className={`text-[10px] font-mono px-2 py-0.5 rounded border ${
                          f === "yields" ? "bg-amber-500/10 border-amber-500/25 text-amber-500" :
                          f === "blocking" ? "bg-purple-500/10 border-purple-500/25 text-purple-500" :
                          "bg-neutral-900 border-neutral-800 text-neutral-300"
                        }`}>
                          {f}
                        </span>
                      ))
                    )}
                  </div>
                </div>
              </div>
            ) : (
              <div className="p-8 text-center text-muted-foreground italic font-mono text-xs">
                Select an import NID to inspect details.
              </div>
            )}
          </Panel>
        </div>
      </div>
    </div>
  );
}
