import { NextRequest } from "next/server";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { existsSync, readFileSync, realpathSync, statSync } from "node:fs";
import path from "node:path";

export const runtime = "nodejs";

function resolveAssetPath(assetRoot: string, requestedPath: string): string | null {
  const normalizedRoot = path.resolve(/* turbopackIgnore: true */ assetRoot);
  const candidate = path.resolve(/* turbopackIgnore: true */ normalizedRoot, requestedPath);
  const relative = path.relative(normalizedRoot, candidate);
  if (!relative || relative.startsWith("..") || path.isAbsolute(relative)) return null;

  // Preserve the caller's 404 behavior for absent files. Existing files need a
  // second containment check after symlink/junction resolution.
  if (!existsSync(/* turbopackIgnore: true */ candidate)) return candidate;
  const realRoot = realpathSync.native(/* turbopackIgnore: true */ normalizedRoot);
  const realCandidate = realpathSync.native(/* turbopackIgnore: true */ candidate);
  const realRelative = path.relative(realRoot, realCandidate);
  if (!realRelative || realRelative.startsWith("..") || path.isAbsolute(realRelative)) return null;
  if (!statSync(/* turbopackIgnore: true */ realCandidate).isFile()) return null;
  return realCandidate;
}

// GET /api/recompiler/assets/file?path=menu/010_inpane.xb.d/data/menu/loadmes/A00.png
export async function GET(req: NextRequest) {
  const url = new URL(req.url);
  const relPath = url.searchParams.get("path");
  if (!relPath) return new Response("Missing path parameter", { status: 400 });

  try {
    const repoRoot = findRepoRoot();
    const assetRoot = path.join(/* turbopackIgnore: true */ repoRoot, "place_game_here", "EXTRACTED", "PSP_GAME", "USRDIR", "xbdata_extracted");
    const fullPath = resolveAssetPath(assetRoot, relPath);

    if (!fullPath) {
      return new Response("Invalid asset path", { status: 400 });
    }

    if (!existsSync(/* turbopackIgnore: true */ fullPath)) {
      return new Response("Asset file not found", { status: 404 });
    }

    const data = readFileSync(/* turbopackIgnore: true */ fullPath);
    const ext = path.extname(fullPath).toLowerCase();

    let contentType = "application/octet-stream";
    if (ext === ".png") {
      contentType = "image/png";
    } else if (ext === ".wav") {
      contentType = "audio/wav";
    } else if (ext === ".at3") {
      contentType = "audio/at3";
    } else if (ext === ".vag") {
      contentType = "audio/vag";
    } else if (ext === ".gim") {
      contentType = "image/gim";
    }

    return new Response(data, {
      headers: {
        "Content-Type": contentType,
        "Cache-Control": "private, no-store",
        "X-Content-Type-Options": "nosniff",
      },
    });
  } catch (err) {
    return new Response(`Error reading asset file: ${err}`, { status: 500 });
  }
}
