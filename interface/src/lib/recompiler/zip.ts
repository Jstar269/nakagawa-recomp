// Minimal pure-Node ZIP writer (STORE / no compression).
// Refactored to operate directly on Uint8Array buffers without intermediate number[] byte arrays.

const CRC32_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    t[i] = c >>> 0;
  }
  return t;
})();

export function crc32(buf: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    c = CRC32_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function dosDateTime(d: Date): { time: number; date: number } {
  const time =
    ((d.getHours() & 0x1f) << 11) | ((d.getMinutes() & 0x3f) << 5) | ((d.getSeconds() / 2) & 0x1f);
  const date =
    (((d.getFullYear() - 1980) & 0x7f) << 9) |
    (((d.getMonth() + 1) & 0x0f) << 5) |
    (d.getDate() & 0x1f);
  return { time, date };
}

export interface ZipEntry {
  name: string; // path inside zip
  data: Uint8Array;
}

function writeU16(buf: Uint8Array, offset: number, n: number): number {
  buf[offset] = n & 0xff;
  buf[offset + 1] = (n >>> 8) & 0xff;
  return offset + 2;
}

function writeU32(buf: Uint8Array, offset: number, n: number): number {
  buf[offset] = n & 0xff;
  buf[offset + 1] = (n >>> 8) & 0xff;
  buf[offset + 2] = (n >>> 16) & 0xff;
  buf[offset + 3] = (n >>> 24) & 0xff;
  return offset + 4;
}

export function buildZip(entries: ZipEntry[]): Uint8Array {
  const encoder = new TextEncoder();
  const now = new Date();
  const { time, date } = dosDateTime(now);

  interface EntryMeta {
    nameBytes: Uint8Array;
    crc: number;
    size: number;
    localOffset: number;
  }

  const metas: EntryMeta[] = [];
  let totalLocalSize = 0;
  let totalCentralSize = 0;

  for (const e of entries) {
    const nameBytes = encoder.encode(e.name);
    const crc = crc32(e.data);
    const size = e.data.length;
    const localHeaderLen = 30 + nameBytes.length + size;
    const centralHeaderLen = 46 + nameBytes.length;

    metas.push({
      nameBytes,
      crc,
      size,
      localOffset: totalLocalSize,
    });

    totalLocalSize += localHeaderLen;
    totalCentralSize += centralHeaderLen;
  }

  const eocdSize = 22;
  const totalZipSize = totalLocalSize + totalCentralSize + eocdSize;
  const out = new Uint8Array(totalZipSize);

  let p = 0;

  // 1. Write Local Headers + File Data
  for (let i = 0; i < entries.length; i++) {
    const e = entries[i];
    const m = metas[i];

    p = writeU32(out, p, 0x04034b50); // local header sig
    p = writeU16(out, p, 20); // version needed
    p = writeU16(out, p, 0); // flags
    p = writeU16(out, p, 0); // compression method (STORE)
    p = writeU16(out, p, time);
    p = writeU16(out, p, date);
    p = writeU32(out, p, m.crc);
    p = writeU32(out, p, m.size); // compressed size
    p = writeU32(out, p, m.size); // uncompressed size
    p = writeU16(out, p, m.nameBytes.length);
    p = writeU16(out, p, 0); // extra len

    out.set(m.nameBytes, p);
    p += m.nameBytes.length;

    out.set(e.data, p);
    p += m.size;
  }

  const centralStartOffset = p;

  // 2. Write Central Directory Headers
  for (let i = 0; i < entries.length; i++) {
    const m = metas[i];

    p = writeU32(out, p, 0x02014b50); // central header sig
    p = writeU16(out, p, 20); // version made by
    p = writeU16(out, p, 20); // version needed
    p = writeU16(out, p, 0); // flags
    p = writeU16(out, p, 0); // compression method
    p = writeU16(out, p, time);
    p = writeU16(out, p, date);
    p = writeU32(out, p, m.crc);
    p = writeU32(out, p, m.size);
    p = writeU32(out, p, m.size);
    p = writeU16(out, p, m.nameBytes.length);
    p = writeU16(out, p, 0); // extra len
    p = writeU16(out, p, 0); // comment len
    p = writeU16(out, p, 0); // disk num
    p = writeU16(out, p, 0); // internal attrs
    p = writeU32(out, p, 0); // external attrs
    p = writeU32(out, p, m.localOffset);

    out.set(m.nameBytes, p);
    p += m.nameBytes.length;
  }

  // 3. Write End of Central Directory (EOCD)
  p = writeU32(out, p, 0x06054b50); // EOCD sig
  p = writeU16(out, p, 0); // disk num
  p = writeU16(out, p, 0); // central dir disk
  p = writeU16(out, p, entries.length); // entries on disk
  p = writeU16(out, p, entries.length); // total entries
  p = writeU32(out, p, totalCentralSize);
  p = writeU32(out, p, centralStartOffset);
  p = writeU16(out, p, 0); // comment len

  return out;
}
