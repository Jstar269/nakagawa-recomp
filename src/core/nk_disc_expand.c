/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Compressed disc images expanded once at import (see nk_disc_expand.h).
 *
 * Project-authored from the public descriptions of the formats: the CISO
 * header and block index (CSO v1, and the v2 revision that adds LZ4 blocks and
 * marks a stored block by its size), ZISO (the same layout with LZ4 blocks)
 * and the LZ4 block format. No third-party CSO, ZSO or LZ4 source was copied.
 * DEFLATE blocks go through the project's own nk_psp_inflate_raw; CHD goes
 * through the registered backend (nk_disc_chd.c).
 */

#if !defined(_WIN32) && !defined(_WIN64)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "nk_disc_expand.h"

#include "nk_iso.h"
#include "nk_platform.h"
#include "nk_psp_inflate.h"
#include "nk_psp_sha1.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define NK_CISO_HEADER_BYTES 24u
#define NK_CISO_MIN_BLOCK 2048u
#define NK_CISO_MAX_BLOCK (1024u * 1024u)
#define NK_CISO_MAX_ALIGN 16u
#define NK_CISO_PLAIN_FLAG 0x80000000u
#define NK_CHD_MAX_HUNK (16u * 1024u * 1024u)
#define NK_DISC_FINGERPRINT_HEAD (64u * 1024u)
#define NK_DISC_PROGRESS_STEP (1024u * 1024u)
#define NK_DISC_IO_BUFFER (1024u * 1024u)
#define NK_DISC_EXPANDED_INVALID "DISC_IMAGE_EXPANDED_INVALID"

static const NkDiscChdBackend *s_chd_backend = NULL;

void nk_disc_set_chd_backend(const NkDiscChdBackend *backend) {
    s_chd_backend = backend;
}

static bool disc_fail(NkDiscExpandStatus *status, const char *code, const char *fmt, ...) {
    if (status) {
        va_list args;
        snprintf(status->code, sizeof(status->code), "%s", code);
        va_start(args, fmt);
        vsnprintf(status->message, sizeof(status->message), fmt, args);
        va_end(args);
    }
    return false;
}

