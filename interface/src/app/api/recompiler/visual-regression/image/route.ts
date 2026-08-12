import { NextRequest } from "next/server";
import { readFileSync, existsSync } from "node:fs";
import { findRepoRoot } from "@/lib/recompiler/runner";
import { parseP6Ppm, PpmFormatError } from "@/lib/recompiler/ppm.mjs";
import path from "node:path";
import sharp from "sharp";

export const runtime = "nodejs";

const SAFE_IMAGE_NAME = /^[A-Za-z0-9][A-Za-z0-9_.-]*\.(?:png|jpe?g|ppm)$/i;

// RGBA difference-buffer ceiling for the diff path (issue #174).  PSP capture
// workflows are 480x272 (≈0.5 MB RGBA); this headroom covers arbitrarily
// larger local captures while keeping allocations provably bounded.
const MAX_DIFF_BUFFER_BYTES = 256 * 1024 * 1024;

// Shared bounded loader (issue #174): P6 PPMs are parsed by the validated
// ppm.mjs parser (dimension/byte-budget limits, exact payload, deterministic
// truncation rejection); PNG/JPEG are decoded through sharp, which applies its
// own validation.
async function getRawPixels(imgPath: string): Promise<{ data: Buffer, width: number, height: number, channels: number }> {
  const fileBuf = readFileSync(/* turbopackIgnore: true */ imgPath);
  const ppm = parseP6Ppm(fileBuf);
  if (ppm) {
    return { data: Buffer.from(ppm.data), width: ppm.width, height: ppm.height, channels: 3 };
  }
  const { data, info } = await sharp(fileBuf).raw().toBuffer({ resolveWithObject: true });
  return { data, width: info.width, height: info.height, channels: info.channels };
}

