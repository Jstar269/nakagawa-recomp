// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* nkstate v1 container (#2146, #2147). The format and the rules are in nkstate.h. */

#include "nkstate.h"
#include "vfpu_tables.h" /* sr_vfpu_sha256: the runtime's SHA-256 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const s_status_names[] = {
    [NKSTATE_OK] = "ok",
    [NKSTATE_BAD_MAGIC] = "bad-magic",
    [NKSTATE_FORMAT_VERSION_MISMATCH] = "format-version",
    [NKSTATE_KIND_MISMATCH] = "kind",
    [NKSTATE_ABI_VERSION] = "abi-version",
    [NKSTATE_CPUSTATE_SIZE] = "cpustate-size",
    [NKSTATE_EXECUTABLE_IDENTITY] = "executable-identity",
    [NKSTATE_RUNTIME_IDENTITY] = "runtime-identity",
    [NKSTATE_DISC_ID] = "disc-id",
    [NKSTATE_FLAGS] = "flags",
    [NKSTATE_CLOCK_NOT_DETERMINISTIC] = "clock-not-deterministic",
    [NKSTATE_TRUNCATED] = "truncated",
    [NKSTATE_TOO_MANY_SEGMENTS] = "too-many-segments",
    [NKSTATE_BAD_TAG] = "bad-tag",
    [NKSTATE_DUPLICATE_SEGMENT] = "duplicate-segment",
    [NKSTATE_UNKNOWN_SEGMENT] = "unknown-segment",
    [NKSTATE_MISSING_SEGMENT] = "missing-segment",
    [NKSTATE_SEGMENT_VERSION] = "segment-version",
    [NKSTATE_TRAILING_BYTES] = "trailing-bytes",
    [NKSTATE_SEGMENT_HASH] = "segment-hash",
    [NKSTATE_FILE_HASH] = "file-hash",
    [NKSTATE_SEGMENT_PAYLOAD] = "segment-payload",
    [NKSTATE_RESTORE_VERIFY] = "restore-verify",
    [NKSTATE_NO_RUNTIME_IDENTITY] = "no-runtime-identity",
    [NKSTATE_MISSING_EXPORTER] = "missing-exporter",
    [NKSTATE_NOT_QUIET] = "not-quiet",
    [NKSTATE_IO_ERROR] = "io-error",
    [NKSTATE_NO_MEMORY] = "out-of-memory",
};

const char *nkstate_status_name(NkStateStatus status) {
    if ((unsigned)status < sizeof s_status_names / sizeof s_status_names[0] && s_status_names[status])
        return s_status_names[status];
    return "unknown-status";
}

static void set_detail(char *detail, size_t cap, const char *fmt, ...) {
    if (!detail || cap == 0u) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, cap, fmt, ap);
    va_end(ap);
}

static NkStateStatus refuse(NkStateStatus status, char *detail, size_t cap, const char *fmt, ...) {
    if (detail && cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, cap, fmt, ap);
        va_end(ap);
    }
    return status;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* ---- buffer ---------------------------------------------------------------------- */

int nkstate_buf_put(NkStateBuf *buf, const void *bytes, size_t len) {
    if (len == 0u) return 1;
    if (len > SIZE_MAX - buf->len) return 0;
    if (buf->len + len > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 256u;
        while (cap < buf->len + len) {
            if (cap > SIZE_MAX / 2u) { cap = buf->len + len; break; }
            cap *= 2u;
        }
        uint8_t *grown = (uint8_t *)realloc(buf->data, cap);
        if (!grown) return 0;
        buf->data = grown;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, bytes, len);
    buf->len += len;
    return 1;
}

