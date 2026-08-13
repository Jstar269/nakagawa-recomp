// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Focused PSP-EABI bridge for the retail sprintf entry.  The translated
 * formatter previously corrupted string arguments, so generated code routes
 * that entry here; the bridge must also preserve floating conversions used by
 * UI animation code.  Keep the ABI word cursor explicit: PSP EABI supplies r4
 * through r11, leaving r6..r11 for sprintf's variadic words before the stack.
 *
 * Host-format safety contract
 * ---------------------------
 * The guest owns every byte of the format string.  This bridge therefore never
 * assembles guest bytes into a host printf-family format.  The format string is
 * parsed into an explicit `gp_spec` (flags, width, precision, length modifier,
 * conversion), and every host `snprintf()` call in this file uses a
 * *compile-time string literal* whose conversion and length modifier are fixed
 * by that literal, with an argument whose C type matches it by construction.
 * Integer, character, string, pointer and `%n` conversions are rendered without
 * any host printf call at all.  Floating conversions call `snprintf()` with a
 * literal that carries no length modifier, so the host argument type is exactly
 * `double`; width, precision, sign and padding are then applied by this file.
 * A guest length modifier can therefore never select a host variadic type.
 *
 * PSP ABI notes (deliberately independent of host C type sizes)
 * ------------------------------------------------------------
 * PSP MIPS o32/EABI: `int`, `long`, `size_t`, `ptrdiff_t` and pointers are all
 * 32 bits and occupy one argument word.  `long long` and `intmax_t` are 64 bits
 * and occupy two words starting at an even word slot.  `long double` is
 * identical to `double` (64 bits), so `%Lf` consumes one aligned double and is
 * rendered as a double -- it must never be widened to a host `long double`.
 * Host `long`, `size_t`, `ptrdiff_t` and `long double` are irrelevant here and
 * are never used to carry a guest value.
 *
 * Out-of-grammar behavior is deterministic: the spec's argument is still
 * consumed (so the variadic cursor stays synchronized for the rest of the
 * format) and the literal text `%<conversion>` is emitted.  See gp_reject().
 */

#include "recomp.h"

#include <limits.h>

/* Bounds on guest-supplied field width and precision.  These cap the work a
 * single conversion can demand; padding is written straight to guest memory, so
 * an unbounded width would otherwise be an effective hang.  A spec exceeding
 * either bound is rejected through the deterministic gp_reject() path rather
 * than silently clamped, so the output never misrepresents the request. */
#define GP_MAX_WIDTH 4096
#define GP_MAX_PREC  1024

/* Large enough for the longest bounded floating body this file can request:
 * sign + 309 integer digits (DBL_MAX) + '.' + GP_MAX_PREC fraction digits + NUL. */
#define GP_FLOAT_BUF 1440

/* Upper bound on a guest %s walk, preserved from the previous implementation. */
#define GP_MAX_STRING 0x00100000u

typedef enum {
    GP_LEN_NONE = 0,
    GP_LEN_CHAR,      /* hh */
    GP_LEN_SHORT,     /* h  */
    GP_LEN_LONG,      /* l  -- 32-bit on PSP */
    GP_LEN_LLONG,     /* ll -- 64-bit, two words */
    GP_LEN_INTMAX,    /* j  -- 64-bit, two words */
    GP_LEN_SIZE,      /* z  -- 32-bit on PSP */
    GP_LEN_PTRDIFF,   /* t  -- 32-bit on PSP */
    GP_LEN_LDOUBLE    /* L  -- identical to double on PSP */
} gp_len;

typedef struct {
    unsigned left  : 1;   /* '-' */
    unsigned plus  : 1;   /* '+' */
    unsigned space : 1;   /* ' ' */
    unsigned alt   : 1;   /* '#' */
    unsigned zero  : 1;   /* '0' */
    int width;            /* >= 0 */
    int precision;        /* < 0 when absent */
    gp_len len;
    char conv;
} gp_spec;

/* ---------------------------------------------------------------- ABI cursor */

static uint32_t guest_printf_next_word(CpuState *s, uint32_t entry_sp,
                                       uint32_t *argi) {
    uint32_t index = (*argi)++;
    return index < 6u ? s->r[6u + index]
                      : MEM_R32(entry_sp + 4u * (index - 6u));
}

/* PSP EABI aligns a 64-bit argument to an even argument-word slot.  argi zero is
 * r6, itself even; the stack continuation preserves the same alignment. */
static uint64_t guest_printf_next_dword(CpuState *s, uint32_t entry_sp,
                                        uint32_t *argi) {
    if ((*argi & 1u) != 0u) (*argi)++;
    uint64_t bits = guest_printf_next_word(s, entry_sp, argi);
    bits |= (uint64_t)guest_printf_next_word(s, entry_sp, argi) << 32;
    return bits;
}

