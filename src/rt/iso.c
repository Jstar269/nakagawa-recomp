// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.

/* ISO9660 VFS with Joliet and Rock Ridge names, RR symbolic links, multi-extent files, and
 * immutable directory/path caches.  All FILE positioning and cache publication is serialized:
 * asset-streaming threads never observe partially built cache entries. */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "iso.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
#endif

#define SECTOR 2048u
#define MAX_EXTENTS 16
#define MAX_DIR_CACHE 192
#define MAX_PATH_CACHE 512
#define MAX_CHAINS 256

typedef struct { uint32_t lba, size; } IsoExtent;
typedef struct {
    char name[256];
    char link[512];
    IsoExtent ext[MAX_EXTENTS];
    uint32_t size;
    uint8_t nextents, isdir, islink, continuation, joliet, rrname;
} IsoNode;
typedef struct {
    int valid, joliet;
    uint32_t lba, size, count;
    IsoNode *nodes;
} DirCache;
typedef struct { int valid, joliet; char path[512]; IsoNode node; } PathCache;
typedef struct { int valid; uint8_t count; IsoExtent ext[MAX_EXTENTS]; } ExtentChain;

static FILE *s_iso;
static uint32_t s_iso_offset;
static uint32_t s_primary_lba, s_primary_size, s_joliet_lba, s_joliet_size;
static int s_have_joliet;
static DirCache s_dirs[MAX_DIR_CACHE];
static PathCache s_paths[MAX_PATH_CACHE];
static ExtentChain s_chains[MAX_CHAINS];
static atomic_flag s_iso_lock = ATOMIC_FLAG_INIT;

