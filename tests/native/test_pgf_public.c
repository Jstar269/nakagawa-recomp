// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#define _POSIX_C_SOURCE 200809L

#include "pgf_api.h"
#include "recomp.h"
#include "ge_shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scratch root for the file this test writes: the checkout's build/ by default, or the
 * BUILD_ROOT the Makefile passes as -DSR_SELFTEST_BUILD_ROOT, so a scratch run never touches
 * the checkout. The product stores only the basename, so the name checks stay bare. */
#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#define TEST_HEADER_SIZE 392u
#define TEST_REV3_HEADER_SIZE 412u
#define TEST_FONT_INFO_SIZE 0x108u
#define TEST_CHAR_INFO_SIZE 0x3cu
#define TEST_GLYPH_IMAGE_SIZE 0x18u
#define TEST_GUEST_BASE 0x08010000u
#define TEST_INFO_ADDR (TEST_GUEST_BASE + 0x100u)
#define TEST_IMAGE_ADDR (TEST_GUEST_BASE + 0x500u)
#define TEST_BUFFER_ADDR (TEST_GUEST_BASE + 0x1000u)
#define TEST_GUEST_SIZE 0x04000000u
#define TEST_FONT_CAP 32768u
#define TEST_MAX_GLYPHS 4u
#define TEST_MAX_MAP 32u
#define TEST_MAX_TABLE 4u

typedef struct TestGlyph {
    uint8_t flags;
    uint8_t indexes[4];
    uint32_t width;
    uint32_t height;
    int32_t adjustment_x;
    int32_t adjustment_y;
    uint32_t row_order;
    uint32_t shadow_id;
    uint32_t shadow_row_order;
    uint16_t composite[3];
    int32_t inline_pairs[3][2];
    uint8_t bitmap[256];
    size_t bitmap_size;
} TestGlyph;

typedef struct TestConfig {
    uint32_t revision;
    uint32_t base_header;
    uint32_t first_glyph;
    uint32_t glyph_count;
    /* Inclusive code span from first_glyph; 0 means glyph_count (a dense font). A
       sparse font has a code span larger than its character-pointer count. */
    uint32_t code_span;
    uint32_t char_map_count;
    uint32_t char_map_bits;
    uint32_t char_pointer_bits;
    uint32_t map_values[TEST_MAX_MAP];
    uint8_t metric_counts[4];
    int32_t table_pairs[4][TEST_MAX_TABLE][2];
    uint32_t shadow_count;
    uint32_t shadow_bits;
    uint16_t shadow_codes[TEST_MAX_MAP];
    uint16_t revision3_counts[2];
    TestGlyph glyphs[TEST_MAX_GLYPHS];
} TestConfig;

typedef struct TestFont {
    uint8_t bytes[TEST_FONT_CAP];
    size_t size;
    size_t char_map_offset;
    size_t char_pointer_offset;
    size_t glyph_data_offset;
    size_t glyph_record_offsets[TEST_MAX_GLYPHS];
    size_t glyph_record_sizes[TEST_MAX_GLYPHS];
    size_t glyph_bitmap_offsets[TEST_MAX_GLYPHS];
} TestFont;

typedef struct TestDirty {
    uint32_t address;
    uint32_t size;
} TestDirty;

static int failures;
static TestDirty dirty_spans[256];
static size_t dirty_count;

uint8_t *g_mem;

