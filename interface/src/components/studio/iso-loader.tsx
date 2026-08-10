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
        title="Game ISO"
        subtitle="Inspect a Hot Shots Tennis: Get a Grip UMD image locally. Only the ISO9660 metadata and directory sectors needed for inspection are read; the selected file stays in your browser."
      />

      {!hasIso ? (
        <>
          <div className="rounded-xl border border-primary/20 bg-gradient-to-br from-primary/10 via-card/40 to-card/40 p-4 relative overflow-hidden">
            <div className="absolute -right-6 -top-6 size-32 rounded-full bg-primary/10 blur-2xl" />
            <div className="relative flex items-start gap-3">
              <div className="size-10 rounded-xl bg-primary/20 border border-primary/30 grid place-items-center shrink-0">
                <Disc3 className="size-5 text-primary" />
              </div>
              <div className="min-w-0">
                <h3 className="text-sm font-semibold">Welcome to Nakagawa Recomp</h3>
                <p className="text-[11px] text-muted-foreground mt-0.5 leading-snug">
                  Select your legally obtained game ISO to inspect its metadata. The other panels
                  expose the native build, runtime diagnostics, asset inventory, and experimental
                  settings; controls without a matching runtime switch are clearly marked as plans.
                </p>
                <div className="flex flex-wrap gap-1.5 mt-2">
                  <span className="text-[9px] font-mono px-1.5 py-0.5 rounded bg-background/50 border border-border/60">Local ISO inspection</span>
                  <span className="text-[9px] font-mono px-1.5 py-0.5 rounded bg-background/50 border border-border/60">Native build control</span>
                  <span className="text-[9px] font-mono px-1.5 py-0.5 rounded bg-background/50 border border-border/60">Runtime telemetry</span>
                  <span className="text-[9px] font-mono px-1.5 py-0.5 rounded bg-background/50 border border-border/60">Asset diagnostics</span>
                </div>
              </div>
            </div>
          </div>
          <Panel title="Drop your ISO" icon={<Upload className="size-4" />}>
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
              {loading ? "Inspecting ISO header…" : "Drop your .iso here or click to browse"}
            </p>
            <p className="text-[11px] text-muted-foreground mt-1">
              Supports uncompressed .iso PSP UMD images. Parsing stays in this browser tab.
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
                className="h-7"
                onClick={() => inputRef.current?.click()}
              >
                <Upload className="size-3.5 mr-1" /> Replace
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
            title="Title match"
            icon={matched ? <CircleCheck className="size-4" /> : <TriangleAlert className="size-4" />}
          >
            {matched ? (
              <div className="space-y-3">
                <div className="flex items-center gap-2">
                  <CircleCheck className="size-4 text-emerald-400" />
                  <span className="text-sm font-semibold">{isoMeta.matchedTitle}</span>
                  <Badge className="bg-emerald-500/15 text-emerald-300 border-emerald-500/30 hover:bg-emerald-500/15">
                    Verified
                  </Badge>
                </div>
                <div className="grid grid-cols-2 gap-2 text-xs">
                  <Info label="Developer" value={GAME_PROFILE.developer} />
                  <Info label="Publisher" value={GAME_PROFILE.publisher} />
                  <Info label="Release" value={GAME_PROFILE.release} />
                  <Info label="Platform" value={GAME_PROFILE.platform} />
                  <Info label="Native res" value={GAME_PROFILE.nativeResolution} />
                  <Info label="Native FPS" value={GAME_PROFILE.nativeFrameRate} />
                  <Info label="CPU" value={GAME_PROFILE.cpu} />
                  <Info label="GPU" value={GAME_PROFILE.gpu} />
                </div>
                <Button className="w-full" onClick={() => setSection("graphics")}>
                  Continue to graphics →
                </Button>
              </div>
            ) : (
              <div className="space-y-3">
                <div className="flex items-start gap-2">
                  <TriangleAlert className="size-4 text-amber-400 mt-0.5 shrink-0" />
                  <p className="text-xs text-muted-foreground leading-relaxed">
                    This ISO wasn&apos;t recognized as <em>Hot Shots Tennis: Get a Grip</em>
                    {isoMeta.gameCode ? (
                      <>
                        {" "}(DISC_ID <span className="font-mono">{isoMeta.gameCode}</span>).
                      </>
                    ) : (
                      ". No PARAM.SFO DISC_ID was found."
                    )}
                    . The recompiler can still process it, but game-specific patches may not apply
                    correctly.
                  </p>
                </div>
                <div className="text-[11px] text-muted-foreground">
                  Expected codes:{" "}
                  {GAME_PROFILE.gameCodes.map((c) => (
                    <Badge key={c} variant="outline" className="mr-1 font-mono text-[10px] h-5">
                      {c}
                    </Badge>
                  ))}
                </div>
                <Button variant="outline" className="w-full" onClick={() => setSection("graphics")}>
                  Continue anyway →
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

function Info({ label, value }: { label: string; value: string }) {
  return (
    <div className="rounded-md bg-background/30 border border-border/40 px-2 py-1.5">
      <div className="text-[10px] text-muted-foreground uppercase tracking-wide">{label}</div>
      <div className="text-[11px] font-mono mt-0.5">{value}</div>
    </div>
  );
}
