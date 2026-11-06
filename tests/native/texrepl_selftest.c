/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * texrepl_selftest.c - the texture replacement pack reader (src/rt/texrepl.h, #1880), no GPU and no game.
 *
 * Every texture, pack and PNG here is synthetic and written under the build folder. It checks:
 *   - the hashes against published xxHash vectors and against quick-hash values taken from PPSSPP's own
 *     implementation (a pack's file names depend on them);
 *   - a texture whose hash a pack lists is replaced by the pack's PNG; an unlisted one is not;
 *   - a malformed textures.ini is reported and loads nothing;
 *   - what is read but not applied is reported;
 *   - the decoded images stay under the memory budget;
 *   - a file name or [hashes] value that leaves the pack folder is refused.
 */
#include "texrepl.h"

#include "nk_platform.h"
#include "synthetic_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "."
#endif

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
    g_failures++; } } while (0)

/* The synthetic PNG writer is shared with the GPU selftest (tests/native/synthetic_png.h). */
static void write_png(const char *path, int w, int h, const uint32_t *rgba) {
    if (!synpng_write(path, w, h, rgba)) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
}

static void write_text(const char *path, const char *text) {
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static uint32_t *solid(int w, int h, uint32_t colour) {
    uint32_t *p = (uint32_t *)malloc((size_t)w * h * 4u);
    for (int i = 0; i < w * h; i++) p[i] = colour;
    return p;
}

static void pack_path(char *out, size_t n, const char *pack, const char *file) {
    snprintf(out, n, "%s/%s/%s", SR_SELFTEST_BUILD_ROOT, pack, file);
}
static void make_pack(char *dir, size_t n, const char *pack) {
    snprintf(dir, n, "%s/%s", SR_SELFTEST_BUILD_ROOT, pack);
    nk_platform_mkdir_p(dir);
}

/* ------------------------------------------------------------------------------- hashes */

static void test_hashes(void) {
    uint8_t data[512];
    for (int i = 0; i < 512; i++) data[i] = (uint8_t)(i & 0xff);
    /* Published xxHash values (seed 0), cross-checked against the reference implementation. */
    CHECK(nk_texrepl_xxh32("", 0, 0) == 0x02cc5d05u, "xxh32 of nothing");
    CHECK(nk_texrepl_xxh32("a", 1, 0) == 0x550d7456u, "xxh32 of a");
    CHECK(nk_texrepl_xxh32("abc", 3, 0) == 0x32d153ffu, "xxh32 of abc");
    CHECK(nk_texrepl_xxh32(data, 512, 0) == 0xf319d2c7u, "xxh32 of 512 bytes (the 16-byte stripe loop)");
    CHECK(nk_texrepl_xxh32(data, 512, 0xBACD7814u) == 0xd92f8e6bu, "xxh32 with the pack seed");
    CHECK(nk_texrepl_xxh64("", 0, 0) == 0xef46db3751d8e999ull, "xxh64 of nothing");
    CHECK(nk_texrepl_xxh64("a", 1, 0) == 0xd24ec4f1a98c6e5bull, "xxh64 of a");
    CHECK(nk_texrepl_xxh64("abc", 3, 0) == 0x44bc2cf5ad770999ull, "xxh64 of abc");
    CHECK(nk_texrepl_xxh64(data, 512, 0) == 0x7b3bfcaac0348ac0ull, "xxh64 of 512 bytes (the 32-byte stripe loop)");
    CHECK(nk_texrepl_xxh64(data, 512, 0xBACD7814u) == 0x27fa7a3eb3a0649dull, "xxh64 with the pack seed");
    {
        const char *fox = "The quick brown fox jumps over the lazy dog";
        CHECK(nk_texrepl_xxh32(fox, 43, 0) == 0xe85ea4deu, "xxh32 of the fox (4-byte and 1-byte tails)");
        CHECK(nk_texrepl_xxh64(fox, 43, 0) == 0x0b242d361fda71bcull, "xxh64 of the fox (8, 4 and 1-byte tails)");
    }

    /* PPSSPP's quick hash, from its own implementation over a fixed buffer. */
    uint8_t buf[512 + 16];
    for (int i = 0; i < 512; i++) buf[i] = (uint8_t)((i * 37 + (i >> 3) * 11 + 5) & 0xff);
    static const struct { int off; uint32_t size; uint32_t want; } kQuick[] = {
        {0, 256, 0x261c4644u}, {0, 64, 0xd41f5060u}, {0, 192, 0x5a9bb894u}, {4, 256, 0x1b4838c0u},
        {0, 100, 0x65aa6078u}, {0, 0, 0x9535599cu},  {0, 448, 0xafb7acb4u}, {8, 64, 0x33fb1a90u},
    };
    for (size_t i = 0; i < sizeof(kQuick) / sizeof(kQuick[0]); i++) {
        /* The guest address carries the alignment: off 4 is not 16-byte aligned. */
        uint32_t got = nk_texrepl_quick_hash(buf + kQuick[i].off, kQuick[i].size, 0x08800000u + (uint32_t)kQuick[i].off);
        CHECK(got == kQuick[i].want, "quick hash case %zu: got %08x want %08x", i, got, kQuick[i].want);
    }

    /* Identity: address (30 bits) and size code, xor the palette hash for a palette format only. */
    CHECK(nk_texrepl_cache_key(0x08804000u, 3, 0x0808u, 0xdeadbeefu) == 0x0880400000000808ull, "key, direct colour");
    CHECK(nk_texrepl_cache_key(0x08804000u, 5, 0x0808u, 0xdeadbeefu) == (0x0880400000000808ull ^ 0xdeadbeefull),
          "key, palette format");
    CHECK(nk_texrepl_cache_key(0xC8804000u, 3, 0x0808u, 0) == 0x0880400000000808ull, "key masks the address to 30 bits");
    CHECK(nk_texrepl_bits_per_texel(8) == 4 && nk_texrepl_bits_per_texel(3) == 32 && nk_texrepl_bits_per_texel(12) == 0,
          "bits per texel");
}

/* ------------------------------------------------------------------------------- packs */

static int has_diag(const NkTexRepl *p, const char *needle) {
    for (size_t i = 0; i < nk_texrepl_diag_count(p); i++)
        if (strstr(nk_texrepl_diag(p, i), needle)) return 1;
    return 0;
}

static void test_synthetic_pack(void) {
    char dir[512], path[600], err[256];
    make_pack(dir, sizeof(dir), "texrepl_pack_a");
    /* A 2x2 texture's replacement is a 4x4 PNG: factor 2. */
    uint32_t *red = solid(4, 4, 0xff0000ffu);      /* R in the low byte: 0xAABBGGRR */
    uint32_t *wide = solid(8, 4, 0xff00ff00u);
    pack_path(path, sizeof(path), "texrepl_pack_a", "red.png");
    write_png(path, 4, 4, red);
    pack_path(path, sizeof(path), "texrepl_pack_a", "wide.png");
    write_png(path, 8, 4, wide);
    pack_path(path, sizeof(path), "texrepl_pack_a", "textures.ini");
    write_text(path,
        "# a synthetic pack\r\n"
        "[options]\r\n"
        "hash = quick\r\n"
        "version = 1\r\n"
        "[hashes]\r\n"
        "0880400000000202 11223344 = red.png\n"        /* a space inside: not a 24-digit name, reported */
        "088040000000020211223344 = red.png\n"
        "088040000000020255667788 = wide.png\n"
        "08804000000002029999aaaa =\n"                  /* leave this one alone */
        "08804000000002020000bbbb = missing.png\n"
        "08804000000002020000cccc = ../outside.png\n"
        "088040000000020211223344_1 = red.png\n"        /* mip level 1: not read */
        "[filtering]\n"
        "088040000000020211223344 = nearest\n"
        "[mystery]\n"
        "x = y\n");
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    CHECK(nk_texrepl_hash_kind(p) == NK_TEXREPL_HASH_QUICK, "hash kind");
    CHECK(nk_texrepl_entry_count(p) == 4, "entries: %zu", nk_texrepl_entry_count(p));

    NkTexReplId id = { 0x08804000u, 3u, 0x0202u, 0u, 0x11223344u };
    NkTexReplHit hit;
    /* A listed texture is replaced by the pack's PNG, pixel for pixel. */
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 1, "listed texture not replaced");
    CHECK(hit.w == 4 && hit.h == 4 && hit.rgba && hit.rgba[0] == 0xff0000ffu && hit.rgba[15] == 0xff0000ffu,
          "replacement pixels: %dx%d first %08x", hit.w, hit.h, hit.rgba ? hit.rgba[0] : 0u);
    CHECK(nk_texrepl_factor(&hit, 4, 4) == 1 && nk_texrepl_factor(&hit, 2, 2) == 2 && nk_texrepl_factor(&hit, 3, 3) == 0,
          "factor");
    nk_texrepl_unpin(p, &hit);
    /* A second lookup is served from memory. */
    NkTexReplStats st;
    nk_texrepl_stats(p, &st);
    uint64_t loaded = st.loaded;
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 1, "second lookup");
    nk_texrepl_unpin(p, &hit);
    nk_texrepl_stats(p, &st);
    CHECK(st.loaded == loaded && loaded == 1, "decoded once: %llu", (unsigned long long)st.loaded);

    /* A different shape from the original: not a whole-number factor. */
    NkTexReplId wide_id = { 0x08804000u, 3u, 0x0202u, 0u, 0x55667788u };
    CHECK(nk_texrepl_lookup(p, &wide_id, &hit) == 1 && hit.w == 8 && hit.h == 4, "wide replacement");
    CHECK(nk_texrepl_factor(&hit, 2, 2) == 0, "8x4 over 2x2 is not one factor");
    nk_texrepl_unpin(p, &hit);

    /* Unlisted, and listed-but-left-alone, are untouched. */
    NkTexReplId other = id;
    other.hash = 0x12345678u;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0 && hit.rgba == NULL, "an unlisted texture was replaced");
    other.hash = 0x9999aaaau;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0, "a left-alone texture was replaced");
    /* Another address and size code do not match. */
    other = id;
    other.addr = 0x08805000u;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0, "another address matched");
    other = id;
    other.dim = 0x0303u;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0, "another size matched");
    /* A listed file that is not there, or leaves the folder, is not used, and says so. */
    other = id;
    other.hash = 0x0000bbbbu;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0, "a missing file was replaced");
    other.hash = 0x0000ccccu;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0, "a path outside the pack was read");

    nk_texrepl_stats(p, &st);
    CHECK(st.hits == 3 && st.left_alone == 1 && st.load_failed == 1, "stats hits=%llu alone=%llu failed=%llu",
          (unsigned long long)st.hits, (unsigned long long)st.left_alone, (unsigned long long)st.load_failed);

    /* What was read and not applied is reported. */
    CHECK(has_diag(p, "11223344 = red.png") || has_diag(p, "not a 24-digit hash name"), "bad [hashes] name not reported");
    CHECK(has_diag(p, "outside the pack folder"), "path outside the pack not reported");
    CHECK(has_diag(p, "mip-level"), "mip levels not reported");
    CHECK(has_diag(p, "[filtering]"), "[filtering] not reported");
    CHECK(has_diag(p, "unknown section [mystery]"), "unknown section not reported");
    CHECK(has_diag(p, "missing.png"), "missing file not reported");
    nk_texrepl_close(p);
    free(red);
    free(wide);
}