void sr_gpu_vram_dirty(uint32_t address, uint32_t bytes) {
    if (dirty_count < sizeof(dirty_spans) / sizeof(dirty_spans[0])) {
        dirty_spans[dirty_count].address = address;
        dirty_spans[dirty_count].size = bytes;
        ++dirty_count;
    }
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static uint16_t test_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t test_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_put_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void test_put_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static size_t test_packed_size(uint32_t count, uint32_t bits) {
    uint64_t total = (uint64_t)count * bits;
    return (size_t)(((total + 31u) / 32u) * 4u);
}

static void test_set_bits(uint8_t *bytes, size_t byte_count, uint32_t index,
                          uint32_t bits, uint32_t value) {
    uint64_t start = (uint64_t)index * bits;
    uint32_t bit;
    for (bit = 0; bit < bits; ++bit) {
        uint64_t pos = start + bit;
        size_t byte = (size_t)(pos / 8u);
        uint8_t mask = (uint8_t)(1u << (pos % 8u));
        if (byte >= byte_count) return;
        if (bit < 32u && ((value >> bit) & 1u) != 0u) bytes[byte] |= mask;
        else bytes[byte] &= (uint8_t)~mask;
    }
}

static void test_set_field(uint8_t *bytes, size_t byte_count, uint32_t bit_offset,
                           uint32_t bits, uint32_t value) {
    uint32_t bit;
    for (bit = 0; bit < bits; ++bit) {
        uint64_t pos = (uint64_t)bit_offset + bit;
        size_t byte = (size_t)(pos / 8u);
        uint8_t mask = (uint8_t)(1u << (pos % 8u));
        if (byte >= byte_count) return;
        if (bit < 32u && ((value >> bit) & 1u) != 0u) bytes[byte] |= mask;
        else bytes[byte] &= (uint8_t)~mask;
    }
}

static size_t test_metric_record_size(const TestGlyph *glyph) {
    size_t size = 9u;
    unsigned group;
    for (group = 0; group < 3u; ++group) {
        size += (glyph->flags & (1u << group)) != 0u ? 1u : 8u;
    }
    return size;
}

static void test_default_config(TestConfig *config) {
    unsigned table;
    unsigned glyph;
    memset(config, 0, sizeof(*config));
    config->revision = 2u;
    config->first_glyph = 65u;
    config->glyph_count = 3u;
    config->char_map_count = 3u;
    config->char_map_bits = 2u;
    config->char_pointer_bits = 20u;
    for (table = 0; table < 4u; ++table) config->metric_counts[table] = 1u;
    config->table_pairs[0][0][0] = 64;
    config->table_pairs[0][0][1] = 128;
    config->table_pairs[1][0][0] = -64;
    config->table_pairs[1][0][1] = 64;
    config->table_pairs[2][0][0] = 256;
    config->table_pairs[2][0][1] = 128;
    config->table_pairs[3][0][0] = 384;
    config->table_pairs[3][0][1] = 512;
    for (glyph = 0; glyph < config->glyph_count; ++glyph) {
        config->map_values[glyph] = glyph;
        config->glyphs[glyph].flags = 7u;
        config->glyphs[glyph].width = 1u;
        config->glyphs[glyph].height = 1u;
        config->glyphs[glyph].row_order = 1u;
        config->glyphs[glyph].bitmap[0] = 0x50u; /* control 0, sample 5 */
        config->glyphs[glyph].bitmap_size = 1u;
    }
}

static int test_reserve(TestFont *font, size_t count, size_t *offset) {
    if (font->size > sizeof(font->bytes) || count > sizeof(font->bytes) - font->size) {
        return 0;
    }
    *offset = font->size;
    memset(font->bytes + font->size, 0, count);
    font->size += count;
    return 1;
}

static size_t test_payload_size(const TestGlyph *glyph) {
    return glyph->row_order == 3u ? 6u : glyph->bitmap_size;
}

static int test_build_font(TestFont *font, const TestConfig *config) {
    size_t cursor;
    size_t header_size = config->revision == 3u && config->base_header == 0u ?
                         TEST_REV3_HEADER_SIZE : TEST_HEADER_SIZE;
    size_t shadow_bits = config->shadow_bits != 0u ? config->shadow_bits :
                         (config->shadow_count == 0u ? 0u : 16u);
    size_t length;
    size_t table_offset[4];
    size_t glyph_start;
    uint32_t code_span = config->code_span != 0u ? config->code_span : config->glyph_count;
    unsigned table;
    unsigned glyph_id;

    memset(font, 0, sizeof(*font));
    if (config->glyph_count == 0u || config->glyph_count > TEST_MAX_GLYPHS ||
        config->char_map_count > TEST_MAX_MAP || code_span > TEST_MAX_MAP ||
        config->revision > 3u ||
        config->first_glyph + code_span - 1u > 0xffffu ||
        config->shadow_count > TEST_MAX_MAP ||
        (config->shadow_count == 0u && shadow_bits != 0u && shadow_bits != 16u) ||
        (config->shadow_count != 0u && shadow_bits != 16u)) {
        return 0;
    }
    font->size = header_size;
    memcpy(font->bytes + 4u, "PGF0", 4u);
    test_put_u16(font->bytes, 0u);
    test_put_u16(font->bytes + 2u, (uint16_t)header_size);
    test_put_u32(font->bytes + 8u, config->revision);
    test_put_u32(font->bytes + 12u, 0u);
    test_put_u32(font->bytes + 0x10u, config->char_map_count);
    test_put_u32(font->bytes + 0x14u, config->glyph_count);
    test_put_u32(font->bytes + 0x18u, config->char_map_bits);
    test_put_u32(font->bytes + 0x1cu, config->char_pointer_bits);
    font->bytes[0x22u] = 4u;
    test_put_u32(font->bytes + 0x24u, 640u);
    test_put_u32(font->bytes + 0x28u, 1280u);
    test_put_u32(font->bytes + 0x2cu, 1920u);
    test_put_u32(font->bytes + 0x30u, 3840u);
    memcpy(font->bytes + 0x35u, "Synthetic PGF", 13u);
    test_put_u16(font->bytes + 0xb6u, (uint16_t)config->first_glyph);
    test_put_u16(font->bytes + 0xb8u,
                 (uint16_t)(config->first_glyph + code_span - 1u));
    test_put_u32(font->bytes + 0xd4u, 640u);
    test_put_u32(font->bytes + 0xd8u, 128u);
    test_put_u32(font->bytes + 0xdcu, (uint32_t)-64);
    test_put_u32(font->bytes + 0xe0u, 256u);
    test_put_u32(font->bytes + 0xe4u, 64u);
    test_put_u32(font->bytes + 0xe8u, 128u);
    test_put_u32(font->bytes + 0xecu, 384u);
    test_put_u32(font->bytes + 0xf0u, 512u);
    test_put_u32(font->bytes + 0xf4u, 768u);
    test_put_u32(font->bytes + 0xf8u, 832u);
    test_put_u16(font->bytes + 0xfcu, 12u);
    test_put_u16(font->bytes + 0xfeu, 13u);
    for (table = 0; table < 4u; ++table) {
        font->bytes[0x102u + table] = config->metric_counts[table];
    }
    test_put_u32(font->bytes + 0x16cu, config->shadow_count);
    test_put_u32(font->bytes + 0x170u, (uint32_t)shadow_bits);
    if (config->revision == 3u && config->base_header == 0u) {
        test_put_u16(font->bytes + 0x18cu, config->revision3_counts[0]);
        test_put_u16(font->bytes + 0x194u, config->revision3_counts[1]);
        test_put_u32(font->bytes + 0x188u, 7u);
        test_put_u32(font->bytes + 0x190u, 9u);
    }

    cursor = font->size;
    for (table = 0; table < 4u; ++table) {
        unsigned entry;
        if (config->metric_counts[table] > TEST_MAX_TABLE ||
            !test_reserve(font, (size_t)config->metric_counts[table] * 8u,
                          &table_offset[table])) {
            return 0;
        }
        for (entry = 0; entry < config->metric_counts[table]; ++entry) {
            size_t pos = table_offset[table] + (size_t)entry * 8u;
            test_put_u32(font->bytes + pos, (uint32_t)config->table_pairs[table][entry][0]);
            test_put_u32(font->bytes + pos + 4u,
                         (uint32_t)config->table_pairs[table][entry][1]);
        }
    }
    if (!test_reserve(font, test_packed_size(config->shadow_count,
                                              (uint32_t)shadow_bits),
                      &cursor)) {
        return 0;
    }
    font->size = cursor + test_packed_size(config->shadow_count,
                                           (uint32_t)shadow_bits);
    for (table = 0; table < config->shadow_count; ++table) {
        test_put_u16(font->bytes + cursor + (size_t)table * 2u,
                     config->shadow_codes[table]);
    }
    if (config->revision == 3u && config->base_header == 0u) {
        size_t bytes = ((size_t)config->revision3_counts[0] +
                        config->revision3_counts[1]) * 4u;
        if (!test_reserve(font, bytes, &cursor)) return 0;
        for (table = 0; table < bytes; ++table) {
            font->bytes[cursor + table] = (uint8_t)(0xa0u + table);
        }
    }
    length = test_packed_size(config->char_map_count, config->char_map_bits);
    if (!test_reserve(font, length, &font->char_map_offset)) return 0;
    for (table = 0; table < config->char_map_count; ++table) {
        test_set_bits(font->bytes + font->char_map_offset, length, table,
                      config->char_map_bits, config->map_values[table]);
    }
    length = test_packed_size(config->glyph_count, config->char_pointer_bits);
    if (!test_reserve(font, length, &font->char_pointer_offset)) return 0;
    font->glyph_data_offset = font->size;
    glyph_start = font->glyph_data_offset;
    for (glyph_id = 0; glyph_id < config->glyph_count; ++glyph_id) {
        const TestGlyph *glyph = &config->glyphs[glyph_id];
        size_t record_size = test_metric_record_size(glyph);
        size_t payload_size = test_payload_size(glyph);
        size_t record_offset;
        size_t cursor_at;
        uint32_t shadow_offset = 0u;
        unsigned group;
        while ((font->size & 3u) != 0u) {
            if (!test_reserve(font, 1u, &record_offset)) return 0;
        }
        record_offset = font->size;
        font->glyph_record_offsets[glyph_id] = record_offset;
        font->glyph_record_sizes[glyph_id] = record_size;
        font->glyph_bitmap_offsets[glyph_id] = record_offset + record_size;
        if (glyph->shadow_id != 0u) {
            shadow_offset = (uint32_t)(record_size + payload_size);
        }
        if (!test_reserve(font, record_size, &cursor_at)) return 0;
        test_set_field(font->bytes + record_offset, record_size, 0u, 14u, shadow_offset);
        test_set_field(font->bytes + record_offset, record_size, 14u, 7u, glyph->width);
        test_set_field(font->bytes + record_offset, record_size, 21u, 7u, glyph->height);
        test_set_field(font->bytes + record_offset, record_size, 28u, 7u,
                      (uint32_t)glyph->adjustment_x & 0x7fu);
        test_set_field(font->bytes + record_offset, record_size, 35u, 7u,
                      (uint32_t)glyph->adjustment_y & 0x7fu);
        test_set_field(font->bytes + record_offset, record_size, 42u, 2u, glyph->row_order);
        test_set_field(font->bytes + record_offset, record_size, 44u, 1u, 0u);
        test_set_field(font->bytes + record_offset, record_size, 45u, 3u, glyph->flags);
        test_set_field(font->bytes + record_offset, record_size, 55u, 9u, glyph->shadow_id);
        cursor_at = record_offset + 8u;
        for (group = 0; group < 3u; ++group) {
            if ((glyph->flags & (1u << group)) != 0u) {
                font->bytes[cursor_at++] = glyph->indexes[group];
            } else {
                test_put_u32(font->bytes + cursor_at,
                             (uint32_t)glyph->inline_pairs[group][0]);
                test_put_u32(font->bytes + cursor_at + 4u,
                             (uint32_t)glyph->inline_pairs[group][1]);
                cursor_at += 8u;
            }
        }
        font->bytes[cursor_at++] = glyph->indexes[3];
        if (cursor_at != record_offset + record_size) return 0;
        if (!test_reserve(font, payload_size, &cursor_at)) return 0;
        if (glyph->row_order == 3u) {
            unsigned part;
            for (part = 0; part < 3u; ++part) {
                test_put_u16(font->bytes + font->glyph_bitmap_offsets[glyph_id] +
                                 (size_t)part * 2u, glyph->composite[part]);
            }
        } else if (payload_size != 0u) {
            memcpy(font->bytes + cursor_at, glyph->bitmap, payload_size);
        }
        if (glyph->shadow_id != 0u) {
            size_t shadow_at = record_offset + shadow_offset;
            size_t current_end = font->size;
            if (shadow_at < current_end || shadow_at > TEST_FONT_CAP - 6u) return 0;
            if (shadow_at > current_end) {
                if (!test_reserve(font, shadow_at - current_end, &cursor_at)) return 0;
            }
            if (!test_reserve(font, 6u, &cursor_at)) return 0;
            test_set_field(font->bytes + shadow_at, 6u, 42u, 2u,
                          glyph->shadow_row_order);
        }
        test_set_bits(font->bytes + font->char_pointer_offset,
                      length, glyph_id, config->char_pointer_bits,
                      (uint32_t)((record_offset - glyph_start) / 4u));
    }
    return font->size <= sizeof(font->bytes);
}

static uint8_t *test_guest(uint32_t address) {
    return SR_HOST(address);
}

static PGF *test_open(const TestFont *font) {
    return pgf_open_memory(font->bytes, font->size);
}

static void test_reset_dirty(void) {
    dirty_count = 0u;
    memset(dirty_spans, 0, sizeof(dirty_spans));
}

static void test_set_image(uint32_t format, int32_t x, int32_t y,
                           uint16_t width, uint16_t height, uint16_t bytes_per_line,
                           uint32_t buffer) {
    uint8_t *record = test_guest(TEST_IMAGE_ADDR);
    memset(record, 0, TEST_GLYPH_IMAGE_SIZE);
    test_put_u32(record, format);
    test_put_u32(record + 4u, (uint32_t)x);
    test_put_u32(record + 8u, (uint32_t)y);
    test_put_u16(record + 0x0cu, width);
    test_put_u16(record + 0x0eu, height);
    test_put_u16(record + 0x10u, bytes_per_line);
    test_put_u16(record + 0x12u, 0xbeefu);
    test_put_u32(record + 0x14u, buffer);
}

static void test_open_headers_and_sections(void) {
    TestConfig config;
    TestFont font;
    PGF *pgf;
    uint8_t *large;
    unsigned revision;
    test_default_config(&config);
    for (revision = 0; revision <= 3u; ++revision) {
        config.revision = revision;
        CHECK(test_build_font(&font, &config));
        pgf = test_open(&font);
        CHECK(pgf != NULL);
        pgf_close(pgf);
    }

    test_default_config(&config);
    config.revision = 3u;
    config.revision3_counts[0] = 2u;
    config.revision3_counts[1] = 1u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_has_char(pgf, 65));
    pgf_close(pgf);
    font.size = TEST_REV3_HEADER_SIZE + 32u + 8u + 3u;
    CHECK(test_open(&font) == NULL);

    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    CHECK(pgf_open_memory(NULL, font.size) == NULL);
    CHECK(pgf_open_memory(font.bytes, 0u) == NULL);
    CHECK(pgf_open_memory(font.bytes, TEST_HEADER_SIZE - 1u) == NULL);
    CHECK(pgf_open_memory(font.bytes, 16u * 1024u * 1024u + 1u) == NULL);
    large = (uint8_t *)calloc(1u, 16u * 1024u * 1024u);
    CHECK(large != NULL);
    if (large) {
        memcpy(large, font.bytes, font.size);
        pgf = pgf_open_memory(large, 16u * 1024u * 1024u);
        CHECK(pgf != NULL);
        pgf_close(pgf);
        free(large);
    }
    CHECK(pgf_open(NULL) == NULL);
    CHECK(pgf_open("missing-synthetic-pgf-file") == NULL);

    font.bytes[0] = 1u;
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    font.bytes[0] = 1u;
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_put_u16(font.bytes + 2u, 391u);
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    memcpy(font.bytes + 4u, "BAD!", 4u);
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_put_u32(font.bytes + 8u, UINT32_MAX);
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_put_u32(font.bytes + 8u, 4u);
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_put_u32(font.bytes + 12u, UINT32_MAX);
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_put_u16(font.bytes + 0xb6u, 67u);
    test_put_u16(font.bytes + 0xb8u, 65u);
    CHECK(test_open(&font) == NULL);

    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_put_u32(font.bytes + 0x10u, 1048577u);
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_put_u32(font.bytes + 0x14u, 1048577u);
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    /* Two character pointers under a three-code map is a sparse font (PGF_SPEC.md
       3.1, O-15): the pointer count is the glyph count, and code 'C' maps to glyph 2,
       which is at or above it, so 'C' is a lookup miss. The pointer section keeps the
       same packed size, so both remaining glyph records still validate. */
    test_put_u32(font.bytes + 0x14u, 2u);
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    if (pgf) {
        CHECK(pgf_has_char(pgf, 65) && pgf_has_char(pgf, 66));
        CHECK(!pgf_has_char(pgf, 67));
        pgf_close(pgf);
    }
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_put_u32(font.bytes + 0x16cu, 1048577u);
    CHECK(test_open(&font) == NULL);

    test_default_config(&config);
    config.char_map_bits = 0u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.char_map_bits = 33u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.char_pointer_bits = 0u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.char_pointer_bits = 33u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.char_map_bits = 1u;
    config.char_pointer_bits = 32u;
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL && pgf_has_char(pgf, 65));
    pgf_close(pgf);
    test_default_config(&config);
    config.char_map_count = 0u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL && !pgf_has_char(pgf, 65));
    pgf_close(pgf);
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_put_u32(font.bytes + 0x170u, 16u);
    pgf = test_open(&font);
    CHECK(pgf != NULL && pgf_has_char(pgf, 65));
    pgf_close(pgf);
    test_put_u32(font.bytes + 0x170u, 8u);
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.shadow_count = 1u;
    config.shadow_codes[0] = 65u;
    CHECK(test_build_font(&font, &config));
    test_put_u32(font.bytes + 0x170u, 15u);
    CHECK(test_open(&font) == NULL);

    /* Every section class is independently truncated at its final byte. */
    test_default_config(&config);
    config.metric_counts[0] = 1u;
    CHECK(test_build_font(&font, &config));
    font.size = TEST_HEADER_SIZE + 7u;
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.metric_counts[0] = 0u;
    config.metric_counts[1] = 1u;
    CHECK(test_build_font(&font, &config));
    font.size = TEST_HEADER_SIZE + 7u;
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.metric_counts[0] = config.metric_counts[1] = 0u;
    config.metric_counts[2] = 1u;
    CHECK(test_build_font(&font, &config));
    font.size = TEST_HEADER_SIZE + 7u;
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.metric_counts[0] = config.metric_counts[1] = config.metric_counts[2] = 0u;
    config.metric_counts[3] = 1u;
    CHECK(test_build_font(&font, &config));
    font.size = TEST_HEADER_SIZE + 7u;
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    memset(config.metric_counts, 0, sizeof(config.metric_counts));
    config.shadow_count = 1u;
    config.shadow_codes[0] = 65u;
    CHECK(test_build_font(&font, &config));
    font.size = TEST_HEADER_SIZE + 3u;
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.revision = 3u;
    config.revision3_counts[0] = 1u;
    CHECK(test_build_font(&font, &config));
    font.size = TEST_REV3_HEADER_SIZE + 3u;
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.revision = 3u;
    config.revision3_counts[1] = 1u;
    CHECK(test_build_font(&font, &config));
    font.size = TEST_REV3_HEADER_SIZE + 3u;
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.char_map_count = 1u;
    CHECK(test_build_font(&font, &config));
    font.size = font.char_map_offset + 3u;
    CHECK(test_open(&font) == NULL);
    test_default_config(&config);
    config.char_map_count = 0u;
    CHECK(test_build_font(&font, &config));
    font.size = font.char_pointer_offset + 3u;
    CHECK(test_open(&font) == NULL);
}

