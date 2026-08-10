// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>

uint8_t *g_mem;
CpuState *s_cpu;
int g_sr_heap_watch;
int g_hle_depth;

/* Standalone support required by recomp.h's checked memory accessors. */
void sr_oor(uint32_t addr, uint32_t value, int store) {
    fprintf(stderr,
            "guest printf selftest: out-of-range %s addr=0x%08x value=0x%08x\n",
            store ? "write" : "read", addr, value);
}

void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}

uint32_t sched_current_uid(void) { return 0u; }
uint32_t sr_get_ge_status(void) { return 0u; }

static void put_string(uint32_t addr, const char *text) {
    memcpy(SR_HOST(addr), text, strlen(text) + 1u);
}

static int expect_string(uint32_t addr, const char *expected) {
    return strcmp((const char *)SR_HOST(addr), expected) == 0;
}

static void set_double_words(uint32_t *lo, uint32_t *hi, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    *lo = (uint32_t)bits;
    *hi = (uint32_t)(bits >> 32);
}

static void make_star_format(char *out, int flag_count, int precision) {
    int n = 0;
    out[n++] = '%';
    for (int i = 0; i < flag_count; i++) out[n++] = '0';
    if (precision) out[n++] = '.';
    out[n++] = '*';
    out[n++] = 'd';
    out[n] = '\0';
}

static void make_repeated_format(char *out, char repeated, int repeat_count,
                                 const char *suffix) {
    int n = 0;
    out[n++] = '%';
    for (int i = 0; i < repeat_count; i++) out[n++] = repeated;
    while (*suffix != '\0') out[n++] = *suffix++;
    out[n] = '\0';
}

