/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Disc verification (src/core/nk_disc_verify.c) and the library keyed by disc
 * ID plus content hash (src/core/nk_library.c), on synthetic data only: the
 * SHA-1 and CRC-32 known answers, the whole-image hash with progress and
 * cancel, the Redump datafile reader, the three tiers, the import into the
 * data folder, and library files: one written before hashing still loads, a
 * fan translation and its retail disc are two records, the modified one
 * labelled. The CSO/ZSO/CHD round trips in test_disc_expand.c check that an
 * expansion hashes the same as its .iso.
 */

#define _POSIX_C_SOURCE 200809L

#include "native_test_isolation.h"
#include "nk_disc_verify.h"
#include "nk_library.h"
#include "nk_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

static void path_in(char *out, size_t size, const char *name) {
    int n = snprintf(out, size, "%s%c%s", native_test_get_root(), nk_platform_path_separator(),
                     name);
    CHECK(n > 0 && (size_t)n < size);
}

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *f = nk_fopen_utf8(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(data, 1, size, f) == size);
    CHECK(fclose(f) == 0);
}

static char *read_text(const char *path) {
    FILE *f = nk_fopen_utf8(path, "rb");
    CHECK(f != NULL);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    CHECK(size >= 0 && fseek(f, 0, SEEK_SET) == 0);
    char *text = (char *)malloc((size_t)size + 1u);
    CHECK(text != NULL);
    CHECK(fread(text, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    text[size] = '\0';
    return text;
}

static void sha1_hex(const uint8_t *data, size_t len, char out[41]) {
    static const char hex[] = "0123456789abcdef";
    NkSha1 ctx;
    uint8_t digest[20];
    nk_sha1_init(&ctx);
    nk_sha1_update(&ctx, data, len);
    nk_sha1_finish(&ctx, digest);
    for (int i = 0; i < 20; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[40] = '\0';
}

/* ---- hashes ------------------------------------------------------------ */

static void test_sha1_and_crc32_known_answers(void) {
    char hex[41];
    /* FIPS 180-4 / RFC 3174 examples. */
    sha1_hex((const uint8_t *)"", 0, hex);
    CHECK(strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0);
    sha1_hex((const uint8_t *)"abc", 3, hex);
    CHECK(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1_hex((const uint8_t *)two_blocks, strlen(two_blocks), hex);
    CHECK(strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0);
    uint8_t *million = (uint8_t *)malloc(1000000u);
    CHECK(million != NULL);
    memset(million, 'a', 1000000u);
    /* Fed in uneven pieces, so every block-boundary case of update runs. */
    NkSha1 ctx;
    uint8_t digest[20];
    nk_sha1_init(&ctx);
    size_t done = 0, step = 1;
    while (done < 1000000u) {
        size_t take = step < 1000000u - done ? step : 1000000u - done;
        nk_sha1_update(&ctx, million + done, take);
        done += take;
        step = step * 7u % 131u + 1u;
    }
    nk_sha1_finish(&ctx, digest);
    static const uint8_t expected[20] = {
        0x34, 0xaa, 0x97, 0x3c, 0xd4, 0xc4, 0xda, 0xa4, 0xf6, 0x1e,
        0xeb, 0x2b, 0xdb, 0xad, 0x27, 0x31, 0x65, 0x34, 0x01, 0x6f,
    };
    CHECK(memcmp(digest, expected, 20) == 0);

    /* The CRC-32 check value, and the same CRC fed byte by byte and in pieces
       that start off the four-byte stride. */
    CHECK(nk_crc32_update(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(nk_crc32_update(0, (const uint8_t *)"", 0) == 0);
    uint32_t whole = nk_crc32_update(0, million, 100003u);
    uint32_t pieces = nk_crc32_update(0, million, 1);
    pieces = nk_crc32_update(pieces, million + 1, 6);
    pieces = nk_crc32_update(pieces, million + 7, 100003u - 7u);
    CHECK(whole == pieces);
    uint32_t bytes = 0;
    for (size_t i = 0; i < 100003u; i++) bytes = nk_crc32_update(bytes, million + i, 1);
    CHECK(whole == bytes);
    free(million);
    printf("PASS SHA-1 and CRC-32 known answers\n");
}

typedef struct {
    uint64_t last_done;
    uint64_t total;
    int calls;
    int cancel_after;
} Progress;

static void on_progress(uint64_t done, uint64_t total, void *user) {
    Progress *p = (Progress *)user;
    CHECK(done >= p->last_done);
    p->last_done = done;
    p->total = total;
    p->calls++;
}

static bool on_cancel(void *user) {
    Progress *p = (Progress *)user;
    return p->cancel_after >= 0 && p->calls >= p->cancel_after;
}

static void test_hash_file(void) {
    /* Larger than one read chunk, and not a whole number of chunks. */
    size_t size = 3u * 1024u * 1024u + 12345u;
    uint8_t *image = (uint8_t *)malloc(size);
    CHECK(image != NULL);
    for (size_t i = 0; i < size; i++) image[i] = (uint8_t)(i * 2654435761u >> 13);
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "hash.iso");
    write_bytes(path, image, size);

    Progress progress = {0, 0, 0, -1};
    NkDiscExpandCallbacks callbacks = {on_progress, on_cancel, &progress};
    NkDiscDigest digest;
    NkDiscExpandStatus status;
    CHECK(nk_disc_hash_file(path, &callbacks, &digest, &status));
    char hex[41];
    sha1_hex(image, size, hex);
    char crc[9];
    snprintf(crc, sizeof(crc), "%08x", (unsigned)nk_crc32_update(0, image, size));
    CHECK(strcmp(digest.sha1, hex) == 0);
    CHECK(strcmp(digest.crc32, crc) == 0);
    CHECK(digest.size_bytes == size);
    CHECK(progress.last_done == size && progress.total == size && progress.calls == 4);
    CHECK(status.code[0] == '\0');

    /* A cancel leaves no digest behind. */
    Progress stop = {0, 0, 0, 2};
    NkDiscExpandCallbacks cancel = {on_progress, on_cancel, &stop};
    CHECK(!nk_disc_hash_file(path, &cancel, &digest, &status));
    CHECK(strcmp(status.code, NK_DISC_CANCELLED) == 0);
    CHECK(digest.sha1[0] == '\0' && digest.size_bytes == 0);

    char missing[NATIVE_TEST_PATH_MAX];
    path_in(missing, sizeof(missing), "missing.iso");
    CHECK(!nk_disc_hash_file(missing, NULL, &digest, &status));
    CHECK(strcmp(status.code, NK_DISC_READ_FAILED) == 0);
    free(image);
    printf("PASS whole-image hash, progress and cancel\n");
}

/* ---- datafile ---------------------------------------------------------- */

/* Three synthetic images: the retail one the list names, a modified copy of
   it (one byte changed, as a patch would), and one the list does not know. */
#define IMAGE_BYTES 8192u
static uint8_t g_retail[IMAGE_BYTES];
static uint8_t g_modified[IMAGE_BYTES];
static uint8_t g_other[IMAGE_BYTES];
static NkDiscDigest g_retail_digest;
static NkDiscDigest g_modified_digest;
static NkDiscDigest g_other_digest;

static void digest_of(const uint8_t *data, size_t size, NkDiscDigest *out) {
    memset(out, 0, sizeof(*out));
    sha1_hex(data, size, out->sha1);
    snprintf(out->crc32, sizeof(out->crc32), "%08x", (unsigned)nk_crc32_update(0, data, size));
    out->size_bytes = size;
}

static void build_images(void) {
    for (size_t i = 0; i < IMAGE_BYTES; i++) {
        g_retail[i] = (uint8_t)(i * 31u + 7u);
        g_other[i] = (uint8_t)(i * 17u + 3u);
    }
    memcpy(g_modified, g_retail, IMAGE_BYTES);
    g_modified[4096] ^= 0x5Au;
    digest_of(g_retail, IMAGE_BYTES, &g_retail_digest);
    digest_of(g_modified, IMAGE_BYTES, &g_modified_digest);
    digest_of(g_other, IMAGE_BYTES, &g_other_digest);
}

/* A datafile in Redump's layout naming the retail image (with serials) and
   another disc, plus a rom with a malformed hash and a repeated SHA-1. */
static char *build_dat(bool with_serials) {
    char *dat = (char *)malloc(8192);
    CHECK(dat != NULL);
    snprintf(dat, 8192,
             "<?xml version=\"1.0\"?>\n"
             "<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM Management Datafile//EN\" "
             "\"http://www.logiqx.com/Dats/datafile.dtd\">\n"
             "<datafile>\n"
             "\t<header>\n\t\t<name>Synthetic &amp; Test</name>\n"
             "\t\t<version>2026-10-31</version>\n\t</header>\n"
             "\t<game name=\"Retail Disc (USA)\">\n"
             "\t\t<category>Games</category>\n%s"
             "\t\t<rom name=\"Retail Disc (USA).iso\" size=\"%u\" crc=\"%s\" md5=\"0\" sha1=\"%s\"/>\n"
             "\t</game>\n"
             "\t<game name=\"Bad Row\">\n"
             "\t\t<rom name=\"Bad.iso\" size=\"12\" crc=\"zz\" sha1=\"not-a-hash\"/>\n"
             "\t</game>\n"
             "\t<game name=\"Repeated\">\n"
             "\t\t<rom name=\"Again.iso\" size=\"%u\" crc=\"%s\" sha1=\"%s\"/>\n"
             "\t</game>\n"
             "\t<game name=\"Elsewhere (Japan)\">\n"
             "\t\t<rom name=\"Elsewhere.iso\" size=\"4\" crc=\"00000001\" "
             "sha1=\"00112233445566778899aabbccddeeff00112233\"/>\n"
             "\t</game>\n"
             "</datafile>\n",
             with_serials ? "\t\t<serial>TEST-70001, TEST-70001GA</serial>\n" : "",
             IMAGE_BYTES, g_retail_digest.crc32, g_retail_digest.sha1, IMAGE_BYTES,
             g_retail_digest.crc32, g_retail_digest.sha1);
    return dat;
}

static void test_dat_reader_and_tiers(void) {
    NkDiscExpandStatus status;
    NkDiscDat dat;
    char *text = build_dat(true);
    CHECK(nk_disc_dat_parse(text, strlen(text), &dat, &status));
    CHECK(strcmp(dat.name, "Synthetic & Test") == 0);
    CHECK(strcmp(dat.version, "2026-10-31") == 0);
    CHECK(dat.games == 4 && dat.rejected_roms == 1);
    CHECK(dat.rom_count == 2); /* the repeated SHA-1 is kept once */
    CHECK(dat.serial_count == 1 && nk_disc_dat_has_serial(&dat, "TEST70001"));
    CHECK(!nk_disc_dat_has_serial(&dat, "TEST70002"));

    /* Verified retail: the SHA-1 matches and the CRC-32 and size agree. */
    CHECK(nk_disc_classify(&dat, "TEST70001", &g_retail_digest, false) ==
          NK_DISC_TIER_VERIFIED_RETAIL);
    /* Modified: the list names the disc ID but not this image. */
    CHECK(nk_disc_classify(&dat, "TEST70001", &g_modified_digest, false) ==
          NK_DISC_TIER_MODIFIED);
    /* Unknown: neither the image nor the disc ID. */
    CHECK(nk_disc_classify(&dat, "TEST70009", &g_other_digest, false) == NK_DISC_TIER_UNKNOWN);
    /* ... unless a verified image in the library carries the disc ID. */
    CHECK(nk_disc_classify(&dat, "TEST70009", &g_other_digest, true) == NK_DISC_TIER_MODIFIED);
    /* A matching SHA-1 with another CRC-32 or size is not verified. */
    NkDiscDigest bent = g_retail_digest;
    bent.crc32[7] = bent.crc32[7] == '0' ? '1' : '0';
    CHECK(nk_disc_classify(&dat, "TEST70009", &bent, false) == NK_DISC_TIER_UNKNOWN);
    bent = g_retail_digest;
    bent.size_bytes += 2048u;
    CHECK(nk_disc_classify(&dat, "TEST70009", &bent, false) == NK_DISC_TIER_UNKNOWN);
    /* No list, or no hash: not checked. */
    CHECK(nk_disc_classify(NULL, "TEST70001", &g_retail_digest, false) ==
          NK_DISC_TIER_NOT_CHECKED);
    NkDiscDigest empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(nk_disc_classify(&dat, "TEST70001", &empty, false) == NK_DISC_TIER_NOT_CHECKED);
    nk_disc_dat_free(&dat);
    free(text);

    /* A list without serials (the form Redump's PSP list takes): a modified
       image is unknown until a verified image of its disc ID is beside it. */
    text = build_dat(false);
    CHECK(nk_disc_dat_parse(text, strlen(text), &dat, &status));
    CHECK(dat.serial_count == 0);
    CHECK(nk_disc_classify(&dat, "TEST70001", &g_modified_digest, false) == NK_DISC_TIER_UNKNOWN);
    CHECK(nk_disc_classify(&dat, "TEST70001", &g_modified_digest, true) == NK_DISC_TIER_MODIFIED);
    nk_disc_dat_free(&dat);
    free(text);

    /* The first of a repeated SHA-1 is the one kept, whatever qsort does. */
    char repeated[1024];
    snprintf(repeated, sizeof(repeated),
             "<datafile><game name=\"a\"><rom size=\"%u\" crc=\"%s\" sha1=\"%s\"/></game>"
             "<game name=\"b\"><rom size=\"4\" crc=\"00000000\" sha1=\"%s\"/></game>"
             "<game name=\"c\"><rom size=\"5\" crc=\"00000001\" sha1=\"%s\"/></game>"
             "</datafile>",
             IMAGE_BYTES, g_retail_digest.crc32, g_retail_digest.sha1, g_retail_digest.sha1,
             g_retail_digest.sha1);
    CHECK(nk_disc_dat_parse(repeated, strlen(repeated), &dat, &status));
    CHECK(dat.rom_count == 1 && dat.roms[0].size_bytes == IMAGE_BYTES);
    CHECK(nk_disc_classify(&dat, "TEST70001", &g_retail_digest, false) ==
          NK_DISC_TIER_VERIFIED_RETAIL);
    nk_disc_dat_free(&dat);
    /* A datafile that isn't UTF-8 text is refused by name. */
    const char latin1[] = "<datafile><header><name>Caf\xe9</name></header></datafile>";
    CHECK(!nk_disc_dat_parse(latin1, strlen(latin1), &dat, &status));
    CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0);

    const char *not_dat = "<html><body>nothing</body></html>";
    CHECK(!nk_disc_dat_parse(not_dat, strlen(not_dat), &dat, &status));
    CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0);
    /* A list cut short is refused, at a game's end or inside one. */
    {
        char *whole = build_dat(false);
        const char *cut_at = strstr(whole, "\t<game name=\"Elsewhere (Japan)\">");
        CHECK(cut_at != NULL);
        CHECK(!nk_disc_dat_parse(whole, (size_t)(cut_at - whole), &dat, &status));
        CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0 && dat.roms == NULL);
        const char *inside = strstr(whole, "Elsewhere.iso");
        CHECK(inside != NULL);
        char *spliced = (char *)malloc(strlen(whole) + 1u);
        CHECK(spliced != NULL);
        size_t head = (size_t)(inside - whole);
        memcpy(spliced, whole, head);
        snprintf(spliced + head, strlen(whole) + 1u - head, "</datafile>\n");
        CHECK(!nk_disc_dat_parse(spliced, strlen(spliced), &dat, &status));
        CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0 && dat.roms == NULL);
        /* Cut inside the closing tag itself. */
        const char *closing = strstr(whole, "</datafile>");
        CHECK(closing != NULL);
        CHECK(!nk_disc_dat_parse(whole, (size_t)(closing - whole) + strlen("</datafile"), &dat,
                                 &status));
        CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0 && dat.roms == NULL);
        free(spliced);
        free(whole);
    }
    const char *empty_dat = "<datafile><game name=\"x\"><rom name=\"x\" size=\"1\"/></game></datafile>";
    CHECK(!nk_disc_dat_parse(empty_dat, strlen(empty_dat), &dat, &status));
    CHECK(strcmp(status.code, NK_DISC_DAT_EMPTY) == 0);
    CHECK(dat.roms == NULL && dat.rom_count == 0);

    char ids[4][10];
    const char *serials = "TEST-71002, TEST-71002GA, test 71003,BAD,TEST-7100";
    CHECK(nk_disc_normalize_serials(serials, strlen(serials), ids, 4) == 2);
    CHECK(strcmp(ids[0], "TEST71002") == 0 && strcmp(ids[1], "TEST71003") == 0);

    CHECK(strcmp(nk_disc_tier_name(NK_DISC_TIER_MODIFIED), "MODIFIED") == 0);
    NkDiscTier tier;
    CHECK(nk_disc_tier_from_name("VERIFIED_RETAIL", &tier) && tier == NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(!nk_disc_tier_from_name("GOLD", &tier) && tier == NK_DISC_TIER_NOT_CHECKED);
    printf("PASS datafile reader: verified retail, modified, unknown, not checked\n");
}

static void test_dat_import(void) {
    NkDiscExpandStatus status;
    NkDiscDat dat;
    char source[NATIVE_TEST_PATH_MAX];
    char data_root[NATIVE_TEST_PATH_MAX];
    char imported[NATIVE_TEST_PATH_MAX];
    char *text = build_dat(true);
    path_in(source, sizeof(source), "user-chosen.dat");
    path_in(data_root, sizeof(data_root), "data");
    write_bytes(source, text, strlen(text));
    CHECK(nk_disc_dat_import(source, data_root, &dat, &status));
    CHECK(dat.rom_count == 2);
    nk_disc_dat_free(&dat);
    CHECK(nk_disc_dat_path(data_root, imported, sizeof(imported)));
    char *copy = read_text(imported);
    CHECK(strcmp(copy, text) == 0);
    free(copy);
    CHECK(nk_disc_dat_load(imported, &dat, &status) && dat.rom_count == 2);
    nk_disc_dat_free(&dat);

    /* A file that is not a list is refused, and the earlier import stays. */
    char bad[NATIVE_TEST_PATH_MAX];
    path_in(bad, sizeof(bad), "not-a-list.dat");
    write_bytes(bad, "hello", 5);
    CHECK(!nk_disc_dat_import(bad, data_root, NULL, &status));
    CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0);
    copy = read_text(imported);
    CHECK(strcmp(copy, text) == 0);
    free(copy);
    free(text);

    /* A later import replaces the list whole. */
    text = build_dat(false);
    write_bytes(source, text, strlen(text));
    CHECK(nk_disc_dat_import(source, data_root, NULL, &status));
    CHECK(nk_disc_dat_load(imported, &dat, &status) && dat.serial_count == 0);
    nk_disc_dat_free(&dat);
    free(text);
    printf("PASS datafile import into the data folder\n");
}