static void disc_status_clear(NkDiscExpandStatus *status) {
    if (status) {
        status->code[0] = '\0';
        status->message[0] = '\0';
    }
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

NkDiscFormat nk_disc_format_from_magic(const uint8_t *bytes, size_t length) {
    if (!bytes) return NK_DISC_FORMAT_NONE;
    if (length >= 4u && memcmp(bytes, "CISO", 4) == 0) return NK_DISC_FORMAT_CSO;
    if (length >= 4u && memcmp(bytes, "ZISO", 4) == 0) return NK_DISC_FORMAT_ZSO;
    if (length >= 8u && memcmp(bytes, "MComprHD", 8) == 0) return NK_DISC_FORMAT_CHD;
    return NK_DISC_FORMAT_NONE;
}

NkDiscFormat nk_disc_image_format(const char *path) {
    uint8_t magic[8];
    size_t got;
    FILE *f;
    if (!path || !path[0]) return NK_DISC_FORMAT_NONE;
    f = nk_fopen_utf8(path, "rb");
    if (!f) return NK_DISC_FORMAT_NONE;
    got = fread(magic, 1, sizeof(magic), f);
    fclose(f);
    return nk_disc_format_from_magic(magic, got);
}

const char *nk_disc_format_extension(NkDiscFormat format) {
    switch (format) {
    case NK_DISC_FORMAT_CSO: return "cso";
    case NK_DISC_FORMAT_ZSO: return "zso";
    case NK_DISC_FORMAT_CHD: return "chd";
    default: return "";
    }
}

/* ---- source file ------------------------------------------------------- */

typedef struct {
    FILE *f;
    uint64_t size;
    uint64_t pos; /* where the next fread starts; UINT64_MAX when unknown */
} NkDiscSource;

static bool source_open(NkDiscSource *source, const char *path, NkDiscExpandStatus *status) {
    int64_t end;
    memset(source, 0, sizeof(*source));
    source->f = nk_fopen_utf8(path, "rb");
    if (!source->f) {
        return disc_fail(status, NK_DISC_READ_FAILED,
                         "The disc image could not be opened for reading.");
    }
    /* Before any other use of the stream, as setvbuf requires. */
    setvbuf(source->f, NULL, _IOFBF, NK_DISC_IO_BUFFER);
    if (nk_fseek64(source->f, 0, SEEK_END) != 0 || (end = nk_ftell64(source->f)) < 0 ||
        nk_fseek64(source->f, 0, SEEK_SET) != 0) {
        fclose(source->f);
        source->f = NULL;
        return disc_fail(status, NK_DISC_READ_FAILED, "The disc image's size could not be read.");
    }
    source->size = (uint64_t)end;
    source->pos = 0;
    return true;
}

static void source_close(NkDiscSource *source) {
    if (source->f) fclose(source->f);
    source->f = NULL;
}

/* Read exactly `bytes` at `offset`; a span past the end of the file is a
   truncated image. Blocks are read in file order, so the stream seeks only
   when the next span does not start where the last one ended (a seek would
   throw away the read buffer). */
static bool source_read_at(NkDiscSource *source, uint64_t offset, void *dst, size_t bytes,
                           NkDiscExpandStatus *status) {
    if (offset > source->size || (uint64_t)bytes > source->size - offset) {
        return disc_fail(status, NK_DISC_TRUNCATED,
                         "The disc image ends before the data its header describes.");
    }
    if (offset != source->pos &&
        (offset > (uint64_t)INT64_MAX || nk_fseek64(source->f, (int64_t)offset, SEEK_SET) != 0)) {
        source->pos = UINT64_MAX;
        return disc_fail(status, NK_DISC_READ_FAILED, "The disc image could not be read.");
    }
    if (fread(dst, 1, bytes, source->f) != bytes) {
        source->pos = UINT64_MAX;
        return disc_fail(status, NK_DISC_READ_FAILED, "The disc image could not be read.");
    }
    source->pos = offset + bytes;
    return true;
}

/* ---- output ------------------------------------------------------------ */

typedef struct {
    FILE *f;
    uint64_t written;
    uint64_t total;
    uint64_t next_report;
    const NkDiscExpandCallbacks *callbacks;
} NkDiscSink;

static bool sink_write(NkDiscSink *sink, const uint8_t *data, size_t bytes,
                       NkDiscExpandStatus *status) {
    if (bytes > sink->total - sink->written) {
        return disc_fail(status, NK_DISC_INTERNAL, "The expansion produced more data than expected.");
    }
    if (bytes && fwrite(data, 1, bytes, sink->f) != bytes) {
        return disc_fail(status, NK_DISC_WRITE_FAILED,
                         "The expanded disc image could not be written (%s). Check that the "
                         "drive has enough free space.", strerror(errno));
    }
    sink->written += bytes;
    if (sink->written >= sink->next_report || sink->written == sink->total) {
        sink->next_report = sink->written + NK_DISC_PROGRESS_STEP;
        if (sink->callbacks && sink->callbacks->on_progress) {
            sink->callbacks->on_progress(sink->written, sink->total, sink->callbacks->user);
        }
        if (sink->callbacks && sink->callbacks->is_cancelled &&
            sink->callbacks->is_cancelled(sink->callbacks->user)) {
            return disc_fail(status, NK_DISC_CANCELLED,
                             "The expansion was cancelled, and the partial disc image was removed.");
        }
    }
    return true;
}

/* ---- LZ4 block decoder ------------------------------------------------- */

/* Decode one LZ4 block into dst (capacity dst_cap) until at least `want`
   bytes are out. Bytes after that point (index alignment padding) are not
   read. Every length, offset and copy is checked against both buffers. */
static bool lz4_block_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap,
                             size_t want, size_t *out_len, const char **why) {
    size_t ip = 0;
    size_t op = 0;
    while (op < want) {
        uint8_t token;
        size_t literals;
        size_t offset;
        size_t match;
        if (ip >= src_len) {
            *why = "the block ends before its data is complete";
            return false;
        }
        token = src[ip++];
        literals = (size_t)(token >> 4);
        if (literals == 15u) {
            uint8_t more;
            do {
                if (ip >= src_len) {
                    *why = "a literal length runs past the block";
                    return false;
                }
                more = src[ip++];
                literals += more;
                if (literals > dst_cap) {
                    *why = "a literal run is longer than the block";
                    return false;
                }
            } while (more == 255u);
        }
        if (literals > src_len - ip) {
            *why = "a literal run runs past the block";
            return false;
        }
        if (literals > dst_cap - op) {
            *why = "a literal run is longer than the block";
            return false;
        }
        memcpy(dst + op, src + ip, literals);
        ip += literals;
        op += literals;
        if (op >= want) break;
        if (src_len - ip < 2u) {
            *why = "a match offset runs past the block";
            return false;
        }
        offset = (size_t)src[ip] | ((size_t)src[ip + 1u] << 8);
        ip += 2u;
        if (offset == 0u || offset > op) {
            *why = "a match points before the start of the block";
            return false;
        }
        match = (size_t)(token & 15u);
        if (match == 15u) {
            uint8_t more;
            do {
                if (ip >= src_len) {
                    *why = "a match length runs past the block";
                    return false;
                }
                more = src[ip++];
                match += more;
                if (match > dst_cap) {
                    *why = "a match is longer than the block";
                    return false;
                }
            } while (more == 255u);
        }
        match += 4u;
        if (match > dst_cap - op) {
            *why = "a match is longer than the block";
            return false;
        }
        /* Byte by byte: a match may overlap the bytes it is copying. */
        for (size_t i = 0; i < match; i++) dst[op + i] = dst[op - offset + i];
        op += match;
    }
    *out_len = op;
    return true;
}

