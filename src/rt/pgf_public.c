// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Project-authored public PGF reader, specified in docs/cleanroom/PGF_SPEC.md. */

#include "pgf_api.h"

#include "recomp.h"
#include "ge_shared.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <wchar.h>
#endif

#define PGF_BASE_HEADER_SIZE 392u
#define PGF_REV3_HEADER_SIZE 412u
#define PGF_MAX_IMAGE_SIZE (16u * 1024u * 1024u)
#define PGF_MAX_COUNT 1048576u
#define PGF_MAX_SAMPLES 65536u
#define PGF_MAX_GLYPH_EDGE 127u
#define PGF_FONT_INFO_SIZE 0x108u
#define PGF_CHAR_INFO_SIZE 0x3cu
#define PGF_GLYPH_IMAGE_SIZE 0x18u

enum {
    PGF_TABLE_DIMENSION = 0,
    PGF_TABLE_X_ADJUSTMENT = 1,
    PGF_TABLE_Y_ADJUSTMENT = 2,
    PGF_TABLE_ADVANCE = 3,
    PGF_TABLE_COUNT = 4
};

struct PGF {
    uint8_t *image;
    size_t size;
    uint32_t revision;
    uint32_t first_glyph;
    uint32_t glyph_count;
    uint32_t char_map_count;
    uint32_t char_map_bits;
    uint32_t char_pointer_bits;
    uint32_t shadow_count;
    uint8_t metric_counts[PGF_TABLE_COUNT];
    size_t metric_offsets[PGF_TABLE_COUNT];
    size_t shadow_map_offset;
    size_t char_map_offset;
    size_t char_pointer_offset;
    size_t glyph_data_offset;
    uint8_t file_name[64];
};

typedef struct PgfGlyph {
    size_t record_offset;
    size_t record_size;
    size_t bitmap_offset;
    uint32_t width;
    uint32_t height;
    int32_t bitmap_adjust_x;
    int32_t bitmap_adjust_y;
    uint32_t row_order;
    uint32_t shadow_offset;
    uint32_t shadow_id;
    uint32_t shadow_row_order;
    int32_t dimension_width;
    int32_t dimension_height;
    int32_t x_left;
    int32_t x_center;
    int32_t y_base;
    int32_t y_top;
    int32_t advance_x;
    int32_t advance_y;
} PgfGlyph;

typedef struct PgfDirtySpan {
    uint32_t address;
    uint32_t size;
} PgfDirtySpan;

static uint16_t pgf_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t pgf_u32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t pgf_i32(const uint8_t *p) {
    uint32_t raw = pgf_u32(p);
    int64_t value = raw <= INT32_MAX ? (int64_t)raw : (int64_t)raw - 4294967296LL;
    return (int32_t)value;
}

static void pgf_put_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void pgf_put_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t pgf_get_bits(const uint8_t *bytes, size_t byte_count,
                             uint64_t bit_offset, uint32_t bit_count,
                             int *valid) {
    uint64_t end_bit = bit_offset + bit_count;
    uint64_t available_bits = (uint64_t)byte_count * 8u;
    uint64_t first_byte;
    uint32_t shift;
    uint32_t bytes_needed;
    uint64_t packed = 0;
    uint32_t i;

    if (valid) *valid = 0;
    if (bit_count == 0 || bit_count > 32 || end_bit < bit_offset ||
        end_bit > available_bits) {
        return 0;
    }
    first_byte = bit_offset / 8u;
    shift = (uint32_t)(bit_offset % 8u);
    bytes_needed = (shift + bit_count + 7u) / 8u;
    for (i = 0; i < bytes_needed; ++i) {
        packed |= (uint64_t)bytes[(size_t)first_byte + i] << (8u * i);
    }
    packed >>= shift;
    if (valid) *valid = 1;
    return bit_count == 32 ? (uint32_t)packed :
           (uint32_t)(packed & (((uint64_t)1u << bit_count) - 1u));
}

static int pgf_packed_size(uint32_t count, uint32_t bits, size_t *result) {
    uint64_t total_bits = (uint64_t)count * bits;
    uint64_t words = (total_bits + 31u) / 32u;
    uint64_t bytes = words * 4u;
    if (bytes > SIZE_MAX) return 0;
    *result = (size_t)bytes;
    return 1;
}

static int pgf_add_section(size_t *cursor, uint64_t length, size_t total_size,
                           size_t *section_offset) {
    if (length > SIZE_MAX || *cursor > total_size ||
        (size_t)length > total_size - *cursor) {
        return 0;
    }
    *section_offset = *cursor;
    *cursor += (size_t)length;
    return 1;
}

static int pgf_map_entry(const PGF *p, uint32_t char_code, uint32_t *glyph_id) {
    int64_t index = (int64_t)char_code - (int64_t)p->first_glyph;
    uint32_t value;
    int valid;

    if (index < 0 || (uint64_t)index >= p->char_map_count) return 0;
    value = pgf_get_bits(p->image + p->char_map_offset,
                         p->size - p->char_map_offset,
                         (uint64_t)index * p->char_map_bits,
                         p->char_map_bits, &valid);
    if (!valid || value == (p->char_map_bits == 32 ? UINT32_MAX :
                             (((uint32_t)1u << p->char_map_bits) - 1u)) ||
        value >= p->glyph_count) {
        return 0;
    }
    *glyph_id = value;
    return 1;
}

