// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#ifndef _WIN32
/* glibc hides setenv under strict C11 unless a POSIX feature level is selected
 * before any system header is included.  Keep the selftest on the same strict
 * hosted flags as the CI native gate without inventing a local prototype. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

uint8_t *g_mem;
CpuState *s_cpu;
volatile int g_sr_vram_read_pending;
void sr_ge_vram_read_prepare(uint32_t address, uint32_t bytes) {
    (void)address; (void)bytes;
}
void sr_ge_vram_write_prepare(uint32_t address, uint32_t bytes) {
    (void)address; (void)bytes;
}
void sr_ge_vram_write_commit(uint32_t address, uint32_t bytes) {
    (void)address; (void)bytes;
}

static void set_test_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* ---- diagnostic streams (#1296) ----------------------------------------------
 * stderr is pointed at a channel that keeps write boundaries, a message-mode named
 * pipe on Windows and a datagram socket elsewhere, so every write the C library
 * issues arrives as one message and the test counts them. */
#ifdef _WIN32
static HANDLE s_stream_channel = INVALID_HANDLE_VALUE;

static int stream_channel_open(void) {
    char name[96];
    snprintf(name, sizeof name, "\\\\.\\pipe\\nk-debug-streams-%lu",
             (unsigned long)GetCurrentProcessId());
    s_stream_channel = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND,
                                        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                        1, 0, 1u << 20, 0, NULL);
    if (s_stream_channel == INVALID_HANDLE_VALUE) return -1;
    HANDLE client = CreateFileA(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (client == INVALID_HANDLE_VALUE) return -1;
    if (!ConnectNamedPipe(s_stream_channel, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
        return -1;
    return _open_osfhandle((intptr_t)client, _O_WRONLY | _O_BINARY);
}

/* Appends every message already waiting; 0 when none is left, -1 on an error. */
static int stream_channel_drain(char *buf, size_t cap, size_t *len, unsigned *messages) {
    for (;;) {
        DWORD avail = 0, got = 0;
        char msg[4096];
        if (!PeekNamedPipe(s_stream_channel, NULL, 0, NULL, &avail, NULL)) return -1;
        if (avail == 0) return 0;
        if (!ReadFile(s_stream_channel, msg, sizeof msg, &got, NULL)) return -1;
        if (got > cap - *len) return -1;
        memcpy(buf + *len, msg, got);
        *len += got;
        (*messages)++;
    }
}

static void stream_wait_ms(unsigned ms) { Sleep(ms); }
#define stream_dup _dup
#define stream_dup2 _dup2
#define stream_close _close
#else
static int s_stream_channel = -1;

static int stream_channel_open(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return -1;
    if (fcntl(sv[0], F_SETFL, O_NONBLOCK) != 0) return -1;
    s_stream_channel = sv[0];
    return sv[1];
}

static int stream_channel_drain(char *buf, size_t cap, size_t *len, unsigned *messages) {
    for (;;) {
        char msg[4096];
        ssize_t got = recv(s_stream_channel, msg, sizeof msg, 0);
        if (got < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        if ((size_t)got > cap - *len) return -1;
        memcpy(buf + *len, msg, (size_t)got);
        *len += (size_t)got;
        (*messages)++;
    }
}

static void stream_wait_ms(unsigned ms) {
    struct timespec ts = {0, (long)ms * 1000000L};
    nanosleep(&ts, NULL);
}
#define stream_dup dup
#define stream_dup2 dup2
#define stream_close close
#endif

/* Three diagnostic lines printed to a redirected stderr reach it complete, without an
 * explicit flush, and in at most one write per printf call. Windows' C runtime wrote
 * an unbuffered redirected stream one byte per WriteFile, one message per byte here. */
static int streams_selftest(void) {
    static const char payload[] = "pc=0x08804000 uid=0x00000042 status=diagnostic-stream";
    char expected[512], seen[1024];
    size_t expected_len = 0, seen_len = 0;
    unsigned messages = 0;

    int fd = stream_channel_open();
    if (fd < 0) {
        fprintf(stderr, "debug streams selftest: channel unavailable\n");
        return 1;
    }
    fflush(stderr);
    int saved = stream_dup(2);
    if (saved < 0 || stream_dup2(fd, 2) < 0) {
        fprintf(stderr, "debug streams selftest: cannot redirect stderr\n");
        return 1;
    }
    stream_close(fd);

    int buffered = sr_debug_streams_init();
    for (int i = 0; i < 3; i++) {
        expected_len += (size_t)snprintf(expected + expected_len, sizeof expected - expected_len,
                                         "DEBUG_STREAMS line=%d %s\n", i, payload);
        fprintf(stderr, "DEBUG_STREAMS line=%d %s\n", i, payload);
    }
    /* No fflush: the runtime's own drain has to deliver the lines while it runs. */
    int rc = 0;
    for (unsigned waited = 0; rc == 0 && seen_len < expected_len &&
                              waited < 100u * SR_DEBUG_STREAM_FLUSH_MS; waited += 5u) {
        stream_wait_ms(5u);
        rc = stream_channel_drain(seen, sizeof seen, &seen_len, &messages);
    }
    fflush(stderr);
    stream_dup2(saved, 2);
    stream_close(saved);

    if (rc != 0 || seen_len != expected_len || memcmp(seen, expected, expected_len) != 0) {
        fprintf(stderr, "debug streams selftest: %zu of %zu bytes arrived (rc=%d)\n",
                seen_len, expected_len, rc);
        return 1;
    }
    if (messages > 3u) {
        fprintf(stderr, "debug streams selftest: %zu bytes took %u writes, not at most 3\n",
                seen_len, messages);
        return 1;
    }
#ifdef _WIN32
    if (buffered < 1) {
        fprintf(stderr, "debug streams selftest: redirected stderr was not buffered\n");
        return 1;
    }
#else
    if (buffered != 0) {
        fprintf(stderr, "debug streams selftest: stderr buffered off Windows\n");
        return 1;
    }
#endif
    printf("debug streams selftest: OK (%u writes for %zu bytes)\n", messages, seen_len);
    return 0;
}

/* abort() must not lose what the buffered stream still holds. */
static int streams_abort_selftest(void) {
    (void)sr_debug_streams_init();
    fputs("DEBUG_STREAMS_ABORT_MARKER\n", stderr);
    abort();
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "streams") == 0) return streams_selftest();
    if (argc > 1 && strcmp(argv[1], "streams-abort") == 0) return streams_abort_selftest();

    set_test_env("SR_VALUE_WATCH_0", "0x440b4000,PANEL_X0");
    set_test_env("SR_WATCH_CONTEXT_PC", "0x1234");
    set_test_env("SR_WATCH_CONTEXT_LIMIT", "1");
    set_test_env("SR_WATCH_CONTEXT_FPR", "20,0x436e0000");
    set_test_env("SR_STORE_CONTEXT_PC", "0x5678");
    set_test_env("SR_STORE_CONTEXT_LIMIT", "2");
    set_test_env("SR_STORE_CONTEXT_MEM", "16,0x24,3");
    set_test_env("SR_TRACK_LAST_WRITER", "1");

    g_sr_debug = sr_debug_init();
    if (g_sr_debug != 0) {
        fprintf(stderr, "debug value watch selftest: unexpected debug mask\n");
        return 1;
    }
    sr_debug_init_watches();
    if (g_sr_mem_watch_count != 1 || !g_sr_mem_watches[0].match_value ||
        g_sr_mem_watches[0].value != 0x440b4000u ||
        g_sr_mem_watch_context_pc != 0x1234u ||
        g_sr_mem_watch_context_limit != 1u ||
        g_sr_mem_watch_context_fpr != 20 ||
        g_sr_mem_watch_context_fpr_value != 0x436e0000u ||
        g_sr_store_context_pc != 0x5678u ||
        g_sr_store_context_limit != 2u ||
        g_sr_store_context_mem_gpr != 16 ||
        g_sr_store_context_mem_offset != 0x24u ||
        g_sr_store_context_mem_words != 3u ||
        !g_sr_last_writer_enabled) {
        fprintf(stderr, "debug value watch selftest: parser mismatch\n");
        return 1;
    }

    if (sr_check_mem_watch(0x0a123400u, 0x3f800000u, 1, 0x00001230u)) {
        fprintf(stderr, "debug value watch selftest: false positive\n");
        return 1;
    }
    if (!sr_check_mem_watch(0x0a123404u, 0x440b4000u, 1, 0x00001234u)) {
        fprintf(stderr, "debug value watch selftest: missed match\n");
        return 1;
    }
    sr_add_mem_watch(0x0a200000u, 0x0a200020u, "range_a");
    sr_add_mem_watch(0x0a200000u, 0x0a200020u, "range_a_duplicate");
    if (g_sr_mem_watch_count != 2) {
        fprintf(stderr, "debug value watch selftest: range dedupe mismatch\n");
        return 1;
    }

    CpuState cpu = {0};
    cpu.pc = 0x5678u;
    cpu.r[17] = 0x09abcdefu;
    cpu.fi[22] = 0x440b4000u;
    s_cpu = &cpu;
    sr_log_mem_watch_context(0x00001230u);
    sr_log_mem_watch_context(0x00001234u);
    cpu.fi[20] = 0x436e0000u;
    sr_log_mem_watch_context(0x00001234u);
    sr_log_mem_watch_context(0x00001234u);
    if (g_sr_mem_watch_context_count != 1u) {
        fprintf(stderr, "debug value watch selftest: context limit mismatch\n");
        return 1;
    }

    g_mem = (uint8_t *)calloc(1u, 0x40u);
    if (!g_mem) return 1;
    cpu.r[16] = SR_RAM_BASE;
    uint32_t context_words[3] = {0x00000001u, 0x00000005u, 0x00000002u};
    memcpy(g_mem + 0x24u, context_words, sizeof context_words);
    sr_log_store_context(0x09000000u, 0x43110000u, 4u, 0x1234u);
    sr_log_store_context(0x09000000u, 0x43110000u, 4u, 0x5678u);
    if (g_sr_store_context_count != 1u) {
        fprintf(stderr, "debug value watch selftest: store context mismatch\n");
        return 1;
    }
    free(g_mem);
    g_mem = NULL;

    sr_note_mem_write(0x0a300000u, 4u, 0x440b4000u, 0x000525a0u);
    sr_note_mem_write(0x0a300004u, 4u, 0x43de8000u, 0x00052944u);
    sr_note_mem_write(0x0a300002u, 2u, 0x0000beefu, 0x00060000u);
    uint32_t addr = 0, width = 0, value = 0, pc = 0;
    if (!sr_find_last_writer(0x0a300003u, 1u, &addr, &width, &value, &pc) ||
        addr != 0x0a300002u || width != 2u || value != 0x0000beefu ||
        pc != 0x00060000u) {
        fprintf(stderr, "debug value watch selftest: overlapping last writer mismatch\n");
        return 1;
    }
    if (!sr_find_last_writer(0x0a300004u, 4u, &addr, &width, &value, &pc) ||
        addr != 0x0a300004u || width != 4u || value != 0x43de8000u ||
        pc != 0x00052944u) {
        fprintf(stderr, "debug value watch selftest: exact last writer mismatch\n");
        return 1;
    }
    sr_note_mem_write(0x4a300008u, 4u, 0x4446c000u, 0x00052628u);
    if (!sr_find_last_writer(0x0a300008u, 4u, &addr, &width, &value, &pc) ||
        addr != 0x0a300008u || value != 0x4446c000u || pc != 0x00052628u) {
        fprintf(stderr, "debug value watch selftest: physical alias mismatch\n");
        return 1;
    }

    puts("debug value watch selftest: OK");
    return 0;
}
