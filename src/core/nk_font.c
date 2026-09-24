/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_font.h"
#include "nk_types.h"
#include "nk_platform.h"
#include "nk_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static uint16_t font_read_le16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* -----------------------------------------------------------------------------
 * PGF Structural Validation
 * -------------------------------------------------------------------------- */
bool nk_font_validate_pgf(const char *file_path, uint64_t *out_size,
                          char *out_sha256, char *out_error, size_t error_len) {
    if (!file_path || !*file_path) {
        if (out_error && error_len > 0) snprintf(out_error, error_len, "Missing font file path.");
        return false;
    }
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        if (out_error && error_len > 0) snprintf(out_error, error_len, "Cannot open font file '%s'.", file_path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (out_error && error_len > 0) snprintf(out_error, error_len, "Cannot seek font file '%s'.", file_path);
        return false;
    }
    int64_t sz = nk_ftell64(f);
    if (sz < 0) {
        fclose(f);
        if (out_error && error_len > 0) snprintf(out_error, error_len, "Cannot determine font file size for '%s'.", file_path);
        return false;
    }
    uint64_t file_size = (uint64_t)sz;
    if (out_size) *out_size = file_size;
    if (file_size < 8) {
        fclose(f);
        if (out_error && error_len > 0) {
            snprintf(out_error, error_len, "Font file '%s' is too small (%llu bytes; minimum header is 8 bytes).",
                     file_path, (unsigned long long)file_size);
        }
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        if (out_error && error_len > 0) snprintf(out_error, error_len, "Cannot rewind font file '%s'.", file_path);
        return false;
    }
    uint8_t header[392];
    size_t to_read = sizeof(header) < file_size ? sizeof(header) : (size_t)file_size;
    size_t read_bytes = fread(header, 1, to_read, f);
    if (read_bytes < 8) {
        fclose(f);
        if (out_error && error_len > 0) snprintf(out_error, error_len, "Short read on font file '%s'.", file_path);
        return false;
    }

    uint16_t header_offset = font_read_le16(header);
    uint16_t header_size = font_read_le16(header + 2);
    if (header_size < 8) {
        fclose(f);
        if (out_error && error_len > 0) {
            snprintf(out_error, error_len, "Font file '%s' declares invalid header size (%u bytes).",
                     file_path, (unsigned)header_size);
        }
        return false;
    }
    if ((uint64_t)header_offset + (uint64_t)header_size > file_size) {
        fclose(f);
        if (out_error && error_len > 0) {
            snprintf(out_error, error_len, "Font file '%s' is truncated: declared header end (%u bytes) exceeds file size (%llu bytes).",
                     file_path, (unsigned)(header_offset + header_size), (unsigned long long)file_size);
        }
        return false;
    }
    /* The magic sits at header_offset + 4 (two u16 header fields precede it). A header that
     * places it beyond the bytes read cannot be validated, so it fails closed. */
    if ((size_t)header_offset + 8u > read_bytes ||
        memcmp(header + header_offset + 4, "PGF0", 4) != 0) {
        fclose(f);
        if (out_error && error_len > 0) {
            snprintf(out_error, error_len, "Font file '%s' has invalid PGF magic; expected 'PGF0'.", file_path);
        }
        return false;
    }
    if (header_size >= 186 && (size_t)header_offset + 186u <= read_bytes) {
        uint16_t first_glyph = font_read_le16(header + header_offset + 182);
        uint16_t last_glyph = font_read_le16(header + header_offset + 184);
        if (first_glyph > last_glyph) {
            fclose(f);
            if (out_error && error_len > 0) {
                snprintf(out_error, error_len, "Font file '%s' has corrupt glyph indices: first glyph (%u) exceeds last glyph (%u).",
                         file_path, (unsigned)first_glyph, (unsigned)last_glyph);
            }
            return false;
        }
    }

    /* Compute SHA-256 over entire file */
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    FontSha256 sha;
    font_sha256_init(&sha);
    uint8_t buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        font_sha256_update(&sha, buffer, n);
    }
    fclose(f);
    uint8_t digest[32];
    font_sha256_final(&sha, digest);
    if (out_sha256) {
        for (size_t i = 0; i < 32; i++) {
            snprintf(out_sha256 + i * 2, 3, "%02x", digest[i]);
        }
        out_sha256[64] = '\0';
    }
    return true;
}

