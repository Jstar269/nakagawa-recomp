/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/* Clean-room PSP module (PRX) loader.
 * Implements docs/cleanroom/PRX_LOADER_SPEC.md sections 2-3.
 * C11, only C standard library includes.
 *
 * Layout choices where the spec leaves field widths to the implementer:
 * - SceModuleInfo block is 52 bytes: attr u16 @0, version 2 bytes @2,
 *   name 28 bytes @4, gp u32 @32, exp_start u32 @36, exp_end u32 @40,
 *   imp_start u32 @44, imp_end u32 @48 (all little-endian).
 * - Export records: 16-byte head, little-endian: libname u32 @0,
 *   version 2 bytes @4, attribute u16 @6, len(words, u8) @8, nvar(u8) @9,
 *   nfunc(u16) @10, entrytable u32 @12. Length is at least 4 words;
 *   bytes beyond offset 16 are ignored.
 *   The entry table is (nfunc+nvar) NIDs followed by (nfunc+nvar) addresses,
 *   functions first.
 * - Import stub records: 20-byte head (24 with variable table pointer),
 *   little-endian: libname u32 @0, version 2 bytes @4, attribute u16 @6,
 *   len(words, u8) @8, nvar(u8) @9, nfunc(u16) @10, nidtable u32 @12,
 *   stubtable u32 @16, vartable u32 @20 (only when len>=6, unused here).
 *   Length is at least 5 words.
 * - sr_prx_guest_write takes (uint32_t guest_addr, const void *src, uint32_t n).
 * - Relocation stream order: program headers in header order (both formats
 *   interleaved by header order); format-A section sources only when no
 *   format-A program header exists, processed in section-header order.
 * - Initial format-B running offset segment/offset are 0/0.
 * - Format-A HI16 groups peek at the partner site word before the partner
 *   itself is relocated (stream order: HI16 records first).
 * - Format-B kind-index mode 01 full addend: (sign_extended_d << 16) | ext.
 * - Section sh_addr values are unbased guest addresses: guest = base+sh_addr
 *   for relocatable inputs, sh_addr for ET_EXEC.
 * - Export/import count caps (fail-closed): at most 65536 exports and 65536
 *   import stubs; larger claims fail before any unbounded loop.
 */

#include "prx_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR_PRX_MODINFO_SIZE 52u
#define SR_PRX_EXPORT_REC_SIZE 16u
#define SR_PRX_IMPORT_REC_MIN 20u
#define SR_PRX_MAX_ENTRIES 65536u
#define SR_PRX_MAX_FILE_BYTES (256u * 1024u * 1024u)
#define SR_PRX_MAX_IMAGE_BYTES (64u * 1024u * 1024u)

static void set_err(char *err, size_t errlen, const char *msg) {
    size_t i;
    if (err == NULL || errlen == 0) {
        return;
    }
    for (i = 0; i + 1 < errlen && msg[i] != '\0'; i++) {
        err[i] = msg[i];
    }
    err[i] = '\0';
}

