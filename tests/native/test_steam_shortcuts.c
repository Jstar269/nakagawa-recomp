/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Add to Steam (#2152, src/core/nk_steam_shortcuts.c): the binary shortcuts.vdf edit and the
 * file-level rules around it. The files are built here byte by byte in a temporary folder; no
 * Steam client, registry entry or network is involved. */

#include "nk_platform.h"
#include "nk_steam_shortcuts.h"
#include "native_test_isolation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fixture paths are short temporary-folder names; the compiler can only assume the longest
 * a buffer allows, and every path is checked by the calls that use it. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

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

#define CHECK_STATUS(actual, expected, what)                                     \
    do {                                                                         \
        ++g_checks;                                                              \
        if ((actual) != (expected)) {                                            \
            ++g_failures;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s: got %s, want %s\n", __FILE__,       \
                    __LINE__, (what), nk_steam_status_name(actual),              \
                    nk_steam_status_name(expected));                             \
        }                                                                        \
    } while (0)

static const NkSteamShortcut kPlayer = {
    "Nakagawa Recomp", "C:/Games/Nakagawa/bin/nakagawa_player.exe", "C:/Games/Nakagawa/bin", ""};

/* ---- A byte builder for existing files ---- */

typedef struct {
    uint8_t data[8192];
    size_t size;
} Raw;

static void raw_byte(Raw *r, uint8_t b) { r->data[r->size++] = b; }
static void raw_cstr(Raw *r, const char *s) {
    memcpy(r->data + r->size, s, strlen(s) + 1u);
    r->size += strlen(s) + 1u;
}
static void raw_str(Raw *r, const char *key, const char *value) {
    raw_byte(r, 0x01);
    raw_cstr(r, key);
    raw_cstr(r, value);
}
static void raw_int(Raw *r, const char *key, uint32_t value) {
    raw_byte(r, 0x02);
    raw_cstr(r, key);
    for (int i = 0; i < 4; i++) raw_byte(r, (uint8_t)(value >> (8 * i)));
}
/* One entry as Steam itself wrote it, with a lower-case "exe" key and fields this code
 * never writes (a tag and a float), so the preserved bytes are not only our own layout. */
static void raw_entry(Raw *r, const char *index, const char *name, const char *quoted_exe) {
    raw_byte(r, 0x00);
    raw_cstr(r, index);
    raw_int(r, "appid", 0x80001234u);
    raw_str(r, "AppName", name);
    raw_str(r, "exe", quoted_exe);
    raw_str(r, "StartDir", "\"C:/Other\"");
    raw_int(r, "LastPlayTime", 1700000000u);
    raw_byte(r, 0x03);
    raw_cstr(r, "Score");
    for (int i = 0; i < 4; i++) raw_byte(r, 0);
    raw_byte(r, 0x00);
    raw_cstr(r, "tags");
    raw_str(r, "0", "favorite");
    raw_byte(r, 0x08);
    raw_byte(r, 0x08);
}
static void raw_open(Raw *r) {
    raw_byte(r, 0x00);
    raw_cstr(r, "shortcuts");
}
static void raw_close(Raw *r) {
    raw_byte(r, 0x08);
    raw_byte(r, 0x08);
}

static void two_entries(Raw *r) {
    memset(r, 0, sizeof(*r));
    raw_open(r);
    raw_entry(r, "0", "Some Game", "\"C:/Games/A/a.exe\"");
    raw_entry(r, "1", "Other Game", "\"C:/Games/B/b.exe\"");
    raw_close(r);
}

/* ---- Files ---- */

static char g_root[NATIVE_TEST_PATH_MAX];

static void path_in(char *out, size_t cap, const char *leaf) {
    snprintf(out, cap, "%s/%s", g_root, leaf);
}

static bool write_bytes(const char *path, const uint8_t *data, size_t size) {
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) return false;
    bool ok = size == 0u || fwrite(data, 1u, size, f) == size;
    return fclose(f) == 0 && ok;
}

static uint8_t *read_bytes(const char *path, size_t *size) {
    *size = 0;
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return NULL;
    uint8_t *data = (uint8_t *)malloc(1u << 20);
    if (!data) {
        fclose(f);
        return NULL;
    }
    *size = fread(data, 1u, 1u << 20, f);
    fclose(f);
    return data;
}

static bool file_is(const char *path, const uint8_t *data, size_t size) {
    size_t got = 0;
    uint8_t *back = read_bytes(path, &got);
    bool same = back && got == size && (size == 0u || memcmp(back, data, size) == 0);
    free(back);
    return same;
}

static const void *find_bytes(const void *hay, size_t hay_size, const void *needle, size_t needle_size) {
    const uint8_t *h = (const uint8_t *)hay;
    for (size_t i = 0; needle_size <= hay_size && i + needle_size <= hay_size; i++) {
        if (memcmp(h + i, needle, needle_size) == 0) return h + i;
    }
    return NULL;
}

static bool is_running_yes(void) { return true; }
static bool is_running_no(void) { return false; }

/* ---- Tests ---- */

static void test_app_id(void) {
    /* The value zlib's crc32 gives for the quoted path followed by the name, top bit set. */
    CHECK(nk_steam_shortcut_app_id(kPlayer.exe, kPlayer.app_name) == 0xcb3fa139u,
          "the app ID is the CRC-32 of the quoted exe and name, top bit set");
    CHECK((nk_steam_shortcut_app_id("a", "b") & 0x80000000u) != 0u, "the top bit is always set");
}

static void test_new_file(void) {
    uint8_t *out = NULL;
    size_t size = 0;
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &kPlayer, &out, &size), NK_STEAM_OK, "new file");
    CHECK(out && size > 16u, "a new file has content");
    if (!out) return;
    static const uint8_t head[] = {0x00, 's', 'h', 'o', 'r', 't', 'c', 'u', 't', 's', 0x00,
                                   0x00, '0', 0x00};
    CHECK(size >= sizeof(head) && memcmp(out, head, sizeof(head)) == 0,
          "starts with the shortcuts map and entry 0");
    CHECK(size >= 2u && out[size - 1u] == 0x08 && out[size - 2u] == 0x08,
          "ends with the end of shortcuts and the end of the root");
    bool found = false;
    CHECK_STATUS(nk_steam_vdf_has_exe(out, size, kPlayer.exe, &found), NK_STEAM_OK, "re-parse");
    CHECK(found, "the new file lists the executable");
    /* The executable is stored quoted, as Steam does, and the app ID is written. */
    static const char quoted[] = "\"C:/Games/Nakagawa/bin/nakagawa_player.exe\"";
    CHECK(find_bytes(out, size, quoted, sizeof(quoted)) != NULL, "the executable is quoted");
    static const uint8_t appid[] = {0x02, 'a', 'p', 'p', 'i', 'd', 0x00, 0x39, 0xa1, 0x3f, 0xcb};
    CHECK(find_bytes(out, size, appid, sizeof(appid)) != NULL, "the app ID is written little endian");
    nk_steam_free(out);
}