export async function GET(req: NextRequest) {
  const { searchParams } = new URL(req.url);
  const type = searchParams.get("type"); // "snapshot", "golden", or "diff"
  const file = searchParams.get("file"); // e.g. "frame_0001.ppm"

  if (!type || !file) {
    return new Response("Missing type or file parameter", { status: 400 });
  }

  const cleanFile = path.basename(file);
  if (cleanFile !== file || !SAFE_IMAGE_NAME.test(cleanFile)) {
    return new Response("Invalid image filename", { status: 400 });
  }
  const repoRoot = findRepoRoot();

  if (type === "diff") {
    const snapPath1 = path.join(/* turbopackIgnore: true */ repoRoot, "build", "snapshots", cleanFile);
    const snapPath2 = path.join(/* turbopackIgnore: true */ repoRoot, cleanFile);
    const snapPath = existsSync(/* turbopackIgnore: true */ snapPath1) ? snapPath1 : snapPath2;
    const goldPath = path.join(/* turbopackIgnore: true */ repoRoot, "build", "golden", cleanFile);

    if (!existsSync(/* turbopackIgnore: true */ snapPath) || !existsSync(/* turbopackIgnore: true */ goldPath)) {
      return new Response(`Snapshot or golden reference missing for ${cleanFile}`, { status: 404 });
    }

    try {
      const snapPixels = await getRawPixels(snapPath);
      const goldPixels = await getRawPixels(goldPath);

      if (snapPixels.width !== goldPixels.width || snapPixels.height !== goldPixels.height) {
        return new Response("Dimension mismatch between snapshot and golden reference", { status: 400 });
      }

      // Check optional channels parameter
      const channelsParam = searchParams.get("channels");
      const activeChannels = channelsParam ? channelsParam.split(",") : ["R", "G", "B", "A"];

      const w = snapPixels.width;
      const h = snapPixels.height;
      const chSnap = snapPixels.channels;
      const chGold = goldPixels.channels;
      const bufA = snapPixels.data;
      const bufB = goldPixels.data;

      // Checked arithmetic before any allocation (issue #174): dimensions are
      // already bounded by the shared PPM loader / sharp, but the RGBA
      // difference buffer must still be proven safe-integer and in budget
      // before Buffer.alloc, never trusting raw multiplication.
      const pixelCount = w * h;
      if (!Number.isSafeInteger(pixelCount) || pixelCount * 4 > MAX_DIFF_BUFFER_BYTES) {
        return new Response("Difference buffer exceeds the safe allocation budget", { status: 400 });
      }

      const diffBuf = Buffer.alloc(pixelCount * 4); // RGBA

      for (let p = 0; p < w * h; p++) {
        const idxA = p * chSnap;
        const idxB = p * chGold;

        const rA = bufA[idxA];
        const gA = bufA[idxA + 1];
        const bA = bufA[idxA + 2];
        const aA = chSnap === 4 ? bufA[idxA + 3] : 255;

        const rB = bufB[idxB];
        const gB = bufB[idxB + 1];
        const bB = bufB[idxB + 2];
        const aB = chGold === 4 ? bufB[idxB + 3] : 255;

        const dR = activeChannels.includes("R") ? Math.abs(rA - rB) : 0;
        const dG = activeChannels.includes("G") ? Math.abs(gA - gB) : 0;
        const dB = activeChannels.includes("B") ? Math.abs(bA - bB) : 0;
        const dA = activeChannels.includes("A") ? Math.abs(aA - aB) : 0;

        if (Math.max(dR, dG, dB, dA) > 3) {
          diffBuf[p * 4] = 255;     // R
          diffBuf[p * 4 + 1] = 0;   // G
          diffBuf[p * 4 + 2] = 128; // B (Neon Pink)
          diffBuf[p * 4 + 3] = 255; // A (Opaque)
        } else {
          diffBuf[p * 4] = 0;
          diffBuf[p * 4 + 1] = 0;
          diffBuf[p * 4 + 2] = 0;
          diffBuf[p * 4 + 3] = 0;   // Transparent
        }
      }

      const pngBuf = await sharp(diffBuf, {
        raw: {
          width: w,
          height: h,
          channels: 4
        }
      })
      .png()
      .toBuffer();

      return new Response(new Uint8Array(pngBuf), {
        headers: {
          "Content-Type": "image/png",
          "Cache-Control": "public, max-age=10"
        }
      });
    } catch (e) {
      if (e instanceof PpmFormatError) {
        return new Response(`Malformed PPM: ${e.message}`, { status: 400 });
      }
      return new Response(`Failed to generate difference mask: ${String(e)}`, { status: 500 });
    }
  }

  let imgPath = "";

  if (type === "snapshot") {
    // Check both build/snapshots and repository root for snap files
    const snapshotPath = path.join(/* turbopackIgnore: true */ repoRoot, "build", "snapshots", cleanFile);
    const rootSnapPath = path.join(/* turbopackIgnore: true */ repoRoot, cleanFile);
    if (existsSync(/* turbopackIgnore: true */ snapshotPath)) {
      imgPath = snapshotPath;
    } else if (existsSync(/* turbopackIgnore: true */ rootSnapPath)) {
      imgPath = rootSnapPath;
    } else {
      imgPath = snapshotPath;
    }
  } else if (type === "golden") {
    imgPath = path.join(/* turbopackIgnore: true */ repoRoot, "build", "golden", cleanFile);
  } else {
    return new Response("Invalid type parameter", { status: 400 });
  }

  if (!existsSync(/* turbopackIgnore: true */ imgPath)) {
    return new Response(`File not found: ${type}/${cleanFile}`, { status: 404 });
  }


  try {
    const fileBuf = readFileSync(/* turbopackIgnore: true */ imgPath);

    // Shared bounded P6 loader (issue #174); falls back to sharp for
    // PNG/JPEG.  Malformed/oversized PPMs surface as deterministic 400s.
    const ppm = parseP6Ppm(fileBuf);
    if (ppm) {
      const pngBuf = await sharp(Buffer.from(ppm.data), {
        raw: {
          width: ppm.width,
          height: ppm.height,
          channels: 3
        }
      })
      .png()
      .toBuffer();

      return new Response(new Uint8Array(pngBuf), {
        headers: {
          "Content-Type": "image/png",
          "Cache-Control": "public, max-age=60"
        }
      });
    }

    // Normalize supported PNG/JPEG inputs to a known response format instead
    // of guessing their content type from an extension.
    const pngBuf = await sharp(fileBuf).png().toBuffer();
    return new Response(new Uint8Array(pngBuf), {
      headers: {
        "Content-Type": "image/png",
        "Cache-Control": "public, max-age=60"
      }
    });
  } catch (e) {
    if (e instanceof PpmFormatError) {
      return new Response(`Malformed PPM: ${e.message}`, { status: 400 });
    }
    return new Response(`Failed to convert image: ${String(e)}`, { status: 500 });
  }
}
