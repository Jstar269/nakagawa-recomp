// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Focused PSP-EABI bridge for the retail sprintf entry.  The translated
 * formatter previously corrupted string arguments, so generated code routes
 * that entry here; the bridge must also preserve floating conversions used by
 * UI animation code.
 *
 * Host-format safety contract
 * ---------------------------
 * The guest owns every byte of the format string.  This bridge therefore never
 * assembles guest bytes into a host printf-family format.  The format string is
 * parsed into an explicit `gp_spec`, and every host `snprintf()` call in this
 * file uses a *compile-time string literal* whose conversion and length
 * modifier are fixed by that literal, with an argument whose C type matches it
 * by construction.  Integer, character, string, pointer and `%n` conversions
 * are rendered without any host printf call at all.  Floating conversions call
 * `snprintf()` with a literal that carries no length modifier, so the host
 * argument type is exactly `double`; width, precision, sign and padding are
 * then applied by this file.  A guest length modifier can therefore never
 * select a host variadic type.  `-Werror=format-nonliteral` enforces this
 * mechanically (tools/test_guest_printf.py).
 *
 * PSP ABI (measured, not assumed)
 * -------------------------------
 * Proven with the PSPSDK compiler (psp-gcc 15.2.0, default `-mabi=eabi`) by
 * inspecting emitted MIPS for variadic `sprintf` calls:
 *   - int/long/size_t/ptrdiff_t/pointer are 32 bits and take one argument word;
 *   - long long / intmax_t are 64 bits and take two words at an even slot;
 *   - long double is identical to double (8 bytes) and travels as two words;
 *   - a variadic float is promoted to double and travels in integer registers;
 *   - variadic word 0 is $6, words 0..5 are $6..$11, and word 6 onward live on
 *     the stack starting at entry $sp + 0 (EABI reserves no home area);
 *   - the even-slot rule is preserved across the register/stack transition, so
 *     five ints then a double places the double at stack+0, and seven ints then
 *     a double places it at stack+8.
 *
 * Formatting semantics (measured against PSP newlib)
 * --------------------------------------------------
 * The cells below were measured by running the real PSP libc under PPSSPP
 * rather than inferred from host behavior:
 *   - `%p` is a literal "0x" prefix followed by lowercase hex rendered like
 *     `%x`, honouring width/precision/zero/left; sign flags are ignored and the
 *     prefix is present even for a zero value (`%p` of 0 is "0x0", `%.0p` of 0
 *     is "0x").
 *   - an unknown conversion character emits that character alone and consumes
 *     NO argument, so following conversions keep their own arguments;
 *   - an inapplicable length modifier is ignored rather than rejected: `%hf`,
 *     `%zf`, `%llf` and `%lLf` all print a double, `%Ld` prints one word, and
 *     `%lp`/`%lc`/`%ls` behave as `%p`/`%c`/`%s`;
 *   - repeated `h` toggles short/char exactly as newlib does, and short takes
 *     precedence over char when both are set; two or more `l` mean 64-bit and
 *     further `l` are idempotent; `q` is a 64-bit modifier;
 *   - PSP newlib has no wide-character support: `%ls` reads its argument
 *     byte-wise, identical to `%s`;
 *   - the `0` flag zero-pads `%c` and `%s` (and `%s` of a null pointer);
 *   - `%n` ignores flags, width and precision and still writes; its write width
 *     follows the length modifier (hh=1, h=2, ll/j/q=8, otherwise 4).
 *
 * Project safety limits (NOT PSP semantics)
 * -----------------------------------------
 * GP_MAX_WIDTH / GP_MAX_PREC are this project's denial-of-service bounds, not
 * PSP behavior: measured PSP newlib formats `%20000d` and `%5000.4000f`
 * successfully.  Padding here is written straight into guest memory one checked
 * byte at a time, so an unbounded width would be an effective hang.  A spec
 * exceeding either bound consumes its argument normally (keeping the cursor
 * correct) and emits a visible `%<conversion>` marker instead of the field.
 * The bounds sit far above observed title usage.  Note also that measured PSP
 * newlib itself terminates abnormally on `%*d` with an INT32_MIN width, which
 * this bridge refuses deterministically instead.
 */

#include "recomp.h"

#include <limits.h>

#define GP_MAX_WIDTH 4096
#define GP_MAX_PREC  1024

/* Large enough for the longest bounded floating body this file can request:
 * sign + 309 integer digits (DBL_MAX) + '.' + GP_MAX_PREC fraction digits. */
#define GP_FLOAT_BUF 1440

/* Upper bound on a guest %s walk. */
#define GP_MAX_STRING 0x00100000u