static void test_existing_entries_are_preserved(void) {
    Raw in;
    two_entries(&in);
    uint8_t *out = NULL;
    size_t size = 0;
    CHECK_STATUS(nk_steam_vdf_add(in.data, in.size, &kPlayer, &out, &size), NK_STEAM_OK,
                 "add to a file with two entries");
    if (!out) return;
    size_t end = in.size - 2u; /* the two 0x08 */
    CHECK(size > in.size, "the file grew");
    CHECK(memcmp(out, in.data, end) == 0, "every earlier byte, entry 0 and entry 1, is unchanged");
    CHECK(memcmp(out + size - 2u, in.data + end, 2u) == 0, "the closing bytes are unchanged");
    CHECK(out[end] == 0x00 && out[end + 1u] == '2' && out[end + 2u] == 0x00,
          "the new entry is key 2, after the last one");
    bool found = false;
    nk_steam_vdf_has_exe(out, size, "C:/Games/B/b.exe", &found);
    CHECK(found, "the other games are still listed");
    nk_steam_free(out);
}

static void test_three_adds_number_in_order(void) {
    NkSteamShortcut a = {"A", "C:/a/a.exe", "C:/a", NULL};
    NkSteamShortcut b = {"B", "C:/b/b.exe", "C:/b", "--ui=next"};
    NkSteamShortcut c = {"C", "C:/c/c.exe", "C:/c", ""};
    uint8_t *one = NULL, *two = NULL, *three = NULL;
    size_t n1 = 0, n2 = 0, n3 = 0;
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &a, &one, &n1), NK_STEAM_OK, "first");
    CHECK_STATUS(nk_steam_vdf_add(one, n1, &b, &two, &n2), NK_STEAM_OK, "second");
    CHECK_STATUS(nk_steam_vdf_add(two, n2, &c, &three, &n3), NK_STEAM_OK, "third");
    if (three) {
        static const uint8_t k1[] = {0x00, '1', 0x00, 0x02, 'a', 'p', 'p', 'i', 'd'};
        static const uint8_t k2[] = {0x00, '2', 0x00, 0x02, 'a', 'p', 'p', 'i', 'd'};
        CHECK(find_bytes(three, n3, k1, sizeof(k1)) != NULL, "entry 1 exists");
        CHECK(find_bytes(three, n3, k2, sizeof(k2)) != NULL, "entry 2 exists");
        CHECK(find_bytes(three, n3, "--ui=next", 10u) != NULL, "launch options are written");
    }
    nk_steam_free(one);
    nk_steam_free(two);
    nk_steam_free(three);
}

