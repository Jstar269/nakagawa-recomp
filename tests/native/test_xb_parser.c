/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include "nk_xb.h"
#include "archive_vfs.h"
#include "nk_iso.h"
#include "nk_platform.h"
#include "setup_staging.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#include <windows.h>
#define test_rmdir _rmdir
#else
#include <errno.h>
#include <unistd.h>
#define test_rmdir rmdir
#endif

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} ByteBuffer;

typedef struct {
    const char *path;
    const uint8_t *data;
    size_t size;
    NkXbCompression compression;
} FixtureEntry;

static void write_file_bytes(const char *path, const void *data, size_t size);

static void capture_environment_value(const char *name, char *value,
                                      size_t value_size, bool *was_set) {
    const char *current = getenv(name);
    *was_set = current != NULL;
    if (current) {
        assert(strlen(current) < value_size);
        memcpy(value, current, strlen(current) + 1);
    } else {
        value[0] = '\0';
    }
}

static void set_environment_value(const char *name, const char *value) {
#if defined(_WIN32) || defined(_WIN64)
    assert(_putenv_s(name, value) == 0);
#else
    assert(setenv(name, value, 1) == 0);
#endif
}

static void restore_environment_value(const char *name, const char *value,
                                      bool was_set) {
#if defined(_WIN32) || defined(_WIN64)
    assert(_putenv_s(name, was_set ? value : "") == 0);
#else
    if (was_set) assert(setenv(name, value, 1) == 0);
    else assert(unsetenv(name) == 0);
#endif
}

static void buffer_reserve(ByteBuffer *buffer, size_t extra) {
    assert(extra <= SIZE_MAX - buffer->size);
    size_t required = buffer->size + extra;
    if (required <= buffer->capacity) return;
    size_t capacity = buffer->capacity ? buffer->capacity : 64;
    while (capacity < required) {
        assert(capacity <= SIZE_MAX / 2);
        capacity *= 2;
    }
    uint8_t *grown = (uint8_t *)realloc(buffer->data, capacity);
    assert(grown != NULL);
    buffer->data = grown;
    buffer->capacity = capacity;
}

static void buffer_append(ByteBuffer *buffer, const void *data, size_t size) {
    buffer_reserve(buffer, size);
    if (size > 0) memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
}

static void buffer_u16(ByteBuffer *buffer, uint16_t value, bool big_endian) {
    uint8_t bytes[2] = {
        (uint8_t)(big_endian ? value >> 8 : value),
        (uint8_t)(big_endian ? value : value >> 8)
    };
    buffer_append(buffer, bytes, sizeof(bytes));
}

static void buffer_u32(ByteBuffer *buffer, uint32_t value, bool big_endian) {
    uint8_t bytes[4] = {
        (uint8_t)(big_endian ? value >> 24 : value),
        (uint8_t)(big_endian ? value >> 16 : value >> 8),
        (uint8_t)(big_endian ? value >> 8 : value >> 16),
        (uint8_t)(big_endian ? value : value >> 24)
    };
    buffer_append(buffer, bytes, sizeof(bytes));
}

