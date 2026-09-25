/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* lstat is POSIX: without this, -std=c99/c11 on glibc leaves it undeclared. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "nk_title_manifest.h"
#include "nk_iso.h"
#include "nk_json.h"
#include "nk_platform.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

/* ASCII-only case-insensitive compare.
 *
 * strcasecmp is POSIX and _stricmp is Win32, so every call site needed its own
 * #if -- and under -std=c99 on Linux strcasecmp is not declared at all, which
 * left the native build emitting an implicit-declaration warning there while
 * staying clean on Windows. Both are also locale-sensitive: in a Turkish
 * locale 'I' and 'i' do not fold onto each other, so whether two manifest
 * module names counted as duplicates depended on the host's locale. Manifest
 * identifiers are ASCII by schema, so fold them as ASCII and keep the answer
 * the same on every host. */
static int nk_ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int nk_ascii_casecmp(const char *a, const char *b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        int ca = nk_ascii_lower((unsigned char)*a);
        int cb = nk_ascii_lower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return nk_ascii_lower((unsigned char)*a) - nk_ascii_lower((unsigned char)*b);
}

#define MAX_MODULES 32
#define MAX_COMPAT_DISC_IDS 16
#define NK_MANIFEST_MAX_OVERLAYS 8

/* The window a guest module's base must lie in: its code and data are both placed there, so a
 * base outside it lets translated code run while every data write is dropped. Mirrors
 * GUEST_MODULE_RAM_LO/HI in tools/title_manifest.py (64 MB models extend user RAM; the runtime
 * arena ends where this window ends). */
#define GUEST_MODULE_RAM_LO 0x08800000u
#define GUEST_MODULE_RAM_HI 0x0C000000u

#define SR_DISPATCH_VFPU_TAG  0x40000000U
#define SR_DISPATCH_VFPU_MASK 0xFC000000U

/* Bounded storage for in-memory registered overlays */
typedef struct {
    char id[65];
    char game_name[65];
    char display_name[129];
    NkTitleKind kind;
    char primary_disc_id[17];
    char compat_ids_storage[MAX_COMPAT_DISC_IDS][17];
    const char *compat_id_ptrs[MAX_COMPAT_DISC_IDS + 1];
    uint32_t executable_base;
    uint32_t executable_entry;
    char bss_metadata_source[16];
    char data_root[257];
    char memory_stick_root[129];
    char hle_profile[65];
    char codegen_profile[65];
    uint32_t expected_data_file_count;
    bool requires_font_firmware;
    bool requires_psmf;
    NkModuleDefinition modules[MAX_MODULES];
    char module_names[MAX_MODULES][129];
    int module_count;
    NkTitleEntry entry;
} OverlayStorageSlot;

static OverlayStorageSlot s_overlay_slots[NK_MANIFEST_MAX_OVERLAYS];
static int s_overlay_slot_count = 0;

/* Installed into the catalog the first time an overlay is stored, so that
   nk_title_catalog_clear_overlay releases this storage as well as its own
   pointer registry. Without it a manifest reusing a cleared overlay's disc ID
   was still refused as colliding with an overlay the caller had cleared, and
   the fixed slot capacity stayed consumed for the life of the process. */
static void nk_manifest_reset_overlay_storage(void) {
    memset(s_overlay_slots, 0, sizeof(s_overlay_slots));
    s_overlay_slot_count = 0;
}

static FILE *manifest_fopen(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    if (!path || !*path) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wlen <= 0 || wlen > 32768) return NULL;
    WCHAR wpath[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, wlen) <= 0) return NULL;
    return _wfopen(wpath, L"rb");
#else
    if (!path || !*path) return NULL;
    return fopen(path, "rb");
#endif
}

/* -----------------------------------------------------------------------------
 * UTF-8 Validator
 * -------------------------------------------------------------------------- */
static inline bool validate_utf8(const uint8_t *s, size_t len) {
    return nk_json_validate_utf8(s, len);
}



/* -----------------------------------------------------------------------------
 * JSON Compatibility Adapters (shared parser in nk_json.h/.c)
 * -------------------------------------------------------------------------- */
static inline void json_free(JsonNode *node) {
    nk_json_free(node);
}

static inline JsonNode *json_parse(const char *src, size_t len, char *err_buf, size_t err_len) {
    return nk_json_parse(src, len, err_buf, err_len);
}

static inline JsonNode *obj_get(const JsonNode *obj, const char *key) {
    return nk_json_obj_get(obj, key);
}