/* ---- the one call an add worker makes --------------------------------- */

static void test_verify_image_for_an_add_worker(void) {
    char root[NATIVE_TEST_PATH_MAX], retail[NATIVE_TEST_PATH_MAX], modified[NATIVE_TEST_PATH_MAX];
    char other[NATIVE_TEST_PATH_MAX], source[NATIVE_TEST_PATH_MAX], imported[NATIVE_TEST_PATH_MAX];
    NkDiscVerification check;
    NkDiscExpandStatus status;
    path_in(root, sizeof(root), "worker-data");
    path_in(retail, sizeof(retail), "worker-retail.iso");
    path_in(modified, sizeof(modified), "worker-modified.iso");
    path_in(other, sizeof(other), "worker-other.iso");
    write_bytes(retail, g_retail, IMAGE_BYTES);
    write_bytes(modified, g_modified, IMAGE_BYTES);
    write_bytes(other, g_other, IMAGE_BYTES);

    /* No list imported yet: hashed, not checked, and it says so. */
    CHECK(nk_disc_verify_image(retail, root, "TEST70001", false, NULL, &check));
    CHECK(check.result == NK_DISC_VERIFY_HASHED && check.tier == NK_DISC_TIER_NOT_CHECKED);
    CHECK(strcmp(check.digest.sha1, g_retail_digest.sha1) == 0 && check.code[0] == '\0');
    CHECK(strstr(check.reason, "no Redump list") != NULL);

    path_in(source, sizeof(source), "worker.dat");
    char *text = build_dat(false); /* serial-less, as Redump's PSP list is */
    write_bytes(source, text, strlen(text));
    free(text);
    CHECK(nk_disc_dat_import(source, root, NULL, &status));

    Progress progress = {0, 0, 0, -1};
    NkDiscExpandCallbacks callbacks = {on_progress, on_cancel, &progress};
    CHECK(nk_disc_verify_image(retail, root, "TEST70001", false, &callbacks, &check));
    CHECK(check.tier == NK_DISC_TIER_VERIFIED_RETAIL && progress.last_done == IMAGE_BYTES);
    CHECK(strncmp(check.reason, "Verified retail", 15) == 0);
    /* A lone modified image is unknown; beside its verified retail disc, modified. */
    CHECK(nk_disc_verify_image(modified, root, "TEST70001", false, NULL, &check));
    CHECK(check.tier == NK_DISC_TIER_UNKNOWN && strncmp(check.reason, "Unknown", 7) == 0);
    CHECK(nk_disc_verify_image(modified, root, "TEST70001", true, NULL, &check));
    CHECK(check.tier == NK_DISC_TIER_MODIFIED && strncmp(check.reason, "Modified", 8) == 0);
    CHECK(nk_disc_verify_image(other, root, "TEST70009", false, NULL, &check));
    CHECK(check.tier == NK_DISC_TIER_UNKNOWN);

    NkGameEntry entry;
    memset(&entry, 0, sizeof(entry));
    CHECK(nk_disc_verify_image(retail, root, "TEST70001", false, NULL, &check));
    nk_disc_verification_to_entry(&check, &entry);
    CHECK(strcmp(entry.content_sha1, g_retail_digest.sha1) == 0 &&
          strcmp(entry.content_crc32, g_retail_digest.crc32) == 0 &&
          entry.disc_verification == NK_DISC_TIER_VERIFIED_RETAIL);

    /* A cancel or an unreadable image keeps nothing, and the entry stays unhashed. */
    Progress stop = {0, 0, 0, 0};
    NkDiscExpandCallbacks cancel = {on_progress, on_cancel, &stop};
    CHECK(!nk_disc_verify_image(retail, root, "TEST70001", false, &cancel, &check));
    CHECK(check.result == NK_DISC_VERIFY_CANCELLED && check.digest.sha1[0] == '\0');
    CHECK(strcmp(check.code, NK_DISC_CANCELLED) == 0);
    memset(&entry, 0, sizeof(entry));
    nk_disc_verification_to_entry(&check, &entry);
    CHECK(entry.content_sha1[0] == '\0' && entry.disc_verification == NK_DISC_TIER_NOT_CHECKED);
    char missing[NATIVE_TEST_PATH_MAX];
    path_in(missing, sizeof(missing), "worker-missing.iso");
    CHECK(!nk_disc_verify_image(missing, root, "TEST70001", false, NULL, &check));
    CHECK(check.result == NK_DISC_VERIFY_READ_FAILED && strcmp(check.code, NK_DISC_READ_FAILED) == 0);

    /* A damaged imported list: still hashed, not checked, with the list's code. */
    CHECK(nk_disc_dat_path(root, imported, sizeof(imported)));
    write_bytes(imported, "damaged", 7);
    CHECK(nk_disc_verify_image(retail, root, "TEST70001", false, NULL, &check));
    CHECK(check.result == NK_DISC_VERIFY_LIST_UNUSABLE && check.tier == NK_DISC_TIER_NOT_CHECKED);
    CHECK(strcmp(check.code, NK_DISC_DAT_NOT_DATAFILE) == 0 && check.digest.sha1[0] != '\0');
    printf("PASS one call for an add worker: tier, reason, cancel, damaged list\n");
}

