// ppm.test.mjs — pure tests for the shared bounded P6 PPM loader (issue #174).
import assert from "node:assert/strict";
import test from "node:test";
import {
  parseP6Ppm,
  PpmFormatError,
  PPM_MAX_DIMENSION,
  PPM_MAX_PIXEL_BYTES,
} from "./ppm.mjs";

function ppm(width, height, pixelBytes, opts = {}) {
  const header = opts.header ?? `P6\n${width} ${height}\n255\n`;
  const payload = pixelBytes ?? width * height * 3;
  const body = opts.pixels ?? new Uint8Array(payload);
  return Buffer.concat([Buffer.from(header, "ascii"), Buffer.from(body)]);
}

function canonical(w, h) {
  return ppm(w, h);
}

test("parses a canonical PSP-sized P6 PPM", () => {
  const w = 480;
  const h = 272;
  const buf = canonical(w, h);
  const parsed = parseP6Ppm(buf);
  assert.ok(parsed);
  assert.equal(parsed.width, w);
  assert.equal(parsed.height, h);
  assert.equal(parsed.channels, 3);
  assert.equal(parsed.data.length, w * h * 3);
});

test("returns null for non-P6 input (PNG magic, empty, short)", () => {
  assert.equal(parseP6Ppm(new Uint8Array(0)), null);
  assert.equal(parseP6Ppm(new Uint8Array([0x89, 0x50, 0x4e])), null);
  assert.equal(parseP6Ppm(new Uint8Array([0x50, 0x35])), null); // 'P5' gray
  assert.equal(parseP6Ppm(Buffer.from("not a ppm")), null);
});

test("reports P6-magic truncation as malformed, not as non-PPM", () => {
  // A buffer starting with "P6" but with no complete header is a malformed
  // PPM (deterministic 400 via PpmFormatError), never silently handed to the
  // sharp fallback as if it were a PNG/JPEG.
  assert.throws(() => parseP6Ppm(Buffer.from("P6")), (e) => e.code === "bad-header");
  assert.throws(() => parseP6Ppm(Buffer.from("P6\n8 8")), (e) => e.code === "bad-header");
  assert.throws(() => parseP6Ppm(Buffer.from("P6\n8 8\n255\n")), (e) => e.code === "truncated-payload");
});

test("rejects parseInt-style prefix acceptance (0x10junk)", () => {
  const header = "P6\n0x10junk 8\n255\n";
  const buf = Buffer.concat([Buffer.from(header, "ascii"), new Uint8Array(0x10 * 8 * 3)]);
  assert.throws(() => parseP6Ppm(buf), (e) => {
    assert.ok(e instanceof PpmFormatError);
    assert.equal(e.code, "bad-header");
    return true;
  });
});

test("rejects decimal-with-suffix width (123junk)", () => {
  const buf = Buffer.concat([
    Buffer.from("P6\n123junk 8\n255\n", "ascii"),
    new Uint8Array(123 * 8 * 3),
  ]);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "bad-header");
});

test("rejects negative dimensions", () => {
  const buf = Buffer.concat([
    Buffer.from("P6\n-5 8\n255\n", "ascii"),
    new Uint8Array(40),
  ]);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "bad-header");
});

test("rejects missing/NaN-equivalent dimensions", () => {
  const buf = Buffer.concat([
    Buffer.from("P6\n 8\n255\n", "ascii"),
    new Uint8Array(24),
  ]);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "bad-header");
});

test("rejects zero dimensions", () => {
  assert.throws(() => parseP6Ppm(ppm(0, 8)), (e) => e.code === "bad-dimensions");
  assert.throws(() => parseP6Ppm(ppm(8, 0)), (e) => e.code === "bad-dimensions");
});

test("rejects dimensions above the explicit limit", () => {
  const w = PPM_MAX_DIMENSION + 1;
  const buf = ppm(w, 2);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "dimension-limit");
});

test("rejects unsafe-integer (overflowing) dimensions", () => {
  // 2^60 exceeds Number.MAX_SAFE_INTEGER; 15+ digit tokens are rejected.
  const buf = Buffer.concat([
    Buffer.from(`P6\n${2 ** 60} 8\n255\n`, "ascii"),
    new Uint8Array(0),
  ]);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "bad-header");
});

