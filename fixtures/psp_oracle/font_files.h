// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Read-only copy of the console's system font files (oracle case font-files, evidence id
 * PSP-FONTFILES-001).
 *
 * The case lists flash0:/font and copies every regular file in it to host0:/fonts/ byte for
 * byte, so a maintainer can import the fonts of their own console. The copies and their digests
 * are private evidence; nothing here names a font or a file: names come from the listing at run
 * time. Shared by the probe (fixtures/psp_oracle/probe.c, CASE=font-files) and a host harness
 * (fixtures/psp_oracle/io_cases_host.c) that runs the same code against a fake file system.
 *
 * The user-facing font-copy app (src/psp_companion/fontcopy.c) runs the same copy with three
 * settings the oracle case leaves unset, so the case's calls and records are unchanged:
 * dest_dir (its own memory-stick folder instead of host0:/fonts, with dest_parent created
 * first), pgf_only (only names ending in .pgf, in any case, are copied) and known_names (if the
 * listing cannot be opened, each of these names is opened read-only under flash0:/font instead).
 *
 * Safety contract (checked by the host harness tests in tools/test_psp_oracle.py):
 *  - flash0: is only ever listed (sceIoDopen/sceIoDread/sceIoDclose), opened PSP_O_RDONLY and,
 *    for a known name, measured with sceIoLseek. No write, create, truncate, remove, rename or
 *    mkdir names a flash0: path.
 *  - Writes go only to <destination>/<name> (host0:/fonts unless dest_dir names another folder),
 *    where <name> is a listed or known regular file whose name is 1 to 63 bytes of
 *    [A-Za-z0-9_.-] not starting with '.'; other names are recorded and skipped. sceIoMkdir names
 *    only dest_parent and the destination.
 *  - If the listing cannot be opened and no known name is given, the case records the result
 *    and stops; nothing is copied.
 *  - Each copy is verified: pass A reads flash0 and writes host0 while hashing (SHA-256), pass B
 *    reads the host0 copy back, pass C re-reads flash0. A copy passes only when the three digests
 *    and byte counts agree with each other and with the listed size.
 *  - Every loop is bounded (entries, file size, chunk count, write retries) and every buffer is
 *    static. Every descriptor opened is closed.
 *
 * The includer provides SceUID, SceIoDirent, SceMode, SceSize, PSP_O_RDONLY, PSP_O_WRONLY,
 * PSP_O_CREAT, PSP_O_TRUNC, FIO_S_ISREG and the IoFileMgrForUser calls used below (pspiofilemgr.h
 * on the PSP; the fake on the host). */

#ifndef NAKAGAWA_FONT_FILES_H
#define NAKAGAWA_FONT_FILES_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FONTFILES_TEST_ID "PSP-FONTFILES-001"
#define FONTFILES_DIR "flash0:/font"
#define FONTFILES_DIR_SLASH "flash0:/font/"
#define FONTFILES_HOST_DIR "host0:/fonts"
#define FONTFILES_ENTRIES_MAX 64u
#define FONTFILES_NAME_MAX 63u
#define FONTFILES_FILE_MAX (16u * 1024u * 1024u)
#define FONTFILES_CHUNK (64u * 1024u)
#define FONTFILES_CHUNKS_MAX (FONTFILES_FILE_MAX / FONTFILES_CHUNK + 2u)
#define FONTFILES_WRITE_TRIES 64u
#define FONTFILES_PATH_MAX 96u
#define FONTFILES_LINE_MAX 1024u
#define FONTFILES_ERROR 0xffffffffu

_Static_assert(sizeof(FONTFILES_HOST_DIR) + 1u + FONTFILES_NAME_MAX < FONTFILES_PATH_MAX,
               "a host0 copy path must fit FONTFILES_PATH_MAX");

