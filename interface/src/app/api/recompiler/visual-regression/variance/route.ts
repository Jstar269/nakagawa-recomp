import { NextRequest, NextResponse } from "next/server";
import { readFileSync, existsSync } from "node:fs";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { parseP6Ppm, PpmFormatError } from "@/lib/recompiler/ppm.mjs";
import path from "node:path";
import sharp from "sharp";

export const runtime = "nodejs";

const SAFE_IMAGE_NAME = /^[A-Za-z0-9][A-Za-z0-9_.-]*\.(?:png|jpe?g|ppm)$/i;

// Variance pixel-count ceiling for the O(width*height) loops (issue #174).
// PSP captures are 480x272 (≈130k pixels); this headroom covers arbitrary
// larger local captures while keeping loop work provably bounded.
const MAX_VARIANCE_PIXELS = 16 * 1024 * 1024;

// Shared bounded loader (issue #174): P6 PPMs go through the validated
// ppm.mjs parser (dimension/byte limits, exact payload); PNG/JPEG through
// sharp.  This replaces the old local parseInt-based parser.
async function getRawPixels(imgPath: string): Promise<{ data: Buffer, width: number, height: number, channels: number }> {
  const fileBuf = readFileSync(imgPath);
  const ppm = parseP6Ppm(fileBuf);
  if (ppm) {
    return { data: Buffer.from(ppm.data), width: ppm.width, height: ppm.height, channels: 3 };
  }
  const { data, info } = await sharp(fileBuf).raw().toBuffer({ resolveWithObject: true });
  return { data, width: info.width, height: info.height, channels: info.channels };
}

export async function GET(req: NextRequest) {
  const { searchParams } = new URL(req.url);
  const file = searchParams.get("file");

  if (!file) {
    return NextResponse.json({ error: "Missing file parameter" }, { status: 400 });
  }

  const cleanFile = path.basename(file);
  if (cleanFile !== file || !SAFE_IMAGE_NAME.test(cleanFile)) {
    return NextResponse.json({ error: "Invalid image filename" }, { status: 400 });
  }
  const repoRoot = findRepoRoot();

  const snapPath1 = path.join(repoRoot, "build", "snapshots", cleanFile);
  const snapPath2 = path.join(repoRoot, cleanFile);
  const snapPath = existsSync(snapPath1) ? snapPath1 : snapPath2;
  const goldPath = path.join(repoRoot, "build", "golden", cleanFile);

  if (!existsSync(snapPath) || !existsSync(goldPath)) {
    return NextResponse.json({ error: "Snapshot or golden reference file missing" }, { status: 404 });
  }

  try {
    const snap = await getRawPixels(snapPath);
    const gold = await getRawPixels(goldPath);

    if (snap.width !== gold.width || snap.height !== gold.height) {
      return NextResponse.json({ error: "Dimension mismatch between frames" }, { status: 400 });
    }

    const w = snap.width;
    const h = snap.height;
    const bufA = snap.data;
    const bufB = gold.data;
    const chA = snap.channels;
    const chB = gold.channels;

    // Checked arithmetic before any O(w*h) work (issue #174): dimensions are
    // bounded by the shared loader / sharp, but the pixel count driving the
    // loops must be proven safe-integer and in budget, never trusted raw.
    const pixelCount = w * h;
    if (!Number.isSafeInteger(pixelCount) || pixelCount > MAX_VARIANCE_PIXELS) {
      return NextResponse.json({ error: "Pixel count exceeds the variance budget" }, { status: 400 });
    }

    // 1. Initialize histograms (16 bins, each of size 16)
    const histR = Array(16).fill(0);
    const histG = Array(16).fill(0);
    const histB = Array(16).fill(0);
    const histA = Array(16).fill(0);

    // 2. Initialize 30x17 spatial delta grid.  Cell sizes are floored, but
    //    clamped to >= 1 so dimensions below 30x17 can never produce zero
    //    cell sizes / division-by-zero or invalid grid indexing (issue #174).
    const gridCols = 30;
    const gridRows = 17;
    const cellWidth = Math.max(1, Math.floor(w / gridCols));
    const cellHeight = Math.max(1, Math.floor(h / gridRows));

    // Grid data accumulator
    const gridAcc = Array(gridRows * gridCols).fill(null).map(() => ({
      sumR: 0, sumG: 0, sumB: 0, sumA: 0, count: 0
    }));

    for (let y = 0; y < h; y++) {
      const rowIdx = Math.min(Math.floor(y / cellHeight), gridRows - 1);
      for (let x = 0; x < w; x++) {
        const colIdx = Math.min(Math.floor(x / cellWidth), gridCols - 1);
        const p = y * w + x;

        const idxA = p * chA;
        const idxB = p * chB;

        const rA = bufA[idxA];
        const gA = bufA[idxA + 1];
        const bA = bufA[idxA + 2];
        const aA = chA === 4 ? bufA[idxA + 3] : 255;

        const rB = bufB[idxB];
        const gB = bufB[idxB + 1];
        const bB = bufB[idxB + 2];
        const aB = chB === 4 ? bufB[idxB + 3] : 255;

        const dR = Math.abs(rA - rB);
        const dG = Math.abs(gA - gB);
        const dB = Math.abs(bA - bB);
        const dA = Math.abs(aA - aB);

        // Update histograms
        histR[Math.min(Math.floor(dR / 16), 15)]++;
        histG[Math.min(Math.floor(dG / 16), 15)]++;
        histB[Math.min(Math.floor(dB / 16), 15)]++;
        histA[Math.min(Math.floor(dA / 16), 15)]++;

        // Update grid cell accumulator
        const cellIdx = rowIdx * gridCols + colIdx;
        const cell = gridAcc[cellIdx];
        cell.sumR += dR;
        cell.sumG += dG;
        cell.sumB += dB;
        cell.sumA += dA;
        cell.count++;
      }
    }

    // Format spatial grid results
    const grid = gridAcc.map(cell => ({
      r: cell.count > 0 ? Math.round(cell.sumR / cell.count * 100) / 100 : 0,
      g: cell.count > 0 ? Math.round(cell.sumG / cell.count * 100) / 100 : 0,
      b: cell.count > 0 ? Math.round(cell.sumB / cell.count * 100) / 100 : 0,
      a: cell.count > 0 ? Math.round(cell.sumA / cell.count * 100) / 100 : 0,
    }));

    // Format histogram bins for charting
    const histogram = Array(16).fill(null).map((_, i) => ({
      range: `${i * 16}-${i * 16 + 15}`,
      R: histR[i],
      G: histG[i],
      B: histB[i],
      A: histA[i],
    }));

    return NextResponse.json({
      width: w,
      height: h,
      histogram,
      grid: {
        rows: gridRows,
        cols: gridCols,
        cells: grid
      }
    });

  } catch (e) {
    if (e instanceof PpmFormatError) {
      return NextResponse.json({ error: "Malformed PPM", detail: e.message }, { status: 400 });
    }
    return NextResponse.json({ error: "Failed to parse spatial variance data", detail: String(e) }, { status: 500 });
  }
}