static int pgf_pointer_entry(const PGF *p, uint32_t glyph_id, uint64_t *offset) {
    uint32_t units;
    int valid;
    if (glyph_id >= p->glyph_count) return 0;
    units = pgf_get_bits(p->image + p->char_pointer_offset,
                         p->size - p->char_pointer_offset,
                         (uint64_t)glyph_id * p->char_pointer_bits,
                         p->char_pointer_bits, &valid);
    if (!valid) return 0;
    *offset = (uint64_t)units * 4u;
    return 1;
}

static int32_t pgf_sign7(uint32_t value) {
    value &= 0x7fu;
    return value & 0x40u ? (int32_t)value - 128 : (int32_t)value;
}

static int pgf_table_pair(const PGF *p, unsigned table, uint32_t index,
                          int32_t *first, int32_t *second) {
    size_t entry;
    if (table >= PGF_TABLE_COUNT || index >= p->metric_counts[table]) return 0;
    entry = p->metric_offsets[table] + (size_t)index * 8u;
    *first = pgf_i32(p->image + entry);
    *second = pgf_i32(p->image + entry + 4u);
    return 1;
}

static int pgf_primary_glyph(const PGF *p, uint32_t glyph_id, PgfGlyph *glyph) {
    uint64_t relative;
    uint64_t absolute64;
    size_t absolute;
    size_t available;
    size_t cursor;
    uint32_t flags;
    uint32_t group;
    uint32_t field;
    int valid;
    int64_t descender;

    memset(glyph, 0, sizeof(*glyph));
    if (!pgf_pointer_entry(p, glyph_id, &relative) ||
        relative > p->size - p->glyph_data_offset) {
        return 0;
    }
    absolute64 = (uint64_t)p->glyph_data_offset + relative;
    if (absolute64 > SIZE_MAX || absolute64 > p->size) return 0;
    absolute = (size_t)absolute64;
    available = p->size - absolute;
    if (available < 12u) return 0;

    glyph->record_offset = absolute;
    glyph->shadow_offset = pgf_get_bits(p->image + absolute, available, 0, 14, &valid);
    if (!valid) return 0;
    glyph->width = pgf_get_bits(p->image + absolute, available, 14, 7, &valid);
    if (!valid) return 0;
    glyph->height = pgf_get_bits(p->image + absolute, available, 21, 7, &valid);
    if (!valid) return 0;
    glyph->bitmap_adjust_x = pgf_sign7(pgf_get_bits(p->image + absolute, available,
                                                    28, 7, &valid));
    if (!valid) return 0;
    glyph->bitmap_adjust_y = pgf_sign7(pgf_get_bits(p->image + absolute, available,
                                                    35, 7, &valid));
    if (!valid) return 0;
    glyph->row_order = pgf_get_bits(p->image + absolute, available, 42, 2, &valid);
    if (!valid) return 0;
    flags = pgf_get_bits(p->image + absolute, available, 45, 3, &valid);
    if (!valid) return 0;
    glyph->shadow_id = pgf_get_bits(p->image + absolute, available, 55, 9, &valid);
    if (!valid) return 0;

    cursor = absolute + 8u;
    for (group = 0; group < 3u; ++group) {
        unsigned table = group;
        int32_t first;
        int32_t second;
        if ((flags & (1u << group)) != 0u) {
            if (cursor >= p->size) return 0;
            field = p->image[cursor++];
            if (!pgf_table_pair(p, table, field, &first, &second)) return 0;
        } else {
            if (cursor > p->size || p->size - cursor < 8u) return 0;
            first = pgf_i32(p->image + cursor);
            second = pgf_i32(p->image + cursor + 4u);
            cursor += 8u;
        }
        if (group == PGF_TABLE_DIMENSION) {
            glyph->dimension_width = first;
            glyph->dimension_height = second;
        } else if (group == PGF_TABLE_X_ADJUSTMENT) {
            glyph->x_left = first;
            glyph->x_center = second;
        } else {
            glyph->y_base = first;
            glyph->y_top = second;
        }
    }
    if (cursor >= p->size) return 0;
    field = p->image[cursor++];
    if (!pgf_table_pair(p, PGF_TABLE_ADVANCE, field,
                        &glyph->advance_x, &glyph->advance_y)) {
        return 0;
    }
    glyph->record_size = cursor - absolute;
    glyph->bitmap_offset = cursor;
    descender = (int64_t)glyph->y_base - (int64_t)glyph->dimension_height;
    if (descender < INT32_MIN || descender > INT32_MAX) return 0;
    return 1;
}

static int pgf_checked_glyph(const PGF *p, uint32_t glyph_id, PgfGlyph *glyph) {
    uint32_t shadow_code;
    uint32_t mapped_glyph;
    uint64_t target;
    int valid;

    if (!pgf_primary_glyph(p, glyph_id, glyph)) return 0;
    if (glyph->shadow_id == 0u) return 1;
    if (glyph->shadow_id > p->shadow_count) return 0;

    shadow_code = pgf_u16(p->image + p->shadow_map_offset +
                          ((size_t)glyph->shadow_id - 1u) * 2u);
    if (!pgf_map_entry(p, shadow_code, &mapped_glyph)) return 0;
    (void)mapped_glyph;

    if (glyph->shadow_offset < glyph->record_size) return 0;
    target = (uint64_t)glyph->record_offset + glyph->shadow_offset;
    if (target < glyph->record_offset || target > p->size ||
        p->size - (size_t)target < 6u) {
        return 0;
    }
    glyph->shadow_row_order = pgf_get_bits(p->image + (size_t)target,
                                            p->size - (size_t)target,
                                            42, 2, &valid);
    return valid;
}

