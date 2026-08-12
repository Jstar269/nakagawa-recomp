"use client";

import {
  Disc3,
  Hammer,
  Cpu,
  BarChart3,
  Compass,
  AlertTriangle,
  FolderOpen,
  Eye,
  FlaskConical,
  Activity,
  Flame,
  type LucideIcon,
} from "lucide-react";
import { cn } from "@/lib/utils";
import { useStudio, type SectionId } from "./studio-context";
import { Button } from "@/components/ui/button";

const NAV: { id: SectionId; label: string; icon: LucideIcon; hint: string }[] = [
  { id: "iso", label: "Game ISO", icon: Disc3, hint: "Mount & inspect" },
  { id: "profiler", label: "Perf Profiler", icon: Flame, hint: "Hottest loops & paths" },
  { id: "build", label: "Build & Run", icon: Hammer, hint: "Native manager" },
  { id: "test-lab", label: "Test Lab", icon: FlaskConical, hint: "Microtests & Fuzz" },
  { id: "build-health", label: "Build Health", icon: Activity, hint: "Test matrix & watch" },
  { id: "assets", label: "Assets Tree", icon: FolderOpen, hint: "GIMs · Sound streams" },
  { id: "internals", label: "Internals", icon: Cpu, hint: "Real pipeline" },
  { id: "visual-regression", label: "Visual Checks", icon: Eye, hint: "Shader regression" },
  { id: "progress", label: "Progress", icon: BarChart3, hint: "7-phase tracker" },
  { id: "porting", label: "Architecture", icon: Compass, hint: "Runtime guide" },
  { id: "troubleshooting", label: "Black Screen", icon: AlertTriangle, hint: "Issue tracker" },
];

export function Sidebar() {
  const { section, setSection } = useStudio();

  return (
    <aside className="flex flex-col gap-3 h-full">
      <nav className="flex flex-col gap-1 court-grid rounded-xl border border-border/60 bg-card/40 p-2">
        {NAV.map((n) => {
          const active = section === n.id;
          const Icon = n.icon;
          return (
            <button
              key={n.id}
              onClick={() => setSection(n.id)}
              className={cn(
                "group flex items-center gap-3 rounded-lg px-2.5 py-2 text-left transition-colors",
                active
                  ? "bg-primary/15 text-primary border border-primary/30"
                  : "text-muted-foreground hover:text-foreground hover:bg-accent/40 border border-transparent",
              )}
            >
              <Icon className={cn("size-4 shrink-0", active ? "text-primary" : "")} />
              <span className="flex-1 min-w-0">
                <span className="block text-xs font-medium leading-tight">{n.label}</span>
                <span className="block text-[10px] text-muted-foreground/80 leading-tight">
                  {n.hint}
                </span>
              </span>
            </button>
          );
        })}
      </nav>

      <div className="rounded-xl border border-border/60 bg-card/40 p-3 text-[10px] text-muted-foreground leading-relaxed">
        All controls in this navigation read or operate on the local native project. Experimental
        graphics and patch presets remain source-controlled until matching runtime switches exist.
      </div>
    </aside>
  );
}

export function MobileNav() {
  const { section, setSection } = useStudio();
  return (
    <div className="lg:hidden -mx-4 px-4 overflow-x-auto thin-scroll">
      <div className="flex gap-1.5 w-max pb-1">
        {NAV.map((n) => {
          const Icon = n.icon;
          const active = section === n.id;
          return (
            <Button
              key={n.id}
              size="sm"
              variant={active ? "default" : "outline"}
              onClick={() => setSection(n.id)}
              className="h-8 gap-1.5 shrink-0"
            >
              <Icon className="size-3.5" />
              {n.label}
            </Button>
          );
        })}
      </div>
    </div>
  );
}