typedef struct {
    unsigned left  : 1;   /* '-' */
    unsigned plus  : 1;   /* '+' */
    unsigned space : 1;   /* ' ' */
    unsigned alt   : 1;   /* '#' */
    unsigned zero  : 1;   /* '0' */
    /* Length-modifier state, mirroring newlib's flag model. */
    unsigned m_short : 1; /* h  */
    unsigned m_char  : 1; /* hh */
    unsigned m_long  : 1; /* l, z, t -- all 32-bit on PSP */
    unsigned m_quad  : 1; /* ll, j, q -- 64-bit */
    unsigned m_ldbl  : 1; /* L -- a double on PSP */
    int width;            /* >= 0 */
    int precision;        /* < 0 when absent */
    char conv;
} gp_spec;

/* ---------------------------------------------------------------- ABI cursor */

static uint32_t guest_printf_next_word(CpuState *s, uint32_t entry_sp,
                                       uint32_t *argi) {
    uint32_t index = (*argi)++;
    return index < 6u ? s->r[6u + index]
                      : MEM_R32(entry_sp + 4u * (index - 6u));
}

/* A 64-bit argument starts at an even argument-word slot.  Word 0 is $6, itself
 * even, and the stack continuation preserves the same parity. */
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

/* ---------------------------------------------------------------- integer path */

/* Render an integer entirely without a host printf call, so no host variadic
 * type is ever selected by guest syntax.  `hex_prefix` is decided by the caller
 * because `%p` carries the prefix unconditionally while `%#x` drops it for a
 * zero value. */
static void gp_emit_int(gp_out *o, const gp_spec *sp, uint64_t mag,
                        int is_signed, int negative, unsigned base,
                        int upper, int hex_prefix) {
    /* Base 8 of UINT64_MAX is 22 digits; 24 covers every supported base. */
    char digits[24];
    int ndig = 0;
    const char *tab = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (mag == 0u) {
        /* C: precision 0 with value 0 produces no digits at all. */
        if (sp->precision != 0) digits[ndig++] = '0';
    } else {
        while (mag != 0u) {
            digits[ndig++] = tab[mag % base];
            mag /= base;
        }
    }

    /* '+' and ' ' apply only to signed conversions. */
    char prefix[2] = { 0, 0 };
    int npre = 0;
    if (negative)                     prefix[npre++] = '-';
    else if (is_signed && sp->plus)   prefix[npre++] = '+';
    else if (is_signed && sp->space)  prefix[npre++] = ' ';

    char alt_prefix[2] = { 0, 0 };
    int nalt = 0;
    if (hex_prefix) {
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

    /* '0' padding is ignored when an explicit precision is present. */
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

/* Every branch uses a compile-time literal format whose only argument types are
 * (int, double) or (double).  -Wformat checks these fully. */
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
    default:
        return -1;
    }
}

#undef GP_FLOAT_CASE

static int gp_conv_is_upper(char c) {
    return c == 'F' || c == 'E' || c == 'G' || c == 'A';
}

/* Infinities and NaNs are spelled by this file rather than by the host libc.
 * Host spellings diverge (Windows UCRT renders a negative quiet NaN as
 * "nan(ind)" where PSP newlib and glibc render "-nan"), and %f reaches this
 * bridge from title code, so the bytes must not depend on the build host.
 * Measured PSP newlib: "inf"/"nan", uppercase for F/E/G/A, sign from the sign
 * bit, '+'/' ' flags honoured, and never zero-padded. */
static void gp_emit_nonfinite(gp_out *o, const gp_spec *sp, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    int neg = (bits >> 63) != 0u;
    int is_nan = (bits & 0x000fffffffffffffULL) != 0u;
    int upper = gp_conv_is_upper(sp->conv);
    const char *txt = is_nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");

    char sign = neg ? '-' : (sp->plus ? '+' : (sp->space ? ' ' : '\0'));
    int len = 3 + (sign != '\0' ? 1 : 0);
    int pad = (sp->width > len) ? sp->width - len : 0;

    if (sp->left) {
        if (sign) gp_put(o, sign);
        gp_put_n(o, txt, 3);
        gp_pad(o, ' ', pad);
    } else {
        gp_pad(o, ' ', pad);
        if (sign) gp_put(o, sign);
        gp_put_n(o, txt, 3);
    }
}

/* %a / %A rendered from the IEEE-754 bits by this file.  The host forms are not
 * interchangeable: PSP newlib and glibc emit the shortest mantissa
 * ("0x1.8p+0", denormals normalised to a leading 1 as in "0x1p-1074") while
 * Windows UCRT pads to full precision ("0x1.8000000000000p+0") and keeps
 * denormals unnormalised.  Producing the digits here removes that
 * host-dependence entirely. */
