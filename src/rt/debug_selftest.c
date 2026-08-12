// SPDX-License-Identifier: GPL-2.0-or-later
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

uint8_t *g_mem;
CpuState *s_cpu;

static void set_test_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int main(void) {
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