int nkstate_buf_put_u32(NkStateBuf *buf, uint32_t value) {
    uint8_t b[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    return nkstate_buf_put(buf, b, sizeof b);
}

int nkstate_buf_put_u64(NkStateBuf *buf, uint64_t value) {
    return nkstate_buf_put_u32(buf, (uint32_t)value) && nkstate_buf_put_u32(buf, (uint32_t)(value >> 32));
}

void nkstate_buf_free(NkStateBuf *buf) {
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = buf->cap = 0u;
}

/* ---- identity -------------------------------------------------------------------- */

static int tag_valid(const char *tag) {
    for (unsigned i = 0; i < NKSTATE_TAG_LEN; i++)
        if ((unsigned char)tag[i] < 0x20u || (unsigned char)tag[i] > 0x7Eu) return 0;
    return 1;
}

static size_t fixed_strlen(const char *s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

static void hex32(const uint8_t bytes[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32u; i++) {
        out[i * 2u] = digits[bytes[i] >> 4];
        out[i * 2u + 1u] = digits[bytes[i] & 15u];
    }
    out[64] = '\0';
}

/* ---- capture --------------------------------------------------------------------- */

static const NkStateExporter *find_exporter(const NkStateExporter *exporters, size_t count, const char *tag) {
    for (size_t i = 0; i < count; i++)
        if (memcmp(exporters[i].tag, tag, NKSTATE_TAG_LEN) == 0) return &exporters[i];
    return NULL;
}

NkStateStatus nkstate_capture(const NkStateIdentity *self,
                              const NkStateRequirement *required, size_t required_count,
                              const NkStateExporter *exporters, size_t exporter_count,
                              NkStateBuf *out, char *detail, size_t detail_cap) {
    NkStateBuf payloads[NKSTATE_MAX_SEGMENTS];
    const NkStateExporter *order[NKSTATE_MAX_SEGMENTS];
    size_t n = 0;
    NkStateStatus status = NKSTATE_OK;

    memset(out, 0, sizeof *out);
    if (!self || fixed_strlen(self->runtime, NKSTATE_RUNTIME_ID_LEN) == 0u)
        return refuse(NKSTATE_NO_RUNTIME_IDENTITY, detail, detail_cap,
                      "this build carries no SR_BUILD_ID, so nothing could tell its states from another build's");
    if ((self->flags & ~NKSTATE_FLAGS_KNOWN) != 0u)
        return refuse(NKSTATE_FLAGS, detail, detail_cap, "flags 0x%x has unknown bits", self->flags);
    if (exporter_count > NKSTATE_MAX_SEGMENTS)
        return refuse(NKSTATE_TOO_MANY_SEGMENTS, detail, detail_cap, "%zu exporters, at most %u",
                      exporter_count, NKSTATE_MAX_SEGMENTS);
    for (size_t i = 0; i < exporter_count; i++) {
        if (!tag_valid(exporters[i].tag))
            return refuse(NKSTATE_BAD_TAG, detail, detail_cap, "exporter %zu has a tag that is not printable ASCII", i);
        for (size_t j = 0; j < i; j++)
            if (memcmp(exporters[i].tag, exporters[j].tag, NKSTATE_TAG_LEN) == 0)
                return refuse(NKSTATE_DUPLICATE_SEGMENT, detail, detail_cap, "two exporters use tag '%.4s'",
                              exporters[i].tag);
    }
    /* Every requirement first: a capture missing one is refused before any save runs. */
    for (size_t i = 0; i < required_count; i++) {
        const NkStateExporter *e = find_exporter(exporters, exporter_count, required[i].tag);
        if (!e || !e->save || !e->load)
            return refuse(NKSTATE_MISSING_EXPORTER, detail, detail_cap,
                          "subsystem '%s' (segment '%.4s') has no %s, so a state would miss it",
                          required[i].name ? required[i].name : "?", required[i].tag,
                          !e ? "exporter" : (!e->save ? "save function" : "load function"));
    }
    for (size_t i = 0; i < exporter_count; i++) {
        if (!exporters[i].save || !exporters[i].load)
            return refuse(NKSTATE_MISSING_EXPORTER, detail, detail_cap,
                          "subsystem '%s' (segment '%.4s') has no %s function",
                          exporters[i].name ? exporters[i].name : "?", exporters[i].tag,
                          !exporters[i].save ? "save" : "load");
        order[n++] = &exporters[i];
    }

    memset(payloads, 0, sizeof payloads);
    for (size_t i = 0; i < n; i++) {
        char sub[192] = {0};
        status = order[i]->save(order[i]->ctx, &payloads[i], sub, sizeof sub);
        if (status != NKSTATE_OK) {
            set_detail(detail, detail_cap, "subsystem '%s' (segment '%.4s'): %s",
                       order[i]->name ? order[i]->name : "?", order[i]->tag, sub[0] ? sub : nkstate_status_name(status));
            goto done;
        }
    }

    {
        NkStateBuf h = {0};
        int ok = nkstate_buf_put(&h, NKSTATE_MAGIC, NKSTATE_MAGIC_LEN) &&
                 nkstate_buf_put_u32(&h, NKSTATE_FORMAT_VERSION) &&
                 nkstate_buf_put_u32(&h, self->kind) &&
                 nkstate_buf_put_u32(&h, SR_CPUSTATE_ABI_VERSION) &&
                 nkstate_buf_put_u32(&h, (uint32_t)sizeof(CpuState)) &&
                 nkstate_buf_put(&h, self->executable, 32u) &&
                 nkstate_buf_put(&h, self->runtime, NKSTATE_RUNTIME_ID_LEN) &&
                 nkstate_buf_put(&h, self->disc_id, NKSTATE_DISC_ID_LEN) &&
                 nkstate_buf_put_u32(&h, self->flags) &&
                 nkstate_buf_put_u32(&h, (uint32_t)n);
        for (size_t i = 0; ok && i < n; i++) {
            uint8_t digest[32];
            sr_vfpu_sha256(payloads[i].data ? payloads[i].data : (const uint8_t *)"", payloads[i].len, digest);
            ok = nkstate_buf_put(&h, order[i]->tag, NKSTATE_TAG_LEN) &&
                 nkstate_buf_put_u32(&h, order[i]->version) &&
                 nkstate_buf_put_u64(&h, (uint64_t)payloads[i].len) &&
                 nkstate_buf_put(&h, digest, 32u);
        }
        for (size_t i = 0; ok && i < n; i++) ok = nkstate_buf_put(&h, payloads[i].data, payloads[i].len);
        if (ok) {
            uint8_t digest[32];
            sr_vfpu_sha256(h.data, h.len, digest);
            ok = nkstate_buf_put(&h, digest, 32u);
        }
        if (!ok) {
            nkstate_buf_free(&h);
            status = refuse(NKSTATE_NO_MEMORY, detail, detail_cap, "building the file image");
            goto done;
        }
        *out = h;
    }

done:
    for (size_t i = 0; i < n; i++) nkstate_buf_free(&payloads[i]);
    return status;
}

/* ---- parse ----------------------------------------------------------------------- */

void nkstate_file_free(NkStateFile *file) {
    if (!file) return;
    free(file->storage);
    memset(file, 0, sizeof *file);
}

NkStateStatus nkstate_parse(const uint8_t *data, size_t len, const NkStateExpect *expect,
                            NkStateFile *out, char *detail, size_t detail_cap) {
    NkStateFile f;
    memset(&f, 0, sizeof f);
    memset(out, 0, sizeof *out);
    if (detail && detail_cap) detail[0] = '\0';

    if (len < NKSTATE_MAGIC_LEN) {
        if (memcmp(data, NKSTATE_MAGIC, len) != 0)
            return refuse(NKSTATE_BAD_MAGIC, detail, detail_cap, "not an nkstate file");
        return refuse(NKSTATE_TRUNCATED, detail, detail_cap, "%zu bytes, shorter than the magic", len);
    }
    if (memcmp(data, NKSTATE_MAGIC, NKSTATE_MAGIC_LEN) != 0)
        return refuse(NKSTATE_BAD_MAGIC, detail, detail_cap, "not an nkstate file");
    if (len < NKSTATE_HEADER_SIZE)
        return refuse(NKSTATE_TRUNCATED, detail, detail_cap, "%zu bytes, shorter than the %u-byte header",
                      len, NKSTATE_HEADER_SIZE);

    const uint8_t *p = data + NKSTATE_MAGIC_LEN;
    const uint32_t format = rd32(p); p += 4;
    f.identity.kind = rd32(p); p += 4;
    f.abi_version = rd32(p); p += 4;
    f.cpustate_size = rd32(p); p += 4;
    memcpy(f.identity.executable, p, 32u); p += 32;
    memcpy(f.identity.runtime, p, NKSTATE_RUNTIME_ID_LEN); p += NKSTATE_RUNTIME_ID_LEN;
    memcpy(f.identity.disc_id, p, NKSTATE_DISC_ID_LEN); p += NKSTATE_DISC_ID_LEN;
    f.identity.flags = rd32(p); p += 4;
    f.segment_count = rd32(p); p += 4;

    const NkStateIdentity *self = expect->self;
    if (format != NKSTATE_FORMAT_VERSION)
        return refuse(NKSTATE_FORMAT_VERSION_MISMATCH, detail, detail_cap, "file %u, this build reads %u",
                      format, NKSTATE_FORMAT_VERSION);
    if (f.identity.kind != self->kind)
        return refuse(NKSTATE_KIND_MISMATCH, detail, detail_cap, "file kind %u, expected %u",
                      f.identity.kind, self->kind);
    if (f.abi_version != SR_CPUSTATE_ABI_VERSION)
        return refuse(NKSTATE_ABI_VERSION, detail, detail_cap, "file CpuState ABI %u, this build %u",
                      f.abi_version, (unsigned)SR_CPUSTATE_ABI_VERSION);
    if (f.cpustate_size != (uint32_t)sizeof(CpuState))
        return refuse(NKSTATE_CPUSTATE_SIZE, detail, detail_cap, "file sizeof(CpuState) %u, this build %u",
                      f.cpustate_size, (unsigned)sizeof(CpuState));
    if (memcmp(f.identity.executable, self->executable, 32u) != 0) {
        char a[65], b[65];
        hex32(f.identity.executable, a);
        hex32(self->executable, b);
        return refuse(NKSTATE_EXECUTABLE_IDENTITY, detail, detail_cap, "file %s, this build %s", a, b);
    }
    if (memcmp(f.identity.runtime, self->runtime, NKSTATE_RUNTIME_ID_LEN) != 0)
        return refuse(NKSTATE_RUNTIME_IDENTITY, detail, detail_cap, "file '%.*s', this build '%.*s'",
                      (int)fixed_strlen(f.identity.runtime, NKSTATE_RUNTIME_ID_LEN), f.identity.runtime,
                      (int)fixed_strlen(self->runtime, NKSTATE_RUNTIME_ID_LEN), self->runtime);
    if (memcmp(f.identity.disc_id, self->disc_id, NKSTATE_DISC_ID_LEN) != 0)
        return refuse(NKSTATE_DISC_ID, detail, detail_cap, "file '%.*s', this run '%.*s'",
                      (int)fixed_strlen(f.identity.disc_id, NKSTATE_DISC_ID_LEN), f.identity.disc_id,
                      (int)fixed_strlen(self->disc_id, NKSTATE_DISC_ID_LEN), self->disc_id);
    if ((f.identity.flags & ~NKSTATE_FLAGS_KNOWN) != 0u)
        return refuse(NKSTATE_FLAGS, detail, detail_cap, "flags 0x%x has unknown bits", f.identity.flags);
    if (expect->require_deterministic_clock && !(f.identity.flags & NKSTATE_FLAG_DETERMINISTIC_CLOCK))
        return refuse(NKSTATE_CLOCK_NOT_DETERMINISTIC, detail, detail_cap,
                      "the file was written without SR_DETERMINISTIC_CLOCK=1");
    if (f.segment_count > NKSTATE_MAX_SEGMENTS)
        return refuse(NKSTATE_TOO_MANY_SEGMENTS, detail, detail_cap, "%u segments, at most %u",
                      f.segment_count, NKSTATE_MAX_SEGMENTS);

    const size_t table_size = (size_t)f.segment_count * NKSTATE_TABLE_ENTRY_SIZE;
    if (len - NKSTATE_HEADER_SIZE < table_size)
        return refuse(NKSTATE_TRUNCATED, detail, detail_cap, "the segment table runs past the end");
    for (uint32_t i = 0; i < f.segment_count; i++) {
        NkStateSegment *s = &f.segments[i];
        memcpy(s->tag, p, NKSTATE_TAG_LEN); s->tag[NKSTATE_TAG_LEN] = '\0'; p += 4;
        s->version = rd32(p); p += 4;
        s->length = rd64(p); p += 8;
        memcpy(s->sha256, p, 32u); p += 32;
        if (!tag_valid(s->tag))
            return refuse(NKSTATE_BAD_TAG, detail, detail_cap, "segment %u", i);
        for (uint32_t j = 0; j < i; j++)
            if (memcmp(f.segments[j].tag, s->tag, NKSTATE_TAG_LEN) == 0)
                return refuse(NKSTATE_DUPLICATE_SEGMENT, detail, detail_cap, "segment '%s' appears twice", s->tag);
        const NkStateExporter *e = find_exporter(expect->exporters, expect->exporter_count, s->tag);
        if (!e)
            return refuse(NKSTATE_UNKNOWN_SEGMENT, detail, detail_cap,
                          "segment '%s': this build has no loader for it", s->tag);
        if (s->version != e->version)
            return refuse(NKSTATE_SEGMENT_VERSION, detail, detail_cap, "segment '%s' version %u, this build reads %u",
                          s->tag, s->version, e->version);
    }
    for (size_t i = 0; i < expect->exporter_count; i++) {
        int found = 0;
        for (uint32_t j = 0; j < f.segment_count && !found; j++)
            found = memcmp(f.segments[j].tag, expect->exporters[i].tag, NKSTATE_TAG_LEN) == 0;
        if (!found)
            return refuse(NKSTATE_MISSING_SEGMENT, detail, detail_cap, "no segment '%.4s' (%s)",
                          expect->exporters[i].tag, expect->exporters[i].name ? expect->exporters[i].name : "?");
    }

    /* Payload extents, with checked arithmetic: header + table + payloads + 32 == len. */
    size_t offset = NKSTATE_HEADER_SIZE + table_size;
    for (uint32_t i = 0; i < f.segment_count; i++) {
        const uint64_t length = f.segments[i].length;
        if (length > (uint64_t)(len - offset))
            return refuse(NKSTATE_TRUNCATED, detail, detail_cap, "segment '%s' runs past the end", f.segments[i].tag);
        offset += (size_t)length;
    }
    if (len - offset < 32u)
        return refuse(NKSTATE_TRUNCATED, detail, detail_cap, "no room for the file hash");
    if (len - offset > 32u)
        return refuse(NKSTATE_TRAILING_BYTES, detail, detail_cap, "%zu bytes after the file hash", len - offset - 32u);

    offset = NKSTATE_HEADER_SIZE + table_size;
    for (uint32_t i = 0; i < f.segment_count; i++) {
        uint8_t digest[32];
        sr_vfpu_sha256(data + offset, (size_t)f.segments[i].length, digest);
        if (memcmp(digest, f.segments[i].sha256, 32u) != 0)
            return refuse(NKSTATE_SEGMENT_HASH, detail, detail_cap, "segment '%s' does not match its hash",
                          f.segments[i].tag);
        offset += (size_t)f.segments[i].length;
    }
    {
        uint8_t digest[32];
        sr_vfpu_sha256(data, offset, digest);
        if (memcmp(digest, data + offset, 32u) != 0)
            return refuse(NKSTATE_FILE_HASH, detail, detail_cap, "the header or segment table was changed");
    }

    f.storage = (uint8_t *)malloc(len);
    if (!f.storage) return refuse(NKSTATE_NO_MEMORY, detail, detail_cap, "copying %zu bytes", len);
    memcpy(f.storage, data, len);
    offset = NKSTATE_HEADER_SIZE + table_size;
    for (uint32_t i = 0; i < f.segment_count; i++) {
        f.segments[i].payload = f.storage + offset;
        offset += (size_t)f.segments[i].length;
    }
    *out = f;
    return NKSTATE_OK;
}

NkStateStatus nkstate_restore(const uint8_t *data, size_t len, const NkStateExpect *expect,
                              char *detail, size_t detail_cap) {
    NkStateFile f;
    NkStateStatus status = nkstate_parse(data, len, expect, &f, detail, detail_cap);
    if (status != NKSTATE_OK) return status;
    for (uint32_t i = 0; i < f.segment_count && status == NKSTATE_OK; i++) {
        const NkStateSegment *s = &f.segments[i];
        const NkStateExporter *e = find_exporter(expect->exporters, expect->exporter_count, s->tag);
        char sub[192] = {0};
        if (!e->load || !e->save) {
            status = refuse(NKSTATE_MISSING_EXPORTER, detail, detail_cap, "segment '%s' has no load or save function", s->tag);
            break;
        }
        status = e->load(e->ctx, s->payload, (size_t)s->length, sub, sizeof sub);
        if (status != NKSTATE_OK)
            status = refuse(NKSTATE_SEGMENT_PAYLOAD, detail, detail_cap, "segment '%s' (%s): %s", s->tag,
                            e->name ? e->name : "?", sub[0] ? sub : "refused");
    }
    /* Every loaded subsystem must save back to the exact bytes it was given. */
    for (uint32_t i = 0; i < f.segment_count && status == NKSTATE_OK; i++) {
        const NkStateSegment *s = &f.segments[i];
        const NkStateExporter *e = find_exporter(expect->exporters, expect->exporter_count, s->tag);
        NkStateBuf back = {0};
        char sub[192] = {0};
        NkStateStatus saved = e->save(e->ctx, &back, sub, sizeof sub);
        if (saved != NKSTATE_OK) {
            status = refuse(NKSTATE_RESTORE_VERIFY, detail, detail_cap, "segment '%s' (%s) could not save back: %s",
                            s->tag, e->name ? e->name : "?", sub[0] ? sub : nkstate_status_name(saved));
        } else {
            uint8_t digest[32];
            sr_vfpu_sha256(back.data ? back.data : (const uint8_t *)"", back.len, digest);
            if (back.len != s->length || memcmp(digest, s->sha256, 32u) != 0)
                status = refuse(NKSTATE_RESTORE_VERIFY, detail, detail_cap,
                                "segment '%s' (%s) saves back differently after the load", s->tag,
                                e->name ? e->name : "?");
        }
        nkstate_buf_free(&back);
    }
    nkstate_file_free(&f);
    return status;
}

/* ---- files ----------------------------------------------------------------------- */

NkStateStatus nkstate_write_file(const char *path, const NkStateBuf *image, char *detail, size_t detail_cap) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return refuse(NKSTATE_IO_ERROR, detail, detail_cap, "cannot create %s", path);
    size_t wrote = image->len ? fwrite(image->data, 1, image->len, fp) : 0u;
    int closed = fclose(fp);
    if (wrote != image->len || closed != 0)
        return refuse(NKSTATE_IO_ERROR, detail, detail_cap, "cannot write %s", path);
    return NKSTATE_OK;
}

NkStateStatus nkstate_read_file(const char *path, NkStateBuf *out, char *detail, size_t detail_cap) {
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return refuse(NKSTATE_IO_ERROR, detail, detail_cap, "cannot open %s", path);
    uint8_t chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, fp)) > 0u) {
        if (!nkstate_buf_put(out, chunk, got)) {
            fclose(fp);
            nkstate_buf_free(out);
            return refuse(NKSTATE_NO_MEMORY, detail, detail_cap, "reading %s", path);
        }
    }
    int failed = ferror(fp);
    fclose(fp);
    if (failed) {
        nkstate_buf_free(out);
        return refuse(NKSTATE_IO_ERROR, detail, detail_cap, "cannot read %s", path);
    }
    return NKSTATE_OK;
}