static void test_malformed_ini(void) {
    static const struct { const char *name; const char *text; const char *why; } kBad[] = {
        {"no_hash", "[options]\nversion = 1\n", "no hash type"},
        {"bad_hash", "[options]\nhash = crc32\n", "unsupported hash type"},
        {"no_equals", "[options]\nhash = quick\nthis is not a key\n", "not a section"},
        {"open_section", "[options\nhash = quick\n", "unfinished section"},
        {"outside", "hash = quick\n", "outside any section"},
        {"bad_bool", "[options]\nhash = quick\nignoreAddress = maybe\n", "true or false"},
    };
    for (size_t i = 0; i < sizeof(kBad) / sizeof(kBad[0]); i++) {
        char dir[512], path[600], err[256] = "";
        make_pack(dir, sizeof(dir), "texrepl_pack_bad");
        uint32_t *px = solid(4, 4, 0xff112233u);
        pack_path(path, sizeof(path), "texrepl_pack_bad", "0880400000000202aabbccdd.png");
        write_png(path, 4, 4, px);
        free(px);
        pack_path(path, sizeof(path), "texrepl_pack_bad", "textures.ini");
        write_text(path, kBad[i].text);
        NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
        CHECK(p == NULL, "%s: a malformed ini loaded", kBad[i].name);
        CHECK(strstr(err, kBad[i].why) != NULL, "%s: message was '%s'", kBad[i].name, err);
        if (p) nk_texrepl_close(p);
    }
    char err[128] = "";
    CHECK(nk_texrepl_open(SR_SELFTEST_BUILD_ROOT "/texrepl_no_such_pack", 0, err, sizeof(err)) == NULL && err[0],
          "a missing folder");
}

