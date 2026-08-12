"use client";

import React, { useCallback, useEffect, useRef, useState } from "react";
import { Terminal, Send, Play, Pause, RefreshCw } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Panel } from "./ui-bits";

interface ConsoleLine {
  type: "input" | "output" | "error" | "system";
  text: string;
}

interface DebugFrame {
  pc: string;
  sp: string;
  ra: string;
  note: string;
}

interface DebugCpuState {
  r: number[];
  hi: number;
  lo: number;
  pc: number;
}

interface DebugConsoleResponse {
  success?: boolean;
  error?: string;
  detail?: string;
  online?: boolean;
  mode?: "process" | "offline" | "simulation";
  status?: string;
  pid?: number;
  base_address?: string;
  rvas?: Record<string, string | number>;
  capabilities?: { liveControl?: boolean };
  frames?: DebugFrame[];
  cpu?: DebugCpuState;
  address?: string;
  hex?: string;
  words?: string[];
  string?: string;
  bytes_written?: number;
  field?: string;
  value_written?: number;
}

function responseError(response: DebugConsoleResponse, fallback: string) {
  return response.detail || response.error || fallback;
}

function connectionLabel(mode: DebugConsoleResponse["mode"]) {
  if (mode === "process") return "Process attached (hst.exe)";
  if (mode === "simulation") return "Standalone simulation";
  return "Offline (hst.exe not running)";
}

