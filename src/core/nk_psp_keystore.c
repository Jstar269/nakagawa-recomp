/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Local-only KeyStore loader/validator for the PSP decryption boundary
 * (issue #295).  Project-authored from the entry schema documented in
 * docs/SETUP.md; parses with the project's own bounded JSON reader
 * (nk_json) and fails closed on any shape, name or length violation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_json.h"
#include "nk_psp_crypto.h"
#include "nk_psp_keystore.h"

typedef struct {
    char name[72];
    uint8_t bytes[144];
    size_t len;
} FlatEntry;

typedef struct {
    uint32_t tag;
    int have_code;
    int code;
    int have_key;
    uint8_t key[16];
    int have_key144;
    uint8_t key144[144];
    int have_seed;
    uint8_t seed[16];
    int have_xorpad;
    uint8_t xorpad[16];
} TagEntry;

struct NkKeystore {
    FlatEntry *flat;
    size_t flat_count;
    TagEntry *tags;
    size_t tag_count;
};

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_hex(const char *text, uint8_t *out, size_t out_len,
                      char *err, size_t err_len)
{
    size_t n = strlen(text);
    if (n != out_len * 2) {
        snprintf(err, err_len,
                 "value length must be %zu bytes (%zu hex characters), got %zu characters",
                 out_len, out_len * 2, n);
        return -1;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(text[i * 2]);
        int lo = hex_nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            snprintf(err, err_len, "value must be hexadecimal");
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* Expected flat-entry byte length, -1 when the name is not a known entry. */
static int flat_entry_length(const char *name)
{
    static const struct { const char *name; int len; } exact[] = {
        {"kirk.cmd1.key", 16},
        {"kirk.ecdsa.p", 20},
        {"kirk.ecdsa.a", 20},
        {"kirk.ecdsa.b1", 20},
        {"kirk.ecdsa.b2", 20},
        {"kirk.ecdsa.n1", 21},
        {"kirk.ecdsa.n2", 21},
        {"kirk.ecdsa.gx1", 20},
        {"kirk.ecdsa.gy1", 20},
        {"kirk.ecdsa.gx2", 20},
        {"kirk.ecdsa.gy2", 20},
        {"kirk.ecdsa.px1", 20},
        {"kirk.ecdsa.py1", 20},
        {"psar.k1", 16},
        {"psar.k2", 16},
    };
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        if (strcmp(name, exact[i].name) == 0) return exact[i].len;
    }
    if (strncmp(name, "kirk.keyvault.", 14) == 0) {
        const char *p = name + 14;
        if (*p == '\0') return -1;
        long v = 0;
        for (; *p; p++) {
            if (*p < '0' || *p > '9') return -1;
            v = v * 10 + (*p - '0');
            if (v > 127) return -1;
        }
        return 16;
    }
    if (strncmp(name, "psar.list_keys.", 15) == 0) {
        const char *p = name + 15;
        if (p[0] < '0' || p[0] > '5' || p[1] != '\0') return -1;
        return 16;
    }
    if (strncmp(name, "prx.tag.", 8) == 0) return -2; /* structured entry */
    return -1;
}

static int parse_tag_name(const char *name, uint32_t *tag)
{
    if (strncmp(name, "prx.tag.0x", 10) != 0) return -1;
    const char *p = name + 10;
    if (strlen(p) != 8) return -1;
    uint32_t v = 0;
    for (; *p; p++) {
        int nib = hex_nibble(*p);
        if (nib < 0) return -1;
        v = (v << 4) | (uint32_t)nib;
    }
    *tag = v;
    return 0;
}

static int load_hex_entry(NkKeystore *ks, const char *name,
                          const NkJsonNode *value, char *err, size_t err_len)
{
    int want = flat_entry_length(name);
    if (want == -2) return 1; /* structured; handled by caller */
    if (want < 0) {
        snprintf(err, err_len, "unknown key entry name '%s'", name);
        return -1;
    }
    if (!nk_json_is_string(value)) {
        snprintf(err, err_len, "entry '%s': value must be a hex string", name);
        return -1;
    }
    if (ks->flat_count >= NK_KEYSTORE_MAX_ENTRIES) {
        snprintf(err, err_len, "too many entries (max %d)", NK_KEYSTORE_MAX_ENTRIES);
        return -1;
    }
    const char *text = nk_json_get_string(value);
    char detail[128];
    FlatEntry *slot = &ks->flat[ks->flat_count];
    if (decode_hex(text, slot->bytes, (size_t)want, detail, sizeof(detail)) != 0) {
        snprintf(err, err_len, "entry '%s': %s", name, detail);
        return -1;
    }
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->len = (size_t)want;
    ks->flat_count++;
    return 0;
}

static int load_tag_field(const char *name, const char *field,
                          const NkJsonNode *value, uint8_t *out, int *have,
                          size_t want, char *err, size_t err_len)
{
    if (!nk_json_is_string(value)) {
        snprintf(err, err_len, "entry '%s': field '%s' must be a hex string",
                 name, field);
        return -1;
    }
    char detail[128];
    if (decode_hex(nk_json_get_string(value), out, want,
                   detail, sizeof(detail)) != 0) {
        snprintf(err, err_len, "entry '%s': field '%s': %s", name, field, detail);
        return -1;
    }
    *have = 1;
    return 0;
}

static int load_tag_entry(NkKeystore *ks, const char *name,
                          const NkJsonNode *value, char *err, size_t err_len)
{
    uint32_t tag = 0;
    if (parse_tag_name(name, &tag) != 0) {
        snprintf(err, err_len,
                 "entry '%s': tag names must look like prx.tag.0xXXXXXXXX "
                 "with eight hexadecimal digits", name);
        return -1;
    }
    if (!nk_json_is_object(value)) {
        snprintf(err, err_len,
                 "entry '%s': recipe must be an object with a \"code\" and "
                 "one of \"key\" or \"key144\"", name);
        return -1;
    }
    if (ks->tag_count >= NK_KEYSTORE_MAX_ENTRIES) {
        snprintf(err, err_len, "too many entries (max %d)", NK_KEYSTORE_MAX_ENTRIES);
        return -1;
    }
    for (size_t i = 0; i < ks->tag_count; i++) {
        if (ks->tags[i].tag == tag) {
            snprintf(err, err_len, "duplicate entry '%s'", name);
            return -1;
        }
    }
    TagEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.tag = tag;

    NkJsonNode *code = nk_json_obj_get(value, "code");
    if (code == NULL || !nk_json_is_number(code)) {
        snprintf(err, err_len, "entry '%s': recipe needs integer field \"code\" (0..127)",
                 name);
        return -1;
    }
    int64_t code_val = 0;
    if (!nk_json_get_int64(code, &code_val) || code_val < 0 || code_val > 127) {
        snprintf(err, err_len, "entry '%s': \"code\" must be an integer in 0..127", name);
        return -1;
    }
    entry.have_code = 1;
    entry.code = (int)code_val;

    static const struct { const char *field; uint8_t *dst; int *have; size_t len; }
    fields[] = {
        {"key", NULL, NULL, 16},
        {"key144", NULL, NULL, 144},
        {"seed", NULL, NULL, 16},
        {"xor", NULL, NULL, 16},
    };
    const NkJsonNode *members[4];
    for (size_t i = 0; i < 4; i++) members[i] = nk_json_obj_get(value, fields[i].field);

    /* Reject unknown fields (fail closed on schema drift). */
    for (size_t i = 0; i < value->u.obj.count; i++) {
        const char *key = value->u.obj.members[i].key;
        int known = strcmp(key, "code") == 0;
        for (size_t j = 0; j < 4 && !known; j++) known = strcmp(key, fields[j].field) == 0;
        if (!known) {
            snprintf(err, err_len, "entry '%s': unknown recipe field '%s'", name, key);
            return -1;
        }
    }
    if (members[0] != NULL &&
        load_tag_field(name, "key", members[0], entry.key,
                       &entry.have_key, 16, err, err_len) != 0) return -1;
    if (members[1] != NULL &&
        load_tag_field(name, "key144", members[1], entry.key144,
                       &entry.have_key144, 144, err, err_len) != 0) return -1;
    if (members[2] != NULL &&
        load_tag_field(name, "seed", members[2], entry.seed,
                       &entry.have_seed, 16, err, err_len) != 0) return -1;
    if (members[3] != NULL &&
        load_tag_field(name, "xor", members[3], entry.xorpad,
                       &entry.have_xorpad, 16, err, err_len) != 0) return -1;
    if (!entry.have_key && !entry.have_key144) {
        snprintf(err, err_len,
                 "entry '%s': recipe needs one of \"key\" (16 bytes) or "
                 "\"key144\" (144 bytes)", name);
        return -1;
    }

    ks->tags[ks->tag_count++] = entry;
    return 0;
}

int nk_keystore_load_json(NkKeystore *ks, const char *text, size_t len,
                          char *err, size_t err_len)
{
    if (err_len) err[0] = '\0';
    if (ks == NULL) return NK_PSP_ERR_KEYFILE;
    if (len > NK_KEYSTORE_MAX_BYTES) {
        snprintf(err, err_len, "key file exceeds %u bytes", NK_KEYSTORE_MAX_BYTES);
        return NK_PSP_ERR_KEYFILE;
    }
    char json_err[192];
    NkJsonNode *root = nk_json_parse(text, len, json_err, sizeof(json_err));
    if (root == NULL) {
        snprintf(err, err_len, "invalid JSON: %s", json_err);
        return NK_PSP_ERR_KEYFILE;
    }
    int status = NK_PSP_OK;
    if (!nk_json_is_object(root)) {
        snprintf(err, err_len, "top level must be a JSON object");
        status = NK_PSP_ERR_KEYFILE;
        goto done;
    }
    NkJsonNode *format = nk_json_obj_get(root, "format");
    if (format == NULL || !nk_json_is_string(format) ||
        strcmp(nk_json_get_string(format), NK_KEYSTORE_FORMAT) != 0) {
        snprintf(err, err_len,
                 "missing or unsupported \"format\" (expected \"%s\")",
                 NK_KEYSTORE_FORMAT);
        status = NK_PSP_ERR_KEYFILE;
        goto done;
    }
    NkJsonNode *entries = nk_json_obj_get(root, "entries");
    if (entries == NULL || !nk_json_is_object(entries)) {
        snprintf(err, err_len, "missing \"entries\" object");
        status = NK_PSP_ERR_KEYFILE;
        goto done;
    }
    for (size_t i = 0; i < entries->u.obj.count; i++) {
        const char *name = entries->u.obj.members[i].key;
        if (strlen(name) >= sizeof(ks->flat[0].name)) {
            snprintf(err, err_len, "entry name too long");
            status = NK_PSP_ERR_KEYFILE;
            goto done;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(entries->u.obj.members[j].key, name) == 0) {
                snprintf(err, err_len, "duplicate entry '%s'", name);
                status = NK_PSP_ERR_KEYFILE;
                goto done;
            }
        }
        char detail[192];
        int rc = load_hex_entry(ks, name, entries->u.obj.members[i].val,
                                detail, sizeof(detail));
        if (rc < 0) {
            snprintf(err, err_len, "%s", detail);
            status = NK_PSP_ERR_KEYFILE;
            goto done;
        }
        if (rc > 0) {
            if (load_tag_entry(ks, name, entries->u.obj.members[i].val,
                               detail, sizeof(detail)) != 0) {
                snprintf(err, err_len, "%s", detail);
                status = NK_PSP_ERR_KEYFILE;
                goto done;
            }
        }
    }
done:
    nk_json_free(root);
    return status;
}

int nk_keystore_load_file(NkKeystore *ks, const char *path,
                          char *err, size_t err_len)
{
    if (err_len) err[0] = '\0';
    if (ks == NULL || path == NULL) return NK_PSP_ERR_KEYFILE;
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        snprintf(err, err_len, "no key file at %s", path);
        return NK_PSP_ERR_KEYFILE;
    }
    char *buffer = (char *)malloc(NK_KEYSTORE_MAX_BYTES + 1);
    if (buffer == NULL) {
        fclose(stream);
        snprintf(err, err_len, "out of memory reading key file");
        return NK_PSP_ERR_KEYFILE;
    }
    size_t got = fread(buffer, 1, NK_KEYSTORE_MAX_BYTES + 1, stream);
    int overflow = got > NK_KEYSTORE_MAX_BYTES;
    fclose(stream);
    int status = NK_PSP_OK;
    if (overflow) {
        snprintf(err, err_len, "key file exceeds %u bytes", NK_KEYSTORE_MAX_BYTES);
        status = NK_PSP_ERR_KEYFILE;
    } else {
        buffer[got] = '\0';
        char detail[192];
        status = nk_keystore_load_json(ks, buffer, got, detail, sizeof(detail));
        if (status != NK_PSP_OK) {
            snprintf(err, err_len, "key file %s: %s", path, detail);
        }
    }
    free(buffer);
    return status;
}

