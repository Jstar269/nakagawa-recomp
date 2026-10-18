// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* UMD and bare-device open semantics (oracle case umd-io, evidence id PSP-UMDIO-001).
 *
 * Titles open bare device names such as "umd1:" or "umd0:" and retry until the open succeeds,
 * while the runtime answers them not-found. This case records what the console answers for a
 * fixed list of device and disc paths: sceIoOpen(path, flags, 0) and sceIoDopen(path), each
 * descriptor closed again, with sceUmdCheckMedium and sceUmdGetDriveStat before and after. It
 * runs the same with or without a UMD inserted. Shared by the probe (fixtures/psp_oracle/probe.c,
 * CASE=umd-io) and a host harness (fixtures/psp_oracle/io_cases_host.c).
 *
 * Safety contract (checked by the host harness tests in tools/test_psp_oracle.py):
 *  - Opens use PSP_O_RDONLY, or PSP_O_RDONLY with the 0x000f0000 bits one title passes for
 *    "umd0:"; never a write, create or truncate flag. Nothing is read from or written to any
 *    descriptor; every descriptor opened is closed.
 *  - The UMD is never activated, deactivated or otherwise changed: the case imports exactly
 *    sceUmdCheckMedium and sceUmdGetDriveStat from sceUmdUser (the build pins that set).
 *  - The path lists are fixed in the source; every loop runs over them once.
 *
 * The includer provides SceUID, PSP_O_RDONLY, the IoFileMgrForUser calls sceIoOpen, sceIoClose,
 * sceIoDopen and sceIoDclose, and sceUmdCheckMedium and sceUmdGetDriveStat. */

#ifndef NAKAGAWA_UMD_IO_H
#define NAKAGAWA_UMD_IO_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define UMDIO_TEST_ID "PSP-UMDIO-001"
/* The extra open bits a title was seen passing with "umd0:" (flags 0x000f0001). */
#define UMDIO_RAW_FLAGS (PSP_O_RDONLY | 0x000f0000)
#if !defined(PSP_ORACLE_CASE_UMD_RAW_READ) || !defined(PSP_ORACLE_CASE) || \
    PSP_ORACLE_CASE != PSP_ORACLE_CASE_UMD_RAW_READ
#define UMDIO_LINE_MAX 512u
#define UMDIO_ERROR 0xffffffffu

typedef struct UmdIoOpen {
    const char *path;
    int flags;
} UmdIoOpen;

/* Bare UMD devices, the disc file system with and without a path, and the two raw opens and the
 * memory-stick partition device seen in title traces (that title opened it 0x3; this case opens
 * it read-only). */
static const UmdIoOpen s_umdio_opens[] = {
    {"umd0:", PSP_O_RDONLY},
    {"umd1:", PSP_O_RDONLY},
    {"umd:", PSP_O_RDONLY},
    {"umd0:/", PSP_O_RDONLY},
    {"umd1:/", PSP_O_RDONLY},
    {"disc0:", PSP_O_RDONLY},
    {"disc0:/", PSP_O_RDONLY},
    {"disc0:/PSP_GAME", PSP_O_RDONLY},
    {"disc0:/PSP_GAME/SYSDIR", PSP_O_RDONLY},
    {"disc0:/UMD_DATA.BIN", PSP_O_RDONLY},
    {"umd0:", UMDIO_RAW_FLAGS},
    {"umd1:", UMDIO_RAW_FLAGS},
    {"msstor0p1:/", PSP_O_RDONLY},
};

static const char *const s_umdio_dopens[] = {
    "umd0:",
    "umd1:",
    "umd:",
    "disc0:",
    "disc0:/",
    "disc0:/PSP_GAME",
};

#define UMDIO_OPENS (sizeof(s_umdio_opens) / sizeof(s_umdio_opens[0]))
#define UMDIO_DOPENS (sizeof(s_umdio_dopens) / sizeof(s_umdio_dopens[0]))

typedef struct UmdIo {
    void (*emit)(void *ctx, const char *line, size_t length);
    void (*step)(void *ctx, const char *step);
    void *ctx;
    uint32_t records;
    uint32_t opened;
    uint32_t dopened;
} UmdIo;

static char s_umdio_line[UMDIO_LINE_MAX];
static char s_umdio_step[64];

static void umdio_emit(UmdIo *u, int used) {
    if (used <= 0 || (size_t)used + 1u >= UMDIO_LINE_MAX) return;
    s_umdio_line[used++] = '\n';
    s_umdio_line[used] = '\0';
    u->emit(u->ctx, s_umdio_line, (size_t)used);
    u->records++;
}