static uint32_t read_le32_test(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint8_t sjis_hash_test(const uint8_t *data, size_t size) {
    uint8_t value = 0;
    for (size_t i = 0; i < size; i++) {
        value = (uint8_t)((((value & 0x7fu) << 1) | ((value & 0x80u) >> 7)) ^ data[i]);
    }
    return value;
}

static void pad4(ByteBuffer *buffer) {
    static const uint8_t zeros[4] = { 0, 0, 0, 0 };
    size_t padding = (4u - (buffer->size & 3u)) & 3u;
    buffer_append(buffer, zeros, padding);
}

static void append_lzs_body(ByteBuffer *out, const uint8_t *data, size_t size) {
    for (size_t offset = 0; offset < size;) {
        size_t chunk = size - offset;
        if (chunk > 64) chunk = 64;
        uint8_t control = (uint8_t)((chunk - 1) << 2);
        buffer_append(out, &control, 1);
        buffer_append(out, data + offset, chunk);
        offset += chunk;
    }
}

static uint16_t reverse_bits_test(uint16_t value, unsigned width) {
    uint16_t reversed = 0;
    for (unsigned i = 0; i < width; i++) {
        reversed = (uint16_t)((reversed << 1) | (value & 1u));
        value >>= 1;
    }
    return reversed;
}

static void append_huffman_body(ByteBuffer *out, const uint8_t *data, size_t size,
                                bool big_endian) {
    static const uint8_t zero_counts[7] = { 0, 0, 0, 0, 0, 0, 0 };
    uint8_t max_length = 9;
    buffer_append(out, &max_length, 1);
    buffer_append(out, zero_counts, sizeof(zero_counts));
    uint8_t count_8 = 254;
    buffer_append(out, &count_8, 1);
    for (unsigned symbol = 0; symbol < 254; symbol++) {
        uint8_t value = (uint8_t)symbol;
        buffer_append(out, &value, 1);
    }
    uint8_t count_9 = 4;
    const uint8_t tail_symbols[4] = { 254, 255, 254, 255 };
    buffer_append(out, &count_9, 1);
    buffer_append(out, tail_symbols, sizeof(tail_symbols));
    assert((out->size & 1u) == 0);

    uint64_t bits = 0;
    unsigned bit_count = 0;
    for (size_t i = 0; i < size; i++) {
        uint16_t code = data[i] < 254 ? data[i] : (uint16_t)(508 + (data[i] - 254));
        unsigned width = data[i] < 254 ? 8u : 9u;
        uint16_t reversed = reverse_bits_test(code, width);
        bits |= (uint64_t)reversed << bit_count;
        bit_count += width;
        while (bit_count >= 16) {
            buffer_u16(out, (uint16_t)(bits & 0xffffu), big_endian);
            bits >>= 16;
            bit_count -= 16;
        }
    }
    if (bit_count > 0) buffer_u16(out, (uint16_t)(bits & 0xffffu), big_endian);
}

static ByteBuffer encoded_payload(const FixtureEntry *entry, bool big_endian) {
    ByteBuffer body = { 0 };
    ByteBuffer payload = { 0 };
    if (entry->compression == NK_XB_COMPRESSION_NONE) {
        buffer_append(&payload, entry->data, entry->size);
        pad4(&payload);
        return payload;
    }

    if (entry->compression == NK_XB_COMPRESSION_LZS) {
        append_lzs_body(&body, entry->data, entry->size);
    } else if (entry->compression == NK_XB_COMPRESSION_HUFFMAN) {
        append_huffman_body(&body, entry->data, entry->size, big_endian);
    } else {
        ByteBuffer lzs = { 0 };
        append_lzs_body(&lzs, entry->data, entry->size);
        ByteBuffer inner = { 0 };
        buffer_u32(&inner, (uint32_t)entry->size, big_endian);
        buffer_u32(&inner, (uint32_t)lzs.size, big_endian);
        buffer_append(&inner, lzs.data, lzs.size);
        append_huffman_body(&body, inner.data, inner.size, big_endian);
        free(lzs.data);
        buffer_u32(&payload, (uint32_t)inner.size, big_endian);
        buffer_u32(&payload, (uint32_t)body.size, big_endian);
        buffer_append(&payload, body.data, body.size);
        pad4(&payload);
        free(inner.data);
        free(body.data);
        return payload;
    }

    buffer_u32(&payload, (uint32_t)entry->size, big_endian);
    buffer_u32(&payload, (uint32_t)body.size, big_endian);
    buffer_append(&payload, body.data, body.size);
    pad4(&payload);
    free(body.data);
    return payload;
}

static ByteBuffer make_archive(const FixtureEntry *entries, size_t entry_count,
                               bool big_endian) {
    ByteBuffer names = { 0 };
    for (size_t i = 0; i < entry_count; i++) {
        size_t path_size = strlen(entries[i].path);
        assert(path_size > 0 && path_size <= 255);
        uint8_t length = (uint8_t)path_size;
        uint8_t hash = sjis_hash_test((const uint8_t *)entries[i].path, path_size);
        buffer_append(&names, &length, 1);
        buffer_append(&names, &hash, 1);
        buffer_append(&names, entries[i].path, path_size);
        uint8_t zero = 0;
        buffer_append(&names, &zero, 1);
    }

    ByteBuffer string_section = { 0 };
    buffer_u32(&string_section, (uint32_t)names.size, big_endian);
    buffer_u32(&string_section, 0, big_endian);
    buffer_append(&string_section, names.data, names.size);
    pad4(&string_section);

    ByteBuffer *payloads = (ByteBuffer *)calloc(entry_count, sizeof(*payloads));
    assert(payloads != NULL);
    size_t data_start = 8 + entry_count * 8 + string_section.size;
    assert((data_start & 3u) == 0);
    size_t total_size = data_start;
    for (size_t i = 0; i < entry_count; i++) {
        payloads[i] = encoded_payload(&entries[i], big_endian);
        total_size += payloads[i].size;
    }

    ByteBuffer archive = { 0 };
    const uint8_t signature[4] = { 0x78, 0x65, 0x00, 0x01 };
    buffer_append(&archive, signature, sizeof(signature));
    buffer_u32(&archive, (uint32_t)entry_count, big_endian);
    size_t offset = data_start;
    for (size_t i = 0; i < entry_count; i++) {
        buffer_u32(&archive, (uint32_t)entries[i].size, big_endian);
        uint32_t packed = ((uint32_t)entries[i].compression << 28) |
                          (uint32_t)(offset / 4);
        buffer_u32(&archive, packed, big_endian);
        offset += payloads[i].size;
    }
    buffer_append(&archive, string_section.data, string_section.size);
    for (size_t i = 0; i < entry_count; i++) {
        buffer_append(&archive, payloads[i].data, payloads[i].size);
        free(payloads[i].data);
    }
    assert(archive.size == total_size);
    free(payloads);
    free(names.data);
    free(string_section.data);
    return archive;
}

static NkResult open_archive(const ByteBuffer *bytes, NkXbArchive *archive,
                             char *error, size_t error_size) {
    return nk_xb_open_memory(bytes->data, bytes->size, "fixture.xb", false,
                             NULL, archive, error, error_size);
}

static void test_roundtrip_all_compression_modes(void) {
    static const uint8_t raw_data[] = "raw bytes";
    static const uint8_t lzs_data[] = "lzs bytes with a literal run";
    static const uint8_t huffman_data[] = "huffman bytes with a complete table";
    static const uint8_t deflate_data[] = "nested huffman and lzs bytes";
    const FixtureEntry entries[] = {
        { "data/raw.bin", raw_data, sizeof(raw_data) - 1, NK_XB_COMPRESSION_NONE },
        { "data/lzs.bin", lzs_data, sizeof(lzs_data) - 1, NK_XB_COMPRESSION_LZS },
        { "data/huffman.bin", huffman_data, sizeof(huffman_data) - 1, NK_XB_COMPRESSION_HUFFMAN },
        { "data/deflate.bin", deflate_data, sizeof(deflate_data) - 1, NK_XB_COMPRESSION_DEFLATE }
    };
    ByteBuffer bytes = make_archive(entries, 4, false);
    NkXbArchive archive;
    char error[256];
    assert(open_archive(&bytes, &archive, error, sizeof(error)) == NK_OK);
    assert(archive.entry_count == 4);
    for (size_t i = 0; i < archive.entry_count; i++) {
        uint8_t output[128];
        size_t output_size = 0;
        assert(nk_xb_read_entry(&archive, i, output, sizeof(output), &output_size,
                                error, sizeof(error)) == NK_OK);
        assert(output_size == entries[i].size);
        assert(memcmp(output, entries[i].data, entries[i].size) == 0);
        assert(strcmp(archive.entries[i].path, entries[i].path) == 0);
    }
    nk_xb_close(&archive);
    free(bytes.data);
    printf("[XB_TEST] RAW/LZS/HUFFMAN/DEFLATE in-memory roundtrip PASSED\n");
}

static void test_file_backed_archive_is_lazy(void) {
    static const uint8_t raw_data[] = "lazy raw bytes";
    static const uint8_t lzs_data[] = "lazy lzs bytes";
    static const uint8_t huffman_data[] = "lazy huffman bytes";
    const FixtureEntry entries[] = {
        { "data/raw.bin", raw_data, sizeof(raw_data) - 1u, NK_XB_COMPRESSION_NONE },
        { "data/lzs.bin", lzs_data, sizeof(lzs_data) - 1u, NK_XB_COMPRESSION_LZS },
        { "data/huffman.bin", huffman_data, sizeof(huffman_data) - 1u,
          NK_XB_COMPRESSION_HUFFMAN }
    };
    ByteBuffer bytes = make_archive(entries, sizeof(entries) / sizeof(entries[0]), false);
    const char *path = "build/test_xb_lazy.xb";
    write_file_bytes(path, bytes.data, bytes.size);
    NkXbArchive strict;
    char error[256];
    assert(nk_xb_open_file(path, false, NULL, &strict, error, sizeof(error)) == NK_OK);
    assert(strict.data != NULL);
    nk_xb_close(&strict);
    NkXbArchive archive;
    assert(nk_xb_open_file_lazy(path, false, NULL, &archive, error, sizeof(error)) == NK_OK);
    assert(archive.data == NULL);
    assert(archive.data_size == 0u);
    for (size_t i = 0; i < archive.entry_count; i++) {
        uint8_t output[128];
        size_t output_size = 0;
        assert(nk_xb_read_entry(&archive, i, output, sizeof(output), &output_size,
                                error, sizeof(error)) == NK_OK);
        assert(output_size == entries[i].size);
        assert(memcmp(output, entries[i].data, entries[i].size) == 0);
    }
    SrArchiveVfs vfs;
    SrArchiveFile file;
    sr_archive_vfs_init(&vfs);
    assert(sr_archive_vfs_mount_file(&vfs, path, false, -1, NULL) == NK_OK);
    assert(sr_archive_vfs_lookup(&vfs, "data/raw.bin", -2, &file));
    uint8_t vfs_output[128];
    size_t vfs_output_size = 0;
    assert(sr_archive_vfs_read(&vfs, &file, 2u, vfs_output, 5u,
                               &vfs_output_size) == NK_OK);
    assert(vfs_output_size == 5u && memcmp(vfs_output, raw_data + 2u, 5u) == 0);
    assert(sr_archive_vfs_lookup(&vfs, "data/lzs.bin", -2, &file));
    assert(sr_archive_vfs_read(&vfs, &file, 0u, vfs_output, sizeof(vfs_output),
                               &vfs_output_size) == NK_OK);
    assert(vfs_output_size == sizeof(lzs_data) - 1u &&
           memcmp(vfs_output, lzs_data, vfs_output_size) == 0);
    sr_archive_vfs_destroy(&vfs);
    remove(path);
    uint8_t output[128];
    assert(nk_xb_read_entry(&archive, 0u, output, sizeof(output), NULL,
                            error, sizeof(error)) != NK_OK);
    nk_xb_close(&archive);
    free(bytes.data);
    printf("[XB_TEST] file-backed metadata-only mount and lazy member reads PASSED\n");
}

static void test_file_backed_duplicate_validation(void) {
    static const uint8_t first[] = "same";
    static const uint8_t second[] = "same";
    static const uint8_t different[] = "different";
    const FixtureEntry identical[] = {
        { "data/shared.bin", first, sizeof(first) - 1u, NK_XB_COMPRESSION_NONE },
        { "data/shared.bin", second, sizeof(second) - 1u, NK_XB_COMPRESSION_NONE }
    };
    ByteBuffer bytes = make_archive(identical, 2u, false);
    const char *path = "build/test_xb_lazy_duplicate.xb";
    write_file_bytes(path, bytes.data, bytes.size);
    NkXbArchive archive;
    char error[256];
    assert(nk_xb_open_file_lazy(path, false, NULL, &archive, error, sizeof(error)) == NK_OK);
    assert(archive.data == NULL && archive.entry_count == 2u);
    nk_xb_close(&archive);
    free(bytes.data);

    FixtureEntry conflict[2];
    memcpy(conflict, identical, sizeof(conflict));
    conflict[1].data = different;
    conflict[1].size = sizeof(different) - 1u;
    bytes = make_archive(conflict, 2u, false);
    write_file_bytes(path, bytes.data, bytes.size);
    assert(nk_xb_open_file_lazy(path, false, NULL, &archive, error, sizeof(error)) != NK_OK);
    remove(path);
    free(bytes.data);
    printf("[XB_TEST] file-backed duplicate-path validation PASSED\n");
}

static void test_lazy_payload_failure_is_deferred(void) {
    static const uint8_t data[] = "lazy payload";
    const FixtureEntry entry = {
        "data/payload.bin", data, sizeof(data) - 1u, NK_XB_COMPRESSION_LZS
    };
    ByteBuffer bytes = make_archive(&entry, 1u, false);
    size_t header_offset = 0u;
    for (size_t i = bytes.size - 8u; i > 8u; i--) {
        if (read_le32_test(bytes.data + i) == entry.size) {
            header_offset = i;
            break;
        }
    }
    assert(header_offset != 0u);
    bytes.data[header_offset] = 0u;
    const char *path = "build/test_xb_lazy_bad_payload.xb";
    write_file_bytes(path, bytes.data, bytes.size);
    NkXbArchive archive;
    char error[256];
    assert(nk_xb_open_file_lazy(path, false, NULL, &archive, error, sizeof(error)) == NK_OK);
    uint8_t output[64];
    assert(nk_xb_read_entry(&archive, 0u, output, sizeof(output), NULL,
                            error, sizeof(error)) != NK_OK);
    nk_xb_close(&archive);
    assert(nk_xb_open_file(path, false, NULL, &archive, error, sizeof(error)) != NK_OK);
    remove(path);
    free(bytes.data);
    printf("[XB_TEST] deferred payload validation fails closed on first read\n");
}

static void test_huffman_final_buffered_bits(void) {
    uint8_t data[32];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i + 32);
    for (unsigned endian = 0; endian < 2; endian++) {
        for (unsigned nested = 0; nested < 2; nested++) {
            for (size_t size = 1; size <= sizeof(data); size++) {
                FixtureEntry entry = { "data/tail.bin", data, size,
                    nested ? NK_XB_COMPRESSION_DEFLATE : NK_XB_COMPRESSION_HUFFMAN };
                ByteBuffer bytes = make_archive(&entry, 1, endian != 0);
                NkXbArchive archive;
                char error[256];
                assert(nk_xb_open_memory(bytes.data, bytes.size, "tail.xb", endian != 0,
                                         NULL, &archive, error, sizeof(error)) == NK_OK);
                uint8_t output[32];
                size_t output_size = 0;
                NkResult result = nk_xb_read_entry(&archive, 0, output, sizeof(output),
                                                   &output_size, error, sizeof(error));
                if (result != NK_OK) {
                    fprintf(stderr, "tail endian=%u nested=%u size=%zu: %s\n",
                            endian, nested, size, error);
                }
                assert(result == NK_OK);
                assert(output_size == size);
                assert(memcmp(output, data, size) == 0);
                nk_xb_close(&archive);
                free(bytes.data);
            }
        }
    }
    printf("[XB_TEST] Huffman buffered tails and nested LZS, both endians PASSED\n");
}

