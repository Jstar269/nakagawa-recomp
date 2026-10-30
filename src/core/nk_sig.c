/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_sig.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <string.h>

bool nk_ed25519_verify(const uint8_t sig[NK_ED25519_SIGNATURE_BYTES],
                       const uint8_t pub[NK_ED25519_PUBLIC_KEY_BYTES],
                       const uint8_t *msg, size_t len) {
    static const uint8_t empty[1] = {0};
    if (sig == NULL || pub == NULL || (msg == NULL && len != 0u)) {
        return false;
    }
    if (msg == NULL) {
        msg = empty;
    }
    /* crypto_ed25519_check returns 0 for a valid signature and -1 otherwise. */
    return crypto_ed25519_check(sig, pub, msg, len) == 0;
}

/* ---- minisign ---------------------------------------------------------------------- */

#define MINISIGN_UNTRUSTED_PREFIX "untrusted comment: "
#define MINISIGN_TRUSTED_PREFIX "trusted comment: "
/* minisign's TRUSTEDCOMMENTMAXBYTES (minisign.h): the "trusted comment: " line,
   with its prefix and line end, fits in 8192 bytes, so `minisign -S` refuses a
   longer comment and `minisign -V` cannot read one. */
#define MINISIGN_TRUSTED_LINE_MAX 8192u
#define MINISIGN_TRUSTED_COMMENT_MAX \
    (MINISIGN_TRUSTED_LINE_MAX - (sizeof(MINISIGN_TRUSTED_PREFIX) - 1u) - 2u)
/* Algorithm (2), key ID (8), Ed25519 public key or signature. */
#define MINISIGN_KEY_BYTES (2u + NK_MINISIGN_KEY_ID_BYTES + NK_ED25519_PUBLIC_KEY_BYTES)
#define MINISIGN_SIG_BYTES (2u + NK_MINISIGN_KEY_ID_BYTES + NK_ED25519_SIGNATURE_BYTES)

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Standard base64 of exactly out_size bytes: the padded length, '=' only where
   the padding goes, and zero bits in what the padding drops (the one encoding
   minisign writes), else false. */
static bool base64_decode_exact(const char *text, size_t length, uint8_t *out,
                                size_t out_size) {
    size_t groups = (out_size + 2u) / 3u;
    size_t pad = groups * 3u - out_size;
    size_t used = 0;
    if (out_size == 0u || length != groups * 4u) return false;
    for (size_t g = 0; g < groups; g++) {
        uint32_t bits = 0;
        bool last = g + 1u == groups;
        for (size_t k = 0; k < 4u; k++) {
            char c = text[g * 4u + k];
            int value;
            if (last && k >= 4u - pad) {
                if (c != '=') return false;
                value = 0;
            } else if ((value = base64_value(c)) < 0) {
                return false;
            }
            bits = (bits << 6) | (uint32_t)value;
        }
        for (size_t k = 0; k < 3u; k++) {
            uint8_t byte = (uint8_t)(bits >> (16u - 8u * k));
            if (used < out_size) {
                out[used++] = byte;
            } else if (byte != 0u) {
                return false;   /* a non-canonical tail */
            }
        }
    }
    return true;
}

typedef struct {
    const char *text;
    size_t length;
    size_t pos;
} LineReader;

/* The next line without its LF or CRLF. A NUL or a CR inside a line fails. */
static bool read_line(LineReader *reader, const char **line, size_t *line_length) {
    size_t start = reader->pos;
    size_t end = start;
    if (start >= reader->length) return false;
    while (end < reader->length && reader->text[end] != '\n') end++;
    reader->pos = end < reader->length ? end + 1u : end;
    if (end > start && reader->text[end - 1u] == '\r') end--;
    for (size_t i = start; i < end; i++) {
        if (reader->text[i] == '\r' || reader->text[i] == '\0') return false;
    }
    *line = reader->text + start;
    *line_length = end - start;
    return true;
}

/* Nothing but line ends is left. */
static bool at_end(const LineReader *reader) {
    for (size_t i = reader->pos; i < reader->length; i++) {
        if (reader->text[i] != '\r' && reader->text[i] != '\n') return false;
    }
    return true;
}

static bool starts_with(const char *line, size_t length, const char *prefix) {
    size_t n = strlen(prefix);
    return length >= n && memcmp(line, prefix, n) == 0;
}

