/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The store-only ZIP reader and writer behind save backups (#1883,
 * src/core/nk_zip.c): a round trip keeps every name and byte, and every
 * hostile or damaged archive is refused with its own status before a byte is
 * written. The hostile archives are built here byte by byte, since the writer
 * refuses to make them. */

#include "nk_platform.h"
#include "nk_zip.h"
#include "native_test_isolation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));     \
        }                                                                        \
    } while (0)

/* ---- A raw archive builder: any name, any field ---- */

typedef struct {
    const char *name;
    size_t name_size;          /* 0: strlen(name) */
    const char *data;
    uint32_t size;
    const uint8_t *central_extra;
    uint16_t central_extra_size;
    const uint8_t *local_extra;
    uint16_t local_extra_size;
} TItem;

#define TZIP_CAP 16384u
#define TZIP_ITEMS 8u

typedef struct {
    uint8_t buf[TZIP_CAP];
    size_t size;
    size_t local[TZIP_ITEMS];
    size_t data[TZIP_ITEMS];
    size_t central[TZIP_ITEMS];
    size_t central_offset;
    size_t end;
} TZip;

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void tzip_bytes(TZip *z, const void *data, size_t size) {
    if (z->size + size > TZIP_CAP) {
        fprintf(stderr, "test archive too large\n");
        exit(1);
    }
    if (size) memcpy(z->buf + z->size, data, size);
    z->size += size;
}

/* A valid store-only archive of `items`, with an optional comment and, when
   asked, a ZIP64 end locator between the central directory and the end. */
static void tzip_build_ex(TZip *z, const TItem *items, size_t n, const char *comment,
                          bool zip64_locator) {
    uint8_t h[46];
    memset(z, 0, sizeof(*z));
    for (size_t i = 0; i < n; ++i) {
        size_t name_size = items[i].name_size ? items[i].name_size : strlen(items[i].name);
        uint32_t crc = nk_zip_crc32(0u, items[i].data, items[i].size);
        z->local[i] = z->size;
        memset(h, 0, sizeof(h));
        put32(h, 0x04034b50u);
        put16(h + 4, 10u);
        put32(h + 14, crc);
        put32(h + 18, items[i].size);
        put32(h + 22, items[i].size);
        put16(h + 26, (uint16_t)name_size);
        put16(h + 28, items[i].local_extra_size);
        tzip_bytes(z, h, 30);
        tzip_bytes(z, items[i].name, name_size);
        tzip_bytes(z, items[i].local_extra, items[i].local_extra_size);
        z->data[i] = z->size;
        tzip_bytes(z, items[i].data, items[i].size);
    }
    z->central_offset = z->size;
    for (size_t i = 0; i < n; ++i) {
        size_t name_size = items[i].name_size ? items[i].name_size : strlen(items[i].name);
        uint32_t crc = nk_zip_crc32(0u, items[i].data, items[i].size);
        z->central[i] = z->size;
        memset(h, 0, sizeof(h));
        put32(h, 0x02014b50u);
        put16(h + 4, 20u);
        put16(h + 6, 10u);
        put32(h + 16, crc);
        put32(h + 20, items[i].size);
        put32(h + 24, items[i].size);
        put16(h + 28, (uint16_t)name_size);
        put16(h + 30, items[i].central_extra_size);
        put32(h + 42, (uint32_t)z->local[i]);
        tzip_bytes(z, h, 46);
        tzip_bytes(z, items[i].name, name_size);
        tzip_bytes(z, items[i].central_extra, items[i].central_extra_size);
    }
    {
        size_t central_size = z->size - z->central_offset;
        size_t comment_size = comment ? strlen(comment) : 0u;
        if (zip64_locator) {
            uint8_t locator[20];
            memset(locator, 0, sizeof(locator));
            put32(locator, 0x07064b50u);
            tzip_bytes(z, locator, sizeof(locator));
        }
        z->end = z->size;
        memset(h, 0, sizeof(h));
        put32(h, 0x06054b50u);
        put16(h + 8, (uint16_t)n);
        put16(h + 10, (uint16_t)n);
        put32(h + 12, (uint32_t)central_size);
        put32(h + 16, (uint32_t)z->central_offset);
        put16(h + 20, (uint16_t)comment_size);
        tzip_bytes(z, h, 22);
        tzip_bytes(z, comment, comment_size);
    }
}

static void tzip_build(TZip *z, const TItem *items, size_t n) {
    tzip_build_ex(z, items, n, NULL, false);
}