/* out4 of a font-entry record. */
enum {
    FONTFILES_ENTRY_COPY = 0,      /* regular file with a safe name, within the size bound */
    FONTFILES_ENTRY_NOT_FILE = 1,  /* not a regular file (directory, link, ...): not copied */
    FONTFILES_ENTRY_NAME = 2,      /* name not 1-63 bytes of [A-Za-z0-9_.-] or starts with '.' */
    FONTFILES_ENTRY_SIZE = 3,      /* size negative or above FONTFILES_FILE_MAX: not copied */
    FONTFILES_ENTRY_NOT_PGF = 4,   /* pgf_only: the name does not end in .pgf: not copied */
    FONTFILES_ENTRY_ABSENT = 5,    /* a known name that does not open: not copied */
};

/* out8 bits of a font-copy record. */
#define FONTFILES_MATCH_READBACK 1u /* host0 copy re-read: same bytes and digest as pass A */
#define FONTFILES_MATCH_REREAD 2u   /* flash0 re-read: same bytes and digest as pass A */
#define FONTFILES_MATCH_SIZE 4u     /* pass A byte count equals the listed size */
#define FONTFILES_MATCH_ALL 7u

typedef struct FontFiles {
    void (*emit)(void *ctx, const char *line, size_t length);
    void (*step)(void *ctx, const char *step);
    void *ctx;
    /* Copy settings; all zero for the oracle case (see the header comment). */
    const char *dest_parent;          /* created before dest_dir; NULL for none */
    const char *dest_dir;             /* NULL: FONTFILES_HOST_DIR */
    uint32_t pgf_only;
    const char *const *known_names;   /* tried only when the listing cannot be opened */
    uint32_t known_count;
    uint32_t entries;
    uint32_t candidates;
    uint32_t copied;
    uint32_t records;
} FontFiles;

typedef struct FontFilesSha256 {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[64];
    uint32_t used;
} FontFilesSha256;

static SceIoDirent s_fontfiles_dirent;
static char s_fontfiles_names[FONTFILES_ENTRIES_MAX][FONTFILES_NAME_MAX + 1u];
static uint32_t s_fontfiles_sizes[FONTFILES_ENTRIES_MAX];
static uint8_t s_fontfiles_chunk[FONTFILES_CHUNK];
static char s_fontfiles_line[FONTFILES_LINE_MAX];
static char s_fontfiles_path[FONTFILES_PATH_MAX];
static char s_fontfiles_host[FONTFILES_PATH_MAX];
static char s_fontfiles_step[FONTFILES_PATH_MAX + 16u];

/* SHA-256 as specified in FIPS 180-4. */
static const uint32_t s_fontfiles_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

static uint32_t fontfiles_rotr(uint32_t value, unsigned int amount) {
    return (value >> amount) | (value << (32u - amount));
}