bool nk_minisign_parse_key(const char *text, size_t length, NkMinisignKey *out) {
    LineReader reader;
    const char *line;
    size_t line_length;
    uint8_t raw[MINISIGN_KEY_BYTES];
    if (text == NULL || out == NULL) return false;
    reader.text = text;
    reader.length = length;
    reader.pos = 0;
    if (!read_line(&reader, &line, &line_length)) return false;
    if (starts_with(line, line_length, MINISIGN_UNTRUSTED_PREFIX) &&
        !read_line(&reader, &line, &line_length)) {
        return false;
    }
    /* A public key is always "Ed": the algorithm byte of a signature says
       whether the file was prehashed, not the key. */
    if (!base64_decode_exact(line, line_length, raw, sizeof(raw)) || memcmp(raw, "Ed", 2) != 0 ||
        !at_end(&reader)) {
        return false;
    }
    memcpy(out->key_id, raw + 2, NK_MINISIGN_KEY_ID_BYTES);
    memcpy(out->public_key, raw + 2 + NK_MINISIGN_KEY_ID_BYTES, NK_ED25519_PUBLIC_KEY_BYTES);
    return true;
}

NkMinisignResult nk_minisign_verify(const NkMinisignKey *key, const char *signature,
                                    size_t signature_length, const uint8_t *msg,
                                    size_t msg_length, const char **trusted_comment,
                                    size_t *trusted_comment_length) {
    static const uint8_t empty[1] = {0};
    /* The global signature's message: the file signature, then the comment. */
    uint8_t signed_comment[NK_ED25519_SIGNATURE_BYTES + MINISIGN_TRUSTED_COMMENT_MAX];
    uint8_t sig[MINISIGN_SIG_BYTES];
    uint8_t global[NK_ED25519_SIGNATURE_BYTES];
    uint8_t digest[64];
    const uint8_t *file_signature = sig + 2 + NK_MINISIGN_KEY_ID_BYTES;
    LineReader reader;
    const char *line;
    const char *comment;
    size_t line_length;
    size_t comment_length;
    bool prehashed;
    bool ok;
    if (trusted_comment) *trusted_comment = NULL;
    if (trusted_comment_length) *trusted_comment_length = 0;
    if (key == NULL || signature == NULL || (msg == NULL && msg_length != 0u)) {
        return NK_MINISIGN_MALFORMED;
    }
    if (msg == NULL) msg = empty;
    reader.text = signature;
    reader.length = signature_length;
    reader.pos = 0;
    if (!read_line(&reader, &line, &line_length) ||
        !starts_with(line, line_length, MINISIGN_UNTRUSTED_PREFIX) ||
        !read_line(&reader, &line, &line_length) ||
        !base64_decode_exact(line, line_length, sig, sizeof(sig)) ||
        !read_line(&reader, &line, &line_length) ||
        !starts_with(line, line_length, MINISIGN_TRUSTED_PREFIX)) {
        return NK_MINISIGN_MALFORMED;
    }
    comment = line + (sizeof(MINISIGN_TRUSTED_PREFIX) - 1u);
    comment_length = line_length - (sizeof(MINISIGN_TRUSTED_PREFIX) - 1u);
    if (comment_length > MINISIGN_TRUSTED_COMMENT_MAX ||
        !read_line(&reader, &line, &line_length) ||
        !base64_decode_exact(line, line_length, global, sizeof(global)) || !at_end(&reader)) {
        return NK_MINISIGN_MALFORMED;
    }
    if (memcmp(sig, "ED", 2) == 0) {
        prehashed = true;
    } else if (memcmp(sig, "Ed", 2) == 0) {
        prehashed = false;
    } else {
        return NK_MINISIGN_MALFORMED;
    }
    if (memcmp(sig + 2, key->key_id, NK_MINISIGN_KEY_ID_BYTES) != 0) {
        return NK_MINISIGN_UNKNOWN_KEY;
    }
    if (prehashed) {
        /* minisign's crypto_generichash with the largest output: unkeyed BLAKE2b-512. */
        crypto_blake2b(digest, sizeof(digest), msg, msg_length);
        ok = nk_ed25519_verify(file_signature, key->public_key, digest, sizeof(digest));
    } else {
        ok = nk_ed25519_verify(file_signature, key->public_key, msg, msg_length);
    }
    if (!ok) return NK_MINISIGN_BAD_SIGNATURE;
    memcpy(signed_comment, file_signature, NK_ED25519_SIGNATURE_BYTES);
    memcpy(signed_comment + NK_ED25519_SIGNATURE_BYTES, comment, comment_length);
    if (!nk_ed25519_verify(global, key->public_key, signed_comment,
                           NK_ED25519_SIGNATURE_BYTES + comment_length)) {
        return NK_MINISIGN_BAD_COMMENT;
    }
    if (trusted_comment) *trusted_comment = comment;
    if (trusted_comment_length) *trusted_comment_length = comment_length;
    return NK_MINISIGN_VALID;
}