static void test_maps_pointers_and_metrics(void) {
    TestConfig config;
    TestFont font;
    PGF *pgf;
    uint8_t expected[TEST_CHAR_INFO_SIZE];
    unsigned glyph;
    unsigned flags;
    size_t cut;
    test_default_config(&config);
    config.char_map_bits = 5u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_has_char(pgf, 65));
    CHECK(pgf_has_char(pgf, 66));
    CHECK(pgf_has_char(pgf, 67));
    CHECK(!pgf_has_char(pgf, 64));
    CHECK(!pgf_has_char(pgf, 68));
    CHECK(!pgf_has_char(pgf, -1));
    pgf_close(pgf);

    test_default_config(&config);
    config.char_map_count = 2u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_has_char(pgf, 65) && pgf_has_char(pgf, 66));
    CHECK(!pgf_has_char(pgf, 67));
    pgf_close(pgf);

    test_default_config(&config);
    config.char_map_bits = 5u;
    config.map_values[1] = 31u;
    config.map_values[2] = 4u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_has_char(pgf, 65));
    CHECK(!pgf_has_char(pgf, 66));
    CHECK(!pgf_has_char(pgf, 67));
    pgf_close(pgf);

    test_default_config(&config);
    config.char_map_bits = 32u;
    config.char_pointer_bits = 1u;
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_has_char(pgf, 65));
    pgf_close(pgf);

    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_set_bits(font.bytes + font.char_pointer_offset,
                  test_packed_size(config.glyph_count, config.char_pointer_bits),
                  0u, config.char_pointer_bits, 0xfffffu);
    CHECK(test_open(&font) == NULL);

    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_set_bits(font.bytes + font.char_pointer_offset,
                  test_packed_size(config.glyph_count, config.char_pointer_bits),
                  1u, config.char_pointer_bits, 0u);
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 66, 0, TEST_INFO_ADDR));
    CHECK(test_u32(test_guest(TEST_INFO_ADDR)) == 1u);
    pgf_close(pgf);

    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    for (cut = 0; cut < font.glyph_record_sizes[0]; ++cut) {
        TestFont bad = font;
        bad.size = bad.glyph_record_offsets[0] + cut;
        CHECK(test_open(&bad) == NULL);
    }

    for (flags = 0; flags < 8u; ++flags) {
        size_t inline_count = 3u;
        test_default_config(&config);
        config.glyph_count = 1u;
        config.char_map_count = 1u;
        config.map_values[0] = 0u;
        config.glyphs[0].flags = (uint8_t)flags;
        CHECK(test_build_font(&font, &config));
        for (glyph = 0; glyph < 3u; ++glyph) {
            if ((flags & (1u << glyph)) != 0u) --inline_count;
        }
        CHECK(font.glyph_record_sizes[0] == 12u + inline_count * 7u);
        pgf = test_open(&font);
        CHECK(pgf != NULL);
        CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
        memset(test_guest(TEST_BUFFER_ADDR), 0xee, 4u);
        test_reset_dirty();
        test_set_image(2u, 0, 0, 1u, 1u, 1u, TEST_BUFFER_ADDR);
        CHECK(pgf_draw_glyph_by_id(pgf, 0, TEST_IMAGE_ADDR));
        CHECK(test_guest(TEST_BUFFER_ADDR)[0] == 5u);
        pgf_close(pgf);
    }

    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    memcpy(expected, test_guest(TEST_INFO_ADDR), sizeof(expected));
    pgf_close(pgf);
    test_set_field(font.bytes + font.glyph_record_offsets[0],
                   font.size - font.glyph_record_offsets[0], 44u, 1u, 1u);
    test_set_field(font.bytes + font.glyph_record_offsets[0],
                   font.size - font.glyph_record_offsets[0], 48u, 7u, 0x7fu);
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    CHECK(memcmp(test_guest(TEST_INFO_ADDR), expected, sizeof(expected)) == 0);
    pgf_close(pgf);

    test_default_config(&config);
    config.glyphs[0].flags = 0u;
    config.glyphs[0].inline_pairs[0][0] = 320;
    config.glyphs[0].inline_pairs[0][1] = 448;
    config.glyphs[0].inline_pairs[1][0] = -128;
    config.glyphs[0].inline_pairs[1][1] = 192;
    config.glyphs[0].inline_pairs[2][0] = 128;
    config.glyphs[0].inline_pairs[2][1] = 64;
    config.glyphs[0].adjustment_x = -64;
    config.glyphs[0].adjustment_y = 63;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    memset(expected, 0, sizeof(expected));
    test_put_u32(expected + 0x00u, 1u);
    test_put_u32(expected + 0x04u, 1u);
    test_put_u32(expected + 0x08u, (uint32_t)-64);
    test_put_u32(expected + 0x0cu, 63u);
    test_put_u32(expected + 0x10u, 320u);
    test_put_u32(expected + 0x14u, 448u);
    test_put_u32(expected + 0x18u, 128u);
    test_put_u32(expected + 0x1cu, (uint32_t)-320);
    test_put_u32(expected + 0x20u, (uint32_t)-128);
    test_put_u32(expected + 0x24u, 128u);
    test_put_u32(expected + 0x28u, 192u);
    test_put_u32(expected + 0x2cu, 64u);
    test_put_u32(expected + 0x30u, 384u);
    test_put_u32(expected + 0x34u, 512u);
    CHECK(memcmp(test_guest(TEST_INFO_ADDR), expected, sizeof(expected)) == 0);
    CHECK(pgf_get_char_info(pgf, 64, 65, TEST_INFO_ADDR) == 0);
    CHECK(memcmp(test_guest(TEST_INFO_ADDR), (uint8_t[TEST_CHAR_INFO_SIZE]){0},
                 TEST_CHAR_INFO_SIZE) == 0);
    pgf_close(pgf);

    test_default_config(&config);
    config.glyphs[0].flags = 3u;
    config.glyphs[0].indexes[0] = 1u;
    config.metric_counts[0] = 2u;
    config.table_pairs[0][1][0] = 700;
    config.table_pairs[0][1][1] = 900;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x10u) == 700u);
    CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x14u) == 900u);
    pgf_close(pgf);

    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    for (glyph = 0; glyph < 4u; ++glyph) {
        TestFont bad = font;
        uint32_t bad_index = 1u;
        size_t index_offset = bad.glyph_record_offsets[0] + 8u;
        if (glyph < 3u) {
            bad.bytes[index_offset + glyph] = (uint8_t)bad_index;
        } else {
            bad.bytes[index_offset + 3u] = (uint8_t)bad_index;
        }
        CHECK(test_open(&bad) == NULL);
    }

    test_default_config(&config);
    config.glyphs[0].flags = 0u;
    config.glyphs[0].inline_pairs[0][1] = INT32_MIN;
    config.glyphs[0].inline_pairs[2][0] = INT32_MAX;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
}

