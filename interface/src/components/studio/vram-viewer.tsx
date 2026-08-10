"use client";

import React, { useState, useEffect, useRef } from "react";
import { Monitor, RefreshCw, Layers } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Panel } from "./ui-bits";

type PixelFormat = "RGBA8888" | "5650" | "5551" | "4444";

export function VramViewerPanel() {
  const [format, setFormat] = useState<PixelFormat>("RGBA8888");
  const [loading, setLoading] = useState(false);
  const [autoRefresh, setAutoRefresh] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);

  const width = 512;
  const height = format === "RGBA8888" ? 1024 : 2048;

  const fetchVram = async () => {
    setLoading(true);
    setError(null);
    try {
      const res = await fetch("/api/recompiler/debug/console", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ action: "read_vram" }),
      });
      const data = await res.json();

      if (data.error) {
        setError(data.error);
        return;
      }

      if (data.base64) {
        renderVram(data.base64);
      }
    } catch (e: any) {
      setError(e.message);
    } finally {
      setLoading(false);
    }
  };

  const renderVram = (b64: string) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    // Decode base64 to Uint8Array
    const binary = atob(b64);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }

    const imgData = ctx.createImageData(width, height);
    const out = imgData.data;

    let p = 0;
    if (format === "RGBA8888") {
      // 32-bit: R, G, B, A
      for (let i = 0; i < bytes.length && p < out.length; i += 4) {
        out[p++] = bytes[i];     // R
        out[p++] = bytes[i + 1]; // G
        out[p++] = bytes[i + 2]; // B
        out[p++] = bytes[i + 3]; // A
      }
    } else if (format === "5650") {
      // 16-bit: R5 G6 B5
      for (let i = 0; i < bytes.length && p < out.length; i += 2) {
        const val = bytes[i] | (bytes[i + 1] << 8);
        const r = (val & 0x1F);
        const g = ((val >> 5) & 0x3F);
        const b = ((val >> 11) & 0x1F);
        out[p++] = (r * 255) / 31;
        out[p++] = (g * 255) / 63;
        out[p++] = (b * 255) / 31;
        out[p++] = 255;
      }
    } else if (format === "5551") {
      // 16-bit: R5 G5 B5 A1
      for (let i = 0; i < bytes.length && p < out.length; i += 2) {
        const val = bytes[i] | (bytes[i + 1] << 8);
        const r = (val & 0x1F);
        const g = ((val >> 5) & 0x1F);
        const b = ((val >> 10) & 0x1F);
        const a = (val >> 15) & 0x1;
        out[p++] = (r * 255) / 31;
        out[p++] = (g * 255) / 31;
        out[p++] = (b * 255) / 31;
        out[p++] = a ? 255 : 0;
      }
    } else if (format === "4444") {
      // 16-bit: R4 G4 B4 A4
      for (let i = 0; i < bytes.length && p < out.length; i += 2) {
        const val = bytes[i] | (bytes[i + 1] << 8);
        const r = (val & 0xF);
        const g = ((val >> 4) & 0xF);
        const b = ((val >> 8) & 0xF);
        const a = ((val >> 12) & 0xF);
        out[p++] = (r * 255) / 15;
        out[p++] = (g * 255) / 15;
        out[p++] = (b * 255) / 15;
        out[p++] = (a * 255) / 15;
      }
    }

    ctx.putImageData(imgData, 0, 0);
  };

  useEffect(() => {
    let initial: ReturnType<typeof setTimeout> | undefined;
    let interval: ReturnType<typeof setInterval> | undefined;
    if (autoRefresh) {
      initial = setTimeout(fetchVram, 0);
      interval = setInterval(fetchVram, 2000);
    }
    return () => {
      if (initial) clearTimeout(initial);
      if (interval) clearInterval(interval);
    };
  }, [autoRefresh, format]);

  return (
    <Panel
      title="Live VRAM Inspector"
      icon={<Monitor className="size-4" />}
      right={
        <div className="flex gap-2">
          <Button
            variant={autoRefresh ? "default" : "outline"}
            size="sm"
            onClick={() => setAutoRefresh(!autoRefresh)}
            className="h-7 text-xs font-mono"
          >
            {autoRefresh ? "Stop Live" : "Live View"}
          </Button>
          <Button
            variant="outline"
            size="sm"
            onClick={fetchVram}
            disabled={loading || autoRefresh}
            className="h-7 px-2 gap-1 text-xs font-mono"
          >
            <RefreshCw className={`size-3 ${loading && !autoRefresh ? "animate-spin" : ""}`} />
            Refresh
          </Button>
        </div>
      }
    >
      <div className="space-y-4">
        {/* Controls */}
        <div className="flex items-center gap-3">
          <Layers className="size-4 text-muted-foreground" />
          <span className="text-xs font-mono text-muted-foreground">Pixel Format:</span>
          {(["RGBA8888", "5650", "5551", "4444"] as PixelFormat[]).map(f => (
            <Button
              key={f}
              variant={format === f ? "default" : "outline"}
              size="sm"
              onClick={() => { setFormat(f); if (!autoRefresh) fetchVram(); }}
              className="h-6 text-[10px] font-mono px-2"
            >
              {f}
            </Button>
          ))}
        </div>

        {error && (
          <div className="text-xs text-red-400 bg-red-950/30 p-2 rounded border border-red-500/20 font-mono">
            {error}
          </div>
        )}

        {/* Canvas Area */}
        <div className="w-full overflow-auto bg-black/40 rounded-xl border border-border/50 max-h-[500px] flex justify-center">
          <canvas
            ref={canvasRef}
            width={width}
            height={height}
            className="bg-black rendering-pixelated"
            style={{ imageRendering: "pixelated", maxWidth: "100%", height: "auto" }}
          />
        </div>
      </div>
    </Panel>
  );
}