static int pgf_parse_directory(PGF *p) {
    const uint8_t *h = p->image;
    int32_t revision;
    int32_t version;
    uint16_t header_size;
    uint32_t pointer_count;
    uint32_t char_map_bits;
    uint32_t pointer_bits;
    uint32_t shadow_bits;
    uint32_t first;
    uint32_t last;
    uint32_t glyph_count;
    uint32_t shadow_count;
    uint16_t rev3_count_a = 0;
    uint16_t rev3_count_b = 0;
    size_t cursor;
    size_t length;
    unsigned i;
    size_t table_offset;

    if (p->size < PGF_BASE_HEADER_SIZE || p->size > PGF_MAX_IMAGE_SIZE ||
        pgf_u16(h) != 0u || memcmp(h + 4u, "PGF0", 4u) != 0) {
        return 0;
    }
    header_size = pgf_u16(h + 2u);
    revision = pgf_i32(h + 8u);
    version = pgf_i32(h + 12u);
    if (revision < 0 || revision > 3 || version < 0) return 0;
    if (header_size != (revision == 3 ? PGF_REV3_HEADER_SIZE : PGF_BASE_HEADER_SIZE) ||
        p->size < header_size) {
        return 0;
    }

    p->revision = (uint32_t)revision;
    p->char_map_count = pgf_u32(h + 0x10u);
    pointer_count = pgf_u32(h + 0x14u);
    char_map_bits = pgf_u32(h + 0x18u);
    pointer_bits = pgf_u32(h + 0x1cu);
    first = pgf_u16(h + 0xb6u);
    last = pgf_u16(h + 0xb8u);
    shadow_count = pgf_u32(h + 0x16cu);
    shadow_bits = pgf_u32(h + 0x170u);
    if (first > last || char_map_bits == 0u || char_map_bits > 32u ||
        pointer_bits == 0u || pointer_bits > 32u ||
        p->char_map_count > PGF_MAX_COUNT || pointer_count > PGF_MAX_COUNT ||
        shadow_count > PGF_MAX_COUNT) {
        return 0;
    }
    glyph_count = last - first + 1u;
    if (glyph_count > PGF_MAX_COUNT || pointer_count != glyph_count ||
        (shadow_count == 0u ? shadow_bits != 0u : shadow_bits != 16u)) {
        return 0;
    }
    p->first_glyph = first;
    p->glyph_count = glyph_count;
    p->char_map_bits = char_map_bits;
    p->char_pointer_bits = pointer_bits;
    p->shadow_count = shadow_count;
    for (i = 0; i < PGF_TABLE_COUNT; ++i) p->metric_counts[i] = h[0x102u + i];

    if (revision == 3) {
        rev3_count_a = pgf_u16(h + 0x18cu);
        rev3_count_b = pgf_u16(h + 0x194u);
    }

    cursor = header_size;
    for (i = 0; i < PGF_TABLE_COUNT; ++i) {
        if (!pgf_add_section(&cursor, (uint64_t)p->metric_counts[i] * 8u,
                             p->size, &table_offset)) {
            return 0;
        }
        p->metric_offsets[i] = table_offset;
    }
    if (!pgf_packed_size(shadow_count, shadow_bits, &length) ||
        !pgf_add_section(&cursor, length, p->size, &p->shadow_map_offset)) {
        return 0;
    }
    if (revision == 3) {
        if (!pgf_add_section(&cursor, (uint64_t)rev3_count_a * 4u,
                             p->size, &table_offset) ||
            !pgf_add_section(&cursor, (uint64_t)rev3_count_b * 4u,
                             p->size, &table_offset)) {
            return 0;
        }
    }
    if (!pgf_packed_size(p->char_map_count, char_map_bits, &length) ||
        !pgf_add_section(&cursor, length, p->size, &p->char_map_offset)) {
        return 0;
    }
    if (!pgf_packed_size(pointer_count, pointer_bits, &length) ||
        !pgf_add_section(&cursor, length, p->size, &p->char_pointer_offset)) {
        return 0;
    }
    p->glyph_data_offset = cursor;
    return 1;
}

static PGF *pgf_open_owned(uint8_t *image, size_t size, const uint8_t *name,
                           size_t name_length) {
    PGF *p;
    uint32_t i;
    if (!image || size < PGF_BASE_HEADER_SIZE || size > PGF_MAX_IMAGE_SIZE) {
        free(image);
        return NULL;
    }
    p = (PGF *)calloc(1u, sizeof(*p));
    if (!p) {
        free(image);
        return NULL;
    }
    p->image = image;
    p->size = size;
    if (name && name_length != 0u) {
        if (name_length > sizeof(p->file_name) - 1u) {
            name_length = sizeof(p->file_name) - 1u;
        }
        memcpy(p->file_name, name, name_length);
    }
    if (!pgf_parse_directory(p)) goto fail;
    for (i = 0; i < p->glyph_count; ++i) {
        PgfGlyph glyph;
        if (!pgf_checked_glyph(p, i, &glyph)) goto fail;
    }
    return p;

fail:
    free(p->image);
    free(p);
    return NULL;
}