static void test_existing_executable_is_reported_not_added(void) {
    Raw in;
    two_entries(&in);
    uint8_t *out = (uint8_t *)1;
    size_t size = 7;
    NkSteamShortcut same = {"Renamed", "c:/games/a/A.EXE", "C:/Games/A", ""};
    CHECK_STATUS(nk_steam_vdf_add(in.data, in.size, &same, &out, &size), NK_STEAM_ERR_EXISTS,
                 "the same executable, in another case and with another name");
    CHECK(out == NULL && size == 0u, "nothing is returned");
}

static void test_no_shortcuts_map_gets_one(void) {
    static const uint8_t only_root_end[] = {0x08};
    uint8_t *out = NULL;
    size_t size = 0;
    CHECK_STATUS(nk_steam_vdf_add(only_root_end, 1u, &kPlayer, &out, &size), NK_STEAM_OK,
                 "a valid file without a shortcuts map");
    bool found = false;
    CHECK(out && nk_steam_vdf_has_exe(out, size, kPlayer.exe, &found) == NK_STEAM_OK && found,
          "the map is created and lists the executable");
    nk_steam_free(out);
}

static void test_corrupt_files_are_refused(void) {
    Raw good;
    two_entries(&good);
    uint8_t *out = NULL;
    size_t size = 0;

    /* Every proper prefix is damaged, never accepted, never a crash. */
    int accepted = 0;
    for (size_t cut = 1; cut < good.size; cut++) {
        NkSteamStatus s = nk_steam_vdf_add(good.data, cut, &kPlayer, &out, &size);
        if (s == NK_STEAM_OK) accepted++;
        CHECK(s == NK_STEAM_ERR_CORRUPT || s == NK_STEAM_OK, "a cut file is corrupt or still whole");
        nk_steam_free(out);
        out = NULL;
    }
    /* The one cut that stays valid is after the first root map closes: none here, since the
     * file ends with the only two closing bytes. */
    CHECK(accepted == 0, "no proper prefix of a file is accepted as a file");

    Raw bad;
    memcpy(&bad, &good, sizeof(bad));
    bad.data[bad.size++] = 0x00; /* trailing byte */
    CHECK_STATUS(nk_steam_vdf_add(bad.data, bad.size, &kPlayer, &out, &size), NK_STEAM_ERR_CORRUPT,
                 "trailing bytes");

    memset(&bad, 0, sizeof(bad));
    raw_open(&bad);
    raw_byte(&bad, 0x09); /* an unknown type */
    raw_cstr(&bad, "x");
    raw_close(&bad);
    CHECK_STATUS(nk_steam_vdf_add(bad.data, bad.size, &kPlayer, &out, &size), NK_STEAM_ERR_CORRUPT,
                 "an unknown value type");

    memset(&bad, 0, sizeof(bad));
    raw_open(&bad);
    raw_byte(&bad, 0x01);
    raw_cstr(&bad, "key");
    memcpy(bad.data + bad.size, "no terminator", 13u); /* a string that never ends */
    bad.size += 13u;
    CHECK_STATUS(nk_steam_vdf_add(bad.data, bad.size, &kPlayer, &out, &size), NK_STEAM_ERR_CORRUPT,
                 "an unterminated string");

    memset(&bad, 0, sizeof(bad));
    for (int i = 0; i < 40; i++) {
        raw_byte(&bad, 0x00);
        raw_cstr(&bad, "n");
    }
    for (int i = 0; i < 41; i++) raw_byte(&bad, 0x08);
    CHECK_STATUS(nk_steam_vdf_add(bad.data, bad.size, &kPlayer, &out, &size), NK_STEAM_ERR_CORRUPT,
                 "nesting past the cap");

    memset(&bad, 0, sizeof(bad));
    raw_open(&bad);
    raw_byte(&bad, 0x02);
    raw_cstr(&bad, "short");
    raw_byte(&bad, 1); /* an int32 with one byte */
    CHECK_STATUS(nk_steam_vdf_add(bad.data, bad.size, &kPlayer, &out, &size), NK_STEAM_ERR_CORRUPT,
                 "a short integer");

    CHECK_STATUS(nk_steam_vdf_add((const uint8_t *)"text", 4u, &kPlayer, &out, &size),
                 NK_STEAM_ERR_CORRUPT, "plain text");

    /* Single-byte damage must never crash. A refused file yields no output and leaves its input as it
       was; an accepted one parses, lists the executable, and keeps every byte of the damaged input
       but for the new entry (everything before the last two end markers, and those two). */
    for (size_t at = 0; at < good.size; at++) {
        Raw flip, before;
        memcpy(&flip, &good, sizeof(flip));
        flip.data[at] ^= 0xFFu;
        memcpy(&before, &flip, sizeof(before));
        out = (uint8_t *)0x1;
        size = 12345u;
        NkSteamStatus s = nk_steam_vdf_add(flip.data, flip.size, &kPlayer, &out, &size);
        CHECK(memcmp(&before, &flip, sizeof(flip)) == 0, "the input is not modified");
        if (s == NK_STEAM_OK) {
            bool found = false;
            CHECK(nk_steam_vdf_has_exe(out, size, kPlayer.exe, &found) == NK_STEAM_OK && found,
                  "a returned file parses and lists the executable");
            CHECK(size > flip.size && memcmp(out, flip.data, flip.size - 2u) == 0 &&
                      memcmp(out + size - 2u, flip.data + flip.size - 2u, 2u) == 0,
                  "an accepted file keeps every byte it had");
        } else {
            CHECK(out == NULL && size == 0u, "a refused file yields no output");
            out = NULL;
        }
        nk_steam_free(out);
        out = NULL;
    }
}

