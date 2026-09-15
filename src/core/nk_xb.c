/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "nk_xb.h"
#include "nk_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#if defined(_MSC_VER)
#define strcasecmp _stricmp
#endif

#define NK_XB_HUFFMAN_LOOKAHEAD_BITS 10u
#define NK_XB_HUFFMAN_TABLE_SIZE (1u << NK_XB_HUFFMAN_LOOKAHEAD_BITS)

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    bool big_endian;
} XbReader;

typedef struct {
    uint32_t expanded_size;
    uint64_t offset;
    NkXbCompression compression;
} XbFstRow;

typedef struct {
    uint64_t offset;
    size_t index;
} XbOffsetIndex;

typedef struct {
    bool valid;
    uint8_t length;
    uint8_t symbol;
} XbHuffmanSymbol;

static void xb_error(char *message, size_t message_size, const char *format, ...) {
    if (!message || message_size == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(message, message_size, format, args);
    va_end(args);
    message[message_size - 1] = '\0';
}

static NkResult xb_fail(char *message, size_t message_size, const char *format, ...) {
    if (message && message_size > 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(message, message_size, format, args);
        va_end(args);
        message[message_size - 1] = '\0';
    }
    return NK_ERROR_INVALID_XB;
}

static bool checked_mul_size(size_t left, size_t right, size_t *out) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *out = left * right;
    return true;
}

static bool reader_require(const XbReader *reader, size_t amount) {
    if (!reader || reader->pos > reader->size) return false;
    return amount <= reader->size - reader->pos;
}

static bool reader_read(XbReader *reader, void *out, size_t amount) {
    if (!reader || !reader_require(reader, amount)) return false;
    if (out && amount > 0) memcpy(out, reader->data + reader->pos, amount);
    reader->pos += amount;
    return true;
}

static bool reader_u8(XbReader *reader, uint8_t *out) {
    return reader_read(reader, out, 1);
}

static bool reader_u16(XbReader *reader, uint16_t *out) {
    uint8_t bytes[2];
    if (!reader_read(reader, bytes, sizeof(bytes))) return false;
    if (reader->big_endian) {
        *out = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    } else {
        *out = (uint16_t)(((uint16_t)bytes[1] << 8) | bytes[0]);
    }
    return true;
}

static bool reader_u32(XbReader *reader, uint32_t *out) {
    uint8_t bytes[4];
    if (!reader_read(reader, bytes, sizeof(bytes))) return false;
    if (reader->big_endian) {
        *out = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
               ((uint32_t)bytes[2] << 8) | bytes[3];
    } else {
        *out = ((uint32_t)bytes[3] << 24) | ((uint32_t)bytes[2] << 16) |
               ((uint32_t)bytes[1] << 8) | bytes[0];
    }
    return true;
}

static bool bytes_are_zero(const uint8_t *data, size_t size) {
    if (!data && size > 0) return false;
    for (size_t i = 0; i < size; i++) {
        if (data[i] != 0) return false;
    }
    return true;
}

static bool reader_align_zero(XbReader *reader, size_t alignment) {
    if (!reader || alignment == 0 || (alignment & (alignment - 1)) != 0) return false;
    size_t padding = (alignment - (reader->pos & (alignment - 1))) & (alignment - 1);
    if (!reader_require(reader, padding)) return false;
    if (!bytes_are_zero(reader->data + reader->pos, padding)) return false;
    reader->pos += padding;
    return true;
}

static bool sjis_lead(uint8_t value) {
    return (value >= 0x81u && value <= 0x9fu) ||
           (value >= 0xe0u && value <= 0xfcu);
}

static bool sjis_trail(uint8_t value) {
    return (value >= 0x40u && value <= 0x7eu) ||
           (value >= 0x80u && value <= 0xfcu && value != 0x7fu);
}

static uint8_t sjis_hash(const uint8_t *data, size_t size) {
    uint8_t value = 0;
    for (size_t i = 0; i < size; i++) {
        value = (uint8_t)((((value & 0x7fu) << 1) | ((value & 0x80u) >> 7)) ^ data[i]);
    }
    return value;
}

static uint32_t path_hash(const char *path) {
    uint32_t value = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        value ^= *p;
        value *= 16777619u;
    }
    return value;
}

/* Keep Shift-JIS bytes byte-for-byte. The native writer accepts UTF-8 paths
 * from the host, but XB names are format bytes; preserving them here avoids a
 * platform-specific codec while still validating every multi-byte sequence.
 * ASCII separators are normalized only when they are standalone bytes. */
static bool normalize_inner_path(const uint8_t *raw, size_t raw_size,
                                 size_t max_name_bytes,
                                 char *out_path, size_t out_size) {
    if (!raw || raw_size == 0 || raw_size > max_name_bytes ||
        !out_path || out_size <= raw_size) return false;

    size_t out_pos = 0;
    size_t component_start = 0;
    for (size_t i = 0; i < raw_size;) {
        uint8_t value = raw[i];
        if (value < 0x20u || value == 0x7fu ||
            value == '<' || value == '>' || value == ':' || value == '"' ||
            value == '|' || value == '?' || value == '*') return false;
        if (sjis_lead(value)) {
            if (i + 1 >= raw_size || !sjis_trail(raw[i + 1])) return false;
            out_path[out_pos++] = (char)value;
            out_path[out_pos++] = (char)raw[i + 1];
            i += 2;
            continue;
        }
        if (value >= 0x80u && value <= 0xa0u) return false;
        if (value >= 0xfdu) return false;

        if (value == '/' || value == '\\') {
            if (out_pos == component_start) return false;
            if (out_pos - component_start == 1 && out_path[component_start] == '.') return false;
            if (out_pos - component_start == 2 && out_path[component_start] == '.' &&
                out_path[component_start + 1] == '.') return false;
            out_path[out_pos++] = '/';
            component_start = out_pos;
        } else {
            out_path[out_pos++] = (char)value;
        }
        i++;
    }

    if (out_pos == 0 || out_pos == component_start) return false;
    if (out_pos - component_start == 1 && out_path[component_start] == '.') return false;
    if (out_pos - component_start == 2 && out_path[component_start] == '.' &&
        out_path[component_start + 1] == '.') return false;
    if (out_pos >= 2 && ((out_path[0] >= 'A' && out_path[0] <= 'Z') ||
                         (out_path[0] >= 'a' && out_path[0] <= 'z')) &&
        out_path[1] == ':') return false;

    out_path[out_pos] = '\0';
    return true;
}

