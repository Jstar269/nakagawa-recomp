export const PPM_MAX_DIMENSION: number;
export const PPM_MAX_PIXEL_BYTES: number;

export class PpmFormatError extends Error {
  code:
    | "bad-header"
    | "bad-dimensions"
    | "dimension-limit"
    | "unsupported-maxval"
    | "pixel-budget"
    | "truncated-payload"
    | "trailing-data";
}

export interface ParsedP6Ppm {
  width: number;
  height: number;
  channels: 3;
  data: Uint8Array;
}

/**
 * Parse a P6 PPM buffer (documented canonical subset: P6, optional header
 * comments, whitespace-separated width/height/maxval=255, exactly one
 * whitespace separator, exact width*height*3 payload).
 *
 * Returns null when `buf` is not a P6 file; throws PpmFormatError for
 * malformed P6 (bad dimensions, dimension limit, unsupported maxval, byte
 * budget, truncation, or trailing data).
 */
export function parseP6Ppm(buf: Uint8Array): ParsedP6Ppm | null;
