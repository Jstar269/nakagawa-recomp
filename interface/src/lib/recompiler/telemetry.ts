import { db } from "@/lib/db";
import { findRepoRoot, findLatestRunLog, readLogTailContent } from "./runner";
import { existsSync, readFileSync, statSync } from "node:fs";
import path from "node:path";

// Issue #189: every telemetry input is read with a hard byte budget.  Logs are
// scanned as a bounded tail (the most-recent section markers are what the
// parsers need); JSON documents larger than the budget are refused outright so
// an oversized/corrupt file can never be parsed into memory.
const TELEMETRY_LOG_BUDGET = 4 * 1024 * 1024; // 4 MiB for section-scanning logs
const TELEMETRY_JSON_BUDGET = 16 * 1024 * 1024; // 16 MiB for JSON documents

/**
 * Read a bounded tail of a log for section scanning.  Returns empty content on
 * any read error; callers treat empty content as "no data".
 */
function readBoundedLogTail(pathName: string): string {
  try {
    return readLogTailContent(pathName, TELEMETRY_LOG_BUDGET).content;
  } catch {
    return "";
  }
}

/**
 * Read a JSON document only if it fits the byte budget.  Returns null when the
 * file is missing, oversized, or unreadable (#189).
 */
function readBoundedJsonText(pathName: string): string | null {
  try {
    const st = statSync(pathName);
    if (st.size > TELEMETRY_JSON_BUDGET) return null;
    return readFileSync(pathName, "utf8");
  } catch {
    return null;
  }
}

const MIPS_REGS = [
  "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
  "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
  "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
];

function getFriendlyReg(regStr: string): string {
  const regIdx = parseInt(regStr, 10);
  if (!isNaN(regIdx) && regIdx >= 0 && regIdx < MIPS_REGS.length) {
    return `r${regIdx} (${MIPS_REGS[regIdx]})`;
  }
  return `r${regStr}`;
}

export function parsePerfProfiler(repoRoot: string) {
  try {
    const latestLog = findLatestRunLog(repoRoot);
    if (!latestLog || !latestLog.path || !existsSync(latestLog.path)) {
      return { functions: [], blocks: [], timestamp: null, lookupDrops: 0, watchpointStats: [] };
    }
    const content = readBoundedLogTail(latestLog.path);

    // Parse watch hits from the retained tail of the log.  A log larger than
    // TELEMETRY_LOG_BUDGET is scanned as a bounded tail (#189); watch hits from
    // before the retained window are not loaded into memory.
    const allLines = content.split(/\r?\n/);
    const watchHits: { label: string; type: "READ" | "WRITE"; pc: number }[] = [];
    const watchpointStatsMap = new Map<string, { label: string; reads: number; writes: number; hits: number }>();

    for (const line of allLines) {
      if (line.includes("MEM_WATCH[")) {
        const match = line.match(/MEM_WATCH\[(.*?)\]:\s+(WRITE|READ)\s+addr=(0x[0-9a-fA-F]+)\s+val=(0x[0-9a-fA-F]+)\s+pc=(0x[0-9a-fA-F]+)/);
        if (match) {
          const [_, label, type, addrStr, valStr, pcStr] = match;
          const pc = parseInt(pcStr, 16);
          watchHits.push({ label, type: type as "READ" | "WRITE", pc });

          if (!watchpointStatsMap.has(label)) {
            watchpointStatsMap.set(label, { label, reads: 0, writes: 0, hits: 0 });
          }
          const wstats = watchpointStatsMap.get(label)!;
          if (type === "READ") {
            wstats.reads++;
          } else {
            wstats.writes++;
          }
          wstats.hits++;
        }
      }
    }

    const lastIndex = content.lastIndexOf("--- PERF_PROFILE ---");
    if (lastIndex === -1) {
      return { functions: [], blocks: [], timestamp: null, lookupDrops: 0, watchpointStats: Array.from(watchpointStatsMap.values()) };
    }
    const endIndex = content.indexOf("--- END_PERF_PROFILE ---", lastIndex);
    const slice = endIndex !== -1
      ? content.substring(lastIndex, endIndex)
      : content.substring(lastIndex);

    const lines = slice.split(/\r?\n/);
    const functions: any[] = [];
    const blocks: any[] = [];
    let timestamp: number | null = null;
    let lookupDrops = 0;

    for (const line of lines) {
      if (line.startsWith("timestamp:")) {
        timestamp = Number(line.split(":")[1]);
      } else if (line.startsWith("lookup_drops:")) {
        lookupDrops = Number(line.split(":")[1]);
      } else if (line.startsWith("pc=")) {
        const match = line.match(/pc=(0x[0-9a-fA-F]+) calls=(\d+) blocks=(\d+) duration_ns=(\d+)/);
        if (match) {
          const [_, pcStr, callsStr, blocksStr, durationStr] = match;
          const calls = Number(callsStr);
          const blocksCount = Number(blocksStr);
          const durationNs = Number(durationStr);

          if (calls > 0) {
            functions.push({
              pc: pcStr,
              calls,
              durationNs,
              avgDurationNs: calls > 0 ? Math.round(durationNs / calls) : 0,
              readHits: 0,
              writeHits: 0
            });
          }
          if (blocksCount > 0) {
            blocks.push({
              pc: pcStr,
              count: blocksCount
            });
          }
        }
      }
    }

    // Map watchpoint hit PCs to containing functions
    if (watchHits.length > 0 && functions.length > 0) {
      const sortedFuncs = functions
        .map((f, idx) => ({ f, idx, pcNum: parseInt(f.pc, 16) }))
        .sort((a, b) => a.pcNum - b.pcNum);

      for (const hit of watchHits) {
        let low = 0;
        let high = sortedFuncs.length - 1;
        let matchIdx = -1;
        while (low <= high) {
          const mid = Math.floor((low + high) / 2);
          if (sortedFuncs[mid].pcNum <= hit.pc) {
            matchIdx = mid;
            low = mid + 1;
          } else {
            high = mid - 1;
          }
        }
        if (matchIdx !== -1) {
          const matched = sortedFuncs[matchIdx];
          if (hit.type === "READ") {
            functions[matched.idx].readHits++;
          } else {
            functions[matched.idx].writeHits++;
          }
        }
      }
    }

    functions.sort((a, b) => b.durationNs - a.durationNs);
    blocks.sort((a, b) => b.count - a.count);

    return {
      functions,
      blocks,
      timestamp,
      lookupDrops,
      watchpointStats: Array.from(watchpointStatsMap.values())
    };
  } catch (err) {
    console.error("Failed to parse performance profile log:", err);
    return { functions: [], blocks: [], timestamp: null, watchpointStats: [] };
  }
}

