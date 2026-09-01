# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Production-path regressions for the bounded savedata security successor.

The C harness includes the real src/rt/savedata.c and drives
sr_savedata_execute(), while the POSIX seam cases also exercise the new
descriptor-relative open primitive directly.  Fixtures are synthetic and
confined to a temporary directory; no title input or private asset is used.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SAVEDATA_C = ROOT / "src" / "rt" / "savedata.c"
SEAM_H = ROOT / "src" / "rt" / "vfs_contained.h"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
POSIX_PROFILE = [] if os.name == "nt" else ["-D_POSIX_C_SOURCE=200809L"]


SECURITY_HARNESS = r"""
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#include "recomp.h"

uint8_t *g_mem;
CpuState *s_cpu;
int g_sr_heap_watch, g_hle_depth;
static int g_oor;
void sr_oor(uint32_t a, uint32_t v, int s) { (void)a; (void)v; (void)s; g_oor++; }
void sr_heap_note_write(uint32_t a, uint32_t w, uint32_t v, uint32_t p)
    { (void)a; (void)w; (void)v; (void)p; }
void sr_heap_note_bulk_write(uint32_t a, uint32_t w, uint32_t p)
    { (void)a; (void)w; (void)p; }
uint32_t sched_current_uid(void) { return 0; }
uint32_t sr_get_ge_status(void) { return 0; }

#ifdef ORDER_INSTRUMENTED
static int g_write_open_count;
static int g_write_open_trunc;
static int g_write_fstat_seen;
static int g_write_nonregular_reported;
static int g_write_ftruncate_count;
static int g_write_bad_order;
static int g_force_nonregular;
static int g_fail_fstat;
static int g_fail_ftruncate;
static int g_fail_fdopen;
static int g_write_pending;
static int g_write_fd = -1;
static int g_write_fd_close_count;

int __real_openat(int dirfd, const char *path, int flags, ...);
int __wrap_openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    int result;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        result = __real_openat(dirfd, path, flags, mode);
    } else {
        result = __real_openat(dirfd, path, flags);
    }
    if ((flags & O_WRONLY) && !(flags & O_DIRECTORY)) {
        g_write_open_count++;
        if (flags & O_TRUNC) g_write_open_trunc++;
        g_write_fstat_seen = 0;
        g_write_nonregular_reported = 0;
        g_write_pending = 1;
        g_write_fd = result;
    }
    return result;
}

int __real_fstat(int fd, struct stat *st);
int __wrap_fstat(int fd, struct stat *st) {
    if (g_fail_fstat && g_write_pending) {
        errno = EIO;
        return -1;
    }
    int result = __real_fstat(fd, st);
    if (result == 0 && g_write_pending) {
        g_write_fstat_seen = 1;
        if (g_force_nonregular) {
            st->st_mode = (st->st_mode & ~S_IFMT) | S_IFDIR;
            g_write_nonregular_reported = 1;
        }
    }
    return result;
}

int __real_ftruncate(int fd, off_t length);
int __wrap_ftruncate(int fd, off_t length) {
    if (g_write_pending) {
        if (!g_write_fstat_seen || g_write_nonregular_reported) g_write_bad_order++;
        g_write_ftruncate_count++;
    }
    if (g_fail_ftruncate) {
        errno = EIO;
        return -1;
    }
    return __real_ftruncate(fd, length);
}

FILE *__real_fdopen(int fd, const char *mode);
FILE *__wrap_fdopen(int fd, const char *mode) {
    if (g_fail_fdopen && g_write_pending && fd == g_write_fd) {
        errno = EIO;
        return NULL;
    }
    return __real_fdopen(fd, mode);
}

int __real_close(int fd);
int __wrap_close(int fd) {
    if (g_write_pending && fd == g_write_fd) {
        g_write_fd_close_count++;
        g_write_fd = -1;
    }
    return __real_close(fd);
}

static void order_reset(void) {
    g_write_open_count = 0;
    g_write_open_trunc = 0;
    g_write_fstat_seen = 0;
    g_write_nonregular_reported = 0;
    g_write_ftruncate_count = 0;
    g_write_bad_order = 0;
    g_force_nonregular = 0;
    g_fail_fstat = 0;
    g_fail_ftruncate = 0;
    g_fail_fdopen = 0;
    g_write_pending = 0;
    g_write_fd = -1;
    g_write_fd_close_count = 0;
}
#else
static void order_reset(void) { }
#endif

#include "savedata.c"

#define PARAM 0x08800000u
#define DATA 0x08900000u
#define SIZE_INFO 0x08a00000u
#define SEC_ENTRIES 0x08a10000u
#define NORM_ENTRIES 0x08a11000u
#define ARENA 0x0c000000u

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void set_guest_string(uint32_t addr, const char *s) {
    size_t n = strlen(s) + 1u;
    for (size_t i = 0; i < n; i++) sr_w8(addr + (uint32_t)i, (uint8_t)s[i]);
}

static void reset_guest(void) {
    memset(g_mem - 0x08000000u, 0, ARENA);
    g_oor = 0;
}

static void set_save_request(const char *root, uint32_t mode, const char *file) {
    setenv("SR_MEMSTICK", root, 1);
    reset_guest();
    sr_w32(PARAM + SDP_mode, mode);
    set_guest_string(PARAM + SDP_gameName, "ULUS99999");
    set_guest_string(PARAM + SDP_saveName, "SLOT00");
    set_guest_string(PARAM + SDP_fileName, file);
    sr_w32(PARAM + SDP_dataBuf, DATA);
}

static uint32_t run_save(const char *root, const char *file, const char *data) {
    size_t n = strlen(data);
    set_save_request(root, SD_SAVE, file);
    sr_w32(PARAM + SDP_dataSize, (uint32_t)n);
    for (size_t i = 0; i < n; i++) sr_w8(DATA + (uint32_t)i, (uint8_t)data[i]);
    return sr_savedata_execute(PARAM);
}

static uint32_t run_load(const char *root, const char *file, char *out, size_t cap) {
    set_save_request(root, SD_LOAD, file);
    sr_w32(PARAM + SDP_dataBufSize, (uint32_t)cap);
    uint32_t result = sr_savedata_execute(PARAM);
    uint32_t n = MEM_R32(PARAM + SDP_dataSize);
    if (out && cap > 0) {
        if (n >= cap) n = (uint32_t)cap - 1u;
        for (uint32_t i = 0; i < n; i++) out[i] = (char)MEM_R8(DATA + i);
        out[n] = '\0';
    }
    return result;
}

static int make_dir(const char *path) {
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int exists_no_follow(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static int write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 0;
    size_t n = strlen(data);
    int ok = write(fd, data, n) == (ssize_t)n;
    close(fd);
    return ok;
}

static int read_file(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0 || cap == 0) { if (fd >= 0) close(fd); return 0; }
    ssize_t n = read(fd, out, cap - 1u);
    close(fd);
    if (n < 0) return 0;
    out[n] = '\0';
    return 1;
}

static int make_savedata_tree(const char *root) {
    char p[512];
    if (!make_dir(root)) return 0;
    snprintf(p, sizeof(p), "%s/PSP", root); if (!make_dir(p)) return 0;
    snprintf(p, sizeof(p), "%s/PSP/SAVEDATA", root); if (!make_dir(p)) return 0;
    snprintf(p, sizeof(p), "%s/PSP/SAVEDATA/ULUS99999SLOT00", root);
    return make_dir(p);
}

typedef struct SwapFileCtx {
    char file[512], moved[512], target[512];
    int fired;
} SwapFileCtx;

static void swap_final_file(void *opaque) {
    SwapFileCtx *ctx = (SwapFileCtx *)opaque;
    ctx->fired = 1;
    (void)rename(ctx->file, ctx->moved);
    (void)symlink(ctx->target, ctx->file);
}

static void check_open_cases(const char *base) {
#if defined(SR_CD_BACKEND_POSIX_AT)
    char root[512], path[512], outside[512], target[512], inroot[512], inroot_link[512], fifo[512], content[64];

    /* Ordinary SAVE then LOAD: the production path still works. */
    snprintf(root, sizeof(root), "%s/normal", base);
    CHECK(run_save(root, "DATA.BIN", "GOOD") == 0, "ordinary SAVE must succeed");
    snprintf(path, sizeof(path), "%s/PSP/SAVEDATA/ULUS99999SLOT00/DATA.BIN", root);
    CHECK(read_file(path, content, sizeof(content)) && !strcmp(content, "GOOD"),
          "ordinary SAVE must remain in-root");
    memset(content, 0, sizeof(content));
    CHECK(run_load(root, "DATA.BIN", content, sizeof(content)) == 0 && !strcmp(content, "GOOD"),
          "ordinary LOAD must read the in-root file");

    /* Final symlink: the data file is a guest-named leaf and must not follow it. */
    snprintf(root, sizeof(root), "%s/final", base);
    snprintf(outside, sizeof(outside), "%s/final-outside", base);
    CHECK(make_savedata_tree(root) && make_dir(outside), "final-link fixture");
    snprintf(target, sizeof(target), "%s/target.bin", outside);
    snprintf(path, sizeof(path), "%s/PSP/SAVEDATA/ULUS99999SLOT00/DATA.BIN", root);
    CHECK(write_file(target, "OUTSIDE") && symlink(target, path) == 0, "final-link setup");
    CHECK(run_save(root, "DATA.BIN", "ATTACK") != 0, "final symlink SAVE must fail closed");
    CHECK(read_file(target, content, sizeof(content)) && !strcmp(content, "OUTSIDE"),
          "final symlink SAVE must not modify outside target");
    memset(content, 0, sizeof(content));
    CHECK(run_load(root, "DATA.BIN", content, sizeof(content)) != 0 && !strcmp(content, ""),
          "final symlink LOAD must fail closed");

    /* An in-root final symlink is still a link object, not a direct regular
     * save member.  The explicit no-follow policy must reject it too. */
    snprintf(inroot, sizeof(inroot), "%s/PSP/SAVEDATA/ULUS99999SLOT00/inroot-target.bin", root);
    snprintf(inroot_link, sizeof(inroot_link), "%s/PSP/SAVEDATA/ULUS99999SLOT00/INROOT.LNK", root);
    CHECK(write_file(inroot, "INROOT") && symlink(inroot, inroot_link) == 0,
          "in-root-link setup");
    CHECK(run_save(root, "INROOT.LNK", "ATTACK") != 0,
          "in-root final symlink SAVE must fail closed");
    CHECK(read_file(inroot, content, sizeof(content)) && !strcmp(content, "INROOT"),
          "in-root final symlink SAVE must not modify its target");

    /* A directory is not a regular save member and must fail closed. */
    snprintf(path, sizeof(path), "%s/PSP/SAVEDATA/ULUS99999SLOT00/DIRECTORY", root);
    CHECK(make_dir(path), "directory setup");
    CHECK(run_save(root, "DIRECTORY", "ATTACK") != 0,
          "directory SAVE must fail closed");

    /* A special-file final member must not turn regular-file qualification
     * into a blocking FIFO open. */
    snprintf(fifo, sizeof(fifo), "%s/PSP/SAVEDATA/ULUS99999SLOT00/FIFO", root);
    CHECK(mkfifo(fifo, 0600) == 0, "FIFO setup");
    CHECK(run_save(root, "FIFO", "ATTACK") != 0,
          "special-file SAVE must fail closed without a reader");

    /* Intermediate symlink: preparation itself must not create below the link. */
    snprintf(root, sizeof(root), "%s/intermediate", base);
    snprintf(outside, sizeof(outside), "%s/intermediate-outside", base);
    snprintf(path, sizeof(path), "%s/PSP", root);
    CHECK(make_dir(root) && make_dir(path) && make_dir(outside), "intermediate fixture");
    snprintf(target, sizeof(target), "%s/SAVEDATA", outside);
    CHECK(make_dir(target), "intermediate target");
    snprintf(path, sizeof(path), "%s/PSP/SAVEDATA", root);
    CHECK(symlink(target, path) == 0, "intermediate-link setup");
    CHECK(run_save(root, "DATA.BIN", "ATTACK") != 0, "intermediate symlink SAVE must fail closed");
    snprintf(path, sizeof(path), "%s/ULUS99999SLOT00/DATA.BIN", target);
    CHECK(!exists_no_follow(path), "intermediate symlink SAVE must not create outside data");
    memset(content, 0, sizeof(content));
    CHECK(run_load(root, "DATA.BIN", content, sizeof(content)) != 0,
          "intermediate symlink LOAD must fail closed");

    /* An operator-configured root symlink remains valid: only below-root
     * guest-named components receive the no-follow policy. */
    snprintf(outside, sizeof(outside), "%s/root-target", base);
    snprintf(root, sizeof(root), "%s/root-alias", base);
    CHECK(make_savedata_tree(outside) && symlink(outside, root) == 0, "root-link setup");
    CHECK(run_save(root, "DATA.BIN", "ROOT") == 0, "configured root symlink must remain usable");
    snprintf(path, sizeof(path), "%s/PSP/SAVEDATA/ULUS99999SLOT00/DATA.BIN", outside);
    CHECK(read_file(path, content, sizeof(content)) && !strcmp(content, "ROOT"),
          "configured root symlink must bind to its target");

    /* POSIX host filenames retain their own policy; no Win32 trailing-dot rule
     * is imported into the descriptor seam. */
    sr_cd_root bound;
    CHECK(sr_cd_root_open(outside, &bound) == SR_CD_OK, "open root for POSIX-name case");
    FILE *stream = NULL;
    CHECK(sr_cd_open_file(&bound, "PSP/SAVEDATA/ULUS99999SLOT00", "ordinary. ",
                          SR_CD_FILE_WRITE_TRUNCATE, &stream) == SR_CD_OK && stream,
          "ordinary POSIX filename must be accepted");
    if (stream) { fputs("ODD", stream); fclose(stream); }
    stream = NULL;
    CHECK(sr_cd_open_file(&bound, "PSP/SAVEDATA/ULUS99999SLOT00", "ordinary. ",
                          SR_CD_FILE_READ, &stream) == SR_CD_OK && stream,
          "ordinary POSIX filename must remain readable");
    if (stream) {
        memset(content, 0, sizeof(content));
        fgets(content, sizeof(content), stream);
        CHECK(!strcmp(content, "ODD"), "ordinary POSIX filename content must round-trip");
        fclose(stream);
    }
    sr_cd_root_close(&bound);

#if defined(SR_CD_TEST_HOOKS)
    /* Deterministic replacement after validation: the hook swaps the final
     * name for an outside symlink immediately before openat(). */
    snprintf(root, sizeof(root), "%s/race", base);
    snprintf(outside, sizeof(outside), "%s/race-outside", base);
    CHECK(make_savedata_tree(root) && make_dir(outside), "race fixture");
    SwapFileCtx ctx;
    snprintf(ctx.file, sizeof(ctx.file), "%s/PSP/SAVEDATA/ULUS99999SLOT00/DATA.BIN", root);
    snprintf(ctx.moved, sizeof(ctx.moved), "%s/PSP/SAVEDATA/ULUS99999SLOT00/DATA.old", root);
    snprintf(ctx.target, sizeof(ctx.target), "%s/target.bin", outside);
    CHECK(write_file(ctx.file, "INSIDE") && write_file(ctx.target, "OUTSIDE"), "race file setup");
    CHECK(sr_cd_root_open(root, &bound) == SR_CD_OK, "open root for race case");
    sr_cd_test_hooks_clear();
    sr_cd_test_hook_set(SR_CD_HOOK_BEFORE_FILE_OPEN, swap_final_file, &ctx);
    stream = NULL;
    sr_cd_status race = sr_cd_open_file(&bound, "PSP/SAVEDATA/ULUS99999SLOT00", "DATA.BIN",
                                        SR_CD_FILE_READ, &stream);
    sr_cd_test_hooks_clear();
    CHECK(ctx.fired == 1 && race != SR_CD_OK && !stream,
          "replacement after validation must be rejected by the final open");
    CHECK(read_file(ctx.target, content, sizeof(content)) && !strcmp(content, "OUTSIDE"),
          "replacement race must not read outside target");
    sr_cd_root_close(&bound);
#endif
#else
    (void)base;
    printf("SKIP: POSIX savedata cases require the POSIX descriptor backend\n");
#endif
}

static void check_write_order_cases(const char *base) {
#if defined(SR_CD_BACKEND_POSIX_AT) && defined(ORDER_INSTRUMENTED)
    char root[512], path[512], content[64];
    snprintf(root, sizeof(root), "%s/order", base);
    CHECK(make_savedata_tree(root), "write-order fixture");
    snprintf(path, sizeof(path), "%s/PSP/SAVEDATA/ULUS99999SLOT00/DATA.BIN", root);

    /* R1/R2: an existing regular file is opened, truncated only after the
     * regular-file check, and receives the exact replacement bytes. */
    CHECK(write_file(path, "OLD-TRAILING-CONTENT"), "existing regular-file setup");
    order_reset();
    CHECK(run_save(root, "DATA.BIN", "NEW") == 0,
          "existing regular-file SAVE must succeed");
    CHECK(g_write_open_count > 0 && g_write_open_trunc == 0,
          "initial write opens must not request O_TRUNC");
    CHECK(g_write_ftruncate_count == g_write_open_count && g_write_bad_order == 0,
          "every write open must fstat before post-validation ftruncate");
    CHECK(read_file(path, content, sizeof(content)) && !strcmp(content, "NEW"),
          "existing regular file must receive exact replacement bytes");

    /* R3: creation of a missing regular file remains successful. */
    char create_root[512], create_path[512];
    snprintf(create_root, sizeof(create_root), "%s/order-create", base);
    CHECK(run_save(create_root, "NEW.BIN", "CREATED") == 0,
          "missing regular file SAVE must create successfully");
    snprintf(create_path, sizeof(create_path),
             "%s/PSP/SAVEDATA/ULUS99999SLOT00/NEW.BIN", create_root);
    CHECK(read_file(create_path, content, sizeof(content)) && !strcmp(content, "CREATED"),
          "created regular file must contain exact bytes");

    /* M3 guard: if fstat reports a non-regular final object, no truncation may
     * occur before the type decision and the old contents must remain. */
    CHECK(write_file(path, "KEEP"), "non-regular validation setup");
    order_reset();
    g_force_nonregular = 1;
    CHECK(run_save(root, "DATA.BIN", "ATTACK") != 0,
          "forced non-regular final object must fail closed");
    CHECK(g_write_ftruncate_count == 0 && g_write_bad_order == 0,
          "non-regular final object must not be truncated");
    CHECK(read_file(path, content, sizeof(content)) && !strcmp(content, "KEEP"),
          "non-regular rejection must preserve the existing file");

    /* An fstat failure must close the acquired descriptor before reporting
     * failure, and must not reach the post-validation truncate. */
    order_reset();
    g_fail_fstat = 1;
    CHECK(run_save(root, "DATA.BIN", "ATTACK") != 0,
          "fstat failure must fail the SAVE");
    CHECK(g_write_ftruncate_count == 0 && g_write_fd_close_count == 1,
          "fstat failure must close the descriptor without truncating");
    CHECK(read_file(path, content, sizeof(content)) && !strcmp(content, "KEEP"),
          "fstat failure must preserve the existing file");

    /* Ftruncate failure must close the descriptor and report SAVE failure;
     * the injected failure occurs before the real truncate, preserving data. */
    order_reset();
    g_fail_ftruncate = 1;
    CHECK(run_save(root, "DATA.BIN", "ATTACK") != 0,
          "ftruncate failure must fail the SAVE");
    CHECK(g_write_ftruncate_count == 1 && g_write_bad_order == 0 &&
              g_write_fd_close_count == 1,
          "ftruncate failure must validate first and close the descriptor");
    CHECK(read_file(path, content, sizeof(content)) && !strcmp(content, "KEEP"),
          "ftruncate failure must not modify the existing file");

    /* fdopen failure occurs after the descriptor has been validated and
     * truncated; the untransferred descriptor still must be closed and the
     * production SAVE must report failure without writing payload bytes. */
    order_reset();
    g_fail_fdopen = 1;
    CHECK(run_save(root, "DATA.BIN", "ATTACK") != 0,
          "fdopen failure must fail the SAVE");
    CHECK(g_write_fd_close_count == 1,
          "fdopen failure must close the untransferred descriptor");
    CHECK(read_file(path, content, sizeof(content)) && !strcmp(content, ""),
          "fdopen failure must not write payload bytes");
#else
    (void)base;
    printf("SKIP: write-order instrumentation requires the POSIX descriptor backend\n");
#endif
}

static void set_size_entry(uint32_t base, uint32_t index, uint64_t size) {
    uint32_t entry = base + index * SAVEDATA_SIZE_ENTRY_BYTES;
    sr_w32(entry + 0, (uint32_t)size);
    sr_w32(entry + 4, (uint32_t)(size >> 32));
}

static uint32_t run_getsize(uint32_t si, uint32_t n_sec, uint32_t n_norm,
                            uint32_t p_sec, uint32_t p_norm) {
    sr_w32(PARAM + SDP_mode, SD_GETSIZE);
    sr_w32(PARAM + SDP_sizeInfo, si);
    if (si != 0x0bffffd0u) {
        sr_w32(si + 0, n_sec);
        sr_w32(si + 4, n_norm);
        sr_w32(si + 8, p_sec);
        sr_w32(si + 12, p_norm);
    }
    return sr_savedata_execute(PARAM);
}

static void check_getsize_cases(void) {
    uint32_t result;

    /* G1: a normal zero-count request remains a successful no-op calculation. */
    reset_guest();
    result = run_getsize(SIZE_INFO, 0, 0, 0, 0);
    CHECK(result == 0 && MEM_R32(SIZE_INFO + 36) == 64,
          "normal zero-count GETSIZE must preserve the base requirement");

    /* G1: normal small request and the existing cluster model. */
    reset_guest();
    set_size_entry(SEC_ENTRIES, 0, 0x400);
    set_size_entry(NORM_ENTRIES, 0, 0x100);
    result = run_getsize(SIZE_INFO, 1, 1, SEC_ENTRIES, NORM_ENTRIES);
    CHECK(result == 0, "normal GETSIZE must succeed");
    CHECK(MEM_R32(SIZE_INFO + 16) == CLUSTER && MEM_R32(SIZE_INFO + 36) == 128,
          "normal GETSIZE output must preserve the cluster semantics");

    /* G2: both arrays at the maintained 99-entry ceiling. */
    reset_guest();
    for (uint32_t i = 0; i < SAVEDATA_MAX_FILE_ENTRIES; i++) {
        set_size_entry(SEC_ENTRIES, i, 1);
        set_size_entry(NORM_ENTRIES, i, 1);
    }
    result = run_getsize(SIZE_INFO, SAVEDATA_MAX_FILE_ENTRIES,
                         SAVEDATA_MAX_FILE_ENTRIES, SEC_ENTRIES, NORM_ENTRIES);
    CHECK(result == 0 && g_oor == 0, "largest legitimate GETSIZE must succeed without OOR");

    /* G3/G4/G5: one-past, huge unsigned, and negative signed counts reject
     * before any entry is read or output is changed. */
    reset_guest();
    sr_w32(PARAM + SDP_mode, SD_GETSIZE);
    sr_w32(PARAM + SDP_sizeInfo, SIZE_INFO);
    sr_w32(SIZE_INFO + 0, SAVEDATA_MAX_FILE_ENTRIES + 1u);
    sr_w32(SIZE_INFO + 8, SEC_ENTRIES);
    sr_w32(SIZE_INFO + 16, 0xdeadbeefu);
    result = sr_savedata_execute(PARAM);
    CHECK(result != 0 && MEM_R32(SIZE_INFO + 16) == 0xdeadbeefu && g_oor == 0,
          "one-past GETSIZE count must fail before output");

    reset_guest();
    result = run_getsize(SIZE_INFO, UINT32_MAX, 0, SEC_ENTRIES, 0);
    CHECK(result != 0 && g_oor == 0, "huge unsigned GETSIZE count must fail pre-loop");
    reset_guest();
    result = run_getsize(SIZE_INFO, 0, UINT32_MAX, 0, NORM_ENTRIES);
    CHECK(result != 0 && g_oor == 0, "negative signed GETSIZE count must fail pre-loop");

    /* G6: 2^29 * 24 overflows uint32_t, even though the raw count is positive. */
    reset_guest();
    result = run_getsize(SIZE_INFO, 0x20000000u, 0, SEC_ENTRIES, 0);
    CHECK(result != 0 && g_oor == 0, "count-times-entry overflow must fail pre-loop");

    /* G7: a count with no readable entry span must not be iterated. */
    reset_guest();
    result = run_getsize(SIZE_INFO, 1, 0, 0x0bfffff0u, 0);
    CHECK(result != 0 && g_oor == 0, "too-small entry span must fail pre-loop");

    /* G8: the nested output object itself is a 60-byte read/write span. */
    reset_guest();
    result = run_getsize(0x0bffffd0u, 0, 0, 0, 0);
    CHECK(result != 0 && g_oor == 0, "too-small output span must fail before scalar writes");

    /* Checked rounding/accumulation also refuses a forged u64 size. */
    reset_guest();
    set_size_entry(SEC_ENTRIES, 0, UINT64_MAX);
    result = run_getsize(SIZE_INFO, 1, 0, SEC_ENTRIES, 0);
    CHECK(result != 0 && g_oor == 0, "u64 size rounding overflow must fail closed");

    /* Existing null-sizeInfo compatibility remains success/no-op. */
    reset_guest();
    result = run_getsize(0, 0, 0, 0, 0);
    CHECK(result == 0 && g_oor == 0, "null sizeInfo must remain a no-op");
}

static void check_unsupported_host(void) {
    reset_guest();
    sr_w32(PARAM + SDP_mode, SD_SAVE);
    sr_w32(PARAM + SDP_gameName, 0);
    CHECK(sr_savedata_execute(PARAM) != 0, "unsupported host must fail closed");
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 2;
    uint8_t *arena = (uint8_t *)calloc(1, ARENA);
    if (!arena) return 2;
    g_mem = arena + 0x08000000u;
    if (!strcmp(argv[1], "open") && argc == 3) check_open_cases(argv[2]);
    else if (!strcmp(argv[1], "order") && argc == 3) check_write_order_cases(argv[2]);
    else if (!strcmp(argv[1], "getsize")) check_getsize_cases();
    else if (!strcmp(argv[1], "unsupported")) check_unsupported_host();
    else { free(arena); return 2; }
    printf(g_fail ? "security_selftest: %d FAILURE(S)\n" : "security_selftest: OK\n", g_fail);
    free(arena);
    return g_fail ? 1 : 0;
}
"""


