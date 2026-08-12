import { NextResponse } from "next/server";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import path from "node:path";

export const runtime = "nodejs";

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

let cachedTree: unknown[] | null = null;
let cachedAt = 0;
const CACHE_MS = 5 * 60 * 1000;

function walkDir(dir: string, fileList: string[] = []): string[] {
  const files = readdirSync(dir);
  for (const file of files) {
    const filePath = path.join(dir, file);
    if (statSync(filePath).isDirectory()) {
      walkDir(filePath, fileList);
    } else if (file === "inventory_map.json") {
      fileList.push(filePath);
    }
  }
  return fileList;
}

export async function GET() {
  try {
    const repoRoot = findRepoRoot();
    const extractedDir = path.join(repoRoot, "place_game_here", "EXTRACTED", "PSP_GAME", "USRDIR", "xbdata_extracted");

    if (!existsSync(extractedDir)) {
      return NextResponse.json({ error: "extracted-assets-missing", detail: "Asset extraction folder not found" }, { status: 404 });
    }

    if (cachedTree && Date.now() - cachedAt < CACHE_MS) {
      const cachedResponse = NextResponse.json({ success: true, archives: cachedTree });
      cachedResponse.headers.set("Cache-Control", "private, max-age=60");
      cachedResponse.headers.set("X-Content-Type-Options", "nosniff");
      return cachedResponse;
    }

    const inventoryFiles = walkDir(extractedDir);
    const treeData: any[] = [];

    for (const invPath of inventoryFiles) {
      const invDir = path.dirname(invPath);
      const relDir = path.relative(extractedDir, invDir).replace(/\\/g, "/");

      try {
        const raw = JSON.parse(readFileSync(invPath, "utf8")) as InventoryMap;

        // Add node for the archive
        treeData.push({
          name: relDir.replace(".xb.d", ".xb"),
          path: relDir,
          type: "archive",
          texturesCount: raw.textures?.length ?? 0,
          soundsCount: raw.sounds?.length ?? 0,
          sceneGraphsCount: raw.scene_graphs?.length ?? 0,
          otherCount: raw.other?.length ?? 0,
          textures: raw.textures ?? [],
          sounds: raw.sounds ?? [],
          sceneGraphs: raw.scene_graphs ?? [],
          other: raw.other ?? [],
        });
      } catch (err) {
        // Skip bad json
      }
    }

    cachedTree = treeData;
    cachedAt = Date.now();
    const response = NextResponse.json({
      success: true,
      archives: treeData,
    });
    response.headers.set("Cache-Control", "private, max-age=60");
    response.headers.set("X-Content-Type-Options", "nosniff");
    return response;
  } catch (e) {
    return NextResponse.json({ error: "assets-fetch-failed", detail: String(e) }, { status: 500 });
  }
}
