"use client";

import { useRef, useState } from "react";
import { Layers, ChevronDown, Check, Plus, Trash2, Pencil, Copy, Star, Download, Upload } from "lucide-react";
import { useStudio } from "./studio-context";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
  DropdownMenuLabel,
} from "@/components/ui/dropdown-menu";
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogDescription,
} from "@/components/ui/dialog";
import { Input } from "@/components/ui/input";
import { useToast } from "@/hooks/use-toast";
import { cn } from "@/lib/utils";

export function ProfileSwitcher() {
  const {
    config,
    profiles,
    activeProfileId,
    activateProfile,
    createProfile,
    deleteProfile,
    renameProfile,
    profilesOpen,
    setProfilesOpen,
    dirty,
  } = useStudio();
  const { toast } = useToast();
  const [newName, setNewName] = useState("");
  const [renaming, setRenaming] = useState<string | null>(null);
  const [renameVal, setRenameVal] = useState("");

  const active = profiles.find((p) => p.id === activeProfileId);

  async function handleCreate(name: string, dupFrom?: string) {
    if (!name.trim()) return;
    await createProfile(name.trim(), dupFrom);
    setNewName("");
    toast({ title: "Profile created", description: name.trim() });
  }

  async function handleActivate(id: string, name: string) {
    await activateProfile(id);
    toast({ title: "Profile switched", description: name });
  }

  async function handleDelete(id: string, name: string) {
    await deleteProfile(id);
    toast({ title: "Profile deleted", description: name });
  }

  async function handleRename(id: string) {
    if (!renameVal.trim()) return;
    await renameProfile(id, renameVal.trim());
    setRenaming(null);
    setRenameVal("");
  }

  return (
    <>
      <DropdownMenu open={profilesOpen} onOpenChange={setProfilesOpen}>
        <DropdownMenuTrigger asChild>
          <Button variant="outline" size="sm" className="h-8 gap-1.5 max-w-[180px]">
            <Layers className="size-3.5 text-primary shrink-0" />
            <span className="truncate">{active?.name ?? config.profileName}</span>
            {dirty ? (
              <span className="size-1.5 rounded-full bg-amber-400 shrink-0" />
            ) : null}
            <ChevronDown className="size-3 text-muted-foreground shrink-0" />
          </Button>
        </DropdownMenuTrigger>
        <DropdownMenuContent align="end" className="w-72">
          <DropdownMenuLabel className="text-[10px] uppercase tracking-wide text-muted-foreground">
            Switch profile
          </DropdownMenuLabel>
          <div className="max-h-64 overflow-y-auto thin-scroll">
            {profiles.map((p) => (
              <DropdownMenuItem
                key={p.id}
                className="flex items-center gap-2 py-2 cursor-pointer"
                onClick={() => handleActivate(p.id, p.name)}
              >
                <div className="min-w-0 flex-1">
                  <div className="flex items-center gap-1.5">
                    <span className="text-xs font-medium truncate">{p.name}</span>
                    {p.isDefault ? (
                      <Star className="size-3 text-primary fill-primary" />
                    ) : null}
                  </div>
                  <div className="text-[9px] text-muted-foreground font-mono">
                    {p.summary.resolution} · {p.summary.fps}fps · -{p.summary.limitsRemoved} · {p.summary.patches}p
                  </div>
                </div>
                {p.id === activeProfileId ? (
                  <Check className="size-3.5 text-primary shrink-0" />
                ) : null}
              </DropdownMenuItem>
            ))}
          </div>
          <DropdownMenuSeparator />
          <div className="p-2 space-y-2">
            <div className="flex gap-1.5">
              <Input
                placeholder="New profile name…"
                value={newName}
                onChange={(e) => setNewName(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === "Enter") handleCreate(newName);
                }}
                className="h-7 text-xs"
              />
              <Button
                size="sm"
                variant="outline"
                className="h-7 px-2 shrink-0"
                onClick={() => handleCreate(newName)}
                disabled={!newName.trim()}
              >
                <Plus className="size-3.5" />
              </Button>
            </div>
            <p className="text-[9px] text-muted-foreground">
              New profile saves the current config. Use the manager to duplicate or delete.
            </p>
          </div>
        </DropdownMenuContent>
      </DropdownMenu>
    </>
  );
}