static bool is_valid_identifier(const char *s) {
    if (!s || !*s) return false;
    size_t len = strlen(s);
    if (len < 1 || len > 64) return false;
    char c0 = s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9'))) return false;
    for (size_t i = 1; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* Keep the native fallback identical to the generic manager's documented
 * derivation. Explicit manifest game_name values are copied by the caller;
 * this helper only handles older manifests that rely on the version suffix
 * convention. */
static void derive_game_name(const char *id, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!id || !*id) return;

    snprintf(out, out_size, "%s", id);
    size_t len = strlen(out);
    size_t suffix_start = len;
    while (suffix_start > 0) {
        char c = out[suffix_start - 1];
        if (c < '0' || c > '9') break;
        suffix_start--;
    }
    if (suffix_start < len && suffix_start >= 2 &&
        out[suffix_start - 2] == '-' && out[suffix_start - 1] == 'v') {
        out[suffix_start - 2] = '\0';
    }
}

static bool is_valid_disc_id(const char *s) {
    if (!s || strlen(s) != 9) return false;
    for (int i = 0; i < 4; i++) {
        if (s[i] < 'A' || s[i] > 'Z') return false;
    }
    for (int i = 4; i < 9; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static bool is_valid_filename(const char *s) {
    if (!s || !*s) return false;
    size_t len = strlen(s);
    if (len < 1 || len > 128) return false;
    char c0 = s[0];
    if (!isalnum((unsigned char)c0)) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
    }
    if (s[len - 1] == '.') return false;

    /* Check Windows reserved names */
    static const char * const reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", NULL
    };
    char base[16];
    size_t blen = 0;
    while (blen < sizeof(base) - 1 && s[blen] && s[blen] != '.') {
        base[blen] = (char)toupper((unsigned char)s[blen]);
        blen++;
    }
    base[blen] = '\0';
    for (int r = 0; reserved[r]; r++) {
        if (strcmp(base, reserved[r]) == 0) return false;
    }
    return true;
}

static bool is_valid_portable_path(const char *s) {
    if (!s || !*s) return false;
    size_t len = strlen(s);
    if (len > 240) return false;
    if (s[0] == '/' || s[0] == '\\' || strchr(s, '\\') != NULL || strchr(s, ':') != NULL) return false;
    /* strtok() treats repeated and trailing separators as delimiters rather
       than empty path components. Reject them before tokenization so the
       native parser agrees with the schema parser and never normalizes an
       ambiguous path into a different path. */
    if (s[len - 1] == '/' || strstr(s, "//") != NULL) return false;

    char temp[256];
    snprintf(temp, sizeof(temp), "%s", s);
    char *token = strtok(temp, "/");
    while (token) {
        if (strcmp(token, "") == 0 || strcmp(token, ".") == 0 || strcmp(token, "..") == 0) return false;
        size_t tlen = strlen(token);
        if (token[tlen - 1] == '.' || token[tlen - 1] == ' ') return false;
        for (size_t i = 0; i < tlen; i++) {
            char c = token[i];
            if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
        }
        /* Check Windows reserved */
        char base[16];
        size_t blen = 0;
        while (blen < sizeof(base) - 1 && token[blen] && token[blen] != '.') {
            base[blen] = (char)toupper((unsigned char)token[blen]);
            blen++;
        }
        base[blen] = '\0';
        static const char * const reserved[] = {
            "CON", "PRN", "AUX", "NUL",
            "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", NULL
        };
        for (int r = 0; reserved[r]; r++) {
            if (strcmp(base, reserved[r]) == 0) return false;
        }
        token = strtok(NULL, "/");
    }
    return true;
}

/* filesystem.disc_image is only handed to the runtime (PSP_ISO), never to Make, so its
 * components may carry spaces, brackets and UTF-8 (ordinary dump names). Mirrors
 * tools/title_manifest.py disc_image_path(): Windows-forbidden and control characters,
 * empty/'.'/'..' components, a leading space, a trailing space or dot, and reserved
 * device names stay rejected. */
static bool is_valid_disc_image_path(const char *s) {
    if (!s || !*s) return false;
    size_t len = strlen(s);
    if (len > 240) return false;
    if (s[0] == '/' || s[len - 1] == '/' || strstr(s, "//") != NULL) return false;
    const char *part = s;
    while (*part) {
        const char *end = strchr(part, '/');
        size_t plen = end ? (size_t)(end - part) : strlen(part);
        if (plen == 0) return false;
        if ((plen == 1 && part[0] == '.') || (plen == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (part[0] == ' ' || part[plen - 1] == ' ' || part[plen - 1] == '.') return false;
        for (size_t i = 0; i < plen; i++) {
            unsigned char c = (unsigned char)part[i];
            if (c < 0x20 || c == 0x7f || strchr("<>:\"\\|?*", (int)c) != NULL) return false;
        }
        /* Reserved device name: the text before the first '.', trailing spaces removed. */
        size_t pre = 0;
        while (pre < plen && part[pre] != '.') pre++;
        while (pre > 0 && part[pre - 1] == ' ') pre--;
        if (pre <= 4) {
            char base[5];
            for (size_t i = 0; i < pre; i++) base[i] = (char)toupper((unsigned char)part[i]);
            base[pre] = '\0';
            static const char * const reserved[] = {
                "CON", "PRN", "AUX", "NUL",
                "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", NULL
            };
            for (int r = 0; reserved[r]; r++) {
                if (strcmp(base, reserved[r]) == 0) return false;
            }
        }
        part = end ? end + 1 : part + plen;
    }
    return true;
}

static bool is_guest_device_path(const char *s) {
    if (!s || !*s) return false;
    size_t len = strlen(s);
    if (len > 256) return false;
    const char *colon = strchr(s, ':');
    if (!colon || colon[1] != '/') return false;
    static const char * const devices[] = {"disc0", "umd0", "ms0", "flash0", "host0", NULL};
    size_t dlen = (size_t)(colon - s);
    for (int i = 0; devices[i]; i++) {
        if (strlen(devices[i]) == dlen && strncmp(s, devices[i], dlen) == 0) return true;
    }
    return false;
}

static bool is_load_address_evidence(const char *s) {
    static const char * const classes[] = {"measured-hw", "measured-ppsspp", "provisional", NULL};
    for (int i = 0; classes[i]; i++) {
        if (strcmp(s, classes[i]) == 0) return true;
    }
    return false;
}

static bool parse_uint32(const JsonNode *node, uint32_t *out_val) {
    if (!node) return false;
    if (node->type == JSON_NUMBER) {
        if (!node->u.num.is_integer) return false;
        if (node->u.num.int_val < 0 || node->u.num.int_val > 4294967295LL) return false;
        *out_val = (uint32_t)node->u.num.int_val;
        return true;
    }
    if (node->type == JSON_STRING) {
        const char *s = node->u.str_val;
        if (!s) return false;
        size_t slen = strlen(s);
        if (slen == 10 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
            for (int i = 2; i < 10; i++) {
                if (!isxdigit((unsigned char)s[i])) return false;
            }
            char *end = NULL;
            unsigned long long v = strtoull(s + 2, &end, 16);
            if (!end || *end != '\0' || v > 0xFFFFFFFFULL) return false;
            *out_val = (uint32_t)v;
            return true;
        }
        return false;
    }
    return false;
}

static bool is_core_reserved_target(uint32_t addr) {
    return (addr & SR_DISPATCH_VFPU_MASK) == SR_DISPATCH_VFPU_TAG;
}

static bool check_object_keys(
    const JsonNode *node,
    const char *path,
    const char * const *allowed_keys,
    const char * const *required_keys,
    char *err_buf,
    size_t err_len
) {
    if (!node || node->type != JSON_OBJECT) {
        if (err_buf) snprintf(err_buf, err_len, "%s: must be an object", path);
        return false;
    }
    /* Check unknown keys */
    for (size_t i = 0; i < node->u.obj.count; i++) {
        const char *k = node->u.obj.members[i].key;
        bool ok = false;
        for (int a = 0; allowed_keys[a]; a++) {
            if (strcmp(k, allowed_keys[a]) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            if (err_buf) snprintf(err_buf, err_len, "%s: unknown field(s): %s", path, k);
            return false;
        }
    }
    /* Check required keys */
    if (required_keys) {
        for (int r = 0; required_keys[r]; r++) {
            if (!obj_get(node, required_keys[r])) {
                if (err_buf) snprintf(err_buf, err_len, "%s: missing required field(s): %s", path, required_keys[r]);
                return false;
            }
        }
    }
    return true;
}

/* -----------------------------------------------------------------------------
 * Manifest Buffer Parser
 * -------------------------------------------------------------------------- */
bool nk_title_manifest_parse_buffer(
    const char *json_str,
    size_t json_len,
    bool allow_override,
    NkTitleEntry *out_entry,
    char *error_buf,
    size_t error_buf_len
) {
    if (error_buf && error_buf_len > 0) error_buf[0] = '\0';

    if (json_len > NK_MANIFEST_MAX_BYTES) {
        if (error_buf) {
            snprintf(error_buf, error_buf_len,
                "Manifest size (%zu bytes) exceeds %s limit (%d bytes)",
                json_len, NK_MANIFEST_LIMIT_CLASS, NK_MANIFEST_MAX_BYTES);
        }
        return false;
    }

    JsonNode *root = json_parse(json_str, json_len, error_buf, error_buf_len);
    if (!root) return false;

    /* 1. Root keys validation */
    static const char * const allowed_root_keys[] = {
        "schema_version", "id", "display_name", "kind", "disc", "executable",
        "modules", "filesystem", "hle_profile", "feature_requirements",
        "compatibility_manifest", "verification_profile", "codegen_profile", "notes",
        "runtime_contract", "profile_zero", "runtime_bindings", "game_name", NULL
    };
    static const char * const required_root_keys[] = {
        "schema_version", "id", "display_name", "kind", "executable",
        "modules", "filesystem", "hle_profile", "feature_requirements",
        "verification_profile", NULL
    };

    if (!check_object_keys(root, "$", allowed_root_keys, required_root_keys, error_buf, error_buf_len)) {
        json_free(root);
        return false;
    }

    /* 2. schema_version == 1 */
    JsonNode *sv_node = obj_get(root, "schema_version");
    if (sv_node->type != JSON_NUMBER || !sv_node->u.num.is_integer || sv_node->u.num.int_val != 1) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.schema_version: only schema version 1 is supported");
        json_free(root);
        return false;
    }

    /* 3. id: valid identifier */
    JsonNode *id_node = obj_get(root, "id");
    if (id_node->type != JSON_STRING || !is_valid_identifier(id_node->u.str_val)) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.id: must match ^[a-z0-9][a-z0-9._-]{0,63}$");
        json_free(root);
        return false;
    }

    /* 4. display_name */
    JsonNode *dn_node = obj_get(root, "display_name");
    if (dn_node->type != JSON_STRING || strlen(dn_node->u.str_val) < 1 || strlen(dn_node->u.str_val) > 128) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.display_name: length must be in range 1..128");
        json_free(root);
        return false;
    }

    /* 4b. game_name: optional explicit build-name declaration (issue #196 Phase 4).
     * The manager accepts -GameName, this field, or a portable id derivation —
     * never an id-prefix mint. Same identifier contract as $.id. */
    JsonNode *gn_node = obj_get(root, "game_name");
    if (gn_node && (gn_node->type != JSON_STRING || !is_valid_identifier(gn_node->u.str_val))) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.game_name: must match ^[a-z0-9][a-z0-9._-]{0,63}$");
        json_free(root);
        return false;
    }

    /* 5. kind */
    JsonNode *kind_node = obj_get(root, "kind");
    if (kind_node->type != JSON_STRING) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.kind: must be a string");
        json_free(root);
        return false;
    }
    NkTitleKind kind;
    if (strcmp(kind_node->u.str_val, "retail") == 0) kind = NK_TITLE_KIND_RETAIL;
    else if (strcmp(kind_node->u.str_val, "synthetic") == 0) kind = NK_TITLE_KIND_SYNTHETIC;
    else if (strcmp(kind_node->u.str_val, "homebrew") == 0) kind = NK_TITLE_KIND_HOMEBREW;
    else {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.kind: unsupported title kind '%s'", kind_node->u.str_val);
        json_free(root);
        return false;
    }

    /* 6. disc object validation */
    JsonNode *disc_node = obj_get(root, "disc");
    if (kind == NK_TITLE_KIND_RETAIL) {
        if (!disc_node || disc_node->type != JSON_OBJECT) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$: retail kind requires disc object");
            json_free(root);
            return false;
        }
        static const char * const allowed_disc_keys[] = {"id", "region", "revision_policy", "compatible_revisions", NULL};
        static const char * const required_disc_keys[] = {"id", "region", "revision_policy", NULL};
        if (!check_object_keys(disc_node, "$.disc", allowed_disc_keys, required_disc_keys, error_buf, error_buf_len)) {
            json_free(root);
            return false;
        }

        JsonNode *disc_id_node = obj_get(disc_node, "id");
        if (!disc_id_node || disc_id_node->type != JSON_STRING || !is_valid_disc_id(disc_id_node->u.str_val)) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.id: must match a nine-character PSP disc ID such as TEST00001");
            json_free(root);
            return false;
        }

        JsonNode *reg_node = obj_get(disc_node, "region");
        if (!reg_node || reg_node->type != JSON_STRING) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.region: must be a string");
            json_free(root);
            return false;
        }
        const char *reg = reg_node->u.str_val;
        if (strcmp(reg, "JP") != 0 && strcmp(reg, "NA") != 0 && strcmp(reg, "EU") != 0 &&
            strcmp(reg, "KR") != 0 && strcmp(reg, "ASIA") != 0 && strcmp(reg, "OTHER") != 0) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.region: unsupported region");
            json_free(root);
            return false;
        }

        JsonNode *pol_node = obj_get(disc_node, "revision_policy");
        if (!pol_node || pol_node->type != JSON_STRING) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.revision_policy: must be a string");
            json_free(root);
            return false;
        }
        const char *pol = pol_node->u.str_val;
        if (strcmp(pol, "exact-disc-id") != 0 && strcmp(pol, "explicit-compatible-revisions") != 0) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.revision_policy: unsupported revision policy");
            json_free(root);
            return false;
        }

        JsonNode *cr_node = obj_get(disc_node, "compatible_revisions");
        if (strcmp(pol, "exact-disc-id") == 0) {
            if (cr_node) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.compatible_revisions: is forbidden for exact-disc-id policy");
                json_free(root);
                return false;
            }
        } else {
            if (!cr_node || cr_node->type != JSON_ARRAY) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.compatible_revisions: is required for explicit-compatible-revisions");
                json_free(root);
                return false;
            }
            if (cr_node->u.arr.count < 1 || cr_node->u.arr.count > MAX_COMPAT_DISC_IDS) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.compatible_revisions: must contain 1..%d items", MAX_COMPAT_DISC_IDS);
                json_free(root);
                return false;
            }
            for (size_t c = 0; c < cr_node->u.arr.count; c++) {
                JsonNode *citem = cr_node->u.arr.items[c];
                if (!citem || citem->type != JSON_STRING || !is_valid_disc_id(citem->u.str_val)) {
                    if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.compatible_revisions[%zu]: must be a distinct PSP disc ID", c);
                    json_free(root);
                    return false;
                }
                if (strcmp(citem->u.str_val, disc_id_node->u.str_val) == 0) {
                    if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.compatible_revisions[%zu]: must be a distinct PSP disc ID from primary", c);
                    json_free(root);
                    return false;
                }
                for (size_t prev = 0; prev < c; prev++) {
                    if (strcmp(cr_node->u.arr.items[prev]->u.str_val, citem->u.str_val) == 0) {
                        if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.compatible_revisions[%zu]: duplicate disc ID", c);
                        json_free(root);
                        return false;
                    }
                }
            }
        }
    } else {
        if (disc_node) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.disc: disc object is forbidden for non-retail kind '%s'", kind_node->u.str_val);
            json_free(root);
            return false;
        }
    }

    /* 7. executable validation */
    JsonNode *exe_node = obj_get(root, "executable");
    static const char * const allowed_exe_keys[] = {"base", "entry", "bss_metadata_source", "extra_executable_spans", NULL};
    if (!check_object_keys(exe_node, "$.executable", allowed_exe_keys, allowed_exe_keys, error_buf, error_buf_len)) {
        json_free(root);
        return false;
    }
    uint32_t exe_base = 0;
    uint32_t exe_entry = 0;
    if (!parse_uint32(obj_get(exe_node, "base"), &exe_base)) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.executable.base: invalid base address");
        json_free(root);
        return false;
    }
    if (!parse_uint32(obj_get(exe_node, "entry"), &exe_entry)) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.executable.entry: invalid entry address");
        json_free(root);
        return false;
    }
    JsonNode *bss_node = obj_get(exe_node, "bss_metadata_source");
    if (!bss_node || bss_node->type != JSON_STRING) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.executable.bss_metadata_source: must be a string");
        json_free(root);
        return false;
    }
    const char *bss_src = bss_node->u.str_val;
    if (strcmp(bss_src, "elf") != 0 && strcmp(bss_src, "psp-header") != 0 && strcmp(bss_src, "none") != 0) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.executable.bss_metadata_source: unsupported metadata source");
        json_free(root);
        return false;
    }
    JsonNode *spans_node = obj_get(exe_node, "extra_executable_spans");
    if (!spans_node || spans_node->type != JSON_ARRAY) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.executable.extra_executable_spans: must be an array");
        json_free(root);
        return false;
    }
    if (spans_node->u.arr.count > 64) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.executable.extra_executable_spans: contains %zu items; maximum is 64", spans_node->u.arr.count);
        json_free(root);
        return false;
    }
    for (size_t s = 0; s < spans_node->u.arr.count; s++) {
        JsonNode *span = spans_node->u.arr.items[s];
        static const char * const span_keys[] = {"start", "end", NULL};
        char span_path[64];
        snprintf(span_path, sizeof(span_path), "$.executable.extra_executable_spans[%zu]", s);
        if (!check_object_keys(span, span_path, span_keys, span_keys, error_buf, error_buf_len)) {
            json_free(root);
            return false;
        }
        uint32_t s_start = 0;
        uint32_t s_end = 0;
        if (!parse_uint32(obj_get(span, "start"), &s_start) || !parse_uint32(obj_get(span, "end"), &s_end) || s_end <= s_start) {
            if (error_buf) snprintf(error_buf, error_buf_len, "%s: end must be greater than start", span_path);
            json_free(root);
            return false;
        }
    }

    /* 8. modules validation */
    JsonNode *mods_node = obj_get(root, "modules");
    if (!mods_node || mods_node->type != JSON_ARRAY) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.modules: must be an array");
        json_free(root);
        return false;
    }
    if (mods_node->u.arr.count > MAX_MODULES) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.modules: contains %zu modules; maximum is %d", mods_node->u.arr.count, MAX_MODULES);
        json_free(root);
        return false;
    }
    /* guest_path and load_address_evidence are optional schema v1 module declarations: the
       manifest names where a guest module's image lives on the PSP's own device tree, and how
       its base was established. Both are part of the schema and are validated by the Python
       parser, so they belong in the accepted set here rather than being rejected as unknown
       fields -- rejecting them made the two parsers disagree on any manifest that uses them. */
    static const char * const allowed_mod_keys[] = {"name", "load_address", "required", "role",
                                                    "guest_path", "load_address_evidence", NULL};
    static const char * const required_mod_keys[] = {"name", "load_address", "required", "role", NULL};
    for (size_t i = 0; i < mods_node->u.arr.count; i++) {
        JsonNode *m = mods_node->u.arr.items[i];
        char mod_path[64];
        snprintf(mod_path, sizeof(mod_path), "$.modules[%zu]", i);
        if (!check_object_keys(m, mod_path, allowed_mod_keys, required_mod_keys, error_buf, error_buf_len)) {
            json_free(root);
            return false;
        }
        JsonNode *mname = obj_get(m, "name");
        if (!mname || mname->type != JSON_STRING || !is_valid_filename(mname->u.str_val)) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].name: must be a portable module filename without path separators", i);
            json_free(root);
            return false;
        }
        /* Check case-insensitive duplicate module names */
        for (size_t prev = 0; prev < i; prev++) {
            JsonNode *pm = mods_node->u.arr.items[prev];
            JsonNode *pname = obj_get(pm, "name");
            if (pname && pname->type == JSON_STRING) {
                if (nk_ascii_casecmp(pname->u.str_val, mname->u.str_val) == 0)
                {
                    if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].name: duplicate module name under case-insensitive comparison", i);
                    json_free(root);
                    return false;
                }
            }
        }

        uint32_t mod_addr = 0;
        if (!parse_uint32(obj_get(m, "load_address"), &mod_addr)) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].load_address: invalid address", i);
            json_free(root);
            return false;
        }
        for (size_t prev = 0; prev < i; prev++) {
            uint32_t prev_addr = 0;
            if (parse_uint32(obj_get(mods_node->u.arr.items[prev], "load_address"), &prev_addr)) {
                if (prev_addr == mod_addr) {
                    if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].load_address: duplicate module load address", i);
                    json_free(root);
                    return false;
                }
            }
        }

        JsonNode *req_node = obj_get(m, "required");
        if (!req_node || req_node->type != JSON_BOOL) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].required: must be a boolean", i);
            json_free(root);
            return false;
        }

        JsonNode *role_node = obj_get(m, "role");
        if (!role_node || role_node->type != JSON_STRING) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].role: must be a string", i);
            json_free(root);
            return false;
        }
        const char *role_str = role_node->u.str_val;
        if (strcmp(role_str, "guest-prx") != 0 && strcmp(role_str, "hle-capability") != 0 && strcmp(role_str, "optional-guest-prx") != 0) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].role: unsupported module role", i);
            json_free(root);
            return false;
        }
        if (strcmp(role_str, "optional-guest-prx") == 0 && req_node->u.bool_val) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu]: optional-guest-prx cannot be marked required", i);
            json_free(root);
            return false;
        }
        if (strcmp(role_str, "hle-capability") == 0 && !req_node->u.bool_val) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu]: hle-capability must be marked required", i);
            json_free(root);
            return false;
        }
        if (strcmp(role_str, "guest-prx") == 0 || strcmp(role_str, "optional-guest-prx") == 0) {
            /* A guest module's code AND data live at load_address, so it must be real user RAM:
               a base outside it lets translated code run while every data write is dropped (the
               late-PRX bases once sat at 0x322xxxxx). The Python parser enforces the same window. */
            if (mod_addr < GUEST_MODULE_RAM_LO || mod_addr >= GUEST_MODULE_RAM_HI) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].load_address: guest module base must lie in user RAM [0x%08x, 0x%08x)", i, GUEST_MODULE_RAM_LO, GUEST_MODULE_RAM_HI);
                json_free(root);
                return false;
            }
        }
        JsonNode *gpath_node = obj_get(m, "guest_path");
        if (gpath_node) {
            if (gpath_node->type != JSON_STRING || !is_guest_device_path(gpath_node->u.str_val)) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].guest_path: must be an absolute PSP device path (e.g. disc0:/...)", i);
                json_free(root);
                return false;
            }
        }
        JsonNode *evidence_node = obj_get(m, "load_address_evidence");
        if (evidence_node) {
            if (evidence_node->type != JSON_STRING || !is_load_address_evidence(evidence_node->u.str_val)) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].load_address_evidence: unsupported evidence class", i);
                json_free(root);
                return false;
            }
        }
    }

    /* 9. filesystem validation */
    JsonNode *fs_node = obj_get(root, "filesystem");
    /* executable/module_dir/psp_header/disc_image are optional input-location declarations
     * (issue #196 Phase 4): a manifest declares where its private inputs live.
     * The runtime consumes only the required runtime-filesystem contract today;
     * the optional declarations are accepted (and validated) for tool parity. */
    static const char * const allowed_fs_keys[] = {"data_root", "memory_stick_root", "device_prefixes", "executable", "module_dir", "psp_header", "disc_image", NULL};
    static const char * const required_fs_keys[] = {"data_root", "memory_stick_root", "device_prefixes", NULL};
    if (!check_object_keys(fs_node, "$.filesystem", allowed_fs_keys, required_fs_keys, error_buf, error_buf_len)) {
        json_free(root);
        return false;
    }
    JsonNode *dr_node = obj_get(fs_node, "data_root");
    JsonNode *ms_node = obj_get(fs_node, "memory_stick_root");
    if (!dr_node || dr_node->type != JSON_STRING || !is_valid_portable_path(dr_node->u.str_val)) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.filesystem.data_root: must be a portable relative POSIX-style path");
        json_free(root);
        return false;
    }
    if (!ms_node || ms_node->type != JSON_STRING || !is_valid_portable_path(ms_node->u.str_val)) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.filesystem.memory_stick_root: must be a portable relative POSIX-style path");
        json_free(root);
        return false;
    }
    {
        /* Paths the manager hands to Make keep the strict Make-safe component rule. */
        static const char * const optional_fs_paths[] = {"executable", "module_dir", "psp_header"};
        for (int oi = 0; oi < 3; oi++) {
            JsonNode *opt_node = obj_get(fs_node, optional_fs_paths[oi]);
            if (opt_node && (opt_node->type != JSON_STRING || !is_valid_portable_path(opt_node->u.str_val))) {
                if (error_buf) snprintf(error_buf, error_buf_len,
                    "$.filesystem.%s: must be a portable relative POSIX-style path", optional_fs_paths[oi]);
                json_free(root);
                return false;
            }
        }
        JsonNode *disc_node = obj_get(fs_node, "disc_image");
        if (disc_node && (disc_node->type != JSON_STRING || !is_valid_disc_image_path(disc_node->u.str_val))) {
            if (error_buf) snprintf(error_buf, error_buf_len,
                "$.filesystem.disc_image: must be a relative POSIX-style path without Windows-forbidden characters");
            json_free(root);
            return false;
        }
    }
    JsonNode *pfx_node = obj_get(fs_node, "device_prefixes");
    if (!pfx_node || pfx_node->type != JSON_ARRAY) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.filesystem.device_prefixes: must be an array");
        json_free(root);
        return false;
    }
    if (pfx_node->u.arr.count > 16) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.filesystem.device_prefixes: contains %zu items; maximum is 16", pfx_node->u.arr.count);
        json_free(root);
        return false;
    }
    for (size_t p = 0; p < pfx_node->u.arr.count; p++) {
        JsonNode *item = pfx_node->u.arr.items[p];
        if (!item || item->type != JSON_STRING) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.filesystem.device_prefixes[%zu]: must be a string", p);
            json_free(root);
            return false;
        }
        const char *ps = item->u.str_val;
        size_t plen = strlen(ps);
        if (plen < 2 || plen > 17 || ps[plen - 1] != ':' || !isalpha((unsigned char)ps[0])) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.filesystem.device_prefixes[%zu]: must look like host0: or ms0:", p);
            json_free(root);
            return false;
        }
        for (size_t prev = 0; prev < p; prev++) {
            if (nk_ascii_casecmp(pfx_node->u.arr.items[prev]->u.str_val, ps) == 0)
            {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.filesystem.device_prefixes[%zu]: duplicate device prefix", p);
                json_free(root);
                return false;
            }
        }
    }

    /* 10. hle_profile & verification_profile */
    JsonNode *hle_node = obj_get(root, "hle_profile");
    if (!hle_node || hle_node->type != JSON_STRING || !is_valid_identifier(hle_node->u.str_val)) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.hle_profile: must match ^[a-z0-9][a-z0-9._-]{0,63}$");
        json_free(root);
        return false;
    }
    JsonNode *vp_node = obj_get(root, "verification_profile");
    if (!vp_node || vp_node->type != JSON_STRING || !is_valid_identifier(vp_node->u.str_val)) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.verification_profile: must match ^[a-z0-9][a-z0-9._-]{0,63}$");
        json_free(root);
        return false;
    }

    /* 11. codegen_profile (optional) */
    JsonNode *cg_node = obj_get(root, "codegen_profile");
    if (cg_node) {
        if (cg_node->type != JSON_STRING) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.codegen_profile: must be a string");
            json_free(root);
            return false;
        }
        if (strcmp(cg_node->u.str_val, "none") != 0 && strcmp(cg_node->u.str_val, "hst") != 0) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.codegen_profile: unsupported codegen profile");
            json_free(root);
            return false;
        }
    }

    /* 12. feature_requirements */
    JsonNode *feats_node = obj_get(root, "feature_requirements");
    if (!feats_node || feats_node->type != JSON_ARRAY) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.feature_requirements: must be an array");
        json_free(root);
        return false;
    }
    if (feats_node->u.arr.count > 64) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.feature_requirements: contains %zu items; maximum is 64", feats_node->u.arr.count);
        json_free(root);
        return false;
    }
    bool has_font_feature = false;
    bool has_psmf_feature = false;
    for (size_t i = 0; i < feats_node->u.arr.count; i++) {
        JsonNode *fitem = feats_node->u.arr.items[i];
        if (!fitem || fitem->type != JSON_STRING || !is_valid_identifier(fitem->u.str_val)) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.feature_requirements[%zu]: must match ^[a-z0-9][a-z0-9._-]{0,63}$", i);
            json_free(root);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(feats_node->u.arr.items[j]->u.str_val, fitem->u.str_val) == 0) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.feature_requirements: duplicate feature requirement '%s'", fitem->u.str_val);
                json_free(root);
                return false;
            }
        }
        if (strcmp(fitem->u.str_val, "font") == 0 || strcmp(fitem->u.str_val, "font-firmware") == 0) {
            has_font_feature = true;
        }
        if (strcmp(fitem->u.str_val, "psmf") == 0) {
            has_psmf_feature = true;
        }
    }

    /* 13. compatibility_manifest (optional) */
    JsonNode *compat_mf_node = obj_get(root, "compatibility_manifest");
    if (compat_mf_node) {
        if (compat_mf_node->type != JSON_STRING || !is_valid_portable_path(compat_mf_node->u.str_val)) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.compatibility_manifest: must be a portable relative POSIX-style path");
            json_free(root);
            return false;
        }
    }

    /* 14. notes (optional) */
    JsonNode *notes_node = obj_get(root, "notes");
    if (notes_node) {
        if (notes_node->type != JSON_STRING || strlen(notes_node->u.str_val) > 2048) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.notes: must be a string <= 2048 characters");
            json_free(root);
            return false;
        }
    }

    /* 15. runtime_bindings (optional) */
    uint32_t expected_file_count = 0;
    JsonNode *rb_node = obj_get(root, "runtime_bindings");
    if (rb_node) {
        if (rb_node->type != JSON_OBJECT) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings: must be an object");
            json_free(root);
            return false;
        }
        static const char * const allowed_rb_keys[] = {
            "schema_version", "fallback_entry", "worker_thread_entry", "launcher_thread_entry",
            "vblank_frame_counter_addr", "vblank_vsync_counter_addr", "libfont_ready_flag_addr",
            "frame_ready_latch_addr", "expected_data_file_count", "dispatch_aliases",
            "callback_terminators", "display_bringup", "runtime_sync", NULL
        };
        static const char * const required_rb_keys[] = {"schema_version", NULL};
        if (!check_object_keys(rb_node, "$.runtime_bindings", allowed_rb_keys, required_rb_keys, error_buf, error_buf_len)) {
            json_free(root);
            return false;
        }
        JsonNode *rb_sv = obj_get(rb_node, "schema_version");
        if (rb_sv->type != JSON_NUMBER || !rb_sv->u.num.is_integer || rb_sv->u.num.int_val != 1) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings.schema_version: only runtime-binding schema version 1 is supported");
            json_free(root);
            return false;
        }
        if (rb_node->u.obj.count <= 1) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings: must configure at least one binding; omit the block instead");
            json_free(root);
            return false;
        }

        /* Validate scalar addresses in runtime_bindings */
        static const char * const scalar_binding_fields[] = {
            "fallback_entry", "worker_thread_entry", "launcher_thread_entry",
            "vblank_frame_counter_addr", "vblank_vsync_counter_addr",
            "libfont_ready_flag_addr", "frame_ready_latch_addr", NULL
        };
        for (int s = 0; scalar_binding_fields[s]; s++) {
            JsonNode *s_node = obj_get(rb_node, scalar_binding_fields[s]);
            if (s_node) {
                uint32_t bind_addr = 0;
                if (!parse_uint32(s_node, &bind_addr) || bind_addr == 0 || (bind_addr % 4 != 0)) {
                    if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings.%s: must be a non-zero 4-byte aligned integer", scalar_binding_fields[s]);
                    json_free(root);
                    return false;
                }
            }
        }

        /* Check paired vblank counters */
        JsonNode *vf = obj_get(rb_node, "vblank_frame_counter_addr");
        JsonNode *vs = obj_get(rb_node, "vblank_vsync_counter_addr");
        if ((vf && !vs) || (!vf && vs)) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings: vblank counters must be configured together");
            json_free(root);
            return false;
        }
        if (vf && vs) {
            uint32_t vf_a = 0, vs_a = 0;
            parse_uint32(vf, &vf_a);
            parse_uint32(vs, &vs_a);
            if (vf_a == vs_a) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings: vblank counters must be distinct");
                json_free(root);
                return false;
            }
        }

        /* Check distinct worker and launcher */
        JsonNode *wk = obj_get(rb_node, "worker_thread_entry");
        JsonNode *ln = obj_get(rb_node, "launcher_thread_entry");
        if (wk && ln) {
            uint32_t wk_a = 0, ln_a = 0;
            parse_uint32(wk, &wk_a);
            parse_uint32(ln, &ln_a);
            if (wk_a == ln_a) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings: worker and launcher must be distinct");
                json_free(root);
                return false;
            }
        }

        /* Check expected_data_file_count */
        JsonNode *ef_node = obj_get(rb_node, "expected_data_file_count");
        if (ef_node) {
            uint32_t count = 0;
            if (!parse_uint32(ef_node, &count) || count == 0 || count > 1000000) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings.expected_data_file_count: must be > 0 and <= 1000000");
                json_free(root);
                return false;
            }
            expected_file_count = count;
        }

        /* Check dispatch_aliases */
        JsonNode *da_node = obj_get(rb_node, "dispatch_aliases");
        if (da_node) {
            if (da_node->type != JSON_ARRAY) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings.dispatch_aliases: must be an array");
                json_free(root);
                return false;
            }
            if (da_node->u.arr.count == 0) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings.dispatch_aliases: must not be empty; omit the field instead");
                json_free(root);
                return false;
            }
            if (da_node->u.arr.count > 32) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings.dispatch_aliases: contains %zu items; maximum is 32", da_node->u.arr.count);
                json_free(root);
                return false;
            }
            uint32_t da_srcs[32];
            uint32_t da_dsts[32];
            static const char * const da_keys[] = {"from", "to", NULL};
            for (size_t a = 0; a < da_node->u.arr.count; a++) {
                JsonNode *da_item = da_node->u.arr.items[a];
                char item_path[64];
                snprintf(item_path, sizeof(item_path), "$.runtime_bindings.dispatch_aliases[%zu]", a);
                if (!check_object_keys(da_item, item_path, da_keys, da_keys, error_buf, error_buf_len)) {
                    json_free(root);
                    return false;
                }
                uint32_t src = 0, dst = 0;
                JsonNode *from_n = obj_get(da_item, "from");
                JsonNode *to_n = obj_get(da_item, "to");
                if (!parse_uint32(from_n, &src) || src == 0 || (src % 4 != 0)) {
                    if (error_buf) snprintf(error_buf, error_buf_len, "%s.from: must be a non-zero 4-byte aligned integer", item_path);
                    json_free(root);
                    return false;
                }
                if (!parse_uint32(to_n, &dst) || dst == 0 || (dst % 4 != 0)) {
                    if (error_buf) snprintf(error_buf, error_buf_len, "%s.to: must be a non-zero 4-byte aligned integer", item_path);
                    json_free(root);
                    return false;
                }
                if (is_core_reserved_target(src)) {
                    if (error_buf) snprintf(error_buf, error_buf_len, "%s.from: 0x%08x is inside the core VFPU dispatch-target range", item_path, src);
                    json_free(root);
                    return false;
                }
                if (src == dst) {
                    if (error_buf) snprintf(error_buf, error_buf_len, "%s: from and to must differ", item_path);
                    json_free(root);
                    return false;
                }
                for (size_t prev = 0; prev < a; prev++) {
                    if (da_srcs[prev] == src) {
                        if (error_buf) snprintf(error_buf, error_buf_len, "%s.from: duplicate alias source", item_path);
                        json_free(root);
                        return false;
                    }
                }
                da_srcs[a] = src;
                da_dsts[a] = dst;
            }
            for (size_t a = 0; a < da_node->u.arr.count; a++) {
                for (size_t b = 0; b < da_node->u.arr.count; b++) {
                    if (da_dsts[a] == da_srcs[b]) {
                        if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings.dispatch_aliases[%zu].to: is itself an alias source", a);
                        json_free(root);
                        return false;
                    }
                }
            }
        }
    }

    /* 16. Extract parsed fields into temporary storage */
    OverlayStorageSlot temp;
    memset(&temp, 0, sizeof(temp));

    snprintf(temp.id, sizeof(temp.id), "%s", id_node->u.str_val);
    if (gn_node) {
        snprintf(temp.game_name, sizeof(temp.game_name), "%s", gn_node->u.str_val);
    } else {
        derive_game_name(temp.id, temp.game_name, sizeof(temp.game_name));
    }
    snprintf(temp.display_name, sizeof(temp.display_name), "%s", dn_node->u.str_val);
    temp.kind = kind;

    if (disc_node) {
        JsonNode *did = obj_get(disc_node, "id");
        snprintf(temp.primary_disc_id, sizeof(temp.primary_disc_id), "%s", did->u.str_val);

        JsonNode *cr_node = obj_get(disc_node, "compatible_revisions");
        if (cr_node && cr_node->type == JSON_ARRAY) {
            int c_count = 0;
            for (size_t c = 0; c < cr_node->u.arr.count && c < MAX_COMPAT_DISC_IDS; c++) {
                JsonNode *citem = cr_node->u.arr.items[c];
                if (citem && citem->type == JSON_STRING) {
                    snprintf(temp.compat_ids_storage[c_count], 17, "%s", citem->u.str_val);
                    temp.compat_id_ptrs[c_count] = temp.compat_ids_storage[c_count];
                    c_count++;
                }
            }
            temp.compat_id_ptrs[c_count] = NULL;
        }
    }

    temp.executable_base = exe_base;
    temp.executable_entry = exe_entry; /* Canonical projection: represents executable.entry */
    snprintf(temp.bss_metadata_source, sizeof(temp.bss_metadata_source), "%s", bss_src);
    snprintf(temp.data_root, sizeof(temp.data_root), "%s", dr_node->u.str_val);
    snprintf(temp.memory_stick_root, sizeof(temp.memory_stick_root), "%s", ms_node->u.str_val);
    snprintf(temp.hle_profile, sizeof(temp.hle_profile), "%s", hle_node->u.str_val);

    if (cg_node && cg_node->type == JSON_STRING) {
        snprintf(temp.codegen_profile, sizeof(temp.codegen_profile), "%s", cg_node->u.str_val);
    } else {
        snprintf(temp.codegen_profile, sizeof(temp.codegen_profile), "none");
    }

    temp.expected_data_file_count = expected_file_count;
    temp.requires_font_firmware = has_font_feature;
    temp.requires_psmf = has_psmf_feature;

    temp.module_count = (int)mods_node->u.arr.count;
    for (int i = 0; i < temp.module_count; i++) {
        JsonNode *m = mods_node->u.arr.items[i];
        JsonNode *mn = obj_get(m, "name");
        snprintf(temp.module_names[i], sizeof(temp.module_names[i]), "%s", mn->u.str_val);
        temp.modules[i].name = temp.module_names[i];

        uint32_t laddr = 0;
        parse_uint32(obj_get(m, "load_address"), &laddr);
        temp.modules[i].load_address = laddr;

        JsonNode *req = obj_get(m, "required");
        temp.modules[i].required = (req && req->type == JSON_BOOL) ? req->u.bool_val : false;
    }

    temp.entry.id = temp.id;
    temp.entry.game_name = temp.game_name;
    temp.entry.display_name = temp.display_name;
    temp.entry.kind = temp.kind;
    temp.entry.primary_disc_id = temp.primary_disc_id[0] ? temp.primary_disc_id : NULL;
    temp.entry.compatible_disc_ids = temp.compat_id_ptrs[0] ? temp.compat_id_ptrs : NULL;
    temp.entry.executable_base = temp.executable_base;
    temp.entry.executable_entry = temp.executable_entry;
    temp.entry.bss_metadata_source = temp.bss_metadata_source;
    temp.entry.data_root = temp.data_root;
    temp.entry.memory_stick_root = temp.memory_stick_root;
    temp.entry.hle_profile = temp.hle_profile;
    temp.entry.codegen_profile = temp.codegen_profile;
    temp.entry.expected_data_file_count = temp.expected_data_file_count;
    temp.entry.requires_font_firmware = temp.requires_font_firmware;
    temp.entry.requires_psmf = temp.requires_psmf;
    temp.entry.modules = temp.modules;
    temp.entry.module_count = temp.module_count;

    /* 17. Collision Checking across all identities:
     * - id
     * - primary_disc_id
     * - each compatible_revisions disc ID
     * Against:
     * - All public catalog entries
     * - All already-loaded external overlay identities
     */
    bool collision = false;
    const char *collision_id = NULL;

    /* Check against public canonical catalog */
    for (int i = 0; i < nk_title_catalog_count; i++) {
        const NkTitleEntry *pub = &nk_title_catalog_entries[i];
        if (strcmp(pub->id, temp.entry.id) == 0) {
            collision = true;
            collision_id = pub->id;
            break;
        }
        if (temp.entry.primary_disc_id && pub->primary_disc_id) {
            if (strcmp(pub->primary_disc_id, temp.entry.primary_disc_id) == 0) {
                collision = true;
                collision_id = pub->id;
                break;
            }
        }
        if (temp.entry.primary_disc_id && pub->compatible_disc_ids) {
            for (int c = 0; pub->compatible_disc_ids[c]; c++) {
                if (strcmp(pub->compatible_disc_ids[c], temp.entry.primary_disc_id) == 0) {
                    collision = true;
                    collision_id = pub->id;
                    break;
                }
            }
            if (collision) break;
        }
        if (temp.entry.compatible_disc_ids && pub->primary_disc_id) {
            for (int c = 0; temp.entry.compatible_disc_ids[c]; c++) {
                if (strcmp(temp.entry.compatible_disc_ids[c], pub->primary_disc_id) == 0) {
                    collision = true;
                    collision_id = pub->id;
                    break;
                }
            }
            if (collision) break;
        }
        if (temp.entry.compatible_disc_ids && pub->compatible_disc_ids) {
            for (int c = 0; temp.entry.compatible_disc_ids[c]; c++) {
                for (int pc = 0; pub->compatible_disc_ids[pc]; pc++) {
                    if (strcmp(temp.entry.compatible_disc_ids[c], pub->compatible_disc_ids[pc]) == 0) {
                        collision = true;
                        collision_id = pub->id;
                        break;
                    }
                }
                if (collision) break;
            }
            if (collision) break;
        }
    }

    /* Check against already-loaded external overlay identities */
    if (!collision) {
        for (int i = 0; i < s_overlay_slot_count; i++) {
            const OverlayStorageSlot *ov = &s_overlay_slots[i];
            /* Allow self-update if reloading the exact same overlay id */
            if (strcmp(ov->entry.id, temp.entry.id) == 0) continue;

            if (strcmp(ov->entry.id, temp.entry.id) == 0) {
                collision = true;
                collision_id = ov->entry.id;
                break;
            }
            if (temp.entry.primary_disc_id && ov->entry.primary_disc_id) {
                if (strcmp(ov->entry.primary_disc_id, temp.entry.primary_disc_id) == 0) {
                    collision = true;
                    collision_id = ov->entry.id;
                    break;
                }
            }
            if (temp.entry.primary_disc_id && ov->entry.compatible_disc_ids) {
                for (int c = 0; ov->entry.compatible_disc_ids[c]; c++) {
                    if (strcmp(ov->entry.compatible_disc_ids[c], temp.entry.primary_disc_id) == 0) {
                        collision = true;
                        collision_id = ov->entry.id;
                        break;
                    }
                }
                if (collision) break;
            }
            if (temp.entry.compatible_disc_ids && ov->entry.primary_disc_id) {
                for (int c = 0; temp.entry.compatible_disc_ids[c]; c++) {
                    if (strcmp(temp.entry.compatible_disc_ids[c], ov->entry.primary_disc_id) == 0) {
                        collision = true;
                        collision_id = ov->entry.id;
                        break;
                    }
                }
                if (collision) break;
            }
            if (temp.entry.compatible_disc_ids && ov->entry.compatible_disc_ids) {
                for (int c = 0; temp.entry.compatible_disc_ids[c]; c++) {
                    for (int pc = 0; ov->entry.compatible_disc_ids[pc]; pc++) {
                        if (strcmp(temp.entry.compatible_disc_ids[c], ov->entry.compatible_disc_ids[pc]) == 0) {
                            collision = true;
                            collision_id = ov->entry.id;
                            break;
                        }
                    }
                    if (collision) break;
                }
                if (collision) break;
            }
        }
    }

    if (collision) {
        if (!allow_override) {
            if (error_buf) {
                snprintf(error_buf, error_buf_len,
                    "Overlay identity '%s' conflicts with public canonical catalog title '%s'; override forbidden in standard mode",
                    temp.entry.id, collision_id);
            }
            json_free(root);
            return false;
        } else {
            fprintf(stderr, "[WARNING] Overriding title definition for '%s' with external manifest (conflict: '%s')!\n", temp.entry.id, collision_id);
        }
    }

    /* Assign to multi-overlay storage slot */
    int target_slot = -1;
    for (int i = 0; i < s_overlay_slot_count; i++) {
        if (strcmp(s_overlay_slots[i].entry.id, temp.entry.id) == 0) {
            target_slot = i;
            break;
        }
    }
    if (target_slot == -1) {
        if (s_overlay_slot_count < NK_MANIFEST_MAX_OVERLAYS) {
            target_slot = s_overlay_slot_count++;
        } else {
            target_slot = NK_MANIFEST_MAX_OVERLAYS - 1; /* bounded replacement of last slot */
        }
    }

    nk_title_catalog_set_overlay_storage_reset(nk_manifest_reset_overlay_storage);
    s_overlay_slots[target_slot] = temp;
    /* Re-anchor self pointers for the chosen slot */
    OverlayStorageSlot *dest = &s_overlay_slots[target_slot];
    dest->entry.id = dest->id;
    dest->entry.game_name = dest->game_name;
    dest->entry.display_name = dest->display_name;
    dest->entry.kind = dest->kind;
    dest->entry.primary_disc_id = dest->primary_disc_id[0] ? dest->primary_disc_id : NULL;
    for (int c = 0; dest->compat_id_ptrs[c]; c++) {
        dest->compat_id_ptrs[c] = dest->compat_ids_storage[c];
    }
    dest->entry.compatible_disc_ids = dest->compat_id_ptrs[0] ? dest->compat_id_ptrs : NULL;
    dest->entry.executable_base = dest->executable_base;
    dest->entry.executable_entry = dest->executable_entry;
    dest->entry.bss_metadata_source = dest->bss_metadata_source;
    dest->entry.data_root = dest->data_root;
    dest->entry.memory_stick_root = dest->memory_stick_root;
    dest->entry.hle_profile = dest->hle_profile;
    dest->entry.codegen_profile = dest->codegen_profile;
    dest->entry.expected_data_file_count = dest->expected_data_file_count;
    dest->entry.requires_font_firmware = dest->requires_font_firmware;
    dest->entry.requires_psmf = dest->requires_psmf;
    for (int m = 0; m < dest->module_count; m++) {
        dest->modules[m].name = dest->module_names[m];
    }
    dest->entry.modules = dest->modules;
    dest->entry.module_count = dest->module_count;

    if (out_entry) {
        *out_entry = dest->entry;
    }

    json_free(root);
    return true;
}