static NkXbLimits normalized_limits(const NkXbLimits *requested) {
    NkXbLimits limits = {
        NK_XB_MAX_ARCHIVE_BYTES,
        NK_XB_MAX_FILES,
        NK_XB_MAX_NAME_BYTES,
        NK_XB_MAX_STRING_TABLE_BYTES,
        NK_XB_MAX_ENTRY_BYTES,
        NK_XB_MAX_TOTAL_EXPANDED_BYTES,
        NK_XB_MAX_HUFFMAN_DEPTH,
        NK_XB_MAX_HUFFMAN_CODES
    };
    if (!requested) return limits;
    if (requested->max_archive_bytes > 0 && requested->max_archive_bytes < limits.max_archive_bytes)
        limits.max_archive_bytes = requested->max_archive_bytes;
    if (requested->max_files > 0 && requested->max_files < limits.max_files)
        limits.max_files = requested->max_files;
    if (requested->max_name_bytes > 0 && requested->max_name_bytes < limits.max_name_bytes)
        limits.max_name_bytes = requested->max_name_bytes;
    if (requested->max_string_table_bytes > 0 && requested->max_string_table_bytes < limits.max_string_table_bytes)
        limits.max_string_table_bytes = requested->max_string_table_bytes;
    if (requested->max_entry_bytes > 0 && requested->max_entry_bytes < limits.max_entry_bytes)
        limits.max_entry_bytes = requested->max_entry_bytes;
    if (requested->max_total_expanded_bytes > 0 && requested->max_total_expanded_bytes < limits.max_total_expanded_bytes)
        limits.max_total_expanded_bytes = requested->max_total_expanded_bytes;
    if (requested->max_huffman_depth > 0 && requested->max_huffman_depth < limits.max_huffman_depth)
        limits.max_huffman_depth = requested->max_huffman_depth;
    if (requested->max_huffman_codes > 0 && requested->max_huffman_codes < limits.max_huffman_codes)
        limits.max_huffman_codes = requested->max_huffman_codes;
    return limits;
}

NkXbLimits nk_xb_default_limits(void) {
    return normalized_limits(NULL);
}

static NkResult decode_lzs_body(const uint8_t *payload, size_t payload_size,
                                size_t expanded_size, const NkXbLimits *limits,
                                uint8_t *output, char *error_message,
                                size_t error_message_size) {
    if (!payload || !limits || !output || expanded_size == 0 ||
        expanded_size > limits->max_entry_bytes) {
        return xb_fail(error_message, error_message_size,
                       "invalid or oversized LZS expanded size");
    }

    XbReader reader = { payload, payload_size, 0, false };
    size_t produced = 0;
    while (produced < expanded_size) {
        uint8_t code = 0;
        if (!reader_u8(&reader, &code)) {
            return xb_fail(error_message, error_message_size,
                           "truncated LZS control byte");
        }
        if ((code & 0x03u) == 0) {
            size_t copy_len = ((size_t)code >> 2) + 1u;
            if (copy_len > expanded_size - produced ||
                !reader_require(&reader, copy_len)) {
                return xb_fail(error_message, error_message_size,
                               "LZS literal exceeds expanded size or payload");
            }
            memcpy(output + produced, reader.data + reader.pos, copy_len);
            reader.pos += copy_len;
            produced += copy_len;
            continue;
        }

        uint32_t value = code;
        size_t run_length;
        size_t run_offset;
        if (code & 0x01u) {
            uint8_t b0 = 0;
            if (!reader_u8(&reader, &b0)) {
                return xb_fail(error_message, error_message_size,
                               "truncated LZS short-run distance");
            }
            value = ((uint32_t)b0 << 8) | code;
            run_length = ((value & 0x0eu) >> 1) + 3u;
            run_offset = value >> 4;
        } else {
            uint8_t b0 = 0;
            uint8_t b1 = 0;
            if (!reader_u8(&reader, &b0) || !reader_u8(&reader, &b1)) {
                return xb_fail(error_message, error_message_size,
                               "truncated LZS long-run distance");
            }
            value = ((uint32_t)b1 << 16) | ((uint32_t)b0 << 8) | code;
            run_length = ((value & 0x0ffcu) >> 2) + 3u;
            run_offset = value >> 12;
        }

        if (run_offset == 0 || run_offset > produced) {
            return xb_fail(error_message, error_message_size,
                           "LZS back-reference leaves output");
        }
        if (run_length > expanded_size - produced) {
            return xb_fail(error_message, error_message_size,
                           "LZS run exceeds expanded size");
        }
        size_t source = produced - run_offset;
        for (size_t i = 0; i < run_length; i++) {
            output[produced++] = output[source++];
        }
    }

    return NK_OK;
}

