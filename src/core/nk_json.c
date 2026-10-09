/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------------
 * UTF-8 Validator
 * -------------------------------------------------------------------------- */

bool nk_json_validate_utf8(const uint8_t *s, size_t len) {
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
            if (c > 0xF4 || i + 3 >= len) return false;
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
 * AST Deallocation
 * -------------------------------------------------------------------------- */

void nk_json_free(NkJsonNode *node) {
    if (!node) return;
    if (node->type == NK_JSON_STRING) {
        free(node->u.str_val);
    } else if (node->type == NK_JSON_ARRAY) {
        for (size_t i = 0; i < node->u.arr.count; i++) {
            nk_json_free(node->u.arr.items[i]);
        }
        free(node->u.arr.items);
    } else if (node->type == NK_JSON_OBJECT) {
        for (size_t i = 0; i < node->u.obj.count; i++) {
            free(node->u.obj.members[i].key);
            nk_json_free(node->u.obj.members[i].val);
        }
        free(node->u.obj.members);
    }
    free(node);
}

/* -----------------------------------------------------------------------------
 * String escapes (shared by every JSON string reader in the tree)
 * -------------------------------------------------------------------------- */

static int json_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool json_read_hex4(const char *s, size_t avail, uint32_t *out) {
    if (avail < 4) return false;
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        int digit = json_hex_digit(s[i]);
        if (digit < 0) return false;
        value = (value << 4) | (uint32_t)digit;
    }
    *out = value;
    return true;
}

size_t nk_json_utf8_encode(uint32_t cp, char out[4]) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

const char *nk_json_decode_escape(const char *src, size_t avail,
                                  uint32_t *out_cp, size_t *out_len) {
    if (!src || !out_cp || !out_len || avail < 1) return "truncated escape sequence";
    uint32_t cp = 0;
    size_t used = 1;
    switch (src[0]) {
    case '"':  cp = '"';  break;
    case '\\': cp = '\\'; break;
    case '/':  cp = '/';  break;
    case 'b':  cp = '\b'; break;
    case 'f':  cp = '\f'; break;
    case 'n':  cp = '\n'; break;
    case 'r':  cp = '\r'; break;
    case 't':  cp = '\t'; break;
    case 'u': {
        uint32_t unit = 0;
        if (!json_read_hex4(src + 1, avail - 1, &unit)) {
            return "invalid \\u escape: expected four hex digits";
        }
        used = 5;
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return "unpaired low surrogate in \\u escape";
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (avail < 7 || src[5] != '\\' || src[6] != 'u') {
                return "unpaired high surrogate in \\u escape";
            }
            uint32_t low = 0;
            if (!json_read_hex4(src + 7, avail - 7, &low)) {
                return "invalid \\u escape: expected four hex digits";
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                return "high surrogate not followed by a low surrogate";
            }
            cp = 0x10000 + (((unit - 0xD800) << 10) | (low - 0xDC00));
            used = 11;
        } else {
            cp = unit;
        }
        break;
    }
    default:
        return "invalid escape sequence";
    }
    *out_cp = cp;
    *out_len = used;
    return NULL;
}

/* -----------------------------------------------------------------------------
 * Parser State and Internals
 * -------------------------------------------------------------------------- */

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

static NkJsonNode *parse_value(JsonParser *p, int depth);

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
            uint32_t cp = 0;
            size_t used = 0;
            const char *escape_error = nk_json_decode_escape(
                p->src + p->pos, p->len - p->pos, &cp, &used);
            if (escape_error) {
                set_error(p, escape_error);
                free(buf);
                return NULL;
            }
            p->pos += used;
            char encoded[4];
            size_t encoded_len = nk_json_utf8_encode(cp, encoded);
            for (size_t i = 0; i < encoded_len; i++) APPEND_CHAR(encoded[i]);
        } else {
            APPEND_CHAR(c);
        }
    }

    #undef APPEND_CHAR

    set_error(p, "Unterminated string literal");
    free(buf);
    return NULL;
}