static void test_shadow_and_character_info(void) {
    TestConfig config;
    TestFont font;
    PGF *pgf;
    test_default_config(&config);
    config.shadow_count = 2u;
    config.shadow_codes[0] = 65u;
    config.shadow_codes[1] = 66u;
    config.glyphs[0].shadow_id = 1u;
    config.glyphs[0].shadow_row_order = 2u;
    config.glyphs[1].shadow_id = 2u;
    config.glyphs[1].shadow_row_order = 3u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    CHECK(test_u16(test_guest(TEST_INFO_ADDR) + 0x38u) == 2u);
    CHECK(test_u16(test_guest(TEST_INFO_ADDR) + 0x3au) == 1u);
    CHECK(pgf_get_char_info(pgf, 66, 0, TEST_INFO_ADDR));
    CHECK(test_u16(test_guest(TEST_INFO_ADDR) + 0x38u) == 3u);
    CHECK(test_u16(test_guest(TEST_INFO_ADDR) + 0x3au) == 2u);
    pgf_close(pgf);

    test_default_config(&config);
    config.shadow_count = 1u;
    config.shadow_codes[0] = 65u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_has_char(pgf, 65));
    pgf_close(pgf);

    test_default_config(&config);
    config.shadow_count = 1u;
    config.shadow_codes[0] = 65u;
    config.glyphs[0].shadow_id = 2u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);

    test_default_config(&config);
    config.shadow_count = 1u;
    config.shadow_codes[0] = 65u;
    config.glyphs[0].shadow_id = 1u;
    CHECK(test_build_font(&font, &config));
    test_set_field(font.bytes + font.glyph_record_offsets[0],
                  font.size - font.glyph_record_offsets[0], 0u, 14u, 0u);
    CHECK(test_open(&font) == NULL);
    test_set_field(font.bytes + font.glyph_record_offsets[0],
                  font.size - font.glyph_record_offsets[0], 0u, 14u,
                  (uint32_t)(font.glyph_record_sizes[0] - 1u));
    CHECK(test_open(&font) == NULL);
    test_set_field(font.bytes + font.glyph_record_offsets[0],
                   font.size - font.glyph_record_offsets[0], 0u, 14u,
                  (uint32_t)(font.size - font.glyph_record_offsets[0] + 1u));
    CHECK(test_open(&font) == NULL);
    test_set_field(font.bytes + font.glyph_record_offsets[0],
                   font.size - font.glyph_record_offsets[0], 0u, 14u,
                   (uint32_t)(font.size - font.glyph_record_offsets[0] - 5u));
    CHECK(test_open(&font) == NULL);

    test_default_config(&config);
    config.shadow_count = 1u;
    config.shadow_codes[0] = 99u;
    config.glyphs[0].shadow_id = 1u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);

    test_default_config(&config);
    config.map_values[0] = 3u;
    config.shadow_count = 1u;
    config.shadow_codes[0] = 65u;
    config.glyphs[0].shadow_id = 1u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);

    test_default_config(&config);
    config.glyphs[0].width = 0u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(!pgf_has_char(pgf, 65));
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    CHECK(test_u32(test_guest(TEST_INFO_ADDR)) == 0u);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
    pgf_close(pgf);

    test_default_config(&config);
    config.glyphs[0].row_order = 0u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    test_set_image(2u, 0, 0, 1u, 1u, 1u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
    pgf_close(pgf);

    /* A row-order-3 record's bitmap offset holds three character codes, not an
       RLE stream, so a payload that cannot supply six bytes is invalid. */
    test_default_config(&config);
    config.glyphs[0].row_order = 3u;
    CHECK(test_build_font(&font, &config));
    font.size = font.glyph_bitmap_offsets[0] + 1u;
    CHECK(test_open(&font) == NULL);
}

/* Row order 3: the record's bitmap offset holds three little-endian character
   codes that resolve through the direct character map and are overlaid. Glyph 0
   is the composite; glyphs 1..3 are its raster components. */
static void test_composite_config(TestConfig *config) {
    test_default_config(config);
    config->glyph_count = 4u;
    config->char_map_count = 4u;
    config->char_map_bits = 3u;
    config->map_values[3] = 3u;
    config->metric_counts[3] = 2u;
    config->table_pairs[3][1][0] = 512;
    config->table_pairs[3][1][1] = 768;
    config->glyphs[0].width = 3u;
    config->glyphs[0].height = 4u;
    config->glyphs[0].adjustment_x = -5;
    config->glyphs[0].adjustment_y = 7;
    config->glyphs[0].row_order = 3u;
    config->glyphs[0].indexes[3] = 1u;
    config->glyphs[0].composite[0] = 66u;
    config->glyphs[0].composite[1] = 67u;
    config->glyphs[0].composite[2] = 68u;
    /* Component 1, row order 1: rows (4,1) and (0,0). */
    config->glyphs[1].width = 2u;
    config->glyphs[1].height = 2u;
    config->glyphs[1].bitmap[0] = 0x48u;
    config->glyphs[1].bitmap[1] = 0x01u;
    config->glyphs[1].bitmap[2] = 0x00u;
    config->glyphs[1].bitmap_size = 3u;
    /* Component 2, row order 1: rows (0,6) and (0,3). */
    config->glyphs[2].width = 2u;
    config->glyphs[2].height = 2u;
    config->glyphs[2].bitmap[0] = 0x08u;
    config->glyphs[2].bitmap[1] = 0x06u;
    config->glyphs[2].bitmap[2] = 0x03u;
    config->glyphs[2].bitmap_size = 3u;
    /* Component 3, row order 2: rows (7,0) and (0,2). */
    config->glyphs[3].width = 2u;
    config->glyphs[3].height = 2u;
    config->glyphs[3].row_order = 2u;
    config->glyphs[3].bitmap[0] = 0x78u;
    config->glyphs[3].bitmap[1] = 0x00u;
    config->glyphs[3].bitmap[2] = 0x02u;
    config->glyphs[3].bitmap_size = 3u;
}

static void test_composite_glyphs(void) {
    static const uint8_t expected[4] = {7u, 6u, 0u, 3u};
    static const uint8_t expected_half[6] = {3u, 3u, 3u, 0u, 1u, 1u};
    TestConfig config;
    TestFont font;
    PGF *pgf;
    uint8_t *buffer = test_guest(TEST_BUFFER_ADDR);

    test_composite_config(&config);
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_has_char(pgf, 65));
    CHECK(pgf_has_char(pgf, 66));
    /* The composite reports its own record's metrics, not a component's. */
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x00u) == 3u);
    CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x04u) == 4u);
    CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x08u) == -5);
    CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x0cu) == 7);
    CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x30u) == 512);
    CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x34u) == 768);
    CHECK(pgf_get_char_info(pgf, 66, 0, TEST_INFO_ADDR));
    CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x00u) == 2u);
    CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x30u) == 384);

    /* fx = fy = 0: three components overlay, the first one overwriting. */
    memset(buffer, 0xee, sizeof(expected_half));
    test_reset_dirty();
    test_set_image(2u, 0, 0, 2u, 2u, 2u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
    CHECK(memcmp(buffer, expected, sizeof(expected)) == 0);
    CHECK(dirty_count == 6u);
    CHECK(dirty_spans[0].address == TEST_BUFFER_ADDR && dirty_spans[0].size == 2u);
    CHECK(dirty_spans[1].address == TEST_BUFFER_ADDR + 2u && dirty_spans[1].size == 2u);
    CHECK(dirty_spans[2].address == TEST_BUFFER_ADDR && dirty_spans[2].size == 2u);
    CHECK(dirty_spans[4].address == TEST_BUFFER_ADDR && dirty_spans[4].size == 2u);

    /* fx = 32: every component grows one footprint column. */
    memset(buffer, 0xee, sizeof(expected_half));
    test_reset_dirty();
    test_set_image(2u, 32, 0, 3u, 2u, 3u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
    CHECK(memcmp(buffer, expected_half, sizeof(expected_half)) == 0);
    CHECK(dirty_count == 6u);
    CHECK(dirty_spans[0].size == 3u);

    /* A packed 4-bit destination keeps partner nibbles through both rules. */
    memset(buffer, 0x6bu, 2u);
    test_reset_dirty();
    test_set_image(1u, 0, 0, 2u, 2u, 1u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
    CHECK(buffer[0] == 0x76u && buffer[1] == 0x03u);
    pgf_close(pgf);

    /* A composite is invalid when its payload, components, or reference graph
       is not, and each case fails closed at open with no handle. */
    test_composite_config(&config);
    CHECK(test_build_font(&font, &config));
    font.size = font.glyph_bitmap_offsets[0] + 4u;
    CHECK(test_open(&font) == NULL);
    test_composite_config(&config);
    config.glyphs[0].composite[0] = 64u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_composite_config(&config);
    config.glyphs[0].composite[0] = 69u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_composite_config(&config);
    config.map_values[1] = 7u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_composite_config(&config);
    config.map_values[1] = 4u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_composite_config(&config);
    config.glyphs[0].composite[0] = 65u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_composite_config(&config);
    config.glyphs[1].row_order = 3u;
    config.glyphs[1].composite[0] = 66u;
    config.glyphs[1].composite[1] = 66u;
    config.glyphs[1].composite[2] = 66u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_composite_config(&config);
    config.glyphs[0].composite[0] = 69u;
    config.glyphs[0].composite[1] = 66u;
    config.glyphs[0].composite[2] = 66u;
    config.glyphs[1].row_order = 3u;
    config.glyphs[1].composite[0] = 65u;
    config.glyphs[1].composite[1] = 65u;
    config.glyphs[1].composite[2] = 65u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_composite_config(&config);
    config.glyphs[2].width = 0u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);
    test_composite_config(&config);
    config.glyphs[2].row_order = 0u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_open(&font) == NULL);

    /* Every component is preflighted before the first pixel: a third component
       whose last row leaves the guest arena fails the whole draw. */
    test_composite_config(&config);
    config.glyphs[1].height = 1u;
    config.glyphs[2].height = 1u;
    config.glyphs[3].height = 3u;
    config.glyphs[3].bitmap[0] = 0x78u;
    config.glyphs[3].bitmap[1] = 0x00u;
    config.glyphs[3].bitmap[2] = 0x20u;
    config.glyphs[3].bitmap[3] = 0x00u;
    config.glyphs[3].bitmap_size = 4u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(test_guest(0x0bfffffcu), 0xc7, 4u);
    test_reset_dirty();
    test_set_image(2u, 0, 0, 2u, 4u, 4u, 0x0bfffffcu);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
    CHECK(test_guest(0x0bfffffcu)[0] == 0xc7u &&
          test_guest(0x0bfffffcu)[1] == 0xc7u &&
          test_guest(0x0bfffffcu)[2] == 0xc7u &&
          test_guest(0x0bfffffcu)[3] == 0xc7u && dirty_count == 0u);
    pgf_close(pgf);
}

