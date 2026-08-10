"use client";

import { useState } from "react";
import { Folder, FileText, ChevronRight, ChevronDown, FileBox } from "lucide-react";
import type { IsoTreeNode } from "@/lib/recompiler/types";
import { cn } from "@/lib/utils";

function fmtSize(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(2)} MB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

export function IsoFileTree({ nodes }: { nodes: IsoTreeNode[] }) {
  return (
    <div className="rounded-lg border border-border/60 bg-background/30 overflow-hidden">
      <div className="flex items-center gap-2 px-3 py-2 bg-background/40 border-b border-border/60 text-[10px] uppercase tracking-wide text-muted-foreground">
        <FileBox className="size-3" />
        <span>ISO contents</span>
        <span className="ml-auto font-mono normal-case">{nodes.length} entries</span>
      </div>
      <div className="max-h-80 overflow-y-auto thin-scroll p-1">
        {nodes.map((n, i) => (
          <TreeRow key={i} node={n} depth={0} />
        ))}
      </div>
    </div>
  );
}

function TreeRow({ node, depth }: { node: IsoTreeNode; depth: number }) {
  const [open, setOpen] = useState(depth < 1);
  const hasChildren = node.isDir && node.children && node.children.length > 0;

  return (
    <div>
      <button
        onClick={() => hasChildren && setOpen(!open)}
        className={cn(
          "flex items-center gap-1.5 w-full text-left rounded-md px-1.5 py-1 text-[11px] hover:bg-accent/40 transition-colors",
          !hasChildren && "cursor-default",
        )}
        style={{ paddingLeft: `${depth * 14 + 6}px` }}
      >
        {hasChildren ? (
          open ? (
            <ChevronDown className="size-3 text-muted-foreground shrink-0" />
          ) : (
            <ChevronRight className="size-3 text-muted-foreground shrink-0" />
          )
        ) : (
          <span className="size-3 shrink-0" />
        )}
        {node.isDir ? (
          <Folder className="size-3.5 text-primary/70 shrink-0" />
        ) : (
          <FileText className="size-3.5 text-muted-foreground shrink-0" />
        )}
        <span className={cn("font-mono truncate flex-1", node.isDir && "font-medium")}>
          {node.name}
        </span>
        <span className="text-[9px] font-mono text-muted-foreground shrink-0">
          {fmtSize(node.size)}
        </span>
      </button>
      {hasChildren && open ? (
        <div>
          {node.children!.map((c, i) => (
            <TreeRow key={i} node={c} depth={depth + 1} />
          ))}
        </div>
      ) : null}
    </div>
  );
}