static NkResult decode_huffman_body(const uint8_t *payload, size_t payload_size,
                                    size_t expanded_size, const NkXbLimits *limits,
                                    bool big_endian, uint8_t *output,
                                    char *error_message, size_t error_message_size) {
    if (!payload || !limits || !output || expanded_size == 0 ||
        expanded_size > limits->max_entry_bytes) {
        return xb_fail(error_message, error_message_size,
                       "invalid or oversized Huffman expanded size");
    }

    XbReader reader = { payload, payload_size, 0, big_endian };
    uint8_t max_length = 0;
    if (!reader_u8(&reader, &max_length) || max_length == 0 ||
        max_length > limits->max_huffman_depth ||
        max_length > NK_XB_MAX_HUFFMAN_DEPTH) {
        return xb_fail(error_message, error_message_size,
                       "unsupported or invalid Huffman depth");
    }

    XbHuffmanSymbol table[NK_XB_HUFFMAN_TABLE_SIZE];
    memset(table, 0, sizeof(table));
    uint32_t code = 0;
    size_t code_count = 0;
    for (unsigned length = 1; length <= max_length; length++) {
        uint8_t count = 0;
        if (!reader_u8(&reader, &count)) {
            return xb_fail(error_message, error_message_size,
                           "truncated Huffman code table");
        }
        code_count += count;
        if (code_count > limits->max_huffman_codes ||
            code_count > NK_XB_MAX_HUFFMAN_CODES) {
            return xb_fail(error_message, error_message_size,
                           "Huffman code table is too large");
        }
        for (unsigned n = 0; n < count; n++) {
            if (length <= NK_XB_HUFFMAN_LOOKAHEAD_BITS && code >= (1u << length)) {
                return xb_fail(error_message, error_message_size,
                               "oversubscribed Huffman table");
            }
            uint32_t code_bits = code;
            uint32_t index = 0;
            for (unsigned bit = 0; bit < length; bit++) {
                index = (index << 1) | (code_bits & 1u);
                code_bits >>= 1;
            }
            uint8_t symbol = 0;
            if (!reader_u8(&reader, &symbol)) {
                return xb_fail(error_message, error_message_size,
                               "truncated Huffman symbol table");
            }
            uint32_t stride = 1u << length;
            for (uint32_t slot = index; slot < NK_XB_HUFFMAN_TABLE_SIZE; slot += stride) {
                table[slot].valid = true;
                table[slot].length = (uint8_t)length;
                table[slot].symbol = symbol;
            }
            if (length <= NK_XB_HUFFMAN_LOOKAHEAD_BITS) {
                code++;
            }
        }
        code <<= 1;
    }

    if ((reader.pos & 1u) != 0) {
        if (!reader_require(&reader, 1)) {
            return xb_fail(error_message, error_message_size,
                           "truncated Huffman table alignment");
        }
        reader.pos++;
    }

    uint64_t bit_buffer = 0;
    unsigned bit_count = 0;
    size_t produced = 0;
    while (produced < expanded_size) {
        if (bit_count < NK_XB_HUFFMAN_LOOKAHEAD_BITS) {
            uint16_t word = 0;
            if (!reader_u16(&reader, &word)) {
                return xb_fail(error_message, error_message_size,
                               "truncated Huffman bitstream");
            }
            bit_buffer |= (uint64_t)word << bit_count;
            bit_count += 16;
        }

        XbHuffmanSymbol symbol = table[bit_buffer & (NK_XB_HUFFMAN_TABLE_SIZE - 1u)];
        if (!symbol.valid) {
            return xb_fail(error_message, error_message_size,
                           "Huffman code has no table entry");
        }
        if (symbol.length <= NK_XB_HUFFMAN_LOOKAHEAD_BITS) {
            output[produced++] = symbol.symbol;
            bit_buffer >>= symbol.length;
            bit_count -= symbol.length;
        } else {
            bit_buffer >>= NK_XB_HUFFMAN_LOOKAHEAD_BITS;
            bit_count -= NK_XB_HUFFMAN_LOOKAHEAD_BITS;
            if (bit_count < 16) {
                uint16_t word = 0;
                if (!reader_u16(&reader, &word)) {
                    return xb_fail(error_message, error_message_size,
                                   "truncated Huffman bitstream");
                }
                bit_buffer |= (uint64_t)word << bit_count;
                bit_count += 16;
            }
            output[produced++] = (uint8_t)(bit_buffer & 0xffu);
            bit_buffer >>= 8;
            bit_count -= 8;
        }
    }

    return NK_OK;
}