bool nk_title_manifest_load_overlay_ext(
    const char *manifest_path,
    bool allow_override,
    char *error_buf,
    size_t error_buf_len
) {
    if (error_buf && error_buf_len > 0) error_buf[0] = '\0';

    if (!manifest_path || !*manifest_path) {
        if (error_buf) snprintf(error_buf, error_buf_len, "Manifest path is NULL or empty");
        return false;
    }

    FILE *f = manifest_fopen(manifest_path);
    if (!f) {
        if (error_buf) snprintf(error_buf, error_buf_len, "Could not open manifest file: %s", manifest_path);
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        if (error_buf) snprintf(error_buf, error_buf_len, "Failed to seek to end of file");
        fclose(f);
        return false;
    }
    long fsize = ftell(f);
    if (fsize < 0) {
        if (error_buf) snprintf(error_buf, error_buf_len, "Failed to determine file size");
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        if (error_buf) snprintf(error_buf, error_buf_len, "Failed to seek to start of file");
        fclose(f);
        return false;
    }

    if (fsize > NK_MANIFEST_MAX_BYTES) {
        if (error_buf) {
            snprintf(error_buf, error_buf_len,
                "Manifest file size (%ld bytes) exceeds %s limit of %d bytes",
                fsize, NK_MANIFEST_LIMIT_CLASS, NK_MANIFEST_MAX_BYTES);
        }
        fclose(f);
        return false;
    }

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) {
        if (error_buf) snprintf(error_buf, error_buf_len, "Out of memory allocating manifest buffer");
        fclose(f);
        return false;
    }

    size_t bytes_read = fread(buf, 1, (size_t)fsize, f);
    if (ferror(f) || bytes_read != (size_t)fsize) {
        if (error_buf) snprintf(error_buf, error_buf_len, "Short read or error reading manifest file");
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    buf[bytes_read] = '\0';

    NkTitleEntry entry;
    bool ok = nk_title_manifest_parse_buffer(buf, bytes_read, allow_override, &entry, error_buf, error_buf_len);
    free(buf);

    if (ok) {
        /* Find matching slot and register with catalog */
        for (int i = 0; i < s_overlay_slot_count; i++) {
            if (strcmp(s_overlay_slots[i].entry.id, entry.id) == 0) {
                nk_title_catalog_register_overlay(&s_overlay_slots[i].entry);
                break;
            }
        }
    }
    return ok;
}