static NkZipStatus tzip_open(const TZip *z, NkZipError *err) {
    NkZipReader *reader = NULL;
    NkZipStatus status = nk_zip_reader_open_memory(&reader, z->buf, z->size, err);
    if (status == NK_ZIP_OK) {
        status = nk_zip_reader_verify(reader, err);
        nk_zip_reader_close(reader);
    } else if (reader != NULL) {
        status = NK_ZIP_STATUS_COUNT; /* a failed open must not hand out a reader */
    }
    return status;
}

static void expect_status(const TZip *z, NkZipStatus want, const char *what) {
    NkZipError err;
    NkZipStatus got = tzip_open(z, &err);
    char line[256];
    snprintf(line, sizeof(line), "%s: want %s, got %s (%s)", what, nk_zip_status_code(want),
             nk_zip_status_code(got), err.entry);
    CHECK(got == want && err.status == want, line);
}

static const TItem k_save = {"PSP/SAVEDATA/TEST00001DATA/DATA.BIN", 0, "save bytes", 10, NULL, 0,
                             NULL, 0};

static void expect_name(const char *name, size_t size, NkZipStatus want, const char *what) {
    TItem item = k_save;
    TZip z;
    item.name = name;
    item.name_size = size;
    tzip_build(&z, &item, 1);
    expect_status(&z, want, what);
    CHECK(nk_zip_check_name(name, size ? size : strlen(name), NULL) == want, what);
}

/* ---- Host files ---- */

static void path_join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s%c%s", a, nk_platform_path_separator(), b);
    if (n <= 0 || (size_t)n >= cap) {
        fprintf(stderr, "path too long\n");
        exit(1);
    }
    for (char *p = out + strlen(a) + 1; *p; ++p) {
        if (*p == '/') *p = nk_platform_path_separator();
    }
}

static void write_file(const char *path, const void *data, size_t size) {
    char parent[1024];
    char *cut;
    FILE *f;
    snprintf(parent, sizeof(parent), "%s", path);
    cut = strrchr(parent, nk_platform_path_separator());
    if (cut) {
        *cut = '\0';
        (void)nk_platform_mkdir_p(parent);
    }
    f = nk_fopen_utf8(path, "wb");
    if (!f || (size && fwrite(data, 1, size, f) != size) || fclose(f) != 0) {
        fprintf(stderr, "could not write %s\n", path);
        exit(1);
    }
}