static NkResult decode_prefixed(const uint8_t *payload, size_t payload_size,
                                NkXbCompression compression, size_t expected_size,
                                const NkXbLimits *limits, bool big_endian,
                                uint8_t *output, char *error_message,
                                size_t error_message_size) {
    if (!payload || !limits || !output || payload_size < 8) {
        return xb_fail(error_message, error_message_size,
                       "compressed entry is missing its size header");
    }
    XbReader reader = { payload, payload_size, 0, big_endian };
    uint32_t expanded_u32 = 0;
    uint32_t compressed_u32 = 0;
    if (!reader_u32(&reader, &expanded_u32) || !reader_u32(&reader, &compressed_u32)) {
        return xb_fail(error_message, error_message_size,
                       "truncated compressed entry size header");
    }
    size_t expanded_size = (size_t)expanded_u32;
    size_t compressed_size = (size_t)compressed_u32;
    if (expanded_size == 0 || expanded_size > limits->max_entry_bytes) {
        return xb_fail(error_message, error_message_size,
                       "invalid or oversized compressed expanded size");
    }
    size_t body_size = payload_size >= 8 ? payload_size - 8 : 0;
    if (compression == NK_XB_COMPRESSION_LZS) {
        if (compressed_size > 0 && compressed_size <= body_size) {
            body_size = compressed_size;
        } else if (compressed_size > 8 && compressed_size - 8 <= body_size) {
            body_size = compressed_size - 8;
        }
    }
    if (!reader_require(&reader, body_size)) {
        return xb_fail(error_message, error_message_size,
                       "compressed payload is truncated");
    }
    const uint8_t *body = reader.data + reader.pos;
    reader.pos += body_size;

    if (compression == NK_XB_COMPRESSION_LZS || compression == NK_XB_COMPRESSION_HUFFMAN) {
        if (expanded_size != expected_size) {
            return xb_fail(error_message, error_message_size,
                           "compressed expanded size disagrees with FST");
        }
        if (compressed_size == 0) {
            memcpy(output, body, expected_size);
            return NK_OK;
        }
        if (compression == NK_XB_COMPRESSION_LZS) {
            return decode_lzs_body(body, body_size, expected_size, limits,
                                   output, error_message, error_message_size);
        }
        return decode_huffman_body(body, body_size, expected_size, limits,
                                   big_endian, output, error_message,
                                   error_message_size);
    }

    if (compression == NK_XB_COMPRESSION_DEFLATE) {
        /* In this archive family tag 0 is the source-owned nested
         * Huffman -> LZS stream documented by xb_probe.py. It is not a zlib
         * stream; using zlib here would accept a different format. */
        uint8_t *intermediate = (uint8_t *)malloc(expanded_size);
        if (!intermediate) {
            return NK_ERROR_OUT_OF_MEMORY;
        }
        NkResult result;
        if (compressed_size == 0) {
            memcpy(intermediate, body, expanded_size);
            result = NK_OK;
        } else {
            result = decode_huffman_body(body, body_size, expanded_size, limits,
                                         big_endian, intermediate, error_message,
                                         error_message_size);
        }
        if (result == NK_OK) {
            result = decode_prefixed(intermediate, expanded_size,
                                     NK_XB_COMPRESSION_LZS, expected_size,
                                     limits, big_endian, output, error_message,
                                     error_message_size);
        }
        free(intermediate);
        return result;
    }

    return xb_fail(error_message, error_message_size,
                   "unsupported XB compression tag");
}