static void test_steam_running_fails_closed(void) {
    CHECK(!nk_steam_open_error_means_running(87ul), "a pid that is gone is not running");
    CHECK(nk_steam_open_error_means_running(5ul), "access denied (an elevated Steam) is running");
    CHECK(nk_steam_open_error_means_running(0ul), "an unexplained failure is running");
    CHECK(nk_steam_open_error_means_running(1450ul), "any other error is running");
    CHECK(!nk_steam_registry_error_means_running(2ul), "no pid value: Steam never ran");
    CHECK(!nk_steam_registry_error_means_running(3ul), "no pid key: Steam never ran");
    CHECK(nk_steam_registry_error_means_running(5ul), "a pid that cannot be read is running");
    CHECK(nk_steam_registry_error_means_running(234ul), "a short buffer is running");
    uint8_t *out = NULL;
    size_t size = 0;
    NkSteamShortcut quoted = kPlayer;
    quoted.exe = "\"C:/Games/Nakagawa/nakagawa_player.exe\"";
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &quoted, &out, &size), NK_STEAM_ERR_ARGUMENT,
                 "a quoted executable is refused: the app ID adds the quotes itself");
}

static void test_arguments(void) {
    uint8_t *out = NULL;
    size_t size = 0;
    NkSteamShortcut bad = kPlayer;
    bad.app_name = "";
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size), NK_STEAM_ERR_ARGUMENT, "empty name");
    bad = kPlayer;
    bad.exe = NULL;
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size), NK_STEAM_ERR_ARGUMENT, "no exe");
    bad = kPlayer;
    bad.start_dir = "C:/x\ny";
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size), NK_STEAM_ERR_ARGUMENT,
                 "a control character in a field");
    bad = kPlayer;
    bad.launch_options = "--a\tb";
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size), NK_STEAM_ERR_ARGUMENT,
                 "a tab in the launch options");
    char huge[NK_STEAM_FIELD_MAX + 8u];
    memset(huge, 'x', sizeof(huge) - 1u);
    huge[sizeof(huge) - 1u] = '\0';
    bad = kPlayer;
    bad.app_name = huge;
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size), NK_STEAM_ERR_ARGUMENT,
                 "a name past the cap");
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, NULL, &out, &size), NK_STEAM_ERR_ARGUMENT, "no shortcut");
    /* A non-ASCII name is fine: its bytes are all 0x80 or more. */
    bad = kPlayer;
    bad.app_name = "\xE4\xB8\xAD\xE5\xB7\x9D";
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size), NK_STEAM_OK, "a UTF-8 name");
    nk_steam_free(out);
}