int main(void) {
    g_mem = (uint8_t *)calloc(1u, 0x400u);
    if (!g_mem) return 1;

    CpuState cpu = {0};
    s_cpu = &cpu;
    cpu.r[29] = SR_RAM_BASE + 0x300u;
    cpu.r[31] = 0x12345678u;

    uint32_t dst = SR_RAM_BASE + 0x100u;
    uint32_t fmt = SR_RAM_BASE + 0x020u;
    put_string(fmt, "%5.0f");
    cpu.r[4] = dst;
    cpu.r[5] = fmt;
    set_double_words(&cpu.r[6], &cpu.r[7], 242.0);
    sr_guest_sprintf(&cpu);
    if (!expect_string(dst, "  242") || cpu.r[2] != 5u || cpu.pc != cpu.r[31]) {
        fprintf(stderr, "guest printf float regression: '%s' len=%u pc=0x%08x\n",
                (char *)SR_HOST(dst), cpu.r[2], cpu.pc);
        return 1;
    }

    memset(SR_HOST(dst), 0, 0x80u);
    put_string(fmt, "%d %.1f");
    cpu.r[4] = dst;
    cpu.r[5] = fmt;
    cpu.r[6] = 7u;
    cpu.r[7] = 0xdeadbeefu; /* skipped to align the following double */
    set_double_words(&cpu.r[8], &cpu.r[9], 12.5);
    sr_guest_sprintf(&cpu);
    if (!expect_string(dst, "7 12.5")) {
        fprintf(stderr, "guest printf EABI alignment regression: '%s'\n",
                (char *)SR_HOST(dst));
        return 1;
    }

    memset(SR_HOST(dst), 0, 0x80u);
    uint32_t text = SR_RAM_BASE + 0x060u;
    put_string(text, "host0");
    put_string(fmt, "%s:%d");
    cpu.r[4] = dst;
    cpu.r[5] = fmt;
    cpu.r[6] = text;
    cpu.r[7] = 9u;
    sr_guest_sprintf(&cpu);
    if (!expect_string(dst, "host0:9")) {
        fprintf(stderr, "guest printf existing-format regression: '%s'\n",
                (char *)SR_HOST(dst));
        return 1;
    }

    /* A long flag prefix leaves only three bytes in conv[].  INT32_MIN needs
     * eleven decimal bytes, so snprintf() truncates it but returns 11.  The old
     * code added that return value to ci and then wrote the conversion/NUL past
     * conv[31].  The hardened bridge consumes the arguments and emits a bounded
     * visible fallback instead. */
    char hostile[64];
    memset(SR_HOST(dst), 0, 0x80u);
    make_star_format(hostile, 28, 0);
    put_string(fmt, hostile);
    cpu.r[4] = dst;
    cpu.r[5] = fmt;
    cpu.r[6] = 0x80000000u;
    cpu.r[7] = 123u;
    sr_guest_sprintf(&cpu);
    if (!expect_string(dst, "%d")) {
        fprintf(stderr, "guest printf dynamic-width bound regression: '%s'\n",
                (char *)SR_HOST(dst));
        return 1;
    }

    memset(SR_HOST(dst), 0, 0x80u);
    make_star_format(hostile, 27, 1);
    put_string(fmt, hostile);
    cpu.r[4] = dst;
    cpu.r[5] = fmt;
    cpu.r[6] = 0x80000000u;
    cpu.r[7] = 456u;
    sr_guest_sprintf(&cpu);
    if (!expect_string(dst, "%d")) {
        fprintf(stderr, "guest printf dynamic-precision bound regression: '%s'\n",
                (char *)SR_HOST(dst));
        return 1;
    }

    /* Start the overlong double at odd argument word one.  The fallback must
     * align to r8/r9, consume both words, and leave the following integer at
     * r10 rather than desynchronizing the variadic cursor. */
    memset(SR_HOST(dst), 0, 0x80u);
    make_repeated_format(hostile, '0', 30, "f|%d");
    put_string(fmt, "%d|");
    strcat((char *)SR_HOST(fmt), hostile);
    cpu.r[4] = dst;
    cpu.r[5] = fmt;
    cpu.r[6] = 7u;
    cpu.r[7] = 0xdeadbeefu;
    set_double_words(&cpu.r[8], &cpu.r[9], 12.5);
    cpu.r[10] = 99u;
    sr_guest_sprintf(&cpu);
    if (!expect_string(dst, "7|%f|99")) {
        fprintf(stderr, "guest printf overlong-float cursor regression: '%s'\n",
                (char *)SR_HOST(dst));
        return 1;
    }

    memset(SR_HOST(dst), 0, 0x80u);
    make_repeated_format(hostile, '0', 30, "s:%d");
    put_string(fmt, hostile);
    cpu.r[4] = dst;
    cpu.r[5] = fmt;
    cpu.r[6] = text;
    cpu.r[7] = 9u;
    sr_guest_sprintf(&cpu);
    if (!expect_string(dst, "%s:9")) {
        fprintf(stderr, "guest printf overlong-string regression: '%s'\n",
                (char *)SR_HOST(dst));
        return 1;
    }

    uint32_t count_addr = SR_RAM_BASE + 0x090u;
    MEM_W32(count_addr, 0xa5a55a5au);
    memset(SR_HOST(dst), 0, 0x80u);
    make_repeated_format(hostile, '0', 30, "n:%d");
    put_string(fmt, hostile);
    cpu.r[4] = dst;
    cpu.r[5] = fmt;
    cpu.r[6] = count_addr;
    cpu.r[7] = 44u;
    sr_guest_sprintf(&cpu);
    if (!expect_string(dst, "%n:44") || MEM_R32(count_addr) != 0xa5a55a5au) {
        fprintf(stderr, "guest printf overlong-count regression: '%s' value=0x%08x\n",
                (char *)SR_HOST(dst), MEM_R32(count_addr));
        return 1;
    }

    memset(SR_HOST(dst), 0, 0x80u);
    make_repeated_format(hostile, '0', 30, "q:%d");
    put_string(fmt, hostile);
    cpu.r[4] = dst;
    cpu.r[5] = fmt;
    cpu.r[6] = 0x11223344u;
    cpu.r[7] = 55u;
    sr_guest_sprintf(&cpu);
    if (!expect_string(dst, "%q:55")) {
        fprintf(stderr, "guest printf overlong-unknown regression: '%s'\n",
                (char *)SR_HOST(dst));
        return 1;
    }

    memset(SR_HOST(dst), 0, 0x80u);
    make_repeated_format(hostile, 'l', 30, "d:%d");
    put_string(fmt, hostile);
    cpu.r[4] = dst;
    cpu.r[5] = fmt;
    cpu.r[6] = 11u;
    cpu.r[7] = 22u;
    sr_guest_sprintf(&cpu);
    if (!expect_string(dst, "%d:22")) {
        fprintf(stderr, "guest printf overlong-length regression: '%s'\n",
                (char *)SR_HOST(dst));
        return 1;
    }

    puts("guest printf selftest: OK");
    free(g_mem);
    return 0;
}
