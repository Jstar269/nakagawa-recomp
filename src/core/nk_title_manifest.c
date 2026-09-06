/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_title_manifest.h"
#include "nk_platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#define MAX_MODULES 32
#define MAX_COMPAT_DISC_IDS 16
#define SR_DISPATCH_VFPU_TAG  0x40000000U
#define SR_DISPATCH_VFPU_MASK 0xFC000000U

/* Static storage for the in-memory registered overlay */
static struct {
    char id[64];
    char display_name[128];
    NkTitleKind kind;
    char primary_disc_id[16];
    char compat_ids_storage[MAX_COMPAT_DISC_IDS][16];
    const char *compat_id_ptrs[MAX_COMPAT_DISC_IDS + 1];
    uint32_t executable_base;
    uint32_t executable_entry;
    char data_root[256];
    char memory_stick_root[128];
    char hle_profile[64];
    char codegen_profile[64];
    uint32_t expected_data_file_count;
    bool requires_font_firmware;
    bool requires_psmf;
    NkModuleDefinition modules[MAX_MODULES];
    char module_names[MAX_MODULES][64];
    int module_count;
    NkTitleEntry entry;
} s_overlay_storage;

static FILE *manifest_fopen(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wpath[32768];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 32768);
    return _wfopen(wpath, L"rb");
#else
    return fopen(path, "rb");
#endif
}

/* -----------------------------------------------------------------------------
 * UTF-8 Validator
 * -------------------------------------------------------------------------- */