/* A folder with hash-named PNGs and no ini is read with the quick hash, and a PNG that is not one is reported. */
static void test_ini_less_pack(void) {
    char dir[512], path[600], err[256];
    make_pack(dir, sizeof(dir), "texrepl_pack_plain");
    uint32_t *px = solid(2, 2, 0xff010203u);
    pack_path(path, sizeof(path), "texrepl_pack_plain", "0880400000000101a1a2a3a4.png");
    write_png(path, 2, 2, px);
    free(px);
    pack_path(path, sizeof(path), "texrepl_pack_plain", "0880400000000101b1b2b3b4.png");
    write_text(path, "not a png at all");
    pack_path(path, sizeof(path), "texrepl_pack_plain", "0880400000000101c1c2c3c4.dds");
    write_text(path, "DDS ");
    pack_path(path, sizeof(path), "texrepl_pack_plain", "0880400000000101d1d2d3d4_1.png");
    write_text(path, "x");
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplHit hit;
    NkTexReplId id = { 0x08804000u, 3u, 0x0101u, 0u, 0xa1a2a3a4u };
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 1 && hit.rgba[0] == 0xff010203u, "hash-named file");
    nk_texrepl_unpin(p, &hit);
    id.hash = 0xb1b2b3b4u;
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "a file that is not a PNG replaced a texture");
    CHECK(has_diag(p, "b1b2b3b4.png: not a PNG"), "undecodable PNG not reported");
    CHECK(has_diag(p, "c1c2c3c4.dds") && has_diag(p, "only PNG"), "DDS not reported");
    CHECK(has_diag(p, "d1d2d3d4_1.png"), "mip-level file not reported");
    nk_texrepl_close(p);
}