def _build(tmp, savedata_override=None, seam_override=None, hooks=False,
           unsupported=False, order=False):
    harness = Path(tmp) / "savedata_security_selftest.c"
    harness.write_text(SECURITY_HARNESS, encoding="utf-8", newline="\n")
    if savedata_override is not None:
        (Path(tmp) / "savedata.c").write_text(savedata_override, encoding="utf-8", newline="\n")
    if seam_override is not None:
        (Path(tmp) / "vfs_contained.h").write_text(seam_override, encoding="utf-8", newline="\n")
    exe = Path(tmp) / ("savedata_security_selftest.exe" if os.name == "nt" else "savedata_security_selftest")
    rt = ROOT / "src" / "rt"
    flags = [CC, "-std=c11", "-O1"] + POSIX_PROFILE
    if hooks:
        flags.append("-DSR_CD_TEST_HOOKS")
    if order:
        flags += ["-DORDER_INSTRUMENTED", "-Wl,--wrap=openat", "-Wl,--wrap=fstat",
                  "-Wl,--wrap=ftruncate", "-Wl,--wrap=fdopen", "-Wl,--wrap=close"]
    if unsupported:
        flags += ["-DSR_CD_FORCE_UNSUPPORTED_BACKEND", "-DSR_CD_ALLOW_UNSUPPORTED_HOST"]
    flags += ["-I", str(tmp), "-I", str(rt), "-o", str(exe), str(harness),
              str(rt / "debug.c"), str(rt / "watchpoints_file.c")]
    build = subprocess.run(flags, capture_output=True, text=True)
    if build.returncode != 0:
        raise AssertionError("savedata security harness failed to build:\n" + build.stderr[-5000:])
    return str(exe)