static void umdio_step(UmdIo *u, const char *prefix, const char *path) {
    int used = snprintf(s_umdio_step, sizeof(s_umdio_step), "%s%s", prefix, path);
    if (used > 0 && (size_t)used < sizeof(s_umdio_step)) u->step(u->ctx, s_umdio_step);
}

/* umd-state-before / umd-state-after: result = sceUmdCheckMedium, out0 = sceUmdGetDriveStat. */
static void umdio_state(UmdIo *u, const char *case_id) {
    umdio_step(u, "", case_id);
    const int medium = sceUmdCheckMedium();
    const int drive = sceUmdGetDriveStat();
    const int used = snprintf(s_umdio_line, UMDIO_LINE_MAX,
        "NAKAGAWA_PSP_TEST schema=1 test_id=" UMDIO_TEST_ID " case_id=%s status=PASS "
        "result=0x%08x out0=0x%08x", case_id, (unsigned int)medium, (unsigned int)drive);
    umdio_emit(u, used);
}

static void umdio_run(UmdIo *u) {
    umdio_state(u, "umd-state-before");
    for (uint32_t i = 0; i < UMDIO_OPENS; i++) {
        const UmdIoOpen *open = &s_umdio_opens[i];
        umdio_step(u, "open:", open->path);
        const SceUID fd = sceIoOpen(open->path, open->flags, 0);
        int close_rc = (int)UMDIO_ERROR;
        if (fd >= 0) {
            close_rc = sceIoClose(fd);
            u->opened++;
        }
        /* io-open-NNNN: result = sceIoOpen result, out0 = flags, out1 = sceIoClose result
         * (0xffffffff when nothing was opened); detail = path. */
        const int used = snprintf(s_umdio_line, UMDIO_LINE_MAX,
            "NAKAGAWA_PSP_TEST schema=1 test_id=" UMDIO_TEST_ID " case_id=io-open-%04u "
            "status=PASS result=0x%08x out0=0x%08x out1=0x%08x detail=%s",
            (unsigned int)i, (unsigned int)fd, (unsigned int)open->flags, (unsigned int)close_rc,
            open->path);
        umdio_emit(u, used);
    }
    for (uint32_t i = 0; i < UMDIO_DOPENS; i++) {
        const char *path = s_umdio_dopens[i];
        umdio_step(u, "dopen:", path);
        const SceUID dir = sceIoDopen(path);
        int close_rc = (int)UMDIO_ERROR;
        if (dir >= 0) {
            close_rc = sceIoDclose(dir);
            u->dopened++;
        }
        /* io-dopen-NNNN: result = sceIoDopen result, out0 = sceIoDclose result (0xffffffff
         * when nothing was opened); detail = path. */
        const int used = snprintf(s_umdio_line, UMDIO_LINE_MAX,
            "NAKAGAWA_PSP_TEST schema=1 test_id=" UMDIO_TEST_ID " case_id=io-dopen-%04u "
            "status=PASS result=0x%08x out0=0x%08x detail=%s",
            (unsigned int)i, (unsigned int)dir, (unsigned int)close_rc, path);
        umdio_emit(u, used);
    }
    umdio_state(u, "umd-state-after");
    /* umdio-done: out0 opens tried, out1 opens that succeeded, out2 dopens tried, out3 dopens
     * that succeeded, out4 records before this one. */
    const int used = snprintf(s_umdio_line, UMDIO_LINE_MAX,
        "NAKAGAWA_PSP_TEST schema=1 test_id=" UMDIO_TEST_ID " case_id=umdio-done status=PASS "
        "result=0x00000000 out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x out4=0x%08x",
        (unsigned int)UMDIO_OPENS, (unsigned int)u->opened, (unsigned int)UMDIO_DOPENS,
        (unsigned int)u->dopened, (unsigned int)u->records);
    umdio_emit(u, used);
}
#endif /* UMDIO open-list case or host harness */