static void test_big_endian_fields(void) {
    static const uint8_t data[] = "big endian";
    const FixtureEntry entry = { "data/be.bin", data, sizeof(data) - 1, NK_XB_COMPRESSION_LZS };
    ByteBuffer bytes = make_archive(&entry, 1, true);
    NkXbArchive archive;
    char error[256];
    assert(nk_xb_open_memory(bytes.data, bytes.size, "fixture.xb", true, NULL,
                             &archive, error, sizeof(error)) == NK_OK);
    uint8_t output[32];
    size_t output_size = 0;
    assert(nk_xb_read_entry(&archive, 0, output, sizeof(output), &output_size,
                            error, sizeof(error)) == NK_OK);
    assert(output_size == sizeof(data) - 1);
    assert(memcmp(output, data, output_size) == 0);
    nk_xb_close(&archive);
    free(bytes.data);
    printf("[XB_TEST] Big-endian integer fields PASSED\n");
}

static void test_truncated_header(void) {
    const uint8_t truncated[] = { 0x78, 0x65, 0x00, 0x01, 0x01, 0x00, 0x00 };
    NkXbArchive archive;
    char error[256];
    assert(nk_xb_open_memory(truncated, sizeof(truncated), "truncated.xb", false,
                             NULL, &archive, error, sizeof(error)) != NK_OK);
    printf("[XB_TEST] Truncated archive header rejected: %s\n", error);
}

static void test_path_traversal(void) {
    static const uint8_t data[] = "escape";
    const FixtureEntry entry = { "../../Windows/System32", data, sizeof(data) - 1,
                                 NK_XB_COMPRESSION_NONE };
    ByteBuffer bytes = make_archive(&entry, 1, false);
    NkXbArchive archive;
    char error[256];
    assert(open_archive(&bytes, &archive, error, sizeof(error)) != NK_OK);
    free(bytes.data);
    printf("[XB_TEST] Path traversal rejected: %s\n", error);
}

static void test_corrupt_huffman_prefix(void) {
    static const uint8_t data[] = "huffman";
    const FixtureEntry entry = { "data/corrupt.bin", data, sizeof(data) - 1,
                                 NK_XB_COMPRESSION_HUFFMAN };
    ByteBuffer bytes = make_archive(&entry, 1, false);
    uint32_t packed = read_le32_test(bytes.data + 12);
    size_t payload_offset = (size_t)(packed & 0x0fffffffu) * 4u;
    assert(payload_offset + 9 < bytes.size);
    /* The first code-length count becomes oversubscribed at length one. */
    bytes.data[payload_offset + 8 + 1] = 255;
    NkXbArchive archive;
    char error[256];
    assert(open_archive(&bytes, &archive, error, sizeof(error)) == NK_OK);
    uint8_t output[32];
    size_t output_size = 0;
    assert(nk_xb_read_entry(&archive, 0, output, sizeof(output), &output_size,
                            error, sizeof(error)) != NK_OK);
    nk_xb_close(&archive);
    free(bytes.data);
    printf("[XB_TEST] Corrupt Huffman prefix rejected: %s\n", error);
}

static void test_truncated_huffman_body(void) {
    static const uint8_t data[] = "huffman";
    const FixtureEntry entry = { "data/truncated.bin", data, sizeof(data) - 1,
                                 NK_XB_COMPRESSION_HUFFMAN };
    ByteBuffer bytes = make_archive(&entry, 1, false);
    uint32_t packed = read_le32_test(bytes.data + 12);
    size_t payload_offset = (size_t)(packed & 0x0fffffffu) * 4u;
    assert(payload_offset < bytes.size);
    /* Keep the structural header and table, but remove the final encoded word.
     * The decoder must reject the missing word instead of fabricating zeros. */
    assert(bytes.size >= payload_offset + 2 + 2);
    bytes.size -= 2;

    NkXbArchive archive;
    char error[256];
    assert(open_archive(&bytes, &archive, error, sizeof(error)) == NK_OK);
    uint8_t output[32];
    size_t output_size = 0;
    assert(nk_xb_read_entry(&archive, 0, output, sizeof(output), &output_size,
                            error, sizeof(error)) != NK_OK);
    assert(strstr(error, "truncated Huffman bitstream") != NULL);
    nk_xb_close(&archive);
    free(bytes.data);
    printf("[XB_TEST] Truncated Huffman bitstream rejected: %s\n", error);
}

static void test_lzs_lookback_floor(void) {
    static const uint8_t data[] = "bad";
    const FixtureEntry entry = { "data/bad-lzs.bin", data, sizeof(data) - 1,
                                 NK_XB_COMPRESSION_LZS };
    ByteBuffer bytes = make_archive(&entry, 1, false);
    uint32_t packed = read_le32_test(bytes.data + 12);
    size_t payload_offset = (size_t)(packed & 0x0fffffffu) * 4u;
    /* Replace the legal literal body with a short run at offset 16 while no
     * bytes have been produced. The archive remains structurally parseable;
     * failure must happen in the bounded decoder. */
    assert(payload_offset + 10 < bytes.size);
    bytes.data[payload_offset + 8] = 1;
    bytes.data[payload_offset + 9] = 1;
    bytes.data[payload_offset + 10] = 0;
    NkXbArchive archive;
    char error[256];
    assert(open_archive(&bytes, &archive, error, sizeof(error)) == NK_OK);
    uint8_t output[32];
    size_t output_size = 0;
    assert(nk_xb_read_entry(&archive, 0, output, sizeof(output), &output_size,
                            error, sizeof(error)) != NK_OK);
    nk_xb_close(&archive);
    free(bytes.data);
    printf("[XB_TEST] LZS lookback below output floor rejected: %s\n", error);
}

