const ACTIONS = [
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

const ACTION_SET = new Set(ACTIONS);
const MUTATING_ACTIONS = new Set(["pause", "resume", "write_mem", "write_cpu"]);
const NO_ARGUMENT_ACTIONS = new Set(["status", "pause", "resume", "read_cpu", "read_vram", "trace_exit"]);
const MEMORY_FORMATS = new Set(["hex", "string", "words"]);

function isRecord(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function rejectUnknownKeys(value, allowed, scope) {
  const unknown = Object.keys(value).filter((key) => !allowed.has(key));
  if (unknown.length > 0) {
    throw new Error(`${scope} contains unsupported field(s): ${unknown.join(", ")}`);
  }
}

function requireUint32(value, label) {
  if (!/^(?:0x[0-9a-f]+|[0-9]+)$/i.test(value)) {
    throw new Error(`${label} must be an unsigned decimal or 0x-prefixed hexadecimal integer`);
  }
  const parsed = Number.parseInt(value, value.toLowerCase().startsWith("0x") ? 16 : 10);
  if (!Number.isSafeInteger(parsed) || parsed < 0 || parsed > 0xffff_ffff) {
    throw new Error(`${label} must fit in an unsigned 32-bit integer`);
  }
}

function requireArgumentCount(action, args, min, max = min) {
  if (args.length < min || args.length > max) {
    const expected = min === max ? `${min}` : `${min} to ${max}`;
    throw new Error(`${action} expects ${expected} argument(s)`);
  }
}

function validateReadMemory(args) {
  requireArgumentCount("read_mem", args, 2, 3);
  requireUint32(args[0], "read_mem address");
  if (!/^[0-9]+$/.test(args[1])) {
    throw new Error("read_mem size must be a decimal integer");
  }
  const size = Number.parseInt(args[1], 10);
  if (size < 1 || size > 4096) {
    throw new Error("read_mem size must be from 1 to 4096 bytes");
  }
  if (args[2] !== undefined && !MEMORY_FORMATS.has(args[2])) {
    throw new Error("read_mem format must be one of: hex, string, words");
  }
}

function validateWriteMemory(args) {
  requireArgumentCount("write_mem", args, 2);
  requireUint32(args[0], "write_mem address");
  const compactHex = args[1].replace(/[\s,]/g, "");
  if (compactHex.length === 0 || compactHex.length > 8192 || compactHex.length % 2 !== 0) {
    throw new Error("write_mem data must contain from 1 to 4096 complete bytes");
  }
  if (!/^[0-9a-f]+$/i.test(compactHex)) {
    throw new Error("write_mem data must contain hexadecimal bytes only");
  }
}

function validateWriteCpu(args) {
  requireArgumentCount("write_cpu", args, 2);
  const field = args[0];
  const validField =
    /^(?:r(?:[0-9]|[12][0-9]|3[01])|f(?:[0-9]|[12][0-9]|3[01])|v(?:[0-9]|[1-9][0-9]|1[01][0-9]|12[0-7])|hi|lo|pc|fcr31|fpcond|status|next_pc|in_delay_slot)$/.test(
      field,
    );
  if (!validField) {
    throw new Error("write_cpu field is not a supported CpuState register");
  }
  requireUint32(args[1], "write_cpu value");
}

export const DEBUG_CONSOLE_ACTIONS = Object.freeze([...ACTIONS]);

export function parseDebugConsoleRequest(value) {
  if (!isRecord(value)) throw new Error("request body must be a JSON object");
  rejectUnknownKeys(value, new Set(["action", "args"]), "request");

  if (typeof value.action !== "string" || !ACTION_SET.has(value.action)) {
    throw new Error(`action must be one of: ${ACTIONS.join(", ")}`);
  }
  const action = value.action;
  const rawArgs = value.args ?? [];
  if (!Array.isArray(rawArgs) || rawArgs.some((arg) => typeof arg !== "string")) {
    throw new Error("args must be an array of strings");
  }
  const args = [...rawArgs];

  if (NO_ARGUMENT_ACTIONS.has(action)) {
    requireArgumentCount(action, args, 0);
  } else if (action === "read_mem") {
    validateReadMemory(args);
  } else if (action === "write_mem") {
    validateWriteMemory(args);
  } else if (action === "write_cpu") {
    validateWriteCpu(args);
  }

  return {
    action,
    args,
    mutating: MUTATING_ACTIONS.has(action),
  };
}