static NkJsonNode *parse_object(JsonParser *p, int depth) {
    if (depth > NK_JSON_MAX_DEPTH) {
        snprintf(p->error, sizeof(p->error), "JSON nesting exceeds maximum depth %d", NK_JSON_MAX_DEPTH);
        return NULL;
    }
    p->pos++; /* Skip '{' */

    NkJsonNode *node = (NkJsonNode *)calloc(1, sizeof(NkJsonNode));
    if (!node) {
        set_error(p, "Out of memory allocating JSON object node");
        return NULL;
    }
    node->type = NK_JSON_OBJECT;

    /* A comma is a separator, not a terminator. Consuming a comma and then
       accepting '}' violates strict JSON grammar and would introduce a divergence
       from Python's json module. */
    bool after_comma = false;

    while (true) {
        char c = peek_char(p);
        if (c == '}') {
            if (after_comma) {
                set_error(p, "Trailing comma before '}' in object");
                nk_json_free(node);
                return NULL;
            }
            p->pos++;
            return node;
        }
        if (c == '\0') {
            set_error(p, "Unterminated object");
            nk_json_free(node);
            return NULL;
        }

        char *key = parse_json_string(p);
        if (!key) {
            nk_json_free(node);
            return NULL;
        }

        /* Detect duplicate key */
        for (size_t i = 0; i < node->u.obj.count; i++) {
            if (strcmp(node->u.obj.members[i].key, key) == 0) {
                snprintf(p->error, sizeof(p->error), "$: duplicate JSON object key: '%s'", key);
                free(key);
                nk_json_free(node);
                return NULL;
            }
        }

        if (peek_char(p) != ':') {
            set_error(p, "Expected ':' after object key");
            free(key);
            nk_json_free(node);
            return NULL;
        }
        p->pos++; /* Skip ':' */

        NkJsonNode *val = parse_value(p, depth + 1);
        if (!val) {
            free(key);
            nk_json_free(node);
            return NULL;
        }

        if (node->u.obj.count >= node->u.obj.capacity) {
            size_t new_cap = node->u.obj.capacity ? node->u.obj.capacity * 2 : 8;
            NkJsonMember *new_mem = (NkJsonMember *)realloc(node->u.obj.members, new_cap * sizeof(NkJsonMember));
            if (!new_mem) {
                free(key);
                nk_json_free(val);
                nk_json_free(node);
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
            nk_json_free(node);
            return NULL;
        }
    }
}

static NkJsonNode *parse_array(JsonParser *p, int depth) {
    if (depth > NK_JSON_MAX_DEPTH) {
        snprintf(p->error, sizeof(p->error), "JSON nesting exceeds maximum depth %d", NK_JSON_MAX_DEPTH);
        return NULL;
    }
    p->pos++; /* Skip '[' */

    NkJsonNode *node = (NkJsonNode *)calloc(1, sizeof(NkJsonNode));
    if (!node) {
        set_error(p, "Out of memory allocating JSON array node");
        return NULL;
    }
    node->type = NK_JSON_ARRAY;

    bool after_comma = false;

    while (true) {
        char c = peek_char(p);
        if (c == ']') {
            if (after_comma) {
                set_error(p, "Trailing comma before ']' in array");
                nk_json_free(node);
                return NULL;
            }
            p->pos++;
            return node;
        }
        if (c == '\0') {
            set_error(p, "Unterminated array");
            nk_json_free(node);
            return NULL;
        }

        NkJsonNode *item = parse_value(p, depth + 1);
        if (!item) {
            nk_json_free(node);
            return NULL;
        }

        if (node->u.arr.count >= node->u.arr.capacity) {
            size_t new_cap = node->u.arr.capacity ? node->u.arr.capacity * 2 : 8;
            NkJsonNode **new_items = (NkJsonNode **)realloc(node->u.arr.items, new_cap * sizeof(NkJsonNode *));
            if (!new_items) {
                nk_json_free(item);
                nk_json_free(node);
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
            nk_json_free(node);
            return NULL;
        }
    }
}

static NkJsonNode *parse_value(JsonParser *p, int depth) {
    char c = peek_char(p);
    if (c == '{') return parse_object(p, depth);
    if (c == '[') return parse_array(p, depth);
    if (c == '"') {
        char *str = parse_json_string(p);
        if (!str) return NULL;
        NkJsonNode *node = (NkJsonNode *)calloc(1, sizeof(NkJsonNode));
        if (!node) {
            free(str);
            set_error(p, "Out of memory allocating string node");
            return NULL;
        }
        node->type = NK_JSON_STRING;
        node->u.str_val = str;
        return node;
    }
    if (c == 't') {
        if (p->pos + 4 <= p->len && strncmp(&p->src[p->pos], "true", 4) == 0) {
            p->pos += 4;
            NkJsonNode *node = (NkJsonNode *)calloc(1, sizeof(NkJsonNode));
            if (!node) { set_error(p, "Out of memory"); return NULL; }
            node->type = NK_JSON_BOOL;
            node->u.bool_val = true;
            return node;
        }
        set_error(p, "Unexpected token");
        return NULL;
    }
    if (c == 'f') {
        if (p->pos + 5 <= p->len && strncmp(&p->src[p->pos], "false", 5) == 0) {
            p->pos += 5;
            NkJsonNode *node = (NkJsonNode *)calloc(1, sizeof(NkJsonNode));
            if (!node) { set_error(p, "Out of memory"); return NULL; }
            node->type = NK_JSON_BOOL;
            node->u.bool_val = false;
            return node;
        }
        set_error(p, "Unexpected token");
        return NULL;
    }
    if (c == 'n') {
        if (p->pos + 4 <= p->len && strncmp(&p->src[p->pos], "null", 4) == 0) {
            p->pos += 4;
            NkJsonNode *node = (NkJsonNode *)calloc(1, sizeof(NkJsonNode));
            if (!node) { set_error(p, "Out of memory"); return NULL; }
            node->type = NK_JSON_NULL;
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

        NkJsonNode *node = (NkJsonNode *)calloc(1, sizeof(NkJsonNode));
        if (!node) { set_error(p, "Out of memory"); return NULL; }
        node->type = NK_JSON_NUMBER;
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

/* -----------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

NkJsonNode *nk_json_parse(const char *src, size_t len, char *err_buf, size_t err_len) {
    if (err_buf && err_len > 0) err_buf[0] = '\0';
    if (!src) return NULL;
    if (!nk_json_validate_utf8((const uint8_t *)src, len)) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Invalid UTF-8 encoding in manifest");
        return NULL;
    }

    JsonParser p;
    memset(&p, 0, sizeof(p));
    p.src = src;
    p.len = len;

    NkJsonNode *root = parse_value(&p, 0);
    if (!root) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", p.error);
        return NULL;
    }

    skip_whitespace(&p);
    if (p.pos < p.len) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Trailing garbage after JSON root object at offset %zu", p.pos);
        }
        nk_json_free(root);
        return NULL;
    }

    return root;
}

NkJsonNode *nk_json_obj_get(const NkJsonNode *obj, const char *key) {
    if (!obj || obj->type != NK_JSON_OBJECT || !key) return NULL;
    for (size_t i = 0; i < obj->u.obj.count; i++) {
        if (strcmp(obj->u.obj.members[i].key, key) == 0) {
            return obj->u.obj.members[i].val;
        }
    }
    return NULL;
}

NkJsonNode *nk_json_object_get(const NkJsonNode *obj, const char *key) {
    return nk_json_obj_get(obj, key);
}

/* -----------------------------------------------------------------------------
 * Typed Accessors and Query Helpers
 * -------------------------------------------------------------------------- */

NkJsonType nk_json_get_type(const NkJsonNode *node) {
    return node ? node->type : NK_JSON_NULL;
}

bool nk_json_is_null(const NkJsonNode *node) {
    return node && node->type == NK_JSON_NULL;
}

bool nk_json_is_bool(const NkJsonNode *node) {
    return node && node->type == NK_JSON_BOOL;
}

bool nk_json_is_number(const NkJsonNode *node) {
    return node && node->type == NK_JSON_NUMBER;
}

bool nk_json_is_string(const NkJsonNode *node) {
    return node && node->type == NK_JSON_STRING;
}

bool nk_json_is_array(const NkJsonNode *node) {
    return node && node->type == NK_JSON_ARRAY;
}

bool nk_json_is_object(const NkJsonNode *node) {
    return node && node->type == NK_JSON_OBJECT;
}

const char *nk_json_get_string(const NkJsonNode *node) {
    if (!node || node->type != NK_JSON_STRING) return NULL;
    return node->u.str_val;
}

bool nk_json_get_bool(const NkJsonNode *node, bool *out_val) {
    if (!node || node->type != NK_JSON_BOOL) return false;
    if (out_val) *out_val = node->u.bool_val;
    return true;
}

bool nk_json_get_number(const NkJsonNode *node, double *out_val) {
    if (!node || node->type != NK_JSON_NUMBER) return false;
    if (out_val) *out_val = node->u.num.num_val;
    return true;
}

bool nk_json_get_int64(const NkJsonNode *node, int64_t *out_val) {
    if (!node || node->type != NK_JSON_NUMBER || !node->u.num.is_integer) return false;
    if (out_val) *out_val = node->u.num.int_val;
    return true;
}

bool nk_json_get_uint32(const NkJsonNode *node, uint32_t *out_val) {
    if (!node || node->type != NK_JSON_NUMBER || !node->u.num.is_integer) return false;
    if (node->u.num.int_val < 0 || node->u.num.int_val > 4294967295LL) return false;
    if (out_val) *out_val = (uint32_t)node->u.num.int_val;
    return true;
}

size_t nk_json_array_count(const NkJsonNode *node) {
    if (!node || node->type != NK_JSON_ARRAY) return 0;
    return node->u.arr.count;
}

NkJsonNode *nk_json_array_get(const NkJsonNode *node, size_t index) {
    if (!node || node->type != NK_JSON_ARRAY || index >= node->u.arr.count) return NULL;
    return node->u.arr.items[index];
}