static void test_font_information_and_lifetime(void) {
    TestConfig config;
    TestFont font;
    PGF *pgf;
    uint8_t *info = test_guest(TEST_INFO_ADDR);
    uint8_t expected[TEST_FONT_INFO_SIZE] = {0};
    FILE *stream;
    const char *file_name = "pgf_public_synthetic.tmp";
    const char *file_path = SR_SELFTEST_BUILD_ROOT "/pgf_public_synthetic.tmp";

    test_default_config(&config);
    config.first_glyph = 0x3042u;
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(info, 0xa5, TEST_FONT_INFO_SIZE);
    pgf_get_font_info(pgf, TEST_INFO_ADDR);
    CHECK(test_u32(info + 0x00u) == 768u);
    CHECK(test_u32(info + 0x04u) == 832u);
    CHECK(test_u32(info + 0x28u) == 0x41400000u);
    CHECK(test_u32(info + 0x2cu) == 0x41500000u);
    CHECK(test_u32(info + 0x5cu) == 0x41200000u);
    CHECK(test_u32(info + 0x60u) == 0x41a00000u);
    CHECK(test_u32(info + 0x64u) == 0x41f00000u);
    CHECK(test_u32(info + 0x68u) == 0x42700000u);
    CHECK(test_u16(info + 0x70u) == 1u);
    CHECK(test_u16(info + 0x72u) == 1u);
    CHECK(test_u16(info + 0x76u) == 1u);
    CHECK(memcmp(info + 0x7cu, "Synthetic PGF", 13u) == 0);
    CHECK(test_u32(info + 0xfcu) == 0u && test_u32(info + 0x100u) == 0u);
    CHECK(info[0x104u] == 4u);
    CHECK(info[0x105u] == 0u && info[0x106u] == 0u && info[0x107u] == 0u);
    CHECK(memcmp(info + 0xbcu, (uint8_t[64]){0}, 64u) == 0);
    {
        static const uint32_t raw[10] = {768u, 832u, 640u, 128u, 0xffffffc0u,
                                         256u, 64u, 128u, 384u, 512u};
        static const uint32_t fixed[10] = {0x41400000u, 0x41500000u, 0x41200000u,
                                           0x40000000u, 0xbf800000u, 0x40800000u,
                                           0x3f800000u, 0x40000000u, 0x40c00000u,
                                           0x41000000u};
        unsigned i;
        for (i = 0; i < 10u; ++i) {
            test_put_u32(expected + i * 4u, raw[i]);
            test_put_u32(expected + 0x28u + i * 4u, fixed[i]);
        }
        test_put_u16(expected + 0x50u, 12u);
        test_put_u16(expected + 0x52u, 13u);
        test_put_u32(expected + 0x54u, 1u);
        test_put_u32(expected + 0x58u, 0u);
        test_put_u32(expected + 0x5cu, 0x41200000u);
        test_put_u32(expected + 0x60u, 0x41a00000u);
        test_put_u32(expected + 0x64u, 0x41f00000u);
        test_put_u32(expected + 0x68u, 0x42700000u);
        test_put_u16(expected + 0x70u, 1u);
        test_put_u16(expected + 0x72u, 1u);
        test_put_u16(expected + 0x76u, 1u);
        test_put_u16(expected + 0x7au, 1u);
        memcpy(expected + 0x7cu, "Synthetic PGF", 13u);
        expected[0x104u] = 4u;
    }
    CHECK(memcmp(info, expected, sizeof(expected)) == 0);
    pgf_close(pgf);

    test_default_config(&config);
    config.first_glyph = 0x3042u;
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 1u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    pgf_get_font_info(pgf, TEST_INFO_ADDR);
    CHECK(test_u16(test_guest(TEST_INFO_ADDR) + 0x76u) == 2u);
    pgf_close(pgf);

    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    CHECK(test_build_font(&font, &config));
    test_put_u32(font.bytes + 0xf4u, INT32_MAX);
    test_put_u32(font.bytes + 0xf8u, (uint32_t)INT32_MIN);
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    pgf_get_font_info(pgf, TEST_INFO_ADDR);
    CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x28u) == 0x4c000000u);
    CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x2cu) == 0xcc000000u);
    pgf_close(pgf);

    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(font.bytes, 0, font.size);
    CHECK(pgf_has_char(pgf, 65));
    pgf_close(pgf);

    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    stream = fopen(file_path, "wb");
    CHECK(stream != NULL);
    if (stream) {
        CHECK(fwrite(font.bytes, 1u, font.size, stream) == font.size);
        CHECK(fclose(stream) == 0);
        pgf = pgf_open(file_path);
        CHECK(pgf != NULL);
        if (pgf) {
            memset(info, 0, TEST_FONT_INFO_SIZE);
            pgf_get_font_info(pgf, TEST_INFO_ADDR);
            CHECK(memcmp(info + 0xbcu, file_name, strlen(file_name)) == 0);
            CHECK(info[0xbcu + strlen(file_name)] == 0u);
            pgf_close(pgf);
        }
#ifdef _WIN32
        wchar_t wide_path[512];
        CHECK(mbstowcs(wide_path, file_path, sizeof(wide_path) / sizeof(wide_path[0])) != (size_t)-1);
        pgf = pgf_open_w(wide_path);
        CHECK(pgf != NULL);
        if (pgf) {
            memset(info, 0, TEST_FONT_INFO_SIZE);
            pgf_get_font_info(pgf, TEST_INFO_ADDR);
            CHECK(memcmp(info + 0xbcu, file_name, strlen(file_name)) == 0);
            pgf_close(pgf);
        }
#endif
        CHECK(remove(file_path) == 0);
    }
    pgf_get_font_info(NULL, TEST_INFO_ADDR);
    memset(info, 0xa5, TEST_FONT_INFO_SIZE);
    pgf_get_font_info(NULL, TEST_INFO_ADDR);
    CHECK(info[0] == 0xa5u);
}