static void test_file_add_and_backup(void) {
    char dir[NATIVE_TEST_PATH_MAX], vdf[NATIVE_TEST_PATH_MAX], backup[NK_MAX_PATH];
    path_in(dir, sizeof(dir), "file-add");
    snprintf(vdf, sizeof(vdf), "%s/config/shortcuts.vdf", dir);
    Raw in;
    two_entries(&in);
    CHECK(nk_platform_mkdir_p(dir), "make the folder");
    char config[NATIVE_TEST_PATH_MAX];
    snprintf(config, sizeof(config), "%s/config", dir);
    CHECK(nk_platform_mkdir_p(config), "make config");
    CHECK(write_bytes(vdf, in.data, in.size), "write the existing file");

    CHECK_STATUS(nk_steam_add_to_file(vdf, &kPlayer, false, "20261106-120000", backup,
                                      sizeof(backup)), NK_STEAM_OK, "add");
    CHECK(strstr(backup, "shortcuts.vdf.nakagawa-backup-20261106-120000") != NULL,
          "the backup is named for the stamp");
    CHECK(file_is(backup, in.data, in.size), "the backup holds the original bytes");
    size_t now = 0;
    uint8_t *after = read_bytes(vdf, &now);
    bool found = false;
    CHECK(after && nk_steam_vdf_has_exe(after, now, kPlayer.exe, &found) == NK_STEAM_OK && found,
          "the file lists the player");
    CHECK(after && memcmp(after, in.data, in.size - 2u) == 0, "earlier entries are intact");
    free(after);
    char temp[NATIVE_TEST_PATH_MAX];
    snprintf(temp, sizeof(temp), "%s.nakagawa-tmp", vdf);
    CHECK(nk_platform_path_missing(temp), "no temporary file is left");

    /* A second entry with the same stamp must not overwrite the first backup. */
    NkSteamShortcut second = {"Second", "C:/Second/s.exe", "C:/Second", ""};
    char backup2[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file(vdf, &second, false, "20261106-120000", backup2,
                                      sizeof(backup2)), NK_STEAM_OK, "add again");
    CHECK(strcmp(backup, backup2) != 0, "a second backup has its own name");
    CHECK(file_is(backup, in.data, in.size), "the first backup is untouched");

    /* The same executable again is reported and writes nothing, no backup either. */
    size_t before_size = 0;
    uint8_t *before = read_bytes(vdf, &before_size);
    char backup3[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file(vdf, &kPlayer, false, "20261106-120001", backup3,
                                      sizeof(backup3)), NK_STEAM_ERR_EXISTS, "existing entry");
    CHECK(backup3[0] == '\0', "no backup for a refused add");
    CHECK(before && file_is(vdf, before, before_size), "the file is unchanged");
    free(before);
}

static void test_running_steam_blocks_every_write(void) {
    char dir[NATIVE_TEST_PATH_MAX], vdf[NATIVE_TEST_PATH_MAX];
    path_in(dir, sizeof(dir), "running");
    snprintf(vdf, sizeof(vdf), "%s/shortcuts.vdf", dir);
    Raw in;
    two_entries(&in);
    CHECK(nk_platform_mkdir_p(dir) && write_bytes(vdf, in.data, in.size), "fixture");
    char backup[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file(vdf, &kPlayer, true, "20261106-120000", backup,
                                      sizeof(backup)), NK_STEAM_ERR_STEAM_RUNNING, "Steam runs");
    CHECK(file_is(vdf, in.data, in.size), "the file is untouched");
    char leftover[NATIVE_TEST_PATH_MAX];
    snprintf(leftover, sizeof(leftover), "%s.nakagawa-backup-20261106-120000", vdf);
    CHECK(nk_platform_path_missing(leftover), "no backup is made either");
}