static void fontfiles_sha_block(FontFilesSha256 *sha, const uint8_t *block) {
    static uint32_t w[64]; /* static: the probe starts on a small stack */
    for (unsigned int i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)block[4u * i] << 24) | ((uint32_t)block[4u * i + 1u] << 16) |
               ((uint32_t)block[4u * i + 2u] << 8) | (uint32_t)block[4u * i + 3u];
    }
    for (unsigned int i = 16; i < 64u; i++) {
        uint32_t s0 = fontfiles_rotr(w[i - 15u], 7) ^ fontfiles_rotr(w[i - 15u], 18) ^
                      (w[i - 15u] >> 3);
        uint32_t s1 = fontfiles_rotr(w[i - 2u], 17) ^ fontfiles_rotr(w[i - 2u], 19) ^
                      (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    uint32_t a = sha->state[0], b = sha->state[1], c = sha->state[2], d = sha->state[3];
    uint32_t e = sha->state[4], f = sha->state[5], g = sha->state[6], h = sha->state[7];
    for (unsigned int i = 0; i < 64u; i++) {
        uint32_t t1 = h + (fontfiles_rotr(e, 6) ^ fontfiles_rotr(e, 11) ^ fontfiles_rotr(e, 25)) +
                      ((e & f) ^ (~e & g)) + s_fontfiles_k[i] + w[i];
        uint32_t t2 = (fontfiles_rotr(a, 2) ^ fontfiles_rotr(a, 13) ^ fontfiles_rotr(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    sha->state[0] += a;
    sha->state[1] += b;
    sha->state[2] += c;
    sha->state[3] += d;
    sha->state[4] += e;
    sha->state[5] += f;
    sha->state[6] += g;
    sha->state[7] += h;
}

static void fontfiles_sha_init(FontFilesSha256 *sha) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    memcpy(sha->state, initial, sizeof(initial));
    sha->bytes = 0u;
    sha->used = 0u;
}

static void fontfiles_sha_update(FontFilesSha256 *sha, const uint8_t *data, size_t length) {
    sha->bytes += length;
    while (length > 0u) {
        size_t take = 64u - sha->used;
        if (take > length) take = length;
        memcpy(sha->block + sha->used, data, take);
        sha->used += (uint32_t)take;
        data += take;
        length -= take;
        if (sha->used == 64u) {
            fontfiles_sha_block(sha, sha->block);
            sha->used = 0u;
        }
    }
}

static void fontfiles_sha_final(FontFilesSha256 *sha, uint8_t digest[32]) {
    const uint64_t bits = sha->bytes * 8u;
    const uint8_t pad = 0x80u;
    const uint8_t zero = 0u;
    fontfiles_sha_update(sha, &pad, 1u);
    while (sha->used != 56u) fontfiles_sha_update(sha, &zero, 1u);
    uint8_t length[8];
    for (unsigned int i = 0; i < 8u; i++) length[i] = (uint8_t)(bits >> (56u - 8u * i));
    fontfiles_sha_update(sha, length, 8u);
    for (unsigned int i = 0; i < 8u; i++) {
        digest[4u * i] = (uint8_t)(sha->state[i] >> 24);
        digest[4u * i + 1u] = (uint8_t)(sha->state[i] >> 16);
        digest[4u * i + 2u] = (uint8_t)(sha->state[i] >> 8);
        digest[4u * i + 3u] = (uint8_t)sha->state[i];
    }
}

static int fontfiles_hex(char *out, size_t capacity, const uint8_t *bytes, size_t count) {
    static const char digits[] = "0123456789abcdef";
    if (2u * count + 1u > capacity) return 0;
    for (size_t i = 0; i < count; i++) {
        out[2u * i] = digits[bytes[i] >> 4];
        out[2u * i + 1u] = digits[bytes[i] & 0x0fu];
    }
    out[2u * count] = '\0';
    return 1;
}

/* 1-63 bytes of [A-Za-z0-9_.-], not starting with '.': no path, no "." or "..". */
static size_t fontfiles_safe_name(const char *name) {
    size_t length = 0u;
    while (length <= FONTFILES_NAME_MAX && name[length] != '\0') {
        const char c = name[length];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) return 0u;
        length++;
    }
    if (length == 0u || length > FONTFILES_NAME_MAX || name[0] == '.') return 0u;
    return length;
}

/* A safe name of the given length that ends in ".pgf", in any case. */
static int fontfiles_is_pgf(const char *name, size_t length) {
    if (length < 5u) return 0;
    const char *tail = name + length - 4u;
    return tail[0] == '.' && (tail[1] | 0x20) == 'p' && (tail[2] | 0x20) == 'g' &&
           (tail[3] | 0x20) == 'f';
}

/* Where copies go, and whether that folder leaves room for any safe name. */
static const char *fontfiles_dest(const FontFiles *f) {
    return f->dest_dir != NULL ? f->dest_dir : FONTFILES_HOST_DIR;
}

static int fontfiles_dest_fits(const FontFiles *f) {
    const char *dest = fontfiles_dest(f);
    const size_t length = strlen(dest);
    if (length == 0u || length + 1u + FONTFILES_NAME_MAX >= FONTFILES_PATH_MAX) return 0;
    return f->dest_parent == NULL || strlen(f->dest_parent) < length;
}

static void fontfiles_emit(FontFiles *f, int used) {
    if (used <= 0 || (size_t)used + 1u >= FONTFILES_LINE_MAX) return;
    s_fontfiles_line[used++] = '\n';
    s_fontfiles_line[used] = '\0';
    f->emit(f->ctx, s_fontfiles_line, (size_t)used);
    f->records++;
}

static void fontfiles_step(FontFiles *f, const char *prefix, const char *path) {
    int used = snprintf(s_fontfiles_step, sizeof(s_fontfiles_step), "%s%s", prefix, path);
    if (used > 0 && (size_t)used < sizeof(s_fontfiles_step)) f->step(f->ctx, s_fontfiles_step);
}

/* Read one whole file (at most FONTFILES_FILE_MAX bytes) through the chunk buffer, hashing it,
 * optionally writing each chunk to an open host0 descriptor. Returns the byte count read, or a
 * negative result; *write_result receives the first failing write result (0 when none). */
static int64_t fontfiles_pass(SceUID in, SceUID out, FontFilesSha256 *sha, int64_t *written,
                              int *write_result) {
    int64_t total = 0;
    *written = 0;
    *write_result = 0;
    for (uint32_t chunk = 0; chunk < FONTFILES_CHUNKS_MAX; chunk++) {
        const int got = sceIoRead(in, s_fontfiles_chunk, FONTFILES_CHUNK);
        if (got < 0) return got;
        if (got == 0) return total;
        if ((uint32_t)got > FONTFILES_CHUNK || total + got > (int64_t)FONTFILES_FILE_MAX) {
            return (int)0x80000104; /* project code: the file outgrew FONTFILES_FILE_MAX */
        }
        fontfiles_sha_update(sha, s_fontfiles_chunk, (size_t)got);
        total += got;
        if (out >= 0 && *write_result == 0) {
            uint32_t offset = 0u;
            for (uint32_t tries = 0; tries < FONTFILES_WRITE_TRIES && offset < (uint32_t)got;
                 tries++) {
                const int put = sceIoWrite(out, s_fontfiles_chunk + offset,
                                           (SceSize)((uint32_t)got - offset));
                if (put <= 0) {
                    *write_result = put < 0 ? put : (int)0x80000105;
                    break;
                }
                offset += (uint32_t)put;
            }
            if (*write_result == 0 && offset != (uint32_t)got) *write_result = (int)0x80000105;
            *written += offset;
        }
    }
    return (int)0x80000104;
}

/* Hash one file read-only (passes B and C). Returns the byte count or a negative result. */
static int64_t fontfiles_hash_file(const char *path, int *open_result, uint8_t digest[32]) {
    static FontFilesSha256 sha;
    int64_t written = 0;
    int write_result = 0;
    fontfiles_sha_init(&sha);
    const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    *open_result = fd;
    if (fd < 0) return fd;
    const int64_t total = fontfiles_pass(fd, -1, &sha, &written, &write_result);
    (void)sceIoClose(fd);
    fontfiles_sha_final(&sha, digest);
    return total;
}

static void fontfiles_copy(FontFiles *f, uint32_t index) {
    const char *name = s_fontfiles_names[index];
    static char digest_a[65], digest_b[65], digest_c[65];
    static uint8_t sha_a[32], sha_b[32], sha_c[32];
    memset(sha_a, 0, sizeof(sha_a));
    memset(sha_b, 0, sizeof(sha_b));
    memset(sha_c, 0, sizeof(sha_c));
    snprintf(s_fontfiles_path, sizeof(s_fontfiles_path), "%s/%.63s", FONTFILES_DIR, name);
    snprintf(s_fontfiles_host, sizeof(s_fontfiles_host), "%s/%.63s", fontfiles_dest(f), name);

    /* Pass A: flash0 read-only -> host0, hashing what was read. */
    static FontFilesSha256 sha;
    fontfiles_sha_init(&sha);
    int64_t read_a = (int64_t)(int32_t)FONTFILES_ERROR;
    int64_t written = 0;
    int write_result = 0;
    int host_open = (int)FONTFILES_ERROR;
    fontfiles_step(f, "open:", s_fontfiles_path);
    const SceUID in = sceIoOpen(s_fontfiles_path, PSP_O_RDONLY, 0);
    if (in >= 0) {
        fontfiles_step(f, "write:", s_fontfiles_host);
        const SceUID out = sceIoOpen(s_fontfiles_host, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                                     0644);
        host_open = out;
        if (out >= 0) {
            read_a = fontfiles_pass(in, out, &sha, &written, &write_result);
            (void)sceIoClose(out);
        }
        (void)sceIoClose(in);
    }
    fontfiles_sha_final(&sha, sha_a);

    /* Pass B: the host0 copy read back. Pass C: flash0 read again. */
    int readback_open = (int)FONTFILES_ERROR;
    int reread_open = (int)FONTFILES_ERROR;
    int64_t read_b = (int64_t)(int32_t)FONTFILES_ERROR;
    int64_t read_c = (int64_t)(int32_t)FONTFILES_ERROR;
    if (in >= 0 && host_open >= 0) {
        fontfiles_step(f, "readback:", s_fontfiles_host);
        read_b = fontfiles_hash_file(s_fontfiles_host, &readback_open, sha_b);
        fontfiles_step(f, "reread:", s_fontfiles_path);
        read_c = fontfiles_hash_file(s_fontfiles_path, &reread_open, sha_c);
    }
    uint32_t match = 0u;
    if (read_a >= 0 && write_result == 0 && written == read_a && read_b == read_a &&
        memcmp(sha_a, sha_b, sizeof(sha_a)) == 0) match |= FONTFILES_MATCH_READBACK;
    if (read_a >= 0 && read_c == read_a && memcmp(sha_a, sha_c, sizeof(sha_a)) == 0) {
        match |= FONTFILES_MATCH_REREAD;
    }
    if (read_a >= 0 && (uint64_t)read_a == (uint64_t)s_fontfiles_sizes[index]) {
        match |= FONTFILES_MATCH_SIZE;
    }
    if (match == FONTFILES_MATCH_ALL) f->copied++;
    int result = 0;
    if (in < 0) result = in;
    else if (host_open < 0) result = host_open;
    else if (read_a < 0) result = (int)read_a;
    else if (write_result != 0) result = write_result;
    else if (match != FONTFILES_MATCH_ALL) result = (int)0x80000106;
    (void)fontfiles_hex(digest_a, sizeof(digest_a), sha_a, sizeof(sha_a));
    (void)fontfiles_hex(digest_b, sizeof(digest_b), sha_b, sizeof(sha_b));
    (void)fontfiles_hex(digest_c, sizeof(digest_c), sha_c, sizeof(sha_c));
    /* font-copy-NNNN: result = first failure (0 when the copy verified), out0 flash0 open,
     * out1 pass A bytes, out2 host0 open, out3 bytes written, out4 read-back open, out5 read-back
     * bytes, out6 re-read open, out7 re-read bytes, out8 FONTFILES_MATCH_* bits. */
    const int used = snprintf(s_fontfiles_line, FONTFILES_LINE_MAX,
        "NAKAGAWA_PSP_TEST schema=1 test_id=" FONTFILES_TEST_ID " case_id=font-copy-%04u "
        "status=%s result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x out4=0x%08x "
        "out5=0x%08x out6=0x%08x out7=0x%08x out8=0x%08x detail=%s sha256=%s "
        "readback_sha256=%s reread_sha256=%s",
        (unsigned int)index, result == 0 ? "PASS" : "FAIL", (unsigned int)result,
        (unsigned int)in, (unsigned int)(uint32_t)read_a, (unsigned int)host_open,
        (unsigned int)(uint32_t)written, (unsigned int)readback_open,
        (unsigned int)(uint32_t)read_b, (unsigned int)reread_open,
        (unsigned int)(uint32_t)read_c, (unsigned int)match, name, digest_a, digest_b,
        digest_c);
    fontfiles_emit(f, used);
}

/* font-mkdir-parent (only when dest_parent is set), then font-mkdir: result = sceIoMkdir
 * result. A folder that already exists is not an error here; the copies show whether it is
 * usable. */
static void fontfiles_mkdir(FontFiles *f) {
    int used;
    if (f->dest_parent != NULL) {
        fontfiles_step(f, "mkdir:", f->dest_parent);
        const int parent_rc = sceIoMkdir(f->dest_parent, 0777);
        used = snprintf(s_fontfiles_line, FONTFILES_LINE_MAX,
            "NAKAGAWA_PSP_TEST schema=1 test_id=" FONTFILES_TEST_ID " case_id=font-mkdir-parent "
            "status=PASS result=0x%08x", (unsigned int)parent_rc);
        fontfiles_emit(f, used);
    }
    fontfiles_step(f, "mkdir:", fontfiles_dest(f));
    const int mkdir_rc = sceIoMkdir(fontfiles_dest(f), 0777);
    used = snprintf(s_fontfiles_line, FONTFILES_LINE_MAX,
        "NAKAGAWA_PSP_TEST schema=1 test_id=" FONTFILES_TEST_ID " case_id=font-mkdir "
        "status=PASS result=0x%08x", (unsigned int)mkdir_rc);
    fontfiles_emit(f, used);
}

/* The listing could not be opened: open each known name read-only instead and measure it with
 * sceIoLseek. font-known-NNNN: result = the sceIoOpen result (0xffffffff when the name was not
 * opened), out0/out1 = the measured size low/high (0xffffffff when not measured), out2 =
 * disposition; detail = the name, or - when it is not a safe name. */
static void fontfiles_known(FontFiles *f) {
    for (uint32_t i = 0; i < f->known_count && i < FONTFILES_ENTRIES_MAX; i++) {
        const char *name = f->known_names[i] != NULL ? f->known_names[i] : "";
        const size_t length = fontfiles_safe_name(name);
        uint32_t disposition = FONTFILES_ENTRY_COPY;
        int open_rc = (int)FONTFILES_ERROR;
        int64_t size = -1;
        if (length == 0u) {
            disposition = FONTFILES_ENTRY_NAME;
        } else if (f->pgf_only && !fontfiles_is_pgf(name, length)) {
            disposition = FONTFILES_ENTRY_NOT_PGF;
        } else {
            snprintf(s_fontfiles_path, sizeof(s_fontfiles_path), "%s/%.63s", FONTFILES_DIR, name);
            fontfiles_step(f, "open:", s_fontfiles_path);
            const SceUID fd = sceIoOpen(s_fontfiles_path, PSP_O_RDONLY, 0);
            open_rc = fd;
            if (fd < 0) {
                disposition = FONTFILES_ENTRY_ABSENT;
            } else {
                fontfiles_step(f, "seek:", s_fontfiles_path);
                size = (int64_t)sceIoLseek(fd, 0, SEEK_END);
                (void)sceIoClose(fd);
                if (size < 0 || size > (int64_t)FONTFILES_FILE_MAX) {
                    disposition = FONTFILES_ENTRY_SIZE;
                }
            }
        }
        const uint64_t bits = size < 0 ? UINT64_MAX : (uint64_t)size;
        const int used = snprintf(s_fontfiles_line, FONTFILES_LINE_MAX,
            "NAKAGAWA_PSP_TEST schema=1 test_id=" FONTFILES_TEST_ID " case_id=font-known-%04u "
            "status=PASS result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x detail=%s",
            (unsigned int)i, (unsigned int)open_rc, (unsigned int)(uint32_t)(bits & 0xffffffffu),
            (unsigned int)(uint32_t)(bits >> 32), (unsigned int)disposition,
            length != 0u ? name : "-");
        fontfiles_emit(f, used);
        f->entries++;
        if (disposition == FONTFILES_ENTRY_COPY) {
            memcpy(s_fontfiles_names[f->candidates], name, length + 1u);
            s_fontfiles_sizes[f->candidates] = (uint32_t)size;
            f->candidates++;
        }
    }
}

/* font-dir-open, font-mkdir, font-entry-NNNN..., font-dir-close, font-copy-NNNN...,
 * font-files-done. A listing that cannot be opened goes straight to font-files-done, unless
 * known names are given: then font-known-NNNN..., and font-mkdir and font-copy-NNNN... when
 * one of them opened. A destination that cannot hold a copy path is refused (font-dest) before
 * any call. */
static void fontfiles_list_and_copy(FontFiles *f) {
    int used;
    f->step(f->ctx, "dopen:" FONTFILES_DIR);
    SceUID dir = sceIoDopen(FONTFILES_DIR);
    const int first = dir;
    int second = (int)FONTFILES_ERROR;
    if (dir < 0) {
        f->step(f->ctx, "dopen:" FONTFILES_DIR_SLASH);
        dir = sceIoDopen(FONTFILES_DIR_SLASH);
        second = dir;
    }
    used = snprintf(s_fontfiles_line, FONTFILES_LINE_MAX,
        "NAKAGAWA_PSP_TEST schema=1 test_id=" FONTFILES_TEST_ID " case_id=font-dir-open "
        "status=%s result=0x%08x out0=0x%08x out1=0x%08x",
        dir >= 0 ? "PASS" : "FAIL", (unsigned int)dir, (unsigned int)first,
        (unsigned int)second);
    fontfiles_emit(f, used);
    if (dir >= 0) {
        fontfiles_mkdir(f);
        int end = (int)FONTFILES_ERROR;
        for (uint32_t i = 0; i < FONTFILES_ENTRIES_MAX; i++) {
            memset(&s_fontfiles_dirent, 0, sizeof(s_fontfiles_dirent));
            const int got = sceIoDread(dir, &s_fontfiles_dirent);
            if (got <= 0) {
                end = got;
                break;
            }
            s_fontfiles_dirent.d_name[sizeof(s_fontfiles_dirent.d_name) - 1u] = '\0';
            const char *name = s_fontfiles_dirent.d_name;
            const int64_t size = (int64_t)s_fontfiles_dirent.d_stat.st_size;
            const size_t length = fontfiles_safe_name(name);
            uint32_t disposition = FONTFILES_ENTRY_COPY;
            if (!FIO_S_ISREG(s_fontfiles_dirent.d_stat.st_mode)) {
                disposition = FONTFILES_ENTRY_NOT_FILE;
            } else if (length == 0u) {
                disposition = FONTFILES_ENTRY_NAME;
            } else if (f->pgf_only && !fontfiles_is_pgf(name, length)) {
                disposition = FONTFILES_ENTRY_NOT_PGF;
            } else if (size < 0 || size > (int64_t)FONTFILES_FILE_MAX) {
                disposition = FONTFILES_ENTRY_SIZE;
            }
            /* font-entry-NNNN: result = sceIoDread result, out0 st_mode, out1 st_attr, out2/out3
             * st_size low/high, out4 disposition; detail = the name when it is safe to print,
             * name_hex = its bytes (up to 64) otherwise. */
            used = snprintf(s_fontfiles_line, FONTFILES_LINE_MAX,
                "NAKAGAWA_PSP_TEST schema=1 test_id=" FONTFILES_TEST_ID " case_id=font-entry-%04u "
                "status=PASS result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x "
                "out4=0x%08x ",
                (unsigned int)i, (unsigned int)got,
                (unsigned int)s_fontfiles_dirent.d_stat.st_mode,
                (unsigned int)s_fontfiles_dirent.d_stat.st_attr,
                (unsigned int)(uint32_t)((uint64_t)size & 0xffffffffu),
                (unsigned int)(uint32_t)((uint64_t)size >> 32), (unsigned int)disposition);
            if (used > 0 && (size_t)used < FONTFILES_LINE_MAX) {
                size_t raw = strlen(name);
                if (raw > 64u) raw = 64u;
                int more;
                if (fontfiles_safe_name(name) != 0u) {
                    more = snprintf(s_fontfiles_line + used, FONTFILES_LINE_MAX - (size_t)used,
                                    "detail=%s", name);
                } else {
                    static char hex[129];
                    if (!fontfiles_hex(hex, sizeof(hex), (const uint8_t *)name, raw)) hex[0] = '\0';
                    more = snprintf(s_fontfiles_line + used, FONTFILES_LINE_MAX - (size_t)used,
                                    "name_hex=%s", raw == 0u ? "00" : hex);
                }
                used = more < 0 ? -1 : used + more;
            }
            fontfiles_emit(f, used);
            f->entries++;
            if (disposition == FONTFILES_ENTRY_COPY) {
                memcpy(s_fontfiles_names[f->candidates], name, length + 1u);
                s_fontfiles_sizes[f->candidates] = (uint32_t)size;
                f->candidates++;
            }
        }
        const int close_rc = sceIoDclose(dir);
        /* font-dir-close: result = sceIoDclose result, out0 entries, out1 the sceIoDread result
         * that ended the listing (0xffffffff when FONTFILES_ENTRIES_MAX stopped it). */
        used = snprintf(s_fontfiles_line, FONTFILES_LINE_MAX,
            "NAKAGAWA_PSP_TEST schema=1 test_id=" FONTFILES_TEST_ID " case_id=font-dir-close "
            "status=%s result=0x%08x out0=0x%08x out1=0x%08x",
            close_rc == 0 && end == 0 ? "PASS" : "FAIL", (unsigned int)close_rc,
            (unsigned int)f->entries, (unsigned int)end);
        fontfiles_emit(f, used);
        for (uint32_t i = 0; i < f->candidates; i++) fontfiles_copy(f, i);
    } else if (f->known_count > 0u && f->known_names != NULL) {
        fontfiles_known(f);
        if (f->candidates > 0u) {
            fontfiles_mkdir(f);
            for (uint32_t i = 0; i < f->candidates; i++) fontfiles_copy(f, i);
        }
    }
}

static void fontfiles_run(FontFiles *f) {
    int used;
    if (fontfiles_dest_fits(f)) {
        fontfiles_list_and_copy(f);
    } else {
        /* font-dest: result = project code 0x80000107, the destination is unusable. */
        used = snprintf(s_fontfiles_line, FONTFILES_LINE_MAX,
            "NAKAGAWA_PSP_TEST schema=1 test_id=" FONTFILES_TEST_ID " case_id=font-dest "
            "status=FAIL result=0x80000107");
        fontfiles_emit(f, used);
    }
    /* font-files-done: out0 entries, out1 copy candidates, out2 verified copies, out3 records
     * before this one. */
    used = snprintf(s_fontfiles_line, FONTFILES_LINE_MAX,
        "NAKAGAWA_PSP_TEST schema=1 test_id=" FONTFILES_TEST_ID " case_id=font-files-done "
        "status=PASS result=0x00000000 out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x",
        (unsigned int)f->entries, (unsigned int)f->candidates, (unsigned int)f->copied,
        (unsigned int)f->records);
    fontfiles_emit(f, used);
}

#endif /* NAKAGAWA_FONT_FILES_H */