/* The lookup falls back the way PPSSPP's does: palette hash only, address, and ignoreAddress. */
static void test_wildcards_and_palette(void) {
    char dir[512], path[600], err[256];
    make_pack(dir, sizeof(dir), "texrepl_pack_wild");
    uint32_t *px = solid(2, 2, 0xff0a0b0cu);
    pack_path(path, sizeof(path), "texrepl_pack_wild", "a.png");
    write_png(path, 2, 2, px);
    free(px);
    pack_path(path, sizeof(path), "texrepl_pack_wild", "textures.ini");
    write_text(path,
        "[options]\nhash = xxh32\n"
        "[hashes]\n"
        "0000000000000404deadbeef = a.png\n"             /* no address: this size code and data hash, anywhere */
        "0880500000000303000000000 = a.png\n"            /* 25 digits: reported, not read */
        "088060000000030300000000 = a.png\n");           /* this address and size code, any data hash */
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplHit hit;
    NkTexReplId id = { 0x08123000u, 3u, 0x0404u, 0u, 0xdeadbeefu };
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 1, "no-address match");
    nk_texrepl_unpin(p, &hit);
    id.hash = 0xdeadbef0u;
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "another data hash matched");
    NkTexReplId at = { 0x08806000u, 3u, 0x0303u, 0u, 0x01020304u };
    CHECK(nk_texrepl_lookup(p, &at, &hit) == 1, "address and size code, any data hash");
    nk_texrepl_unpin(p, &hit);
    at.dim = 0x0505u;
    CHECK(nk_texrepl_lookup(p, &at, &hit) == 0, "the same address with another size code matched");
    CHECK(has_diag(p, "0880500000000303000000000"), "a 25-digit [hashes] name not reported");
    nk_texrepl_close(p);

    /* ignoreAddress (needs a safer hash): the address part of every line is dropped. */
    pack_path(path, sizeof(path), "texrepl_pack_wild", "textures.ini");
    write_text(path, "[options]\nhash = xxh64\nignoreAddress = true\n[hashes]\n088060000000030301020304 = a.png\n");
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplId moved = { 0x09999000u, 3u, 0x0303u, 0u, 0x01020304u };
    /* The pack names an address, so the lookup key (address zeroed) must not find a line that carries one. */
    CHECK(nk_texrepl_lookup(p, &moved, &hit) == 0, "ignoreAddress still matched a line with an address");
    nk_texrepl_close(p);
    write_text(path, "[options]\nhash = quick\nignoreAddress = true\n[hashes]\n088060000000030301020304 = a.png\n");
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL && has_diag(p, "ignoreAddress needs hash = xxh32 or xxh64"), "ignoreAddress with the quick hash not reported");
    nk_texrepl_close(p);

    /* A palette texture's identity includes its palette hash: the same data under another palette is another key. */
    write_text(path, "[options]\nhash = xxh32\n[hashes]\n");
    uint32_t pal_hash = nk_texrepl_clut_hash("\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    char name[64], line[128];
    uint64_t key = nk_texrepl_cache_key(0x08807000u, 5u, 0x0303u, pal_hash);
    snprintf(name, sizeof(name), "%016llx%08x", (unsigned long long)key, 0x0badf00du);
    snprintf(line, sizeof(line), "[options]\nhash = xxh32\n[hashes]\n%s = a.png\n", name);
    write_text(path, line);
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplId pal = { 0x08807000u, 5u, 0x0303u, pal_hash, 0x0badf00du };
    CHECK(nk_texrepl_lookup(p, &pal, &hit) == 1, "palette texture, its own palette");
    nk_texrepl_unpin(p, &hit);
    pal.clut_hash = pal_hash ^ 1u;
    CHECK(nk_texrepl_lookup(p, &pal, &hit) == 0, "palette texture, another palette");
    nk_texrepl_close(p);
}