static PGF *pgf_read_stream(FILE *stream, const uint8_t *name, size_t name_length) {
    long file_length;
    size_t size;
    size_t received;
    uint8_t *image;
    PGF *result;

    if (!stream || fseek(stream, 0L, SEEK_END) != 0) return NULL;
    file_length = ftell(stream);
    if (file_length < (long)PGF_BASE_HEADER_SIZE ||
        (uint64_t)file_length > PGF_MAX_IMAGE_SIZE ||
        fseek(stream, 0L, SEEK_SET) != 0) {
        return NULL;
    }
    size = (size_t)file_length;
    image = (uint8_t *)malloc(size);
    if (!image) return NULL;
    received = fread(image, 1u, size, stream);
    if (received != size || ferror(stream)) {
        free(image);
        return NULL;
    }
    result = pgf_open_owned(image, size, name, name_length);
    return result;
}

static size_t pgf_narrow_basename(const char *path, const char **name) {
    const char *component = path;
    const char *cursor;
    if (!path) {
        *name = NULL;
        return 0;
    }
    for (cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' ||
#ifdef _WIN32
            *cursor == '\\' ||
#endif
            0) {
            component = cursor + 1;
        }
    }
    *name = component;
    return strlen(component);
}

PGF *pgf_open(const char *path) {
    const char *name;
    size_t name_length;
    FILE *stream;
    PGF *result;
    if (!path) return NULL;
    name_length = pgf_narrow_basename(path, &name);
    stream = fopen(path, "rb");
    if (!stream) return NULL;
    result = pgf_read_stream(stream, (const uint8_t *)name, name_length);
    fclose(stream);
    return result;
}

#ifdef _WIN32
static uint32_t pgf_wide_scalar(const wchar_t *text, size_t *index) {
    uint32_t first = (uint32_t)text[*index];
    if (sizeof(wchar_t) == 2u && first >= 0xd800u && first <= 0xdbffu) {
        uint32_t second = (uint32_t)text[*index + 1u];
        if (second >= 0xdc00u && second <= 0xdfffu) {
            ++*index;
            return 0x10000u + ((first - 0xd800u) << 10) + (second - 0xdc00u);
        }
        return 0xfffdu;
    }
    if (sizeof(wchar_t) == 2u && first >= 0xdc00u && first <= 0xdfffu) return 0xfffdu;
    if (first > 0x10ffffu || (first >= 0xd800u && first <= 0xdfffu)) return 0xfffdu;
    return first;
}

static size_t pgf_wide_basename(const wchar_t *path, const wchar_t **name) {
    const wchar_t *component = path;
    const wchar_t *cursor;
    if (!path) {
        *name = NULL;
        return 0;
    }
    for (cursor = path; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'/' || *cursor == L'\\') component = cursor + 1;
    }
    *name = component;
    return wcslen(component);
}

static size_t pgf_encode_wide_name(const wchar_t *wide, uint8_t output[64]) {
    size_t input_length = wcslen(wide);
    size_t input_index = 0;
    size_t output_length = 0;
    while (input_index < input_length) {
        uint32_t scalar = pgf_wide_scalar(wide, &input_index);
        uint8_t encoded[4];
        size_t encoded_length;
        if (scalar <= 0x7fu) {
            encoded[0] = (uint8_t)scalar;
            encoded_length = 1u;
        } else if (scalar <= 0x7ffu) {
            encoded[0] = (uint8_t)(0xc0u | (scalar >> 6));
            encoded[1] = (uint8_t)(0x80u | (scalar & 0x3fu));
            encoded_length = 2u;
        } else if (scalar <= 0xffffu) {
            encoded[0] = (uint8_t)(0xe0u | (scalar >> 12));
            encoded[1] = (uint8_t)(0x80u | ((scalar >> 6) & 0x3fu));
            encoded[2] = (uint8_t)(0x80u | (scalar & 0x3fu));
            encoded_length = 3u;
        } else {
            encoded[0] = (uint8_t)(0xf0u | (scalar >> 18));
            encoded[1] = (uint8_t)(0x80u | ((scalar >> 12) & 0x3fu));
            encoded[2] = (uint8_t)(0x80u | ((scalar >> 6) & 0x3fu));
            encoded[3] = (uint8_t)(0x80u | (scalar & 0x3fu));
            encoded_length = 4u;
        }
        if (output_length + encoded_length > 63u) break;
        memcpy(output + output_length, encoded, encoded_length);
        output_length += encoded_length;
        ++input_index;
    }
    return output_length;
}

PGF *pgf_open_w(const wchar_t *path) {
    const wchar_t *wide_name;
    uint8_t encoded_name[64] = {0};
    size_t encoded_length;
    FILE *stream;
    PGF *result;
    if (!path) return NULL;
    (void)pgf_wide_basename(path, &wide_name);
    encoded_length = pgf_encode_wide_name(wide_name, encoded_name);
    stream = _wfopen(path, L"rb");
    if (!stream) return NULL;
    result = pgf_read_stream(stream, encoded_name, encoded_length);
    fclose(stream);
    return result;
}
#endif