int nk_keystore_get(const NkKeystore *ks, const char *name,
                    const uint8_t **out, size_t *out_len)
{
    if (ks == NULL || name == NULL) return -1;
    for (size_t i = 0; i < ks->flat_count; i++) {
        if (strcmp(ks->flat[i].name, name) == 0) {
            if (out) *out = ks->flat[i].bytes;
            if (out_len) *out_len = ks->flat[i].len;
            return 0;
        }
    }
    return -1;
}

int nk_keystore_get_prx_tag(const NkKeystore *ks, uint32_t tag,
                            NkPrxTagEntry *out)
{
    if (ks == NULL || out == NULL) return -1;
    for (size_t i = 0; i < ks->tag_count; i++) {
        const TagEntry *e = &ks->tags[i];
        if (e->tag != tag) continue;
        memset(out, 0, sizeof(*out));
        out->have_code = e->have_code;
        out->code = e->code;
        if (e->have_key) { out->key = e->key; }
        if (e->have_key144) { out->key144 = e->key144; }
        if (e->have_seed) { out->seed = e->seed; }
        if (e->have_xorpad) { out->xorpad = e->xorpad; }
        return 0;
    }
    return -1;
}

size_t nk_keystore_count(const NkKeystore *ks)
{
    return ks ? ks->flat_count + ks->tag_count : 0;
}

NkKeystore *nk_keystore_create(void)
{
    NkKeystore *ks = (NkKeystore *)calloc(1, sizeof(*ks));
    if (ks == NULL) return NULL;
    ks->flat = (FlatEntry *)calloc(NK_KEYSTORE_MAX_ENTRIES, sizeof(FlatEntry));
    ks->tags = (TagEntry *)calloc(NK_KEYSTORE_MAX_ENTRIES, sizeof(TagEntry));
    if (ks->flat == NULL || ks->tags == NULL) {
        nk_keystore_free(ks);
        return NULL;
    }
    return ks;
}

void nk_keystore_free(NkKeystore *ks)
{
    if (ks == NULL) return;
    free(ks->flat);
    free(ks->tags);
    free(ks);
}