export function ExecutionConsole() {
  const [input, setInput] = useState("");
  const [lines, setLines] = useState<ConsoleLine[]>([
    { type: "system", text: "Nakagawa Recomp Runtime Diagnostics [v0.1]" },
    { type: "system", text: "Read-only by default. Type 'help' for available commands." },
  ]);
  const [history, setHistory] = useState<string[]>([]);
  const [historyIdx, setHistoryIdx] = useState(-1);
  const [status, setStatus] = useState<DebugConsoleResponse | null>(null);
  const [loading, setLoading] = useState(false);
  const hasPrintedTrace = useRef(false);
  const terminalEndRef = useRef<HTMLDivElement>(null);

  const fetchStatus = useCallback(async () => {
    try {
      const res = await fetch("/api/recompiler/debug/console", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ action: "status" }),
      });
      const data = (await res.json()) as DebugConsoleResponse;
      if (!res.ok) throw new Error(responseError(data, `Status request failed (${res.status})`));
      setStatus(data);

      if (data.status === "exited" && !hasPrintedTrace.current) {
        hasPrintedTrace.current = true;
        const traceRes = await fetch("/api/recompiler/debug/console", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ action: "trace_exit" }),
        });
        const traceData = (await traceRes.json()) as DebugConsoleResponse;

        if (traceRes.ok && traceData.success && traceData.frames) {
          const frameLines = traceData.frames.map((frame) =>
            `  PC: ${frame.pc} | SP: ${frame.sp} | RA: ${frame.ra} | ${frame.note}`
          );
          setLines((previous) => [
            ...previous,
            { type: "system", text: "--- GAME EXITED TRAP DETECTED ---" },
            { type: "output", text: "Crash dump stack trace unwound:" },
            ...frameLines.map((text: string) => ({ type: "output" as const, text })),
            { type: "system", text: "--- END STACK TRACE ---" },
          ]);
        }
      }

      return data;
    } catch {
      setStatus(null);
      return null;
    }
  }, []);

  useEffect(() => {
    const initial = window.setTimeout(() => {
      void fetchStatus();
    }, 0);
    const interval = window.setInterval(() => {
      void fetchStatus();
    }, 3000);
    return () => {
      window.clearTimeout(initial);
      window.clearInterval(interval);
    };
  }, [fetchStatus]);

  useEffect(() => {
    terminalEndRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [lines]);

  const executeApi = async (action: string, args: string[] = []): Promise<DebugConsoleResponse> => {
    setLoading(true);
    try {
      const res = await fetch("/api/recompiler/debug/console", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ action, args }),
      });
      return (await res.json()) as DebugConsoleResponse;
    } catch (e) {
      return { error: String(e) };
    } finally {
      setLoading(false);
    }
  };

  const handleCommand = async (cmdStr: string) => {
    const trimmed = cmdStr.trim();
    if (!trimmed) return;

    // Add to history
    setHistory((previous) => [trimmed, ...previous.filter((entry) => entry !== trimmed)]);
    setHistoryIdx(-1);

    // Print command input line
    setLines((previous) => [...previous, { type: "input", text: `> ${trimmed}` }]);
    setInput("");

    const parts = trimmed.split(/\s+/);
    const command = parts[0].toLowerCase();
    const args = parts.slice(1);

    if (command === "clear") {
      setLines([]);
      return;
    }

    if (command === "help") {
      setLines((previous) => [
        ...previous,
        { type: "system", text: "Available commands:" },
        { type: "output", text: "  status          Display current connection state and RVAs" },
        { type: "output", text: "  pause           Suspend hst.exe (requires live-control opt-in)" },
        { type: "output", text: "  resume          Resume hst.exe (requires live-control opt-in)" },
        { type: "output", text: "  regs            Dump active CpuState MIPS registers" },
        { type: "output", text: "  set <reg> <val> Write a register (requires live-control opt-in)" },
        { type: "output", text: "  read <addr> [sz] [fmt]  Read memory range. fmt: hex | string | words" },
        { type: "output", text: "  read_str <addr> [sz]    Read string from address" },
        { type: "output", text: "  write <addr> <hex>      Write bytes (requires live-control opt-in)" },
        { type: "output", text: "  trace                   Manually dump stack trace from crash_dump.bin" },
        { type: "output", text: "  clear                   Clear the console terminal screen" },
      ]);
      return;
    }

    if (command === "status") {
      const data = await fetchStatus();
      if (!data) {
        setLines((previous) => [...previous, { type: "error", text: "Failed to fetch status." }]);
        return;
      }
      setLines((previous) => [
        ...previous,
        { type: "output", text: `Connection Mode: ${connectionLabel(data.mode)}` },
        { type: "output", text: `Process Status: ${data.status}` },
        { type: "output", text: `PID: ${data.pid || "N/A"} | Base: ${data.base_address || "N/A"}` },
        { type: "output", text: `Symbol RVAs: g_mem=${data.rvas?.g_mem || "N/A"}, s_cpu=${data.rvas?.s_cpu || "N/A"}` },
        {
          type: "output",
          text: `Live Control: ${data.capabilities?.liveControl ? "enabled" : "disabled (read-only diagnostics)"}`,
        },
      ]);
      return;
    }

    if (command === "pause") {
      const res = await executeApi("pause");
      if (res.success) {
        setLines((previous) => [...previous, { type: "system", text: `Process paused successfully (${res.mode} mode).` }]);
        void fetchStatus();
      } else {
        setLines((previous) => [
          ...previous,
          { type: "error", text: `Failed to pause: ${responseError(res, "unknown error")}` },
        ]);
      }
      return;
    }

    if (command === "resume") {
      const res = await executeApi("resume");
      if (res.success) {
        setLines((previous) => [...previous, { type: "system", text: `Process resumed successfully (${res.mode} mode).` }]);
        void fetchStatus();
      } else {
        setLines((previous) => [
          ...previous,
          { type: "error", text: `Failed to resume: ${responseError(res, "unknown error")}` },
        ]);
      }
      return;
    }

    if (command === "read") {
      if (args.length < 1) {
        setLines((previous) => [...previous, { type: "error", text: "Usage: read <addr> [size] [format]" }]);
        return;
      }
      const addr = args[0];
      const size = args[1] || "16";
      const fmt = args[2] || "hex";

      const res = await executeApi("read_mem", [addr, size, fmt]);
      if (res.error) {
        setLines((previous) => [
          ...previous,
          { type: "error", text: `Read error: ${responseError(res, "unknown error")}` },
        ]);
      } else {
        if (res.hex) {
          setLines((previous) => [...previous, { type: "output", text: `${res.address}:  ${res.hex}` }]);
        } else if (res.words) {
          res.words.forEach((word, idx) => {
            const currentAddr = "0x" + (parseInt(addr, 16) + idx * 4).toString(16).toUpperCase();
            setLines((previous) => [...previous, { type: "output", text: `${currentAddr}:  ${word}` }]);
          });
        } else if (res.string) {
          setLines((previous) => [...previous, { type: "output", text: `${res.address}:  "${res.string}"` }]);
        }
      }
      return;
    }

    if (command === "read_str") {
      if (args.length < 1) {
        setLines((previous) => [...previous, { type: "error", text: "Usage: read_str <addr> [size]" }]);
        return;
      }
      const addr = args[0];
      const size = args[1] || "64";
      const res = await executeApi("read_mem", [addr, size, "string"]);
      if (res.error) {
        setLines((previous) => [
          ...previous,
          { type: "error", text: `Read error: ${responseError(res, "unknown error")}` },
        ]);
      } else {
        setLines((previous) => [...previous, { type: "output", text: `${res.address}:  "${res.string}"` }]);
      }
      return;
    }

    if (command === "write") {
      if (args.length < 2) {
        setLines((previous) => [...previous, { type: "error", text: "Usage: write <addr> <hex_bytes>" }]);
        return;
      }
      const addr = args[0];
      const bytes = args.slice(1).join(" ");
      const res = await executeApi("write_mem", [addr, bytes]);
      if (res.success) {
        setLines((previous) => [...previous, { type: "system", text: `Wrote ${res.bytes_written} bytes to ${res.address}.` }]);
      } else {
        setLines((previous) => [
          ...previous,
          { type: "error", text: `Write failed: ${responseError(res, "unknown error")}` },
        ]);
      }
      return;
    }

    if (command === "regs") {
      const res = await executeApi("read_cpu");
      if (res.error || !res.cpu) {
        setLines((previous) => [
          ...previous,
          { type: "error", text: `Failed to read registers: ${responseError(res, "missing CpuState")}` },
        ]);
        return;
      }
      const cpu = res.cpu;
      const outputLines: string[] = [];
      outputLines.push(`PC:  0x${cpu.pc.toString(16).toUpperCase()} | HI: 0x${cpu.hi.toString(16).toUpperCase()} | LO: 0x${cpu.lo.toString(16).toUpperCase()}`);

      const regNames = [
        "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3",
        "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
        "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
      ];

      for (let i = 0; i < 32; i += 4) {
        const row = [i, i+1, i+2, i+3].map((rIdx) => {
          const val = cpu.r[rIdx] || 0;
          return `$${regNames[rIdx]}(r${rIdx}): 0x${val.toString(16).toUpperCase()}`;
        }).join(" | ");
        outputLines.push(row);
      }

      setLines((previous) => [
        ...previous,
        { type: "system", text: `CpuState registers (${res.mode} mode):` },
        ...outputLines.map((text) => ({ type: "output" as const, text })),
      ]);
      return;
    }

    if (command === "set") {
      if (args.length < 2) {
        setLines((previous) => [...previous, { type: "error", text: "Usage: set <reg> <val>" }]);
        return;
      }
      const reg = args[0];
      const val = args[1];
      const res = await executeApi("write_cpu", [reg, val]);
      if (res.success) {
        setLines((previous) => [...previous, { type: "system", text: `Wrote ${res.field} = ${res.value_written}.` }]);
      } else {
        setLines((previous) => [
          ...previous,
          { type: "error", text: `Write failed: ${responseError(res, "unknown error")}` },
        ]);
      }
      return;
    }

    if (command === "trace") {
      const res = await executeApi("trace_exit");
      if (res.success && res.frames) {
        const frameLines = res.frames.map((frame) =>
          `  PC: ${frame.pc} | SP: ${frame.sp} | RA: ${frame.ra} | ${frame.note}`
        );
        setLines((previous) => [
          ...previous,
          { type: "system", text: "Crash dump stack trace unwound:" },
          ...frameLines.map((text) => ({ type: "output" as const, text })),
        ]);
      } else {
        setLines((previous) => [
          ...previous,
          { type: "error", text: `Trace failed: ${responseError(res, "unknown error")}` },
        ]);
      }
      return;
    }

    setLines((previous) => [
      ...previous,
      { type: "error", text: `Command not recognized: '${command}'. Type 'help' for command listing.` },
    ]);
  };

  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === "Enter") {
      handleCommand(input);
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      if (history.length > 0) {
        const nextIdx = Math.min(history.length - 1, historyIdx + 1);
        setHistoryIdx(nextIdx);
        setInput(history[nextIdx]);
      }
    } else if (e.key === "ArrowDown") {
      e.preventDefault();
      const nextIdx = historyIdx - 1;
      setHistoryIdx(nextIdx);
      if (nextIdx >= 0) {
        setInput(history[nextIdx]);
      } else {
        setInput("");
      }
    }
  };

  const togglePause = async () => {
    if (!status?.online || !status.capabilities?.liveControl) return;
    const isPaused = status.status === "paused";
    const res = await executeApi(isPaused ? "resume" : "pause");
    if (res.success) {
      void fetchStatus();
      setLines((previous) => [
        ...previous,
        { type: "system", text: `Process state updated: ${isPaused ? "Resumed (Running)" : "Suspended (Paused)"}` },
      ]);
    }
  };

  const isOnline = status?.online;
  const isPaused = status?.status === "paused";
  const liveControlEnabled = status?.capabilities?.liveControl === true;
  const badgeTone =
    status?.mode === "process" && status.online
      ? "bg-emerald-500/10 text-emerald-500 border-emerald-500/25"
      : status?.mode === "simulation"
        ? "bg-amber-500/10 text-amber-500 border-amber-500/25"
        : "bg-neutral-500/10 text-neutral-400 border-neutral-500/25";
  const badgeText =
    status?.mode === "process" && status.online
      ? "PROCESS ATTACHED"
      : status?.mode === "simulation"
        ? "STANDALONE SIMULATION"
        : "OFFLINE";

  return (
    <Panel
      title="Runtime Diagnostics Console"
      icon={<Terminal className="size-4" />}
      right={
        <div className="flex items-center gap-2">
          {status && (
            <span className={`text-[10px] px-2 py-0.5 rounded font-mono font-semibold border ${badgeTone}`}>
              {badgeText}
            </span>
          )}

          <Button
            variant="outline"
            size="icon"
            className="size-7 h-7 w-7"
            disabled={!isOnline || !liveControlEnabled}
            onClick={togglePause}
            title={
              liveControlEnabled
                ? isPaused
                  ? "Resume Process"
                  : "Pause Process"
                : "Live control is disabled; diagnostics remain read-only"
            }
          >
            {isPaused ? <Play className="size-3 text-emerald-500" /> : <Pause className="size-3 text-amber-500" />}
          </Button>

          <Button
            variant="outline"
            size="icon"
            className="size-7 h-7 w-7"
            onClick={() => void fetchStatus()}
            title="Refresh Connection Status"
          >
            <RefreshCw className="size-3" />
          </Button>
        </div>
      }
    >
      <div className="flex flex-col h-[400px]">
        {/* Terminal screen */}
        <div className="flex-1 overflow-y-auto rounded-lg border border-neutral-800 bg-neutral-950 p-3 font-mono text-xs leading-relaxed thin-scroll space-y-1">
          {lines.map((line, idx) => {
            let color = "text-neutral-300";
            if (line.type === "input") color = "text-primary/90 font-bold";
            else if (line.type === "error") color = "text-amber-500 font-semibold";
            else if (line.type === "system") color = "text-purple-400";
            else if (line.type === "output") color = "text-neutral-400";

            return (
              <div key={idx} className={`${color} whitespace-pre-wrap select-all`}>
                {line.text}
              </div>
            );
          })}
          {loading && (
            <div className="text-muted-foreground animate-pulse font-bold">
              Connecting...
            </div>
          )}
          <div ref={terminalEndRef} />
        </div>

        {/* Console Input */}
        <div className="flex items-center gap-2 mt-3 pt-1">
          <span className="font-mono text-xs text-primary font-bold select-none">&gt;</span>
          <Input
            value={input}
            onChange={(e) => setInput(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="Type 'help' for options..."
            className="flex-1 h-8 text-xs font-mono bg-black/20 border-neutral-800 focus:border-primary/50"
          />
          <Button
            onClick={() => void handleCommand(input)}
            size="icon"
            className="size-8 h-8 w-8 shrink-0"
            disabled={loading || input.trim().length === 0}
          >
            <Send className="size-3" />
          </Button>
        </div>
      </div>
    </Panel>
  );
}
