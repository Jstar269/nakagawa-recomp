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

#define SR_DISPATCH_VFPU_TAG  0x40000000U
#define SR_DISPATCH_VFPU_MASK 0xFC000000U

/* Bounded storage for in-memory registered overlays */
typedef struct {
    char id[65];
    char display_name[129];
    NkTitleKind kind;
    char primary_disc_id[17];
    char compat_ids_storage[MAX_COMPAT_DISC_IDS][17];
    const char *compat_id_ptrs[MAX_COMPAT_DISC_IDS + 1];
    uint32_t executable_base;
    uint32_t executable_entry;
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

typedef struct {
    double num_val;
    int64_t int_val;
    bool is_integer;
} JsonNumber;

struct JsonNode {
    JsonType type;
    union {
        bool bool_val;
        JsonNumber num;
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
    if (!buf) {
        set_error(p, "Out of memory allocating string buffer");
        return NULL;
    }
    size_t out_len = 0;

    #define APPEND_CHAR(ch) do { \
        if (out_len + 1 >= cap) { \
            size_t new_cap = cap * 2; \
            char *new_buf = (char *)realloc(buf, new_cap); \
            if (!new_buf) { \
                free(buf); \
                set_error(p, "Out of memory expanding string buffer"); \
                return NULL; \
            } \
            buf = new_buf; \
            cap = new_cap; \
        } \
        buf[out_len++] = (char)(ch); \
    } while (0)

    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') {
            APPEND_CHAR('\0');
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
            if (esc == '"') APPEND_CHAR('"');
            else if (esc == '\\') APPEND_CHAR('\\');
            else if (esc == '/') APPEND_CHAR('/');
            else if (esc == 'b') APPEND_CHAR('\b');
            else if (esc == 'f') APPEND_CHAR('\f');
            else if (esc == 'n') APPEND_CHAR('\n');
            else if (esc == 'r') APPEND_CHAR('\r');
            else if (esc == 't') APPEND_CHAR('\t');
            else if (esc == 'u') {
                if (p->pos + 4 > p->len) {
                    set_error(p, "Incomplete unicode escape");
                    free(buf);
                    return NULL;
                }
                uint32_t cp = 0;
                for (int h = 0; h < 4; h++) {
                    char hc = p->src[p->pos++];
                    cp <<= 4;
                    if (hc >= '0' && hc <= '9') cp |= (uint32_t)(hc - '0');
                    else if (hc >= 'a' && hc <= 'f') cp |= (uint32_t)(hc - 'a' + 10);
                    else if (hc >= 'A' && hc <= 'F') cp |= (uint32_t)(hc - 'A' + 10);
                    else {
                        set_error(p, "Invalid hex in unicode escape");
                        free(buf);
                        return NULL;
                    }
                }
                /* Handle UTF-16 surrogate pairs */
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* High surrogate: must be immediately followed by \uDC00..\uDFFF */
                    if (p->pos + 6 <= p->len && p->src[p->pos] == '\\' && p->src[p->pos + 1] == 'u') {
                        p->pos += 2;
                        uint32_t low_cp = 0;
                        for (int h = 0; h < 4; h++) {
                            char hc = p->src[p->pos++];
                            low_cp <<= 4;
                            if (hc >= '0' && hc <= '9') low_cp |= (uint32_t)(hc - '0');
                            else if (hc >= 'a' && hc <= 'f') low_cp |= (uint32_t)(hc - 'a' + 10);
                            else if (hc >= 'A' && hc <= 'F') low_cp |= (uint32_t)(hc - 'A' + 10);
                            else {
                                set_error(p, "Invalid hex in unicode surrogate low escape");
                                free(buf);
                                return NULL;
                            }
                        }
                        if (low_cp >= 0xDC00 && low_cp <= 0xDFFF) {
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (low_cp - 0xDC00));
                        } else {
                            set_error(p, "Invalid low surrogate in unicode surrogate pair");
                            free(buf);
                            return NULL;
                        }
                    } else {
                        set_error(p, "Unpaired high surrogate in unicode escape");
                        free(buf);
                        return NULL;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    set_error(p, "Unpaired low surrogate in unicode escape");
                    free(buf);
                    return NULL;
                }

                /* Encode valid codepoint to UTF-8 */
                if (cp <= 0x7F) {
                    APPEND_CHAR((uint8_t)cp);
                } else if (cp <= 0x7FF) {
                    APPEND_CHAR((uint8_t)(0xC0 | (cp >> 6)));
                    APPEND_CHAR((uint8_t)(0x80 | (cp & 0x3F)));
                } else if (cp <= 0xFFFF) {
                    APPEND_CHAR((uint8_t)(0xE0 | (cp >> 12)));
                    APPEND_CHAR((uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
                    APPEND_CHAR((uint8_t)(0x80 | (cp & 0x3F)));
                } else if (cp <= 0x10FFFF) {
                    APPEND_CHAR((uint8_t)(0xF0 | (cp >> 18)));
                    APPEND_CHAR((uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
                    APPEND_CHAR((uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
                    APPEND_CHAR((uint8_t)(0x80 | (cp & 0x3F)));
                } else {
                    set_error(p, "Unicode codepoint out of range");
                    free(buf);
                    return NULL;
                }
            } else {
                set_error(p, "Invalid escape sequence");
                free(buf);
                return NULL;
            }
        } else {
            APPEND_CHAR(c);
        }
    }

    #undef APPEND_CHAR

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
    if (!node) {
        set_error(p, "Out of memory allocating JSON object node");
        return NULL;
    }
    node->type = JSON_OBJECT;

    /* A comma is a separator, not a terminator. Returning to the top of the
       loop after consuming one and then accepting '}' made the native parser
       admit {"a":1,}, which Python's json module rejects -- breaking the
       differential parser contract and allowing a manifest that only the
       native overlay loader would accept. */
    bool after_comma = false;

    while (true) {
        char c = peek_char(p);
        if (c == '}') {
            if (after_comma) {
                set_error(p, "Trailing comma before '}' in object");
                json_free(node);
                return NULL;
            }
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
                set_error(p, "Out of memory expanding object members");
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
            after_comma = true;
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
    if (!node) {
        set_error(p, "Out of memory allocating JSON array node");
        return NULL;
    }
    node->type = JSON_ARRAY;

    /* Same state transition as the object parser: a consumed comma must be
       followed by another element, never by the closing bracket. */
    bool after_comma = false;

    while (true) {
        char c = peek_char(p);
        if (c == ']') {
            if (after_comma) {
                set_error(p, "Trailing comma before ']' in array");
                json_free(node);
                return NULL;
            }
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
                set_error(p, "Out of memory expanding array items");
                return NULL;
            }
            node->u.arr.items = new_items;
            node->u.arr.capacity = new_cap;
        }
        node->u.arr.items[node->u.arr.count++] = item;

        char next = peek_char(p);
        if (next == ',') {
            p->pos++;
            after_comma = true;
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
        if (!node) {
            free(str);
            set_error(p, "Out of memory allocating string node");
            return NULL;
        }
        node->type = JSON_STRING;
        node->u.str_val = str;
        return node;
    }
    if (c == 't') {
        if (p->pos + 4 <= p->len && strncmp(&p->src[p->pos], "true", 4) == 0) {
            p->pos += 4;
            JsonNode *node = (JsonNode *)calloc(1, sizeof(JsonNode));
            if (!node) { set_error(p, "Out of memory"); return NULL; }
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
            if (!node) { set_error(p, "Out of memory"); return NULL; }
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
            if (!node) { set_error(p, "Out of memory"); return NULL; }
            node->type = JSON_NULL;
            return node;
        }
        set_error(p, "Unexpected token");
        return NULL;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = p->pos;
        bool has_frac_or_exp = false;
        if (c == '-') p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
            set_error(p, "Invalid number");
            return NULL;
        }
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
        if (p->pos < p->len && p->src[p->pos] == '.') {
            has_frac_or_exp = true;
            p->pos++;
            if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
                set_error(p, "Invalid number float");
                return NULL;
            }
            while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
        }
        if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
            has_frac_or_exp = true;
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
        if (!node) { set_error(p, "Out of memory"); return NULL; }
        node->type = JSON_NUMBER;
        node->u.num.num_val = strtod(num_buf, NULL);
        node->u.num.is_integer = !has_frac_or_exp;
        if (node->u.num.is_integer) {
            char *endptr = NULL;
            node->u.num.int_val = (int64_t)strtoll(num_buf, &endptr, 10);
        } else {
            node->u.num.int_val = 0;
        }
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
        "runtime_contract", "profile_zero", "runtime_bindings", NULL
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
    static const char * const allowed_mod_keys[] = {"name", "load_address", "required", "role", NULL};
    for (size_t i = 0; i < mods_node->u.arr.count; i++) {
        JsonNode *m = mods_node->u.arr.items[i];
        char mod_path[64];
        snprintf(mod_path, sizeof(mod_path), "$.modules[%zu]", i);
        if (!check_object_keys(m, mod_path, allowed_mod_keys, allowed_mod_keys, error_buf, error_buf_len)) {
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
    }

    /* 9. filesystem validation */
    JsonNode *fs_node = obj_get(root, "filesystem");
    static const char * const allowed_fs_keys[] = {"data_root", "memory_stick_root", "device_prefixes", NULL};
    if (!check_object_keys(fs_node, "$.filesystem", allowed_fs_keys, allowed_fs_keys, error_buf, error_buf_len)) {
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
    temp.entry.display_name = temp.display_name;
    temp.entry.kind = temp.kind;
    temp.entry.primary_disc_id = temp.primary_disc_id[0] ? temp.primary_disc_id : NULL;
    temp.entry.compatible_disc_ids = temp.compat_id_ptrs[0] ? temp.compat_id_ptrs : NULL;
    temp.entry.executable_base = temp.executable_base;
    temp.entry.executable_entry = temp.executable_entry;
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

    s_overlay_slots[target_slot] = temp;
    /* Re-anchor self pointers for the chosen slot */
    OverlayStorageSlot *dest = &s_overlay_slots[target_slot];
    dest->entry.id = dest->id;
    dest->entry.display_name = dest->display_name;
    dest->entry.kind = dest->kind;
    dest->entry.primary_disc_id = dest->primary_disc_id[0] ? dest->primary_disc_id : NULL;
    for (int c = 0; dest->compat_id_ptrs[c]; c++) {
        dest->compat_id_ptrs[c] = dest->compat_ids_storage[c];
    }
    dest->entry.compatible_disc_ids = dest->compat_id_ptrs[0] ? dest->compat_id_ptrs : NULL;
    dest->entry.executable_base = dest->executable_base;
    dest->entry.executable_entry = dest->executable_entry;
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