/* ---- CSO / ZSO ----------------------------------------------------------- */

typedef struct {
    NkDiscFormat format;
    uint32_t version;     /* 1 or 2 for CSO, 1 for ZSO */
    uint64_t total_bytes;
    uint32_t block_size;
    uint32_t align;
    uint32_t block_count;
    uint32_t *index;      /* block_count + 1 entries */
} NkCisoImage;

static uint64_t ciso_offset(const NkCisoImage *image, uint32_t entry) {
    return (uint64_t)(image->index[entry] & ~NK_CISO_PLAIN_FLAG) << image->align;
}

static uint64_t ciso_max_block_span(const NkCisoImage *image) {
    return (uint64_t)image->block_size * 2u + ((uint64_t)1u << image->align);
}

static bool ciso_open(NkDiscSource *source, NkDiscFormat format, NkCisoImage *image,
                      NkDiscExpandStatus *status) {
    uint8_t header[NK_CISO_HEADER_BYTES];
    uint32_t header_size;
    uint32_t raw_version;
    uint64_t index_bytes;
    uint64_t data_start;
    const char *name = format == NK_DISC_FORMAT_CSO ? "CSO" : "ZSO";
    memset(image, 0, sizeof(*image));
    image->format = format;
    if (source->size < NK_CISO_HEADER_BYTES) {
        return disc_fail(status, NK_DISC_TRUNCATED,
                         "This %s disc image is shorter than its header.", name);
    }
    if (!source_read_at(source, 0, header, sizeof(header), status)) return false;
    if (nk_disc_format_from_magic(header, sizeof(header)) != format) {
        return disc_fail(status, NK_DISC_HEADER_INVALID, "This is not a %s disc image.", name);
    }
    header_size = read_le32(header + 4);
    image->total_bytes = read_le64(header + 8);
    image->block_size = read_le32(header + 16);
    raw_version = header[20];
    image->align = header[21];

    if (format == NK_DISC_FORMAT_CSO && raw_version <= 1u) {
        image->version = 1u;
    } else if (format == NK_DISC_FORMAT_CSO && raw_version == 2u) {
        image->version = 2u;
    } else if (format == NK_DISC_FORMAT_ZSO && raw_version <= 1u) {
        image->version = 1u;
    } else {
        return disc_fail(status, NK_DISC_VERSION_UNSUPPORTED,
                         "This %s disc image is format version %u, which Nakagawa can't read.",
                         name, (unsigned)raw_version);
    }
    /* v2 fixes the header size; older writers left it 0 or wrote 24. */
    if (header_size != NK_CISO_HEADER_BYTES &&
        !(image->version == 1u && header_size == 0u)) {
        return disc_fail(status, NK_DISC_HEADER_INVALID,
                         "This %s disc image's header names a size of %u bytes, not %u.",
                         name, (unsigned)header_size, (unsigned)NK_CISO_HEADER_BYTES);
    }
    if (image->block_size < NK_CISO_MIN_BLOCK || image->block_size > NK_CISO_MAX_BLOCK ||
        (image->block_size & (image->block_size - 1u)) != 0u) {
        return disc_fail(status, NK_DISC_HEADER_INVALID,
                         "This %s disc image's block size (%u bytes) is not a power of two "
                         "between 2 KiB and 1 MiB.", name, (unsigned)image->block_size);
    }
    if (image->align > NK_CISO_MAX_ALIGN) {
        return disc_fail(status, NK_DISC_HEADER_INVALID,
                         "This %s disc image's index alignment (%u) is out of range.", name,
                         (unsigned)image->align);
    }
    if (image->total_bytes == 0u || image->total_bytes > NK_DISC_MAX_EXPANDED_BYTES ||
        image->total_bytes % NK_DISC_SECTOR_BYTES != 0u) {
        return disc_fail(status, NK_DISC_SIZE_INVALID,
                         "This %s disc image's header names an expanded size that no PSP disc "
                         "has.", name);
    }
    /* Bounded by the size check: at most 2^21 blocks of 2 KiB. */
    image->block_count = (uint32_t)((image->total_bytes + image->block_size - 1u) /
                                    image->block_size);
    index_bytes = ((uint64_t)image->block_count + 1u) * 4u;
    data_start = NK_CISO_HEADER_BYTES + index_bytes;
    if (data_start > source->size) {
        return disc_fail(status, NK_DISC_TRUNCATED,
                         "This %s disc image ends inside its block index.", name);
    }
    image->index = (uint32_t *)malloc((size_t)index_bytes);
    if (!image->index) return disc_fail(status, NK_DISC_INTERNAL, "Out of memory.");
    {
        uint8_t *raw = (uint8_t *)image->index;
        if (!source_read_at(source, NK_CISO_HEADER_BYTES, raw, (size_t)index_bytes, status)) {
            free(image->index);
            image->index = NULL;
            return false;
        }
        for (uint32_t i = 0; i <= image->block_count; i++) {
            image->index[i] = read_le32(raw + (size_t)i * 4u);
        }
    }
    for (uint32_t i = 0; i < image->block_count; i++) {
        uint64_t start = ciso_offset(image, i);
        uint64_t end = ciso_offset(image, i + 1u);
        if ((i == 0u && start < data_start) || end < start || end > source->size ||
            end - start > ciso_max_block_span(image)) {
            free(image->index);
            image->index = NULL;
            return disc_fail(status, NK_DISC_INDEX_INVALID,
                             "Block %u of this %s disc image points outside the file.",
                             (unsigned)i, name);
        }
    }
    return true;
}