bool nk_title_manifest_load_overlay(
    const char *manifest_path,
    char *error_buf,
    size_t error_buf_len
) {
    return nk_title_manifest_load_overlay_ext(manifest_path, false, error_buf, error_buf_len);
}

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_len;
} NkSha256;

static uint32_t nk_sha256_rotr(uint32_t value, unsigned int amount) {
    return (value >> amount) | (value << (32u - amount));
}

static void nk_sha256_transform(NkSha256 *ctx, const uint8_t block[64]) {
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
        uint32_t s0 = nk_sha256_rotr(words[i - 15], 7) ^
                      nk_sha256_rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 = nk_sha256_rotr(words[i - 2], 17) ^
                      nk_sha256_rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t sum1 = nk_sha256_rotr(e, 6) ^ nk_sha256_rotr(e, 11) ^ nk_sha256_rotr(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t t1 = h + sum1 + choose + k[i] + words[i];
        uint32_t sum0 = nk_sha256_rotr(a, 2) ^ nk_sha256_rotr(a, 13) ^ nk_sha256_rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = sum0 + majority;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void nk_sha256_init(NkSha256 *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bit_count = 0;
    ctx->block_len = 0;
}

static void nk_sha256_update(NkSha256 *ctx, const uint8_t *data, size_t len) {
    ctx->bit_count += (uint64_t)len * 8u;
    while (len > 0) {
        size_t room = sizeof(ctx->block) - ctx->block_len;
        size_t take = len < room ? len : room;
        memcpy(ctx->block + ctx->block_len, data, take);
        ctx->block_len += take;
        data += take;
        len -= take;
        if (ctx->block_len == sizeof(ctx->block)) {
            nk_sha256_transform(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

static void nk_sha256_finish(NkSha256 *ctx, uint8_t digest[32]) {
    uint64_t bits = ctx->bit_count;
    ctx->block[ctx->block_len++] = 0x80;
    if (ctx->block_len > 56) {
        memset(ctx->block + ctx->block_len, 0, sizeof(ctx->block) - ctx->block_len);
        nk_sha256_transform(ctx, ctx->block);
        ctx->block_len = 0;
    }
    memset(ctx->block + ctx->block_len, 0, 56 - ctx->block_len);
    for (size_t i = 0; i < 8; i++) {
        ctx->block[63 - i] = (uint8_t)(bits >> (i * 8));
    }
    nk_sha256_transform(ctx, ctx->block);
    for (size_t i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

static bool nk_manifest_normalize_disc_id(const char *source, char out[10]) {
    if (!source || strlen(source) != 9) return false;
    for (size_t i = 0; i < 9; i++) {
        unsigned char ch = (unsigned char)source[i];
        if (i < 4) {
            if (!isalpha(ch) || ch > 0x7f) return false;
            out[i] = (char)toupper(ch);
        } else {
            if (!isdigit(ch)) return false;
            out[i] = (char)ch;
        }
    }
    out[9] = '\0';
    return true;
}

static bool nk_manifest_json_escape(const char *source, char *out, size_t out_len) {
    if (!source || !out || out_len == 0) return false;
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)source; *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (used + 2 >= out_len) return false;
            out[used++] = '\\';
            out[used++] = (char)*p;
        } else if (*p < 0x20) {
            if (used + 6 >= out_len) return false;
            int written = snprintf(out + used, out_len - used, "\\u%04x", (unsigned)*p);
            if (written != 6) return false;
            used += 6;
        } else {
            if (used + 1 >= out_len) return false;
            out[used++] = (char)*p;
        }
    }
    out[used] = '\0';
    return true;
}

static bool nk_manifest_hash_iso_executable(const char *iso_path,
                                            const char *selected_executable,
                                            char out_hex[65]) {
    if (!iso_path || !selected_executable || !out_hex) return false;
    const char *relative_path = NULL;
    if (strcmp(selected_executable, "EBOOT.BIN") == 0) {
        relative_path = "PSP_GAME/SYSDIR/EBOOT.BIN";
    } else if (strcmp(selected_executable, "BOOT.BIN") == 0) {
        relative_path = "PSP_GAME/SYSDIR/BOOT.BIN";
    } else {
        return false;
    }

    NkIsoReader *reader = nk_iso_reader_open(iso_path);
    if (!reader) return false;
    uint32_t lba = 0, size = 0;
    bool is_dir = false;
    bool ok = nk_iso_reader_lookup(reader, relative_path, &lba, &size, &is_dir) == 0 &&
              !is_dir && size > 0 && size <= 512u * 1024u * 1024u;
    NkSha256 ctx;
    nk_sha256_init(&ctx);
    uint8_t buffer[32768];
    uint64_t offset = 0;
    while (ok && offset < size) {
        uint32_t count = (uint32_t)((uint64_t)size - offset < sizeof(buffer)
            ? (uint64_t)size - offset : sizeof(buffer));
        int read_count = nk_iso_reader_read(reader, lba, offset, buffer, count);
        if (read_count != (int)count) {
            ok = false;
            break;
        }
        nk_sha256_update(&ctx, buffer, count);
        offset += count;
    }
    nk_iso_reader_close(reader);
    if (!ok) return false;

    uint8_t digest[32];
    static const char hex[] = "0123456789abcdef";
    nk_sha256_finish(&ctx, digest);
    for (size_t i = 0; i < sizeof(digest); i++) {
        out_hex[i * 2] = hex[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[64] = '\0';
    return true;
}

static bool nk_manifest_replace_file(const char *temporary, const char *target) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR w_temporary[32768], w_target[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, temporary, -1,
                            w_temporary, (int)(sizeof(w_temporary) / sizeof(w_temporary[0]))) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target, -1,
                            w_target, (int)(sizeof(w_target) / sizeof(w_target[0]))) <= 0) return false;
    return MoveFileExW(w_temporary, w_target,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(temporary, target) == 0;
#endif
}

static void nk_manifest_remove_file(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR w_path[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            w_path, (int)(sizeof(w_path) / sizeof(w_path[0]))) > 0) {
        DeleteFileW(w_path);
    }
#else
    remove(path);
#endif
}

bool nk_title_manifest_write_experimental_profile(
    const char *iso_path,
    bool param_sfo_parsed,
    const char *disc_id,
    const char *title,
    const char *selected_executable,
    const char *user_data_root,
    char *out_profile_id,
    size_t out_profile_id_len,
    char *error_buf,
    size_t error_buf_len
) {
    if (error_buf && error_buf_len) error_buf[0] = '\0';
    if (!iso_path || !param_sfo_parsed || !user_data_root || !*user_data_root) {
        if (error_buf && error_buf_len) {
            snprintf(error_buf, error_buf_len,
                     "Experimental profiles require a readable disc and parsed PARAM.SFO.");
        }
        return false;
    }

    char normalized_id[10];
    if (!nk_manifest_normalize_disc_id(disc_id, normalized_id)) {
        if (error_buf && error_buf_len) {
            snprintf(error_buf, error_buf_len, "PARAM.SFO does not contain a valid PSP disc ID.");
        }
        return false;
    }
    char profile_id[64];
    for (size_t i = 0; i < 9; i++) profile_id[13 + i] = (char)tolower((unsigned char)normalized_id[i]);
    memcpy(profile_id, "experimental-", 13);
    profile_id[22] = '\0';
    if (!out_profile_id || out_profile_id_len <= strlen(profile_id)) {
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "Profile ID output buffer is too small.");
        return false;
    }

    char fallback_title[64];
    snprintf(fallback_title, sizeof(fallback_title), "PSP Title (%s)", normalized_id);
    char normalized_title[129];
    const char *display_name = fallback_title;
    if (title && *title) {
        size_t source_len = strlen(title);
        size_t start = 0, end = source_len;
        while (start < end && isspace((unsigned char)title[start])) start++;
        while (end > start && isspace((unsigned char)title[end - 1])) end--;
        size_t title_len = end - start;
        bool safe_title = title_len > 0 && title_len <= 128;
        for (size_t i = start; safe_title && i < end; i++) {
            if ((unsigned char)title[i] < 0x20) safe_title = false;
        }
        if (safe_title && validate_utf8((const uint8_t *)(title + start), title_len)) {
            memcpy(normalized_title, title + start, title_len);
            normalized_title[title_len] = '\0';
            display_name = normalized_title;
        }
    }

    const char *region = "OTHER";
    if (strncmp(normalized_id, "UCUS", 4) == 0 || strncmp(normalized_id, "ULUS", 4) == 0) region = "NA";
    else if (strncmp(normalized_id, "UCES", 4) == 0 || strncmp(normalized_id, "ULES", 4) == 0) region = "EU";
    else if (strncmp(normalized_id, "UCJS", 4) == 0 || strncmp(normalized_id, "ULJS", 4) == 0) region = "JP";
    else if (strncmp(normalized_id, "UCAS", 4) == 0 || strncmp(normalized_id, "ULAS", 4) == 0) region = "ASIA";

    char escaped_title[768];
    if (!nk_manifest_json_escape(display_name, escaped_title, sizeof(escaped_title))) {
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "PARAM.SFO title cannot be represented safely.");
        return false;
    }
    char manifest[2048];
    int manifest_len = snprintf(manifest, sizeof(manifest),
        "{\"schema_version\":1,\"id\":\"%s\",\"game_name\":\"%s\","
        "\"display_name\":\"%s\","
        "\"kind\":\"retail\",\"disc\":{\"id\":\"%s\",\"region\":\"%s\","
        "\"revision_policy\":\"exact-disc-id\"},"
        "\"executable\":{\"base\":0,\"entry\":0,\"bss_metadata_source\":\"none\","
        "\"extra_executable_spans\":[]},\"modules\":[],"
        "\"filesystem\":{\"data_root\":\"data\",\"memory_stick_root\":\"savedata\","
        "\"device_prefixes\":[\"disc0:\",\"ms0:\"]},\"hle_profile\":\"generic\","
        "\"feature_requirements\":[],\"verification_profile\":\"experimental-unverified\"}",
        profile_id, profile_id, escaped_title, normalized_id, region);
    if (manifest_len < 0 || (size_t)manifest_len >= sizeof(manifest)) {
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "Experimental manifest exceeded its size limit.");
        return false;
    }
    NkTitleEntry parsed_entry;
    if (!nk_title_manifest_parse_buffer(manifest, (size_t)manifest_len, false,
                                        &parsed_entry, error_buf, error_buf_len)) {
        return false;
    }

    char executable_hash[65] = "";
    const char *identity_executable = "";
    bool has_executable = selected_executable && selected_executable[0];
    if (has_executable) {
        if (!nk_manifest_hash_iso_executable(iso_path, selected_executable, executable_hash)) {
            if (error_buf && error_buf_len) {
                snprintf(error_buf, error_buf_len, "Selected executable could not be hashed from the ISO safely.");
            }
            return false;
        }
        identity_executable = strcmp(selected_executable, "EBOOT.BIN") == 0
            ? "PSP_GAME/SYSDIR/EBOOT.BIN" : "PSP_GAME/SYSDIR/BOOT.BIN";
    }

    char identity_executable_json[96];
    if (!nk_manifest_json_escape(identity_executable, identity_executable_json,
                                 sizeof(identity_executable_json))) return false;
    char profile[4096];
    char hash_json[67] = "null";
    if (has_executable) {
        int hash_json_len = snprintf(hash_json, sizeof(hash_json), "\"%s\"", executable_hash);
        if (hash_json_len < 0 || (size_t)hash_json_len >= sizeof(hash_json)) return false;
    }
    int profile_len = snprintf(profile, sizeof(profile),
        "{\"schema_version\":1,\"manifest\":%s,\"input_identity\":{"
        "\"disc_id\":\"%s\",\"selected_executable\":\"%s\","
        "\"executable_sha256\":%s,\"elf_sha256\":%s}}\n",
        manifest, normalized_id, identity_executable_json,
        hash_json, hash_json);
    if (profile_len < 0 || (size_t)profile_len >= sizeof(profile)) {
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "Experimental profile exceeded its size limit.");
        return false;
    }

    char experimental_root[NK_MAX_PATH];
    char profile_dir[NK_MAX_PATH];
    char profile_path[NK_MAX_PATH];
    char temporary_path[NK_MAX_PATH];
    char separator = nk_platform_path_separator();
    int written = snprintf(experimental_root, sizeof(experimental_root), "%s%cexperimental",
                           user_data_root, separator);
    if (written < 0 || (size_t)written >= sizeof(experimental_root) ||
        !nk_platform_mkdir_p_private(user_data_root) ||
        !nk_platform_mkdir_p_private(experimental_root)) {
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "User data directory is unavailable for the experimental profile.");
        return false;
    }
    written = snprintf(profile_dir, sizeof(profile_dir), "%s%c%s", experimental_root,
                       separator, normalized_id);
    if (written < 0 || (size_t)written >= sizeof(profile_dir) ||
        !nk_platform_mkdir_p_private(profile_dir)) {
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "Experimental profile directory could not be created.");
        return false;
    }
    written = snprintf(profile_path, sizeof(profile_path), "%s%cprofile.json", profile_dir, separator);
    if (written < 0 || (size_t)written >= sizeof(profile_path)) return false;
    /* The temporary file is a sibling, so replacement is atomic on the host. */
    written = snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", profile_path);
    if (written < 0 || (size_t)written >= sizeof(temporary_path)) return false;

    FILE *f = nk_platform_fopen_private(temporary_path, "wb");
    if (!f) {
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "Experimental profile could not be written to user data.");
        return false;
    }
    bool saved = fwrite(profile, 1, (size_t)profile_len, f) == (size_t)profile_len &&
                 fflush(f) == 0;
    if (fclose(f) != 0) saved = false;
    if (!saved || !nk_manifest_replace_file(temporary_path, profile_path)) {
        nk_manifest_remove_file(temporary_path);
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "Experimental profile could not be committed to user data.");
        return false;
    }
    snprintf(out_profile_id, out_profile_id_len, "%s", profile_id);
    return true;
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} NkJsonWriter;

