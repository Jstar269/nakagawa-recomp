/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * texrepl.h - Texture replacement packs in PPSSPP's layout (#1880), phase 1: the host half.
 *
 * A pack is a local folder the user supplies (the project ships none and never downloads
 * one): an optional textures.ini and PNG files, each named for the texture it replaces. This
 * module reads a pack and answers "what replaces this guest texture?". It never touches the GPU,
 * guest memory or the network: the renderer (ge_gpu.c) hashes a texture it has just decoded,
 * asks here, and uploads whatever comes back through its scaled-copy path (tex_scale.h).
 *
 * What is read, from PPSSPP's documented layout (design reading only; the code is this project's):
 *   [options]          hash = quick | xxh32 | xxh64 (required when the ini exists), ignoreAddress,
 *                      reduceHash, skipLastDXT1Blocks128x64, skipLastDXT1Blocks128x128, version
 *   [hashes]           <24 hex digits>[_<level>] = <file under the pack folder>; an empty file marks a
 *                      texture to leave alone
 *   [hashranges]       addr,w,h = newW,newH   (the area a texture's hash covers)
 *   [reducehashranges] w,h = fraction          (with reduceHash)
 *   and hash-named PNG files in the folder's root, which need no ini line.
 * What is not read is reported, never silently ignored: [filtering], [indexed], [games],
 * saveIndexed, ignoreMipmap, mip-level files (name_1.png), DDS/KTX2/ZIM files, and any key or
 * section this module does not know each add a line to nk_texrepl_diag.
 *
 * A malformed ini (a line that is no comment, section or key = value, an unfinished section header,
 * no hash type, an unknown hash type) makes nk_texrepl_open fail with a message, and nothing
 * loads: a half-read pack would replace some textures and not others in a way nobody asked for.
 */
#ifndef NK_TEXREPL_H
#define NK_TEXREPL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_TEXREPL_DEFAULT_BUDGET_MB 256u
/* A replacement image is at most this many texels on a side. */
#define NK_TEXREPL_MAX_DIM 2048
/* The diagnostics kept; the rest are counted in the last line. */
#define NK_TEXREPL_MAX_DIAG 40

typedef enum {
    NK_TEXREPL_HASH_QUICK = 0,
    NK_TEXREPL_HASH_XXH32,
    NK_TEXREPL_HASH_XXH64
} NkTexReplHash;

/* ---- hashes ----------------------------------------------------------------------------- */
/* PPSSPP's quick texture hash over `size` bytes. `guest_addr` is the texture's guest address:
 * its 16-byte alignment picks the vectorised variant, as the host pointer's does there. */
uint32_t nk_texrepl_quick_hash(const void *data, uint32_t size, uint32_t guest_addr);
uint32_t nk_texrepl_xxh32(const void *data, size_t len, uint32_t seed);
uint64_t nk_texrepl_xxh64(const void *data, size_t len, uint64_t seed);
/* The palette's hash in a texture's identity (XXH32 with the seed packs use). */
uint32_t nk_texrepl_clut_hash(const void *clut, uint32_t bytes);
/* The palette part of a palette texture's identity, as PPSSPP's TextureCacheCommon computes it
 * (UpdateCurrentClut, then "cluthash = clutHash_ ^ gstate.clutformat"): XXH32 of the loaded palette
 * bytes, then xor the CLUTFORMAT command word with its opcode (0xC5) in the top byte. The bytes
 * hashed are min(last_load_bytes + start_bytes, max_load_bytes), where `last_load_bytes` is the size
 * of the last CLUT load, `max_load_bytes` the largest load seen, and `start_bytes` the format's index
 * start position (in 16-colour units) times the entry size. A game that has loaded no palette yet
 * (`max_load_bytes` 0, as in a replayed capture) hashes the 16 entries a CLUT4 texture can index,
 * else 256, instead. `texture_format` is the PSP texture format word (4..7 are the palette ones). */
uint32_t nk_texrepl_clut_identity(const void *clutram, uint32_t clutram_bytes, uint32_t clut_format,
                                  uint32_t texture_format, uint32_t last_load_bytes,
                                  uint32_t max_load_bytes);
/* The 64-bit cache key of a texture: address (30 bits) and size code, xor the palette hash for a
 * palette format. `dim` is the PSP's size register (log2 width, log2 height << 8). */
uint64_t nk_texrepl_cache_key(uint32_t addr, uint32_t format, uint32_t dim, uint32_t clut_hash);
/* Bits per texel of a PSP texture format (0 for an invalid one). */
uint32_t nk_texrepl_bits_per_texel(uint32_t format);

/* ---- pack ------------------------------------------------------------------------------- */
typedef struct NkTexRepl NkTexRepl;

/* Read the pack in `dir`. `budget_bytes` bounds the decoded images kept in memory (0 takes
 * NK_TEXREPL_DEFAULT_BUDGET_MB). Returns NULL and fills `err` when the folder cannot be read or its
 * textures.ini is malformed; nothing is loaded then. */
NkTexRepl *nk_texrepl_open(const char *dir, uint64_t budget_bytes, char *err, size_t err_cap);
void nk_texrepl_close(NkTexRepl *pack);

NkTexReplHash nk_texrepl_hash_kind(const NkTexRepl *pack);
/* How many textures the pack lists (replacements and "leave alone" lines alike). */
size_t nk_texrepl_entry_count(const NkTexRepl *pack);
/* What was read but not applied, one line each; the last line says how many more were dropped. */
size_t nk_texrepl_diag_count(const NkTexRepl *pack);
const char *nk_texrepl_diag(const NkTexRepl *pack, size_t index);
/* Take the oldest note not yet taken, so a caller can print notes as they appear (a file that
 * fails to load shows up at its first use, long after the pack opened). Copies it to `out` and
 * returns 1, or returns 0 when there are none. Taken notes leave the list and free its room, so
 * the 40-line cap bounds the notes waiting, not the notes a run can report. When notes were dropped
 * for lack of room, the last one taken says how many. */
int nk_texrepl_diag_take(NkTexRepl *pack, char *out, size_t cap);

/* The content hash PPSSPP's replacer takes of a guest texture, with this pack's hash kind and
 * ranges. `data` points at the texture's first byte and `avail` is how many bytes are readable
 * from it (guest memory up to its end); a texture that reaches past it hashes to 0, like there.
 * `bufw` is the row stride in texels, `max_seen_v` the tallest row the game has drawn (0 when not
 * tracked: a 512-high texture is then hashed whole). */
uint32_t nk_texrepl_hash_texture(const NkTexRepl *pack, const uint8_t *data, size_t avail,
                                 uint32_t guest_addr, int bufw, int w, int h, uint32_t format,
                                 int max_seen_v);

typedef struct NkTexReplId {
    uint32_t addr;       /* guest address of level 0 */
    uint32_t format;     /* PSP texture format 0..10 */
    uint32_t dim;        /* the PSP's size register: log2 width | log2 height << 8 */
    uint32_t clut_hash;  /* nk_texrepl_clut_hash of the palette; ignored for a non-palette format */
    uint32_t hash;       /* nk_texrepl_hash_texture */
} NkTexReplId;

typedef struct NkTexReplHit {
    const uint32_t *rgba;  /* R in the low byte, as ge.c's decoder writes; owned by the pack */
    int w, h;
    void *pin;             /* internal; give the hit back with nk_texrepl_unpin */
} NkTexReplHit;

/* The replacement for a texture, or none. Returns 1 and fills `out` (the image stays in memory
 * until nk_texrepl_unpin), or 0: no entry, a "leave alone" entry, an unsupported or unreadable file,
 * or an image the budget cannot hold. The first lookup of an entry decodes its PNG on the calling
 * thread. */
int nk_texrepl_lookup(NkTexRepl *pack, const NkTexReplId *id, NkTexReplHit *out);
void nk_texrepl_unpin(NkTexRepl *pack, NkTexReplHit *hit);
/* The whole-number factor a hit is larger than a w x h texture by, the same on both axes, or 0
 * when it is not one (the renderer draws such a texture unreplaced and counts it). */
int nk_texrepl_factor(const NkTexReplHit *hit, int w, int h);

typedef struct NkTexReplStats {
    uint64_t lookups, hits, misses;
    uint64_t left_alone;      /* entries that mark a texture to keep */
    uint64_t loaded;          /* images decoded */
    uint64_t load_failed;     /* files missing, unsupported or not decodable */
    uint64_t over_budget;     /* images the budget could not hold */
    uint64_t evicted;
    uint64_t bad_factor;      /* counted by the renderer through nk_texrepl_note_bad_factor */
    uint64_t cache_bytes, cache_peak;
    uint64_t decode_ns;
} NkTexReplStats;
void nk_texrepl_stats(const NkTexRepl *pack, NkTexReplStats *out);
void nk_texrepl_note_bad_factor(NkTexRepl *pack);

#ifdef __cplusplus
}
#endif

#endif
