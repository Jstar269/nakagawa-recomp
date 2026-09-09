"use client";

import { CircleDot, CircleHelp, Volleyball, Sun, Moon, Hammer, Loader2 } from "lucide-react";
import { useStudio } from "./studio-context";
import { useTheme } from "@/hooks/use-theme";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";

export function Topbar() {
  const { isoMeta, section, buildStatus, setSection, requestBuild } = useStudio();
  const { theme, toggle: toggleTheme } = useTheme();
  const startBuild = () => {
    setSection("build");
    requestBuild();
  };
  const openTour = () => window.dispatchEvent(new Event("hst-restart-tour"));
  const buildRunning = buildStatus === "running";
  const isoStatus = isoMeta.matchedTitle
    ? "ISO matched"
    : isoMeta.fileName
      ? "ISO unmatched"
      : "No ISO inspected";
  const action =
    section === "build"
      ? {
          label: buildRunning ? "Build running…" : "BuildFull",
          onClick: startBuild,
          disabled: buildRunning,
          title: buildRunning ? "A native manager task is already running" : "Start the full native build pipeline",
        }
      : isoMeta.matchedTitle
        ? {
            label: "Open Build & Run",
            onClick: () => setSection("build"),
            disabled: false,
            title: "Open host preflight and native build controls",
          }
        : isoMeta.fileName
          ? {
              label: "Review ISO",
              onClick: () => setSection("iso"),
              disabled: false,
              title: "Review the browser-local ISO inspection",
            }
          : {
              label: "Start with ISO",
              onClick: () => setSection("iso"),
              disabled: false,
              title: "Choose an ISO for browser-local inspection",
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
          <Badge
            variant="outline"
            aria-live="polite"
            className={
              isoMeta.matchedTitle
                ? "hidden sm:inline-flex border-primary/30 bg-primary/10 text-primary"
                : isoMeta.fileName
                  ? "hidden sm:inline-flex border-amber-500/30 bg-amber-500/10 text-amber-300"
                  : "hidden sm:inline-flex border-border/60 text-muted-foreground"
            }
          >
            {isoMeta.matchedTitle ? <CircleDot className="size-3 mr-1" /> : null}
            {isoStatus}
          </Badge>
          {buildRunning ? (
            <Badge className="hidden md:inline-flex border-primary/30 bg-primary/10 text-primary">
              <Loader2 className="size-3 mr-1 animate-spin" /> Build running
            </Badge>
          ) : null}
          <Button variant="ghost" size="sm" className="size-8 p-0" onClick={openTour} aria-label="Open orientation tour" title="Open orientation tour">
            <CircleHelp className="size-4" />
          </Button>
          <Button variant="ghost" size="sm" className="size-8 p-0" onClick={toggleTheme} aria-label="Toggle color theme">
            {theme === "dark" ? <Sun className="size-4" /> : <Moon className="size-4" />}
          </Button>
          <Button size="sm" className="h-8 gap-1.5" onClick={action.onClick} disabled={action.disabled} title={action.title}>
            <Hammer className="size-3.5" /> {action.label}
          </Button>
        </div>
      </div>
    </header>
  );
}
