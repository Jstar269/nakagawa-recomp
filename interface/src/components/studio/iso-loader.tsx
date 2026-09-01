"use client";

import { useRef, useState } from "react";
import {
  Disc3,
  Upload,
  Loader2,
  CircleCheck,
  TriangleAlert,
  FileBox,
  Calendar,
  Hash,
  Globe,
  Building2,
  Info,
  Hammer,
} from "lucide-react";
import { useStudio } from "./studio-context";
import { Panel, SectionHeader, StatPill } from "./ui-bits";
import { IsoFileTree } from "./iso-file-tree";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { GAME_PROFILE } from "@/lib/recompiler/profiles";
import { formatBytes } from "@/lib/format";
import { inspectIso, type SectorReader } from "@/lib/recompiler/iso";
import type { IsoMeta } from "@/lib/recompiler/types";
import { useToast } from "@/hooks/use-toast";

function fmtBytes(n: number) {
  return formatBytes(n);
}

export function IsoLoader() {
  const { isoMeta, setIsoMeta, setSection } = useStudio();
  const { toast } = useToast();
  const inputRef = useRef<HTMLInputElement>(null);
  const [dragOver, setDragOver] = useState(false);
  const [loading, setLoading] = useState(false);

  async function handleFile(file: File) {
    setLoading(true);
    try {
      const read: SectorReader = async (lba, count) => {
        const start = lba * 2048;
        const end = Math.min(file.size, start + count * 2048);
        if (start < 0 || count < 1 || start >= file.size) {
          throw new Error(`ISO sector range is outside the selected file (LBA ${lba})`);
        }
        return new Uint8Array(await file.slice(start, end).arrayBuffer());
      };
      const meta: IsoMeta = await inspectIso(read, file.name, file.size);
      setIsoMeta(meta);
      toast({
        title: meta.matchedTitle ? "ISO recognized" : "ISO inspected",
        description: meta.matchedTitle
          ? `${meta.matchedTitle} (${meta.gameCode})`
          : meta.volumeId || file.name,
      });
    } catch (e) {
      toast({
        title: "Inspection failed",
        description: String(e),
        variant: "destructive",
      });
    } finally {
      setLoading(false);
    }
  }

  function onDrop(e: React.DragEvent) {
    e.preventDefault();
    setDragOver(false);
    const f = e.dataTransfer.files?.[0];
    if (f) handleFile(f);
  }

  const hasIso = !!isoMeta.fileName;
  const matched = !!isoMeta.matchedTitle;

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<Disc3 className="size-4.5" />}
        title="Game ISO Inspector (Browser-Local Only)"
        subtitle="Inspect an uncompressed PSP UMD image header locally in your browser. Reads ISO9660 volume descriptors and filesystem directory structure. Does not upload, copy, or bind files to the host backend."
      />

      {/* Explicit Browser-Inspector Boundary Notice */}
      <div className="rounded-lg border border-primary/30 bg-primary/10 p-3 text-xs flex items-start gap-2.5">
        <Info className="size-4 text-primary mt-0.5 shrink-0" />
        <div className="space-y-1">
          <span className="font-semibold text-foreground">Local Browser Inspection Boundary</span>
          <p className="text-[11px] text-muted-foreground leading-relaxed">
            This tool parses ISO9660 descriptors and directory trees <strong>entirely inside this browser tab</strong>.
            Selecting an ISO here <strong>does not</strong> copy it to <code className="font-mono text-foreground">place_game_here/ISO/</code>,
            extract XB archives, decrypt PRX/ELF modules, or trigger native compilation. For native recompilation, place lawfully
            decrypted binaries in <code className="font-mono text-foreground">place_game_here/</code> on the host machine as verified by Preflight.
          </p>
        </div>
      </div>

      {!hasIso ? (
        <>
          <div className="rounded-xl border border-primary/20 bg-gradient-to-br from-primary/10 via-card/40 to-card/40 p-4 relative overflow-hidden">
            <div className="absolute -right-6 -top-6 size-32 rounded-full bg-primary/10 blur-2xl" />
            <div className="relative flex items-start gap-3">
              <div className="size-10 rounded-xl bg-primary/20 border border-primary/30 grid place-items-center shrink-0">
                <Disc3 className="size-5 text-primary" />
              </div>
              <div className="min-w-0">
                <h3 className="text-sm font-semibold">ISO Header & Structure Explorer</h3>
                <p className="text-[11px] text-muted-foreground mt-0.5 leading-snug">
                  Select your legally obtained game ISO to inspect its volume descriptor, region, DISC_ID, and internal directory layout.
                  Native build and execution controls are managed separately in the Recompile & Run tab.
                </p>
                <div className="flex flex-wrap gap-1.5 mt-2">
                  <span className="text-[9px] font-mono px-1.5 py-0.5 rounded bg-background/50 border border-border/60">
                    Client-side ISO9660 parser
                  </span>
                  <span className="text-[9px] font-mono px-1.5 py-0.5 rounded bg-background/50 border border-border/60">
                    No data leaves browser
                  </span>
                  <span className="text-[9px] font-mono px-1.5 py-0.5 rounded bg-background/50 border border-border/60">
                    DISC_ID validation
                  </span>
                </div>
              </div>
            </div>
          </div>

          <Panel title="Drop your ISO for Local Inspection" icon={<Upload className="size-4" />}>
            <div
              onDragOver={(e) => {
                e.preventDefault();
                setDragOver(true);
              }}
              onDragLeave={() => setDragOver(false)}
              onDrop={onDrop}
              onClick={() => inputRef.current?.click()}
              className={`relative cursor-pointer rounded-xl border-2 border-dashed transition-colors court-grid p-8 text-center ${
                dragOver
                  ? "border-primary bg-primary/10"
                  : "border-border/70 hover:border-primary/50 hover:bg-accent/20"
              }`}
            >
              <input
                ref={inputRef}
                type="file"
                accept=".iso,application/octet-stream"
                className="hidden"
                onChange={(e) => {
                  const f = e.target.files?.[0];
                  if (f) handleFile(f);
                }}
              />
              <div className="mx-auto size-12 rounded-xl bg-primary/15 border border-primary/30 grid place-items-center mb-3">
                {loading ? (
                  <Loader2 className="size-5 animate-spin text-primary" />
                ) : (
                  <Upload className="size-5 text-primary" />
                )}
              </div>
              <p className="text-sm font-medium">
                {loading ? "Inspecting ISO header sectors…" : "Drop your .iso here or click to browse"}
              </p>
              <p className="text-[11px] text-muted-foreground mt-1">
                Supports standard ISO9660 PSP images. Parsing occurs strictly within this browser tab memory.
              </p>
            </div>
          </Panel>
        </>
      ) : (
        <>
          <Panel
            title="Volume descriptor"
            icon={<FileBox className="size-4" />}
            right={
              <Button
                size="sm"
                variant="outline"
                className="h-7 text-xs"
                onClick={() => inputRef.current?.click()}
              >
                <Upload className="size-3.5 mr-1" /> Replace ISO
              </Button>
            }
          >
            <input
              ref={inputRef}
              type="file"
              accept=".iso,application/octet-stream"
              className="hidden"
              onChange={(e) => {
                const f = e.target.files?.[0];
                if (f) handleFile(f);
              }}
            />
            <div className="grid grid-cols-2 sm:grid-cols-4 gap-2 mb-3">
              <StatPill label="File" value={isoMeta.fileName.slice(0, 18)} />
              <StatPill label="Size" value={fmtBytes(isoMeta.sizeBytes)} accent />
              <StatPill label="Files" value={String(isoMeta.fileCount)} />
              <StatPill label="Region" value={isoMeta.region ?? "—"} accent={!!isoMeta.region} />
            </div>

            <dl className="space-y-1.5 text-xs">
              <Row icon={<Hash className="size-3" />} label="Volume ID" value={isoMeta.volumeId} />
              <Row icon={<Globe className="size-3" />} label="System" value={isoMeta.systemId} />
              <Row icon={<Building2 className="size-3" />} label="Application" value={isoMeta.application} />
              <Row icon={<Building2 className="size-3" />} label="Publisher" value={isoMeta.publisher} />
              <Row icon={<Calendar className="size-3" />} label="Created" value={isoMeta.creationDate} />
              <Row
                icon={<Hash className="size-3" />}
                label="DISC_ID"
                value={isoMeta.gameCode ?? "(not found)"}
                accent={!!isoMeta.gameCode}
              />
            </dl>
          </Panel>

          {isoMeta.tree && isoMeta.tree.length > 0 ? (
            <IsoFileTree nodes={isoMeta.tree} />
          ) : null}

          <Panel
            title="Title Verification & Host Pipeline Guidance"
            icon={matched ? <CircleCheck className="size-4" /> : <TriangleAlert className="size-4" />}
          >
            {matched ? (
              <div className="space-y-3">
                <div className="flex items-center gap-2">
                  <CircleCheck className="size-4 text-emerald-400" />
                  <span className="text-sm font-semibold">{isoMeta.matchedTitle}</span>
                  <Badge className="bg-emerald-500/15 text-emerald-300 border-emerald-500/30">
                    Recognized Disc ID
                  </Badge>
                </div>
                <div className="grid grid-cols-2 gap-2 text-xs">
                  <InfoItem label="Developer" value={GAME_PROFILE.developer} />
                  <InfoItem label="Publisher" value={GAME_PROFILE.publisher} />
                  <InfoItem label="Release" value={GAME_PROFILE.release} />
                  <InfoItem label="Platform" value={GAME_PROFILE.platform} />
                  <InfoItem label="Native res" value={GAME_PROFILE.nativeResolution} />
                  <InfoItem label="Native FPS" value={GAME_PROFILE.nativeFrameRate} />
                  <InfoItem label="CPU" value={GAME_PROFILE.cpu} />
                  <InfoItem label="GPU" value={GAME_PROFILE.gpu} />
                </div>

                <div className="rounded-lg border border-border/60 bg-background/40 p-3 text-xs space-y-1 text-muted-foreground">
                  <span className="font-semibold text-foreground block">Next Steps for Native Compilation:</span>
                  <p className="text-[11px] leading-relaxed">
                    Ensure your decrypted <code className="font-mono text-foreground">EBOOT.elf</code>, PRX modules, and extracted assets
                    are placed in <code className="font-mono text-foreground">place_game_here/</code> on your computer. You can verify
                    their status using Workspace Doctor in the Recompile & Run tab.
                  </p>
                </div>

                <div className="flex flex-col sm:flex-row gap-2 pt-1">
                  <Button className="flex-1 gap-1.5" onClick={() => setSection("build")}>
                    <Hammer className="size-4" /> Open Recompile & Preflight →
                  </Button>
                  <Button variant="outline" className="flex-1" onClick={() => setSection("graphics")}>
                    Inspect Config Settings →
                  </Button>
                </div>
              </div>
            ) : (
              <div className="space-y-3">
                <div className="flex items-start gap-2">
                  <TriangleAlert className="size-4 text-amber-400 mt-0.5 shrink-0" />
                  <p className="text-xs text-muted-foreground leading-relaxed">
                    This ISO was not matched as <em>Hot Shots Tennis: Get a Grip</em>
                    {isoMeta.gameCode ? (
                      <>
                        {" "}(detected DISC_ID: <span className="font-mono text-foreground">{isoMeta.gameCode}</span>).
                      </>
                    ) : (
                      " (no DISC_ID detected in ISO header)."
                    )}
                  </p>
                </div>
                <div className="text-[11px] text-muted-foreground">
                  Supported game code:{" "}
                  {GAME_PROFILE.gameCodes.map((c) => (
                    <Badge key={c} variant="outline" className="mr-1 font-mono text-[10px] h-5">
                      {c}
                    </Badge>
                  ))}
                </div>
                <div className="rounded-lg border border-border/60 bg-background/40 p-3 text-xs text-muted-foreground">
                  <span className="font-semibold text-foreground block mb-1">Host Recompilation Notice:</span>
                  <span className="text-[11px] leading-relaxed block">
                    The native compiler pipeline expects the supported UCUS98701 title assets in{" "}
                    <code className="font-mono text-foreground">place_game_here/</code>.
                  </span>
                </div>
                <Button variant="outline" className="w-full gap-1.5" onClick={() => setSection("build")}>
                  <Hammer className="size-4" /> Open Recompile & Preflight →
                </Button>
              </div>
            )}
          </Panel>
        </>
      )}
    </div>
  );
}

function Row({
  icon,
  label,
  value,
  accent,
}: {
  icon: React.ReactNode;
  label: string;
  value: string;
  accent?: boolean;
}) {
  return (
    <div className="flex items-center gap-2 rounded-md bg-background/30 border border-border/40 px-2 py-1.5">
      <span className="text-muted-foreground">{icon}</span>
      <span className="text-muted-foreground w-24 shrink-0">{label}</span>
      <span className={`font-mono truncate ${accent ? "text-ball" : ""}`}>{value || "—"}</span>
    </div>
  );
}

function InfoItem({ label, value }: { label: string; value: string }) {
  return (
    <div className="rounded-md bg-background/30 border border-border/40 px-2 py-1.5">
      <div className="text-[10px] text-muted-foreground uppercase tracking-wide">{label}</div>
      <div className="text-[11px] font-mono mt-0.5">{value}</div>
    </div>
  );
}
