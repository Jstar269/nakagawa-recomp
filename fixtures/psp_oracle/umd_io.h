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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define UMDIO_TEST_ID "PSP-UMDIO-001"
/* The extra open bits a title was seen passing with "umd0:" (flags 0x000f0001). */
#define UMDIO_RAW_FLAGS (PSP_O_RDONLY | 0x000f0000)
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

#endif /* NAKAGAWA_UMD_IO_H */