static double guest_printf_next_double(CpuState *s, uint32_t entry_sp,
                                       uint32_t *argi) {
    uint64_t bits = guest_printf_next_dword(s, entry_sp, argi);
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* ---------------------------------------------------------------- output sink */

typedef struct {
    uint32_t dst;
    int total;
} gp_out;

/* The PSP return value is an int.  Saturate rather than overflow it; signed
 * overflow would be host UB and the guest cannot represent more anyway. */
static void gp_put(gp_out *o, char c) {
    MEM_W8(o->dst, (uint8_t)c);
    o->dst++;
    if (o->total < INT_MAX) o->total++;
}

static void gp_put_n(gp_out *o, const char *p, int n) {
    for (int i = 0; i < n; i++) gp_put(o, p[i]);
}

static void gp_put_rev(gp_out *o, const char *p, int n) {
    for (int i = n; i-- > 0;) gp_put(o, p[i]);
}

static void gp_pad(gp_out *o, char c, int n) {
    for (int i = 0; i < n; i++) gp_put(o, c);
}

/* Deterministic out-of-grammar output.  The caller has already consumed this
 * spec's argument, so only the visible text is produced here. */
static void gp_reject(gp_out *o, char conv) {
    gp_put(o, '%');
    if (conv != '\0') gp_put(o, conv);
}

/* ---------------------------------------------------------------- integer path */

/* Render an integer entirely without a host printf call, so no host variadic
 * type is ever selected by guest syntax. */
static void gp_emit_int(gp_out *o, const gp_spec *sp, uint64_t mag,
                        int is_signed, int negative, unsigned base, int upper) {
    /* Base 8 of UINT64_MAX is 22 digits; 24 covers every supported base. */
    char digits[24];
    int ndig = 0;
    const char *tab = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int nonzero = (mag != 0u);

    if (mag == 0u) {
        /* C: precision 0 with value 0 produces no digits at all. */
        if (sp->precision != 0) digits[ndig++] = '0';
    } else {
        while (mag != 0u) {
            digits[ndig++] = tab[mag % base];
            mag /= base;
        }
    }

    /* C99: '+' and ' ' apply only to signed conversions. */
    char prefix[2] = { 0, 0 };
    int npre = 0;
    if (negative)                     prefix[npre++] = '-';
    else if (is_signed && sp->plus)   prefix[npre++] = '+';
    else if (is_signed && sp->space)  prefix[npre++] = ' ';

    char alt_prefix[2] = { 0, 0 };
    int nalt = 0;
    /* C99: '#' prefixes 0x/0X only when the value itself is nonzero. */
    if (sp->alt && base == 16u && nonzero) {
        alt_prefix[nalt++] = '0';
        alt_prefix[nalt++] = upper ? 'X' : 'x';
    }

    int zeros = (sp->precision > ndig) ? sp->precision - ndig : 0;
    /* '#' on octal forces at least one leading zero. */
    if (sp->alt && base == 8u && zeros == 0 &&
        (ndig == 0 || digits[ndig - 1] != '0')) {
        zeros = 1;
    }

    int body = npre + nalt + zeros + ndig;
    int pad = (sp->width > body) ? sp->width - body : 0;

    /* '0' padding is ignored when an explicit precision is present (C99). */
    int zero_pad = (!sp->left && sp->zero && sp->precision < 0);

    if (sp->left) {
        gp_put_n(o, prefix, npre);
        gp_put_n(o, alt_prefix, nalt);
        gp_pad(o, '0', zeros);
        gp_put_rev(o, digits, ndig);
        gp_pad(o, ' ', pad);
    } else if (zero_pad) {
        gp_put_n(o, prefix, npre);
        gp_put_n(o, alt_prefix, nalt);
        gp_pad(o, '0', pad);
        gp_pad(o, '0', zeros);
        gp_put_rev(o, digits, ndig);
    } else {
        gp_pad(o, ' ', pad);
        gp_put_n(o, prefix, npre);
        gp_put_n(o, alt_prefix, nalt);
        gp_pad(o, '0', zeros);
        gp_put_rev(o, digits, ndig);
    }
}

/* ---------------------------------------------------------------- float path */

static int gp_is_finite(double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    return ((bits >> 52) & 0x7ffu) != 0x7ffu;
}

/* Every branch here uses a compile-time literal format whose only argument
 * types are (int, double) or (double).  -Wformat fully checks these; no guest
 * byte reaches the format string. */
#define GP_FLOAT_CASE(ch, lit)                                            \
    case ch:                                                              \
        if (prec < 0)                                                     \
            return alt ? snprintf(buf, cap, "%#" lit, v)                  \
                       : snprintf(buf, cap, "%" lit, v);                  \
        return alt ? snprintf(buf, cap, "%#.*" lit, prec, v)              \
                   : snprintf(buf, cap, "%.*" lit, prec, v);

static int gp_float_body(char *buf, size_t cap, const gp_spec *sp, double v) {
    int prec = sp->precision;
    int alt = sp->alt;
    switch (sp->conv) {
        GP_FLOAT_CASE('f', "f")
        GP_FLOAT_CASE('F', "F")
        GP_FLOAT_CASE('e', "e")
        GP_FLOAT_CASE('E', "E")
        GP_FLOAT_CASE('g', "g")
        GP_FLOAT_CASE('G', "G")
        GP_FLOAT_CASE('a', "a")
        GP_FLOAT_CASE('A', "A")
    default:
        return -1;
    }
}

#undef GP_FLOAT_CASE

static void gp_emit_float(gp_out *o, const gp_spec *sp, double v) {
    char buf[GP_FLOAT_BUF];
    int n = gp_float_body(buf, sizeof buf, sp, v);
    if (n < 0 || (size_t)n >= sizeof buf) { gp_reject(o, sp->conv); return; }

    /* The host produced the digits and any '-'; this file owns sign policy,
     * field width and padding. */
    const char *body = buf;
    char sign = '\0';
    if (buf[0] == '-' || buf[0] == '+') {
        sign = buf[0];
        body = buf + 1;
        n -= 1;
    } else if (sp->plus) {
        sign = '+';
    } else if (sp->space) {
        sign = ' ';
    }

    int body_len = n + (sign != '\0' ? 1 : 0);
    int pad = (sp->width > body_len) ? sp->width - body_len : 0;
    /* Infinities and NaNs are never zero-padded. */
    int zero_pad = (!sp->left && sp->zero && gp_is_finite(v));

    if (sp->left) {
        if (sign) gp_put(o, sign);
        gp_put_n(o, body, n);
        gp_pad(o, ' ', pad);
    } else if (zero_pad) {
        if (sign) gp_put(o, sign);
        gp_pad(o, '0', pad);
        gp_put_n(o, body, n);
    } else {
        gp_pad(o, ' ', pad);
        if (sign) gp_put(o, sign);
        gp_put_n(o, body, n);
    }
}

/* ---------------------------------------------------------------- parsing */

static int gp_is_len_char(uint8_t c) {
    return c == 'h' || c == 'l' || c == 'j' || c == 'z' || c == 't' || c == 'L';
}

/* Consume the whole run of length-modifier characters and accept only an exact
 * standard modifier.  `%hhhhd` and `%lLf` are rejected rather than handed to the
 * host in any form. */
static gp_len gp_parse_len(uint32_t *fmt, int *ok) {
    char m[2];
    int n = 0;
    while (gp_is_len_char(MEM_R8(*fmt))) {
        if (n < 2) m[n] = (char)MEM_R8(*fmt);
        n++;
        (*fmt)++;
    }
    if (n == 0) return GP_LEN_NONE;
    if (n == 1) {
        switch (m[0]) {
        case 'h': return GP_LEN_SHORT;
        case 'l': return GP_LEN_LONG;
        case 'j': return GP_LEN_INTMAX;
        case 'z': return GP_LEN_SIZE;
        case 't': return GP_LEN_PTRDIFF;
        case 'L': return GP_LEN_LDOUBLE;
        default:  break;
        }
    } else if (n == 2) {
        if (m[0] == 'h' && m[1] == 'h') return GP_LEN_CHAR;
        if (m[0] == 'l' && m[1] == 'l') return GP_LEN_LLONG;
    }
    *ok = 0;
    return GP_LEN_NONE;
}

/* Parse a decimal run.  An absent run yields 0, which is the correct value for
 * both an omitted width and a bare '.' precision.  `*out` is set to -1 when the
 * value exceeds `limit`, which the caller treats as out of grammar. */
static void gp_parse_decimal(uint32_t *fmt, int limit, int *out) {
    long v = 0;
    while (MEM_R8(*fmt) >= '0' && MEM_R8(*fmt) <= '9') {
        if (v <= (long)limit) v = v * 10 + (long)(MEM_R8(*fmt) - '0');
        (*fmt)++;
    }
    *out = (v > (long)limit) ? -1 : (int)v;
}

static int gp_len_allowed(gp_len len, char conv) {
    switch (conv) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
    case 'n':
        return len != GP_LEN_LDOUBLE;
    case 'f': case 'F': case 'e': case 'E':
    case 'g': case 'G': case 'a': case 'A':
        /* C99 permits `l` before a floating conversion with no effect; `L` is a
         * PSP long double, which is a double. */
        return len == GP_LEN_NONE || len == GP_LEN_LONG || len == GP_LEN_LDOUBLE;
    case 'c': case 's': case 'p':
        return len == GP_LEN_NONE;
    default:
        return 0;
    }
}

