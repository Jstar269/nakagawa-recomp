export const DEBUG_CONSOLE_ACTIONS: readonly [
  "status",
  "pause",
  "resume",
  "read_mem",
  "write_mem",
  "read_cpu",
  "write_cpu",
  "read_vram",
  "trace_exit",
];

export type DebugConsoleAction = (typeof DEBUG_CONSOLE_ACTIONS)[number];

export interface DebugConsoleRequest {
  action: DebugConsoleAction;
  args: string[];
  mutating: boolean;
}

export function parseDebugConsoleRequest(value: unknown): DebugConsoleRequest;