/* ---- CPU and memory segments ------------------------------------------------------ */

NkStateStatus nkstate_cpu_save(const NkStateThread *threads, uint32_t count, NkStateBuf *out,
                               char *detail, size_t detail_cap) {
    if (!nkstate_buf_put_u32(out, count)) return refuse(NKSTATE_NO_MEMORY, detail, detail_cap, "CPU segment");
    for (uint32_t i = 0; i < count; i++) {
        /* CpuState is all 32-bit words; each is written little-endian so the bytes do not
         * depend on the host. */
        const uint32_t *words = (const uint32_t *)(const void *)threads[i].cpu;
        if (!nkstate_buf_put_u32(out, threads[i].uid))
            return refuse(NKSTATE_NO_MEMORY, detail, detail_cap, "CPU segment");
        for (size_t w = 0; w < sizeof(CpuState) / 4u; w++)
            if (!nkstate_buf_put_u32(out, words[w])) return refuse(NKSTATE_NO_MEMORY, detail, detail_cap, "CPU segment");
    }
    return NKSTATE_OK;
}

_Static_assert(sizeof(CpuState) % 4u == 0u, "CpuState is serialized as 32-bit words");

NkStateStatus nkstate_cpu_load(const NkStateThread *threads, uint32_t count,
                               const uint8_t *payload, size_t len, char *detail, size_t detail_cap) {
    const size_t per = 4u + sizeof(CpuState);
    if (len < 4u) return refuse(NKSTATE_TRUNCATED, detail, detail_cap, "no thread count");
    const uint32_t n = rd32(payload);
    if (n != count)
        return refuse(NKSTATE_SEGMENT_PAYLOAD, detail, detail_cap, "%u threads in the state, %u here", n, count);
    if ((len - 4u) / per < n || (len - 4u) != (size_t)n * per)
        return refuse(NKSTATE_TRUNCATED, detail, detail_cap, "%zu payload bytes for %u threads", len, n);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t uid = rd32(payload + 4u + (size_t)i * per);
        if (uid != threads[i].uid)
            return refuse(NKSTATE_SEGMENT_PAYLOAD, detail, detail_cap, "thread %u is uid 0x%x in the state, 0x%x here",
                          i, uid, threads[i].uid);
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *src = payload + 4u + (size_t)i * per + 4u;
        uint32_t *words = (uint32_t *)(void *)threads[i].cpu;
        for (size_t w = 0; w < sizeof(CpuState) / 4u; w++) words[w] = rd32(src + w * 4u);
    }
    return NKSTATE_OK;
}