static bool json_writer_append(NkJsonWriter *writer, const char *format, ...) {
    if (!writer || writer->failed) return false;
    va_list args;
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    int needed = vsnprintf(NULL, 0, format, measure);
    va_end(measure);
    if (needed < 0 || (size_t)needed > SIZE_MAX - writer->length - 1) {
        writer->failed = true;
        va_end(args);
        return false;
    }
    size_t required = writer->length + (size_t)needed + 1;
    if (required > writer->capacity) {
        size_t capacity = writer->capacity ? writer->capacity : 256;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                writer->failed = true;
                va_end(args);
                return false;
            }
            capacity *= 2;
        }
        char *grown = (char *)realloc(writer->data, capacity);
        if (!grown) {
            writer->failed = true;
            va_end(args);
            return false;
        }
        writer->data = grown;
        writer->capacity = capacity;
    }
    vsnprintf(writer->data + writer->length,
              writer->capacity - writer->length, format, args);
    va_end(args);
    writer->length += (size_t)needed;
    return true;
}

static bool json_writer_string(NkJsonWriter *writer, const char *value) {
    if (!json_writer_append(writer, "\"")) return false;
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; p++) {
        switch (*p) {
            case '"': if (!json_writer_append(writer, "\\\"")) return false; break;
            case '\\': if (!json_writer_append(writer, "\\\\")) return false; break;
            case '\b': if (!json_writer_append(writer, "\\b")) return false; break;
            case '\f': if (!json_writer_append(writer, "\\f")) return false; break;
            case '\n': if (!json_writer_append(writer, "\\n")) return false; break;
            case '\r': if (!json_writer_append(writer, "\\r")) return false; break;
            case '\t': if (!json_writer_append(writer, "\\t")) return false; break;
            default:
                if (*p < 0x20) {
                    if (!json_writer_append(writer, "\\u%04x", (unsigned)*p)) return false;
                } else if (!json_writer_append(writer, "%c", *p)) {
                    return false;
                }
                break;
        }
    }
    return json_writer_append(writer, "\"");
}

static bool json_writer_node(NkJsonWriter *writer, const JsonNode *node,
                             unsigned int depth) {
    if (!writer || !node || depth > NK_MANIFEST_MAX_JSON_DEPTH) return false;
    switch (node->type) {
        case JSON_NULL: return json_writer_append(writer, "null");
        case JSON_BOOL: return json_writer_append(writer, "%s", node->u.bool_val ? "true" : "false");
        case JSON_NUMBER:
            if (node->u.num.is_integer) {
                return json_writer_append(writer, "%lld", (long long)node->u.num.int_val);
            }
            return json_writer_append(writer, "%.17g", node->u.num.num_val);
        case JSON_STRING: return json_writer_string(writer, node->u.str_val);
        case JSON_ARRAY:
            if (!json_writer_append(writer, "[")) return false;
            for (size_t i = 0; i < node->u.arr.count; i++) {
                if (i && !json_writer_append(writer, ",")) return false;
                if (!json_writer_node(writer, node->u.arr.items[i], depth + 1)) return false;
            }
            return json_writer_append(writer, "]");
        case JSON_OBJECT:
            if (!json_writer_append(writer, "{")) return false;
            for (size_t i = 0; i < node->u.obj.count; i++) {
                if (i && !json_writer_append(writer, ",")) return false;
                if (!json_writer_string(writer, node->u.obj.members[i].key) ||
                    !json_writer_append(writer, ":") ||
                    !json_writer_node(writer, node->u.obj.members[i].val, depth + 1)) return false;
            }
            return json_writer_append(writer, "}");
    }
    return false;
}

static bool json_writer_canonical_node(NkJsonWriter *writer, const JsonNode *node,
                                       unsigned int depth) {
    if (!writer || !node || depth > NK_MANIFEST_MAX_JSON_DEPTH) return false;
    if (node->type != JSON_OBJECT) return json_writer_node(writer, node, depth);
    size_t *order = NULL;
    if (node->u.obj.count > 1) {
        order = malloc(node->u.obj.count * sizeof(*order));
        if (!order) return false;
        for (size_t i = 0; i < node->u.obj.count; i++) order[i] = i;
        for (size_t i = 1; i < node->u.obj.count; i++) {
            size_t selected = i;
            for (size_t j = i + 1; j < node->u.obj.count; j++) {
                if (strcmp(node->u.obj.members[order[j]].key,
                           node->u.obj.members[order[selected]].key) < 0) {
                    selected = j;
                }
            }
            size_t temporary = order[i];
            order[i] = order[selected];
            order[selected] = temporary;
        }
    }
    bool ok = json_writer_append(writer, "{");
    for (size_t i = 0; ok && i < node->u.obj.count; i++) {
        const JsonMember *member = &node->u.obj.members[order ? order[i] : i];
        if ((i && !json_writer_append(writer, ",")) ||
            !json_writer_string(writer, member->key) ||
            !json_writer_append(writer, ":") ||
            !json_writer_canonical_node(writer, member->val, depth + 1)) {
            ok = false;
        }
    }
    free(order);
    return ok && json_writer_append(writer, "}");
}

static bool package_hash_json_node(const JsonNode *node, char out_hex[65]) {
    if (!node || !out_hex) return false;
    NkJsonWriter writer = {0};
    if (!json_writer_canonical_node(&writer, node, 0) ||
        !json_writer_append(&writer, "\n")) {
        free(writer.data);
        return false;
    }
    NkSha256 ctx;
    nk_sha256_init(&ctx);
    nk_sha256_update(&ctx, (const uint8_t *)writer.data, writer.length);
    uint8_t digest[32];
    nk_sha256_finish(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); i++) {
        out_hex[i * 2] = hex[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[64] = '\0';
    free(writer.data);
    return true;
}

static bool json_nodes_equal(const JsonNode *a, const JsonNode *b) {
    if (!a || !b || a->type != b->type) return a == b;
    switch (a->type) {
        case JSON_NULL: return true;
        case JSON_BOOL: return a->u.bool_val == b->u.bool_val;
        case JSON_NUMBER:
            return a->u.num.is_integer == b->u.num.is_integer &&
                (a->u.num.is_integer ? a->u.num.int_val == b->u.num.int_val
                                     : a->u.num.num_val == b->u.num.num_val);
        case JSON_STRING: return strcmp(a->u.str_val, b->u.str_val) == 0;
        case JSON_ARRAY:
            if (a->u.arr.count != b->u.arr.count) return false;
            for (size_t i = 0; i < a->u.arr.count; i++) {
                if (!json_nodes_equal(a->u.arr.items[i], b->u.arr.items[i])) return false;
            }
            return true;
        case JSON_OBJECT:
            if (a->u.obj.count != b->u.obj.count) return false;
            for (size_t i = 0; i < a->u.obj.count; i++) {
                JsonNode *other = obj_get(b, a->u.obj.members[i].key);
                if (!other || !json_nodes_equal(a->u.obj.members[i].val, other)) return false;
            }
            return true;
    }
    return false;
}

static bool package_check_object(const JsonNode *node, const char *path,
                                 const char * const *allowed,
                                 const char * const *required,
                                 char *error, size_t error_size) {
    if (!check_object_keys(node, path, allowed, required, error, error_size)) return false;
    for (size_t i = 0; i < node->u.obj.count; i++) {
        for (size_t j = i + 1; j < node->u.obj.count; j++) {
            if (strcmp(node->u.obj.members[i].key, node->u.obj.members[j].key) == 0) {
                if (error && error_size) snprintf(error, error_size,
                    "%s: duplicate field '%s'", path, node->u.obj.members[i].key);
                return false;
            }
        }
    }
    return true;
}

static bool package_string(const JsonNode *node, const char **out) {
    if (!node || node->type != JSON_STRING || !node->u.str_val || !node->u.str_val[0]) return false;
    if (out) *out = node->u.str_val;
    return true;
}

static bool package_sha256(const JsonNode *node, const char **out) {
    const char *value = NULL;
    if (!package_string(node, &value) || strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    if (out) *out = value;
    return true;
}

static bool package_number(const JsonNode *node, int64_t expected) {
    return node && node->type == JSON_NUMBER && node->u.num.is_integer &&
           node->u.num.int_val == expected;
}

static bool package_read_json(const char *path, size_t max_bytes,
                              char **out_text, size_t *out_length,
                              char *error, size_t error_size) {
    if (!path || !out_text || !out_length) return false;
    FILE *file = manifest_fopen(path);
    if (!file) {
        if (error && error_size) snprintf(error, error_size, "Could not open %s", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        if (error && error_size) snprintf(error, error_size, "Could not seek %s", path);
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size < 0 || (unsigned long)size > max_bytes ||
        fseek(file, 0, SEEK_SET) != 0) {
        if (error && error_size) snprintf(error, error_size,
            "%s is larger than the supported JSON limit", path);
        fclose(file);
        return false;
    }
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        if (error && error_size) snprintf(error, error_size, "Out of memory reading %s", path);
        fclose(file);
        return false;
    }
    size_t got = fread(text, 1, (size_t)size, file);
    bool ok = got == (size_t)size && !ferror(file);
    fclose(file);
    if (!ok) {
        if (error && error_size) snprintf(error, error_size, "Short read from %s", path);
        free(text);
        return false;
    }
    text[got] = '\0';
    *out_text = text;
    *out_length = got;
    return true;
}

static bool package_join_path(char *out, size_t out_size,
                              const char *root, const char *relative) {
    if (!out || out_size == 0 || !root || !*root || !relative || !*relative) return false;
    size_t root_length = strlen(root);
    char sep = nk_platform_path_separator();
    bool has_sep = root_length > 0 &&
        (root[root_length - 1] == '/' || root[root_length - 1] == '\\');
    int written = snprintf(out, out_size, "%s%s%s", root, has_sep ? "" : (sep == '/' ? "/" : "\\"), relative);
    return written > 0 && (size_t)written < out_size;
}

static bool package_resolved_path(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0) return false;
#if defined(_WIN32) || defined(_WIN64)
    /* Extended-length paths need 32K-character buffers; keep them off the stack. */
    enum { WIDE_PATH_CHARS = 32768 };
    WCHAR *input = (WCHAR *)malloc(sizeof(WCHAR) * WIDE_PATH_CHARS * 2);
    if (!input) return false;
    WCHAR *resolved = input + WIDE_PATH_CHARS;
    bool ok = false;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            input, WIDE_PATH_CHARS) > 0) {
        HANDLE handle = CreateFileW(input, FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD length = GetFinalPathNameByHandleW(handle, resolved, WIDE_PATH_CHARS,
                                                     FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            CloseHandle(handle);
            ok = length > 0 && length < WIDE_PATH_CHARS &&
                 WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, resolved, -1,
                                     out, (int)out_size, NULL, NULL) > 0;
        }
    }
    free(input);
    return ok;