/* ---- library keyed by disc ID plus content hash ------------------------ */

static void seed(NkGameEntry *entry, const char *disc_id, const char *iso,
                 const NkDiscDigest *digest, NkDiscTier tier) {
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->disc_id, sizeof(entry->disc_id), "%s", disc_id);
    snprintf(entry->title_name, sizeof(entry->title_name), "Title %s", disc_id);
    snprintf(entry->disc_version, sizeof(entry->disc_version), "1.00");
    snprintf(entry->title_id, sizeof(entry->title_id), "%s-id", disc_id);
    snprintf(entry->iso_path, sizeof(entry->iso_path), "games/%s", iso);
    snprintf(entry->boot_executable, sizeof(entry->boot_executable), "EBOOT.BIN");
    entry->status = NK_STATUS_IDENTIFIED;
    entry->iso_size_bytes = IMAGE_BYTES;
    if (digest) {
        memcpy(entry->content_sha1, digest->sha1, sizeof(entry->content_sha1));
        memcpy(entry->content_crc32, digest->crc32, sizeof(entry->content_crc32));
    }
    entry->disc_verification = (uint32_t)tier;
}

/* A library.json exactly as the writer before disc hashing wrote it. */
static const char k_old_library[] =
    "{\n  \"schema_version\": 1,\n  \"games\": [\n"
    "    {\n      \"disc_id\": \"TEST70001\",\n      \"title_name\": \"Old Retail\",\n"
    "      \"disc_version\": \"1.00\",\n      \"iso_path\": \"games/old.iso\",\n"
    "      \"prepared_root\": \"\",\n      \"title_id\": \"TEST70001-id\",\n"
    "      \"iso_size_bytes\": 8192,\n      \"status\": 1,\n      \"is_experimental\": false,\n"
    "      \"executable_eboot_kind\": 0,\n      \"executable_boot_kind\": 0,\n"
    "      \"executable_selection\": 0,\n      \"executable_boot_fallback\": false,\n"
    "      \"boot_executable\": \"EBOOT.BIN\",\n      \"selected_executable\": \"\",\n"
    "      \"is_prepared\": false,\n      \"assets_staged\": false,\n"
    "      \"extracted_asset_count\": 0,\n      \"extracted_audio_count\": 0,\n"
    "      \"extracted_visual_count\": 0,\n      \"extracted_layout_count\": 0,\n"
    "      \"last_played\": \"\"\n    },\n"
    "    {\n      \"disc_id\": \"TEST70002\",\n      \"title_name\": \"Old Other\",\n"
    "      \"disc_version\": \"1.01\",\n      \"iso_path\": \"games/other.iso\",\n"
    "      \"prepared_root\": \"\",\n      \"title_id\": \"TEST70002-id\",\n"
    "      \"iso_size_bytes\": 4096,\n      \"status\": 2,\n      \"is_experimental\": true,\n"
    "      \"executable_eboot_kind\": 1,\n      \"executable_boot_kind\": 0,\n"
    "      \"executable_selection\": 1,\n      \"executable_boot_fallback\": false,\n"
    "      \"boot_executable\": \"EBOOT.BIN\",\n      \"selected_executable\": \"EBOOT.BIN\",\n"
    "      \"is_prepared\": true,\n      \"assets_staged\": false,\n"
    "      \"extracted_asset_count\": 0,\n      \"extracted_audio_count\": 0,\n"
    "      \"extracted_visual_count\": 0,\n      \"extracted_layout_count\": 0,\n"
    "      \"last_played\": \"2026-10-01T00:00:00Z\"\n    }\n"
    "  ]\n}\n";