NkStateStatus nkstate_mem_save(const NkStateMemRegion *regions, uint32_t count, NkStateBuf *out,
                               char *detail, size_t detail_cap) {
    if (!nkstate_buf_put_u32(out, count)) return refuse(NKSTATE_NO_MEMORY, detail, detail_cap, "MEM segment");
    for (uint32_t i = 0; i < count; i++) {
        if (!nkstate_buf_put_u32(out, regions[i].addr) || !nkstate_buf_put_u32(out, regions[i].size) ||
            !nkstate_buf_put(out, regions[i].host, regions[i].size))
            return refuse(NKSTATE_NO_MEMORY, detail, detail_cap, "MEM segment");
    }
    return NKSTATE_OK;
}

NkStateStatus nkstate_mem_load(const NkStateMemRegion *regions, uint32_t count,
                               const uint8_t *payload, size_t len, char *detail, size_t detail_cap) {
    if (len < 4u) return refuse(NKSTATE_TRUNCATED, detail, detail_cap, "no region count");
    const uint32_t n = rd32(payload);
    if (n != count)
        return refuse(NKSTATE_SEGMENT_PAYLOAD, detail, detail_cap, "%u regions in the state, %u here", n, count);
    /* Check every region header before copying a byte. */
    size_t off = 4u;
    for (uint32_t i = 0; i < n; i++) {
        if (len - off < 8u) return refuse(NKSTATE_TRUNCATED, detail, detail_cap, "region %u header", i);
        const uint32_t addr = rd32(payload + off), size = rd32(payload + off + 4u);
        if (addr != regions[i].addr || size != regions[i].size)
            return refuse(NKSTATE_SEGMENT_PAYLOAD, detail, detail_cap,
                          "region %u is 0x%08x+0x%x in the state, 0x%08x+0x%x here", i, addr, size,
                          regions[i].addr, regions[i].size);
        off += 8u;
        if (len - off < size) return refuse(NKSTATE_TRUNCATED, detail, detail_cap, "region %u bytes", i);
        off += size;
    }
    if (off != len) return refuse(NKSTATE_TRAILING_BYTES, detail, detail_cap, "%zu bytes after the last region", len - off);
    off = 4u;
    for (uint32_t i = 0; i < n; i++) {
        off += 8u;
        memcpy(regions[i].host, payload + off, regions[i].size);
        off += regions[i].size;
    }
    return NKSTATE_OK;
}
