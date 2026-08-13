// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Behavioral regressions for the PSP-EABI guest sprintf bridge.
 *
 * The bridge never assembles guest bytes into a host printf format, so these
 * cases pin the PSP-visible contract directly: argument consumption, PSP
 * integer widths and sign extension, floating arguments, guest %s/%p/%n, and
 * the deterministic out-of-grammar fallback.
 *
 * The arena is allocated exactly as src/rt/recomp.c does (physical
 * [0, 0x0c000000) with g_mem at guest 0x08000000) so sr_inrange() agrees with
 * the real allocation and the span-boundary cases below are meaningful. */

#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>

uint8_t *g_mem;
CpuState *s_cpu;
int g_sr_heap_watch;
int g_hle_depth;

static long g_oor_events;

/* Standalone support required by recomp.h's checked memory accessors. */
void sr_oor(uint32_t addr, uint32_t value, int store) {
    (void)addr; (void)value; (void)store;
    g_oor_events++;
}

void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}

uint32_t sched_current_uid(void) { return 0u; }
uint32_t sr_get_ge_status(void) { return 0u; }

#define ARENA_BYTES 0x0c000000u
#define GUEST_LAST  0x0bffffffu   /* highest addressable guest byte */

static CpuState cpu;
static int failures;
static uint32_t last_ret;

static uint32_t DST, FMT, TEXT, STACK, COUNTER;

static void put_string(uint32_t addr, const char *text) {
    memcpy(SR_HOST(addr), text, strlen(text) + 1u);
}

static void run_words(const char *fmtstr, const uint32_t *w, int n) {
    memset(SR_HOST(DST), 0, 0x200u);
    memset(SR_HOST(STACK), 0, 0x80u);
    put_string(FMT, fmtstr);
    memset(cpu.r, 0, sizeof cpu.r);
    cpu.r[4] = DST;
    cpu.r[5] = FMT;
    cpu.r[29] = STACK;
    cpu.r[31] = 0x12345678u;
    for (int i = 0; i < n && i < 6; i++) cpu.r[6 + i] = w[i];
    for (int i = 6; i < n; i++) MEM_W32(STACK + 4u * (uint32_t)(i - 6), w[i]);
    sr_guest_sprintf(&cpu);
    last_ret = cpu.r[2];
}

static void expect_words(const char *label, const char *fmtstr,
                         const uint32_t *w, int n, const char *want) {
    run_words(fmtstr, w, n);
    const char *got = (const char *)SR_HOST(DST);
    if (strcmp(got, want) != 0) {
        failures++;
        fprintf(stderr, "FAIL %-30s fmt=\"%s\" want=\"%s\" got=\"%s\"\n",
                label, fmtstr, want, got);
        return;
    }
    if (last_ret != (uint32_t)strlen(want)) {
        failures++;
        fprintf(stderr, "FAIL %-30s fmt=\"%s\" return=%u expected=%zu\n",
                label, fmtstr, last_ret, strlen(want));
    }
    if (cpu.pc != cpu.r[31]) {
        failures++;
        fprintf(stderr, "FAIL %-30s fmt=\"%s\" pc=0x%08x not restored\n",
                label, fmtstr, cpu.pc);
    }
}

#define WORDS(...) ((const uint32_t[]){ __VA_ARGS__ })
#define NWORDS(...) \
    ((int)(sizeof((const uint32_t[]){ __VA_ARGS__ }) / sizeof(uint32_t)))
#define EXPECT(label, fmtstr, want, ...) \
    expect_words((label), (fmtstr), WORDS(__VA_ARGS__), NWORDS(__VA_ARGS__), (want))
#define EXPECT0(label, fmtstr, want) \
    expect_words((label), (fmtstr), NULL, 0, (want))

static uint32_t dbl_lo(double v) {
    uint64_t b; memcpy(&b, &v, sizeof b); return (uint32_t)b;
}
static uint32_t dbl_hi(double v) {
    uint64_t b; memcpy(&b, &v, sizeof b); return (uint32_t)(b >> 32);
}