static void test_expansion_limit(void) {
    static const uint8_t data[] = "oversized";
    const FixtureEntry entry = { "data/oversized.bin", data, sizeof(data) - 1,
                                 NK_XB_COMPRESSION_NONE };
    ByteBuffer bytes = make_archive(&entry, 1, false);
    NkXbLimits limits = nk_xb_default_limits();
    limits.max_entry_bytes = 4;
    NkXbArchive archive;
    char error[256];
    assert(nk_xb_open_memory(bytes.data, bytes.size, "bomb.xb", false, &limits,
                             &archive, error, sizeof(error)) != NK_OK);
    free(bytes.data);
    printf("[XB_TEST] Decompression-bomb entry limit rejected: %s\n", error);
}

static void test_compressed_header_expansion_limit(void) {
    static const uint8_t data[] = "abc";
    const FixtureEntry entry = { "data/compressed-bomb.bin", data, sizeof(data) - 1,
                                 NK_XB_COMPRESSION_LZS };
    ByteBuffer bytes = make_archive(&entry, 1, false);
    uint32_t packed = read_le32_test(bytes.data + 12);
    size_t payload_offset = (size_t)(packed & 0x0fffffffu) * 4u;
    assert(payload_offset + 8 < bytes.size);
    bytes.data[payload_offset] = 5;
    bytes.data[payload_offset + 1] = 0;
    bytes.data[payload_offset + 2] = 0;
    bytes.data[payload_offset + 3] = 0;
    NkXbLimits limits = nk_xb_default_limits();
    limits.max_entry_bytes = 4;
    NkXbArchive archive;
    char error[256];
    assert(nk_xb_open_memory(bytes.data, bytes.size, "compressed-bomb.xb", false,
                             &limits, &archive, error, sizeof(error)) != NK_OK);
    free(bytes.data);
    printf("[XB_TEST] Compressed-header expansion limit rejected: %s\n", error);
}

static void test_duplicate_canonical_path(void) {
    static const uint8_t first[] = "one";
    static const uint8_t second[] = "two";
    const FixtureEntry entries[] = {
        { "data/item.bin", first, sizeof(first) - 1, NK_XB_COMPRESSION_NONE },
        { "data\\item.bin", second, sizeof(second) - 1, NK_XB_COMPRESSION_NONE }
    };
    ByteBuffer bytes = make_archive(entries, 2, false);
    NkXbArchive archive;
    char error[256];
    assert(open_archive(&bytes, &archive, error, sizeof(error)) != NK_OK);
    free(bytes.data);
    printf("[XB_TEST] Duplicate canonical path rejected: %s\n", error);
}

