"use client";

import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from "react";
import type { ReactNode } from "react";
import { defaultConfig, nativeConfig } from "@/lib/recompiler/defaults";
import type { IsoMeta, RecompilerConfig } from "@/lib/recompiler/types";
import { emptyIsoMeta } from "@/lib/recompiler/profiles";

export type SectionId =
  | "iso"
  | "graphics"
  | "performance"
  | "limitations"
  | "controllers"
  | "patches"
  | "build"
  | "internals"
  | "progress"
  | "porting"
  | "troubleshooting"
  | "assets"
  | "visual-regression"
  | "test-lab"
  | "build-health"
  | "profiler";

export interface ProfileSummary {
  id: string;
  name: string;
  isDefault: boolean;
  updatedAt: string;
  createdAt: string;
  summary: {
    resolution: string;
    fps: string;
    limitsRemoved: number;
    patches: number;
    strategy: string;
  };
}

interface StudioState {
  config: RecompilerConfig;
  native: RecompilerConfig;
  isoMeta: IsoMeta;
  section: SectionId;
  dirty: boolean;
  saving: boolean;
  lastSavedAt: string | null;
  // Actual manager task state.
  buildStatus: "idle" | "running" | "completed" | "failed";
  // gamepad capture state
  captureTarget: string | null;
  capturePadIdx: number | null;
  // profile management
  activeProfileId: string | null;
  profiles: ProfileSummary[];
  profilesOpen: boolean;
  buildRequestNonce: number;
  // undo/redo
  canUndo: boolean;
  canRedo: boolean;
}

interface StudioActions {
  setSection: (s: SectionId) => void;
  update: <K extends keyof RecompilerConfig>(
    key: K,
    patch: Partial<RecompilerConfig[K]> | RecompilerConfig[K],
  ) => void;
  updateGraphics: (patch: Partial<RecompilerConfig["graphics"]>) => void;
  updatePerformance: (patch: Partial<RecompilerConfig["performance"]>) => void;
  updateLimitations: (patch: Partial<RecompilerConfig["limitations"]["removed"]>) => void;
  updateControllers: (patch: Partial<RecompilerConfig["controllers"]>) => void;
  updatePatches: (patch: Partial<RecompilerConfig["patches"]["enabled"]>) => void;
  setStrategy: (s: RecompilerConfig["minimizeStrategy"]) => void;
  setIsoMeta: (m: IsoMeta) => void;
  resetConfig: () => void;
  save: () => Promise<void>;
  setBuild: (b: Partial<StudioState>) => void;
  startCapture: (pspAction: string, padIdx: number) => void;
  stopCapture: () => void;
  // profile management
  refreshProfiles: () => Promise<void>;
  createProfile: (name: string, duplicateFrom?: string) => Promise<void>;
  activateProfile: (id: string) => Promise<void>;
  renameProfile: (id: string, name: string) => Promise<void>;
  deleteProfile: (id: string) => Promise<void>;
  loadProfile: (id: string) => Promise<void>;
  setProfilesOpen: (open: boolean) => void;
  requestBuild: () => void;
  undo: () => void;
  redo: () => void;
}

type StudioCtx = StudioState & StudioActions;

const Ctx = createContext<StudioCtx | null>(null);

export function useStudio() {
  const c = useContext(Ctx);
  if (!c) throw new Error("useStudio must be used within StudioProvider");
  return c;
}

