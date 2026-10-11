/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_font.h"
#include "nk_types.h"
#include "nk_platform.h"
#include "nk_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -----------------------------------------------------------------------------
 * Embedded SHA-256 Digest Implementation (self-contained, public-domain design)
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_len;
} FontSha256;

static uint32_t font_sha256_rotr(uint32_t value, unsigned int amount) {
    return (value >> amount) | (value << (32u - amount));
}

static void font_sha256_transform(FontSha256 *ctx, const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t words[64];
    for (size_t i = 0; i < 16; i++) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   (uint32_t)block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = font_sha256_rotr(words[i - 15], 7) ^
                      font_sha256_rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 = font_sha256_rotr(words[i - 2], 17) ^
                      font_sha256_rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t sum1 = font_sha256_rotr(e, 6) ^ font_sha256_rotr(e, 11) ^ font_sha256_rotr(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t t1 = h + sum1 + choose + k[i] + words[i];
        uint32_t sum0 = font_sha256_rotr(a, 2) ^ font_sha256_rotr(a, 13) ^ font_sha256_rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = sum0 + majority;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void font_sha256_init(FontSha256 *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bit_count = 0;
    ctx->block_len = 0;
}

static void font_sha256_update(FontSha256 *ctx, const uint8_t *data, size_t len) {
    ctx->bit_count += (uint64_t)len * 8u;
    while (len > 0) {
        size_t room = sizeof(ctx->block) - ctx->block_len;
        size_t take = len < room ? len : room;
        memcpy(ctx->block + ctx->block_len, data, take);
        ctx->block_len += take;
        data += take;
        len -= take;
        if (ctx->block_len == sizeof(ctx->block)) {
            font_sha256_transform(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

static void font_sha256_final(FontSha256 *ctx, uint8_t digest[32]) {
    ctx->block[ctx->block_len++] = 0x80u;
    if (ctx->block_len > 56) {
        memset(ctx->block + ctx->block_len, 0, sizeof(ctx->block) - ctx->block_len);
        font_sha256_transform(ctx, ctx->block);
        ctx->block_len = 0;
    }
    memset(ctx->block + ctx->block_len, 0, 56 - ctx->block_len);
    for (size_t i = 0; i < 8; i++) {
        ctx->block[56 + i] = (uint8_t)(ctx->bit_count >> ((7 - i) * 8u));
    }
    font_sha256_transform(ctx, ctx->block);
    for (size_t i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

/* -----------------------------------------------------------------------------
 * Inspection: the native reader's verdict, the slot rule, and the file identity
 * -------------------------------------------------------------------------- */
static const char *const kSlotRole[NK_FONT_SLOT_COUNT] = { "Japanese", "Latin", "Korean" };
static const char *const kSlotId[NK_FONT_SLOT_COUNT] = { "japanese", "latin", "korean" };
static const char *const kSlotFile[NK_FONT_SLOT_COUNT] = {
    NK_FONT_SLOT_JAPANESE_FILE, NK_FONT_SLOT_LATIN_FILE, NK_FONT_SLOT_KOREAN_FILE
};

static void font_copy_text(char *out, size_t out_len, const char *text) {
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "%s", text ? text : "");
}

static const char *font_refusal_text(PgfRefusal refusal) {
    switch (refusal) {
    case PGF_REFUSE_TOO_LARGE: return "larger than the 16 MiB reader ceiling";
    case PGF_REFUSE_TRUNCATED: return "truncated";
    case PGF_REFUSE_HEADER_OFFSET: return "header offset is not zero";
    case PGF_REFUSE_MAGIC: return "invalid PGF magic (expected PGF0)";
    case PGF_REFUSE_REVISION: return "unsupported revision or version";
    case PGF_REFUSE_HEADER_SIZE: return "header size does not match the revision";
    case PGF_REFUSE_GLYPH_RANGE: return "corrupt glyph indices (first exceeds last)";
    case PGF_REFUSE_COUNTS: return "count or bit width out of range";
    case PGF_REFUSE_NO_GLYPHS: return "no glyph records";
    case PGF_REFUSE_GLYPH: return "a glyph record fails the reader's checks";
    case PGF_REFUSE_NONE: return "";
    }
    return "refused by the reader";
}

static void font_sha256_hex(const uint8_t *bytes, size_t length, char out[65]) {
    FontSha256 sha;
    uint8_t digest[32];
    font_sha256_init(&sha);
    font_sha256_update(&sha, bytes, length);
    font_sha256_final(&sha, digest);
    for (size_t i = 0; i < 32; i++) {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
    out[64] = '\0';
}

int nk_font_classify_verdict(const PgfVerdict *verdict, char *reason, size_t reason_len) {
    if (!verdict || verdict->refusal != PGF_REFUSE_NONE) {
        font_copy_text(reason, reason_len, "the reader refused the file");
        return -1;
    }
    if (verdict->has_hangul && verdict->has_kana) {
        font_copy_text(reason, reason_len, "covers both Hangul and kana, so it names no slot");
        return -1;
    }
    if (verdict->has_hangul) return NK_FONT_SLOT_KOREAN;
    if (verdict->has_kana) return NK_FONT_SLOT_JAPANESE;
    if (verdict->has_latin) return NK_FONT_SLOT_LATIN;
    font_copy_text(reason, reason_len, "covers none of the slot probe characters");
    return -1;
}

/* Read a whole open file no larger than the reader ceiling, from its start, and leave the
 * handle at its start again. The caller frees *out. */
static bool font_read_stream(FILE *f, uint8_t **out, size_t *out_len,
                             char *out_error, size_t error_len) {
    int64_t size;
    uint8_t *bytes;
    size_t received;
    *out = NULL;
    *out_len = 0;
    if (fseek(f, 0, SEEK_END) != 0 || (size = nk_ftell64(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        font_copy_text(out_error, error_len, "cannot read the file size");
        return false;
    }
    if ((uint64_t)size > NK_FONT_PGF_MAX_BYTES) {
        font_copy_text(out_error, error_len, "larger than the 16 MiB reader ceiling");
        return false;
    }
    bytes = (uint8_t *)malloc(size > 0 ? (size_t)size : 1u);
    if (!bytes) {
        font_copy_text(out_error, error_len, "out of memory");
        return false;
    }
    received = size > 0 ? fread(bytes, 1u, (size_t)size, f) : 0u;
    if (received != (size_t)size || ferror(f) || fseek(f, 0, SEEK_SET) != 0) {
        free(bytes);
        font_copy_text(out_error, error_len, "cannot read the file");
        return false;
    }
    *out = bytes;
    *out_len = (size_t)size;
    return true;
}

/* Read a whole file no larger than the reader ceiling. The caller frees *out. */
static bool font_read_file(const char *path, uint8_t **out, size_t *out_len,
                           char *out_error, size_t error_len) {
    FILE *f = nk_fopen_utf8(path, "rb");
    bool ok;
    *out = NULL;
    *out_len = 0;
    if (!f) {
        font_copy_text(out_error, error_len, "cannot open the file");
        return false;
    }
    ok = font_read_stream(f, out, out_len, out_error, error_len);
    fclose(f);
    return ok;
}

/* Check bytes already in memory. Returns true when the reader accepts them. */
static bool font_inspect_bytes(const uint8_t *bytes, size_t length, NkFontInspection *out) {
    memset(out, 0, sizeof(*out));
    out->size = (uint64_t)length;
    out->slot = -1;
    font_sha256_hex(bytes, length, out->sha256);
    if (!pgf_validate_memory(bytes, length, &out->verdict)) {
        font_copy_text(out->slot_reason, sizeof(out->slot_reason),
                       font_refusal_text(out->verdict.refusal));
        return false;
    }
    out->slot = nk_font_classify_verdict(&out->verdict, out->slot_reason, sizeof(out->slot_reason));
    return true;
}

bool nk_font_inspect_pgf(const char *file_path, NkFontInspection *out,
                         char *out_error, size_t error_len) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    bool accepted;
    if (!file_path || !*file_path || !out) {
        font_copy_text(out_error, error_len, "no font file was named");
        return false;
    }
    if (!font_read_file(file_path, &bytes, &length, out_error, error_len)) return false;
    accepted = font_inspect_bytes(bytes, length, out);
    free(bytes);
    if (!accepted) {
        char text[NK_FONT_DETAIL_MAX * 2];
        snprintf(text, sizeof(text), "the PGF reader refused the file: %s", out->slot_reason);
        font_copy_text(out_error, error_len, text);
        return false;
    }
    return true;
}

bool nk_font_validate_pgf(const char *file_path, uint64_t *out_size,
                          char *out_sha256, char *out_error, size_t error_len) {
    NkFontInspection inspection;
    if (!nk_font_inspect_pgf(file_path, &inspection, out_error, error_len)) return false;
    if (out_size) *out_size = inspection.size;
    if (out_sha256) memcpy(out_sha256, inspection.sha256, sizeof(inspection.sha256));
    return true;
}

/* -----------------------------------------------------------------------------
 * The imported-font manifest (schema 2): <user data>/fonts/v2/manifest.json
 * -------------------------------------------------------------------------- */
typedef struct {
    bool present[NK_FONT_SLOT_COUNT];
    uint64_t size[NK_FONT_SLOT_COUNT];
    char sha256[NK_FONT_SLOT_COUNT][65];
} FontManifest;

bool nk_font_get_cache_dir(const char *user_data_root, char *out_dir, size_t max_len) {
    if (!out_dir || max_len == 0) return false;
    char base[NK_MAX_PATH];
    if (user_data_root && *user_data_root) {
        snprintf(base, sizeof(base), "%s", user_data_root);
    } else {
        if (!nk_platform_get_path(NK_PATH_DATA, base, sizeof(base))) return false;
    }
    char sep = nk_platform_path_separator();
    int written = snprintf(out_dir, max_len, "%s%c%s%c%s", base, sep,
                           NK_FONT_CACHE_PARENT, sep, NK_FONT_CACHE_SUBDIR);
    return written > 0 && (size_t)written < max_len;
}

static void font_join(char *out, size_t out_len, const char *dir, const char *name) {
    snprintf(out, out_len, "%s%c%s", dir, nk_platform_path_separator(), name);
}

/* The staging name for a target path: the target with ".tmp" appended. */
static void font_staging_name(char *out, size_t out_len, const char *target) {
    size_t length = strlen(target);
    if (out_len == 0) return;
    if (length + 5u > out_len) {
        out[0] = '\0';
        return;
    }
    memcpy(out, target, length);
    memcpy(out + length, ".tmp", 4u);
    out[length + 4u] = '\0';
}

static NkFontStatus font_load_manifest(const char *cache_dir, FontManifest *manifest,
                                       char *message, size_t message_len) {
    char path[NK_MAX_PATH * 2];
    FILE *mf;
    long length;
    char *json_buf;
    size_t read_bytes;
    char parse_err[128] = "";
    NkJsonNode *root;
    NkJsonNode *version;
    NkJsonNode *files;
    int64_t schema = 0;
    size_t count;

    memset(manifest, 0, sizeof(*manifest));
    font_join(path, sizeof(path), cache_dir, NK_FONT_MANIFEST_NAME);
    if (!nk_platform_file_exists(path)) {
        font_copy_text(message, message_len, "No imported PSP font.");
        return NK_FONT_STATUS_MISSING;
    }
    mf = nk_fopen_utf8(path, "rb");
    if (!mf || fseek(mf, 0, SEEK_END) != 0 || (length = ftell(mf)) <= 0 || length > 1024 * 1024 ||
        fseek(mf, 0, SEEK_SET) != 0) {
        if (mf) fclose(mf);
        font_copy_text(message, message_len, "PSP font manifest is unreadable; run fonts import <folder>.");
        return NK_FONT_STATUS_INVALID;
    }
    json_buf = (char *)malloc((size_t)length + 1u);
    if (!json_buf) {
        fclose(mf);
        return NK_FONT_STATUS_INVALID;
    }
    read_bytes = fread(json_buf, 1u, (size_t)length, mf);
    fclose(mf);
    json_buf[read_bytes] = '\0';
    root = nk_json_parse(json_buf, read_bytes, parse_err, sizeof(parse_err));
    free(json_buf);
    if (!root || !nk_json_is_object(root)) {
        if (root) nk_json_free(root);
        font_copy_text(message, message_len, "PSP font manifest is malformed; run fonts import <folder>.");
        return NK_FONT_STATUS_INVALID;
    }
    version = nk_json_obj_get(root, "schema_version");
    if (!version || !nk_json_get_int64(version, &schema) || schema != NK_FONT_CACHE_SCHEMA_VERSION) {
        nk_json_free(root);
        font_copy_text(message, message_len, "PSP font manifest schema is not version 2; run fonts import <folder>.");
        return NK_FONT_STATUS_INVALID;
    }
    files = nk_json_obj_get(root, "files");
    if (!files || !nk_json_is_object(files) || files->u.obj.count == 0) {
        nk_json_free(root);
        font_copy_text(message, message_len, "PSP font manifest lists no files; run fonts import <folder>.");
        return NK_FONT_STATUS_INVALID;
    }
    for (count = 0; count < files->u.obj.count; count++) {
        const char *name = files->u.obj.members[count].key;
        NkJsonNode *entry = files->u.obj.members[count].val;
        NkJsonNode *slot_node;
        NkJsonNode *size_node;
        NkJsonNode *sha_node;
        NkJsonNode *source_node;
        const char *slot_text;
        const char *sha_text;
        const char *source_text;
        int64_t size = 0;
        int slot = -1;
        int i;
        bool entry_ok = false;
        if (!name || !entry || !nk_json_is_object(entry)) break;
        for (i = 0; i < NK_FONT_SLOT_COUNT; i++) {
            if (strcmp(name, kSlotFile[i]) == 0) slot = i;
        }
        if (slot < 0 || manifest->present[slot]) break;
        slot_node = nk_json_obj_get(entry, "slot");
        size_node = nk_json_obj_get(entry, "size");
        sha_node = nk_json_obj_get(entry, "sha256");
        source_node = nk_json_obj_get(entry, "source");
        slot_text = nk_json_get_string(slot_node);
        sha_text = nk_json_get_string(sha_node);
        source_text = nk_json_get_string(source_node);
        entry_ok = slot_text && strcmp(slot_text, kSlotId[slot]) == 0 &&
                   size_node && nk_json_get_int64(size_node, &size) && size > 0 &&
                   sha_text && strlen(sha_text) == 64u &&
                   source_text && strcmp(source_text, NK_FONT_MANIFEST_SOURCE_USER) == 0;
        if (!entry_ok) break;
        manifest->present[slot] = true;
        manifest->size[slot] = (uint64_t)size;
        snprintf(manifest->sha256[slot], sizeof(manifest->sha256[slot]), "%s", sha_text);
    }
    if (count < files->u.obj.count) {
        nk_json_free(root);
        font_copy_text(message, message_len,
                       "PSP font manifest has an entry this build does not accept; run fonts import <folder>.");
        return NK_FONT_STATUS_INVALID;
    }
    nk_json_free(root);
    font_copy_text(message, message_len, "Imported PSP fonts are listed in the manifest.");
    return NK_FONT_STATUS_OK;
}

/* Check one slot's bytes against its manifest entry: the reader accepts them, they name the slot,
 * and their size and SHA-256 match the entry. */
static bool font_check_slot_bytes(int slot, const FontManifest *manifest, const uint8_t *bytes,
                                  size_t length, char *reason, size_t reason_len) {
    NkFontInspection inspection;
    if (!font_inspect_bytes(bytes, length, &inspection)) {
        snprintf(reason, reason_len, "the reader refused it: %s", inspection.slot_reason);
        return false;
    }
    if (inspection.slot != slot) {
        snprintf(reason, reason_len, "it does not name the %s slot", kSlotRole[slot]);
        return false;
    }
    if (inspection.size != manifest->size[slot] || strcmp(inspection.sha256, manifest->sha256[slot]) != 0) {
        font_copy_text(reason, reason_len, "its size or SHA-256 differs from the manifest");
        return false;
    }
    return true;
}

/* Check one manifest entry against its file: the file exists, its size and SHA-256 match,
 * the reader accepts it, and it names the slot the manifest gives. */
static bool font_check_slot_file(const char *cache_dir, int slot, const FontManifest *manifest,
                                 char *reason, size_t reason_len) {
    char path[NK_MAX_PATH * 2];
    uint8_t *bytes = NULL;
    size_t length = 0;
    bool ok;
    font_join(path, sizeof(path), cache_dir, kSlotFile[slot]);
    if (!font_read_file(path, &bytes, &length, reason, reason_len)) return false;
    ok = font_check_slot_bytes(slot, manifest, bytes, length, reason, reason_len);
    free(bytes);
    return ok;
}

NkFontStatus nk_font_check_cache(const char *user_data_root, char *out_message, size_t message_max_len) {
    char cache_dir[NK_MAX_PATH];
    FontManifest manifest;
    NkFontStatus status;
    char message[NK_FONT_TEXT_MAX] = "";
    if (!nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        font_copy_text(out_message, message_max_len, "PSP font cache location is unavailable.");
        return NK_FONT_STATUS_MISSING;
    }
    status = font_load_manifest(cache_dir, &manifest, message, sizeof(message));
    if (status == NK_FONT_STATUS_OK) {
        for (int slot = 0; slot < NK_FONT_SLOT_COUNT && status == NK_FONT_STATUS_OK; slot++) {
            char reason[NK_FONT_TEXT_MAX] = "";
            if (!manifest.present[slot]) continue;
            if (!font_check_slot_file(cache_dir, slot, &manifest, reason, sizeof(reason))) {
                snprintf(message, sizeof(message), "Imported %s font is invalid: %.400s; run fonts import <folder>.",
                         kSlotRole[slot], reason);
                status = NK_FONT_STATUS_INVALID;
            }
        }
    }
    font_copy_text(out_message, message_max_len, message);
    return status;
}

NkFontSlotCheck nk_font_check_cache_slot_file(const char *user_data_root, NkFontSlot slot,
                                              FILE *cache_file, char *reason, size_t reason_len) {
    char cache_dir[NK_MAX_PATH];
    FontManifest manifest;
    NkFontStatus status;
    char message[NK_FONT_TEXT_MAX] = "";
    uint8_t *bytes = NULL;
    size_t length = 0;
    bool ok;
    if (reason_len > 0) reason[0] = '\0';
    if (!cache_file || (unsigned)slot >= NK_FONT_SLOT_COUNT) {
        font_copy_text(reason, reason_len, "no cache file or font slot was named");
        return NK_FONT_SLOT_CHECK_FILE_INVALID;
    }
    if (!nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        font_copy_text(reason, reason_len, "the per-user font cache location is unavailable");
        return NK_FONT_SLOT_CHECK_NO_MANIFEST;
    }
    status = font_load_manifest(cache_dir, &manifest, message, sizeof(message));
    if (status == NK_FONT_STATUS_MISSING) {
        font_copy_text(reason, reason_len,
                       "the font cache has no manifest.json (import the font from your PSP to write one)");
        return NK_FONT_SLOT_CHECK_NO_MANIFEST;
    }
    if (status != NK_FONT_STATUS_OK) {
        font_copy_text(reason, reason_len, message);
        return NK_FONT_SLOT_CHECK_MANIFEST_INVALID;
    }
    if (!manifest.present[slot]) {
        snprintf(reason, reason_len, "the manifest does not list the %s slot", kSlotRole[slot]);
        return NK_FONT_SLOT_CHECK_NOT_LISTED;
    }
    if (!font_read_stream(cache_file, &bytes, &length, reason, reason_len)) return NK_FONT_SLOT_CHECK_FILE_INVALID;
    ok = font_check_slot_bytes((int)slot, &manifest, bytes, length, reason, reason_len);
    free(bytes);
    return ok ? NK_FONT_SLOT_CHECK_OK : NK_FONT_SLOT_CHECK_FILE_INVALID;
}

void nk_font_slot_states(const char *user_data_root, const char *project_root,
                         NkFontSlotState states[NK_FONT_SLOT_COUNT]) {
    char cache_dir[NK_MAX_PATH];
    FontManifest manifest;
    char manifest_message[NK_FONT_TEXT_MAX] = "";
    NkFontStatus manifest_status = NK_FONT_STATUS_MISSING;
    if (!states) return;
    memset(&manifest, 0, sizeof(manifest));
    if (nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        manifest_status = font_load_manifest(cache_dir, &manifest, manifest_message, sizeof(manifest_message));
    }
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        NkFontSlotState *state = &states[slot];
        char reason[NK_FONT_TEXT_MAX] = "";
        memset(state, 0, sizeof(*state));
        state->source = NK_FONT_SOURCE_NONE;
        if (manifest_status == NK_FONT_STATUS_OK && manifest.present[slot]) {
            if (font_check_slot_file(cache_dir, slot, &manifest, reason, sizeof(reason))) {
                state->source = NK_FONT_SOURCE_USER;
                snprintf(state->detail, sizeof(state->detail), "%s: imported from your PSP.", kSlotRole[slot]);
                continue;
            }
        } else if (manifest_status == NK_FONT_STATUS_INVALID) {
            font_copy_text(reason, sizeof(reason), manifest_message);
        }
        if (project_root && *project_root) {
            char project_dir[NK_MAX_PATH * 2];
            char project_path[NK_MAX_PATH * 2];
            NkFontInspection inspection;
            char error[NK_FONT_DETAIL_MAX] = "";
            snprintf(project_dir, sizeof(project_dir), "%s%c%s", project_root,
                     nk_platform_path_separator(), NK_FONT_PROJECT_SUBDIR);
            font_join(project_path, sizeof(project_path), project_dir, kSlotFile[slot]);
            if (nk_platform_file_exists(project_path) &&
                nk_font_inspect_pgf(project_path, &inspection, error, sizeof(error)) &&
                inspection.slot == slot) {
                state->source = NK_FONT_SOURCE_PROJECT;
                snprintf(state->detail, sizeof(state->detail), "%s: project font.", kSlotRole[slot]);
                continue;
            }
        }
        if (reason[0]) {
            snprintf(state->detail, sizeof(state->detail), "%s: missing (%.400s).", kSlotRole[slot], reason);
        } else {
            snprintf(state->detail, sizeof(state->detail),
                     "%s: missing. Import a font from your PSP or install the project font.", kSlotRole[slot]);
        }
    }
}

/* -----------------------------------------------------------------------------
 * Writing: file replacement and the manifest
 * -------------------------------------------------------------------------- */
static bool font_write_bytes(const char *path, const uint8_t *bytes, size_t length) {
    FILE *f = nk_fopen_utf8(path, "wb");
    bool ok;
    if (!f) return false;
    ok = length == 0u || fwrite(bytes, 1u, length, f) == length;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* Write the manifest for the slots marked present. An empty manifest is not written. */
static bool font_write_manifest(const char *cache_dir, const FontManifest *manifest) {
    char path[NK_MAX_PATH * 2];
    char tmp[NK_MAX_PATH * 2];
    char stamp[32] = "unknown";
    char text[4096];
    size_t used = 0;
    bool any = false;
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    font_join(path, sizeof(path), cache_dir, NK_FONT_MANIFEST_NAME);
    font_staging_name(tmp, sizeof(tmp), path);
    if (utc) strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", utc);
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) if (manifest->present[slot]) any = true;
    if (!any) return false;
    used += (size_t)snprintf(text + used, sizeof(text) - used,
                             "{\n  \"schema_version\": %d,\n  \"import_time\": \"%s\",\n  \"files\": {\n",
                             NK_FONT_CACHE_SCHEMA_VERSION, stamp);
    any = false;
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        if (!manifest->present[slot]) continue;
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "%s    \"%s\": {\"slot\": \"%s\", \"size\": %llu, \"sha256\": \"%s\", "
                                 "\"source\": \"%s\", \"reader_version\": %d}",
                                 any ? ",\n" : "", kSlotFile[slot], kSlotId[slot],
                                 (unsigned long long)manifest->size[slot], manifest->sha256[slot],
                                 NK_FONT_MANIFEST_SOURCE_USER, NK_FONT_READER_VERSION);
        any = true;
    }
    used += (size_t)snprintf(text + used, sizeof(text) - used, "\n  }\n}\n");
    if (used >= sizeof(text)) return false;
    if (!font_write_bytes(tmp, (const uint8_t *)text, used)) return false;
    if (nk_rename_utf8(tmp, path) != 0) {
        nk_remove_utf8(tmp);
        return false;
    }
    return true;
}

/* -----------------------------------------------------------------------------
 * Import and removal
 * -------------------------------------------------------------------------- */
typedef struct {
    char names[NK_FONT_IMPORT_MAX_FILES][256];
    int count;
    bool overflow;
} FontFolderScan;

static bool font_collect_pgf(const char *name, void *context) {
    FontFolderScan *scan = (FontFolderScan *)context;
    size_t length = strlen(name);
    if (length < 5u) return true;
    if (tolower((unsigned char)name[length - 4u]) != '.' ||
        tolower((unsigned char)name[length - 3u]) != 'p' ||
        tolower((unsigned char)name[length - 2u]) != 'g' ||
        tolower((unsigned char)name[length - 1u]) != 'f') {
        return true;
    }
    if (scan->count >= NK_FONT_IMPORT_MAX_FILES) {
        scan->overflow = true;
        return false;
    }
    snprintf(scan->names[scan->count], sizeof(scan->names[scan->count]), "%s", name);
    scan->count++;
    return true;
}

static void font_sort_names(FontFolderScan *scan) {
    for (int i = 1; i < scan->count; i++) {
        char held[256];
        int j = i;
        memcpy(held, scan->names[i], sizeof(held));
        while (j > 0 && strcmp(scan->names[j - 1], held) > 0) {
            memcpy(scan->names[j], scan->names[j - 1], sizeof(held));
            j--;
        }
        memcpy(scan->names[j], held, sizeof(held));
    }
}

bool nk_font_import_folder(const char *user_data_root, const char *folder,
                           const char *const choose[NK_FONT_SLOT_COUNT],
                           NkFontImportResult *result, char *out_error, size_t error_len) {
    FontFolderScan scan;
    NkFontInspection inspections[NK_FONT_IMPORT_MAX_FILES];
    int candidates[NK_FONT_SLOT_COUNT][NK_FONT_IMPORT_MAX_FILES];
    int candidate_count[NK_FONT_SLOT_COUNT] = { 0, 0, 0 };
    int selected[NK_FONT_SLOT_COUNT] = { -1, -1, -1 };
    char cache_dir[NK_MAX_PATH];
    FontManifest manifest;
    char manifest_message[NK_FONT_TEXT_MAX] = "";
    NkFontStatus manifest_status;
    uint8_t *bytes_for_slot[NK_FONT_SLOT_COUNT] = { NULL, NULL, NULL };
    size_t length_for_slot[NK_FONT_SLOT_COUNT] = { 0, 0, 0 };

    if (!result || !folder || !*folder) {
        font_copy_text(out_error, error_len, "no folder was given");
        return false;
    }
    memset(result, 0, sizeof(*result));
    memset(&scan, 0, sizeof(scan));
    if (!nk_platform_list_files(folder, font_collect_pgf, &scan)) {
        font_copy_text(out_error, error_len, "the folder cannot be read");
        return false;
    }
    if (scan.overflow) {
        snprintf(out_error, error_len, "the folder holds more than %d .pgf files", NK_FONT_IMPORT_MAX_FILES);
        return false;
    }
    if (scan.count == 0) {
        font_copy_text(out_error, error_len, "no .pgf files are in the folder");
        return false;
    }
    font_sort_names(&scan);
    if (!nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir)) ||
        !nk_platform_mkdir_p_private(cache_dir)) {
        font_copy_text(out_error, error_len, "the PSP font cache folder cannot be created");
        return false;
    }

    /* Read and classify every candidate. Only the metadata is kept; the bytes are read again
     * for the chosen file, so the copy is the file that was checked. */
    result->file_count = scan.count;
    for (int i = 0; i < scan.count; i++) {
        char path[NK_MAX_PATH * 2];
        uint8_t *bytes = NULL;
        size_t length = 0;
        char error[NK_FONT_DETAIL_MAX] = "";
        NkFontFileOutcome *outcome = &result->files[i];
        snprintf(outcome->name, sizeof(outcome->name), "%s", scan.names[i]);
        outcome->slot = -1;
        font_join(path, sizeof(path), folder, scan.names[i]);
        if (!font_read_file(path, &bytes, &length, error, sizeof(error))) {
            snprintf(outcome->detail, sizeof(outcome->detail), "refused: %s", error);
            continue;
        }
        if (!font_inspect_bytes(bytes, length, &inspections[i])) {
            snprintf(outcome->detail, sizeof(outcome->detail), "refused: %s", inspections[i].slot_reason);
        } else if (inspections[i].slot < 0) {
            snprintf(outcome->detail, sizeof(outcome->detail), "refused: %s", inspections[i].slot_reason);
        } else {
            outcome->slot = inspections[i].slot;
            snprintf(outcome->detail, sizeof(outcome->detail), "names the %s slot", kSlotRole[outcome->slot]);
            candidates[outcome->slot][candidate_count[outcome->slot]++] = i;
        }
        free(bytes);
    }

    /* Choose one file per slot: the named choice, else the only candidate. A slot with several
     * candidates and no choice is left alone. */
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        const char *want = choose ? choose[slot] : NULL;
        if (want && *want) {
            bool found = false;
            for (int c = 0; c < candidate_count[slot]; c++) {
                int index = candidates[slot][c];
                if (strcmp(result->files[index].name, want) == 0) {
                    selected[slot] = index;
                    found = true;
                }
            }
            if (!found) {
                snprintf(out_error, error_len, "the chosen %s file does not name that slot", kSlotRole[slot]);
                for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
                return false;
            }
        } else if (candidate_count[slot] == 1) {
            selected[slot] = candidates[slot][0];
        } else if (candidate_count[slot] > 1) {
            for (int c = 0; c < candidate_count[slot]; c++) {
                NkFontFileOutcome *outcome = &result->files[candidates[slot][c]];
                snprintf(outcome->detail, sizeof(outcome->detail),
                         "not imported: %d files name the %s slot; choose one",
                         candidate_count[slot], kSlotRole[slot]);
            }
        }
        if (selected[slot] >= 0) {
            for (int c = 0; c < candidate_count[slot]; c++) {
                NkFontFileOutcome *outcome = &result->files[candidates[slot][c]];
                if (candidates[slot][c] != selected[slot]) {
                    snprintf(outcome->detail, sizeof(outcome->detail), "not chosen for the %s slot",
                             kSlotRole[slot]);
                }
            }
        }
    }

    /* Read each chosen file once more, check the bytes that will be copied, and stage them. */
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        char source[NK_MAX_PATH * 2];
        char error[NK_FONT_DETAIL_MAX] = "";
        NkFontInspection checked;
        if (selected[slot] < 0) continue;
        font_join(source, sizeof(source), folder, result->files[selected[slot]].name);
        if (!font_read_file(source, &bytes_for_slot[slot], &length_for_slot[slot], error, sizeof(error)) ||
            !font_inspect_bytes(bytes_for_slot[slot], length_for_slot[slot], &checked) ||
            checked.slot != slot ||
            strcmp(checked.sha256, inspections[selected[slot]].sha256) != 0) {
            snprintf(out_error, error_len, "the chosen %s file changed or failed its check; try again", kSlotRole[slot]);
            for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
            return false;
        }
    }

    /* Merge into the manifest: keep the entries for slots this run does not change. */
    manifest_status = font_load_manifest(cache_dir, &manifest, manifest_message, sizeof(manifest_message));
    if (manifest_status != NK_FONT_STATUS_OK) memset(&manifest, 0, sizeof(manifest));
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        char target[NK_MAX_PATH * 2];
        char staged[NK_MAX_PATH * 2];
        if (selected[slot] < 0) continue;
        font_join(target, sizeof(target), cache_dir, kSlotFile[slot]);
        font_staging_name(staged, sizeof(staged), target);
        if (!font_write_bytes(staged, bytes_for_slot[slot], length_for_slot[slot])) {
            for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) {
                char target_path[NK_MAX_PATH * 2];
                char left[NK_MAX_PATH * 2];
                font_join(target_path, sizeof(target_path), cache_dir, kSlotFile[s]);
                font_staging_name(left, sizeof(left), target_path);
                nk_remove_utf8(left);
            }
            font_copy_text(out_error, error_len, "the font could not be written to the cache");
            for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
            return false;
        }
    }
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        char target[NK_MAX_PATH * 2];
        char staged[NK_MAX_PATH * 2];
        if (selected[slot] < 0) continue;
        font_join(target, sizeof(target), cache_dir, kSlotFile[slot]);
        font_staging_name(staged, sizeof(staged), target);
        if (nk_rename_utf8(staged, target) != 0) {
            nk_remove_utf8(staged);
            font_copy_text(out_error, error_len, "the font could not be committed to the cache");
            for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
            return false;
        }
        manifest.present[slot] = true;
        manifest.size[slot] = inspections[selected[slot]].size;
        snprintf(manifest.sha256[slot], sizeof(manifest.sha256[slot]), "%s", inspections[selected[slot]].sha256);
        result->slot_imported[slot] = true;
        result->imported_count++;
        result->files[selected[slot]].imported = true;
        snprintf(result->files[selected[slot]].detail, sizeof(result->files[selected[slot]].detail),
                 "imported as the %s font", kSlotRole[slot]);
    }
    for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
    if (result->imported_count > 0 && !font_write_manifest(cache_dir, &manifest)) {
        font_copy_text(out_error, error_len, "the font manifest could not be written");
        return false;
    }
    return true;
}

