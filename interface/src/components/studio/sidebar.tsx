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
  Gauge,
  Gamepad2,
  Monitor,
  Puzzle,
  SlidersHorizontal,
  type LucideIcon,
} from "lucide-react";
import { cn } from "@/lib/utils";
import { useStudio, type SectionId } from "./studio-context";
import { Button } from "@/components/ui/button";

type NavItem = { id: SectionId; label: string; icon: LucideIcon; hint: string };

const NAV_GROUPS: { label: string; items: NavItem[] }[] = [
  {
    label: "Workflow",
    items: [
      { id: "iso", label: "Game ISO", icon: Disc3, hint: "Inspect locally" },
      { id: "build", label: "Build & Run", icon: Hammer, hint: "Preflight · manager" },
    ],
  },
  {
    label: "Design previews",
    items: [
      { id: "graphics", label: "Graphics", icon: Monitor, hint: "Renderer targets" },
      { id: "performance", label: "Performance", icon: Gauge, hint: "Host tuning" },
      { id: "limitations", label: "PSP Limits", icon: SlidersHorizontal, hint: "Compatibility targets" },
      { id: "controllers", label: "Controllers", icon: Gamepad2, hint: "Input mappings" },
      { id: "patches", label: "Game Patches", icon: Puzzle, hint: "Patch concepts" },
    ],
  },
  {
    label: "Developer tools",
    items: [
      { id: "profiler", label: "Perf Profiler", icon: Flame, hint: "Hottest loops & paths" },
      { id: "test-lab", label: "Test Lab", icon: FlaskConical, hint: "Microtests & fuzz" },
      { id: "build-health", label: "Build Health", icon: Activity, hint: "Test matrix & watch" },
      { id: "assets", label: "Assets Tree", icon: FolderOpen, hint: "GIMs · sound streams" },
      { id: "internals", label: "Internals", icon: Cpu, hint: "Real pipeline" },
      { id: "visual-regression", label: "Visual Checks", icon: Eye, hint: "Shader regression" },
      { id: "progress", label: "Progress", icon: BarChart3, hint: "Evidence tracker" },
      { id: "porting", label: "Architecture", icon: Compass, hint: "Runtime guide" },
      { id: "troubleshooting", label: "Black Screen", icon: AlertTriangle, hint: "Issue tracker" },
    ],
  },
];

const NAV = NAV_GROUPS.flatMap((group) => group.items);

function NavButton({ item, active, onSelect }: { item: NavItem; active: boolean; onSelect: () => void }) {
  const Icon = item.icon;
  return (
    <button
      type="button"
      onClick={onSelect}
      aria-current={active ? "page" : undefined}
      title={item.hint}
      className={cn(
        "group flex items-center gap-3 rounded-lg px-2.5 py-2 text-left transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring",
        active
          ? "bg-primary/15 text-primary border border-primary/30"
          : "text-muted-foreground hover:text-foreground hover:bg-accent/40 border border-transparent",
      )}
    >
      <Icon className={cn("size-4 shrink-0", active ? "text-primary" : "")} />
      <span className="flex-1 min-w-0">
        <span className="block text-xs font-medium leading-tight">{item.label}</span>
        <span className="block text-[10px] text-muted-foreground/80 leading-tight">{item.hint}</span>
      </span>
    </button>
  );
}

function MobileNavButton({ item, active, onSelect }: { item: NavItem; active: boolean; onSelect: () => void }) {
  const Icon = item.icon;
  return (
    <Button
      type="button"
      size="sm"
      variant={active ? "default" : "outline"}
      onClick={onSelect}
      aria-current={active ? "page" : undefined}
      title={item.hint}
      className="h-8 gap-1.5 shrink-0"
    >
      <Icon className="size-3.5" />
      {item.label}
    </Button>
  );
}

export function Sidebar() {
  const { section, setSection } = useStudio();

  return (
    <aside className="flex flex-col gap-3 h-full">
      <nav aria-label="Studio sections" className="flex flex-col gap-3 court-grid rounded-xl border border-border/60 bg-card/40 p-2">
        {NAV_GROUPS.map((group) => (
          <div key={group.label} className="space-y-1">
            <div className="px-2.5 pt-1 text-[9px] font-semibold uppercase tracking-[0.16em] text-muted-foreground/70">
              {group.label}
            </div>
            {group.items.map((item) => (
              <NavButton
                key={item.id}
                item={item}
                active={section === item.id}
                onSelect={() => setSection(item.id)}
              />
            ))}
          </div>
        ))}
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
    <nav aria-label="Studio sections" className="lg:hidden -mx-4 px-4 overflow-x-auto thin-scroll">
      <div className="flex gap-1.5 w-max pb-1">
        {NAV.map((n) => {
          const active = section === n.id;
          return (
            <MobileNavButton
              key={n.id}
              item={n}
              active={active}
              onSelect={() => setSection(n.id)}
            />
          );
        })}
      </div>
    </nav>
  );
}
