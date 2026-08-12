"use client";

import { useState } from "react";
import { Share2, Copy, Check, Link2, Download } from "lucide-react";
import { useStudio } from "./studio-context";
import { buildShareUrl, decodeConfigFromHash } from "@/lib/recompiler/share";
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogDescription,
  DialogTrigger,
} from "@/components/ui/dialog";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { useToast } from "@/hooks/use-toast";

export function ShareDialog() {
  const { config } = useStudio();
  const { toast } = useToast();
  const [open, setOpen] = useState(false);
  const [copied, setCopied] = useState(false);
  const [importUrl, setImportUrl] = useState("");

  const shareUrl = buildShareUrl(config);

  async function copyLink() {
    try {
      await navigator.clipboard.writeText(shareUrl);
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
      toast({ title: "Share link copied", description: "Paste it anywhere to share this config." });
    } catch {
      toast({ title: "Copy failed", description: "Select the text manually.", variant: "destructive" });
    }
  }

  function importFromUrl() {
    try {
      const url = new URL(importUrl);
      const hash = url.hash.replace(/^#cfg=/, "");
      if (!hash) {
        toast({ title: "Invalid link", description: "No config found in the URL.", variant: "destructive" });
        return;
      }
      const decoded = decodeConfigFromHash(hash);
      if (!decoded) {
        toast({ title: "Invalid link", description: "Could not decode the config.", variant: "destructive" });
        return;
      }
      // Apply the decoded config via the studio context.
      // We use a custom event so the context can pick it up.
      window.dispatchEvent(new CustomEvent("hst-import-config", { detail: decoded }));
      setOpen(false);
      setImportUrl("");
      toast({ title: "Config imported", description: "Shared config applied. Save to persist." });
    } catch {
      toast({ title: "Invalid URL", variant: "destructive" });
    }
  }

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>
        <Button variant="ghost" size="sm" className="size-8 p-0 hidden sm:grid" title="Share config">
          <Share2 className="size-4" />
        </Button>
      </DialogTrigger>
      <DialogContent className="max-w-lg">
        <DialogHeader>
          <DialogTitle>Share configuration</DialogTitle>
          <DialogDescription>
            Generate a shareable link that encodes your current config. Anyone who opens it gets
            the exact same settings applied.
          </DialogDescription>
        </DialogHeader>

        <div className="space-y-3">
          <div>
            <label className="text-[10px] uppercase tracking-wide text-muted-foreground mb-1.5 block">
              Share link
            </label>
            <div className="flex gap-1.5">
              <Input readOnly value={shareUrl} className="h-8 text-[11px] font-mono" />
              <Button size="sm" className="h-8 gap-1.5 shrink-0" onClick={copyLink}>
                {copied ? <Check className="size-3.5" /> : <Copy className="size-3.5" />}
                {copied ? "Copied" : "Copy"}
              </Button>
            </div>
            <p className="text-[10px] text-muted-foreground mt-1">
              The link contains the full config (graphics, performance, limits, controllers,
              patches, strategy) encoded in the URL hash.
            </p>
          </div>

          <div className="border-t border-border/40 pt-3">
            <label className="text-[10px] uppercase tracking-wide text-muted-foreground mb-1.5 block">
              Import from link
            </label>
            <div className="flex gap-1.5">
              <Input
                placeholder="Paste a share link…"
                value={importUrl}
                onChange={(e) => setImportUrl(e.target.value)}
                className="h-8 text-[11px] font-mono"
              />
              <Button
                size="sm"
                variant="outline"
                className="h-8 gap-1.5 shrink-0"
                onClick={importFromUrl}
                disabled={!importUrl.trim()}
              >
                <Link2 className="size-3.5" /> Import
              </Button>
            </div>
          </div>
        </div>
      </DialogContent>
    </Dialog>
  );
}