static uint32_t fuzz_next(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void test_deterministic_mutation_fuzz(void) {
    static const uint8_t data[] = "mutation corpus";
    const FixtureEntry entry = { "data/fuzz.bin", data, sizeof(data) - 1,
                                 NK_XB_COMPRESSION_DEFLATE };
    ByteBuffer seed = make_archive(&entry, 1, false);
    uint8_t *mutated = (uint8_t *)malloc(seed.size);
    assert(mutated != NULL);
    uint32_t state = 0x9e3779b9u;
    char error[256];
    for (unsigned iteration = 0; iteration < 2048; iteration++) {
        memcpy(mutated, seed.data, seed.size);
        size_t mutations = (size_t)(fuzz_next(&state) % 4u) + 1u;
        for (size_t mutation = 0; mutation < mutations; mutation++) {
            size_t index = (size_t)(fuzz_next(&state) % seed.size);
            mutated[index] ^= (uint8_t)fuzz_next(&state);
        }
        NkXbArchive archive;
        if (nk_xb_open_memory(mutated, seed.size, "mutated.xb", false, NULL,
                              &archive, error, sizeof(error)) == NK_OK) {
            for (size_t i = 0; i < archive.entry_count; i++) {
                uint8_t output[128];
                size_t output_size = 0;
                (void)nk_xb_read_entry(&archive, i, output, sizeof(output),
                                       &output_size, error, sizeof(error));
            }
            nk_xb_close(&archive);
        }
    }
    free(mutated);
    free(seed.data);
    printf("[XB_TEST] Deterministic 2048-mutation hostile corpus completed PASSED\n");
}

static void test_staging_cleanup_boundary(void) {
    const char *root = "build/.staging_xb_cleanup";
    const char *nested = "build/.staging_xb_cleanup/nested";
    const char *file_path = "build/.staging_xb_cleanup/nested/member.bin";
    assert(!player_stage_discard("build"));
    (void)player_stage_discard(root);
    assert(nk_platform_mkdir_p(nested));
    static const uint8_t data[] = "cleanup";
    write_file_bytes(file_path, data, sizeof(data) - 1);
    assert(nk_platform_dir_exists(root));
    assert(player_stage_discard(root));
    assert(!nk_platform_dir_exists(root));
    printf("[XB_TEST] Staging cleanup boundary and safe basename check PASSED\n");
}

static void test_staging_discard_reparse_boundary(void) {
#if defined(_WIN32) || defined(_WIN64)
    const char *root = "build/.staging_reparse_boundary";
    const char *outside = "build/staging_discard_outside";
    const char *outside_file = "build/staging_discard_outside/sentinel.bin";
    (void)player_stage_discard(root);
    remove(outside_file);
    test_rmdir(outside);
    assert(nk_platform_mkdir_p(root));
    assert(nk_platform_mkdir_p(outside));
    static const uint8_t data[] = "outside";
    write_file_bytes(outside_file, data, sizeof(data) - 1);

    /* The relative target resolves outside the staging root. On hosts without
     * symlink creation rights, retain the rest of the native suite but make
     * the missing adversarial capability explicit. */
    if (!CreateSymbolicLinkW(L"build/.staging_reparse_boundary/escaped",
                             L"..\\staging_discard_outside",
                             SYMBOLIC_LINK_FLAG_DIRECTORY |
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        DWORD error = GetLastError();
        remove(outside_file);
        test_rmdir(outside);
        (void)player_stage_discard(root);
        if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD) {
            printf("[XB_TEST] Windows reparse-point discard regression SKIP (symlink creation unavailable)\n");
            return;
        }
        assert(!"CreateSymbolicLinkW failed unexpectedly");
    }

    assert(player_stage_discard(root));
    assert(GetFileAttributesW(L"build/staging_discard_outside/sentinel.bin") !=
           INVALID_FILE_ATTRIBUTES);
    assert(GetFileAttributesW(L"build/.staging_reparse_boundary") ==
           INVALID_FILE_ATTRIBUTES);
    remove(outside_file);
    test_rmdir(outside);
    printf("[XB_TEST] Windows reparse-point discard boundary PASSED\n");
#else
    const char *root = "build/.staging_symlink_boundary";
    const char *outside = "build/staging_discard_posix_outside";
    const char *outside_file = "build/staging_discard_posix_outside/sentinel.bin";
    const char *escaped = "build/.staging_symlink_boundary/escaped";
    (void)player_stage_discard(root);
    remove(outside_file);
    test_rmdir(outside);
    assert(nk_platform_mkdir_p(root));
    assert(nk_platform_mkdir_p(outside));
    static const uint8_t data[] = "outside";
    write_file_bytes(outside_file, data, sizeof(data) - 1);

    if (symlink("../staging_discard_posix_outside", escaped) != 0) {
        int error = errno;
        (void)player_stage_discard(root);
        remove(outside_file);
        test_rmdir(outside);
        if (error == EACCES || error == EPERM || error == ENOSYS) {
            printf("[XB_TEST] POSIX symlink discard regression SKIP (symlink creation unavailable)\n");
            return;
        }
        assert(!"symlink failed unexpectedly");
    }

    assert(player_stage_discard(root));
    assert(nk_platform_get_file_size(outside_file) == (int64_t)(sizeof(data) - 1));
    assert(!nk_platform_dir_exists(root));
    remove(outside_file);
    test_rmdir(outside);
    printf("[XB_TEST] POSIX symlink discard boundary PASSED\n");
#endif
}

typedef struct {
    const char *name;
    uint32_t lba;
    uint32_t size;
    uint8_t flags;
} IsoFixtureChild;

static size_t iso_record(uint8_t *record, uint32_t lba, uint32_t size,
                         uint8_t flags, const uint8_t *name, size_t name_size) {
    size_t record_size = 33 + name_size + ((name_size & 1u) == 0 ? 1u : 0u);
    memset(record, 0, record_size);
    record[0] = (uint8_t)record_size;
    record[2] = (uint8_t)lba;
    record[3] = (uint8_t)(lba >> 8);
    record[4] = (uint8_t)(lba >> 16);
    record[5] = (uint8_t)(lba >> 24);
    record[6] = (uint8_t)(lba >> 24);
    record[7] = (uint8_t)(lba >> 16);
    record[8] = (uint8_t)(lba >> 8);
    record[9] = (uint8_t)lba;
    record[10] = (uint8_t)size;
    record[11] = (uint8_t)(size >> 8);
    record[12] = (uint8_t)(size >> 16);
    record[13] = (uint8_t)(size >> 24);
    record[14] = (uint8_t)(size >> 24);
    record[15] = (uint8_t)(size >> 16);
    record[16] = (uint8_t)(size >> 8);
    record[17] = (uint8_t)size;
    record[25] = flags;
    record[32] = (uint8_t)name_size;
    memcpy(record + 33, name, name_size);
    return record_size;
}

static void iso_directory(uint8_t *sector, uint32_t self_lba, uint32_t parent_lba,
                          const IsoFixtureChild *children, size_t child_count) {
    size_t offset = 0;
    const uint8_t dot = 0;
    const uint8_t dotdot = 1;
    offset += iso_record(sector + offset, self_lba, 2048, 2, &dot, 1);
    offset += iso_record(sector + offset, parent_lba, 2048, 2, &dotdot, 1);
    for (size_t i = 0; i < child_count; i++) {
        size_t name_size = strlen(children[i].name);
        offset += iso_record(sector + offset, children[i].lba, children[i].size,
                             children[i].flags, (const uint8_t *)children[i].name,
                             name_size);
    }
    assert(offset < 2048);
}

static void write_file_bytes(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void staging_progress(const char *current_path, int percent,
                             size_t files_extracted, size_t total_files,
                             void *userdata) {
    (void)current_path;
    (void)percent;
    (void)userdata;
    assert(files_extracted <= total_files || total_files == 0);
}

static void test_iso_to_native_staging_pipeline(void) {
    static const uint8_t raw_data[] = "staged native XB data";
    static const uint8_t audio_data[] = "synthetic sound";
    static const uint8_t visual_data[] = "synthetic texture";
    static const uint8_t layout_data[] = "synthetic menu layout";
    const FixtureEntry xb_entries[] = {
        { "data/raw.bin", raw_data, sizeof(raw_data) - 1, NK_XB_COMPRESSION_NONE },
        { "data/sound/theme.sgd", audio_data, sizeof(audio_data) - 1, NK_XB_COMPRESSION_NONE },
        { "data/menu/title.gim", visual_data, sizeof(visual_data) - 1, NK_XB_COMPRESSION_NONE },
        { "data/menu/layout.bin", layout_data, sizeof(layout_data) - 1, NK_XB_COMPRESSION_NONE }
    };
    ByteBuffer xb = make_archive(xb_entries, sizeof(xb_entries) / sizeof(xb_entries[0]), false);
    assert(xb.size < 2048);

    const char *iso_path = "build/test_xb_staging.iso";
    const char *stage_root = "build/test_xb_staging_output";
    const char *appdata_root = "build/test_ppsspp_appdata";
    const char *userprofile_root = "build/test_ppsspp_user";
    const char *libfont_source = "build/test_ppsspp_appdata/PPSSPP/PSP/SYSTEM/DUMP/libfont.prx";
    const char *psmf_source = "build/test_ppsspp_appdata/PPSSPP/PSP/SYSTEM/DUMP/PRX/scePsmf_library.prx";
    const char *psmfp_source = "build/test_ppsspp_user/Documents/PPSSPP/PSP/SYSTEM/DUMP/scePsmfP_library.prx";
    const char *libfont_destination = "build/test_xb_staging_output/EXTRACTED/decrypted/libfont.prx";
    const char *psmf_destination = "build/test_xb_staging_output/EXTRACTED/decrypted/scePsmf_library.prx";
    const char *psmfp_destination = "build/test_xb_staging_output/EXTRACTED/decrypted/scePsmfP_library.prx";
    assert(nk_platform_mkdir_p("build"));
    assert(nk_platform_mkdir_p("build/test_ppsspp_appdata/PPSSPP/PSP/SYSTEM/DUMP/PRX"));
    assert(nk_platform_mkdir_p("build/test_ppsspp_user/Documents/PPSSPP/PSP/SYSTEM/DUMP"));
    static const uint8_t libfont_data[] = "synthetic libfont";
    static const uint8_t psmf_data[] = "synthetic psmf";
    static const uint8_t psmfp_data[] = "synthetic psmfp";
    write_file_bytes(libfont_source, libfont_data, sizeof(libfont_data) - 1);
    write_file_bytes(psmf_source, psmf_data, sizeof(psmf_data) - 1);
    write_file_bytes(psmfp_source, psmfp_data, sizeof(psmfp_data) - 1);
    char old_appdata[4096];
    char old_userprofile[4096];
    bool had_appdata;
    bool had_userprofile;
    capture_environment_value("APPDATA", old_appdata, sizeof(old_appdata), &had_appdata);
    capture_environment_value("USERPROFILE", old_userprofile, sizeof(old_userprofile), &had_userprofile);
    set_environment_value("APPDATA", appdata_root);
    set_environment_value("USERPROFILE", userprofile_root);
    remove(iso_path);
    char eboot_path[256];
    char archive_path[256];
    char raw_path[256];
    char audio_path[256];
    char visual_path[256];
    char layout_path[256];
    snprintf(eboot_path, sizeof(eboot_path), "%s/EBOOT.BIN", stage_root);
    snprintf(archive_path, sizeof(archive_path), "%s/xbdata/assets.xb", stage_root);
    snprintf(raw_path, sizeof(raw_path), "%s/xbdata/assets.xb.d/data/raw.bin", stage_root);
    snprintf(audio_path, sizeof(audio_path), "%s/xbdata/assets.xb.d/data/sound/theme.sgd", stage_root);
    snprintf(visual_path, sizeof(visual_path), "%s/xbdata/assets.xb.d/data/menu/title.gim", stage_root);
    snprintf(layout_path, sizeof(layout_path), "%s/xbdata/assets.xb.d/data/menu/layout.bin", stage_root);
    remove(raw_path);
    remove(audio_path);
    remove(visual_path);
    remove(layout_path);
    test_rmdir("build/test_xb_staging_output/xbdata/assets.xb.d/data/sound");
    test_rmdir("build/test_xb_staging_output/xbdata/assets.xb.d/data/menu");
    test_rmdir("build/test_xb_staging_output/xbdata/assets.xb.d/data");
    test_rmdir("build/test_xb_staging_output/xbdata/assets.xb.d");
    remove(archive_path);
    test_rmdir("build/test_xb_staging_output/xbdata");
    remove(eboot_path);
    remove(libfont_destination);
    remove(psmf_destination);
    remove(psmfp_destination);
    test_rmdir("build/test_xb_staging_output/EXTRACTED/decrypted");
    test_rmdir("build/test_xb_staging_output/EXTRACTED");
    test_rmdir(stage_root);

    const size_t sector_count = 32;
    const size_t image_size = sector_count * 2048;
    uint8_t *image = (uint8_t *)calloc(1, image_size);
    assert(image != NULL);
    uint8_t *pvd = image + 16 * 2048;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    const uint8_t root_name = 0;
    iso_record(pvd + 156, 17, 2048, 2, &root_name, 1);

    const IsoFixtureChild root_children[] = {
        { "PSP_GAME", 18, 2048, 2 }
    };
    const IsoFixtureChild psp_children[] = {
        { "SYSDIR", 19, 2048, 2 },
        { "USRDIR", 20, 2048, 2 }
    };
    const IsoFixtureChild sys_children[] = {
        { "EBOOT.BIN", 21, 4, 0 }
    };
    const IsoFixtureChild usr_children[] = {
        { "xbdata", 22, 2048, 2 }
    };
    const IsoFixtureChild xb_children[] = {
        { "assets.xb", 23, (uint32_t)xb.size, 0 }
    };
    iso_directory(image + 17 * 2048, 17, 17, root_children, 1);
    iso_directory(image + 18 * 2048, 18, 17, psp_children, 2);
    iso_directory(image + 19 * 2048, 19, 18, sys_children, 1);
    iso_directory(image + 20 * 2048, 20, 18, usr_children, 1);
    iso_directory(image + 22 * 2048, 22, 20, xb_children, 1);
    memcpy(image + 21 * 2048, "BOOT", 4);
    memcpy(image + 23 * 2048, xb.data, xb.size);

    PlayerStageCallbacks callbacks = { NULL, staging_progress, NULL };
    PlayerStageSummary summary;
    char error[256];

    /* ISO directory identifiers carry an explicit byte length. A raw NUL in
     * that span must not be accepted and then truncated into a colliding C
     * string by the staging walker. The first root child starts after the two
     * 34-byte structural records. */
    uint8_t *root_child = image + 17 * 2048 + 68;
    assert(root_child[0] == 42 && root_child[32] == 8);
    root_child[32] = 9; /* PSP_GAME plus the following raw NUL byte */
    const char *nul_stage_root = "build/.staging_xb_nul_identifier";
    (void)player_stage_discard(nul_stage_root);
    write_file_bytes(iso_path, image, image_size);
    assert(player_stage_game_with_summary(iso_path, nul_stage_root, &callbacks,
                                          &summary, error, sizeof(error)) == NK_ERROR_INVALID_ISO);
    assert(player_stage_discard(nul_stage_root));
    remove(iso_path);
    root_child[32] = 8;
    write_file_bytes(iso_path, image, image_size);
    free(image);
    free(xb.data);

    NkResult stage_result = player_stage_game_with_summary(
        iso_path, stage_root, &callbacks, &summary, error, sizeof(error));
    if (stage_result != NK_OK) {
        fprintf(stderr, "[XB_TEST] valid staging unexpectedly failed (%d): %s\n",
                (int)stage_result, error);
    }
    assert(stage_result == NK_OK);
    assert(summary.extracted_asset_count == 4);
    assert(summary.extracted_audio_count == 1);
    assert(summary.extracted_visual_count == 1);
    assert(summary.extracted_layout_count == 2);

    /* The source-owned TEST00007 showcase ISO has no PSP_GAME/USRDIR/xbdata.
     * A self-contained EBOOT is a valid staging input; the host gets an empty
     * xbdata root so consumers see the same prepared-tree shape. */
    const char *empty_iso_path = "build/test_xb_empty_stage.iso";
    const char *empty_stage_root = "build/.staging_xb_empty_assets";
    const size_t empty_image_size = 32u * 2048u;
    uint8_t *empty_image = (uint8_t *)calloc(1, empty_image_size);
    assert(empty_image != NULL);
    uint8_t *empty_pvd = empty_image + 16u * 2048u;
    empty_pvd[0] = 1;
    memcpy(empty_pvd + 1, "CD001", 5);
    empty_pvd[6] = 1;
    iso_record(empty_pvd + 156, 17, 2048, 2, &root_name, 1);
    const IsoFixtureChild empty_root_children[] = {
        { "PSP_GAME", 18, 2048, 2 }
    };
    const IsoFixtureChild empty_game_children[] = {
        { "SYSDIR", 19, 2048, 2 }
    };
    const IsoFixtureChild empty_sysdir_children[] = {
        { "EBOOT.BIN", 21, 4, 0 }
    };
    iso_directory(empty_image + 17u * 2048u, 17, 17,
                  empty_root_children, 1);
    iso_directory(empty_image + 18u * 2048u, 18, 17,
                  empty_game_children, 1);
    iso_directory(empty_image + 19u * 2048u, 19, 18,
                  empty_sysdir_children, 1);
    memcpy(empty_image + 21u * 2048u, "BOOT", 4);
    (void)player_stage_discard(empty_stage_root);
    write_file_bytes(empty_iso_path, empty_image, empty_image_size);
    free(empty_image);
    assert(player_stage_game_with_summary(empty_iso_path, empty_stage_root,
                                          &callbacks, &summary, error,
                                          sizeof(error)) == NK_OK);
    assert(error[0] == '\0');
    assert(summary.extracted_asset_count == 0);
    char empty_eboot_path[256];
    char empty_xbdata_path[256];
    snprintf(empty_eboot_path, sizeof(empty_eboot_path), "%s/EBOOT.BIN",
             empty_stage_root);
    snprintf(empty_xbdata_path, sizeof(empty_xbdata_path), "%s/xbdata",
             empty_stage_root);
    assert(nk_platform_file_exists(empty_eboot_path));
    assert(nk_platform_dir_exists(empty_xbdata_path));
    assert(player_stage_discard(empty_stage_root));
    remove(empty_iso_path);

    /* Direct ISO-stage failures carry an actionable code/message into the
     * wizard card instead of the previous generic "game staging failed". */
    const char *missing_stage_root = "build/.staging_xb_missing_input";
    (void)player_stage_discard(missing_stage_root);
    assert(player_stage_game_with_summary("build/no_such_source_owned_iso.iso",
                                          missing_stage_root, &callbacks,
                                          &summary, error, sizeof(error)) ==
           NK_ERROR_FILE_NOT_FOUND);
    assert(strstr(error, "[STAGE_REQUIRED_FILE_MISSING]") != NULL);
    assert(!nk_platform_dir_exists(missing_stage_root));
    assert(player_stage_game_with_summary(NULL, missing_stage_root, &callbacks,
                                          &summary, error, sizeof(error)) ==
           NK_ERROR_GENERIC);
    assert(strstr(error, "[STAGE_REQUEST_INVALID]") != NULL);
    const char *bad_iso_path = "build/test_xb_malformed_stage.iso";
    const char *bad_stage_root = "build/.staging_xb_malformed_input";
    (void)player_stage_discard(bad_stage_root);
    write_file_bytes(bad_iso_path, "not an ISO", 10);
    assert(player_stage_game_with_summary(bad_iso_path, bad_stage_root,
                                          &callbacks, &summary, error,
                                          sizeof(error)) == NK_ERROR_INVALID_ISO);
    assert(strstr(error, "[STAGE_ISO_INVALID]") != NULL);
    assert(!nk_platform_dir_exists(bad_stage_root));
    remove(bad_iso_path);

    FILE *prx_file = fopen(libfont_destination, "rb");
    assert(prx_file != NULL);
    char prx_buffer[64] = { 0 };
    assert(fread(prx_buffer, 1, sizeof(libfont_data) - 1, prx_file) == sizeof(libfont_data) - 1);
    assert(fclose(prx_file) == 0);
    assert(memcmp(prx_buffer, libfont_data, sizeof(libfont_data) - 1) == 0);
    prx_file = fopen(psmf_destination, "rb");
    assert(prx_file != NULL);
    memset(prx_buffer, 0, sizeof(prx_buffer));
    assert(fread(prx_buffer, 1, sizeof(psmf_data) - 1, prx_file) == sizeof(psmf_data) - 1);
    assert(fclose(prx_file) == 0);
    assert(memcmp(prx_buffer, psmf_data, sizeof(psmf_data) - 1) == 0);
    prx_file = fopen(psmfp_destination, "rb");
    assert(prx_file != NULL);
    memset(prx_buffer, 0, sizeof(prx_buffer));
    assert(fread(prx_buffer, 1, sizeof(psmfp_data) - 1, prx_file) == sizeof(psmfp_data) - 1);
    assert(fclose(prx_file) == 0);
    assert(memcmp(prx_buffer, psmfp_data, sizeof(psmfp_data) - 1) == 0);

    FILE *file = fopen(eboot_path, "rb");
    assert(file != NULL);
    char eboot[8] = { 0 };
    assert(fread(eboot, 1, 4, file) == 4);
    assert(fclose(file) == 0);
    assert(memcmp(eboot, "BOOT", 4) == 0);
    file = fopen(raw_path, "rb");
    assert(file != NULL);
    char staged[64] = { 0 };
    assert(fread(staged, 1, sizeof(raw_data) - 1, file) == sizeof(raw_data) - 1);
    assert(fclose(file) == 0);
    assert(memcmp(staged, raw_data, sizeof(raw_data) - 1) == 0);

    remove(raw_path);
    remove(audio_path);
    remove(visual_path);
    remove(layout_path);
    test_rmdir("build/test_xb_staging_output/xbdata/assets.xb.d/data/sound");
    test_rmdir("build/test_xb_staging_output/xbdata/assets.xb.d/data/menu");
    test_rmdir("build/test_xb_staging_output/xbdata/assets.xb.d/data");
    test_rmdir("build/test_xb_staging_output/xbdata/assets.xb.d");
    remove(archive_path);
    test_rmdir("build/test_xb_staging_output/xbdata");
    remove(eboot_path);
    remove(iso_path);
    remove(libfont_destination);
    remove(psmf_destination);
    remove(psmfp_destination);
    test_rmdir("build/test_xb_staging_output/EXTRACTED/decrypted");
    test_rmdir("build/test_xb_staging_output/EXTRACTED");
    test_rmdir(stage_root);
    remove(libfont_source);
    remove(psmf_source);
    remove(psmfp_source);
    test_rmdir("build/test_ppsspp_appdata/PPSSPP/PSP/SYSTEM/DUMP/PRX");
    test_rmdir("build/test_ppsspp_appdata/PPSSPP/PSP/SYSTEM/DUMP");
    test_rmdir("build/test_ppsspp_appdata/PPSSPP/PSP/SYSTEM");
    test_rmdir("build/test_ppsspp_appdata/PPSSPP/PSP");
    test_rmdir("build/test_ppsspp_appdata/PPSSPP");
    test_rmdir(appdata_root);
    test_rmdir("build/test_ppsspp_user/Documents/PPSSPP/PSP/SYSTEM/DUMP");
    test_rmdir("build/test_ppsspp_user/Documents/PPSSPP/PSP/SYSTEM");
    test_rmdir("build/test_ppsspp_user/Documents/PPSSPP/PSP");
    test_rmdir("build/test_ppsspp_user/Documents/PPSSPP");
    test_rmdir("build/test_ppsspp_user/Documents");
    test_rmdir(userprofile_root);
    restore_environment_value("APPDATA", old_appdata, had_appdata);
    restore_environment_value("USERPROFILE", old_userprofile, had_userprofile);
    printf("[XB_TEST] ISO EBOOT + xbdata native staging pipeline PASSED\n");
}

static int archive_list_has(const SrVfsDirList *list, const char *name) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].name, name) == 0) return 1;
    }
    return 0;
}

