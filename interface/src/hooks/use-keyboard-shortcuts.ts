"use client";

import { useEffect } from "react";
import type { SectionId } from "@/components/studio/studio-context";

const SECTION_KEYS: Record<string, SectionId> = {
  "1": "iso",
  "2": "graphics",
  "3": "performance",
  "4": "limitations",
  "5": "controllers",
  "6": "patches",
  "7": "build",
  "8": "internals",
  "9": "progress",
  "0": "porting",
  "t": "troubleshooting",
};

interface Handlers {
  setSection: (s: SectionId) => void;
  save: () => Promise<void>;
  startBuild: () => void;
  toggleProfiles: () => void;
  undo: () => void;
  redo: () => void;
}

// Global keyboard shortcuts:
//  1-7     switch section
//  Ctrl+S  save profile
//  Ctrl+B  go to build & start recompile
//  Ctrl+P  toggle profiles dropdown
//  Ctrl+Z  undo
//  Ctrl+Shift+Z / Ctrl+Y  redo
export function useKeyboardShortcuts(h: Handlers) {
  useEffect(() => {
    function onKey(e: KeyboardEvent) {
      const target = e.target as HTMLElement | null;
      const tag = target?.tagName?.toLowerCase();
      const typing = tag === "input" || tag === "textarea" || target?.isContentEditable;

      // Ctrl/Cmd combos work even when typing.
      if (e.ctrlKey || e.metaKey) {
        const k = e.key.toLowerCase();
        if (k === "s") {
          e.preventDefault();
          void h.save();
          return;
        }
        if (k === "b") {
          e.preventDefault();
          h.setSection("build");
          setTimeout(() => h.startBuild(), 250);
          return;
        }
        if (k === "p") {
          e.preventDefault();
          h.toggleProfiles();
          return;
        }
        if (k === "z") {
          e.preventDefault();
          if (e.shiftKey) h.redo();
          else h.undo();
          return;
        }
        if (k === "y") {
          e.preventDefault();
          h.redo();
          return;
        }
        return;
      }

      if (typing) return;
      if (e.key in SECTION_KEYS) {
        e.preventDefault();
        h.setSection(SECTION_KEYS[e.key]);
      }
    }
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [h]);
}