static void gp_emit_hexfloat(gp_out *o, const gp_spec *sp, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    int neg = (bits >> 63) != 0u;
    uint64_t mant = bits & 0x000fffffffffffffULL;
    int biased = (int)((bits >> 52) & 0x7ffu);
    int upper = (sp->conv == 'A');
    const char *tab = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    int lead;
    int exp;
    if (biased == 0 && mant == 0u) {
        lead = 0;
        exp = 0;
    } else if (biased == 0) {
        /* Denormal: normalise so the leading hex digit is 1. */
        int shift = 0;
        while ((mant & 0x0010000000000000ULL) == 0u) { mant <<= 1; shift++; }
        mant &= 0x000fffffffffffffULL;
        lead = 1;
        exp = -1022 - shift;
    } else {
        lead = 1;
        exp = biased - 1023;
    }

    /* The 52-bit fraction is exactly 13 hex digits; an explicit precision may
     * ask for fewer (rounded half-to-even) or more (zero-filled).  Precision is
     * already bounded by GP_MAX_PREC before this point. */
    char frac[GP_MAX_PREC + 16];
    int ndig;

    if (sp->precision < 0) {
        for (int i = 0; i < 13; i++) frac[i] = tab[(mant >> (48 - 4 * i)) & 0xfu];
        ndig = 13;
        while (ndig > 0 && frac[ndig - 1] == '0') ndig--;
    } else {
        uint64_t m = mant;
        if (sp->precision < 13) {
            int keep = sp->precision;              /* 0..12, so dropped >= 4 */
            int dropped = 52 - 4 * keep;
            uint64_t kept = m >> dropped;
            uint64_t rest = m & (((uint64_t)1 << dropped) - 1u);
            uint64_t half = (uint64_t)1 << (dropped - 1);
            if (rest > half || (rest == half && (kept & 1u) != 0u)) {
                kept++;
                if (keep == 0) {
                    if (kept != 0u) { lead++; kept = 0u; }
                } else if ((kept >> (4 * keep)) != 0u) {
                    kept &= ((uint64_t)1 << (4 * keep)) - 1u;
                    lead++;
                }
            }
            m = (keep > 0) ? (kept << dropped) : 0u;
        }
        ndig = sp->precision;
        for (int i = 0; i < ndig; i++) {
            frac[i] = (i < 13) ? tab[(m >> (48 - 4 * i)) & 0xfu] : '0';
        }
    }

    /* Assemble: sign, "0x", leading digit, optional ".fraction", "p<exp>". */
    char sign = neg ? '-' : (sp->plus ? '+' : (sp->space ? ' ' : '\0'));

    char expbuf[8];
    int nexp = 0;
    {
        int e = exp < 0 ? -exp : exp;
        char tmp[8];
        int t = 0;
        do { tmp[t++] = (char)('0' + e % 10); e /= 10; } while (e != 0);
        expbuf[nexp++] = upper ? 'P' : 'p';
        expbuf[nexp++] = exp < 0 ? '-' : '+';
        while (t-- > 0) expbuf[nexp++] = tmp[t];
    }

    int want_dot = (ndig > 0) || sp->alt;
    int body = 2 /* 0x */ + 1 /* lead */ + (want_dot ? 1 : 0) + ndig + nexp;
    int total = body + (sign != '\0' ? 1 : 0);
    int pad = (sp->width > total) ? sp->width - total : 0;
    int zero_pad = (!sp->left && sp->zero);

    if (!sp->left && !zero_pad) gp_pad(o, ' ', pad);
    if (sign) gp_put(o, sign);
    gp_put(o, '0');
    gp_put(o, upper ? 'X' : 'x');
    if (zero_pad && !sp->left) gp_pad(o, '0', pad);
    gp_put(o, (char)('0' + lead));
    if (want_dot) gp_put(o, '.');
    gp_put_n(o, frac, ndig);
    gp_put_n(o, expbuf, nexp);
    if (sp->left) gp_pad(o, ' ', pad);
}

static void gp_emit_float(gp_out *o, const gp_spec *sp, double v) {
    if (!gp_is_finite(v)) { gp_emit_nonfinite(o, sp, v); return; }
    if (sp->conv == 'a' || sp->conv == 'A') { gp_emit_hexfloat(o, sp, v); return; }

    char buf[GP_FLOAT_BUF];
    int n = gp_float_body(buf, sizeof buf, sp, v);
    if (n < 0 || (size_t)n >= sizeof buf) {
        gp_put(o, '%');
        gp_put(o, sp->conv);
        return;
    }

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
    return c == 'h' || c == 'l' || c == 'j' || c == 'z' || c == 't' ||
           c == 'L' || c == 'q';
}