static void test_corrupt_file_is_left_alone(void) {
    char dir[NATIVE_TEST_PATH_MAX], vdf[NATIVE_TEST_PATH_MAX], leftover[NATIVE_TEST_PATH_MAX];
    path_in(dir, sizeof(dir), "corrupt");
    snprintf(vdf, sizeof(vdf), "%s/shortcuts.vdf", dir);
    static const uint8_t junk[] = "this is not a shortcuts file";
    CHECK(nk_platform_mkdir_p(dir) && write_bytes(vdf, junk, sizeof(junk)), "fixture");
    char backup[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file(vdf, &kPlayer, false, "20261106-120000", backup,
                                      sizeof(backup)), NK_STEAM_ERR_CORRUPT, "corrupt file");
    CHECK(file_is(vdf, junk, sizeof(junk)), "the file is untouched");
    snprintf(leftover, sizeof(leftover), "%s.nakagawa-backup-20261106-120000", vdf);
    CHECK(nk_platform_path_missing(leftover), "no backup of a file that was refused");
    snprintf(leftover, sizeof(leftover), "%s.nakagawa-tmp", vdf);
    CHECK(nk_platform_path_missing(leftover), "no temporary file");
}

static void test_a_failed_backup_writes_nothing(void) {
    char dir[NATIVE_TEST_PATH_MAX], vdf[NATIVE_TEST_PATH_MAX], name[NATIVE_TEST_PATH_MAX];
    path_in(dir, sizeof(dir), "nobackup");
    snprintf(vdf, sizeof(vdf), "%s/shortcuts.vdf", dir);
    Raw in;
    two_entries(&in);
    CHECK(nk_platform_mkdir_p(dir) && write_bytes(vdf, in.data, in.size), "fixture");
    /* Every backup name the code would try already exists. */
    snprintf(name, sizeof(name), "%s.nakagawa-backup-20261106-120000", vdf);
    CHECK(write_bytes(name, in.data, 1u), "first backup name taken");
    for (unsigned i = 2; i <= 100u; i++) {
        snprintf(name, sizeof(name), "%s.nakagawa-backup-20261106-120000-%u", vdf, i);
        CHECK(write_bytes(name, in.data, 1u), "backup name taken");
    }
    char backup[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file(vdf, &kPlayer, false, "20261106-120000", backup,
                                      sizeof(backup)), NK_STEAM_ERR_BACKUP, "no backup possible");
    CHECK(file_is(vdf, in.data, in.size), "the original is untouched");
    snprintf(name, sizeof(name), "%s.nakagawa-backup-20261106-120000", vdf);
    CHECK(file_is(name, in.data, 1u), "an earlier backup is not overwritten");
}

static void test_bad_stamp(void) {
    char backup[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file("unused.vdf", &kPlayer, false, "../x", backup, sizeof(backup)),
                 NK_STEAM_ERR_ARGUMENT, "a stamp with path characters");
    CHECK_STATUS(nk_steam_add_to_file("unused.vdf", &kPlayer, false, "", backup, sizeof(backup)),
                 NK_STEAM_ERR_ARGUMENT, "an empty stamp");
}