#else
    return nk_platform_absolute_path(path, out, out_size);
#endif
}

static bool package_path_is_within(const char *root, const char *path) {
    char abs_root[NK_MAX_PATH * 2];
    char abs_path[NK_MAX_PATH * 2];
    if (!package_resolved_path(root, abs_root, sizeof(abs_root)) ||
        !package_resolved_path(path, abs_path, sizeof(abs_path))) return false;
    size_t root_length = strlen(abs_root);
    while (root_length > 1 &&
           (abs_root[root_length - 1] == '/' || abs_root[root_length - 1] == '\\')) {
        abs_root[--root_length] = '\0';
    }
    for (size_t i = 0; i < root_length; i++) {
        if (nk_ascii_lower((unsigned char)abs_root[i]) !=
            nk_ascii_lower((unsigned char)abs_path[i])) return false;
    }
    return abs_path[root_length] == '/' || abs_path[root_length] == '\\';
}

static bool package_path_is_symlink(const char *path) {
    if (!path || !*path) return true;
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                           wide, 32768) <= 0) return true;
    DWORD attributes = GetFileAttributesW(wide);
    return attributes == INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat info;
    return lstat(path, &info) != 0 || S_ISLNK(info.st_mode);
#endif
}

static bool package_direct_file(const char *package_root, const char *relative,
                                char *out_path, size_t out_size) {
    if (!relative || !is_valid_portable_path(relative)) return false;
    char joined[NK_MAX_PATH * 2];
    if (!package_join_path(joined, sizeof(joined), package_root, relative) ||
        !nk_platform_file_exists(joined) ||
        package_path_is_symlink(joined) ||
        !package_path_is_within(package_root, joined)) return false;
    char absolute[NK_MAX_PATH * 2];
    if (!nk_platform_absolute_path(joined, absolute, sizeof(absolute)) ||
        strlen(absolute) >= out_size) return false;
    snprintf(out_path, out_size, "%s", absolute);
    return true;
}