static NkLibrary g_lib;   /* too large for the stack (NK_MAX_GAMES) */
static NkLibrary g_loaded;

static void test_old_library_still_loads(void) {
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "old_library.json");
    write_bytes(path, k_old_library, strlen(k_old_library));
    CHECK(nk_library_load(&g_lib, path) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    const NkGameEntry *old = nk_library_find_by_disc_id(&g_lib, "TEST70001");
    CHECK(old && strcmp(old->title_name, "Old Retail") == 0);
    CHECK(old->content_sha1[0] == '\0' && old->disc_verification == NK_DISC_TIER_NOT_CHECKED);
    const NkGameEntry *other = nk_library_find_by_disc_id(&g_lib, "TEST70002");
    CHECK(other && other->is_prepared && other->is_experimental &&
          strcmp(other->last_played, "2026-10-01T00:00:00Z") == 0);

    /* Saved again, it writes no hash members for records never hashed, and
       reads back the same records. */
    CHECK(nk_library_save(&g_lib, path) == NK_OK);
    char *saved = read_text(path);
    CHECK(strstr(saved, "content_sha1") == NULL && strstr(saved, "disc_verification") == NULL);
    free(saved);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_count(&g_loaded) == 2);
    CHECK(memcmp(nk_library_get(&g_loaded, 0), nk_library_get(&g_lib, 0), sizeof(NkGameEntry)) == 0);
    CHECK(memcmp(nk_library_get(&g_loaded, 1), nk_library_get(&g_lib, 1), sizeof(NkGameEntry)) == 0);
    printf("PASS a library written before disc hashing still loads and saves the same\n");
}