static int offset_index_compare(const void *left, const void *right) {
    const XbOffsetIndex *a = (const XbOffsetIndex *)left;
    const XbOffsetIndex *b = (const XbOffsetIndex *)right;
    if (a->offset < b->offset) return -1;
    if (a->offset > b->offset) return 1;
    return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

static bool entries_content_identical(const NkXbArchive *archive,
                                      const NkXbEntry *a,
                                      const NkXbEntry *b) {
    if (a->expanded_size != b->expanded_size || a->compression != b->compression) {
        return false;
    }
    if (a->offset == b->offset && a->span_size == b->span_size) {
        return true;
    }
    if (a->compression == NK_XB_COMPRESSION_NONE) {
        if (a->offset + a->expanded_size > archive->data_size ||
            b->offset + b->expanded_size > archive->data_size) return false;
        return memcmp(archive->data + a->offset, archive->data + b->offset,
                      (size_t)a->expanded_size) == 0;
    }
    uint8_t *buf_a = (uint8_t *)malloc((size_t)a->expanded_size);
    uint8_t *buf_b = (uint8_t *)malloc((size_t)b->expanded_size);
    if (!buf_a || !buf_b) {
        free(buf_a); free(buf_b);
        return false;
    }
    char err[128];
    NkResult res_a = decode_prefixed(archive->data + a->offset, (size_t)a->span_size,
                                     a->compression, (size_t)a->expanded_size,
                                     &archive->limits, archive->big_endian,
                                     buf_a, err, sizeof(err));
    NkResult res_b = decode_prefixed(archive->data + b->offset, (size_t)b->span_size,
                                     b->compression, (size_t)b->expanded_size,
                                     &archive->limits, archive->big_endian,
                                     buf_b, err, sizeof(err));
    bool identical = (res_a == NK_OK && res_b == NK_OK &&
                      memcmp(buf_a, buf_b, (size_t)a->expanded_size) == 0);
    free(buf_a);
    free(buf_b);
    return identical;
}

static NkResult parse_archive(NkXbArchive *archive, char *error_message,
                              size_t error_message_size) {
    if (!archive || !archive->data || archive->data_size < 8) {
        return xb_fail(error_message, error_message_size,
                       "archive is shorter than its header");
    }

    XbReader reader = { archive->data, archive->data_size, 0, archive->big_endian };
    const uint8_t signature[4] = {
        NK_XB_SIGNATURE_0, NK_XB_SIGNATURE_1,
        NK_XB_SIGNATURE_2, NK_XB_SIGNATURE_3
    };
    uint8_t actual_signature[4];
    if (!reader_read(&reader, actual_signature, sizeof(actual_signature)) ||
        memcmp(actual_signature, signature, sizeof(signature)) != 0) {
        return xb_fail(error_message, error_message_size,
                       "not an XB archive");
    }

    uint32_t file_count_u32 = 0;
    if (!reader_u32(&reader, &file_count_u32) || file_count_u32 == 0 ||
        file_count_u32 > archive->limits.max_files ||
        file_count_u32 > NK_XB_MAX_FILES) {
        return xb_fail(error_message, error_message_size,
                       "invalid or excessive XB file count");
    }
    size_t file_count = (size_t)file_count_u32;
    size_t fst_bytes = 0;
    if (!checked_mul_size(file_count, 8, &fst_bytes) || !reader_require(&reader, fst_bytes)) {
        return xb_fail(error_message, error_message_size,
                       "truncated XB filesystem table");
    }

    XbFstRow *fst = (XbFstRow *)calloc(file_count, sizeof(*fst));
    NkXbEntry *entries = (NkXbEntry *)calloc(file_count, sizeof(*entries));
    XbOffsetIndex *offsets = (XbOffsetIndex *)calloc(file_count, sizeof(*offsets));
    if (!fst || !entries || !offsets) {
        free(fst);
        free(entries);
        free(offsets);
        return NK_ERROR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < file_count; i++) {
        uint32_t expanded_size = 0;
        uint32_t packed = 0;
        if (!reader_u32(&reader, &expanded_size) || !reader_u32(&reader, &packed)) {
            free(fst); free(entries); free(offsets);
            return xb_fail(error_message, error_message_size,
                           "truncated XB filesystem row");
        }
        uint32_t compression_code = packed >> 28;
        if (compression_code > (uint32_t)NK_XB_COMPRESSION_NONE) {
            free(fst); free(entries); free(offsets);
            return xb_fail(error_message, error_message_size,
                           "unknown XB compression tag");
        }
        fst[i].expanded_size = expanded_size;
        fst[i].offset = (uint64_t)(packed & 0x0fffffffu) * 4u;
        fst[i].compression = (NkXbCompression)compression_code;
    }

    uint32_t string_expanded_u32 = 0;
    uint32_t string_compressed_u32 = 0;
    if (!reader_align_zero(&reader, 4) ||
        !reader_u32(&reader, &string_expanded_u32) ||
        !reader_u32(&reader, &string_compressed_u32)) {
        free(fst); free(entries); free(offsets);
        return xb_fail(error_message, error_message_size,
                       "truncated XB string-table header");
    }
    size_t string_expanded = (size_t)string_expanded_u32;
    size_t string_compressed = (size_t)string_compressed_u32;
    if (string_expanded == 0 || string_expanded > archive->limits.max_string_table_bytes ||
        string_expanded > NK_XB_MAX_STRING_TABLE_BYTES) {
        free(fst); free(entries); free(offsets);
        return xb_fail(error_message, error_message_size,
                       "invalid or oversized string-table expanded size");
    }
    if (string_compressed > archive->limits.max_string_table_bytes ||
        string_compressed > NK_XB_MAX_STRING_TABLE_BYTES) {
        free(fst); free(entries); free(offsets);
        return xb_fail(error_message, error_message_size,
                       "oversized string-table compressed size");
    }

    uint8_t *string_table = (uint8_t *)malloc(string_expanded);
    if (!string_table) {
        free(fst); free(entries); free(offsets);
        return NK_ERROR_OUT_OF_MEMORY;
    }
    NkResult result = NK_OK;
    if (string_compressed == 0) {
        if (!reader_read(&reader, string_table, string_expanded)) {
            result = xb_fail(error_message, error_message_size,
                             "truncated XB string table");
        }
    } else {
        NkResult lzs_result = NK_ERROR_GENERIC;
        if (string_compressed > 8 && reader_require(&reader, string_compressed - 8)) {
            size_t retail_body_size = string_compressed - 8;
            lzs_result = decode_lzs_body(reader.data + reader.pos, retail_body_size,
                                         string_expanded, &archive->limits,
                                         string_table, error_message,
                                         error_message_size);
            if (lzs_result == NK_OK) {
                reader.pos += retail_body_size;
            }
        }
        if (lzs_result != NK_OK) {
            if (!reader_require(&reader, string_compressed)) {
                result = xb_fail(error_message, error_message_size,
                                 "truncated compressed XB string table");
            } else {
                result = decode_lzs_body(reader.data + reader.pos, string_compressed,
                                         string_expanded, &archive->limits,
                                         string_table, error_message,
                                         error_message_size);
                if (result == NK_OK) {
                    reader.pos += string_compressed;
                }
            }
        } else {
            result = NK_OK;
        }
    }
    if (result != NK_OK) {
        free(string_table); free(fst); free(entries); free(offsets);
        return result;
    }
    if (!reader_align_zero(&reader, 4)) {
        free(string_table); free(fst); free(entries); free(offsets);
        return xb_fail(error_message, error_message_size,
                       "invalid XB string-table alignment");
    }

    XbReader names = { string_table, string_expanded, 0, archive->big_endian };
    for (size_t i = 0; i < file_count; i++) {
        uint8_t name_length = 0;
        uint8_t expected_hash = 0;
        if (!reader_u8(&names, &name_length) || !reader_u8(&names, &expected_hash) ||
            name_length == 0 || name_length > archive->limits.max_name_bytes) {
            free(string_table); free(fst); free(entries); free(offsets);
            return xb_fail(error_message, error_message_size,
                           "invalid XB string-table name length");
        }
        size_t start = names.pos;
        while (names.pos < names.size && names.data[names.pos] != 0) names.pos++;
        if (names.pos >= names.size || names.pos - start != name_length ||
            expected_hash != sjis_hash(names.data + start, name_length) ||
            !normalize_inner_path(names.data + start, name_length,
                                  archive->limits.max_name_bytes,
                                  entries[i].path, sizeof(entries[i].path))) {
            free(string_table); free(fst); free(entries); free(offsets);
            return xb_fail(error_message, error_message_size,
                           "invalid or unsafe XB string-table path");
        }
        names.pos++;
    }
    if (names.pos != names.size) {
        free(string_table); free(fst); free(entries); free(offsets);
        return xb_fail(error_message, error_message_size,
                       "string table has trailing bytes");
    }
    free(string_table);

    size_t data_start = reader.pos;
    if (data_start >= archive->data_size) {
        free(fst); free(entries); free(offsets);
        return xb_fail(error_message, error_message_size,
                       "archive has no file-data section");
    }

    for (size_t i = 0; i < file_count; i++) {
        offsets[i].offset = fst[i].offset;
        offsets[i].index = i;
    }
    qsort(offsets, file_count, sizeof(*offsets), offset_index_compare);

    uint64_t total_expanded = 0;
    uint64_t previous_offset = UINT64_MAX;
    for (size_t position = 0; position < file_count; position++) {
        size_t index = offsets[position].index;
        uint64_t offset = offsets[position].offset;
        XbFstRow row = fst[index];
        if (row.expanded_size == 0 || row.expanded_size > archive->limits.max_entry_bytes ||
            row.expanded_size > NK_XB_MAX_ENTRY_BYTES ||
            offset < data_start || offset >= archive->data_size || (offset & 3u) != 0) {
            free(fst); free(entries); free(offsets);
            return xb_fail(error_message, error_message_size,
                           "FST entry is outside the bounded data section");
        }
        if (previous_offset == offset) {
            free(fst); free(entries); free(offsets);
            return xb_fail(error_message, error_message_size,
                           "duplicate FST data offset");
        }
        previous_offset = offset;
        uint64_t next_offset = position + 1 < file_count
            ? offsets[position + 1].offset : (uint64_t)archive->data_size;
        if (next_offset <= offset || next_offset > archive->data_size) {
            free(fst); free(entries); free(offsets);
            return xb_fail(error_message, error_message_size,
                           "FST data offsets are not increasing");
        }
        uint64_t span_size = next_offset - offset;
        const uint8_t *payload = archive->data + (size_t)offset;
        size_t payload_size = (size_t)span_size;
        uint64_t stored_size = row.expanded_size;
        if (row.compression != NK_XB_COMPRESSION_NONE) {
            if (payload_size < 8) {
                free(fst); free(entries); free(offsets);
                return xb_fail(error_message, error_message_size,
                               "compressed entry is missing its size header");
            }
            XbReader header = { payload, payload_size, 0, archive->big_endian };
            uint32_t header_expanded = 0;
            uint32_t header_compressed = 0;
            if (!reader_u32(&header, &header_expanded) ||
                !reader_u32(&header, &header_compressed) ||
                header_expanded == 0 || header_expanded > archive->limits.max_entry_bytes ||
                header_expanded > NK_XB_MAX_ENTRY_BYTES) {
                free(fst); free(entries); free(offsets);
                return xb_fail(error_message, error_message_size,
                               "invalid compressed entry expanded size");
            }
            if ((row.compression == NK_XB_COMPRESSION_LZS ||
                 row.compression == NK_XB_COMPRESSION_HUFFMAN) &&
                header_expanded != row.expanded_size) {
                free(fst); free(entries); free(offsets);
                return xb_fail(error_message, error_message_size,
                               "compressed entry expanded size disagrees with FST");
            }
            uint64_t compressed_body_size = header_compressed == 0
                ? (uint64_t)header_expanded : (uint64_t)header_compressed;
            if (compressed_body_size > UINT64_MAX - 8u) {
                free(fst); free(entries); free(offsets);
                return xb_fail(error_message, error_message_size,
                               "compressed entry size overflows");
            }
            if (row.compression == NK_XB_COMPRESSION_DEFLATE) {
                stored_size = span_size;
            } else if (header_compressed > 8u && (uint64_t)header_compressed <= span_size) {
                stored_size = (uint64_t)header_compressed;
            } else if (8u + compressed_body_size <= span_size) {
                stored_size = 8u + compressed_body_size;
            } else {
                free(fst); free(entries); free(offsets);
                return xb_fail(error_message, error_message_size,
                               "compressed entry exceeds its FST span");
            }
        } else if (stored_size > span_size) {
            free(fst); free(entries); free(offsets);
            return xb_fail(error_message, error_message_size,
                           "raw entry is truncated or exceeds its FST span");
        }

        total_expanded += row.expanded_size;
        if (total_expanded > archive->limits.max_total_expanded_bytes ||
            total_expanded > NK_XB_MAX_TOTAL_EXPANDED_BYTES) {
            free(fst); free(entries); free(offsets);
            return xb_fail(error_message, error_message_size,
                           "total expanded size exceeds the configured limit");
        }
        entries[index].index = index;
        entries[index].offset = offset;
        entries[index].expanded_size = row.expanded_size;
        entries[index].compression = row.compression;
        entries[index].stored_size = stored_size;
        entries[index].span_size = span_size;
    }

    size_t slot_count = 1;
    while (slot_count < file_count * 2u) slot_count <<= 1;
    int32_t *slots = (int32_t *)malloc(slot_count * sizeof(int32_t));
    if (!slots) {
        free(fst); free(entries); free(offsets);
        return NK_ERROR_OUT_OF_MEMORY;
    }
    for (size_t s = 0; s < slot_count; s++) slots[s] = -1;

    for (size_t i = 0; i < file_count; i++) {
        size_t slot = (size_t)path_hash(entries[i].path) & (slot_count - 1u);
        while (slots[slot] != -1) {
            size_t prev_i = (size_t)slots[slot];
            if (strcmp(entries[prev_i].path, entries[i].path) == 0) {
                if (!entries_content_identical(archive, &entries[prev_i], &entries[i])) {
                    free(slots); free(fst); free(entries); free(offsets);
                    return xb_fail(error_message, error_message_size,
                                   "duplicate canonical XB path");
                }
                break;
            }
            slot = (slot + 1u) & (slot_count - 1u);
        }
        if (slots[slot] == -1) {
            slots[slot] = (int32_t)i;
        }
    }
    free(slots);

    free(fst);
    free(offsets);
    archive->entries = entries;
    archive->entry_count = file_count;
    archive->data_start = data_start;
    return NK_OK;
}

void nk_xb_close(NkXbArchive *archive) {
    if (!archive) return;
    free(archive->entries);
    if (archive->owns_data) free((void *)archive->data);
    memset(archive, 0, sizeof(*archive));
}

NkResult nk_xb_open_memory(const void *data, size_t data_size,
                           const char *source_name, bool big_endian,
                           const NkXbLimits *limits, NkXbArchive *out_archive,
                           char *error_message, size_t error_message_size) {
    if (error_message && error_message_size > 0) error_message[0] = '\0';
    if (!out_archive || (!data && data_size > 0) || data_size == 0) {
        return xb_fail(error_message, error_message_size,
                       "invalid XB memory input");
    }
    memset(out_archive, 0, sizeof(*out_archive));
    out_archive->limits = normalized_limits(limits);
    out_archive->big_endian = big_endian;
    if (data_size > out_archive->limits.max_archive_bytes ||
        data_size > NK_XB_MAX_ARCHIVE_BYTES) {
        return xb_fail(error_message, error_message_size,
                       "archive exceeds the configured byte limit");
    }
    uint8_t *copy = (uint8_t *)malloc(data_size);
    if (!copy) return NK_ERROR_OUT_OF_MEMORY;
    memcpy(copy, data, data_size);
    out_archive->data = copy;
    out_archive->data_size = data_size;
    out_archive->owns_data = true;
    snprintf(out_archive->source_name, sizeof(out_archive->source_name), "%s",
             source_name && source_name[0] ? source_name : "memory.xb");
    NkResult result = parse_archive(out_archive, error_message, error_message_size);
    if (result != NK_OK) nk_xb_close(out_archive);
    return result;
}

static FILE *xb_fopen(const char *path, const char *mode) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wpath[32768];
    WCHAR wmode[32];
    if (!path || !mode ||
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) <= 0 ||
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, (int)(sizeof(wmode) / sizeof(wmode[0]))) <= 0) {
        return NULL;
    }
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