export function StudioProvider({ children }: { children: ReactNode }) {
  const native = useMemo(() => nativeConfig(), []);
  const [config, setConfig] = useState<RecompilerConfig>(() => defaultConfig("minimal"));
  const [isoMeta, setIsoMeta] = useState<IsoMeta>(() => emptyIsoMeta());
  const [section, setSection] = useState<SectionId>("iso");
  const [dirty, setDirty] = useState(false);
  const [saving, setSaving] = useState(false);
  const [lastSavedAt, setLastSavedAt] = useState<string | null>(null);

  const [buildStatus, setBuildStatus] = useState<StudioState["buildStatus"]>("idle");
  const [captureTarget, setCaptureTarget] = useState<string | null>(null);
  const [capturePadIdx, setCapturePadIdx] = useState<number | null>(null);
  const [activeProfileId, setActiveProfileId] = useState<string | null>(null);
  const [profiles, setProfiles] = useState<ProfileSummary[]>([]);
  const [profilesOpen, setProfilesOpen] = useState(false);
  const [buildRequestNonce, setBuildRequestNonce] = useState(0);

  // --- Undo/redo history ---
  const undoStackRef = useRef<RecompilerConfig[]>([]);
  const redoStackRef = useRef<RecompilerConfig[]>([]);
  const lastSnapshotRef = useRef<number>(0);
  const [canUndo, setCanUndo] = useState(false);
  const [canRedo, setCanRedo] = useState(false);

  const syncUndoFlags = useCallback(() => {
    setCanUndo(undoStackRef.current.length > 0);
    setCanRedo(redoStackRef.current.length > 0);
  }, []);

  // Snapshot the config into the undo stack (throttled to 1 snapshot per 400ms).
  const pushUndoSnapshot = useCallback(
    (cfg: RecompilerConfig) => {
      const now = Date.now();
      if (now - lastSnapshotRef.current < 400) return;
      lastSnapshotRef.current = now;
      undoStackRef.current.push(cfg);
      if (undoStackRef.current.length > 50) undoStackRef.current.shift();
      redoStackRef.current = [];
      syncUndoFlags();
    },
    [syncUndoFlags],
  );

  // Load saved profile and profile list on mount.
  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const res = await fetch("/api/recompiler/config");
        if (!res.ok) return;
        const data = await res.json();
        if (!cancelled && data?.config) {
          setConfig(data.config);
          setLastSavedAt(data.updatedAt ?? null);
          setActiveProfileId(data.id ?? null);
        }
      } catch {
        /* ignore */
      }
      // Load profiles list.
      try {
        const pr = await fetch("/api/recompiler/profiles");
        if (pr.ok) {
          const pd = await pr.json();
          if (!cancelled) setProfiles(pd.profiles ?? []);
        }
      } catch {
        /* ignore */
      }
    })();
    return () => {
      cancelled = true;
    };
  }, []);

  const markDirty = useCallback(() => setDirty(true), []);

  // Helper: snapshot prev config for undo before a change.
  const snapshotForUndo = useCallback(
    (prev: RecompilerConfig) => {
      const now = Date.now();
      if (now - lastSnapshotRef.current < 400) return;
      lastSnapshotRef.current = now;
      undoStackRef.current.push(prev);
      if (undoStackRef.current.length > 50) undoStackRef.current.shift();
      redoStackRef.current = [];
      syncUndoFlags();
    },
    [syncUndoFlags],
  );

  // --- Share-config: load from URL hash on mount, listen for import events ---
  useEffect(() => {
    async function loadFromHash() {
      if (typeof window === "undefined") return;
      const hash = window.location.hash.replace(/^#cfg=/, "");
      if (!hash) return;
      const { decodeConfigFromHash } = await import("@/lib/recompiler/share");
      const decoded = decodeConfigFromHash(hash);
      if (decoded) {
        setConfig(decoded);
        setDirty(true);
        window.history.replaceState(null, "", window.location.pathname + window.location.search);
      }
    }
    loadFromHash();

    function onImport(e: Event) {
      const detail = (e as CustomEvent).detail as RecompilerConfig;
      if (detail) {
        setConfig((prev) => {
          snapshotForUndo(prev);
          return detail;
        });
        setDirty(true);
      }
    }
    window.addEventListener("hst-import-config", onImport);
    return () => window.removeEventListener("hst-import-config", onImport);
  }, [snapshotForUndo]);

  const update: StudioActions["update"] = useCallback(
    (key, patch) => {
      setConfig((prev) => {
        snapshotForUndo(prev);
        const current = prev[key];
        const next =
          patch && typeof patch === "object" && !Array.isArray(patch)
            ? ({ ...(current as object), ...(patch as object) } as never)
            : (patch as never);
        return { ...prev, [key]: next };
      });
      markDirty();
    },
    [markDirty, snapshotForUndo],
  );

  const updateGraphics: StudioActions["updateGraphics"] = useCallback(
    (patch) => {
      setConfig((prev) => {
        snapshotForUndo(prev);
        return { ...prev, graphics: { ...prev.graphics, ...patch } };
      });
      markDirty();
    },
    [markDirty, snapshotForUndo],
  );

  const updatePerformance: StudioActions["updatePerformance"] = useCallback(
    (patch) => {
      setConfig((prev) => {
        snapshotForUndo(prev);
        return { ...prev, performance: { ...prev.performance, ...patch } };
      });
      markDirty();
    },
    [markDirty, snapshotForUndo],
  );

  const updateLimitations: StudioActions["updateLimitations"] = useCallback(
    (patch) => {
      setConfig((prev) => {
        snapshotForUndo(prev);
        return {
          ...prev,
          limitations: { removed: { ...prev.limitations.removed, ...patch } },
        };
      });
      markDirty();
    },
    [markDirty, snapshotForUndo],
  );

  const updateControllers: StudioActions["updateControllers"] = useCallback(
    (patch) => {
      setConfig((prev) => {
        snapshotForUndo(prev);
        return { ...prev, controllers: { ...prev.controllers, ...patch } };
      });
      markDirty();
    },
    [markDirty, snapshotForUndo],
  );

  const updatePatches: StudioActions["updatePatches"] = useCallback(
    (patch) => {
      setConfig((prev) => {
        snapshotForUndo(prev);
        return {
          ...prev,
          patches: { enabled: { ...prev.patches.enabled, ...patch } },
        };
      });
      markDirty();
    },
    [markDirty, snapshotForUndo],
  );

  const setStrategy: StudioActions["setStrategy"] = useCallback(
    (s) => {
      setConfig((prev) => {
        snapshotForUndo(prev);
        return { ...prev, minimizeStrategy: s };
      });
      markDirty();
    },
    [markDirty, snapshotForUndo],
  );

  const resetConfig = useCallback(() => {
    setConfig((prev) => {
      snapshotForUndo(prev);
      return defaultConfig("minimal");
    });
    setDirty(true);
  }, []);

  const save = useCallback(async () => {
    setSaving(true);
    try {
      const res = await fetch("/api/recompiler/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ config }),
      });
      if (res.ok) {
        const data = await res.json();
        setLastSavedAt(data.updatedAt ?? new Date().toISOString());
        setActiveProfileId(data.id ?? null);
        setDirty(false);
        // Refresh profiles list to reflect updated summary.
        try {
          const pr = await fetch("/api/recompiler/profiles");
          if (pr.ok) {
            const pd = await pr.json();
            setProfiles(pd.profiles ?? []);
          }
        } catch {
          /* ignore */
        }
      }
    } finally {
      setSaving(false);
    }
  }, [config]);

  // --- Profile management ---

  const refreshProfiles: StudioActions["refreshProfiles"] = useCallback(async () => {
    try {
      const pr = await fetch("/api/recompiler/profiles");
      if (pr.ok) {
        const pd = await pr.json();
        setProfiles(pd.profiles ?? []);
      }
    } catch {
      /* ignore */
    }
  }, []);

  const createProfile: StudioActions["createProfile"] = useCallback(
    async (name, duplicateFrom) => {
      try {
        const res = await fetch("/api/recompiler/profiles", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            name,
            config: duplicateFrom ? undefined : config,
            duplicateFrom,
          }),
        });
        if (res.ok) {
          await refreshProfiles();
        }
      } catch {
        /* ignore */
      }
    },
    [config, refreshProfiles],
  );

  const activateProfile: StudioActions["activateProfile"] = useCallback(async (id) => {
    try {
      const res = await fetch(`/api/recompiler/profiles/${id}`, {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ activate: true }),
      });
      if (res.ok) {
        // Load its full config.
        const lr = await fetch(`/api/recompiler/profiles/${id}`);
        if (lr.ok) {
          const data = await lr.json();
          setConfig(data.config);
          setActiveProfileId(id);
          setLastSavedAt(data.updatedAt ?? null);
          setDirty(false);
        }
        await refreshProfiles();
      }
    } catch {
      /* ignore */
    }
  }, [refreshProfiles]);

  const renameProfile: StudioActions["renameProfile"] = useCallback(
    async (id, name) => {
      try {
        await fetch(`/api/recompiler/profiles/${id}`, {
          method: "PATCH",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ name }),
        });
        await refreshProfiles();
      } catch {
        /* ignore */
      }
    },
    [refreshProfiles],
  );

  const deleteProfile: StudioActions["deleteProfile"] = useCallback(
    async (id) => {
      try {
        await fetch(`/api/recompiler/profiles/${id}`, { method: "DELETE" });
        await refreshProfiles();
      } catch {
        /* ignore */
      }
    },
    [refreshProfiles],
  );

  const loadProfile: StudioActions["loadProfile"] = useCallback(async (id) => {
    try {
      const res = await fetch(`/api/recompiler/profiles/${id}`);
      if (res.ok) {
        const data = await res.json();
        setConfig(data.config);
        setActiveProfileId(id);
        setLastSavedAt(data.updatedAt ?? null);
        setDirty(false);
      }
    } catch {
      /* ignore */
    }
  }, []);

  const requestBuild: StudioActions["requestBuild"] = useCallback(() => {
    setBuildRequestNonce((n) => n + 1);
  }, []);

  const undo: StudioActions["undo"] = useCallback(() => {
    const prev = undoStackRef.current.pop();
    if (!prev) return;
    setConfig((current) => {
      redoStackRef.current.push(current);
      return prev;
    });
    syncUndoFlags();
    setDirty(true);
  }, [syncUndoFlags]);

  const redo: StudioActions["redo"] = useCallback(() => {
    const next = redoStackRef.current.pop();
    if (!next) return;
    setConfig((current) => {
      undoStackRef.current.push(current);
      return next;
    });
    syncUndoFlags();
    setDirty(true);
  }, [syncUndoFlags]);

  const setBuild: StudioActions["setBuild"] = useCallback((b) => {
    if (b.buildStatus !== undefined) setBuildStatus(b.buildStatus);
  }, []);

  const startCapture: StudioActions["startCapture"] = useCallback((pspAction, padIdx) => {
    setCaptureTarget(pspAction);
    setCapturePadIdx(padIdx);
  }, []);

  const stopCapture: StudioActions["stopCapture"] = useCallback(() => {
    setCaptureTarget(null);
    setCapturePadIdx(null);
  }, []);

  const value: StudioCtx = {
    config,
    native,
    isoMeta,
    section,
    dirty,
    saving,
    lastSavedAt,
    buildStatus,
    captureTarget,
    capturePadIdx,
    activeProfileId,
    profiles,
    profilesOpen,
    buildRequestNonce,
    canUndo,
    canRedo,
    setSection,
    update,
    updateGraphics,
    updatePerformance,
    updateLimitations,
    updateControllers,
    updatePatches,
    setStrategy,
    setIsoMeta,
    resetConfig,
    save,
    setBuild,
    startCapture,
    stopCapture,
    refreshProfiles,
    createProfile,
    activateProfile,
    renameProfile,
    deleteProfile,
    loadProfile,
    setProfilesOpen,
    requestBuild,
    undo,
    redo,
  };

  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}
