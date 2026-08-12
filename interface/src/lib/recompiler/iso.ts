import type { IsoMeta, IsoTreeNode } from "./types";
import { matchGame } from "./profiles";

// Minimal ISO9660 reader. PSP UMD images use an ISO9660 layer, so the Primary
// Volume Descriptor (sector 16 = offset 0x8000) is parseable as-is. We read
// only the sectors we need via a caller-supplied LBA reader so we never have
// to load a 1.8 GB ISO into memory.

const SECTOR = 2048;
const PVD_LBA = 16;

function ascii(buf: Uint8Array, offset: number, len: number): string {
  let s = "";
  for (let i = 0; i < len; i++) {
    const c = buf[offset + i];
    if (c === 0) break;
    s += String.fromCharCode(c);
  }
  return s.trim();
}

function le32(buf: Uint8Array, offset: number): number {
  return (
    (buf[offset] |
      (buf[offset + 1] << 8) |
      (buf[offset + 2] << 16) |
      (buf[offset + 3] << 24)) >>>
    0
  );
}

function isIso(buf: Uint8Array): boolean {
  // PVD at sector 16: type=1, "CD001" at offset 1.
  return buf[0] === 1 && ascii(buf, 1, 5) === "CD001";
}

export interface DirRecord {
  lba: number;
  size: number;
  flags: number;
  name: string;
  isDir: boolean;
}

function parseDirRecord(buf: Uint8Array, base: number): DirRecord | null {
  const len = buf[base];
  if (len === 0) return null;
  const lba = le32(buf, base + 2);
  const size = le32(buf, base + 10);
  const flags = buf[base + 25];
  const nameLen = buf[base + 32];
  let name = ascii(buf, base + 33, nameLen);
  if (name === "\u0000") name = ".";
  if (name === "\u0001") name = "..";
  return { lba, size, flags, name, isDir: (flags & 0x02) !== 0 };
}

// Walk the records inside a directory data buffer. Returns the list (excluding . and ..).
function listDir(buf: Uint8Array): DirRecord[] {
  const out: DirRecord[] = [];
  let off = 0;
  while (off < buf.length) {
    if (buf[off] === 0) {
      // A zero record pads the remainder of the current logical sector. A
      // multi-sector directory may continue with records in the next sector.
      off = Math.ceil((off + 1) / SECTOR) * SECTOR;
      continue;
    }
    const rec = parseDirRecord(buf, off);
    if (!rec) break;
    if (rec.name !== "." && rec.name !== "..") out.push(rec);
    off += buf[off];
  }
  return out;
}

// Reads a contiguous run of sectors via the caller's reader.
export type SectorReader = (lba: number, count: number) => Promise<Uint8Array>;

export async function readRun(
  read: SectorReader,
  lba: number,
  size: number,
): Promise<Uint8Array> {
  const count = Math.max(1, Math.ceil(size / SECTOR));
  return read(lba, count).then((b) => b.subarray(0, Math.min(size, b.length)));
}

// Count files recursively (depth-limited) and look for PSP_GAME/PARAM.SFO.
async function walkRoot(
  read: SectorReader,
  rootLba: number,
  rootSize: number,
): Promise<{ fileCount: number; sfoBytes: Uint8Array | null }> {
  let fileCount = 0;
  let sfoBytes: Uint8Array | null = null;

  async function walkDir(lba: number, size: number, depth: number) {
    if (depth > 3) return;
    const buf = await readRun(read, lba, size);
    const recs = listDir(buf);
    for (const r of recs) {
      if (r.isDir) {
        // Look specifically for PSP_GAME to find PARAM.SFO.
        if (r.name.toUpperCase() === "PSP_GAME" && depth === 0) {
          await walkDir(r.lba, r.size, depth + 1);
        } else if (depth > 0) {
          await walkDir(r.lba, r.size, depth + 1);
        }
      } else {
        fileCount++;
        if (
          !sfoBytes &&
          r.name.toUpperCase() === "PARAM.SFO"
        ) {
          sfoBytes = await readRun(read, r.lba, r.size);
        }
      }
    }
  }

  await walkDir(rootLba, rootSize, 0);
  return { fileCount, sfoBytes };
}

// Build a structured tree of the ISO's directory hierarchy (depth-limited to
// avoid pathological recursion). Returns the root's children.
export async function listIsoTree(
  read: SectorReader,
  rootLba: number,
  rootSize: number,
  maxDepth = 4,
): Promise<IsoTreeNode[]> {
  async function build(lba: number, size: number, depth: number): Promise<IsoTreeNode[]> {
    if (depth > maxDepth) return [];
    const buf = await readRun(read, lba, size);
    const recs = listDir(buf);
    const out: IsoTreeNode[] = [];
    for (const r of recs) {
      const node: IsoTreeNode = {
        name: r.name,
        isDir: r.isDir,
        size: r.size,
        lba: r.lba,
      };
      if (r.isDir) {
        node.children = await build(r.lba, r.size, depth + 1);
      }
      out.push(node);
    }
    return out;
  }
  return build(rootLba, rootSize, 0);
}

// Extract a DISC_ID (e.g. UCES-01420) from raw PARAM.SFO bytes.
function extractGameCode(sfo: Uint8Array): string | null {
  if (!sfo || sfo.length === 0) return null;
  const text = ascii(sfo, 0, Math.min(sfo.length, 4096));
  const m = text.match(/[A-Z]{4}-\d{5}/);
  return m ? m[0] : null;
}

function parseIsoDate(buf: Uint8Array, offset: number): string {
  const y = ascii(buf, offset, 4);
  const mo = ascii(buf, offset + 4, 2);
  const d = ascii(buf, offset + 6, 2);
  const h = ascii(buf, offset + 8, 2);
  const mi = ascii(buf, offset + 10, 2);
  const s = ascii(buf, offset + 12, 2);
  if (!y || y === "0000") return "";
  return `${y}-${mo}-${d} ${h}:${mi}:${s}`;
}

// Main entry: parse PVD + walk root. The reader must return the *full* PVD
// sector (2048 bytes) when called with lba=16, count=1.
export async function inspectIso(
  read: SectorReader,
  fileName: string,
  sizeBytes: number,
): Promise<IsoMeta> {
  const pvd = await read(PVD_LBA, 1);
  if (!isIso(pvd)) {
    return {
      fileName,
      sizeBytes,
      volumeId: "(not a valid ISO9660 image)",
      systemId: "",
      application: "",
      publisher: "",
      creationDate: "",
      fileCount: 0,
      gameCode: null,
      region: null,
      matchedTitle: null,
    };
  }
  const volumeId = ascii(pvd, 40, 32);
  const systemId = ascii(pvd, 8, 32);
  const application = ascii(pvd, 336, 128);
  const publisher = ascii(pvd, 318, 128);
  const creationDate = parseIsoDate(pvd, 813); // volume creation date
  const rootLba = le32(pvd, 158 + 2);
  const rootSize = le32(pvd, 158 + 10);

  const { fileCount, sfoBytes } = await walkRoot(read, rootLba, rootSize);
  const gameCode = extractGameCode(sfoBytes ?? new Uint8Array());
  const { matchedTitle, region } = matchGame(gameCode);
  const tree = await listIsoTree(read, rootLba, rootSize);

  return {
    fileName,
    sizeBytes,
    volumeId,
    systemId,
    application,
    publisher,
    creationDate,
    fileCount,
    gameCode,
    region,
    matchedTitle,
    tree,
  };
}
