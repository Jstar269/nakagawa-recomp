// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "vfs_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed = 0;

#define ASSERT_STR_EQ(actual, expected) \
    if (strcmp(actual, expected) != 0) { \
        fprintf(stderr, "FAIL L%d: got '%s', expected '%s'\n", __LINE__, actual, expected); \
        g_failed = 1; \
    }

#define ASSERT_INT_EQ(actual, expected) \
    if ((actual) != (expected)) { \
        fprintf(stderr, "FAIL L%d: got %d, expected %d\n", __LINE__, (int)(actual), (int)(expected)); \
        g_failed = 1; \
    }

int main(void) {
    char out[512];
    int len;

    /* Test 1: Standard join root="fs", guest="ms0:/PSP/SAVEDATA" (sep='\\') */
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/SAVEDATA", out, sizeof(out), '\\');
    ASSERT_INT_EQ(len, 15);
    ASSERT_STR_EQ(out, "fs\\PSP\\SAVEDATA");

    /* Test 2: Standard join root="fs", guest="ms0:/PSP/SAVEDATA" (sep='/') */
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/SAVEDATA", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 15);
    ASSERT_STR_EQ(out, "fs/PSP/SAVEDATA");

    /* Test 3: Root with trailing slash root="fs/", guest="ms0:/PSP/SAVEDATA" */
    len = sr_vfs_host_dir_path("fs/", "ms0:/PSP/SAVEDATA", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 15);
    ASSERT_STR_EQ(out, "fs/PSP/SAVEDATA");

    /* Test 4: Root with trailing backslash root="fs\\", guest="ms0:\\PSP\\SAVEDATA" */
    len = sr_vfs_host_dir_path("fs\\", "ms0:\\PSP\\SAVEDATA", out, sizeof(out), '\\');
    ASSERT_INT_EQ(len, 15);
    ASSERT_STR_EQ(out, "fs\\PSP\\SAVEDATA");

    /* Test 5: Root without trailing slash, guest with root specifier only "ms0:/" */
    len = sr_vfs_host_dir_path("fs", "ms0:/", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 2);
    ASSERT_STR_EQ(out, "fs");

    /* Test 6: Guest without device prefix "PSP/SAVEDATA" */
    len = sr_vfs_host_dir_path("fs", "PSP/SAVEDATA", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 15);
    ASSERT_STR_EQ(out, "fs/PSP/SAVEDATA");

    /* Test 7: Rejection of traversal "ms0:/PSP/../SAVEDATA" */
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/../SAVEDATA", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);

    /* Test 8: Buffer size overflow limit (15 chars + NUL needs 16 bytes; buffer of 15 fails) */
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/SAVEDATA", out, 15, '/');
    ASSERT_INT_EQ(len, 0);

    /* --- ".." must reject a path *component*, not an arbitrary substring (#127 review) --- */

    /* Test 9: leading ".." component (after device/slash strip) is traversal -> reject */
    len = sr_vfs_host_dir_path("fs", "ms0:/../PSP", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);

    /* Test 10: trailing ".." component -> reject */
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/..", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);

    /* Test 11: ".." component reached via backslashes -> reject */
    len = sr_vfs_host_dir_path("fs", "ms0:\\PSP\\..\\X", out, sizeof(out), '\\');
    ASSERT_INT_EQ(len, 0);

    /* Test 12: ".." embedded in a filename is NOT traversal -> accept */
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/foo..bar", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 15);
    ASSERT_STR_EQ(out, "fs/PSP/foo..bar");

    /* Test 13: three dots is an ordinary component -> accept */
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/.../file", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 15);
    ASSERT_STR_EQ(out, "fs/PSP/.../file");

    /* Test 14: a dotfile ".hidden" is not traversal -> accept */
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/.hidden", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 14);
    ASSERT_STR_EQ(out, "fs/PSP/.hidden");

    /* Test 15: a filename that merely starts with ".." -> accept */
    len = sr_vfs_host_dir_path("fs", "ms0:/..bashrc", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 11);
    ASSERT_STR_EQ(out, "fs/..bashrc");

    /* Test 16: a root-only result must not inspect poisoned bytes beyond the
     * returned length. The canary also proves the helper respected max. */
    struct {
        char out[32];
        unsigned char canary[16];
    } poisoned;
    memset(&poisoned, 0xA5, sizeof(poisoned));
    poisoned.out[3] = '.';
    poisoned.out[4] = '.';
    poisoned.out[5] = '\\';
    len = sr_vfs_host_dir_path("fs", "ms0:/", poisoned.out,
                               sizeof(poisoned.out), '\\');
    ASSERT_INT_EQ(len, 2);
    ASSERT_STR_EQ(poisoned.out, "fs");
    ASSERT_INT_EQ((unsigned char)poisoned.out[3], (unsigned char)'.');
    ASSERT_INT_EQ((unsigned char)poisoned.out[4], (unsigned char)'.');
    for (size_t i = 0; i < sizeof(poisoned.canary); i++) {
        if (poisoned.canary[i] != 0xA5) {
            fprintf(stderr, "FAIL L%d: output overran canary at %zu\n", __LINE__, i);
            g_failed = 1;
            break;
        }
    }

    /* ---- hostile-path containment matrix (security lane) ----
     * Both host-path mappings must keep every guest-controlled string inside
     * the root: sr_vfs_host_dir_path (read-only enumeration) rejects ".."
     * components; sr_vfs_host_flat_path (writable storage) flattens each
     * input into exactly one filename and rejects the directory references
     * "", "." and "..". The checkers below re-derive the invariant from the
     * OUTPUT so a broken mapping fails the test, not just a changed fixture. */

    static const char *flat_contained[] = {
        "../..", "..\\..", "../../etc/passwd", "..\\..\\Windows\\System32\\evil.exe",
        "C:/Windows/System32/evil.exe", "C:\\Windows\\System32", "/etc/passwd",
        "\\etc\\passwd", "//server/share", "\\\\server\\share",
        "\\\\.\\PhysicalDrive0", "\\\\?\\C:\\foo",
        "ms0:/PSP/../x", "ms0:..", "ms0:.", "foo:bar", "foo bar",
        "a/b\\c:d e", "*", "?", "<", ">", "\"", "|", "~",
        "ms0:/PSP/SAVEDATA", "data/menu/text/x.to", "disc0:/PSP_GAME/USRDIR/x.dat",
        "\xd0\xbf\xd1\x83\xd1\x82\xd1\x8c/\xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb",
        "foo.", "foo ", "CON", "NUL", "...", ".hidden", "..bashrc", "a..b",
        "012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
    };
    for (size_t i = 0; i < sizeof(flat_contained) / sizeof(flat_contained[0]); i++) {
        len = sr_vfs_host_flat_path("fs", flat_contained[i], out, sizeof(out));
        ASSERT_INT_EQ(len > 0, 1);
        if (len > 0) {
            if (len < 3 || (strncmp(out, "fs/", 3) != 0 && strncmp(out, "fs\\", 3) != 0)) {
                fprintf(stderr, "FAIL L%d: '%s' -> '%s' escapes the root\n",
                        __LINE__, flat_contained[i], out);
                g_failed = 1;
            }
            for (size_t pos = len >= 3 ? 3u : (size_t)len; pos < (size_t)len; pos++) {
                if (out[pos] == '/' || out[pos] == '\\' || out[pos] == ':') {
                    fprintf(stderr, "FAIL L%d: '%s' -> '%s' contains a path metacharacter\n",
                            __LINE__, flat_contained[i], out);
                    g_failed = 1;
                    break;
                }
            }
        }
    }

    /* The three directory-reference forms are rejected outright. */
    len = sr_vfs_host_flat_path("fs", "", out, sizeof(out));
    ASSERT_INT_EQ(len, 0);
    len = sr_vfs_host_flat_path("fs", ".", out, sizeof(out));
    ASSERT_INT_EQ(len, 0);
    len = sr_vfs_host_flat_path("fs", "..", out, sizeof(out));
    ASSERT_INT_EQ(len, 0);

    /* Exact mapping contract (byte-for-byte, same as hle.c's old inline loop). */
    len = sr_vfs_host_flat_path("fs", "ms0:/PSP/SAVEDATA", out, sizeof(out));
    ASSERT_INT_EQ(len, 20);
    ASSERT_STR_EQ(out, "fs/ms0__PSP_SAVEDATA");
    len = sr_vfs_host_flat_path("fs", "foo:bar", out, sizeof(out));
    ASSERT_INT_EQ(len, 10);
    ASSERT_STR_EQ(out, "fs/foo_bar");
    len = sr_vfs_host_flat_path("fs", "a b", out, sizeof(out));
    ASSERT_INT_EQ(len, 6);
    ASSERT_STR_EQ(out, "fs/a_b");
    len = sr_vfs_host_flat_path("fs", "C:/x", out, sizeof(out));
    ASSERT_INT_EQ(len, 7);
    ASSERT_STR_EQ(out, "fs/C__x");
    len = sr_vfs_host_flat_path("fs/", "x", out, sizeof(out));
    ASSERT_INT_EQ(len, 4);
    ASSERT_STR_EQ(out, "fs/x");
    len = sr_vfs_host_flat_path("fs", "a b", out, 5);
    ASSERT_INT_EQ(len, 0);

    /* sr_vfs_host_dir_path: hostile device/UNC/wildcard/Unicode inputs must
     * either be rejected ("..") or produce a result that stays under root. */
    static const char *dir_hostile[] = {
        "../..", "../../x", "C:/Windows", "C:\\Windows", "\\\\.\\PhysicalDrive0",
        "\\\\server\\share", "//etc/passwd", "C:", "ms0:C:/x", "ms0:foo:bar",
        "ms0:a*b", "ms0:a?b", "ms0:/PSP/../x", "ms0:/a/..", "ms0:..\\x", "ms0:.",
        "\xd0\xbf\xd1\x83\xd1\x82\xd1\x8c/\xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb",
        "a\\b/c", "ms0:/", "ms0:"
    };
    for (size_t i = 0; i < sizeof(dir_hostile) / sizeof(dir_hostile[0]); i++) {
        len = sr_vfs_host_dir_path("fs", dir_hostile[i], out, sizeof(out), '\\');
        if (len == 0) continue;
        if (strcmp(out, "fs") != 0 && strncmp(out, "fs\\", 3) != 0) {
            fprintf(stderr, "FAIL L%d: '%s' -> '%s' escapes the root\n",
                    __LINE__, dir_hostile[i], out);
            g_failed = 1;
        }
        for (size_t pos = len >= 3 ? 3u : (size_t)len; pos < (size_t)len; ) {
            size_t start = pos;
            while (pos < (size_t)len && out[pos] != '\\') pos++;
            if (pos - start == 2u && out[start] == '.' && out[start + 1] == '.') {
                fprintf(stderr, "FAIL L%d: '%s' -> '%s' contains a '..' component\n",
                        __LINE__, dir_hostile[i], out);
                g_failed = 1;
                break;
            }
            if (pos < (size_t)len) pos++;
        }
    }

    /* Test 17: DOS device name detector */
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("NUL", 3), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("CON", 3), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("PRN", 3), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("AUX", 3), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("COM1", 4), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("LPT9", 4), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("nul.txt", 7), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("CON.BIN", 7), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("NULL", 4), 0);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("CONSOLE", 7), 0);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("COMMON", 6), 0);

    /* Test 18: Safe component validator */
    ASSERT_INT_EQ(sr_vfs_is_safe_component("DATA.BIN", 8), 1);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("PARAM.SFO", 9), 1);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("UCUS98701", 9), 1);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("0001", 4), 1);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("file..bak", 9), 1);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("..", 2), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component(".", 1), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a/b", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a\\b", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a:b", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("trailing_dot.", 13), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("trailing_space ", 15), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("NUL", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("CON", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("NUL.txt", 7), 0);

    /* Test 19: dir_path rejection of ADS, devices, and wildcards */
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/DATA.BIN:stream", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/NUL", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/wildcard*name", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);