/* Archive member identifiers are stored as Shift-JIS bytes. On POSIX those
 * bytes remain the filesystem name, matching the format-native VFS view. The
 * Win32 path backend is UTF-8, so transcode the validated identifier before
 * passing it to the wide-character writer. */
static bool xb_member_path_to_host_utf8(const char *member,
                                        char *out_path, size_t out_size) {
    if (!member || !out_path || out_size == 0) return false;
#if defined(_WIN32) || defined(_WIN64)
    int wide_count = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, member, -1,
                                         NULL, 0);
    if (wide_count <= 0) return false;
    WCHAR *wide = (WCHAR *)malloc((size_t)wide_count * sizeof(*wide));
    if (!wide) return false;
    bool okay = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, member, -1,
                                    wide, wide_count) == wide_count;
    if (okay) {
        int utf8_count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                             wide, -1, NULL, 0, NULL, NULL);
        if (utf8_count <= 0 || (size_t)utf8_count > out_size) {
            okay = false;
        } else {
            okay = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                       wide, -1, out_path, utf8_count,
                                       NULL, NULL) == utf8_count;
        }
    }
    free(wide);
    return okay;
#else
    size_t length = strlen(member);
    if (length >= out_size) return false;
    memcpy(out_path, member, length + 1);
    return true;