static bool validate_utf8(const uint8_t *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = s[i];
        if (c <= 0x7F) {
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            if (c < 0xC2 || i + 1 >= len) return false;
            if ((s[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len) return false;
            uint8_t c1 = s[i + 1];
            uint8_t c2 = s[i + 2];
            if (c == 0xE0 && (c1 < 0xA0 || c1 > 0xBF)) return false;
            else if (c == 0xED && (c1 < 0x80 || c1 > 0x9F)) return false; /* surrogate halves */
            else if ((c1 & 0xC0) != 0x80) return false;
            if ((c2 & 0xC0) != 0x80) return false;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= len) return false;
            uint8_t c1 = s[i + 1];
            uint8_t c2 = s[i + 2];
            uint8_t c3 = s[i + 3];
            if (c == 0xF0 && (c1 < 0x90 || c1 > 0xBF)) return false;
            else if (c == 0xF4 && (c1 < 0x80 || c1 > 0x8F)) return false;
            else if ((c1 & 0xC0) != 0x80) return false;
            if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

/* -----------------------------------------------------------------------------
 * AST-Based Strict JSON Parser
 * -------------------------------------------------------------------------- */
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonNode JsonNode;

typedef struct {
    char *key;
    JsonNode *val;
} JsonMember;

struct JsonNode {
    JsonType type;
    union {
        bool bool_val;
        double num_val;
        char *str_val;
        struct {
            JsonNode **items;
            size_t count;
            size_t capacity;
        } arr;
        struct {
            JsonMember *members;
            size_t count;
            size_t capacity;
        } obj;
    } u;
};

static void json_free(JsonNode *node) {
    if (!node) return;
    if (node->type == JSON_STRING) {
        free(node->u.str_val);
    } else if (node->type == JSON_ARRAY) {
        for (size_t i = 0; i < node->u.arr.count; i++) {
            json_free(node->u.arr.items[i]);
        }
        free(node->u.arr.items);
    } else if (node->type == JSON_OBJECT) {
        for (size_t i = 0; i < node->u.obj.count; i++) {
            free(node->u.obj.members[i].key);
            json_free(node->u.obj.members[i].val);
        }
        free(node->u.obj.members);
    }
    free(node);
}

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    char error[256];
} JsonParser;

static void set_error(JsonParser *p, const char *msg) {
    if (p->error[0] == '\0') {
        snprintf(p->error, sizeof(p->error), "%s at offset %zu", msg, p->pos);
    }
}

static void skip_whitespace(JsonParser *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static char peek_char(JsonParser *p) {
    skip_whitespace(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos];
}

static JsonNode *parse_value(JsonParser *p, int depth);

static char *parse_json_string(JsonParser *p) {
    if (peek_char(p) != '"') {
        set_error(p, "Expected string quote");
        return NULL;
    }
    p->pos++; /* Skip opening quote */

    size_t cap = 64;
    char *buf = (char *)malloc(cap);
    size_t out_len = 0;

    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') {
            buf[out_len] = '\0';
            return buf;
        }
        if ((unsigned char)c < 0x20) {
            set_error(p, "Unescaped control character in string");
            free(buf);
            return NULL;
        }
        if (c == '\\') {
            if (p->pos >= p->len) {
                set_error(p, "Unterminated escape sequence");
                free(buf);
                return NULL;
            }
            char esc = p->src[p->pos++];
            if (esc == '"') c = '"';
            else if (esc == '\\') c = '\\';
            else if (esc == '/') c = '/';
            else if (esc == 'b') c = '\b';
            else if (esc == 'f') c = '\f';
            else if (esc == 'n') c = '\n';
            else if (esc == 'r') c = '\r';
            else if (esc == 't') c = '\t';
            else if (esc == 'u') {
                if (p->pos + 4 > p->len) {
                    set_error(p, "Incomplete unicode escape");
                    free(buf);
                    return NULL;
                }
                unsigned int codepoint = 0;
                for (int h = 0; h < 4; h++) {
                    char hc = p->src[p->pos++];
                    codepoint <<= 4;
                    if (hc >= '0' && hc <= '9') codepoint |= (hc - '0');
                    else if (hc >= 'a' && hc <= 'f') codepoint |= (hc - 'a' + 10);
                    else if (hc >= 'A' && hc <= 'F') codepoint |= (hc - 'A' + 10);
                    else {
                        set_error(p, "Invalid hex in unicode escape");
                        free(buf);
                        return NULL;
                    }
                }
                c = (codepoint <= 0x7F) ? (char)codepoint : '?';
            } else {
                set_error(p, "Invalid escape sequence");
                free(buf);
                return NULL;
            }
        }

        if (out_len + 2 >= cap) {
            cap *= 2;
            char *new_buf = (char *)realloc(buf, cap);
            if (!new_buf) {
                free(buf);
                return NULL;
            }
            buf = new_buf;
        }
        buf[out_len++] = c;
    }

    set_error(p, "Unterminated string literal");
    free(buf);
    return NULL;
}

static JsonNode *parse_object(JsonParser *p, int depth) {
    if (depth > NK_MANIFEST_MAX_JSON_DEPTH) {
        snprintf(p->error, sizeof(p->error), "JSON nesting exceeds maximum depth %d", NK_MANIFEST_MAX_JSON_DEPTH);
        return NULL;
    }
    p->pos++; /* Skip '{' */

    JsonNode *node = (JsonNode *)calloc(1, sizeof(JsonNode));
    node->type = JSON_OBJECT;

    while (true) {
        char c = peek_char(p);
        if (c == '}') {
            p->pos++;
            return node;
        }
        if (c == '\0') {
            set_error(p, "Unterminated object");
            json_free(node);
            return NULL;
        }

        char *key = parse_json_string(p);
        if (!key) {
            json_free(node);
            return NULL;
        }

        /* Detect duplicate key */
        for (size_t i = 0; i < node->u.obj.count; i++) {
            if (strcmp(node->u.obj.members[i].key, key) == 0) {
                snprintf(p->error, sizeof(p->error), "$: duplicate JSON object key: '%s'", key);
                free(key);
                json_free(node);
                return NULL;
            }
        }

        if (peek_char(p) != ':') {
            set_error(p, "Expected ':' after object key");
            free(key);
            json_free(node);
            return NULL;
        }
        p->pos++; /* Skip ':' */

        JsonNode *val = parse_value(p, depth + 1);
        if (!val) {
            free(key);
            json_free(node);
            return NULL;
        }

        if (node->u.obj.count >= node->u.obj.capacity) {
            size_t new_cap = node->u.obj.capacity ? node->u.obj.capacity * 2 : 8;
            JsonMember *new_mem = (JsonMember *)realloc(node->u.obj.members, new_cap * sizeof(JsonMember));
            if (!new_mem) {
                free(key);
                json_free(val);
                json_free(node);
                return NULL;
            }
            node->u.obj.members = new_mem;
            node->u.obj.capacity = new_cap;
        }
        node->u.obj.members[node->u.obj.count].key = key;
        node->u.obj.members[node->u.obj.count].val = val;
        node->u.obj.count++;

        char next = peek_char(p);
        if (next == ',') {
            p->pos++;
            continue;
        } else if (next == '}') {
            p->pos++;
            return node;
        } else {
            set_error(p, "Expected ',' or '}' in object");
            json_free(node);
            return NULL;
        }
    }
}

static JsonNode *parse_array(JsonParser *p, int depth) {
    if (depth > NK_MANIFEST_MAX_JSON_DEPTH) {
        snprintf(p->error, sizeof(p->error), "JSON nesting exceeds maximum depth %d", NK_MANIFEST_MAX_JSON_DEPTH);
        return NULL;
    }
    p->pos++; /* Skip '[' */

    JsonNode *node = (JsonNode *)calloc(1, sizeof(JsonNode));
    node->type = JSON_ARRAY;

    while (true) {
        char c = peek_char(p);
        if (c == ']') {
            p->pos++;
            return node;
        }
        if (c == '\0') {
            set_error(p, "Unterminated array");
            json_free(node);
            return NULL;
        }

        JsonNode *item = parse_value(p, depth + 1);
        if (!item) {
            json_free(node);
            return NULL;
        }

        if (node->u.arr.count >= node->u.arr.capacity) {
            size_t new_cap = node->u.arr.capacity ? node->u.arr.capacity * 2 : 8;
            JsonNode **new_items = (JsonNode **)realloc(node->u.arr.items, new_cap * sizeof(JsonNode *));
            if (!new_items) {
                json_free(item);
                json_free(node);
                return NULL;
            }
            node->u.arr.items = new_items;
            node->u.arr.capacity = new_cap;
        }
        node->u.arr.items[node->u.arr.count++] = item;

        char next = peek_char(p);
        if (next == ',') {
            p->pos++;
            continue;
        } else if (next == ']') {
            p->pos++;
            return node;
        } else {
            set_error(p, "Expected ',' or ']' in array");
            json_free(node);
            return NULL;
        }
    }
}

static JsonNode *parse_value(JsonParser *p, int depth) {
    char c = peek_char(p);
    if (c == '{') return parse_object(p, depth);
    if (c == '[') return parse_array(p, depth);
    if (c == '"') {
        char *str = parse_json_string(p);
        if (!str) return NULL;
        JsonNode *node = (JsonNode *)calloc(1, sizeof(JsonNode));
        node->type = JSON_STRING;
        node->u.str_val = str;
        return node;
    }
    if (c == 't') {
        if (p->pos + 4 <= p->len && strncmp(&p->src[p->pos], "true", 4) == 0) {
            p->pos += 4;
            JsonNode *node = (JsonNode *)calloc(1, sizeof(JsonNode));
            node->type = JSON_BOOL;
            node->u.bool_val = true;
            return node;
        }
        set_error(p, "Unexpected token");
        return NULL;
    }
    if (c == 'f') {
        if (p->pos + 5 <= p->len && strncmp(&p->src[p->pos], "false", 5) == 0) {
            p->pos += 5;
            JsonNode *node = (JsonNode *)calloc(1, sizeof(JsonNode));
            node->type = JSON_BOOL;
            node->u.bool_val = false;
            return node;
        }
        set_error(p, "Unexpected token");
        return NULL;
    }
    if (c == 'n') {
        if (p->pos + 4 <= p->len && strncmp(&p->src[p->pos], "null", 4) == 0) {
            p->pos += 4;
            JsonNode *node = (JsonNode *)calloc(1, sizeof(JsonNode));
            node->type = JSON_NULL;
            return node;
        }
        set_error(p, "Unexpected token");
        return NULL;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = p->pos;
        if (c == '-') p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
            set_error(p, "Invalid number");
            return NULL;
        }
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
        if (p->pos < p->len && p->src[p->pos] == '.') {
            p->pos++;
            if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
                set_error(p, "Invalid number float");
                return NULL;
            }
            while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
        }
        if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
            p->pos++;
            if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) p->pos++;
            if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
                set_error(p, "Invalid exponent in number");
                return NULL;
            }
            while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
        }

        char num_buf[64];
        size_t nlen = p->pos - start;
        if (nlen >= sizeof(num_buf)) nlen = sizeof(num_buf) - 1;
        memcpy(num_buf, &p->src[start], nlen);
        num_buf[nlen] = '\0';

        JsonNode *node = (JsonNode *)calloc(1, sizeof(JsonNode));
        node->type = JSON_NUMBER;
        node->u.num_val = strtod(num_buf, NULL);
        return node;
    }

    set_error(p, "Unexpected character");
    return NULL;
}