export function parseStaticVerifyLog(repoRoot: string) {
  try {
    const latestLog = findLatestRunLog(repoRoot);
    if (!latestLog || !latestLog.path || !existsSync(latestLog.path)) {
      return { count: 0, mismatches: [] };
    }
    const content = readBoundedLogTail(latestLog.path);
    const lines = content.split(/\r?\n/);
    const mismatches: any[] = [];
    for (const line of lines) {
      if (line.includes("SV_MISMATCH")) {
        const match = line.match(/SV_MISMATCH pc=(0x[0-9a-fA-F]+) r(\d+)=(0x[0-9a-fA-F]+) expected=(0x[0-9a-fA-F]+)/);
        if (match) {
          const [_, pc, reg, active, expected] = match;
          mismatches.push({
            pc,
            register: getFriendlyReg(reg),
            activeState: active,
            expectedLatticeState: expected,
          });
        }
      }
    }
    return { count: mismatches.length, mismatches };
  } catch (err) {
    console.error("Failed to parse static verification log:", err);
    return { count: 0, mismatches: [] };
  }
}

export function parseFuzzLog(repoRoot: string) {
  const logPath = path.join(repoRoot, "logs", "vfpu_fuzz_latest.log");
  if (!existsSync(logPath)) return null;
  try {
    const content = readBoundedLogTail(logPath);
    const lines = content.split(/\r?\n/);
    const curve: any[] = [];
    let totalTrials = 0;
    let passedTrials = 0;
    let failedTrials = 0;
    let testedWords = 0;
    let totalWords = 0;
    for (const line of lines) {
      if (line.startsWith("FUZZ_PROGRESS")) {
        const match = line.match(/FUZZ_PROGRESS case=(\d+) total=(\d+) passed=(\d+) failed=(\d+) op=(0x[0-9a-fA-F]+)/);
        if (match) {
          const [_, caseIdx, total, passed, failed, op] = match;
          const t = parseInt(total, 10);
          const p = parseInt(passed, 10);
          const f = parseInt(failed, 10);
          totalTrials += t;
          passedTrials += p;
          failedTrials += f;
          curve.push({
            caseIdx: parseInt(caseIdx, 10),
            total: t,
            passed: p,
            failed: f,
            op,
          });
        }
      } else if (line.includes("vfpu_fuzz:")) {
        const match = line.match(/vfpu_fuzz:\s+(\d+)\/(\d+)\s+distinct/);
        if (match) {
          testedWords = parseInt(match[1], 10);
          totalWords = parseInt(match[2], 10);
        }
      }
    }
    const coveragePct = totalWords > 0 ? (testedWords / totalWords) * 100 : 0;
    return {
      totalTrials,
      passedTrials,
      failedTrials,
      coveragePct,
      curve,
    };
  } catch (err) {
    console.error("Failed to parse fuzz log:", err);
    return null;
  }
}

export function parseVisualRegressionReport(repoRoot: string) {
  const p = path.join(repoRoot, "visual_regression_report.json");
  if (!existsSync(p)) {
    return {
      totalFrames: 0,
      passedFrames: 0,
      failedFrames: 0,
      passRate: 0.0,
    };
  }
  try {
    const reportText = readBoundedJsonText(p);
    if (reportText === null) {
      // Oversized/corrupt report: refuse rather than parse an unbounded blob.
      return {
        totalFrames: 0,
        passedFrames: 0,
        failedFrames: 0,
        passRate: 0.0,
      };
    }
    const report = JSON.parse(reportText);
    const summary = report.summary ?? {};
    return {
      totalFrames: Number(summary.total_frames ?? 0),
      passedFrames: Number(summary.passed_frames ?? 0),
      failedFrames: Number(summary.failed_frames ?? 0),
      passRate: Number(summary.pass_rate ?? 0.0),
    };
  } catch (err) {
    console.error("Failed to parse visual regression report:", err);
    return {
      totalFrames: 0,
      passedFrames: 0,
      failedFrames: 0,
      passRate: 0.0,
    };
  }
}