int nk_font_remove_imports(const char *user_data_root,
                           const bool remove[NK_FONT_SLOT_COUNT],
                           char *out_error, size_t error_len) {
    char cache_dir[NK_MAX_PATH];
    FontManifest manifest;
    char message[NK_FONT_TEXT_MAX] = "";
    NkFontStatus status;
    int removed = 0;
    bool any_left = false;
    if (!remove) {
        font_copy_text(out_error, error_len, "no slot was named");
        return -1;
    }
    if (!nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        font_copy_text(out_error, error_len, "PSP font cache location is unavailable");
        return -1;
    }
    status = font_load_manifest(cache_dir, &manifest, message, sizeof(message));
    /* The cache folder holds only this flow's slot files, so a named slot's file is removed
     * even when the manifest is unreadable; the manifest is then rewritten or removed. */
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        char path[NK_MAX_PATH * 2];
        if (!remove[slot]) continue;
        font_join(path, sizeof(path), cache_dir, kSlotFile[slot]);
        if (nk_platform_file_exists(path) && nk_remove_utf8(path) == 0) removed++;
        manifest.present[slot] = false;
    }
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) if (manifest.present[slot]) any_left = true;
    if (status == NK_FONT_STATUS_OK && any_left) {
        if (!font_write_manifest(cache_dir, &manifest)) {
            font_copy_text(out_error, error_len, "the font manifest could not be rewritten");
            return -1;
        }
    } else {
        char path[NK_MAX_PATH * 2];
        font_join(path, sizeof(path), cache_dir, NK_FONT_MANIFEST_NAME);
        if (nk_platform_file_exists(path) && nk_remove_utf8(path) != 0) {
            font_copy_text(out_error, error_len, "the font manifest could not be removed");
            return -1;
        }
    }
    return removed;
}

bool nk_font_resolve_directory(const char *user_data_root,
                               const char *fallback_root,
                               char *out_dir,
                               size_t max_len) {
    char cache_dir[NK_MAX_PATH];
    char message[NK_FONT_TEXT_MAX] = "";
    if (!out_dir || max_len == 0) return false;
    if (nk_font_check_cache(user_data_root, message, sizeof(message)) == NK_FONT_STATUS_OK &&
        nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        snprintf(out_dir, max_len, "%s", cache_dir);
        return true;
    }
    if (fallback_root && *fallback_root) {
        char fpath[NK_MAX_PATH * 2];
        snprintf(fpath, sizeof(fpath), "%s%c%s", fallback_root, nk_platform_path_separator(), NK_FONT_PROJECT_SUBDIR);
        if (nk_platform_dir_exists(fpath)) {
            char abs_font[NK_MAX_PATH];
            if (nk_platform_absolute_path(fpath, abs_font, sizeof(abs_font))) {
                snprintf(out_dir, max_len, "%s", abs_font);
            } else {
                snprintf(out_dir, max_len, "%s", fpath);
            }
            return true;
        }
    }
    return false;
}