#endif
}

NkResult nk_xb_open_file(const char *path, bool big_endian,
                         const NkXbLimits *limits, NkXbArchive *out_archive,
                         char *error_message, size_t error_message_size) {
    if (error_message && error_message_size > 0) error_message[0] = '\0';
    if (!path || !out_archive) {
        return xb_fail(error_message, error_message_size,
                       "invalid XB file input");
    }
    NkXbLimits effective = normalized_limits(limits);
    int64_t file_size = nk_platform_get_file_size(path);
    if (file_size <= 0) {
        return xb_fail(error_message, error_message_size,
                       "cannot stat XB archive");
    }
    if ((uint64_t)file_size > effective.max_archive_bytes ||
        (uint64_t)file_size > NK_XB_MAX_ARCHIVE_BYTES ||
        (uint64_t)file_size > SIZE_MAX) {
        return xb_fail(error_message, error_message_size,
                       "archive exceeds the configured byte limit");
    }
    size_t size = (size_t)file_size;
    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) return NK_ERROR_OUT_OF_MEMORY;
    FILE *file = xb_fopen(path, "rb");
    if (!file) {
        free(data);
        return xb_fail(error_message, error_message_size,
                       "cannot open XB archive");
    }
    size_t read_total = 0;
    while (read_total < size) {
        size_t n = fread(data + read_total, 1, size - read_total, file);
        if (n == 0) break;
        read_total += n;
    }
    int close_failed = fclose(file) != 0;
    if (read_total != size || close_failed) {
        free(data);
        return xb_fail(error_message, error_message_size,
                       "XB archive changed or could not be read");
    }

    memset(out_archive, 0, sizeof(*out_archive));
    out_archive->data = data;
    out_archive->data_size = size;
    out_archive->owns_data = true;
    out_archive->limits = effective;
    out_archive->big_endian = big_endian;
    snprintf(out_archive->source_name, sizeof(out_archive->source_name), "%s", path);
    NkResult result = parse_archive(out_archive, error_message, error_message_size);
    if (result != NK_OK) nk_xb_close(out_archive);
    return result;
}