PGF *pgf_open_memory(const void *data, size_t size) {
    uint8_t *copy;
    if (!data || size < PGF_BASE_HEADER_SIZE || size > PGF_MAX_IMAGE_SIZE) return NULL;
    copy = (uint8_t *)malloc(size);
    if (!copy) return NULL;
    memcpy(copy, data, size);
    return pgf_open_owned(copy, size, NULL, 0u);
}

void pgf_close(PGF *p) {
    if (!p) return;
    free(p->image);
    free(p);
}

static int pgf_guest_span(uint32_t address, size_t size, int write_access) {
    uint64_t end = (uint64_t)address + size;
    if (size > UINT32_MAX || end > 0x100000000ULL || g_mem == NULL) return 0;
    if (write_access) return sr_guest_span_writable(address, (uint32_t)size);
    return sr_guest_span_readable(address, (uint32_t)size);
}

static int pgf_guest_write(uint32_t address, const void *bytes, size_t size) {
    if (!pgf_guest_span(address, size, 1)) return 0;
    if (size != 0u) memcpy(SR_HOST(address), bytes, size);
    return 1;
}

static int pgf_guest_read(uint32_t address, void *bytes, size_t size) {
    if (!pgf_guest_span(address, size, 0)) return 0;
    if (size != 0u) memcpy(bytes, SR_HOST(address), size);
    return 1;
}

static int pgf_lookup(const PGF *p, int char_code, int alt_char_code,
                      int allow_alternate, PgfGlyph *glyph) {
    uint32_t glyph_id;
    if (p && char_code >= 0 &&
        pgf_map_entry(p, (uint32_t)char_code, &glyph_id) &&
        pgf_checked_glyph(p, glyph_id, glyph)) {
        return 1;
    }
    if (allow_alternate && p && char_code >= (int)p->first_glyph &&
        alt_char_code >= 0 &&
        pgf_map_entry(p, (uint32_t)alt_char_code, &glyph_id) &&
        pgf_checked_glyph(p, glyph_id, glyph)) {
        return 1;
    }
    return 0;
}

int pgf_has_char(const PGF *p, int char_code) {
    PgfGlyph glyph;
    if (!pgf_lookup(p, char_code, 0, 0, &glyph)) return 0;
    return glyph.width != 0u && glyph.height != 0u;
}

static uint32_t pgf_fixed_binary32(int32_t value) {
    uint32_t sign = 0;
    uint32_t exponent;
    uint32_t significand;
    uint64_t magnitude;
    unsigned top_bit = 0;
    int shift;

    if (value == 0) return 0u;
    if (value < 0) {
        sign = 0x80000000u;
        magnitude = (uint64_t)(-(int64_t)value);
    } else {
        magnitude = (uint64_t)value;
    }
    {
        uint64_t scan = magnitude;
        while (scan >>= 1) ++top_bit;
    }
    shift = (int)top_bit - 23;
    if (shift > 0) {
        uint64_t base = magnitude >> shift;
        uint64_t remainder = magnitude - (base << shift);
        uint64_t halfway = (uint64_t)1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (base & 1u))) ++base;
        if (base == 0x1000000u) {
            base >>= 1;
            ++top_bit;
        }
        significand = (uint32_t)base;
    } else {
        significand = (uint32_t)(magnitude << (unsigned)(-shift));
    }
    exponent = (uint32_t)((int)top_bit - 6 + 127);
    return sign | (exponent << 23) | (significand & 0x7fffffu);
}

static int pgf_resolves_code(const PGF *p, uint32_t code) {
    uint32_t ignored;
    return pgf_map_entry(p, code, &ignored);
}

void pgf_get_font_info(const PGF *p, uint32_t guest_info) {
    uint8_t output[PGF_FONT_INFO_SIZE] = {0};
    static const size_t raw_offsets[10] = {
        0xf4u, 0xf8u, 0xd4u, 0xd8u, 0xdcu,
        0xe0u, 0xe4u, 0xe8u, 0xecu, 0xf0u
    };
    size_t i;
    uint16_t first;
    if (!p) return;
    for (i = 0; i < 10u; ++i) {
        int32_t value = pgf_i32(p->image + raw_offsets[i]);
        pgf_put_u32(output + i * 4u, (uint32_t)value);
        pgf_put_u32(output + 0x28u + i * 4u, pgf_fixed_binary32(value));
    }
    pgf_put_u16(output + 0x50u, pgf_u16(p->image + 0xfcu));
    pgf_put_u16(output + 0x52u, pgf_u16(p->image + 0xfeu));
    pgf_put_u32(output + 0x54u, p->char_map_count);
    pgf_put_u32(output + 0x58u, p->shadow_count);
    for (i = 0; i < 4u; ++i) {
        int32_t value = pgf_i32(p->image + 0x24u + i * 4u);
        pgf_put_u32(output + 0x5cu + i * 4u, pgf_fixed_binary32(value));
    }
    pgf_put_u32(output + 0x6cu, 0u);
    pgf_put_u16(output + 0x70u, 1u);
    pgf_put_u16(output + 0x72u, 1u);
    pgf_put_u16(output + 0x74u, 0u);
    first = pgf_resolves_code(p, 0x3042u) ? 1u : 2u;
    pgf_put_u16(output + 0x76u, first);
    pgf_put_u16(output + 0x78u, 0u);
    pgf_put_u16(output + 0x7au, 1u);
    memcpy(output + 0x7cu, p->image + 0x35u, 64u);
    memcpy(output + 0xbcu, p->file_name, 64u);
    pgf_put_u32(output + 0xfcu, 0u);
    pgf_put_u32(output + 0x100u, 0u);
    output[0x104u] = p->image[0x22u];
    (void)pgf_guest_write(guest_info, output, sizeof(output));
}