/* -----------------------------------------------------------------------------
 * Font Cache Path Resolution & Manifest Inspection
 * -------------------------------------------------------------------------- */
bool nk_font_get_cache_dir(const char *user_data_root, char *out_dir, size_t max_len) {
    if (!out_dir || max_len == 0) return false;
    char base[NK_MAX_PATH];
    if (user_data_root && *user_data_root) {
        snprintf(base, sizeof(base), "%s", user_data_root);
    } else {
        if (!nk_platform_get_path(NK_PATH_DATA, base, sizeof(base))) return false;
    }
    char sep = nk_platform_path_separator();
    int written = snprintf(out_dir, max_len, "%s%cfonts%cv1", base, sep, sep);
    return written > 0 && (size_t)written < max_len;
}

NkFontStatus nk_font_check_cache(const char *user_data_root,
                                 const char *fallback_root,
                                 char *out_message,
                                 size_t message_max_len) {
    char cache_dir[NK_MAX_PATH];
    if (!nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        if (out_message && message_max_len > 0) {
            snprintf(out_message, message_max_len, "PSP font jpn0.pgf missing; run fonts import <folder> (#300).");
        }
        return NK_FONT_STATUS_MISSING;
    }
    char sep = nk_platform_path_separator();
    char manifest_path[NK_MAX_PATH * 2];
    snprintf(manifest_path, sizeof(manifest_path), "%s%cmanifest.json", cache_dir, sep);
    if (!nk_platform_file_exists(manifest_path)) {
        /* Check fallback path: <fallback_root>/font/jpn0.pgf */
        if (fallback_root && *fallback_root) {
            char fallback_font[NK_MAX_PATH * 2];
            snprintf(fallback_font, sizeof(fallback_font), "%s%cfont%cjpn0.pgf", fallback_root, sep, sep);
            if (nk_platform_file_exists(fallback_font)) {
                if (out_message && message_max_len > 0) {
                    snprintf(out_message, message_max_len, "User-supplied PSP system font jpn0.pgf is available.");
                }
                return NK_FONT_STATUS_OK;
            }
        }
        if (out_message && message_max_len > 0) {
            snprintf(out_message, message_max_len, "PSP font jpn0.pgf missing; run fonts import <folder> (#300).");
        }
        return NK_FONT_STATUS_MISSING;
    }

    /* Manifest exists: read and validate it */
    FILE *mf = fopen(manifest_path, "rb");
    if (!mf) {
        if (out_message && message_max_len > 0) {
            snprintf(out_message, message_max_len, "PSP font cache manifest unreadable; run fonts import <folder> (#300).");
        }
        return NK_FONT_STATUS_INVALID;
    }
    fseek(mf, 0, SEEK_END);
    long mlen = ftell(mf);
    if (mlen <= 0 || mlen > 1024 * 1024) {
        fclose(mf);
        if (out_message && message_max_len > 0) {
            snprintf(out_message, message_max_len, "PSP font cache manifest size invalid; run fonts import <folder> (#300).");
        }
        return NK_FONT_STATUS_INVALID;
    }
    fseek(mf, 0, SEEK_SET);
    char *json_buf = (char *)malloc((size_t)mlen + 1);
    if (!json_buf) {
        fclose(mf);
        return NK_FONT_STATUS_INVALID;
    }
    size_t read_bytes = fread(json_buf, 1, (size_t)mlen, mf);
    fclose(mf);
    json_buf[read_bytes] = '\0';

    char parse_err[128] = "";
    NkJsonNode *root = nk_json_parse(json_buf, read_bytes, parse_err, sizeof(parse_err));
    free(json_buf);
    if (!root || !nk_json_is_object(root)) {
        if (root) nk_json_free(root);
        if (out_message && message_max_len > 0) {
            snprintf(out_message, message_max_len, "PSP font cache manifest is malformed JSON; run fonts import <folder> (#300).");
        }
        return NK_FONT_STATUS_INVALID;
    }

    NkJsonNode *ver_node = nk_json_obj_get(root, "schema_version");
    int64_t schema_ver = 0;
    if (!ver_node || !nk_json_get_int64(ver_node, &schema_ver) || schema_ver != 1) {
        nk_json_free(root);
        if (out_message && message_max_len > 0) {
            snprintf(out_message, message_max_len, "PSP font cache manifest schema version unsupported; run fonts import <folder> (#300).");
        }
        return NK_FONT_STATUS_INVALID;
    }

    NkJsonNode *files_node = nk_json_obj_get(root, "files");
    if (!files_node || !nk_json_is_object(files_node)) {
        nk_json_free(root);
        if (out_message && message_max_len > 0) {
            snprintf(out_message, message_max_len, "PSP font cache manifest missing 'files' object; run fonts import <folder> (#300).");
        }
        return NK_FONT_STATUS_INVALID;
    }

    NkJsonNode *jpn_entry = nk_json_obj_get(files_node, "jpn0.pgf");
    if (!jpn_entry) {
        nk_json_free(root);
        if (out_message && message_max_len > 0) {
            snprintf(out_message, message_max_len, "PSP font cache manifest does not declare jpn0.pgf; run fonts import <folder> (#300).");
        }
        return NK_FONT_STATUS_INVALID;
    }

    /* Check all files declared in manifest */
    size_t member_count = files_node->u.obj.count;
    for (size_t i = 0; i < member_count; i++) {
        const char *fname = files_node->u.obj.members[i].key;
        NkJsonNode *finfo = files_node->u.obj.members[i].val;
        if (!fname || !finfo || !nk_json_is_object(finfo)) {
            nk_json_free(root);
            if (out_message && message_max_len > 0) {
                snprintf(out_message, message_max_len, "PSP font cache manifest entry is invalid; run fonts import <folder> (#300).");
            }
            return NK_FONT_STATUS_INVALID;
        }
        NkJsonNode *sz_node = nk_json_obj_get(finfo, "size");
        NkJsonNode *sha_node = nk_json_obj_get(finfo, "sha256");
        int64_t exp_size = 0;
        const char *exp_sha = nk_json_get_string(sha_node);
        if (!sz_node || !nk_json_get_int64(sz_node, &exp_size) || !exp_sha || strlen(exp_sha) != 64) {
            nk_json_free(root);
            if (out_message && message_max_len > 0) {
                snprintf(out_message, message_max_len, "PSP font cache manifest entry '%s' missing size or sha256; run fonts import <folder> (#300).", fname);
            }
            return NK_FONT_STATUS_INVALID;
        }
        char file_path[NK_MAX_PATH * 2];
        snprintf(file_path, sizeof(file_path), "%s%c%s", cache_dir, sep, fname);
        uint64_t act_size = 0;
        char act_sha[65] = "";
        char val_err[256] = "";
        if (!nk_font_validate_pgf(file_path, &act_size, act_sha, val_err, sizeof(val_err))) {
            nk_json_free(root);
            if (out_message && message_max_len > 0) {
                snprintf(out_message, message_max_len, "PSP font file '%s' invalid in cache (%s); run fonts import <folder> (#300).",
                         fname, val_err[0] ? val_err : "validation failed");
            }
            return NK_FONT_STATUS_INVALID;
        }
        if ((int64_t)act_size != exp_size || strcmp(act_sha, exp_sha) != 0) {
            nk_json_free(root);
            if (out_message && message_max_len > 0) {
                snprintf(out_message, message_max_len, "PSP font file '%s' checksum/size mismatch in cache; run fonts import <folder> (#300).", fname);
            }
            return NK_FONT_STATUS_INVALID;
        }
    }

    nk_json_free(root);
    if (out_message && message_max_len > 0) {
        snprintf(out_message, message_max_len, "User-supplied PSP system font jpn0.pgf is available.");
    }
    return NK_FONT_STATUS_OK;
}

bool nk_font_resolve_directory(const char *user_data_root,
                               const char *fallback_root,
                               char *out_dir,
                               size_t max_len) {
    if (!out_dir || max_len == 0) return false;
    char msg[256];
    NkFontStatus status = nk_font_check_cache(user_data_root, fallback_root, msg, sizeof(msg));
    if (status == NK_FONT_STATUS_OK) {
        char cache_dir[NK_MAX_PATH];
        char sep = nk_platform_path_separator();
        if (nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
            char mpath[NK_MAX_PATH * 2];
            snprintf(mpath, sizeof(mpath), "%s%cmanifest.json", cache_dir, sep);
            if (nk_platform_file_exists(mpath)) {
                snprintf(out_dir, max_len, "%s", cache_dir);
                return true;
            }
        }
        if (fallback_root && *fallback_root) {
            char fpath[NK_MAX_PATH * 2];
            snprintf(fpath, sizeof(fpath), "%s%cfont", fallback_root, sep);
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
    }
    return false;
}