static void test_draw_rle_formats_and_placement(void) {
    TestConfig config;
    TestFont font;
    PGF *pgf;
    uint8_t *buffer = test_guest(TEST_BUFFER_ADDR);
    static const uint8_t rle_bytes[6] = {0x11u, 0x21u, 0x30u, 0x02u, 0xf2u, 0x50u};
    static const uint8_t row_major[12] = {1u, 1u, 2u, 2u, 3u, 0u,
                                          0u, 0u, 15u, 15u, 15u, 5u};
    static const uint8_t column_major[12] = {1u, 2u, 0u, 15u,
                                             1u, 3u, 0u, 15u,
                                             2u, 0u, 15u, 5u};
    uint32_t format;
    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 4u;
    config.glyphs[0].height = 3u;
    config.glyphs[0].bitmap_size = sizeof(rle_bytes);
    memcpy(config.glyphs[0].bitmap, rle_bytes, sizeof(rle_bytes));
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(buffer, 0xee, 32u);
    test_reset_dirty();
    test_set_image(2u, 0, 0, 4u, 3u, 4u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
    CHECK(memcmp(buffer, row_major, sizeof(row_major)) == 0);
    CHECK(dirty_count == 3u);
    CHECK(dirty_spans[0].address == TEST_BUFFER_ADDR && dirty_spans[0].size == 4u);
    CHECK(dirty_spans[1].address == TEST_BUFFER_ADDR + 4u && dirty_spans[1].size == 4u);
    CHECK(dirty_spans[2].address == TEST_BUFFER_ADDR + 8u && dirty_spans[2].size == 4u);
    pgf_close(pgf);

    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 9u;
    config.glyphs[0].height = 1u;
    config.glyphs[0].bitmap[0] = 0x37u; /* control 7 repeats sample 3 eight times */
    config.glyphs[0].bitmap[1] = 0x9fu; /* control 15 emits one literal 9 */
    config.glyphs[0].bitmap_size = 2u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(buffer, 0xee, 16u);
    test_set_image(2u, 0, 0, 9u, 1u, 9u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph_by_id(pgf, 0, TEST_IMAGE_ADDR));
    {
        unsigned i;
        for (i = 0; i < 8u; ++i) CHECK(buffer[i] == 3u);
        CHECK(buffer[8] == 9u);
    }
    pgf_close(pgf);

    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 4u;
    config.glyphs[0].height = 1u;
    config.glyphs[0].bitmap[0] = 0xa8u; /* one literal followed by a short tail */
    config.glyphs[0].bitmap_size = 1u;
    CHECK(test_build_font(&font, &config));
    font.size = font.glyph_bitmap_offsets[0] + 1u;
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(buffer, 0xee, 8u);
    test_set_image(2u, 0, 0, 4u, 1u, 4u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
    CHECK(buffer[0] == 10u && buffer[1] == 0u && buffer[2] == 0u && buffer[3] == 0u);
    pgf_close(pgf);

    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 10u;
    config.glyphs[0].height = 1u;
    config.glyphs[0].bitmap[0] = 0x1eu; /* control 14, literals 1 and 2 */
    config.glyphs[0].bitmap[1] = 0x72u; /* control 7 lacks its following sample */
    config.glyphs[0].bitmap_size = 2u;
    CHECK(test_build_font(&font, &config));
    font.size = font.glyph_bitmap_offsets[0] + 2u;
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(buffer, 0xee, 16u);
    test_set_image(2u, 0, 0, 10u, 1u, 10u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
    CHECK(buffer[0] == 1u && buffer[1] == 2u);
    {
        unsigned i;
        for (i = 2; i < 10u; ++i) CHECK(buffer[i] == 0u);
    }
    pgf_close(pgf);

    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 4u;
    config.glyphs[0].height = 3u;
    config.glyphs[0].row_order = 2u;
    config.glyphs[0].bitmap_size = sizeof(rle_bytes);
    memcpy(config.glyphs[0].bitmap, rle_bytes, sizeof(rle_bytes));
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(buffer, 0xee, 32u);
    test_reset_dirty();
    test_set_image(2u, 0, 0, 4u, 3u, 4u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph_by_id(pgf, 0, TEST_IMAGE_ADDR));
    CHECK(memcmp(buffer, column_major, sizeof(column_major)) == 0);
    pgf_close(pgf);

    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 1u;
    config.glyphs[0].height = 1u;
    config.glyphs[0].bitmap[0] = 0xa0u;
    for (format = 0; format <= 4u; ++format) {
        uint8_t *pixel = buffer;
        CHECK(test_build_font(&font, &config));
        pgf = test_open(&font);
        CHECK(pgf != NULL);
        memset(buffer, 0xb6, 16u);
        test_reset_dirty();
        test_set_image(format, 0, 0, 2u, 1u,
                       (uint16_t)(format <= 1u ? 1u : (format == 3u ? 6u :
                                                       format == 4u ? 8u : 2u)),
                       TEST_BUFFER_ADDR);
        CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
        if (format == 0u) CHECK(pixel[0] == 0xbau);
        if (format == 1u) CHECK(pixel[0] == 0xa6u);
        if (format == 2u) CHECK(pixel[0] == 10u);
        if (format == 3u) CHECK(pixel[0] == 10u && pixel[1] == 10u && pixel[2] == 10u);
        if (format == 4u) CHECK(pixel[0] == 10u && pixel[1] == 10u &&
                                pixel[2] == 10u && pixel[3] == 10u);
        CHECK(dirty_count == 1u);
        pgf_close(pgf);
    }

    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 4u;
    config.glyphs[0].height = 1u;
    config.glyphs[0].bitmap[0] = 0xa8u; /* literal control 8, then 10 */
    config.glyphs[0].bitmap[1] = 0x05u;
    config.glyphs[0].bitmap[2] = 0x00u;
    config.glyphs[0].bitmap_size = 3u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(buffer, 0xee, 16u);
    test_reset_dirty();
    test_set_image(2u, 32, 0, 5u, 1u, 5u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
    CHECK(buffer[0] == 5u && buffer[1] == 7u && buffer[2] == 2u &&
          buffer[3] == 0u && buffer[4] == 0u);
    CHECK(dirty_count == 1u && dirty_spans[0].size == 5u);
    memset(buffer, 0xee, 16u);
    test_reset_dirty();
    test_set_image(2u, -32, 0, 5u, 1u, 5u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
    CHECK(buffer[0] == 7u && buffer[1] == 2u && buffer[2] == 0u);
    CHECK(buffer[3] == 0u && buffer[4] == 0xeeu && dirty_count == 1u);
    pgf_close(pgf);

    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 4u;
    config.glyphs[0].height = 2u;
    config.glyphs[0].bitmap[0] = 0x50u;
    config.glyphs[0].bitmap_size = 1u;
    CHECK(test_build_font(&font, &config));
    font.size = font.glyph_bitmap_offsets[0] + 1u;
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(buffer, 0xee, 16u);
    test_reset_dirty();
    test_set_image(2u, 0, 0, 4u, 2u, 4u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
    CHECK(buffer[0] == 5u && buffer[1] == 0u && buffer[2] == 0u &&
          buffer[3] == 0u && buffer[4] == 0u);
    pgf_close(pgf);
}

static void test_draw_rejections_and_dirty_spans(void) {
    TestConfig config;
    TestFont font;
    PGF *pgf;
    uint8_t *buffer = test_guest(TEST_BUFFER_ADDR);
    uint8_t saved[32];
    uint32_t format;
    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 3u;
    config.glyphs[0].height = 2u;
    config.glyphs[0].bitmap_size = 2u;
    config.glyphs[0].bitmap[0] = 0x00u;
    config.glyphs[0].bitmap[1] = 0x00u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    for (format = 0u; format <= 4u; ++format) {
        memset(buffer, 0x5a, sizeof(saved));
        memcpy(saved, buffer, sizeof(saved));
        test_reset_dirty();
        test_set_image(5u, 0, 0, 8u, 8u, 8u, TEST_BUFFER_ADDR);
        CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
        CHECK(memcmp(buffer, saved, sizeof(saved)) == 0 && dirty_count == 0u);
        test_set_image(format, 0, 0, 0u, 8u, 8u, TEST_BUFFER_ADDR);
        CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
        test_set_image(format, 0, 0, 8u, 0u, 8u, TEST_BUFFER_ADDR);
        CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
        test_set_image(format, 0, 0, 8u, 8u, 0u, TEST_BUFFER_ADDR);
        CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
        test_set_image(format, 0, 0, 8u, 8u, 8u, 0u);
        CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
    }
    test_set_image(2u, 0, 0, 8u, 8u, 8u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(NULL, 65, 0, TEST_IMAGE_ADDR) == 0);
    CHECK(pgf_draw_glyph(pgf, 64, 65, TEST_IMAGE_ADDR) == 0);
    CHECK(pgf_draw_glyph_by_id(pgf, -1, TEST_IMAGE_ADDR) == 0);
    CHECK(pgf_draw_glyph_by_id(pgf, 1, TEST_IMAGE_ADDR) == 0);
    memset(buffer, 0x5a, sizeof(saved));
    memcpy(saved, buffer, sizeof(saved));
    test_reset_dirty();
    test_set_image(2u, 10 * 64, 0, 8u, 8u, 8u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
    CHECK(memcmp(buffer, saved, sizeof(saved)) == 0 && dirty_count == 0u);
    test_set_image(2u, -10 * 64, 0, 8u, 8u, 8u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
    CHECK(memcmp(buffer, saved, sizeof(saved)) == 0 && dirty_count == 0u);
    test_set_image(2u, 0, 0, 8u, 8u, 8u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, 0x0bfffff0u) == 0);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 1);
    test_reset_dirty();
    test_set_image(2u, 0, 64, 8u, 1u, 8u, TEST_BUFFER_ADDR);
    memset(buffer, 0x5a, sizeof(saved));
    memcpy(saved, buffer, sizeof(saved));
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 1);
    CHECK(memcmp(buffer, saved, sizeof(saved)) == 0 && dirty_count == 0u);
    test_set_image(2u, 0, 0, 1u, 1u, 8u, TEST_BUFFER_ADDR);
    test_reset_dirty();
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 1);
    CHECK(dirty_count == 1u && dirty_spans[0].size == 3u);

    /* Packed dirty ranges round out to whole bytes and preserve partner nibbles. */
    memset(buffer, 0x6bu, 8u);
    test_reset_dirty();
    test_set_image(0u, 32, 0, 8u, 1u, 4u, TEST_BUFFER_ADDR);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
    CHECK(dirty_count == 1u && dirty_spans[0].address == TEST_BUFFER_ADDR &&
          dirty_spans[0].size == 2u);
    pgf_close(pgf);

    /* A bad later row fails before changing an earlier valid row. */
    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 4u;
    config.glyphs[0].height = 2u;
    config.glyphs[0].bitmap_size = 3u;
    config.glyphs[0].bitmap[0] = 0x00u;
    config.glyphs[0].bitmap[1] = 0x00u;
    config.glyphs[0].bitmap[2] = 0x00u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(test_guest(0x0bfffff9u), 0xc7, 7u);
    test_reset_dirty();
    test_set_image(2u, 0, 0, 4u, 2u, 4u, 0x0bfffff9u);
    CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR) == 0);
    CHECK(test_guest(0x0bfffff9u)[0] == 0xc7u &&
          test_guest(0x0bfffff9u)[1] == 0xc7u &&
          test_guest(0x0bfffff9u)[2] == 0xc7u &&
          test_guest(0x0bfffff9u)[3] == 0xc7u && dirty_count == 0u);
    pgf_close(pgf);
}

static void test_fallback_and_guest_boundaries(void) {
    TestConfig config;
    TestFont font;
    PGF *pgf;
    uint8_t *info = test_guest(TEST_INFO_ADDR);
    test_default_config(&config);
    config.map_values[1] = 3u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 66, 65, TEST_INFO_ADDR));
    CHECK(test_u32(info) == 1u);
    CHECK(pgf_get_char_info(pgf, 64, 65, TEST_INFO_ADDR) == 0);
    CHECK(test_u32(info) == 0u);
    CHECK(pgf_get_char_info(pgf, 67, 65, TEST_INFO_ADDR));
    CHECK(test_u32(info) == 1u);
    CHECK(pgf_get_char_info(pgf, 65, 66, TEST_INFO_ADDR));
    CHECK(test_u32(info) == 1u);
    pgf_close(pgf);

    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    memset(test_guest(TEST_INFO_ADDR), 0xa5, TEST_CHAR_INFO_SIZE);
    CHECK(pgf_get_char_info(pgf, 65, 0, 0x0bfffff0u) == 0);
    {
        unsigned i;
        for (i = 0; i < TEST_CHAR_INFO_SIZE; ++i) {
            CHECK(test_guest(TEST_INFO_ADDR)[i] == 0xa5u);
        }
    }
    CHECK(pgf_get_char_info(NULL, 65, 0, TEST_INFO_ADDR) == 0);
    CHECK(memcmp(test_guest(TEST_INFO_ADDR), (uint8_t[TEST_CHAR_INFO_SIZE]){0},
                 TEST_CHAR_INFO_SIZE) == 0);
    memset(test_guest(TEST_INFO_ADDR), 0xa5, TEST_FONT_INFO_SIZE);
    pgf_get_font_info(pgf, 0x0bfffff0u);
    {
        unsigned i;
        for (i = 0; i < TEST_FONT_INFO_SIZE; ++i) {
            CHECK(test_guest(TEST_INFO_ADDR)[i] == 0xa5u);
        }
    }
    pgf_close(pgf);
}

static int test_write_file(const char *path, const uint8_t *bytes, size_t size) {
    FILE *stream = fopen(path, "wb");
    size_t written;
    if (!stream) return 0;
    written = fwrite(bytes, 1u, size, stream);
    if (fclose(stream) != 0) return 0;
    return written == size;
}

/* The supported revision and shadow-width variants: an empty shadow map that
   declares the documented 16-bit width, and a revision-3 font whose header has
   no 412-byte extension. Both decode on the ordinary direct-map path. */
static void test_revision_and_shadow_variants(void) {
    TestConfig config;
    TestFont base;
    TestFont variant;
    PGF *pgf;
    uint8_t base_info[TEST_CHAR_INFO_SIZE];
    uint8_t variant_info[TEST_CHAR_INFO_SIZE];
    unsigned revision;

    test_default_config(&config);
    config.revision = 2u;
    CHECK(test_build_font(&base, &config));
    pgf = test_open(&base);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    memcpy(base_info, test_guest(TEST_INFO_ADDR), sizeof(base_info));
    pgf_close(pgf);

    /* An empty shadow map accepts the documented entry width. */
    test_default_config(&config);
    config.shadow_bits = 16u;
    CHECK(test_build_font(&variant, &config));
    pgf = test_open(&variant);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    CHECK(memcmp(test_guest(TEST_INFO_ADDR), base_info, sizeof(base_info)) == 0);
    pgf_close(pgf);

    /* A 412-byte header still requires revision 3, and revision 4 is refused
       with either header size. */
    for (revision = 0; revision <= 2u; ++revision) {
        test_default_config(&config);
        config.revision = 3u;
        config.revision3_counts[0] = 2u;
        CHECK(test_build_font(&variant, &config));
        test_put_u32(variant.bytes + 8u, revision);
        CHECK(test_open(&variant) == NULL);
    }
    test_default_config(&config);
    config.revision = 3u;
    config.base_header = 1u;
    CHECK(test_build_font(&variant, &config));
    test_put_u32(variant.bytes + 8u, 4u);
    CHECK(test_open(&variant) == NULL);
    test_put_u32(variant.bytes + 8u, 3u);
    pgf = test_open(&variant);
    CHECK(pgf != NULL);
    pgf_close(pgf);

    /* Revision 3 with no extension produces the same guest records as the
       otherwise identical revision-2 font. */
    test_default_config(&config);
    config.revision = 3u;
    config.base_header = 1u;
    CHECK(test_build_font(&variant, &config));
    pgf = test_open(&variant);
    CHECK(pgf != NULL);
    CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
    memcpy(variant_info, test_guest(TEST_INFO_ADDR), sizeof(variant_info));
    CHECK(memcmp(variant_info, base_info, sizeof(base_info)) == 0);
    pgf_close(pgf);
}