/* Consume the whole run of length-modifier characters, applying newlib's flag
 * model.  Nothing here can be rejected: an inapplicable or repeated modifier is
 * simply recorded and then ignored by conversions it does not apply to. */
static void gp_parse_len(uint32_t *fmt, gp_spec *sp) {
    while (gp_is_len_char(MEM_R8(*fmt))) {
        switch (MEM_R8(*fmt)) {
        case 'h':
            /* h sets short; a second h converts short into char; further h
             * characters keep toggling, and short wins when both are set. */
            if (sp->m_short) { sp->m_short = 0; sp->m_char = 1; }
            else             { sp->m_short = 1; }
            break;
        case 'l':
            if (sp->m_long) sp->m_quad = 1;
            else            sp->m_long = 1;
            break;
        case 'j':
        case 'q':
            sp->m_quad = 1;
            break;
        case 'z':
        case 't':
            /* PSP size_t and ptrdiff_t are 32-bit, like long. */
            sp->m_long = 1;
            break;
        case 'L':
            sp->m_ldbl = 1;
            break;
        default:
            break;
        }
        (*fmt)++;
    }
}

/* Parse a decimal run.  An absent run yields 0, which is the correct value for
 * both an omitted width and a bare '.' precision.  `*out` is set to -1 when the
 * value exceeds `limit`, which the caller treats as a bound violation. */
static void gp_parse_decimal(uint32_t *fmt, int limit, int *out) {
    long v = 0;
    while (MEM_R8(*fmt) >= '0' && MEM_R8(*fmt) <= '9') {
        if (v <= (long)limit) v = v * 10 + (long)(MEM_R8(*fmt) - '0');
        (*fmt)++;
    }
    *out = (v > (long)limit) ? -1 : (int)v;
}

static int gp_is_float_conv(char c) {
    return c == 'f' || c == 'F' || c == 'e' || c == 'E' ||
           c == 'g' || c == 'G' || c == 'a' || c == 'A';
}

