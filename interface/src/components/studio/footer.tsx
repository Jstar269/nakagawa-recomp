"use client";

import { AlertCircle, CircleCheck, Disc3, Hammer, Loader2, TriangleAlert } from "lucide-react";
import { useStudio } from "./studio-context";
import { Button } from "@/components/ui/button";

export function Footer({ onRecompile }: { onRecompile: () => void }) {
  const { buildStatus, isoMeta } = useStudio();
  const status =
    buildStatus === "running"
      ? { label: "Native manager task running", icon: <Loader2 className="size-4 animate-spin text-primary" /> }
      : buildStatus === "completed"
        ? { label: "Native build completed — inspect results", icon: <CircleCheck className="size-4 text-emerald-400" /> }
        : buildStatus === "failed"
          ? { label: "Native build failed — open Build & Run", icon: <AlertCircle className="size-4 text-rose-400" /> }
          : isoMeta.matchedTitle
            ? { label: "ISO matched — host preflight required", icon: <CircleCheck className="size-4 text-primary" /> }
            : isoMeta.fileName
              ? { label: "ISO inspected — title not matched", icon: <TriangleAlert className="size-4 text-amber-400" /> }
              : { label: "Select an ISO to begin", icon: <Disc3 className="size-4 text-muted-foreground" /> };
  return (
    <footer className="sticky bottom-0 z-30 mt-auto border-t border-border/60 glass">
      <div className="px-4 py-2.5 flex items-center gap-3">
        {status.icon}
        <span className="text-[11px] text-muted-foreground" aria-live="polite">{status.label}</span>
        <span className="hidden md:inline text-[10px] text-muted-foreground/70">
          Unofficial compatibility project · no game files included
        </span>
        <Button
          size="sm"
          onClick={onRecompile}
          className="ml-auto h-9 gap-2 font-semibold shadow-[0_0_20px_-4px] shadow-primary/50"
          title="Open host preflight and native build controls"
        >
          <Hammer className="size-4" />
          Open Build & Run
        </Button>
      </div>
    </footer>
  );
}
