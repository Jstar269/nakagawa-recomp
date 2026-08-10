"use client";

import { useEffect, useState, useMemo } from "react";
import {
  FolderOpen, FileImage, Volume2, Network, Search, Database,
  ChevronRight, ChevronDown, Library, Music, Layers, Eye, Loader2, HelpCircle
} from "lucide-react";
import { Panel, SectionHeader } from "./ui-bits";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";

interface AssetFile {
  name: string;
  path: string;
  png_path?: string;
  width?: number;
  height?: number;
  size_bytes?: number;
}

interface ArchiveNode {
  name: string;
  path: string;
  type: string;
  texturesCount: number;
  soundsCount: number;
  sceneGraphsCount: number;
  otherCount: number;
  textures: AssetFile[];
  sounds: AssetFile[];
  sceneGraphs: AssetFile[];
  other: AssetFile[];
}

export function AssetsPanel() {
  const [archives, setArchives] = useState<ArchiveNode[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  // Selection states
  const [selectedArchive, setSelectedArchive] = useState<ArchiveNode | null>(null);
  const [selectedFile, setSelectedFile] = useState<{ file: AssetFile; archivePath: string; type: "texture" | "sound" | "layout" | "other" } | null>(null);
  const [expandedFolders, setExpandedFolders] = useState<Record<string, boolean>>({});

  // Search & Filter
  const [searchQuery, setSearchQuery] = useState("");
  const [activeTab, setActiveTab] = useState<"textures" | "sounds" | "layouts" | "all">("textures");

  useEffect(() => {
    async function loadAssets() {
      try {
        const res = await fetch("/api/recompiler/assets");
        if (!res.ok) {
          const detail = await res.json().catch(() => ({}));
          throw new Error(detail.detail || "Failed to load assets data");
        }
        const data = await res.json();
        setArchives(data.archives || []);
        if (data.archives && data.archives.length > 0) {
          setSelectedArchive(data.archives[0]);
        }
      } catch (err: any) {
        setError(err.message);
      } finally {
        setLoading(false);
      }
    }
    loadAssets();
  }, []);

  // Compute folder hierarchy of archives
  const folderTree = useMemo(() => {
    const root: Record<string, any> = {};
    for (const arc of archives) {
      const parts = arc.name.split("/");
      let current = root;
      for (let i = 0; i < parts.length; i++) {
        const part = parts[i];
        if (i === parts.length - 1) {
          current[part] = { __archive: arc };
        } else {
          if (!current[part]) current[part] = {};
          current = current[part];
        }
      }
    }
    return root;
  }, [archives]);

  const toggleFolder = (folderKey: string) => {
    setExpandedFolders(prev => ({
      ...prev,
      [folderKey]: !prev[folderKey]
    }));
  };

  // Stats
  const stats = useMemo(() => {
    let textures = 0;
    let sounds = 0;
    let layouts = 0;
    let totalSize = 0;

    for (const arc of archives) {
      textures += arc.texturesCount;
      sounds += arc.soundsCount;
      layouts += arc.sceneGraphsCount;

      const sumSize = (list: AssetFile[]) => list.reduce((acc, f) => acc + (f.size_bytes ?? 0), 0);
      totalSize += sumSize(arc.textures) + sumSize(arc.sounds) + sumSize(arc.sceneGraphs) + sumSize(arc.other);
    }
    return { textures, sounds, layouts, totalSize, archives: archives.length };
  }, [archives]);

  // Search filter
  const filteredFiles = useMemo(() => {
    if (!selectedArchive) return { textures: [], sounds: [], layouts: [], other: [] };

    const filterList = (list: AssetFile[]) => {
      if (!searchQuery) return list;
      const q = searchQuery.toLowerCase();
      return list.filter(f => f.name.toLowerCase().includes(q) || f.path.toLowerCase().includes(q));
    };

    return {
      textures: filterList(selectedArchive.textures),
      sounds: filterList(selectedArchive.sounds),
      layouts: filterList(selectedArchive.sceneGraphs),
      other: filterList(selectedArchive.other)
    };
  }, [selectedArchive, searchQuery]);

  const formatSize = (bytes?: number) => {
    if (bytes === undefined) return "0 B";
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  };

  const renderFolderNode = (node: Record<string, any>, name: string, currentPath: string = "") => {
    const fullPath = currentPath ? `${currentPath}/${name}` : name;

    if (node.__archive) {
      const arc = node.__archive as ArchiveNode;
      const isSelected = selectedArchive?.name === arc.name;
      return (
        <button
          key={arc.name}
          onClick={() => {
            setSelectedArchive(arc);
            setSelectedFile(null);
          }}
          className={cn(
            "w-full flex items-center gap-2 rounded px-2 py-1 text-left text-[11px] font-mono transition-colors",
            isSelected
              ? "bg-primary/20 text-primary border-l-2 border-primary"
              : "text-muted-foreground hover:text-foreground hover:bg-accent/30"
          )}
        >
          <Layers className="size-3.5 shrink-0 text-amber-500" />
          <span className="truncate flex-1">{name}</span>
          <Badge variant="outline" className="text-[9px] px-1 h-4 scale-90 opacity-80 border-border">
            {arc.texturesCount + arc.soundsCount + arc.sceneGraphsCount}
          </Badge>
        </button>
      );
    }

    const isExpanded = expandedFolders[fullPath];
    return (
      <div key={fullPath} className="space-y-0.5">
        <button
          onClick={() => toggleFolder(fullPath)}
          className="w-full flex items-center gap-1.5 rounded px-2 py-1 text-left text-[11px] font-semibold text-muted-foreground hover:text-foreground hover:bg-accent/20 transition-colors"
        >
          {isExpanded ? <ChevronDown className="size-3.5" /> : <ChevronRight className="size-3.5" />}
          <FolderOpen className="size-3.5 text-sky-400 shrink-0" />
          <span className="truncate">{name}</span>
        </button>
        {isExpanded && (
          <div className="pl-3.5 border-l border-border/40 ml-3 space-y-0.5">
            {Object.keys(node).sort().map(key => renderFolderNode(node[key], key, fullPath))}
          </div>
        )}
      </div>
    );
  };

  if (loading) {
    return (
      <div className="flex flex-col items-center justify-center p-20 text-center">
        <Loader2 className="size-10 text-primary animate-spin mb-4" />
        <div className="text-xs font-semibold text-muted-foreground">Loading the local asset inventory…</div>
        <div className="text-[10px] text-muted-foreground/70 mt-1">Large extractions can take several seconds on first load.</div>
      </div>
    );
  }

  if (error) {
    return (
      <div className="flex flex-col items-center justify-center p-12 text-center border border-rose-500/20 bg-rose-500/5 rounded-xl">
        <Database className="size-10 text-rose-400 mb-3" />
        <div className="text-xs font-bold text-rose-300">Failed to load asset index</div>
        <p className="text-[11px] text-muted-foreground mt-1 max-w-md">{error}</p>
        <p className="text-[10px] text-amber-300 mt-2">Make sure you have run the Asset Extraction pipeline (`BuildFull` action or run `extract_xb.py` manually).</p>
      </div>
    );
  }

  return (
    <div className="space-y-4">
      <SectionHeader
        icon={<FolderOpen className="size-4.5" />}
        title="Asset Virtual Tree Map"
        subtitle="Browse unswizzled textures, audio stream descriptors, and scene graphs loaded from the PSP source."
      />

      {/* Summary Cards */}
      <div className="grid grid-cols-2 md:grid-cols-4 gap-3">
        <div className="rounded-lg border border-border/60 bg-card/40 p-3 flex flex-col justify-between">
          <span className="text-[10px] uppercase font-semibold text-muted-foreground tracking-wide">Archives</span>
          <span className="text-xl font-bold font-mono mt-1 text-primary">{stats.archives}</span>
        </div>
        <div className="rounded-lg border border-border/60 bg-card/40 p-3 flex flex-col justify-between">
          <span className="text-[10px] uppercase font-semibold text-muted-foreground tracking-wide">GIM Textures</span>
          <span className="text-xl font-bold font-mono mt-1 text-sky-400">{stats.textures}</span>
        </div>
        <div className="rounded-lg border border-border/60 bg-card/40 p-3 flex flex-col justify-between">
          <span className="text-[10px] uppercase font-semibold text-muted-foreground tracking-wide">Sound Streams</span>
          <span className="text-xl font-bold font-mono mt-1 text-amber-400">{stats.sounds}</span>
        </div>
        <div className="rounded-lg border border-border/60 bg-card/40 p-3 flex flex-col justify-between">
          <span className="text-[10px] uppercase font-semibold text-muted-foreground tracking-wide">Layouts (MAP1)</span>
          <span className="text-xl font-bold font-mono mt-1 text-purple-400">{stats.layouts}</span>
        </div>
      </div>

      {/* Main split layout */}
      <div className="grid lg:grid-cols-[280px_1fr] gap-4">
        {/* Left Side: Archive Tree */}
        <div className="rounded-lg border border-border/60 bg-card/30 p-3 h-[550px] flex flex-col gap-2">
          <span className="text-[10px] uppercase font-bold text-muted-foreground tracking-wider mb-1 flex items-center gap-1.5">
            <Library className="size-3.5 text-primary" /> Archives Tree
          </span>
          <div className="flex-1 overflow-y-auto thin-scroll space-y-1 pr-1">
            {Object.keys(folderTree).sort().map(key => renderFolderNode(folderTree[key], key))}
          </div>
        </div>

        {/* Right Side: Archive Content Panel */}
        <div className="rounded-lg border border-border/60 bg-card/30 p-4 h-[550px] flex flex-col gap-3 overflow-hidden">
          {selectedArchive ? (
            <>
              {/* Header */}
              <div className="flex flex-wrap items-start justify-between gap-3 border-b border-border/40 pb-3 shrink-0">
                <div>
                  <h3 className="text-xs font-bold font-mono text-primary flex items-center gap-2">
                    <Layers className="size-4 text-amber-500" />
                    {selectedArchive.name}
                  </h3>
                  <div className="text-[10px] text-muted-foreground mt-1">
                    Path: <span className="font-mono text-foreground">{selectedArchive.path}</span>
                  </div>
                </div>

                {/* Search in Archive */}
                <div className="flex items-center gap-2 relative">
                  <Search className="size-3.5 text-muted-foreground absolute left-2" />
                  <input
                    type="text"
                    placeholder="Search files inside archive..."
                    value={searchQuery}
                    onChange={(e) => setSearchQuery(e.target.value)}
                    className="h-7 pl-7 pr-2.5 rounded-md border border-border/60 bg-background/50 text-[10px] placeholder:text-muted-foreground focus:outline-none focus:border-primary/50 w-48"
                  />
                </div>
              </div>

              {/* Tabs */}
              <div className="flex border-b border-border/40 shrink-0 text-[10px]">
                <button
                  onClick={() => setActiveTab("textures")}
                  className={cn(
                    "px-3 py-1.5 font-semibold border-b-2 -mb-px transition-colors",
                    activeTab === "textures" ? "border-primary text-primary" : "border-transparent text-muted-foreground"
                  )}
                >
                  Textures ({filteredFiles.textures.length})
                </button>
                <button
                  onClick={() => setActiveTab("sounds")}
                  className={cn(
                    "px-3 py-1.5 font-semibold border-b-2 -mb-px transition-colors",
                    activeTab === "sounds" ? "border-primary text-primary" : "border-transparent text-muted-foreground"
                  )}
                >
                  Sounds ({filteredFiles.sounds.length})
                </button>
                <button
                  onClick={() => setActiveTab("layouts")}
                  className={cn(
                    "px-3 py-1.5 font-semibold border-b-2 -mb-px transition-colors",
                    activeTab === "layouts" ? "border-primary text-primary" : "border-transparent text-muted-foreground"
                  )}
                >
                  Layouts ({filteredFiles.layouts.length})
                </button>
                <button
                  onClick={() => setActiveTab("all")}
                  className={cn(
                    "px-3 py-1.5 font-semibold border-b-2 -mb-px transition-colors",
                    activeTab === "all" ? "border-primary text-primary" : "border-transparent text-muted-foreground"
                  )}
                >
                  Other ({filteredFiles.other.length})
                </button>
              </div>

              {/* View Lists */}
              <div className="flex-1 overflow-y-auto thin-scroll pr-1">
                {/* 1. Textures Grid */}
                {activeTab === "textures" && (
                  <div className="grid grid-cols-2 sm:grid-cols-3 md:grid-cols-4 gap-3">
                    {filteredFiles.textures.length === 0 ? (
                      <div className="col-span-full py-10 text-center text-muted-foreground text-xs italic">
                        No textures in this archive match search.
                      </div>
                    ) : (
                      filteredFiles.textures.map((file) => {
                        const pathString = `${selectedArchive.path}/${file.png_path}`;
                        return (
                          <div
                            key={file.name}
                            onClick={() => setSelectedFile({ file, archivePath: selectedArchive.path, type: "texture" })}
                            className="group flex flex-col justify-between border border-border/40 rounded-lg p-2 bg-background/40 hover:bg-accent/15 hover:border-primary/30 transition-all cursor-pointer overflow-hidden relative"
                          >
                            <div className="aspect-square w-full bg-black/40 rounded border border-border/20 flex items-center justify-center overflow-hidden mb-2 relative">
                              {file.png_path ? (
                                <img
                                  src={`/api/recompiler/assets/file?path=${encodeURIComponent(pathString)}`}
                                  alt={file.name}
                                  className="max-h-full max-w-full object-contain pixelated"
                                  onError={(e) => {
                                    // Fallback to placeholder if error
                                    e.currentTarget.style.display = "none";
                                  }}
                                />
                              ) : (
                                <FileImage className="size-8 text-muted-foreground/40" />
                              )}
                            </div>
                            <span className="text-[10px] font-mono font-semibold truncate block" title={file.name}>
                              {file.name}
                            </span>
                            <span className="text-[9px] text-muted-foreground font-mono mt-1">
                              {file.width} x {file.height} · {formatSize(file.size_bytes)}
                            </span>
                          </div>
                        );
                      })
                    )}
                  </div>
                )}

                {/* 2. Sounds List */}
                {activeTab === "sounds" && (
                  <div className="space-y-1">
                    {filteredFiles.sounds.length === 0 ? (
                      <div className="py-10 text-center text-muted-foreground text-xs italic">
                        No sound streams in this archive match search.
                      </div>
                    ) : (
                      filteredFiles.sounds.map((file) => (
                        <div
                          key={file.name}
                          onClick={() => setSelectedFile({ file, archivePath: selectedArchive.path, type: "sound" })}
                          className="flex items-center justify-between border border-border/30 rounded px-2.5 py-2 bg-background/20 hover:bg-accent/15 transition-all cursor-pointer text-xs"
                        >
                          <div className="flex items-center gap-2">
                            <Volume2 className="size-4 text-amber-400" />
                            <span className="font-mono">{file.name}</span>
                          </div>
                          <span className="font-mono text-muted-foreground text-[10px]">{formatSize(file.size_bytes)}</span>
                        </div>
                      ))
                    )}
                  </div>
                )}

                {/* 3. Layouts (MAP1) List */}
                {activeTab === "layouts" && (
                  <div className="space-y-1">
                    {filteredFiles.layouts.length === 0 ? (
                      <div className="py-10 text-center text-muted-foreground text-xs italic">
                        No scene-graph layouts in this archive match search.
                      </div>
                    ) : (
                      filteredFiles.layouts.map((file) => (
                        <div
                          key={file.name}
                          onClick={() => setSelectedFile({ file, archivePath: selectedArchive.path, type: "layout" })}
                          className="flex items-center justify-between border border-border/30 rounded px-2.5 py-2 bg-background/20 hover:bg-accent/15 transition-all cursor-pointer text-xs"
                        >
                          <div className="flex items-center gap-2">
                            <Network className="size-4 text-purple-400" />
                            <span className="font-mono">{file.name}</span>
                          </div>
                          <span className="font-mono text-muted-foreground text-[10px]">{formatSize(file.size_bytes)}</span>
                        </div>
                      ))
                    )}
                  </div>
                )}

                {/* 4. Other List */}
                {activeTab === "all" && (
                  <div className="space-y-1">
                    {filteredFiles.other.length === 0 ? (
                      <div className="py-10 text-center text-muted-foreground text-xs italic">
                        No other assets found.
                      </div>
                    ) : (
                      filteredFiles.other.map((file) => (
                        <div
                          key={file.name}
                          className="flex items-center justify-between border border-border/30 rounded px-2.5 py-2 bg-background/20 text-xs"
                        >
                          <div className="flex items-center gap-2">
                            <HelpCircle className="size-4 text-sky-400" />
                            <span className="font-mono text-muted-foreground">{file.name}</span>
                          </div>
                          <span className="font-mono text-muted-foreground text-[10px]">{formatSize(file.size_bytes)}</span>
                        </div>
                      ))
                    )}
                  </div>
                )}
              </div>
            </>
          ) : (
            <div className="flex flex-col items-center justify-center p-20 text-center my-auto">
              <FolderOpen className="size-10 text-muted-foreground/40 mb-2" />
              <div className="text-xs font-semibold text-muted-foreground">Select an archive from the tree to view its assets</div>
            </div>
          )}
        </div>
      </div>

      {/* Details Box Lightbox */}
      {selectedFile && (
        <div className="fixed inset-0 bg-black/80 flex items-center justify-center p-4 z-50 animate-fade-in">
          <div className="rounded-lg border border-border bg-card max-w-lg w-full p-5 space-y-4">
            <div className="flex justify-between items-start">
              <div>
                <h4 className="text-sm font-bold font-mono text-primary truncate max-w-sm">
                  {selectedFile.file.name}
                </h4>
                <p className="text-[10px] text-muted-foreground mt-0.5">
                  Type: <span className="uppercase text-foreground font-semibold">{selectedFile.type}</span>
                </p>
              </div>
              <Button size="sm" variant="outline" onClick={() => setSelectedFile(null)}>Close</Button>
            </div>

            {/* Content Preview */}
            <div className="bg-black/50 border border-border/40 rounded-lg p-4 flex flex-col items-center justify-center min-h-48 max-h-[300px] overflow-hidden">
              {selectedFile.type === "texture" && (
                <img
                  src={`/api/recompiler/assets/file?path=${encodeURIComponent(`${selectedFile.archivePath}/${selectedFile.file.png_path}`)}`}
                  alt={selectedFile.file.name}
                  className="max-h-full max-w-full object-contain pixelated"
                />
              )}

              {selectedFile.type === "sound" && (
                <div className="w-full space-y-3 flex flex-col items-center">
                  <Music className="size-10 text-amber-400 animate-bounce" />
                  {/* Try rendering standard HTML5 Audio */}
                  <audio
                    src={`/api/recompiler/assets/file?path=${encodeURIComponent(`${selectedFile.archivePath}/${selectedFile.file.path}`)}`}
                    controls
                    className="w-full max-w-xs"
                  />
                  <div className="text-[9px] text-muted-foreground text-center">
                    Note: Native SAS ADPCM streams (.SGD/.SGB) require transcode to WAV on runtime play.
                  </div>
                </div>
              )}

              {selectedFile.type === "layout" && (
                <div className="flex flex-col items-center text-center gap-2">
                  <Layers className="size-10 text-purple-400" />
                  <span className="text-xs font-semibold">MAP1 Layout Scene Graph File</span>
                  <div className="text-[10px] text-muted-foreground leading-relaxed max-w-sm">
                    Contains binary UI/map layouts, node structure hierarchical vectors, and coordinate parameters.
                  </div>
                </div>
              )}
            </div>

            {/* File info */}
            <div className="grid grid-cols-2 gap-3 text-xs bg-accent/20 rounded-lg p-3 font-mono">
              <div>
                <span className="text-muted-foreground block text-[9px] uppercase">File size</span>
                <span className="font-semibold">{formatSize(selectedFile.file.size_bytes)}</span>
              </div>
              <div>
                <span className="text-muted-foreground block text-[9px] uppercase">Archive folder</span>
                <span className="font-semibold truncate block" title={selectedFile.archivePath}>{selectedFile.archivePath}</span>
              </div>
              {selectedFile.type === "texture" && (
                <>
                  <div>
                    <span className="text-muted-foreground block text-[9px] uppercase">Width</span>
                    <span className="font-semibold">{selectedFile.file.width} px</span>
                  </div>
                  <div>
                    <span className="text-muted-foreground block text-[9px] uppercase">Height</span>
                    <span className="font-semibold">{selectedFile.file.height} px</span>
                  </div>
                </>
              )}
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
