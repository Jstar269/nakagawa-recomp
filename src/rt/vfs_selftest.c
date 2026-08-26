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

    /* Test 17: DOS reserved-device detector (F114-5 edge semantics). */
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("NUL", 3), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("CON", 3), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("PRN", 3), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("AUX", 3), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("COM1", 4), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("com5", 4), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("LPT9", 4), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("nul.txt", 7), 1);   /* extension is stripped by Win32 */
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("CON.BIN", 7), 1);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("NULL", 4), 0);      /* superset: ordinary name */
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("CONSOLE", 7), 0);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("COMMON", 6), 0);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("COM0", 4), 0);      /* never a reserved base */
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("LPT0", 4), 0);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("NU", 2), 0);
    ASSERT_INT_EQ(sr_vfs_is_dos_device_name("DATA.BIN", 8), 0);

    /* Test 18: safe-component validator (savedata filenames). */
    ASSERT_INT_EQ(sr_vfs_is_safe_component("DATA.BIN", 8), 1);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("PARAM.SFO", 9), 1);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("UCUS98701", 9), 1);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("0001", 4), 1);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("file..bak", 9), 1);   /* embedded dots stay legal */
    ASSERT_INT_EQ(sr_vfs_is_safe_component("...", 3), 0);         /* trailing dot aliases */
    ASSERT_INT_EQ(sr_vfs_is_safe_component("..", 2), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component(".", 1), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("", 0), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component(NULL, 5), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a/b", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a\\b", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a:b", 3), 0);         /* ADS stream syntax */
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a*b", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a?b", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a<b", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a|b", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a\"b", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("a\x01""b", 3), 0);    /* control byte */
    ASSERT_INT_EQ(sr_vfs_is_safe_component("trailing_dot.", 13), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("trailing_space ", 15), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("NUL", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("CON", 3), 0);
    ASSERT_INT_EQ(sr_vfs_is_safe_component("NUL.txt", 7), 0);

    /* Test 19: dir_path rejection of ADS, devices, wildcards, control bytes. */
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/DATA.BIN:stream", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/NUL", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/com4/x.dat", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/wildcard*name", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);
    len = sr_vfs_host_dir_path("fs", "ms0:/PSP/a<b", out, sizeof(out), '/');
    ASSERT_INT_EQ(len, 0);
    /* Repeated separators must collapse rather than smuggle an UNC prefix. */
    len = sr_vfs_host_dir_path("fs", "ms0://PSP//SAVEDATA", out, sizeof(out), '\\');
    ASSERT_INT_EQ(len > 0, 1);
    if (len > 0) {
        ASSERT_STR_EQ(out, "fs\\PSP\\SAVEDATA");
        if (strstr(out, "\\\\") != NULL) {
            fprintf(stderr, "FAIL L%d: doubled separator survived: '%s'\n", __LINE__, out);
            g_failed = 1;
        }
    }