static void ciso_close(NkCisoImage *image) {
    free(image->index);
    image->index = NULL;
}

static bool ciso_expand(NkDiscSource *source, NkCisoImage *image, NkDiscSink *sink,
                        NkDiscExpandStatus *status) {
    size_t span_cap = (size_t)ciso_max_block_span(image);
    uint8_t *packed = (uint8_t *)malloc(span_cap);
    uint8_t *plain = (uint8_t *)malloc(image->block_size);
    bool ok = true;
    if (!packed || !plain) {
        free(packed);
        free(plain);
        return disc_fail(status, NK_DISC_INTERNAL, "Out of memory.");
    }
    for (uint32_t i = 0; ok && i < image->block_count; i++) {
        uint64_t start = ciso_offset(image, i);
        size_t span = (size_t)(ciso_offset(image, i + 1u) - start);
        uint64_t remaining = image->total_bytes - (uint64_t)i * image->block_size;
        size_t want = remaining < image->block_size ? (size_t)remaining : image->block_size;
        bool flag = (image->index[i] & NK_CISO_PLAIN_FLAG) != 0u;
        bool stored;
        bool lz4;
        if (image->format == NK_DISC_FORMAT_CSO && image->version == 2u) {
            /* v2: a block as large as a whole block is stored; the flag picks
               LZ4 over DEFLATE for the others. */
            stored = span >= image->block_size;
            lz4 = !stored && flag;
        } else {
            stored = flag;
            lz4 = image->format == NK_DISC_FORMAT_ZSO;
        }
        if (!source_read_at(source, start, packed, span, status)) {
            ok = false;
            break;
        }
        if (stored) {
            if (span < want) {
                ok = disc_fail(status, NK_DISC_BLOCK_CORRUPT,
                               "Stored block %u of this disc image is shorter than a block.",
                               (unsigned)i);
                break;
            }
            ok = sink_write(sink, packed, want, status);
        } else if (lz4) {
            size_t produced = 0;
            const char *why = "";
            if (!lz4_block_decode(packed, span, plain, image->block_size, want, &produced, &why)) {
                ok = disc_fail(status, NK_DISC_BLOCK_CORRUPT,
                               "Block %u of this disc image is damaged: %s.", (unsigned)i, why);
                break;
            }
            ok = sink_write(sink, plain, want, status);
        } else {
            uint8_t *inflated = NULL;
            size_t inflated_len = 0;
            char why[128] = "";
            int rc = nk_psp_inflate_raw(packed, span, &inflated, &inflated_len, image->block_size,
                                        image->block_size, why, sizeof(why));
            /* A short last block may be padded to a whole block. */
            if (rc != NK_INFLATE_OK || inflated_len < want ||
                (inflated_len != want && inflated_len != image->block_size)) {
                free(inflated);
                ok = disc_fail(status, NK_DISC_BLOCK_CORRUPT,
                               "Block %u of this disc image is damaged: %s.", (unsigned)i,
                               rc != NK_INFLATE_OK ? why : "it holds the wrong amount of data");
                break;
            }
            ok = sink_write(sink, inflated, want, status);
            free(inflated);
        }
    }
    free(packed);
    free(plain);
    return ok;
}

/* ---- CHD (through the registered backend) ------------------------------ */