/* One deterministic source-owned synthetic PGF stands in for the converter
   output the project does not ship yet (#313): identical config bytes on every
   run, no firmware font and no retail bytes. It is staged as a host file so
   the production pgf_open() file path -- not only pgf_open_memory() -- is
   exercised for metrics, drawing, and fail-closed rejection. */
static void test_production_file_path_metrics_and_render(void) {
    static const uint8_t rle_bytes[6] = {0x11u, 0x21u, 0x30u, 0x02u, 0xf2u, 0x50u};
    static const uint8_t expected_row_major[12] = {1u, 1u, 2u, 2u, 3u, 0u,
                                                  0u, 0u, 15u, 15u, 15u, 5u};
    static const char *const good_path = SR_SELFTEST_BUILD_ROOT "/synthetic-converter-output.pgf";
    static const char *const bad_magic_path = SR_SELFTEST_BUILD_ROOT "/synthetic-converter-output-badmagic.pgf";
    static const char *const short_path = SR_SELFTEST_BUILD_ROOT "/synthetic-converter-output-short.pgf";
    static const char *const composite_path = SR_SELFTEST_BUILD_ROOT "/synthetic-composite-output.pgf";
    static const char *const bad_composite_path =
        SR_SELFTEST_BUILD_ROOT "/synthetic-composite-output-absent.pgf";
    TestConfig config;
    TestFont font;
    TestFont repeat;
    TestFont bad_magic;
    PGF *pgf;
    uint8_t *buffer;

    test_default_config(&config);
    config.glyph_count = 1u;
    config.char_map_count = 1u;
    config.map_values[0] = 0u;
    config.glyphs[0].width = 4u;
    config.glyphs[0].height = 3u;
    config.glyphs[0].bitmap_size = sizeof(rle_bytes);
    memcpy(config.glyphs[0].bitmap, rle_bytes, sizeof(rle_bytes));
    CHECK(test_build_font(&font, &config));
    CHECK(test_build_font(&repeat, &config));
    CHECK(font.size == repeat.size);
    CHECK(memcmp(font.bytes, repeat.bytes, font.size) == 0);
    CHECK(test_write_file(good_path, font.bytes, font.size));

    pgf = pgf_open(good_path);
    CHECK(pgf != NULL);
    if (pgf) {
        CHECK(pgf_has_char(pgf, 65));
        CHECK(!pgf_has_char(pgf, 66));
        CHECK(pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR));
        CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x00u) == 4u);
        CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x04u) == 3u);
        CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x10u) == 64u);
        CHECK(test_u32(test_guest(TEST_INFO_ADDR) + 0x14u) == 128u);
        CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x18u) == 256);
        CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x1cu) == 128);
        CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x20u) == -64);
        CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x28u) == 64);
        CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x2cu) == 128);
        CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x30u) == 384);
        CHECK((int32_t)test_u32(test_guest(TEST_INFO_ADDR) + 0x34u) == 512);
        buffer = test_guest(TEST_BUFFER_ADDR);
        memset(buffer, 0xee, sizeof(expected_row_major));
        test_reset_dirty();
        test_set_image(2u, 0, 0, 4u, 3u, 4u, TEST_BUFFER_ADDR);
        CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
        CHECK(memcmp(buffer, expected_row_major, sizeof(expected_row_major)) == 0);
        CHECK(dirty_count == 3u);
        pgf_close(pgf);
    }

    /* Malformed input: a clobbered PGF0 signature and a payload shorter than
       one header must both fail closed through the same host file path. */
    bad_magic = font;
    bad_magic.bytes[4u] = 'X';
    CHECK(test_write_file(bad_magic_path, bad_magic.bytes, bad_magic.size));
    CHECK(pgf_open(bad_magic_path) == NULL);
    CHECK(test_write_file(short_path, font.bytes, 64u));
    CHECK(pgf_open(short_path) == NULL);
    pgf = pgf_open(good_path);
    CHECK(pgf != NULL);
    pgf_close(pgf);
    (void)remove(good_path);
    (void)remove(bad_magic_path);
    (void)remove(short_path);

    /* The composite form takes the same host file path, and a composite whose
       payload names an absent character fails closed through it. */
    test_composite_config(&config);
    CHECK(test_build_font(&font, &config));
    CHECK(test_write_file(composite_path, font.bytes, font.size));
    pgf = pgf_open(composite_path);
    CHECK(pgf != NULL);
    if (pgf) {
        buffer = test_guest(TEST_BUFFER_ADDR);
        memset(buffer, 0xee, 4u);
        test_reset_dirty();
        test_set_image(2u, 0, 0, 2u, 2u, 2u, TEST_BUFFER_ADDR);
        CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
        CHECK(memcmp(buffer, (uint8_t[4]){7u, 6u, 0u, 3u}, 4u) == 0);
        CHECK(dirty_count == 6u);
        pgf_close(pgf);
    }
    test_composite_config(&config);
    config.glyphs[0].composite[2] = 0u;
    CHECK(test_build_font(&font, &config));
    CHECK(test_write_file(bad_composite_path, font.bytes, font.size));
    CHECK(pgf_open(bad_composite_path) == NULL);
    (void)remove(composite_path);
    (void)remove(bad_composite_path);
}

/* Draw one code of an 8x8 format-2 target and copy the whole target out, so a dense
   and a sparse font can be compared pixel for pixel. */
static int test_draw_target(const TestFont *font, uint32_t code, uint8_t pixels[64]) {
    PGF *pgf = test_open(font);
    int drawn = 0;
    if (!pgf) return 0;
    memset(test_guest(TEST_BUFFER_ADDR), 0, 64u);
    test_set_image(2u, 0, 0, 8u, 8u, 8u, TEST_BUFFER_ADDR);
    drawn = pgf_draw_glyph(pgf, (int)code, 0, TEST_IMAGE_ADDR);
    memcpy(pixels, test_guest(TEST_BUFFER_ADDR), 64u);
    pgf_close(pgf);
    return drawn;
}

/* Sparse character maps (PGF_SPEC.md 3.1, 3.2, O-9, O-15): the character-pointer
   count is the glyph count. The character map may cover a wider code span whose
   absent codes hold the all-ones sentinel, a map value at or above the pointer count
   is a lookup miss, and every glyph record still validates at open. */
static void test_sparse_character_maps(void) {
    TestConfig config;
    TestFont font;
    TestFont dense;
    PGF *pgf;
    uint32_t code;
    uint8_t dense_pixels[64];
    uint8_t sparse_pixels[64];
    unsigned nonzero = 0u;
    size_t i;

    /* Codes 'A'..'Z' (26 map entries) over three glyph records. */
    test_default_config(&config);
    config.char_map_bits = 5u;
    config.code_span = 26u;
    config.char_map_count = 26u;
    for (code = 0; code < 26u; ++code) config.map_values[code] = 31u;
    config.map_values[0] = 0u;   /* 'A' -> glyph 0 */
    config.map_values[2] = 1u;   /* 'C' -> glyph 1 */
    config.map_values[25] = 2u;  /* 'Z' -> glyph 2 */
    config.glyphs[0].bitmap[0] = 0x50u; /* sample 5 */
    config.glyphs[1].bitmap[0] = 0x70u; /* sample 7 */
    config.glyphs[2].bitmap[0] = 0x30u; /* sample 3 */
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    if (pgf) {
        CHECK(pgf_has_char(pgf, 65));
        CHECK(!pgf_has_char(pgf, 66));
        CHECK(pgf_has_char(pgf, 67));
        CHECK(pgf_has_char(pgf, 90));
        CHECK(!pgf_has_char(pgf, 64));
        CHECK(!pgf_has_char(pgf, 91));
        test_set_image(2u, 0, 0, 1u, 1u, 1u, TEST_BUFFER_ADDR);
        CHECK(pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR));
        CHECK(test_guest(TEST_BUFFER_ADDR)[0] == 5u);
        CHECK(pgf_draw_glyph(pgf, 67, 0, TEST_IMAGE_ADDR));
        CHECK(test_guest(TEST_BUFFER_ADDR)[0] == 7u);
        CHECK(pgf_draw_glyph(pgf, 90, 0, TEST_IMAGE_ADDR));
        CHECK(test_guest(TEST_BUFFER_ADDR)[0] == 3u);
        CHECK(!pgf_draw_glyph(pgf, 66, 0, TEST_IMAGE_ADDR));
        pgf_close(pgf);
    }

    /* Map values 3 and 4 name glyph records past the pointer count of three, so those
       codes are lookup misses. Value 7 is the three-bit absence sentinel. */
    test_default_config(&config);
    config.char_map_bits = 3u;
    config.code_span = 5u;
    config.char_map_count = 5u;
    config.map_values[0] = 0u;  /* 'A' */
    config.map_values[1] = 2u;  /* 'B' */
    config.map_values[2] = 4u;  /* 'C' -> glyph 4, past the pointer count */
    config.map_values[3] = 3u;  /* 'D' -> glyph 3, past the pointer count */
    config.map_values[4] = 7u;  /* 'E' -> sentinel */
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    if (pgf) {
        CHECK(pgf_has_char(pgf, 65) && pgf_has_char(pgf, 66));
        CHECK(!pgf_has_char(pgf, 67) && !pgf_has_char(pgf, 68) && !pgf_has_char(pgf, 69));
        pgf_close(pgf);
    }

    /* A font with no character pointers has no glyph records and is refused. */
    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_put_u32(font.bytes + 0x14u, 0u);
    CHECK(test_open(&font) == NULL);

    /* Glyph 2 is named by no code, yet its record is validated at open. */
    test_default_config(&config);
    config.code_span = 26u;
    config.char_map_count = 26u;
    config.char_map_bits = 5u;
    for (code = 0; code < 26u; ++code) config.map_values[code] = 31u;
    config.map_values[0] = 0u;
    config.map_values[2] = 1u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    if (pgf) pgf_close(pgf);
    font.size = font.glyph_record_offsets[2] + 1u;
    CHECK(test_open(&font) == NULL);

    /* A composite whose components are named through a sparse map (codes 'B'..'D' of
       an eight-code span) draws the same pixels as the dense font it was derived
       from, and the sparse map's absent codes stay misses. */
    test_composite_config(&config);
    CHECK(test_build_font(&dense, &config));
    CHECK(test_draw_target(&dense, 65, dense_pixels));
    test_composite_config(&config);
    config.code_span = 8u;
    config.char_map_count = 8u;
    for (code = 4; code < 8u; ++code) config.map_values[code] = 7u;
    CHECK(test_build_font(&font, &config));
    pgf = test_open(&font);
    CHECK(pgf != NULL);
    if (pgf) {
        CHECK(!pgf_has_char(pgf, 69));
        pgf_close(pgf);
    }
    CHECK(test_draw_target(&font, 65, sparse_pixels));
    CHECK(memcmp(dense_pixels, sparse_pixels, sizeof(dense_pixels)) == 0);
    for (i = 0; i < sizeof(dense_pixels); ++i) {
        if (dense_pixels[i] != 0u) ++nonzero;
    }
    CHECK(nonzero != 0u);
}