static void pgf_fill_char_info(const PgfGlyph *glyph, uint8_t output[PGF_CHAR_INFO_SIZE]) {
    int64_t descender = (int64_t)glyph->y_base - (int64_t)glyph->dimension_height;
    memset(output, 0, PGF_CHAR_INFO_SIZE);
    pgf_put_u32(output + 0x00u, glyph->width);
    pgf_put_u32(output + 0x04u, glyph->height);
    pgf_put_u32(output + 0x08u, (uint32_t)glyph->bitmap_adjust_x);
    pgf_put_u32(output + 0x0cu, (uint32_t)glyph->bitmap_adjust_y);
    pgf_put_u32(output + 0x10u, (uint32_t)glyph->dimension_width);
    pgf_put_u32(output + 0x14u, (uint32_t)glyph->dimension_height);
    pgf_put_u32(output + 0x18u, (uint32_t)glyph->y_base);
    pgf_put_u32(output + 0x1cu, (uint32_t)(int32_t)descender);
    pgf_put_u32(output + 0x20u, (uint32_t)glyph->x_left);
    pgf_put_u32(output + 0x24u, (uint32_t)glyph->y_base);
    pgf_put_u32(output + 0x28u, (uint32_t)glyph->x_center);
    pgf_put_u32(output + 0x2cu, (uint32_t)glyph->y_top);
    pgf_put_u32(output + 0x30u, (uint32_t)glyph->advance_x);
    pgf_put_u32(output + 0x34u, (uint32_t)glyph->advance_y);
    pgf_put_u16(output + 0x38u, (uint16_t)glyph->shadow_row_order);
    pgf_put_u16(output + 0x3au, (uint16_t)glyph->shadow_id);
}

int pgf_get_char_info(const PGF *p, int char_code, int alt_char_code,
                      uint32_t guest_info) {
    uint8_t output[PGF_CHAR_INFO_SIZE] = {0};
    PgfGlyph glyph;
    if (!pgf_guest_span(guest_info, sizeof(output), 1)) return 0;
    (void)pgf_guest_write(guest_info, output, sizeof(output));
    if (!pgf_lookup(p, char_code, alt_char_code, 1, &glyph)) return 0;
    pgf_fill_char_info(&glyph, output);
    (void)pgf_guest_write(guest_info, output, sizeof(output));
    return 1;
}

static int pgf_get_glyph_by_id(const PGF *p, int glyph_id, PgfGlyph *glyph) {
    if (!p || glyph_id < 0 || (uint32_t)glyph_id >= p->glyph_count) return 0;
    return pgf_checked_glyph(p, (uint32_t)glyph_id, glyph);
}

static int pgf_floor_64(int32_t value, int64_t *integer, uint32_t *fraction) {
    int64_t quotient = (int64_t)value / 64;
    int64_t remainder = (int64_t)value % 64;
    if (remainder < 0) {
        --quotient;
        remainder += 64;
    }
    *integer = quotient;
    *fraction = (uint32_t)remainder;
    return 1;
}

static int pgf_nibble(const PGF *p, size_t bitmap_offset, uint64_t nibble_index,
                      uint8_t *value) {
    uint64_t byte_index = nibble_index / 2u;
    size_t absolute;
    if (byte_index > SIZE_MAX || bitmap_offset > p->size ||
        (size_t)byte_index >= p->size - bitmap_offset) {
        return 0;
    }
    absolute = bitmap_offset + (size_t)byte_index;
    *value = (nibble_index & 1u) == 0u ?
             (uint8_t)(p->image[absolute] & 0x0fu) :
             (uint8_t)(p->image[absolute] >> 4);
    return 1;
}

static void pgf_decode_bitmap(const PGF *p, const PgfGlyph *glyph,
                              uint8_t samples[PGF_MAX_GLYPH_EDGE * PGF_MAX_GLYPH_EDGE]) {
    uint32_t total = glyph->width * glyph->height;
    uint32_t produced = 0;
    uint64_t nibble_index = 0;
    memset(samples, 0, PGF_MAX_GLYPH_EDGE * PGF_MAX_GLYPH_EDGE);
    while (produced < total) {
        uint8_t control;
        uint8_t sample;
        uint32_t run;
        uint32_t count;
        if (!pgf_nibble(p, glyph->bitmap_offset, nibble_index++, &control)) break;
        if (control < 8u) {
            if (!pgf_nibble(p, glyph->bitmap_offset, nibble_index++, &sample)) break;
            run = (uint32_t)control + 1u;
            count = run < total - produced ? run : total - produced;
            memset(samples + produced, sample, count);
            produced += count;
        } else {
            run = 16u - control;
            count = run < total - produced ? run : total - produced;
            while (count != 0u) {
                if (!pgf_nibble(p, glyph->bitmap_offset, nibble_index++, &sample)) {
                    return;
                }
                samples[produced++] = sample;
                --count;
            }
        }
    }
}