static uint16_t rd16le(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32le(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* Sign-extend the low 16 bits of v. */
static int32_t sx16(uint32_t v) {
    return (int32_t)(int16_t)(uint16_t)(v & 0xFFFFu);
}

static int32_t sign_extend(uint32_t v, unsigned bits) {
    if (bits == 0 || bits >= 32) {
        return bits == 0 ? 0 : (int32_t)v;
    }
    if ((v >> (bits - 1)) & 1u) {
        v |= (0xFFFFFFFFu << bits);
    }
    return (int32_t)v;
}

typedef struct ScratchSeg {
    uint32_t guest;
    uint32_t memsz;
    unsigned char *buf;
} ScratchSeg;

typedef struct LoaderCtx {
    const unsigned char *data;
    size_t size;
    uint32_t base;
    int is_reloc;
    uint32_t nseg;
    uint32_t seg_p_offset[SR_PRX_MAX_SEGMENTS];
    ScratchSeg segs[SR_PRX_MAX_SEGMENTS];
    char *err;
    size_t errlen;
} LoaderCtx;

/* True when every byte of [addr, addr+len) is covered by scratch segments. */
static int range_covered(LoaderCtx *c, uint32_t addr, uint64_t len) {
    uint64_t cur;
    uint64_t end;
    unsigned pass;
    if (len == 0) {
        return 1;
    }
    if (len > 0x100000000ULL) {
        return 0;
    }
    cur = addr;
    end = (uint64_t)addr + len;
    if (end > 0x100000000ULL) {
        return 0;
    }
    for (pass = 0; pass <= c->nseg && cur < end; pass++) {
        uint32_t i;
        int found = 0;
        for (i = 0; i < c->nseg; i++) {
            uint64_t s = c->segs[i].guest;
            uint64_t e = s + (uint64_t)c->segs[i].memsz;
            if (cur >= s && cur < e) {
                uint64_t chunk = e - cur;
                if (chunk > end - cur) {
                    chunk = end - cur;
                }
                cur += chunk;
                found = 1;
                break;
            }
        }
        if (!found) {
            return 0;
        }
    }
    return cur == end;
}

static int read_guest(LoaderCtx *c, uint32_t addr, unsigned char *dst, size_t len) {
    size_t done = 0;
    uint64_t cur = addr;
    uint64_t end = (uint64_t)addr + (uint64_t)len;
    unsigned pass;
    if (len == 0) {
        return 0;
    }
    if (end > 0x100000000ULL) {
        return -1;
    }
    for (pass = 0; pass <= c->nseg + 1 && cur < end; pass++) {
        uint32_t i;
        int found = 0;
        for (i = 0; i < c->nseg; i++) {
            uint64_t s = c->segs[i].guest;
            uint64_t e = s + (uint64_t)c->segs[i].memsz;
            if (cur >= s && cur < e) {
                uint64_t chunk = e - cur;
                uint64_t want = end - cur;
                if (chunk > want) {
                    chunk = want;
                }
                if (c->segs[i].buf != NULL) {
                    memcpy(dst + done, c->segs[i].buf + (size_t)(cur - s),
                           (size_t)chunk);
                } else {
                    memset(dst + done, 0, (size_t)chunk);
                }
                done += (size_t)chunk;
                cur += chunk;
                found = 1;
                break;
            }
        }
        if (!found) {
            return -1;
        }
    }
    return cur == end ? 0 : -1;
}

static int read_guest_u32(LoaderCtx *c, uint32_t addr, uint32_t *v) {
    unsigned char b[4];
    if (read_guest(c, addr, b, 4) != 0) {
        return -1;
    }
    *v = rd32le(b);
    return 0;
}

/* Read a NUL-terminated string inside the image, max 255 chars. */
static int read_guest_str(LoaderCtx *c, uint32_t addr, char out[SR_PRX_LIBNAME_LEN]) {
    uint32_t i;
    for (i = 0; i < SR_PRX_LIBNAME_LEN; i++) {
        unsigned char b;
        if (read_guest(c, addr + i, &b, 1) != 0) {
            return -1;
        }
        out[i] = (char)b;
        if (b == 0) {
            return 0;
        }
    }
    return -1;
}

static unsigned char *site_ptr(LoaderCtx *c, uint32_t oseg, uint32_t off) {
    if (oseg >= c->nseg) {
        return NULL;
    }
    if ((uint64_t)off + 4 > (uint64_t)c->segs[oseg].memsz) {
        return NULL;
    }
    if (c->segs[oseg].buf == NULL) {
        return NULL;
    }
    return c->segs[oseg].buf + off;
}

static uint32_t apply_jump(uint32_t w, uint32_t site_addr, uint32_t s) {
    uint32_t field = w & 0x03FFFFFFu;
    uint32_t t = (site_addr & 0xF0000000u) | ((field << 2) & 0x0FFFFFFFu);
    uint32_t nt = t + s;
    return (w & 0xFC000000u) | ((nt >> 2) & 0x03FFFFFFu);
}

static int apply_format_a(LoaderCtx *c, const unsigned char *tab, size_t len) {
    size_t n;
    size_t i;
    if (len % 8 != 0) {
        set_err(c->err, c->errlen, "format A size not a multiple of 8");
        return -1;
    }
    n = len / 8;
    i = 0;
    while (i < n) {
        uint32_t off = rd32le(tab + i * 8);
        uint32_t info = rd32le(tab + i * 8 + 4);
        unsigned kind = info & 0xFFu;
        if (kind == 5) {
            size_t j = i;
            size_t k;
            uint32_t p_off;
            uint32_t p_info;
            unsigned p_kind;
            unsigned p_oseg;
            unsigned p_aseg;
            unsigned char *pp;
            uint32_t pw;
            /* Collect the HI16 run; validate each member's segments. */
            while (j < n) {
                uint32_t ji = rd32le(tab + j * 8 + 4);
                unsigned jk = ji & 0xFFu;
                unsigned jo = (ji >> 8) & 0xFFu;
                unsigned ja = (ji >> 16) & 0xFFu;
                if (jk != 5) {
                    break;
                }
                if (jo >= c->nseg) {
                    set_err(c->err, c->errlen,
                            "format A offset segment out of range");
                    return -1;
                }
                if (ja >= c->nseg) {
                    set_err(c->err, c->errlen,
                            "format A address segment out of range");
                    return -1;
                }
                j++;
            }
            if (j >= n) {
                set_err(c->err, c->errlen,
                        "format A HI16 run without partner");
                return -1;
            }
            p_off = rd32le(tab + j * 8);
            p_info = rd32le(tab + j * 8 + 4);
            p_kind = p_info & 0xFFu;
            p_oseg = (p_info >> 8) & 0xFFu;
            p_aseg = (p_info >> 16) & 0xFFu;
            if (p_oseg >= c->nseg) {
                set_err(c->err, c->errlen,
                        "format A offset segment out of range");
                return -1;
            }
            if (p_aseg >= c->nseg) {
                set_err(c->err, c->errlen,
                        "format A address segment out of range");
                return -1;
            }
            pp = site_ptr(c, p_oseg, p_off);
            if (pp == NULL) {
                set_err(c->err, c->errlen, "format A site out of range");
                return -1;
            }
            pw = rd32le(pp);
            for (k = i; k < j; k++) {
                uint32_t koff = rd32le(tab + k * 8);
                uint32_t kinfo = rd32le(tab + k * 8 + 4);
                unsigned koseg = (kinfo >> 8) & 0xFFu;
                unsigned kaseg = (kinfo >> 16) & 0xFFu;
                unsigned char *sp = site_ptr(c, koseg, koff);
                uint32_t w;
                uint32_t s;
                uint32_t v;
                uint32_t nlo;
                if (sp == NULL) {
                    set_err(c->err, c->errlen, "format A site out of range");
                    return -1;
                }
                w = rd32le(sp);
                s = c->segs[kaseg].guest;
                /* Revised §3.6 kind 5: V = (low16(W)*65536)
                 * + sx16(partner low16) + S; result low16 =
                 * (V+0x8000)>>16. High 16 unchanged. */
                v = ((w & 0xFFFFu) << 16) + (uint32_t)sx16(pw) + s;
                nlo = ((v + 0x8000u) >> 16) & 0xFFFFu;
                wr32le(sp, (w & 0xFFFF0000u) | nlo);
            }
            /* Now apply the partner record itself. */
            {
                unsigned char *sp = site_ptr(c, p_oseg, p_off);
                uint32_t w = rd32le(sp);
                uint32_t s = c->segs[p_aseg].guest;
                uint32_t site_addr = c->segs[p_oseg].guest + p_off;
                uint32_t nw = w;
                switch (p_kind) {
                case 0:
                    break;
                case 1:
                case 6: {
                    int64_t t = (int64_t)sx16(w) + (int64_t)s;
                    nw = (w & 0xFFFF0000u) | ((uint32_t)t & 0xFFFFu);
                    break;
                }
                case 2:
                    nw = w + s;
                    break;
                case 4:
                    nw = apply_jump(w, site_addr, s);
                    break;
                case 5:
                    set_err(c->err, c->errlen,
                            "format A HI16 run without partner");
                    return -1;
                case 7:
                    set_err(c->err, c->errlen,
                            "format A kind 7 (GPREL16) refused");
                    return -1;
                case 8:
                    break;
                default:
                    set_err(c->err, c->errlen, "format A unknown kind");
                    return -1;
                }
                wr32le(sp, nw);
            }
            i = j + 1;
            continue;
        }
        {
            unsigned oseg = (info >> 8) & 0xFFu;
            unsigned aseg = (info >> 16) & 0xFFu;
            unsigned char *sp;
            uint32_t w;
            uint32_t s;
            uint32_t site_addr;
            uint32_t nw;
            if (oseg >= c->nseg) {
                set_err(c->err, c->errlen,
                        "format A offset segment out of range");
                return -1;
            }
            if (aseg >= c->nseg) {
                set_err(c->err, c->errlen,
                        "format A address segment out of range");
                return -1;
            }
            sp = site_ptr(c, oseg, off);
            if (sp == NULL) {
                set_err(c->err, c->errlen, "format A site out of range");
                return -1;
            }
            w = rd32le(sp);
            s = c->segs[aseg].guest;
            site_addr = c->segs[oseg].guest + off;
            nw = w;
            switch (kind) {
            case 0:
                break;
            case 1:
            case 6: {
                int64_t t = (int64_t)sx16(w) + (int64_t)s;
                nw = (w & 0xFFFF0000u) | ((uint32_t)t & 0xFFFFu);
                break;
            }
            case 2:
                nw = w + s;
                break;
            case 4:
                nw = apply_jump(w, site_addr, s);
                break;
            case 7:
                set_err(c->err, c->errlen,
                        "format A kind 7 (GPREL16) refused");
                return -1;
            case 8:
                break;
            default:
                set_err(c->err, c->errlen, "format A unknown kind");
                return -1;
            }
            wr32le(sp, nw);
        }
        i++;
    }
    return 0;
}

static int apply_format_b(LoaderCtx *c, const unsigned char *d, size_t len) {
    unsigned f, t, segw, dw;
    size_t nf, nk_off, nk, cmd;
    uint32_t cur_seg = 0;
    uint32_t cur_off = 0;
    int32_t prev_addend = 0;
    int prev_kind = -1;
    if (len < 4) {
        set_err(c->err, c->errlen, "format B truncated header");
        return -1;
    }
    if (d[0] != 0 || d[1] != 0) {
        set_err(c->err, c->errlen, "format B nonzero header bytes 0-1");
        return -1;
    }
    f = d[2];
    t = d[3];
    segw = (c->nseg >= 3) ? 2 : 1;
    if (f < 1 || f > 8 || t < 1 || t > 8) {
        set_err(c->err, c->errlen, "format B bad flag/kind width");
        return -1;
    }
    if (f + t + segw > 16) {
        set_err(c->err, c->errlen, "format B header widths exceed 16 bits");
        return -1;
    }
    if (len < 5) {
        set_err(c->err, c->errlen, "format B flag table truncated");
        return -1;
    }
    nf = d[4];
    if (nf < 1 || 4 + nf > len) {
        set_err(c->err, c->errlen, "format B flag table truncated");
        return -1;
    }
    nk_off = 4 + nf;
    if (nk_off >= len) {
        set_err(c->err, c->errlen, "format B kind table truncated");
        return -1;
    }
    nk = d[nk_off];
    if (nk < 1 || nk_off + nk > len) {
        set_err(c->err, c->errlen, "format B kind table truncated");
        return -1;
    }
    cmd = nk_off + nk;
    dw = 16 - f - segw - t;
    while (cmd < len) {
        uint32_t cc;
        uint32_t flag_idx, seg_idx, kind_idx, draw;
        int32_t disp;
        uint32_t g;
        if (cmd + 2 > len) {
            set_err(c->err, c->errlen, "format B truncated command");
            return -1;
        }
        cc = (uint32_t)d[cmd] | ((uint32_t)d[cmd + 1] << 8);
        cmd += 2;
        flag_idx = cc & ((f == 32) ? 0xFFFFFFFFu : ((1u << f) - 1u));
        seg_idx = (cc >> f) & ((segw >= 32) ? 0xFFFFFFFFu : ((1u << segw) - 1u));
        kind_idx = (cc >> (f + segw)) & ((t == 32) ? 0xFFFFFFFFu : ((1u << t) - 1u));
        draw = dw == 0 ? 0 : (cc >> (f + segw + t));
        disp = sign_extend(draw, dw);
        if (flag_idx == 0) {
            set_err(c->err, c->errlen, "format B flag index 0");
            return -1;
        }
        if (flag_idx >= nf) {
            set_err(c->err, c->errlen, "format B flag index out of range");
            return -1;
        }
        g = d[4 + flag_idx];
        if ((g & 1u) == 0) {
            uint32_t b12 = g & 6u;
            if (seg_idx >= c->nseg) {
                set_err(c->err, c->errlen,
                        "format B segment index out of range");
                return -1;
            }
            cur_seg = seg_idx;
            if (b12 == 0) {
                cur_off = cc >> (f + segw);
            } else if (b12 == 4) {
                if (cmd + 4 > len) {
                    set_err(c->err, c->errlen,
                            "format B truncated extension word");
                    return -1;
                }
                cur_off = rd32le(d + cmd);
                cmd += 4;
            } else {
                set_err(c->err, c->errlen, "format B bad set-offset form");
                return -1;
            }
        } else {
            uint32_t b12 = g & 6u;
            uint32_t b345 = (g >> 3) & 7u;
            uint32_t kind;
            int32_t add = 0;
            unsigned char *sp;
            uint32_t w, s, nw;
            uint32_t site_addr;
            if (seg_idx >= c->nseg) {
                set_err(c->err, c->errlen,
                        "format B segment index out of range");
                return -1;
            }
            if (kind_idx == 0) {
                set_err(c->err, c->errlen, "format B kind index 0");
                return -1;
            }
            if (kind_idx >= nk) {
                set_err(c->err, c->errlen,
                        "format B kind index out of range");
                return -1;
            }
            kind = d[nk_off + kind_idx];
            if (b12 == 0) {
                cur_off = (uint32_t)((int64_t)cur_off + (int64_t)disp);
            } else if (b12 == 2) {
                uint32_t ext;
                int32_t dse;
                int32_t full;
                if (cmd + 2 > len) {
                    set_err(c->err, c->errlen,
                            "format B truncated extension word");
                    return -1;
                }
                ext = (uint32_t)d[cmd] | ((uint32_t)d[cmd + 1] << 8);
                cmd += 2;
                dse = sign_extend(draw, dw);
                full = (int32_t)(((uint32_t)dse << 16) | (ext & 0xFFFFu));
                cur_off = (uint32_t)((int64_t)cur_off + (int64_t)full);
            } else if (b12 == 4) {
                if (cmd + 4 > len) {
                    set_err(c->err, c->errlen,
                            "format B truncated extension word");
                    return -1;
                }
                cur_off = rd32le(d + cmd);
                cmd += 4;
            } else {
                set_err(c->err, c->errlen,
                        "format B bad relocate offset form");
                return -1;
            }
            if (b345 == 0) {
                add = 0;
            } else if (b345 == 1) {
                add = (prev_kind == 4) ? prev_addend : 0;
            } else if (b345 == 2) {
                uint32_t ext;
                if (cmd + 2 > len) {
                    set_err(c->err, c->errlen,
                            "format B truncated addend word");
                    return -1;
                }
                ext = (uint32_t)d[cmd] | ((uint32_t)d[cmd + 1] << 8);
                cmd += 2;
                add = (int32_t)(int16_t)(uint16_t)ext;
            } else {
                set_err(c->err, c->errlen, "format B bad addend mode");
                return -1;
            }
            if (cur_seg >= c->nseg) {
                set_err(c->err, c->errlen,
                        "format B segment index out of range");
                return -1;
            }
            sp = site_ptr(c, cur_seg, cur_off);
            if (sp == NULL) {
                set_err(c->err, c->errlen, "format B site out of range");
                return -1;
            }
            w = rd32le(sp);
            s = c->segs[seg_idx].guest;
            site_addr = c->segs[cur_seg].guest + cur_off;
            nw = w;
            switch (kind) {
            case 0:
                break;
            case 1:
            case 5: {
                int64_t tt = (int64_t)sx16(w) + (int64_t)s;
                nw = (w & 0xFFFF0000u) | ((uint32_t)tt & 0xFFFFu);
                break;
            }
            case 2:
                nw = w + s;
                break;
            case 3:
                nw = apply_jump(w, site_addr, s);
                break;
            case 4: {
                /* Revised §3.6 kind 4: same rule as format A kind 5.
                 * V = (low16(W)*65536) + sx16(A) + S; result low16 =
                 * (V+0x8000)>>16. High 16 unchanged. */
                uint32_t vv = ((w & 0xFFFFu) << 16) +
                    (uint32_t)sx16((uint32_t)add) + s;
                uint32_t nlo = ((vv + 0x8000u) >> 16) & 0xFFFFu;
                nw = (w & 0xFFFF0000u) | nlo;
                break;
            }
            case 6:
                nw = apply_jump(w, site_addr, s);
                nw = (nw & 0x03FFFFFFu) | (2u << 26);
                break;
            case 7:
                nw = apply_jump(w, site_addr, s);
                nw = (nw & 0x03FFFFFFu) | (3u << 26);
                break;
            default:
                set_err(c->err, c->errlen, "format B unknown kind");
                return -1;
            }
            wr32le(sp, nw);
            prev_kind = (int)kind;
            prev_addend = add;
        }
    }
    return 0;
}

typedef struct PhdrInfo {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
} PhdrInfo;

static int do_load(const unsigned char *data, size_t size, uint32_t base,
                   SrPrxImage *out, char *err, size_t errlen) {
    LoaderCtx c;
    uint16_t etype, machine, phentsize, phnum, shentsize, shnum, shstrndx;
    uint32_t e_entry, e_phoff, e_shoff;
    uint32_t i;
    PhdrInfo *ph = NULL;
    uint64_t image_bytes = 0;
    uint32_t seg_idx = 0;
    uint32_t start = 0, end = 0;
    int has_a_phdr = 0;
    uint32_t p_paddr0 = 0, p_offset0 = 0;
    uint32_t mod_guest;
    uint32_t mod_off_in_seg;
    unsigned char modblk[SR_PRX_MODINFO_SIZE];
    uint16_t mod_attr;
    uint8_t mod_ver[2];
    char mod_name[SR_PRX_MODNAME_LEN];
    uint32_t gp, exp_start, exp_end, imp_start, imp_end;
    SrPrxExport *exps = NULL;
    uint32_t nexp = 0, exps_cap = 0;
    SrPrxImportStub *imps = NULL;
    uint32_t nimp = 0, imps_cap = 0;
    uint32_t s;

    memset(&c, 0, sizeof c);
    c.data = data;
    c.size = size;
    c.base = base;
    c.err = err;
    c.errlen = errlen;
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }

    if (out == NULL) {
        set_err(err, errlen, "null output record");
        return -1;
    }
    if (data == NULL || size == 0) {
        set_err(err, errlen, "empty input");
        return -1;
    }
    if (size >= 4 && data[0] == 0x7Eu && data[1] == 0x50u &&
        data[2] == 0x53u && data[3] == 0x50u) {
        set_err(err, errlen,
                "refused ~PSP container: decryption/decompression out of scope");
        return -1;
    }
    if (size > SR_PRX_MAX_FILE_BYTES) {
        set_err(err, errlen, "PRX input exceeds 256 MiB");
        return -1;
    }
    if (size < 52) {
        set_err(err, errlen, "truncated ELF header");
        return -1;
    }
    if (!(data[0] == 0x7Fu && data[1] == 0x45u && data[2] == 0x4Cu &&
          data[3] == 0x46u)) {
        set_err(err, errlen, "not an ELF image");
        return -1;
    }
    if (data[4] != 1) {
        set_err(err, errlen, "unsupported ELF class (need ELF32)");
        return -1;
    }
    if (data[5] != 1) {
        set_err(err, errlen, "unsupported ELF endianness (need little-endian)");
        return -1;
    }
    machine = rd16le(data + 18);
    if (machine != 8) {
        set_err(err, errlen, "unsupported ELF machine (need MIPS)");
        return -1;
    }
    etype = rd16le(data + 16);
    if (etype != 0xFFA0u && etype != 1 && etype != 2) {
        set_err(err, errlen, "unsupported ELF type");
        return -1;
    }
    c.is_reloc = (etype == 0xFFA0u || etype == 1);
    if (etype == 2 && base != 0) {
        set_err(err, errlen,
                "ET_EXEC with nonzero base refused (no relocations to rebase)");
        return -1;
    }
    e_entry = rd32le(data + 24);
    e_phoff = rd32le(data + 28);
    e_shoff = rd32le(data + 32);
    phentsize = rd16le(data + 42);
    phnum = rd16le(data + 44);
    shentsize = rd16le(data + 46);
    shnum = rd16le(data + 48);
    shstrndx = rd16le(data + 50);

    if (phnum > 0) {
        uint64_t need;
        if (phentsize < 32) {
            set_err(err, errlen, "bad program header entry size");
            return -1;
        }
        need = (uint64_t)e_phoff + (uint64_t)phnum * (uint64_t)phentsize;
        if (need > (uint64_t)size) {
            set_err(err, errlen, "program headers exceed input size");
            return -1;
        }
        ph = (PhdrInfo *)calloc(phnum, sizeof *ph);
        if (ph == NULL) {
            set_err(err, errlen, "out of memory");
            return -1;
        }
        for (i = 0; i < phnum; i++) {
            const unsigned char *p = data + e_phoff + (size_t)i * phentsize;
            ph[i].p_type = rd32le(p);
            ph[i].p_offset = rd32le(p + 4);
            ph[i].p_vaddr = rd32le(p + 8);
            ph[i].p_paddr = rd32le(p + 12);
            ph[i].p_filesz = rd32le(p + 16);
            ph[i].p_memsz = rd32le(p + 20);
        }
    }

    /* Collect loadable segments in header order. */
    for (i = 0; i < (uint32_t)phnum; i++) {
        if (ph[i].p_type == 1) {
            uint64_t fr;
            uint64_t guest64;
            uint32_t guest;
            if (seg_idx >= SR_PRX_MAX_SEGMENTS) {
                set_err(err, errlen, "too many loadable segments (max 4)");
                free(ph);
                return -1;
            }
            if (ph[i].p_filesz > ph[i].p_memsz) {
                set_err(err, errlen, "segment p_filesz exceeds p_memsz");
                free(ph);
                return -1;
            }
            if (ph[i].p_memsz > SR_PRX_MAX_IMAGE_BYTES - image_bytes) {
                set_err(err, errlen, "PRX image exceeds 64 MiB");
                free(ph);
                return -1;
            }
            image_bytes += ph[i].p_memsz;
            fr = (uint64_t)ph[i].p_offset + (uint64_t)ph[i].p_filesz;
            if (fr > 0xFFFFFFFFULL) {
                set_err(err, errlen, "segment file range overflows 32 bits");
                free(ph);
                return -1;
            }
            if (fr > (uint64_t)size) {
                set_err(err, errlen, "segment file range exceeds input size");
                free(ph);
                return -1;
            }
            if (c.is_reloc) {
                guest64 = (uint64_t)base + (uint64_t)ph[i].p_vaddr;
            } else {
                guest64 = ph[i].p_vaddr;
            }
            if (guest64 > 0xFFFFFFFFULL) {
                set_err(err, errlen,
                        "segment memory address overflows 32 bits");
                free(ph);
                return -1;
            }
            guest = (uint32_t)guest64;
            if ((uint64_t)guest + (uint64_t)ph[i].p_memsz > 0xFFFFFFFFULL &&
                ph[i].p_memsz != 0) {
                if ((uint64_t)guest + (uint64_t)ph[i].p_memsz > 0x100000000ULL) {
                    set_err(err, errlen,
                            "segment memory range overflows 32 bits");
                    free(ph);
                    return -1;
                }
                /* Exclusive end of exactly 2^32 cannot be represented;
                 * treat as overflow to keep start/end exact. */
                set_err(err, errlen,
                        "segment memory range overflows 32 bits");
                free(ph);
                return -1;
            }
            c.seg_p_offset[seg_idx] = ph[i].p_offset;
            c.segs[seg_idx].guest = guest;
            c.segs[seg_idx].memsz = ph[i].p_memsz;
            c.segs[seg_idx].buf = NULL;
            if (seg_idx == 0) {
                p_paddr0 = ph[i].p_paddr;
                p_offset0 = ph[i].p_offset;
            }
            seg_idx++;
        }
    }
    c.nseg = seg_idx;
    if (c.nseg == 0) {
        set_err(err, errlen, "no loadable segments");
        free(ph);
        return -1;
    }
    /* Overlap check on memory ranges. */
    for (i = 0; i < c.nseg; i++) {
        uint32_t j;
        uint64_t si = c.segs[i].guest;
        uint64_t ei = si + c.segs[i].memsz;
        if (c.segs[i].memsz == 0) {
            continue;
        }
        for (j = i + 1; j < c.nseg; j++) {
            uint64_t sj = c.segs[j].guest;
            uint64_t ej = sj + c.segs[j].memsz;
            uint64_t lo, hi;
            if (c.segs[j].memsz == 0) {
                continue;
            }
            lo = si > sj ? si : sj;
            hi = ei < ej ? ei : ej;
            if (lo < hi) {
                set_err(err, errlen, "overlapping loadable segments");
                free(ph);
                return -1;
            }
        }
    }
    start = c.segs[0].guest;
    end = c.segs[0].guest + c.segs[0].memsz;
    for (i = 1; i < c.nseg; i++) {
        uint32_t gs = c.segs[i].guest;
        uint32_t ge = gs + c.segs[i].memsz;
        if (gs < start) {
            start = gs;
        }
        if (ge > end) {
            end = ge;
        }
    }

    /* Validate relocation program data ranges before allocating. */
    for (i = 0; i < (uint32_t)phnum; i++) {
        if (ph[i].p_type == 0x700000A0u || ph[i].p_type == 0x700000A1u) {
            uint64_t rr = (uint64_t)ph[i].p_offset + (uint64_t)ph[i].p_filesz;
            if (rr > (uint64_t)size) {
                set_err(err, errlen, "relocation data exceeds input size");
                free(ph);
                return -1;
            }
            if (ph[i].p_type == 0x700000A0u) {
                has_a_phdr = 1;
            }
        }
    }

    /* Allocate scratch and lay out segments. */
    for (i = 0; i < c.nseg; i++) {
        uint32_t k = 0;
        uint32_t filesz = 0;
        uint32_t memsz = c.segs[i].memsz;
        /* Find the matching loadable phdr (k-th loadable). */
        uint32_t seen = 0;
        uint32_t j;
        for (j = 0; j < (uint32_t)phnum; j++) {
            if (ph[j].p_type == 1) {
                if (seen == i) {
                    break;
                }
                seen++;
            }
        }
        k = j;
        filesz = ph[k].p_filesz;
        if (memsz > 0) {
            c.segs[i].buf = (unsigned char *)calloc(1, memsz);
            if (c.segs[i].buf == NULL) {
                uint32_t r;
                set_err(err, errlen, "out of memory");
                for (r = 0; r < i; r++) {
                    free(c.segs[r].buf);
                }
                free(ph);
                return -1;
            }
            if (filesz > 0) {
                memcpy(c.segs[i].buf, data + ph[k].p_offset, filesz);
            }
        }
    }

    /* Apply relocations in program-header order. */
    for (i = 0; i < (uint32_t)phnum; i++) {
        if (ph[i].p_type == 0x700000A0u) {
            if (apply_format_a(&c, data + ph[i].p_offset, ph[i].p_filesz) != 0) {
                uint32_t r;
                for (r = 0; r < c.nseg; r++) {
                    free(c.segs[r].buf);
                }
                free(ph);
                return -1;
            }
        } else if (ph[i].p_type == 0x700000A1u) {
            if (apply_format_b(&c, data + ph[i].p_offset, ph[i].p_filesz) != 0) {
                uint32_t r;
                for (r = 0; r < c.nseg; r++) {
                    free(c.segs[r].buf);
                }
                free(ph);
                return -1;
            }
        }
    }

    /* Format-A from section headers when no such program header exists. */
    if (!has_a_phdr && e_shoff != 0 && shnum != 0) {
        uint64_t shneed;
        const unsigned char *strtab = NULL;
        uint32_t strsz = 0;
        uint32_t si;
        if (shentsize < 40) {
            set_err(err, errlen, "bad section header entry size");
            goto fail_scratch;
        }
        shneed = (uint64_t)e_shoff + (uint64_t)shnum * (uint64_t)shentsize;
        if (shneed > (uint64_t)size) {
            set_err(err, errlen, "section headers exceed input size");
            goto fail_scratch;
        }
        if (shstrndx >= shnum) {
            set_err(err, errlen, "bad section string index");
            goto fail_scratch;
        }
        (void)strtab;
        (void)strsz;
        for (si = 0; si < shnum; si++) {
            const unsigned char *sh = data + e_shoff + (size_t)si * shentsize;
            uint32_t shtype = rd32le(sh + 4);
            uint32_t shoff = rd32le(sh + 16);
            uint32_t shsize = rd32le(sh + 20);
            if (shtype == 0x700000A0u) {
                if ((uint64_t)shoff + (uint64_t)shsize > (uint64_t)size) {
                    set_err(err, errlen, "section data exceeds input size");
                    goto fail_scratch;
                }
                if (apply_format_a(&c, data + shoff, shsize) != 0) {
                    goto fail_scratch;
                }
            }
        }
    }

    /* Module info location from the first segment's p_paddr. */
    if (p_paddr0 < p_offset0) {
        set_err(err, errlen, "module info p_paddr below segment offset");
        goto fail_scratch;
    }
    mod_off_in_seg = p_paddr0 - p_offset0;
    if ((uint64_t)mod_off_in_seg + SR_PRX_MODINFO_SIZE >
        (uint64_t)c.segs[0].memsz) {
        set_err(err, errlen, "module info block outside segment");
        goto fail_scratch;
    }
    mod_guest = c.segs[0].guest + mod_off_in_seg;

    /* Section agreement for .rodata.sceModuleInfo when present. */
    if (e_shoff != 0 && shnum != 0) {
        uint64_t shneed;
        const unsigned char *strtab = NULL;
        uint32_t strsz = 0;
        uint32_t si;
        static const char want[] = ".rodata.sceModuleInfo";
        if (shentsize < 40) {
            set_err(err, errlen, "bad section header entry size");
            goto fail_scratch;
        }
        shneed = (uint64_t)e_shoff + (uint64_t)shnum * (uint64_t)shentsize;
        if (shneed > (uint64_t)size) {
            set_err(err, errlen, "section headers exceed input size");
            goto fail_scratch;
        }
        if (shstrndx >= shnum) {
            set_err(err, errlen, "bad section string index");
            goto fail_scratch;
        }
        {
            const unsigned char *ssh =
                data + e_shoff + (size_t)shstrndx * shentsize;
            uint32_t soff = rd32le(ssh + 16);
            uint32_t ssize = rd32le(ssh + 20);
            if ((uint64_t)soff + (uint64_t)ssize > (uint64_t)size) {
                set_err(err, errlen, "section data exceeds input size");
                goto fail_scratch;
            }
            strtab = data + soff;
            strsz = ssize;
        }
        for (si = 0; si < shnum; si++) {
            const unsigned char *sh = data + e_shoff + (size_t)si * shentsize;
            uint32_t shname = rd32le(sh);
            uint32_t shaddr = rd32le(sh + 12);
            uint32_t shoff = rd32le(sh + 16);
            uint32_t shsize = rd32le(sh + 20);
            size_t k;
            int match = 0;
            if ((uint64_t)shoff + (uint64_t)shsize > (uint64_t)size) {
                set_err(err, errlen, "section data exceeds input size");
                goto fail_scratch;
            }
            if (shname >= strsz) {
                continue;
            }
            match = 1;
            for (k = 0; want[k] != '\0'; k++) {
                if ((uint64_t)shname + k >= (uint64_t)strsz ||
                    strtab[shname + k] != (unsigned char)want[k]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                if ((uint64_t)shname + sizeof want - 1 >= (uint64_t)strsz ||
                    strtab[shname + sizeof want - 1] != 0) {
                    match = 0;
                }
            }
            if (match) {
                uint64_t sec_guest64;
                uint32_t sec_guest;
                if (c.is_reloc) {
                    sec_guest64 = (uint64_t)base + (uint64_t)shaddr;
                } else {
                    sec_guest64 = shaddr;
                }
                if (sec_guest64 > 0xFFFFFFFFULL) {
                    set_err(err, errlen,
                            "section address overflows 32 bits");
                    goto fail_scratch;
                }
                sec_guest = (uint32_t)sec_guest64;
                if (sec_guest != mod_guest) {
                    set_err(err, errlen,
                            ".rodata.sceModuleInfo address disagrees with "
                            "p_paddr");
                    goto fail_scratch;
                }
            }
        }
    }

    if (c.segs[0].buf == NULL) {
        set_err(err, errlen, "module info block outside segment");
        goto fail_scratch;
    }
    memcpy(modblk, c.segs[0].buf + mod_off_in_seg, SR_PRX_MODINFO_SIZE);
    mod_attr = rd16le(modblk);
    mod_ver[0] = modblk[2];
    mod_ver[1] = modblk[3];
    memcpy(mod_name, (const char *)(modblk + 4), SR_PRX_MODNAME_LEN);
    {
        int has_nul = 0;
        uint32_t k;
        for (k = 0; k < SR_PRX_MODNAME_LEN; k++) {
            if (mod_name[k] == '\0') {
                has_nul = 1;
                break;
            }
        }
        if (!has_nul) {
            set_err(err, errlen, "module name without NUL terminator");
            goto fail_scratch;
        }
    }
    gp = rd32le(modblk + 32);
    exp_start = rd32le(modblk + 36);
    exp_end = rd32le(modblk + 40);
    imp_start = rd32le(modblk + 44);
    imp_end = rd32le(modblk + 48);

    /* Exports. */
    if (!(exp_start == 0 && exp_end == 0)) {
        uint32_t cur;
        if (exp_start > exp_end) {
            set_err(err, errlen, "export table end before start");
            goto fail_scratch;
        }
        if (exp_start == exp_end) {
            cur = exp_start;
        } else {
            if (!range_covered(&c, exp_start,
                               (uint64_t)exp_end - exp_start)) {
                set_err(err, errlen, "export table outside image");
                goto fail_scratch;
            }
            cur = exp_start;
            while (cur < exp_end) {
                unsigned char h[SR_PRX_EXPORT_REC_SIZE];
                uint32_t libptr, etab;
                uint32_t len, nvar, nfunc;
                uint64_t reclen;
                uint64_t nn;
                unsigned char *raw = NULL;
                uint32_t *nids = NULL;
                uint32_t *addrs = NULL;
                char lib[SR_PRX_LIBNAME_LEN];
                uint32_t k;
                if ((uint64_t)cur + SR_PRX_EXPORT_REC_SIZE > exp_end) {
                    set_err(err, errlen,
                            "export record crosses table end");
                    goto fail_scratch;
                }
                if (read_guest(&c, cur, h, sizeof h) != 0) {
                    set_err(err, errlen, "export record outside image");
                    goto fail_scratch;
                }
                /* Revised §3.7 layout: libname u32 @0, version 2B @4,
                 * attribute u16 @6, len u8 @8, nvar u8 @9,
                 * nfunc u16 @10, entrytable u32 @12. */
                libptr = rd32le(h);
                len = h[8];
                nvar = h[9];
                nfunc = rd16le(h + 10);
                etab = rd32le(h + 12);
                if (len == 0) {
                    set_err(err, errlen, "export record zero length");
                    goto fail_scratch;
                }
                reclen = (uint64_t)len * 4u;
                if (len < 4 || reclen < SR_PRX_EXPORT_REC_SIZE) {
                    set_err(err, errlen, "export record too short");
                    goto fail_scratch;
                }
                if ((uint64_t)cur + reclen > exp_end) {
                    set_err(err, errlen,
                            "export record crosses table end");
                    goto fail_scratch;
                }
                nn = (uint64_t)nfunc + (uint64_t)nvar;
                if (nn > SR_PRX_MAX_ENTRIES) {
                    set_err(err, errlen, "too many export entries");
                    goto fail_scratch;
                }
                if (nexp + nn > SR_PRX_MAX_ENTRIES) {
                    set_err(err, errlen, "too many export entries");
                    goto fail_scratch;
                }
                if (nn > 0) {
                    if (!range_covered(&c, etab, nn * 8u)) {
                        set_err(err, errlen,
                                "export entry table outside image");
                        goto fail_scratch;
                    }
                    raw = (unsigned char *)malloc((size_t)(nn * 8u));
                    if (raw == NULL) {
                        set_err(err, errlen, "out of memory");
                        goto fail_scratch;
                    }
                    if (read_guest(&c, etab, raw, (size_t)(nn * 8u)) != 0) {
                        free(raw);
                        set_err(err, errlen,
                                "export entry table outside image");
                        goto fail_scratch;
                    }
                    nids = (uint32_t *)malloc((size_t)(nn * 4u));
                    addrs = (uint32_t *)malloc((size_t)(nn * 4u));
                    if (nids == NULL || addrs == NULL) {
                        free(raw);
                        free(nids);
                        free(addrs);
                        set_err(err, errlen, "out of memory");
                        goto fail_scratch;
                    }
                    for (k = 0; k < (uint32_t)nn; k++) {
                        nids[k] = rd32le(raw + (size_t)k * 4);
                        addrs[k] = rd32le(raw + ((size_t)nn + k) * 4);
                    }
                    free(raw);
                    raw = NULL;
                }
                if (libptr == 0) {
                    lib[0] = '\0';
                } else {
                    if (read_guest_str(&c, libptr, lib) != 0) {
                        free(nids);
                        free(addrs);
                        set_err(err, errlen,
                                "export library name outside image or "
                                "unterminated within 256 bytes");
                        goto fail_scratch;
                    }
                }
                for (k = 0; k < (uint32_t)nn; k++) {
                    SrPrxExport e;
                    memset(&e, 0, sizeof e);
                    memcpy(e.lib, lib, sizeof e.lib);
                    e.nid = nids ? nids[k] : 0;
                    e.addr = addrs ? addrs[k] : 0;
                    e.is_func = (k < nfunc) ? 1 : 0;
                    if (nexp >= exps_cap) {
                        uint32_t ncap = exps_cap ? exps_cap * 2 : 16;
                        SrPrxExport *ng = (SrPrxExport *)realloc(
                            exps, (size_t)ncap * sizeof *ng);
                        if (ng == NULL) {
                            free(nids);
                            free(addrs);
                            set_err(err, errlen, "out of memory");
                            goto fail_scratch;
                        }
                        exps = ng;
                        exps_cap = ncap;
                    }
                    exps[nexp++] = e;
                }
                free(nids);
                free(addrs);
                cur = (uint32_t)((uint64_t)cur + reclen);
            }
        }
    }

    /* Imports. */
    if (!(imp_start == 0 && imp_end == 0)) {
        uint32_t cur;
        if (imp_start > imp_end) {
            set_err(err, errlen, "import table end before start");
            goto fail_entries;
        }
        if (imp_start == imp_end) {
            cur = imp_start;
        } else {
            if (!range_covered(&c, imp_start,
                               (uint64_t)imp_end - imp_start)) {
                set_err(err, errlen, "import table outside image");
                goto fail_entries;
            }
            cur = imp_start;
            while (cur < imp_end) {
                unsigned char h[SR_PRX_IMPORT_REC_MIN];
                uint32_t libptr, nidtab, stubtab;
                uint32_t len, nvar, nfunc;
                uint64_t reclen;
                char lib[SR_PRX_LIBNAME_LEN];
                uint32_t *nids = NULL;
                uint32_t k;
                (void)nvar;
                if ((uint64_t)cur + SR_PRX_IMPORT_REC_MIN > imp_end) {
                    set_err(err, errlen,
                            "import record crosses table end");
                    goto fail_entries;
                }
                if (read_guest(&c, cur, h, SR_PRX_IMPORT_REC_MIN) != 0) {
                    set_err(err, errlen, "import record outside image");
                    goto fail_entries;
                }
                /* Revised §3.8 layout: libname u32 @0, version 2B @4,
                 * attribute u16 @6, len u8 @8, nvar u8 @9,
                 * nfunc u16 @10, nidtab u32 @12, stubtab u32 @16.
                 * Vartable u32 @20 present only when len>=6, unused. */
                libptr = rd32le(h);
                len = h[8];
                nvar = h[9];
                nfunc = rd16le(h + 10);
                nidtab = rd32le(h + 12);
                stubtab = rd32le(h + 16);
                if (len == 0) {
                    set_err(err, errlen, "import record zero length");
                    goto fail_entries;
                }
                reclen = (uint64_t)len * 4u;
                if (len < 5 || reclen < SR_PRX_IMPORT_REC_MIN) {
                    set_err(err, errlen, "import record too short");
                    goto fail_entries;
                }
                if ((uint64_t)cur + reclen > imp_end) {
                    set_err(err, errlen,
                            "import record crosses table end");
                    goto fail_entries;
                }
                if (nfunc > SR_PRX_MAX_ENTRIES ||
                    nimp + nfunc > SR_PRX_MAX_ENTRIES) {
                    set_err(err, errlen, "too many import stubs");
                    goto fail_entries;
                }
                if (nfunc > 0) {
                    if (!range_covered(&c, nidtab, (uint64_t)nfunc * 4u)) {
                        set_err(err, errlen,
                                "import NID table outside image");
                        goto fail_entries;
                    }
                    if (!range_covered(&c, stubtab, (uint64_t)nfunc * 8u)) {
                        set_err(err, errlen,
                                "import stub array outside image");
                        goto fail_entries;
                    }
                    nids = (uint32_t *)malloc((size_t)nfunc * 4u);
                    if (nids == NULL) {
                        set_err(err, errlen, "out of memory");
                        goto fail_entries;
                    }
                    for (k = 0; k < nfunc; k++) {
                        if (read_guest_u32(&c, nidtab + (uint64_t)k * 4u,
                                           &nids[k]) != 0) {
                            free(nids);
                            set_err(err, errlen,
                                    "import NID table outside image");
                            goto fail_entries;
                        }
                    }
                }
                if (libptr == 0) {
                    lib[0] = '\0';
                } else {
                    if (read_guest_str(&c, libptr, lib) != 0) {
                        free(nids);
                        set_err(err, errlen,
                                "import library name outside image or "
                                "unterminated within 256 bytes");
                        goto fail_entries;
                    }
                }
                for (k = 0; k < nfunc; k++) {
                    SrPrxImportStub st;
                    memset(&st, 0, sizeof st);
                    memcpy(st.lib, lib, sizeof st.lib);
                    st.nid = nids[k];
                    st.stub_addr = (uint32_t)((uint64_t)stubtab +
                                             (uint64_t)k * 8u);
                    if (nimp >= imps_cap) {
                        uint32_t ncap = imps_cap ? imps_cap * 2 : 16;
                        SrPrxImportStub *ng = (SrPrxImportStub *)realloc(
                            imps, (size_t)ncap * sizeof *ng);
                        if (ng == NULL) {
                            free(nids);
                            set_err(err, errlen, "out of memory");
                            goto fail_entries;
                        }
                        imps = ng;
                        imps_cap = ncap;
                    }
                    imps[nimp++] = st;
                }
                free(nids);
                cur = (uint32_t)((uint64_t)cur + reclen);
            }
        }
    }

    /* Fill the result record; guest writes happen only after this point. */
    memcpy(out->modname, mod_name, sizeof out->modname);
    out->attr = mod_attr;
    out->version[0] = mod_ver[0];
    out->version[1] = mod_ver[1];
    out->gp = gp;
    out->entry = c.is_reloc ? (e_entry + base) : e_entry;
    out->has_mod_start = 0;
    out->mod_start = 0;
    for (s = 0; s < nexp; s++) {
        if (exps[s].lib[0] == '\0' && exps[s].nid == 0xD632ACDBu) {
            out->has_mod_start = 1;
            out->mod_start = exps[s].addr;
            break;
        }
    }
    out->start = start;
    out->end = end;
    out->nseg = c.nseg;
    for (i = 0; i < c.nseg; i++) {
        uint32_t j;
        uint32_t filesz = 0;
        uint32_t seen = 0;
        for (j = 0; j < (uint32_t)phnum; j++) {
            if (ph[j].p_type == 1) {
                if (seen == i) {
                    break;
                }
                seen++;
            }
        }
        filesz = ph[j].p_filesz;
        out->segs[i].file_offset = ph[j].p_offset;
        out->segs[i].guest_addr = c.segs[i].guest;
        out->segs[i].file_size = filesz;
        out->segs[i].mem_size = c.segs[i].memsz;
    }
    out->modinfo_addr = mod_guest;
    out->nexp = nexp;
    out->exps = exps;
    out->nimp = nimp;
    out->imps = imps;
    exps = NULL;
    imps = NULL;

    /* Commit to guest memory. */
    for (i = 0; i < c.nseg; i++) {
        if (c.segs[i].memsz == 0) {
            continue;
        }
        if (sr_prx_guest_write(c.segs[i].guest, c.segs[i].buf,
                               c.segs[i].memsz) != 0) {
            char msg[128];
            /* snprintf is standard C; keep the message one line. */
            snprintf(msg, sizeof msg,
                     "guest write failed at 0x%08X size %u",
                     c.segs[i].guest, c.segs[i].memsz);
            set_err(err, errlen, msg);
            for (s = 0; s < c.nseg; s++) {
                free(c.segs[s].buf);
            }
            free(ph);
            /* out already owns exps/imps; caller frees via image_free. */
            return -1;
        }
    }
    for (i = 0; i < c.nseg; i++) {
        free(c.segs[i].buf);
    }
    free(ph);
    return 0;

fail_entries:
    free(exps);
    free(imps);
    for (i = 0; i < c.nseg; i++) {
        free(c.segs[i].buf);
    }
    free(ph);
    return -1;

fail_scratch:
    free(exps);
    free(imps);
    for (i = 0; i < c.nseg; i++) {
        free(c.segs[i].buf);
    }
    free(ph);
    return -1;
}

int sr_prx_load_from_memory(const unsigned char *data, size_t size,
                            uint32_t base, SrPrxImage *out, char *err,
                            size_t errlen) {
    return do_load(data, size, base, out, err, errlen);
}

int sr_prx_load(const char *host_path, uint32_t base, SrPrxImage *out,
                char *err, size_t errlen) {
    FILE *f;
    long sz;
    unsigned char *buf;
    size_t got;
    int rc;
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (host_path == NULL) {
        set_err(err, errlen, "null host path");
        return -1;
    }
    f = fopen(host_path, "rb");
    if (f == NULL) {
        set_err(err, errlen, "cannot open input file");
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        set_err(err, errlen, "cannot stat input file");
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < 0) {
        set_err(err, errlen, "cannot stat input file");
        fclose(f);
        return -1;
    }
    if ((unsigned long)sz > SR_PRX_MAX_FILE_BYTES) {
        set_err(err, errlen, "PRX input exceeds 256 MiB");
        fclose(f);
        return -1;
    }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (buf == NULL) {
        set_err(err, errlen, "out of memory");
        fclose(f);
        return -1;
    }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        set_err(err, errlen, "short read of input file");
        free(buf);
        return -1;
    }
    rc = do_load(buf, got, base, out, err, errlen);
    free(buf);
    return rc;
}

void sr_prx_image_free(SrPrxImage *out) {
    if (out == NULL) {
        return;
    }
    free(out->exps);
    out->exps = NULL;
    out->nexp = 0;
    free(out->imps);
    out->imps = NULL;
    out->nimp = 0;
}