static JsonNode *json_parse(const char *src, size_t len, char *err_buf, size_t err_len) {
    if (!src) return NULL;
    if (!validate_utf8((const uint8_t *)src, len)) {
        if (err_buf) snprintf(err_buf, err_len, "Invalid UTF-8 encoding in manifest");
        return NULL;
    }

    JsonParser p;
    memset(&p, 0, sizeof(p));
    p.src = src;
    p.len = len;

    JsonNode *root = parse_value(&p, 0);
    if (!root) {
        if (err_buf) snprintf(err_buf, err_len, "%s", p.error);
        return NULL;
    }

    skip_whitespace(&p);
    if (p.pos < p.len) {
        if (err_buf) snprintf(err_buf, err_len, "Trailing garbage after JSON root object at offset %zu", p.pos);
        json_free(root);
        return NULL;
    }

    return root;
}

/* -----------------------------------------------------------------------------
 * Canonical Schema Validation and Field Extraction
 * -------------------------------------------------------------------------- */
static JsonNode *obj_get(const JsonNode *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->u.obj.count; i++) {
        if (strcmp(obj->u.obj.members[i].key, key) == 0) {
            return obj->u.obj.members[i].val;
        }
    }
    return NULL;
}

static bool is_valid_identifier(const char *s) {
    if (!s || !*s) return false;
    size_t len = strlen(s);
    if (len > 64) return false;
    if (!isalnum((unsigned char)s[0])) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
    }
    return true;
}