static uint8_t pgf_source_sample(const PgfGlyph *glyph,
                                const uint8_t samples[PGF_MAX_GLYPH_EDGE * PGF_MAX_GLYPH_EDGE],
                                int64_t x, int64_t y) {
    uint64_t index;
    if (x < 0 || y < 0 || (uint64_t)x >= glyph->width ||
        (uint64_t)y >= glyph->height) {
        return 0u;
    }
    if (glyph->row_order == 1u) {
        index = (uint64_t)y * glyph->width + (uint64_t)x;
    } else {
        index = (uint64_t)x * glyph->height + (uint64_t)y;
    }
    return samples[(size_t)index];
}

static uint8_t pgf_interpolated_sample(const PgfGlyph *glyph,
                                      const uint8_t samples[PGF_MAX_GLYPH_EDGE * PGF_MAX_GLYPH_EDGE],
                                      uint32_t fx, uint32_t fy,
                                      uint32_t ox, uint32_t oy) {
    int upper = (int)fx * pgf_source_sample(glyph, samples,
                                           (int64_t)ox - 1, (int64_t)oy - 1) +
                (int)(64u - fx) * pgf_source_sample(glyph, samples, ox, (int64_t)oy - 1);
    int lower = (int)fx * pgf_source_sample(glyph, samples,
                                           (int64_t)ox - 1, oy) +
                (int)(64u - fx) * pgf_source_sample(glyph, samples, ox, oy);
    return (uint8_t)((upper * (int)fy + lower * (int)(64u - fy)) / 4096);
}

static int pgf_guest_row_span(uint32_t base, uint64_t row_offset,
                              uint64_t byte_offset, uint64_t byte_count,
                              uint32_t *address) {
    uint64_t start = (uint64_t)base + row_offset + byte_offset;
    uint64_t end = start + byte_count;
    if (byte_count == 0u || start > UINT32_MAX || end > 0x100000000ULL ||
        byte_count > UINT32_MAX || g_mem == NULL ||
        !sr_guest_span_writable((uint32_t)start, (uint32_t)byte_count)) {
        return 0;
    }
    *address = (uint32_t)start;
    return 1;
}

static void pgf_store_pixel(uint32_t address, uint32_t x, uint32_t format,
                            uint8_t sample) {
    uint8_t *destination = SR_HOST(address);
    uint8_t old;
    if (format == 0u || format == 1u) {
        uint8_t shift = (uint8_t)(((x & 1u) ^ (format == 1u ? 1u : 0u)) * 4u);
        uint8_t mask = (uint8_t)(0x0fu << shift);
        old = *destination;
        *destination = (uint8_t)((old & (uint8_t)~mask) |
                                 ((uint8_t)(sample & 0x0fu) << shift));
    } else if (format == 2u) {
        *destination = sample;
    } else if (format == 3u) {
        destination[0] = sample;
        destination[1] = sample;
        destination[2] = sample;
    } else {
        destination[0] = sample;
        destination[1] = sample;
        destination[2] = sample;
        destination[3] = sample;
    }
}