export async function logTelemetry() {
  try {
    const repoRoot = findRepoRoot();
    const progressPath = path.join(repoRoot, "progress.json");
    if (!existsSync(progressPath)) return null;

    const rawText = readBoundedJsonText(progressPath);
    if (rawText === null) return null; // oversized/unreadable progress.json (#189)
    const raw = JSON.parse(rawText);
    const totalUnits = Number(raw.total_units ?? raw.total ?? 0);
    const unitsEarned = Number(raw.units_earned ?? raw.earned ?? 0);
    const unitsRegressed = Number(raw.units_regressed ?? raw.regressed ?? 0);
    const completionPct = raw.completion_pct !== undefined ? Number(raw.completion_pct) : (totalUnits ? Math.round(((unitsEarned - unitsRegressed) / totalUnits) * 10000) / 100 : 0);

    const opengripProgress = Array.isArray(raw.opengrip_progress) ? raw.opengrip_progress : [];

    // 1. Sync historical opengrip progress entries
    for (const entry of opengripProgress) {
      if (!entry.timestamp) continue;
      const t = new Date(entry.timestamp);

      const existing = await db.telemetryRun.findFirst({
        where: { timestamp: t },
      });

      if (!existing) {
        await db.telemetryRun.create({
          data: {
            timestamp: t,
            totalUnits,
            unitsEarned,
            unitsRegressed,
            completionPct,
            totalFunctions: Number(entry.total_functions ?? 0),
            matchedFunctions: Number(entry.matched_functions ?? 0),
            totalBytes: Number(entry.total_bytes ?? 0),
            matchedBytes: Number(entry.matched_bytes ?? 0),
            byteCompletionPct: Number(entry.byte_completion_pct ?? 0.0),
            rawJson: JSON.stringify(entry),
            // Default new fields for old logs
            svMismatchesCount: 0,
            svMismatchesJson: "[]",
            fuzzTotalTrials: 0,
            fuzzPassedTrials: 0,
            fuzzFailedTrials: 0,
            fuzzCoveragePct: 0.0,
            fuzzCurveJson: "[]",
            vrTotalFrames: 0,
            vrPassedFrames: 0,
            vrFailedFrames: 0,
            vrPassRate: 0.0,
          },
        });
      }
    }

    // 2. Parse current snapshot of other telemetry sources
    const svData = parseStaticVerifyLog(repoRoot);
    const fuzzData = parseFuzzLog(repoRoot);
    const vrData = parseVisualRegressionReport(repoRoot);
    const profData = parsePerfProfiler(repoRoot);

    const latestEntry = opengripProgress[opengripProgress.length - 1];
    const currentRun = await db.telemetryRun.create({
      data: {
        timestamp: new Date(),
        totalUnits,
        unitsEarned,
        unitsRegressed,
        completionPct,
        totalFunctions: latestEntry ? Number(latestEntry.total_functions ?? 0) : 0,
        matchedFunctions: latestEntry ? Number(latestEntry.matched_functions ?? 0) : 0,
        totalBytes: latestEntry ? Number(latestEntry.total_bytes ?? 0) : 0,
        matchedBytes: latestEntry ? Number(latestEntry.matched_bytes ?? 0) : 0,
        byteCompletionPct: latestEntry ? Number(latestEntry.byte_completion_pct ?? 0.0) : 0.0,

        // Static Verification
        svMismatchesCount: svData.count,
        svMismatchesJson: JSON.stringify(svData.mismatches),

        // Fuzzing
        fuzzTotalTrials: fuzzData ? fuzzData.totalTrials : 0,
        fuzzPassedTrials: fuzzData ? fuzzData.passedTrials : 0,
        fuzzFailedTrials: fuzzData ? fuzzData.failedTrials : 0,
        fuzzCoveragePct: fuzzData ? fuzzData.coveragePct : 0.0,
        fuzzCurveJson: JSON.stringify(fuzzData ? fuzzData.curve : []),

        // Visual Regression
        vrTotalFrames: vrData.totalFrames,
        vrPassedFrames: vrData.passedFrames,
        vrFailedFrames: vrData.failedFrames,
        vrPassRate: vrData.passRate,

        rawJson: JSON.stringify({
          by_phase: raw.by_phase,
          perfProfile: {
            // Keep top 50 to avoid database bloating
            functions: profData.functions.slice(0, 50),
            blocks: profData.blocks.slice(0, 50),
            timestamp: profData.timestamp,
            lookupDrops: profData.lookupDrops,
            watchpointStats: profData.watchpointStats,
          }
        }),
      },
    });

    return currentRun;
  } catch (err) {
    console.error("Failed to log telemetry:", err);
    return null;
  }
}