static void check_u32(const char *label, uint32_t got, uint32_t want) {
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL %-30s got=0x%08x want=0x%08x\n", label, got, want);
    }
}

/* Build "%" + `repeat` copies of `ch` + suffix. */
static void make_repeated(char *out, char ch, int repeat, const char *suffix) {
    int n = 0;
    out[n++] = '%';
    for (int i = 0; i < repeat; i++) out[n++] = ch;
    while (*suffix != '\0') out[n++] = *suffix++;
    out[n] = '\0';
}

int main(void) {
    uint8_t *arena = (uint8_t *)calloc(ARENA_BYTES, 1u);
    if (!arena) { fprintf(stderr, "guest printf selftest: out of memory\n"); return 1; }
    g_mem = arena + 0x08000000u;
    s_cpu = &cpu;

    DST     = SR_RAM_BASE + 0x1000u;
    FMT     = SR_RAM_BASE + 0x0400u;
    TEXT    = SR_RAM_BASE + 0x0600u;
    STACK   = SR_RAM_BASE + 0x2000u;
    COUNTER = SR_RAM_BASE + 0x0800u;

    put_string(TEXT, "host0");

    /* ---- preserved baseline behavior ------------------------------------ */
    EXPECT("float width/precision", "%5.0f", "  242", dbl_lo(242.0), dbl_hi(242.0));
    EXPECT("EABI double alignment", "%d %.1f", "7 12.5",
           7u, 0xdeadbeefu, dbl_lo(12.5), dbl_hi(12.5));
    EXPECT("string then int", "%s:%d", "host0:9", TEXT, 9u);
    EXPECT0("literal percent", "100%%", "100%");
    EXPECT0("no conversions", "plain text", "plain text");

    /* ---- PSP integer widths and sign extension --------------------------- */
    /* PSP long/size_t/ptrdiff_t are 32 bits.  Before the rewrite these reached a
     * host printf as "%ld"/"%zu" with a 32-bit argument, so on any LP64 host
     * guest -1 printed as 4294967295. */
    EXPECT("l is 32-bit signed", "%ld", "-1", 0xffffffffu);
    EXPECT("l is 32-bit unsigned", "%lu", "4294967295", 0xffffffffu);
    EXPECT("l hex", "%lx", "deadbeef", 0xdeadbeefu);
    EXPECT("z is 32-bit unsigned", "%zu", "4294967295", 0xffffffffu);
    EXPECT("z is 32-bit signed", "%zd", "-1", 0xffffffffu);
    EXPECT("t is 32-bit signed", "%td", "-1", 0xffffffffu);
    EXPECT("hh signed narrowing", "%hhd", "-1", 0x000000ffu);
    EXPECT("hh unsigned narrowing", "%hhu", "255", 0x000001ffu);
    EXPECT("h signed narrowing", "%hd", "-1", 0x0000ffffu);
    EXPECT("h unsigned narrowing", "%hu", "65535", 0x0001ffffu);

    /* ll/j are 64-bit and occupy two words at an even slot. */
    EXPECT("ll negative", "%lld", "-4294967295", 0x00000001u, 0xffffffffu);
    EXPECT("ll unsigned max", "%llu", "18446744073709551615",
           0xffffffffu, 0xffffffffu);
    EXPECT("ll hex", "%llx", "123456789abcdef0", 0x9abcdef0u, 0x12345678u);
    EXPECT("j is 64-bit", "%jd", "-4294967295", 0x00000001u, 0xffffffffu);

    /* ---- signed/unsigned extrema ----------------------------------------- */
    EXPECT("int32 min", "%d", "-2147483648", 0x80000000u);
    EXPECT("int32 max", "%d", "2147483647", 0x7fffffffu);
    EXPECT("uint32 max", "%u", "4294967295", 0xffffffffu);
    EXPECT("int64 min", "%lld", "-9223372036854775808", 0x00000000u, 0x80000000u);
    EXPECT("int64 max", "%lld", "9223372036854775807", 0xffffffffu, 0x7fffffffu);
    EXPECT("uint32 hex max", "%x", "ffffffff", 0xffffffffu);
    EXPECT("uint32 octal max", "%o", "37777777777", 0xffffffffu);

    /* ll argument slots must not desynchronize the cursor. */
    EXPECT("ll keeps cursor aligned", "%d|%lld|%d", "1|-4294967295|2",
           1u, 0xdeadbeefu, 0x00000001u, 0xffffffffu, 2u);

    /* ---- floating conversions -------------------------------------------- */
    /* PSP long double is identical to double, so %Lf consumes one aligned
     * double and must never be widened to a host long double. */
    EXPECT("L is a PSP double", "%Lf", "12.500000", dbl_lo(12.5), dbl_hi(12.5));
    EXPECT("l before float", "%lf", "12.500000", dbl_lo(12.5), dbl_hi(12.5));
    EXPECT("exponent form", "%.2e", "1.25e+01", dbl_lo(12.5), dbl_hi(12.5));
    EXPECT("general form", "%g", "12.5", dbl_lo(12.5), dbl_hi(12.5));
    EXPECT("negative float", "%.2f", "-3.50", dbl_lo(-3.5), dbl_hi(-3.5));
    EXPECT("float plus flag", "%+.2f", "+3.50", dbl_lo(3.5), dbl_hi(3.5));
    EXPECT("float zero pad", "%08.2f", "00003.50", dbl_lo(3.5), dbl_hi(3.5));
    EXPECT("negative zero pad", "%08.2f", "-0003.50", dbl_lo(-3.5), dbl_hi(-3.5));
    EXPECT("float left justify", "%-8.2f|", "3.50    |", dbl_lo(3.5), dbl_hi(3.5));

    /* ---- flags, width, precision ----------------------------------------- */
    EXPECT("plus flag", "%+d", "+42", 42u);
    EXPECT("space flag", "% d", " 42", 42u);
    EXPECT("zero pad", "%05d", "00042", 42u);
    EXPECT("left justify", "%-5d|", "42   |", 42u);
    EXPECT("alt hex", "%#x", "0xff", 255u);
    EXPECT("alt HEX", "%#X", "0XFF", 255u);
    EXPECT("alt octal", "%#o", "010", 8u);
    EXPECT("alt hex of zero", "%#x", "0", 0u);
    EXPECT("precision zero of zero", "%.0d", "", 0u);
    EXPECT("precision zero of value", "%.0d", "5", 5u);
    EXPECT("precision then width", "%5.3d", "  007", 7u);
    EXPECT("zero flag ignored", "%05.3d", "  007", 7u);

    /* ---- dynamic width and precision -------------------------------------- */
    EXPECT("dynamic width", "%*d", "      42", 8u, 42u);
    EXPECT("dynamic width left", "%-*d|", "42      |", 8u, 42u);
    /* C99: a negative * width means the '-' flag with |width|. */
    EXPECT("negative dynamic width", "%*d|", "42      |", 0xfffffff8u, 42u);
    EXPECT("dynamic precision", "%.*d", "00042", 5u, 42u);
    /* C99: a negative * precision is as if precision were omitted. */
    EXPECT("negative dynamic precision", "%.*d", "42", 0xfffffffbu, 42u);
    EXPECT("dynamic width and precision", "%*.*f", "      3.14",
           10u, 2u, dbl_lo(3.14159), dbl_hi(3.14159));

    /* Width/precision beyond the supported bound are rejected deterministically
     * rather than silently clamped or handed to the host. */
    EXPECT("static width over bound", "%5000d", "%d", 42u);
    EXPECT("static precision over bound", "%.5000d", "%d", 42u);
    EXPECT("dynamic width over bound", "%*d", "%d", 100000u, 42u);
    EXPECT("dynamic precision over bound", "%.*d", "%d", 100000u, 42u);
    EXPECT("int32-min dynamic width", "%*d", "%d", 0x80000000u, 42u);

    /* ---- %c --------------------------------------------------------------- */
    EXPECT("char", "%c", "A", 65u);
    EXPECT("char width", "%3c", "  A", 65u);
    EXPECT("char left justify", "%-3c|", "A  |", 65u);
    EXPECT("char truncates to byte", "%c", "A", 0x12341841u);

    /* ---- %s --------------------------------------------------------------- */
    EXPECT("string precision", "%.3s", "hos", TEXT);
    EXPECT("string width", "%8s", "   host0", TEXT);
    EXPECT("string left justify", "%-8s|", "host0   |", TEXT);
    EXPECT("null string", "%s", "(null)", 0u);
    EXPECT("out-of-range string", "%s", "(null)", 0x0c000000u);

    /* ---- %p --------------------------------------------------------------- */
    EXPECT("pointer", "%p", "0xdeadbeef", 0xdeadbeefu);
    EXPECT("null pointer", "%p", "0x00000000", 0u);

    /* ---- guest strings at the arena span boundary -------------------------- */
    /* A string whose terminator is the last addressable guest byte. */
    put_string(GUEST_LAST - 2u, "AB");
    EXPECT("string ends at arena end", "%s", "AB", GUEST_LAST - 2u);

    /* An unterminated string running into the end of the arena must stop at the
     * boundary through the checked accessor, not walk past the allocation. */
    MEM_W8(GUEST_LAST, (uint8_t)'C');
    g_oor_events = 0;
    EXPECT("unterminated at arena end", "%s", "C", GUEST_LAST);
    if (g_oor_events == 0) {
        failures++;
        fprintf(stderr, "FAIL %-30s expected a bounds rejection at the arena end\n",
                "unterminated at arena end");
    }
    MEM_W8(GUEST_LAST, 0u);

    /* ---- %n --------------------------------------------------------------- */
    MEM_W32(COUNTER, 0xa5a55a5au);
    EXPECT("count write", "1234%n", "1234", COUNTER);
    check_u32("count value", MEM_R32(COUNTER), 4u);

    MEM_W32(COUNTER, 0xa5a55a5au);
    EXPECT("half count write", "1234%hn", "1234", COUNTER);
    /* Sentinel 0xa5a55a5a is 5a 5a a5 a5 in memory; a 16-bit store touches only
     * the first two bytes. */
    check_u32("half count value", MEM_R32(COUNTER), 0xa5a50004u);

    MEM_W32(COUNTER, 0xa5a55a5au);
    EXPECT("byte count write", "1234%hhn", "1234", COUNTER);
    check_u32("byte count value", MEM_R32(COUNTER), 0xa5a55a04u);

    MEM_W32(COUNTER, 0xa5a55a5au);
    MEM_W32(COUNTER + 4u, 0xa5a55a5au);
    EXPECT("long long count write", "1234%lln", "1234", COUNTER);
    check_u32("ll count low", MEM_R32(COUNTER), 4u);
    check_u32("ll count high", MEM_R32(COUNTER + 4u), 0u);

    /* %n takes a pointer, which is one word regardless of the modifier; the
     * following conversion must still find its own argument. */
    MEM_W32(COUNTER, 0u);
    EXPECT("lln consumes one word", "%lln%d", "77", COUNTER, 77u);

    /* C99 forbids flags/width/precision on %n.  It is the only conversion that
     * writes guest memory, so an out-of-grammar %n is fail-closed. */
    MEM_W32(COUNTER, 0xa5a55a5au);
    EXPECT("flagged count rejected", "1234%0n", "1234%n", COUNTER);
    check_u32("flagged count untouched", MEM_R32(COUNTER), 0xa5a55a5au);

    MEM_W32(COUNTER, 0xa5a55a5au);
    EXPECT("width count rejected", "1234%5n", "1234%n", COUNTER);
    check_u32("width count untouched", MEM_R32(COUNTER), 0xa5a55a5au);

    EXPECT0("null count pointer", "%n", "");
    EXPECT("out-of-range count pointer", "%n", "", 0x0c000000u);

    /* ---- malformed and unsupported grammar --------------------------------- */
    /* Repeated length modifiers are not a valid modifier; the guest must never be
     * able to name a host variadic type. */
    EXPECT("repeated h", "%hhhhd", "%d", 7u);
    EXPECT("repeated l", "%lllld", "%d", 7u);
    EXPECT("mixed l and L", "%lLf", "%f", dbl_lo(12.5), dbl_hi(12.5));
    EXPECT("h on float", "%hf", "%f", dbl_lo(12.5), dbl_hi(12.5));
    EXPECT("z on float", "%zf", "%f", dbl_lo(12.5), dbl_hi(12.5));
    EXPECT("L on integer", "%Ld", "%d", 7u);
    EXPECT("l on string", "%ls", "%s", TEXT);
    EXPECT("l on char", "%lc", "%c", 65u);
    EXPECT("l on pointer", "%lp", "%p", 0xdeadbeefu);
    EXPECT("unsupported conversion", "%q", "%q", 0x11223344u);

    /* A rejected spec still consumes exactly its own argument. */
    EXPECT("cursor synced after reject", "%hhhhd|%d", "%d|8", 7u, 8u);
    EXPECT("cursor synced after float reject", "%hf|%d", "%f|9",
           dbl_lo(12.5), dbl_hi(12.5), 9u);

    /* A truncated spec at the end of the format emits nothing further. */
    EXPECT0("trailing percent", "abc%", "abc");
    EXPECT0("trailing spec", "abc%-5.2l", "abc");

    /* A long flag run is now formatted normally.  The previous implementation
     * assembled flags into a fixed-size host format buffer and fell back to
     * "%d" once that buffer filled; that limit was an artifact of host format
     * assembly, not PSP behavior.  newlib treats repeated flags as idempotent. */
    {
        char hostile[64];
        make_repeated(hostile, '0', 29, "d");
        EXPECT("repeated zero flag", hostile, "11", 11u);
        make_repeated(hostile, '0', 29, "s");
        EXPECT("repeated zero flag on string", hostile, "host0", TEXT);
        make_repeated(hostile, '0', 29, "f");
        EXPECT("repeated zero flag on float", hostile, "12.500000",
               dbl_lo(12.5), dbl_hi(12.5));
    }

    /* ---- missing arguments -------------------------------------------------- */
    /* More conversions than the caller supplied: the bridge keeps reading the
     * ABI word sequence.  Register words are zero here and the stack
     * continuation is read through the checked accessor. */
    EXPECT0("missing register arguments", "%d %d %d %d %d %d %d %d",
            "0 0 0 0 0 0 0 0");

    /* Stack continuation past the end of the arena must be refused by the
     * checked accessor rather than read out of the host allocation. */
    memset(SR_HOST(DST), 0, 0x200u);
    put_string(FMT, "%d %d %d %d %d %d %d %d");
    memset(cpu.r, 0, sizeof cpu.r);
    cpu.r[4] = DST;
    cpu.r[5] = FMT;
    cpu.r[29] = 0x0c000000u;   /* every stack word lies past the arena */
    cpu.r[31] = 0x12345678u;
    g_oor_events = 0;
    sr_guest_sprintf(&cpu);
    if (strcmp((const char *)SR_HOST(DST), "0 0 0 0 0 0 0 0") != 0) {
        failures++;
        fprintf(stderr, "FAIL %-30s got=\"%s\"\n", "out-of-arena stack args",
                (const char *)SR_HOST(DST));
    }
    if (g_oor_events == 0) {
        failures++;
        fprintf(stderr, "FAIL %-30s expected a bounds rejection past the arena\n",
                "out-of-arena stack args");
    }

    /* ---- output length ------------------------------------------------------ */
    EXPECT("return counts padding", "%20d", "                  42", 42u);

    if (failures != 0) {
        fprintf(stderr, "guest printf selftest: %d failure(s)\n", failures);
        free(arena);
        return 1;
    }

    puts("guest printf selftest: OK");
    free(arena);
    return 0;
}