#if !defined(PSP_ORACLE_CASE) || PSP_ORACLE_CASE == PSP_ORACLE_CASE_UMD_RAW_READ
/* Raw, read-only UMD descriptor measurements (PSP-UMDRAW-001).
 *
 * Safety contract: open only "umd0:" with PSP_O_RDONLY or UMDIO_RAW_FLAGS; never write, create,
 * truncate, activate, deactivate, devctl, ioctl, or chstat. The fixed cells below run once. The
 * only payload-derived bytes emitted are the first eight bytes of the designated sector-16 reads,
 * packed as little-endian low/high words; no other bytes or checksums leave the static buffer.
 * Synchronous and asynchronous read requests are at most UMDRAW_BUFFER_BYTES. The asynchronous
 * cell polls once immediately after sceIoReadAsync and then calls sceIoWaitAsync once before close;
 * there is no retry loop. Both successfully opened descriptors are explicitly closed.
 *
 * The includer provides SceUID, SceOff, SceSize, SceIoStat, PSP_O_RDONLY, UMDIO_RAW_FLAGS, SEEK_*
 * and the listed read-only IoFileMgrForUser functions plus sceUmdCheckMedium/GetDriveStat. */
#define UMDRAW_TEST_ID "PSP-UMDRAW-001"
#define UMDRAW_LINE_MAX 384u
#define UMDRAW_BUFFER_BYTES 32768u

typedef struct UmdRawRead {
    void (*emit)(void *ctx, const char *line, size_t length);
    void (*step)(void *ctx, const char *step);
    void *ctx;
    uint32_t records;
} UmdRawRead;

static char s_umdraw_line[UMDRAW_LINE_MAX];
static unsigned char s_umdraw_buffer[UMDRAW_BUFFER_BYTES];

static void umdraw_record(UmdRawRead *u, const char *case_id, const char *status,
                          const char *format, ...) {
    int used = snprintf(s_umdraw_line, sizeof(s_umdraw_line),
                        "NAKAGAWA_PSP_TEST schema=1 test_id=" UMDRAW_TEST_ID
                        " case_id=%s status=%s ", case_id, status);
    if (used <= 0 || (size_t)used >= sizeof(s_umdraw_line)) return;
    va_list args;
    va_start(args, format);
    const int fields = vsnprintf(s_umdraw_line + used, sizeof(s_umdraw_line) - (size_t)used,
                                 format, args);
    va_end(args);
    if (fields <= 0 || (size_t)fields + (size_t)used + 2u >= sizeof(s_umdraw_line)) return;
    used += fields;
    s_umdraw_line[used++] = '\n';
    s_umdraw_line[used] = '\0';
    u->emit(u->ctx, s_umdraw_line, (size_t)used);
    u->records++;
}

static void umdraw_step(UmdRawRead *u, const char *step) {
    u->step(u->ctx, step);
}

static void umdraw_record_off(UmdRawRead *u, const char *case_id, SceOff result) {
    const uint64_t bits = (uint64_t)result;
    umdraw_record(u, case_id, "PASS", "result=0x%08x out0=0x%08x",
                  (unsigned int)(uint32_t)bits, (unsigned int)(uint32_t)(bits >> 32));
}

static void umdraw_record_read(UmdRawRead *u, const char *case_id, int result,
                               uint32_t requested, int include_header) {
    uint32_t header_low = 0u;
    uint32_t header_high = 0u;
    uint32_t zero = 1u;
    if (include_header) {
        header_low = (uint32_t)s_umdraw_buffer[0]
            | ((uint32_t)s_umdraw_buffer[1] << 8)
            | ((uint32_t)s_umdraw_buffer[2] << 16)
            | ((uint32_t)s_umdraw_buffer[3] << 24);
        header_high = (uint32_t)s_umdraw_buffer[4]
            | ((uint32_t)s_umdraw_buffer[5] << 8)
            | ((uint32_t)s_umdraw_buffer[6] << 16)
            | ((uint32_t)s_umdraw_buffer[7] << 24);
        for (uint32_t i = 0u; i < UMDRAW_BUFFER_BYTES; i++) {
            if (s_umdraw_buffer[i] != 0u) {
                zero = 0u;
                break;
            }
        }
        umdraw_record(u, case_id, "PASS",
                      "result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x",
                      (unsigned int)result, (unsigned int)header_low,
                      (unsigned int)header_high, (unsigned int)zero,
                      (unsigned int)requested);
    } else {
        umdraw_record(u, case_id, "PASS", "result=0x%08x",
                      (unsigned int)result);
    }
}

static void umdraw_state(UmdRawRead *u, const char *case_id) {
    umdraw_step(u, case_id);
    const int medium = sceUmdCheckMedium();
    const int drive = sceUmdGetDriveStat();
    umdraw_record(u, case_id, "PASS", "result=0x%08x out0=0x%08x",
                  (unsigned int)medium, (unsigned int)drive);
}