static int pgf_draw(const PGF *p, const PgfGlyph *glyph, uint32_t guest_image) {
    uint8_t image[PGF_GLYPH_IMAGE_SIZE];
    uint8_t samples[PGF_MAX_GLYPH_EDGE * PGF_MAX_GLYPH_EDGE];
    uint32_t format;
    int32_t x_position;
    int32_t y_position;
    uint32_t buffer_width;
    uint32_t buffer_height;
    uint32_t bytes_per_line;
    uint32_t buffer_address;
    uint64_t addressable_width;
    uint64_t writable_width;
    uint64_t unit_bytes;
    uint64_t footprint_width;
    uint64_t footprint_height;
    uint64_t actual_x0;
    uint64_t actual_x1;
    uint64_t dirty_x0;
    uint64_t dirty_x1;
    int64_t base_x;
    int64_t base_y;
    uint32_t fraction_x;
    uint32_t fraction_y;
    int64_t pixel_x1;
    int64_t pixel_y1;
    uint32_t row;
    uint32_t dirty_count = 0;
    PgfDirtySpan dirty[PGF_MAX_GLYPH_EDGE + 1u];

    if (!p || !glyph || glyph->width == 0u || glyph->height == 0u ||
        glyph->width > PGF_MAX_GLYPH_EDGE || glyph->height > PGF_MAX_GLYPH_EDGE ||
        (uint64_t)glyph->width * glyph->height > PGF_MAX_SAMPLES ||
        (glyph->row_order != 1u && glyph->row_order != 2u) ||
        !pgf_guest_read(guest_image, image, sizeof(image))) {
        return 0;
    }
    format = pgf_u32(image);
    x_position = pgf_i32(image + 4u);
    y_position = pgf_i32(image + 8u);
    buffer_width = pgf_u16(image + 0x0cu);
    buffer_height = pgf_u16(image + 0x0eu);
    bytes_per_line = pgf_u16(image + 0x10u);
    buffer_address = pgf_u32(image + 0x14u);
    if (format > 4u || buffer_address == 0u || buffer_width == 0u ||
        buffer_height == 0u || bytes_per_line == 0u) {
        return 0;
    }
    if (format <= 1u) {
        addressable_width = (uint64_t)bytes_per_line * 2u;
        unit_bytes = 1u;
    } else if (format == 2u) {
        addressable_width = bytes_per_line;
        unit_bytes = 1u;
    } else if (format == 3u) {
        addressable_width = bytes_per_line / 3u;
        unit_bytes = 3u;
    } else {
        addressable_width = bytes_per_line / 4u;
        unit_bytes = 4u;
    }
    if (addressable_width == 0u) return 0;
    writable_width = buffer_width < addressable_width ? buffer_width : addressable_width;
    pgf_floor_64(x_position, &base_x, &fraction_x);
    pgf_floor_64(y_position, &base_y, &fraction_y);
    footprint_width = glyph->width + (fraction_x != 0u ? 1u : 0u);
    footprint_height = glyph->height + (fraction_y != 0u ? 1u : 0u);
    pixel_x1 = base_x + (int64_t)footprint_width;
    pixel_y1 = base_y + (int64_t)footprint_height;
    actual_x0 = base_x < 0 ? 0u : (uint64_t)base_x;
    actual_x1 = pixel_x1 < 0 ? 0u : (uint64_t)pixel_x1;
    if (actual_x1 > writable_width) actual_x1 = writable_width;
    if (actual_x0 >= actual_x1) return 0;

    pgf_decode_bitmap(p, glyph, samples);
    if (base_y < 0) {
        uint64_t skip = (uint64_t)(-base_y);
        row = skip >= footprint_height ? (uint32_t)footprint_height : (uint32_t)skip;
    } else {
        row = 0u;
    }
    while (row < footprint_height) {
        int64_t y = base_y + (int64_t)row;
        uint64_t row_offset;
        uint64_t byte_start;
        uint64_t byte_end;
        uint32_t ignored_address;
        if (y >= (int64_t)buffer_height) break;
        if (y >= 0) {
            if ((uint64_t)y > UINT64_MAX / bytes_per_line) return 0;
            row_offset = (uint64_t)y * bytes_per_line;
            if (format <= 1u) {
                byte_start = actual_x0 / 2u;
                byte_end = (actual_x1 + 1u) / 2u;
            } else {
                byte_start = actual_x0 * unit_bytes;
                byte_end = actual_x1 * unit_bytes;
            }
            if (byte_end <= byte_start ||
                !pgf_guest_row_span(buffer_address, row_offset, byte_start,
                                    byte_end - byte_start, &ignored_address)) {
                return 0;
            }
        }
        ++row;
    }

    dirty_x0 = base_x < 0 ? 0u : (uint64_t)base_x;
    dirty_x1 = pixel_x1 < 0 ? 0u : (uint64_t)pixel_x1;
    if (dirty_x1 > addressable_width) dirty_x1 = addressable_width;
    if (dirty_x0 < dirty_x1) {
        int64_t y0 = base_y < 0 ? 0 : base_y;
        int64_t y1 = pixel_y1 > (int64_t)buffer_height ?
                     (int64_t)buffer_height : pixel_y1;
        for (; y0 < y1; ++y0) {
            uint64_t row_offset = (uint64_t)y0 * bytes_per_line;
            uint64_t first_unit = dirty_x0 / (format <= 1u ? 2u : 1u);
            uint64_t last_unit = (dirty_x1 + (format <= 1u ? 1u : 0u)) /
                                 (format <= 1u ? 2u : 1u);
            uint64_t byte_start = first_unit * unit_bytes;
            uint64_t byte_count = (last_unit - first_unit) * unit_bytes;
            uint32_t address;
            if (byte_count == 0u || byte_count > UINT32_MAX ||
                !pgf_guest_row_span(buffer_address, row_offset, byte_start,
                                    byte_count, &address)) {
                return 0;
            }
            dirty[dirty_count].address = address;
            dirty[dirty_count].size = (uint32_t)byte_count;
            ++dirty_count;
        }
    }

    for (row = 0; row < footprint_height; ++row) {
        int64_t y = base_y + (int64_t)row;
        uint32_t x;
        if (y < 0 || y >= (int64_t)buffer_height) continue;
        for (x = (uint32_t)actual_x0; (uint64_t)x < actual_x1; ++x) {
            uint32_t ox = (uint32_t)((int64_t)x - base_x);
            uint8_t sample = pgf_interpolated_sample(glyph, samples,
                                                     fraction_x, fraction_y,
                                                     ox, row);
            uint64_t byte_offset = format <= 1u ? x / 2u : (uint64_t)x * unit_bytes;
            uint64_t address64 = (uint64_t)buffer_address +
                                 (uint64_t)y * bytes_per_line + byte_offset;
            pgf_store_pixel((uint32_t)address64, x, format, sample);
        }
    }
    for (row = 0; row < dirty_count; ++row) {
        sr_gpu_vram_dirty(dirty[row].address, dirty[row].size);
    }
    return 1;
}

int pgf_draw_glyph(const PGF *p, int char_code, int alt_char_code,
                   uint32_t guest_image) {
    PgfGlyph glyph;
    if (!pgf_lookup(p, char_code, alt_char_code, 1, &glyph)) return 0;
    return pgf_draw(p, &glyph, guest_image);
}

int pgf_draw_glyph_by_id(const PGF *p, int glyph_id, uint32_t guest_image) {
    PgfGlyph glyph;
    if (!pgf_get_glyph_by_id(p, glyph_id, &glyph)) return 0;
    return pgf_draw(p, &glyph, guest_image);
}