static void test_deterministic_mutations(void) {
    TestConfig config;
    TestFont font;
    uint32_t state = 0x6d2b79f5u;
    uint32_t composite_state = 0x1b7a93c5u;
    unsigned accepted = 0u;
    unsigned rejected = 0u;
    unsigned composite_accepted = 0u;
    unsigned composite_rejected = 0u;
    unsigned iteration;

    test_default_config(&config);
    CHECK(test_build_font(&font, &config));
    test_set_image(2u, 0, 0, 16u, 16u, 16u, TEST_BUFFER_ADDR);
    for (iteration = 0; iteration < 10000u; ++iteration) {
        size_t offset;
        uint8_t mask;
        PGF *pgf;
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        offset = (size_t)(state % font.size);
        mask = (uint8_t)(1u << ((state >> 24) & 7u));
        font.bytes[offset] ^= mask;
        pgf = test_open(&font);
        if (pgf) {
            ++accepted;
            (void)pgf_has_char(pgf, 65);
            (void)pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR);
            test_reset_dirty();
            (void)pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR);
            pgf_close(pgf);
        } else {
            ++rejected;
        }
        font.bytes[offset] ^= mask;
    }
    CHECK(accepted != 0u && rejected != 0u);

    /* The same sweep over a composite font, whose components are followed one
       level deep and whose invalid payloads must fail closed. */
    test_composite_config(&config);
    CHECK(test_build_font(&font, &config));
    for (iteration = 0; iteration < 10000u; ++iteration) {
        size_t offset;
        uint8_t mask;
        PGF *pgf;
        composite_state ^= composite_state << 13;
        composite_state ^= composite_state >> 17;
        composite_state ^= composite_state << 5;
        offset = (size_t)(composite_state % font.size);
        mask = (uint8_t)(1u << ((composite_state >> 24) & 7u));
        font.bytes[offset] ^= mask;
        pgf = test_open(&font);
        if (pgf) {
            ++composite_accepted;
            (void)pgf_has_char(pgf, 65);
            (void)pgf_get_char_info(pgf, 65, 0, TEST_INFO_ADDR);
            test_reset_dirty();
            (void)pgf_draw_glyph(pgf, 65, 0, TEST_IMAGE_ADDR);
            pgf_close(pgf);
        } else {
            ++composite_rejected;
        }
        font.bytes[offset] ^= mask;
    }
    CHECK(composite_accepted != 0u && composite_rejected != 0u);
}

/* The validator verdict: dense and sparse images are accepted with their header facts, and
   each refusal names the rule the reader applies first. */
static void test_validate_memory_verdicts(void) {
    TestConfig config;
    TestFont dense;
    TestFont sparse;
    TestFont mutated;
    PgfVerdict verdict;
    uint8_t *huge;
    const size_t max_image = 16u * 1024u * 1024u;

    test_default_config(&config);
    CHECK(test_build_font(&dense, &config));
    memset(&verdict, 0, sizeof(verdict));
    CHECK(pgf_validate_memory(dense.bytes, dense.size, &verdict) == 1);
    CHECK(verdict.refusal == PGF_REFUSE_NONE);
    CHECK(verdict.revision == 2u);
    CHECK(verdict.header_size == 392u);
    CHECK(verdict.first_glyph == 65u && verdict.last_glyph == 67u);
    CHECK(verdict.glyph_count == 3u && verdict.char_map_count == 3u);
    CHECK(verdict.has_latin == 0u && verdict.has_kana == 0u && verdict.has_hangul == 0u);

    /* 'A'..'Z' over three glyph records: a sparse character map. */
    test_default_config(&config);
    config.char_map_bits = 5u;
    config.code_span = 26u;
    config.char_map_count = 26u;
    for (unsigned code = 0; code < 26u; ++code) config.map_values[code] = 31u;
    config.map_values[0] = 0u;
    config.map_values[2] = 1u;
    config.map_values[25] = 2u;
    config.glyphs[0].bitmap[0] = 0x50u;
    config.glyphs[1].bitmap[0] = 0x70u;
    config.glyphs[2].bitmap[0] = 0x30u;
    CHECK(test_build_font(&sparse, &config));
    memset(&verdict, 0, sizeof(verdict));
    CHECK(pgf_validate_memory(sparse.bytes, sparse.size, &verdict) == 1);
    CHECK(verdict.refusal == PGF_REFUSE_NONE);
    CHECK(verdict.glyph_count == 3u);
    CHECK(verdict.char_map_count == 26u);
    CHECK(verdict.char_map_count > verdict.glyph_count);

    /* Each refusal is named by its first rule. */
    memset(&verdict, 0, sizeof(verdict));
    CHECK(pgf_validate_memory(dense.bytes, 100u, &verdict) == 0);
    CHECK(verdict.refusal == PGF_REFUSE_TRUNCATED);

    memcpy(mutated.bytes, dense.bytes, dense.size);
    mutated.bytes[4] = 'B'; mutated.bytes[5] = 'A'; mutated.bytes[6] = 'D'; mutated.bytes[7] = '0';
    CHECK(pgf_validate_memory(mutated.bytes, dense.size, &verdict) == 0);
    CHECK(verdict.refusal == PGF_REFUSE_MAGIC);

    memcpy(mutated.bytes, dense.bytes, dense.size);
    test_put_u16(mutated.bytes + 0xb6u, 70u);
    CHECK(pgf_validate_memory(mutated.bytes, dense.size, &verdict) == 0);
    CHECK(verdict.refusal == PGF_REFUSE_GLYPH_RANGE);

    memcpy(mutated.bytes, dense.bytes, dense.size);
    test_put_u32(mutated.bytes + 0x08u, 4u);
    CHECK(pgf_validate_memory(mutated.bytes, dense.size, &verdict) == 0);
    CHECK(verdict.refusal == PGF_REFUSE_REVISION);

    memcpy(mutated.bytes, dense.bytes, dense.size);
    test_put_u16(mutated.bytes + 0x02u, 412u);
    CHECK(pgf_validate_memory(mutated.bytes, dense.size, &verdict) == 0);
    CHECK(verdict.refusal == PGF_REFUSE_HEADER_SIZE);

    memcpy(mutated.bytes, dense.bytes, dense.size);
    test_put_u32(mutated.bytes + 0x14u, 0u);
    CHECK(pgf_validate_memory(mutated.bytes, dense.size, &verdict) == 0);
    CHECK(verdict.refusal == PGF_REFUSE_NO_GLYPHS);

    memcpy(mutated.bytes, dense.bytes, dense.size);
    mutated.bytes[dense.char_pointer_offset] = 0xffu;
    mutated.bytes[dense.char_pointer_offset + 1u] = 0xffu;
    mutated.bytes[dense.char_pointer_offset + 2u] = 0xffu;
    CHECK(pgf_validate_memory(mutated.bytes, dense.size, &verdict) == 0);
    CHECK(verdict.refusal == PGF_REFUSE_GLYPH);

    huge = (uint8_t *)calloc(1u, max_image + 1u);
    CHECK(huge != NULL);
    if (huge) {
        memcpy(huge, dense.bytes, dense.size);
        memset(&verdict, 0, sizeof(verdict));
        CHECK(pgf_validate_memory(huge, max_image + 1u, &verdict) == 0);
        CHECK(verdict.refusal == PGF_REFUSE_TOO_LARGE);
        CHECK(pgf_validate_memory(huge, max_image, &verdict) == 1 || verdict.refusal != PGF_REFUSE_TOO_LARGE);
        free(huge);
    }
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--write-base") == 0) {
        TestConfig config;
        TestFont font;
        FILE *stream;
        int result;
        test_default_config(&config);
        if (!test_build_font(&font, &config)) return 2;
        stream = fopen(argv[2], "wb");
        if (!stream) return 2;
        result = fwrite(font.bytes, 1u, font.size, stream) == font.size ? 0 : 2;
        if (fclose(stream) != 0) result = 2;
        return result;
    }
    if (argc != 1) return 2;
    g_mem = (uint8_t *)calloc(1u, TEST_GUEST_SIZE);
    if (!g_mem) {
        fprintf(stderr, "unable to allocate synthetic guest memory\n");
        return 2;
    }
    test_open_headers_and_sections();
    test_maps_pointers_and_metrics();
    test_sparse_character_maps();
    test_shadow_and_character_info();
    test_composite_glyphs();
    test_revision_and_shadow_variants();
    test_font_information_and_lifetime();
    test_draw_rle_formats_and_placement();
    test_draw_rejections_and_dirty_spans();
    test_fallback_and_guest_boundaries();
    test_production_file_path_metrics_and_render();
    test_deterministic_mutations();
    test_validate_memory_verdicts();
    free(g_mem);
    if (failures != 0) {
        fprintf(stderr, "%d PGF public tests failed\n", failures);
        return 1;
    }
    puts("ALL PUBLIC PGF READER TESTS PASSED");
    return 0;
}