static void test_library_keys_by_disc_id_and_hash(void) {
    NkGameEntry retail, translation, entry;
    nk_library_init(&g_lib);

    /* The fan translation first: unknown, since nothing shows its disc ID is
       retail yet. The retail disc beside it is verified, which labels the
       translation modified. Both stay. */
    seed(&translation, "TEST70001", "translation.iso", &g_modified_digest, NK_DISC_TIER_UNKNOWN);
    seed(&retail, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &translation) == NK_OK);
    CHECK(nk_library_add_or_update(&g_lib, &retail) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    const NkGameEntry *t = nk_library_find_by_key(&g_lib, "TEST70001", g_modified_digest.sha1);
    const NkGameEntry *r = nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1);
    CHECK(t && r && t != r);
    CHECK(t->disc_verification == NK_DISC_TIER_MODIFIED);
    CHECK(r->disc_verification == NK_DISC_TIER_VERIFIED_RETAIL);
    /* The disc-ID API still answers, with the first record. */
    CHECK(nk_library_find_by_disc_id(&g_lib, "TEST70001") == t);
    CHECK(nk_library_has_verified_disc_id(&g_lib, "TEST70001"));
    CHECK(!nk_library_has_verified_disc_id(&g_lib, "TEST70002"));

    /* The same image again replaces its own record only. */
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    snprintf(entry.title_name, sizeof(entry.title_name), "Retail, renamed");
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    r = nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1);
    CHECK(r && strcmp(r->title_name, "Retail, renamed") == 0);

    /* An update of the same file made without hashing replaces that file's
       record, not the first record's, and leaves it unhashed: the file may hold
       other bytes now. An update that carries the hashes keeps them. */
    seed(&entry, "TEST70001", "retail.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    snprintf(entry.last_played, sizeof(entry.last_played), "2026-10-31T00:00:00Z");
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1) == NULL);
    t = nk_library_find_by_key(&g_lib, "TEST70001", g_modified_digest.sha1);
    CHECK(t && t->last_played[0] == '\0');
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    snprintf(entry.last_played, sizeof(entry.last_played), "2026-10-31T00:00:00Z");
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    r = nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1);
    CHECK(r && strcmp(r->last_played, "2026-10-31T00:00:00Z") == 0 &&
          r->disc_verification == NK_DISC_TIER_VERIFIED_RETAIL);

    /* Saved and read back: both records, their hashes and tiers. */
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "keyed_library.json");
    CHECK(nk_library_save(&g_lib, path) == NK_OK);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_count(&g_loaded) == 2);
    for (int i = 0; i < 2; i++) {
        CHECK(memcmp(nk_library_get(&g_loaded, i), nk_library_get(&g_lib, i),
                     sizeof(NkGameEntry)) == 0);
    }

    /* Removing by key takes the named image out and leaves the other. */
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1) != NULL);
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) ==
          NK_ERROR_FILE_NOT_FOUND);

    /* A record written before hashing (a library built before this change, its
       retail disc unhashed) is kept when a hashed image of another file
       arrives: nothing shows they are the same image. */
    nk_library_init(&g_lib);
    seed(&entry, "TEST70002", "old.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70002", "new.iso", &g_other_digest, NK_DISC_TIER_UNKNOWN);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70002", "") ==
          nk_library_get(&g_lib, 0)); /* the old record, still there */
    /* The record of the same file is replaced: the file holds this image now
       (a compressed image expanded over the same path), and the old record of
       that file is hashed by the new one. */
    seed(&entry, "TEST70002", "new.iso", &g_retail_digest, NK_DISC_TIER_UNKNOWN);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2 &&
          strcmp(nk_library_get(&g_lib, 1)->content_sha1, g_retail_digest.sha1) == 0);
    seed(&entry, "TEST70002", "old.iso", &g_modified_digest, NK_DISC_TIER_UNKNOWN);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2 &&
          strcmp(nk_library_get(&g_lib, 0)->content_sha1, g_modified_digest.sha1) == 0);

    /* An unhashed entry of a new file replaces the record its disc ID alone
       picks out when that record's file is gone (the disc moved), as before
       hashing; with two hashed images of the disc ID it can pick out neither,
       so it is added and both images stay. */
    nk_library_init(&g_lib);
    seed(&entry, "TEST70003", "one.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70003", "moved.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_find_replaced(&g_lib, &entry) == nk_library_get(&g_lib, 0));
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1 &&
          strcmp(nk_library_get(&g_lib, 0)->iso_path, "games/moved.iso") == 0);
    /* While the first image's file is still there, an unhashed second image
       (added before either is hashed) is another card, not a move. */
    {
        char first_file[NATIVE_TEST_PATH_MAX];
        path_in(first_file, sizeof(first_file), "still_here.iso");
        write_bytes(first_file, "image", 5u);
        nk_library_init(&g_lib);
        seed(&entry, "TEST70003", "unused.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
        CHECK(strlen(first_file) < sizeof(entry.iso_path));
        memcpy(entry.iso_path, first_file, strlen(first_file) + 1u);
        CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
        seed(&entry, "TEST70003", "second.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
        CHECK(nk_library_find_replaced(&g_lib, &entry) == NULL);
        CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
        CHECK(nk_library_count(&g_lib) == 2);
        remove(first_file);
    }
    nk_library_init(&g_lib);
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70001", "translation.iso", &g_modified_digest, NK_DISC_TIER_MODIFIED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70001", "third.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 3);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1) != NULL);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) != NULL);
    /* Nor does it replace the only record when that one is hashed: an image
       whose hash failed may be another image. */
    nk_library_init(&g_lib);
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70001", "unread.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2 &&
          nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1) != NULL);

    /* The image moved, then was added again: the new file's record, unhashed,
       stays beside the hashed one until it is hashed. Then it takes over the
       image's record, which now names the new file, and the record the new
       file got before hashing goes: one file holds one image. */
    nk_library_init(&g_lib);
    seed(&entry, "TEST70004", "first.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70004", "second.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    seed(&entry, "TEST70004", "second.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1 &&
          strcmp(nk_library_get(&g_lib, 0)->iso_path, "games/second.iso") == 0 &&
          strcmp(nk_library_get(&g_lib, 0)->content_sha1, g_retail_digest.sha1) == 0);
    /* Another image of the disc ID is never dropped with it. */
    seed(&entry, "TEST70004", "translation.iso", &g_modified_digest, NK_DISC_TIER_UNKNOWN);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70004", "third.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2 &&
          nk_library_find_by_key(&g_lib, "TEST70004", g_modified_digest.sha1) != NULL &&
          strcmp(nk_library_find_by_key(&g_lib, "TEST70004", g_retail_digest.sha1)->iso_path,
                 "games/third.iso") == 0);

    /* A player re-add of a record a bring-up wrote replaces it, unhashed until
       the player's check of the add ends: one record, never a stale tier. */
    nk_library_init(&g_lib);
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    entry.iso_size_bytes = 0;
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70001", "retail.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1);
    CHECK(nk_library_get(&g_lib, 0)->content_sha1[0] == '\0' &&
          nk_library_get(&g_lib, 0)->disc_verification == NK_DISC_TIER_NOT_CHECKED);
    printf("PASS library keyed by disc ID plus hash: both discs kept, the modified one labelled\n");
}