static void test_archive_vfs_mount_scaling(void) {
    static const size_t archive_counts[] = { 250u, 500u, 1000u, 2000u };
    const size_t members_per_archive = 8u;
    uint8_t payload[8] = { 0 };
    FixtureEntry entries[8];
    char paths[8][64];

    for (size_t scale = 0; scale < sizeof(archive_counts) / sizeof(archive_counts[0]); scale++) {
        size_t archive_count = archive_counts[scale];
        ByteBuffer *archives = (ByteBuffer *)calloc(archive_count, sizeof(*archives));
        assert(archives != NULL);
        for (size_t archive_index = 0; archive_index < archive_count; archive_index++) {
            for (size_t member_index = 0; member_index < members_per_archive; member_index++) {
                snprintf(paths[member_index], sizeof(paths[member_index]),
                         "data/gen/a%04zu/f%02zu.bin", archive_index, member_index);
                entries[member_index].path = paths[member_index];
                entries[member_index].data = payload + member_index;
                entries[member_index].size = 1u;
                entries[member_index].compression = NK_XB_COMPRESSION_NONE;
            }
            archives[archive_index] = make_archive(entries, members_per_archive, false);
        }

        SrArchiveVfs vfs;
        sr_archive_vfs_init(&vfs);
        clock_t started = clock();
        for (size_t archive_index = 0; archive_index < archive_count; archive_index++) {
            assert(sr_archive_vfs_mount_memory(&vfs, archives[archive_index].data,
                                               archives[archive_index].size, "scale.xb", false,
                                               -1, NULL) == NK_OK);
        }
        clock_t finished = clock();
        assert(sr_archive_vfs_mount_count(&vfs) == archive_count);
        assert(sr_archive_vfs_entry_count(&vfs) == archive_count * members_per_archive);
        assert(sr_archive_vfs_index_sort_count(&vfs) == 0u);
        clock_t finalize_started = clock();
        assert(sr_archive_vfs_finalize(&vfs));
        clock_t finalize_finished = clock();
        assert(sr_archive_vfs_index_sort_count(&vfs) == 1u);
        for (size_t archive_index = 0; archive_index < archive_count; archive_index++) {
            for (size_t member_index = 0; member_index < members_per_archive; member_index++) {
                char query[64];
                SrArchiveFile file;
                snprintf(query, sizeof(query), "data/gen/a%04zu/f%02zu.bin",
                         archive_index, member_index);
                assert(sr_archive_vfs_lookup(&vfs, query, -2, &file));
                assert(file.mount_index == archive_index);
                assert(file.entry_index == member_index);
                assert(file.size == 1u);
            }
        }
        assert(sr_archive_vfs_index_sort_count(&vfs) == 1u);
        printf("[ARCHIVE_VFS] mount scaling N=%zu M=%zu mount=%.6f finalize=%.6f total=%.6f CPU seconds sorts=%zu\n",
               archive_count, members_per_archive,
               (double)(finished - started) / CLOCKS_PER_SEC,
               (double)(finalize_finished - finalize_started) / CLOCKS_PER_SEC,
               (double)(finalize_finished - started) / CLOCKS_PER_SEC,
               sr_archive_vfs_index_sort_count(&vfs));
        sr_archive_vfs_destroy(&vfs);
        for (size_t archive_index = 0; archive_index < archive_count; archive_index++) {
            free(archives[archive_index].data);
        }
        free(archives);
    }
}