static bool chd_open_checked(const char *path, void **handle, uint64_t *logical_bytes,
                             uint32_t *hunk_bytes, NkDiscExpandStatus *status) {
    if (!s_chd_backend) {
        return disc_fail(status, NK_DISC_CHD_UNAVAILABLE,
                         "This build of Nakagawa can't read CHD disc images.");
    }
    if (!s_chd_backend->open(path, handle, logical_bytes, hunk_bytes, status)) {
        if (status && !status->code[0]) {
            disc_fail(status, NK_DISC_CHD_INVALID, "This CHD disc image could not be read.");
        }
        return false;
    }
    if (*logical_bytes == 0u || *logical_bytes > NK_DISC_MAX_EXPANDED_BYTES ||
        *logical_bytes % NK_DISC_SECTOR_BYTES != 0u) {
        s_chd_backend->close(*handle);
        *handle = NULL;
        return disc_fail(status, NK_DISC_SIZE_INVALID,
                         "This CHD disc image names an expanded size that no PSP disc has.");
    }
    if (*hunk_bytes < NK_DISC_SECTOR_BYTES || *hunk_bytes > NK_CHD_MAX_HUNK ||
        *hunk_bytes % NK_DISC_SECTOR_BYTES != 0u) {
        s_chd_backend->close(*handle);
        *handle = NULL;
        return disc_fail(status, NK_DISC_CHD_UNSUPPORTED,
                         "This CHD disc image uses a block size (%u bytes) that is not a "
                         "whole number of 2048-byte sectors.", (unsigned)*hunk_bytes);
    }
    return true;
}

static bool chd_expand(void *handle, uint64_t logical_bytes, uint32_t hunk_bytes,
                       NkDiscSink *sink, NkDiscExpandStatus *status) {
    uint8_t *hunk = (uint8_t *)malloc(hunk_bytes);
    uint64_t hunks = (logical_bytes + hunk_bytes - 1u) / hunk_bytes;
    bool ok = true;
    if (!hunk) return disc_fail(status, NK_DISC_INTERNAL, "Out of memory.");
    for (uint64_t i = 0; ok && i < hunks; i++) {
        uint64_t remaining = logical_bytes - i * hunk_bytes;
        size_t want = remaining < hunk_bytes ? (size_t)remaining : hunk_bytes;
        if (!s_chd_backend->read_hunk(handle, (uint32_t)i, hunk, status)) {
            if (status && !status->code[0]) {
                disc_fail(status, NK_DISC_BLOCK_CORRUPT,
                          "Block %u of this CHD disc image is damaged.", (unsigned)i);
            }
            ok = false;
            break;
        }
        ok = sink_write(sink, hunk, want, status);
    }
    free(hunk);
    return ok;
}

/* ---- expansion --------------------------------------------------------- */

bool nk_disc_expanded_size(const char *source_path, NkDiscFormat *out_format,
                           uint64_t *out_expanded_bytes, NkDiscExpandStatus *status) {
    NkDiscFormat format;
    disc_status_clear(status);
    if (out_format) *out_format = NK_DISC_FORMAT_NONE;
    if (out_expanded_bytes) *out_expanded_bytes = 0;
    if (!source_path || !source_path[0]) {
        return disc_fail(status, NK_DISC_INTERNAL, "No disc image was named.");
    }
    format = nk_disc_image_format(source_path);
    if (out_format) *out_format = format;
    if (format == NK_DISC_FORMAT_CSO || format == NK_DISC_FORMAT_ZSO) {
        NkDiscSource source;
        NkCisoImage image;
        bool ok;
        if (!source_open(&source, source_path, status)) return false;
        ok = ciso_open(&source, format, &image, status);
        source_close(&source);
        if (!ok) return false;
        if (out_expanded_bytes) *out_expanded_bytes = image.total_bytes;
        ciso_close(&image);
        return true;
    }
    if (format == NK_DISC_FORMAT_CHD) {
        void *handle = NULL;
        uint64_t logical = 0;
        uint32_t hunk = 0;
        if (!chd_open_checked(source_path, &handle, &logical, &hunk, status)) return false;
        s_chd_backend->close(handle);
        if (out_expanded_bytes) *out_expanded_bytes = logical;
        return true;
    }
    return disc_fail(status, NK_DISC_NOT_COMPRESSED,
                     "This file is not a compressed disc image (.cso, .zso or .chd).");
}

static bool path_join(char *out, size_t out_size, const char *dir, const char *name) {
    int written = snprintf(out, out_size, "%s%c%s", dir, nk_platform_path_separator(), name);
    return written > 0 && (size_t)written < out_size;
}

static bool path_suffix(char *out, size_t out_size, const char *path, const char *suffix) {
    int written = snprintf(out, out_size, "%s%s", path, suffix);
    return written > 0 && (size_t)written < out_size;
}