static bool package_hash_file(const char *path, char out_hex[65]) {
    FILE *file = manifest_fopen(path);
    if (!file) return false;
    NkSha256 ctx;
    nk_sha256_init(&ctx);
    uint8_t buffer[32768];
    size_t count;
    bool ok = true;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        nk_sha256_update(&ctx, buffer, count);
    }
    if (ferror(file)) ok = false;
    fclose(file);
    if (!ok) return false;
    uint8_t digest[32];
    static const char hex[] = "0123456789abcdef";
    nk_sha256_finish(&ctx, digest);
    for (size_t i = 0; i < sizeof(digest); i++) {
        out_hex[i * 2] = hex[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[64] = '\0';
    return true;
}

static void package_rebuild_reason(char *reason, size_t reason_size,
                                   const char *detail, const char *root,
                                   const char *disc_id) {
    char manifest_path[NK_MAX_PATH * 2];
    char executable_path[NK_MAX_PATH * 2];
    char output_path[NK_MAX_PATH * 2];
    char relative[NK_MAX_DISC_ID_LEN + 32];
    snprintf(relative, sizeof(relative), "cache%cpackages%c%s%cmanifest.json",
             nk_platform_path_separator(), nk_platform_path_separator(), disc_id,
             nk_platform_path_separator());
    (void)package_join_path(manifest_path, sizeof(manifest_path), root, relative);
    snprintf(relative, sizeof(relative), "cache%cpackages%c%s%cselected.elf",
             nk_platform_path_separator(), nk_platform_path_separator(), disc_id,
             nk_platform_path_separator());
    (void)package_join_path(executable_path, sizeof(executable_path), root, relative);
    snprintf(relative, sizeof(relative), "cache%cpackages%c%s%cpackage",
             nk_platform_path_separator(), nk_platform_path_separator(), disc_id,
             nk_platform_path_separator());
    (void)package_join_path(output_path, sizeof(output_path), root, relative);
    if (reason && reason_size) {
        /* The short library command comes first: UI error fields are bounded
           and the path-heavy developer route may be truncated. */
        snprintf(reason, reason_size,
            "%s Cache component/epoch mismatch or incomplete entry (#316). "
            "Build it with: python tools/nk_cli.py build-package %s (#296/#297). "
            "Developer route: python tools/title_codegen_plan.py \"%s\" --package --game-elf \"%s\" --output-dir \"%s\".",
            detail ? detail : "Runtime package needs rebuilding.",
            disc_id, manifest_path, executable_path, output_path);
    }
}

static bool package_check_sha_object(const JsonNode *node, const char *path,
                                     const char **out_sha, char *error,
                                     size_t error_size) {
    static const char * const keys[] = {"sha256", NULL};
    if (!package_check_object(node, path, keys, keys, error, error_size) ||
        !package_sha256(obj_get(node, "sha256"), out_sha)) {
        if (error && error_size && !error[0]) snprintf(error, error_size,
            "%s.sha256 must be a lowercase SHA-256 digest", path);
        return false;
    }
    return true;
}

static bool package_cache_text(const JsonNode *node) {
    return node && node->type == JSON_STRING;
}

static bool package_validate_cache(const JsonNode *package,
                                   const char *input_executable_hash,
                                   char *error, size_t error_size) {
    static const char * const cache_keys[] = {
        "format", "schema_version", "key", "codegen_options",
        "runtime_abi_compatibility", NULL
    };
    static const char * const key_keys[] = {"schema_version", "aot", "native", NULL};
    static const char * const aot_keys[] = {"digest", "components", NULL};
    static const char * const aot_component_keys[] = {
        "executable_sha256", "manifest_sha256", "modules_sha256", "psp_header_sha256",
        "analyzer_codegen_epoch", "analyzer_sha256", "codegen_sha256",
        "codegen_options_sha256", "generated_code_abi_epoch", "runtime_abi_epoch", NULL
    };
    static const char * const native_keys[] = {"digest", "components", NULL};
    static const char * const native_component_keys[] = {
        "generated_code_digest", "compiler_identity", "compiler_target",
        "runtime_source_digest", "compile_flags", "link_flags", "runtime_abi_epoch", NULL
    };
    static const char * const compatibility_keys[] = {
        "current_epoch", "generated_code_reusable", NULL
    };
    const JsonNode *cache = obj_get(package, "cache");
    const JsonNode *key = obj_get(cache, "key");
    const JsonNode *aot = obj_get(key, "aot");
    const JsonNode *native = obj_get(key, "native");
    const JsonNode *components = obj_get(aot, "components");
    const JsonNode *native_components = obj_get(native, "components");
    const JsonNode *inputs = obj_get(package, "inputs");
    const char *value = NULL;
    const char *aot_digest = NULL;
    const char *native_digest = NULL;
    const char *manifest_digest = NULL;
    const char *codegen_options_digest = NULL;
    const char *modules_digest = NULL;
    char computed_digest[65];
    if (!package_check_object(cache, "$.cache", cache_keys, cache_keys, error, error_size) ||
        !package_check_object(key, "$.cache.key", key_keys, key_keys, error, error_size) ||
        !package_check_object(aot, "$.cache.key.aot", aot_keys, aot_keys, error, error_size) ||
        !package_check_object(native, "$.cache.key.native", native_keys, native_keys, error, error_size) ||
        !package_check_object(components, "$.cache.key.aot.components", aot_component_keys,
                              aot_component_keys, error, error_size) ||
        !package_check_object(native_components, "$.cache.key.native.components",
                              native_component_keys, native_component_keys, error, error_size)) return false;
    if (!package_string(obj_get(cache, "format"), &value) ||
        strcmp(value, "nakagawa-aot-cache") != 0 ||
        !package_number(obj_get(cache, "schema_version"), NK_AOT_CACHE_SCHEMA_VERSION) ||
        !package_number(obj_get(key, "schema_version"), NK_AOT_CACHE_SCHEMA_VERSION) ||

        obj_get(cache, "codegen_options")->type != JSON_OBJECT ||
        !package_check_object(obj_get(cache, "runtime_abi_compatibility"),
                              "$.cache.runtime_abi_compatibility", compatibility_keys,
                              compatibility_keys, error, error_size) ||
        !package_number(obj_get(obj_get(cache, "runtime_abi_compatibility"), "current_epoch"),
                        NK_AOT_RUNTIME_ABI_EPOCH) ||
        obj_get(obj_get(cache, "runtime_abi_compatibility"), "generated_code_reusable")->type != JSON_BOOL) {
        snprintf(error, error_size, "package cache metadata does not match the v1 cache contract");
        return false;
    }
    if (!inputs || inputs->type != JSON_OBJECT ||
        !package_sha256(obj_get(aot, "digest"), &aot_digest) ||
        !package_sha256(obj_get(native, "digest"), &native_digest) ||
        !package_hash_json_node(components, computed_digest) ||
        strcmp(computed_digest, aot_digest) != 0 ||
        !package_hash_json_node(native_components, computed_digest) ||
        strcmp(computed_digest, native_digest) != 0 ||
        !package_sha256(obj_get(components, "executable_sha256"), &value) ||
        strcmp(value, input_executable_hash) != 0 ||
        !package_sha256(obj_get(components, "manifest_sha256"), &manifest_digest) ||
        !package_sha256(obj_get(obj_get(inputs, "manifest"), "sha256"), NULL) ||
        strcmp(manifest_digest, obj_get(obj_get(inputs, "manifest"), "sha256")->u.str_val) != 0 ||
        !package_sha256(obj_get(components, "modules_sha256"), &modules_digest) ||
        !package_hash_json_node(obj_get(inputs, "modules"), computed_digest) ||
        strcmp(computed_digest, modules_digest) != 0 ||
        !package_string(obj_get(components, "analyzer_codegen_epoch"), NULL) ||
        !package_sha256(obj_get(components, "analyzer_sha256"), NULL) ||
        !package_sha256(obj_get(components, "codegen_sha256"), NULL) ||
        !package_sha256(obj_get(components, "codegen_options_sha256"), &codegen_options_digest) ||
        !package_hash_json_node(obj_get(cache, "codegen_options"), computed_digest) ||
        strcmp(computed_digest, codegen_options_digest) != 0 ||
        !package_number(obj_get(components, "generated_code_abi_epoch"),
                        NK_AOT_GENERATED_CODE_ABI_EPOCH) ||
        !package_number(obj_get(components, "runtime_abi_epoch"), NK_AOT_RUNTIME_ABI_EPOCH)) {
        snprintf(error, error_size, "package cache AOT key is invalid or incompatible");
        return false;
    }
    const JsonNode *input_psp_header = obj_get(inputs, "psp_header");
    const JsonNode *key_psp_header = obj_get(components, "psp_header_sha256");
    if ((!input_psp_header || input_psp_header->type != JSON_NULL) &&
        (!key_psp_header || key_psp_header->type != JSON_STRING ||
         !package_sha256(key_psp_header, NULL) ||
         !input_psp_header || input_psp_header->type != JSON_OBJECT ||
         !package_sha256(obj_get(input_psp_header, "sha256"), NULL) ||
         strcmp(key_psp_header->u.str_val, obj_get(input_psp_header, "sha256")->u.str_val) != 0)) {
        snprintf(error, error_size, "package PSP header digest disagrees with cache key");
        return false;
    }
    if ((input_psp_header && input_psp_header->type == JSON_NULL) &&
        (!key_psp_header || key_psp_header->type != JSON_NULL)) {
        snprintf(error, error_size, "package PSP header digest disagrees with cache key");
        return false;
    }
    if (!package_sha256(obj_get(native_components, "generated_code_digest"), NULL) ||
        !package_cache_text(obj_get(native_components, "compiler_identity")) ||
        !package_cache_text(obj_get(native_components, "compiler_target")) ||
        !package_sha256(obj_get(native_components, "runtime_source_digest"), NULL) ||
        !package_cache_text(obj_get(native_components, "compile_flags")) ||
        !package_cache_text(obj_get(native_components, "link_flags")) ||
        !package_number(obj_get(native_components, "runtime_abi_epoch"), NK_AOT_RUNTIME_ABI_EPOCH)) {
        snprintf(error, error_size, "package cache native key is invalid or incompatible");
        return false;
    }
    return true;
}

static bool package_validate_contract(const JsonNode *package,
                                      const char *expected_title_id,
                                      uint32_t player_abi_version,
                                      const char **exe_relative,
                                      const char **exe_hash,
                                      const char **input_exe_hash,
                                      char *error, size_t error_size) {
    static const char * const root_keys[] = {
        "format", "schema_version", "title", "inputs", "runtime", "executable", "cache",
        "generated_objects", "required_local_assets", "build_report", NULL
    };
    static const char * const title_keys[] = {
        "id", "display_name", "kind", "manifest_sha256", "protected_digest", NULL
    };
    static const char * const title_required[] = {
        "id", "display_name", "kind", "manifest_sha256", "protected_digest", NULL
    };
    static const char * const inputs_keys[] = {"manifest", "executable", "modules", "psp_header", NULL};
    static const char * const inputs_required[] = {"manifest", "executable", "modules", "psp_header", NULL};
    static const char * const runtime_keys[] = {
        "abi", "abi_version", "abi_header_sha256", "run_entry", "runtime_contract",
        "runtime_bindings", "required_runtime_bindings", NULL
    };
    static const char * const runtime_required[] = {
        "abi", "abi_version", "abi_header_sha256", "run_entry", "runtime_contract",
        "runtime_bindings", "required_runtime_bindings", NULL
    };
    static const char * const executable_keys[] = {"path", "sha256", "guest_entry", NULL};
    static const char * const executable_required[] = {"path", "sha256", "guest_entry", NULL};
    static const char * const object_keys[] = {"path", "sha256", NULL};
    static const char * const object_required[] = {"path", "sha256", NULL};
    const char *value = NULL;
    if (!package_check_object(package, "$", root_keys, root_keys, error, error_size)) return false;
    if (!package_string(obj_get(package, "format"), &value) || strcmp(value, "nakagawa-aot-package") != 0 ||
        !package_number(obj_get(package, "schema_version"), 1)) {
        snprintf(error, error_size, "package format or schema_version is not v1");
        return false;
    }

    const JsonNode *title = obj_get(package, "title");
    if (!package_check_object(title, "$.title", title_keys, title_required, error, error_size)) return false;
    if (!package_string(obj_get(title, "id"), &value)) {
        snprintf(error, error_size, "$.title.id must be a string");
        return false;
    }
    if (strcmp(value, expected_title_id) != 0) {
        snprintf(error, error_size, "package title identity does not match this disc");
        return false;
    }
    if (!package_string(obj_get(title, "display_name"), NULL) ||
        !package_string(obj_get(title, "kind"), NULL) ||
        !package_sha256(obj_get(title, "manifest_sha256"), NULL) ||
        !package_sha256(obj_get(title, "protected_digest"), NULL)) {
        snprintf(error, error_size, "$.title does not match the v1 package schema");
        return false;
    }

    const JsonNode *inputs = obj_get(package, "inputs");
    if (!package_check_object(inputs, "$.inputs", inputs_keys, inputs_required, error, error_size)) return false;
    const JsonNode *manifest_input = obj_get(inputs, "manifest");
    const JsonNode *executable_input = obj_get(inputs, "executable");
    const char *manifest_hash = NULL;
    if (!package_check_sha_object(manifest_input, "$.inputs.manifest", &manifest_hash, error, error_size) ||
        !package_check_sha_object(executable_input, "$.inputs.executable", input_exe_hash, error, error_size)) return false;
    if (strcmp(manifest_hash, obj_get(title, "manifest_sha256")->u.str_val) != 0) {
        snprintf(error, error_size, "package manifest hash disagrees with its title record");
        return false;
    }
    const JsonNode *modules = obj_get(inputs, "modules");
    if (!modules || modules->type != JSON_ARRAY) {
        snprintf(error, error_size, "$.inputs.modules must be an array");
        return false;
    }
    for (size_t i = 0; i < modules->u.arr.count; i++) {
        const JsonNode *module = modules->u.arr.items[i];
        static const char * const module_keys[] = {"name", "load_address", "sha256", NULL};
        static const char * const module_required[] = {"name", "load_address", "sha256", NULL};
        if (!package_check_object(module, "$.inputs.modules[]", module_keys, module_required, error, error_size) ||
            !package_string(obj_get(module, "name"), NULL) ||
            !package_string(obj_get(module, "load_address"), NULL) ||
            !package_sha256(obj_get(module, "sha256"), NULL)) {
            if (error && error_size && !error[0]) snprintf(error, error_size, "$.inputs.modules contains an invalid record");
            return false;
        }
    }
    const JsonNode *psp_header = obj_get(inputs, "psp_header");
    if (psp_header->type != JSON_NULL &&
        !package_check_sha_object(psp_header, "$.inputs.psp_header", NULL, error, error_size)) return false;
    if (!package_validate_cache(package, *input_exe_hash, error, error_size)) return false;

    const JsonNode *runtime = obj_get(package, "runtime");
    if (!package_check_object(runtime, "$.runtime", runtime_keys, runtime_required, error, error_size)) return false;
    if (!package_string(obj_get(runtime, "abi"), &value) || strcmp(value, "CpuState") != 0 ||
        !package_number(obj_get(runtime, "abi_version"), player_abi_version)) {
        snprintf(error, error_size, "runtime ABI is incompatible with this player build");
        return false;
    }
    if (!package_sha256(obj_get(runtime, "abi_header_sha256"), NULL) ||
        !package_string(obj_get(runtime, "run_entry"), NULL) ||
        (obj_get(runtime, "runtime_contract")->type != JSON_NULL &&
         obj_get(runtime, "runtime_contract")->type != JSON_OBJECT) ||
        obj_get(runtime, "runtime_bindings")->type != JSON_OBJECT ||
        obj_get(runtime, "required_runtime_bindings")->type != JSON_ARRAY) {
        snprintf(error, error_size, "$.runtime does not match the v1 package schema");
        return false;
    }

    const JsonNode *executable = obj_get(package, "executable");
    if (!package_check_object(executable, "$.executable", executable_keys, executable_required, error, error_size)) return false;
    if (!package_string(obj_get(executable, "path"), exe_relative) ||
        !is_valid_portable_path(*exe_relative) ||
        !package_sha256(obj_get(executable, "sha256"), exe_hash) ||
        !package_string(obj_get(executable, "guest_entry"), NULL)) {
        snprintf(error, error_size, "package executable path/hash is invalid or escapes its package");
        return false;
    }

    const JsonNode *objects = obj_get(package, "generated_objects");
    if (!objects || objects->type != JSON_ARRAY) {
        snprintf(error, error_size, "$.generated_objects must be an array");
        return false;
    }
    for (size_t i = 0; i < objects->u.arr.count; i++) {
        const JsonNode *object = objects->u.arr.items[i];
        if (!package_check_object(object, "$.generated_objects[]", object_keys, object_required, error, error_size) ||
            !package_string(obj_get(object, "path"), &value) || !is_valid_portable_path(value) ||
            !package_sha256(obj_get(object, "sha256"), NULL)) {
            if (error && error_size && !error[0]) snprintf(error, error_size, "$.generated_objects contains an invalid record");
            return false;
        }
    }

    const JsonNode *assets = obj_get(package, "required_local_assets");
    if (!assets || assets->type != JSON_ARRAY) {
        snprintf(error, error_size, "$.required_local_assets must be an array");
        return false;
    }
    for (size_t i = 0; i < assets->u.arr.count; i++) {
        const JsonNode *asset = assets->u.arr.items[i];
        static const char * const asset_keys[] = {
            "kind", "path", "name", "required", "provisioning", "load_address",
            "included_in_aot", "sha256", "bundled", "resolution", NULL
        };
        static const char * const asset_required[] = {"kind", "required", NULL};
        const char *kind = NULL;
        const JsonNode *required = NULL;
        if (!package_check_object(asset, "$.required_local_assets[]", asset_keys, asset_required, error, error_size) ||
            !package_string(obj_get(asset, "kind"), &kind)) return false;
        required = obj_get(asset, "required");
        if (!required || required->type != JSON_BOOL) {
            snprintf(error, error_size, "required local asset flag must be boolean");
            return false;
        }
        if ((strcmp(kind, "title-data-root") == 0 || strcmp(kind, "runtime-resource-locator") == 0) &&
            (!package_string(obj_get(asset, "path"), &value) || !is_valid_portable_path(value) ||
             !package_string(obj_get(asset, "provisioning"), NULL))) {
            snprintf(error, error_size, "local data asset record is invalid");
            return false;
        }
        if (strcmp(kind, "guest-prx") == 0 &&
            (!package_string(obj_get(asset, "name"), NULL) ||
             !package_string(obj_get(asset, "load_address"), NULL) ||
             !package_string(obj_get(asset, "provisioning"), NULL) ||
             !obj_get(asset, "included_in_aot") || obj_get(asset, "included_in_aot")->type != JSON_BOOL ||
             (obj_get(asset, "sha256") && !package_sha256(obj_get(asset, "sha256"), NULL)))) {
            snprintf(error, error_size, "guest PRX asset record is invalid");
            return false;
        }
        if (strcmp(kind, "host-runtime-library") == 0 &&
            (!package_string(obj_get(asset, "name"), NULL) ||
             !package_string(obj_get(asset, "resolution"), NULL) ||
             (obj_get(asset, "bundled") && obj_get(asset, "bundled")->type != JSON_BOOL))) {
            snprintf(error, error_size, "host runtime library record is invalid");
            return false;
        }
        if (strcmp(kind, "title-data-root") != 0 &&
            strcmp(kind, "runtime-resource-locator") != 0 &&
            strcmp(kind, "guest-prx") != 0 &&
            strcmp(kind, "host-runtime-library") != 0) {
            snprintf(error, error_size, "unknown required_local_assets kind '%s'", kind);
            return false;
        }
    }

    if (!package_string(obj_get(package, "build_report"), &value) ||
        strcmp(value, "build-report.json") != 0) {
        snprintf(error, error_size, "$.build_report must be build-report.json");
        return false;
    }
    return true;
}

static bool package_validate_build_report(const JsonNode *report,
                                          const char *title_id,
                                          const JsonNode *package_inputs,
                                          const JsonNode *package_cache,
                                          uint32_t player_abi_version,
                                          char *error, size_t error_size) {
    static const char * const allowed_root_keys[] = {
        "format", "schema_version", "title_id", "runtime_abi", "cache", "input_hashes",
        "tools", "coverage", "unsupported", "analysis_diagnostics", "artifacts",
        "backends", "limits", NULL
    };
    static const char * const required_root_keys[] = {
        "format", "schema_version", "title_id", "runtime_abi", "cache", "input_hashes",
        "tools", "coverage", "unsupported", "analysis_diagnostics", "artifacts", NULL
    };
    static const char * const runtime_keys[] = {"name", "version", NULL};
    static const char * const runtime_required[] = {"name", "version", NULL};
    const char *value = NULL;
    if (!package_check_object(report, "build-report", allowed_root_keys, required_root_keys, error, error_size) ||
        !package_string(obj_get(report, "format"), &value) || strcmp(value, "nakagawa-build-report") != 0 ||
        !package_number(obj_get(report, "schema_version"), 1) ||
        !package_string(obj_get(report, "title_id"), &value) || strcmp(value, title_id) != 0) {
        if (error && error_size && !error[0]) snprintf(error, error_size, "build report identity/schema does not match package");
        return false;
    }
    const JsonNode *backends = obj_get(report, "backends");
    if (backends) {
        const char *backend_mode = NULL;
        if (!package_string(backends, &backend_mode) ||
            (strcmp(backend_mode, "public") != 0 && strcmp(backend_mode, "private") != 0)) {
            if (error && error_size) snprintf(error, error_size, "build report backends mode must be 'public' or 'private'");
            return false;
        }
    }
    const JsonNode *limits = obj_get(report, "limits");
    if (limits && limits->type != JSON_ARRAY) {
        if (error && error_size) snprintf(error, error_size, "build report limits must be an array");
        return false;
    }
    const JsonNode *runtime = obj_get(report, "runtime_abi");
    if (!package_check_object(runtime, "build-report.runtime_abi", runtime_keys, runtime_required, error, error_size) ||
        !package_string(obj_get(runtime, "name"), &value) || strcmp(value, "CpuState") != 0 ||
        !package_number(obj_get(runtime, "version"), player_abi_version)) {
        if (error && error_size && !error[0]) snprintf(error, error_size, "build report ABI does not match this player");
        return false;
    }
    if (!json_nodes_equal(obj_get(report, "cache"), package_cache) ||
        !json_nodes_equal(obj_get(report, "input_hashes"), package_inputs) ||
        obj_get(report, "tools")->type != JSON_OBJECT ||
        obj_get(report, "coverage")->type != JSON_OBJECT ||
        obj_get(report, "unsupported")->type != JSON_OBJECT ||
        obj_get(report, "analysis_diagnostics")->type != JSON_ARRAY ||
        obj_get(report, "artifacts")->type != JSON_OBJECT) {
        if (error && error_size) snprintf(error, error_size, "build report inputs or v1 fields do not match package");
        return false;
    }
    const JsonNode *unsupported = obj_get(report, "unsupported");
    if (obj_get(unsupported, "imports") == NULL || obj_get(unsupported, "imports")->type != JSON_ARRAY ||
        obj_get(unsupported, "instructions") == NULL || obj_get(unsupported, "instructions")->type != JSON_ARRAY ||
        obj_get(unsupported, "regions") == NULL || obj_get(unsupported, "regions")->type != JSON_ARRAY) {
        if (error && error_size) snprintf(error, error_size, "build report unsupported boundaries must be arrays");
        return false;
    }
    return true;
}

static bool package_completion_has_path(const JsonNode *artifacts, const char *path) {
    if (!artifacts || artifacts->type != JSON_ARRAY || !path) return false;
    for (size_t i = 0; i < artifacts->u.arr.count; i++) {
        const char *value = NULL;
        const JsonNode *record = artifacts->u.arr.items[i];
        if (package_string(obj_get(record, "path"), &value) && strcmp(value, path) == 0) {
            return true;
        }
    }
    return false;
}

static bool package_validate_completion(const char *package_root,
                                        const JsonNode *package,
                                        const JsonNode *package_cache,
                                        const char *exe_relative,
                                        char *error, size_t error_size) {
    static const char * const allowed_keys[] = {
        "format", "schema_version", "status", "cache_key", "artifacts", "backends", "limits", NULL
    };
    static const char * const required_keys[] = {
        "format", "schema_version", "status", "cache_key", "artifacts", NULL
    };
    static const char * const artifact_keys[] = {"path", "sha256", NULL};
    char completion_path[NK_MAX_PATH * 2];
    char *completion_text = NULL;
    size_t completion_length = 0;
    if (!package_direct_file(package_root, NK_AOT_COMPLETION_MANIFEST,
                             completion_path, sizeof(completion_path)) ||
        !package_read_json(completion_path, 1024u * 1024u,
                           &completion_text, &completion_length, error, error_size)) {
        snprintf(error, error_size, "Runtime package completion manifest is missing or unreadable.");
        return false;
    }
    JsonNode *completion = json_parse(completion_text, completion_length, error, error_size);
    free(completion_text);
    if (!completion) {
        snprintf(error, error_size, "Runtime package completion manifest is malformed.");
        return false;
    }
    const char *value = NULL;
    if (!package_check_object(completion, "completion-manifest", allowed_keys, required_keys,
                              error, error_size) ||
        !package_string(obj_get(completion, "format"), &value) ||
        strcmp(value, "nakagawa-aot-cache-completion") != 0 ||
        !package_number(obj_get(completion, "schema_version"), 1) ||
        !package_string(obj_get(completion, "status"), &value) ||
        strcmp(value, "complete") != 0 ||
        !json_nodes_equal(obj_get(completion, "cache_key"), obj_get(package_cache, "key"))) {
        snprintf(error, error_size, "Runtime package completion manifest does not match the cache key.");
        json_free(completion);
        return false;
    }
    const JsonNode *artifacts = obj_get(completion, "artifacts");
    if (!artifacts || artifacts->type != JSON_ARRAY || artifacts->u.arr.count == 0) {
        snprintf(error, error_size, "Runtime package completion manifest has no artifacts.");
        json_free(completion);
        return false;
    }
    bool package_seen = false;
    bool report_seen = false;
    bool executable_seen = false;
    char image_relative[NK_MAX_PATH];
    const char *extension = strrchr(exe_relative, '.');
    size_t stem_length = extension && extension != exe_relative
        ? (size_t)(extension - exe_relative) : strlen(exe_relative);
    int image_length = snprintf(image_relative, sizeof(image_relative),
                                "%.*s_image.bin", (int)stem_length, exe_relative);
    for (size_t i = 0; i < artifacts->u.arr.count; i++) {
        const JsonNode *record = artifacts->u.arr.items[i];
        const char *relative = NULL;
        const char *expected_hash = NULL;
        if (!package_check_object(record, "completion-manifest.artifacts[]", artifact_keys,
                                  artifact_keys, error, error_size) ||
            !package_string(obj_get(record, "path"), &relative) ||
            !is_valid_portable_path(relative) ||
            !package_sha256(obj_get(record, "sha256"), &expected_hash)) {
            if (error && error_size && !error[0]) snprintf(error, error_size,
                "Runtime package completion manifest contains an invalid artifact path near '%s'.",
                relative ? relative : "(missing)");
            json_free(completion);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            const char *previous = NULL;
            if (package_string(obj_get(artifacts->u.arr.items[j], "path"), &previous) &&
                strcmp(previous, relative) == 0) {
                snprintf(error, error_size, "Runtime package completion manifest repeats an artifact.");
                json_free(completion);
                return false;
            }
        }
        char artifact_path[NK_MAX_PATH * 2];
        char actual_hash[65];
        if (!package_direct_file(package_root, relative, artifact_path,
                                 sizeof(artifact_path)) ||
            !package_hash_file(artifact_path, actual_hash) ||
            strcmp(actual_hash, expected_hash) != 0) {
            if (strcmp(relative, exe_relative) == 0) {
                snprintf(error, error_size,
                         "Package executable hash is stale; completion artifact digest mismatch: %s.", relative);
            } else {
                snprintf(error, error_size,
                         "Runtime package completion artifact digest mismatch: %s.", relative);
            }
            json_free(completion);
            return false;
        }
        if (strcmp(relative, "package.json") == 0) package_seen = true;
        if (strcmp(relative, "build-report.json") == 0) report_seen = true;
        if (strcmp(relative, exe_relative) == 0) executable_seen = true;
    }
    bool image_seen = image_length > 0 && (size_t)image_length < sizeof(image_relative) &&
                      package_completion_has_path(artifacts, image_relative);
    if (!package_seen || !report_seen || !executable_seen || !image_seen) {
        snprintf(error, error_size,
                 "Runtime package completion manifest does not cover required package artifacts.");
        json_free(completion);
        return false;
    }
    const JsonNode *objects = obj_get(package, "generated_objects");
    if (!objects || objects->type != JSON_ARRAY) {
        snprintf(error, error_size, "Runtime package generated object list is invalid.");
        json_free(completion);
        return false;
    }
    for (size_t i = 0; i < objects->u.arr.count; i++) {
        const char *relative = NULL;
        if (!package_string(obj_get(objects->u.arr.items[i], "path"), &relative) ||
            !package_completion_has_path(artifacts, relative)) {
            snprintf(error, error_size,
                     "Runtime package completion manifest omits a generated object.");
            json_free(completion);
            return false;
        }
    }
    json_free(completion);
    return true;
}

bool nk_title_manifest_read_experimental_profile(
    const char *user_data_root,
    const char *disc_id,
    const char *title_id,
    const char *selected_executable,
    NkTitleEntry *out_title,
    char out_executable_sha256[65],
    char *error_buf,
    size_t error_buf_len
) {
    if (error_buf && error_buf_len) error_buf[0] = '\0';
    if (!user_data_root || !*user_data_root || !out_title || !out_executable_sha256) return false;
    char normalized[10];
    if (!nk_manifest_normalize_disc_id(disc_id, normalized)) {
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "Experimental package has an invalid disc ID.");
        return false;
    }
    char expected_id[64];
    for (size_t i = 0; i < 9; i++) expected_id[13 + i] = (char)tolower((unsigned char)normalized[i]);
    memcpy(expected_id, "experimental-", 13);
    expected_id[22] = '\0';
    if (!title_id || strcmp(title_id, expected_id) != 0) {
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "Experimental title identity does not match disc %s.", normalized);
        return false;
    }

    char relative[NK_MAX_DISC_ID_LEN + 32];
    char profile_path[NK_MAX_PATH * 2];
    snprintf(relative, sizeof(relative), "experimental%c%s%cprofile.json",
             nk_platform_path_separator(), normalized, nk_platform_path_separator());
    if (!package_join_path(profile_path, sizeof(profile_path), user_data_root, relative)) {
        if (error_buf && error_buf_len) snprintf(error_buf, error_buf_len, "Experimental profile path exceeds the supported limit.");
        return false;
    }
    char *profile_text = NULL;
    size_t profile_length = 0;
    if (!package_read_json(profile_path, NK_MANIFEST_MAX_BYTES, &profile_text,
                           &profile_length, error_buf, error_buf_len)) return false;
    JsonNode *profile = json_parse(profile_text, profile_length, error_buf, error_buf_len);
    free(profile_text);
    if (!profile) return false;

    static const char * const profile_keys[] = {"schema_version", "manifest", "input_identity", NULL};
    static const char * const profile_required[] = {"schema_version", "manifest", "input_identity", NULL};
    static const char * const identity_keys[] = {
        "disc_id", "selected_executable", "executable_sha256", "elf_sha256", NULL
    };
    static const char * const identity_required[] = {
        "disc_id", "selected_executable", "executable_sha256", "elf_sha256", NULL
    };
    bool valid = package_check_object(profile, "experimental profile", profile_keys,
                                      profile_required, error_buf, error_buf_len) &&
                 package_number(obj_get(profile, "schema_version"), 1);
    const JsonNode *identity = obj_get(profile, "input_identity");
    if (valid) valid = package_check_object(identity, "experimental input_identity",
                                             identity_keys, identity_required,
                                             error_buf, error_buf_len);
    const char *profile_disc = NULL;
    const char *profile_executable = NULL;
    const char *profile_hash = NULL;
    const char *elf_hash = NULL;
    if (valid) {
        valid = package_string(obj_get(identity, "disc_id"), &profile_disc) &&
                strcmp(profile_disc, normalized) == 0 &&
                package_string(obj_get(identity, "selected_executable"), &profile_executable) &&
                package_sha256(obj_get(identity, "executable_sha256"), &profile_hash) &&
                package_sha256(obj_get(identity, "elf_sha256"), &elf_hash) &&
                strcmp(profile_hash, elf_hash) == 0;
    }
    const char *selected_leaf = selected_executable;
    if (selected_leaf && strrchr(selected_leaf, '/')) selected_leaf = strrchr(selected_leaf, '/') + 1;
    if (valid && (!selected_leaf || !*selected_leaf ||
        !(strcmp(profile_executable, "PSP_GAME/SYSDIR/EBOOT.BIN") == 0 ||
          strcmp(profile_executable, "PSP_GAME/SYSDIR/BOOT.BIN") == 0) ||
        strcmp(strrchr(profile_executable, '/') + 1, selected_leaf) != 0)) {
        valid = false;
    }

    const JsonNode *manifest = obj_get(profile, "manifest");
    NkJsonWriter writer = {0};
    if (valid) valid = json_writer_node(&writer, manifest, 0) && !writer.failed &&
                       writer.length <= NK_MANIFEST_MAX_BYTES;
    if (valid) {
        valid = nk_title_manifest_parse_buffer(writer.data, writer.length, false,
                                                out_title, error_buf, error_buf_len) &&
                out_title->id && strcmp(out_title->id, expected_id) == 0 &&
                out_title->primary_disc_id && strcmp(out_title->primary_disc_id, normalized) == 0;
    }
    if (!valid && error_buf && error_buf_len && !error_buf[0]) {
        snprintf(error_buf, error_buf_len,
                 "Experimental profile is stale or does not match disc %s and selected executable %s.",
                 normalized, selected_leaf ? selected_leaf : "(none)");
    }
    if (valid) snprintf(out_executable_sha256, 65, "%s", profile_hash);
    json_free(profile);
    free(writer.data);
    return valid;
}