static size_t read_file(const char *path, uint8_t *out, size_t cap) {
    FILE *f = nk_fopen_utf8(path, "rb");
    size_t n;
    if (!f) return (size_t)-1;
    n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

static bool file_equals(const char *path, const void *data, size_t size) {
    static uint8_t buf[1u << 20];
    size_t n = read_file(path, buf, sizeof(buf));
    return n == size && (size == 0 || memcmp(buf, data, size) == 0);
}

static bool folder_is_empty_cb(const char *name, bool is_dir, void *ctx) {
    (void)name;
    (void)is_dir;
    *(bool *)ctx = false;
    return false;
}

static bool folder_is_empty(const char *path) {
    bool empty = true;
    return nk_platform_list_dir(path, folder_is_empty_cb, &empty) && empty;
}

/* ---- Tests ---- */

static void test_round_trip(const char *root) {
    static uint8_t big[200000];
    char tree[1024], zip[1024], out[1024], path[1024];
    NkZipReader *reader = NULL;
    NkZipError err;
    size_t entries = 0;
    uint64_t bytes = 0;
    const char *names[] = {"PSP/SAVEDATA/TEST00001DATA/PARAM.SFO",
                           "PSP/SAVEDATA/TEST00001DATA/DATA.BIN",
                           "PSP/SAVEDATA/TEST00001DATA/EMPTY.BIN",
                           "PSP/GAME/readme \xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96.txt"};
    for (size_t i = 0; i < sizeof(big); ++i) big[i] = (uint8_t)(i * 131u + 7u);
    path_join(tree, sizeof(tree), root, "tree");
    path_join(zip, sizeof(zip), root, "tree.zip");
    path_join(out, sizeof(out), root, "out");
    path_join(path, sizeof(path), tree, names[0]);
    write_file(path, "PSF\x01", 4);
    path_join(path, sizeof(path), tree, names[1]);
    write_file(path, big, sizeof(big));
    path_join(path, sizeof(path), tree, names[2]);
    write_file(path, "", 0);
    path_join(path, sizeof(path), tree, names[3]);
    write_file(path, "hello", 5);
    path_join(path, sizeof(path), tree, "PSP/SYSTEM/EMPTYDIR");
    CHECK(nk_platform_mkdir_p(path), "an empty folder for the round trip");

    CHECK(nk_zip_write_folder(tree, zip, 0x5964u, 0x7A00u, &entries, &bytes, &err) == NK_ZIP_OK,
          "a save tree is written to a ZIP");
    /* PSP, PSP/GAME, PSP/SAVEDATA, the save folder, PSP/SYSTEM, its empty
       folder, and four files. */
    CHECK(entries == 10u && bytes == 4u + sizeof(big) + 0u + 5u, "every file and folder went in");
    CHECK(nk_zip_reader_open_file(&reader, zip, &err) == NK_ZIP_OK && reader != NULL,
          "the written ZIP opens");
    if (reader) {
        size_t files = 0;
        for (size_t i = 0; i < nk_zip_reader_count(reader); ++i) {
            NkZipEntryInfo info;
            CHECK(nk_zip_reader_entry(reader, i, &info), "each entry is listed");
            CHECK(strncmp(info.name, "PSP/", 4) == 0, "every name is relative to the tree");
            CHECK(strchr(info.name, '\\') == NULL, "names use forward slashes");
            if (!info.is_folder) ++files;
        }
        CHECK(files == 4u, "four files are listed");
        CHECK(nk_zip_reader_verify(reader, &err) == NK_ZIP_OK, "every CRC matches");
        CHECK(nk_platform_mkdir_p(out), "an output folder");
        CHECK(nk_zip_reader_extract(reader, out, &err) == NK_ZIP_OK, "the ZIP extracts");
        nk_zip_reader_close(reader);
        reader = NULL;
    }
    path_join(path, sizeof(path), out, names[0]);
    CHECK(file_equals(path, "PSF\x01", 4), "PARAM.SFO comes back byte for byte");
    path_join(path, sizeof(path), out, names[1]);
    CHECK(file_equals(path, big, sizeof(big)), "a 200 KB file comes back byte for byte");
    path_join(path, sizeof(path), out, names[2]);
    CHECK(file_equals(path, "", 0), "an empty file comes back empty");
    path_join(path, sizeof(path), out, names[3]);
    CHECK(file_equals(path, "hello", 5), "a UTF-8 name comes back with its bytes");
    path_join(path, sizeof(path), out, "PSP/SYSTEM/EMPTYDIR");
    CHECK(nk_platform_dir_exists(path) && folder_is_empty(path), "an empty folder comes back");

    /* The same tree makes the same bytes: names in order, one fixed time. */
    {
        static uint8_t first[300000], second[300000];
        char again[1024];
        size_t a, b;
        path_join(again, sizeof(again), root, "again.zip");
        CHECK(nk_zip_write_folder(tree, again, 0u, 0u, NULL, NULL, &err) == NK_ZIP_OK &&
                  nk_zip_write_folder(tree, zip, 0u, 0u, NULL, NULL, &err) == NK_ZIP_OK,
              "the tree is written twice");
        a = read_file(zip, first, sizeof(first));
        b = read_file(again, second, sizeof(second));
        CHECK(a == b && a != (size_t)-1 && memcmp(first, second, a) == 0,
              "two backups of one tree are the same bytes");
    }

    /* Extraction never overwrites: the second run meets the first's files. */
    CHECK(nk_zip_reader_open_file(&reader, zip, &err) == NK_ZIP_OK, "the ZIP opens again");
    if (reader) {
        CHECK(nk_zip_reader_extract(reader, out, &err) == NK_ZIP_ERR_EXISTS,
              "a file already in the folder is refused, not overwritten");
        nk_zip_reader_close(reader);
    }
    printf("PASS: nk_zip round trip keeps every name, byte and folder\n");
}

static void test_writer_refusals(const char *root) {
    char zip[1024];
    NkZipWriter *w = NULL;
    NkZipError err;
    path_join(zip, sizeof(zip), root, "dup.zip");
    CHECK(nk_zip_writer_open(&w, zip, 0u, 0u, &err) == NK_ZIP_OK, "writer opens");
    CHECK(nk_zip_writer_add_bytes(w, "SAVE/DATA.BIN", "a", 1, &err) == NK_ZIP_OK, "first name");
    CHECK(nk_zip_writer_add_bytes(w, "save/data.bin", "b", 1, &err) == NK_ZIP_OK,
          "a case duplicate is only found at the end");
    CHECK(nk_zip_writer_finish(w, &err) == NK_ZIP_ERR_NAME_DUPLICATE,
          "the writer refuses names Windows would merge");
    CHECK(!nk_platform_file_exists(zip), "a refused archive leaves no file");

    CHECK(nk_zip_writer_open(&w, zip, 0u, 0u, &err) == NK_ZIP_OK, "writer opens again");
    CHECK(nk_zip_writer_add_bytes(w, "../escape", "x", 1, &err) == NK_ZIP_ERR_NAME_PARENT,
          "the writer refuses a name that climbs out");
    CHECK(nk_zip_writer_add_bytes(w, "fine", "x", 1, &err) == NK_ZIP_ERR_NAME_PARENT,
          "after a failure every call returns it");
    CHECK(nk_zip_writer_finish(w, &err) == NK_ZIP_ERR_NAME_PARENT && !nk_platform_file_exists(zip),
          "finish reports the first failure and deletes the partial file");

    CHECK(nk_zip_writer_open(&w, zip, 0u, 0u, &err) == NK_ZIP_OK, "writer opens a third time");
    {
        NkZipStatus status = NK_ZIP_OK;
        char name[32];
        for (unsigned i = 0; i <= NK_ZIP_MAX_ENTRIES && status == NK_ZIP_OK; ++i) {
            snprintf(name, sizeof(name), "F%05u", i);
            status = nk_zip_writer_add_bytes(w, name, NULL, 0, &err);
        }
        CHECK(status == NK_ZIP_ERR_TOO_MANY_ENTRIES, "the writer stops at 4096 entries");
    }
    nk_zip_writer_discard(w);
    CHECK(!nk_platform_file_exists(zip), "discard deletes the partial file");
    printf("PASS: nk_zip writer refuses what the reader would refuse\n");
}

static void test_name_refusals(void) {
    char deep[600];
    char longest[NK_ZIP_MAX_NAME_BYTES + 8];
    size_t at = 0;
    expect_name("/etc/passwd", 0, NK_ZIP_ERR_NAME_ABSOLUTE, "absolute name");
    expect_name("PSP/../../escape.txt", 0, NK_ZIP_ERR_NAME_PARENT, "a .. folder");
    expect_name("../escape.txt", 0, NK_ZIP_ERR_NAME_PARENT, "a leading .. folder");
    expect_name("PSP/SAVEDATA/..", 0, NK_ZIP_ERR_NAME_PARENT, "a trailing .. folder");
    expect_name("C:/Windows/evil.dll", 0, NK_ZIP_ERR_NAME_DRIVE, "a drive letter");
    expect_name("c:evil", 0, NK_ZIP_ERR_NAME_DRIVE, "a drive-relative name");
    expect_name("PSP\\SAVEDATA\\x", 0, NK_ZIP_ERR_NAME_BACKSLASH, "backslashes");
    expect_name("..\\escape", 0, NK_ZIP_ERR_NAME_BACKSLASH, "a backslash .. name");
    expect_name("PSP//x", 0, NK_ZIP_ERR_NAME_EMPTY_SEGMENT, "an empty folder name");
    expect_name("PSP/./x", 0, NK_ZIP_ERR_NAME_DOT, "a . folder");
    expect_name("PSP/a\0b", 7, NK_ZIP_ERR_NAME_NUL, "a NUL byte");
    expect_name("PSP/CON", 0, NK_ZIP_ERR_NAME_RESERVED, "a device name");
    expect_name("PSP/nul.txt", 0, NK_ZIP_ERR_NAME_RESERVED, "a device name with an extension");
    expect_name("PSP/COM1", 0, NK_ZIP_ERR_NAME_RESERVED, "COM1");
    expect_name("PSP/COM\xC2\xB9", 0, NK_ZIP_ERR_NAME_RESERVED, "COM with a superscript 1");
    expect_name("PSP/lpt\xC2\xB3.txt", 0, NK_ZIP_ERR_NAME_RESERVED,
                "LPT with a superscript 3 and an extension");
    expect_name("PSP/COM\xC2\xB4", 0, NK_ZIP_OK, "COM and an acute accent is a name");
    expect_name("PSP/a<b", 0, NK_ZIP_ERR_NAME_CHARACTER, "a < in a name");
    expect_name("PSP/file:stream", 0, NK_ZIP_ERR_NAME_CHARACTER, "an alternate data stream");
    expect_name("PSP/trailing.", 0, NK_ZIP_ERR_NAME_CHARACTER, "a trailing dot");
    expect_name("PSP/trailing ", 0, NK_ZIP_ERR_NAME_CHARACTER, "a trailing space");
    expect_name("PSP/\x01", 0, NK_ZIP_ERR_NAME_CHARACTER, "a control character");
    expect_name("PSP/\xFF\xFE", 0, NK_ZIP_ERR_NAME_ENCODING, "not UTF-8");
    expect_name("PSP/\xC0\xAF", 0, NK_ZIP_ERR_NAME_ENCODING, "an overlong '/'");
    expect_name("PSP/\xED\xA0\x80", 0, NK_ZIP_ERR_NAME_ENCODING, "a UTF-16 surrogate");
    for (unsigned i = 0; i < 33u; ++i) {
        deep[at++] = 'd';
        deep[at++] = '/';
    }
    deep[at++] = 'f';
    deep[at] = '\0';
    expect_name(deep, 0, NK_ZIP_ERR_NAME_TOO_DEEP, "34 levels");
    memset(longest, 'a', sizeof(longest));
    longest[NK_ZIP_MAX_NAME_BYTES + 1] = '\0';
    expect_name(longest, 0, NK_ZIP_ERR_NAME_TOO_LONG, "a name past the cap");
    {
        TItem item = k_save;
        TZip z;
        item.name = "";
        item.name_size = 0;
        /* name_size 0 means strlen: an empty name. */
        tzip_build(&z, &item, 1);
        expect_status(&z, NK_ZIP_ERR_NAME_EMPTY, "an empty name");
    }
    CHECK(nk_zip_check_name("PSP/SAVEDATA/ULUS12345SAVE00/PARAM.SFO", 38, NULL) == NK_ZIP_OK,
          "a PSP save name is fine");
    CHECK(nk_zip_check_name("CONFIG/console.txt", 18, NULL) == NK_ZIP_OK,
          "CONFIG and console are not device names");
    printf("PASS: nk_zip refuses every unsafe name with its own code\n");
}

static void test_duplicate_refusals(void) {
    TItem items[3] = {k_save, k_save, k_save};
    TZip z;
    tzip_build(&z, items, 2);
    expect_status(&z, NK_ZIP_ERR_NAME_DUPLICATE, "the same name twice");
    items[1].name = "psp/savedata/test00001data/data.bin";
    tzip_build(&z, items, 2);
    expect_status(&z, NK_ZIP_ERR_NAME_DUPLICATE, "the same name in other case");
    items[0].name = "PSP/SAVEDATA/X";
    items[1].name = "psp/savedata/x/DATA.BIN";
    tzip_build(&z, items, 2);
    expect_status(&z, NK_ZIP_ERR_NAME_CONFLICT, "a file named like a folder");
    items[0].name = "PSP/SAVEDATA/X/";
    items[0].size = 0;
    items[0].data = "";
    items[1].name = "PSP/SAVEDATA/X";
    tzip_build(&z, items, 2);
    expect_status(&z, NK_ZIP_ERR_NAME_DUPLICATE, "a folder entry and a file of one name");
    items[1].name = "PSP/SAVEDATA/X/DATA.BIN";
    items[2].name = "PSP/SAVEDATA/X-1/DATA.BIN";
    tzip_build(&z, items, 3);
    expect_status(&z, NK_ZIP_OK, "a folder, its file and a similar name are fine");
    printf("PASS: nk_zip refuses duplicate and case-duplicate names\n");
}

static void test_structure_refusals(const char *root) {
    TItem items[2] = {k_save, k_save};
    TZip z;
    items[1].name = "PSP/SAVEDATA/TEST00001DATA/PARAM.SFO";
    items[1].data = "PSF parameters";
    items[1].size = 14;

    tzip_build(&z, items, 2);
    expect_status(&z, NK_ZIP_OK, "the unmodified two-file archive");

    /* Method 8 (deflate), set as a real deflate entry would, in both headers. */
    tzip_build(&z, items, 1);
    put16(z.buf + z.local[0] + 8, 8u);
    put16(z.buf + z.central[0] + 10, 8u);
    expect_status(&z, NK_ZIP_ERR_METHOD, "a compressed entry");

    tzip_build(&z, items, 1);
    put16(z.buf + z.local[0] + 6, 1u);
    put16(z.buf + z.central[0] + 8, 1u);
    expect_status(&z, NK_ZIP_ERR_ENCRYPTED, "an encrypted entry");

    tzip_build(&z, items, 1);
    put16(z.buf + z.local[0] + 6, 8u);
    put16(z.buf + z.central[0] + 8, 8u);
    expect_status(&z, NK_ZIP_ERR_DATA_DESCRIPTOR, "a data descriptor");

    tzip_build(&z, items, 1);
    put16(z.buf + z.local[0] + 6, 2u);
    put16(z.buf + z.central[0] + 8, 2u);
    expect_status(&z, NK_ZIP_ERR_FLAGS, "an unknown flag");

    tzip_build(&z, items, 1);
    put16(z.buf + z.local[0] + 6, 0x0800u);
    put16(z.buf + z.central[0] + 8, 0x0800u);
    expect_status(&z, NK_ZIP_OK, "the UTF-8 name flag is fine");

    /* ZIP64: each marker, the extra field, and the end locator. */
    tzip_build(&z, items, 1);
    put16(z.buf + z.end + 10, 0xFFFFu);
    put16(z.buf + z.end + 8, 0xFFFFu);
    expect_status(&z, NK_ZIP_ERR_ZIP64, "a ZIP64 entry-count marker");
    tzip_build(&z, items, 1);
    put32(z.buf + z.end + 16, 0xFFFFFFFFu);
    expect_status(&z, NK_ZIP_ERR_ZIP64, "a ZIP64 offset marker");
    tzip_build(&z, items, 1);
    put32(z.buf + z.central[0] + 20, 0xFFFFFFFFu);
    put32(z.buf + z.central[0] + 24, 0xFFFFFFFFu);
    expect_status(&z, NK_ZIP_ERR_ZIP64, "a ZIP64 size marker in an entry");
    {
        static const uint8_t extra[] = {0x01, 0x00, 0x08, 0x00, 1, 0, 0, 0, 0, 0, 0, 0};
        TItem z64 = items[0];
        z64.central_extra = extra;
        z64.central_extra_size = sizeof(extra);
        tzip_build(&z, &z64, 1);
        expect_status(&z, NK_ZIP_ERR_ZIP64, "a ZIP64 extra field in the central entry");
        z64.central_extra = NULL;
        z64.central_extra_size = 0;
        z64.local_extra = extra;
        z64.local_extra_size = sizeof(extra);
        tzip_build(&z, &z64, 1);
        expect_status(&z, NK_ZIP_ERR_ZIP64, "a ZIP64 extra field in the local header");
    }
    tzip_build_ex(&z, items, 1, NULL, true);
    expect_status(&z, NK_ZIP_ERR_ZIP64, "a ZIP64 end locator");
    {
        /* Another extra field (an extended timestamp) is read past. */
        static const uint8_t stamp[] = {0x55, 0x54, 0x05, 0x00, 1, 0x10, 0x20, 0x30, 0x40};
        TItem timed = items[0];
        timed.central_extra = stamp;
        timed.central_extra_size = sizeof(stamp);
        timed.local_extra = stamp;
        timed.local_extra_size = sizeof(stamp);
        tzip_build(&z, &timed, 1);
        expect_status(&z, NK_ZIP_OK, "an extended-timestamp extra field is fine");
    }

    tzip_build(&z, items, 1);
    put16(z.buf + z.end + 4, 1u);
    expect_status(&z, NK_ZIP_ERR_MULTI_DISK, "a second disk");
    tzip_build(&z, items, 1);
    put16(z.buf + z.central[0] + 34, 1u);
    expect_status(&z, NK_ZIP_ERR_MULTI_DISK, "an entry on another disk");

    /* Local header against central entry: name, CRC, size, flags. */
    tzip_build(&z, items, 1);
    z.buf[z.local[0] + 30 + 4] = 'X';
    expect_status(&z, NK_ZIP_ERR_LOCAL_MISMATCH, "a local name that differs");
    tzip_build(&z, items, 1);
    put32(z.buf + z.local[0] + 14, 0x12345678u);
    expect_status(&z, NK_ZIP_ERR_LOCAL_MISMATCH, "a local CRC that differs");
    tzip_build(&z, items, 1);
    put32(z.buf + z.local[0] + 18, 9u);
    put32(z.buf + z.local[0] + 22, 9u);
    expect_status(&z, NK_ZIP_ERR_LOCAL_MISMATCH, "local sizes that differ");
    tzip_build(&z, items, 1);
    put16(z.buf + z.local[0] + 6, 0x0800u);
    expect_status(&z, NK_ZIP_ERR_LOCAL_MISMATCH, "local flags that differ");
    tzip_build(&z, items, 1);
    put32(z.buf + z.local[0], 0x08074b50u);
    expect_status(&z, NK_ZIP_ERR_LOCAL_MISMATCH, "no local header where the entry points");

    /* Sizes. */
    tzip_build(&z, items, 1);
    put32(z.buf + z.central[0] + 20, 9u);
    expect_status(&z, NK_ZIP_ERR_SIZE_MISMATCH, "stored sizes that differ");
    {
        TItem folder = items[0];
        folder.name = "PSP/SAVEDATA/";
        tzip_build(&z, &folder, 1);
        expect_status(&z, NK_ZIP_ERR_SIZE_MISMATCH, "a folder holding data");
    }

    /* CRC: the archive opens, and verify and extract refuse it with nothing written. */
    tzip_build(&z, items, 2);
    z.buf[z.data[1] + 3] ^= 0x20u;
    expect_status(&z, NK_ZIP_ERR_CRC, "a damaged byte");
    {
        char out[1024], zip[1024];
        NkZipReader *reader = NULL;
        NkZipError err;
        FILE *f;
        path_join(out, sizeof(out), root, "crc-out");
        path_join(zip, sizeof(zip), root, "crc.zip");
        f = nk_fopen_utf8(zip, "wb");
        CHECK(f && fwrite(z.buf, 1, z.size, f) == z.size && fclose(f) == 0,
              "the damaged archive is on disk");
        CHECK(nk_platform_mkdir_p(out), "an output folder");
        CHECK(nk_zip_reader_open_file(&reader, zip, &err) == NK_ZIP_OK,
              "a damaged byte is not found until the data is read");
        if (reader) {
            CHECK(nk_zip_reader_extract(reader, out, &err) == NK_ZIP_ERR_CRC &&
                      strcmp(err.entry, "PSP/SAVEDATA/TEST00001DATA/PARAM.SFO") == 0,
                  "extract names the damaged file");
            nk_zip_reader_close(reader);
        }
        CHECK(folder_is_empty(out), "nothing was written before the CRC check");
    }

    /* Truncated, and the end record. */
    tzip_build(&z, items, 2);
    z.size -= 1;
    expect_status(&z, NK_ZIP_ERR_END_RECORD, "one byte cut from the end");
    tzip_build(&z, items, 2);
    z.size = z.central_offset + 10;
    expect_status(&z, NK_ZIP_ERR_NOT_ZIP, "cut inside the central directory");
    tzip_build(&z, items, 2);
    z.size = 21;
    expect_status(&z, NK_ZIP_ERR_NOT_ZIP, "shorter than an end record");
    tzip_build_ex(&z, items, 2, "a comment for the archive", false);
    expect_status(&z, NK_ZIP_OK, "an archive comment is fine");
    {
        /* A comment that holds a second, matching end record. */
        char fake[23];
        memset(fake, 'c', sizeof(fake));
        put32((uint8_t *)fake + 0, 0x06054b50u);
        fake[20] = 0;
        fake[21] = 0;
        fake[22] = '\0';
        tzip_build_ex(&z, items, 2, NULL, false);
        put16(z.buf + z.end + 20, 22u);
        memcpy(z.buf + z.size, fake, 22);
        z.size += 22;
        expect_status(&z, NK_ZIP_ERR_END_RECORD, "two end records");
    }

    /* The central directory where the end record says. */
    tzip_build(&z, items, 2);
    put32(z.buf + z.end + 16, (uint32_t)z.central_offset - 1u);
    expect_status(&z, NK_ZIP_ERR_CENTRAL_DIRECTORY, "a central directory off by one");
    tzip_build(&z, items, 2);
    put16(z.buf + z.end + 8, 3u);
    put16(z.buf + z.end + 10, 3u);
    expect_status(&z, NK_ZIP_ERR_CENTRAL_DIRECTORY, "more entries than the directory holds");
    tzip_build(&z, items, 2);
    put32(z.buf + z.central[1], 0x01020304u);
    expect_status(&z, NK_ZIP_ERR_CENTRAL_DIRECTORY, "a damaged central signature");

    /* Offsets past the end of their area. */
    tzip_build(&z, items, 2);
    put32(z.buf + z.central[1] + 42, 0x7FFFFFF0u);
    expect_status(&z, NK_ZIP_ERR_OFFSET, "a local header past the end");
    tzip_build(&z, items, 2);
    put32(z.buf + z.central[1] + 42, (uint32_t)z.central_offset - 20u);
    expect_status(&z, NK_ZIP_ERR_OFFSET, "a local header running into the directory");
    tzip_build(&z, items, 2);
    put32(z.buf + z.central[1] + 20, 0x01000000u);
    put32(z.buf + z.central[1] + 24, 0x01000000u);
    put32(z.buf + z.local[1] + 18, 0x01000000u);
    put32(z.buf + z.local[1] + 22, 0x01000000u);
    expect_status(&z, NK_ZIP_ERR_OFFSET, "data past the end of the archive");

    /* Overlap: the first entry's data runs over the second's header. */
    tzip_build(&z, items, 2);
    put32(z.buf + z.central[0] + 20, items[0].size + 12u);
    put32(z.buf + z.central[0] + 24, items[0].size + 12u);
    put32(z.buf + z.local[0] + 18, items[0].size + 12u);
    put32(z.buf + z.local[0] + 22, items[0].size + 12u);
    expect_status(&z, NK_ZIP_ERR_OVERLAP, "two entries sharing bytes");
    tzip_build(&z, items, 2);
    put32(z.buf + z.central[1] + 42, (uint32_t)z.local[0]);
    expect_status(&z, NK_ZIP_ERR_LOCAL_MISMATCH, "two entries pointing at one header");

    /* The caps. */
    tzip_build(&z, items, 2);
    put16(z.buf + z.end + 8, (uint16_t)(NK_ZIP_MAX_ENTRIES + 1u));
    put16(z.buf + z.end + 10, (uint16_t)(NK_ZIP_MAX_ENTRIES + 1u));
    expect_status(&z, NK_ZIP_ERR_TOO_MANY_ENTRIES, "4097 entries");
    tzip_build(&z, items, 2);
    put32(z.buf + z.central[0] + 20, 0x10000000u);
    put32(z.buf + z.central[0] + 24, 0x10000000u);
    put32(z.buf + z.central[1] + 20, 0x10000001u);
    put32(z.buf + z.central[1] + 24, 0x10000001u);
    expect_status(&z, NK_ZIP_ERR_TOO_LARGE, "files adding up past 512 MiB");

    /* Not a ZIP at all. */
    memset(&z, 0, sizeof(z));
    memset(z.buf, 'x', 4096);
    z.size = 4096;
    expect_status(&z, NK_ZIP_ERR_NOT_ZIP, "4 KB of text");
    {
        NkZipReader *reader = (NkZipReader *)1;
        CHECK(nk_zip_reader_open_memory(&reader, NULL, 0, NULL) == NK_ZIP_ERR_NOT_ZIP &&
                  reader == NULL,
              "an empty buffer is refused and no reader is handed out");
    }
    printf("PASS: nk_zip refuses every damaged or unsupported archive with its own code\n");
}

/* nk_zip_folder_state: only a folder that lists whole with nothing in it is empty. */
static void test_folder_state(const char *root) {
    char folder[1200];
    char file[1300];
    snprintf(folder, sizeof(folder), "%s%cstate", root, nk_platform_path_separator());
    CHECK(nk_zip_folder_state(folder) == NK_ZIP_FOLDER_UNREADABLE, "a missing folder is unreadable");
    CHECK(nk_platform_mkdir_p(folder), "make the folder");
    CHECK(nk_zip_folder_state(folder) == NK_ZIP_FOLDER_EMPTY, "an empty folder is empty");
    snprintf(file, sizeof(file), "%s%cone.bin", folder, nk_platform_path_separator());
    {
        FILE *f = nk_fopen_utf8(file, "wb");
        CHECK(f != NULL, "make a file");
        if (f) fclose(f);
    }
    CHECK(nk_zip_folder_state(folder) == NK_ZIP_FOLDER_HAS_ENTRIES, "a file is an entry");
    printf("PASS: nk_zip folder state: missing, empty, with an entry\n");
}

static void test_status_words(void) {
    for (int i = 0; i < (int)NK_ZIP_STATUS_COUNT; ++i) {
        const char *code = nk_zip_status_code((NkZipStatus)i);
        const char *message = nk_zip_status_message((NkZipStatus)i);
        CHECK(strncmp(code, "ZIP_", 4) == 0 && message[0] != '\0', "every status has words");
        for (int j = 0; j < i; ++j) {
            CHECK(strcmp(code, nk_zip_status_code((NkZipStatus)j)) != 0, "codes are distinct");
            CHECK(strcmp(message, nk_zip_status_message((NkZipStatus)j)) != 0,
                  "messages are distinct");
        }
    }
    CHECK(strcmp(nk_zip_status_code(NK_ZIP_STATUS_COUNT), "ZIP_UNKNOWN") == 0,
          "an unknown status has a code");
    CHECK(nk_zip_crc32(0u, "123456789", 9) == 0xCBF43926u, "the CRC-32 check value");
    printf("PASS: nk_zip status codes and messages are distinct; CRC-32 check value\n");
}

int main(void) {
    char root[1024];
    native_test_create_root("nk_zip");
    snprintf(root, sizeof(root), "%s", native_test_get_root());
    test_status_words();
    test_round_trip(root);
    test_writer_refusals(root);
    test_name_refusals();
    test_duplicate_refusals();
    test_structure_refusals(root);
    test_folder_state(root);
    if (g_failures) {
        fprintf(stderr, "%d of %d nk_zip checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("ALL NK ZIP TESTS PASSED (%d checks)\n", g_checks);
    return 0;
}