def _run(exe, *args, timeout=10):
    try:
        return subprocess.run([exe, *args], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


class TestSavedataSecurityShape(unittest.TestCase):
    def test_posix_storage_uses_the_contained_descriptor_open(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        for marker, end in (("static int host_write_file", "static int host_read_file"),
                            ("static int host_read_file", "/* Storage boundary")):
            code = text[text.index(marker):text.index(end, text.index(marker))]
            posix = code.split("#else", 1)[1]
            self.assertIn("sr_cd_open_file", posix)
            self.assertNotIn("fopen(", posix)
        posix = SEAM_H.read_text(encoding="utf-8")
        self.assertIn("openat", posix)
        self.assertIn("O_NOFOLLOW", posix)
        self.assertIn("fdopen", posix)

    def test_getsize_preflights_nested_output_and_entry_spans(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        start = text.index("static int validate_getsize_entries")
        code = text[start:text.index("/* Resolve the effective saveName", start)]
        self.assertIn("SAVEDATA_SIZE_INFO_BYTES", code)
        self.assertIn("sr_guest_span_readable(si", code)
        self.assertIn("sr_guest_span_writable(si", code)
        self.assertIn("validate_getsize_entries", code)
        self.assertIn("sr_size_mul_ok", code)
        self.assertNotIn("pSec + i * 24u", code)
        self.assertNotIn("pNorm + i * 24u", code)

    def test_windows_storage_branch_keeps_verified_handle_path(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        for marker, end in (("static int host_write_file", "static int host_read_file"),
                            ("static int host_read_file", "/* Storage boundary")):
            code = text[text.index(marker):text.index(end, text.index(marker))]
            windows = code.split("#ifdef _WIN32", 1)[1].split("#else", 1)[0]
            self.assertIn("sr_vfs_open_contained_utf8", windows)
            self.assertNotIn("fopen(", windows)


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestSavedataSecurityProduction(unittest.TestCase):
    @unittest.skipIf(os.name == "nt", "POSIX descriptor cases run in the WSL POSIX lane")
    def test_posix_open_and_symlink_regressions(self):
        with tempfile.TemporaryDirectory(prefix="savedata_open_") as tmp:
            exe = _build(tmp, hooks=True)
            run = _run(exe, "open", tmp)
            self.assertIsNotNone(run, "POSIX open selftest timed out")
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("security_selftest: OK", run.stdout)

    @unittest.skipIf(os.name == "nt", "write-order instrumentation runs in the WSL POSIX lane")
    def test_write_order_regressions(self):
        with tempfile.TemporaryDirectory(prefix="savedata_order_") as tmp:
            exe = _build(tmp, order=True)
            run = _run(exe, "order", tmp)
            self.assertIsNotNone(run, "write-order selftest timed out")
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("security_selftest: OK", run.stdout)

    @unittest.skipIf(os.name == "nt", "GETSIZE production harness runs in the WSL POSIX lane")
    def test_getsize_regressions(self):
        with tempfile.TemporaryDirectory(prefix="savedata_getsize_") as tmp:
            exe = _build(tmp)
            run = _run(exe, "getsize")
            self.assertIsNotNone(run, "bounded GETSIZE selftest timed out")
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("security_selftest: OK", run.stdout)

    @unittest.skipIf(os.name == "nt", "unsupported POSIX-host build runs in the WSL lane")
    def test_unsupported_host_is_explicitly_fail_closed(self):
        with tempfile.TemporaryDirectory(prefix="savedata_unsupported_") as tmp:
            exe = _build(tmp, unsupported=True)
            run = _run(exe, "unsupported")
            self.assertIsNotNone(run)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("security_selftest: OK", run.stdout)

    def test_load_bearing_mutants_are_killed(self):
        if os.name == "nt":
            self.skipTest("mutant matrix is executed in the WSL POSIX lane")
        original_savedata = SAVEDATA_C.read_text(encoding="utf-8")
        original_seam = SEAM_H.read_text(encoding="utf-8")
        write_block = (
            "    char rel[SR_CD_REL_MAX];\n"
            "    if (!savedata_root_relative(dir, rel, sizeof(rel))) return 0;\n"
            "    sr_cd_root root;\n"
            "    if (sr_cd_root_open(ms_root(), &root) != SR_CD_OK) return 0;\n"
            "    FILE *f = NULL;\n"
            "    sr_cd_status st = sr_cd_open_file(&root, rel, name, SR_CD_FILE_WRITE_TRUNCATE, &f);\n"
            "    sr_cd_root_close(&root);\n"
            "    if (st != SR_CD_OK || !f) return 0;\n"
        )
        read_block = write_block.replace("SR_CD_FILE_WRITE_TRUNCATE", "SR_CD_FILE_READ")
        unsafe_write = (
            "    char path[PATH_MAX];\n"
            "    if (!path_join(path, sizeof(path), dir, name)) return 0;\n"
            "    FILE *f = fopen(path, \"wb\");\n"
            "    if (!f) return 0;\n"
        )
        unsafe_read = unsafe_write.replace("\"wb\"", "\"rb\"")
        self.assertIn(write_block, original_savedata)
        self.assertIn(read_block, original_savedata)

        mutants = [
            ("path-fopen", original_savedata.replace(write_block, unsafe_write, 1)
             .replace(read_block, unsafe_read, 1), original_seam, "open"),
            ("final-no-follow", original_savedata, original_seam.replace(
                "int flags = O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;",
                "int flags = O_CLOEXEC | O_NONBLOCK;", 1), "open"),
            ("intermediate-no-follow", original_savedata, original_seam.replace(
                "return openat(parent, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);",
                "return openat(parent, comp, O_RDONLY | O_DIRECTORY | O_CLOEXEC);", 1), "open"),
            ("getsize-bound", original_savedata.replace(
                "if (!span || count > SAVEDATA_MAX_FILE_ENTRIES ||\n"
                "        !sr_size_mul_ok(count, SAVEDATA_SIZE_ENTRY_BYTES, span)) {",
                "if (!span || !sr_size_mul_ok(count, SAVEDATA_SIZE_ENTRY_BYTES, span)) {", 1),
             original_seam, "getsize"),
            ("getsize-overflow", original_savedata.replace(
                "if (!span || count > SAVEDATA_MAX_FILE_ENTRIES ||\n"
                "        !sr_size_mul_ok(count, SAVEDATA_SIZE_ENTRY_BYTES, span)) {",
                "if (!span) {", 1), original_seam, "getsize"),
            ("getsize-span-order", original_savedata.replace(
                "if (!sr_guest_span_readable(si, SAVEDATA_SIZE_INFO_BYTES) ||\n"
                "        !sr_guest_span_writable(si, SAVEDATA_SIZE_INFO_BYTES)) return 0x80110381u;",
                "/* nested output span validation removed */", 1), original_seam, "getsize"),
        ]

        survivors = []
        outcomes = {}
        for name, savedata, seam, mode in mutants:
            with tempfile.TemporaryDirectory(prefix="savedata_mutant_") as tmp:
                exe = _build(tmp, savedata_override=savedata, seam_override=seam,
                             hooks=(mode == "open"))
                run = _run(exe, mode, tmp, timeout=3)
                if run is None:
                    outcomes[name] = "TIMEOUT"
                elif run.returncode == 0:
                    outcomes[name] = "SURVIVED"
                    survivors.append(name)
                else:
                    outcomes[name] = f"FAILED({run.returncode})"
        self.assertEqual(survivors, [], "savedata security mutants survived: " + repr(outcomes))
        self.assertEqual(set(outcomes), {name for name, *_ in mutants}, repr(outcomes))

    @unittest.skipIf(os.name == "nt", "write-order mutant matrix runs in the WSL POSIX lane")
    def test_write_order_mutants_are_killed(self):
        original_savedata = SAVEDATA_C.read_text(encoding="utf-8")
        original_seam = SEAM_H.read_text(encoding="utf-8")
        ftruncate_block = (
            "    if (mode == SR_CD_FILE_WRITE_TRUNCATE && ftruncate(fd, 0) != 0) {\n"
            "        close(fd);\n"
            "        return SR_CD_IO_ERROR;\n"
            "    }\n"
        )
        regular_check = (
            "    if (!S_ISREG(info.st_mode)) {\n"
            "        close(fd);\n"
            "        return SR_CD_NOT_CONTAINED;\n"
            "    }\n"
        )
        self.assertIn(ftruncate_block, original_seam)
        self.assertIn(regular_check + ftruncate_block, original_seam)

        write_block = (
            "    char rel[SR_CD_REL_MAX];\n"
            "    if (!savedata_root_relative(dir, rel, sizeof(rel))) return 0;\n"
            "    sr_cd_root root;\n"
            "    if (sr_cd_root_open(ms_root(), &root) != SR_CD_OK) return 0;\n"
            "    FILE *f = NULL;\n"
            "    sr_cd_status st = sr_cd_open_file(&root, rel, name, SR_CD_FILE_WRITE_TRUNCATE, &f);\n"
            "    sr_cd_root_close(&root);\n"
            "    if (st != SR_CD_OK || !f) return 0;\n"
        )
        unsafe_write = (
            "    char path[PATH_MAX];\n"
            "    if (!path_join(path, sizeof(path), dir, name)) return 0;\n"
            "    FILE *f = fopen(path, \"wb\");\n"
            "    if (!f) return 0;\n"
        )
        self.assertIn(write_block, original_savedata)

        mutants = [
            ("O_TRUNC", original_seam.replace(
                "        flags |= O_WRONLY | O_CREAT;\n",
                "        flags |= O_WRONLY | O_CREAT | O_TRUNC;\n", 1),
             original_savedata, "order", True),
            ("NO_FTRUNCATE", original_seam.replace(
                ftruncate_block, "    /* post-validation ftruncate removed */\n", 1),
             original_savedata, "order", True),
            ("EARLY_FTRUNCATE", original_seam.replace(
                regular_check + ftruncate_block, ftruncate_block + regular_check, 1),
             original_savedata, "order", True),
            ("FINAL_SYMLINK", original_seam.replace(
                "int flags = O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;",
                "int flags = O_CLOEXEC | O_NONBLOCK;", 1),
             original_savedata, "open", False),
            ("PATH_FOPEN", original_seam,
             original_savedata.replace(write_block, unsafe_write, 1), "open", False),
        ]

        survivors = []
        outcomes = {}
        for name, seam, savedata, mode, instrumented in mutants:
            with tempfile.TemporaryDirectory(prefix="savedata_order_mutant_") as tmp:
                exe = _build(tmp, savedata_override=savedata, seam_override=seam,
                             hooks=(mode == "open"), order=instrumented)
                run = _run(exe, mode, tmp, timeout=10)
                if run is None:
                    outcomes[name] = "TIMEOUT"
                elif run.returncode == 0:
                    outcomes[name] = "SURVIVED"
                    survivors.append(name)
                else:
                    outcomes[name] = f"FAILED({run.returncode})"
        self.assertEqual(survivors, [], "savedata write-order mutants survived: " + repr(outcomes))
        self.assertEqual(set(outcomes), {name for name, *_ in mutants}, repr(outcomes))


if __name__ == "__main__":
    unittest.main()
