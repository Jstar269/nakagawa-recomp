/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Player library state: entry lookup and honest add results.
 *
 * Two defects lived here:
 *
 *   - nk_library_add_or_update updates an existing record IN PLACE, so the
 *     entry just written is not necessarily the last one. --launch-now took
 *     app.game_count - 1 and could therefore prepare and launch a different
 *     game while the console reported the requested ISO;
 *   - player_app_add_game discarded both the insert and the save result and
 *     returned true unconditionally, so a rejected insert or an unwritable
 *     user-data directory still reported success and the entry vanished on
 *     the next start.
 *
 * These link player_state.c directly. Nothing here touches SDL, and nothing
 * here writes to the user's real library: the failure path returns before the
 * save, which is the whole point of the assertion.
 */

/* Feature-test macro first: POSIX tests use setenv, fchmod, and fileno. */
#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include "player_state.h"
#include "iso_reader.h"
#include "nk_font.h"
#include "nk_title_manifest.h"
#include "nk_platform.h"
#include "native_test_isolation.h"
#include "nk_psp_builtin_keys.h"
#include "nk_psp_keystore.h"
#include "psp_seal_fixture.h"
#include "pgf_golden_vectors.h"
#include "../../src/rt/recomp.h"

/* The per-run root setup and its recursive cleanup run inside assert(); keep
 * them even in a build that defines NDEBUG (#735). */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* Scratch root for the files this test writes: the checkout's build/ by default, or
 * the BUILD_ROOT the Makefile passes as -DSR_SELFTEST_BUILD_ROOT, so a scratch run never
 * touches the checkout. */
#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#include <process.h>
#include <windows.h>
#include <wchar.h>
#define test_rmdir _rmdir
#define nk_ps_chdir _chdir
#define nk_ps_getcwd _getcwd
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define test_rmdir rmdir
#define nk_ps_chdir chdir
#define nk_ps_getcwd getcwd
#endif

/* Set (value non-NULL) or clear (value NULL) a process environment variable
 * for the duration of a probe, restored by the caller. */
static void nk_ps_set_env(const char *key, const char *value) {
    size_t len = strlen(key) + (value ? strlen(value) : 0) + 2;
    char *pair = (char *)malloc(len);
    assert(pair != NULL);
    if (value) snprintf(pair, len, "%s=%s", key, value);
    else snprintf(pair, len, "%s=", key);
#if defined(_WIN32) || defined(_WIN64)
    /* _putenv may retain the pointer, so the string outlives this call. */
    _putenv(pair);
#else
    if (value) setenv(key, value, 1); /* setenv copies */
    else unsetenv(key);
    free(pair);
#endif
}

static void count_gui_foreground_request(void *context) {
    int *count = (int *)context;
    ++*count;
}

/* Copy an environment value out before any mutation invalidates getenv's pointer. */
static void nk_ps_copy_env(const char *key, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    const char *live = getenv(key);
    if (live && live[0]) snprintf(out, out_size, "%s", live);
}

static void seed_entry(NkGameEntry *entry, const char *disc_id, const char *name) {
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->disc_id, sizeof(entry->disc_id), "%s", disc_id);
    snprintf(entry->title_name, sizeof(entry->title_name), "%s", name);
    snprintf(entry->disc_version, sizeof(entry->disc_version), "1.00");
    entry->status = NK_STATUS_IDENTIFIED;
}

static bool test_cancel_always(void *user) {
    (void)user;
    return true;
}

static void write_file(const char *path) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite("fixture", 1, 7, file) == 7);
    assert(fclose(file) == 0);
}

static void put_iso_both32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
    out[4] = (uint8_t)(value >> 24);
    out[5] = (uint8_t)(value >> 16);
    out[6] = (uint8_t)(value >> 8);
    out[7] = (uint8_t)value;
}

static void write_iso_record(uint8_t *sector, size_t offset,
                             const uint8_t *name, size_t name_size,
                             uint32_t lba, uint32_t size, bool is_directory) {
    size_t record_size = 33u + name_size;
    if (record_size & 1u) record_size++;
    memset(sector + offset, 0, record_size);
    sector[offset] = (uint8_t)record_size;
    put_iso_both32(sector + offset + 2, lba);
    put_iso_both32(sector + offset + 10, size);
    sector[offset + 25] = is_directory ? 0x02u : 0u;
    sector[offset + 32] = (uint8_t)name_size;
    memcpy(sector + offset + 33, name, name_size);
}

static void write_iso_with_fixture_eboot(const char *path) {
    enum { SECTOR_SIZE = 2048, ISO_SECTORS = 21 };
    static const uint8_t dot[] = {0};
    static const uint8_t dotdot[] = {1};
    static const uint8_t game[] = "PSP_GAME";
    static const uint8_t sysdir[] = "SYSDIR";
    static const uint8_t eboot[] = "EBOOT.BIN;1";
    uint8_t image[ISO_SECTORS * SECTOR_SIZE];
    memset(image, 0, sizeof(image));

    uint8_t *pvd = image + 16 * SECTOR_SIZE;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    put_iso_both32(pvd + 158, 17);
    put_iso_both32(pvd + 166, SECTOR_SIZE);
    write_iso_record(pvd, 156, dot, sizeof(dot), 17, SECTOR_SIZE, true);

    uint8_t *root = image + 17 * SECTOR_SIZE;
    write_iso_record(root, 0, dot, sizeof(dot), 17, SECTOR_SIZE, true);
    write_iso_record(root, 34, dotdot, sizeof(dotdot), 17, SECTOR_SIZE, true);
    write_iso_record(root, 68, game, sizeof(game) - 1, 18, SECTOR_SIZE, true);

    uint8_t *game_dir = image + 18 * SECTOR_SIZE;
    write_iso_record(game_dir, 0, dot, sizeof(dot), 18, SECTOR_SIZE, true);
    write_iso_record(game_dir, 34, dotdot, sizeof(dotdot), 17, SECTOR_SIZE, true);
    write_iso_record(game_dir, 68, sysdir, sizeof(sysdir) - 1, 19,
                     SECTOR_SIZE, true);

    uint8_t *sysdir_dir = image + 19 * SECTOR_SIZE;
    write_iso_record(sysdir_dir, 0, dot, sizeof(dot), 19, SECTOR_SIZE, true);
    write_iso_record(sysdir_dir, 34, dotdot, sizeof(dotdot), 18, SECTOR_SIZE, true);
    write_iso_record(sysdir_dir, 68, eboot, sizeof(eboot) - 1, 20, 7, false);
    memcpy(image + 20 * SECTOR_SIZE, "fixture", 7);

    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(image, 1, sizeof(image), file) == sizeof(image));
    assert(fclose(file) == 0);
}

static void write_binary_file(const char *path, const unsigned char *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(bytes, 1, size, file) == size);
    assert(fclose(file) == 0);
}

/* The old key-file override, left set to an existing file for the whole run
 * so the preflight checks can prove it is ignored. */
static char g_decoy_key_path[NATIVE_TEST_PATH_MAX + 32];

static void write_text_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    size_t length = strlen(text);
    assert(fwrite(text, 1, length, file) == length);
    assert(fclose(file) == 0);
}

#define TEST_ISO_SECTOR_BYTES 2048u
#define TEST_ISO_MAX_NODES 2048u
#define TEST_ISO_MODULE_BYTES 128u

typedef struct {
    char name[256];
    size_t parent;
    uint32_t lba;
    uint32_t size;
    bool is_directory;
} TestIsoNode;

static void test_iso_put_both32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
        out[i] = (uint8_t)(value >> (i * 8u));
        out[4u + i] = (uint8_t)(value >> ((3u - i) * 8u));
    }
}

static size_t test_iso_record_size(size_t name_size) {
    size_t record_size = 33u + name_size;
    return record_size + (record_size & 1u);
}

static size_t test_iso_record_end(size_t offset, size_t name_size) {
    size_t record_size = test_iso_record_size(name_size);
    size_t sector_offset = offset % TEST_ISO_SECTOR_BYTES;
    if (sector_offset + record_size > TEST_ISO_SECTOR_BYTES) {
        offset += TEST_ISO_SECTOR_BYTES - sector_offset;
    }
    return offset + record_size;
}

static size_t test_iso_write_record(uint8_t *directory, size_t offset,
                                    const uint8_t *name, size_t name_size,
                                    uint32_t lba, uint32_t size,
                                    bool is_directory) {
    size_t record_size = test_iso_record_size(name_size);
    assert(record_size <= UINT8_MAX);
    memset(directory + offset, 0, record_size);
    directory[offset] = (uint8_t)record_size;
    test_iso_put_both32(directory + offset + 2, lba);
    test_iso_put_both32(directory + offset + 10, size);
    directory[offset + 25] = is_directory ? 0x02u : 0u;
    directory[offset + 28] = 1;
    directory[offset + 31] = 1;
    directory[offset + 32] = (uint8_t)name_size;
    memcpy(directory + offset + 33, name, name_size);
    return record_size;
}

static size_t test_iso_add_node(TestIsoNode *nodes, size_t *node_count,
                                size_t parent, const char *name,
                                bool is_directory) {
    assert(*node_count < TEST_ISO_MAX_NODES);
    assert(strlen(name) < sizeof(nodes[*node_count].name));
    size_t index = (*node_count)++;
    memset(&nodes[index], 0, sizeof(nodes[index]));
    snprintf(nodes[index].name, sizeof(nodes[index].name), "%s", name);
    nodes[index].parent = parent;
    nodes[index].is_directory = is_directory;
    return index;
}

static void write_module_scan_iso(const char *path, size_t candidate_count,
                                  bool duplicate_name, size_t child_directories,
                                  bool malformed_usrdir) {
    TestIsoNode *nodes = (TestIsoNode *)calloc(TEST_ISO_MAX_NODES,
                                                sizeof(*nodes));
    assert(nodes != NULL);
    size_t node_count = 1;
    nodes[0].is_directory = true;
    nodes[0].parent = 0;
    size_t psp_game = test_iso_add_node(nodes, &node_count, 0, "PSP_GAME", true);
    size_t sysdir = test_iso_add_node(nodes, &node_count, psp_game, "SYSDIR", true);
    size_t usrdir = test_iso_add_node(nodes, &node_count, psp_game, "USRDIR", true);
    if (duplicate_name) {
        test_iso_add_node(nodes, &node_count, sysdir, "shared.prx", false);
        test_iso_add_node(nodes, &node_count, usrdir, "SHARED.PRX", false);
    }
    for (size_t i = 0; i < candidate_count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "module-%03lu.prx", (unsigned long)i);
        test_iso_add_node(nodes, &node_count, usrdir, name, false);
    }
    for (size_t i = 0; i < child_directories; i++) {
        char name[32];
        snprintf(name, sizeof(name), "directory-%04lu", (unsigned long)i);
        test_iso_add_node(nodes, &node_count, usrdir, name, true);
    }

    for (size_t i = 0; i < node_count; i++) {
        if (!nodes[i].is_directory) continue;
        size_t bytes = test_iso_record_end(0, 1);
        bytes = test_iso_record_end(bytes, 1);
        for (size_t child = 1; child < node_count; child++) {
            if (nodes[child].parent != i) continue;
            size_t name_size = strlen(nodes[child].name) +
                               (nodes[child].is_directory ? 0u : 2u);
            bytes = test_iso_record_end(bytes, name_size);
        }
        size_t sectors = (bytes + TEST_ISO_SECTOR_BYTES - 1u) /
                         TEST_ISO_SECTOR_BYTES;
        nodes[i].size = (uint32_t)(sectors * TEST_ISO_SECTOR_BYTES);
    }
    uint32_t next_lba = 33;
    for (size_t i = 0; i < node_count; i++) {
        if (!nodes[i].is_directory) continue;
        nodes[i].lba = next_lba;
        next_lba += nodes[i].size / TEST_ISO_SECTOR_BYTES;
    }
    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i].is_directory) continue;
        nodes[i].lba = next_lba++;
        nodes[i].size = TEST_ISO_MODULE_BYTES;
    }
    size_t image_size = (size_t)next_lba * TEST_ISO_SECTOR_BYTES;
    uint8_t *image = (uint8_t *)calloc(1, image_size);
    assert(image != NULL);

    uint8_t *pvd = image + 16u * TEST_ISO_SECTOR_BYTES;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    (void)test_iso_write_record(pvd, 156, (const uint8_t *)"\0", 1,
                                nodes[0].lba, nodes[0].size, true);

    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i].is_directory) {
            uint8_t *directory = image + (size_t)nodes[i].lba * TEST_ISO_SECTOR_BYTES;
            size_t offset = 0;
            const uint8_t self_name[] = { 0 };
            const uint8_t parent_name[] = { 1 };
            size_t parent = i == 0 ? 0 : nodes[i].parent;
            offset += test_iso_write_record(directory, offset, self_name, 1,
                                            nodes[i].lba, nodes[i].size, true);
            offset += test_iso_write_record(directory, offset, parent_name, 1,
                                            nodes[parent].lba, nodes[parent].size,
                                            true);
            for (size_t child = 1; child < node_count; child++) {
                if (nodes[child].parent != i) continue;
                char versioned_name[260];
                const char *entry_name = nodes[child].name;
                if (!nodes[child].is_directory) {
                    int written = snprintf(versioned_name, sizeof(versioned_name),
                                           "%s;1", entry_name);
                    assert(written > 0 && (size_t)written < sizeof(versioned_name));
                    entry_name = versioned_name;
                }
                size_t record_size = test_iso_record_size(strlen(entry_name));
                size_t sector_offset = offset % TEST_ISO_SECTOR_BYTES;
                if (sector_offset + record_size > TEST_ISO_SECTOR_BYTES) {
                    offset += TEST_ISO_SECTOR_BYTES - sector_offset;
                }
                offset += test_iso_write_record(
                    directory, offset, (const uint8_t *)entry_name,
                    strlen(entry_name), nodes[child].lba, nodes[child].size,
                    nodes[child].is_directory);
            }
            if (i == usrdir && malformed_usrdir) {
                assert(offset < nodes[i].size);
                directory[offset] = 1;
            }
        } else {
            uint8_t *module = image + (size_t)nodes[i].lba * TEST_ISO_SECTOR_BYTES;
            memcpy(module, "~PSP", 4);
            module[0x27] = 1;
            module[0x54] = 0x40;
        }
    }

    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(image, 1, image_size, file) == image_size);
    assert(fclose(file) == 0);
    free(image);
    free(nodes);
}

static bool test_remove_tree(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    char pattern[1024];
    WIN32_FIND_DATAA data;
    int written = snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if (written < 0 || (size_t)written >= sizeof(pattern)) return false;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
                continue;
            }
            char child[1024];
            written = snprintf(child, sizeof(child), "%s\\%s", path,
                               data.cFileName);
            if (written < 0 || (size_t)written >= sizeof(child)) {
                FindClose(find);
                return false;
            }
            bool removed;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                removed = test_remove_tree(child);
            } else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                removed = RemoveDirectoryA(child) != 0;
            } else {
                removed = DeleteFileA(child) != 0;
            }
            if (!removed) {
                FindClose(find);
                return false;
            }
        } while (FindNextFileA(find, &data));
        FindClose(find);
    }
    /* A missing tree counts as removed, as the POSIX branch's ENOENT does: a missing leaf is
       ERROR_FILE_NOT_FOUND, a missing parent ERROR_PATH_NOT_FOUND. */
    if (RemoveDirectoryA(path) != 0) return true;
    {
        DWORD error = GetLastError();
        return error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND;
    }
#else
    DIR *directory = opendir(path);
    if (directory == NULL) return errno == ENOENT;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[1024];
        int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child)) {
            ok = false;
            break;
        }
        /* Act first, then classify the failure: no stat precedes the removal. */
        if (unlink(child) != 0) {
            bool removed = (errno == EISDIR || errno == EPERM) ? test_remove_tree(child)
                                                                : errno == ENOENT;
            if (!removed) {
                ok = false;
                break;
            }
        }
    }
    closedir(directory);
    return ok && (rmdir(path) == 0 || errno == ENOENT);
#endif
}

static void append_json_whitespace(const char *path, size_t count) {
    FILE *file = fopen(path, "ab");
    assert(file != NULL);
    char spaces[4096];
    memset(spaces, ' ', sizeof(spaces));
    while (count > 0) {
        size_t chunk = count < sizeof(spaces) ? count : sizeof(spaces);
        assert(fwrite(spaces, 1, chunk, file) == chunk);
        count -= chunk;
    }
    assert(fclose(file) == 0);
}

/* Bytes on disk. The package identity the validator compares is (size, write
 * timestamp, platform change value) per file, so a mutation whose size
 * provably changes is a guaranteed identity change on every host, while a
 * same-size rewrite is visible when the platform change value advances
 * (#683) - see rewrite_preserving_size_and_mtime below. */
static long fixture_file_size(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0);
    assert(fclose(file) == 0);
    return size;
}

/* Match the platform change value used by the package cache identity. Some
 * filesystems coalesce consecutive writes into the same change-time tick. */
static uint64_t fixture_change_value(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide, 32768) <= 0) {
        return 0;
    }
    HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    FILE_BASIC_INFO basic;
    memset(&basic, 0, sizeof(basic));
    BOOL ok = GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                           sizeof(basic));
    CloseHandle(handle);
    return ok ? (uint64_t)basic.ChangeTime.QuadPart : 0;
#else
    struct stat info;
    if (stat(path, &info) != 0) return 0;
    return (uint64_t)info.st_ctim.tv_sec * 1000000000ull +
           (uint64_t)info.st_ctim.tv_nsec;
#endif
}

/* Rewrite *path* with *bytes* and put the write timestamp back to the value the
 * file had on entry, so the (size, write timestamp) half of the package
 * identity is provably unchanged and only the platform change value can
 * differ. *bytes* must have exactly the current length of the file. Returns
 * false when the host cannot restore the timestamp exactly, which the caller
 * asserts rather than tolerating: the point of the probe is that the bytes on
 * disk really are indistinguishable in size and write time. */
static bool rewrite_preserving_size_and_mtime(const char *path, const char *bytes) {
    assert(strlen(bytes) == (size_t)fixture_file_size(path));
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide, 32768) <= 0) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA before;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &before)) return false;
    write_text_file(path, bytes);
    HANDLE handle = CreateFileW(wide, FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BOOL restored = SetFileTime(handle, NULL, NULL, &before.ftLastWriteTime);
    CloseHandle(handle);
    if (!restored) return false;
    WIN32_FILE_ATTRIBUTE_DATA after;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &after)) return false;
    return after.ftLastWriteTime.dwLowDateTime ==
               before.ftLastWriteTime.dwLowDateTime &&
           after.ftLastWriteTime.dwHighDateTime ==
               before.ftLastWriteTime.dwHighDateTime;
#else
    struct stat before;
    if (stat(path, &before) != 0) return false;
    write_text_file(path, bytes);
    struct timespec times[2];
    times[0] = before.st_atim;
    times[1] = before.st_mtim;
    if (utimensat(AT_FDCWD, path, times, 0) != 0) return false;
    struct stat after;
    if (stat(path, &after) != 0) return false;
    return after.st_mtim.tv_sec == before.st_mtim.tv_sec &&
           after.st_mtim.tv_nsec == before.st_mtim.tv_nsec;
#endif
}

/* Seed a fixture executable with real bytes (a self-copy of this test binary
 * standing in for a recompiled runtime). The stub path keeps writing the
 * historical "fixture" payload, whose digest is FIXTURE_SHA256 below. */
static void copy_executable_file(const char *source_path,
                                 const char *destination_path) {
    FILE *source = fopen(source_path, "rb");
    FILE *destination = fopen(destination_path, "wb");
    assert(source != NULL);
    assert(destination != NULL);
    uint8_t buffer[16384];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), source)) != 0) {
        assert(fwrite(buffer, 1, count, destination) == count);
    }
    assert(!ferror(source));
#if !defined(_WIN32) && !defined(_WIN64)
    assert(fchmod(fileno(destination), S_IRUSR | S_IWUSR | S_IXUSR) == 0);
#endif
    assert(fclose(destination) == 0);
    assert(fclose(source) == 0);
}

static void set_environment_value(const char *name, const char *value) {
#if defined(_WIN32) || defined(_WIN64)
    assert(_putenv_s(name, value) == 0);
#else
    assert(setenv(name, value, 1) == 0);
#endif
}

static void restore_environment_value(const char *name, const char *value,
                                      bool was_set) {
#if defined(_WIN32) || defined(_WIN64)
    assert(_putenv_s(name, was_set ? value : "") == 0);
#else
    if (was_set) assert(setenv(name, value, 1) == 0);
    else assert(unsetenv(name) == 0);
#endif
}

static char *capture_environment_value(const char *name, bool *was_set) {
    const char *current = getenv(name);
    *was_set = current != NULL;
    if (!current) return NULL;
    size_t length = strlen(current);
    char *copy = (char *)malloc(length + 1);
    assert(copy != NULL);
    memcpy(copy, current, length + 1);
    return copy;
}

static bool path_prefix_equal(const char *a, const char *b, size_t n) {
#if defined(_WIN32) || defined(_WIN64)
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (tolower(ca) != tolower(cb)) return false;
    }
    return true;
#else
    return strncmp(a, b, n) == 0;
#endif
}

static bool path_is_within(const char *child, const char *parent) {
    size_t parent_length = strlen(parent);
    if (!path_prefix_equal(child, parent, parent_length)) return false;
    return child[parent_length] == '\0' || child[parent_length] == '/' ||
           child[parent_length] == '\\';
}

static bool get_working_directory(char *out, size_t size) {
#if defined(_WIN32) || defined(_WIN64)
    return _getcwd(out, (int)size) != NULL;
#else
    return getcwd(out, size) != NULL;
#endif
}

/* Repository cleanliness gate (#511): the launch runs must not change any
 * repository-tracked file. Uses the shell's redirection because the platform
 * spawn API offers no stdout capture; reports false when git is unavailable
 * so the caller can print an explicit SKIP rather than a silent pass. */
static bool git_status_snapshot(const char *out_path) {
    char command[1200];
    int written = snprintf(command, sizeof(command),
                           "git status --porcelain > \"%s\" 2>&1", out_path);
    if (written <= 0 || (size_t)written >= sizeof(command)) return false;
    return system(command) == 0;
}

/* Reads a small file whole into out. Returns its length, or -1 when it cannot be read or
 * does not fit. */
static long read_small_file(const char *path, char *out, size_t max) {
    FILE *file = fopen(path, "rb");
    size_t length;
    bool failed;
    if (!file) return -1;
    length = fread(out, 1, max, file);
    failed = ferror(file) != 0 || length == max;
    fclose(file);
    return failed ? -1 : (long)length;
}

static bool files_identical(const char *left, const char *right) {
    FILE *a = fopen(left, "rb");
    FILE *b = fopen(right, "rb");
    if (!a || !b) {
        if (a) fclose(a);
        if (b) fclose(b);
        return false;
    }
    bool equal = true;
    for (;;) {
        int ca = fgetc(a);
        int cb = fgetc(b);
        if (ca != cb) {
            equal = false;
            break;
        }
        if (ca == EOF) break;
    }
    fclose(a);
    fclose(b);
    return equal;
}

typedef struct {
    char memstick[1024];
    bool marker_present;
    bool valid;
} RepeatLaunchReport;

static RepeatLaunchReport read_repeat_launch_report(const char *path) {
    RepeatLaunchReport report;
    memset(&report, 0, sizeof(report));
    FILE *file = fopen(path, "rb");
    if (!file) return report;
    char line[2048];
    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        if (strncmp(line, "SR_MEMSTICK=", 12) == 0) {
            snprintf(report.memstick, sizeof(report.memstick), "%s", line + 12);
        } else if (strcmp(line, "MARKER_PRESENT=1") == 0) {
            report.marker_present = true;
            report.valid = true;
        } else if (strcmp(line, "MARKER_PRESENT=0") == 0) {
            report.marker_present = false;
            report.valid = true;
        }
    }
    fclose(file);
    return report;
}

static void assert_session_released(const NkLaunchSession *session) {
    /* nk_platform_close_process ran: nothing from the finished child is
       still held by the session (a leaked Win32 process/job handle would
       survive here across relaunches). */
    assert(session->process.native_handle == NULL);
    assert(session->process.job_handle == NULL);
    assert(session->process.process_id == 0);
    assert(session->process.is_active == false);
}

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} FixtureSha256;

static uint32_t fixture_rotr(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

static void fixture_sha_transform(FixtureSha256 *ctx, const uint8_t block[64]) {
    static const uint32_t round[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t words[64];
    for (size_t i = 0; i < 16; i++) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   (uint32_t)block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = fixture_rotr(words[i - 15], 7) ^ fixture_rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 = fixture_rotr(words[i - 2], 17) ^ fixture_rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = fixture_rotr(e, 6) ^ fixture_rotr(e, 11) ^ fixture_rotr(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + choose + round[i] + words[i];
        uint32_t s0 = fixture_rotr(a, 2) ^ fixture_rotr(a, 13) ^ fixture_rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void fixture_sha_init(FixtureSha256 *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bits = 0;
    ctx->used = 0;
}

static void fixture_sha_update(FixtureSha256 *ctx, const uint8_t *data, size_t length) {
    ctx->bits += (uint64_t)length * 8u;
    while (length > 0) {
        size_t room = sizeof(ctx->block) - ctx->used;
        size_t take = length < room ? length : room;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        data += take;
        length -= take;
        if (ctx->used == sizeof(ctx->block)) {
            fixture_sha_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void fixture_sha_finish(FixtureSha256 *ctx, char out[65]) {
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        memset(ctx->block + ctx->used, 0, sizeof(ctx->block) - ctx->used);
        fixture_sha_transform(ctx, ctx->block);
        ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, 56 - ctx->used);
    for (size_t i = 0; i < 8; i++) {
        ctx->block[63 - i] = (uint8_t)(ctx->bits >> (8u * i));
    }
    fixture_sha_transform(ctx, ctx->block);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(ctx->state) / sizeof(ctx->state[0]); i++) {
        for (size_t j = 0; j < 4; j++) {
            uint8_t byte = (uint8_t)(ctx->state[i] >> (24u - 8u * j));
            out[i * 8 + j * 2] = hex[byte >> 4];
            out[i * 8 + j * 2 + 1] = hex[byte & 0x0f];
        }
    }
    out[64] = '\0';
}

static void fixture_sha_bytes(const void *data, size_t length, char out[65]) {
    FixtureSha256 ctx;
    fixture_sha_init(&ctx);
    fixture_sha_update(&ctx, (const uint8_t *)data, length);
    fixture_sha_finish(&ctx, out);
}

static void fixture_sha_file(const char *path, char out[65]) {
    FixtureSha256 ctx;
    fixture_sha_init(&ctx);
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    uint8_t buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        fixture_sha_update(&ctx, buffer, count);
    }
    assert(!ferror(file));
    assert(fclose(file) == 0);
    fixture_sha_finish(&ctx, out);
}

static void build_synthetic_mips_elf(unsigned char elf[88]) {
    memset(elf, 0, 88);
    memcpy(elf, "\x7f" "ELF", 4);
    elf[4] = 1; /* ELF32 */
    elf[5] = 1; /* little-endian */
    elf[6] = 1; /* current ELF version */
    elf[16] = 2; /* executable */
    elf[18] = 8; /* MIPS */
    elf[20] = 1;
    elf[24] = 0x00; elf[25] = 0x00; elf[26] = 0x80; elf[27] = 0x08;
    elf[28] = 52; /* program-header table offset */
    elf[40] = 52; /* ELF header size */
    elf[42] = 32; /* program-header entry size */
    elf[44] = 1;  /* program-header count */
    elf[52] = 1;  /* PT_LOAD */
    elf[56] = 84; /* file offset */
    elf[60] = 0x00; elf[61] = 0x00; elf[62] = 0x80; elf[63] = 0x08;
    elf[64] = 0x00; elf[65] = 0x00; elf[66] = 0x80; elf[67] = 0x08;
    elf[68] = 4;  /* file size */
    elf[72] = 4;  /* memory size */
    elf[76] = 5;  /* readable + executable */
    elf[80] = 4;  /* alignment */
    elf[84] = 0x34; elf[85] = 0x12;
}

static void write_synthetic_mips_elf(const char *path) {
    unsigned char elf[88];
    build_synthetic_mips_elf(elf);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(elf, 1, sizeof(elf), file) == sizeof(elf));
    assert(fclose(file) == 0);
}

static void test_put_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void test_put_le32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static size_t build_player_test_param_sfo(uint8_t *out, size_t out_size,
                                          const char *disc_id,
                                          const char *title) {
    static const char *const keys[] = { "DISC_ID", "DISC_VERSION", "TITLE" };
    const char *const values[] = { disc_id, "1.00", title };
    uint8_t key_table[64] = { 0 };
    uint8_t data_table[256] = { 0 };
    size_t key_offsets[3];
    size_t data_offsets[3];
    size_t value_lengths[3];
    size_t key_bytes = 0;
    size_t data_bytes = 0;
    const size_t key_table_start = 20u + 3u * 16u;
    size_t data_table_start;

    for (size_t i = 0; i < 3; ++i) {
        size_t key_length = strlen(keys[i]) + 1u;
        size_t value_length = strlen(values[i]) + 1u;
        assert(key_bytes + key_length <= sizeof(key_table));
        assert(data_bytes + value_length <= sizeof(data_table));
        key_offsets[i] = key_bytes;
        memcpy(key_table + key_bytes, keys[i], key_length);
        key_bytes += key_length;
        data_offsets[i] = data_bytes;
        value_lengths[i] = value_length;
        memcpy(data_table + data_bytes, values[i], value_length);
        data_bytes = (data_bytes + value_length + 3u) & ~(size_t)3u;
    }
    data_table_start = (key_table_start + key_bytes + 3u) & ~(size_t)3u;
    assert(data_table_start + data_bytes <= out_size);
    memset(out, 0, data_table_start + data_bytes);
    out[0] = 0;
    memcpy(out + 1, "PSF", 3);
    out[4] = 1;
    out[5] = 1;
    test_put_le32(out + 8, (uint32_t)key_table_start);
    test_put_le32(out + 12, (uint32_t)data_table_start);
    test_put_le32(out + 16, 3u);
    for (size_t i = 0; i < 3; ++i) {
        uint8_t *entry = out + 20u + i * 16u;
        test_put_le16(entry, (uint16_t)key_offsets[i]);
        test_put_le16(entry + 2, 0x0204u);
        test_put_le32(entry + 4, (uint32_t)value_lengths[i]);
        test_put_le32(entry + 8, (uint32_t)value_lengths[i]);
        test_put_le32(entry + 12, (uint32_t)data_offsets[i]);
    }
    memcpy(out + key_table_start, key_table, key_bytes);
    memcpy(out + data_table_start, data_table, data_bytes);
    return data_table_start + data_bytes;
}

static void write_player_package_preflight_iso(const char *path,
                                               const char *disc_id,
                                               const char *title,
                                               bool encrypted_eboot) {
    enum { SECTOR = 2048, SECTORS = 23, ROOT_LBA = 17, GAME_LBA = 18,
           SYSDIR_LBA = 19, USRDIR_LBA = 20, SFO_LBA = 21, EBOOT_LBA = 22 };
    static const uint8_t dot[] = { 0 };
    static const uint8_t dotdot[] = { 1 };
    static const uint8_t game_name[] = "PSP_GAME";
    static const uint8_t sysdir_name[] = "SYSDIR";
    static const uint8_t usrdir_name[] = "USRDIR";
    static const uint8_t sfo_name[] = "PARAM.SFO;1";
    static const uint8_t eboot_name[] = "EBOOT.BIN;1";
    uint8_t image[SECTORS * SECTOR] = { 0 };
    uint8_t sfo[256];
    uint8_t eboot[0x100];
    size_t eboot_size;
    size_t sfo_size = build_player_test_param_sfo(
        sfo, sizeof(sfo), disc_id, title);
    uint8_t *pvd = image + 16u * SECTOR;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    put_iso_both32(pvd + 80, SECTORS);
    put_iso_both32(pvd + 158, ROOT_LBA);
    put_iso_both32(pvd + 166, SECTOR);

    uint8_t *root = image + ROOT_LBA * SECTOR;
    write_iso_record(root, 0, dot, sizeof(dot), ROOT_LBA, SECTOR, true);
    write_iso_record(root, 34, dotdot, sizeof(dotdot), ROOT_LBA, SECTOR, true);
    write_iso_record(root, 68, game_name, sizeof(game_name) - 1u,
                     GAME_LBA, SECTOR, true);

    uint8_t *game = image + GAME_LBA * SECTOR;
    write_iso_record(game, 0, dot, sizeof(dot), GAME_LBA, SECTOR, true);
    write_iso_record(game, 34, dotdot, sizeof(dotdot), ROOT_LBA, SECTOR, true);
    write_iso_record(game, 68, sfo_name, sizeof(sfo_name) - 1u,
                     SFO_LBA, (uint32_t)sfo_size, false);
    write_iso_record(game, 112, sysdir_name, sizeof(sysdir_name) - 1u,
                     SYSDIR_LBA, SECTOR, true);
    write_iso_record(game, 152, usrdir_name, sizeof(usrdir_name) - 1u,
                     USRDIR_LBA, SECTOR, true);

    uint8_t *sysdir = image + SYSDIR_LBA * SECTOR;
    write_iso_record(sysdir, 0, dot, sizeof(dot), SYSDIR_LBA, SECTOR, true);
    write_iso_record(sysdir, 34, dotdot, sizeof(dotdot), GAME_LBA, SECTOR, true);
    if (encrypted_eboot) {
        memset(eboot, 0, sizeof(eboot));
        memcpy(eboot, "~PSP", 4);
        eboot[0x27] = 1;
        eboot[0x55] = 0x10; /* one nonzero encrypted segment of size 0x1000 */
        eboot_size = sizeof(eboot);
    } else {
        build_synthetic_mips_elf(eboot);
        eboot_size = 88u;
    }
    write_iso_record(sysdir, 68, eboot_name, sizeof(eboot_name) - 1u,
                     EBOOT_LBA, (uint32_t)eboot_size, false);
    uint8_t *usrdir = image + USRDIR_LBA * SECTOR;
    write_iso_record(usrdir, 0, dot, sizeof(dot), USRDIR_LBA, SECTOR, true);
    write_iso_record(usrdir, 34, dotdot, sizeof(dotdot), GAME_LBA, SECTOR, true);
    memcpy(image + SFO_LBA * SECTOR, sfo, sfo_size);
    memcpy(image + EBOOT_LBA * SECTOR, eboot, eboot_size);

    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(image, 1, sizeof(image), file) == sizeof(image));
    assert(fclose(file) == 0);
}

/* A PSP PRX guest module (issue #729): e_type 0xFFA0 with the unset e_entry
 * 0xFFFFFFFF, relocatable at 0, and one executable PT_LOAD with code bytes.
 * Its start routine comes from the module info, so e_entry is not an address. */
static void build_synthetic_guest_prx(unsigned char elf[88]) {
    memset(elf, 0, 88);
    memcpy(elf, "\x7f" "ELF", 4);
    elf[4] = 1; /* ELF32 */
    elf[5] = 1; /* little-endian */
    elf[6] = 1; /* current ELF version */
    elf[16] = 0xa0; elf[17] = 0xff; /* ET_SCE_PRX */
    elf[18] = 8; /* MIPS */
    elf[20] = 1;
    elf[24] = 0xff; elf[25] = 0xff; elf[26] = 0xff; elf[27] = 0xff; /* e_entry */
    elf[28] = 52; /* program-header table offset */
    elf[40] = 52; /* ELF header size */
    elf[42] = 32; /* program-header entry size */
    elf[44] = 1;  /* program-header count */
    elf[52] = 1;  /* PT_LOAD */
    elf[56] = 84; /* file offset */
    elf[68] = 4;  /* file size */
    elf[72] = 4;  /* memory size */
    elf[76] = 5;  /* readable + executable */
    elf[80] = 4;  /* alignment */
    elf[84] = 0x34; elf[85] = 0x12;
}

static void write_synthetic_guest_prx(const char *path, size_t length) {
    unsigned char elf[88];
    build_synthetic_guest_prx(elf);
    assert(length <= sizeof(elf));
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(elf, 1, length, file) == length);
    assert(fclose(file) == 0);
}

/* ISO 9660 directory record (ECMA-119 9.1); both-endian extents and sizes. */
static size_t iso_dir_record(uint8_t *record, const uint8_t *name, size_t name_len,
                             uint32_t lba, uint32_t size, bool is_dir) {
    size_t rec_len = 33 + name_len + ((name_len & 1u) == 0 ? 1u : 0u);
    memset(record, 0, rec_len);
    record[0] = (uint8_t)rec_len;
    for (int i = 0; i < 4; i++) {
        record[2 + i] = (uint8_t)(lba >> (8 * i));
        record[9 - i] = (uint8_t)(lba >> (8 * i));
        record[10 + i] = (uint8_t)(size >> (8 * i));
        record[17 - i] = (uint8_t)(size >> (8 * i));
    }
    record[25] = is_dir ? 0x02 : 0x00;
    record[28] = 1; /* volume sequence number 1, both-endian */
    record[31] = 1;
    record[32] = (uint8_t)name_len;
    memcpy(record + 33, name, name_len);
    return rec_len;
}

/* A minimal ISO whose PSP_GAME/USRDIR/module holds one guest module: PVD at
 * 16, root 17, PSP_GAME 18, USRDIR 19, module directory 20, data 21. */
static void write_guest_module_iso(const char *path, const char *name,
                                   const unsigned char *module, size_t module_size) {
    enum { SECTOR = 2048, DATA_LBA = 21 };
    static const uint8_t dot[1] = { 0 };
    static const uint8_t dotdot[1] = { 1 };
    size_t data_sectors = (module_size + SECTOR - 1) / SECTOR;
    size_t total = ((size_t)DATA_LBA + data_sectors) * SECTOR;
    uint8_t *image = (uint8_t *)calloc(1, total);
    assert(image != NULL);
    uint8_t *pvd = image + 16 * SECTOR;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    iso_dir_record(pvd + 156, dot, 1, 17, SECTOR, true);
    uint8_t *root = image + 17 * SECTOR;
    size_t off = 0;
    off += iso_dir_record(root + off, dot, 1, 17, SECTOR, true);
    off += iso_dir_record(root + off, dotdot, 1, 17, SECTOR, true);
    off += iso_dir_record(root + off, (const uint8_t *)"PSP_GAME", 8, 18, SECTOR, true);
    uint8_t *game = image + 18 * SECTOR;
    off = 0;
    off += iso_dir_record(game + off, dot, 1, 18, SECTOR, true);
    off += iso_dir_record(game + off, dotdot, 1, 17, SECTOR, true);
    off += iso_dir_record(game + off, (const uint8_t *)"USRDIR", 6, 19, SECTOR, true);
    uint8_t *usrdir = image + 19 * SECTOR;
    off = 0;
    off += iso_dir_record(usrdir + off, dot, 1, 19, SECTOR, true);
    off += iso_dir_record(usrdir + off, dotdot, 1, 18, SECTOR, true);
    iso_dir_record(usrdir + off, (const uint8_t *)"module", 6, 20, SECTOR, true);
    uint8_t *module_dir = image + 20 * SECTOR;
    off = 0;
    off += iso_dir_record(module_dir + off, dot, 1, 20, SECTOR, true);
    off += iso_dir_record(module_dir + off, dotdot, 1, 19, SECTOR, true);
    iso_dir_record(module_dir + off, (const uint8_t *)name, strlen(name), DATA_LBA,
                   (uint32_t)module_size, false);
    memcpy(image + (size_t)DATA_LBA * SECTOR, module, module_size);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(image, 1, total, file) == total);
    assert(fclose(file) == 0);
    free(image);
}

/* A disc whose PSP_GAME/SYSDIR/EBOOT.BIN holds the given bytes. */
static void write_sysdir_eboot_iso(const char *path, const unsigned char *eboot,
                                   size_t eboot_size) {
    enum { SECTOR = 2048, DATA_LBA = 20 };
    static const uint8_t dot[1] = { 0 };
    static const uint8_t dotdot[1] = { 1 };
    size_t data_sectors = (eboot_size + SECTOR - 1) / SECTOR;
    size_t total = ((size_t)DATA_LBA + data_sectors) * SECTOR;
    uint8_t *image = (uint8_t *)calloc(1, total);
    assert(image != NULL);
    uint8_t *pvd = image + 16 * SECTOR;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    iso_dir_record(pvd + 156, dot, 1, 17, SECTOR, true);
    uint8_t *root = image + 17 * SECTOR;
    size_t off = 0;
    off += iso_dir_record(root + off, dot, 1, 17, SECTOR, true);
    off += iso_dir_record(root + off, dotdot, 1, 17, SECTOR, true);
    off += iso_dir_record(root + off, (const uint8_t *)"PSP_GAME", 8, 18, SECTOR, true);
    uint8_t *game = image + 18 * SECTOR;
    off = 0;
    off += iso_dir_record(game + off, dot, 1, 18, SECTOR, true);
    off += iso_dir_record(game + off, dotdot, 1, 17, SECTOR, true);
    off += iso_dir_record(game + off, (const uint8_t *)"SYSDIR", 6, 19, SECTOR, true);
    uint8_t *sysdir = image + 19 * SECTOR;
    off = 0;
    off += iso_dir_record(sysdir + off, dot, 1, 19, SECTOR, true);
    off += iso_dir_record(sysdir + off, dotdot, 1, 18, SECTOR, true);
    iso_dir_record(sysdir + off, (const uint8_t *)"EBOOT.BIN", 9, DATA_LBA,
                   (uint32_t)eboot_size, false);
    memcpy(image + (size_t)DATA_LBA * SECTOR, eboot, eboot_size);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(image, 1, total, file) == total);
    assert(fclose(file) == 0);
    free(image);
}

/* One built-in value by name (kirk.cmd1.key, kirk.keyvault.<code>). */
static const uint8_t *builtin_value(const char *name) {
    for (size_t i = 0; i < nk_psp_builtin_value_count; i++) {
        if (strcmp(nk_psp_builtin_values[i].name, name) == 0) {
            assert(nk_psp_builtin_values[i].len == 16u);
            return nk_psp_builtin_values[i].bytes;
        }
    }
    assert(!"built-in value missing");
    return NULL;
}

/* payload sealed under one built-in game tag recipe, with the built-in
   keyvault row and command-1 key; seal_tag may differ from recipe_tag to
   put another tag in the header. */
static size_t seal_with_builtin_recipe(uint8_t *out, size_t cap, const uint8_t *payload,
                                       size_t payload_len, uint32_t recipe_tag,
                                       uint32_t seal_tag) {
    const NkPspBuiltinTag *recipe = NULL;
    char vault_name[32];
    for (size_t i = 0; i < nk_psp_builtin_tag_count; i++) {
        if (nk_psp_builtin_tags[i].tag == recipe_tag) recipe = &nk_psp_builtin_tags[i];
    }
    assert(recipe != NULL && recipe->key != NULL);
    snprintf(vault_name, sizeof(vault_name), "kirk.keyvault.%d", recipe->code);
    return psp_seal_type2(out, cap, payload, payload_len, 0, (uint32_t)payload_len,
                          seal_tag, recipe->key, builtin_value(vault_name),
                          builtin_value("kirk.cmd1.key"));
}

static const char *const FIXTURE_SHA256 =
    "f16d05ec6b29248d2c61adb1e9263f78e4f7bace1b955014a2d17872cfe4064d";

static void write_runtime_package_fixture_with_options(
    const char *user_root, const char *disc_id, const char *title_id,
    uint32_t abi_version, const char *executable_relative_path,
    const char *input_executable_sha256, const char *seed_executable_path,
    const char *source_media_json, size_t report_target_size,
    bool synthetic_title_identity) {
    char packages[768], package_dir[896], executable[1100], image[1100];
    char package_json[16384], report_json[8192], cache_json[4096], cache_key_json[3072];
    char aot_components_json[2048], native_components_json[2048], identity_json[2048];
    char aot_hash_input[2050], native_hash_input[2050], identity_hash_input[2050];
    char aot_digest[65], native_digest[65], modules_digest[65], identity_digest[65];
    const char *codegen_options_digest =
        "ca3d163bab055381827226140568f3bef7eaac187cebd76878e0b63e9e442356";
    snprintf(packages, sizeof(packages), "%s%cpackages", user_root,
             nk_platform_path_separator());
    snprintf(package_dir, sizeof(package_dir), "%s%c%s", packages,
             nk_platform_path_separator(), disc_id);
    assert(nk_platform_mkdir_p(package_dir));
    snprintf(executable, sizeof(executable), "%s%c%s.exe", package_dir,
             nk_platform_path_separator(), title_id);
    snprintf(image, sizeof(image), "%s%c%s_image.bin", package_dir,
             nk_platform_path_separator(), title_id);
    if (seed_executable_path) {
        copy_executable_file(seed_executable_path, executable);
    } else {
        write_file(executable);
    }
    write_file(image);
    /* package.json must carry the digest of the bytes actually on disk: the
       validator compares it against the file ("Package executable hash is
       stale"). For the stub payload this is exactly FIXTURE_SHA256. */
    char executable_hash[65];
    fixture_sha_file(executable, executable_hash);

    const char *region = strncmp(disc_id, "UL", 2) == 0 ? "NA" : "TEST";
    char disc_identity_json[256];
    if (synthetic_title_identity) {
        snprintf(disc_identity_json, sizeof(disc_identity_json),
                 "{\"disc_version\":null,\"id\":null,\"region\":null}");
    } else {
        snprintf(disc_identity_json, sizeof(disc_identity_json),
                 "{\"disc_version\":null,\"id\":\"%s\",\"region\":\"%s\"}",
                 disc_id, region);
    }
    if (!source_media_json) source_media_json = "null";
    int identity_length = snprintf(identity_json, sizeof(identity_json),
        "{\"container\":null,\"disc\":%s,\"format\":\"nakagawa-title-input-identity\","
        "\"main_executable\":{\"name\":\"EBOOT.BIN\",\"sha256\":\"%s\"},"
        "\"manifest\":{\"id\":\"%s\",\"schema_version\":1},"
        "\"modules\":[],\"param_sfo\":null,\"psp_header\":null,\"schema_version\":2,"
        "\"source_media\":%s}",
        disc_identity_json, input_executable_sha256, title_id, source_media_json);
    assert(identity_length > 0 && (size_t)identity_length < sizeof(identity_json));
    int identity_hash_length = snprintf(identity_hash_input,
        sizeof(identity_hash_input), "%s\n", identity_json);
    assert(identity_hash_length > 0 &&
           (size_t)identity_hash_length < sizeof(identity_hash_input));
    fixture_sha_bytes(identity_hash_input, (size_t)identity_hash_length, identity_digest);

    char identities[768], identity_dir[896], identity_path[1100];
    snprintf(identities, sizeof(identities), "%s%ctitle-input-identities", user_root,
             nk_platform_path_separator());
    snprintf(identity_dir, sizeof(identity_dir), "%s%c%s", identities,
             nk_platform_path_separator(), disc_id);
    assert(nk_platform_mkdir_p(identity_dir));
    snprintf(identity_path, sizeof(identity_path), "%s%ctitle-input-identity.json",
             identity_dir, nk_platform_path_separator());
    write_text_file(identity_path, identity_json);

    fixture_sha_bytes("[]\n", 3, modules_digest);
    int aot_components_length = snprintf(aot_components_json, sizeof(aot_components_json),
        "{\"analyzer_codegen_epoch\":\"analyzer-codegen-v1\","
        "\"analyzer_sha256\":\"%064d\",\"codegen_options_sha256\":\"%s\","
        "\"codegen_sha256\":\"%064d\",\"executable_sha256\":\"%s\","
        "\"generated_code_abi_epoch\":%d,\"manifest_sha256\":\"%064d\","
        "\"modules_sha256\":\"%s\",\"psp_header_sha256\":null,"
        "\"runtime_abi_epoch\":%d,\"title_input_identity_sha256\":\"%s\"}",
        0, codegen_options_digest, 0, input_executable_sha256, NK_AOT_GENERATED_CODE_ABI_EPOCH, 0,
        modules_digest, NK_AOT_RUNTIME_ABI_EPOCH, identity_digest);
    assert(aot_components_length > 0 && (size_t)aot_components_length < sizeof(aot_components_json));
    int native_components_length = snprintf(native_components_json, sizeof(native_components_json),
        "{\"compile_flags\":\"\",\"compiler_identity\":\"gcc-fixture\","
        "\"compiler_target\":\"fixture-target\",\"generated_code_digest\":\"%064d\","
        "\"link_flags\":\"\",\"runtime_abi_epoch\":%d,\"runtime_source_digest\":\"%064d\"}",
        0, NK_AOT_RUNTIME_ABI_EPOCH, 0);
    assert(native_components_length > 0 && (size_t)native_components_length < sizeof(native_components_json));
    int aot_hash_length = snprintf(aot_hash_input, sizeof(aot_hash_input), "%s\n", aot_components_json);
    int native_hash_length = snprintf(native_hash_input, sizeof(native_hash_input), "%s\n", native_components_json);
    assert(aot_hash_length > 0 && (size_t)aot_hash_length < sizeof(aot_hash_input));
    assert(native_hash_length > 0 && (size_t)native_hash_length < sizeof(native_hash_input));
    fixture_sha_bytes(aot_hash_input, (size_t)aot_hash_length, aot_digest);
    fixture_sha_bytes(native_hash_input, (size_t)native_hash_length, native_digest);
    int cache_key_length = snprintf(cache_key_json, sizeof(cache_key_json),
        "{\"schema_version\":2,\"aot\":{\"digest\":\"%s\",\"components\":%s},"
        "\"native\":{\"digest\":\"%s\",\"components\":%s}}",
        aot_digest, aot_components_json, native_digest, native_components_json);
    assert(cache_key_length > 0 && (size_t)cache_key_length < sizeof(cache_key_json));
    int cache_length = snprintf(cache_json, sizeof(cache_json),
        "{\"format\":\"nakagawa-aot-cache\",\"schema_version\":2,\"key\":%s,"
        "\"codegen_options\":{},\"runtime_abi_compatibility\":{"
        "\"current_epoch\":%d,\"generated_code_reusable\":true}}",
        cache_key_json, NK_AOT_RUNTIME_ABI_EPOCH);
    assert(cache_length > 0 && (size_t)cache_length < sizeof(cache_json));
    int report_length = snprintf(report_json, sizeof(report_json),
        "{\"format\":\"nakagawa-build-report\",\"schema_version\":1,"
        "\"title_id\":\"%s\",\"runtime_abi\":{\"name\":\"CpuState\",\"version\":%u},"
        "\"cache\":%s,"
        "\"input_hashes\":{\"manifest\":{\"sha256\":\"%064d\"},"
        "\"executable\":{\"sha256\":\"%s\"},\"modules\":[],\"psp_header\":null},"
        "\"tools\":{},\"coverage\":{},\"unsupported\":{\"imports\":[],"
        "\"instructions\":[],\"regions\":[]},\"analysis_diagnostics\":[],\"artifacts\":{}}\n",
        title_id, (unsigned)abi_version, cache_json, 0, input_executable_sha256);
    assert(report_length > 0 && (size_t)report_length < sizeof(report_json));
    char report_path[1100];
    snprintf(report_path, sizeof(report_path), "%s%cbuild-report.json", package_dir,
             nk_platform_path_separator());
    write_text_file(report_path, report_json);
    assert(report_target_size == 0 || report_target_size >= (size_t)report_length);
    if (report_target_size > (size_t)report_length) {
        append_json_whitespace(report_path,
                               report_target_size - (size_t)report_length);
    }

    int package_length = snprintf(package_json, sizeof(package_json),
        "{\"format\":\"nakagawa-aot-package\",\"schema_version\":2,\"cache\":%s,"
        "\"title\":{\"id\":\"%s\",\"display_name\":\"Synthetic fixture\","
        "\"kind\":\"retail\",\"manifest_sha256\":\"%064d\","
        "\"protected_digest\":\"%064d\"},"
        "\"inputs\":{\"manifest\":{\"sha256\":\"%064d\"},"
        "\"executable\":{\"sha256\":\"%s\"},\"modules\":[],\"psp_header\":null},"
        "\"title_input_identity\":%s,"
        "\"runtime\":{\"abi\":\"CpuState\",\"abi_version\":%u,"
        "\"abi_header_sha256\":\"%064d\",\"run_entry\":\"0x00000000\","
        "\"runtime_contract\":null,\"runtime_bindings\":{},"
        "\"required_runtime_bindings\":[]},"
        "\"executable\":{\"path\":\"%s\",\"sha256\":\"%s\","
        "\"guest_entry\":\"0x00000000\"},\"generated_objects\":[],"
        "\"required_local_assets\":[],\"build_report\":\"build-report.json\"}\n",
        cache_json, title_id, 0, 0, 0, input_executable_sha256, identity_json,
        (unsigned)abi_version, 0, executable_relative_path, executable_hash);
    assert(package_length > 0 && (size_t)package_length < sizeof(package_json));
    char package_path[1100];
    snprintf(package_path, sizeof(package_path), "%s%cpackage.json", package_dir,
             nk_platform_path_separator());
    write_text_file(package_path, package_json);

    char package_hash[65], report_hash[65], image_hash[65];
    fixture_sha_file(package_path, package_hash);
    fixture_sha_file(report_path, report_hash);
    fixture_sha_file(image, image_hash);
    char image_relative[256];
    const char *extension = strrchr(executable_relative_path, '.');
    size_t stem_length = extension && extension != executable_relative_path
        ? (size_t)(extension - executable_relative_path) : strlen(executable_relative_path);
    snprintf(image_relative, sizeof(image_relative), "%.*s_image.bin",
             (int)stem_length, executable_relative_path);
    char completion_json[4096];
    int completion_length = snprintf(completion_json, sizeof(completion_json),
        "{\"format\":\"nakagawa-aot-cache-completion\",\"schema_version\":2,"
        "\"status\":\"complete\",\"cache_key\":%s,\"title_input_identity\":%s,\"artifacts\":["
        "{\"path\":\"package.json\",\"sha256\":\"%s\"},"
        "{\"path\":\"build-report.json\",\"sha256\":\"%s\"},"
        "{\"path\":\"%s\",\"sha256\":\"%s\"},"
        "{\"path\":\"%s\",\"sha256\":\"%s\"}]}\n",
        cache_key_json, identity_json, package_hash, report_hash, executable_relative_path,
        executable_hash, image_relative, image_hash);
    assert(completion_length > 0 && (size_t)completion_length < sizeof(completion_json));
    char completion_path[1100];
    snprintf(completion_path, sizeof(completion_path), "%s%ccompletion-manifest.json",
             package_dir, nk_platform_path_separator());
    write_text_file(completion_path, completion_json);
}

static void write_runtime_package_fixture(const char *user_root,
                                          const char *disc_id,
                                          const char *title_id,
                                          uint32_t abi_version,
                                          const char *executable_relative_path,
                                          const char *input_executable_sha256,
                                          const char *seed_executable_path) {
    write_runtime_package_fixture_with_options(
        user_root, disc_id, title_id, abi_version, executable_relative_path,
        input_executable_sha256, seed_executable_path, NULL, 0, false);
}

static void write_runtime_package_fixture_with_source_media(
    const char *user_root, const char *disc_id, const char *title_id,
    uint32_t abi_version, const char *executable_relative_path,
    const char *input_executable_sha256, const char *seed_executable_path,
    const char *source_media_json) {
    write_runtime_package_fixture_with_options(
        user_root, disc_id, title_id, abi_version, executable_relative_path,
        input_executable_sha256, seed_executable_path, source_media_json, 0, false);
}

static void write_runtime_package_fixture_with_report_size(
    const char *user_root, const char *disc_id, const char *title_id,
    uint32_t abi_version, const char *executable_relative_path,
    const char *input_executable_sha256, const char *seed_executable_path,
    size_t report_target_size) {
    write_runtime_package_fixture_with_options(
        user_root, disc_id, title_id, abi_version, executable_relative_path,
        input_executable_sha256, seed_executable_path, NULL, report_target_size,
        false);
}

static void write_runtime_package_fixture_with_synthetic_title_identity(
    const char *user_root, const char *disc_id, const char *title_id,
    uint32_t abi_version, const char *executable_relative_path,
    const char *input_executable_sha256, const char *seed_executable_path) {
    write_runtime_package_fixture_with_options(
        user_root, disc_id, title_id, abi_version, executable_relative_path,
        input_executable_sha256, seed_executable_path, NULL, 0, true);
}

/* Remove exactly what write_runtime_package_fixture() writes under *user_root*
 * for *disc_id*, then the directories that held it. Every subtest that builds a
 * synthetic package owns a dedicated root under the cache directory and clears
 * it before it writes, so a re-run or a later subtest can never inherit a
 * package, an identity document or a completion manifest from an earlier one.
 *
 * Best-effort by design: rmdir() only removes an empty directory, so a root
 * that still holds something unrecognised is left in place rather than followed
 * into. Every failure mode here is "the tree is still there", which the next
 * pre-clean handles. */
static void remove_runtime_package_fixture(const char *user_root,
                                           const char *disc_id,
                                           const char *title_id) {
    char sep = nk_platform_path_separator();
    char packages[768], package_dir[896];
    char identities[768], identity_dir[896];
    char path[1100];
    static const char *const kPackageFiles[] = {
        "package.json", "build-report.json", "completion-manifest.json",
    };
    snprintf(packages, sizeof(packages), "%s%cpackages", user_root, sep);
    snprintf(package_dir, sizeof(package_dir), "%s%c%s", packages, sep, disc_id);
    snprintf(identities, sizeof(identities), "%s%ctitle-input-identities",
             user_root, sep);
    snprintf(identity_dir, sizeof(identity_dir), "%s%c%s", identities, sep,
             disc_id);
    for (size_t i = 0; i < sizeof(kPackageFiles) / sizeof(kPackageFiles[0]); i++) {
        snprintf(path, sizeof(path), "%s%c%s", package_dir, sep, kPackageFiles[i]);
        remove(path);
    }
    snprintf(path, sizeof(path), "%s%c%s.exe", package_dir, sep, title_id);
    remove(path);
    snprintf(path, sizeof(path), "%s%c%s_image.bin", package_dir, sep, title_id);
    remove(path);
    snprintf(path, sizeof(path), "%s%ctitle-input-identity.json", identity_dir, sep);
    remove(path);
    test_rmdir(package_dir);
    test_rmdir(identity_dir);
    test_rmdir(packages);
    test_rmdir(identities);
    test_rmdir(user_root);
}

static void write_experimental_profile_fixture(const char *user_root,
                                               const char *disc_id,
                                               const char *title_id,
                                               const char *selected_executable,
                                               const char *executable_sha256) {
    char experimental[768], profile_dir[896], profile_path[1100];
    char profile_json[4096];
    snprintf(experimental, sizeof(experimental), "%s%cexperimental", user_root,
             nk_platform_path_separator());
    snprintf(profile_dir, sizeof(profile_dir), "%s%c%s", experimental,
             nk_platform_path_separator(), disc_id);
    assert(nk_platform_mkdir_p(profile_dir));
    snprintf(profile_path, sizeof(profile_path), "%s%cprofile.json", profile_dir,
             nk_platform_path_separator());
    int length = snprintf(profile_json, sizeof(profile_json),
        "{\"schema_version\":1,\"manifest\":{\"schema_version\":1,"
        "\"id\":\"%s\",\"game_name\":\"%s\","
        "\"display_name\":\"Synthetic experiment\",\"kind\":\"retail\","
        "\"disc\":{\"id\":\"%s\",\"region\":\"NA\","
        "\"revision_policy\":\"exact-disc-id\"},"
        "\"executable\":{\"base\":0,\"entry\":0,\"bss_metadata_source\":\"none\","
        "\"extra_executable_spans\":[]},\"modules\":[],"
        "\"filesystem\":{\"data_root\":\"data\",\"memory_stick_root\":\"savedata\","
        "\"device_prefixes\":[\"disc0:\",\"ms0:\"]},\"hle_profile\":\"generic\","
        "\"feature_requirements\":[],\"verification_profile\":\"experimental-unverified\"},"
        "\"input_identity\":{\"disc_id\":\"%s\","
        "\"selected_executable\":\"PSP_GAME/SYSDIR/%s\","
        "\"executable_sha256\":\"%s\",\"elf_sha256\":\"%s\"}}\n",
        title_id, title_id, disc_id, disc_id, selected_executable,
        executable_sha256, executable_sha256);
    assert(length > 0 && (size_t)length < sizeof(profile_json));
    write_text_file(profile_path, profile_json);
}

static const PlayerPreflightCheck *find_preflight_check(
    const PlayerCompatibilityPreflight *preflight, const char *code) {
    if (!preflight || !code) return NULL;
    for (size_t i = 0; i < preflight->count; i++) {
        if (strcmp(preflight->checks[i].code, code) == 0) return &preflight->checks[i];
    }
    return NULL;
}

static void assert_module_scan_boundary(PlayerApp *app, const char *iso_path,
                                        const NkIsoExecutableReport *executables,
                                        const char *message_fragment) {
    snprintf(app->inspecting_game.iso_path,
             sizeof(app->inspecting_game.iso_path), "%s", iso_path);
    player_app_build_compatibility_preflight(app, true, true, executables);
    const PlayerPreflightCheck *check = find_preflight_check(
        &app->wizard.preflight, "GUEST_MODULES");
    assert(check != NULL && check->status == PREFLIGHT_UNSUPPORTED);
    if (strstr(check->message, message_fragment) == NULL) {
        fprintf(stderr, "expected module boundary %s, got: %s\n",
                message_fragment, check->message);
    }
    assert(strstr(check->message, message_fragment) != NULL);
    assert(check->issue_count == 1 && check->issue_numbers[0] == 308);
}

/* The package-status identity the validator itself computes for *user_root*
 * (nk_launch_runtime_package_cache_identity) - the digest a package-validation
 * cache entry is bound to. Re-measuring it is how this test tells "the identity
 * came back" from "the identity changed" instead of guessing from a clock. */
static bool fixture_status_identity(const char *user_root, const NkGameEntry *game,
                                    char out_identity[65]) {
    return nk_launch_runtime_package_cache_identity(user_root, game, out_identity);
}

/* Track the identity of the last successful cache entry across package
 * repairs. A hit is valid only when the current status identity still matches
 * that entry; a successful miss may replace it with the repaired identity.
 * The key uses (size, write timestamp, platform change value) for each
 * metadata file and artifact; the timestamps are not generation counters, so
 * a same-size edit inside both timestamp granularities can still collide
 * (#683). */
static void assert_repaired_validation(const char *user_root, const NkGameEntry *game,
                                       char identity_when_cached[65],
                                       NkRuntimePackageStatus status,
                                       const NkRuntimePackageInfo *info) {
    assert(status == NK_RUNTIME_PACKAGE_OK);
    char identity_now[65];
    assert(fixture_status_identity(user_root, game, identity_now));
    if (info->validation_cache_hit) {
        assert(strcmp(identity_now, identity_when_cached) == 0);
    }
    memcpy(identity_when_cached, identity_now, sizeof(identity_now));
}

static const char *runtime_package_status_name(NkRuntimePackageStatus status) {
    switch (status) {
        case NK_RUNTIME_PACKAGE_OK: return "OK";
        case NK_RUNTIME_PACKAGE_MISSING: return "MISSING";
        case NK_RUNTIME_PACKAGE_INCOMPATIBLE: return "INCOMPATIBLE";
        case NK_RUNTIME_PACKAGE_STALE: return "STALE";
        case NK_RUNTIME_PACKAGE_UNKNOWN: return "UNKNOWN";
        default: return "UNKNOWN";
    }
}

/* Fake-runtime child mode for the repeat-launch subtest (#511): spawned by
 * nk_launch_start as `--image <path> ...`. Observes the save root the session
 * handed over (SR_MEMSTICK), reports what it saw BEFORE writing, then leaves
 * one line of savedata behind for the next launch to observe. Exit code is
 * scripted through NK_REPEAT_LAUNCH_EXIT_CODE so early-failure handling can
 * be exercised deterministically. */
static int repeat_launch_child_mode(void) {
    const char *report_path = getenv("NK_REPEAT_LAUNCH_REPORT_FILE");
    if (!report_path || !*report_path) return 2;
    int exit_code = 0;
    const char *code_text = getenv("NK_REPEAT_LAUNCH_EXIT_CODE");
    if (code_text && *code_text) {
        char *end = NULL;
        long parsed = strtol(code_text, &end, 10);
        if (!end || *end || parsed < 0 || parsed > 125) return 2;
        exit_code = (int)parsed;
    }
    const char *memstick = getenv("SR_MEMSTICK");
    char marker[NK_MAX_PATH * 2];
    marker[0] = '\0';
    bool marker_present = false;
    if (memstick && *memstick) {
        snprintf(marker, sizeof(marker), "%s%cf511_repeat.marker", memstick,
                 nk_platform_path_separator());
        FILE *existing = fopen(marker, "rb");
        if (existing) {
            marker_present = true;
            fclose(existing);
        }
    }
    FILE *report = fopen(report_path, "wb");
    if (!report) return 3;
    int ok = fprintf(report, "SR_MEMSTICK=%s\nMARKER_PRESENT=%d\n",
                     (memstick && *memstick) ? memstick : "<unset>",
                     marker_present ? 1 : 0) >= 0;
    if (fclose(report) != 0) ok = 0;
    if (!ok) return 4;
    /* Runtime output: the launch sends stdout and stderr to the boot-event
       file, which the player keeps as logs/game-<disc>-last.txt (#1303). */
    printf("synthetic runtime stdout marker_present=%d\n", marker_present ? 1 : 0);
    fflush(stdout);
    fprintf(stderr, "synthetic runtime stderr line\n");
    fflush(stderr);
    const char *stop_reason = getenv("NK_REPEAT_LAUNCH_STOP_REASON");
    const char *boot_event_path = getenv("SR_BOOT_EVENT_FILE");
    if (stop_reason && strcmp(stop_reason, "hold-without-boot-event") == 0) {
        /* A child that runs but never reaches the runtime's first boot event
           (#1290): it writes none and stays alive until the player stops it.
           The 60 s bound only keeps a failed test from leaving it behind. */
        for (int i = 0; i < 600; i++) {
#if defined(_WIN32) || defined(_WIN64)
            Sleep(100);
#else
            const struct timespec delay = { 0, 100000000L };
            (void)nanosleep(&delay, NULL);
#endif
        }
        return 8;
    }
    if (stop_reason && strcmp(stop_reason, "semantic-boundary") == 0 &&
        boot_event_path && *boot_event_path) {
        FILE *events = fopen(boot_event_path, "ab");
        if (!events) return 6;
        ok = fputs("BOOT_EVENT phase=stop reason=semantic-boundary boundary=unsupported-interpreter-form issue=308\n",
                   events) >= 0;
        if (fclose(events) != 0) ok = 0;
        if (!ok) return 6;
    } else if (stop_reason && strcmp(stop_reason, "host-fiber-stack-exhausted") == 0 &&
               boot_event_path && *boot_event_path) {
        FILE *events = fopen(boot_event_path, "ab");
        if (!events) return 6;
        ok = fputs("BOOT_EVENT phase=stop reason=semantic-boundary "
                   "boundary=host-fiber-stack-exhausted\n", events) >= 0;
        if (fclose(events) != 0) ok = 0;
        if (!ok) return 6;
    }
    if (marker[0]) {
        FILE *save = fopen(marker, "ab");
        if (!save) return 5;
        ok = fputs("run\n", save) >= 0;
        if (fclose(save) != 0) ok = 0;
        if (!ok) return 5;
    }
    const char *hold_open = getenv("NK_REPEAT_LAUNCH_HOLD_OPEN");
    if (hold_open && strcmp(hold_open, "1") == 0) {
        if (boot_event_path && *boot_event_path) {
            FILE *events = fopen(boot_event_path, "ab");
            if (!events) return 7;
            ok = fputs("BOOT_EVENT phase=display_flip source=synthetic\n", events) >= 0;
            if (fclose(events) != 0) ok = 0;
            if (!ok) return 7;
        }
#if defined(_WIN32) || defined(_WIN64)
        Sleep(INFINITE);
#else
        for (;;) (void)pause();
#endif
    }
    return exit_code;
}

/* True when the file at `path` holds `needle`. */
static bool run_log_contains(const char *path, const char *needle) {
    FILE *file = nk_fopen_utf8(path, "rb");
    if (!file) return false;
    char text[8192];
    size_t got = fread(text, 1, sizeof(text) - 1u, file);
    fclose(file);
    text[got] = '\0';
    return strstr(text, needle) != NULL;
}

static bool append_runtime_event(const char *path, const char *event) {
    FILE *file = path ? fopen(path, "ab") : NULL;
    if (!file) return false;
    bool ok = fputs(event, file) >= 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}

/* True when a developer log line holds `needle`. */
static bool dev_log_contains(const PlayerApp *app, const char *needle) {
    for (size_t i = 0; i < player_dev_log_count(app); i++) {
        const char *line = player_dev_log_line(app, i);
        if (line && strstr(line, needle)) return true;
    }
    return false;
}

/* Launch through the player-owned session path, let the child reach a
 * bounded exit, then settle the session at `settle_tick`. Pins the repeat
 * contract: identical save root, no stuck running state, no retained child
 * handle, and no new structured error on a clean exit. */
static void repeat_launch_and_settle(PlayerApp *app, int game_index,
                                     uint64_t launch_tick, uint64_t settle_tick,
                                     const char *expected_memstick) {
    char error_before[32];
    snprintf(error_before, sizeof(error_before), "%s", app->last_error.error_code);
    assert(player_app_launch_game(app, game_index));
    assert(app->is_game_running);
    assert(strcmp(app->launch_session.memstick_root, expected_memstick) == 0);
    app->launch_time_ms = launch_tick;
    assert(nk_launch_wait(&app->launch_session, 10000) == 0);
    assert(player_app_monitor_game_session(app, settle_tick));
    assert(!app->is_game_running);
    assert_session_released(&app->launch_session);
    assert(strcmp(app->last_error.error_code, error_before) == 0);
}

/* Catalog-epoch coherence probe (#670). Built only with the test seam macro,
 * like the SR_CD_TEST_HOOKS selftests. The checkpoint runs inside
 * nk_title_manifest_validate_aot_package at the package-validation cache
 * publication point, so the forced catalog reload is deterministic rather than
 * timing-dependent: it cannot land before the epoch snapshot or after the
 * cache insertion, only exactly between them. */
#if defined(NK_TITLE_MANIFEST_TEST_SEAMS)
static uint64_t s_epoch_publication_before = 0;
static uint64_t s_epoch_publication_after = 0;

typedef struct {
    const char *overlay_path;
    uint64_t epoch_before;
    uint64_t epoch_after;
    bool loaded;
    char error[512];
} CatalogReloadAtPublication;

static void reload_catalog_overlay(CatalogReloadAtPublication *reload) {
    reload->epoch_before = nk_title_catalog_epoch();
    nk_title_catalog_clear_overlay();
    reload->loaded = nk_title_manifest_load_overlay(
        reload->overlay_path, reload->error, sizeof(reload->error));
    reload->epoch_after = nk_title_catalog_epoch();
}

#if defined(_WIN32) || defined(_WIN64)
static unsigned __stdcall reload_catalog_overlay_thread(void *ctx) {
    reload_catalog_overlay((CatalogReloadAtPublication *)ctx);
    return 0;
}
#else
static void *reload_catalog_overlay_thread(void *ctx) {
    reload_catalog_overlay((CatalogReloadAtPublication *)ctx);
    return NULL;
}
#endif

static void force_catalog_reload_at_publication(void *ctx) {
    CatalogReloadAtPublication *reload = (CatalogReloadAtPublication *)ctx;
    assert(reload != NULL);
#if defined(_WIN32) || defined(_WIN64)
    uintptr_t thread = _beginthreadex(NULL, 0,
                                      reload_catalog_overlay_thread, reload,
                                      0, NULL);
    if (!thread) abort();
    HANDLE handle = (HANDLE)thread;
    if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0) abort();
    CloseHandle(handle);
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, reload_catalog_overlay_thread, reload) != 0) {
        abort();
    }
    if (pthread_join(thread, NULL) != 0) abort();
#endif
    if (!reload->loaded) {
        fprintf(stderr, "[PLAYER_STATE_TEST] catalog reload failed: %s\n",
                reload->error);
    }
    assert(reload->loaded);
    s_epoch_publication_before = reload->epoch_before;
    s_epoch_publication_after = reload->epoch_after;
}
#endif /* NK_TITLE_MANIFEST_TEST_SEAMS */

#if defined(NK_TITLE_MANIFEST_TEST_SEAMS)
static const struct {
    const char *path;
    bool valid;
} source_iso_path_cases[] = {
    {"PSP_GAME/SYSDIR/EBOOT.BIN", true},
    {"psp_game/USRDIR/module/fixture.prx", true},
    {"PSP_GAME/USRDIR/space in name.prx", true},
    {"PSP_GAME/USRDIR/punctuation_#%!+.prx", true},
    {"PSP_GAME/EBOOT.BIN", false},
    {"OTHER_GAME/USRDIR/fixture.prx", false},
    {"PSP_GAME//USRDIR/fixture.prx", false},
    {"PSP_GAME/USRDIR/../SYSDIR/EBOOT.BIN", false},
    {"PSP_GAME/USRDIR/./fixture.prx", false},
    {"C:/PSP_GAME/USRDIR/fixture.prx", false},
    {"/PSP_GAME/USRDIR/fixture.prx", false},
    {"PSP_GAME/USRDIR/back\\\\slash.prx", false},
    {"PSP_GAME/USRDIR/fixture?.prx", false},
    {"PSP_GAME/USRDIR/fixture*.prx", false},
    {"PSP_GAME/USRDIR/fixture\".prx", false},
    {"PSP_GAME/USRDIR/fixture<.prx", false},
    {"PSP_GAME/USRDIR/fixture>.prx", false},
    {"PSP_GAME/USRDIR/fixture|.prx", false},
    {"PSP_GAME/USRDIR/control\x1f.prx", false},
    {"PSP_GAME/USRDIR/del\x7f.prx", false},
    {"PSP_GAME/USRDIR/non-ascii-\xc3\xa9.prx", false},
};

static void test_source_iso_member_path_validation(void) {
    for (size_t i = 0;
         i < sizeof(source_iso_path_cases) / sizeof(source_iso_path_cases[0]);
         i++) {
        assert(nk_title_manifest_test_source_iso_path_valid(
                   source_iso_path_cases[i].path) ==
               source_iso_path_cases[i].valid);
    }
    char too_long[600];
    const char prefix[] = "PSP_GAME/USRDIR/";
    size_t prefix_length = sizeof(prefix) - 1u;
    memcpy(too_long, prefix, prefix_length);
    memset(too_long + prefix_length, 'a', 513u);
    too_long[prefix_length + 513u] = '\0';
    assert(!nk_title_manifest_test_source_iso_path_valid(too_long));
}
#endif

/* The bitmap fallback (SDL_RenderDebugText) draws ASCII only. Every UTF-8 text
 * it draws goes through player_text_for_bitmap_font, so a title that the system
 * font can draw (the trademark sign) is readable there too, and nothing else
 * reaches the debug font as raw bytes. */
static void test_bitmap_font_text_fallback(void) {
    char out[64];
    size_t n = player_text_for_bitmap_font("Fixture\xE2\x84\xA2", out, sizeof(out));
    assert(strcmp(out, "FixtureTM") == 0);
    assert(n == strlen(out));

    player_text_for_bitmap_font("caf\xC3\xA9", out, sizeof(out));
    assert(strcmp(out, "caf?") == 0);

    /* A truncated sequence and a stray continuation byte each become one '?'. */
    player_text_for_bitmap_font("a\xE2\x84", out, sizeof(out));
    assert(strcmp(out, "a??") == 0);
    player_text_for_bitmap_font("\x80" "b", out, sizeof(out));
    assert(strcmp(out, "?b") == 0);

    player_text_for_bitmap_font("tab\there\x7f", out, sizeof(out));
    assert(strcmp(out, "tab here?") == 0);

    /* Output never overruns its buffer; the cut is at a whole glyph. */
    char small[4];
    n = player_text_for_bitmap_font("abcdef", small, sizeof(small));
    assert(strcmp(small, "abc") == 0);
    assert(n == 3);
    char tiny[3];
    n = player_text_for_bitmap_font("\xE2\x84\xA2" "x", tiny, sizeof(tiny));
    assert(strcmp(tiny, "TM") == 0);
    assert(n == 2);

    printf("[PLAYER_STATE_TEST] bitmap font text fallback PASS\n");
}

/* Paths under the user's profile are shown with the profile folder replaced by
 * "~" (the stored path is untouched). The profile folder is matched on whole
 * path components only, so a profile name that is a prefix of another folder
 * name is left alone. */
/* Open licence texts asks for <data root>/prerequisites/notices, from the same
   data root the rest of the player uses, and asks for nothing without an app. */
static void test_license_folder_request(void) {
    PlayerApp *app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    char expected[NK_MAX_PATH];
    char sep = nk_platform_path_separator();
    assert(app != NULL);
    assert(!player_app_request_license_folder(NULL));
    snprintf(app->runtime_root, sizeof(app->runtime_root), "synthetic-data-root");
    assert(player_app_request_license_folder(app));
    assert(app->request_open_folder);
    snprintf(expected, sizeof(expected), "synthetic-data-root%cprerequisites%cnotices", sep, sep);
    assert(strcmp(app->requested_open_path, expected) == 0);
    free(app);
}

static void test_display_path_hides_profile_folder(void) {
    /* Each platform's own path spelling: the profile folder and its separator. */
#if defined(_WIN32) || defined(_WIN64)
#define TP_HOME "C:\\path\\synthetic-user"
#define TP_OTHER "D:"
#define TP_SEP "\\"
#else
#define TP_HOME "/srv/synthetic-user"
#define TP_OTHER "/media"
#define TP_SEP "/"
#endif
    char out[160];
    const char *home = TP_HOME;

    /* The saves root from the Settings screen. */
    size_t n = player_display_text_with_home(
        TP_HOME TP_SEP "AppData" TP_SEP "Local" TP_SEP "Nakagawa" TP_SEP "saves", home,
        out, sizeof(out));
    assert(strcmp(out, "~" TP_SEP "AppData" TP_SEP "Local" TP_SEP "Nakagawa" TP_SEP "saves") == 0);
    assert(n == strlen(out));

    /* Exactly the profile folder, a trailing separator on the profile, and a
     * path embedded in a sentence. */
    player_display_text_with_home(TP_HOME, home, out, sizeof(out));
    assert(strcmp(out, "~") == 0);
    player_display_text_with_home(TP_HOME TP_SEP "Games" TP_SEP "disc.iso",
                                  TP_HOME TP_SEP, out, sizeof(out));
    assert(strcmp(out, "~" TP_SEP "Games" TP_SEP "disc.iso") == 0);
    player_display_text_with_home("Promoted to " TP_HOME TP_SEP "AppData, done.",
                                  home, out, sizeof(out));
    assert(strcmp(out, "Promoted to ~" TP_SEP "AppData, done.") == 0);

    /* A sibling folder whose name only starts with the profile name is untouched,
     * and so is a path outside the profile. */
    player_display_text_with_home(TP_HOME "2" TP_SEP "Games", home, out, sizeof(out));
    assert(strcmp(out, TP_HOME "2" TP_SEP "Games") == 0);
    player_display_text_with_home(TP_OTHER TP_SEP "Games" TP_SEP "disc.iso", home,
                                  out, sizeof(out));
    assert(strcmp(out, TP_OTHER TP_SEP "Games" TP_SEP "disc.iso") == 0);

    /* No profile known: nothing is rewritten. */
    player_display_text_with_home(TP_HOME TP_SEP "x", "", out, sizeof(out));
    assert(strcmp(out, TP_HOME TP_SEP "x") == 0);
    player_display_text_with_home(TP_HOME TP_SEP "x", NULL, out, sizeof(out));
    assert(strcmp(out, TP_HOME TP_SEP "x") == 0);

    /* The buffer bound holds: the cut is never past the end. */
    char tiny[6];
    player_display_text_with_home(TP_HOME TP_SEP "AppData", home, tiny, sizeof(tiny));
    assert(strlen(tiny) < sizeof(tiny));
    assert(strcmp(tiny, "~" TP_SEP "App") == 0);
#undef TP_HOME
#undef TP_OTHER
#undef TP_SEP

#if defined(_WIN32) || defined(_WIN64)
    /* Windows paths compare case-insensitively and accept either separator. */
    player_display_text_with_home("c:/PATH/SYNTHETIC-USER/Games/disc.iso", home,
                                  out, sizeof(out));
    assert(strcmp(out, "~/Games/disc.iso") == 0);
#endif

    printf("[PLAYER_STATE_TEST] display path hides the profile folder PASS\n");
}

/* A library card says what is true of the title's package now. A title whose
 * package has not been checked yet is "checking", never "Not prepared"; a
 * package that cannot be used says why, with the validator's first sentence. */
static void test_library_card_status_text(void) {
    char out[256];
    size_t n = player_library_status_text(true, false, false, false, 0, NULL,
                                          out, sizeof(out));
    assert(strcmp(out, "Status: Prepared") == 0);
    assert(n == strlen(out));

    player_library_status_text(false, true, false, false, 0, NULL, out, sizeof(out));
    assert(strcmp(out, "Status: Checking package...") == 0);

    /* The reason from a copied library: the identity record is missing. */
    const char *reason =
        "Title input identity record is missing or unreadable; source media cannot be qualified. "
        "Cache component/epoch mismatch or incomplete entry; build it from the library.";
    player_library_status_text(false, false, false, false, 0, reason, out, sizeof(out));
    assert(strcmp(out, "Not prepared: Title input identity record is missing or unreadable; "
                       "source media cannot be qualified") == 0);

    player_library_status_text(false, false, false, false, 0,
                               "Runtime package is missing.", out, sizeof(out));
    assert(strcmp(out, "Not prepared: Runtime package is missing") == 0);

    player_library_status_text(false, false, true, false, 0,
                               "Runtime package is missing.", out, sizeof(out));
    assert(strcmp(out, "Check failed: Runtime package is missing") == 0);

    player_library_status_text(false, false, false, false, 0, NULL, out, sizeof(out));
    assert(strcmp(out, "Status: Not prepared") == 0);

    /* Staged assets keep their count, and say why the runtime is still missing. */
    player_library_status_text(false, false, false, true, 12, NULL, out, sizeof(out));
    assert(strcmp(out, "Assets staged: 12") == 0);
    player_library_status_text(false, false, false, true, 12,
                               "Runtime package is missing.", out, sizeof(out));
    assert(strcmp(out, "Assets staged: 12. Runtime package is missing") == 0);

    /* A small buffer is never overrun. */
    char tiny[8];
    n = player_library_status_text(true, false, false, false, 0, NULL, tiny, sizeof(tiny));
    assert(strcmp(tiny, "Status:") == 0);
    assert(n == strlen(tiny));

    printf("[PLAYER_STATE_TEST] library card status text PASS\n");
}

/* The bundled sample entries (the UI fixtures from player_app_populate_sample_games)
 * live in memory only. Every library save writes library.json, so a save made for
 * a real title (an add, or the last-played record after a launch) must leave the
 * samples out of the file. The file is redirected to a disposable path. */
static void test_sample_entries_never_reach_library_json(void) {
    printf("[PLAYER_STATE_TEST] Subtest: sample entries never reach library.json\n");
    char cache[512];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache, sizeof(cache)));
    char path[1024];
    snprintf(path, sizeof(path), "%s%csample_isolation.json", cache,
             nk_platform_path_separator());
    remove(path);

    PlayerApp *app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(app != NULL);
    nk_library_init(&app->library);
    assert(strlen(path) < sizeof(app->library.library_path));
    strcpy(app->library.library_path, path);

    /* The samples enter the in-memory library, as they do for --demo runs. */
    player_app_populate_sample_games(app);
    assert(app->library.count >= 2);

    /* A real title is added, which saves the library. */
    GameRecord user;
    memset(&user, 0, sizeof(user));
    seed_entry(&user, "TEST00041", "Synthetic User Title");
    assert(player_app_add_game(app, &user));

    NkLibrary loaded;
    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, path) == NK_OK);
    assert(nk_library_count(&loaded) == 1);
    assert(nk_library_find_by_disc_id(&loaded, "TEST00041") != NULL);
    assert(nk_library_find_by_disc_id(&loaded, "TEST00005") == NULL);
    assert(nk_library_find_by_disc_id(&loaded, "TEST00006") == NULL);

    remove(path);
    free(app);
    printf("[PLAYER_STATE_TEST] sample entries stay out of library.json PASS\n");
}

static int player_state_package_builder_child(int argc, char **argv) {
    if (argc < 4 || strcmp(argv[2], "build-package") != 0) return 90;
    const char *disc_id = argv[3];
    const char *progress_path = NULL;
    const char *user_root = NULL;
    for (int i = 4; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--progress-json") == 0) progress_path = argv[++i];
        else if (strcmp(argv[i], "--user-data-root") == 0) user_root = argv[++i];
    }
    if (!progress_path || !user_root) return 91;

    const char *mode = getenv("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE");
    if (mode && strcmp(mode, "success") == 0) {
        const char *title_id = getenv("NAKAGAWA_TEST_PACKAGE_REBUILD_TITLE_ID");
        const char *source_media = getenv("NAKAGAWA_TEST_PACKAGE_REBUILD_SOURCE_MEDIA");
        if (!title_id || !title_id[0]) return 92;
        char executable[128];
        snprintf(executable, sizeof(executable), "%s.exe", title_id);
        if (source_media && source_media[0]) {
            write_runtime_package_fixture_with_source_media(
                user_root, disc_id, title_id, SR_CPUSTATE_ABI_VERSION,
                executable, FIXTURE_SHA256, NULL, source_media);
        } else {
            write_runtime_package_fixture(user_root, disc_id, title_id,
                                          SR_CPUSTATE_ABI_VERSION, executable,
                                          FIXTURE_SHA256, NULL);
        }
        write_text_file(progress_path,
            "{\"stage\":\"package\",\"status\":\"PASS\","
            "\"message\":\"Synthetic rebuild completed.\"}\n");
        return 0;
    }
    if (mode && strcmp(mode, "reuse") == 0) {
        write_text_file(progress_path,
            "{\"stage\":\"package\",\"status\":\"PASS\","
            "\"message\":\"Package reused.\"}\n");
        return 0;
    }
    if (mode && strcmp(mode, "failure") == 0) {
        write_text_file(progress_path,
            "{\"stage\":\"compile\",\"status\":\"FAIL\","
            "\"message\":\"Synthetic rebuild failed before package promotion.\"}\n");
        return 17;
    }
    return 93;
}

static void append_test_text_file(const char *path, const char *text) {
    FILE *file = fopen(path, "ab");
    assert(file != NULL);
    size_t length = strlen(text);
    assert(fwrite(text, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static void poll_player_package_build_to_terminal(PlayerApp *app) {
    assert(app != NULL);
    for (unsigned i = 0; i < 1000 && app->build_session.is_building; i++) {
        player_app_poll_package_build(app, UINT64_C(2000) + i);
        if (app->build_session.is_building &&
            app->build_session.process.is_active &&
            nk_platform_is_process_running(&app->build_session.process)) {
#if defined(_WIN32) || defined(_WIN64)
            Sleep(10);
#else
            const struct timespec delay = { 0, 10000000L };
            (void)nanosleep(&delay, NULL);
#endif
        }
    }
    if (app->build_session.is_building) {
        fprintf(stderr,
                "package test child still active: process_active=%d running=%d "
                "complete=%d failed=%d exit=%d stage=%s message=%s offset=%lld\n",
                app->build_session.process.is_active ? 1 : 0,
                app->build_session.process.is_active
                    ? nk_platform_is_process_running(&app->build_session.process) : 0,
                app->build_session.is_complete ? 1 : 0,
                app->build_session.is_failed ? 1 : 0,
                app->build_session.exit_code,
                app->build_session.current_stage_name,
                app->build_session.current_message,
                (long long)app->build_session.progress_file_offset);
    }
    assert(!app->build_session.is_building);
}

/* #1282, #1286, #1280: the cards say what happened in plain words. Codes and
   raw boundaries go to the details, the developer log and the copied report,
   never the badge or the headline. */
static void test_error_cards_are_plain(void) {
    /* The reason table, one place for every stop boundary. */
    assert(strcmp(player_runtime_stop_feature("unsupported-import-scenetadhocinit"),
                  "ad hoc wireless play") == 0);
    assert(strcmp(player_runtime_stop_feature("unsupported-import-scenetinit"),
                  "online features") == 0);
    assert(strcmp(player_runtime_stop_feature("unsupported-import-scehttpinit"),
                  "online features") == 0);
    assert(strcmp(player_runtime_stop_feature("unsupported-import-sceatracgetsecondbufferinfo"),
                  "a sound feature") == 0);
    assert(strcmp(player_runtime_stop_feature("unsupported-import-scesascore"),
                  "a sound feature") == 0);
    assert(strcmp(player_runtime_stop_feature("unsupported-import-scempegcreate"),
                  "video playback") == 0);
    assert(strcmp(player_runtime_stop_feature("unsupported-import-scepsmfgetnumberofstreams"),
                  "video playback") == 0);
    assert(strcmp(player_runtime_stop_feature("unsupported-import-sceutilitysavedatainitstart"),
                  "a system dialog") == 0);
    assert(strcmp(player_runtime_stop_feature("unsupported-interpreter-form"),
                  "a processor feature") == 0);
    assert(strcmp(player_runtime_stop_feature("host-fiber-stack-exhausted"),
                  "a processor feature") == 0);
    assert(strcmp(player_runtime_stop_feature("unsupported-import-scekernelfoo"),
                  "a system feature") == 0);
    assert(strcmp(player_runtime_stop_feature("some-new-boundary"), "a system feature") == 0);
    assert(strcmp(player_runtime_stop_feature(NULL), "a system feature") == 0);

    assert(strcmp(player_plain_reason("[STAGE_BUSY] Let that finish."), "Let that finish.") == 0);
    assert(strcmp(player_plain_reason("ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted."),
                  "EBOOT.BIN is encrypted.") == 0);
    assert(strcmp(player_plain_reason("EBOOT.BIN: plain text"), "EBOOT.BIN: plain text") == 0);
    assert(strcmp(player_plain_reason("OK: short"), "OK: short") == 0);

    PlayerApp *app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(app != NULL);
    nk_library_init(&app->library);
    snprintf(app->games[0].disc_id, sizeof(app->games[0].disc_id), "TEST00042");
    snprintf(app->games[0].title_name, sizeof(app->games[0].title_name), "Plain Card Fixture");
    app->game_count = 1;

    /* An unknown code gets a plain badge, never the code. */
    player_app_set_error(app, "SOME_INTERNAL_CODE", "Something Went Wrong", "Plain words.",
                         "Return to Library", VIEW_LIBRARY);
    assert(strcmp(player_error_badge(&app->last_error), "Problem") == 0);
    assert(player_error_retry_label(&app->last_error) == NULL);
    assert(!player_app_retry_from_error(app));

    /* The stop card and Copy details. */
    player_dev_log_add(app, "runtime",
                       "BOOT_EVENT phase=boot reason=entry");
    player_dev_log_add(app, "runtime",
                       "BOOT_EVENT phase=stop reason=semantic-boundary "
                       "boundary=unsupported-import-scenetadhocinit issue=308");
    player_app_set_game_stop_error(app, "RUNTIME_SEMANTIC_BOUNDARY",
                                   "unsupported-import-scenetadhocinit", 308u, 9, 9000u,
                                   "TEST00042", "C:/logs/boot_TEST00042.log");
    assert(strcmp(app->last_error.title, "Game Stopped Early") == 0);
    assert(strcmp(player_error_badge(&app->last_error), "Game stopped") == 0);
    assert(strstr(app->last_error.message, "ad hoc wireless play") != NULL);
    assert(strstr(app->last_error.message, "unsupported-") == NULL);
    assert(strstr(app->last_error.boundary_text,
                  "unsupported-import-scenetadhocinit") != NULL);
    assert(strcmp(player_error_retry_label(&app->last_error), "Restart game") == 0);
    char report[PLAYER_ERROR_DETAILS_MAX + 4096];
    assert(player_app_format_error_report(app, "v0.0.1", report, sizeof(report)) > 0);
    assert(strstr(report, "Nakagawa Recomp v0.0.1\n") != NULL);
    assert(strstr(report, "Problem: Game Stopped Early\n") != NULL);
    assert(strstr(report, "Code: RUNTIME_SEMANTIC_BOUNDARY\n") != NULL);
    assert(strstr(report, "Game: Plain Card Fixture\n") != NULL);
    assert(strstr(report, "Disc ID: TEST00042\n") != NULL);
    assert(strstr(report, "Reason: Stopped at unsupported-import-scenetadhocinit") != NULL);
    assert(strstr(report, "Log file: C:/logs/boot_TEST00042.log\n") != NULL);
    assert(strstr(report, "Last boot events:\n  BOOT_EVENT phase=boot reason=entry\n"
                          "  BOOT_EVENT phase=stop") != NULL);
    /* Only the user's own log location: no other path, no profile folder. */
    assert(strchr(report, '\\') == NULL);
    assert(strstr(report, "C:/") == strstr(report, "Log file: C:/") + 10);
    const char *profile = getenv("USERPROFILE");
    if (!profile || !profile[0]) profile = getenv("HOME");
    if (profile && strlen(profile) > 3) assert(strstr(report, profile) == NULL);

    /* Restart game asks the main loop to relaunch the same game off the UI
       thread (#1298); here its package is missing, so the launch says so. */
    assert(!app->request_launch);
    assert(player_app_retry_from_error(app));
    assert(app->request_launch && app->request_launch_index == 0);
    app->request_launch = false;
    assert(!player_app_launch_game(app, app->request_launch_index));
    assert(strcmp(app->last_error.error_code, "RUNTIME_PACKAGE_NOT_READY") == 0);
    assert(app->selected_game_index == 0);

    /* The build cards. */
    package_builder_init_session(&app->build_session, "TEST00042", "Plain Card Fixture");
    player_app_set_build_error(app, "compile", "PACKAGE_BUILD_FAILED: build exited with code 2",
                               NULL);
    assert(strcmp(app->last_error.title, "Couldn't Finish Building") == 0);
    assert(strstr(app->last_error.message, "PACKAGE_BUILD_FAILED") == NULL);
    assert(strcmp(player_error_retry_label(&app->last_error), "Try again") == 0);
    assert(player_error_leads_with_message(&app->last_error));
    player_app_set_build_error(app, "package",
                               "PACKAGE_DISK_FULL: At least 1,234 bytes are needed for "
                               "the package files; 56 bytes are free.", NULL);
    assert(strcmp(app->last_error.error_code, "PACKAGE_DISK_FULL") == 0);
    assert(strcmp(app->last_error.title, "Not Enough Disk Space") == 0);
    assert(strstr(app->last_error.message, "1,234 bytes") != NULL);
    assert(strstr(app->last_error.message, "56 bytes") != NULL);
    assert(strstr(app->last_error.message, "PACKAGE_DISK_FULL") == NULL);
    player_app_set_build_error(app, "package",
                               "[Errno 28] No space left on device: 'package.staging'", NULL);
    assert(strcmp(app->last_error.error_code, "PACKAGE_BUILD_FAILED") == 0);
    assert(strcmp(app->last_error.title, "Couldn't Finish Building") == 0);
    assert(strstr(app->last_error.message, "Compilation ran out of disk space") != NULL);
    assert(strstr(app->last_error.message, "package size could be measured") != NULL);
    assert(strcmp(player_error_retry_label(&app->last_error), "Try again") == 0);
    player_app_set_build_error(app, "preflight",
                               "Build refused: TEST00042 is already being built by another "
                               "Nakagawa window.", NULL);
    assert(strcmp(app->last_error.error_code, "PACKAGE_BUILD_REFUSED") == 0);
    assert(strcmp(player_error_badge(&app->last_error), "Build not started") == 0);
    assert(strstr(app->last_error.message, "already being built") != NULL);
    assert(strstr(app->last_error.details, "check that runs before a build") != NULL);

    /* Preflight rows in plain words (#1280). */
    PlayerCompatibilityPreflight preflight;
    memset(&preflight, 0, sizeof(preflight));
    PlayerPreflightCheck *check = &preflight.checks[preflight.count++];
    snprintf(check->code, sizeof(check->code), "RUNTIME_PACKAGE");
    check->status = PREFLIGHT_MISSING;
    snprintf(check->message, sizeof(check->message),
             "Cache component/epoch mismatch or incomplete entry");
    assert(strcmp(player_preflight_label(check), "Game build") == 0);
    assert(strcmp(player_preflight_status_word(check), "Not built yet") == 0);
    assert(strstr(player_preflight_plain_message(check), "epoch") == NULL);
    assert(player_preflight_blocking_check(&preflight) == NULL);
    check = &preflight.checks[preflight.count++];
    snprintf(check->code, sizeof(check->code), "SYSTEM_FONTS");
    check->status = PREFLIGHT_MISSING;
    snprintf(check->message, sizeof(check->message), "L1 missing ltn0.pgf L2 missing ltn1.pgf");
    assert(strstr(player_preflight_plain_message(check), "ltn0") == NULL);
    check = &preflight.checks[preflight.count++];
    snprintf(check->code, sizeof(check->code), "EXECUTABLE");
    check->status = PREFLIGHT_UNSUPPORTED;
    snprintf(check->message, sizeof(check->message),
             "ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted and built-in decryption stopped: KEY_REQUIRED.");
    assert(strcmp(player_preflight_label(check), "Game program") == 0);
    assert(strcmp(player_preflight_status_word(check), "Can't use") == 0);
    assert(strcmp(player_preflight_plain_message(check),
                  "Nakagawa can't use the game's program on this disc.") == 0);
    assert(player_preflight_blocking_check(&preflight) == check);
    /* A check with its own facts (the folder to put a file in) says them. */
    snprintf(check->plain, sizeof(check->plain), "Put a decrypted EBOOT.elf in C:/synthetic.");
    assert(strcmp(player_preflight_plain_message(check),
                  "Put a decrypted EBOOT.elf in C:/synthetic.") == 0);

    /* #1302: every code and status reads as a player sentence: no developer
       code, no empty name, and "Can't use" only where the disc is kept out. */
    {
        static const char *const codes[] = {
            "EXPERIMENTAL", "DISC_SFO", "EXECUTABLE", "GUEST_MODULES", "RUNTIME_PACKAGE",
            "DATA_ROOT", "SYSTEM_FONTS", "AUDIO_OUTPUT", "SOMETHING_NEW"
        };
        static const PlayerPreflightStatus statuses[] = {
            PREFLIGHT_OK, PREFLIGHT_MISSING, PREFLIGHT_INCOMPATIBLE, PREFLIGHT_STALE,
            PREFLIGHT_UNSUPPORTED, PREFLIGHT_IN_PROGRESS, PREFLIGHT_INVALID
        };
        PlayerPreflightCheck row;
        for (size_t c = 0; c < sizeof(codes) / sizeof(codes[0]); c++) {
            for (size_t s = 0; s < sizeof(statuses) / sizeof(statuses[0]); s++) {
                const char *message;
                const char *word;
                size_t length;
                memset(&row, 0, sizeof(row));
                snprintf(row.code, sizeof(row.code), "%s", codes[c]);
                row.status = statuses[s];
                snprintf(row.message, sizeof(row.message),
                         "SYNTHETIC_CODE: developer text with an empty name .");
                message = player_preflight_plain_message(&row);
                word = player_preflight_status_word(&row);
                length = strlen(message);
                assert(length > 1u && message[length - 1u] == '.');
                assert(strstr(message, "SYNTHETIC_CODE") == NULL);
                assert(strstr(message, "developer text") == NULL);
                assert(strstr(message, " .") == NULL);
                assert(strcmp(word, "Not supported") != 0);
                assert(player_preflight_label(&row)[0] != '\0');
                assert((strcmp(word, "Can't use") == 0) == player_preflight_check_blocks(&row));
            }
        }
    }

    free(app);
    printf("[PLAYER_STATE_TEST] error cards are plain, with their codes in the details PASS\n");
}

/* #1277: the main loop asks for the build outcome every frame. A failure card
   and a cancel's return to the Library are shown once per build session, so
   the card can be dismissed and the Library left. */
static void test_build_outcome_is_shown_once_per_session(void) {
    PlayerApp *app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(app != NULL);

    package_builder_init_session(&app->build_session, "ULUS99998", "Outcome fixture");
    app->build_session.is_failed = true;
    snprintf(app->build_session.current_stage_name,
             sizeof(app->build_session.current_stage_name), "compile");
    snprintf(app->build_session.failure_boundary,
             sizeof(app->build_session.failure_boundary), "Synthetic compile failure.");
    player_app_set_view(app, VIEW_BUILDING_PACKAGE);
    assert(player_app_apply_build_outcome(app));
    assert(app->active_view == VIEW_ERROR);
    assert(strcmp(app->last_error.error_code, "PACKAGE_BUILD_FAILED") == 0);
    assert(strcmp(app->last_error.failed_stage, "compile") == 0);
    player_app_set_view(app, app->last_error.return_view);
    for (int frame = 0; frame < 3; frame++) {
        assert(!player_app_apply_build_outcome(app));
        assert(app->active_view == VIEW_LIBRARY);
    }

    package_builder_init_session(&app->build_session, "ULUS99998", "Outcome fixture");
    app->build_session.is_cancelled = true;
    player_app_set_view(app, VIEW_BUILDING_PACKAGE);
    assert(player_app_apply_build_outcome(app));
    assert(app->active_view == VIEW_LIBRARY);
    player_app_set_view(app, VIEW_SETTINGS);
    for (int frame = 0; frame < 3; frame++) {
        assert(!player_app_apply_build_outcome(app));
        assert(app->active_view == VIEW_SETTINGS);
    }

    /* The prerequisite fetcher runs in a build session of its own; its card,
       not the package build card, reports how it ended. */
    package_builder_init_session(&app->build_session, "ULUS99998", "Outcome fixture");
    app->prerequisite_session_generation = app->build_session.session_generation;
    app->build_session.is_failed = true;
    player_app_prereq_fail(app, "PREREQUISITE_INSTALL_FAILED", "Synthetic download failure.");
    assert(!player_app_apply_build_outcome(app));
    assert(strcmp(app->last_error.error_code, "PREREQUISITE_INSTALL_FAILED") == 0);

    free(app);
    printf("[PLAYER_STATE_TEST] build failure and cancel are shown once per session PASS\n");
}

static void test_package_build_lifecycle(const char *self_executable) {
    assert(self_executable && self_executable[0]);
    char sep = nk_platform_path_separator();

    /* The same player-state poll path runs while a different view is active. */
    PlayerApp *poll_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(poll_app != NULL);
    char progress_path[NK_MAX_PATH * 2];
    snprintf(progress_path, sizeof(progress_path), "%s%cbuild-view-progress.jsonl",
             native_test_get_root(), sep);
    package_builder_init_session(&poll_app->build_session, "ULUS99998",
                                 "Build view polling fixture");
    poll_app->build_session.is_building = true;
    assert(strlen(progress_path) <
           sizeof(poll_app->build_session.progress_file_path));
    memcpy(poll_app->build_session.progress_file_path, progress_path,
           strlen(progress_path) + 1);
    write_text_file(progress_path,
        "{\"stage\":\"extract\",\"status\":\"RUNNING\","
        "\"message\":\"Extracting first module.\"}\n");
    player_app_set_view(poll_app, VIEW_BUILDING_PACKAGE);
    player_app_poll_package_build(poll_app, 1000);
    assert(poll_app->build_session.current_stage == PACKAGE_BUILD_STAGE_EXTRACT);
    assert(strcmp(poll_app->build_session.current_message,
                  "Extracting first module.") == 0);

    player_app_set_view(poll_app, VIEW_LIBRARY);
    append_test_text_file(progress_path,
        "{\"stage\":\"compile\",\"status\":\"RUNNING\","
        "\"message\":\"Compiling while away.\"}\n");
    player_app_poll_package_build(poll_app, 1025);
    assert(poll_app->build_session.current_stage == PACKAGE_BUILD_STAGE_COMPILE);
    assert(strcmp(poll_app->build_session.current_message,
                  "Compiling while away.") == 0);
    assert(poll_app->build_session.elapsed_ms == 25);

    player_app_set_view(poll_app, VIEW_BUILDING_PACKAGE);
    append_test_text_file(progress_path,
        "{\"stage\":\"compile\",\"status\":\"RUNNING\","
        "\"message\":\"Progress after returning.\"}\n");
    player_app_poll_package_build(poll_app, 1075);
    assert(strcmp(poll_app->build_session.current_message,
                  "Progress after returning.") == 0);
    assert(poll_app->build_session.elapsed_ms == 75);
    poll_app->build_session.is_building = false;
    remove(progress_path);
    free(poll_app);
    printf("[PLAYER_STATE_TEST] build polling continues while away from build view PASS\n");

    /* Drive the public rebuild action through the same package-builder child
       path. The synthetic child emits a current fixture on success and leaves
       the prior incompatible package byte-identical on failure. */
    char cache_root[NK_MAX_PATH];
    char rebuild_root[NATIVE_TEST_PATH_MAX + 64];
    char user_root[NATIVE_TEST_PATH_MAX + 96];
    char install_root[NATIVE_TEST_PATH_MAX + 96];
    char source_iso_path[NATIVE_TEST_PATH_MAX + 96];
    char decrypted_dir[NATIVE_TEST_PATH_MAX + 128];
    char decrypted_elf[NATIVE_TEST_PATH_MAX + 160];
    char source_media_json[512];
    char source_media_sha256[65];
    unsigned char encrypted_eboot[0x100] = { 0 };
    char install_tools[NATIVE_TEST_PATH_MAX + 128];
    char cli_path[NATIVE_TEST_PATH_MAX + 160];
    char prereq_bin[NATIVE_TEST_PATH_MAX + 160];
    char tool_path[NATIVE_TEST_PATH_MAX + 192];
    char package_path[NATIVE_TEST_PATH_MAX + 192];
    char incompatible_reason[2048];
    char package_hash_before[65];
    char package_hash_after[65];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_root, sizeof(cache_root)));
    snprintf(rebuild_root, sizeof(rebuild_root), "%s%crebuild-package-state",
             cache_root, sep);
    snprintf(user_root, sizeof(user_root), "%s%cuser", rebuild_root, sep);
    snprintf(install_root, sizeof(install_root), "%s%cinstall", rebuild_root, sep);
    snprintf(source_iso_path, sizeof(source_iso_path), "%s%csource.iso",
             rebuild_root, sep);
    snprintf(decrypted_dir, sizeof(decrypted_dir), "%s%ctitles%cULUS99998%cdecrypted",
             user_root, sep, sep, sep);
    snprintf(decrypted_elf, sizeof(decrypted_elf), "%s%cEBOOT.elf",
             decrypted_dir, sep);
    snprintf(install_tools, sizeof(install_tools), "%s%ctools", install_root, sep);
    snprintf(cli_path, sizeof(cli_path), "%s%cnk_cli.py", install_tools, sep);
    snprintf(prereq_bin, sizeof(prereq_bin),
             "%s%cprerequisites%cmsys64%cucrt64%cbin", user_root,
             sep, sep, sep, sep);
    assert(nk_platform_mkdir_p(user_root));
    assert(nk_platform_mkdir_p(install_tools));
    assert(nk_platform_mkdir_p(prereq_bin));
    write_text_file(cli_path, "# native state test child entry point\n");
#if defined(_WIN32) || defined(_WIN64)
    snprintf(tool_path, sizeof(tool_path), "%s%cgcc.exe", prereq_bin, sep);
    write_text_file(tool_path, "fixture\n");
    snprintf(tool_path, sizeof(tool_path), "%s%cmingw32-make.exe", prereq_bin, sep);
#else
    snprintf(tool_path, sizeof(tool_path), "%s%cgcc", prereq_bin, sep);
    write_text_file(tool_path, "fixture\n");
    snprintf(tool_path, sizeof(tool_path), "%s%c%s", prereq_bin, sep,
             "mingw32-make");
#endif
    write_text_file(tool_path, "fixture\n");

    PlayerApp *rebuild_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(rebuild_app != NULL);
    assert(strlen(user_root) < sizeof(rebuild_app->runtime_root));
    memcpy(rebuild_app->runtime_root, user_root, strlen(user_root) + 1);
    assert(strlen(install_root) < sizeof(rebuild_app->install_root));
    memcpy(rebuild_app->install_root, install_root, strlen(install_root) + 1);
    rebuild_app->game_count = 1;
    snprintf(rebuild_app->games[0].disc_id,
             sizeof(rebuild_app->games[0].disc_id), "ULUS99998");
    snprintf(rebuild_app->games[0].title_id,
             sizeof(rebuild_app->games[0].title_id), "experimental-ulus99998");
    snprintf(rebuild_app->games[0].title_name,
             sizeof(rebuild_app->games[0].title_name), "Rebuild fixture");
    snprintf(rebuild_app->games[0].selected_executable,
             sizeof(rebuild_app->games[0].selected_executable),
             "EBOOT.BIN");
    snprintf(rebuild_app->games[0].disc_version,
             sizeof(rebuild_app->games[0].disc_version), "1.00");
    assert(strlen(source_iso_path) < sizeof(rebuild_app->games[0].iso_path));
    memcpy(rebuild_app->games[0].iso_path, source_iso_path,
           strlen(source_iso_path) + 1u);
    write_player_package_preflight_iso(source_iso_path, "ULUS99998",
                                       "Rebuild fixture", true);
    memcpy(encrypted_eboot, "~PSP", 4);
    encrypted_eboot[0x27] = 1;
    encrypted_eboot[0x55] = 0x10;
    fixture_sha_bytes(encrypted_eboot, sizeof(encrypted_eboot),
                      source_media_sha256);
    int source_media_length = snprintf(source_media_json, sizeof(source_media_json),
        "{\"executable\":{\"path\":\"PSP_GAME/SYSDIR/EBOOT.BIN\","
        "\"sha256\":\"%s\"},\"modules\":[]}", source_media_sha256);
    assert(source_media_length > 0 &&
           (size_t)source_media_length < sizeof(source_media_json));
    rebuild_app->games[0].iso_size_bytes =
        (uint64_t)nk_platform_get_file_size(source_iso_path);
    rebuild_app->games[0].executable_eboot_kind = NK_ISO_EXEC_PSP_ENCRYPTED;
    rebuild_app->games[0].executable_selection = NK_ISO_EXEC_SELECTION_NONE;
    rebuild_app->games[0].is_experimental = true;
    assert(nk_platform_mkdir_p(decrypted_dir));
    write_synthetic_mips_elf(decrypted_elf);

    bool had_python = false;
    bool had_build_mode = false;
    bool had_title_id = false;
    bool had_source_media = false;
    bool had_install_override = false;
    char *saved_python = capture_environment_value("PYTHON", &had_python);
    char *saved_build_mode = capture_environment_value(
        "NAKAGAWA_TEST_PACKAGE_REBUILD_MODE", &had_build_mode);
    char *saved_title_id = capture_environment_value(
        "NAKAGAWA_TEST_PACKAGE_REBUILD_TITLE_ID", &had_title_id);
    char *saved_source_media = capture_environment_value(
        "NAKAGAWA_TEST_PACKAGE_REBUILD_SOURCE_MEDIA", &had_source_media);
    char *saved_install_override = capture_environment_value(
        "NK_INSTALL_ROOT", &had_install_override);
    nk_ps_set_env("PYTHON", self_executable);
    nk_ps_set_env("NAKAGAWA_TEST_PACKAGE_REBUILD_TITLE_ID",
                  rebuild_app->games[0].title_id);
    nk_ps_set_env("NAKAGAWA_TEST_PACKAGE_REBUILD_SOURCE_MEDIA",
                  source_media_json);
    nk_ps_set_env("NK_INSTALL_ROOT", NULL);

    uint32_t incompatible_abi = SR_CPUSTATE_ABI_VERSION + 100u;
    write_experimental_profile_fixture(
        user_root, rebuild_app->games[0].disc_id,
        rebuild_app->games[0].title_id,
        rebuild_app->games[0].selected_executable, FIXTURE_SHA256);
    write_runtime_package_fixture_with_source_media(
        user_root, rebuild_app->games[0].disc_id,
        rebuild_app->games[0].title_id, incompatible_abi,
        "experimental-ulus99998.exe", FIXTURE_SHA256, NULL,
        source_media_json);
    NkRuntimePackageStatus rebuild_status = player_app_validate_runtime_package(
        rebuild_app, &rebuild_app->games[0], NULL,
        incompatible_reason, sizeof(incompatible_reason));
    if (rebuild_status != NK_RUNTIME_PACKAGE_INCOMPATIBLE) {
        fprintf(stderr, "rebuild fixture status=%s reason=%s\n",
                runtime_package_status_name(rebuild_status), incompatible_reason);
    }
    assert(rebuild_status == NK_RUNTIME_PACKAGE_INCOMPATIBLE);
    nk_ps_set_env("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE", "success");
    bool rebuild_started = player_app_rebuild_package(rebuild_app, 0);
    if (!rebuild_started) {
        fprintf(stderr, "rebuild action refused: %s: %s\n",
                rebuild_app->last_error.error_code,
                rebuild_app->last_error.message);
    }
    assert(rebuild_started);
    assert(rebuild_app->active_view == VIEW_BUILDING_PACKAGE);
    poll_player_package_build_to_terminal(rebuild_app);
    assert(rebuild_app->build_session.is_complete);
    player_app_runtime_package_cache_invalidate(rebuild_app);
    assert(player_app_validate_runtime_package(
               rebuild_app, &rebuild_app->games[0], NULL,
               incompatible_reason, sizeof(incompatible_reason)) ==
           NK_RUNTIME_PACKAGE_OK);

    write_runtime_package_fixture_with_source_media(
        user_root, rebuild_app->games[0].disc_id,
        rebuild_app->games[0].title_id, incompatible_abi,
        "experimental-ulus99998.exe", FIXTURE_SHA256, NULL,
        source_media_json);
    player_app_runtime_package_cache_invalidate(rebuild_app);
    assert(player_app_validate_runtime_package(
               rebuild_app, &rebuild_app->games[0], NULL,
               incompatible_reason, sizeof(incompatible_reason)) ==
           NK_RUNTIME_PACKAGE_INCOMPATIBLE);
    snprintf(package_path, sizeof(package_path), "%s%cpackages%c%s%cpackage.json",
             user_root, sep, sep, rebuild_app->games[0].disc_id, sep);
    fixture_sha_file(package_path, package_hash_before);
    nk_ps_set_env("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE", "failure");
    assert(player_app_rebuild_package(rebuild_app, 0));
    poll_player_package_build_to_terminal(rebuild_app);
    assert(rebuild_app->build_session.is_failed);
    fixture_sha_file(package_path, package_hash_after);
    assert(strcmp(package_hash_before, package_hash_after) == 0);
    player_app_runtime_package_cache_invalidate(rebuild_app);
    assert(player_app_validate_runtime_package(
               rebuild_app, &rebuild_app->games[0], NULL,
               incompatible_reason, sizeof(incompatible_reason)) ==
           NK_RUNTIME_PACKAGE_INCOMPATIBLE);

    /* #1276: a disc imported while its EBOOT.BIN was encrypted was saved with
       no selected executable. The build (with a user-supplied EBOOT.elf)
       binds EBOOT.BIN in the profile and passes, so the check after it must
       pass too and the title must end READY, not on a failure card. */
    char library_path[NATIVE_TEST_PATH_MAX + 192];
    snprintf(library_path, sizeof(library_path), "%s%clibrary.json", user_root, sep);
    nk_library_init(&rebuild_app->library);
    assert(strlen(library_path) < sizeof(rebuild_app->library.library_path));
    memcpy(rebuild_app->library.library_path, library_path, strlen(library_path) + 1);
    rebuild_app->games[0].selected_executable[0] = '\0';
    assert(nk_library_add_or_update(&rebuild_app->library, &rebuild_app->games[0]) == NK_OK);
    assert(nk_library_save(&rebuild_app->library, library_path) == NK_OK);
    nk_ps_set_env("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE", "success");
    assert(player_app_start_package_build(rebuild_app, 0));
    poll_player_package_build_to_terminal(rebuild_app);
    assert(rebuild_app->build_session.is_complete);
    assert(!player_app_apply_build_outcome(rebuild_app));
    assert(rebuild_app->active_view == VIEW_BUILDING_PACKAGE);
    player_app_runtime_package_cache_invalidate(rebuild_app);
    /* The empty selection is what the check used to run with: it refused the
       package it had just built. */
    assert(player_app_validate_runtime_package(
               rebuild_app, &rebuild_app->games[0], NULL,
               incompatible_reason, sizeof(incompatible_reason)) ==
           NK_RUNTIME_PACKAGE_STALE);
    assert(player_app_game_data_root_status(
               rebuild_app, &rebuild_app->games[0], NULL, 0, NULL, 0) ==
           NK_LAUNCH_DATA_ROOT_INVALID);
    assert(player_app_adopt_profile_executable(rebuild_app, 0));
    assert(strcmp(rebuild_app->games[0].selected_executable, "EBOOT.BIN") == 0);
    assert(!player_app_adopt_profile_executable(rebuild_app, 0));
    assert(player_app_validate_runtime_package(
               rebuild_app, &rebuild_app->games[0], NULL,
               incompatible_reason, sizeof(incompatible_reason)) ==
           NK_RUNTIME_PACKAGE_OK);
    assert(player_app_game_data_root_status(
               rebuild_app, &rebuild_app->games[0], NULL, 0, NULL, 0) !=
           NK_LAUNCH_DATA_ROOT_INVALID);
    NkLibrary *reloaded = (NkLibrary *)calloc(1, sizeof(NkLibrary));
    assert(reloaded != NULL);
    nk_library_init(reloaded);
    assert(nk_library_load(reloaded, library_path) == NK_OK);
    assert(reloaded->count == 1);
    assert(strcmp(reloaded->entries[0].selected_executable, "EBOOT.BIN") == 0);
    free(reloaded);

    /* Public synthetic reliability gate: ten BUILD PACKAGE sessions from a
       fresh per-run profile, then an incompatible-package REBUILD, then one
       more build that the package layer reports as reuse. Every run must
       finish without a failure card and leave a package the native validator
       accepts. Hosted native-core-tests runs this same source-owned sequence. */
    for (unsigned int run = 0; run < 10; ++run) {
        nk_ps_set_env("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE",
                      run == 0 ? "success" : "reuse");
        rebuild_app->last_error.error_code[0] = '\0';
        rebuild_app->active_view = VIEW_LIBRARY;
        assert(player_app_start_package_build(rebuild_app, 0));
        poll_player_package_build_to_terminal(rebuild_app);
        assert(rebuild_app->build_session.is_complete);
        assert(!rebuild_app->build_session.is_failed);
        assert(rebuild_app->active_view != VIEW_ERROR);
        player_app_runtime_package_cache_invalidate(rebuild_app);
        assert(player_app_validate_runtime_package(
                   rebuild_app, &rebuild_app->games[0], NULL,
                   incompatible_reason, sizeof(incompatible_reason)) ==
               NK_RUNTIME_PACKAGE_OK);
    }

    write_runtime_package_fixture_with_source_media(
        user_root, rebuild_app->games[0].disc_id,
        rebuild_app->games[0].title_id, incompatible_abi,
        "experimental-ulus99998.exe", FIXTURE_SHA256, NULL,
        source_media_json);
    player_app_runtime_package_cache_invalidate(rebuild_app);
    assert(player_app_validate_runtime_package(
               rebuild_app, &rebuild_app->games[0], NULL,
               incompatible_reason, sizeof(incompatible_reason)) ==
           NK_RUNTIME_PACKAGE_INCOMPATIBLE);
    nk_ps_set_env("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE", "success");
    rebuild_app->last_error.error_code[0] = '\0';
    rebuild_app->active_view = VIEW_LIBRARY;
    assert(player_app_rebuild_package(rebuild_app, 0));
    poll_player_package_build_to_terminal(rebuild_app);
    assert(rebuild_app->build_session.is_complete);
    assert(!rebuild_app->build_session.is_failed);
    assert(rebuild_app->active_view != VIEW_ERROR);
    player_app_runtime_package_cache_invalidate(rebuild_app);
    assert(player_app_validate_runtime_package(
               rebuild_app, &rebuild_app->games[0], NULL,
               incompatible_reason, sizeof(incompatible_reason)) ==
           NK_RUNTIME_PACKAGE_OK);

    nk_ps_set_env("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE", "reuse");
    rebuild_app->last_error.error_code[0] = '\0';
    rebuild_app->active_view = VIEW_LIBRARY;
    assert(player_app_start_package_build(rebuild_app, 0));
    poll_player_package_build_to_terminal(rebuild_app);
    assert(rebuild_app->build_session.is_complete);
    assert(!rebuild_app->build_session.is_failed);
    assert(rebuild_app->active_view != VIEW_ERROR);
    player_app_runtime_package_cache_invalidate(rebuild_app);
    assert(player_app_validate_runtime_package(
               rebuild_app, &rebuild_app->games[0], NULL,
               incompatible_reason, sizeof(incompatible_reason)) ==
           NK_RUNTIME_PACKAGE_OK);
    printf("[PLAYER_STATE_TEST] synthetic package reliability 10 builds, rebuild, reuse PASS\n");

    /* A profile that binds no executable gives the entry nothing to adopt. */
    write_experimental_profile_fixture(
        user_root, rebuild_app->games[0].disc_id,
        rebuild_app->games[0].title_id, "", FIXTURE_SHA256);
    rebuild_app->games[0].selected_executable[0] = '\0';
    assert(!player_app_adopt_profile_executable(rebuild_app, 0));
    assert(rebuild_app->games[0].selected_executable[0] == '\0');

    /* A check that fails after a passing build is named as such, with its
       internal code in the developer log, not as a failed build. */
    player_app_set_package_revalidation_error(
        rebuild_app, "Synthetic stale package.", rebuild_app->build_session.log_file_path);
    assert(rebuild_app->active_view == VIEW_ERROR);
    assert(strcmp(rebuild_app->last_error.error_code, "PACKAGE_REVALIDATION_FAILED") == 0);
    assert(strcmp(rebuild_app->last_error.title, "Package Build Failed") != 0);
    assert(rebuild_app->last_error.failed_stage[0] == '\0');
    /* #1286: the card says what happened in plain words; the checker's reason
       is the details. */
    assert(strstr(rebuild_app->last_error.message, "Synthetic stale package.") == NULL);
    assert(strstr(rebuild_app->last_error.message, "Try again") != NULL);
    assert(strcmp(rebuild_app->last_error.boundary_text, "Synthetic stale package.") == 0);
    assert(player_error_leads_with_message(&rebuild_app->last_error));
    bool logged_code = false;
    for (size_t i = 0; i < player_dev_log_count(rebuild_app); i++) {
        const char *line = player_dev_log_line(rebuild_app, i);
        if (line && strstr(line, "PACKAGE_REVALIDATION_FAILED: Synthetic stale package.")) {
            logged_code = true;
        }
    }
    assert(logged_code);
    printf("[PLAYER_STATE_TEST] experimental build adopts its profile executable PASS\n");

    free(rebuild_app);
    assert(test_remove_tree(rebuild_root));
    restore_environment_value("PYTHON", saved_python, had_python);
    restore_environment_value("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE",
                              saved_build_mode, had_build_mode);
    restore_environment_value("NAKAGAWA_TEST_PACKAGE_REBUILD_TITLE_ID",
                              saved_title_id, had_title_id);
    restore_environment_value("NAKAGAWA_TEST_PACKAGE_REBUILD_SOURCE_MEDIA",
                              saved_source_media, had_source_media);
    restore_environment_value("NK_INSTALL_ROOT", saved_install_override,
                              had_install_override);
    free(saved_python);
    free(saved_build_mode);
    free(saved_title_id);
    free(saved_source_media);
    free(saved_install_override);
    printf("[PLAYER_STATE_TEST] incompatible package rebuild success and failed-build preservation PASS\n");
}

/* An archive disc stages a deep tree (thirteen levels for one retail layout). The
 * discard walk used to keep 128 KiB of buffers on the stack per level and overflowed
 * the 1 MiB thread stack on Windows (0xC00000FD) while discarding such a tree. */
static void test_stage_discard_removes_a_deep_tree(void) {
    char root[NK_MAX_PATH];
    char path[NK_MAX_PATH * 4];
    char file[NK_MAX_PATH * 4 + 16];
    const char sep = nk_platform_path_separator();
    int written = snprintf(root, sizeof(root), "%s%c.staging_DEEP00001",
                           native_test_get_root(), sep);
    assert(written > 0 && (size_t)written < sizeof(root));
    snprintf(path, sizeof(path), "%s", root);
    for (int depth = 0; depth < 32; depth++) {
        size_t length = strlen(path);
        assert(length + 4 < sizeof(path));
        snprintf(path + length, sizeof(path) - length, "%cd%d", sep, depth % 10);
        assert(nk_platform_mkdir_p(path));
        written = snprintf(file, sizeof(file), "%s%cf.bin", path, sep);
        assert(written > 0 && (size_t)written < sizeof(file));
        write_text_file(file, "x");
    }
    assert(player_stage_discard(root));
    assert(!nk_platform_file_exists(file));
    assert(!nk_platform_dir_exists(root));
    printf("PASS: staging discard removes a 32-level tree without exhausting the stack\n");
}

/* Joins root and up to three parts with the platform separator. */
static void removal_test_path(char *out, size_t size, const char *root, const char *a,
                              const char *b, const char *c) {
    const char sep = nk_platform_path_separator();
    int n;
    if (c) n = snprintf(out, size, "%s%c%s%c%s%c%s", root, sep, a, sep, b, sep, c);
    else if (b) n = snprintf(out, size, "%s%c%s%c%s", root, sep, a, sep, b);
    else n = snprintf(out, size, "%s%c%s", root, sep, a);
    assert(n > 0 && (size_t)n < size);
}

/* A folder holding one file of the given size. */
static void removal_test_tree(const char *dir, const char *name, size_t bytes) {
    char file[NK_MAX_PATH * 2];
    assert(nk_platform_mkdir_p(dir));
    removal_test_path(file, sizeof(file), dir, name, NULL, NULL);
    FILE *out = fopen(file, "wb");
    assert(out != NULL);
    for (size_t i = 0; i < bytes; ++i) assert(fputc('x', out) != EOF);
    assert(fclose(out) == 0);
}

typedef struct {
    char build_root[NK_MAX_PATH];
    char data_root[NK_MAX_PATH];
    char saves_root[NK_MAX_PATH];
    char package[NK_MAX_PATH];
    char cache_packages[NK_MAX_PATH];
    char cache_bringup[NK_MAX_PATH];
    char games[NK_MAX_PATH];
    char memstick[NK_MAX_PATH];
    char staging[NK_MAX_PATH];
    char saves[NK_MAX_PATH];
    char decrypted[NK_MAX_PATH];
    char manifests[NK_MAX_PATH];
    char iso[NK_MAX_PATH];
} RemovalFixture;

/* Everything Nakagawa keeps for one disc, plus what it must never delete:
   the disc image, the user's decrypted files and the shared manifests. */
static void removal_fixture_make(RemovalFixture *f, const char *disc) {
    char name[64];
    memset(f, 0, sizeof(*f));
    removal_test_path(f->build_root, sizeof(f->build_root), native_test_get_root(),
                      "rt1304", NULL, NULL);
    assert(nk_platform_get_app_data_dir(f->data_root, sizeof(f->data_root)));
    assert(nk_platform_get_path(NK_PATH_SAVES, f->saves_root, sizeof(f->saves_root)));
    assert(native_test_path_within(f->data_root, native_test_get_root()));
    assert(native_test_path_within(f->saves_root, native_test_get_root()));
    removal_test_path(f->package, sizeof(f->package), f->build_root, "packages", disc, NULL);
    removal_test_path(f->cache_packages, sizeof(f->cache_packages), f->build_root, "cache",
                      "packages", disc);
    removal_test_path(f->cache_bringup, sizeof(f->cache_bringup), f->build_root, "cache",
                      "bringup", disc);
    removal_test_path(f->games, sizeof(f->games), f->data_root, "games", disc, NULL);
    removal_test_path(f->memstick, sizeof(f->memstick), f->games, "memstick", NULL, NULL);
    snprintf(name, sizeof(name), ".staging_%s", disc);
    removal_test_path(f->staging, sizeof(f->staging), f->data_root, "games", name, NULL);
    removal_test_path(f->saves, sizeof(f->saves), f->saves_root, disc, NULL, NULL);
    removal_test_path(f->decrypted, sizeof(f->decrypted), f->build_root, "titles", disc,
                      "decrypted");
    removal_test_path(f->manifests, sizeof(f->manifests), f->build_root, "manifests", NULL,
                      NULL);
    snprintf(name, sizeof(name), "%s.iso", disc);
    removal_test_path(f->iso, sizeof(f->iso), native_test_get_root(), name, NULL, NULL);
    removal_test_tree(f->package, "package.json", 3000);
    removal_test_tree(f->cache_packages, "objects.bin", 200);
    removal_test_tree(f->cache_bringup, "bringup.json", 100);
    removal_test_tree(f->games, "EBOOT.BIN", 1000);
    removal_test_tree(f->memstick, "DATA.BIN", 40);
    removal_test_tree(f->staging, "partial.bin", 7);
    removal_test_tree(f->saves, "SAVE.BIN", 2);
    removal_test_tree(f->decrypted, "EBOOT.BIN", 9);
    removal_test_tree(f->manifests, "title.json", 5);
    write_text_file(f->iso, "disc image");
}

static PlayerApp *removal_test_app(const RemovalFixture *f, const char *disc) {
    PlayerApp *app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    NkGameEntry entry;
    size_t root_length = strlen(f->build_root);
    assert(app != NULL);
    nk_library_init(&app->library);
    /* Joined straight into the field: a snprintf from a larger buffer fails
       -Wformat-truncation in optimized builds. */
    removal_test_path(app->library.library_path, sizeof(app->library.library_path),
                      f->build_root, "library1304.json", NULL, NULL);
    remove(app->library.library_path);
    assert(root_length < sizeof(app->runtime_root));
    memcpy(app->runtime_root, f->build_root, root_length + 1);
    seed_entry(&entry, disc, "Removal Fixture");
    snprintf(entry.iso_path, sizeof(entry.iso_path), "%s", f->iso);
    assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    seed_entry(&entry, "RMV01305", "Next Fixture");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    player_app_sync_library(app);
    assert(app->game_count == 2);
    assert(player_app_find_game_by_disc_id(app, disc) == 0);
    return app;
}

/* The tree helpers behind Remove from library walk a deep tree without
   running out of stack, keep the named top-level child, and delete a link as
   itself, never what it points at. */
static void test_tree_helpers_walk_a_deep_tree(void) {
    char root[NK_MAX_PATH];
    char path[NK_MAX_PATH * 4];
    char keep[NK_MAX_PATH * 2];
    uint64_t bytes = 0;
    const char sep = nk_platform_path_separator();
    removal_test_path(root, sizeof(root), native_test_get_root(), "tree1304", NULL, NULL);
    snprintf(path, sizeof(path), "%s", root);
    for (int depth = 0; depth < 48; depth++) {
        size_t length = strlen(path);
        assert(length + 4 < sizeof(path));
        snprintf(path + length, sizeof(path) - length, "%cd%d", sep, depth % 10);
        removal_test_tree(path, "f.bin", 3);
    }
    removal_test_path(keep, sizeof(keep), root, "memstick", NULL, NULL);
    removal_test_tree(keep, "SAVE.BIN", 5);
    assert(nk_platform_tree_size(root, NULL, &bytes) && bytes == 48u * 3u + 5u);
    assert(nk_platform_tree_size(root, "memstick", &bytes) && bytes == 48u * 3u);
    assert(nk_platform_remove_tree(root, "memstick"));
    assert(nk_platform_dir_exists(keep) && !nk_platform_path_missing(root));
    removal_test_path(path, sizeof(path), root, "d0", NULL, NULL);
    assert(!nk_platform_dir_exists(path));
#if !defined(_WIN32) && !defined(_WIN64)
    {
        /* Windows needs a privilege for a symlink, so only POSIX checks it. */
        char outside[NK_MAX_PATH * 2];
        char outside_file[NK_MAX_PATH * 2];
        char link[NK_MAX_PATH * 2];
        removal_test_path(outside, sizeof(outside), native_test_get_root(), "tree1304-outside",
                          NULL, NULL);
        removal_test_tree(outside, "KEEP.BIN", 4);
        removal_test_path(outside_file, sizeof(outside_file), outside, "KEEP.BIN", NULL, NULL);
        removal_test_path(link, sizeof(link), root, "link", NULL, NULL);
        assert(symlink(outside, link) == 0);
        assert(nk_platform_tree_size(root, "memstick", &bytes) && bytes == 0u);
        assert(nk_platform_remove_tree(root, NULL));
        assert(nk_platform_file_exists(outside_file));
    }
#else
    assert(nk_platform_remove_tree(root, NULL));
#endif
    assert(nk_platform_path_missing(root));
    assert(nk_platform_remove_tree(root, NULL));
    assert(nk_platform_tree_size(root, NULL, &bytes) && bytes == 0u);
    printf("PASS: tree size and removal walk a 48-level tree and keep the named child\n");
}

#if defined(_WIN32) || defined(_WIN64)
/* Walkthrough blocker #4: Remove from library on a package cache deeper than
   MAX_PATH. The player has no long-path manifest and a default Windows install
   has long paths off, so the plain path API could neither list nor delete the
   tree: removal failed after deleting the cache's ownership marker, leaving a
   game that could neither rebuild nor be removed. The tree is made here with
   extended-length paths, as the package build makes it. */
static bool long_tree_mkdir(const WCHAR *path) {
    return CreateDirectoryW(path, NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void long_tree_file(const WCHAR *dir, const WCHAR *name) {
    WCHAR file[2048];
    HANDLE handle;
    DWORD written = 0;
    _snwprintf(file, sizeof(file) / sizeof(file[0]), L"%ls\\%ls", dir, name);
    file[sizeof(file) / sizeof(file[0]) - 1u] = L'\0';
    handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(handle != INVALID_HANDLE_VALUE);
    assert(WriteFile(handle, "x", 1, &written, NULL) && written == 1);
    assert(CloseHandle(handle));
}

static void test_remove_tree_deeper_than_max_path(void) {
    char root[NK_MAX_PATH];
    char marker[NK_MAX_PATH * 2];
    WCHAR wroot[1024];
    WCHAR deep[2048];
    WCHAR held[2048];
    HANDLE open_file;
    removal_test_path(root, sizeof(root), native_test_get_root(), "cache-long-paths", NULL, NULL);
    assert(MultiByteToWideChar(CP_UTF8, 0, root, -1, wroot, 1024) > 0);
    for (WCHAR *c = wroot; *c; ++c) if (*c == L'/') *c = L'\\';
    _snwprintf(deep, sizeof(deep) / sizeof(deep[0]), L"\\\\?\\%ls", wroot);
    deep[sizeof(deep) / sizeof(deep[0]) - 1u] = L'\0';
    assert(long_tree_mkdir(deep));
    long_tree_file(deep, L".nk-aot-package-cache-v1");
    removal_test_path(marker, sizeof(marker), root, ".nk-aot-package-cache-v1", NULL, NULL);
    /* entries\<64 hex>\<64 hex>\package\.build-package-xxxxxxxx: the old
       layout, well past 260 characters. */
    {
        static const WCHAR *const parts[] = {
            L"entries",
            L"859fc2090e790b33178fbc2fac7cad1f4373b8329d93853391bbaec122ff47c4",
            L"b1d2d67321a0b2f2256602e755b7ba5ab15922486ecfb74cced10098475e51ac",
            L"package", L".build-package-9vu6x8d0"
        };
        for (size_t i = 0u; i < sizeof(parts) / sizeof(parts[0]); ++i) {
            size_t length = wcslen(deep);
            _snwprintf(deep + length, sizeof(deep) / sizeof(deep[0]) - length, L"\\%ls", parts[i]);
            deep[sizeof(deep) / sizeof(deep[0]) - 1u] = L'\0';
            assert(long_tree_mkdir(deep));
        }
    }
    long_tree_file(deep, L".title-config-0545df4ab68e93462714bd56795427e73b204d45fff70335feb0224c247cd081");

    /* A file still open stops the removal; the folder's own files, the
       ownership marker among them, are left for the next try. */
    _snwprintf(held, sizeof(held) / sizeof(held[0]),
               L"%ls\\.title-config-0545df4ab68e93462714bd56795427e73b204d45fff70335feb0224c247cd081",
               deep);
    held[sizeof(held) / sizeof(held[0]) - 1u] = L'\0';
    assert(wcslen(held) - 4u > 280u); /* well past MAX_PATH without the prefix */
    open_file = CreateFileW(held, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    assert(open_file != INVALID_HANDLE_VALUE);
    assert(!nk_platform_remove_tree(root, NULL));
    assert(nk_platform_file_exists(marker));
    assert(CloseHandle(open_file));

    assert(nk_platform_remove_tree(root, NULL));
    assert(nk_platform_path_missing(root));
    printf("PASS: removal deletes a cache tree deeper than MAX_PATH, marker last\n");
}
#endif

/* #1304: Remove from library deletes the package, the build cache and the disc
   files Nakagawa copied, keeps the saves unless asked, and never touches the
   disc image, the user's decrypted files or another title's data. */
static void test_remove_from_library_deletes_what_nakagawa_built(void) {
    RemovalFixture f;
    PlayerRemovalPlan plan;
    char error[NK_MAX_PATH + 128];
    removal_fixture_make(&f, "RMV01304");
    PlayerApp *app = removal_test_app(&f, "RMV01304");

    assert(player_app_plan_game_removal(app, 0, &plan));
    assert(plan.kind_present[PLAYER_REMOVAL_PACKAGE] &&
           plan.kind_bytes[PLAYER_REMOVAL_PACKAGE] == 3000u);
    assert(plan.kind_present[PLAYER_REMOVAL_BUILD_CACHE] &&
           plan.kind_bytes[PLAYER_REMOVAL_BUILD_CACHE] == 300u);
    /* The copied disc files without their Memory Stick folder, and the
       leftover staging folder. */
    assert(plan.kind_present[PLAYER_REMOVAL_EXTRACTED] &&
           plan.kind_bytes[PLAYER_REMOVAL_EXTRACTED] == 1007u);
    assert(plan.has_saves && plan.saves_bytes == 42u);
    for (size_t i = 0; i < plan.path_count; ++i) {
        assert(strcmp(plan.paths[i].path, f.iso) != 0);
        assert(!native_test_path_within(plan.paths[i].path, f.decrypted));
        assert(!native_test_path_within(plan.paths[i].path, f.manifests));
    }

    /* Nothing goes while the game starts, runs or builds. */
    app->launch_pending = true;
    assert(player_app_removal_blocker(app, 0) != NULL);
    assert(!player_app_remove_game_and_data(app, 0, false, error, sizeof(error)));
    assert(error[0] != '\0');
    app->launch_pending = false;
    app->is_game_running = true;
    assert(!player_app_plan_game_removal(app, 0, &plan));
    app->is_game_running = false;
    app->build_session.is_building = true;
    assert(player_app_removal_blocker(app, 0) != NULL);
    app->build_session.is_building = false;
    assert(nk_platform_dir_exists(f.package));

    assert(player_app_remove_game_and_data(app, 0, false, error, sizeof(error)));
    assert(app->game_count == 1 && strcmp(app->games[0].disc_id, "RMV01305") == 0);
    assert(!nk_platform_dir_exists(f.package));
    assert(!nk_platform_dir_exists(f.cache_packages));
    assert(!nk_platform_dir_exists(f.cache_bringup));
    assert(!nk_platform_dir_exists(f.staging));
    assert(nk_platform_dir_exists(f.games) && nk_platform_dir_exists(f.memstick));
    {
        char eboot[NK_MAX_PATH * 2];
        removal_test_path(eboot, sizeof(eboot), f.games, "EBOOT.BIN", NULL, NULL);
        assert(!nk_platform_file_exists(eboot));
    }
    assert(nk_platform_dir_exists(f.saves));
    assert(nk_platform_dir_exists(f.decrypted));
    assert(nk_platform_dir_exists(f.manifests));
    assert(nk_platform_file_exists(f.iso));
    remove(app->library.library_path);
    free(app);

    /* With the box ticked the saves go too, and so does the emptied folder. */
    removal_fixture_make(&f, "RMV01304");
    app = removal_test_app(&f, "RMV01304");
    assert(player_app_remove_game_and_data(app, 0, true, error, sizeof(error)));
    assert(!nk_platform_dir_exists(f.games));
    assert(!nk_platform_dir_exists(f.saves));
    assert(!nk_platform_dir_exists(f.package));
    assert(nk_platform_dir_exists(f.decrypted));
    assert(nk_platform_file_exists(f.iso));
    remove(app->library.library_path);

    /* A showcase demo and a disc ID that is not a plain folder name are
       refused before anything is planned. */
    snprintf(app->games[0].title_id, sizeof(app->games[0].title_id), "showcase-scene");
    assert(!player_app_plan_game_removal(app, 0, &plan));
    app->games[0].title_id[0] = '\0';
    snprintf(app->games[0].disc_id, sizeof(app->games[0].disc_id), "..");
    assert(player_app_removal_blocker(app, 0) != NULL);
    assert(!player_app_remove_game_and_data(app, 0, false, error, sizeof(error)));
    free(app);
    printf("PASS: remove from library deletes what Nakagawa built and keeps the disc "
           "image and, unless asked, the saves\n");
}

/* #1285: a release zip's verified prereqs/ tree beside bin/ runs BUILD PACKAGE
   with no consent card and no download, even with nothing on PATH; a damaged
   tree is refused by name and never falls back to a download. */
static void write_bundle_state_file(const char *base, const char *relative, const char *text) {
    char path[NATIVE_TEST_PATH_MAX + 128];
    char sep = nk_platform_path_separator();
    int n = snprintf(path, sizeof(path), "%s%c%s", base, sep, relative);
    assert(n > 0 && (size_t)n < sizeof(path));
    for (char *p = path + strlen(base); *p; p++) {
        if (*p == '/') *p = sep;
    }
    char parent[NATIVE_TEST_PATH_MAX + 128];
    memcpy(parent, path, strlen(path) + 1);
    *strrchr(parent, sep) = '\0';
    assert(nk_platform_mkdir_p(parent));
    write_text_file(path, text);
}

static void test_bundled_prerequisite_build(const char *self_executable) {
    printf("[PLAYER_STATE_TEST] bundled prerequisites: BUILD PACKAGE without consent or download\n");
    fflush(stdout);
    char sep = nk_platform_path_separator();
#if defined(_WIN32) || defined(_WIN64)
    const char *python_rel = "python/python.exe";
    const char *gcc_rel = "msys64/ucrt64/bin/gcc.exe";
    const char *make_rel = "msys64/ucrt64/bin/mingw32-make.exe";
#else
    const char *python_rel = "python/python";
    const char *gcc_rel = "msys64/ucrt64/bin/gcc";
    const char *make_rel = "msys64/ucrt64/bin/mingw32-make";
#endif
    char root[NK_MAX_PATH + 32];
    char release[NK_MAX_PATH + 48];
    char install_root[NK_MAX_PATH + 64];
    char prereqs[NK_MAX_PATH + 64];
    char user_root[NK_MAX_PATH + 48];
    char iso_path[NK_MAX_PATH + 48];
    char path[NK_MAX_PATH + 128];
    assert(nk_platform_get_path(NK_PATH_CACHE, root, NK_MAX_PATH));
    size_t root_len = strlen(root);
    snprintf(root + root_len, sizeof(root) - root_len, "%cbundle-build-state", sep);
    if (nk_platform_dir_exists(root)) assert(native_test_remove_tree(root));
    snprintf(release, sizeof(release), "%s%crelease", root, sep);
    snprintf(install_root, sizeof(install_root), "%s%cbin", release, sep);
    snprintf(prereqs, sizeof(prereqs), "%s%cprereqs", release, sep);
    snprintf(user_root, sizeof(user_root), "%s%cuser", root, sep);
    snprintf(iso_path, sizeof(iso_path), "%s%cbundle.iso", root, sep);
    assert(nk_platform_mkdir_p(install_root));
    assert(nk_platform_mkdir_p(user_root));

    write_bundle_state_file(release, "source/tools/nk_cli.py", "# synthetic CLI\n");
    write_bundle_state_file(release, "source/assets/prereq_manifest.json",
        "{\"schema_version\": 1, \"total_download_bytes\": 100, \"artifacts\": [\n"
        " {\"id\": \"cpython-embed-amd64\", \"name\": \"Synthetic CPython\", \"version\": \"3.15.1\",\n"
        "  \"url\": \"https://www.python.org/ftp/synthetic.zip\", \"allowed_hosts\": [\"www.python.org\"],\n"
        "  \"filename\": \"synthetic.zip\", \"size_bytes\": 100,\n"
        "  \"sha256\": \"1111111111111111111111111111111111111111111111111111111111111111\",\n"
        "  \"license\": \"PSF-2.0\", \"status\": \"ready\"}]}\n");
    write_bundle_state_file(prereqs, python_rel, "");
    snprintf(path, sizeof(path), "%s%c%s", prereqs, sep, python_rel);
    for (char *p = path + strlen(prereqs); *p; p++) if (*p == '/') *p = sep;
    copy_executable_file(self_executable, path);
    int64_t python_size = nk_platform_get_file_size(path);
    assert(python_size > 0);
    write_bundle_state_file(prereqs, gcc_rel, "gcc\n");
    write_bundle_state_file(prereqs, make_rel, "make\n");
    char inventory[NATIVE_TEST_PATH_MAX * 2];
    snprintf(inventory, sizeof(inventory),
             "%064d %lld %s\n%064d 4 %s\n%064d 5 %s\n",
             0, (long long)python_size, python_rel, 0, gcc_rel, 0, make_rel);
    write_bundle_state_file(prereqs, "bundle-inventory.txt", inventory);
    char record[1024];
    snprintf(record, sizeof(record),
             "{\"schema_version\": 1, \"bundle\": {\"schema_version\": 1, "
             "\"inventory\": \"bundle-inventory.txt\", \"file_count\": 3, \"unpacked_bytes\": %lld},\n"
             " \"installed\": {\"cpython-embed-amd64\": {\"version\": \"3.15.1\", \"size_bytes\": 100,\n"
             "  \"sha256\": \"1111111111111111111111111111111111111111111111111111111111111111\",\n"
             "  \"license\": \"PSF-2.0\", \"notices\": []}}}\n",
             (long long)python_size + 9);
    write_bundle_state_file(prereqs, "installed.json", record);

    PlayerApp *app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(app != NULL);
    nk_library_init(&app->library);
    app->game_count = 1;
    app->selected_game_index = 0;
    write_player_package_preflight_iso(iso_path, "TEST00011", "Bundle fixture", false);
    snprintf(app->games[0].disc_id, sizeof(app->games[0].disc_id), "TEST00011");
    snprintf(app->games[0].title_id, sizeof(app->games[0].title_id), "experimental-test00011");
    snprintf(app->games[0].title_name, sizeof(app->games[0].title_name), "Bundle fixture");
    snprintf(app->games[0].disc_version, sizeof(app->games[0].disc_version), "1.00");
    snprintf(app->games[0].selected_executable, sizeof(app->games[0].selected_executable),
             "EBOOT.BIN");
    assert(strlen(iso_path) < sizeof(app->games[0].iso_path));
    memcpy(app->games[0].iso_path, iso_path, strlen(iso_path) + 1u);
    app->games[0].iso_size_bytes = (uint64_t)nk_platform_get_file_size(iso_path);
    app->games[0].is_experimental = true;
    app->games[0].executable_eboot_kind = NK_ISO_EXEC_MIPS_ELF32;
    app->games[0].executable_selection = NK_ISO_EXEC_SELECTION_EBOOT;
    assert(strlen(user_root) < sizeof(app->runtime_root));
    assert(strlen(install_root) < sizeof(app->install_root));
    memcpy(app->runtime_root, user_root, strlen(user_root) + 1u);
    memcpy(app->install_root, install_root, strlen(install_root) + 1u);
    write_experimental_profile_fixture(user_root, app->games[0].disc_id, app->games[0].title_id,
                                       app->games[0].selected_executable, FIXTURE_SHA256);

    bool had_path = false;
    bool had_python = false;
    bool had_override = false;
    bool had_mode = false;
    char *saved_path = capture_environment_value("PATH", &had_path);
    char *saved_python = capture_environment_value("PYTHON", &had_python);
    char *saved_override = capture_environment_value("NK_INSTALL_ROOT", &had_override);
    char *saved_mode = capture_environment_value("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE", &had_mode);
    /* Nothing on the host can stand in for the bundle. */
    nk_ps_set_env("PATH", "");
#if defined(_WIN32) || defined(_WIN64)
    nk_ps_set_env("PYTHON", "C:/Windows/System32/where.exe");
#else
    nk_ps_set_env("PYTHON", "/bin/sh");
#endif
    nk_ps_set_env("NK_INSTALL_ROOT", NULL);
    nk_ps_set_env("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE", "reuse");

    bool started = player_app_start_package_build(app, 0);
    if (!started) {
        fprintf(stderr, "bundled build refused: %s: %s\n", app->last_error.error_code,
                app->last_error.message);
    }
    assert(started);
    assert(app->active_view == VIEW_BUILDING_PACKAGE);
    assert(app->prerequisites.phase == PLAYER_PREREQ_IDLE);
    assert(!app->prerequisite_job_started && !app->prerequisite_fetcher_started);
    char expected_root[NK_MAX_PATH];
    assert(nk_platform_absolute_path(prereqs, expected_root, sizeof(expected_root)));
    assert(strcmp(app->build_session.prerequisites_root, expected_root) == 0);
    poll_player_package_build_to_terminal(app);
    assert(app->build_session.is_complete);
    snprintf(path, sizeof(path), "%s%cprerequisites", user_root, sep);
    assert(!nk_platform_dir_exists(path));

    /* About & Licenses lists the bundle and opens its notices. */
    assert(player_app_open_prerequisite_about(app));
    assert(app->prerequisites.item_count == 1);
    assert(strcmp(app->prerequisites.bundle_root, expected_root) == 0);
    assert(player_app_request_license_folder(app));
    snprintf(path, sizeof(path), "%s%cnotices", expected_root, sep);
    assert(strcmp(app->requested_open_path, path) == 0);

    /* A record that no longer matches the pinned manifest is refused by name. */
    player_app_set_view(app, VIEW_LIBRARY);
    char *stale_version = strstr(record, "\"3.15.1\"");
    assert(stale_version != NULL);
    stale_version[6] = '0';
    write_bundle_state_file(prereqs, "installed.json", record);
    assert(!player_app_start_package_build(app, 0));
    assert(app->active_view == VIEW_ERROR);
    assert(strcmp(app->last_error.error_code, "PREREQUISITE_BUNDLE_INVALID") == 0);
    assert(strstr(app->last_error.message, "cpython-embed-amd64") != NULL);
    assert(app->prerequisites.phase != PLAYER_PREREQ_CONSENT);
    assert(!app->build_session.is_building);

    restore_environment_value("PATH", saved_path, had_path);
    restore_environment_value("PYTHON", saved_python, had_python);
    restore_environment_value("NK_INSTALL_ROOT", saved_override, had_override);
    restore_environment_value("NAKAGAWA_TEST_PACKAGE_REBUILD_MODE", saved_mode, had_mode);
    free(saved_path);
    free(saved_python);
    free(saved_override);
    free(saved_mode);
    free(app);
    assert(native_test_remove_tree(root));
    printf("[PLAYER_STATE_TEST] bundled prerequisites PASS\n");
}

/* ---- PSN releases of UMD games (#295 phase 2) ---------------------------- */

typedef struct {
    int calls;
    uint64_t done;
    uint64_t total;
} PsnTestProgress;

static void psn_test_progress(uint64_t done, uint64_t total, void *userdata) {
    PsnTestProgress *progress = (PsnTestProgress *)userdata;
    progress->calls++;
    progress->done = done;
    progress->total = total;
}

static bool psn_test_never(void *userdata) {
    (void)userdata;
    return false;
}

static bool psn_test_cancel_now(void *userdata) {
    (void)userdata;
    return true;
}

static uint8_t *psn_test_read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *bytes;
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    assert(length > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    bytes = (uint8_t *)malloc((size_t)length);
    assert(bytes != NULL);
    assert(fread(bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

/* A refused package: the named code, a plain sentence, nothing written. */
static void psn_test_refused(const char *path, const char *want_code, const char *want_text) {
    PlayerPsnPackage package;
    char code[64];
    char message[512];
    assert(!player_psn_package_inspect(path, &package, code, sizeof(code),
                                       message, sizeof(message)));
    assert(strcmp(code, want_code) == 0);
    assert(strstr(message, want_text) != NULL);
    assert(package.iso_path[0] == '\0');
}

/* A synthetic PSN package imports as its ISO: converted once into
   <data>/discs/<ID>/<ID>.iso byte for byte, with progress, a cancel that keeps
   nothing, reuse of a checked conversion, and the wizard panel returning to
   the view the package was picked from. PSOne Classics, updaters and other
   packages are refused by name before any key lookup. */
static void test_psn_package_imports_as_its_iso(void) {
    const char sep = nk_platform_path_separator();
    char cache_root[NK_MAX_PATH];
    char base[NATIVE_TEST_PATH_MAX + 64];
    char data_root[NATIVE_TEST_PATH_MAX + 96];
    char source_iso[NATIVE_TEST_PATH_MAX + 96];
    char pbp_path[NATIVE_TEST_PATH_MAX + 96];
    char other_path[NATIVE_TEST_PATH_MAX + 96];
    char expected_iso[NATIVE_TEST_PATH_MAX + 160];
    char partial[NATIVE_TEST_PATH_MAX + 192];
    char key_error[160];
    char code[64];
    char message[512];
    uint8_t *iso;
    uint8_t *pbp;
    uint8_t *converted;
    size_t iso_len = 0, pbp_len = 0, converted_len = 0;
    NkKeystore *keys;
    NkPspCtx ctx;
    PlayerPsnPackage package;
    PlayerPsnPackage again;
    PsnTestProgress progress;
    PlayerPsnConvertCallbacks cancel_hooks = { NULL, psn_test_cancel_now, NULL };
    PlayerPsnConvertCallbacks hooks = { psn_test_progress, psn_test_never, NULL };
    IsoInspectResult inspected;
    PlayerApp *app;

    printf("[PLAYER_STATE_TEST] Subtest: a PSN package (NPUMDIMG) imports as its ISO\n");
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_root, sizeof(cache_root)));
    snprintf(base, sizeof(base), "%s%cpsn-package-state", cache_root, sep);
    assert(test_remove_tree(base));
    snprintf(data_root, sizeof(data_root), "%s%cdata", base, sep);
    assert(nk_platform_mkdir_p(data_root));
    snprintf(source_iso, sizeof(source_iso), "%s%csource.iso", base, sep);
    snprintf(pbp_path, sizeof(pbp_path), "%s%cEBOOT.PBP", base, sep);
    snprintf(expected_iso, sizeof(expected_iso), "%s%cdiscs%cTEST80901%cTEST80901.iso",
             data_root, sep, sep, sep);
    snprintf(partial, sizeof(partial), "%s.converting", expected_iso);

    write_player_package_preflight_iso(source_iso, "TEST80901", "Synthetic PSN Game", false);
    iso = psn_test_read_file(source_iso, &iso_len);
    keys = nk_keystore_create();
    assert(keys != NULL);
    assert(nk_keystore_load_builtin(keys, key_error, sizeof(key_error)) == NK_PSP_OK);
    nk_psp_ctx_init(&ctx, keys);
    pbp = psp_seal_npumdimg_pbp(&ctx, "TEST80901", iso, iso_len, 4u, &pbp_len);
    nk_keystore_free(keys);
    write_binary_file(pbp_path, pbp, pbp_len);
    free(pbp);
    assert(nk_platform_set_app_data_dir_override(data_root));

    assert(player_path_is_pbp_package(pbp_path));
    assert(!player_path_is_pbp_package(source_iso));
    assert(player_psn_package_inspect(pbp_path, &package, code, sizeof(code),
                                      message, sizeof(message)));
    assert(strcmp(package.iso_path, expected_iso) == 0);
    assert(strcmp(package.disc_id, "TEST80901") == 0);
    assert(strcmp(package.title, "Synthetic PSN package") == 0);
    assert(!package.iso_ready);

    /* A cancel removes the partial image and keeps nothing. */
    assert(!player_psn_package_convert(&package, &cancel_hooks, code, sizeof(code),
                                       message, sizeof(message)));
    assert(strcmp(code, "CANCELLED") == 0);
    assert(strstr(message, "partial disc image was removed") != NULL);
    assert(nk_platform_path_missing(expected_iso));
    assert(nk_platform_path_missing(partial));

    /* The conversion restores the ISO byte for byte, reporting progress. */
    memset(&progress, 0, sizeof(progress));
    hooks.userdata = &progress;
    assert(player_psn_package_convert(&package, &hooks, code, sizeof(code),
                                      message, sizeof(message)));
    assert(progress.calls == (int)((iso_len / 2048u + 3u) / 4u));
    assert(progress.done == progress.total && progress.total == iso_len);
    assert(nk_platform_path_missing(partial));
    converted = psn_test_read_file(expected_iso, &converted_len);
    assert(converted_len == iso_len && memcmp(converted, iso, iso_len) == 0);
    free(converted);

    /* The ISO is then imported exactly as a picked disc image. */
    assert(iso_inspect_file(package.iso_path, &inspected));
    assert(strcmp(inspected.disc_id, "TEST80901") == 0);
    assert(strcmp(inspected.title_name, "Synthetic PSN Game") == 0);

    /* Picking the package again reuses the checked conversion. */
    assert(player_psn_package_inspect(pbp_path, &again, code, sizeof(code),
                                      message, sizeof(message)));
    assert(again.iso_ready);

    /* The wizard's panel shows the conversion and hands back the view the
       package was picked from. */
    app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(app != NULL);
    app->active_view = VIEW_LIBRARY;
    player_app_psn_conversion_begin(app, &package);
    assert(app->active_view == VIEW_SETUP_WIZARD);
    assert(app->wizard.step == WIZARD_STEP_INSPECT_VERIFY);
    assert(app->wizard.is_extracting && app->psn_conversion_active);
    assert(strcmp(app->inspecting_game.disc_id, "TEST80901") == 0);
    assert(strstr(app->wizard.status_message, "PSN package") != NULL);
    assert(player_app_psn_conversion_take_request(app));
    assert(!player_app_psn_conversion_take_request(app));
    player_app_psn_conversion_progress(app, iso_len / 2u, iso_len);
    assert(app->wizard.extraction_percent == 50);
    player_app_wizard_cancel(app);
    assert(player_app_wizard_cancel_requested(app));
    assert(strcmp(app->wizard.status_message, "Cancelling the conversion...") == 0);
    player_app_psn_conversion_finish(app);
    assert(app->active_view == VIEW_LIBRARY);
    assert(!app->wizard.is_extracting && !app->psn_conversion_active);
    assert(!player_app_wizard_cancel_requested(app));
    free(app);

    /* PSOne Classics, updaters and other packages keep a named refusal. */
    snprintf(other_path, sizeof(other_path), "%s%cOTHER.PBP", base, sep);
    {
        static const char *const magics[3] = { "PSISOIMG0000", "PSTITLEIMG000000", "NOTANIMAGE" };
        static const char *const categories[3] = { "ME", "ME", "EG" };
        static const char *const codes[3] = { "PSONE_CLASSIC_UNSUPPORTED",
                                               "PSONE_CLASSIC_UNSUPPORTED",
                                               "PBP_PACKAGE_UNSUPPORTED" };
        static const char *const texts[3] = { "is a PSOne Classic", "is a PSOne Classic",
                                              "Only PSN releases of UMD games" };
        uint8_t psar[0x400];
        for (unsigned i = 0; i < 3u; i++) {
            memset(psar, 0, sizeof(psar));
            memcpy(psar, magics[i], strlen(magics[i]));
            pbp = psp_seal_pbp(categories[i], "SLUS99999", psar, sizeof(psar), &pbp_len);
            write_binary_file(other_path, pbp, pbp_len);
            free(pbp);
            psn_test_refused(other_path, codes[i], texts[i]);
        }
        memset(psar, 0, sizeof(psar));
        memcpy(psar, "PSAR", 4);
        pbp = psp_seal_pbp("MG", "MSTKUPDAT", psar, sizeof(psar), &pbp_len);
        write_binary_file(other_path, pbp, pbp_len);
        free(pbp);
        psn_test_refused(other_path, "FIRMWARE_OUT_OF_SCOPE", "never decrypts firmware");
    }
    psn_test_refused(source_iso, "PBP_PACKAGE_UNSUPPORTED", "not a PSP package");

    assert(nk_platform_set_app_data_dir_override(NULL));
    free(iso);
    assert(test_remove_tree(base));
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--image") == 0) {
        return repeat_launch_child_mode();
    }
    if (argc == 3 && strcmp(argv[1], "-c") == 0) {
        static const char python_probe[] =
            "import sys, sysconfig; sys.exit(sys.implementation.name != 'cpython' or "
            "sysconfig.get_platform().startswith('mingw') or "
            "not ((3, 14) <= sys.version_info[:2] <= (3, 15)))";
        /* The pinned runtime (a bundled or downloaded CPython) must be 3.15. */
        static const char python_probe_315[] =
            "import sys, sysconfig; sys.exit(sys.implementation.name != 'cpython' or "
            "sysconfig.get_platform().startswith('mingw') or "
            "sys.version_info[:2] != (3, 15))";
        return strcmp(argv[2], python_probe) == 0 ||
               strcmp(argv[2], python_probe_315) == 0 ? 0 : 94;
    }
    if (argc >= 3 && strcmp(argv[2], "build-package") == 0) {
        return player_state_package_builder_child(argc, argv);
    }
    if (argc == 7 && strcmp(argv[1], "--validate-package") == 0) {
        char *end = NULL;
        unsigned long experimental = strtoul(argv[5], &end, 10);
        if (!end || *end || experimental > 1) return 2;
        PlayerApp *probe = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(probe != NULL);
        player_app_set_runtime_root(probe, argv[2]);
        GameRecord game;
        memset(&game, 0, sizeof(game));
        snprintf(game.disc_id, sizeof(game.disc_id), "%s", argv[3]);
        snprintf(game.title_id, sizeof(game.title_id), "%s", argv[4]);
        game.is_experimental = experimental != 0;
        snprintf(game.selected_executable, sizeof(game.selected_executable), "%s", argv[6]);
        NkRuntimePackageInfo info;
        char reason[2048];
        NkRuntimePackageStatus status = player_app_validate_runtime_package(
            probe, &game, &info, reason, sizeof(reason));
        printf("PACKAGE_STATUS=%s\nPACKAGE_REASON=%s\n",
               runtime_package_status_name(status), reason);
        free(probe);
        return 0;
    }

#if defined(NK_TITLE_MANIFEST_TEST_SEAMS)
    test_source_iso_member_path_validation();
#endif

    /* Per-run cache isolation (#735 item 8): every fixture below resolves its
     * cache, save and boot-event paths inside a temporary root, never inside
     * the developer's real per-user directories. */
    native_test_create_root("player-state");
    char self_executable[NK_MAX_PATH];
    if (!nk_platform_absolute_path(argv[0], self_executable,
                                   sizeof(self_executable))) {
        snprintf(self_executable, sizeof(self_executable), "%s", argv[0]);
    }
    {
        /* Nakagawa reads no key file: the built-in tables are its only key
         * source. Leave the old key-file override pointing at an existing file
         * for the whole run: the encrypted-executable and encrypted-module
         * checks below must ignore it and never name it. */
        int written = snprintf(g_decoy_key_path, sizeof(g_decoy_key_path),
                               "%s%cdeveloper-keyfile.json",
                               native_test_get_root(), nk_platform_path_separator());
        assert(written > 0 && (size_t)written < sizeof(g_decoy_key_path));
        write_text_file(g_decoy_key_path, "{\"format\":\"nakagawa-psp-keystore-1\",\"entries\":{}}");
        native_test_set_env("NAKAGAWA_PSP_KEY_FILE", g_decoy_key_path);
    }
    native_test_isolate_user_data_roots();
    {
        const char *key_override = getenv("NAKAGAWA_PSP_KEY_FILE");
        assert(key_override != NULL && strcmp(key_override, g_decoy_key_path) == 0);
    }
    native_test_assert_cache_root_isolated();
    native_test_assert_config_root_isolated();

    test_psn_package_imports_as_its_iso();
    test_stage_discard_removes_a_deep_tree();
    test_tree_helpers_walk_a_deep_tree();
#if defined(_WIN32) || defined(_WIN64)
    test_remove_tree_deeper_than_max_path();
#endif
    test_remove_from_library_deletes_what_nakagawa_built();
    test_package_build_lifecycle(self_executable);
    test_bundled_prerequisite_build(self_executable);
    test_error_cards_are_plain();
    test_build_outcome_is_shown_once_per_session();
    test_bitmap_font_text_fallback();
    test_display_path_hides_profile_folder();
    test_license_folder_request();
    test_library_card_status_text();
    /* Saves a library file under the cache root, so it runs only once that
     * root is the temporary one. */
    test_sample_entries_never_reach_library_json();

    /* PlayerApp holds 64 game records twice over; keep it off the stack. */
    PlayerApp *app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(app != NULL);

    /* Build the library directly rather than through player_app_init, which
       would read (and later write) the real user data directory. */
    nk_library_init(&app->library);

    NkGameEntry entry;
    seed_entry(&entry, "TEST00001", "First");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    seed_entry(&entry, "TEST00002", "Second");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    seed_entry(&entry, "TEST00005", "Third");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    player_app_sync_library(app);
    assert(app->game_count == 3);

    /* Bundled showcase entries are merged into the visible view only. They
       survive library resyncs without entering or leaving the user's library. */
    memset(&app->showcase_games[0], 0, sizeof(app->showcase_games[0]));
    snprintf(app->showcase_games[0].disc_id, sizeof(app->showcase_games[0].disc_id),
             "TEST00007");
    snprintf(app->showcase_games[0].title_id, sizeof(app->showcase_games[0].title_id),
             "showcase-scene-v1");
    app->showcase_count = 1;
    player_app_sync_library(app);
    assert(app->game_count == 4 && app->library.count == 3);
    assert(player_game_is_showcase(&app->games[3]));
    assert(!player_app_remove_game(app, 3));
    assert(app->library.count == 3 && app->game_count == 4);
    player_app_sync_library(app);
    assert(app->game_count == 4 && strcmp(app->games[3].disc_id, "TEST00007") == 0);
    app->showcase_count = 0;
    player_app_sync_library(app);
    assert(app->game_count == 3 && app->library.count == 3);

    /* 1. An entry that is NOT last is found at its real index.
     *
     * This is the case --launch-now got wrong: re-adding TEST00001 updates it
     * in place at index 0, while game_count - 1 names TEST00005. */
    printf("[PLAYER_STATE_TEST] Subtest 1: lookup by disc ID\n");
    fflush(stdout);
    assert(player_app_find_game_by_disc_id(app, "TEST00001") == 0);
    assert(player_app_find_game_by_disc_id(app, "TEST00002") == 1);
    assert(player_app_find_game_by_disc_id(app, "TEST00005") == 2);
    assert(player_app_find_game_by_disc_id(app, "TEST00001") != app->game_count - 1);

    /* 2. Updating an existing record keeps its index rather than appending. */
    printf("[PLAYER_STATE_TEST] Subtest 2: update is in place\n");
    fflush(stdout);
    seed_entry(&entry, "TEST00001", "First, revisited");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    player_app_sync_library(app);
    assert(app->game_count == 3);
    assert(player_app_find_game_by_disc_id(app, "TEST00001") == 0);
    assert(strcmp(app->games[0].title_name, "First, revisited") == 0);

    /* 2b. Re-adding the same disc keeps its completed extraction.
     *
     * Re-inspecting an ISO that is already in the library produced a fresh
     * record with assets_staged=false and no prepared root, and the update
     * replaced the staged record wholesale, so the next launch had no data root. */
    printf("[PLAYER_STATE_TEST] Subtest 2b: re-adding a disc keeps its staged assets\n");
    fflush(stdout);
    {
        GameRecord staged;
        seed_entry(&staged, "TEST00001", "First");
        staged.assets_staged = true;
        snprintf(staged.prepared_root, sizeof(staged.prepared_root), "games%cTEST00001",
                 nk_platform_path_separator());
        staged.extracted_asset_count = 42;
        snprintf(staged.last_played, sizeof(staged.last_played), "2026-09-25");
        GameRecord again;
        seed_entry(&again, "TEST00001", "First");
        assert(player_merge_readded_game(&staged, &again));
        assert(again.assets_staged && strcmp(again.prepared_root, staged.prepared_root) == 0);
        assert(again.extracted_asset_count == 42);
        assert(strcmp(again.last_played, "2026-09-25") == 0);

        GameRecord other_disc;
        seed_entry(&other_disc, "TEST00001", "First");
        other_disc.iso_size_bytes = staged.iso_size_bytes + 2048;
        assert(!player_merge_readded_game(&staged, &other_disc));
        assert(!other_disc.assets_staged && other_disc.prepared_root[0] == '\0');
    }

    /* 3. Unknown and malformed disc IDs report absence, not index 0. */
    printf("[PLAYER_STATE_TEST] Subtest 3: absent disc IDs\n");
    fflush(stdout);
    assert(player_app_find_game_by_disc_id(app, "TEST09999") == -1);
    assert(player_app_find_game_by_disc_id(app, "") == -1);
    assert(player_app_find_game_by_disc_id(app, NULL) == -1);
    assert(player_app_find_game_by_disc_id(NULL, "TEST00001") == -1);

    /* 4. A rejected insert must be reported as a failure.
     *
     * Fill the library to its limit, then add one more. The insert fails, so
     * player_app_add_game returns before ever reaching nk_library_save -- no
     * file is touched by this test. */
    printf("[PLAYER_STATE_TEST] Subtest 4: a rejected insert reports failure\n");
    fflush(stdout);
    nk_library_init(&app->library);
    for (int i = 0; i < NK_MAX_GAMES; i++) {
        char disc_id[NK_MAX_DISC_ID_LEN];
        snprintf(disc_id, sizeof(disc_id), "FULL%05d", i);
        seed_entry(&entry, disc_id, "Filler");
        assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    }
    player_app_sync_library(app);
    assert(app->library.count == NK_MAX_GAMES);

    seed_entry(&entry, "OVERFLOW1", "One too many");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_ERROR_OUT_OF_MEMORY);
    assert(player_app_add_game(app, &entry) == false);
    assert(player_app_find_game_by_disc_id(app, "OVERFLOW1") == -1);

    /* 5. A record with no disc ID is refused outright. */
    printf("[PLAYER_STATE_TEST] Subtest 5: a record with no disc ID is refused\n");
    fflush(stdout);
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.title_name, sizeof(entry.title_name), "Nameless");
    assert(player_app_add_game(app, &entry) == false);

    /* 6. Every library entry must be reachable.
     *
     * The strip draws cards left to right on a 280-pixel pitch, so a
     * 1280-wide window shows about four. Everything past those was drawn
     * outside the window, and src/player had no wheel, paging, keyboard or
     * offset handling at all -- so with a full library most games could
     * neither be selected nor launched. */
    printf("[PLAYER_STATE_TEST] Subtest 6: every entry is reachable\n");
    fflush(stdout);

    app->window_width = 1280;
    app->window_height = 720;
    int visible = player_app_visible_library_cards(app);
    assert(visible >= 1);
    assert(visible < NK_MAX_GAMES);   /* otherwise this proves nothing */

    /* The library is already full from subtest 4. */
    assert(app->game_count == NK_MAX_GAMES);
    app->selected_game_index = 0;
    for (int i = 1; i < NK_MAX_GAMES; i++) {
        player_app_move_selection(app, 1);
        assert(app->selected_game_index == i);
    }
    /* Including the ones that never fit on screen at once. */
    assert(app->selected_game_index == NK_MAX_GAMES - 1);
    assert(app->selected_game_index >= visible);

    /* Selection clamps rather than wrapping or running off either end. */
    player_app_move_selection(app, 1);
    assert(app->selected_game_index == NK_MAX_GAMES - 1);
    player_app_move_selection(app, -NK_MAX_GAMES * 2);
    assert(app->selected_game_index == 0);
    player_app_move_selection(app, -1);
    assert(app->selected_game_index == 0);

    /* A window too narrow for even one card still offers one. */
    app->window_width = 100;
    assert(player_app_visible_library_cards(app) == 1);
    app->window_width = 1920;
    assert(player_app_visible_library_cards(app) > visible);

    /* 7. The demo fixture set must contain a title that can actually launch.
     *
     * A fixture library whose every entry resolves to no runtime made the launch
     * path look implemented while it had never once been reached end to end.
     * display-smoke-v1 is the public title whose build layout nk_launch.c can
     * resolve, so it has to be in the set a fresh install shows. This also pins
     * the memory-only contract: populate must leave a populated library alone. */
    printf("[PLAYER_STATE_TEST] Subtest 7: demo fixtures stay unprepared without qualified source media\n");
    fflush(stdout);
    PlayerApp *fresh = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(fresh != NULL);
    nk_library_init(&fresh->library);
    player_app_sync_library(fresh);
    assert(fresh->game_count == 0);

    /* Point package discovery at a disposable root. A bare executable/image
       pair or an unqualified identity must not make the fixture launchable. */
    char cache_dir[512];
    char fixture_root[700];
    char fixture_package_dir[900];
    char fixture_package_json[1100];
    char fixture_report[1100];
    char fixture_exe[1100];
    char fixture_image[1100];
    char fixture_data_root[900];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
    snprintf(fixture_root, sizeof(fixture_root), "%s%cpr177_player_state_root",
             cache_dir, nk_platform_path_separator());
    snprintf(fixture_package_dir, sizeof(fixture_package_dir), "%s%cpackages%cTEST00006",
             fixture_root, nk_platform_path_separator(), nk_platform_path_separator());
    /* The synthetic package names display-smoke-v1.exe on every host. */
    snprintf(fixture_exe, sizeof(fixture_exe), "%s%cdisplay-smoke-v1.exe",
             fixture_package_dir, nk_platform_path_separator());
    snprintf(fixture_image, sizeof(fixture_image), "%s%cdisplay-smoke-v1_image.bin",
             fixture_package_dir, nk_platform_path_separator());
    snprintf(fixture_package_json, sizeof(fixture_package_json), "%s%cpackage.json",
             fixture_package_dir, nk_platform_path_separator());
    snprintf(fixture_report, sizeof(fixture_report), "%s%cbuild-report.json",
             fixture_package_dir, nk_platform_path_separator());
    snprintf(fixture_data_root, sizeof(fixture_data_root), "%s%cfixtures%cdisplay_smoke",
             fixture_root, nk_platform_path_separator(), nk_platform_path_separator());
    assert(nk_platform_mkdir_p(fixture_data_root));
    write_runtime_package_fixture(fixture_root, "TEST00006", "display-smoke-v1", SR_CPUSTATE_ABI_VERSION,
                                  "display-smoke-v1.exe", FIXTURE_SHA256, NULL);
    player_app_set_runtime_root(fresh, fixture_root);

    player_app_populate_sample_games(fresh);
    assert(fresh->game_count > 0);
    int disp = player_app_find_game_by_disc_id(fresh, "TEST00006");
    assert(disp >= 0);
    assert(strcmp(fresh->games[disp].title_id, "display-smoke-v1") == 0);
    assert(fresh->games[disp].is_prepared == false);
    assert(fresh->games[disp].status == NK_STATUS_IDENTIFIED);

    int before = fresh->game_count;
    player_app_populate_sample_games(fresh);
    assert(fresh->game_count == before);
    assert(remove(fixture_exe) == 0);
    assert(remove(fixture_image) == 0);
    assert(remove(fixture_report) == 0);
    assert(remove(fixture_package_json) == 0);
    assert(test_rmdir(fixture_data_root) == 0);
    free(fresh);

    /* 8. A launch started from the player requests a window. */
    printf("[PLAYER_STATE_TEST] Subtest 8: a player launch requests a window\n");
    fflush(stdout);
    PlayerApp *launcher = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(launcher != NULL);
    nk_library_init(&launcher->library);

    NkGameEntry unknown;
    seed_entry(&unknown, "ZZZZ99999", "Not In The Catalog");
    snprintf(unknown.title_id, sizeof(unknown.title_id), "not-a-catalog-title");
    assert(nk_library_add_or_update(&launcher->library, &unknown) == NK_OK);
    player_app_sync_library(launcher);
    assert(launcher->game_count == 1);

    assert(player_app_launch_game(launcher, 0) == false);
    assert(launcher->launch_session.config.gui_mode == true);
    assert(launcher->is_game_running == false);
    free(launcher);

    /* 9. Settings mutations validate and clamp: previously the settings
     * screen drew preset buttons whose clicks were discarded, so this is
     * the failing-before contract for the rehaul. */
    printf("[PLAYER_STATE_TEST] Subtest 9: settings mutations validate\n");
    fflush(stdout);
    PlayerApp *settings = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(settings != NULL);
    nk_library_init(&settings->library);
    settings->settings.resolution_scale = 4;
    settings->settings.fps_cap = -1;
    settings->settings.master_volume = 80;
    settings->settings.vsync = true;
    settings->settings.fullscreen = false;

    player_app_set_resolution_scale(settings, 2);
    assert(settings->settings.resolution_scale == 2);
    player_app_set_resolution_scale(settings, 7);
    assert(settings->settings.resolution_scale == 2);
    player_app_set_resolution_scale(settings, 0);
    assert(settings->settings.resolution_scale == 2);
    player_app_cycle_resolution_scale(settings, 1);
    assert(settings->settings.resolution_scale == 4);
    player_app_cycle_resolution_scale(settings, -1);
    assert(settings->settings.resolution_scale == 2);
    /* 8x is no longer offered: the GPU rasterizer caps at 4x, so the setter
       refuses it instead of letting the UI claim an unsupported preset. */
    player_app_set_resolution_scale(settings, 8);
    assert(settings->settings.resolution_scale == 2);

    /* Game-frame scaling (#1129): three modes, cycled in both directions; an unknown value is refused. */
    settings->settings.present_scaling = NK_PRESENT_SCALING_SHARP;
    player_app_cycle_present_scaling(settings, 1);
    assert(settings->settings.present_scaling == NK_PRESENT_SCALING_INTEGER);
    player_app_cycle_present_scaling(settings, 1);
    assert(settings->settings.present_scaling == NK_PRESENT_SCALING_BILINEAR);
    player_app_cycle_present_scaling(settings, 1);
    assert(settings->settings.present_scaling == NK_PRESENT_SCALING_SHARP);
    player_app_cycle_present_scaling(settings, -1);
    assert(settings->settings.present_scaling == NK_PRESENT_SCALING_BILINEAR);
    player_app_set_present_scaling(settings, NK_PRESENT_SCALING_INTEGER);
    assert(settings->settings.present_scaling == NK_PRESENT_SCALING_INTEGER);
    player_app_set_present_scaling(settings, 3);
    assert(settings->settings.present_scaling == NK_PRESENT_SCALING_INTEGER);
    player_app_set_present_scaling(settings, -1);
    assert(settings->settings.present_scaling == NK_PRESENT_SCALING_INTEGER);
    assert(strcmp(player_present_scaling_label(NK_PRESENT_SCALING_SHARP), "Sharp") == 0);
    assert(strcmp(player_present_scaling_label(NK_PRESENT_SCALING_INTEGER), "Integer") == 0);
    assert(strcmp(player_present_scaling_label(NK_PRESENT_SCALING_BILINEAR), "Bilinear") == 0);

    player_app_set_fps_cap(settings, 0);
    assert(settings->settings.fps_cap == 0);
    player_app_set_fps_cap(settings, 999);
    assert(settings->settings.fps_cap == 0);
    player_app_set_fps_cap(settings, 30);
    assert(settings->settings.fps_cap == 0);
    player_app_cycle_fps_cap(settings, -1);
    assert(settings->settings.fps_cap == -1);
    player_app_cycle_fps_cap(settings, 1);
    assert(settings->settings.fps_cap == 0);

    player_app_toggle_vsync(settings);
    assert(settings->settings.vsync == false);
    player_app_toggle_vsync(settings);
    assert(settings->settings.vsync == true);
    player_app_toggle_fullscreen(settings);
    assert(settings->settings.fullscreen == true);
    player_app_toggle_fullscreen(settings);
    assert(settings->settings.fullscreen == false);
    assert(settings->settings.reduce_motion == false);
    player_app_toggle_reduce_motion(settings);
    assert(settings->settings.reduce_motion == true);
    player_app_toggle_reduce_motion(settings);
    assert(settings->settings.reduce_motion == false);

    assert(settings->settings.experimental_60hz == false);
    player_app_toggle_experimental_60hz(settings);
    assert(settings->settings.experimental_60hz == true);
    player_app_toggle_experimental_60hz(settings);
    assert(settings->settings.experimental_60hz == false);
    player_app_set_experimental_60hz(settings, true);
    assert(settings->settings.experimental_60hz == true);
    player_app_set_experimental_60hz(settings, false);
    assert(settings->settings.experimental_60hz == false);

    player_app_adjust_volume(settings, 5);
    assert(settings->settings.master_volume == 85);
    player_app_adjust_volume(settings, 1000);
    assert(settings->settings.master_volume == 100);
    player_app_adjust_volume(settings, -2000);
    assert(settings->settings.master_volume == 0);
    free(settings);

    /* 9b. The launch-relevant settings map onto the session's runtime config;
     * reduce_motion is launcher-UI state and must not leak into the child. */
    printf("[PLAYER_STATE_TEST] Subtest 9b: settings apply to the launch session\n");
    fflush(stdout);
    {
        PlayerSettings applied;
        memset(&applied, 0, sizeof(applied));
        player_app_settings_init_default(&applied);
        applied.resolution_scale = 2;
        applied.present_scaling = NK_PRESENT_SCALING_INTEGER;
        applied.fps_cap = -1;
        applied.vsync = false;
        applied.fullscreen = true;
        applied.master_volume = 55;
        applied.reduce_motion = true;

        NkRuntimeConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.gui_mode = true;
        player_app_apply_settings_to_session(&applied, &cfg);
        assert(cfg.resolution_scale == 2);
        assert(cfg.present_scaling == NK_PRESENT_SCALING_INTEGER);
        assert(cfg.fps_cap == -1);
        assert(cfg.vsync == false);
        assert(cfg.fullscreen == true);
        assert(cfg.master_volume == 55);
        assert(cfg.gui_mode == true);   /* untouched by the mapping */
        /* NULL arguments are safe no-ops. */
        player_app_apply_settings_to_session(NULL, &cfg);
        player_app_apply_settings_to_session(&applied, NULL);
        assert(cfg.master_volume == 55);
    }

    /* 9c. The host input mapping the launched runtime receives is the one the
     * selected disc asked for: its own per-title mapping when it has one, the
     * global profile otherwise (#520). */
    printf("[PLAYER_STATE_TEST] Subtest 9c: per-title mapping reaches the launch session\n");
    fflush(stdout);
    {
        PlayerApp *mapped = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(mapped != NULL);
        nk_library_init(&mapped->library);
        input_settings_init(&mapped->input_settings);
        /* Scratch root: CMake points NK_TEST_SCRATCH_DIR into its binary tree so
         * an out-of-source build never depends on <source>/build existing; the
         * Makefile target runs from the source tree, where build/ is the
         * conventional scratch directory. */
        const char *scratch_dir = getenv("NK_TEST_SCRATCH_DIR");
        if (scratch_dir == NULL || scratch_dir[0] == '\0') scratch_dir = SR_SELFTEST_BUILD_ROOT;
        char global_profile[NK_MAX_PATH];
        snprintf(global_profile, sizeof(global_profile),
                 "%s/test_launch_global_profile.json", scratch_dir);
        assert(nk_platform_mkdir_p(scratch_dir));
        snprintf(mapped->input_settings.profile_path,
                 sizeof(mapped->input_settings.profile_path),
                 "%s", global_profile);

        GameRecord tennis;
        memset(&tennis, 0, sizeof(tennis));
        snprintf(tennis.disc_id, sizeof(tennis.disc_id), "UCUS98701");
        snprintf(tennis.title_name, sizeof(tennis.title_name), "Mapping Fixture");

        /* No per-title entry: the disc runs the global profile. */
        assert(player_app_apply_input_profile_to_session(mapped, &tennis) == NK_OK);
        assert(strcmp(mapped->launch_session.config.input_profile_path,
                      global_profile) == 0);
        assert(mapped->input_profile_notice[0] == '\0');

        /* Give that disc its own mapping. */
        assert(input_settings_set_scope(&mapped->input_settings, "UCUS98701"));
        NkBindingSource left_stick;
        left_stick.type = NK_BINDING_HOST_BUTTON;
        left_stick.index = NK_HOST_BUTTON_LEFT_STICK;
        assert(input_settings_assign_binding(&mapped->input_settings,
                                             INPUT_CONTROL_BTN_CROSS, left_stick));
        assert(input_settings_save(&mapped->input_settings, NULL) == NK_OK);

        /* The session now names a profile file of its own for that disc, and the
         * file the child would load is the disc's mapping, not the global one. */
        assert(player_app_apply_input_profile_to_session(mapped, &tennis) == NK_OK);
        const char *profile_path = mapped->launch_session.config.input_profile_path;
        assert(strstr(profile_path, "UCUS98701") != NULL);
        assert(strcmp(profile_path, global_profile) != 0);
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
        NkInputProfile handed;
        assert(nk_input_profile_load(&handed, profile_path, diag, sizeof(diag)) == NK_OK);
        assert(handed.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_LEFT_STICK);
        assert(handed.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == NK_INPUT_DEFAULT_DEADZONE_INNER);

        /* Another disc still gets the global profile: two mappings, one session. */
        GameRecord boxing;
        memset(&boxing, 0, sizeof(boxing));
        snprintf(boxing.disc_id, sizeof(boxing.disc_id), "TEST80001");
        assert(player_app_apply_input_profile_to_session(mapped, &boxing) == NK_OK);
        assert(strcmp(mapped->launch_session.config.input_profile_path,
                      global_profile) == 0);

        /* A disc ID the document has no entry for is not an error, and NULL
         * arguments are safe no-ops that change nothing. */
        char before[NK_MAX_PATH];
        snprintf(before, sizeof(before), "%s",
                 mapped->launch_session.config.input_profile_path);
        assert(player_app_apply_input_profile_to_session(mapped, &boxing) == NK_OK);
        assert(strcmp(mapped->launch_session.config.input_profile_path, before) == 0);
        assert(player_app_apply_input_profile_to_session(mapped, NULL) != NK_OK);
        assert(player_app_apply_input_profile_to_session(NULL, &tennis) != NK_OK);
        assert(strcmp(mapped->launch_session.config.input_profile_path, before) == 0);

        remove(profile_path);
        remove(global_profile);
        free(mapped);
    }

    /* 10. Focus clamps into range so keyboard/gamepad activation can never
     * target a control the view no longer draws. */
    printf("[PLAYER_STATE_TEST] Subtest 10: focus clamps\n");
    fflush(stdout);
    PlayerApp *focus = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(focus != NULL);
    focus->focus_index = 0;
    player_app_move_focus(focus, 5, 3);
    assert(focus->focus_index == 2);
    player_app_move_focus(focus, -10, 3);
    assert(focus->focus_index == 0);
    player_app_move_focus(focus, 1, 0);
    assert(focus->focus_index == 0);
    player_app_move_focus(NULL, 1, 3);
    free(focus);

    /* 11. Library removal touches only the entry, never the ISO file, and
     * invalid indices are refused before any save. */
    printf("[PLAYER_STATE_TEST] Subtest 11: library removal is entry-only\n");
    fflush(stdout);
    assert(player_app_remove_game(NULL, 0) == false);
    assert(player_app_remove_game(app, -1) == false);
    assert(player_app_remove_game(app, NK_MAX_GAMES + 100) == false);
    {
        PlayerApp *removal = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(removal != NULL);
        nk_library_init(&removal->library);
        /* Redirect persistence at a disposable file: remove saves, and the
         * test must never write the user's real library. */
        char rcache[512];
        assert(nk_platform_get_path(NK_PATH_CACHE, rcache, sizeof(rcache)));
        char rpath_tmp[1024];
        snprintf(rpath_tmp, sizeof(rpath_tmp),
                 "%s%cpr177_rm.json", rcache, nk_platform_path_separator());
        assert(strlen(rpath_tmp) < sizeof(removal->library.library_path));
        strcpy(removal->library.library_path, rpath_tmp);
        remove(removal->library.library_path);
        seed_entry(&entry, "RMV00001", "First");
        assert(nk_library_add_or_update(&removal->library, &entry) == NK_OK);
        seed_entry(&entry, "RMV00002", "Second");
        assert(nk_library_add_or_update(&removal->library, &entry) == NK_OK);
        player_app_sync_library(removal);
        assert(removal->game_count == 2);
        removal->selected_game_index = 1;
        assert(player_app_remove_game(removal, 0) == true);
        assert(removal->game_count == 1);
        assert(strcmp(removal->games[0].disc_id, "RMV00002") == 0);
        assert(player_app_find_game_by_disc_id(removal, "RMV00001") == -1);
        assert(player_app_remove_game(removal, 5) == false);
        remove(removal->library.library_path);
        free(removal);
    }

    /* 12. Focus-stop counts match the buttons each view draws, so the
     * event loop can never park focus on a control that does not exist. */
    printf("[PLAYER_STATE_TEST] Subtest 12: focus stops match drawn buttons\n");
    fflush(stdout);
    {
        PlayerApp *stops = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(stops != NULL);
        nk_library_init(&stops->library);
        stops->window_width = 1280;
        stops->window_height = 720;
        assert(player_app_focus_count(NULL) == 1);

        stops->active_view = VIEW_LIBRARY;
        stops->game_count = 0;
        assert(player_app_focus_count(stops) == 1); /* empty: add only */

        stops->active_view = PLAYER_VIEW_READY_LIBRARY;
        assert(player_app_focus_count(stops) == 1); /* empty ready library */
        stops->active_view = VIEW_LIBRARY;

        seed_entry(&entry, "FCS00001", "Unprepared");
        entry.is_prepared = false;
        stops->games[0] = entry;
        stops->game_count = 1;
        stops->selected_game_index = 0;
        assert(player_app_focus_count(stops) == 4); /* incompatible package: rebuild + add + remove + mapping (#1147) */

        stops->games[0].is_prepared = true;
        assert(player_app_focus_count(stops) == 4); /* no validated package is still incompatible: rebuild + add + remove + mapping */

        stops->is_game_running = true;
        assert(player_app_focus_count(stops) == 4); /* stop + add + remove + mapping */

        /* Overflow adds the two paging stops. */
        stops->is_game_running = false;
        stops->window_width = 640;
        assert(player_app_visible_library_cards(stops) == 2);
        stops->game_count = 1;
        assert(player_app_focus_count(stops) == 4); /* rebuild + add + remove + mapping */
        seed_entry(&entry, "FCS00002", "Second");
        stops->games[1] = entry;
        seed_entry(&entry, "FCS00003", "Third");
        stops->games[2] = entry;
        stops->game_count = 3;
        assert(player_app_focus_count(stops) == 6); /* + both paging stops */

        /* A title with missing package offers the BUILD PACKAGE button */
        stops->window_width = 1280;
        stops->game_count = 1;
        seed_entry(&entry, "TEST80001", "Synthetic Package Fixture");
        snprintf(entry.title_id, sizeof(entry.title_id), "test-80001");
        stops->games[0] = entry;
        assert(player_app_focus_count(stops) == 4); /* build package + add + remove + mapping */
        stops->focus_index = 0;
        player_app_runtime_package_cache_mark_pending(stops, 0);
        assert(player_app_focus_count(stops) == 4);
        assert(stops->focus_index == 0); /* background validation keeps primary focus */
        stops->runtime_package_cache[0].validation_pending = false;

        stops->active_view = VIEW_BUILDING_PACKAGE;
        assert(player_app_focus_count(stops) == 1); /* cancel build */

        stops->active_view = VIEW_INSPECTING;
        assert(player_app_focus_count(stops) == 1);
        stops->active_view = VIEW_SUPPORTED_TITLE;
        assert(player_app_focus_count(stops) == 2);
        stops->active_view = VIEW_UNSUPPORTED_TITLE;
        assert(player_app_focus_count(stops) == 1);
        stops->active_view = VIEW_PREPARING;
        assert(player_app_focus_count(stops) == 1);
        stops->active_view = VIEW_SETTINGS;
        assert(player_app_focus_count(stops) == 18); /* two pacing modes; scaling; launcher fullscreen; online options; UI sounds */
        stops->active_view = VIEW_PREREQ_CONSENT;
        assert(player_app_focus_count(stops) == 2);
        stops->active_view = VIEW_PREREQ_PROGRESS;
        assert(player_app_focus_count(stops) == 1);
        stops->active_view = VIEW_PREREQ_ABOUT;
        stops->prerequisites.item_count = 0;
        assert(player_app_focus_count(stops) == 1);
        stops->prerequisites.item_count = 1;
        assert(player_app_focus_count(stops) == 2);
        stops->active_view = VIEW_CONFIRM_REMOVE_TOOLS;
        assert(player_app_focus_count(stops) == 2);
        stops->active_view = VIEW_CONTROLLER_SETTINGS;
        assert(player_app_focus_count(stops) == 23); /* + the global / this-game choice */
        stops->input_settings.calib.stage = CALIBRATION_STAGE_REST;
        assert(player_app_focus_count(stops) == 1);
        stops->input_settings.calib.stage = CALIBRATION_STAGE_EXTREMES;
        assert(player_app_focus_count(stops) == 2);
        stops->input_settings.calib.stage = CALIBRATION_STAGE_RESULT;
        assert(player_app_focus_count(stops) == 2);
        stops->input_settings.calib.stage = CALIBRATION_STAGE_INACTIVE;
        assert(player_app_focus_count(stops) == 23);
        /* With no library disc there is nothing to name, so the choice is not a
         * focus stop and the count is the pre-#520 one. */
        stops->selected_game_index = -1;
        assert(player_app_focus_count(stops) == 22);
        stops->selected_game_index = 0;
        stops->active_view = VIEW_ERROR;
        assert(player_app_focus_count(stops) == 1);

        stops->active_view = VIEW_SETUP_WIZARD;
        stops->wizard.step = WIZARD_STEP_WELCOME;
        assert(player_app_focus_count(stops) == 2);
        stops->wizard.step = WIZARD_STEP_SELECT_GAME;
        stops->wizard.iso_selected = false;
        assert(player_app_focus_count(stops) == 3);
        stops->wizard.iso_selected = true;
        assert(player_app_focus_count(stops) == 4);
        stops->wizard.step = WIZARD_STEP_INSPECT_VERIFY;
        assert(player_app_focus_count(stops) == 3);
        stops->wizard.step = WIZARD_STEP_SYSTEM_FONTS;
        assert(player_app_focus_count(stops) == 6); /* accept, cycle, back, cancel, import, remove */
        stops->wizard.step = WIZARD_STEP_ONLINE_OPTIONS;
        assert(player_app_focus_count(stops) == 5); /* online, developer, continue, skip, back */
        stops->wizard.step = WIZARD_STEP_READY_LAUNCH;
        assert(player_app_focus_count(stops) == 3);
        free(stops);
    }

    /* 13. Setup Wizard state machine: start, step navigation, and cancellation. */
    printf("[PLAYER_STATE_TEST] Subtest 13: setup wizard transitions\n");
    fflush(stdout);
    {
        PlayerApp *wiz = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(wiz != NULL);
        nk_library_init(&wiz->library);

        player_app_start_setup_wizard(wiz);
        assert(wiz->active_view == VIEW_SETUP_WIZARD);
        assert(wiz->wizard.step == WIZARD_STEP_WELCOME);
        assert(wiz->focus_index == 0);

        player_app_wizard_next(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_ONLINE_OPTIONS);
        player_app_wizard_next(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_SELECT_GAME);

        /* Moving next without selected ISO requests file picker */
        wiz->request_file_picker = false;
        player_app_wizard_next(wiz);
        assert(wiz->request_file_picker == true);
        assert(wiz->wizard.step == WIZARD_STEP_SELECT_GAME);

        /* Simulating ISO inspection */
        snprintf(wiz->inspecting_game.iso_path, sizeof(wiz->inspecting_game.iso_path), "test.iso");
        snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id), "UCUS98701");
        wiz->inspecting_game.status = NK_STATUS_VERIFIED;
        player_app_wizard_next(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_INSPECT_VERIFY);

        /* Step 3 starts an asynchronous extraction request and remains
           visible until the reactive worker completion is delivered. */
        player_app_wizard_next(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_INSPECT_VERIFY);
        assert(wiz->wizard.is_extracting == true);
        assert(player_app_wizard_take_extraction_request(wiz) == true);
        assert(player_app_wizard_take_extraction_request(wiz) == false);
        assert(player_app_focus_count(wiz) == 1);
        player_app_wizard_set_extraction_progress(wiz, 42, 2, 5, "xbdata/menu.xb");
        assert(wiz->wizard.extraction_percent == 42);
        assert(wiz->wizard.files_extracted == 2);
        snprintf(wiz->inspecting_game.prepared_root,
                 sizeof(wiz->inspecting_game.prepared_root), "stage/TEST00001");
        wiz->inspecting_game.assets_staged = true;
        wiz->inspecting_game.extracted_asset_count = 4;
        wiz->inspecting_game.extracted_audio_count = 1;
        wiz->inspecting_game.extracted_visual_count = 1;
        wiz->inspecting_game.extracted_layout_count = 2;
        char cache_dir[512];
        char staged_library_path[700];
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
        snprintf(staged_library_path, sizeof(staged_library_path),
                 "%s%cplayer_state_staged_library.json", cache_dir,
                 nk_platform_path_separator());
        remove(staged_library_path);
        {
            char staged_library_bak[720];
            snprintf(staged_library_bak, sizeof(staged_library_bak), "%s.bak",
                     staged_library_path);
            remove(staged_library_bak);
        }
        assert(strlen(staged_library_path) < sizeof(wiz->library.library_path));
        memcpy(wiz->library.library_path, staged_library_path,
               strlen(staged_library_path) + 1);
        assert(player_app_register_staged_game(wiz) == true);
        assert(wiz->game_count == 1);
        assert(player_app_find_game_by_disc_id(wiz, "UCUS98701") == 0);
        assert(wiz->games[0].extracted_asset_count == 4);
        assert(wiz->active_view == PLAYER_VIEW_READY_LIBRARY);
        NkLibrary persisted;
        assert(nk_library_load(&persisted, staged_library_path) == NK_OK);
        const NkGameEntry *persisted_game = nk_library_find_by_disc_id(&persisted, "UCUS98701");
        assert(persisted_game != NULL);
        assert(persisted_game->assets_staged == true);
        assert(persisted_game->extracted_audio_count == 1);
        player_app_wizard_finish_extraction(wiz, NK_OK, NULL);
        assert(wiz->wizard.step == WIZARD_STEP_READY_LAUNCH);
        assert(wiz->wizard.extraction_complete == true);
        assert(wiz->active_view == PLAYER_VIEW_READY_LIBRARY);

        /* Back navigation */
        player_app_wizard_back(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_SYSTEM_FONTS);
        player_app_wizard_back(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_INSPECT_VERIFY);
        player_app_wizard_back(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_SELECT_GAME);
        player_app_wizard_back(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_ONLINE_OPTIONS);
        player_app_wizard_back(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_WELCOME);
        player_app_wizard_back(wiz);
        assert(wiz->active_view == VIEW_LIBRARY);
        remove(staged_library_path);
        {
            char staged_library_bak[720];
            snprintf(staged_library_bak, sizeof(staged_library_bak), "%s.bak",
                     staged_library_path);
            remove(staged_library_bak);
        }

        /* Cancel directly */
        player_app_start_setup_wizard(wiz);
        assert(wiz->active_view == VIEW_SETUP_WIZARD);
        player_app_wizard_cancel(wiz);
        assert(wiz->active_view == VIEW_LIBRARY);

        /* The compatibility report exposes the selected plaintext BOOT
           fallback and every current pre-launch boundary without touching a
           real title or runtime package. */
        char preflight_root[640], font_dir[720], font_path[800];
        char latin_font_path[800], korean_font_path[800];
        char preflight_fixtures_root[900], preflight_data_root[1024];
        unsigned long preflight_run_id;
#if defined(_WIN32) || defined(_WIN64)
        preflight_run_id = (unsigned long)_getpid();
#else
        preflight_run_id = (unsigned long)getpid();
#endif
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
        snprintf(preflight_root, sizeof(preflight_root),
                 "%s%cplayer-preflight-synthetic-%lu", cache_dir,
                 nk_platform_path_separator(), preflight_run_id);
        assert(nk_platform_mkdir_p(preflight_root));
        snprintf(preflight_fixtures_root, sizeof(preflight_fixtures_root),
                 "%s%cfixtures", preflight_root, nk_platform_path_separator());
        snprintf(preflight_data_root, sizeof(preflight_data_root), "%s%cprofile_zero",
                 preflight_fixtures_root, nk_platform_path_separator());
        snprintf(font_dir, sizeof(font_dir), "%s%cfont", preflight_root,
                 nk_platform_path_separator());
        snprintf(font_path, sizeof(font_path), "%s%cnkjpn.pgf", font_dir,
                 nk_platform_path_separator());
        snprintf(latin_font_path, sizeof(latin_font_path), "%s%cnkltn.pgf", font_dir,
                 nk_platform_path_separator());
        snprintf(korean_font_path, sizeof(korean_font_path), "%s%cnkkr.pgf", font_dir,
                 nk_platform_path_separator());
        NkTitleEntrySnapshot synthetic_snapshot = {0};
        assert(nk_title_catalog_find_by_id("synthetic-allegrex-v1",
                                           &synthetic_snapshot));
        const NkTitleEntry *synthetic = &synthetic_snapshot.entry;
        assert(synthetic != NULL && synthetic->game_name != NULL &&
               synthetic->primary_disc_id != NULL);
        char synthetic_game_name[65];
        char synthetic_disc_id[MAX_DISC_ID_LEN];
        snprintf(synthetic_game_name, sizeof(synthetic_game_name), "%s",
                 synthetic->game_name);
        snprintf(synthetic_disc_id, sizeof(synthetic_disc_id), "%s",
                 synthetic->primary_disc_id);
        nk_title_catalog_snapshot_release(&synthetic_snapshot);
        char build_dir[760], runtime_exe[900], runtime_image[900];
        char package_dir[900], package_json[1100], package_report[1100];
        snprintf(build_dir, sizeof(build_dir), "%s%cbuild%c%s", preflight_root,
                 nk_platform_path_separator(), nk_platform_path_separator(),
                 synthetic_game_name);
        snprintf(runtime_exe, sizeof(runtime_exe), "%s%c%s.exe", build_dir,
                 nk_platform_path_separator(), synthetic_game_name);
        snprintf(runtime_image, sizeof(runtime_image), "%s%c%s_image.bin", build_dir,
                 nk_platform_path_separator(), synthetic_game_name);
        snprintf(package_dir, sizeof(package_dir), "%s%cpackages%c%s", preflight_root,
                 nk_platform_path_separator(), nk_platform_path_separator(),
                 synthetic_disc_id);
        snprintf(package_json, sizeof(package_json), "%s%cpackage.json", package_dir,
                 nk_platform_path_separator());
        snprintf(package_report, sizeof(package_report), "%s%cbuild-report.json", package_dir,
                 nk_platform_path_separator());
        remove(runtime_exe);
        remove(runtime_image);
        remove(package_json);
        remove(package_report);
        remove(font_path);
        remove(latin_font_path);
        remove(korean_font_path);
        player_app_set_runtime_root(wiz, preflight_root);
        snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id),
                 "%s", synthetic_disc_id);
        snprintf(wiz->inspecting_game.title_id, sizeof(wiz->inspecting_game.title_id),
                 "synthetic-allegrex-v1");
        wiz->inspecting_game.iso_path[0] = '\0';
        NkIsoExecutableReport executable_report;
        memset(&executable_report, 0, sizeof(executable_report));
        executable_report.eboot.kind = NK_ISO_EXEC_PSP_ENCRYPTED;
        executable_report.boot.kind = NK_ISO_EXEC_MIPS_ELF32;
        executable_report.selected = NK_ISO_EXEC_SELECTION_BOOT;
        executable_report.boot_fallback = true;
        snprintf(executable_report.selected_path, sizeof(executable_report.selected_path),
                 "BOOT.BIN");
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        assert(wiz->wizard.preflight.count == 7);
        const PlayerPreflightCheck *check = find_preflight_check(
            &wiz->wizard.preflight, "DISC_SFO");
        assert(check && check->status == PREFLIGHT_OK);
        check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
        assert(check && check->status == PREFLIGHT_OK);
        assert(strstr(check->message, "BOOT.BIN selected for analysis") != NULL);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        check = find_preflight_check(&wiz->wizard.preflight, "SYSTEM_FONTS");
        assert(check && check->status == PREFLIGHT_MISSING);
        check = find_preflight_check(&wiz->wizard.preflight, "AUDIO_OUTPUT");
        assert(check && check->status == PREFLIGHT_OK);

        /* Exercise the production ISO module scan through player preflight,
           using only generated synthetic images. Each assertion protects a
           named fail-closed boundary. */
        char module_iso_path[900];
        snprintf(wiz->inspecting_game.selected_executable,
                 sizeof(wiz->inspecting_game.selected_executable), "EBOOT.BIN");
        snprintf(module_iso_path, sizeof(module_iso_path), "%s%cmodules-duplicate.iso",
                 preflight_root, nk_platform_path_separator());
        write_module_scan_iso(module_iso_path, 0, true, 0, false);
        assert_module_scan_boundary(wiz, module_iso_path, &executable_report,
                                    "DUPLICATE_DISC_MODULE_BASENAME");
        assert(remove(module_iso_path) == 0);

        snprintf(module_iso_path, sizeof(module_iso_path), "%s%cmodules-candidate-limit.iso",
                 preflight_root, nk_platform_path_separator());
        write_module_scan_iso(module_iso_path, 257, false, 0, false);
        assert_module_scan_boundary(wiz, module_iso_path, &executable_report,
                                    "DISC_MODULE_CANDIDATE_LIMIT");
        assert(remove(module_iso_path) == 0);

        snprintf(module_iso_path, sizeof(module_iso_path), "%s%cmodules-directory-limit.iso",
                 preflight_root, nk_platform_path_separator());
        write_module_scan_iso(module_iso_path, 0, false, 1023, false);
        assert_module_scan_boundary(wiz, module_iso_path, &executable_report,
                                    "DISC_MODULE_DIRECTORY_LIMIT");
        assert(remove(module_iso_path) == 0);

        snprintf(module_iso_path, sizeof(module_iso_path), "%s%cmodules-invalid-tree.iso",
                 preflight_root, nk_platform_path_separator());
        write_module_scan_iso(module_iso_path, 1, false, 0, true);
        assert_module_scan_boundary(wiz, module_iso_path, &executable_report,
                                    "DISC_MODULE_TREE_INVALID");
        assert(remove(module_iso_path) == 0);
        wiz->inspecting_game.iso_path[0] = '\0';

        assert(nk_platform_mkdir_p(build_dir));
        write_file(runtime_exe);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "DATA_ROOT");
        assert(check && check->status == PREFLIGHT_MISSING);
        assert(strstr(check->message, "data folder") != NULL);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        write_file(runtime_image);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        write_runtime_package_fixture(preflight_root, synthetic_disc_id,
                                      "synthetic-allegrex-v1", SR_CPUSTATE_ABI_VERSION,
                                      "synthetic-allegrex-v1.exe", FIXTURE_SHA256, NULL);
        assert(!player_app_game_has_runtime(wiz, &wiz->inspecting_game));
        snprintf(preflight_data_root, sizeof(preflight_data_root), "%s%cfixtures%cprofile_zero",
                 preflight_root, nk_platform_path_separator(),
                 nk_platform_path_separator());
        assert(nk_platform_mkdir_p(preflight_data_root));
        assert(nk_platform_mkdir_p(font_dir));
        write_binary_file(font_path, kGoldenJapanesePgf, sizeof(kGoldenJapanesePgf));
        write_binary_file(latin_font_path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));
        write_binary_file(korean_font_path, kGoldenKoreanPgf, sizeof(kGoldenKoreanPgf));
        assert(player_app_game_has_runtime(wiz, &wiz->inspecting_game));
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_OK);
        check = find_preflight_check(&wiz->wizard.preflight, "DATA_ROOT");
        assert(check && check->status == PREFLIGHT_OK);
        check = find_preflight_check(&wiz->wizard.preflight, "SYSTEM_FONTS");
        assert(check && check->status == PREFLIGHT_OK);

        /* A refused local title profile remains identifiable by its declared
           disc ID, so the player can show the same refusal beside the card. */
        char refusal_dir[1024];
        char refusal_path[1200];
        snprintf(refusal_dir, sizeof(refusal_dir), "%s%cprofile-refusal-manifests",
                 preflight_root, nk_platform_path_separator());
        assert(nk_platform_mkdir_p(refusal_dir));
        snprintf(refusal_path, sizeof(refusal_path), "%s%cstale-profile.json",
                 refusal_dir, nk_platform_path_separator());
        static const char refused_manifest[] =
            "{\"schema_version\":1,\"id\":\"refused-profile-v1\","
            "\"display_name\":\"Refused Profile Fixture\",\"kind\":\"retail\","
            "\"disc\":{\"id\":\"TEST00001\",\"region\":\"NA\","
            "\"revision_policy\":\"explicit-compatible-revisions\","
            "\"compatible_revisions\":[\"TEST00002\"]},"
            "\"executable\":{\"base\":\"0x08800000\","
            "\"entry\":\"0x08804000\",\"bss_metadata_source\":\"elf\","
            "\"extra_executable_spans\":[]},\"modules\":[],"
            "\"filesystem\":{\"data_root\":\"data\","
            "\"memory_stick_root\":\"savedata\","
            "\"device_prefixes\":[\"disc0:\",\"ms0:\"]},"
            "\"hle_profile\":\"generic\",\"feature_requirements\":[],"
            "\"verification_profile\":\"unverified\","
            "\"runtime_bindings\":{\"schema_version\":1,"
            "\"vblank_frame_counter_addr\":\"0x08804000\"}}";
        write_text_file(refusal_path, refused_manifest);
        char refusal_report[2048];
        assert(nk_title_manifest_load_overlay_dir(refusal_dir, refusal_report,
                                                  sizeof(refusal_report)) == 0);
        assert(strstr(refusal_report, "(disc TEST00001)") != NULL);
        assert(strstr(refusal_report, "(disc TEST00002)") != NULL);
        PlayerApp refusal_app;
        memset(&refusal_app, 0, sizeof(refusal_app));
        snprintf(refusal_app.title_manifest_report,
                 sizeof(refusal_app.title_manifest_report), "%s", refusal_report);
        char refusal_reason[512];
        assert(player_app_title_profile_refusal_for_disc(
            &refusal_app, "TEST00001", refusal_reason, sizeof(refusal_reason)));
        assert(strstr(refusal_reason, "vblank_frame_counter_addr") != NULL);
        assert(player_app_title_profile_refusal_for_disc(
            &refusal_app, "TEST00002", refusal_reason, sizeof(refusal_reason)));
        assert(strstr(refusal_reason, "vblank_frame_counter_addr") != NULL);
        remove(refusal_path);
        assert(test_rmdir(refusal_dir) == 0);

        executable_report.selected = NK_ISO_EXEC_SELECTION_NONE;
        executable_report.boot_fallback = false;
        executable_report.boot.kind = NK_ISO_EXEC_UNKNOWN;
        char decrypted_dir[1024], decrypted_elf[1280];
        int dir_written = snprintf(decrypted_dir, sizeof(decrypted_dir),
            "%s%ctitles%c%s%cdecrypted", preflight_root,
            nk_platform_path_separator(), nk_platform_path_separator(),
            synthetic_disc_id, nk_platform_path_separator());
        assert(dir_written > 0 && (size_t)dir_written < sizeof(decrypted_dir));
        assert(nk_platform_mkdir_p(decrypted_dir));
        int elf_written = snprintf(decrypted_elf, sizeof(decrypted_elf), "%s%cEBOOT.elf",
            decrypted_dir, nk_platform_path_separator());
        assert(elf_written > 0 && (size_t)elf_written < sizeof(decrypted_elf));
        remove(decrypted_elf);
        /* A key file at the old default location is ignored as well. */
        char old_key_dir[700], old_key_file[760], staging_dir[700];
        int key_written = snprintf(old_key_dir, sizeof(old_key_dir), "%s%ckeys",
                                   preflight_root, nk_platform_path_separator());
        assert(key_written > 0 && (size_t)key_written < sizeof(old_key_dir));
        assert(nk_platform_mkdir_p(old_key_dir));
        key_written = snprintf(old_key_file, sizeof(old_key_file), "%s%cpsp-keyfile.json",
                               old_key_dir, nk_platform_path_separator());
        assert(key_written > 0 && (size_t)key_written < sizeof(old_key_file));
        write_text_file(old_key_file, "{\"format\":\"nakagawa-psp-keystore-1\",\"entries\":{}}");
        key_written = snprintf(staging_dir, sizeof(staging_dir), "%s%ccache%cdecrypted",
                               preflight_root, nk_platform_path_separator(),
                               nk_platform_path_separator());
        assert(key_written > 0 && (size_t)key_written < sizeof(staging_dir));
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
        assert(check && check->status == PREFLIGHT_UNSUPPORTED);
        /* Built-in decryption runs and stops with a named result (this disc's
           EBOOT.BIN is not a sealed container); the card still names the
           folder a decrypted copy from the user's own disc can go in. */
        assert(strstr(check->message, "ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted and "
                                      "built-in decryption stopped: ") != NULL);
        assert(strstr(check->message, "Place a decrypted EBOOT.elf from your own copy in ")
               != NULL);
        assert(strstr(check->message, decrypted_dir) != NULL);
        assert(strstr(check->message, "key file") == NULL && strstr(check->message, "keyfile") == NULL);
        assert(strstr(check->message, g_decoy_key_path) == NULL);
        assert(check->issue_count == 1 && check->issue_numbers[0] == 308);
        /* Nothing was decrypted or staged. */
        {
            FILE *produced = fopen(decrypted_elf, "rb");
            assert(produced == NULL);
        }
        assert(!nk_platform_dir_exists(staging_dir));
        assert(remove(old_key_file) == 0);
        assert(test_rmdir(old_key_dir) == 0);

        write_synthetic_mips_elf(decrypted_elf);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
        assert(check && check->status == PREFLIGHT_OK);
        assert(strstr(check->message, "decrypted EBOOT.elf") != NULL);
        executable_report.eboot.kind = NK_ISO_EXEC_SCE_WRAPPER;
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
        assert(check && check->status == PREFLIGHT_OK);
        executable_report.eboot.kind = NK_ISO_EXEC_PBP;
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
        assert(check && check->status == PREFLIGHT_OK);
        executable_report.eboot.kind = NK_ISO_EXEC_PSP_ENCRYPTED;
        write_text_file(decrypted_elf, "not an ELF");
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
        assert(check && check->status == PREFLIGHT_UNSUPPORTED);
        assert(strstr(check->message, "not a usable MIPS ELF32") != NULL);
        assert(strstr(check->message, "replace it with a valid decrypted EBOOT.elf") != NULL);
        assert(strstr(check->message, "decryption is in the works") == NULL);
        assert(check->issue_count == 0);
        assert(remove(decrypted_elf) == 0);

        /* Built-in decryption (#295, owner decision 2026-10-27): an EBOOT.BIN
           sealed under one built-in game tag recipe comes out as a valid
           EBOOT.elf in the title's folder and the check passes; the next check
           uses that file; a copy the user put there is never replaced; and a
           firmware updater tag is refused by name, writing nothing. */
        {
            char sealed_iso[sizeof(wiz->inspecting_game.iso_path)];
            char staged_leftover[1300];
            int sealed_written;
            char saved_iso_path[sizeof(wiz->inspecting_game.iso_path)];
            unsigned char plain_elf[88];
            unsigned char user_elf[88];
            uint8_t sealed[0x150 + 96];
            unsigned char read_back[128];
            size_t sealed_len;
            FILE *produced;

            build_synthetic_mips_elf(plain_elf);
            sealed_len = seal_with_builtin_recipe(sealed, sizeof(sealed), plain_elf,
                                                  sizeof(plain_elf), 0xD91613F0u, 0xD91613F0u);
            sealed_written = snprintf(sealed_iso, sizeof(sealed_iso), "%s%cbuiltin-sealed.iso",
                                      preflight_root, nk_platform_path_separator());
            assert(sealed_written > 0 && (size_t)sealed_written < sizeof(sealed_iso));
            write_sysdir_eboot_iso(sealed_iso, sealed, sealed_len);
            snprintf(saved_iso_path, sizeof(saved_iso_path), "%s",
                     wiz->inspecting_game.iso_path);
            memcpy(wiz->inspecting_game.iso_path, sealed_iso, sizeof(sealed_iso));
            sealed_written = snprintf(staged_leftover, sizeof(staged_leftover), "%s.decrypting",
                                      decrypted_elf);
            assert(sealed_written > 0 && (size_t)sealed_written < sizeof(staged_leftover));

            player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
            check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
            assert(check && check->status == PREFLIGHT_OK);
            assert(strstr(check->message, "decrypted from the disc with the built-in tables") != NULL);
            produced = fopen(decrypted_elf, "rb");
            assert(produced != NULL);
            assert(fread(read_back, 1, sizeof(read_back), produced) == sizeof(plain_elf));
            assert(fclose(produced) == 0);
            assert(memcmp(read_back, plain_elf, sizeof(plain_elf)) == 0);
            assert(!nk_platform_file_exists(staged_leftover));

            /* The next check takes the copy now in the folder as it is. */
            player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
            check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
            assert(check && check->status == PREFLIGHT_OK);
            assert(strstr(check->message, "The decrypted EBOOT.elf in the title's folder") != NULL);

            /* A copy the user put there wins and is never replaced. */
            build_synthetic_mips_elf(user_elf);
            user_elf[sizeof(user_elf) - 1] ^= 0x5Au; /* differs from the disc's */
            produced = fopen(decrypted_elf, "wb");
            assert(produced != NULL);
            assert(fwrite(user_elf, 1, sizeof(user_elf), produced) == sizeof(user_elf));
            assert(fclose(produced) == 0);
            player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
            check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
            assert(check && check->status == PREFLIGHT_OK);
            produced = fopen(decrypted_elf, "rb");
            assert(produced != NULL);
            assert(fread(read_back, 1, sizeof(read_back), produced) == sizeof(user_elf));
            assert(fclose(produced) == 0);
            assert(memcmp(read_back, user_elf, sizeof(user_elf)) == 0);
            assert(remove(decrypted_elf) == 0);

            /* A firmware updater tag is refused before any key is read. */
            sealed_len = seal_with_builtin_recipe(sealed, sizeof(sealed), plain_elf,
                                                  sizeof(plain_elf), 0xD91613F0u, 0x0B000000u);
            write_sysdir_eboot_iso(sealed_iso, sealed, sealed_len);
            player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
            check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
            assert(check && check->status == PREFLIGHT_UNSUPPORTED);
            assert(strstr(check->message, "built-in decryption stopped: FIRMWARE_OUT_OF_SCOPE") != NULL);
            assert(strstr(check->message, "never decrypts firmware") != NULL);
            assert(!nk_platform_file_exists(decrypted_elf));
            assert(!nk_platform_file_exists(staged_leftover));

            assert(remove(sealed_iso) == 0);
            snprintf(wiz->inspecting_game.iso_path, sizeof(wiz->inspecting_game.iso_path),
                     "%s", saved_iso_path);
        }

        /* Issue #729: a PSP PRX guest module whose e_entry is the unset
           0xFFFFFFFF is ready under the module rule, as a plain disc copy and
           as a user-supplied decrypted copy; a truncated copy is not ready. */
        {
            unsigned char prx_bytes[88];
            char module_iso_path[NK_MAX_PATH];
            char module_dir[NK_MAX_PATH];
            char module_copy[NK_MAX_PATH + 32];
            char saved_iso_path[sizeof(wiz->inspecting_game.iso_path)];
            char saved_disc_id[sizeof(wiz->inspecting_game.disc_id)];
            char saved_selected[sizeof(wiz->inspecting_game.selected_executable)];
            const PlayerPreflightCheck *module_check;
            int written;

            build_synthetic_guest_prx(prx_bytes);
            snprintf(saved_iso_path, sizeof(saved_iso_path), "%s",
                     wiz->inspecting_game.iso_path);
            snprintf(saved_disc_id, sizeof(saved_disc_id), "%s",
                     wiz->inspecting_game.disc_id);
            snprintf(saved_selected, sizeof(saved_selected), "%s",
                     wiz->inspecting_game.selected_executable);
            written = snprintf(module_iso_path, sizeof(module_iso_path),
                "%s%cguest-module-729.iso", preflight_root,
                nk_platform_path_separator());
            assert(written > 0 && (size_t)written < sizeof(module_iso_path));
            write_guest_module_iso(module_iso_path, "libfont.prx", prx_bytes,
                                   sizeof(prx_bytes));
            written = snprintf(module_dir, sizeof(module_dir), "%s%ctitles%cULUS99996%cdecrypted",
                preflight_root, nk_platform_path_separator(),
                nk_platform_path_separator(), nk_platform_path_separator());
            assert(written > 0 && (size_t)written < sizeof(module_dir));
            assert(nk_platform_mkdir_p(module_dir));
            written = snprintf(module_copy, sizeof(module_copy), "%s%clibfont.prx",
                module_dir, nk_platform_path_separator());
            assert(written > 0 && (size_t)written < sizeof(module_copy));
            snprintf(wiz->inspecting_game.iso_path, sizeof(wiz->inspecting_game.iso_path),
                     "%s", module_iso_path);
            snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id),
                     "%s", "ULUS99996");
            snprintf(wiz->inspecting_game.selected_executable,
                     sizeof(wiz->inspecting_game.selected_executable), "%s", "EBOOT.BIN");

            /* Plain disc copy only: the PRX is the module and needs no decryption. */
            remove(module_copy);
            player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
            module_check = find_preflight_check(&wiz->wizard.preflight, "GUEST_MODULES");
            assert(module_check && module_check->status == PREFLIGHT_OK);
            assert(strstr(module_check->message, "Guest modules: 1 of 1 ready.") != NULL);

            /* A valid PRX whose PT_LOAD file offset (84) and address (0) differ
               modulo p_align 0x10 is ready: segment bytes are copied from the file
               offset, so the layout rule has no congruence term. */
            {
                unsigned char offset_prx[88];
                build_synthetic_guest_prx(offset_prx);
                offset_prx[80] = 0x10; /* p_align */
                write_guest_module_iso(module_iso_path, "libfont.prx", offset_prx,
                                       sizeof(offset_prx));
                remove(module_copy);
                player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
                module_check = find_preflight_check(&wiz->wizard.preflight, "GUEST_MODULES");
                assert(module_check && module_check->status == PREFLIGHT_OK);
                assert(strstr(module_check->message, "Guest modules: 1 of 1 ready.") != NULL);
                write_guest_module_iso(module_iso_path, "libfont.prx", prx_bytes,
                                       sizeof(prx_bytes));
            }

            /* An encrypted disc module with no plain copy that the built-in
               tables cannot decrypt (this one is not a sealed container) is not
               ready: the check names the module, the result and the folder,
               ignores the key-file override, and writes nothing. */
            {
                unsigned char encrypted_prx[0x100];
                memset(encrypted_prx, 0, sizeof(encrypted_prx));
                memcpy(encrypted_prx, "~PSP", 4);
                encrypted_prx[0x27] = 1;    /* one segment */
                encrypted_prx[0x55] = 0x10; /* segment size 0x1000 */
                write_guest_module_iso(module_iso_path, "libfont.prx", encrypted_prx,
                                       sizeof(encrypted_prx));
                remove(module_copy);
                player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
                module_check = find_preflight_check(&wiz->wizard.preflight, "GUEST_MODULES");
                assert(module_check && module_check->status == PREFLIGHT_MISSING);
                assert(strstr(module_check->message, "Guest modules: 0 of 1 ready; "
                                                     "ENCRYPTED_MODULE: libfont.prx is "
                                                     "encrypted and built-in decryption "
                                                     "stopped: MALFORMED_INPUT (") != NULL);
                assert(strstr(module_check->message, "Place a decrypted libfont.prx "
                                                     "from your own copy in ") != NULL);
                assert(strstr(module_check->message, module_dir) != NULL);
                assert(strstr(module_check->message, "key file") == NULL &&
                       strstr(module_check->message, "keyfile") == NULL);
                assert(strstr(module_check->message, g_decoy_key_path) == NULL);
                {
                    FILE *produced = fopen(module_copy, "rb");
                    assert(produced == NULL);
                }

                /* The user's plain copy makes the encrypted disc module ready. */
                write_synthetic_guest_prx(module_copy, sizeof(prx_bytes));
                player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
                module_check = find_preflight_check(&wiz->wizard.preflight, "GUEST_MODULES");
                assert(module_check && module_check->status == PREFLIGHT_OK);
                assert(strstr(module_check->message, "Guest modules: 1 of 1 ready.") != NULL);
                assert(remove(module_copy) == 0);
                write_guest_module_iso(module_iso_path, "libfont.prx", prx_bytes,
                                       sizeof(prx_bytes));
            }

            /* A user-supplied decrypted copy that is a valid PRX is ready. */
            write_synthetic_guest_prx(module_copy, sizeof(prx_bytes));
            player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
            module_check = find_preflight_check(&wiz->wizard.preflight, "GUEST_MODULES");
            assert(module_check && module_check->status == PREFLIGHT_OK);
            assert(strstr(module_check->message, "Guest modules: 1 of 1 ready.") != NULL);

            /* A truncated user copy (header cut inside the program headers) is not. */
            write_synthetic_guest_prx(module_copy, 60);
            player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
            module_check = find_preflight_check(&wiz->wizard.preflight, "GUEST_MODULES");
            assert(module_check && module_check->status == PREFLIGHT_UNSUPPORTED);
            assert(strstr(module_check->message, "Guest modules: 0 of 1 ready") != NULL);
            assert(strstr(module_check->message, "not a usable plain module") != NULL);

            assert(remove(module_copy) == 0);
            assert(remove(module_iso_path) == 0);
            snprintf(wiz->inspecting_game.iso_path, sizeof(wiz->inspecting_game.iso_path),
                     "%s", saved_iso_path);
            snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id),
                     "%s", saved_disc_id);
            snprintf(wiz->inspecting_game.selected_executable,
                     sizeof(wiz->inspecting_game.selected_executable), "%s", saved_selected);
        }

        wiz->inspecting_game.is_experimental = true;
        snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id),
                 "ULUS99998");
        snprintf(wiz->inspecting_game.selected_executable,
                 sizeof(wiz->inspecting_game.selected_executable), "EBOOT.BIN");
        snprintf(wiz->inspecting_game.title_id, sizeof(wiz->inspecting_game.title_id),
                 "experimental-ulus99998");
        /* A prior interrupted run may have left this synthetic package. */
        remove_runtime_package_fixture(preflight_root, "ULUS99998",
                                       "experimental-ulus99998");
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        assert(wiz->wizard.preflight.count == 8);
        check = find_preflight_check(&wiz->wizard.preflight, "EXPERIMENTAL");
        assert(check && check->status == PREFLIGHT_IN_PROGRESS);
        assert(strstr(check->message,
                      "Experimental: this game has not been verified. Compatibility is unknown.") != NULL);
        assert(check->issue_count == 1 && check->issue_numbers[0] == 308);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        assert(check->issue_count == 1 && check->issue_numbers[0] == 308);

        /* Native v1 package checks bind the profile executable hash, the
           player ABI, and a package-contained executable path. */
        char experimental_package_dir[900], experimental_package_json[1100];
        char experimental_completion[1100];
        char experimental_report[1100], experimental_exe[1100], experimental_image[1100];
        char experimental_root[900], profile_dir[1000], profile_path[1200];
        snprintf(experimental_root, sizeof(experimental_root), "%s%cpackages%cULUS99998",
                 preflight_root, nk_platform_path_separator(), nk_platform_path_separator());
        snprintf(experimental_package_dir, sizeof(experimental_package_dir), "%s",
                 experimental_root);
        snprintf(experimental_package_json, sizeof(experimental_package_json), "%s%cpackage.json",
                 experimental_package_dir, nk_platform_path_separator());
        snprintf(experimental_completion, sizeof(experimental_completion),
                 "%s%ccompletion-manifest.json", experimental_package_dir,
                 nk_platform_path_separator());
        snprintf(experimental_report, sizeof(experimental_report), "%s%cbuild-report.json",
                 experimental_package_dir, nk_platform_path_separator());
        snprintf(experimental_exe, sizeof(experimental_exe), "%s%cexperimental-ulus99998.exe",
                 experimental_package_dir, nk_platform_path_separator());
        snprintf(experimental_image, sizeof(experimental_image), "%s%cexperimental-ulus99998_image.bin",
                 experimental_package_dir, nk_platform_path_separator());
        snprintf(profile_dir, sizeof(profile_dir), "%s%cexperimental%cULUS99998",
                 preflight_root, nk_platform_path_separator(), nk_platform_path_separator());
        snprintf(profile_path, sizeof(profile_path), "%s%cprofile.json", profile_dir,
                 nk_platform_path_separator());
        write_experimental_profile_fixture(preflight_root, "ULUS99998",
                                           "experimental-ulus99998", "EBOOT.BIN",
                                           FIXTURE_SHA256);

        /* #670: reading an experimental profile validates its embedded
         * manifest; it registers nothing. It therefore must not take a parser
         * overlay slot or publish a catalog generation. Before the fix it
         * validated through the storing parser, so every experimental package
         * validation advanced the catalog epoch (invalidating every cached
         * package status) and, with every overlay slot in use, overwrote the
         * last slot, which backs a registered overlay: that title vanished from
         * the catalog and the experimental entry took its place. */
        nk_title_catalog_clear_overlay();
        for (int slot = 0; slot < 8; slot++) {
            char slot_path[1100];
            char slot_json[1536];
            snprintf(slot_path, sizeof(slot_path), "%s%cslot-guard-%d.json",
                     preflight_root, nk_platform_path_separator(), slot);
            int slot_length = snprintf(slot_json, sizeof(slot_json),
                "{\"schema_version\":1,\"id\":\"slot-guard-%d\","
                "\"display_name\":\"Slot Guard %d\",\"kind\":\"retail\","
                "\"disc\":{\"id\":\"ULUS9973%d\",\"region\":\"NA\","
                "\"revision_policy\":\"exact-disc-id\"},"
                "\"executable\":{\"base\":0,\"entry\":0,\"bss_metadata_source\":\"none\","
                "\"extra_executable_spans\":[]},\"modules\":[],"
                "\"filesystem\":{\"data_root\":\"data\",\"memory_stick_root\":\"savedata\","
                "\"device_prefixes\":[\"disc0:\",\"ms0:\"]},\"hle_profile\":\"generic\","
                "\"feature_requirements\":[],\"verification_profile\":\"smoke\"}\n",
                slot, slot, slot);
            assert(slot_length > 0 && (size_t)slot_length < sizeof(slot_json));
            write_text_file(slot_path, slot_json);
            char slot_error[512] = "";
            if (!nk_title_manifest_load_overlay(slot_path, slot_error, sizeof(slot_error))) {
                fprintf(stderr, "[PLAYER_STATE_TEST] slot overlay %d: %s\n", slot, slot_error);
                assert(false);
            }
            remove(slot_path);
        }
        uint64_t epoch_before_profile_read = nk_title_catalog_epoch();
        NkTitleEntrySnapshot guard_profile = {0};
        char guard_hash[65] = "";
        char guard_error[512] = "";
        assert(nk_title_manifest_read_experimental_profile(
            preflight_root, "ULUS99998", "experimental-ulus99998", "EBOOT.BIN",
            &guard_profile, guard_hash, guard_error, sizeof(guard_error)));
        assert(strcmp(guard_profile.entry.id, "experimental-ulus99998") == 0);
        nk_title_catalog_snapshot_release(&guard_profile);
        assert(nk_title_catalog_epoch() == epoch_before_profile_read);
        for (int slot = 0; slot < 8; slot++) {
            char slot_id[32];
            char slot_disc[16];
            snprintf(slot_id, sizeof(slot_id), "slot-guard-%d", slot);
            snprintf(slot_disc, sizeof(slot_disc), "ULUS9973%d", slot);
            NkTitleEntrySnapshot slot_entry = {0};
            assert(nk_title_catalog_find_by_id(slot_id, &slot_entry));
            assert(strcmp(slot_entry.entry.primary_disc_id, slot_disc) == 0);
            nk_title_catalog_snapshot_release(&slot_entry);
            assert(nk_title_catalog_find_by_disc_id(slot_disc, &slot_entry));
            assert(strcmp(slot_entry.entry.id, slot_id) == 0);
            nk_title_catalog_snapshot_release(&slot_entry);
        }
        NkTitleEntrySnapshot leaked_profile = {0};
        assert(!nk_title_catalog_find_by_id("experimental-ulus99998", &leaked_profile));

        /* Writing a profile validates the same way and must not either. With
           no executable selected the writer reads nothing from the disc path. */
        char written_profile_id[64] = "";
        char write_error[512] = "";
        if (!nk_title_manifest_write_experimental_profile(
                "unread.iso", true, "ULUS99997", "Writer Guard", NULL, preflight_root,
                written_profile_id, sizeof(written_profile_id), write_error,
                sizeof(write_error))) {
            fprintf(stderr, "[PLAYER_STATE_TEST] experimental write: %s\n", write_error);
            assert(false);
        }
        assert(strcmp(written_profile_id, "experimental-ulus99997") == 0);
        /* Launcher review B1: that profile, written for a disc whose EBOOT.BIN
           is encrypted, names no executable yet. It reads back unbound, and
           never as bound to EBOOT.BIN. Before the fix the reader refused its
           empty selected_executable, so the title page said "Add the disc
           again" forever and BUILD PACKAGE stayed off. */
        {
            NkTitleEntrySnapshot unbound = {0};
            char unbound_error[512] = "";
            char unbound_hash[65] = "";
            if (!nk_title_manifest_read_unbound_experimental_profile(
                    preflight_root, "ULUS99997", "experimental-ulus99997", &unbound,
                    unbound_error, sizeof(unbound_error))) {
                fprintf(stderr, "[PLAYER_STATE_TEST] unbound profile read: %s\n",
                        unbound_error);
                assert(false);
            }
            assert(strcmp(unbound.entry.id, "experimental-ulus99997") == 0);
            assert(strcmp(unbound.entry.primary_disc_id, "ULUS99997") == 0);
            nk_title_catalog_snapshot_release(&unbound);
            assert(!nk_title_manifest_read_experimental_profile(
                preflight_root, "ULUS99997", "experimental-ulus99997", "EBOOT.BIN",
                &unbound, unbound_hash, unbound_error, sizeof(unbound_error)));
            nk_title_catalog_snapshot_release(&unbound);

            /* The title page's check: an experimental disc with an encrypted
               EBOOT.BIN, its decrypted EBOOT.elf in the title's folder and
               that unbound profile has valid title settings. */
            GameRecord saved_game = wiz->inspecting_game;
            NkIsoExecutableReport encrypted;
            char elf_dir[1024], elf_path[1280];
            unsigned char elf[88];
            FILE *elf_file;
            const PlayerPreflightCheck *profile_check;
            int elf_written = snprintf(elf_dir, sizeof(elf_dir), "%s%ctitles%cULUS99997%cdecrypted",
                                       preflight_root, nk_platform_path_separator(),
                                       nk_platform_path_separator(),
                                       nk_platform_path_separator());
            assert(elf_written > 0 && (size_t)elf_written < sizeof(elf_dir));
            assert(nk_platform_mkdir_p(elf_dir));
            elf_written = snprintf(elf_path, sizeof(elf_path), "%s%cEBOOT.elf", elf_dir,
                                   nk_platform_path_separator());
            assert(elf_written > 0 && (size_t)elf_written < sizeof(elf_path));
            build_synthetic_mips_elf(elf);
            elf_file = fopen(elf_path, "wb");
            assert(elf_file != NULL);
            assert(fwrite(elf, 1, sizeof(elf), elf_file) == sizeof(elf));
            assert(fclose(elf_file) == 0);
            memset(&wiz->inspecting_game, 0, sizeof(wiz->inspecting_game));
            snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id),
                     "ULUS99997");
            snprintf(wiz->inspecting_game.title_id, sizeof(wiz->inspecting_game.title_id),
                     "experimental-ulus99997");
            wiz->inspecting_game.is_experimental = true;
            wiz->inspecting_game.status = NK_STATUS_IDENTIFIED;
            memset(&encrypted, 0, sizeof(encrypted));
            encrypted.eboot.kind = NK_ISO_EXEC_PSP_ENCRYPTED;
            encrypted.selected = NK_ISO_EXEC_SELECTION_NONE;
            player_app_build_compatibility_preflight(wiz, true, true, &encrypted);
            profile_check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
            assert(profile_check && profile_check->status == PREFLIGHT_OK);
            profile_check = find_preflight_check(&wiz->wizard.preflight, "TITLE_PROFILE");
            if (!profile_check || profile_check->status != PREFLIGHT_OK) {
                fprintf(stderr, "[PLAYER_STATE_TEST] title profile: %s\n",
                        profile_check ? profile_check->message : "(missing)");
                assert(false);
            }
            assert(remove(elf_path) == 0);
            wiz->inspecting_game = saved_game;
        }
        assert(nk_title_catalog_epoch() == epoch_before_profile_read);
        for (int slot = 0; slot < 8; slot++) {
            char slot_id[32];
            snprintf(slot_id, sizeof(slot_id), "slot-guard-%d", slot);
            NkTitleEntrySnapshot slot_entry = {0};
            assert(nk_title_catalog_find_by_id(slot_id, &slot_entry));
            nk_title_catalog_snapshot_release(&slot_entry);
        }
        assert(!nk_title_catalog_find_by_id("experimental-ulus99997", &leaked_profile));
        nk_title_catalog_clear_overlay();

        NkTitleEntrySnapshot profile_snapshot = {0};
        char profile_hash[65] = "";
        char profile_error[512] = "";
        assert(nk_title_manifest_read_experimental_profile(
            preflight_root, "ULUS99998", "experimental-ulus99998", "EBOOT.BIN",
            &profile_snapshot, profile_hash, profile_error,
            sizeof(profile_error)));
        assert(strcmp(profile_snapshot.entry.id, "experimental-ulus99998") == 0);
        assert(strcmp(profile_snapshot.entry.display_name,
                      "Synthetic experiment") == 0);
        nk_title_catalog_clear_overlay();
        assert(strcmp(profile_snapshot.entry.id, "experimental-ulus99998") == 0);
        assert(strcmp(profile_snapshot.entry.display_name,
                      "Synthetic experiment") == 0);
        nk_title_catalog_snapshot_release(&profile_snapshot);
        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", SR_CPUSTATE_ABI_VERSION,
                                      "experimental-ulus99998.exe", FIXTURE_SHA256, NULL);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_OK);
        remove(experimental_completion);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_STALE);
        assert(strstr(check->message, "build it from the library") != NULL);
        assert(strstr(check->message, "#") == NULL);
        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", SR_CPUSTATE_ABI_VERSION,
                                      "experimental-ulus99998.exe", FIXTURE_SHA256, NULL);

        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", SR_CPUSTATE_ABI_VERSION,
                                      "experimental-ulus99998.exe",
                                      "0000000000000000000000000000000000000000000000000000000000000000",
                                      NULL);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_STALE);

        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", 99,
                                      "experimental-ulus99998.exe", FIXTURE_SHA256, NULL);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_INCOMPATIBLE);

        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", SR_CPUSTATE_ABI_VERSION,
                                      "../escape.exe", FIXTURE_SHA256, NULL);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_INCOMPATIBLE);

        remove(experimental_package_json);
        remove(experimental_completion);
        remove(experimental_report);
        remove(experimental_exe);
        remove(experimental_image);
        remove(profile_path);
        remove(package_json);
        remove(package_report);

        memset(&wiz->games[0], 0, sizeof(wiz->games[0]));
        snprintf(wiz->games[0].disc_id, sizeof(wiz->games[0].disc_id), "ULUS99998");
        snprintf(wiz->games[0].title_id, sizeof(wiz->games[0].title_id),
                 "experimental-ulus99998");
        snprintf(wiz->games[0].selected_executable,
                 sizeof(wiz->games[0].selected_executable), "EBOOT.BIN");
        wiz->games[0].is_experimental = true;
        wiz->game_count = 1;
        assert(!player_app_launch_game(wiz, 0));
        assert(strcmp(wiz->last_error.error_code, "RUNTIME_PACKAGE_NOT_READY") == 0);
        assert(strstr(wiz->last_error.message, "isn't ready") != NULL);
        assert(strstr(wiz->last_error.boundary_text, "from the library") != NULL);
        assert(strchr(wiz->last_error.boundary_text, '#') == NULL);
        assert(strstr(wiz->last_error.message, "#") == NULL);
        player_app_set_view(wiz, VIEW_EXPERIMENTAL_TITLE);
        assert(player_app_focus_count(wiz) == 2);

        remove(runtime_exe);
        remove(runtime_image);
        remove(font_path);
        assert(test_remove_tree(preflight_root));

        free(wiz);
    }

    /* 14. PSP system fonts: the native reader's verdict, slot import, manifest v2, removal,
       per-slot preflight, and the Fonts & System status text. The fonts are the golden
       two-glyph images generated by tools/pgf_writer.py (pgf_golden_vectors.h), never real fonts. */
    printf("[PLAYER_STATE_TEST] Subtest 14: PSP system font import and per-slot preflight\n");
    fflush(stdout);
    {
        char cache_test_root[512];
        char font_root[640];
        char check_dir[768];
        char source_dir[768];
        char path[1024];
        char err[512];
        char sha[65];
        char msg[512];
        uint64_t font_size = 0;
        NkFontSlotState states[NK_FONT_SLOT_COUNT];
        NkFontImportResult result;
        const char sep = nk_platform_path_separator();
        const char *no_choice[NK_FONT_SLOT_COUNT] = { NULL, NULL, NULL };
        const char *choose_b[NK_FONT_SLOT_COUNT] = { NULL, "dupe_b.pgf", NULL };
        bool remove_all[NK_FONT_SLOT_COUNT] = { true, true, true };
        bool remove_korean[NK_FONT_SLOT_COUNT] = { false, false, true };
        unsigned char mutated[sizeof(kGoldenLatinPgf)];

        assert(nk_platform_get_path(NK_PATH_CACHE, cache_test_root, sizeof(cache_test_root)));
        snprintf(font_root, sizeof(font_root), "%s%cplayer_font_test", cache_test_root, sep);
        snprintf(check_dir, sizeof(check_dir), "%s%ccheck", font_root, sep);
        snprintf(source_dir, sizeof(source_dir), "%s%csource", font_root, sep);
        /* The helper refuses a missing leaf on Windows, so the root is made first. */
        assert(nk_platform_mkdir_p(font_root));
        assert(test_remove_tree(font_root));
        assert(nk_platform_mkdir_p(check_dir));
        assert(nk_platform_mkdir_p(source_dir));

        /* 14a: the reader's verdict on a golden font, and on each refusal the task names. */
        snprintf(path, sizeof(path), "%s%cgolden_latin.pgf", check_dir, sep);
        write_binary_file(path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));
        assert(nk_font_validate_pgf(path, &font_size, sha, err, sizeof(err)) == true);
        assert(font_size == sizeof(kGoldenLatinPgf));
        assert(strlen(sha) == 64);
        {
            NkFontInspection inspection;
            assert(nk_font_inspect_pgf(path, &inspection, err, sizeof(err)) == true);
            assert(inspection.slot == NK_FONT_SLOT_LATIN);
            assert(inspection.verdict.has_latin == 1u);
            assert(inspection.verdict.glyph_count == 2u);
        }

        snprintf(path, sizeof(path), "%s%cbad_truncated.pgf", check_dir, sep);
        write_binary_file(path, kGoldenLatinPgf, 100u);
        assert(nk_font_validate_pgf(path, &font_size, sha, err, sizeof(err)) == false);
        assert(strstr(err, "truncated") != NULL);

        memcpy(mutated, kGoldenLatinPgf, sizeof(mutated));
        memcpy(mutated + 4, "BAD0", 4u);
        snprintf(path, sizeof(path), "%s%cbad_magic.pgf", check_dir, sep);
        write_binary_file(path, mutated, sizeof(mutated));
        assert(nk_font_validate_pgf(path, &font_size, sha, err, sizeof(err)) == false);
        assert(strstr(err, "invalid PGF magic") != NULL);

        memcpy(mutated, kGoldenLatinPgf, sizeof(mutated));
        mutated[0xb6] = 0x62; /* first glyph 0x62 > last glyph 0x61 */
        mutated[0xb7] = 0x00;
        snprintf(path, sizeof(path), "%s%cbad_range.pgf", check_dir, sep);
        write_binary_file(path, mutated, sizeof(mutated));
        assert(nk_font_validate_pgf(path, &font_size, sha, err, sizeof(err)) == false);
        assert(strstr(err, "corrupt glyph indices") != NULL);

        memcpy(mutated, kGoldenLatinPgf, sizeof(mutated));
        mutated[8] = 4; /* revision 4 */
        snprintf(path, sizeof(path), "%s%cbad_revision.pgf", check_dir, sep);
        write_binary_file(path, mutated, sizeof(mutated));
        assert(nk_font_validate_pgf(path, &font_size, sha, err, sizeof(err)) == false);
        assert(strstr(err, "unsupported revision") != NULL);

        memcpy(mutated, kGoldenLatinPgf, sizeof(mutated));
        mutated[2] = 0x9c; /* a 412-byte header declared under revision 2 */
        mutated[3] = 0x01;
        snprintf(path, sizeof(path), "%s%cbad_header_412.pgf", check_dir, sep);
        write_binary_file(path, mutated, sizeof(mutated));
        assert(nk_font_validate_pgf(path, &font_size, sha, err, sizeof(err)) == false);
        assert(strstr(err, "header size") != NULL);

        snprintf(path, sizeof(path), "%s%coversize.pgf", check_dir, sep);
        {
            FILE *big = fopen(path, "wb");
            assert(big != NULL);
            assert(fseek(big, (long)NK_FONT_PGF_MAX_BYTES, SEEK_SET) == 0);
            assert(fputc(0, big) == 0);
            assert(fclose(big) == 0);
        }
        assert(nk_font_validate_pgf(path, &font_size, sha, err, sizeof(err)) == false);
        assert(strstr(err, "16 MiB") != NULL);

        /* 14b: a folder with one file per slot, a refused file, and a file that is not a font.
           Names do not decide the slot; the coverage does. */
        snprintf(path, sizeof(path), "%s%cnotes.txt", source_dir, sep);
        write_text_file(path, "not a font");
        snprintf(path, sizeof(path), "%s%cdump_one.pgf", source_dir, sep);
        write_binary_file(path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));
        snprintf(path, sizeof(path), "%s%cdump_two.pgf", source_dir, sep);
        write_binary_file(path, kGoldenJapanesePgf, sizeof(kGoldenJapanesePgf));
        snprintf(path, sizeof(path), "%s%cdump_three.pgf", source_dir, sep);
        write_binary_file(path, kGoldenKoreanPgf, sizeof(kGoldenKoreanPgf));
        snprintf(path, sizeof(path), "%s%cdump_broken.pgf", source_dir, sep);
        write_binary_file(path, kGoldenLatinPgf, 100u);

        assert(nk_font_check_cache(font_root, msg, sizeof(msg)) == NK_FONT_STATUS_MISSING);
        memset(&result, 0, sizeof(result));
        assert(nk_font_import_folder(font_root, source_dir, no_choice, &result, err, sizeof(err)));
        assert(result.file_count == 4);
        assert(result.imported_count == 3);
        assert(result.slot_imported[NK_FONT_SLOT_JAPANESE]);
        assert(result.slot_imported[NK_FONT_SLOT_LATIN]);
        assert(result.slot_imported[NK_FONT_SLOT_KOREAN]);

        /* The cache is fonts/v2 with the served names, and the manifest lists them. */
        snprintf(path, sizeof(path), "%s%cfonts%cv2%cmanifest.json", font_root, sep, sep, sep);
        assert(nk_platform_file_exists(path));
        assert(nk_font_check_cache(font_root, msg, sizeof(msg)) == NK_FONT_STATUS_OK);
        snprintf(path, sizeof(path), "%s%cfonts%cv2%cnkltn.pgf", font_root, sep, sep, sep);
        assert(nk_platform_file_exists(path));
        snprintf(path, sizeof(path), "%s%cfonts%cv2%cnkjpn.pgf", font_root, sep, sep, sep);
        assert(nk_platform_file_exists(path));
        snprintf(path, sizeof(path), "%s%cfonts%cv2%cnkkr.pgf", font_root, sep, sep, sep);
        assert(nk_platform_file_exists(path));

        /* Per-slot status: every slot is the user's, with plain-language text. */
        nk_font_slot_states(font_root, font_root, states);
        for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
            assert(states[slot].source == NK_FONT_SOURCE_USER);
        }
        assert(strstr(states[NK_FONT_SLOT_JAPANESE].detail, "your imported font") != NULL);

        /* 14c: removal drops only the cache's slot files and the manifest; originals stay. */
        assert(nk_font_remove_imports(font_root, remove_all, err, sizeof(err)) == 3);
        assert(nk_font_check_cache(font_root, msg, sizeof(msg)) == NK_FONT_STATUS_MISSING);
        snprintf(path, sizeof(path), "%s%cdump_one.pgf", source_dir, sep);
        assert(nk_platform_file_exists(path));
        nk_font_slot_states(font_root, font_root, states);
        for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
            assert(states[slot].source == NK_FONT_SOURCE_NONE);
            assert(strstr(states[slot].detail, "missing") != NULL);
        }

        /* 14d: two files name the Latin slot and none is chosen: the slot is left alone. A
           choice imports exactly the chosen file. */
        snprintf(path, sizeof(path), "%s%cdump_two.pgf", source_dir, sep);
        remove(path);
        snprintf(path, sizeof(path), "%s%cdump_three.pgf", source_dir, sep);
        remove(path);
        snprintf(path, sizeof(path), "%s%cdump_one.pgf", source_dir, sep);
        remove(path);
        snprintf(path, sizeof(path), "%s%cdupe_a.pgf", source_dir, sep);
        write_binary_file(path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));
        snprintf(path, sizeof(path), "%s%cdupe_b.pgf", source_dir, sep);
        write_binary_file(path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));
        memset(&result, 0, sizeof(result));
        assert(nk_font_import_folder(font_root, source_dir, no_choice, &result, err, sizeof(err)));
        assert(result.imported_count == 0);
        assert(!result.slot_imported[NK_FONT_SLOT_LATIN]);
        {
            bool said_choose = false;
            for (int i = 0; i < result.file_count; i++) {
                if (strstr(result.files[i].detail, "choose one") != NULL) said_choose = true;
            }
            assert(said_choose);
        }
        memset(&result, 0, sizeof(result));
        assert(nk_font_import_folder(font_root, source_dir, choose_b, &result, err, sizeof(err)));
        assert(result.imported_count == 1);
        assert(result.slot_imported[NK_FONT_SLOT_LATIN]);
        assert(nk_font_remove_imports(font_root, remove_all, err, sizeof(err)) == 1);

        /* 14e: a folder with no .pgf file is refused by name. */
        {
            char empty_dir[800];
            snprintf(empty_dir, sizeof(empty_dir), "%s%cempty", font_root, sep);
            assert(nk_platform_mkdir_p(empty_dir));
            memset(&result, 0, sizeof(result));
            assert(!nk_font_import_folder(font_root, empty_dir, no_choice, &result, err, sizeof(err)));
            assert(strstr(err, "no .pgf files") != NULL);
        }

        /* 14f: the wizard's status text and preflight, through the player's own functions. The
           folder now holds two Latin files and a refused one, so the first import imports nothing. */
        /* The Fonts step reads and writes the per-user data directory, so this block points that
           directory at its own font root, the same folder the runtime root names here. */
        assert(nk_platform_set_app_data_dir_override(font_root));
        PlayerApp *font_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(font_app != NULL);
        nk_library_init(&font_app->library);
        player_app_set_runtime_root(font_app, font_root);
        snprintf(font_app->inspecting_game.disc_id, sizeof(font_app->inspecting_game.disc_id), "UCUS98701");
        snprintf(font_app->inspecting_game.title_id, sizeof(font_app->inspecting_game.title_id),
                 "synthetic-allegrex-v1");
        NkIsoExecutableReport exec_rep;
        memset(&exec_rep, 0, sizeof(exec_rep));
        exec_rep.eboot.kind = NK_ISO_EXEC_PSP_ENCRYPTED;
        exec_rep.boot.kind = NK_ISO_EXEC_MIPS_ELF32;
        exec_rep.selected = NK_ISO_EXEC_SELECTION_BOOT;
        exec_rep.boot_fallback = true;
        snprintf(exec_rep.selected_path, sizeof(exec_rep.selected_path), "BOOT.BIN");

        assert(player_app_fonts_import_folder(font_app, source_dir) == false);
        assert(strstr(font_app->wizard.font_message, "No font was imported") != NULL);
        snprintf(path, sizeof(path), "%s%cdupe_a.pgf", source_dir, sep);
        remove(path);
        snprintf(path, sizeof(path), "%s%cdupe_b.pgf", source_dir, sep);
        remove(path);

        snprintf(path, sizeof(path), "%s%cdump_one.pgf", source_dir, sep);
        write_binary_file(path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));
        snprintf(path, sizeof(path), "%s%cdump_two.pgf", source_dir, sep);
        write_binary_file(path, kGoldenJapanesePgf, sizeof(kGoldenJapanesePgf));
        snprintf(path, sizeof(path), "%s%cdump_three.pgf", source_dir, sep);
        write_binary_file(path, kGoldenKoreanPgf, sizeof(kGoldenKoreanPgf));
        assert(player_app_fonts_import_folder(font_app, source_dir));
        assert(strstr(font_app->wizard.font_message, "Imported 3 PSP font file(s)") != NULL);
        assert(strstr(font_app->wizard.font_slot_detail[NK_FONT_SLOT_KOREAN], "your imported font") != NULL);

        /* Korean removed on its own: preflight is MISSING and names the slot. */
        assert(nk_font_remove_imports(font_root, remove_korean, err, sizeof(err)) == 1);
        player_app_refresh_font_status(font_app);
        player_app_build_compatibility_preflight(font_app, true, true, &exec_rep);
        {
            const PlayerPreflightCheck *fcheck = find_preflight_check(&font_app->wizard.preflight, "SYSTEM_FONTS");
            assert(fcheck != NULL && fcheck->status == PREFLIGHT_MISSING);
            assert(strstr(fcheck->message, "Korean: missing") != NULL);
        }

        /* A project font fills the Korean slot: preflight is then complete. */
        snprintf(path, sizeof(path), "%s%cfont", font_root, sep);
        assert(nk_platform_mkdir_p(path));
        snprintf(path, sizeof(path), "%s%cfont%cnkkr.pgf", font_root, sep, sep);
        write_binary_file(path, kGoldenKoreanPgf, sizeof(kGoldenKoreanPgf));
        player_app_refresh_font_status(font_app);
        assert(strstr(font_app->wizard.font_slot_detail[NK_FONT_SLOT_KOREAN], "project font") != NULL);
        player_app_build_compatibility_preflight(font_app, true, true, &exec_rep);
        {
            const PlayerPreflightCheck *fcheck = find_preflight_check(&font_app->wizard.preflight, "SYSTEM_FONTS");
            assert(fcheck != NULL && fcheck->status == PREFLIGHT_OK);
            assert(strstr(fcheck->message, "Korean: project font") != NULL);
            assert(strstr(fcheck->message, "Japanese: your imported font") != NULL);
        }
        assert(player_app_fonts_remove_imports(font_app));
        assert(strstr(font_app->wizard.font_message, "Removed 2 imported PSP font file(s)") != NULL);

        /* 14g: a slot file that cannot be deleted keeps its manifest entry. The Japanese file is
           replaced by a directory holding a file, so the delete fails for a reason other than the
           file being missing, on POSIX (unlink) and Win32 (DeleteFileW) alike. Japanese is named
           first, so the call must go on past it: the Latin file is removed and its entry dropped,
           and the manifest keeps the entries whose files remain. */
        {
            char blocker[NATIVE_TEST_PATH_MAX + 32];
            const bool remove_japanese_and_latin[NK_FONT_SLOT_COUNT] = { true, true, false };
            int rc;
            memset(&result, 0, sizeof(result));
            assert(nk_font_import_folder(font_root, source_dir, no_choice, &result, err, sizeof(err)));
            assert(result.imported_count == 3);
            snprintf(path, sizeof(path), "%s%cfonts%cv2%cnkjpn.pgf", font_root, sep, sep, sep);
            assert(remove(path) == 0);
            assert(nk_platform_mkdir_p(path));
            snprintf(blocker, sizeof(blocker), "%s%cblock.txt", path, sep);
            write_text_file(blocker, "a directory is not removed by the slot delete");

            rc = nk_font_remove_imports(font_root, remove_japanese_and_latin, err, sizeof(err));
            assert(rc == -1);
            assert(strstr(err, "could not delete the Japanese font file") != NULL);
            snprintf(path, sizeof(path), "%s%cfonts%cv2%cnkltn.pgf", font_root, sep, sep, sep);
            assert(!nk_platform_file_exists(path));
            snprintf(path, sizeof(path), "%s%cfonts%cv2%cnkjpn.pgf", font_root, sep, sep, sep);
            assert(nk_platform_dir_exists(path));
            snprintf(path, sizeof(path), "%s%cfonts%cv2%cnkkr.pgf", font_root, sep, sep, sep);
            assert(nk_platform_file_exists(path));

            /* Read back through the API: the manifest still lists Japanese, so the cache is
               INVALID and names it; Korean is still served; Latin is gone. */
            assert(nk_font_check_cache(font_root, msg, sizeof(msg)) == NK_FONT_STATUS_INVALID);
            assert(strstr(msg, "Japanese") != NULL);
            nk_font_slot_states(font_root, font_root, states);
            assert(states[NK_FONT_SLOT_LATIN].source == NK_FONT_SOURCE_NONE);
            assert(states[NK_FONT_SLOT_JAPANESE].source != NK_FONT_SOURCE_USER);
            assert(strstr(states[NK_FONT_SLOT_JAPANESE].detail, "missing") != NULL);
            assert(states[NK_FONT_SLOT_KOREAN].source == NK_FONT_SOURCE_USER);

            /* The player's Fonts step reports the same outcome: Korean, which is healthy, is
               removed, and the message names the Japanese file that stayed. */
            assert(!player_app_fonts_remove_imports(font_app));
            assert(strstr(font_app->wizard.font_message, "Could not remove the imported fonts") != NULL);
            assert(strstr(font_app->wizard.font_message, "Japanese") != NULL);
            snprintf(path, sizeof(path), "%s%cfonts%cv2%cnkkr.pgf", font_root, sep, sep, sep);
            assert(!nk_platform_file_exists(path));
            assert(nk_font_check_cache(font_root, msg, sizeof(msg)) == NK_FONT_STATUS_INVALID);
            assert(strstr(msg, "Japanese") != NULL);

            /* Once the blocker is gone the file is missing, which is not a failure: the entry is
               dropped, nothing is left, and the manifest is removed. */
            snprintf(path, sizeof(path), "%s%cfonts%cv2%cnkjpn.pgf", font_root, sep, sep, sep);
            assert(test_remove_tree(path));
            assert(nk_font_remove_imports(font_root, remove_all, err, sizeof(err)) == 0);
            assert(nk_font_check_cache(font_root, msg, sizeof(msg)) == NK_FONT_STATUS_MISSING);
        }

        /* 14h: when every named delete fails, nothing changed, so the manifest is not rewritten:
           its bytes are the same afterwards, and the call fails naming all three slots. */
        {
            char blocker[NATIVE_TEST_PATH_MAX + 32];
            char manifest_file[1024];
            char before[4096];
            char after[4096];
            long before_length;
            long after_length;
            const char *slot_files[NK_FONT_SLOT_COUNT] = {
                NK_FONT_SLOT_JAPANESE_FILE, NK_FONT_SLOT_LATIN_FILE, NK_FONT_SLOT_KOREAN_FILE
            };
            memset(&result, 0, sizeof(result));
            assert(nk_font_import_folder(font_root, source_dir, no_choice, &result, err, sizeof(err)));
            assert(result.imported_count == 3);
            for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
                snprintf(path, sizeof(path), "%s%cfonts%cv2%c%s", font_root, sep, sep, sep, slot_files[slot]);
                assert(remove(path) == 0);
                assert(nk_platform_mkdir_p(path));
                snprintf(blocker, sizeof(blocker), "%s%cblock.txt", path, sep);
                write_text_file(blocker, "a directory is not removed by the slot delete");
            }
            snprintf(manifest_file, sizeof(manifest_file), "%s%cfonts%cv2%cmanifest.json", font_root, sep, sep, sep);
            before_length = read_small_file(manifest_file, before, sizeof(before));
            assert(before_length > 0);
            assert(nk_font_remove_imports(font_root, remove_all, err, sizeof(err)) == -1);
            assert(strstr(err, "Japanese") != NULL);
            assert(strstr(err, "Latin") != NULL);
            assert(strstr(err, "Korean") != NULL);
            after_length = read_small_file(manifest_file, after, sizeof(after));
            assert(after_length == before_length);
            assert(memcmp(before, after, (size_t)before_length) == 0);
            assert(nk_font_check_cache(font_root, msg, sizeof(msg)) == NK_FONT_STATUS_INVALID);

            /* Clear the blockers: the same call then drops every entry and removes the manifest. */
            for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
                snprintf(path, sizeof(path), "%s%cfonts%cv2%c%s", font_root, sep, sep, sep, slot_files[slot]);
                assert(test_remove_tree(path));
            }
            assert(nk_font_remove_imports(font_root, remove_all, err, sizeof(err)) == 0);
            assert(nk_font_check_cache(font_root, msg, sizeof(msg)) == NK_FONT_STATUS_MISSING);
        }

        /* 14i: the manifest's staging name is blocked, so the rewrite fails after the Latin file
           is gone. The call reports that the manifest could not be rewritten, and the manifest
           still lists Latin until a later call drops the entry. */
        {
            const bool remove_latin[NK_FONT_SLOT_COUNT] = { false, true, false };
            int rc;
            memset(&result, 0, sizeof(result));
            assert(nk_font_import_folder(font_root, source_dir, no_choice, &result, err, sizeof(err)));
            assert(result.imported_count == 3);
            snprintf(path, sizeof(path), "%s%cfonts%cv2%cmanifest.json.tmp", font_root, sep, sep, sep);
            assert(nk_platform_mkdir_p(path));
            rc = nk_font_remove_imports(font_root, remove_latin, err, sizeof(err));
            assert(rc == -1);
            assert(strstr(err, "the font manifest could not be rewritten") != NULL);
            snprintf(path, sizeof(path), "%s%cfonts%cv2%cnkltn.pgf", font_root, sep, sep, sep);
            assert(!nk_platform_file_exists(path));
            assert(nk_font_check_cache(font_root, msg, sizeof(msg)) == NK_FONT_STATUS_INVALID);
            assert(strstr(msg, "Latin") != NULL);
            snprintf(path, sizeof(path), "%s%cfonts%cv2%cmanifest.json.tmp", font_root, sep, sep, sep);
            assert(test_remove_tree(path));
            assert(nk_font_remove_imports(font_root, remove_all, err, sizeof(err)) == 2);
            assert(nk_font_check_cache(font_root, msg, sizeof(msg)) == NK_FONT_STATUS_MISSING);
        }

        /* A corrupt manifest is INVALID, and the preflight names the issue (#313). */
        snprintf(path, sizeof(path), "%s%cfonts%cv2", font_root, sep, sep);
        assert(nk_platform_mkdir_p(path));
        snprintf(path, sizeof(path), "%s%cfonts%cv2%cmanifest.json", font_root, sep, sep, sep);
        write_text_file(path, "{ broken json: true ");
        player_app_build_compatibility_preflight(font_app, true, true, &exec_rep);
        {
            const PlayerPreflightCheck *fcheck = find_preflight_check(&font_app->wizard.preflight, "SYSTEM_FONTS");
            assert(fcheck != NULL && fcheck->status == PREFLIGHT_INVALID);
            assert(fcheck->issue_count == 1 && fcheck->issue_numbers[0] == 313);
        }
        free(font_app);
        assert(nk_platform_set_app_data_dir_override(NULL));
        assert(test_remove_tree(font_root));
    }

    /* 14r: a runtime root does not move the fonts. Under --runtime-root the Fonts step imports into
       and reads from the per-user data directory (the --user-data-root override here), and the
       runtime root's own fonts/v2 is neither written nor read. */
    printf("[PLAYER_STATE_TEST] Subtest 14r: Fonts step under a runtime root uses the per-user directory\n");
    fflush(stdout);
    {
        char rt_base[512];
        char per_user[768];
        char runtime_only[768];
        char source[768];
        char path[1024];
        const char sep = nk_platform_path_separator();

        assert(nk_platform_get_path(NK_PATH_CACHE, rt_base, sizeof(rt_base)));
        snprintf(per_user, sizeof(per_user), "%s%cfont_root_per_user", rt_base, sep);
        snprintf(runtime_only, sizeof(runtime_only), "%s%cfont_root_runtime", rt_base, sep);
        snprintf(source, sizeof(source), "%s%cfont_root_source", rt_base, sep);
        assert(test_remove_tree(per_user));
        assert(test_remove_tree(runtime_only));
        assert(test_remove_tree(source));
        assert(nk_platform_mkdir_p(per_user));
        assert(nk_platform_mkdir_p(runtime_only));
        assert(nk_platform_mkdir_p(source));
        snprintf(path, sizeof(path), "%s%cdump_latin.pgf", source, sep);
        write_binary_file(path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));

        assert(nk_platform_set_app_data_dir_override(per_user));
        PlayerApp *runtime_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(runtime_app != NULL);
        nk_library_init(&runtime_app->library);
        player_app_set_runtime_root(runtime_app, runtime_only);

        assert(player_app_fonts_import_folder(runtime_app, source));
        assert(strstr(runtime_app->wizard.font_message, "Imported 1 PSP font file(s)") != NULL);
        snprintf(path, sizeof(path), "%s%cfonts%cv2%cmanifest.json", per_user, sep, sep, sep);
        assert(nk_platform_file_exists(path));
        snprintf(path, sizeof(path), "%s%cfonts", runtime_only, sep);
        assert(!nk_platform_dir_exists(path));

        player_app_refresh_font_status(runtime_app);
        assert(strstr(runtime_app->wizard.font_slot_detail[NK_FONT_SLOT_LATIN],
                      "your imported font") != NULL);
        assert(player_app_fonts_remove_imports(runtime_app));
        free(runtime_app);

        /* 14r-b (#827 review): the runtime root holds the project latin font and the per-user
           directory holds no import. The Fonts step and the SYSTEM_FONTS preflight must both name
           that project font, read from <runtime root>/font, not from the per-user directory. */
        {
            char project_font_dir[1024];
            char project_latin[1100];
            NkIsoExecutableReport project_exec;
            snprintf(project_font_dir, sizeof(project_font_dir), "%s%cfont", runtime_only, sep);
            assert(nk_platform_mkdir_p(project_font_dir));
            snprintf(project_latin, sizeof(project_latin), "%s%cnkltn.pgf", project_font_dir, sep);
            write_binary_file(project_latin, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));

            PlayerApp *project_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
            assert(project_app != NULL);
            nk_library_init(&project_app->library);
            player_app_set_runtime_root(project_app, runtime_only);
            snprintf(project_app->inspecting_game.disc_id,
                     sizeof(project_app->inspecting_game.disc_id), "TEST80001");
            snprintf(project_app->inspecting_game.title_id,
                     sizeof(project_app->inspecting_game.title_id), "synthetic-allegrex-v1");
            memset(&project_exec, 0, sizeof(project_exec));
            project_exec.eboot.kind = NK_ISO_EXEC_PSP_ENCRYPTED;
            project_exec.boot.kind = NK_ISO_EXEC_MIPS_ELF32;
            project_exec.selected = NK_ISO_EXEC_SELECTION_BOOT;
            project_exec.boot_fallback = true;
            snprintf(project_exec.selected_path, sizeof(project_exec.selected_path), "BOOT.BIN");

            player_app_refresh_font_status(project_app);
            assert(strstr(project_app->wizard.font_latin_index_detail[0],
                          "L1 missing; project nkltn.pgf") != NULL);
            player_app_build_compatibility_preflight(project_app, true, true, &project_exec);
            {
                const PlayerPreflightCheck *fcheck =
                    find_preflight_check(&project_app->wizard.preflight, "SYSTEM_FONTS");
                assert(fcheck != NULL);
                assert(strstr(fcheck->message, "L1 missing; project nkltn.pgf") != NULL);
            }
            free(project_app);
            assert(test_remove_tree(project_font_dir));
        }

        assert(nk_platform_set_app_data_dir_override(NULL));
        assert(test_remove_tree(per_user));
        assert(test_remove_tree(runtime_only));
        assert(test_remove_tree(source));
    }

    /* 14g: no configured runtime root. The preflight falls back to the app-data directory, and
       the Fonts step must read and write that same directory. The directory is also the project
       root the preflight checks, so a project font there is named by both surfaces. */
    printf("[PLAYER_STATE_TEST] Subtest 14g: Fonts step and preflight with no runtime root\n");
    fflush(stdout);
    {
        char app_data[1024];
        char cache_root[1024];
        char project_dir[1200];
        char project_path[1300];
        char fonts_dir[1300];
        char manifest_path[1400];
        char source_dir[1300];
        char source_path[1400];
        const char sep = nk_platform_path_separator();
        NkIsoExecutableReport exec_rep;

        assert(nk_platform_get_app_data_dir(app_data, sizeof(app_data)));
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_root, sizeof(cache_root)));
        snprintf(project_dir, sizeof(project_dir), "%s%cfont", app_data, sep);
        snprintf(project_path, sizeof(project_path), "%s%cnkkr.pgf", project_dir, sep);
        snprintf(fonts_dir, sizeof(fonts_dir), "%s%cfonts", app_data, sep);
        snprintf(manifest_path, sizeof(manifest_path), "%s%cfonts%cv2%cmanifest.json",
                 app_data, sep, sep, sep);
        snprintf(source_dir, sizeof(source_dir), "%s%cplayer_font_fallback_source",
                 cache_root, sep);

        /* Start with no project or imported fonts under the app-data directory. */
        assert(test_remove_tree(project_dir));
        assert(test_remove_tree(fonts_dir));
        assert(test_remove_tree(source_dir));
        assert(nk_platform_mkdir_p(project_dir));
        assert(nk_platform_mkdir_p(source_dir));
        write_binary_file(project_path, kGoldenKoreanPgf, sizeof(kGoldenKoreanPgf));
        snprintf(source_path, sizeof(source_path), "%s%cdump_one.pgf", source_dir, sep);
        write_binary_file(source_path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));
        snprintf(source_path, sizeof(source_path), "%s%cdump_two.pgf", source_dir, sep);
        write_binary_file(source_path, kGoldenJapanesePgf, sizeof(kGoldenJapanesePgf));
        snprintf(source_path, sizeof(source_path), "%s%cdump_three.pgf", source_dir, sep);
        write_binary_file(source_path, kGoldenKoreanPgf, sizeof(kGoldenKoreanPgf));

        PlayerApp *fallback_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(fallback_app != NULL);
        nk_library_init(&fallback_app->library);
        assert(fallback_app->runtime_root[0] == '\0');
        snprintf(fallback_app->inspecting_game.disc_id,
                 sizeof(fallback_app->inspecting_game.disc_id), "TEST80001");
        snprintf(fallback_app->inspecting_game.title_id,
                 sizeof(fallback_app->inspecting_game.title_id), "synthetic-allegrex-v1");
        memset(&exec_rep, 0, sizeof(exec_rep));
        exec_rep.eboot.kind = NK_ISO_EXEC_PSP_ENCRYPTED;
        exec_rep.boot.kind = NK_ISO_EXEC_MIPS_ELF32;
        exec_rep.selected = NK_ISO_EXEC_SELECTION_BOOT;
        exec_rep.boot_fallback = true;
        snprintf(exec_rep.selected_path, sizeof(exec_rep.selected_path), "BOOT.BIN");

        /* 1. The app-data project font is named by both the Fonts step and the preflight. */
        player_app_refresh_font_status(fallback_app);
        assert(strstr(fallback_app->wizard.font_slot_detail[NK_FONT_SLOT_KOREAN],
                      "Korean: project font.") != NULL);
        player_app_build_compatibility_preflight(fallback_app, true, true, &exec_rep);
        {
            const PlayerPreflightCheck *fcheck =
                find_preflight_check(&fallback_app->wizard.preflight, "SYSTEM_FONTS");
            assert(fcheck != NULL && fcheck->status == PREFLIGHT_MISSING);
            assert(strstr(fcheck->message, "Korean: project font.") != NULL);
        }

        /* 2. The import writes the cache under app-data, and the preflight reads it there. */
        assert(player_app_fonts_import_folder(fallback_app, source_dir));
        assert(strstr(fallback_app->wizard.font_message, "Imported 3 PSP font file(s)") != NULL);
        assert(nk_platform_file_exists(manifest_path));
        assert(strstr(fallback_app->wizard.font_slot_detail[NK_FONT_SLOT_KOREAN],
                      "Korean: your imported font.") != NULL);
        player_app_build_compatibility_preflight(fallback_app, true, true, &exec_rep);
        {
            const PlayerPreflightCheck *fcheck =
                find_preflight_check(&fallback_app->wizard.preflight, "SYSTEM_FONTS");
            assert(fcheck != NULL && fcheck->status == PREFLIGHT_OK);
            assert(strstr(fcheck->message, "Korean: your imported font.") != NULL);
        }

        /* 3. Removing the imports leaves the project font, and both surfaces name it again. */
        assert(player_app_fonts_remove_imports(fallback_app));
        assert(strstr(fallback_app->wizard.font_message,
                      "Removed 3 imported PSP font file(s)") != NULL);
        assert(strstr(fallback_app->wizard.font_slot_detail[NK_FONT_SLOT_KOREAN],
                      "Korean: project font.") != NULL);
        player_app_build_compatibility_preflight(fallback_app, true, true, &exec_rep);
        {
            const PlayerPreflightCheck *fcheck =
                find_preflight_check(&fallback_app->wizard.preflight, "SYSTEM_FONTS");
            assert(fcheck != NULL && fcheck->status == PREFLIGHT_MISSING);
            assert(strstr(fcheck->message, "Korean: project font.") != NULL);
        }

        free(fallback_app);
        assert(test_remove_tree(fonts_dir));
        assert(test_remove_tree(project_dir));
        assert(test_remove_tree(source_dir));
    }

    /* 14s (#1300): the Fonts check names, by PSP file name, every system font that is not the
       user's own import, and says what a game asking for one gets: a stand-in that looks
       different, or no font. Only ltn0-2 and ltn4-6 and the Japanese font are imported first. */
    printf("[PLAYER_STATE_TEST] Subtest 14s: the Fonts check names the PSP fonts not imported\n");
    fflush(stdout);
    {
        static const int first_latin[] = { 0, 1, 2, 4, 5, 6 };
        static const int rest_latin[] = { 3, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
        char base[512];
        char per_user[768];
        char runtime[768];
        char first[800];
        char rest[800];
        char korean[800];
        char path[1024];
        const char sep = nk_platform_path_separator();
        NkIsoExecutableReport exec_rep;
        const PlayerPreflightCheck *fcheck;

        assert(nk_platform_get_path(NK_PATH_CACHE, base, sizeof(base)));
        snprintf(per_user, sizeof(per_user), "%s%cfont_missing_user", base, sep);
        snprintf(runtime, sizeof(runtime), "%s%cfont_missing_runtime", base, sep);
        snprintf(first, sizeof(first), "%s%cfont_missing_first", base, sep);
        snprintf(rest, sizeof(rest), "%s%cfont_missing_rest", base, sep);
        snprintf(korean, sizeof(korean), "%s%cfont_missing_korean", base, sep);
        assert(test_remove_tree(per_user));
        assert(test_remove_tree(runtime));
        assert(test_remove_tree(first));
        assert(test_remove_tree(rest));
        assert(test_remove_tree(korean));
        assert(nk_platform_mkdir_p(per_user));
        assert(nk_platform_mkdir_p(runtime));
        assert(nk_platform_mkdir_p(first));
        assert(nk_platform_mkdir_p(rest));
        assert(nk_platform_mkdir_p(korean));
        snprintf(path, sizeof(path), "%s%cdump_japanese.pgf", first, sep);
        write_binary_file(path, kGoldenJapanesePgf, sizeof(kGoldenJapanesePgf));
        for (size_t i = 0; i < sizeof(first_latin) / sizeof(first_latin[0]); i++) {
            snprintf(path, sizeof(path), "%s%cltn%d.pgf", first, sep, first_latin[i]);
            write_binary_file(path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));
        }
        for (size_t i = 0; i < sizeof(rest_latin) / sizeof(rest_latin[0]); i++) {
            snprintf(path, sizeof(path), "%s%cltn%d.pgf", rest, sep, rest_latin[i]);
            write_binary_file(path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));
        }
        snprintf(path, sizeof(path), "%s%cdump_korean.pgf", korean, sep);
        write_binary_file(path, kGoldenKoreanPgf, sizeof(kGoldenKoreanPgf));

        assert(nk_platform_set_app_data_dir_override(per_user));
        PlayerApp *missing_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(missing_app != NULL);
        nk_library_init(&missing_app->library);
        player_app_set_runtime_root(missing_app, runtime);
        snprintf(missing_app->inspecting_game.disc_id,
                 sizeof(missing_app->inspecting_game.disc_id), "TEST80001");
        snprintf(missing_app->inspecting_game.title_id,
                 sizeof(missing_app->inspecting_game.title_id), "synthetic-allegrex-v1");
        memset(&exec_rep, 0, sizeof(exec_rep));
        exec_rep.eboot.kind = NK_ISO_EXEC_PSP_ENCRYPTED;
        exec_rep.boot.kind = NK_ISO_EXEC_MIPS_ELF32;
        exec_rep.selected = NK_ISO_EXEC_SELECTION_BOOT;
        exec_rep.boot_fallback = true;
        snprintf(exec_rep.selected_path, sizeof(exec_rep.selected_path), "BOOT.BIN");

        /* 1. ltn3 and ltn7 are served by a same-size import; ltn8-15 and Korean have no font. */
        assert(player_app_fonts_import_folder(missing_app, first));
        player_app_build_compatibility_preflight(missing_app, true, true, &exec_rep);
        fcheck = find_preflight_check(&missing_app->wizard.preflight, "SYSTEM_FONTS");
        assert(fcheck != NULL && fcheck->status == PREFLIGHT_MISSING);
        assert(strcmp(player_preflight_plain_message(fcheck),
                      "These PSP system fonts were not found: 10 of the 16 Latin fonts and the "
                      "Korean font. Games that ask for them get a stand-in font that looks "
                      "different; for 8 of the 16 Latin fonts and the Korean font they get no "
                      "font at all. Import them from your own PSP in the setup wizard.") == 0);

        /* 2. Project fonts stand in for the rest: every font resolves, so the check is OK, and
           it still names the fonts a game gets a stand-in for. */
        snprintf(path, sizeof(path), "%s%cfont", runtime, sep);
        assert(nk_platform_mkdir_p(path));
        snprintf(path, sizeof(path), "%s%cfont%cnkltn.pgf", runtime, sep, sep);
        write_binary_file(path, kGoldenLatinPgf, sizeof(kGoldenLatinPgf));
        snprintf(path, sizeof(path), "%s%cfont%cnkkr.pgf", runtime, sep, sep);
        write_binary_file(path, kGoldenKoreanPgf, sizeof(kGoldenKoreanPgf));
        player_app_build_compatibility_preflight(missing_app, true, true, &exec_rep);
        fcheck = find_preflight_check(&missing_app->wizard.preflight, "SYSTEM_FONTS");
        assert(fcheck != NULL && fcheck->status == PREFLIGHT_OK);
        assert(strcmp(player_preflight_plain_message(fcheck),
                      "These PSP system fonts were not found: 10 of the 16 Latin fonts and the "
                      "Korean font. Games that ask for them get a stand-in font that looks "
                      "different. Import them from your own PSP in the setup wizard.") == 0);

        /* 3. Every Latin file imported: only the Korean font is named. */
        assert(player_app_fonts_import_folder(missing_app, rest));
        player_app_build_compatibility_preflight(missing_app, true, true, &exec_rep);
        fcheck = find_preflight_check(&missing_app->wizard.preflight, "SYSTEM_FONTS");
        assert(fcheck != NULL && fcheck->status == PREFLIGHT_OK);
        assert(strcmp(player_preflight_plain_message(fcheck),
                      "This PSP system font was not found: the Korean font. Games that ask for it "
                      "get a stand-in font that looks different. Import it from your own PSP in "
                      "the setup wizard.") == 0);

        /* 4. Every font is the user's own: nothing is named. */
        assert(player_app_fonts_import_folder(missing_app, korean));
        player_app_build_compatibility_preflight(missing_app, true, true, &exec_rep);
        fcheck = find_preflight_check(&missing_app->wizard.preflight, "SYSTEM_FONTS");
        assert(fcheck != NULL && fcheck->status == PREFLIGHT_OK);
        assert(strcmp(player_preflight_plain_message(fcheck), "PSP system fonts found.") == 0);

        free(missing_app);
        assert(nk_platform_set_app_data_dir_override(NULL));
        assert(test_remove_tree(per_user));
        assert(test_remove_tree(runtime));
        assert(test_remove_tree(first));
        assert(test_remove_tree(rest));
        assert(test_remove_tree(korean));
    }

    /* 15. Player settings persistence.
     *
     * Persist render resolution, frame cadence, VSync, fullscreen, reduce
     * motion, and master volume to a versioned JSON file. Verify defaults,
     * round trip with non-defaults, corrupt file fallback with notice, and
     * unknown version fallback with notice. */
    {
        printf("[PLAYER_STATE_TEST] Subtest 15: player settings persistence\n");
        fflush(stdout);

        char cache_dir[512];
        char test_settings_path[700];
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
        snprintf(test_settings_path, sizeof(test_settings_path), "%s%csettings_test.json",
                 cache_dir, nk_platform_path_separator());
        remove(test_settings_path);

        /* Default settings */
        PlayerApp *s_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(s_app != NULL);
        player_app_settings_init_default(&s_app->settings);
        assert(s_app->settings.resolution_scale == 1);
        assert(s_app->settings.present_scaling == NK_PRESENT_SCALING_SHARP);
        assert(s_app->settings.fps_cap == -1);
        assert(s_app->settings.vsync == true);
        assert(s_app->settings.fullscreen == false);
        assert(s_app->settings.launcher_fullscreen == false);
        assert(s_app->settings.reduce_motion == false);
        assert(s_app->settings.master_volume == 80);
        assert(s_app->settings.launcher_window_width == 1280);
        assert(s_app->settings.launcher_window_height == 720);
        assert(s_app->settings.launcher_window_position_valid == false);

        /* Mutate all settings and round-trip */
        s_app->settings.resolution_scale = 2;
        s_app->settings.present_scaling = NK_PRESENT_SCALING_BILINEAR;
        s_app->settings.fps_cap = 0;
        s_app->settings.vsync = false;
        s_app->settings.fullscreen = true;
        s_app->settings.launcher_fullscreen = true;
        s_app->settings.reduce_motion = true;
        /* #1306: shared system libraries are on by default and persist when turned off. */
        assert(s_app->settings.share_system_libraries == true);
        player_app_toggle_share_system_libraries(s_app);
        s_app->settings.master_volume = 55;
        s_app->settings.launcher_window_maximized = true;
        s_app->settings.launcher_window_position_valid = true;
        s_app->settings.launcher_window_x = -1600;
        s_app->settings.launcher_window_y = 80;
        s_app->settings.launcher_window_width = 1100;
        s_app->settings.launcher_window_height = 640;

        assert(player_app_save_settings(s_app, test_settings_path) == NK_OK);

        PlayerApp *s_app2 = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(s_app2 != NULL);
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.resolution_scale == 2);
        assert(s_app2->settings.present_scaling == NK_PRESENT_SCALING_BILINEAR);
        assert(s_app2->settings.fps_cap == 0);
        assert(s_app2->settings.vsync == false);
        assert(s_app2->settings.fullscreen == true);
        assert(s_app2->settings.launcher_fullscreen == true);
        assert(s_app2->settings.reduce_motion == true);
        assert(s_app2->settings.share_system_libraries == false);
        assert(s_app2->settings.master_volume == 55);
        assert(s_app2->settings.launcher_window_maximized == true);
        assert(s_app2->settings.launcher_window_position_valid == true);
        assert(s_app2->settings.launcher_window_x == -1600);
        assert(s_app2->settings.launcher_window_y == 80);
        assert(s_app2->settings.launcher_window_width == 1100);
        assert(s_app2->settings.launcher_window_height == 640);
        assert(s_app2->settings_notice[0] == '\0');

        /* The player's --experimental-60hz is a session-only override: it opts the
           launches of this session in, and a later save never records it. */
        {
            PlayerApp *x_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
            assert(x_app != NULL);
            player_app_settings_init_default(&x_app->settings);
            size_t path_len = strlen(test_settings_path);
            assert(path_len < sizeof(x_app->settings_path));
            memcpy(x_app->settings_path, test_settings_path, path_len + 1u);
            assert(!player_app_experimental_60hz_active(x_app));
            player_app_set_experimental_60hz_session(x_app, true);
            assert(player_app_experimental_60hz_active(x_app));
            assert(x_app->settings.experimental_60hz == false);
            player_app_toggle_vsync(x_app); /* a persisted change writes the file */
            PlayerApp *x_reload = (PlayerApp *)calloc(1, sizeof(PlayerApp));
            assert(x_reload != NULL);
            assert(player_app_load_settings(x_reload, test_settings_path) == NK_OK);
            assert(x_reload->settings.vsync == x_app->settings.vsync);
            assert(x_reload->settings.experimental_60hz == false);
            assert(!player_app_experimental_60hz_active(x_reload));
            /* The saved setting alone also turns it on. */
            player_app_set_experimental_60hz(x_reload, true);
            assert(player_app_experimental_60hz_active(x_reload));
            free(x_reload);
            free(x_app);
        }

        /* Launcher and game fullscreen are independent persisted settings. */
        player_app_toggle_launcher_fullscreen(s_app2);
        assert(s_app2->settings.launcher_fullscreen == false);
        assert(s_app2->settings.fullscreen == true);

        /* Closing a running child requires explicit confirmation; closing an
           idle player remains immediate. */
        assert(player_app_close_decision(s_app2, false) == PLAYER_CLOSE_QUIT);
        s_app2->is_game_running = true;
        assert(player_app_close_decision(s_app2, false) ==
               PLAYER_CLOSE_CONFIRM_REQUIRED);
        assert(player_app_close_decision(s_app2, true) == PLAYER_CLOSE_QUIT);
        assert(!player_app_take_close_confirmation_fallback(s_app2));
        player_app_note_close_confirmation_failure(s_app2);
        assert(player_app_take_close_confirmation_fallback(s_app2));
        assert(!player_app_take_close_confirmation_fallback(s_app2));

        /* The quit confirmation's buttons fit SDL3's Windows message box,
           which showed "Close game and quit" as "ose game and qu". */
        assert(PLAYER_DIALOG_BUTTON_LABEL_MAX <= 12);
        assert(strcmp(PLAYER_QUIT_DIALOG_TITLE, "Quit Nakagawa?") == 0);
        assert(strcmp(PLAYER_QUIT_DIALOG_QUIT_LABEL, "Quit") == 0);
        assert(strcmp(PLAYER_QUIT_DIALOG_CANCEL_LABEL, "Cancel") == 0);
        assert(strlen(PLAYER_QUIT_DIALOG_QUIT_LABEL) <= PLAYER_DIALOG_BUTTON_LABEL_MAX);
        assert(strlen(PLAYER_QUIT_DIALOG_CANCEL_LABEL) <= PLAYER_DIALOG_BUTTON_LABEL_MAX);
        /* Its message names the running game when the launch knows it. */
        {
            static const char unnamed[] =
                "A game is still running.\nQuitting closes it, and anything since "
                "the game's last save is lost.";
            static const char named[] =
                "Hot Shots Tennis is still running.\nQuitting closes it, and anything "
                "since the game's last save is lost.";
            char quit_message[NK_MAX_TITLE_LEN + 128];
            s_app2->launch_session.game_title[0] = '\0';
            assert(player_app_quit_confirmation_message(s_app2, quit_message,
                                                        sizeof(quit_message)));
            assert(strcmp(quit_message, unnamed) == 0);
            snprintf(s_app2->launch_session.game_title,
                     sizeof(s_app2->launch_session.game_title), "Hot Shots Tennis");
            assert(player_app_quit_confirmation_message(s_app2, quit_message,
                                                        sizeof(quit_message)));
            assert(strcmp(quit_message, named) == 0);
            /* A title's line breaks and tabs fold to one space, so the only
               line break is the message's own. */
            snprintf(s_app2->launch_session.game_title,
                     sizeof(s_app2->launch_session.game_title),
                     "  Hot Shots\r\n\tTennis \n");
            assert(player_app_quit_confirmation_message(s_app2, quit_message,
                                                        sizeof(quit_message)));
            assert(strcmp(quit_message, named) == 0);
            /* A title that is only blanks is no name. */
            snprintf(s_app2->launch_session.game_title,
                     sizeof(s_app2->launch_session.game_title), " \r\n\t ");
            assert(player_app_quit_confirmation_message(s_app2, quit_message,
                                                        sizeof(quit_message)));
            assert(strcmp(quit_message, unnamed) == 0);
            /* The longest title still fits the caller's buffer whole. */
            memset(s_app2->launch_session.game_title, 'T',
                   sizeof(s_app2->launch_session.game_title) - 1);
            s_app2->launch_session.game_title[
                sizeof(s_app2->launch_session.game_title) - 1] = '\0';
            assert(player_app_quit_confirmation_message(s_app2, quit_message,
                                                        sizeof(quit_message)));
            assert(strlen(quit_message) ==
                   (sizeof(s_app2->launch_session.game_title) - 1) +
                       (sizeof(unnamed) - 1) - strlen("A game"));
            assert(player_app_quit_confirmation_message(NULL, quit_message,
                                                        sizeof(quit_message)));
            assert(strcmp(quit_message, unnamed) == 0);
            assert(!player_app_quit_confirmation_message(s_app2, NULL, 16));
            assert(!player_app_quit_confirmation_message(s_app2, quit_message, 0));
            s_app2->launch_session.game_title[0] = '\0';
        }

        /* One host close may enter SDL as both QUIT and WINDOW_CLOSE_REQUESTED.
           Canceling the first confirmation leaves the child running, but a
           second close event in that drained batch must not ask again. */
        bool close_request_handled_in_batch = false;
        int confirmation_count = 0;
        for (int event = 0; event < 2; ++event) {
            if (player_app_close_request_batch_claim(
                    &close_request_handled_in_batch) &&
                player_app_close_decision(s_app2, false) ==
                    PLAYER_CLOSE_CONFIRM_REQUIRED) {
                ++confirmation_count;
                /* Cancel keeps s_app2->is_game_running set. */
            }
        }
        assert(confirmation_count == 1);
        assert(s_app2->is_game_running);

        /* A later close starts a new batch and prompts once again. */
        close_request_handled_in_batch = false;
        for (int event = 0; event < 2; ++event) {
            if (player_app_close_request_batch_claim(
                    &close_request_handled_in_batch) &&
                player_app_close_decision(s_app2, false) ==
                    PLAYER_CLOSE_CONFIRM_REQUIRED) {
                ++confirmation_count;
            }
        }
        assert(confirmation_count == 2);
        s_app2->is_game_running = false;

        /* The two-column layout is used from the first card width that can
           hold it, with the right column pushed clear of the wide launcher
           control. A window that wide must never fall into the single-column
           flow, which cannot fit a 720 px client. */
        assert(!player_settings_uses_two_columns(1043, 720));
        assert(player_settings_uses_two_columns(1044, 720));
        assert(player_settings_uses_two_columns(1187, 720));
        assert(player_settings_uses_two_columns(1188, 720));
        for (float card = 980.0f; card <= 1216.0f; card += 1.0f) {
            float offset = player_settings_second_column_offset(card);
            /* launcher control and hint end 562 px into the card */
            assert(offset >= 562.0f + 24.0f);
            /* the right column keeps usable width for the stepper */
            assert(card - 32.0f - offset >= 300.0f);
        }
        assert(player_settings_uses_two_columns(1280, 620));
        assert(!player_settings_uses_two_columns(1280, 619));

        /* The launcher UI is drawn at the window's real pixel size (#1129)
           with the scale the old 1280x720 letterbox used: the design never
           lays out smaller, grows along the longer axis instead of leaving
           bars, and the canvas covers the output with whole layout units. */
        {
            static const struct {
                int pixel_w, pixel_h;
                int width, height;
                float scale;
            } fits[] = {
                { 1280, 720, 1280, 720, 1.0f },        /* 1x */
                { 2560, 1440, 1280, 720, 2.0f },       /* 2x high-DPI window */
                { 1920, 1080, 1280, 720, 1.5f },       /* 1080p fullscreen */
                { 1500, 900, 1280, 768, 1.171875f },   /* fractional window */
                { 3840, 2160, 1280, 720, 3.0f },       /* 4K fullscreen */
                { 1920, 1200, 1280, 800, 1.5f },       /* 16:10 grows down */
                { 2560, 1080, 1706, 720, 1.5f },       /* ultrawide grows across */
                { 960, 540, 1280, 720, 0.75f },        /* minimum window */
                { 1440, 810, 1280, 720, 1.125f },      /* minimum at 150% */
                { 200, 100, 800, 400, 0.25f },         /* scale floor */
                { 30720, 17280, 3840, 2160, 8.0f },    /* scale cap */
            };
            for (size_t i = 0; i < sizeof(fits) / sizeof(fits[0]); ++i) {
                PlayerUiCanvas canvas;
                assert(player_ui_canvas_fit(fits[i].pixel_w, fits[i].pixel_h,
                                            &canvas));
                assert(canvas.width == fits[i].width);
                assert(canvas.height == fits[i].height);
                assert(canvas.scale == fits[i].scale);
                assert(canvas.width * canvas.scale <= fits[i].pixel_w + 0.01f);
                assert(canvas.height * canvas.scale <= fits[i].pixel_h + 0.01f);
                assert((canvas.width + 1) * canvas.scale > fits[i].pixel_w);
                assert((canvas.height + 1) * canvas.scale > fits[i].pixel_h);
            }
            PlayerUiCanvas canvas;
            /* 800 px at 800/720 is 720 units, not 719 from rounding error */
            assert(player_ui_canvas_fit(1500, 800, &canvas));
            assert(canvas.height == 720);
            assert(canvas.width == 1350);
            assert(!player_ui_canvas_fit(0, 720, &canvas));
            assert(!player_ui_canvas_fit(1280, -1, &canvas));
            assert(!player_ui_canvas_fit(1280, 720, NULL));
        }

        /* The focus handoff is attempted once per launch, and only on proven
           evidence. The launcher stays open behind the game (#1298) and hands
           its window the foreground once: retrying every frame would take the
           foreground back from a user who moved to another window. */
        assert(player_app_should_attempt_window_handoff(true, true, false, true));
        assert(player_app_should_attempt_window_handoff(true, true, false, false) == false);
        assert(player_app_should_attempt_window_handoff(true, false, false, true) == false);
        assert(player_app_should_attempt_window_handoff(false, true, false, true) == false);
        assert(player_app_should_attempt_window_handoff(true, true, true, true) == false);
        assert(player_app_should_attempt_window_handoff(true, true, true, false) == false);
        /* The same launch is not retried after a failed attempt. */
        assert(player_app_should_attempt_window_handoff(true, true, true, true) == false);
        /* A later launch gets a fresh attempt once the loop clears the latch. */
        assert(player_app_should_attempt_window_handoff(true, true, false, true) == true);

        /* The child requests foreground once only after the launcher handoff
           marker exists and a visible, non-headless window is available. */
        {
            bool request_issued = false;
            int request_count = 0;

            assert(!sr_gui_request_launcher_foreground_once(
                NULL, true, false, &request_issued, &request_count,
                count_gui_foreground_request));
            assert(!sr_gui_request_launcher_foreground_once(
                "boot-events", false, false, &request_issued, &request_count,
                count_gui_foreground_request));
            assert(!sr_gui_request_launcher_foreground_once(
                "boot-events", true, true, &request_issued, &request_count,
                count_gui_foreground_request));
            assert(!request_issued && request_count == 0);
            assert(sr_gui_request_launcher_foreground_once(
                "boot-events", true, false, &request_issued, &request_count,
                count_gui_foreground_request));
            assert(request_issued && request_count == 1);
            assert(!sr_gui_request_launcher_foreground_once(
                "boot-events", true, false, &request_issued, &request_count,
                count_gui_foreground_request));
            assert(request_count == 1);
        }

        assert(player_app_boot_event_is_window_ready(
            "BOOT_EVENT phase=window_ready backend=sdl"));
        assert(player_app_boot_event_is_window_ready(
            "BOOT_EVENT phase=first_frame source=cpu"));
        assert(!player_app_boot_event_is_window_ready(
            "BOOT_EVENT phase=guest_start mode=scheduler"));

        /* A 1280x720 request fits the 1024x600 usable area after the window
           frame is reserved; a saved position on a missing monitor centers
           safely on the current display. */
        {
            PlayerWindowRect fitted;
            PlayerWindowRect usable = { 0, 0, 1024, 600 };
            PlayerWindowRect requested = { 0, 0, 1280, 720 };
            PlayerWindowFrame frame = { 32, 8, 8, 8 };
            assert(player_window_fit_to_display(requested, usable, frame,
                                                false, &fitted));
            assert(fitted.width > 0 && fitted.height > 0);
            assert(fitted.width + frame.left + frame.right <= usable.width);
            assert(fitted.height + frame.top + frame.bottom <= usable.height);

            usable.x = -1280;
            usable.y = 40;
            usable.width = 1280;
            usable.height = 680;
            requested.x = 5000;
            requested.y = 5000;
            requested.width = 1280;
            requested.height = 720;
            assert(player_window_fit_to_display(requested, usable, frame,
                                                true, &fitted));
            /* The fitted rect is a CLIENT rect, so it is bounded by the
               display's client area, not by the raw usable bounds. */
            assert(fitted.x >= usable.x + frame.left);
            assert(fitted.y >= usable.y + frame.top);
            assert((int64_t)fitted.x + fitted.width <=
                   (int64_t)usable.x + usable.width - frame.right);
            assert((int64_t)fitted.y + fitted.height <=
                   (int64_t)usable.y + usable.height - frame.bottom);

            /* On a 2x content-scale desktop, a 2x native-coordinate request
               still fits by clamping against the same SDL screen units. */
            usable.x = 0;
            usable.y = 0;
            usable.width = 3840;
            usable.height = 2160;
            frame.top = 64;
            frame.left = 16;
            frame.bottom = 16;
            frame.right = 16;
            requested.width = 2560;
            requested.height = 1440;
            assert(player_window_fit_to_display(requested, usable, frame,
                                                false, &fitted));
            assert(fitted.width == requested.width);
            assert(fitted.height == requested.height);

            /* SDL_SetWindowSize and SDL_SetWindowPosition address the CLIENT
               rectangle, and the persisted launcher geometry is a client
               rectangle too, so the fitted rectangle must stay inside the
               display's client area. Clamping against the raw usable bounds
               instead put the title bar and the top of the window off-screen:
               on a real 3840x2160 display a saved window at (0,0) was measured
               at a Win32 outer rect of (-11,-45). */
            {
                PlayerWindowRect live_usable = { 0, 0, 3840, 2088 };
                PlayerWindowFrame live_frame = { 45, 11, 11, 11 };
                PlayerWindowRect live_requested = { 0, 0, 1280, 720 };
                int64_t client_x = (int64_t)live_usable.x + live_frame.left;
                int64_t client_y = (int64_t)live_usable.y + live_frame.top;
                int64_t client_w = (int64_t)live_usable.width -
                                   live_frame.left - live_frame.right;
                int64_t client_h = (int64_t)live_usable.height -
                                   live_frame.top - live_frame.bottom;
                assert(player_window_fit_to_display(live_requested, live_usable,
                                                    live_frame, true, &fitted));
                assert(fitted.x >= client_x);
                assert(fitted.y >= client_y);
                assert((int64_t)fitted.x + fitted.width <= client_x + client_w);
                assert((int64_t)fitted.y + fitted.height <= client_y + client_h);
                assert(fitted.width == live_requested.width);
                assert(fitted.height == live_requested.height);

                /* A saved client rect flush against the right/bottom edge is
                   pulled back by the frame instead of hanging off the display. */
                live_requested.x = 3840 - live_requested.width;
                live_requested.y = 2088 - live_requested.height;
                assert(player_window_fit_to_display(live_requested, live_usable,
                                                    live_frame, true, &fitted));
                assert((int64_t)fitted.x + fitted.width <= client_x + client_w);
                assert((int64_t)fitted.y + fitted.height <= client_y + client_h);

                /* A window larger than the whole display still resolves to a
                   client rectangle that fits, and never a negative maximum. */
                live_requested.x = 0;
                live_requested.y = 0;
                live_requested.width = 5000;
                live_requested.height = 3000;
                assert(player_window_fit_to_display(live_requested, live_usable,
                                                    live_frame, true, &fitted));
                assert(fitted.x >= client_x);
                assert(fitted.y >= client_y);
                assert(fitted.width <= client_w);
                assert(fitted.height <= client_h);
            }
        }

        /* Schema 1's 30/60 values capped host presentations, including the old
         * 60 default. Migrate them to PSP scanout; preserve an explicit 0. */
        write_text_file(test_settings_path,
                        "{\"schema_version\": 1, \"fps_cap\": 60}");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.fps_cap == -1);
        write_text_file(test_settings_path,
                        "{\"schema_version\": 1, \"fps_cap\": 30}");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.fps_cap == -1);
        write_text_file(test_settings_path,
                        "{\"schema_version\": 1, \"fps_cap\": 0}");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.fps_cap == 0);

        /* Corrupt JSON file resets to defaults and produces notice */
        write_text_file(test_settings_path, "{ invalid_json: [1, 2, ");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_ERROR_GENERIC);
        assert(s_app2->settings.resolution_scale == 1);
        assert(s_app2->settings.fps_cap == -1);
        assert(s_app2->settings.vsync == true);
        assert(s_app2->settings.fullscreen == false);
        assert(s_app2->settings.reduce_motion == false);
        assert(s_app2->settings.master_volume == 80);
        assert(strstr(s_app2->settings_notice, "corrupt") != NULL);

        /* Unknown / unsupported schema version resets to defaults and produces notice */
        write_text_file(test_settings_path, "{\"schema_version\": 999, \"resolution_scale\": 8}");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_ERROR_GENERIC);
        assert(s_app2->settings.resolution_scale == 1);
        assert(s_app2->settings.fps_cap == -1);
        assert(s_app2->settings.vsync == true);
        assert(s_app2->settings.fullscreen == false);
        assert(s_app2->settings.reduce_motion == false);
        assert(s_app2->settings.master_volume == 80);
        assert(strstr(s_app2->settings_notice, "Unsupported settings schema version") != NULL);

        /* A legacy persisted 8x preset is refused at load: the GPU rasterizer
           caps at 4x, so it falls back to the default instead of pretending. */
        write_text_file(test_settings_path,
                        "{\"schema_version\": 1, \"resolution_scale\": 8}");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.resolution_scale == 1);
        assert(s_app2->settings_notice[0] == '\0');

        /* Scaling is read by name; a file without it, a number or an unknown name keeps sharp. */
        assert(s_app2->settings.present_scaling == NK_PRESENT_SCALING_SHARP);
        write_text_file(test_settings_path,
                        "{\"schema_version\": 1, \"present_scaling\": \"integer\"}");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.present_scaling == NK_PRESENT_SCALING_INTEGER);
        write_text_file(test_settings_path,
                        "{\"schema_version\": 1, \"present_scaling\": 1}");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.present_scaling == NK_PRESENT_SCALING_SHARP);
        write_text_file(test_settings_path,
                        "{\"schema_version\": 1, \"present_scaling\": \"blurry\"}");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.present_scaling == NK_PRESENT_SCALING_SHARP);

        /* network_mode is reserved: offline is the only accepted value, it is
           written on save, and any stored value (a later "online", or junk) loads
           as offline rather than as something more permissive. */
        assert(s_app2->settings.network_mode == NK_NETWORK_MODE_OFFLINE);
        assert(player_app_save_settings(s_app2, test_settings_path) == NK_OK);
        {
            FILE *nf = fopen(test_settings_path, "rb");
            char saved[4096] = {0};
            assert(nf != NULL);
            size_t n = fread(saved, 1, sizeof(saved) - 1, nf);
            fclose(nf);
            saved[n] = '\0';
            assert(strstr(saved, "\"network_mode\": \"offline\"") != NULL);
        }
        write_text_file(test_settings_path,
                        "{\"schema_version\": 2, \"network_mode\": \"online\"}");
        s_app2->settings.network_mode = (NkNetworkMode)7;
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.network_mode == NK_NETWORK_MODE_OFFLINE);
        write_text_file(test_settings_path,
                        "{\"schema_version\": 2, \"network_mode\": 3}");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.network_mode == NK_NETWORK_MODE_OFFLINE);

        /* Online and developer options: both off by default, the online parts on
           once online mode is; all five round-trip, a non-boolean keeps the
           default, and none of them moves network_mode off offline. */
        player_app_init(s_app2);
        assert(!s_app2->settings.online_mode && !s_app2->settings.developer_mode);
        assert(s_app2->settings.online_updates && s_app2->settings.online_news &&
               s_app2->settings.online_reports);
        s_app2->settings.online_mode = true;
        s_app2->settings.online_news = false;
        s_app2->settings.developer_mode = true;
        assert(player_app_save_settings(s_app2, test_settings_path) == NK_OK);
        player_app_init(s_app2);
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.online_mode && s_app2->settings.developer_mode);
        assert(s_app2->settings.online_updates && !s_app2->settings.online_news &&
               s_app2->settings.online_reports);
        assert(s_app2->settings.network_mode == NK_NETWORK_MODE_OFFLINE);
        write_text_file(test_settings_path,
                        "{\"schema_version\": 2, \"network_mode\": \"online\", "
                        "\"online_mode\": \"yes\", \"developer_mode\": 1}");
        player_app_init(s_app2);
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(!s_app2->settings.online_mode && !s_app2->settings.developer_mode);
        assert(s_app2->settings.network_mode == NK_NETWORK_MODE_OFFLINE);

        remove(test_settings_path);
        free(s_app);
        free(s_app2);
    }

    /* 15b. Developer log: redaction, the ring, the panel and the wizard step. */
    {
        char out[256];
        PlayerApp *dev = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        printf("[PLAYER_STATE_TEST] Subtest 15b: developer log and options step\n");
        fflush(stdout);
        assert(dev != NULL);
#if defined(_WIN32) || defined(_WIN64)
        assert(player_dev_log_redact("open D:\\Profiles\\Synth\\AppData\\x.log",
                                     "D:\\Profiles\\Synth", "Synth", out, sizeof(out)));
        assert(strcmp(out, "open %USERPROFILE%\\AppData\\x.log") == 0);
        /* The other slash style and another case match too. */
        assert(player_dev_log_redact("d:/profiles/synth/saves", "D:\\Profiles\\Synth\\", "Synth",
                                     out, sizeof(out)));
        assert(strcmp(out, "%USERPROFILE%/saves") == 0);
#else
        assert(player_dev_log_redact("open /srv/profiles/synth/.local/x.log", "/srv/profiles/synth/", "synth",
                                     out, sizeof(out)));
        assert(strcmp(out, "open %USERPROFILE%/.local/x.log") == 0);
#endif
        /* The user name is replaced as a whole word only. */
        assert(player_dev_log_redact("by synth, not synthetic", NULL, "synth", out, sizeof(out)));
        assert(strcmp(out, "by <user>, not synthetic") == 0);
        /* Short names are left alone rather than blanking common letters. */
        assert(player_dev_log_redact("ab cd", NULL, "ab", out, sizeof(out)));
        assert(strcmp(out, "ab cd") == 0);
        assert(!player_dev_log_redact("abcdef", NULL, NULL, out, 4u));
        assert(strcmp(out, "abc") == 0);

        player_app_init(dev);
        for (int i = 0; i < PLAYER_DEV_LOG_LINES + 5; ++i) {
            player_dev_log_add(dev, "test", "line %d\nsplit", i);
        }
        assert(player_dev_log_count(dev) == PLAYER_DEV_LOG_LINES);
        assert(strcmp(player_dev_log_line(dev, 0), "[test] line 5 split") == 0);
        snprintf(out, sizeof(out), "[test] line %d split", PLAYER_DEV_LOG_LINES + 4);
        assert(strcmp(player_dev_log_line(dev, PLAYER_DEV_LOG_LINES - 1u), out) == 0);
        player_app_set_error(dev, "SYNTHETIC_ERROR", "Synthetic", "m", "Back", VIEW_LIBRARY);
        assert(strstr(player_dev_log_line(dev, PLAYER_DEV_LOG_LINES - 1u),
                      "SYNTHETIC_ERROR: Synthetic") != NULL);

        /* The panel opens only with developer mode, and closes to where it was. */
        dev->active_view = VIEW_SETTINGS;
        assert(!player_app_open_developer_log(dev));
        assert(dev->active_view == VIEW_SETTINGS);
        dev->settings.developer_mode = true;
        assert(player_app_open_developer_log(dev));
        assert(dev->active_view == VIEW_DEVELOPER_LOG);
        assert(player_app_focus_count(dev) == 3);
        player_app_close_developer_log(dev);
        assert(dev->active_view == VIEW_SETTINGS);
        dev->active_view = VIEW_ONLINE_SETTINGS;
        assert(player_app_focus_count(dev) == 4); /* online, developer, open log, back */
        dev->settings.online_mode = true;
        assert(player_app_focus_count(dev) == 7);

        /* The wizard step: Welcome -> Options -> Select game, and skipping
           turns both off. Every first run passes through it: it needs no game. */
        dev->active_view = VIEW_SETUP_WIZARD;
        dev->wizard.step = WIZARD_STEP_WELCOME;
        player_app_wizard_next(dev);
        assert(dev->wizard.step == WIZARD_STEP_ONLINE_OPTIONS);
        player_app_wizard_skip_options(dev);
        assert(dev->wizard.step == WIZARD_STEP_SELECT_GAME);
        assert(!dev->settings.online_mode && !dev->settings.developer_mode);
        player_app_wizard_back(dev);
        assert(dev->wizard.step == WIZARD_STEP_ONLINE_OPTIONS);
        player_app_set_online_mode(dev, true);
        player_app_wizard_next(dev);
        assert(dev->wizard.step == WIZARD_STEP_SELECT_GAME);
        assert(dev->settings.online_mode);
        assert(dev->settings.network_mode == NK_NETWORK_MODE_OFFLINE);
        free(dev);
    }

    /* 16. Icon and PNG image validation / fallback logic.
     *
     * Disc icons (ICON0.PNG and PIC1.PNG) are read through the ISO reader and
     * decoded with bounds checks (<= 1 MiB, <= 2048x2048). Missing, corrupt,
     * or oversized images report appropriate error status for badge fallback. */
    {
        printf("[PLAYER_STATE_TEST] Subtest 16: icon and image fallback logic\n");
        fflush(stdout);

        uint32_t w = 0;
        uint32_t h = 0;

        /* Missing or null buffers */
        assert(nk_iso_validate_png_header(NULL, 0, &w, &h) == NK_ICON_ERR_CORRUPT);
        uint8_t short_buf[16] = { 0 };
        assert(nk_iso_validate_png_header(short_buf, sizeof(short_buf), &w, &h) == NK_ICON_ERR_CORRUPT);

        /* Corrupt signature */
        uint8_t bad_sig[33] = "NOT_A_PNG_FILE_HEADER_LONGER_BUF";
        assert(nk_iso_validate_png_header(bad_sig, sizeof(bad_sig), &w, &h) == NK_ICON_ERR_CORRUPT);

        /* Valid signature but bad chunk type (must be IHDR) */
        uint8_t bad_chunk[33] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
            0x00, 0x00, 0x00, 0x0d,
            'N', 'O', 'P', 'E',
            0x00, 0x00, 0x00, 0x90,
            0x00, 0x00, 0x00, 0x50,
            0x08, 0x06, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        assert(nk_iso_validate_png_header(bad_chunk, sizeof(bad_chunk), &w, &h) == NK_ICON_ERR_CORRUPT);

        /* Zero dimensions */
        uint8_t zero_dim[33] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
            0x00, 0x00, 0x00, 0x0d,
            'I', 'H', 'D', 'R',
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x50,
            0x08, 0x06, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        assert(nk_iso_validate_png_header(zero_dim, sizeof(zero_dim), &w, &h) == NK_ICON_ERR_CORRUPT);

        /* Oversized dimensions (> 2048) */
        uint8_t oversized_dim[33] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
            0x00, 0x00, 0x00, 0x0d,
            'I', 'H', 'D', 'R',
            0x00, 0x00, 0x09, 0x00, /* 2304 > 2048 */
            0x00, 0x00, 0x05, 0x00,
            0x08, 0x06, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        assert(nk_iso_validate_png_header(oversized_dim, sizeof(oversized_dim), &w, &h) == NK_ICON_ERR_OVERSIZED);

        /* Valid synthetic PNG header: 144x80 (standard PSP ICON0 dimension) */
        uint8_t valid_hdr[33] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
            0x00, 0x00, 0x00, 0x0d,
            'I', 'H', 'D', 'R',
            0x00, 0x00, 0x00, 0x90, /* 144 */
            0x00, 0x00, 0x00, 0x50, /* 80 */
            0x08, 0x06, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        assert(nk_iso_validate_png_header(valid_hdr, sizeof(valid_hdr), &w, &h) == NK_ICON_OK);
        assert(w == 144);
        assert(h == 80);

        /* Missing ISO / image entry */
        uint8_t *bytes = NULL;
        size_t size = 0;
        assert(nk_iso_read_image_entry("nonexistent_disc.iso", "PSP_GAME/ICON0.PNG",
                                       &bytes, &size, &w, &h) == NK_ICON_ERR_MISSING);
        assert(bytes == NULL);
        assert(size == 0);
    }

    /* 17. Package build session and error presentation. */
    printf("[PLAYER_STATE_TEST] Subtest 17: package builder state and error reporting\n");
    fflush(stdout);
    {
        PlayerApp *bapp = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(bapp != NULL);
        nk_library_init(&bapp->library);

        /* Start with invalid index */
        assert(!player_app_start_package_build(bapp, -1));
        assert(!player_app_start_package_build(bapp, 0));

        /* Set build error with stage, boundary text, and log path */
        player_app_set_build_error(bapp, "preflight",
                                   "ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted and built-in decryption stopped: KEY_REQUIRED.",
                                   "C:/logs/build_TEST80001.log");
        assert(bapp->active_view == VIEW_ERROR);
        /* #1286: a preflight refusal is not a failed build. The card leads with
           the reason, without its code. */
        assert(strcmp(bapp->last_error.error_code, "PACKAGE_BUILD_REFUSED") == 0);
        assert(strcmp(bapp->last_error.title, "Build Not Started") == 0);
        assert(strncmp(bapp->last_error.message, "EBOOT.BIN is encrypted and built-in", 35) == 0);
        assert(strstr(bapp->last_error.message, "ENCRYPTED_EXECUTABLE") == NULL);
        assert(strcmp(bapp->last_error.failed_stage, "preflight") == 0);
        assert(strstr(bapp->last_error.boundary_text, "ENCRYPTED_EXECUTABLE") != NULL);
        assert(strcmp(bapp->last_error.log_file_path, "C:/logs/build_TEST80001.log") == 0);

        /* Cancellation transitions session */
        bapp->active_view = VIEW_BUILDING_PACKAGE;
        bapp->build_session.is_building = true;
        player_app_cancel_package_build(bapp);
        assert(bapp->build_session.is_cancelled);
        assert(!bapp->build_session.is_building);

        /* A duplicate BUILD must refuse before session initialization erases
           the child handle and the identity of the build already in flight. */
        char guard_cache[NK_MAX_PATH];
        char guard_cwd[NK_MAX_PATH];
        char guard_root[NK_MAX_PATH * 2];
        char guard_probe[NK_MAX_PATH * 2];
        assert(nk_platform_get_path(NK_PATH_CACHE, guard_cache,
                                    sizeof(guard_cache)));
        assert(nk_ps_getcwd(guard_cwd, sizeof(guard_cwd)) != NULL);
        snprintf(guard_root, sizeof(guard_root), "%s%cno_install",
                 guard_cache, nk_platform_path_separator());
        snprintf(guard_probe, sizeof(guard_probe), "%s%ccli_not_found_probe",
                 guard_cache, nk_platform_path_separator());
        assert(nk_platform_mkdir_p(guard_probe));
        bool had_install_override = false;
        char *saved_install_override = capture_environment_value(
            "NK_INSTALL_ROOT", &had_install_override);
        nk_ps_set_env("NK_INSTALL_ROOT", NULL);
        assert(nk_ps_chdir(guard_probe) == 0);
        snprintf(bapp->install_root, sizeof(bapp->install_root), "%.*s",
                 (int)sizeof(bapp->install_root) - 1, guard_root);
        bapp->game_count = 1;
        snprintf(bapp->games[0].disc_id,
                 sizeof(bapp->games[0].disc_id), "TEST90031");
        snprintf(bapp->games[0].title_name,
                 sizeof(bapp->games[0].title_name), "In-flight build fixture");
        package_builder_init_session(&bapp->build_session,
                                     bapp->games[0].disc_id,
                                     bapp->games[0].title_name);
        bapp->build_session.is_building = true;
        uint64_t in_flight_generation = bapp->build_session.session_generation;
        assert(package_builder_start(&bapp->build_session, "python", "cli",
                                     "data", "logs") ==
               NK_ERROR_ALREADY_EXISTS);
        assert(bapp->build_session.is_building);
        assert(bapp->build_session.session_generation == in_flight_generation);
        assert(!player_app_start_package_build(bapp, 0));
        assert(strcmp(bapp->last_error.error_code,
                      "PACKAGE_BUILD_IN_PROGRESS") == 0);
        assert(strstr(bapp->last_error.message, "already running") != NULL);
        assert(bapp->last_error.return_view == VIEW_BUILDING_PACKAGE);
        assert(bapp->build_session.is_building);
        assert(bapp->build_session.session_generation == in_flight_generation);
        assert(nk_ps_chdir(guard_cwd) == 0);
        restore_environment_value("NK_INSTALL_ROOT", saved_install_override,
                                  had_install_override);
        free(saved_install_override);
        bapp->build_session.is_building = false;

        free(bapp);
    }

    /* 18. Developer-layout catalog title launchability without a package. */
    printf("[PLAYER_STATE_TEST] Subtest 18: developer-layout catalog title launchability\n");
    fflush(stdout);
    {
        PlayerApp *dev_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(dev_app != NULL);
        nk_library_init(&dev_app->library);

        char cache_dir[NK_MAX_PATH];
        char dev_root[576];
        char build_dir[640];
        char dev_exe[768];
        char dev_image[768];
        char dev_data_root[768];
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
        snprintf(dev_root, sizeof(dev_root), "%s%cdev_layout_root",
                 cache_dir, nk_platform_path_separator());
        snprintf(build_dir, sizeof(build_dir), "%s%cbuild%cdisplay-smoke-v1",
                 dev_root, nk_platform_path_separator(), nk_platform_path_separator());
        assert(nk_platform_mkdir_p(build_dir));

        snprintf(dev_exe, sizeof(dev_exe), "%s%cdisplay-smoke-v1.exe",
                 build_dir, nk_platform_path_separator());
        snprintf(dev_image, sizeof(dev_image), "%s%cdisplay-smoke-v1_image.bin",
                 build_dir, nk_platform_path_separator());
        snprintf(dev_data_root, sizeof(dev_data_root), "%s%cfixtures%cdisplay_smoke",
                 dev_root, nk_platform_path_separator(), nk_platform_path_separator());
        assert(nk_platform_mkdir_p(dev_data_root));

        write_text_file(dev_exe, "executable_stub");
        write_text_file(dev_image, "image_stub");

        player_app_set_runtime_root(dev_app, dev_root);
        player_app_populate_sample_games(dev_app);

        int disp_idx = player_app_find_game_by_disc_id(dev_app, "TEST00006");
        assert(disp_idx >= 0);
        /* Without a package in packages/TEST00006, the developer binary is discovered
           and marks the title prepared. */
        assert(dev_app->games[disp_idx].is_prepared == true);
        assert(dev_app->games[disp_idx].status == NK_STATUS_PREPARED);
        assert(player_app_game_has_runtime(dev_app, &dev_app->games[disp_idx]) == true);

        /* Session preparation succeeds with the developer layout executable and image */
        NkResult prep_res = nk_launch_prepare_session(
            &dev_app->launch_session, &dev_app->games[disp_idx], dev_root);
        assert(prep_res == NK_OK);
        assert(dev_app->launch_session.package_launch == false);
        assert(strcmp(dev_app->launch_session.executable_path, dev_exe) == 0);
        assert(strcmp(dev_app->launch_session.image_path, dev_image) == 0);

        /* Cleanup stub files and verify fallback to unprepared when binary is removed */
        assert(remove(dev_exe) == 0);
        assert(remove(dev_image) == 0);
        assert(test_rmdir(dev_data_root) == 0);

        nk_library_init(&dev_app->library);
        dev_app->game_count = 0;
        player_app_populate_sample_games(dev_app);
        assert(dev_app->games[disp_idx].is_prepared == false);
        assert(dev_app->games[disp_idx].status == NK_STATUS_IDENTIFIED);
        assert(player_app_game_has_runtime(dev_app, &dev_app->games[disp_idx]) == false);
        assert(player_app_launch_game(dev_app, disp_idx) == false);

        free(dev_app);
    }

    {
        printf("[PLAYER_STATE_TEST] Subtest 19: package validation cache hit and invalidation\n");
        fflush(stdout);

        char cache_root[512];
        char validation_root[640];
        char package_dir[800];
        char package_json[960];
        char completion_json[960];
        char report_json[960];
        char executable[960];
        char image[960];
        NkTitleEntrySnapshot cached_title_snapshot = {0};
        assert(nk_title_catalog_find_by_id("synthetic-allegrex-v1",
                                           &cached_title_snapshot));
        const NkTitleEntry *cached_title = &cached_title_snapshot.entry;
        assert(cached_title && cached_title->primary_disc_id);
        char cached_disc_id[MAX_DISC_ID_LEN];
        snprintf(cached_disc_id, sizeof(cached_disc_id), "%s",
                 cached_title->primary_disc_id);
        nk_title_catalog_snapshot_release(&cached_title_snapshot);
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_root, sizeof(cache_root)));
        /* A root this subtest owns, named for the probe rather than for a
           cache: it is not a runtime directory, and a name that reads like one
           invites the next reader to assume some other component owns whatever
           it finds underneath. Nothing else in the tree writes here, so a
           previous run's leftovers can only come from this test itself. */
        snprintf(validation_root, sizeof(validation_root), "%s%cpr670_validation_probe",
                 cache_root, nk_platform_path_separator());
        snprintf(package_dir, sizeof(package_dir), "%s%cpackages%c%s",
                 validation_root, nk_platform_path_separator(),
                 nk_platform_path_separator(), cached_disc_id);
        snprintf(package_json, sizeof(package_json), "%s%cpackage.json", package_dir,
                 nk_platform_path_separator());
        snprintf(completion_json, sizeof(completion_json),
                 "%s%ccompletion-manifest.json", package_dir,
                 nk_platform_path_separator());
        snprintf(report_json, sizeof(report_json), "%s%cbuild-report.json", package_dir,
                 nk_platform_path_separator());
        snprintf(executable, sizeof(executable), "%s%csynthetic-allegrex-v1.exe",
                 package_dir, nk_platform_path_separator());
        snprintf(image, sizeof(image), "%s%csynthetic-allegrex-v1_image.bin",
                 package_dir, nk_platform_path_separator());
        /* Start from a clean root: this subtest asserts cache-hit and
           cache-invalidation decisions, so it must not inherit a package tree
           from a previous run. */
        remove_runtime_package_fixture(validation_root, cached_disc_id,
                                       "synthetic-allegrex-v1");
        assert(nk_platform_mkdir_p(package_dir));
        write_runtime_package_fixture_with_report_size(
            validation_root, cached_disc_id, "synthetic-allegrex-v1", SR_CPUSTATE_ABI_VERSION,
            "synthetic-allegrex-v1.exe", FIXTURE_SHA256, NULL,
            (size_t)NK_MANIFEST_MAX_BYTES + 1u);
        assert(fixture_file_size(report_json) > NK_MANIFEST_MAX_BYTES);

        NkGameEntry game;
        memset(&game, 0, sizeof(game));
        snprintf(game.disc_id, sizeof(game.disc_id), "%s", cached_disc_id);
        snprintf(game.title_id, sizeof(game.title_id), "synthetic-allegrex-v1");
        snprintf(game.selected_executable, sizeof(game.selected_executable), "EBOOT.BIN");

        /* A catalog-discovered showcase sample may have a companion synthetic
         * ISO, but its package identity is bound to the project-built inputs.
         * Other samples and ordinary library entries still need live-media
         * identity when an ISO path is present. */
        char sample_root[640];
        char sample_iso[512];
        char sample_identity[1100];
        snprintf(sample_root, sizeof(sample_root), "%s%cpr670_sample_validation_probe",
                 cache_root, nk_platform_path_separator());
        remove_runtime_package_fixture(sample_root, cached_disc_id,
                                       "synthetic-allegrex-v1");
        write_runtime_package_fixture_with_synthetic_title_identity(
            sample_root, cached_disc_id, "synthetic-allegrex-v1",
            SR_CPUSTATE_ABI_VERSION, "synthetic-allegrex-v1.exe",
            FIXTURE_SHA256, NULL);
        int sample_iso_written = snprintf(sample_iso, sizeof(sample_iso),
                                          "%s%cshowcase.iso", sample_root,
                                          nk_platform_path_separator());
        assert(sample_iso_written > 0 && (size_t)sample_iso_written < sizeof(sample_iso));
        write_file(sample_iso);
        snprintf(sample_identity, sizeof(sample_identity),
                 "%s%ctitle-input-identities%c%s%ctitle-input-identity.json",
                 sample_root, nk_platform_path_separator(), nk_platform_path_separator(),
                 cached_disc_id, nk_platform_path_separator());
        assert(remove(sample_identity) == 0);

        NkGameEntry sample_game = game;
        sample_game.is_sample = true;
        sample_game.is_showcase_package = true;
        snprintf(sample_game.iso_path, sizeof(sample_game.iso_path), "%s", sample_iso);
        NkRuntimePackageInfo sample_info;
        char sample_reason[1024] = "";
        NkRuntimePackageStatus sample_status = nk_launch_validate_runtime_package(
            sample_root, &sample_game, &sample_info, sample_reason,
            sizeof(sample_reason));
        if (sample_status != NK_RUNTIME_PACKAGE_OK) {
            fprintf(stderr, "showcase package validation failed: status=%d reason=%s\n",
                    (int)sample_status, sample_reason);
        }
        assert(sample_status == NK_RUNTIME_PACKAGE_OK);
        char sample_cache_identity[65];
        assert(nk_launch_runtime_package_cache_identity(
            sample_root, &sample_game, sample_cache_identity));
        NkGameEntry library_game = sample_game;
        library_game.is_sample = false;
        library_game.is_showcase_package = false;
        NkRuntimePackageInfo library_info;
        assert(nk_launch_validate_runtime_package(
                   sample_root, &library_game, &library_info, sample_reason,
                   sizeof(sample_reason)) == NK_RUNTIME_PACKAGE_STALE);
        assert(strstr(sample_reason,
                      "Title input identity record is missing or unreadable") != NULL);
        char library_cache_identity[65];
        assert(nk_launch_runtime_package_cache_identity(
            sample_root, &library_game, library_cache_identity));
        assert(strcmp(sample_cache_identity, library_cache_identity) != 0);
        assert(remove(sample_iso) == 0);
        remove_runtime_package_fixture(sample_root, cached_disc_id,
                                       "synthetic-allegrex-v1");
        printf("[PLAYER_STATE_TEST] showcase package skips live source-media identity; library does not PASS\n");

        NkRuntimePackageInfo first_info;
        NkRuntimePackageInfo cached_info;
        char identity_before[65];
        char identity_after[65];
        char reason[1024];
        /* Launcher review B5: a player that is closing cancels a validation
           that is still hashing the package, so the window does not wait for
           it. The cancelled result says so and is never cached: the next
           validation hashes the package again and passes. */
        nk_title_manifest_set_cancel_check(test_cancel_always, NULL);
        assert(nk_launch_validate_runtime_package(
                   validation_root, &game, &first_info, reason, sizeof(reason)) !=
               NK_RUNTIME_PACKAGE_OK);
        assert(strcmp(reason, "Package validation was cancelled.") == 0);
        assert(!first_info.validation_cache_hit && first_info.package_root[0] == '\0');
        nk_title_manifest_set_cancel_check(NULL, NULL);
        assert(nk_launch_validate_runtime_package(
                   validation_root, &game, &first_info, reason, sizeof(reason)) ==
               NK_RUNTIME_PACKAGE_OK);
        assert(!first_info.validation_cache_hit);
        assert(nk_launch_validate_runtime_package(
                   validation_root, &game, &cached_info, reason, sizeof(reason)) ==
               NK_RUNTIME_PACKAGE_OK);
        assert(cached_info.validation_cache_hit);
        assert(strcmp(first_info.package_root, cached_info.package_root) == 0);
        assert(strcmp(first_info.executable_path, cached_info.executable_path) == 0);
        assert(strcmp(first_info.image_path, cached_info.image_path) == 0);
        assert(nk_launch_runtime_package_cache_identity(
            validation_root, &game, identity_before));

        write_text_file(report_json, "{}");
        NkRuntimePackageInfo changed_report_info;
        assert(nk_launch_validate_runtime_package(
                   validation_root, &game, &changed_report_info, reason,
                   sizeof(reason)) == NK_RUNTIME_PACKAGE_STALE);
        assert(!changed_report_info.validation_cache_hit);

        write_runtime_package_fixture(validation_root, cached_disc_id,
                                      "synthetic-allegrex-v1", SR_CPUSTATE_ABI_VERSION,
                                      "synthetic-allegrex-v1.exe", FIXTURE_SHA256, NULL);
        NkRuntimePackageInfo refreshed_info;
        NkRuntimePackageStatus refreshed_status;
        refreshed_status = nk_launch_validate_runtime_package(
            validation_root, &game, &refreshed_info, reason, sizeof(reason));
        assert_repaired_validation(validation_root, &game, identity_before,
                                   refreshed_status,
                                   &refreshed_info);
        assert(nk_launch_validate_runtime_package(
                   validation_root, &game, &cached_info, reason,
                   sizeof(reason)) == NK_RUNTIME_PACKAGE_OK);
        assert(cached_info.validation_cache_hit);

        assert(remove(image) == 0);
        assert(!nk_launch_runtime_package_cache_identity(
            validation_root, &game, identity_after));
        NkRuntimePackageInfo missing_image_info;
        NkRuntimePackageStatus missing_image_status = nk_launch_validate_runtime_package(
            validation_root, &game, &missing_image_info, reason, sizeof(reason));
        assert(missing_image_status == NK_RUNTIME_PACKAGE_STALE);
        assert(!missing_image_info.validation_cache_hit);
        write_runtime_package_fixture(validation_root, cached_disc_id,
                                      "synthetic-allegrex-v1", SR_CPUSTATE_ABI_VERSION,
                                      "synthetic-allegrex-v1.exe", FIXTURE_SHA256, NULL);
        refreshed_status = nk_launch_validate_runtime_package(
            validation_root, &game, &refreshed_info, reason, sizeof(reason));
        assert_repaired_validation(validation_root, &game, identity_before,
                                   refreshed_status,
                                   &refreshed_info);
        assert(nk_launch_validate_runtime_package(
                   validation_root, &game, &cached_info, reason,
                   sizeof(reason)) == NK_RUNTIME_PACKAGE_OK);
        assert(cached_info.validation_cache_hit);

        write_text_file(package_json, "{}");
        NkRuntimePackageInfo changed_package_info;
        assert(nk_launch_validate_runtime_package(
                   validation_root, &game, &changed_package_info, reason,
                   sizeof(reason)) == NK_RUNTIME_PACKAGE_INCOMPATIBLE);
        assert(!changed_package_info.validation_cache_hit);

        write_runtime_package_fixture(validation_root, cached_disc_id,
                                      "synthetic-allegrex-v1", SR_CPUSTATE_ABI_VERSION,
                                      "synthetic-allegrex-v1.exe", FIXTURE_SHA256, NULL);
        refreshed_status = nk_launch_validate_runtime_package(
            validation_root, &game, &refreshed_info, reason, sizeof(reason));
        assert_repaired_validation(validation_root, &game, identity_before,
                                   refreshed_status,
                                   &refreshed_info);
        assert(nk_launch_validate_runtime_package(
                   validation_root, &game, &cached_info, reason,
                   sizeof(reason)) == NK_RUNTIME_PACKAGE_OK);
        assert(cached_info.validation_cache_hit);

        /* A complete, self-consistent package built for the previous CpuState
           ABI: the player refuses it by name through the same validation route
           it uses before a launch, never from the validation cache, and leaves
           no launchable paths behind. The validation cache keys on file
           identities (size, write time, change time), not on bytes, so a
           rewrite that only flips the ABI digit and lands inside the write
           timestamp tick of the accepted fixture would be served from the
           entry that validated that fixture (the same-size case is #683's and
           is asserted on its own below). A package built by an older player is
           a different build with a different size, so this one carries a
           larger build report: the identity cannot miss it on any host, and
           the refusal can then only come from the runtime ABI check. */
        long accepted_report_size = fixture_file_size(report_json);
        assert(accepted_report_size > 0);
        char accepted_identity[65];
        assert(nk_launch_runtime_package_cache_identity(
            validation_root, &game, accepted_identity));
        write_runtime_package_fixture_with_report_size(
            validation_root, cached_disc_id, "synthetic-allegrex-v1",
            SR_CPUSTATE_ABI_VERSION - 1u, "synthetic-allegrex-v1.exe", FIXTURE_SHA256, NULL,
            (size_t)accepted_report_size + 64u);
        assert(fixture_file_size(report_json) == accepted_report_size + 64);
        char previous_abi_identity[65];
        assert(nk_launch_runtime_package_cache_identity(
            validation_root, &game, previous_abi_identity));
        assert(strcmp(accepted_identity, previous_abi_identity) != 0);
        NkRuntimePackageInfo previous_abi_info;
        memset(&previous_abi_info, 0xA5, sizeof(previous_abi_info));
        reason[0] = '\0';
        assert(nk_launch_validate_runtime_package(
                   validation_root, &game, &previous_abi_info, reason,
                   sizeof(reason)) == NK_RUNTIME_PACKAGE_INCOMPATIBLE);
        assert(!previous_abi_info.validation_cache_hit);
        assert(strstr(reason, "runtime ABI is incompatible with this player build") != NULL);
        assert(previous_abi_info.package_root[0] == '\0');
        assert(previous_abi_info.executable_path[0] == '\0');
        assert(previous_abi_info.image_path[0] == '\0');
        write_runtime_package_fixture(validation_root, cached_disc_id,
                                      "synthetic-allegrex-v1", SR_CPUSTATE_ABI_VERSION,
                                      "synthetic-allegrex-v1.exe", FIXTURE_SHA256, NULL);
        refreshed_status = nk_launch_validate_runtime_package(
            validation_root, &game, &refreshed_info, reason, sizeof(reason));
        assert_repaired_validation(validation_root, &game, identity_before,
                                   refreshed_status,
                                   &refreshed_info);

        assert(nk_launch_runtime_package_cache_identity(
            validation_root, &game, identity_before));
        /* A mutation the identity cannot miss: size is compared before the
           write timestamp, so a size change is visible on every host and this
           input can never be served from the entry that validated the smaller
           file. The same-size case needs the platform change value to be
           visible at all, and it is asserted on its own below. */
        long executable_size_before = fixture_file_size(executable);
        write_text_file(executable, "modified package executable");
        assert(fixture_file_size(executable) != executable_size_before);
        assert(nk_launch_runtime_package_cache_identity(
            validation_root, &game, identity_after));
        assert(strcmp(identity_before, identity_after) != 0);
        NkRuntimePackageInfo changed_info;
        assert(nk_launch_validate_runtime_package(
                   validation_root, &game, &changed_info, reason, sizeof(reason)) ==
               NK_RUNTIME_PACKAGE_STALE);
        assert(!changed_info.validation_cache_hit);

        /* A same-size edit inside one write-timestamp tick (#683): rewrite the
           executable with different bytes of the same length, then put its
           write timestamp back to the value it had, so size and write time are
           provably unchanged. When the platform change value advances, it must
           invalidate the entry; when that filesystem timestamp also stays in
           one tick, the metadata identity can still collide. */
        write_runtime_package_fixture(validation_root, cached_disc_id,
                                      "synthetic-allegrex-v1", SR_CPUSTATE_ABI_VERSION,
                                      "synthetic-allegrex-v1.exe", FIXTURE_SHA256, NULL);
        NkRuntimePackageInfo pinned_entry_info;
        NkRuntimePackageStatus pinned_entry_status = nk_launch_validate_runtime_package(
            validation_root, &game, &pinned_entry_info, reason, sizeof(reason));
        assert_repaired_validation(validation_root, &game, identity_before,
                                   pinned_entry_status,
                                   &pinned_entry_info);
        char pinned_identity_before[65];
        assert(nk_launch_runtime_package_cache_identity(
            validation_root, &game, pinned_identity_before));
        long pinned_size = fixture_file_size(executable);
        uint64_t change_before = fixture_change_value(executable);
        assert(pinned_size > 0);
        char *same_size_bytes = (char *)malloc((size_t)pinned_size + 1);
        assert(same_size_bytes != NULL);
        memset(same_size_bytes, 'm', (size_t)pinned_size);
        same_size_bytes[pinned_size] = '\0';
        assert(strcmp(same_size_bytes, "fixture") != 0);
        assert(rewrite_preserving_size_and_mtime(executable, same_size_bytes));
        free(same_size_bytes);
        assert(fixture_file_size(executable) == pinned_size);
        char pinned_identity_after[65];
        assert(nk_launch_runtime_package_cache_identity(
            validation_root, &game, pinned_identity_after));
        bool identity_changed =
            strcmp(pinned_identity_before, pinned_identity_after) != 0;
        bool change_value_changed =
            change_before != fixture_change_value(executable);
        assert(identity_changed == change_value_changed);
        NkRuntimePackageInfo pinned_info;
        NkRuntimePackageStatus pinned_status = nk_launch_validate_runtime_package(
            validation_root, &game, &pinned_info, reason, sizeof(reason));
        if (change_value_changed) {
            assert(pinned_status == NK_RUNTIME_PACKAGE_STALE);
            assert(!pinned_info.validation_cache_hit);
        } else {
            assert(pinned_status == NK_RUNTIME_PACKAGE_OK);
            assert(pinned_info.validation_cache_hit);
            fprintf(stderr,
                    "[PLAYER_STATE_TEST] Same-size edit shared the filesystem "
                    "change-value tick; metadata cache identity cannot distinguish it.\n");
        }

        write_runtime_package_fixture_with_report_size(
            validation_root, cached_disc_id, "synthetic-allegrex-v1", SR_CPUSTATE_ABI_VERSION,
            "synthetic-allegrex-v1.exe", FIXTURE_SHA256, NULL,
            (size_t)NK_BUILD_REPORT_MAX_BYTES + 1u);
        assert(fixture_file_size(report_json) ==
               (long)(NK_BUILD_REPORT_MAX_BYTES + 1u));
        NkRuntimePackageInfo oversized_report_info;
        NkRuntimePackageStatus oversized_report_status =
            nk_launch_validate_runtime_package(
                validation_root, &game, &oversized_report_info, reason,
                sizeof(reason));
        assert(oversized_report_status == NK_RUNTIME_PACKAGE_INCOMPATIBLE);
        assert(strstr(reason,
                      "Package build-report.json exceeds the 4 MiB build-report limit") != NULL);

        /* Leave the machine as found. These five removes already ran here, but
           the root they cleaned still held the fixture's
           title-input-identities/<disc>/title-input-identity.json (written by
           write_runtime_package_fixture below the same root) and the package
           directory itself, so every run left a fresh identity document in the
           shared cache tree. remove_runtime_package_fixture() drops the same
           five files and the identity, then the now-empty directories. */
        remove_runtime_package_fixture(validation_root, cached_disc_id,
                                       "synthetic-allegrex-v1");
    }

#if defined(NK_TITLE_MANIFEST_TEST_SEAMS)
    {
        /* 19b. One catalog epoch across package validation and its cache
         * identity (#670).
         *
         * This is deterministic cross-thread interposition at the publication
         * checkpoint, NOT a concurrency stress test: the validator blocks
         * while a second thread clears and reparses a registered overlay, then
         * joins it before cache insertion, so the two never run at the same
         * time. Running the reload on another thread proves the catalog lock
         * and epoch do not depend on the validator's thread identity.
         * The defect it pins is a sequence defect, not a race in the
         * memory-model sense: validate_aot_package() reads the catalog epoch
         * once for the lookup and then read it a second time when it published,
         * so a reload interleaved anywhere between those two reads produced an
         * entry stamped with one epoch whose status identity digest belonged to
         * another. The checkpoint puts the clear/reparse exactly between the
         * snapshot and publication on every run.
         *
         * That is why the assertions below are sufficient and provable rather
         * than probabilistic: the epoch provably advanced between the snapshot
         * and the publication, the digest provably is epoch-sensitive, and the
         * published entry therefore either carries the snapshot epoch with its
         * own epoch's digest (correct) or mixes the two (the defect). The
         * second thread is joined at the checkpoint, so no sleep or scheduler
         * timing determines where the reload lands.
         *
         * Truly concurrent clear/reparse coverage lives in the SDL3-gated
         * player UI regression
         * (test_player_ui.py::test_package_status_worker_and_catalog_reload_do_not_deadlock_or_corrupt_status,
         * 128 reloads against the status worker). */
        printf("[PLAYER_STATE_TEST] Subtest 19b: cross-thread clear/reparse across validation and cache publication\n");
        fflush(stdout);

        char epoch_root[640];
        char epoch_reason[1024];
        NkTitleEntrySnapshot epoch_title_snapshot = {0};
        assert(nk_title_catalog_find_by_id("synthetic-allegrex-v1",
                                           &epoch_title_snapshot));
        char epoch_disc_id[MAX_DISC_ID_LEN];
        snprintf(epoch_disc_id, sizeof(epoch_disc_id), "%s",
                 epoch_title_snapshot.entry.primary_disc_id);
        nk_title_catalog_snapshot_release(&epoch_title_snapshot);
        /* A dedicated probe root, not the cache root itself: a subtest that
           writes a synthetic package straight into the shared cache root
           collides with every other consumer of <cache>/packages. */
        char epoch_cache_root[512];
        assert(nk_platform_get_path(NK_PATH_CACHE, epoch_cache_root,
                                    sizeof(epoch_cache_root)));
        snprintf(epoch_root, sizeof(epoch_root), "%s%cpr670_epoch_probe",
                 epoch_cache_root, nk_platform_path_separator());
        remove_runtime_package_fixture(epoch_root, epoch_disc_id,
                                       "synthetic-allegrex-v1");
        write_runtime_package_fixture(epoch_root, epoch_disc_id,
                                      "synthetic-allegrex-v1", SR_CPUSTATE_ABI_VERSION,
                                      "synthetic-allegrex-v1.exe", FIXTURE_SHA256, NULL);

        NkGameEntry epoch_game;
        memset(&epoch_game, 0, sizeof(epoch_game));
        snprintf(epoch_game.disc_id, sizeof(epoch_game.disc_id), "%s",
                 epoch_disc_id);
        snprintf(epoch_game.title_id, sizeof(epoch_game.title_id),
                 "synthetic-allegrex-v1");
        snprintf(epoch_game.selected_executable,
                 sizeof(epoch_game.selected_executable), "EBOOT.BIN");

        /* A real title-manifest reload: load the overlay from disk, which
           registers it in the catalog and installs the parser's storage-reset
           callback. Registration is asserted below, so the checkpoint really
           clears a registered overlay rather than an empty registry. */
        static const char epoch_overlay_json[] =
            "{\n"
            "  \"schema_version\": 1,\n"
            "  \"id\": \"epoch-coherence-overlay\",\n"
            "  \"display_name\": \"Epoch Coherence Overlay\",\n"
            "  \"kind\": \"retail\",\n"
            "  \"disc\": {\"id\": \"UCUS99901\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"},\n"
            "  \"executable\": {\"base\": \"0x08800000\", \"entry\": \"0x08804000\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
            "  \"modules\": [{\"name\": \"epoch.prx\", \"load_address\": \"0x08900000\", \"required\": true, \"role\": \"guest-prx\"}],\n"
            "  \"filesystem\": {\"data_root\": \"data/epoch\", \"memory_stick_root\": \"ms/epoch\", \"device_prefixes\": [\"host0:\"]},\n"
            "  \"hle_profile\": \"standard\",\n"
            "  \"feature_requirements\": [\"allegrex\"],\n"
            "  \"verification_profile\": \"smoke\"\n"
            "}\n";
        char epoch_overlay_path[768];
        char epoch_overlay_error[512];
        snprintf(epoch_overlay_path, sizeof(epoch_overlay_path),
                 "%s%cepoch-coherence-overlay.json", epoch_root,
                 nk_platform_path_separator());
        write_text_file(epoch_overlay_path, epoch_overlay_json);
        assert(nk_title_manifest_load_overlay(epoch_overlay_path,
                                              epoch_overlay_error,
                                              sizeof(epoch_overlay_error)));

        /* The reload target is genuinely registered: the catalog resolves it. */
        NkTitleEntrySnapshot epoch_registered = {0};
        assert(nk_title_catalog_find_by_id("epoch-coherence-overlay",
                                           &epoch_registered));
        nk_title_catalog_snapshot_release(&epoch_registered);
        uint64_t const epoch_at_snapshot = nk_title_catalog_epoch();

        char identity_at_snapshot[65];
        assert(nk_launch_runtime_package_cache_identity(
            epoch_root, &epoch_game, identity_at_snapshot));

        CatalogReloadAtPublication reload;
        memset(&reload, 0, sizeof(reload));
        reload.overlay_path = epoch_overlay_path;
        s_epoch_publication_before = 0;
        s_epoch_publication_after = 0;
        nk_title_manifest_test_set_validation_checkpoint(
            force_catalog_reload_at_publication, &reload);
        NkRuntimePackageInfo epoch_info;
        NkRuntimePackageStatus epoch_status = nk_launch_validate_runtime_package(
            epoch_root, &epoch_game, &epoch_info, epoch_reason,
            sizeof(epoch_reason));
        nk_title_manifest_test_set_validation_checkpoint(NULL, NULL);
        assert(epoch_status == NK_RUNTIME_PACKAGE_OK);
        assert(!epoch_info.validation_cache_hit);

        /* The forced reload ran, and it ran between the snapshot and the
           publication: without both halves the coherence assertion below would
           be vacuous. */
        assert(s_epoch_publication_before == epoch_at_snapshot);
        assert(s_epoch_publication_after > s_epoch_publication_before);

        /* The second thread completed a real clear/reparse, leaving the
           replacement overlay visible through an owned catalog snapshot. */
        NkTitleEntrySnapshot epoch_reloaded = {0};
        assert(nk_title_catalog_find_by_id("epoch-coherence-overlay",
                                           &epoch_reloaded));
        assert(strcmp(epoch_reloaded.entry.display_name,
                      "Epoch Coherence Overlay") == 0);
        nk_title_catalog_snapshot_release(&epoch_reloaded);

        char identity_after_reload[65];
        assert(nk_launch_runtime_package_cache_identity(
            epoch_root, &epoch_game, identity_after_reload));
        assert(strcmp(identity_at_snapshot, identity_after_reload) != 0);

        uint64_t published_epoch = 0;
        char published_identity[65];
        assert(nk_title_manifest_test_package_cache_entry(&published_epoch,
                                                          published_identity));
        assert(published_epoch == epoch_at_snapshot);
        assert(strcmp(published_identity, identity_at_snapshot) == 0);

        /* A superseded entry is never served: the next validation revalidates
           against the current catalog instead of hitting it. */
        NkRuntimePackageInfo revalidated_info;
        assert(nk_launch_validate_runtime_package(
                   epoch_root, &epoch_game, &revalidated_info, epoch_reason,
                   sizeof(epoch_reason)) == NK_RUNTIME_PACKAGE_OK);
        assert(!revalidated_info.validation_cache_hit);

        nk_title_catalog_clear_overlay();
        /* Leave the machine as found: drop the overlay the probe wrote and the
           synthetic package it validated. */
        remove(epoch_overlay_path);
        remove_runtime_package_fixture(epoch_root, epoch_disc_id,
                                       "synthetic-allegrex-v1");
    }
#else
    printf("[PLAYER_STATE_TEST] Subtest 19b SKIPPED: built without "
           "NK_TITLE_MANIFEST_TEST_SEAMS\n");
#endif /* NK_TITLE_MANIFEST_TEST_SEAMS */

    /* 20. A prepared package launched TWICE through the player-owned session
     * path (#511).
     *
     * The repeat contract: both launches resolve the same save root outside
     * the repository working tree, the second launch's child sees the
     * savedata the first one wrote, early child failures produce the
     * documented structured errors (RUNTIME_PACKAGE_NOT_READY,
     * RUNTIME_PREMATURE_EXIT, RUNTIME_ERROR_EXIT) with a recovery action,
     * every launch after a failure still works -- no stuck running state, no
     * retained child handle -- and no repository-tracked file changes across
     * all runs. The staged executable is a self-copy of this test binary
     * standing in for a recompiled runtime; the package is synthetic. */
    printf("[PLAYER_STATE_TEST] Subtest 20: repeat launch through the player session path\n");
    fflush(stdout);
    {
        char sep = nk_platform_path_separator();
        char self_path[NK_MAX_PATH];
        assert(nk_platform_absolute_path(argv[0], self_path, sizeof(self_path)));

        /* Buffer sizes nest so no path can ever be truncated mid-copy. */
        char cache_dir[384];
        char scratch[448];
        char user_root[700];
        char iso_path[NK_MAX_PATH];
        char report_path[760];
        char git_before[760];
        char git_after[760];
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
        snprintf(scratch, sizeof(scratch), "%s%cplayer_repeat_launch_511",
                 cache_dir, sep);
        snprintf(user_root, sizeof(user_root), "%s%cuser", scratch, sep);
        assert(nk_platform_mkdir_p(user_root));
        snprintf(iso_path, sizeof(iso_path), "%s%cgame.iso", scratch, sep);
        write_iso_with_fixture_eboot(iso_path);
        snprintf(report_path, sizeof(report_path), "%s%crepeat_report.txt",
                 scratch, sep);
        snprintf(git_before, sizeof(git_before), "%s%cgit_before.txt", scratch, sep);
        snprintf(git_after, sizeof(git_after), "%s%cgit_after.txt", scratch, sep);

        NkTitleEntrySnapshot title_snapshot = {0};
        assert(nk_title_catalog_find_by_id("synthetic-allegrex-v1",
                                           &title_snapshot));
        const NkTitleEntry *title = &title_snapshot.entry;
        assert(title && title->primary_disc_id && title->primary_disc_id[0]);
        char disc_id[MAX_DISC_ID_LEN];
        snprintf(disc_id, sizeof(disc_id), "%s", title->primary_disc_id);
        char data_root[1100];
        snprintf(data_root, sizeof(data_root), "%s%cfixtures%cprofile_zero",
                 user_root, sep, sep);
        assert(nk_platform_mkdir_p(data_root));
        nk_title_catalog_snapshot_release(&title_snapshot);
        char logs_dir[NK_MAX_PATH];
        char run_log_last[NK_MAX_PATH + 64];
        char run_log_prev[NK_MAX_PATH + 64];
        char run_log_name[64];
        assert(nk_platform_get_path(NK_PATH_LOGS, logs_dir, sizeof(logs_dir)));
        snprintf(run_log_name, sizeof(run_log_name), "game-%s-last.txt", disc_id);
        snprintf(run_log_last, sizeof(run_log_last), "%s%c%s", logs_dir, sep, run_log_name);
        snprintf(run_log_prev, sizeof(run_log_prev), "%s%cgame-%s-prev.txt", logs_dir, sep,
                 disc_id);
        remove(run_log_last);
        remove(run_log_prev);

        char source_media_json[512];
        snprintf(source_media_json, sizeof(source_media_json),
            "{\"executable\":{\"path\":\"PSP_GAME/SYSDIR/EBOOT.BIN\","
            "\"sha256\":\"%s\"},\"modules\":[]}", FIXTURE_SHA256);
        write_runtime_package_fixture_with_source_media(
            user_root, disc_id, "synthetic-allegrex-v1", SR_CPUSTATE_ABI_VERSION,
            "synthetic-allegrex-v1.exe", FIXTURE_SHA256, self_path,
            source_media_json);
        char package_exe[1100];
        snprintf(package_exe, sizeof(package_exe),
                 "%s%cpackages%c%s%csynthetic-allegrex-v1.exe", user_root, sep,
                 sep, disc_id, sep);

        PlayerApp *rep = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(rep != NULL);
        nk_library_init(&rep->library);
        NkGameEntry entry;
        memset(&entry, 0, sizeof(entry));
        snprintf(entry.disc_id, sizeof(entry.disc_id), "%s", disc_id);
        snprintf(entry.title_id, sizeof(entry.title_id), "synthetic-allegrex-v1");
        snprintf(entry.title_name, sizeof(entry.title_name),
                 "Repeat Launch Fixture");
        snprintf(entry.selected_executable, sizeof(entry.selected_executable),
                 "EBOOT.BIN");
        snprintf(entry.iso_path, sizeof(entry.iso_path), "%s", iso_path);
        entry.status = NK_STATUS_IDENTIFIED;
        assert(nk_library_add_or_update(&rep->library, &entry) == NK_OK);
        player_app_sync_library(rep);
        assert(rep->game_count == 1);
        player_app_set_runtime_root(rep, user_root);

        bool had_report_env;
        bool had_code_env;
        bool had_stop_env;
        bool had_hold_env;
        char *old_report_env =
            capture_environment_value("NK_REPEAT_LAUNCH_REPORT_FILE", &had_report_env);
        char *old_code_env =
            capture_environment_value("NK_REPEAT_LAUNCH_EXIT_CODE", &had_code_env);
        char *old_stop_env =
            capture_environment_value("NK_REPEAT_LAUNCH_STOP_REASON", &had_stop_env);
        char *old_hold_env =
            capture_environment_value("NK_REPEAT_LAUNCH_HOLD_OPEN", &had_hold_env);
        set_environment_value("NK_REPEAT_LAUNCH_REPORT_FILE", report_path);
        set_environment_value("NK_REPEAT_LAUNCH_STOP_REASON", "");
        set_environment_value("NK_REPEAT_LAUNCH_HOLD_OPEN", "");

        bool git_available = git_status_snapshot(git_before);
        if (!git_available) {
            printf("[PLAYER_STATE_TEST] SKIP git-clean check: `git status` is unavailable here.\n");
            fflush(stdout);
        }

        char memstick1[NK_MAX_PATH];
        memstick1[0] = '\0';
        char slot_sentinel[NK_MAX_PATH + 64];
        slot_sentinel[0] = '\0';

        /* The per-disc save slot is created here, inside this run's temporary
           root, as the pre-existing state the repeat contract starts from. A
           sentinel file in it must still exist after the second launch. Launch
           1 must observe an empty save root, so this fixture's own marker is
           cleared first. Resolving the session here is the same preparation
           the launch path performs. */
        {
            NkLaunchSession probe;
            NkResult probe_result = nk_launch_prepare_session(
                &probe, &rep->games[0], user_root);
            if (probe_result != NK_OK) {
                fprintf(stderr, "repeat launch probe failed: %s\n",
                        probe.last_error);
            }
            assert(probe_result == NK_OK);
            assert(probe.memstick_root[0] != '\0');
            snprintf(memstick1, sizeof(memstick1), "%s", probe.memstick_root);
            nk_launch_stop(&probe);
            assert(nk_platform_mkdir_p(memstick1));
            snprintf(slot_sentinel, sizeof(slot_sentinel), "%s%cpre_existing_slot.txt",
                     memstick1, sep);
            write_text_file(slot_sentinel, "pre-existing per-disc savedata\n");
            char stale_marker[1200];
            snprintf(stale_marker, sizeof(stale_marker), "%s%cf511_repeat.marker",
                     memstick1, sep);
            remove(stale_marker);
        }

        /* Launch 1: the child observes an empty save root, writes savedata,
           and exits cleanly (elapsed >= 500 ms, code 0: no error). */
        set_environment_value("NK_REPEAT_LAUNCH_EXIT_CODE", "0");
        {
            char error_before[32];
            snprintf(error_before, sizeof(error_before), "%s",
                     rep->last_error.error_code);
            assert(player_app_launch_game(rep, 0));
            assert(rep->is_game_running);
            assert(rep->launch_session.argv_has_gui == true);
            assert(rep->launch_session.memstick_root[0] != '\0');
            assert(strcmp(rep->launch_session.memstick_root, memstick1) == 0);
            char cwd[NK_MAX_PATH];
            char resolved_memstick[NK_MAX_PATH];
            assert(get_working_directory(cwd, sizeof(cwd)));
            assert(nk_platform_absolute_path(memstick1, resolved_memstick,
                                             sizeof(resolved_memstick)));
            /* The save root must live outside the repository working tree
               the harness runs from (a bare "saves"-style relative root would
               land inside it). */
            assert(!path_is_within(resolved_memstick, cwd));
            rep->launch_time_ms = 1000;
            assert(nk_launch_wait(&rep->launch_session, 10000) == 0);
            assert(player_app_monitor_game_session(rep, 1500));
            assert(!rep->is_game_running);
            assert_session_released(&rep->launch_session);
            assert(strcmp(rep->last_error.error_code, error_before) == 0);
        }
        {
            RepeatLaunchReport report = read_repeat_launch_report(report_path);
            assert(report.valid);
            assert(report.marker_present == false);
            assert(strcmp(report.memstick, memstick1) == 0);
        }
        /* #1303: the ended run's output is in the logs folder, not only in
           the cache file the session deletes, and the developer log names it. */
        assert(run_log_contains(run_log_last, "synthetic runtime stdout marker_present=0"));
        assert(run_log_contains(run_log_last, "synthetic runtime stderr line"));
        assert(!nk_platform_file_exists(run_log_prev));
        assert(!nk_platform_file_exists(rep->boot_event_file_path));
        assert(dev_log_contains(rep, "game output kept in "));
        assert(dev_log_contains(rep, run_log_name));

        /* Launch 2 in the same process: same save root, and the child sees
           the savedata the first launch wrote. */
        repeat_launch_and_settle(rep, 0, 2000, 2500, memstick1);
        {
            RepeatLaunchReport report = read_repeat_launch_report(report_path);
            assert(report.valid);
            assert(report.marker_present == true);
            assert(strcmp(report.memstick, memstick1) == 0);
        }
        /* The second run is -last and the first run moved to -prev. */
        assert(run_log_contains(run_log_last, "synthetic runtime stdout marker_present=1"));
        assert(run_log_contains(run_log_prev, "synthetic runtime stdout marker_present=0"));
        /* The pre-existing per-disc slot survives both launches. */
        assert(nk_platform_file_exists(slot_sentinel));

        /* Early failure A: the staged executable vanishes. The player must
           report a structured, actionable error and leave no stuck state. */
        assert(remove(package_exe) == 0);
        assert(player_app_launch_game(rep, 0) == false);
        assert(rep->is_game_running == false);
        assert(rep->active_view == VIEW_ERROR);
        assert(strcmp(rep->last_error.error_code, "RUNTIME_PACKAGE_NOT_READY") == 0);
        assert(rep->last_error.message[0] != '\0');
        assert(strcmp(rep->last_error.recovery_action_label, "Return to Library") == 0);
        assert(rep->last_error.return_view == VIEW_LIBRARY);
        /* Recovery: restore the executable and launch again. */
        copy_executable_file(self_path, package_exe);
        repeat_launch_and_settle(rep, 0, 3000, 3500, memstick1);

        /* The player must fail closed when it cannot prepare the structured
           launch-details file that carries named child stop reasons. */
#if defined(_WIN32) || defined(_WIN64)
        const char *cache_root_env = "LOCALAPPDATA";
#elif defined(__APPLE__)
        const char *cache_root_env = "HOME";
#else
        const char *cache_root_env = "XDG_CACHE_HOME";
#endif
        bool had_cache_root_env;
        char *old_cache_root_env =
            capture_environment_value(cache_root_env, &had_cache_root_env);
        char oversized_cache_root[NK_MAX_PATH * 2];
        memset(oversized_cache_root, 'x', sizeof(oversized_cache_root) - 1);
        oversized_cache_root[sizeof(oversized_cache_root) - 1] = '\0';
        set_environment_value(cache_root_env, oversized_cache_root);
        rep->enable_focus_handoff = true;
        assert(!player_app_launch_game(rep, 0));
        assert(!rep->is_game_running);
        assert(rep->boot_event_file_path[0] == '\0');
        assert(rep->launch_session.boot_event_file_path[0] == '\0');
        assert(rep->active_view == VIEW_ERROR);
        assert(strcmp(rep->last_error.error_code,
                      "LAUNCH_DETAILS_UNAVAILABLE") == 0);
        assert(strstr(rep->last_error.message, "launch details") != NULL);
        assert(strcmp(rep->last_error.recovery_action_label,
                      "Return to Library") == 0);
        assert(rep->last_error.return_view == VIEW_LIBRARY);
        restore_environment_value(cache_root_env, old_cache_root_env,
                                  had_cache_root_env);
        free(old_cache_root_env);

        /* A stale marker must not authorize a handoff when its pathname
           cannot be removed. A nonempty directory at the exact marker path
           is a portable filesystem failure seam: remove() fails in the real
           production helper, without mocking the filesystem or player path.
           The pathname comes from the production formatter, so the seam lands
           on the sequence the next launch really uses instead of assuming a
           hardcoded sequence number. */
        char stale_marker_path[1100];
        char stale_marker_child[1200];
        char next_marker_path[1200];
        player_app_next_boot_event_path(next_marker_path, sizeof(next_marker_path));
        assert(next_marker_path[0] != '\0');
        assert(strstr(next_marker_path, "player-boot-") != NULL);
        assert(strstr(next_marker_path, ".events") != NULL);
        assert(strchr(next_marker_path, sep) != NULL);
        if ((size_t)snprintf(stale_marker_path, sizeof(stale_marker_path), "%s",
                             next_marker_path) >= sizeof(stale_marker_path)) {
            printf("[PLAYER_STATE_TEST] FAIL stale-marker pathname too long\n");
            return 1;
        }
        assert(nk_platform_mkdir_p(stale_marker_path));
        snprintf(stale_marker_child, sizeof(stale_marker_child), "%s%crecord",
                 stale_marker_path, sep);
        write_text_file(stale_marker_child, "stale marker\n");
        rep->enable_focus_handoff = true;
        assert(!player_app_launch_game(rep, 0));
        assert(!rep->is_game_running);
        assert(rep->boot_event_file_path[0] == '\0');
        assert(rep->launch_session.boot_event_file_path[0] == '\0');
        assert(rep->active_view == VIEW_ERROR);
        assert(strcmp(rep->last_error.error_code,
                      "LAUNCH_DETAILS_UNAVAILABLE") == 0);
        assert(nk_platform_dir_exists(stale_marker_path));
        assert(remove(stale_marker_child) == 0);
        assert(test_rmdir(stale_marker_path) == 0);
        rep->enable_focus_handoff = false;
        /* The same prepared package still launches once the cache route is
           restored, proving the failed attempt did not wedge session state. */
        repeat_launch_and_settle(rep, 0, 4000, 4500, memstick1);

        /* Early failure B: the child exits non-zero immediately. The
           documented classification is RUNTIME_PREMATURE_EXIT. */
        set_environment_value("NK_REPEAT_LAUNCH_EXIT_CODE", "7");
        assert(player_app_launch_game(rep, 0));
        assert(rep->is_game_running);
        rep->launch_time_ms = 5000;
        assert(nk_launch_wait(&rep->launch_session, 10000) == 7);
        assert(player_app_monitor_game_session(rep, 5100));
        assert(!rep->is_game_running);
        assert_session_released(&rep->launch_session);
        assert(rep->active_view == VIEW_ERROR);
        assert(strcmp(rep->last_error.error_code, "RUNTIME_PREMATURE_EXIT") == 0);
        /* #1282: a plain title and sentence; the exit code is in the details. */
        assert(strcmp(rep->last_error.title, "Game Couldn't Start") == 0);
        assert(strstr(rep->last_error.message, "Child") == NULL);
        assert(strstr(rep->last_error.message, "7") == NULL);
        assert(strstr(rep->last_error.details, "Exit code 7") != NULL);
        assert(strcmp(player_error_retry_label(&rep->last_error), "Restart game") == 0);
        assert(strcmp(rep->last_error.subject_disc_id, rep->games[0].disc_id) == 0);
        assert(strcmp(rep->last_error.recovery_action_label, "Return to Library") == 0);
        assert(rep->last_error.return_view == VIEW_LIBRARY);
        /* Recovery after the premature exit. */
        set_environment_value("NK_REPEAT_LAUNCH_EXIT_CODE", "0");
        repeat_launch_and_settle(rep, 0, 6000, 6500, memstick1);

        /* Failure C: a longer-lived child exiting non-zero is classified as
           RUNTIME_ERROR_EXIT. */
        set_environment_value("NK_REPEAT_LAUNCH_EXIT_CODE", "9");
        assert(player_app_launch_game(rep, 0));
        assert(rep->is_game_running);
        rep->launch_time_ms = 8000;
        assert(nk_launch_wait(&rep->launch_session, 10000) == 9);
        assert(player_app_monitor_game_session(rep, 10000));
        assert(!rep->is_game_running);
        assert_session_released(&rep->launch_session);
        assert(rep->active_view == VIEW_ERROR);
        assert(strcmp(rep->last_error.error_code, "RUNTIME_ERROR_EXIT") == 0);
        assert(strcmp(rep->last_error.title, "Game Closed Unexpectedly") == 0);
        assert(strstr(rep->last_error.message, "last in-game save") != NULL);
        assert(strstr(rep->last_error.message, "9") == NULL);
        assert(strstr(rep->last_error.details, "Exit code 9") != NULL);
        assert(strcmp(player_error_retry_label(&rep->last_error), "Restart game") == 0);
        /* A structured runtime stop must reach the player error view with a
           plain sentence first and machine detail kept below it. */
        set_environment_value("NK_REPEAT_LAUNCH_STOP_REASON", "semantic-boundary");
        rep->enable_focus_handoff = false;
        assert(player_app_launch_game(rep, 0));
        char stop_event_path[NK_MAX_PATH];
        snprintf(stop_event_path, sizeof(stop_event_path), "%s",
                 rep->boot_event_file_path);
        assert(stop_event_path[0] != '\0');
        rep->launch_time_ms = 15000;
        assert(nk_launch_wait(&rep->launch_session, 10000) == 9);
        assert(player_app_monitor_game_session(rep, 16000));
        assert(strcmp(rep->last_error.error_code, "RUNTIME_SEMANTIC_BOUNDARY") == 0);
        /* #1282: what the game needed, in plain words, and what happens to
           progress; the raw boundary and its tracker issue are the details. */
        assert(strcmp(rep->last_error.title, "Game Stopped Early") == 0);
        assert(strstr(rep->last_error.message, "a processor feature") != NULL);
        assert(strstr(rep->last_error.message, "last in-game save") != NULL);
        assert(strstr(rep->last_error.message, "unsupported-") == NULL);
        assert(strstr(rep->last_error.boundary_text,
                      "unsupported-interpreter-form") != NULL);
        assert(strstr(rep->last_error.boundary_text, "not supported yet") != NULL);
        assert(strstr(rep->last_error.boundary_text, "tracked in issue 308") != NULL);
        assert(strchr(rep->last_error.boundary_text, '#') == NULL);
        assert(strcmp(player_error_retry_label(&rep->last_error), "Restart game") == 0);
        /* #1303: the card's log file is the kept run log, which holds the
           stop event and the runtime's output; the cache copy is gone. */
        assert(strcmp(rep->last_error.log_file_path, run_log_last) == 0);
        assert(run_log_contains(run_log_last,
                                "BOOT_EVENT phase=stop reason=semantic-boundary "
                                "boundary=unsupported-interpreter-form issue=308"));
        assert(run_log_contains(run_log_last, "synthetic runtime stderr line"));
        assert(!nk_platform_file_exists(stop_event_path));
        {
            char report_text[PLAYER_ERROR_DETAILS_MAX + 4096];
            assert(player_app_format_error_report(rep, "test", report_text,
                                                  sizeof(report_text)) > 0);
            const char *log_line = strstr(report_text, "Log file: ");
            assert(log_line != NULL);
            const char *log_end = strchr(log_line, '\n');
            const char *name = strstr(log_line, run_log_name);
            assert(name != NULL && (!log_end || name < log_end));
        }
        set_environment_value("NK_REPEAT_LAUNCH_STOP_REASON", "");

        /* Runtime stack exhaustion has no tracker issue and must still reach the
           same named, plain-language player boundary. */
        set_environment_value("NK_REPEAT_LAUNCH_STOP_REASON",
                              "host-fiber-stack-exhausted");
        assert(player_app_launch_game(rep, 0));
        rep->launch_time_ms = 17000;
        assert(nk_launch_wait(&rep->launch_session, 10000) == 9);
        assert(player_app_monitor_game_session(rep, 18000));
        assert(strcmp(rep->last_error.error_code, "RUNTIME_SEMANTIC_BOUNDARY") == 0);
        assert(strstr(rep->last_error.boundary_text,
                      "host-fiber-stack-exhausted") != NULL);
        assert(strstr(rep->last_error.boundary_text, "not supported yet") != NULL);
        assert(strstr(rep->last_error.boundary_text, "issue") == NULL);
        assert(strchr(rep->last_error.boundary_text, '#') == NULL);
        assert(strstr(rep->last_error.message, "a processor feature") != NULL);
        if (stop_event_path[0]) remove(stop_event_path);
        set_environment_value("NK_REPEAT_LAUNCH_STOP_REASON", "");

        /* #1290: a child that is running but never writes a boot event never
           started. Until 15 s after launch the session stays running; at
           15 s the player stops the child and shows the start-failure card,
           and the developer log says why. The clock is synthetic. */
        set_environment_value("NK_REPEAT_LAUNCH_STOP_REASON", "hold-without-boot-event");
        set_environment_value("NK_REPEAT_LAUNCH_EXIT_CODE", "0");
        assert(player_app_launch_game(rep, 0));
        assert(rep->is_game_running);
        char hold_event_path[NK_MAX_PATH];
        snprintf(hold_event_path, sizeof(hold_event_path), "%s", rep->boot_event_file_path);
        assert(hold_event_path[0] != '\0');
        rep->launch_time_ms = 20000;
        assert(!player_app_monitor_game_session(rep, 20000));
        assert(!player_app_monitor_game_session(rep, 25000));
        assert(!player_app_monitor_game_session(rep, 34999));
        assert(rep->is_game_running);
        assert(player_app_monitor_game_session(rep, 35000));
        assert(!rep->is_game_running);
        assert_session_released(&rep->launch_session);
        assert(rep->active_view == VIEW_ERROR);
        assert(strcmp(rep->last_error.error_code, "RUNTIME_START_TIMEOUT") == 0);
        assert(strcmp(rep->last_error.title, "Game Couldn't Start") == 0);
        assert(strstr(rep->last_error.message, "The game didn't start") != NULL);
        assert(strcmp(player_error_badge(&rep->last_error), "Game didn't start") == 0);
        assert(player_error_leads_with_message(&rep->last_error));
        assert(strstr(rep->last_error.details, "No boot event") != NULL);
        assert(strstr(rep->last_error.details, "15000 ms") != NULL);
        assert(strcmp(player_error_retry_label(&rep->last_error), "Restart game") == 0);
        assert(strcmp(rep->last_error.subject_disc_id, rep->games[0].disc_id) == 0);
        assert(strcmp(rep->last_error.recovery_action_label, "Return to Library") == 0);
        assert(rep->last_error.return_view == VIEW_LIBRARY);
        assert(strcmp(rep->last_error.log_file_path, run_log_last) == 0);
        assert(!nk_platform_file_exists(hold_event_path));
        assert(dev_log_contains(rep, "is running (first check, 0 ms)"));
        assert(dev_log_contains(rep, "after 5000 ms; still waiting"));
        assert(dev_log_contains(rep, "after 15000 ms; stopping it"));

        /* A child whose first boot event is in the session file is starting,
           however long its window takes: the deadline no longer applies. The
           test appends the event the runtime would write. */
        assert(player_app_launch_game(rep, 0));
        assert(rep->is_game_running);
        snprintf(hold_event_path, sizeof(hold_event_path), "%s", rep->boot_event_file_path);
        {
            FILE *events = nk_fopen_utf8(hold_event_path, "ab");
            assert(events != NULL);
            assert(fputs("BOOT_EVENT phase=init public_safe=1\n", events) >= 0);
            assert(fclose(events) == 0);
        }
        rep->launch_time_ms = 40000;
        assert(!player_app_monitor_game_session(rep, 100000));
        assert(rep->is_game_running);
        assert(dev_log_contains(rep, "first boot event seen after 60000 ms"));
        player_app_stop_game(rep);
        assert(!rep->is_game_running);
        assert_session_released(&rep->launch_session);
        set_environment_value("NK_REPEAT_LAUNCH_STOP_REASON", "");

        /* Final recovery launch: the session is never stuck. */
        set_environment_value("NK_REPEAT_LAUNCH_EXIT_CODE", "0");
        repeat_launch_and_settle(rep, 0, 11000, 12000, memstick1);

        /* #1289: a live synthetic runtime presents, then stops presenting.
           Its watchdog event must surface a notice at 30 seconds, clear on a
           resumed frame, and let Stop game reach the existing early-stop card. */
        set_environment_value("NK_REPEAT_LAUNCH_HOLD_OPEN", "1");
        bool stall_launched = player_app_launch_game(rep, 0);
        bool events_ok = stall_launched && rep->is_game_running &&
                         rep->boot_event_file_path[0] != '\0';
        bool no_early_notice = false;
        bool warning_appeared = false;
        bool warning_raise_requested = false;
        bool keep_waiting_dismissed = false;
        bool dismissed_notice_stayed_hidden = false;
        bool warning_cleared = false;
        bool warning_return_requested = false;
        bool warning_returned = false;
        bool warning_stopped_game = false;
        bool warning_card_has_details = false;
        bool scheduler_summary_kept = false;
        char stall_events_path[NK_MAX_PATH] = "";
        if (events_ok) {
            snprintf(stall_events_path, sizeof(stall_events_path), "%s",
                     rep->boot_event_file_path);
            (void)player_app_monitor_game_session(rep, 13000);
            no_early_notice = !rep->stalled_game_notice_visible;
            events_ok = append_runtime_event(
                stall_events_path,
                "BOOT_EVENT phase=stalled observation=no_new_flip "
                "no_frame_vblanks=1740 seconds=29 ready=1 delay=2 wait=3 spin=4\n");
            if (events_ok) {
                (void)player_app_monitor_game_session(rep, 42000);
                no_early_notice = no_early_notice &&
                                  !rep->stalled_game_notice_visible;
                events_ok = append_runtime_event(
                    stall_events_path,
                    "BOOT_EVENT phase=stalled observation=no_new_flip "
                    "no_frame_vblanks=1800 seconds=30 ready=1 delay=2 wait=3 spin=4\n");
            }
            if (events_ok) {
                (void)player_app_monitor_game_session(rep, 43000);
                warning_appeared = rep->stalled_game_notice_visible;
                warning_raise_requested =
                    rep->stalled_game_notice_raise_requested;
                scheduler_summary_kept = rep->stalled_game_no_frame_vblanks == 1800u &&
                    rep->stalled_game_seconds == 30u && rep->stalled_game_ready == 1u &&
                    rep->stalled_game_delay == 2u && rep->stalled_game_wait == 3u &&
                    rep->stalled_game_spin == 4u;
                player_app_keep_waiting_for_stalled_game(rep);
                keep_waiting_dismissed = !rep->stalled_game_notice_visible &&
                    rep->stalled_game_notice_dismissed &&
                    rep->stalled_game_restore_game_front_requested;
                /* The player main loop consumes the focus request before it
                   polls the next runtime event. */
                rep->stalled_game_restore_game_front_requested = false;
                events_ok = append_runtime_event(
                    stall_events_path,
                    "BOOT_EVENT phase=stalled observation=no_new_flip "
                    "no_frame_vblanks=1860 seconds=31 ready=1 delay=2 wait=3 spin=4\n");
            }
            if (events_ok) {
                (void)player_app_monitor_game_session(rep, 43050);
                dismissed_notice_stayed_hidden =
                    !rep->stalled_game_notice_visible &&
                    rep->stalled_game_notice_dismissed;
                events_ok = append_runtime_event(
                    stall_events_path,
                    "BOOT_EVENT phase=resumed observation=frame_present\n");
            }
            if (events_ok) {
                (void)player_app_monitor_game_session(rep, 43100);
                warning_cleared = !rep->stalled_game_notice_visible &&
                    !rep->stalled_game_notice_dismissed;
                warning_return_requested =
                    rep->stalled_game_restore_game_front_requested;
                events_ok = append_runtime_event(
                    stall_events_path,
                    "BOOT_EVENT phase=stalled observation=no_new_flip "
                    "no_frame_vblanks=1800 seconds=30 ready=1 delay=2 wait=3 spin=4\n");
            }
            if (events_ok) {
                (void)player_app_monitor_game_session(rep, 74000);
                warning_returned = rep->stalled_game_notice_visible;
                player_app_stop_stalled_game(rep);
                warning_stopped_game = !rep->is_game_running &&
                    strcmp(rep->last_error.error_code, "RUNTIME_NO_NEW_FRAMES") == 0;
                warning_card_has_details =
                    strcmp(rep->last_error.title, "Game Stopped Early") == 0 &&
                    strstr(rep->last_error.message, "last in-game save") != NULL &&
                    strstr(rep->last_error.details, "no_frame_vblanks=1800") != NULL &&
                    strstr(rep->last_error.details, "ready=1 delay=2 wait=3 spin=4") != NULL;
            }
        }
        /* Stop even on a failed assertion so the synthetic child cannot outlive
           the native test process. */
        if (rep->is_game_running) player_app_stop_game(rep);
        set_environment_value("NK_REPEAT_LAUNCH_HOLD_OPEN", "");
        bool stall_log_kept = warning_stopped_game &&
            strcmp(rep->last_error.log_file_path, run_log_last) == 0 &&
            run_log_contains(run_log_last, "BOOT_EVENT phase=stalled") &&
            run_log_contains(run_log_last, "ready=1 delay=2 wait=3 spin=4");
        assert(events_ok);
        assert(no_early_notice);
        assert(warning_appeared);
        assert(warning_raise_requested);
        assert(keep_waiting_dismissed);
        assert(dismissed_notice_stayed_hidden);
        assert(warning_cleared);
        assert(warning_return_requested);
        assert(warning_returned);
        assert(scheduler_summary_kept);
        assert(warning_stopped_game);
        assert(warning_card_has_details);
        assert(stall_log_kept);

        /* (d) No repository-tracked file changed across the runs. */
        if (git_available) {
            bool after_ok = git_status_snapshot(git_after);
            if (!after_ok || !files_identical(git_before, git_after)) {
                fprintf(stderr, "[PLAYER_STATE_TEST] repository-tracked state "
                                "changed during the repeat-launch runs\n");
            }
            assert(after_ok);
            assert(files_identical(git_before, git_after));
        }

        restore_environment_value("NK_REPEAT_LAUNCH_REPORT_FILE", old_report_env,
                                  had_report_env);
        restore_environment_value("NK_REPEAT_LAUNCH_EXIT_CODE", old_code_env,
                                  had_code_env);
        restore_environment_value("NK_REPEAT_LAUNCH_STOP_REASON", old_stop_env,
                                  had_stop_env);
        restore_environment_value("NK_REPEAT_LAUNCH_HOLD_OPEN", old_hold_env,
                                  had_hold_env);
        free(old_report_env);
        free(old_code_env);
        free(old_stop_env);
        free(old_hold_env);
        free(rep);

        char marker_path[1200];
        snprintf(marker_path, sizeof(marker_path), "%s%cf511_repeat.marker",
                 memstick1, sep);
        remove(marker_path);
        /* Only removed when this test created it and it is now empty; a
           pre-existing per-disc save slot is left alone. */
        test_rmdir(memstick1);
        remove(report_path);
        remove(run_log_last);
        remove(run_log_prev);
        remove(git_before);
        remove(git_after);
        remove(iso_path);
        remove(package_exe);
        char package_json[1300];
        char build_report_json[1300];
        char completion_json[1300];
        char image_bin[1300];
        char package_dir[1100];
        snprintf(package_dir, sizeof(package_dir), "%s%cpackages%c%s", user_root,
                 sep, sep, disc_id);
        snprintf(package_json, sizeof(package_json), "%s%cpackage.json",
                 package_dir, sep);
        snprintf(build_report_json, sizeof(build_report_json),
                 "%s%cbuild-report.json", package_dir, sep);
        snprintf(completion_json, sizeof(completion_json),
                 "%s%ccompletion-manifest.json", package_dir, sep);
        snprintf(image_bin, sizeof(image_bin),
                 "%s%csynthetic-allegrex-v1_image.bin", package_dir, sep);
        remove(package_json);
        remove(build_report_json);
        remove(completion_json);
        remove(image_bin);
        test_rmdir(package_dir);
        char packages_dir[1100];
        snprintf(packages_dir, sizeof(packages_dir), "%s%cpackages", user_root, sep);
        test_rmdir(packages_dir);
        test_rmdir(user_root);
        test_rmdir(scratch);
    }
    /* 20b. A crafted disc controls module directory names; only plain names
       may be written under the private per-title folder. */
    {
        assert(player_module_name_is_safe("MODULE.PRX"));
        assert(player_module_name_is_safe("libfont_hv.prx"));
        assert(player_module_name_is_safe("a-b.1.elf"));
        assert(!player_module_name_is_safe(""));
        assert(!player_module_name_is_safe("..\\..\\evil.prx"));
        assert(!player_module_name_is_safe("../evil.prx"));
        assert(!player_module_name_is_safe("a/b.prx"));
        assert(!player_module_name_is_safe("C:evil.prx"));
        assert(!player_module_name_is_safe(".hidden.prx"));
        assert(!player_module_name_is_safe("CON.prx"));
        assert(!player_module_name_is_safe("lpt1.prx"));
        assert(!player_module_name_is_safe("trailing."));
        assert(!player_module_name_is_safe("space name.prx"));
        assert(player_module_name_is_safe("CONSOLE.prx"));
    }
    /* 21. Host discovery: build preflight cards and the SDL3_ttf search order. */
    printf("[PLAYER_STATE_TEST] Subtest 21: build preflight cards and SDL3_ttf search order\n");
    fflush(stdout);
    {
        /* 21a: SDL3_ttf candidates put the executable folder first, then the
           platform loader's bare names (PATH on Windows). */
        char with_dir[PLAYER_APP_TTF_MAX_CANDIDATES][MAX_PATH_LEN];
        char without_dir[PLAYER_APP_TTF_MAX_CANDIDATES][MAX_PATH_LEN];
        char trailing[PLAYER_APP_TTF_MAX_CANDIDATES][MAX_PATH_LEN];
        int n_with = player_app_ttf_library_candidates("C:/rel/bin", with_dir,
                                                       PLAYER_APP_TTF_MAX_CANDIDATES);
        int n_without = player_app_ttf_library_candidates(NULL, without_dir,
                                                          PLAYER_APP_TTF_MAX_CANDIDATES);
        int n_trailing = player_app_ttf_library_candidates("C:/rel/bin/", trailing,
                                                           PLAYER_APP_TTF_MAX_CANDIDATES);
        assert(n_with == n_without + 1);
        assert(n_with >= 2);
        assert(strstr(with_dir[0], "C:/rel/bin") != NULL);
        assert(strstr(with_dir[0], "SDL3_ttf") != NULL);
        assert(strcmp(with_dir[1], without_dir[0]) == 0);
        assert(strchr(with_dir[n_with - 1], '/') == NULL);
        assert(strchr(with_dir[n_with - 1], '\\') == NULL);
        assert(n_trailing == n_with);
        assert(strcmp(trailing[0], with_dir[0]) == 0);

        /* 21b/21c: the two discovery cards a release user can hit. */
        PlayerApp *capp = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(capp != NULL);
        nk_library_init(&capp->library);

        char saved_cwd[NK_MAX_PATH];
        assert(nk_ps_getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
        char probe[NK_MAX_PATH + 64];
        assert(nk_platform_get_path(NK_PATH_CACHE, probe, sizeof(probe)));
        size_t probe_len = strlen(probe);
        snprintf(probe + probe_len, sizeof(probe) - probe_len, "%ccli_card_probe",
                 nk_platform_path_separator());
        assert(nk_platform_mkdir_p(probe));

        char preflight_user[NK_MAX_PATH + 128];
        char preflight_iso[NK_MAX_PATH + 128];
        snprintf(preflight_user, sizeof(preflight_user), "%s%cpreflight-user",
                 probe, nk_platform_path_separator());
        snprintf(preflight_iso, sizeof(preflight_iso), "%s%cpreflight.iso",
                 probe, nk_platform_path_separator());
        assert(nk_platform_mkdir_p(preflight_user));
        write_player_package_preflight_iso(preflight_iso, "TEST00009",
                                           "Build preflight fixture", false);
        capp->game_count = 1;
        capp->selected_game_index = 0;
        snprintf(capp->games[0].disc_id, sizeof(capp->games[0].disc_id),
                 "TEST00009");
        snprintf(capp->games[0].title_id, sizeof(capp->games[0].title_id),
                 "experimental-test00009");
        snprintf(capp->games[0].title_name, sizeof(capp->games[0].title_name),
                 "Build preflight fixture");
        snprintf(capp->games[0].disc_version, sizeof(capp->games[0].disc_version),
                 "1.00");
        snprintf(capp->games[0].selected_executable,
                 sizeof(capp->games[0].selected_executable), "EBOOT.BIN");
        assert(strlen(preflight_iso) < sizeof(capp->games[0].iso_path));
        memcpy(capp->games[0].iso_path, preflight_iso,
               strlen(preflight_iso) + 1u);
        capp->games[0].iso_size_bytes =
            (uint64_t)nk_platform_get_file_size(preflight_iso);
        capp->games[0].is_experimental = true;
        capp->games[0].executable_eboot_kind = NK_ISO_EXEC_MIPS_ELF32;
        capp->games[0].executable_selection = NK_ISO_EXEC_SELECTION_EBOOT;
        assert(strlen(preflight_user) < sizeof(capp->runtime_root));
        memcpy(capp->runtime_root, preflight_user,
               strlen(preflight_user) + 1u);
        write_experimental_profile_fixture(
            preflight_user, capp->games[0].disc_id, capp->games[0].title_id,
            capp->games[0].selected_executable, FIXTURE_SHA256);

        char prior_override[1024];
        nk_ps_copy_env("NK_INSTALL_ROOT", prior_override, sizeof(prior_override));
        nk_ps_set_env("NK_INSTALL_ROOT", NULL);
        /* The card repeats the install root three times in a 512-byte message.
           The per-run temporary root makes the absolute probe path longer than
           the cache path this subtest used to get, which pushed the checkout
           hint past that limit. A relative root keeps the whole message while
           discovery still searches the working directory and its parent. */
        assert(nk_ps_chdir(probe) == 0);
        snprintf(capp->install_root, sizeof(capp->install_root), "%s", ".");

        /* CLI_NOT_FOUND gives a plain reinstall instruction and keeps the
           searched paths available in the details/log. */
        assert(!player_app_start_package_build(capp, 0));
        assert(capp->active_view == VIEW_ERROR);
        assert(strcmp(capp->last_error.error_code, "CLI_NOT_FOUND") == 0);
        assert(strcmp(capp->last_error.message,
                      "Nakagawa Recomp's build tools were not found next to the app. "
                      "Reinstall Nakagawa Recomp and keep its folder together.") == 0);
        assert(strstr(capp->last_error.details, "NK_INSTALL_ROOT") != NULL);
        assert(strstr(capp->last_error.details, "source checkout") != NULL);
        assert(strstr(capp->last_error.details, "nk_cli.py") != NULL);

        /* 21c: back in the checkout, an empty PATH requests consent before
           any prerequisite download or child process starts. */
        assert(nk_ps_chdir(saved_cwd) == 0);
        snprintf(capp->install_root, sizeof(capp->install_root), "%s", saved_cwd);
        char prior_path[32768];
        char prior_python[1024];
        nk_ps_copy_env("PATH", prior_path, sizeof(prior_path));
        nk_ps_copy_env("PYTHON", prior_python, sizeof(prior_python));
#if defined(_WIN32) || defined(_WIN64)
        /* A real program that is not Python: the version probe must reject it.
           A console tool that exits at once on an unknown argument; a GUI
           program (notepad.exe, used before) opened a window on the
           developer's desktop and held the probe until it was closed. */
        nk_ps_set_env("PYTHON", "C:/Windows/System32/where.exe");
#else
        nk_ps_set_env("PYTHON", "/bin/sh");
#endif
        nk_ps_set_env("PATH", "");

#if defined(_WIN32) || defined(_WIN64)
        assert(player_app_start_package_build(capp, 0));
        assert(capp->active_view == VIEW_PREREQ_CONSENT);
        assert(capp->prerequisites.phase == PLAYER_PREREQ_CONSENT);
        assert(capp->prerequisites.item_count == 35);
        assert(capp->prerequisites.total_bytes == UINT64_C(101848305));
        assert(capp->build_session.is_building == false);
        /* Consent cancellation has no side effects. */
        player_app_prereq_cancel(capp);
        assert(capp->prerequisites.phase == PLAYER_PREREQ_CANCELLED);
        assert(capp->active_view == VIEW_LIBRARY);
#else
        /* Automatic installation is Windows x64 only for now: other hosts are
           refused with a clear message, and nothing is downloaded. */
        assert(!player_app_start_package_build(capp, 0));
        assert(strstr(capp->last_error.message, "in the works") != NULL);
        assert(strstr(capp->last_error.message, "#") == NULL);
        assert(capp->prerequisites.phase != PLAYER_PREREQ_CONSENT);
#endif
        assert(capp->build_session.is_building == false);

        /* The next attempt records consent for this build only and reaches
           the progress card. */
        assert(player_app_prereq_begin(capp, 0, true, true));
        player_app_prereq_accept(capp, true);
        assert(capp->prerequisites.phase == PLAYER_PREREQ_BOOTSTRAP);
        assert(capp->active_view == VIEW_PREREQ_PROGRESS);
        player_app_prereq_update_progress(capp, "cpython-embed-amd64", 64, 128,
                                          64, UINT64_C(101848305));
        assert(strcmp(capp->prerequisites.current_item, "cpython-embed-amd64") == 0);
        assert(capp->prerequisites.item_received_bytes == 64);
        assert(capp->prerequisites.total_received_bytes == 64);
        player_app_prereq_cancel(capp);
        assert(capp->prerequisites.cancel_requested);
        assert(capp->active_view == VIEW_PREREQ_PROGRESS);
        player_app_prereq_finish_cancel(capp);
        assert(capp->prerequisites.phase == PLAYER_PREREQ_CANCELLED);
        assert(capp->active_view == VIEW_LIBRARY);

        assert(player_app_prereq_begin(capp, 0, false, true));
        player_app_prereq_accept(capp, false);
        assert(capp->prerequisites.phase == PLAYER_PREREQ_DOWNLOAD);
        assert(capp->active_view == VIEW_PREREQ_PROGRESS);
        player_app_prereq_cancel(capp);
        assert(capp->prerequisites.cancel_requested);
        player_app_prereq_finish_cancel(capp);
        assert(capp->prerequisites.phase == PLAYER_PREREQ_CANCELLED);
        assert(capp->active_view == VIEW_LIBRARY);

        assert(player_app_prereq_begin(capp, 0, true, true));
        player_app_prereq_accept(capp, true);
        player_app_prereq_fail(capp, "PYTHON_ARCHIVE_EXTRACT_FAILED",
                               "The verified runtime could not be extracted.");
        assert(capp->active_view == VIEW_ERROR);
        assert(strcmp(capp->last_error.error_code, "PYTHON_ARCHIVE_EXTRACT_FAILED") == 0);
        player_app_prereq_retry(capp);
        assert(capp->active_view == VIEW_PREREQ_CONSENT);
        assert(capp->prerequisites.bootstrap_python);
        player_app_prereq_accept(capp, true);
        assert(capp->prerequisites.phase == PLAYER_PREREQ_BOOTSTRAP);
        player_app_prereq_cancel(capp);
        player_app_prereq_finish_cancel(capp);

        assert(player_app_prereq_begin(capp, 0, false, true));
        player_app_prereq_accept(capp, false);
        {
            static const char *const errors[] = {
                "OFFLINE_OR_NETWORK_ERROR", "HASH_MISMATCH", "SIZE_MISMATCH",
                "REDIRECT_REJECTED", "TRUNCATED_BODY", "DISK_FULL",
                "DESTINATION_WRITE_FAILED", "MANIFEST_INVALID"
            };
            for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
                player_app_prereq_fail(capp, errors[i], "Check the connection or free disk space, then retry.");
                assert(capp->active_view == VIEW_ERROR);
                assert(capp->prerequisites.phase == PLAYER_PREREQ_FAILED);
                assert(strcmp(capp->last_error.error_code, errors[i]) == 0);
                assert(strcmp(capp->last_error.recovery_action_label, "Retry Download") == 0);
                assert(capp->last_error.return_view == VIEW_PREREQ_CONSENT);
                player_app_prereq_retry(capp);
                assert(capp->active_view == VIEW_PREREQ_CONSENT);
                assert(capp->prerequisites.phase == PLAYER_PREREQ_CONSENT);
                player_app_prereq_accept(capp, false);
                assert(capp->active_view == VIEW_PREREQ_PROGRESS);
            }
        }
        player_app_prereq_complete(capp);
        assert(capp->prerequisites.phase == PLAYER_PREREQ_INSTALLED);
        assert(capp->prerequisites.resume_build_pending);
        assert(player_app_prereq_take_resume(capp));
        assert(!player_app_prereq_take_resume(capp));

        /* Restore the process environment before any later probe. */
        if (prior_path[0]) nk_ps_set_env("PATH", prior_path);
        else nk_ps_set_env("PATH", "");
        if (prior_python[0]) nk_ps_set_env("PYTHON", prior_python);
        else nk_ps_set_env("PYTHON", NULL);
        if (prior_override[0]) nk_ps_set_env("NK_INSTALL_ROOT", prior_override);
        else nk_ps_set_env("NK_INSTALL_ROOT", NULL);

        assert(native_test_remove_tree(preflight_user));
        assert(remove(preflight_iso) == 0);
        free(capp);
    }

    /* 22. A worker-start failure is title-owned rather than cache-owned: a
     * cache invalidation preserves it for the same title, while remove and
     * re-add drops the old record so the new title starts unresolved-free. */
    printf("[PLAYER_STATE_TEST] Subtest 22: worker-start failure lifetime\n");
    fflush(stdout);
    {
        PlayerApp *failure_state = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(failure_state != NULL);
        nk_library_init(&failure_state->library);
        char failure_cache[512];
        assert(nk_platform_get_path(NK_PATH_CACHE, failure_cache,
                                    sizeof(failure_cache)));
        const char failure_file[] = "worker-failure-659.json";
        size_t failure_cache_len = strlen(failure_cache);
        assert(failure_cache_len + 1 + sizeof(failure_file) <=
               sizeof(failure_state->library.library_path));
        memcpy(failure_state->library.library_path, failure_cache,
               failure_cache_len);
        failure_state->library.library_path[failure_cache_len] =
            nk_platform_path_separator();
        memcpy(failure_state->library.library_path + failure_cache_len + 1,
               failure_file, sizeof(failure_file));
        remove(failure_state->library.library_path);

        NkGameEntry failed_entry;
        seed_entry(&failed_entry, "FAIL65901", "Worker Failure A");
        snprintf(failed_entry.title_id, sizeof(failed_entry.title_id),
                 "worker-failure-a");
        snprintf(failed_entry.selected_executable,
                 sizeof(failed_entry.selected_executable), "EBOOT.BIN");
        NkGameEntry other_entry;
        seed_entry(&other_entry, "FAIL65902", "Worker Failure B");
        snprintf(other_entry.title_id, sizeof(other_entry.title_id),
                 "worker-failure-b");
        snprintf(other_entry.selected_executable,
                 sizeof(other_entry.selected_executable), "EBOOT.BIN");
        assert(nk_library_add_or_update(&failure_state->library,
                                        &failed_entry) == NK_OK);
        assert(nk_library_add_or_update(&failure_state->library,
                                        &other_entry) == NK_OK);
        player_app_sync_library(failure_state);
        assert(failure_state->game_count == 2);
        GameRecord failed_game = failure_state->games[0];
        player_app_runtime_package_cache_mark_failed(failure_state, 0, 1000);
        assert(!player_app_runtime_package_worker_start_failed_retry_due(
            failure_state, &failed_game, 5999));
        assert(player_app_runtime_package_worker_start_failed_retry_due(
            failure_state, &failed_game, 6000));
        assert(player_app_runtime_package_worker_start_failed(
            failure_state, &failed_game));
        assert(player_app_cached_runtime_package_status(
                   failure_state, &failed_game) == NK_RUNTIME_PACKAGE_UNKNOWN);
        assert(player_app_runtime_package_check_failed(failure_state,
                                                       &failed_game));
        player_app_runtime_package_cache_invalidate(failure_state);
        assert(player_app_cached_runtime_package_status(
                   failure_state, &failed_game) == NK_RUNTIME_PACKAGE_UNKNOWN);
        assert(player_app_runtime_package_check_failed(failure_state,
                                                       &failed_game));

        assert(player_app_remove_game(failure_state, 0));
        assert(!player_app_runtime_package_worker_start_failed(
            failure_state, &failed_game));
        assert(nk_library_add_or_update(&failure_state->library,
                                        &failed_entry) == NK_OK);
        player_app_sync_library(failure_state);
        int readded_index = player_app_find_game_by_disc_id(
            failure_state, failed_game.disc_id);
        assert(readded_index >= 0);
        assert(player_app_cached_runtime_package_status(
                   failure_state, &failure_state->games[readded_index]) ==
               NK_RUNTIME_PACKAGE_MISSING);
        assert(!player_app_runtime_package_check_failed(
            failure_state, &failure_state->games[readded_index]));
        remove(failure_state->library.library_path);
        free(failure_state);
    }

    printf("[PLAYER_STATE_TEST] Subtest 23: runtime-package cache key and transitions\n");
    fflush(stdout);
    {
        PlayerApp *cache_state = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(cache_state != NULL);
        cache_state->game_count = 1;
        seed_entry(&cache_state->games[0], "CACHE6591", "Cache fixture");
        snprintf(cache_state->games[0].title_id,
                 sizeof(cache_state->games[0].title_id), "cache-title-a");
        snprintf(cache_state->games[0].selected_executable,
                 sizeof(cache_state->games[0].selected_executable), "EBOOT.BIN");
        GameRecord cache_game = cache_state->games[0];

        player_app_runtime_package_cache_mark_pending(cache_state, 0);
        assert(player_app_runtime_package_check_pending(cache_state,
                                                        &cache_game));
        assert(!player_app_cached_game_has_runtime(cache_state, &cache_game));
        player_app_runtime_package_cache_store(
            cache_state, 0, &cache_game, true, "fixture-package-identity",
            NK_RUNTIME_PACKAGE_OK, true, 100);
        assert(!player_app_runtime_package_check_pending(cache_state,
                                                         &cache_game));
        assert(player_app_cached_runtime_package_status(
                   cache_state, &cache_game) == NK_RUNTIME_PACKAGE_OK);
        assert(player_app_cached_game_has_runtime(cache_state, &cache_game));

        GameRecord different_executable = cache_game;
        snprintf(different_executable.selected_executable,
                 sizeof(different_executable.selected_executable), "PBOOT.PBP");
        assert(player_app_cached_runtime_package_status(
                   cache_state, &different_executable) == NK_RUNTIME_PACKAGE_MISSING);
        assert(!player_app_cached_game_has_runtime(cache_state,
                                                   &different_executable));

        snprintf(cache_state->games[0].title_id,
                 sizeof(cache_state->games[0].title_id), "cache-title-b");
        GameRecord changed_title = cache_state->games[0];
        assert(player_app_cached_runtime_package_status(
                   cache_state, &changed_title) == NK_RUNTIME_PACKAGE_MISSING);
        assert(!player_app_cached_game_has_runtime(cache_state, &changed_title));
        player_app_runtime_package_cache_mark_pending(cache_state, 0);
        assert(player_app_runtime_package_check_pending(cache_state,
                                                        &changed_title));
        player_app_runtime_package_cache_invalidate(cache_state);
        assert(!player_app_runtime_package_check_pending(cache_state,
                                                        &changed_title));
        assert(player_app_cached_runtime_package_status(
                   cache_state, &changed_title) == NK_RUNTIME_PACKAGE_MISSING);
        assert(!player_app_cached_game_has_runtime(cache_state, &changed_title));
        free(cache_state);
    }

    {
        printf("[PLAYER_STATE_TEST] Subtest: ADD TO LIBRARY stages a title that takes its data from the disc\n");
        char sep = nk_platform_path_separator();
        char manifest_dir[1024];
        char manifest_path[1200];
        snprintf(manifest_dir, sizeof(manifest_dir), "%s%cdisc-data-manifests",
                 native_test_get_root(), sep);
        assert(nk_platform_mkdir_p(manifest_dir));
        /* The archive layout: the whole USRDIR is a loose-content root and
           the data root lies under it, so the data can only come from the disc. */
        snprintf(manifest_path, sizeof(manifest_path), "%s%carchive.json", manifest_dir, sep);
        write_text_file(manifest_path,
            "{\"schema_version\":1,\"id\":\"disc-data-test90021-v1\","
            "\"display_name\":\"Disc Data Fixture\",\"kind\":\"retail\","
            "\"disc\":{\"id\":\"TEST90021\",\"region\":\"NA\","
            "\"revision_policy\":\"exact-disc-id\"},"
            "\"executable\":{\"base\":0,\"entry\":0,\"bss_metadata_source\":\"none\","
            "\"extra_executable_spans\":[]},\"modules\":[],"
            "\"filesystem\":{\"data_root\":\"xbdata\",\"memory_stick_root\":\"memstick\","
            "\"device_prefixes\":[\"host0:\",\"ms0:\"],"
            "\"loose_content_roots\":[{\"root\":\".\",\"mount\":\"\",\"precedence\":0,"
            "\"skip_primary_root\":true}]},"
            "\"hle_profile\":\"generic\",\"feature_requirements\":[],"
            "\"verification_profile\":\"experimental-unverified\"}");
        /* A data root resolved outside the disc: nothing to stage but EBOOT. */
        snprintf(manifest_path, sizeof(manifest_path), "%s%cplain.json", manifest_dir, sep);
        write_text_file(manifest_path,
            "{\"schema_version\":1,\"id\":\"plain-data-test90022-v1\","
            "\"display_name\":\"Plain Data Fixture\",\"kind\":\"retail\","
            "\"disc\":{\"id\":\"TEST90022\",\"region\":\"NA\","
            "\"revision_policy\":\"exact-disc-id\"},"
            "\"executable\":{\"base\":0,\"entry\":0,\"bss_metadata_source\":\"none\","
            "\"extra_executable_spans\":[]},\"modules\":[],"
            "\"filesystem\":{\"data_root\":\"data\",\"memory_stick_root\":\"memstick\","
            "\"device_prefixes\":[\"host0:\",\"ms0:\"]},"
            "\"hle_profile\":\"generic\",\"feature_requirements\":[],"
            "\"verification_profile\":\"experimental-unverified\"}");
        char overlay_report[2048];
        assert(nk_title_manifest_load_overlay_dir(manifest_dir, overlay_report,
                                                  sizeof(overlay_report)) == 2);

        PlayerApp *probe = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(probe != NULL);
        nk_library_init(&probe->library);
        GameRecord *inspected = &probe->inspecting_game;
        snprintf(inspected->disc_id, sizeof(inspected->disc_id), "TEST90021");
        snprintf(inspected->title_id, sizeof(inspected->title_id), "disc-data-test90021-v1");
        snprintf(inspected->disc_version, sizeof(inspected->disc_version), "1.00");
        snprintf(inspected->iso_path, sizeof(inspected->iso_path), "%s%cabsent%cdisc.iso",
                 native_test_get_root(), sep, sep);
        inspected->status = NK_STATUS_VERIFIED;

        PlayerStagePlan *plan = (PlayerStagePlan *)calloc(1, sizeof(PlayerStagePlan));
        assert(plan != NULL);
        assert(player_app_build_stage_plan(inspected, plan));
        assert(plan->request.loose_content_root_count == 1);
        assert(strcmp(plan->request.loose_content_roots[0], ".") == 0);
        assert(strcmp(plan->request.data_root, "xbdata") == 0);
        assert(player_stage_title_takes_data_from_disc(&plan->request));
        free(plan);
        assert(player_app_inspected_game_needs_staging(probe));

        /* ADD TO LIBRARY hands the disc to the wizard's staging worker; the
           game is saved only when its files are in place. */
        assert(player_app_add_inspected_game(probe));
        assert(probe->active_view == VIEW_SETUP_WIZARD);
        assert(probe->wizard.step == WIZARD_STEP_INSPECT_VERIFY);
        assert(probe->wizard.is_extracting && probe->wizard.extraction_requested);
        assert(player_app_find_game_by_disc_id(probe, "TEST90021") < 0);

        /* Files already staged for this disc: nothing to set up again. */
        char staged_root[400];
        char staged_data[480];
        snprintf(staged_root, sizeof(staged_root), "%s%cstaged-test90021",
                 native_test_get_root(), sep);
        snprintf(staged_data, sizeof(staged_data), "%s%cxbdata", staged_root, sep);
        assert(nk_platform_mkdir_p(staged_data));
        inspected->assets_staged = true;
        snprintf(inspected->prepared_root, sizeof(inspected->prepared_root), "%s", staged_root);
        assert(!player_app_inspected_game_needs_staging(probe));
        inspected->assets_staged = false;
        inspected->prepared_root[0] = '\0';

        /* A title whose data does not come from the disc is saved directly. */
        snprintf(inspected->disc_id, sizeof(inspected->disc_id), "TEST90022");
        snprintf(inspected->title_id, sizeof(inspected->title_id), "plain-data-test90022-v1");
        assert(!player_app_inspected_game_needs_staging(probe));
        probe->active_view = VIEW_SUPPORTED_TITLE;
        assert(player_app_add_inspected_game(probe));
        assert(probe->active_view == VIEW_LIBRARY);
        assert(player_app_find_game_by_disc_id(probe, "TEST90022") >= 0);
        free(probe);
        printf("[PLAYER_STATE_TEST] ADD TO LIBRARY staging decision PASSED\n");
    }

    free(app);
    printf("[PLAYER_STATE_TEST] ALL PLAYER STATE TESTS PASSED!\n");
    native_test_assert_real_config_untouched();
    return 0;
}
