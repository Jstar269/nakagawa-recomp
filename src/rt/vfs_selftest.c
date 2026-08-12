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

    if (g_failed) {
        fprintf(stderr, "vfs_selftest: FAILED\n");
        return 1;
    }

    printf("vfs selftest: OK\n");
    return 0;
}