static int gp_is_known_conv(char c) {
    switch (c) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
    case 'f': case 'F': case 'e': case 'E':
    case 'g': case 'G': case 'a': case 'A':
    case 'c': case 's': case 'p': case 'n':
        return 1;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- entry point */

void sr_guest_sprintf(CpuState *s) {
    uint32_t fmt = s->r[5], argi = 0;
    uint32_t entry_sp = s->r[29];
    gp_out out = { s->r[4], 0 };

    while (MEM_R8(fmt) != 0u) {
        uint8_t ch = MEM_R8(fmt++);
        if (ch != '%') { gp_put(&out, (char)ch); continue; }
        if (MEM_R8(fmt) == '%') { fmt++; gp_put(&out, '%'); continue; }

        gp_spec sp;
        memset(&sp, 0, sizeof sp);
        sp.precision = -1;
        int in_bounds = 1;

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
            /* A negative `*` width means the '-' flag with |width|. */
            if (w < 0) {
                sp.left = 1;
                if (w == INT32_MIN) in_bounds = 0; else w = -w;
            }
            if (w > GP_MAX_WIDTH) in_bounds = 0;
            sp.width = in_bounds ? (int)w : 0;
        } else {
            int w = 0;
            gp_parse_decimal(&fmt, GP_MAX_WIDTH, &w);
            if (w < 0) in_bounds = 0; else sp.width = w;
        }

        if (MEM_R8(fmt) == '.') {
            fmt++;
            if (MEM_R8(fmt) == '*') {
                fmt++;
                int32_t p = (int32_t)guest_printf_next_word(s, entry_sp, &argi);
                /* A negative `*` precision is as if precision were omitted. */
                if (p < 0)                 sp.precision = -1;
                else if (p > GP_MAX_PREC)  in_bounds = 0;
                else                       sp.precision = (int)p;
            } else {
                int p = 0;
                gp_parse_decimal(&fmt, GP_MAX_PREC, &p);
                if (p < 0) in_bounds = 0; else sp.precision = p;
            }
        }

        gp_parse_len(&fmt, &sp);

        ch = MEM_R8(fmt);
        if (ch == 0u) break;   /* truncated spec at end of format */
        fmt++;
        sp.conv = (char)ch;

        /* An unknown conversion emits its character and consumes nothing, so
         * every following conversion still receives its own argument. */
        if (!gp_is_known_conv(sp.conv)) {
            gp_put(&out, sp.conv);
            continue;
        }

        /* Fetch this spec's argument, sized by the PSP ABI.  Floating
         * conversions ignore every length modifier and always take a double;
         * %c/%s/%p/%n always take one word. */
        int wants_double = gp_is_float_conv(sp.conv);
        int wants_quad = !wants_double && sp.m_quad &&
                         (sp.conv == 'd' || sp.conv == 'i' || sp.conv == 'u' ||
                          sp.conv == 'o' || sp.conv == 'x' || sp.conv == 'X');
        double dval = 0.0;
        uint64_t uval = 0;
        if (wants_double) {
            dval = guest_printf_next_double(s, entry_sp, &argi);
        } else if (wants_quad) {
            uval = guest_printf_next_dword(s, entry_sp, &argi);
        } else {
            uval = guest_printf_next_word(s, entry_sp, &argi);
        }

        /* A recognized conversion whose width/precision exceeds this project's
         * safety bound still consumed its argument above, so the cursor stays
         * correct; only the field is replaced by a visible marker. */
        if (!in_bounds) {
            gp_put(&out, '%');
            gp_put(&out, sp.conv);
            continue;
        }

        switch (sp.conv) {
        case 'd': case 'i': {
            int64_t v;
            if (sp.m_quad)       v = (int64_t)uval;
            else if (sp.m_short) v = (int16_t)(uval & 0xffffu);
            else if (sp.m_char)  v = (int8_t)(uval & 0xffu);
            else                 v = (int32_t)(uint32_t)uval;
            uint64_t mag = (v < 0) ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            gp_emit_int(&out, &sp, mag, 1, v < 0, 10u, 0, 0);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v;
            if (sp.m_quad)       v = uval;
            else if (sp.m_short) v = uval & 0xffffu;
            else if (sp.m_char)  v = uval & 0xffu;
            else                 v = uval & 0xffffffffu;
            unsigned base = (sp.conv == 'o') ? 8u
                          : (sp.conv == 'u') ? 10u : 16u;
            int upper = (sp.conv == 'X');
            int hex_prefix = (sp.alt && base == 16u && v != 0u);
            gp_emit_int(&out, &sp, v, 0, 0, base, upper, hex_prefix);
            break;
        }
        case 'f': case 'F': case 'e': case 'E':
        case 'g': case 'G': case 'a': case 'A':
            gp_emit_float(&out, &sp, dval);
            break;
        case 'c': {
            /* The '0' flag zero-pads %c unless the field is left-justified. */
            char c = (char)(uval & 0xffu);
            char fill = (sp.zero && !sp.left) ? '0' : ' ';
            int pad = (sp.width > 1) ? sp.width - 1 : 0;
            if (sp.left) { gp_put(&out, c); gp_pad(&out, ' ', pad); }
            else         { gp_pad(&out, fill, pad); gp_put(&out, c); }
            break;
        }
        case 's': {
            /* PSP newlib has no wide-string support, so %ls is byte-wise and
             * identical to %s. */
            uint32_t p = (uint32_t)uval;
            uint32_t max = (sp.precision >= 0) ? (uint32_t)sp.precision
                                               : GP_MAX_STRING;
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
            char fill = (sp.zero && !sp.left) ? '0' : ' ';
            int pad = (sp.width > len) ? sp.width - len : 0;
            if (!sp.left) gp_pad(&out, fill, pad);
            for (int i = 0; i < len; i++) {
                gp_put(&out, valid ? (char)MEM_R8(p + (uint32_t)i)
                                   : null_text[i]);
            }
            if (sp.left) gp_pad(&out, ' ', pad);
            break;
        }
        case 'p': {
            /* "0x" is unconditional, including for a zero value; the digits
             * follow %x rules for precision, width, zero-fill and left-fill.
             * Sign flags do not apply. */
            gp_emit_int(&out, &sp, uval & 0xffffffffu, 0, 0, 16u, 0, 1);
            break;
        }
        case 'n': {
            /* Flags, width and precision are ignored; the length modifier sizes
             * the write.  The destination span is preflighted so a partial
             * store cannot happen at the end of the arena. */
            uint32_t p = (uint32_t)uval;
            uint32_t count = (uint32_t)out.total;
            uint32_t width = sp.m_quad ? 8u
                           : sp.m_short ? 2u
                           : sp.m_char ? 1u : 4u;
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
            /* Unreachable: gp_is_known_conv() gates every case above. */
            break;
        }
    }

    MEM_W8(out.dst, 0u);
    s->r[2] = (uint32_t)out.total;
    s->pc = s->r[31];
}