static bool is_valid_disc_id(const char *s) {
    if (!s || strlen(s) != 9) return false;
    for (int i = 0; i < 4; i++) {
        if (!isupper((unsigned char)s[i])) return false;
    }
    for (int i = 4; i < 9; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static bool parse_uint32(const JsonNode *node, uint32_t *out_val) {
    if (!node) return false;
    if (node->type == JSON_NUMBER) {
        if (node->u.num_val < 0 || node->u.num_val > 4294967295.0) return false;
        *out_val = (uint32_t)node->u.num_val;
        return true;
    }
    if (node->type == JSON_STRING) {
        const char *s = node->u.str_val;
        if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0) {
            char *end = NULL;
            unsigned long long v = strtoull(s, &end, 16);
            if (!end || *end != '\0' || v > 0xFFFFFFFFULL) return false;
            *out_val = (uint32_t)v;
            return true;
        }
        char *end = NULL;
        unsigned long long v = strtoull(s, &end, 10);
        if (!end || *end != '\0' || v > 0xFFFFFFFFULL) return false;
        *out_val = (uint32_t)v;
        return true;
    }
    return false;
}

static bool is_core_reserved_target(uint32_t addr) {
    return (addr & SR_DISPATCH_VFPU_MASK) == SR_DISPATCH_VFPU_TAG;
}

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

    if (root->type != JSON_OBJECT) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$: manifest root must be a JSON object");
        json_free(root);
        return false;
    }

    /* 1. Root keys whitelist */
    static const char * const allowed_root_keys[] = {
        "schema_version", "id", "display_name", "kind", "disc", "executable",
        "modules", "filesystem", "hle_profile", "feature_requirements",
        "compatibility_manifest", "verification_profile", "codegen_profile", "notes",
        "runtime_contract", "profile_zero", "runtime_bindings", NULL
    };

    for (size_t i = 0; i < root->u.obj.count; i++) {
        const char *k = root->u.obj.members[i].key;
        bool found = false;
        for (int a = 0; allowed_root_keys[a]; a++) {
            if (strcmp(k, allowed_root_keys[a]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$: unknown field(s): %s", k);
            json_free(root);
            return false;
        }
    }

    /* 2. Required root keys */
    static const char * const required_root_keys[] = {
        "schema_version", "id", "display_name", "kind", "executable",
        "modules", "filesystem", "hle_profile", "feature_requirements",
        "verification_profile", NULL
    };

    for (int r = 0; required_root_keys[r]; r++) {
        if (!obj_get(root, required_root_keys[r])) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$: missing required field(s): %s", required_root_keys[r]);
            json_free(root);
            return false;
        }
    }

    /* 3. schema_version == 1 */
    JsonNode *sv_node = obj_get(root, "schema_version");
    if (sv_node->type != JSON_NUMBER || (int)sv_node->u.num_val != 1) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$: schema_version must be 1");
        json_free(root);
        return false;
    }

    /* 4. id */
    JsonNode *id_node = obj_get(root, "id");
    if (id_node->type != JSON_STRING || !is_valid_identifier(id_node->u.str_val)) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.id: invalid identifier");
        json_free(root);
        return false;
    }

    /* 5. display_name */
    JsonNode *dn_node = obj_get(root, "display_name");
    if (dn_node->type != JSON_STRING || strlen(dn_node->u.str_val) < 1 || strlen(dn_node->u.str_val) > 128) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.display_name: must be a string of length 1..128");
        json_free(root);
        return false;
    }

    /* 6. kind */
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
        if (error_buf) snprintf(error_buf, error_buf_len, "$.kind: invalid kind '%s'", kind_node->u.str_val);
        json_free(root);
        return false;
    }

    /* 7. disc requirement / prohibition */
    JsonNode *disc_node = obj_get(root, "disc");
    if (kind == NK_TITLE_KIND_RETAIL) {
        if (!disc_node || disc_node->type != JSON_OBJECT) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$: retail kind requires disc object");
            json_free(root);
            return false;
        }
        JsonNode *disc_id_node = obj_get(disc_node, "id");
        if (!disc_id_node || disc_id_node->type != JSON_STRING || !is_valid_disc_id(disc_id_node->u.str_val)) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.disc.id: missing or invalid disc ID");
            json_free(root);
            return false;
        }
    } else {
        if (disc_node) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$: disc object is forbidden for non-retail kind '%s'", kind_node->u.str_val);
            json_free(root);
            return false;
        }
    }

    /* 8. executable */
    JsonNode *exe_node = obj_get(root, "executable");
    if (!exe_node || exe_node->type != JSON_OBJECT) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.executable: must be an object");
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

    /* 9. modules */
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

    /* Check duplicate module names */
    for (size_t i = 0; i < mods_node->u.arr.count; i++) {
        JsonNode *m = mods_node->u.arr.items[i];
        if (!m || m->type != JSON_OBJECT) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu]: item must be an object", i);
            json_free(root);
            return false;
        }
        JsonNode *mname = obj_get(m, "name");
        if (!mname || mname->type != JSON_STRING || !mname->u.str_val[0]) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.modules[%zu].name: missing or empty module name", i);
            json_free(root);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            JsonNode *prev_m = mods_node->u.arr.items[j];
            JsonNode *prev_name = obj_get(prev_m, "name");
            if (prev_name && strcmp(prev_name->u.str_val, mname->u.str_val) == 0) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.modules: duplicate module binding '%s'", mname->u.str_val);
                json_free(root);
                return false;
            }
        }
    }

    /* 10. filesystem */
    JsonNode *fs_node = obj_get(root, "filesystem");
    if (!fs_node || fs_node->type != JSON_OBJECT) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.filesystem: must be an object");
        json_free(root);
        return false;
    }
    JsonNode *dr_node = obj_get(fs_node, "data_root");
    JsonNode *ms_node = obj_get(fs_node, "memory_stick_root");
    if (!dr_node || dr_node->type != JSON_STRING || !ms_node || ms_node->type != JSON_STRING) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.filesystem: missing data_root or memory_stick_root");
        json_free(root);
        return false;
    }

    /* 11. hle_profile & verification_profile */
    JsonNode *hle_node = obj_get(root, "hle_profile");
    if (!hle_node || hle_node->type != JSON_STRING || !is_valid_identifier(hle_node->u.str_val)) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.hle_profile: missing or invalid identifier");
        json_free(root);
        return false;
    }
    JsonNode *vp_node = obj_get(root, "verification_profile");
    if (!vp_node || vp_node->type != JSON_STRING || !is_valid_identifier(vp_node->u.str_val)) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.verification_profile: missing or invalid identifier");
        json_free(root);
        return false;
    }

    /* 12. feature_requirements (unique array) */
    JsonNode *feats_node = obj_get(root, "feature_requirements");
    if (!feats_node || feats_node->type != JSON_ARRAY) {
        if (error_buf) snprintf(error_buf, error_buf_len, "$.feature_requirements: must be an array");
        json_free(root);
        return false;
    }
    for (size_t i = 0; i < feats_node->u.arr.count; i++) {
        JsonNode *fitem = feats_node->u.arr.items[i];
        if (!fitem || fitem->type != JSON_STRING || !is_valid_identifier(fitem->u.str_val)) {
            if (error_buf) snprintf(error_buf, error_buf_len, "$.feature_requirements[%zu]: must be a valid identifier", i);
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
    }

    /* 13. runtime_bindings (validate addresses against reserved VFPU tag) */
    uint32_t fallback_entry = exe_entry;
    uint32_t expected_file_count = 0;
    JsonNode *rb_node = obj_get(root, "runtime_bindings");
    if (rb_node && rb_node->type == JSON_OBJECT) {
        JsonNode *fb_node = obj_get(rb_node, "fallback_entry");
        if (fb_node) {
            if (!parse_uint32(fb_node, &fallback_entry)) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings.fallback_entry: invalid address");
                json_free(root);
                return false;
            }
            if (is_core_reserved_target(fallback_entry)) {
                if (error_buf) snprintf(error_buf, error_buf_len, "0x%08x is inside the core VFPU dispatch-target range", fallback_entry);
                json_free(root);
                return false;
            }
        }
        JsonNode *ef_node = obj_get(rb_node, "expected_data_file_count");
        if (ef_node) {
            if (!parse_uint32(ef_node, &expected_file_count)) {
                if (error_buf) snprintf(error_buf, error_buf_len, "$.runtime_bindings.expected_data_file_count: invalid count");
                json_free(root);
                return false;
            }
        }
    }

    /* 14. Populate in-memory overlay */
    memset(&s_overlay_storage, 0, sizeof(s_overlay_storage));
    snprintf(s_overlay_storage.id, sizeof(s_overlay_storage.id), "%s", id_node->u.str_val);
    snprintf(s_overlay_storage.display_name, sizeof(s_overlay_storage.display_name), "%s", dn_node->u.str_val);
    s_overlay_storage.kind = kind;

    if (disc_node) {
        JsonNode *did = obj_get(disc_node, "id");
        snprintf(s_overlay_storage.primary_disc_id, sizeof(s_overlay_storage.primary_disc_id), "%s", did->u.str_val);

        JsonNode *cr_node = obj_get(disc_node, "compatible_revisions");
        if (cr_node && cr_node->type == JSON_ARRAY) {
            int c_count = 0;
            for (size_t c = 0; c < cr_node->u.arr.count && c < MAX_COMPAT_DISC_IDS; c++) {
                JsonNode *citem = cr_node->u.arr.items[c];
                if (citem && citem->type == JSON_STRING && is_valid_disc_id(citem->u.str_val)) {
                    snprintf(s_overlay_storage.compat_ids_storage[c_count], 16, "%s", citem->u.str_val);
                    s_overlay_storage.compat_id_ptrs[c_count] = s_overlay_storage.compat_ids_storage[c_count];
                    c_count++;
                }
            }
            s_overlay_storage.compat_id_ptrs[c_count] = NULL;
        }
    }

    s_overlay_storage.executable_base = exe_base;
    s_overlay_storage.executable_entry = fallback_entry;
    snprintf(s_overlay_storage.data_root, sizeof(s_overlay_storage.data_root), "%s", dr_node->u.str_val);
    snprintf(s_overlay_storage.memory_stick_root, sizeof(s_overlay_storage.memory_stick_root), "%s", ms_node->u.str_val);
    snprintf(s_overlay_storage.hle_profile, sizeof(s_overlay_storage.hle_profile), "%s", hle_node->u.str_val);

    JsonNode *cg_node = obj_get(root, "codegen_profile");
    if (cg_node && cg_node->type == JSON_STRING) {
        snprintf(s_overlay_storage.codegen_profile, sizeof(s_overlay_storage.codegen_profile), "%s", cg_node->u.str_val);
    } else {
        snprintf(s_overlay_storage.codegen_profile, sizeof(s_overlay_storage.codegen_profile), "none");
    }

    s_overlay_storage.expected_data_file_count = expected_file_count;

    s_overlay_storage.module_count = (int)mods_node->u.arr.count;
    for (int i = 0; i < s_overlay_storage.module_count; i++) {
        JsonNode *m = mods_node->u.arr.items[i];
        JsonNode *mn = obj_get(m, "name");
        snprintf(s_overlay_storage.module_names[i], sizeof(s_overlay_storage.module_names[i]), "%s", mn->u.str_val);
        s_overlay_storage.modules[i].name = s_overlay_storage.module_names[i];

        uint32_t laddr = 0;
        parse_uint32(obj_get(m, "load_address"), &laddr);
        s_overlay_storage.modules[i].load_address = laddr;

        JsonNode *req = obj_get(m, "required");
        s_overlay_storage.modules[i].required = (req && req->type == JSON_BOOL) ? req->u.bool_val : false;

        if (strstr(s_overlay_storage.module_names[i], "font")) s_overlay_storage.requires_font_firmware = true;
        if (strstr(s_overlay_storage.module_names[i], "psmf")) s_overlay_storage.requires_psmf = true;
    }

    s_overlay_storage.entry.id = s_overlay_storage.id;
    s_overlay_storage.entry.display_name = s_overlay_storage.display_name;
    s_overlay_storage.entry.kind = s_overlay_storage.kind;
    s_overlay_storage.entry.primary_disc_id = s_overlay_storage.primary_disc_id[0] ? s_overlay_storage.primary_disc_id : NULL;
    s_overlay_storage.entry.compatible_disc_ids = s_overlay_storage.compat_id_ptrs[0] ? s_overlay_storage.compat_id_ptrs : NULL;
    s_overlay_storage.entry.executable_base = s_overlay_storage.executable_base;
    s_overlay_storage.entry.executable_entry = s_overlay_storage.executable_entry;
    s_overlay_storage.entry.data_root = s_overlay_storage.data_root;
    s_overlay_storage.entry.memory_stick_root = s_overlay_storage.memory_stick_root;
    s_overlay_storage.entry.hle_profile = s_overlay_storage.hle_profile;
    s_overlay_storage.entry.codegen_profile = s_overlay_storage.codegen_profile;
    s_overlay_storage.entry.expected_data_file_count = s_overlay_storage.expected_data_file_count;
    s_overlay_storage.entry.requires_font_firmware = s_overlay_storage.requires_font_firmware;
    s_overlay_storage.entry.requires_psmf = s_overlay_storage.requires_psmf;
    s_overlay_storage.entry.modules = s_overlay_storage.modules;
    s_overlay_storage.entry.module_count = s_overlay_storage.module_count;

    /* 15. Collision Check against public canonical catalog */
    bool collision = false;
    const char *collision_id = NULL;

    for (int i = 0; i < nk_title_catalog_count; i++) {
        const NkTitleEntry *pub = &nk_title_catalog_entries[i];
        if (strcmp(pub->id, s_overlay_storage.entry.id) == 0) {
            collision = true;
            collision_id = pub->id;
            break;
        }
        if (s_overlay_storage.entry.primary_disc_id && pub->primary_disc_id) {
            if (strcmp(pub->primary_disc_id, s_overlay_storage.entry.primary_disc_id) == 0) {
                collision = true;
                collision_id = pub->id;
                break;
            }
        }
    }

    if (collision) {
        if (!allow_override) {
            if (error_buf) {
                snprintf(error_buf, error_buf_len,
                    "Overlay identity '%s' conflicts with public canonical catalog title '%s'; override forbidden in standard mode",
                    s_overlay_storage.entry.id, collision_id);
            }
            json_free(root);
            return false;
        } else {
            fprintf(stderr, "[WARNING] Overriding canonical public title definition for '%s' with external private manifest!\n", s_overlay_storage.entry.id);
        }
    }

    if (out_entry) {
        *out_entry = s_overlay_storage.entry;
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

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0 || fsize > NK_MANIFEST_MAX_BYTES) {
        if (error_buf) {
            snprintf(error_buf, error_buf_len,
                "Manifest file size (%ld bytes) exceeds %s limit of %d bytes",
                fsize, NK_MANIFEST_LIMIT_CLASS, NK_MANIFEST_MAX_BYTES);
        }
        fclose(f);
        return false;
    }

    char *buf = (char *)malloc(fsize + 1);
    if (!buf) {
        if (error_buf) snprintf(error_buf, error_buf_len, "Out of memory allocating manifest buffer");
        fclose(f);
        return false;
    }

    size_t bytes_read = fread(buf, 1, fsize, f);
    fclose(f);
    buf[bytes_read] = '\0';

    NkTitleEntry entry;
    bool ok = nk_title_manifest_parse_buffer(buf, bytes_read, allow_override, &entry, error_buf, error_buf_len);
    free(buf);

    if (ok) {
        nk_title_catalog_register_overlay(&s_overlay_storage.entry);
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