#ifdef _WIN32
    /* Test 20: Win32 handle-based containment primitives */
    char temp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_dir);
    char test_root[512], test_sub[1024];
    snprintf(test_root, sizeof(test_root), "%snakagawa_selftest_root_%lu", temp_dir, (unsigned long)GetCurrentProcessId());
    snprintf(test_sub, sizeof(test_sub), "%s\\sub", test_root);
    CreateDirectoryA(test_root, NULL);
    CreateDirectoryA(test_sub, NULL);

    wchar_t canonical[MAX_PATH * 2];
    int canon_ok = sr_vfs_canonical_root(test_root, canonical, sizeof(canonical)/sizeof(wchar_t));
    ASSERT_INT_EQ(canon_ok, 1);
    if (canon_ok) {
        int sub_contained = sr_vfs_dir_is_contained(test_sub, canonical);
        ASSERT_INT_EQ(sub_contained, 1);
        int root_contained = sr_vfs_dir_is_contained(test_root, canonical);
        ASSERT_INT_EQ(root_contained, 1);
        int outside_contained = sr_vfs_dir_is_contained(temp_dir, canonical);
        ASSERT_INT_EQ(outside_contained, 0);
    }
    RemoveDirectoryA(test_sub);
    RemoveDirectoryA(test_root);
#endif

    if (g_failed) {
        fprintf(stderr, "vfs_selftest: FAILED\n");
        return 1;
    }

    printf("vfs selftest: OK\n");
    return 0;
}