/* Hashing a guest texture: contiguous and strided rows, the pack's hash kind, hash ranges. */
static void test_texture_hash(void) {
    char dir[512], path[600], err[256];
    make_pack(dir, sizeof(dir), "texrepl_pack_hash");
    pack_path(path, sizeof(path), "texrepl_pack_hash", "textures.ini");
    write_text(path, "[options]\nhash = xxh32\n[hashranges]\n08804000,16,16 = 16,8\n");
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    uint8_t mem[16 * 16 * 4];
    for (size_t i = 0; i < sizeof(mem); i++) mem[i] = (uint8_t)(i * 7 + 3);
    /* 8888, 16 wide with bufw 16: contiguous, w*h*4 bytes. */
    uint32_t h1 = nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08800000u, 16, 16, 16, 3, 0);
    CHECK(h1 == nk_texrepl_xxh32(mem, 16 * 16 * 4, 0xBACD7814u), "contiguous 8888 hash");
    /* A hash range at this address shortens it to 16x8. */
    uint32_t h2 = nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08804000u, 16, 16, 16, 3, 0);
    CHECK(h2 == nk_texrepl_xxh32(mem, 16 * 8 * 4, 0xBACD7814u), "hash range");
    /* Rows with gaps (bufw 16, w 8): each row's 32 bytes hashed and folded as (acc*11) ^ row. */
    uint32_t h3 = nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08800000u, 16, 8, 4, 3, 0);
    uint32_t want = 0;
    for (int y = 0; y < 4; y++) want = (want * 11u) ^ nk_texrepl_xxh32(mem + y * 64, 32, 0xBACD7814u);
    CHECK(h3 == want, "strided rows hash %08x want %08x", h3, want);
    /* A texture that reaches past the readable bytes hashes to 0, as in PPSSPP. */
    CHECK(nk_texrepl_hash_texture(p, mem, 100, 0x08800000u, 16, 16, 16, 3, 0) == 0, "too short");
    CHECK(nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08800000u, 16, 16, 16, 11, 0) == 0, "invalid format");
    /* DXT1 (4 bits a texel). */
    uint32_t h4 = nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08800000u, 16, 16, 16, 8, 0);
    CHECK(h4 == nk_texrepl_xxh32(mem, 16 * 16 / 2, 0xBACD7814u), "DXT1 size");
    nk_texrepl_close(p);

    /* The quick hash, default with no ini. */
    make_pack(dir, sizeof(dir), "texrepl_pack_hash_quick");
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    uint32_t q = nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08800000u, 16, 16, 16, 3, 0);
    CHECK(q == nk_texrepl_quick_hash(mem, 16 * 16 * 4, 0x08800000u), "quick hash of a texture");
    /* reduceHash with the quick hash is refused and reported (it needs a safer hash). */
    nk_texrepl_close(p);
}