static void test_accounts_and_add_for_account(void) {
    char steam[NATIVE_TEST_PATH_MAX], sub[NATIVE_TEST_PATH_MAX];
    path_in(steam, sizeof(steam), "Steam");
    const char *dirs[] = {"0", "30001", "4", "abc", "12x", "20002"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(sub, sizeof(sub), "%s/userdata/%s", steam, dirs[i]);
        CHECK(nk_platform_mkdir_p(sub), "make an account folder");
    }
    snprintf(sub, sizeof(sub), "%s/userdata/777", steam);
    CHECK(write_bytes(sub, (const uint8_t *)"f", 1u), "a plain file named like an account");

    NkSteamAccount accounts[NK_STEAM_ACCOUNT_CAP];
    size_t count = 0;
    CHECK_STATUS(nk_steam_list_accounts(steam, accounts, NK_STEAM_ACCOUNT_CAP, &count), NK_STEAM_OK,
                 "list accounts");
    CHECK(count == 3u, "only the digit folders other than 0 are accounts");
    if (count == 3u) {
        CHECK(strcmp(accounts[0].id, "4") == 0 && strcmp(accounts[1].id, "20002") == 0 &&
              strcmp(accounts[2].id, "30001") == 0, "ascending by number");
        char want[NK_MAX_PATH];
        char sep = nk_platform_path_separator();
        snprintf(want, sizeof(want), "userdata%c20002%cconfig%cshortcuts.vdf", sep, sep, sep);
        CHECK(strstr(accounts[1].shortcuts_path, want) != NULL,
              "the shortcuts path is under the account's config folder");
    }
    size_t one = 0;
    CHECK_STATUS(nk_steam_list_accounts(steam, accounts, 2u, &one), NK_STEAM_OK, "a small cap");
    CHECK(one <= 2u, "never more than the cap");

    char missing[NATIVE_TEST_PATH_MAX];
    path_in(missing, sizeof(missing), "no-steam-here");
    CHECK_STATUS(nk_steam_list_accounts(missing, accounts, NK_STEAM_ACCOUNT_CAP, &count),
                 NK_STEAM_ERR_NOT_FOUND, "no Steam folder");

    char backup[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_for_account(steam, "20002", &kPlayer, is_running_yes,
                                          "20261106-120000", backup, sizeof(backup)),
                 NK_STEAM_ERR_STEAM_RUNNING, "Steam runs");
    CHECK_STATUS(nk_steam_add_for_account(steam, "..", &kPlayer, is_running_no, "20261106-120000",
                                          backup, sizeof(backup)), NK_STEAM_ERR_ARGUMENT,
                 "a path is not an account");
    CHECK_STATUS(nk_steam_add_for_account(steam, "0", &kPlayer, is_running_no, "20261106-120000",
                                          backup, sizeof(backup)), NK_STEAM_ERR_ARGUMENT,
                 "account 0 is refused");
    CHECK_STATUS(nk_steam_add_for_account(steam, "999", &kPlayer, is_running_no, "20261106-120000",
                                          backup, sizeof(backup)), NK_STEAM_ERR_NOT_FOUND,
                 "an account that is not there");
    CHECK_STATUS(nk_steam_add_for_account(steam, "20002", &kPlayer, is_running_no,
                                          "20261106-120000", backup, sizeof(backup)), NK_STEAM_OK,
                 "an account with no shortcuts file yet gets one");
    CHECK(backup[0] == '\0', "a new file has nothing to back up");
    char vdf[NATIVE_TEST_PATH_MAX];
    snprintf(vdf, sizeof(vdf), "%s/userdata/20002/config/shortcuts.vdf", steam);
    size_t size = 0;
    uint8_t *data = read_bytes(vdf, &size);
    bool found = false;
    CHECK(data && nk_steam_vdf_has_exe(data, size, kPlayer.exe, &found) == NK_STEAM_OK && found,
          "the file was made and lists the player");
    free(data);
    snprintf(vdf, sizeof(vdf), "%s/userdata/30001/config/shortcuts.vdf", steam);
    CHECK(nk_platform_path_missing(vdf), "no other account was touched");
#ifndef _WIN32
    CHECK_STATUS(nk_steam_add_for_account(steam, "30001", &kPlayer, NULL, "20261106-120000",
                                          backup, sizeof(backup)), NK_STEAM_ERR_UNSUPPORTED,
                 "off Windows, without a probe of its own, nothing is written");
    char root[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_find_root(root, sizeof(root)), NK_STEAM_ERR_UNSUPPORTED,
                 "off Windows, Steam is not found by itself");
#endif
}

int main(void) {
    native_test_create_root("steam_shortcuts");
    snprintf(g_root, sizeof(g_root), "%s", native_test_get_root());
    test_app_id();
    test_new_file();
    test_existing_entries_are_preserved();
    test_three_adds_number_in_order();
    test_existing_executable_is_reported_not_added();
    test_no_shortcuts_map_gets_one();
    test_corrupt_files_are_refused();
    test_arguments();
    test_steam_running_fails_closed();
    test_file_add_and_backup();
    test_running_steam_blocks_every_write();
    test_corrupt_file_is_left_alone();
    test_a_failed_backup_writes_nothing();
    test_bad_stamp();
    test_accounts_and_add_for_account();
    if (g_failures) {
        fprintf(stderr, "test_steam_shortcuts: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_steam_shortcuts: ALL %d CHECKS PASSED\n", g_checks);
    return 0;
}