static int gp_is_float_conv(char c) {
    return c == 'f' || c == 'F' || c == 'e' || c == 'E' ||
           c == 'g' || c == 'G' || c == 'a' || c == 'A';
}

/* A 64-bit guest integer occupies two aligned words; everything else is one. */
static int gp_len_is_64bit(gp_len len) {
    return len == GP_LEN_LLONG || len == GP_LEN_INTMAX;
}

/* ---------------------------------------------------------------- entry point */

void sr_guest_sprintf(CpuState *s) {
    uint32_t dst0 = s->r[4], fmt = s->r[5], argi = 0;
    uint32_t entry_sp = s->r[29];
    gp_out out = { dst0, 0 };

    while (MEM_R8(fmt) != 0u) {
        uint8_t ch = MEM_R8(fmt++);
        if (ch != '%') { gp_put(&out, (char)ch); continue; }
        if (MEM_R8(fmt) == '%') { fmt++; gp_put(&out, '%'); continue; }

        gp_spec sp = { 0, 0, 0, 0, 0, 0, -1, GP_LEN_NONE, '\0' };
        int ok = 1;

        for (;;) {
            ch = MEM_R8(fmt);
            if (ch == '-')      sp.left = 1;
            else if (ch == '+') sp.plus = 1;
            else if (ch == ' ') sp.space = 1;
            else if (ch == '#') sp.alt = 1;
            else if (ch == '0') sp.zero = 1;
            else break;
            fmt++;
        }

        if (MEM_R8(fmt) == '*') {
            fmt++;
            int32_t w = (int32_t)guest_printf_next_word(s, entry_sp, &argi);
            /* C99: a negative `*` width means the '-' flag with |width|. */
            if (w < 0) {
                sp.left = 1;
                if (w == INT32_MIN) ok = 0; else w = -w;
            }
            if (w > GP_MAX_WIDTH) ok = 0;
            sp.width = ok ? (int)w : 0;
        } else {
            int w = 0;
            gp_parse_decimal(&fmt, GP_MAX_WIDTH, &w);
            if (w < 0) ok = 0; else sp.width = w;
        }

        if (MEM_R8(fmt) == '.') {
            fmt++;
            if (MEM_R8(fmt) == '*') {
                fmt++;
                int32_t p = (int32_t)guest_printf_next_word(s, entry_sp, &argi);
                /* C99: a negative `*` precision is as if precision were omitted. */
                if (p < 0)                 sp.precision = -1;
                else if (p > GP_MAX_PREC)  ok = 0;
                else                       sp.precision = (int)p;
            } else {
                int p = 0;
                /* A bare '.' means precision zero. */
                gp_parse_decimal(&fmt, GP_MAX_PREC, &p);
                if (p < 0) ok = 0; else sp.precision = p;
            }
        }

        sp.len = gp_parse_len(&fmt, &ok);

        ch = MEM_R8(fmt);
        if (ch == 0u) break;   /* truncated spec at end of format */
        fmt++;
        sp.conv = (char)ch;

        if (!gp_len_allowed(sp.len, sp.conv)) ok = 0;
        /* C99: no flags, width, or precision may be specified for %n.  It is the
         * only conversion that writes guest memory, so it is fail-closed. */
        if (sp.conv == 'n' &&
            (sp.left || sp.plus || sp.space || sp.alt || sp.zero ||
             sp.width != 0 || sp.precision >= 0)) {
            ok = 0;
        }

        /* Consume this spec's argument exactly once, sized by the PSP ABI, and
         * do it whether or not the spec is well formed so the cursor stays
         * synchronized for every later conversion. */
        int wants_double = gp_is_float_conv(sp.conv);
        /* %n always takes a pointer, which is one 32-bit word on PSP; its length
         * modifier sizes the *write*, not the argument. */
        int wants_64bit = !wants_double && sp.conv != 'n' &&
                          gp_len_is_64bit(sp.len);
        double dval = 0.0;
        uint64_t uval = 0;
        if (wants_double) {
            dval = guest_printf_next_double(s, entry_sp, &argi);
        } else if (wants_64bit) {
            uval = guest_printf_next_dword(s, entry_sp, &argi);
        } else {
            uval = guest_printf_next_word(s, entry_sp, &argi);
        }

        if (!ok) { gp_reject(&out, sp.conv); continue; }

        switch (sp.conv) {
        case 'd': case 'i': {
            int64_t v;
            switch (sp.len) {
            case GP_LEN_CHAR:  v = (int8_t)(uval & 0xffu); break;
            case GP_LEN_SHORT: v = (int16_t)(uval & 0xffffu); break;
            case GP_LEN_LLONG:
            case GP_LEN_INTMAX: v = (int64_t)uval; break;
            /* PSP long/size_t/ptrdiff_t are 32-bit, like int. */
            default: v = (int32_t)(uint32_t)uval; break;
            }
            uint64_t mag = (v < 0) ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            gp_emit_int(&out, &sp, mag, 1, v < 0, 10u, 0);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v;
            switch (sp.len) {
            case GP_LEN_CHAR:  v = uval & 0xffu; break;
            case GP_LEN_SHORT: v = uval & 0xffffu; break;
            case GP_LEN_LLONG:
            case GP_LEN_INTMAX: v = uval; break;
            default: v = uval & 0xffffffffu; break;
            }
            unsigned base = (sp.conv == 'o') ? 8u
                          : (sp.conv == 'u') ? 10u : 16u;
            gp_emit_int(&out, &sp, v, 0, 0, base, sp.conv == 'X');
            break;
        }
        case 'f': case 'F': case 'e': case 'E':
        case 'g': case 'G': case 'a': case 'A':
            gp_emit_float(&out, &sp, dval);
            break;
        case 'c': {
            char c = (char)(uval & 0xffu);
            int pad = (sp.width > 1) ? sp.width - 1 : 0;
            if (sp.left) { gp_put(&out, c); gp_pad(&out, ' ', pad); }
            else         { gp_pad(&out, ' ', pad); gp_put(&out, c); }
            break;
        }
        case 's': {
            uint32_t p = (uint32_t)uval;
            uint32_t max = (sp.precision >= 0) ? (uint32_t)sp.precision
                                               : GP_MAX_STRING;
            /* An implausible guest pointer prints the same placeholder the
             * previous bridge used, and is padded like any other string. */
            static const char null_text[] = "(null)";
            int valid = (p != 0u) && sr_inrange(p);
            int len = 0;
            if (valid) {
                while ((uint32_t)len < max && (uint32_t)len < GP_MAX_STRING &&
                       MEM_R8(p + (uint32_t)len) != 0u) {
                    len++;
                }
            } else {
                len = (int)(sizeof(null_text) - 1u);
                if ((uint32_t)len > max) len = (int)max;
            }
            int pad = (sp.width > len) ? sp.width - len : 0;
            if (!sp.left) gp_pad(&out, ' ', pad);
            for (int i = 0; i < len; i++) {
                gp_put(&out, valid ? (char)MEM_R8(p + (uint32_t)i)
                                   : null_text[i]);
            }
            if (sp.left) gp_pad(&out, ' ', pad);
            break;
        }
        case 'p': {
            /* Preserved verbatim from the previous bridge: "0x" followed by
             * eight lowercase zero-padded hex digits, ignoring flags/width. */
            gp_put(&out, '0');
            gp_put(&out, 'x');
            for (int shift = 28; shift >= 0; shift -= 4) {
                gp_put(&out, "0123456789abcdef"[(uval >> shift) & 0xfu]);
            }
            break;
        }
        case 'n': {
            uint32_t p = (uint32_t)uval;
            uint32_t count = (uint32_t)out.total;
            uint32_t width = 4u;
            switch (sp.len) {
            case GP_LEN_CHAR:  width = 1u; break;
            case GP_LEN_SHORT: width = 2u; break;
            case GP_LEN_LLONG:
            case GP_LEN_INTMAX: width = 8u; break;
            default: width = 4u; break;
            }
            /* Preflight the whole destination span before any partial write. */
            if (p != 0u && sr_guest_span_writable(p, width)) {
                switch (width) {
                case 1u: MEM_W8(p, (uint8_t)count); break;
                case 2u: MEM_W16(p, (uint16_t)count); break;
                case 4u: MEM_W32(p, count); break;
                default:
                    MEM_W32(p, count);
                    MEM_W32(p + 4u, 0u);
                    break;
                }
            }
            break;
        }
        default:
            /* Unsupported conversion; argument already consumed above. */
            gp_reject(&out, sp.conv);
            break;
        }
    }

    MEM_W8(out.dst, 0u);
    s->r[2] = (uint32_t)out.total;
    s->pc = s->r[31];
}