static void lock_iso(void) {
    while (atomic_flag_test_and_set_explicit(&s_iso_lock, memory_order_acquire)) { }
}
static void unlock_iso(void) { atomic_flag_clear_explicit(&s_iso_lock, memory_order_release); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_at_locked(uint32_t lba, uint32_t off, void *buf, uint32_t bytes) {
    int64_t pos = (int64_t)(s_iso_offset + lba) * SECTOR + off;
#ifdef _WIN32
    if (_fseeki64(s_iso, pos, SEEK_SET) != 0) return -1;
#else
    if (fseeko(s_iso, (off_t)pos, SEEK_SET) != 0) return -1;
#endif
    size_t got = fread(buf, 1, bytes, s_iso);
    if (got != bytes && ferror(s_iso)) clearerr(s_iso);
    return (int)got;
}
static int read_sector_locked(uint32_t lba, void *buf) {
    int got = read_at_locked(lba, 0, buf, SECTOR);
    if (got != (int)SECTOR) {
        if (got > 0) memset((unsigned char *)buf + got, 0, SECTOR - (uint32_t)got);
        return -1;
    }
    return 0;
}

/* Some Sony images contain a complete ISO in USER_L0.IMG. */
static void detect_wrapper_locked(void) {
    unsigned char pvd[SECTOR], sec[SECTOR];
    s_iso_offset = 0;
    if (read_sector_locked(16, pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) return;
    uint32_t lba = rd32(pvd + 158), size = rd32(pvd + 166);
    for (uint32_t pos = 0; pos < size;) {
        uint32_t secno = pos / SECTOR, in = pos % SECTOR;
        if (read_sector_locked(lba + secno, sec)) return;
        uint8_t len = sec[in];
        if (!len) { pos = (secno + 1) * SECTOR; continue; }
        if (in + len > SECTOR || len < 34) return;
        uint8_t nl = sec[in + 32];
        if (nl == 11 && !memcmp(sec + in + 33, "USER_L0.IMG", 11)) {
            s_iso_offset = rd32(sec + in + 2);
            if (getenv("SR_ISOLOG")) fprintf(stderr, "iso: wrapper USER_L0.IMG at sector %u\n", s_iso_offset);
            return;
        }
        pos += len;
    }
}

static int init_locked(void) {
    if (s_iso) return 0;
    const char *path = getenv("PSP_ISO");
    if (!path) path = "game.iso";
    s_iso = fopen(path, "rb");
    if (!s_iso) { fprintf(stderr, "iso: cannot open %s\n", path); return -1; }
    detect_wrapper_locked();

    unsigned char vd[SECTOR];
    for (uint32_t lba = 16; lba < 80; lba++) {
        if (read_sector_locked(lba, vd) || memcmp(vd + 1, "CD001", 5)) break;
        if (vd[0] == 255) break;
        if (vd[0] == 1) {
            s_primary_lba = rd32(vd + 158); s_primary_size = rd32(vd + 166);
        } else if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' &&
                   (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
            s_joliet_lba = rd32(vd + 158); s_joliet_size = rd32(vd + 166); s_have_joliet = 1;
        }
    }
    if (!s_primary_lba && !s_joliet_lba) { fclose(s_iso); s_iso = NULL; return -1; }
    return 0;
}

int iso_init(void) {
    int r; lock_iso(); r = init_locked(); unlock_iso(); return r;
}

static int ascii_fold(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static int name_equal(const char *a, const char *b) {
    while (*a && *b && ascii_fold((unsigned char)*a) == ascii_fold((unsigned char)*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}
static void iso_name(char *out, size_t cap, const unsigned char *src, int len, int joliet) {
    size_t n = 0;
    if (joliet) {
        for (int i = 0; i + 1 < len && n + 1 < cap; i += 2) {
            uint32_t cp = ((uint32_t)src[i] << 8) | src[i + 1];
            if (cp < 0x80) out[n++] = (char)cp;
            else if (cp < 0x800 && n + 2 < cap) { out[n++] = (char)(0xc0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 63)); }
            else if (n + 3 < cap) { out[n++] = (char)(0xe0 | (cp >> 12)); out[n++] = (char)(0x80 | ((cp >> 6) & 63)); out[n++] = (char)(0x80 | (cp & 63)); }
        }
    } else {
        while (len > 0 && src[len - 1] >= '0' && src[len - 1] <= '9') len--;
        if (len > 0 && src[len - 1] == ';') len--;
        while (len-- > 0 && n + 1 < cap) out[n++] = (char)*src++;
    }
    out[n] = 0;
}

static void append_text(char *dst, size_t cap, const char *src, size_t len) {
    size_t n = strlen(dst); if (n >= cap) return;
    if (len > cap - n - 1) len = cap - n - 1;
    memcpy(dst + n, src, len); dst[n + len] = 0;
}
static void parse_susp_locked(const unsigned char *p, uint32_t len, IsoNode *node, int depth) {
    if (depth > 3) return;
    for (uint32_t o = 0; o + 4 <= len;) {
        uint8_t elen = p[o + 2];
        if (elen < 4 || o + elen > len) break;
        if (p[o] == 'N' && p[o + 1] == 'M' && elen >= 5) {
            uint8_t flags = p[o + 4];
            if (!node->rrname) { node->name[0] = 0; node->rrname = 1; }
            if (flags & 2) strcpy(node->name, ".");
            else if (flags & 4) strcpy(node->name, "..");
            else append_text(node->name, sizeof(node->name), (const char *)p + o + 5, elen - 5);
        } else if (p[o] == 'S' && p[o + 1] == 'L' && elen >= 5) {
            node->islink = 1;
            for (uint32_t q = o + 5; q + 2 <= o + elen;) {
                uint8_t f = p[q], n = p[q + 1]; q += 2;
                if (q + n > o + elen) break;
                if (node->link[0] && node->link[strlen(node->link) - 1] != '/') append_text(node->link, sizeof(node->link), "/", 1);
                if (f & 8) strcpy(node->link, "/");
                else if (f & 4) append_text(node->link, sizeof(node->link), "..", 2);
                else if (f & 2) append_text(node->link, sizeof(node->link), ".", 1);
                else append_text(node->link, sizeof(node->link), (const char *)p + q, n);
                q += n;
            }
        } else if (p[o] == 'C' && p[o + 1] == 'E' && elen >= 28) {
            uint32_t lba = rd32(p + o + 4), off = rd32(p + o + 12), n = rd32(p + o + 20);
            if (n && n <= 64u * 1024u) {
                unsigned char *ce = (unsigned char *)malloc(n);
                if (ce && read_at_locked(lba, off, ce, n) == (int)n) parse_susp_locked(ce, n, node, depth + 1);
                free(ce);
            }
        }
        o += elen;
    }
}

static DirCache *load_dir_locked(uint32_t lba, uint32_t size, int joliet) {
    for (int i = 0; i < MAX_DIR_CACHE; i++)
        if (s_dirs[i].valid && s_dirs[i].lba == lba && s_dirs[i].size == size && s_dirs[i].joliet == joliet) return &s_dirs[i];
    int slot = -1;
    for (int i = 0; i < MAX_DIR_CACHE; i++) if (!s_dirs[i].valid) { slot = i; break; }
    if (slot < 0 || size > 64u * 1024u * 1024u) return NULL;
    uint32_t bytes = (size + SECTOR - 1) & ~(SECTOR - 1);
    unsigned char *buf = (unsigned char *)malloc(bytes ? bytes : 1);
    if (!buf || read_at_locked(lba, 0, buf, bytes) != (int)bytes) { free(buf); return NULL; }
    uint32_t cap = 32, count = 0;
    IsoNode *nodes = (IsoNode *)calloc(cap, sizeof(*nodes));
    if (!nodes) { free(buf); return NULL; }
    for (uint32_t o = 0; o < size;) {
        uint8_t rl = buf[o];
        if (!rl) { o = ((o / SECTOR) + 1) * SECTOR; continue; }
        if (rl < 34 || o + rl > bytes) break;
        uint8_t nl = buf[o + 32]; const unsigned char *nm = buf + o + 33;
        if (!(nl == 1 && (nm[0] == 0 || nm[0] == 1))) {
            IsoNode node; memset(&node, 0, sizeof(node));
            iso_name(node.name, sizeof(node.name), nm, nl, joliet);
            node.ext[0].lba = rd32(buf + o + 2); node.ext[0].size = rd32(buf + o + 10);
            node.nextents = 1; node.size = node.ext[0].size; node.isdir = (buf[o + 25] & 2) != 0;
            node.continuation = (buf[o + 25] & 0x80) != 0; node.joliet = (uint8_t)joliet;
            uint32_t sua = 33u + nl + ((nl & 1) ? 0u : 1u);
            if (!joliet && sua < rl) parse_susp_locked(buf + o + sua, rl - sua, &node, 0);
            if (count && nodes[count - 1].continuation && name_equal(nodes[count - 1].name, node.name) &&
                nodes[count - 1].nextents < MAX_EXTENTS) {
                IsoNode *prev = &nodes[count - 1];
                prev->ext[prev->nextents++] = node.ext[0]; prev->size += node.size;
                prev->continuation = node.continuation;
            } else {
                if (count == cap) {
                    cap *= 2; IsoNode *grown = (IsoNode *)realloc(nodes, cap * sizeof(*nodes));
                    if (!grown) break;
                    nodes = grown;
                }
                nodes[count++] = node;
            }
        }
        o += rl;
    }
    free(buf);
    s_dirs[slot] = (DirCache){1, joliet, lba, size, count, nodes};
    return &s_dirs[slot];
}

static const char *strip_device(const char *p) {
    const char *c = strchr(p, ':'); if (c) p = c + 1;
    while (*p == '/' || *p == '\\') p++;
    return p;
}
static void normalize_path(const char *in, char *out, size_t cap) {
    char tmp[512]; size_t n = 0; in = strip_device(in);
    while (*in && n + 1 < sizeof(tmp)) { char c = *in++; tmp[n++] = c == '\\' ? '/' : c; }
    tmp[n] = 0; out[0] = 0;
    char *parts[64]; int np = 0; char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *q = p; while (*q && *q != '/') q++; if (*q) *q++ = 0;
        if (!strcmp(p, ".")) { p = q; continue; }
        if (!strcmp(p, "..")) { if (np) np--; p = q; continue; }
        if (np < 64) parts[np++] = p;
        p = q;
    }
    for (int i = 0; i < np; i++) {
        if (i) append_text(out, cap, "/", 1);
        append_text(out, cap, parts[i], strlen(parts[i]));
    }
}

static int resolve_locked(const char *path, int joliet, IsoNode *out, int depth) {
    if (depth > 16) return -1;
    uint32_t lba = joliet ? s_joliet_lba : s_primary_lba;
    uint32_t size = joliet ? s_joliet_size : s_primary_size;
    IsoNode current; memset(&current, 0, sizeof(current)); current.isdir = 1;
    current.ext[0] = (IsoExtent){lba, size}; current.nextents = 1; current.size = size; current.joliet = (uint8_t)joliet;
    char work[512]; normalize_path(path, work, sizeof(work));
    char traversed[512] = ""; char *p = work;
    while (*p) {
        char *slash = strchr(p, '/'); if (slash) *slash = 0;
        DirCache *d = load_dir_locked(lba, size, joliet); if (!d) return -1;
        IsoNode *match = NULL;
        for (uint32_t i = 0; i < d->count; i++) if (name_equal(d->nodes[i].name, p)) { match = &d->nodes[i]; break; }
        if (!match) return -1;
        current = *match;
        char rest[512] = ""; if (slash) strncpy(rest, slash + 1, sizeof(rest) - 1);
        if (current.islink) {
            char redirected[1024];
            redirected[0] = 0;
            if (current.link[0] != '/') {
                append_text(redirected, sizeof(redirected), traversed, strlen(traversed));
                if (traversed[0]) append_text(redirected, sizeof(redirected), "/", 1);
            }
            append_text(redirected, sizeof(redirected), current.link + (current.link[0] == '/'),
                        strlen(current.link + (current.link[0] == '/')));
            if (rest[0]) append_text(redirected, sizeof(redirected), "/", 1);
            append_text(redirected, sizeof(redirected), rest, strlen(rest));
            return resolve_locked(redirected, joliet, out, depth + 1);
        }
        if (traversed[0]) append_text(traversed, sizeof(traversed), "/", 1);
        append_text(traversed, sizeof(traversed), p, strlen(p));
        if (!slash) break;
        if (!current.isdir) return -1;
        lba = current.ext[0].lba; size = current.size; p = slash + 1;
    }
    *out = current; return 0;
}

static unsigned path_hash(const char *p, int joliet) {
    uint32_t h = joliet ? 2166136261u : 16777619u;
    while (*p) { h ^= (uint8_t)ascii_fold((unsigned char)*p++); h *= 16777619u; }
    return h % MAX_PATH_CACHE;
}
static int cached_resolve_locked(const char *path, IsoNode *out) {
    char norm[512]; normalize_path(path, norm, sizeof(norm));
    int order[2] = {s_have_joliet, 0};
    for (int pass = 0; pass < (s_have_joliet ? 2 : 1); pass++) {
        int joliet = order[pass]; unsigned slot = path_hash(norm, joliet);
        PathCache *pc = &s_paths[slot];
        if (pc->valid && pc->joliet == joliet && name_equal(pc->path, norm)) { *out = pc->node; return 0; }
        IsoNode node;
        if (resolve_locked(norm, joliet, &node, 0) == 0) {
            pc->valid = 1; pc->joliet = joliet; strncpy(pc->path, norm, sizeof(pc->path) - 1); pc->path[sizeof(pc->path) - 1] = 0;
            pc->node = node; *out = node; return 0;
        }
    }
    return -1;
}

static uint32_t publish_chain_locked(const IsoNode *node) {
    if (node->nextents <= 1) return node->ext[0].lba;
    for (int i = 0; i < MAX_CHAINS; i++) {
        if (s_chains[i].valid && s_chains[i].count == node->nextents &&
            !memcmp(s_chains[i].ext, node->ext, node->nextents * sizeof(node->ext[0]))) return 0x80000000u | (uint32_t)(i + 1);
    }
    for (int i = 0; i < MAX_CHAINS; i++) if (!s_chains[i].valid) {
        s_chains[i].valid = 1; s_chains[i].count = node->nextents;
        memcpy(s_chains[i].ext, node->ext, node->nextents * sizeof(node->ext[0]));
        return 0x80000000u | (uint32_t)(i + 1);
    }
    return 0;
}

int iso_lookup(const char *guest_path, uint32_t *out_lba, uint32_t *out_size) {
    if (!guest_path || !out_lba || !out_size) return -1;
    lock_iso();
    if (init_locked()) { unlock_iso(); return -1; }
    const char *raw = strstr(guest_path, "sce_lbn");
    if (raw) {
        char *end; unsigned long lba = strtoul(raw + 7, &end, 0);
        const char *sz = strstr(end, "_size"); unsigned long size = sz ? strtoul(sz + 5, NULL, 0) : SECTOR;
        *out_lba = (uint32_t)lba; *out_size = (uint32_t)size; unlock_iso(); return 0;
    }
    IsoNode node; int r = cached_resolve_locked(guest_path, &node);
    if (!r) {
        uint32_t token = publish_chain_locked(&node);
        if (!token && node.nextents > 1) r = -1;
        else { *out_lba = token; *out_size = node.size; }
    }
    unlock_iso();
    return r;
}

uint32_t iso_physical_lba(uint32_t token) {
    uint32_t r = token; lock_iso();
    if (token & 0x80000000u) {
        uint32_t i = (token & 0x7fffffffu) - 1;
        r = i < MAX_CHAINS && s_chains[i].valid ? s_chains[i].ext[0].lba : 0;
    }
    unlock_iso(); return r;
}

int iso_read(uint32_t token, uint32_t offset, void *dst, uint32_t bytes) {
    if (!dst) return -1;
    lock_iso(); if (init_locked()) { unlock_iso(); return -1; }
    int total = 0;
    if (!(token & 0x80000000u)) total = read_at_locked(token, offset, dst, bytes);
    else {
        uint32_t i = (token & 0x7fffffffu) - 1;
        if (i >= MAX_CHAINS || !s_chains[i].valid) { unlock_iso(); return -1; }
        ExtentChain *c = &s_chains[i]; uint32_t logical = 0, remain = bytes;
        for (uint8_t e = 0; e < c->count && remain; e++) {
            uint32_t end = logical + c->ext[e].size;
            if (offset < end) {
                uint32_t within = offset > logical ? offset - logical : 0;
                uint32_t n = c->ext[e].size - within; if (n > remain) n = remain;
                int got = read_at_locked(c->ext[e].lba, within, (unsigned char *)dst + total, n);
                if (got <= 0) break;
                total += got; remain -= (uint32_t)got; offset += (uint32_t)got;
                if ((uint32_t)got < n) break;
            }
            logical = end;
        }
    }
    unlock_iso(); return total;
}

int iso_list(const char *guest_path, uint32_t index, IsoDirEntry *out) {
    if (!guest_path || !out) return -1;
    lock_iso(); if (init_locked()) { unlock_iso(); return -1; }
    IsoNode dir; int r = cached_resolve_locked(guest_path, &dir);
    if (r || !dir.isdir) { unlock_iso(); return -1; }
    int joliet = dir.joliet;
    DirCache *d = load_dir_locked(dir.ext[0].lba, dir.size, joliet);
    if (!d && joliet) d = load_dir_locked(dir.ext[0].lba, dir.size, 0);
    if (!d) { unlock_iso(); return -1; }
    if (index >= d->count) { unlock_iso(); return 0; }
    IsoNode *n = &d->nodes[index]; memset(out, 0, sizeof(*out));
    strncpy(out->name, n->name, sizeof(out->name) - 1); out->size = n->size;
    out->lba = n->ext[0].lba; out->is_dir = n->isdir; out->is_symlink = n->islink;
    if (n->islink) {
        char child[1024]; char norm[512]; normalize_path(guest_path, norm, sizeof(norm));
        snprintf(child, sizeof(child), "%s%s%s", norm, norm[0] ? "/" : "", n->name);
        IsoNode target;
        if (cached_resolve_locked(child, &target) == 0) {
            out->size = target.size; out->lba = target.ext[0].lba; out->is_dir = target.isdir;
        }
    }
    unlock_iso(); return 1;
}