static void test_library_remove_never_takes_the_other_image(void) {
    NkGameEntry retail, translation;
    seed(&retail, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    seed(&translation, "TEST70001", "translation.iso", &g_modified_digest, NK_DISC_TIER_MODIFIED);
    nk_library_init(&g_lib);
    CHECK(nk_library_add_or_update(&g_lib, &retail) == NK_OK);
    CHECK(nk_library_add_or_update(&g_lib, &translation) == NK_OK);
    /* The disc ID alone can't say which image to remove: refused, nothing changed. */
    CHECK(nk_library_remove(&g_lib, "TEST70001") == NK_ERROR_AMBIGUOUS);
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", NULL) == NK_ERROR_AMBIGUOUS);
    /* An empty hash names the unhashed record, and neither image is one. */
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", "") == NK_ERROR_FILE_NOT_FOUND);
    CHECK(nk_library_count(&g_lib) == 2);
    /* Beside an unhashed record of another file (a library built before
       hashing), removing a hashed card by disc ID alone still takes nothing,
       and the empty hash takes the unhashed record only. */
    NkGameEntry legacy;
    seed(&legacy, "TEST70001", "legacy.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    g_lib.entries[g_lib.count++] = legacy;
    CHECK(nk_library_remove(&g_lib, "TEST70001") == NK_ERROR_AMBIGUOUS);
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", "") == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2 &&
          nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1) != NULL &&
          nk_library_find_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) != NULL);
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "remove_ambiguous.json");
    CHECK(nk_library_save(&g_lib, path) == NK_OK);
    CHECK(nk_library_commit_remove(&g_lib, "TEST70001", path) == NK_ERROR_AMBIGUOUS);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK && nk_library_count(&g_loaded) == 2);
    /* By key, the named image goes and the other stays, in memory and on disk. */
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) == NK_OK);
    CHECK(nk_library_commit_remove_by_key(&g_lib, "TEST70001", g_modified_digest.sha1, path) ==
          NK_OK);
    CHECK(nk_library_count(&g_lib) == 1 &&
          strcmp(nk_library_get(&g_lib, 0)->content_sha1, g_retail_digest.sha1) == 0);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK && nk_library_count(&g_loaded) == 1 &&
          strcmp(nk_library_get(&g_loaded, 0)->content_sha1, g_retail_digest.sha1) == 0);
    /* With one image left, the disc ID alone picks it out again. */
    CHECK(nk_library_commit_remove(&g_lib, "TEST70001", path) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 0);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK && nk_library_count(&g_loaded) == 0);
    printf("PASS library remove never takes the other image of a disc ID\n");
}