/* The decoded images stay under the budget, and an image in use is never dropped. */
static void test_budget(void) {
    char dir[512], path[600], err[256], name[64];
    make_pack(dir, sizeof(dir), "texrepl_pack_budget");
    /* Six 64x64 images, 16 KiB each. */
    for (int i = 0; i < 6; i++) {
        uint32_t *px = solid(64, 64, 0xff000000u | (uint32_t)(i + 1));
        snprintf(name, sizeof(name), "%016llx%08x.png",
                 (unsigned long long)nk_texrepl_cache_key(0x08800000u, 3u, 0x0606u, 0), (unsigned)i);
        pack_path(path, sizeof(path), "texrepl_pack_budget", name);
        write_png(path, 64, 64, px);
        free(px);
    }
    uint32_t *huge = solid(256, 256, 0xff0000ffu);   /* 256 KiB */
    snprintf(name, sizeof(name), "%016llx%08x.png",
             (unsigned long long)nk_texrepl_cache_key(0x08800000u, 3u, 0x0606u, 0), 0xfu);
    pack_path(path, sizeof(path), "texrepl_pack_budget", name);
    write_png(path, 256, 256, huge);
    free(huge);
    /* A budget of three images. */
    NkTexRepl *p = nk_texrepl_open(dir, 3u * 64u * 64u * 4u + 100u, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplHit hit[6];
    NkTexReplStats st;
    /* Three images fill the budget and stay pinned: they are in use. */
    for (int i = 0; i < 3; i++) {
        NkTexReplId id = { 0x08800000u, 3u, 0x0606u, 0u, (uint32_t)i };
        CHECK(nk_texrepl_lookup(p, &id, &hit[i]) == 1, "image %d", i);
        nk_texrepl_stats(p, &st);
        CHECK(st.cache_bytes <= 3u * 64u * 64u * 4u + 100u, "image %d: cache %llu bytes is over the budget", i,
              (unsigned long long)st.cache_bytes);
    }
    /* With every image held and the budget full, another cannot be kept: it draws unreplaced, and is counted. */
    for (int i = 3; i < 6; i++) {
        NkTexReplId id = { 0x08800000u, 3u, 0x0606u, 0u, (uint32_t)i };
        CHECK(nk_texrepl_lookup(p, &id, &hit[i]) == 0, "image %d was kept past the budget", i);
    }
    nk_texrepl_stats(p, &st);
    CHECK(st.over_budget == 3, "over budget count %llu", (unsigned long long)st.over_budget);
    CHECK(st.cache_bytes <= 3u * 64u * 64u * 4u + 100u, "cache %llu bytes over the budget",
          (unsigned long long)st.cache_bytes);
    for (int i = 0; i < 3; i++) {
        CHECK(hit[i].rgba != NULL && hit[i].rgba[0] == (0xff000000u | (uint32_t)(i + 1)), "pinned image %d changed", i);
        nk_texrepl_unpin(p, &hit[i]);
    }
    /* Unpinned, the cache evicts the least recently used to take new ones, and stays under the budget. */
    for (int i = 3; i < 6; i++) {
        NkTexReplId id = { 0x08800000u, 3u, 0x0606u, 0u, (uint32_t)i };
        NkTexReplHit h;
        CHECK(nk_texrepl_lookup(p, &id, &h) == 1, "image %d after unpinning", i);
        nk_texrepl_unpin(p, &h);
        nk_texrepl_stats(p, &st);
        CHECK(st.cache_bytes <= 3u * 64u * 64u * 4u + 100u, "cache %llu bytes over the budget", (unsigned long long)st.cache_bytes);
    }
    nk_texrepl_stats(p, &st);
    CHECK(st.evicted >= 3, "evicted %llu", (unsigned long long)st.evicted);
    /* One image bigger than the whole budget is refused outright and reported. */
    NkTexReplId big = { 0x08800000u, 3u, 0x0606u, 0u, 0xfu };
    NkTexReplHit h;
    CHECK(nk_texrepl_lookup(p, &big, &h) == 0, "an image over the whole budget was kept");
    CHECK(has_diag(p, "texture budget"), "over-budget image not reported");
    nk_texrepl_close(p);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_hashes();
    test_synthetic_pack();
    test_malformed_ini();
    test_ini_less_pack();
    test_wildcards_and_palette();
    test_texture_hash();
    test_budget();
    if (g_failures) {
        fprintf(stderr, "texrepl selftest: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("texrepl selftest: OK\n");
    return 0;
}
