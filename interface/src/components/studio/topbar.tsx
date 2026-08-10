"use client";

import { CircleDot, Volleyball, Sun, Moon, Hammer } from "lucide-react";
import { useStudio } from "./studio-context";
import { useTheme } from "@/hooks/use-theme";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";

export function Topbar() {
  const { isoMeta, setSection, requestBuild } = useStudio();
  const { theme, toggle: toggleTheme } = useTheme();
  const build = () => {
    setSection("build");
    requestBuild();
  };
  return (
    <header className="sticky top-0 z-40 border-b border-border/60 glass">
      <div className="flex items-center gap-3 px-4 h-14">
        <div className="size-8 rounded-lg bg-primary text-primary-foreground grid place-items-center shadow-[0_0_18px_-2px] shadow-primary/40">
          <Volleyball className="size-4.5" />
        </div>
        <div className="min-w-0 leading-tight">
          <div className="font-semibold text-sm tracking-tight truncate">Nakagawa Recomp</div>
          <p className="text-[10px] text-muted-foreground truncate">Local native build, run, and boot diagnostics</p>
        </div>
        <div className="ml-auto flex items-center gap-2">
          {isoMeta.matchedTitle ? (
            <Badge className="hidden sm:inline-flex bg-primary/15 text-primary border border-primary/30 hover:bg-primary/15">
              <CircleDot className="size-3 mr-1" /> ISO matched
            </Badge>
          ) : isoMeta.fileName ? (
            <Badge variant="outline" className="hidden sm:inline-flex text-amber-300 border-amber-500/30 bg-amber-500/10">ISO unmatched</Badge>
          ) : null}
          <Button variant="ghost" size="sm" className="size-8 p-0" onClick={toggleTheme} aria-label="Toggle color theme">
            {theme === "dark" ? <Sun className="size-4" /> : <Moon className="size-4" />}
          </Button>
          <Button size="sm" className="h-8 gap-1.5" onClick={build}>
            <Hammer className="size-3.5" /> BuildFull
          </Button>
        </div>
      </div>
    </header>
  );
}