static void umdraw_seek64(UmdRawRead *u, SceUID fd, const char *case_id,
                          SceOff offset, int whence) {
    umdraw_step(u, case_id);
    const SceOff result = sceIoLseek(fd, offset, whence);
    umdraw_record_off(u, case_id, result);
}

static void umdraw_seek32(UmdRawRead *u, SceUID fd, const char *case_id,
                          int offset, int whence) {
    umdraw_step(u, case_id);
    const int result = sceIoLseek32(fd, offset, whence);
    umdraw_record(u, case_id, "PASS", "result=0x%08x", (unsigned int)result);
}

static void umdraw_read_at(UmdRawRead *u, SceUID fd, const char *seek_case,
                           const char *read_case, SceOff offset,
                           uint32_t requested, int include_header) {
    umdraw_step(u, seek_case);
    const SceOff seek_result = sceIoLseek(fd, offset, SEEK_SET);
    umdraw_step(u, read_case);
    memset(s_umdraw_buffer, 0, sizeof(s_umdraw_buffer));
    const int read_result = sceIoRead(fd, s_umdraw_buffer, (SceSize)requested);
    umdraw_record_off(u, seek_case, seek_result);
    umdraw_record_read(u, read_case, read_result, requested, include_header);
}

static void umdraw_skip_primary(UmdRawRead *u, uint32_t open_result) {
    static const char *const cases[] = {
        "seek-set-0", "seek-set-16", "seek-set-32768", "seek-cur-0", "seek-end-0",
        "seek-set-1", "seek-set-neg1", "seek32-set-0", "seek32-set-16", "seek32-set-32768",
        "seek32-end-0", "pos-advance-seek", "pos-advance-first", "pos-advance-second",
        "pos-advance-cur", "byte-sector16-seek", "byte-sector16-read",
        "size-1-seek", "size-1-read", "size-512-seek", "size-512-read",
        "size-2047-seek", "size-2047-read", "size-2048-seek", "size-2048-read",
        "size-2049-seek", "size-2049-read", "size-4096-seek", "size-4096-read",
        "size-32768-seek", "size-32768-read", "size-0-seek", "size-0-read",
        "unaligned-seek", "unaligned-read", "far-seek", "far-read", "async-seek",
        "async-read", "async-poll", "async-wait", "async-data", "getstat",
    };
    for (uint32_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        umdraw_record(u, cases[i], "SKIP", "result=0x%08x", (unsigned int)open_result);
    }
    umdraw_record(u, "raw-close", "SKIP", "result=0xffffffff");
}

