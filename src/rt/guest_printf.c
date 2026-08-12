// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Focused PSP-EABI bridge for the retail sprintf entry.  The translated
 * formatter previously corrupted string arguments, so generated code routes
 * that entry here; the bridge must also preserve floating conversions used by
 * UI animation code.  Keep the ABI word cursor explicit: PSP EABI supplies r4
 * through r11, leaving r6..r11 for sprintf's variadic words before the stack. */

#include "recomp.h"

static uint32_t guest_printf_next_word(CpuState *s, uint32_t entry_sp,
                                       uint32_t *argi) {
    uint32_t index = (*argi)++;
    return index < 6u ? s->r[6u + index]
                      : MEM_R32(entry_sp + 4u * (index - 6u));
}

static double guest_printf_next_double(CpuState *s, uint32_t entry_sp,
                                       uint32_t *argi) {
    /* PSP EABI aligns a double to an even argument-word slot.  argi zero is
     * r6, itself even; the stack continuation preserves the same alignment. */
    if ((*argi & 1u) != 0u) (*argi)++;
    uint64_t bits = guest_printf_next_word(s, entry_sp, argi);
    bits |= (uint64_t)guest_printf_next_word(s, entry_sp, argi) << 32;
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* Append a non-conversion byte while reserving one byte for the eventual
 * conversion character and one byte for the terminating NUL. */
static int guest_printf_append_format_char(char *conv, size_t cap, int *ci, char ch) {
    if (*ci < 0 || (size_t)*ci + 2u >= cap) return 0;
    conv[(*ci)++] = ch;
    return 1;
}

/* snprintf() returns the number of bytes that would have been written. Format
 * a dynamic width/precision into a temporary that is always large enough for
 * int32_t decimal text, then copy only if the complete token fits while still
 * reserving conversion+NUL space in conv[]. */
static int guest_printf_append_decimal(char *conv, size_t cap, int *ci, int value) {
    if (*ci < 0 || (size_t)*ci + 2u >= cap) return 0;
    char token[16];
    int n = snprintf(token, sizeof(token), "%d", value);
    if (n < 0 || (size_t)n >= sizeof(token)) return 0;
    size_t remaining = cap - (size_t)*ci;
    if ((size_t)n > remaining - 2u) return 0;
    memcpy(conv + *ci, token, (size_t)n);
    *ci += n;
    return 1;
}

void sr_guest_sprintf(CpuState *s) {
    uint32_t dst0 = s->r[4], dst = dst0, fmt = s->r[5], argi = 0;
    uint32_t entry_sp = s->r[29];
    int total = 0;
#define NEXT_WORD() guest_printf_next_word(s, entry_sp, &argi)
#define PUT_CHAR(ch) do { MEM_W8(dst++, (uint8_t)(ch)); total++; } while (0)
    while (MEM_R8(fmt) != 0u) {
        uint8_t ch = MEM_R8(fmt++);
        if (ch != '%') { PUT_CHAR(ch); continue; }
        if (MEM_R8(fmt) == '%') { fmt++; PUT_CHAR('%'); continue; }

        char conv[32], tmp[128];
        int ci = 0, long_double = 0, format_ok = 1;
        conv[ci++] = '%';
        for (;;) {
            ch = MEM_R8(fmt);
            if (ch != '-' && ch != '+' && ch != ' ' && ch != '#' && ch != '0') break;
            if (format_ok && !guest_printf_append_format_char(conv, sizeof(conv), &ci, (char)ch))
                format_ok = 0;
            fmt++;
        }
        if (MEM_R8(fmt) == '*') {
            int width = (int32_t)NEXT_WORD();
            if (format_ok && !guest_printf_append_decimal(conv, sizeof(conv), &ci, width))
                format_ok = 0;
            fmt++;
        } else {
            while (MEM_R8(fmt) >= '0' && MEM_R8(fmt) <= '9') {
                if (format_ok && !guest_printf_append_format_char(
                        conv, sizeof(conv), &ci, (char)MEM_R8(fmt)))
                    format_ok = 0;
                fmt++;
            }
        }
        if (MEM_R8(fmt) == '.') {
            if (format_ok && !guest_printf_append_format_char(conv, sizeof(conv), &ci, '.'))
                format_ok = 0;
            fmt++;
            if (MEM_R8(fmt) == '*') {
                int precision = (int32_t)NEXT_WORD();
                if (format_ok && !guest_printf_append_decimal(conv, sizeof(conv), &ci, precision))
                    format_ok = 0;
                fmt++;
            } else {
                while (MEM_R8(fmt) >= '0' && MEM_R8(fmt) <= '9') {
                    if (format_ok && !guest_printf_append_format_char(
                            conv, sizeof(conv), &ci, (char)MEM_R8(fmt)))
                        format_ok = 0;
                    fmt++;
                }
            }
        }
        while (MEM_R8(fmt) == 'h' || MEM_R8(fmt) == 'l' || MEM_R8(fmt) == 'L' ||
               MEM_R8(fmt) == 'z' || MEM_R8(fmt) == 't') {
            ch = MEM_R8(fmt++);
            if (ch == 'L') long_double = 1;
            if (format_ok && !guest_printf_append_format_char(conv, sizeof(conv), &ci, (char)ch))
                format_ok = 0;
        }
        ch = MEM_R8(fmt);
        if (ch == 0u) break;
        fmt++;
        if (format_ok && (size_t)ci + 1u < sizeof(conv)) {
            conv[ci++] = (char)ch;
            conv[ci] = '\0';
        } else {
            format_ok = 0;
            conv[0] = '%'; conv[1] = (char)ch; conv[2] = '\0';
        }
        tmp[0] = '\0';

        int is_float = ch == 'f' || ch == 'F' || ch == 'e' || ch == 'E' ||
                       ch == 'g' || ch == 'G' || ch == 'a' || ch == 'A';
        if (!format_ok) {
            /* Preserve double alignment and the current one-word fallback
             * cursor without handing a truncated host format to snprintf.
             * Length-modified integer semantics remain a separate concern. */
            if (is_float) (void)guest_printf_next_double(s, entry_sp, &argi);
            else (void)NEXT_WORD();
            tmp[0] = '%'; tmp[1] = (char)ch; tmp[2] = '\0';
        } else if (is_float) {
            double value = guest_printf_next_double(s, entry_sp, &argi);
            if (long_double)
                snprintf(tmp, sizeof(tmp), conv, (long double)value);
            else
                snprintf(tmp, sizeof(tmp), conv, value);
        } else {
            uint32_t value = NEXT_WORD();
            if (ch == 's') {
                /* Width and precision for strings are uncommon on this path.
                 * Copy ordinary guest strings without exposing host pointers. */
                uint32_t p = value;
                if (p != 0u && (p & 0x1fffffffu) < 0x0c000000u) {
                    for (uint32_t n = 0; n < 0x00100000u && MEM_R8(p + n) != 0u; n++)
                        PUT_CHAR(MEM_R8(p + n));
                } else {
                    const char *null_text = "(null)";
                    for (int n = 0; null_text[n]; n++) PUT_CHAR(null_text[n]);
                }
                continue;
            } else if (ch == 'c') {
                tmp[0] = (char)(value & 0xffu); tmp[1] = '\0';
            } else if (ch == 'd' || ch == 'i') {
                snprintf(tmp, sizeof(tmp), conv, (int32_t)value);
            } else if (ch == 'u' || ch == 'o' || ch == 'x' || ch == 'X') {
                snprintf(tmp, sizeof(tmp), conv, value);
            } else if (ch == 'p') {
                snprintf(tmp, sizeof(tmp), "0x%08x", value);
            } else if (ch == 'n') {
                if (value != 0u && (value & 0x1fffffffu) < 0x0c000000u)
                    MEM_W32(value, (uint32_t)total);
                continue;
            } else {
                /* Preserve unsupported conversions visibly without handing a
                 * mismatched host varargs type to snprintf. */
                tmp[0] = '%'; tmp[1] = (char)ch; tmp[2] = '\0';
            }
        }
        for (int n = 0; tmp[n] != '\0'; n++) PUT_CHAR(tmp[n]);
    }
    MEM_W8(dst, 0u);
    s->r[2] = (uint32_t)total;
    s->pc = s->r[31];
#undef PUT_CHAR
#undef NEXT_WORD
}
