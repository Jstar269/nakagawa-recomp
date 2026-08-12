"use client";

import type { ReactNode } from "react";
import { cn } from "@/lib/utils";
import { Badge } from "@/components/ui/badge";

export function SectionHeader({
  icon,
  title,
  subtitle,
  right,
}: {
  icon?: ReactNode;
  title: string;
  subtitle?: string;
  right?: ReactNode;
}) {
  return (
    <div className="flex items-start justify-between gap-3 mb-4">
      <div className="flex items-start gap-3">
        {icon ? (
          <div className="size-9 rounded-lg bg-primary/10 border border-primary/20 text-primary grid place-items-center shrink-0">
            {icon}
          </div>
        ) : null}
        <div>
          <h2 className="text-base font-semibold tracking-tight">{title}</h2>
          {subtitle ? (
            <p className="text-xs text-muted-foreground mt-0.5 max-w-prose">{subtitle}</p>
          ) : null}
        </div>
      </div>
      {right}
    </div>
  );
}

export function Panel({
  title,
  description,
  children,
  className,
  icon,
  right,
}: {
  title?: string;
  description?: string;
  children: ReactNode;
  className?: string;
  icon?: ReactNode;
  right?: ReactNode;
}) {
  return (
    <section
      className={cn(
        "rounded-xl border border-border/70 bg-card/60 glass shadow-lg shadow-black/20",
        className,
      )}
    >
      {title ? (
        <header className="flex items-center gap-2.5 px-4 py-3 border-b border-border/60">
          {icon ? <span className="text-primary">{icon}</span> : null}
          <div className="min-w-0 flex-1">
            <h3 className="text-sm font-semibold leading-tight">{title}</h3>
            {description ? (
              <p className="text-[11px] text-muted-foreground leading-tight mt-0.5">
                {description}
              </p>
            ) : null}
          </div>
          {right ? <div className="shrink-0">{right}</div> : null}
        </header>
      ) : null}
      <div className="p-4">{children}</div>
    </section>
  );
}

export function Field({
  label,
  hint,
  children,
  htmlFor,
}: {
  label: string;
  hint?: string;
  children: ReactNode;
  htmlFor?: string;
}) {
  return (
    <div className="space-y-1.5">
      <div className="flex items-baseline justify-between gap-2">
        <label htmlFor={htmlFor} className="text-xs font-medium text-foreground/90">
          {label}
        </label>
        {hint ? <span className="text-[10px] text-muted-foreground font-mono">{hint}</span> : null}
      </div>
      {children}
    </div>
  );
}

const riskStyles: Record<string, string> = {
  safe: "border-emerald-500/30 text-emerald-300 bg-emerald-500/10",
  moderate: "border-amber-500/30 text-amber-300 bg-amber-500/10",
  advanced: "border-amber-500/30 text-amber-300 bg-amber-500/10",
};

export function RiskBadge({ risk }: { risk: "safe" | "moderate" | "advanced" }) {
  const label = risk === "safe" ? "Safe" : risk === "moderate" ? "Moderate" : "Advanced";
  return (
    <Badge
      variant="outline"
      className={cn("text-[10px] h-5 px-1.5 font-medium capitalize", riskStyles[risk])}
    >
      {label}
    </Badge>
  );
}

export function StatPill({
  label,
  value,
  accent,
}: {
  label: string;
  value: string;
  accent?: boolean;
}) {
  return (
    <div className="rounded-lg border border-border/60 bg-background/40 px-2.5 py-1.5">
      <div className="text-[10px] uppercase tracking-wide text-muted-foreground">{label}</div>
      <div
        className={cn(
          "text-xs font-mono font-medium mt-0.5",
          accent ? "text-ball" : "text-foreground",
        )}
      >
        {value}
      </div>
    </div>
  );
}

export function Mono({ children, className }: { children: ReactNode; className?: string }) {
  return <span className={cn("font-mono text-xs", className)}>{children}</span>;
}