test("rejects huge dimensions whose product exceeds the pixel budget", () => {
  const w = 4096;
  const h = 4096; // 4096*4096*3 = 48 MiB > 16 MiB budget
  const buf = ppm(w, h);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "pixel-budget");
});

test("rejects maxval != 255", () => {
  const buf = Buffer.concat([
    Buffer.from("P6\n8 8\n254\n", "ascii"),
    new Uint8Array(8 * 8 * 3),
  ]);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "unsupported-maxval");
});

test("rejects truncated payload (one byte short)", () => {
  const w = 8;
  const h = 8;
  const buf = ppm(w, h, w * h * 3 - 1);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "truncated-payload");
});

test("rejects empty payload when pixels are required", () => {
  const buf = Buffer.concat([
    Buffer.from("P6\n8 8\n255\n", "ascii"),
    new Uint8Array(0),
  ]);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "truncated-payload");
});

test("rejects extra trailing data after the exact payload", () => {
  const w = 4;
  const h = 4;
  const buf = ppm(w, h, w * h * 3 + 7);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "trailing-data");
});

test("preserves a raster byte that equals the separator byte", () => {
  // Canonical writer emits exactly one newline after maxval; a payload that
  // starts with a whitespace byte must not be swallowed by header parsing.
  const header = "P6\n1 1\n255\n";
  const pixels = new Uint8Array([0x0a, 0x00, 0x00]); // first byte is LF
  const buf = Buffer.concat([Buffer.from(header, "ascii"), Buffer.from(pixels)]);
  const parsed = parseP6Ppm(buf);
  assert.ok(parsed);
  assert.equal(parsed.data[0], 0x0a); // LF must be preserved as a pixel byte
});

test("accepts legal header comments and whitespace variants before maxval", () => {
  // Comments are equivalent to whitespace in the header and may appear
  // between tokens before maxval.
  const header = "P6\n# a comment\n2 2\t# another\n255\n";
  const buf = Buffer.concat([Buffer.from(header, "ascii"), new Uint8Array(12)]);
  const parsed = parseP6Ppm(buf);
  assert.ok(parsed);
  assert.equal(parsed.width, 2);
  assert.equal(parsed.height, 2);
});

test("rejects a comment after maxval (not in the canonical subset)", () => {
  // The raster may legitimately begin with any byte, so a comment after
  // maxval would make the raster boundary ambiguous; it is outside the
  // documented canonical subset and rejected deterministically.
  const header = "P6\n2 2\n255# trailing\n";
  const buf = Buffer.concat([Buffer.from(header, "ascii"), new Uint8Array(12)]);
  assert.throws(() => parseP6Ppm(buf), (e) => e.code === "bad-header");
});

test("preserves exact pixel bytes in order", () => {
  const w = 2;
  const h = 2;
  const pixels = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
  const buf = Buffer.concat([
    Buffer.from(`P6\n${w} ${h}\n255\n`, "ascii"),
    Buffer.from(pixels),
  ]);
  const parsed = parseP6Ppm(buf);
  assert.ok(parsed);
  assert.deepEqual(Array.from(parsed.data), Array.from(pixels));
});

test("accepts a one-pixel image at the exact legal boundary", () => {
  const parsed = parseP6Ppm(ppm(1, 1));
  assert.ok(parsed);
  assert.equal(parsed.data.length, 3);
});

test("accepts dimensions at the exact limit boundary", () => {
  const buf = ppm(PPM_MAX_DIMENSION, 1);
  const parsed = parseP6Ppm(buf);
  assert.ok(parsed);
  assert.equal(parsed.width, PPM_MAX_DIMENSION);
});

test("rejects width exactly at the first illegal boundary", () => {
  assert.throws(
    () => parseP6Ppm(ppm(PPM_MAX_DIMENSION + 1, 1)),
    (e) => e.code === "dimension-limit",
  );
});

test("PPM_MAX_PIXEL_BYTES is the enforced ceiling", () => {
  // Largest legal product at the dimension limit: 4096x4096 overflows the
  // budget; 2048x2048*3 = 12 MiB is legal.
  assert.equal(PPM_MAX_PIXEL_BYTES, 16 * 1024 * 1024);
  assert.ok(parseP6Ppm(ppm(2048, 2048)));
  assert.throws(() => parseP6Ppm(ppm(4096, 4096)), (e) => e.code === "pixel-budget");
});