#ifdef _WIN32
    /* Test 20: Win32 handle-based containment primitives. Everything below
     * runs against source-owned temp trees; no private input is involved. */
    {
        char temp_dir[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_dir);
        char test_root[512], test_sub[1024], sibling[512];
        snprintf(test_root, sizeof(test_root), "%snk_vfs_root_%lu", temp_dir,
                 (unsigned long)GetCurrentProcessId());
        snprintf(test_sub, sizeof(test_sub), "%s\\sub", test_root);
        snprintf(sibling, sizeof(sibling), "%snk_vfs_out_%lu", temp_dir,
                 (unsigned long)GetCurrentProcessId());
        CreateDirectoryA(test_root, NULL);
        CreateDirectoryA(test_sub, NULL);
        CreateDirectoryA(sibling, NULL);

        wchar_t canonical[MAX_PATH * 2];
        int canon_ok = sr_vfs_canonical_root(test_root, canonical, sizeof(canonical)/sizeof(wchar_t));
        ASSERT_INT_EQ(canon_ok, 1);
        if (canon_ok) {
            ASSERT_INT_EQ(sr_vfs_dir_is_contained(test_sub, canonical), 1);
            ASSERT_INT_EQ(sr_vfs_dir_is_contained(test_root, canonical), 1);
            /* The temp PARENT is outside the root. */
            ASSERT_INT_EQ(sr_vfs_dir_is_contained(temp_dir, canonical), 0);
            /* Component-boundary discipline: a sibling sharing the root's name
             * as a leading run ("rootX") must not prefix-match into containment. */
            char sib_prefix[600];
            snprintf(sib_prefix, sizeof(sib_prefix), "%sx", test_root);
            CreateDirectoryA(sib_prefix, NULL);
            ASSERT_INT_EQ(sr_vfs_dir_is_contained(sib_prefix, canonical), 0);
            RemoveDirectoryA(sib_prefix);
        }

        /* Test 21: junction escape. A junction inside the root pointing OUTSIDE
         * resolves to its target under an open handle and MUST be rejected.
         * This is the exact behavior old PR #114 demonstrated on unpatched
         * builds; here it is pinned as a permanent hostile fixture. */
        {
            char outside_file[600], junction[600], cmd[1400];
            FILE *f;
            DWORD attrs;
            snprintf(outside_file, sizeof(outside_file), "%s\\secret.txt", sibling);
            f = fopen(outside_file, "wb");
            if (!f) {
                fprintf(stderr, "FAIL L%d: cannot create outside fixture\n", __LINE__);
                g_failed = 1;
            } else {
                fputs("outside", f);
                fclose(f);
            }
            snprintf(junction, sizeof(junction), "%s\\link", test_root);
            snprintf(cmd, sizeof(cmd), "cmd /c mklink /J \"%s\" \"%s\"", junction, sibling);
            int made = system(cmd) == 0 &&
                       (GetFileAttributesA(junction) & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            ASSERT_INT_EQ(made, 1);
            if (made) {
                /* Following the link lands outside: refuse. */
                ASSERT_INT_EQ(sr_vfs_dir_is_contained(junction, canonical), 0);
                /* A file THROUGH the link is refused too. */
                char through[800];
                snprintf(through, sizeof(through), "%s\\secret.txt", junction);
                HANDLE h;
                ASSERT_INT_EQ(sr_vfs_open_contained_utf8(through, GENERIC_READ, 0,
                                                         OPEN_EXISTING, canonical, &h), 0);
                /* mkdirs whose OWNED TAIL passes through a pre-planted link
                 * must fail closed AND must not create anything beyond it
                 * anywhere (F114-2). Plant the junction as the FIRST owned
                 * component so the walk hits it before creating anything. */
                char deep[900], deep_check[1100], psp_link[600];
                snprintf(psp_link, sizeof(psp_link), "%s\\PSP", test_root);
                snprintf(cmd, sizeof(cmd), "cmd /c mklink /J \"%s\" \"%s\"", psp_link, sibling);
                int made_psp = system(cmd) == 0 &&
                               (GetFileAttributesA(psp_link) & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                ASSERT_INT_EQ(made_psp, 1);
                if (made_psp) {
                    snprintf(deep_check, sizeof(deep_check), "%s\\SAVEDATA", sibling);
                    ASSERT_INT_EQ(sr_vfs_mkdirs_contained("PSP\\SAVEDATA\\deep", canonical), 0);
                    attrs = GetFileAttributesA(deep_check);
                    ASSERT_INT_EQ(attrs == INVALID_FILE_ATTRIBUTES, 1);
                    snprintf(deep, sizeof(deep), "%s\\SAVEDATA", psp_link);
                    attrs = GetFileAttributesA(deep);
                    ASSERT_INT_EQ(attrs == INVALID_FILE_ATTRIBUTES, 1);
                    RemoveDirectoryA(psp_link);
                }
                /* Deleting the outside file BY NAME through the junction is
                 * refused; the target survives byte-for-byte (F114-1). */
                int was_dir = 0;
                ASSERT_INT_EQ(sr_vfs_delete_contained_leaf(through, canonical, &was_dir), 0);
                ASSERT_INT_EQ(was_dir, 0);
                f = fopen(outside_file, "rb");
                ASSERT_INT_EQ(f != NULL, 1);
                if (f) fclose(f);
                RemoveDirectoryA(junction);
            }

            /* Test 22: ordered creation happy path -- every level created and
             * verified under a fresh root; nothing above the root appears. */
            {
                wchar_t canonical2[MAX_PATH * 2];
                char root2[512];
                snprintf(root2, sizeof(root2), "%snk_vfs_mkdir_%lu", temp_dir,
                         (unsigned long)GetCurrentProcessId());
                ASSERT_INT_EQ(sr_vfs_canonical_root(root2, canonical2,
                                                    sizeof(canonical2)/sizeof(wchar_t)), 1);
                char tree[900], above[700];
                snprintf(tree, sizeof(tree), "%s\\PSP\\SAVEDATA\\UCUS98701DATA00", root2);
                ASSERT_INT_EQ(sr_vfs_mkdirs_contained("PSP\\SAVEDATA\\UCUS98701DATA00",
                                                      canonical2), 1);
                ASSERT_INT_EQ(sr_vfs_dir_is_contained(tree, canonical2), 1);
                /* A ".." segment is an owned-tail escape: refused outright. */
                snprintf(above, sizeof(above), "%s\\..\\nk_vfs_escape_%lu", root2,
                         (unsigned long)GetCurrentProcessId());
                ASSERT_INT_EQ(sr_vfs_mkdirs_contained("..\\nk_vfs_escape_x", canonical2), 0);
                attrs = GetFileAttributesA(above);
                ASSERT_INT_EQ(attrs == INVALID_FILE_ATTRIBUTES, 1);
                /* A component with trailing-dot aliasing is refused outright. */
                char aliased[900];
                snprintf(aliased, sizeof(aliased), "%s\\bad.\\x", root2);
                ASSERT_INT_EQ(sr_vfs_mkdirs_contained("bad.\\x", canonical2), 0);
                attrs = GetFileAttributesA(aliased);
                ASSERT_INT_EQ(attrs == INVALID_FILE_ATTRIBUTES, 1);
                /* Reserved device component refused. */
                snprintf(aliased, sizeof(aliased), "%s\\NUL\\x", root2);
                ASSERT_INT_EQ(sr_vfs_mkdirs_contained("NUL\\x", canonical2), 0);
            }

            /* Test 23: delete-by-handle semantics inside the root. */
            {
                char inner[700];
                FILE *f2;
                snprintf(inner, sizeof(inner), "%s\\sub\\data.bin", test_root);
                f2 = fopen(inner, "wb");
                if (!f2) {
                    fprintf(stderr, "FAIL L%d: cannot create inner fixture\n", __LINE__);
                    g_failed = 1;
                } else {
                    fputs("data", f2);
                    fclose(f2);
                }
                int was_dir = -1;
                ASSERT_INT_EQ(sr_vfs_delete_contained_leaf(inner, canonical, &was_dir), 1);
                ASSERT_INT_EQ(was_dir, 0);
                ASSERT_INT_EQ(GetFileAttributesA(inner) == INVALID_FILE_ATTRIBUTES, 1);
                /* Directory entry: refused, left in place. */
                was_dir = -1;
                ASSERT_INT_EQ(sr_vfs_delete_contained_leaf(test_sub, canonical, &was_dir), 0);
                ASSERT_INT_EQ(was_dir, 1);
                ASSERT_INT_EQ(GetFileAttributesA(test_sub) != INVALID_FILE_ATTRIBUTES, 1);
                /* Outside leaf: refused, survives. */
                char out_leaf[700];
                snprintf(out_leaf, sizeof(out_leaf), "%s\\z.txt", sibling);
                f2 = fopen(out_leaf, "wb");
                if (f2) fclose(f2);
                was_dir = -1;
                ASSERT_INT_EQ(sr_vfs_delete_contained_leaf(out_leaf, canonical, &was_dir), 0);
                ASSERT_INT_EQ(GetFileAttributesA(out_leaf) != INVALID_FILE_ATTRIBUTES, 1);
            }
        }

        RemoveDirectoryA(test_sub);
        RemoveDirectoryA(test_root);
        RemoveDirectoryA(sibling);
    }
#endif

    if (g_failed) {
        fprintf(stderr, "vfs_selftest: FAILED\n");
        return 1;
    }

    printf("vfs selftest: OK\n");
    return 0;
}