static void test_archive_vfs_precedence(void) {
    static const uint8_t unqualified_data[] = "unqualified";
    static const uint8_t variant_two_data[] = "variant-two-first";
    static const uint8_t variant_two_again_data[] = "variant-two-second";
    static const uint8_t variant_zero_data[] = "variant-zero";
    const FixtureEntry unqualified_entry = {
        "Data/Shared.BIN", unqualified_data, sizeof(unqualified_data) - 1u,
        NK_XB_COMPRESSION_NONE
    };
    const FixtureEntry variant_two_entry = {
        "data/shared.bin", variant_two_data, sizeof(variant_two_data) - 1u,
        NK_XB_COMPRESSION_NONE
    };
    const FixtureEntry variant_two_again_entry = {
        "DATA/SHARED.BIN", variant_two_again_data, sizeof(variant_two_again_data) - 1u,
        NK_XB_COMPRESSION_NONE
    };
    const FixtureEntry variant_zero_entry = {
        "data/shared.bin", variant_zero_data, sizeof(variant_zero_data) - 1u,
        NK_XB_COMPRESSION_NONE
    };
    ByteBuffer archives[4] = { { 0 }, { 0 }, { 0 }, { 0 } };
    archives[0] = make_archive(&unqualified_entry, 1u, false);
    archives[1] = make_archive(&variant_two_entry, 1u, false);
    archives[2] = make_archive(&variant_two_again_entry, 1u, false);
    archives[3] = make_archive(&variant_zero_entry, 1u, false);

    SrArchiveVfs vfs;
    sr_archive_vfs_init(&vfs);
    assert(sr_archive_vfs_mount_memory(&vfs, archives[0].data, archives[0].size,
                                       "shared.xb", false, -1, NULL) == NK_OK);
    assert(sr_archive_vfs_mount_memory(&vfs, archives[1].data, archives[1].size,
                                       "shared.xb2", false, 2, NULL) == NK_OK);
    assert(sr_archive_vfs_mount_memory(&vfs, archives[2].data, archives[2].size,
                                       "shared.xb2", false, 2, NULL) == NK_OK);
    assert(sr_archive_vfs_mount_memory(&vfs, archives[3].data, archives[3].size,
                                       "shared.xb0", false, 0, NULL) == NK_OK);
    assert(sr_archive_vfs_index_sort_count(&vfs) == 0u);
    assert(sr_archive_vfs_finalize(&vfs));
    assert(sr_archive_vfs_index_sort_count(&vfs) == 1u);

    SrArchiveFile file;
    assert(sr_archive_vfs_lookup(&vfs, "DaTa/ShArEd.BiN", -2, &file));
    assert(file.mount_index == 0u && file.size == sizeof(unqualified_data) - 1u);
    assert(sr_archive_vfs_lookup(&vfs, "data/shared.bin", 2, &file));
    assert(file.mount_index == 1u && file.size == sizeof(variant_two_data) - 1u);
    assert(sr_archive_vfs_lookup(&vfs, "data/shared.bin", 0, &file));
    assert(file.mount_index == 3u && file.size == sizeof(variant_zero_data) - 1u);

    SrVfsDirList list;
    sr_vfs_dirlist_init(&list);
    assert(sr_archive_vfs_list_dir(&vfs, "DATA", -2, &list) == 1);
    assert(list.exists && list.count == 1u);
    assert(archive_list_has(&list, "Shared.BIN"));
    sr_vfs_dirlist_destroy(&list);
    assert(sr_archive_vfs_index_sort_count(&vfs) == 1u);

    sr_archive_vfs_destroy(&vfs);
    for (size_t i = 0; i < sizeof(archives) / sizeof(archives[0]); i++) {
        free(archives[i].data);
    }
}