NkRuntimePackageStatus nk_title_manifest_validate_aot_package(
    const char *user_data_root,
    const char *disc_id,
    const char *title_id,
    bool is_experimental,
    const char *selected_executable,
    uint32_t player_abi_version,
    NkRuntimePackageInfo *out_info,
    char *reason,
    size_t reason_size
) {
    if (reason && reason_size) reason[0] = '\0';
    if (out_info) memset(out_info, 0, sizeof(*out_info));
    char normalized[10];
    if (!user_data_root || !*user_data_root || !title_id || !*title_id ||
        !nk_manifest_normalize_disc_id(disc_id, normalized)) {
        if (reason && reason_size) snprintf(reason, reason_size, "Library title identity is incomplete.");
        return NK_RUNTIME_PACKAGE_INCOMPATIBLE;
    }
    if (is_experimental) {
        char profile_id[64];
        for (size_t i = 0; i < 9; i++) profile_id[13 + i] = (char)tolower((unsigned char)normalized[i]);
        memcpy(profile_id, "experimental-", 13);
        profile_id[22] = '\0';
        if (strcmp(title_id, profile_id) != 0) {
            package_rebuild_reason(reason, reason_size,
                "Experimental package identity does not match the disc.", user_data_root, normalized);
            return NK_RUNTIME_PACKAGE_STALE;
        }
    } else {
        const NkTitleEntry *by_disc = nk_title_catalog_find_by_disc_id(normalized);
        const NkTitleEntry *by_id = nk_title_catalog_find_by_id(title_id);
        if (!by_disc || !by_id || by_disc != by_id) {
            package_rebuild_reason(reason, reason_size,
                "Package title identity does not match the catalogued disc.", user_data_root, normalized);
            return NK_RUNTIME_PACKAGE_STALE;
        }
    }

    char package_relative[NK_MAX_DISC_ID_LEN + 32];
    char package_root[NK_MAX_PATH * 2];
    char package_path[NK_MAX_PATH * 2];
    snprintf(package_relative, sizeof(package_relative), "packages%c%s",
             nk_platform_path_separator(), normalized);
    if (!package_join_path(package_root, sizeof(package_root), user_data_root, package_relative) ||
        !nk_platform_dir_exists(package_root)) {
        package_rebuild_reason(reason, reason_size, "Runtime package is missing.", user_data_root, normalized);
        return NK_RUNTIME_PACKAGE_MISSING;
    }
    char resolved_package_root[NK_MAX_PATH * 2];
    if (!nk_platform_absolute_path(package_root, resolved_package_root,
                                   sizeof(resolved_package_root)) ||
        !package_path_is_within(user_data_root, resolved_package_root)) {
        package_rebuild_reason(reason, reason_size,
            "Package directory is outside the per-user data directory.",
            user_data_root, normalized);
        return NK_RUNTIME_PACKAGE_INCOMPATIBLE;
    }
    snprintf(package_root, sizeof(package_root), "%s", resolved_package_root);
    if (!package_direct_file(package_root, "package.json", package_path,
                             sizeof(package_path))) {
        package_rebuild_reason(reason, reason_size, "Runtime package package.json is missing.", user_data_root, normalized);
        return NK_RUNTIME_PACKAGE_MISSING;
    }

    char *package_text = NULL;
    size_t package_length = 0;
    char parse_error[320] = "";
    if (!package_read_json(package_path, NK_MANIFEST_MAX_BYTES, &package_text,
                           &package_length, parse_error, sizeof(parse_error))) {
        package_rebuild_reason(reason, reason_size, parse_error, user_data_root, normalized);
        return NK_RUNTIME_PACKAGE_INCOMPATIBLE;
    }
    JsonNode *package = json_parse(package_text, package_length, parse_error, sizeof(parse_error));
    free(package_text);
    if (!package) {
        package_rebuild_reason(reason, reason_size, parse_error, user_data_root, normalized);
        return NK_RUNTIME_PACKAGE_INCOMPATIBLE;
    }

    const char *exe_relative = NULL;
    const char *exe_hash = NULL;
    const char *input_exe_hash = NULL;
    bool contract_ok = package_validate_contract(package, title_id, player_abi_version,
                                                  &exe_relative, &exe_hash,
                                                  &input_exe_hash, parse_error,
                                                  sizeof(parse_error));
    if (!contract_ok) {
        NkRuntimePackageStatus status = strstr(parse_error, "identity")
            ? NK_RUNTIME_PACKAGE_STALE : NK_RUNTIME_PACKAGE_INCOMPATIBLE;
        package_rebuild_reason(reason, reason_size, parse_error, user_data_root, normalized);
        json_free(package);
        return status;
    }
    if (!package_validate_completion(package_root, package, obj_get(package, "cache"),
                                     exe_relative, parse_error, sizeof(parse_error))) {
        package_rebuild_reason(reason, reason_size, parse_error, user_data_root, normalized);
        json_free(package);
        return NK_RUNTIME_PACKAGE_STALE;
    }

    if (is_experimental) {
        NkTitleEntry profile_title;
        char profile_hash[65];
        char profile_error[320] = "";
        if (!nk_title_manifest_read_experimental_profile(
                user_data_root, normalized, title_id, selected_executable,
                &profile_title, profile_hash, profile_error, sizeof(profile_error))) {
            package_rebuild_reason(reason, reason_size, profile_error,
                                   user_data_root, normalized);
            json_free(package);
            return NK_RUNTIME_PACKAGE_STALE;
        }
        if (strcmp(input_exe_hash, profile_hash) != 0) {
            package_rebuild_reason(reason, reason_size,
                "Package executable input hash is stale for the experimental profile.",
                user_data_root, normalized);
            json_free(package);
            return NK_RUNTIME_PACKAGE_STALE;
        }
    }

    char resolved_executable[NK_MAX_PATH];
    if (!package_direct_file(package_root, exe_relative, resolved_executable,
                              sizeof(resolved_executable))) {
        package_rebuild_reason(reason, reason_size,
            "Package executable path is missing or escapes its package directory.",
            user_data_root, normalized);
        json_free(package);
        return NK_RUNTIME_PACKAGE_INCOMPATIBLE;
    }
    char actual_exe_hash[65];
    if (!package_hash_file(resolved_executable, actual_exe_hash) ||
        strcmp(actual_exe_hash, exe_hash) != 0) {
        package_rebuild_reason(reason, reason_size,
            "Package executable hash is stale or the executable was modified.",
            user_data_root, normalized);
        json_free(package);
        return NK_RUNTIME_PACKAGE_STALE;
    }

    char image_relative[NK_MAX_PATH];
    const char *extension = strrchr(exe_relative, '.');
    size_t stem_length = extension && extension != exe_relative
        ? (size_t)(extension - exe_relative) : strlen(exe_relative);
    int image_name_length = snprintf(image_relative, sizeof(image_relative),
                                     "%.*s_image.bin", (int)stem_length, exe_relative);
    char resolved_image[NK_MAX_PATH];
    if (image_name_length <= 0 || (size_t)image_name_length >= sizeof(image_relative) ||
        !package_direct_file(package_root, image_relative, resolved_image,
                             sizeof(resolved_image))) {
        package_rebuild_reason(reason, reason_size,
            "Package runtime image is missing or outside its package directory.",
            user_data_root, normalized);
        json_free(package);
        return NK_RUNTIME_PACKAGE_MISSING;
    }

    char report_path[NK_MAX_PATH];
    char *report_text = NULL;
    size_t report_length = 0;
    if (!package_direct_file(package_root, "build-report.json", report_path,
                             sizeof(report_path)) ||
        !package_read_json(report_path, NK_MANIFEST_MAX_BYTES, &report_text,
                           &report_length, parse_error, sizeof(parse_error))) {
        package_rebuild_reason(reason, reason_size, "Package build-report.json is missing or unreadable.", user_data_root, normalized);
        json_free(package);
        return NK_RUNTIME_PACKAGE_MISSING;
    }
    JsonNode *report = json_parse(report_text, report_length, parse_error, sizeof(parse_error));
    free(report_text);
    if (!report || !package_validate_build_report(
            report, title_id, obj_get(package, "inputs"), obj_get(package, "cache"),
            player_abi_version,
            parse_error, sizeof(parse_error))) {
        package_rebuild_reason(reason, reason_size,
            parse_error[0] ? parse_error : "Package build report is invalid.",
            user_data_root, normalized);
        if (report) json_free(report);
        json_free(package);
        return NK_RUNTIME_PACKAGE_STALE;
    }
    json_free(report);
    json_free(package);

    if (out_info) {
        if (strlen(package_root) >= sizeof(out_info->package_root) ||
            strlen(resolved_executable) >= sizeof(out_info->executable_path) ||
            strlen(resolved_image) >= sizeof(out_info->image_path)) {
            package_rebuild_reason(reason, reason_size, "Package paths exceed the player path limit.", user_data_root, normalized);
            return NK_RUNTIME_PACKAGE_INCOMPATIBLE;
        }
        memcpy(out_info->package_root, package_root, strlen(package_root) + 1);
        memcpy(out_info->executable_path, resolved_executable, strlen(resolved_executable) + 1);
        memcpy(out_info->image_path, resolved_image, strlen(resolved_image) + 1);
    }
    if (reason && reason_size) snprintf(reason, reason_size, "Runtime package v1 is valid for %s.", normalized);
    return NK_RUNTIME_PACKAGE_OK;
}
