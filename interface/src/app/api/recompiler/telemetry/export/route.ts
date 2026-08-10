import { NextResponse } from "next/server";
import { db } from "@/lib/db";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { buildZip, ZipEntry } from "@/lib/recompiler/zip";
import { existsSync, readFileSync, readdirSync, statSync, lstatSync, realpathSync } from "node:fs";
import path from "node:path";

export const runtime = "nodejs";

const MAX_DB_BYTES = 50 * 1024 * 1024; // 50 MB
const MAX_INVENTORY_BYTES = 20 * 1024 * 1024; // 20 MB
const MAX_ZIP_BYTES = 100 * 1024 * 1024; // 100 MB
const MAX_INVENTORY_FILES = 5000;
const MAX_PER_FILE_BYTES = 16 * 1024 * 1024; // 16 MB per auxiliary report/inventory file
const MAX_TELEMETRY_HISTORY_BYTES = 16 * 1024 * 1024; // 16 MB serialized telemetry history

/** Read a file only when it fits the byte budget; null otherwise (#189). */
function readBoundedFile(pathName: string, maxBytes: number): Uint8Array | null {
  try {
    if (statSync(pathName).size > maxBytes) return null;
    return new Uint8Array(readFileSync(pathName));
  } catch {
    return null;
  }
}

interface AssetFile {
  name: string;
  path: string;
  png_path?: string;
  width?: number;
  height?: number;
  size_bytes?: number;
}

interface InventoryMap {
  textures: AssetFile[];
  sounds: AssetFile[];
  scene_graphs: AssetFile[];
  other: AssetFile[];
}

function walkDir(dir: string, baseRoot: string, fileList: string[] = []): string[] {
  if (!existsSync(dir)) return fileList;
  if (fileList.length >= MAX_INVENTORY_FILES) return fileList;

  const realBase = realpathSync(baseRoot);
  const files = readdirSync(dir);

  for (const file of files) {
    const filePath = path.join(dir, file);
    try {
      // Symlink escape containment check
      const lstat = lstatSync(filePath);
      if (lstat.isSymbolicLink()) continue;

      const realTarget = realpathSync(filePath);
      if (!realTarget.startsWith(realBase)) continue;

      const stat = statSync(filePath);
      if (stat.isDirectory()) {
        walkDir(filePath, baseRoot, fileList);
      } else if (file === "inventory_map.json") {
        fileList.push(filePath);
      }
    } catch {
      // Skip inaccessible or broken entries
    }
  }
  return fileList;
}

const WARNING_MANIFEST = `========================================================================
PRIVATE DIAGNOSTIC TELEMETRY EXPORT — DO NOT PUBLISH OR UPLOAD PUBLICLY
========================================================================
This export contains local diagnostic data, database telemetry, and game
inventory metadata intended ONLY for local debugging and private developer use.

DO NOT upload this ZIP file to public issue trackers, repositories, or AI services.
`;

export async function GET() {
  try {
    const repoRoot = findRepoRoot();
    const entries: ZipEntry[] = [];

    // 0. Include Warning Manifest
    entries.push({
      name: "README_PRIVATE_DIAGNOSTIC_DATA.txt",
      data: new TextEncoder().encode(WARNING_MANIFEST),
    });

    // 1. Pack the Prisma SQLite Database (dev.db) with size check
    const dbPath = path.join(repoRoot, "interface", "prisma", "dev.db");
    if (existsSync(dbPath)) {
      const st = statSync(dbPath);
      if (st.size > MAX_DB_BYTES) {
        return NextResponse.json(
          { error: "db-too-large", detail: `Database size (${st.size} bytes) exceeds limit of ${MAX_DB_BYTES} bytes.` },
          { status: 413 }
        );
      }
      entries.push({
        name: "dev.db",
        data: new Uint8Array(readFileSync(dbPath)),
      });
    }

    // 2. Fetch and serialize telemetry history (bounded rows AND bounded bytes)
    try {
      const runs = await db.telemetryRun.findMany({
        take: 10000,
        orderBy: { timestamp: "asc" },
      });
      const historyBytes = new TextEncoder().encode(JSON.stringify(runs, null, 2));
      if (historyBytes.length > MAX_TELEMETRY_HISTORY_BYTES) {
        console.error("Export: telemetry history exceeds the serialization budget");
      } else {
        entries.push({
          name: "telemetry_history.json",
          data: historyBytes,
        });
      }
    } catch (dbErr) {
      console.error("Export: Telemetry fetch skipped:", dbErr);
    }

    // 3. Pack visual_regression_report.json (bounded read)
    const vrPath = path.join(repoRoot, "visual_regression_report.json");
    if (existsSync(vrPath)) {
      const vrBytes = readBoundedFile(vrPath, MAX_PER_FILE_BYTES);
      if (vrBytes) {
        entries.push({
          name: "visual_regression_report.json",
          data: vrBytes,
        });
      }
    }

    // 4. Gather and pack the combined asset inventory map
    const extractedDir = path.join(repoRoot, "place_game_here", "EXTRACTED", "PSP_GAME", "USRDIR", "xbdata_extracted");
    const combinedInventory: any[] = [];
    if (existsSync(extractedDir)) {
      const inventoryFiles = walkDir(extractedDir, extractedDir);
      for (const invPath of inventoryFiles) {
        const invDir = path.dirname(invPath);
        const relDir = path.relative(extractedDir, invDir).replace(/\\/g, "/");
        try {
          const invBytes = readBoundedFile(invPath, MAX_PER_FILE_BYTES);
          if (!invBytes) continue; // oversized inventory file: skip, do not parse
          const raw = JSON.parse(new TextDecoder().decode(invBytes)) as InventoryMap;
          combinedInventory.push({
            archive: relDir.replace(".xb.d", ".xb"),
            path: relDir,
            texturesCount: raw.textures?.length ?? 0,
            soundsCount: raw.sounds?.length ?? 0,
            sceneGraphsCount: raw.scene_graphs?.length ?? 0,
            otherCount: raw.other?.length ?? 0,
            textures: raw.textures ?? [],
            sounds: raw.sounds ?? [],
            sceneGraphs: raw.scene_graphs ?? [],
            other: raw.other ?? [],
          });
        } catch {
          // Skip corrupt JSONs
        }
      }
    }

    const inventoryBytes = new TextEncoder().encode(JSON.stringify(combinedInventory, null, 2));
    if (inventoryBytes.length > MAX_INVENTORY_BYTES) {
      return NextResponse.json(
        { error: "inventory-too-large", detail: `Combined inventory map size (${inventoryBytes.length} bytes) exceeds limit of ${MAX_INVENTORY_BYTES} bytes.` },
        { status: 413 }
      );
    }

    entries.push({
      name: "inventory_map.json",
      data: inventoryBytes,
    });

    // 5. Generate ZIP
    const zipBytes = buildZip(entries);
    if (zipBytes.length > MAX_ZIP_BYTES) {
      return NextResponse.json(
        { error: "export-zip-too-large", detail: `Export ZIP size (${zipBytes.length} bytes) exceeds limit of ${MAX_ZIP_BYTES} bytes.` },
        { status: 413 }
      );
    }

    return new Response(zipBytes as any, {
      headers: {
        "Content-Type": "application/zip",
        "Content-Disposition": "attachment; filename=private-diagnostic-telemetry-export.zip",
        "Content-Length": String(zipBytes.length),
      },
    });

  } catch (e) {
    return NextResponse.json({ error: "export-failed", detail: String(e) }, { status: 500 });
  }
}