export function ProfilesDialog({
  open,
  onOpenChange,
}: {
  open: boolean;
  onOpenChange: (o: boolean) => void;
}) {
  const {
    profiles,
    activeProfileId,
    activateProfile,
    createProfile,
    deleteProfile,
    renameProfile,
    refreshProfiles,
    config,
  } = useStudio();
  const { toast } = useToast();
  const [renaming, setRenaming] = useState<string | null>(null);
  const [renameVal, setRenameVal] = useState("");
  const [newName, setNewName] = useState("");
  const fileInputRef = useRef<HTMLInputElement>(null);

  function handleExport(id: string, name: string) {
    const a = document.createElement("a");
    a.href = `/api/recompiler/profiles/${id}/export`;
    a.download = `${name.replace(/[^a-z0-9-_]+/gi, "_").slice(0, 40) || "profile"}.hst.json`;
    a.click();
    toast({ title: "Profile exported", description: name });
  }

  async function handleImportFile(file: File) {
    try {
      const text = await file.text();
      const data = JSON.parse(text);
      if (!data || typeof data !== "object" || !data.config) {
        throw new Error("Invalid profile file (missing config)");
      }
      const name = (data.name ?? file.name.replace(/\.hst\.json$/i, "") ?? "Imported").toString();
      const res = await fetch("/api/recompiler/profiles", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name, config: data.config }),
      });
      if (res.ok) {
        await refreshProfiles();
        toast({ title: "Profile imported", description: name });
      } else {
        throw new Error("server rejected import");
      }
    } catch (e) {
      toast({ title: "Import failed", description: String(e), variant: "destructive" });
    }
  }

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-2xl">
        <DialogHeader>
          <DialogTitle>Profile manager</DialogTitle>
          <DialogDescription>
            Save, switch, duplicate, rename or delete configuration profiles. Each profile stores
            the full graphics/performance/limits/controllers/patches setup.
          </DialogDescription>
        </DialogHeader>

        <div className="flex gap-1.5 mb-3">
          <Input
            placeholder="New profile name (saves current config)…"
            value={newName}
            onChange={(e) => setNewName(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "Enter" && newName.trim()) {
                createProfile(newName.trim());
                setNewName("");
                toast({ title: "Profile created", description: newName.trim() });
              }
            }}
            className="h-8 text-xs"
          />
          <Button
            size="sm"
            className="h-8 gap-1.5 shrink-0"
            disabled={!newName.trim()}
            onClick={() => {
              createProfile(newName.trim());
              toast({ title: "Profile created", description: newName.trim() });
              setNewName("");
            }}
          >
            <Plus className="size-3.5" /> Save current as new
          </Button>
          <input
            ref={fileInputRef}
            type="file"
            accept=".json,.hst.json,application/json"
            className="hidden"
            onChange={(e) => {
              const f = e.target.files?.[0];
              if (f) handleImportFile(f);
              e.target.value = "";
            }}
          />
          <Button
            size="sm"
            variant="outline"
            className="h-8 gap-1.5 shrink-0"
            onClick={() => fileInputRef.current?.click()}
          >
            <Upload className="size-3.5" /> Import
          </Button>
        </div>

        <div className="rounded-lg border border-border/60 divide-y divide-border/50 max-h-80 overflow-y-auto thin-scroll">
          {profiles.map((p) => {
            const isActive = p.id === activeProfileId;
            const isRenaming = renaming === p.id;
            return (
              <div key={p.id} className={cn("flex items-center gap-2 px-3 py-2.5", isActive && "bg-primary/5")}>
                <Star className={cn("size-3.5 shrink-0", isActive ? "text-primary fill-primary" : "text-muted-foreground/40")} />
                <div className="min-w-0 flex-1">
                  {isRenaming ? (
                    <Input
                      autoFocus
                      value={renameVal}
                      onChange={(e) => setRenameVal(e.target.value)}
                      onKeyDown={(e) => {
                        if (e.key === "Enter" && renameVal.trim()) {
                          renameProfile(p.id, renameVal.trim());
                          setRenaming(null);
                        }
                        if (e.key === "Escape") setRenaming(null);
                      }}
                      className="h-6 text-xs"
                    />
                  ) : (
                    <>
                      <div className="text-xs font-medium truncate">{p.name}</div>
                      <div className="flex items-center gap-1.5 mt-0.5 flex-wrap">
                        <Badge variant="outline" className="text-[8px] h-4 px-1 font-mono">
                          {p.summary.resolution}
                        </Badge>
                        <Badge variant="outline" className="text-[8px] h-4 px-1 font-mono">
                          {p.summary.fps}fps
                        </Badge>
                        <Badge variant="outline" className="text-[8px] h-4 px-1 font-mono">
                          -{p.summary.limitsRemoved}
                        </Badge>
                        <Badge variant="outline" className="text-[8px] h-4 px-1 font-mono">
                          {p.summary.patches}p
                        </Badge>
                        <Badge variant="outline" className="text-[8px] h-4 px-1 font-mono capitalize">
                          {p.summary.strategy}
                        </Badge>
                      </div>
                    </>
                  )}
                </div>
                <div className="flex items-center gap-0.5 shrink-0">
                  <Button
                    size="sm"
                    variant="ghost"
                    className="size-7 p-0"
                    title="Export as JSON"
                    onClick={() => handleExport(p.id, p.name)}
                  >
                    <Download className="size-3.5" />
                  </Button>
                  <Button
                    size="sm"
                    variant="ghost"
                    className="size-7 p-0"
                    title="Duplicate"
                    onClick={() => {
                      createProfile(`${p.name} (copy)`, p.id);
                      toast({ title: "Profile duplicated", description: p.name });
                    }}
                  >
                    <Copy className="size-3.5" />
                  </Button>
                  <Button
                    size="sm"
                    variant="ghost"
                    className="size-7 p-0"
                    title="Rename"
                    onClick={() => {
                      setRenaming(p.id);
                      setRenameVal(p.name);
                    }}
                  >
                    <Pencil className="size-3.5" />
                  </Button>
                  <Button
                    size="sm"
                    variant="ghost"
                    className="size-7 p-0 hover:text-amber-300"
                    title="Delete"
                    disabled={isActive || profiles.length <= 1}
                    onClick={() => handleDelete(p.id, p.name)}
                  >
                    <Trash2 className="size-3.5" />
                  </Button>
                </div>
              </div>
            );
          })}
        </div>
      </DialogContent>
    </Dialog>
  );

  async function handleDelete(id: string, name: string) {
    await deleteProfile(id);
    toast({ title: "Profile deleted", description: name });
  }
}