bool nk_disc_expand_file(const char *source_path, const char *dest_iso,
                         const NkDiscExpandCallbacks *callbacks,
                         uint64_t *out_expanded_bytes, NkDiscExpandStatus *status) {
    char part[NK_MAX_PATH + 8];
    NkDiscFormat format;
    NkDiscSink sink;
    NkDiscSource source;
    NkCisoImage image;
    void *chd = NULL;
    uint64_t total = 0;
    uint32_t hunk = 0;
    bool ok;
    disc_status_clear(status);
    if (out_expanded_bytes) *out_expanded_bytes = 0;
    memset(&source, 0, sizeof(source));
    memset(&image, 0, sizeof(image));
    if (!source_path || !dest_iso || !dest_iso[0]) {
        return disc_fail(status, NK_DISC_INTERNAL, "No disc image was named.");
    }
    if (!path_suffix(part, sizeof(part), dest_iso, ".part")) {
        return disc_fail(status, NK_DISC_WRITE_FAILED,
                         "The path for the expanded disc image is too long.");
    }
    format = nk_disc_image_format(source_path);
    if (format == NK_DISC_FORMAT_CSO || format == NK_DISC_FORMAT_ZSO) {
        if (!source_open(&source, source_path, status)) return false;
        if (!ciso_open(&source, format, &image, status)) {
            source_close(&source);
            return false;
        }
        total = image.total_bytes;
    } else if (format == NK_DISC_FORMAT_CHD) {
        if (!chd_open_checked(source_path, &chd, &total, &hunk, status)) return false;
    } else {
        return disc_fail(status, NK_DISC_NOT_COMPRESSED,
                         "This file is not a compressed disc image (.cso, .zso or .chd).");
    }

    memset(&sink, 0, sizeof(sink));
    sink.total = total;
    sink.callbacks = callbacks;
    sink.f = nk_platform_fopen_private(part, "wb");
    if (!sink.f) {
        ok = disc_fail(status, NK_DISC_WRITE_FAILED,
                       "The expanded disc image could not be created in Nakagawa's data "
                       "folder.");
    } else {
        setvbuf(sink.f, NULL, _IOFBF, NK_DISC_IO_BUFFER);
        if (callbacks && callbacks->on_progress) callbacks->on_progress(0, total, callbacks->user);
        ok = format == NK_DISC_FORMAT_CHD ? chd_expand(chd, total, hunk, &sink, status)
                                          : ciso_expand(&source, &image, &sink, status);
        if (fclose(sink.f) != 0 && ok) {
            ok = disc_fail(status, NK_DISC_WRITE_FAILED,
                           "The expanded disc image could not be written. Check that the drive "
                           "has enough free space.");
        }
        sink.f = NULL;
    }
    if (format == NK_DISC_FORMAT_CHD) {
        s_chd_backend->close(chd);
    } else {
        ciso_close(&image);
        source_close(&source);
    }
    if (ok && sink.written != total) {
        ok = disc_fail(status, NK_DISC_INTERNAL, "The expansion ended early.");
    }
    if (ok && nk_platform_get_file_size(part) != (int64_t)total) {
        ok = disc_fail(status, NK_DISC_WRITE_FAILED,
                       "The expanded disc image on the drive is not the expected size.");
    }
    if (ok && nk_rename_utf8(part, dest_iso) != 0) {
        ok = disc_fail(status, NK_DISC_WRITE_FAILED,
                       "The expanded disc image could not be moved into place.");
    }
    if (!ok) {
        (void)nk_remove_utf8(part);
        return false;
    }
    if (out_expanded_bytes) *out_expanded_bytes = total;
    return true;
}

/* ---- import: fingerprint, reuse and the per-title folder ---------------- */

static bool file_modified_time(const char *path, uint64_t *out) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[32768];
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768) <= 0 ||
        !GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
        return false;
    }
    *out = ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) |
           data.ftLastWriteTime.dwLowDateTime;
    return true;
#else
    struct stat data;
    if (stat(path, &data) != 0) return false;
    *out = (uint64_t)data.st_mtim.tv_sec * 1000000000ull + (uint64_t)data.st_mtim.tv_nsec;
    return true;
#endif
}

static void put_le64(uint8_t *p, uint64_t value) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static bool disc_fingerprint(const char *path, NkDiscFormat format, uint64_t size,
                             char out[NK_DISC_FINGERPRINT_CHARS + 1],
                             NkDiscExpandStatus *status) {
    static const char hex[] = "0123456789abcdef";
    static const char domain[] = "nk-disc-fingerprint/v1";
    uint8_t fields[17];
    uint8_t digest[20];
    uint64_t modified = 0;
    size_t head_len = size < NK_DISC_FINGERPRINT_HEAD ? (size_t)size : NK_DISC_FINGERPRINT_HEAD;
    uint8_t *head;
    SHA_CTX ctx;
    FILE *f;
    if (!file_modified_time(path, &modified)) {
        return disc_fail(status, NK_DISC_READ_FAILED,
                         "The disc image's modification time could not be read.");
    }
    head = (uint8_t *)malloc(head_len ? head_len : 1u);
    if (!head) return disc_fail(status, NK_DISC_INTERNAL, "Out of memory.");
    f = nk_fopen_utf8(path, "rb");
    if (!f || fread(head, 1, head_len, f) != head_len) {
        if (f) fclose(f);
        free(head);
        return disc_fail(status, NK_DISC_READ_FAILED, "The disc image could not be read.");
    }
    fclose(f);
    fields[0] = (uint8_t)format;
    put_le64(fields + 1, size);
    put_le64(fields + 9, modified);
    SHAInit(&ctx);
    SHAUpdate(&ctx, (BYTE *)domain, (int)(sizeof(domain) - 1u));
    SHAUpdate(&ctx, fields, (int)sizeof(fields));
    SHAUpdate(&ctx, head, (int)head_len);
    SHAFinal(digest, &ctx);
    free(head);
    for (unsigned i = 0; i < NK_DISC_FINGERPRINT_CHARS / 2u; i++) {
        out[i * 2u] = hex[digest[i] >> 4];
        out[i * 2u + 1u] = hex[digest[i] & 15u];
    }
    out[NK_DISC_FINGERPRINT_CHARS] = '\0';
    return true;
}