NkResult nk_xb_read_entry(const NkXbArchive *archive, size_t entry_index,
                          void *output, size_t output_capacity,
                          size_t *output_size, char *error_message,
                          size_t error_message_size) {
    if (error_message && error_message_size > 0) error_message[0] = '\0';
    if (output_size) *output_size = 0;
    if (!archive || !archive->data || !archive->entries ||
        entry_index >= archive->entry_count) {
        return xb_fail(error_message, error_message_size,
                       "XB entry index is out of range");
    }
    const NkXbEntry *entry = &archive->entries[entry_index];
    if (entry->expanded_size > SIZE_MAX || entry->expanded_size > output_capacity ||
        (!output && entry->expanded_size > 0)) {
        return xb_fail(error_message, error_message_size,
                       "XB output buffer is too small");
    }
    if (entry->offset > archive->data_size || entry->span_size > archive->data_size - (size_t)entry->offset) {
        return xb_fail(error_message, error_message_size,
                       "XB entry span exceeds archive");
    }
    const uint8_t *payload = archive->data + (size_t)entry->offset;
    size_t payload_size = (size_t)entry->span_size;
    NkResult result;
    if (entry->compression == NK_XB_COMPRESSION_NONE) {
        memcpy(output, payload, (size_t)entry->expanded_size);
        result = NK_OK;
    } else {
        result = decode_prefixed(payload, payload_size, entry->compression,
                                 (size_t)entry->expanded_size, &archive->limits,
                                 archive->big_endian, (uint8_t *)output,
                                 error_message, error_message_size);
    }
    if (result == NK_OK && output_size) *output_size = (size_t)entry->expanded_size;
    return result;
}

static bool make_member_path(const char *root, const char *member,
                             char *out_path, size_t out_size) {
    if (!root || !root[0] || !member || !member[0] || !out_path || out_size == 0) return false;
    size_t root_len = strlen(root);
    while (root_len > 0 && (root[root_len - 1] == '/' || root[root_len - 1] == '\\')) root_len--;
    int written = snprintf(out_path, out_size, "%.*s%c%s", (int)root_len, root,
                           nk_platform_path_separator(), member);
    return written >= 0 && (size_t)written < out_size;
}

static bool make_parent_directory(const char *path) {
    char parent[4096];
    size_t length = strlen(path);
    if (length >= sizeof(parent)) return false;
    memcpy(parent, path, length + 1);
    char *slash = strrchr(parent, '/');
    char *backslash = strrchr(parent, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    if (!slash) return false;
    *slash = '\0';
    return nk_platform_mkdir_p_private(parent);
}

NkResult nk_xb_unpack(const char *archive_path, const char *destination_root,
                      NkXbProgressCallback progress, void *userdata,
                      char *error_message, size_t error_message_size) {
    if (error_message && error_message_size > 0) error_message[0] = '\0';
    if (!archive_path || !destination_root || !destination_root[0]) {
        return xb_fail(error_message, error_message_size,
                       "invalid XB unpack destination");
    }
    if (!nk_platform_mkdir_p_private(destination_root)) {
        return xb_fail(error_message, error_message_size,
                       "cannot create XB unpack destination");
    }

    NkXbArchive archive;
    NkResult result = nk_xb_open_file(archive_path, false, NULL, &archive,
                                      error_message, error_message_size);
    if (result != NK_OK) return result;

    for (size_t i = 0; i < archive.entry_count; i++) {
        const NkXbEntry *entry = &archive.entries[i];
        if (entry->expanded_size > SIZE_MAX) {
            result = xb_fail(error_message, error_message_size,
                             "XB member is too large for this host");
            break;
        }
        uint8_t *decoded = (uint8_t *)malloc((size_t)entry->expanded_size);
        if (!decoded) {
            result = NK_ERROR_OUT_OF_MEMORY;
            break;
        }
        size_t decoded_size = 0;
        result = nk_xb_read_entry(&archive, i, decoded,
                                  (size_t)entry->expanded_size, &decoded_size,
                                  error_message, error_message_size);
        if (result == NK_OK) {
            char host_member[4096];
            char output_path[4096];
            if (!xb_member_path_to_host_utf8(entry->path, host_member,
                                             sizeof(host_member)) ||
                !make_member_path(destination_root, host_member,
                                  output_path, sizeof(output_path)) ||
                !make_parent_directory(output_path)) {
                result = xb_fail(error_message, error_message_size,
                                 "XB member path is too long or cannot be contained");
            } else {
                FILE *file = nk_platform_fopen_private(output_path, "wb");
                if (!file) {
                    result = NK_ERROR_IO;
                } else {
                    size_t written = fwrite(decoded, 1, decoded_size, file);
                    int close_failed = fclose(file) != 0;
                    if (written != decoded_size || close_failed) result = NK_ERROR_IO;
                }
            }
        }
        free(decoded);
        if (result != NK_OK) break;
        if (progress && !progress(entry, i + 1, archive.entry_count, userdata)) {
            result = NK_ERROR_CANCELLED;
            break;
        }
    }
    nk_xb_close(&archive);
    if (result == NK_ERROR_IO && error_message && error_message_size > 0 && !error_message[0]) {
        xb_error(error_message, error_message_size, "failed writing XB member");
    }
    if (result == NK_ERROR_OUT_OF_MEMORY && error_message && error_message_size > 0 && !error_message[0]) {
        xb_error(error_message, error_message_size, "out of memory while unpacking XB member");
    }
    return result;
}
