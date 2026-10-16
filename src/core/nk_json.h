/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_JSON_H
#define NK_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_JSON_MAX_DEPTH 16

typedef enum {
    NK_JSON_NULL,
    NK_JSON_BOOL,
    NK_JSON_NUMBER,
    NK_JSON_STRING,
    NK_JSON_ARRAY,
    NK_JSON_OBJECT
} NkJsonType;

typedef struct NkJsonNode NkJsonNode;

typedef struct {
    char *key;
    NkJsonNode *val;
} NkJsonMember;

typedef struct {
    double num_val;
    int64_t int_val;
    bool is_integer;
} NkJsonNumber;

struct NkJsonNode {
    NkJsonType type;
    union {
        bool bool_val;
        NkJsonNumber num;
        char *str_val;
        struct {
            NkJsonNode **items;
            size_t count;
            size_t capacity;
        } arr;
        struct {
            NkJsonMember *members;
            size_t count;
            size_t capacity;
        } obj;
    } u;
};

/* Compatibility aliases matching original manifest parser names */
#ifndef NK_JSON_NO_LEGACY_ALIASES
typedef NkJsonType JsonType;
typedef NkJsonNode JsonNode;
typedef NkJsonMember JsonMember;
typedef NkJsonNumber JsonNumber;

#define JSON_NULL   NK_JSON_NULL
#define JSON_BOOL   NK_JSON_BOOL
#define JSON_NUMBER NK_JSON_NUMBER
#define JSON_STRING NK_JSON_STRING
#define JSON_ARRAY  NK_JSON_ARRAY
#define JSON_OBJECT NK_JSON_OBJECT
#endif

/* UTF-8 Validation */
bool nk_json_validate_utf8(const uint8_t *s, size_t len);

/* JSON string escapes, shared by every reader that decodes a string literal.
 * `src` points at the byte just after a backslash and `avail` counts the bytes
 * readable from there; at most 11 are ever needed (a surrogate pair). On
 * success returns NULL, stores the decoded code point in *out_cp and the number
 * of bytes consumed after the backslash in *out_len. On a malformed escape
 * returns a static message naming the defect and stores nothing. \u0000 decodes
 * to a NUL byte, as it always has: a C-string consumer sees the value end
 * there, and no bytes are invented. */
const char *nk_json_decode_escape(const char *src, size_t avail,
                                  uint32_t *out_cp, size_t *out_len);

/* Writes the UTF-8 encoding of a Unicode scalar value into out[0..3] and
 * returns its length (1 to 4). */
size_t nk_json_utf8_encode(uint32_t cp, char out[4]);

/* Parsing and Destruction */
NkJsonNode *nk_json_parse(const char *src, size_t len, char *err_buf, size_t err_len);
void nk_json_free(NkJsonNode *node);

/* Object Member Lookup */
NkJsonNode *nk_json_obj_get(const NkJsonNode *obj, const char *key);
NkJsonNode *nk_json_object_get(const NkJsonNode *obj, const char *key);

/* Typed Accessors and Query Helpers */
NkJsonType nk_json_get_type(const NkJsonNode *node);
bool nk_json_is_null(const NkJsonNode *node);
bool nk_json_is_bool(const NkJsonNode *node);
bool nk_json_is_number(const NkJsonNode *node);
bool nk_json_is_string(const NkJsonNode *node);
bool nk_json_is_array(const NkJsonNode *node);
bool nk_json_is_object(const NkJsonNode *node);

const char *nk_json_get_string(const NkJsonNode *node);
bool nk_json_get_bool(const NkJsonNode *node, bool *out_val);
bool nk_json_get_number(const NkJsonNode *node, double *out_val);
bool nk_json_get_int64(const NkJsonNode *node, int64_t *out_val);
bool nk_json_get_uint32(const NkJsonNode *node, uint32_t *out_val);

size_t nk_json_array_count(const NkJsonNode *node);
NkJsonNode *nk_json_array_get(const NkJsonNode *node, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* NK_JSON_H */
