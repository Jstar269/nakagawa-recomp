// ppm.mjs — shared bounded P6 PPM loader for the visual-regression surface
// (issue #174).
//
// The dashboard previously kept two independent minimal P6 parsers (image and
// variance routes) that trusted `parseInt()` dimensions and fed them straight
// into `Buffer.alloc(w*h*4)` / O(w*h) loops.  This module is the single
// authoritative loader used by both routes:
//
//   * width/height are strict decimal tokens (full-string; no parseInt prefix
//     acceptance, no '+', '-', '0x', '.', or scientific notation);
//   * dimensions must be finite positive safe integers within
//     PPM_MAX_DIMENSION and the decoded payload must fit PPM_MAX_PIXEL_BYTES
//     (checked with safe-integer arithmetic before any allocation);
//   * maxval must be 255 (the project's canonical P6 subset);
//   * the binary raster must follow the header after exactly one whitespace
//     separator byte, and must be EXACTLY width*height*3 bytes — truncation
//     and trailing data are both rejected deterministically (a truncated
//     "same-prefix" capture can never be exposed as valid pixels);
//   * legal header comments ('#' to end of line) between tokens before
//     maxval are accepted.
//
// The accepted canonical subset is deliberately documented: P6, optional
// header comments BEFORE maxval, whitespace-separated width/height/maxval=255,
// exactly one whitespace separator after maxval, exact payload.  Comments
// after maxval are not part of the subset (the raster may legitimately begin
// with any byte, so a post-maxval comment would make the raster boundary
// ambiguous).  Anything else is a PpmFormatError.
//
// Returns null when the buffer is not a P6 file (callers fall back to sharp
// for PNG/JPEG); throws PpmFormatError (with a stable `code`) for malformed
// P6 so callers can map the failure to a deterministic HTTP status.

export const PPM_MAX_DIMENSION = 4096;
export const PPM_MAX_PIXEL_BYTES = 16 * 1024 * 1024; // 16 MiB RGB payload budget

export class PpmFormatError extends Error {
  constructor(code, message) {
    super(message);
    this.name = "PpmFormatError";
    this.code = code;
  }
}

const WS = new Set([0x20, 0x09, 0x0a, 0x0d]); // space, tab, LF, CR
const MAX_TOKEN_DIGITS = 15; // 10^15-1 < 2^53, keeps Number() exact

function isWs(b) {
  return WS.has(b);
}

// Skip whitespace and '#' comments starting at i; returns the first index
// that is neither whitespace nor inside a comment.
function skipWsAndComments(buf, i) {
  while (i < buf.length) {
    const b = buf[i];
    if (b === 0x23) {
      // '#' — consume to end of line (inclusive), then continue scanning.
      while (i < buf.length && buf[i] !== 0x0a) i += 1;
      continue;
    }
    if (isWs(b)) {
      i += 1;
      continue;
    }
    break;
  }
  return i;
}

// Read a strict decimal token (digits only) after skipping whitespace and
// comments.  Returns { value, next } or null when the next non-ws byte is
// not a digit, when the token is longer than MAX_TOKEN_DIGITS, or when the
// parsed value is not a safe integer.
function readDecimalToken(buf, i) {
  i = skipWsAndComments(buf, i);
  const start = i;
  while (i < buf.length && buf[i] >= 0x30 && buf[i] <= 0x39) i += 1;
  if (i === start) return null;
  if (i - start > MAX_TOKEN_DIGITS) return null;
  const value = Number(buf.subarray(start, i).toString("ascii"));
  if (!Number.isSafeInteger(value)) return null;
  return { value, next: i };
}

/**
 * Parse a P6 PPM buffer with the documented canonical subset.
 *
 * @param {Uint8Array} buf
 * @returns {{ width: number, height: number, channels: 3, data: Uint8Array } | null}
 *   null when buf is not a P6 file.
 * @throws {PpmFormatError}
 */
export function parseP6Ppm(buf) {
  // A buffer too short to even carry the two-byte magic cannot be a PPM; a
  // buffer that starts with the P6 magic but is otherwise truncated must be
  // reported as malformed (deterministic 400), never silently handed to the
  // sharp fallback.
  if (!(buf instanceof Uint8Array) || buf.length < 2) return null;
  if (buf[0] !== 0x50 /* 'P' */ || buf[1] !== 0x36 /* '6' */) return null;

  const widthTok = readDecimalToken(buf, 2);
  if (!widthTok) throw new PpmFormatError("bad-header", "missing or invalid width");
  const heightTok = readDecimalToken(buf, widthTok.next);
  if (!heightTok) throw new PpmFormatError("bad-header", "missing or invalid height");
  const maxvalTok = readDecimalToken(buf, heightTok.next);
  if (!maxvalTok) throw new PpmFormatError("bad-header", "missing or invalid maxval");

  const width = widthTok.value;
  const height = heightTok.value;
  if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height)) {
    throw new PpmFormatError("bad-dimensions", "width/height are not safe integers");
  }
  if (width <= 0 || height <= 0) {
    throw new PpmFormatError("bad-dimensions", "width/height must be positive");
  }
  if (width > PPM_MAX_DIMENSION || height > PPM_MAX_DIMENSION) {
    throw new PpmFormatError(
      "dimension-limit",
      `dimensions ${width}x${height} exceed PPM_MAX_DIMENSION=${PPM_MAX_DIMENSION}`,
    );
  }
  if (maxvalTok.value !== 255) {
    throw new PpmFormatError(
      "unsupported-maxval",
      `maxval ${maxvalTok.value} (canonical subset requires 255)`,
    );
  }

  const pixelBytes = width * height * 3;
  if (!Number.isSafeInteger(pixelBytes) || pixelBytes > PPM_MAX_PIXEL_BYTES) {
    throw new PpmFormatError(
      "pixel-budget",
      `decoded payload ${pixelBytes} bytes exceeds PPM_MAX_PIXEL_BYTES=${PPM_MAX_PIXEL_BYTES}`,
    );
  }

  // The header must end with exactly one whitespace separator after the
  // maxval token, then the binary raster begins.  Comments are accepted in
  // the header before maxval (handled by the token reader); a comment after
  // maxval is NOT part of the accepted subset.  Consuming exactly one
  // separator byte is required: the binary raster may legitimately begin with
  // a byte that happens to be whitespace, so we must not skip a whole run.
  const sep = maxvalTok.next;
  if (sep >= buf.length || !isWs(buf[sep])) {
    throw new PpmFormatError("bad-header", "expected single whitespace after maxval");
  }
  const rasterStart = sep + 1; // consume exactly the one separator byte

  const available = buf.length - rasterStart;
  if (available < pixelBytes) {
    throw new PpmFormatError(
      "truncated-payload",
      `payload ${available} bytes, expected exactly ${pixelBytes}`,
    );
  }
  if (available > pixelBytes) {
    throw new PpmFormatError(
      "trailing-data",
      `payload ${available} bytes, expected exactly ${pixelBytes} (trailing data rejected)`,
    );
  }

  return {
    width,
    height,
    channels: 3,
    data: buf.subarray(rasterStart, rasterStart + pixelBytes),
  };
}
