"use client";

import { Hammer, Loader2, CircleCheck } from "lucide-react";
import { useStudio } from "./studio-context";
import { Button } from "@/components/ui/button";

export function Footer({ onRecompile }: { onRecompile: () => void }) {
  const { buildStatus } = useStudio();
  return (
    <footer className="sticky bottom-0 z-30 mt-auto border-t border-border/60 glass">
      <div className="px-4 py-2.5 flex items-center gap-3">
        {buildStatus === "running" ? (
          <Loader2 className="size-4 animate-spin text-primary" />
        ) : buildStatus === "completed" ? (
          <CircleCheck className="size-4 text-emerald-400" />
        ) : (
          <Hammer className="size-4 text-muted-foreground" />
        )}
        <span className="text-[11px] text-muted-foreground">
          {buildStatus === "running" ? "Native manager task running" : buildStatus === "completed" ? "Native build completed" : "Native project ready"}
        </span>
        <span className="hidden md:inline text-[10px] text-muted-foreground/70">
          Unofficial compatibility project · no game files included
        </span>
        <Button
          size="sm"
          onClick={onRecompile}
          className="ml-auto h-9 gap-2 font-semibold shadow-[0_0_20px_-4px] shadow-primary/50"
        >
          <Hammer className="size-4" />
          Build & Run
        </Button>
      </div>
    </footer>
  );
}