static void umdraw_run(UmdRawRead *u) {
    umdraw_state(u, "umd-state-before");

    umdraw_step(u, "raw-open-extra");
    const SceUID fd = sceIoOpen("umd0:", UMDIO_RAW_FLAGS, 0);
    umdraw_record(u, "raw-open-extra", "PASS", "result=0x%08x out0=0x%08x",
                  (unsigned int)fd, (unsigned int)UMDIO_RAW_FLAGS);
    if (fd < 0) {
        umdraw_skip_primary(u, (uint32_t)fd);
        umdraw_state(u, "umd-state-after");
        return;
    }

    umdraw_seek64(u, fd, "seek-set-0", 0, SEEK_SET);
    umdraw_seek64(u, fd, "seek-set-16", 16, SEEK_SET);
    umdraw_seek64(u, fd, "seek-set-32768", 16 * 2048, SEEK_SET);
    umdraw_seek64(u, fd, "seek-cur-0", 0, SEEK_CUR);
    umdraw_seek64(u, fd, "seek-end-0", 0, SEEK_END);
    umdraw_seek64(u, fd, "seek-set-1", 1, SEEK_SET);
    umdraw_seek64(u, fd, "seek-set-neg1", -1, SEEK_SET);

    umdraw_seek32(u, fd, "seek32-set-0", 0, SEEK_SET);
    umdraw_seek32(u, fd, "seek32-set-16", 16, SEEK_SET);
    umdraw_seek32(u, fd, "seek32-set-32768", 16 * 2048, SEEK_SET);
    umdraw_seek32(u, fd, "seek32-end-0", 0, SEEK_END);

    /* Keep the two reads and the position query adjacent: no host0 record write is interposed. */
    umdraw_step(u, "pos-advance-seek");
    const SceOff pos_seek = sceIoLseek(fd, 16, SEEK_SET);
    umdraw_step(u, "pos-advance-read-pair");
    memset(s_umdraw_buffer, 0, sizeof(s_umdraw_buffer));
    const int first_read = sceIoRead(fd, s_umdraw_buffer, 2048u);
    const uint32_t first_low = (uint32_t)s_umdraw_buffer[0]
        | ((uint32_t)s_umdraw_buffer[1] << 8)
        | ((uint32_t)s_umdraw_buffer[2] << 16)
        | ((uint32_t)s_umdraw_buffer[3] << 24);
    const uint32_t first_high = (uint32_t)s_umdraw_buffer[4]
        | ((uint32_t)s_umdraw_buffer[5] << 8)
        | ((uint32_t)s_umdraw_buffer[6] << 16)
        | ((uint32_t)s_umdraw_buffer[7] << 24);
    uint32_t first_zero = 1u;
    for (uint32_t i = 0u; i < UMDRAW_BUFFER_BYTES; i++) {
        if (s_umdraw_buffer[i] != 0u) { first_zero = 0u; break; }
    }
    memset(s_umdraw_buffer, 0, sizeof(s_umdraw_buffer));
    const int second_read = sceIoRead(fd, s_umdraw_buffer, 2048u);
    const uint32_t second_low = (uint32_t)s_umdraw_buffer[0]
        | ((uint32_t)s_umdraw_buffer[1] << 8)
        | ((uint32_t)s_umdraw_buffer[2] << 16)
        | ((uint32_t)s_umdraw_buffer[3] << 24);
    const uint32_t second_high = (uint32_t)s_umdraw_buffer[4]
        | ((uint32_t)s_umdraw_buffer[5] << 8)
        | ((uint32_t)s_umdraw_buffer[6] << 16)
        | ((uint32_t)s_umdraw_buffer[7] << 24);
    uint32_t second_zero = 1u;
    for (uint32_t i = 0u; i < UMDRAW_BUFFER_BYTES; i++) {
        if (s_umdraw_buffer[i] != 0u) { second_zero = 0u; break; }
    }
    umdraw_step(u, "pos-advance-cur");
    const SceOff current_after_reads = sceIoLseek(fd, 0, SEEK_CUR);
    umdraw_record_off(u, "pos-advance-seek", pos_seek);
    umdraw_record(u, "pos-advance-first", "PASS",
                  "result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x00000800",
                  (unsigned int)first_read, (unsigned int)first_low,
                  (unsigned int)first_high, (unsigned int)first_zero);
    umdraw_record(u, "pos-advance-second", "PASS",
                  "result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x00000800",
                  (unsigned int)second_read, (unsigned int)second_low,
                  (unsigned int)second_high, (unsigned int)second_zero);
    umdraw_record_off(u, "pos-advance-cur", current_after_reads);

    umdraw_read_at(u, fd, "byte-sector16-seek", "byte-sector16-read",
                   16 * 2048, 2048u, 1);

    static const uint32_t sizes[] = {1u, 512u, 2047u, 2048u, 2049u, 4096u, 32768u, 0u};
    static const char *const size_seeks[] = {
        "size-1-seek", "size-512-seek", "size-2047-seek", "size-2048-seek",
        "size-2049-seek", "size-4096-seek", "size-32768-seek", "size-0-seek",
    };
    static const char *const size_reads[] = {
        "size-1-read", "size-512-read", "size-2047-read", "size-2048-read",
        "size-2049-read", "size-4096-read", "size-32768-read", "size-0-read",
    };
    for (uint32_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        umdraw_read_at(u, fd, size_seeks[i], size_reads[i], 16, sizes[i], 1);
    }

    umdraw_read_at(u, fd, "unaligned-seek", "unaligned-read", 16 * 2048 + 1,
                   2048u, 0);
    umdraw_read_at(u, fd, "far-seek", "far-read", 0x7fffffff, 2048u, 0);

    /* No host0 write separates sceIoReadAsync from its single immediate poll. */
    umdraw_step(u, "async-seek");
    const SceOff async_seek = sceIoLseek(fd, 16, SEEK_SET);
    memset(s_umdraw_buffer, 0, sizeof(s_umdraw_buffer));
    umdraw_step(u, "async-read");
    const int async_result = sceIoReadAsync(fd, s_umdraw_buffer, 2048u);
    SceOff poll_value = 0;
    const int poll_result = sceIoPollAsync(fd, &poll_value);
    SceOff wait_value = 0;
    umdraw_step(u, "async-wait");
    const int wait_result = sceIoWaitAsync(fd, &wait_value);
    const uint32_t async_low = (uint32_t)s_umdraw_buffer[0]
        | ((uint32_t)s_umdraw_buffer[1] << 8)
        | ((uint32_t)s_umdraw_buffer[2] << 16)
        | ((uint32_t)s_umdraw_buffer[3] << 24);
    const uint32_t async_high = (uint32_t)s_umdraw_buffer[4]
        | ((uint32_t)s_umdraw_buffer[5] << 8)
        | ((uint32_t)s_umdraw_buffer[6] << 16)
        | ((uint32_t)s_umdraw_buffer[7] << 24);
    uint32_t async_zero = 1u;
    for (uint32_t i = 0u; i < UMDRAW_BUFFER_BYTES; i++) {
        if (s_umdraw_buffer[i] != 0u) { async_zero = 0u; break; }
    }
    umdraw_record_off(u, "async-seek", async_seek);
    umdraw_record(u, "async-read", "PASS", "result=0x%08x", (unsigned int)async_result);
    const uint64_t poll_bits = (uint64_t)poll_value;
    umdraw_record(u, "async-poll", "PASS", "result=0x%08x out0=0x%08x out1=0x%08x",
                  (unsigned int)poll_result, (unsigned int)(uint32_t)poll_bits,
                  (unsigned int)(uint32_t)(poll_bits >> 32));
    const uint64_t wait_bits = (uint64_t)wait_value;
    umdraw_record(u, "async-wait", "PASS", "result=0x%08x out0=0x%08x out1=0x%08x",
                  (unsigned int)wait_result, (unsigned int)(uint32_t)wait_bits,
                  (unsigned int)(uint32_t)(wait_bits >> 32));
    umdraw_record(u, "async-data", "PASS",
                  "result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x00000800",
                  (unsigned int)wait_value, (unsigned int)async_low,
                  (unsigned int)async_high, (unsigned int)async_zero);

    umdraw_step(u, "getstat");
    SceIoStat stat;
    memset(&stat, 0, sizeof(stat));
    const int stat_result = sceIoGetstat("umd0:", &stat);
    const uint64_t stat_size = (uint64_t)stat.st_size;
    umdraw_record(u, "getstat", "PASS", "result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x",
                  (unsigned int)stat_result, (unsigned int)(uint32_t)stat_size,
                  (unsigned int)(uint32_t)(stat_size >> 32), (unsigned int)stat.st_mode);

    umdraw_step(u, "raw-close");
    const int close_result = sceIoClose(fd);
    umdraw_record(u, "raw-close", close_result == 0 ? "PASS" : "FAIL",
                  "result=0x%08x", (unsigned int)close_result);
    if (close_result != 0) {
        umdraw_state(u, "umd-state-after");
        return;
    }

    umdraw_step(u, "readonly-open");
    const SceUID readonly_fd = sceIoOpen("umd0:", PSP_O_RDONLY, 0);
    umdraw_record(u, "readonly-open", "PASS", "result=0x%08x out0=0x00000001",
                  (unsigned int)readonly_fd);
    if (readonly_fd < 0) {
        umdraw_step(u, "umd-state-after");
        const int medium = sceUmdCheckMedium();
        const int drive = sceUmdGetDriveStat();
        umdraw_record(u, "umd-state-after", "PASS", "result=0x%08x out0=0x%08x",
                      (unsigned int)medium, (unsigned int)drive);
        return;
    }
    umdraw_seek64(u, readonly_fd, "readonly-seek-0", 0, SEEK_SET);
    umdraw_seek64(u, readonly_fd, "readonly-seek-16", 16, SEEK_SET);
    umdraw_seek64(u, readonly_fd, "readonly-seek-32768", 16 * 2048, SEEK_SET);
    umdraw_step(u, "readonly-close");
    const int readonly_close = sceIoClose(readonly_fd);
    umdraw_record(u, "readonly-close", readonly_close == 0 ? "PASS" : "FAIL",
                  "result=0x%08x", (unsigned int)readonly_close);

    umdraw_state(u, "umd-state-after");
    if (readonly_close != 0) return;
    const int used = snprintf(s_umdraw_line, sizeof(s_umdraw_line),
        "NAKAGAWA_PSP_TEST schema=1 test_id=" UMDRAW_TEST_ID
        " case_id=umdraw-done status=PASS result=0x00000000 out0=0x%08x\n",
        (unsigned int)u->records);
    if (used > 0 && (size_t)used < sizeof(s_umdraw_line)) {
        u->emit(u->ctx, s_umdraw_line, (size_t)used);
        u->records++;
    }
}
#endif /* raw UMD case or host harness */

#endif /* NAKAGAWA_UMD_IO_H */