/* Two unhashed images of one disc ID (added before either was hashed): the
   empty hash can't say which to remove, the file can, in memory and on disk. */
static void test_library_remove_by_file(void) {
    NkGameEntry one, two;
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "remove_by_file.json");
    seed(&one, "TEST70005", "one.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    seed(&two, "TEST70005", "two.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    nk_library_init(&g_lib);
    g_lib.entries[g_lib.count++] = one;
    g_lib.entries[g_lib.count++] = two;
    CHECK(nk_library_save(&g_lib, path) == NK_OK);
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70005", "") == NK_ERROR_AMBIGUOUS);
    CHECK(nk_library_remove_by_file(&g_lib, "TEST70005", "games/two.iso") == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1 &&
          strcmp(nk_library_get(&g_lib, 0)->iso_path, "games/one.iso") == 0);
    CHECK(nk_library_remove_by_file(&g_lib, "TEST70005", "games/two.iso") ==
          NK_ERROR_FILE_NOT_FOUND);
    CHECK(nk_library_commit_remove_by_file(&g_lib, "TEST70005", "games/two.iso", path) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK && nk_library_count(&g_loaded) == 1 &&
          strcmp(nk_library_get(&g_loaded, 0)->iso_path, "games/one.iso") == 0);
    remove(path);
    printf("PASS library remove by file names one of two unhashed images\n");
}