/* Four capital letters and five digits: the only form used as a folder name. */
static bool disc_id_valid(const char *id) {
    if (!id || strlen(id) != 9u) return false;
    for (size_t i = 0; i < 9u; i++) {
        unsigned char c = (unsigned char)id[i];
        if (i < 4u ? !(c >= 'A' && c <= 'Z') : !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

/* One short line, written beside its final name and renamed into place. */
static bool write_line_file(const char *path, const char *line) {
    char tmp[NK_MAX_PATH + 8];
    FILE *f;
    bool ok;
    if (!path_suffix(tmp, sizeof(tmp), path, ".tmp")) return false;
    f = nk_platform_fopen_private(tmp, "wb");
    if (!f) return false;
    ok = fprintf(f, "%s\n", line) > 0;
    ok = (fclose(f) == 0) && ok;
    if (ok) ok = nk_rename_utf8(tmp, path) == 0;
    if (!ok) (void)nk_remove_utf8(tmp);
    return ok;
}

static bool read_line_file(const char *path, char *out, size_t out_size) {
    FILE *f = nk_fopen_utf8(path, "rb");
    size_t got;
    if (!f) return false;
    got = fread(out, 1, out_size - 1u, f);
    fclose(f);
    out[got] = '\0';
    while (got > 0 && (out[got - 1u] == '\n' || out[got - 1u] == '\r')) out[--got] = '\0';
    return got > 0;
}

typedef struct {
    char expanded_dir[NK_MAX_PATH];
    char ref[NK_MAX_PATH];
    char staging[NK_MAX_PATH];
} NkDiscImportPaths;

static bool import_paths(const char *data_root, const char *fingerprint, NkDiscImportPaths *paths) {
    char discs[NK_MAX_PATH];
    char name[NK_DISC_FINGERPRINT_CHARS + 8];
    if (!path_join(discs, sizeof(discs), data_root, "discs") ||
        !path_join(paths->expanded_dir, sizeof(paths->expanded_dir), discs, "expanded")) {
        return false;
    }
    snprintf(name, sizeof(name), "%s.ref", fingerprint);
    if (!path_join(paths->ref, sizeof(paths->ref), paths->expanded_dir, name)) return false;
    snprintf(name, sizeof(name), "%s.iso", fingerprint);
    return path_join(paths->staging, sizeof(paths->staging), paths->expanded_dir, name);
}

/* <data>/discs/<ID>/<ID>.<ext>.iso and its directory. */
static bool title_iso_path(const char *data_root, const char *disc_id, NkDiscFormat format,
                           char *dir, size_t dir_size, char *iso, size_t iso_size) {
    char discs[NK_MAX_PATH];
    char name[32];
    if (!path_join(discs, sizeof(discs), data_root, "discs") ||
        !path_join(dir, dir_size, discs, disc_id)) {
        return false;
    }
    snprintf(name, sizeof(name), "%s.%s.iso", disc_id, nk_disc_format_extension(format));
    return path_join(iso, iso_size, dir, name);
}

bool nk_disc_import_probe(const char *source, const char *data_root, NkDiscImport *out,
                          NkDiscExpandStatus *status) {
    NkDiscImportPaths paths;
    char disc_id[32];
    char dir[NK_MAX_PATH];
    char sidecar[NK_MAX_PATH + 8];
    char recorded[NK_DISC_FINGERPRINT_CHARS + 8];
    int64_t size;
    disc_status_clear(status);
    if (!out) return disc_fail(status, NK_DISC_INTERNAL, "No result was given.");
    memset(out, 0, sizeof(*out));
    if (!source || !source[0] || !data_root || !data_root[0]) {
        return disc_fail(status, NK_DISC_INTERNAL, "No disc image or data folder was named.");
    }
    size = nk_platform_get_file_size(source);
    if (size < 0) {
        return disc_fail(status, NK_DISC_READ_FAILED, "The disc image could not be opened.");
    }
    out->source_bytes = (uint64_t)size;
    if (!nk_disc_expanded_size(source, &out->format, &out->expanded_bytes, status)) return false;
    if (!disc_fingerprint(source, out->format, out->source_bytes, out->fingerprint, status)) {
        return false;
    }
    if (!import_paths(data_root, out->fingerprint, &paths)) {
        return disc_fail(status, NK_DISC_WRITE_FAILED, "The path to Nakagawa's data folder is too long.");
    }
    /* Reuse only an expansion this exact file produced: the reference names
       the disc, the per-title sidecar names the same fingerprint, and the
       image on the drive has the size the header promises. */
    if (!read_line_file(paths.ref, disc_id, sizeof(disc_id)) || !disc_id_valid(disc_id) ||
        !title_iso_path(data_root, disc_id, out->format, dir, sizeof(dir), out->iso_path,
                        sizeof(out->iso_path)) ||
        !path_suffix(sidecar, sizeof(sidecar), out->iso_path, ".source") ||
        !read_line_file(sidecar, recorded, sizeof(recorded)) ||
        strcmp(recorded, out->fingerprint) != 0 ||
        nk_platform_get_file_size(out->iso_path) != (int64_t)out->expanded_bytes) {
        out->iso_path[0] = '\0';
        return true;
    }
    snprintf(out->disc_id, sizeof(out->disc_id), "%.9s", disc_id);
    out->reused = true;
    return true;
}

bool nk_disc_import(const char *source, const char *data_root,
                    const NkDiscExpandCallbacks *callbacks, NkDiscImport *out,
                    NkDiscExpandStatus *status) {
    NkDiscImportPaths paths;
    NkIsoMetadata meta;
    char dir[NK_MAX_PATH];
    char sidecar[NK_MAX_PATH + 8];
    uint64_t written = 0;
    if (!nk_disc_import_probe(source, data_root, out, status)) return false;
    if (out->reused) return true;
    if (!import_paths(data_root, out->fingerprint, &paths)) {
        return disc_fail(status, NK_DISC_WRITE_FAILED, "The path to Nakagawa's data folder is too long.");
    }
    if (!nk_platform_mkdir_p_private(paths.expanded_dir)) {
        return disc_fail(status, NK_DISC_WRITE_FAILED,
                         "Nakagawa's disc folder could not be made in its data folder.");
    }
    if (!nk_disc_expand_file(source, paths.staging, callbacks, &written, status)) return false;

    /* The expansion is a disc image now: name its disc before it gets a
       per-title folder, and refuse anything that is not a PSP game. */
    memset(&meta, 0, sizeof(meta));
    if (nk_iso_inspect(paths.staging, &meta) != NK_OK) {
        (void)nk_remove_utf8(paths.staging);
        return disc_fail(status, NK_DISC_EXPANDED_INVALID,
                         "The compressed file expanded, but what it holds is not a readable "
                         "disc image: %.200s", meta.error_message);
    }
    if (!meta.has_psp_game_folder) {
        (void)nk_remove_utf8(paths.staging);
        return disc_fail(status, NK_DISC_NOT_PSP,
                         "This disc image isn't a PSP game: it has no PSP_GAME folder. "
                         "Choose the disc image of a PSP game disc.");
    }
    if (!disc_id_valid(meta.disc_id)) {
        (void)nk_remove_utf8(paths.staging);
        return disc_fail(status, NK_DISC_ID_MISSING,
                         "This PSP disc image does not name a valid disc ID, so Nakagawa can't "
                         "keep it apart from other games.");
    }
    snprintf(out->disc_id, sizeof(out->disc_id), "%.9s", meta.disc_id);
    if (!title_iso_path(data_root, out->disc_id, out->format, dir, sizeof(dir), out->iso_path,
                        sizeof(out->iso_path)) ||
        !path_suffix(sidecar, sizeof(sidecar), out->iso_path, ".source") ||
        !nk_platform_mkdir_p_private(dir)) {
        (void)nk_remove_utf8(paths.staging);
        out->iso_path[0] = '\0';
        return disc_fail(status, NK_DISC_WRITE_FAILED,
                         "The disc image's folder could not be made in Nakagawa's data folder.");
    }
    /* The old sidecar goes first, so an interrupted replace never leaves an
       earlier source's fingerprint beside this image. */
    (void)nk_remove_utf8(sidecar);
    if (nk_rename_utf8(paths.staging, out->iso_path) != 0 ||
        !write_line_file(sidecar, out->fingerprint) ||
        !write_line_file(paths.ref, out->disc_id)) {
        (void)nk_remove_utf8(paths.staging);
        out->iso_path[0] = '\0';
        return disc_fail(status, NK_DISC_WRITE_FAILED,
                         "The expanded disc image could not be moved into its folder.");
    }
    out->expanded_bytes = written;
    return true;
}