static void test_archive_vfs_provider(void) {
    static const uint8_t raw_data[] = "raw archive bytes";
    static const uint8_t lzs_data[] = "lzs archive bytes";
    static const uint8_t huf_data[] = "huffman archive bytes";
    const FixtureEntry entries[] = {
        { "data/menu/raw.bin", raw_data, sizeof(raw_data) - 1u, NK_XB_COMPRESSION_NONE },
        { "data/menu/lzs.bin", lzs_data, sizeof(lzs_data) - 1u, NK_XB_COMPRESSION_LZS },
        { "data/menu/nested/huffman.bin", huf_data, sizeof(huf_data) - 1u, NK_XB_COMPRESSION_HUFFMAN }
    };
    ByteBuffer bytes = make_archive(entries, 3, false);
    SrArchiveVfs vfs;
    sr_archive_vfs_init(&vfs);
    assert(sr_archive_vfs_configure(&vfs, 32u, 1u));
    assert(sr_archive_vfs_mount_memory(&vfs, bytes.data, bytes.size, "fixture.xb", false,
                                       -1, NULL) == NK_OK);
    assert(sr_archive_vfs_mount_count(&vfs) == 1u);
    assert(sr_archive_vfs_entry_count(&vfs) == 3u);
    SrArchiveFile file;
    assert(sr_archive_vfs_lookup(&vfs, "data/menu/raw.bin", -2, &file));
    uint8_t output[64];
    size_t output_size = 0;
    assert(sr_archive_vfs_read(&vfs, &file, 4u, output, 7u, &output_size) == NK_OK);
    assert(output_size == 7u);
    assert(memcmp(output, raw_data + 4u, 7u) == 0);
    assert(sr_archive_vfs_read(&vfs, &file, sizeof(raw_data) - 2u, output, 20u,
                               &output_size) == NK_OK);
    assert(output_size == 1u && output[0] == raw_data[sizeof(raw_data) - 2u]);
    assert(sr_archive_vfs_lookup(&vfs, "data/menu/lzs.bin", -2, &file));
    assert(sr_archive_vfs_read(&vfs, &file, 0u, output, sizeof(output), &output_size) == NK_OK);
    assert(output_size == sizeof(lzs_data) - 1u);
    assert(memcmp(output, lzs_data, output_size) == 0);
    assert(sr_archive_vfs_lookup(&vfs, "data/menu/nested/huffman.bin", -2, &file));
    assert(sr_archive_vfs_read(&vfs, &file, 0u, output, sizeof(output), &output_size) == NK_OK);
    assert(output_size == sizeof(huf_data) - 1u);
    assert(memcmp(output, huf_data, output_size) == 0);
    assert(sr_archive_vfs_cache_bytes(&vfs) <= 32u);
    assert(sr_archive_vfs_cache_entry_count(&vfs) <= 1u);

    SrVfsDirList list;
    sr_vfs_dirlist_init(&list);
    assert(sr_archive_vfs_list_dir(&vfs, "data/menu", -2, &list) == 1);
    assert(list.exists && list.count == 3u);
    assert(archive_list_has(&list, "raw.bin"));
    assert(archive_list_has(&list, "lzs.bin"));
    assert(archive_list_has(&list, "nested"));
    sr_vfs_dirlist_destroy(&list);

    int variant = 0;
    assert(sr_archive_variant_from_name("fixture.xb", &variant) && variant == -1);
    assert(sr_archive_variant_from_name("fixture.xb0", &variant) && variant == 0);
    assert(sr_archive_variant_from_name("fixture.xb12", &variant) && variant == 12);
    assert(!sr_archive_variant_from_name("fixture.xb2147483648", &variant));
    assert(!sr_archive_variant_from_name("fixture.xb.d", &variant));

    SrVfsDirList missing_variant;
    sr_vfs_dirlist_init(&missing_variant);
    assert(sr_archive_vfs_list_dir(&vfs, "data/menu", 1, &missing_variant) == 1);
    assert(missing_variant.exists && missing_variant.count == 0u);
    sr_vfs_dirlist_destroy(&missing_variant);

    SrArchiveVfs malformed;
    sr_archive_vfs_init(&malformed);
    const uint8_t truncated[] = { 0x78, 0x65, 0x00, 0x01, 0x01, 0x00, 0x00 };
    assert(sr_archive_vfs_mount_memory(&malformed, truncated, sizeof(truncated),
                                       "truncated.xb", false, -1, NULL) != NK_OK);
    assert(sr_archive_vfs_entry_count(&malformed) == 0u);
    sr_archive_vfs_destroy(&malformed);
    sr_archive_vfs_destroy(&vfs);
    free(bytes.data);
    printf("[ARCHIVE_VFS] read equivalence, partial seek, listing, cache, and rejection PASSED\n");
}

static void test_archive_vfs_many_members(void) {
    const size_t member_count = 2048u;
    FixtureEntry *entries = (FixtureEntry *)calloc(member_count, sizeof(*entries));
    uint8_t *payload = (uint8_t *)malloc(member_count);
    char (*paths)[32] = (char (*)[32])calloc(member_count, sizeof(*paths));
    assert(entries != NULL && payload != NULL && paths != NULL);
    for (size_t i = 0; i < member_count; i++) {
        snprintf(paths[i], sizeof(paths[i]), "data/gen/file%04zu.bin", i);
        payload[i] = (uint8_t)(i & 0xffu);
        entries[i].path = paths[i];
        entries[i].data = payload + i;
        entries[i].size = 1u;
        entries[i].compression = NK_XB_COMPRESSION_NONE;
    }
    ByteBuffer bytes = make_archive(entries, member_count, false);
    SrArchiveVfs vfs;
    sr_archive_vfs_init(&vfs);
    clock_t started = clock();
    assert(sr_archive_vfs_mount_memory(&vfs, bytes.data, bytes.size, "many.xb", false,
                                       -1, NULL) == NK_OK);
    clock_t finished = clock();
    assert(sr_archive_vfs_entry_count(&vfs) == member_count);
    SrArchiveFile file;
    assert(sr_archive_vfs_lookup(&vfs, paths[member_count - 1u], -2, &file));
    assert(file.size == 1u);
    printf("[ARCHIVE_VFS] indexed %zu synthetic members in %.3f CPU seconds\n",
           member_count, (double)(finished - started) / CLOCKS_PER_SEC);
    sr_archive_vfs_destroy(&vfs);
    free(bytes.data);
    free(paths);
    free(payload);
    free(entries);
}

int main(void) {
    printf("[XB_TEST] Starting native clean-room XB parser tests...\n");
    test_roundtrip_all_compression_modes();
    test_file_backed_archive_is_lazy();
    test_file_backed_duplicate_validation();
    test_lazy_payload_failure_is_deferred();
    test_huffman_final_buffered_bits();
    test_big_endian_fields();
    test_truncated_header();
    test_path_traversal();
    test_corrupt_huffman_prefix();
    test_truncated_huffman_body();
    test_lzs_lookback_floor();
    test_expansion_limit();
    test_compressed_header_expansion_limit();
    test_duplicate_canonical_path();
    test_deterministic_mutation_fuzz();
    test_archive_vfs_precedence();
    test_archive_vfs_provider();
    test_archive_vfs_many_members();
    test_archive_vfs_mount_scaling();
    test_staging_cleanup_boundary();
    test_staging_discard_reparse_boundary();
    test_iso_to_native_staging_pipeline();
    printf("[XB_TEST] ALL NATIVE XB PARSER TESTS PASSED\n");
    return 0;
}