static void test_library_commit_keeps_both_images(void) {
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "commit_keyed.json");
    NkGameEntry retail, translation;
    seed(&retail, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    seed(&translation, "TEST70001", "translation.iso", &g_modified_digest, NK_DISC_TIER_UNKNOWN);
    nk_library_init(&g_lib);
    CHECK(nk_library_commit_entry(&g_lib, &retail, path) == NK_OK);

    /* Another writer (a bring-up) adds the translation to the file. */
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_add_or_update(&g_loaded, &translation) == NK_OK);
    CHECK(nk_library_save(&g_loaded, path) == NK_OK);

    /* This writer's next commit keeps it and adopts it once. */
    NkGameEntry other;
    seed(&other, "TEST70003", "third.iso", &g_other_digest, NK_DISC_TIER_UNKNOWN);
    CHECK(nk_library_commit_entry(&g_lib, &other, path) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 3);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_count(&g_loaded) == 3);
    const NkGameEntry *t = nk_library_find_by_key(&g_loaded, "TEST70001", g_modified_digest.sha1);
    CHECK(t && t->disc_verification == NK_DISC_TIER_MODIFIED);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) != NULL);

    /* Removing the translation by key leaves the retail record on disk. */
    CHECK(nk_library_commit_remove_by_key(&g_lib, "TEST70001", g_modified_digest.sha1, path) ==
          NK_OK);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_count(&g_loaded) == 2);
    CHECK(nk_library_find_by_key(&g_loaded, "TEST70001", g_retail_digest.sha1) != NULL);
    CHECK(nk_library_find_by_key(&g_loaded, "TEST70001", g_modified_digest.sha1) == NULL);
    CHECK(nk_library_count(&g_lib) == 2);
    printf("PASS library commits keep both images of one disc ID\n");
}

static void test_library_hash_members_are_checked(void) {
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "members.json");
    NkGameEntry entry;
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    nk_library_init(&g_lib);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_save(&g_lib, path) == NK_OK);
    char *text = read_text(path);

    /* A tier name a later build might add reads as not checked. */
    char *tier = strstr(text, "\"VERIFIED_RETAIL\"");
    CHECK(tier != NULL);
    char *changed = (char *)malloc(strlen(text) + 16u);
    CHECK(changed != NULL);
    size_t head = (size_t)(tier - text);
    memcpy(changed, text, head);
    strcpy(changed + head, "\"GOLD_MASTER\"");
    strcat(changed, tier + strlen("\"VERIFIED_RETAIL\""));
    write_bytes(path, changed, strlen(changed));
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_get(&g_loaded, 0)->disc_verification == NK_DISC_TIER_NOT_CHECKED);
    CHECK(strcmp(nk_library_get(&g_loaded, 0)->content_sha1, g_retail_digest.sha1) == 0);

    /* A malformed hash is not a record this player wrote: the file is refused
       (and its .bak would be tried), never read with the hash dropped. */
    char bak[NATIVE_TEST_PATH_MAX + 8];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    (void)nk_remove_utf8(bak);
    char *sha = strstr(text, g_retail_digest.sha1);
    CHECK(sha != NULL);
    memcpy(changed, text, strlen(text) + 1u);
    changed[(size_t)(sha - text) + 3u] = 'x';
    write_bytes(path, changed, strlen(changed));
    CHECK(nk_library_load(&g_loaded, path) == NK_ERROR_GENERIC);
    CHECK(nk_library_count(&g_loaded) == 0);
    free(changed);
    free(text);
    printf("PASS library hash members: unknown tier reads unchecked, malformed hash refused\n");
}

int main(void) {
    native_test_create_root("disc-verify");
    native_test_isolate_user_data_roots();
    build_images();
    test_sha1_and_crc32_known_answers();
    test_hash_file();
    test_dat_reader_and_tiers();
    test_dat_import();
    test_verify_image_for_an_add_worker();
    test_old_library_still_loads();
    test_library_keys_by_disc_id_and_hash();
    test_library_remove_never_takes_the_other_image();
    test_library_remove_by_file();
    test_library_commit_keeps_both_images();
    test_library_hash_members_are_checked();
    printf("test_disc_verify: %d checks passed\n", g_checks);
    return 0;
}
