/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Launch-session resolution: sibling image discovery and save routing.
 *
 * Both behaviours are host-shaped and were wrong in host-specific ways:
 *
 *   - the alongside-image transformation ran only for names ending in .exe,
 *     so an ordinary extensionless POSIX binary never probed its sibling
 *     <executable>_image.bin and the runtime was started with no --image;
 *   - SR_MEMSTICK was hard-coded to the relative path "saves", which a
 *     packaged install below a read-only directory cannot create, and which
 *     routed every title into one shared root.
 *
 * These tests run identically on Win32 and POSIX: nothing here depends on a
 * particular separator or on an executable carrying an extension.
 */

#include "nk_launch.h"
#include "nk_platform.h"
#include "nk_types.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (data && *data) {
        assert(fwrite(data, 1, strlen(data), f) == strlen(data));
    }
    fclose(f);
}

static bool ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    return ls >= lf && strcmp(s + (ls - lf), suffix) == 0;
}

static void make_game(NkGameEntry *game, const char *iso_path) {
    memset(game, 0, sizeof(*game));
    snprintf(game->disc_id, sizeof(game->disc_id), "TEST00001");
    snprintf(game->title_id, sizeof(game->title_id), "synthetic-allegrex-v1");
    snprintf(game->title_name, sizeof(game->title_name), "Launch Resolution Fixture");
    snprintf(game->iso_path, sizeof(game->iso_path), "%s", iso_path);
}

int main(void) {
    char cache_dir[512];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));

    char sep = nk_platform_path_separator();
    char base[600];
    snprintf(base, sizeof(base), "%s%claunch_resolution_tests", cache_dir, sep);
    assert(nk_platform_mkdir_p(base));

    char iso_path[700];
    snprintf(iso_path, sizeof(iso_path), "%s%cgame.iso", base, sep);
    write_file(iso_path, "NOT-A-REAL-ISO");

    NkGameEntry game;
    NkLaunchSession session;

    /* 1. Extensionless executable: <executable>_image.bin must be found.
     *
     * This is the case that regressed. On POSIX the runtime binary normally
     * has no extension at all, so the .exe-only transformation never fired and
     * src/rt/driver.c exited through its insufficient-arguments path. */
    printf("[LAUNCH_TEST] Subtest 1: extensionless executable sibling image\n");
    fflush(stdout);
    char exe_noext[700];
    char img_noext[800];
    snprintf(exe_noext, sizeof(exe_noext), "%s%cmy-title", base, sep);
    snprintf(img_noext, sizeof(img_noext), "%s%cmy-title_image.bin", base, sep);
    write_file(exe_noext, "binary");
    write_file(img_noext, "image");

    make_game(&game, iso_path);
    assert(nk_launch_prepare_session(&session, &game, exe_noext) == NK_OK);
    assert(ends_with(session.executable_path, "my-title"));
    assert(ends_with(session.image_path, "my-title_image.bin"));

    /* 2. A writable, title-specific memory stick root must be resolved.
     *
     * The literal relative "saves" is what the hard-coded value used to be;
     * anything that still equals it means the resolution did not run. */
    printf("[LAUNCH_TEST] Subtest 2: writable memory stick root\n");
    fflush(stdout);
    assert(session.memstick_root[0] != 0);
    assert(strcmp(session.memstick_root, "saves") != 0);
    assert(nk_platform_dir_exists(session.memstick_root));

    /* 3. A .exe name still has its extension replaced, not appended to. */
    printf("[LAUNCH_TEST] Subtest 3: .exe extension is replaced\n");
    fflush(stdout);
    char exe_dot[700];
    char img_dot[800];
    snprintf(exe_dot, sizeof(exe_dot), "%s%cother.exe", base, sep);
    snprintf(img_dot, sizeof(img_dot), "%s%cother_image.bin", base, sep);
    write_file(exe_dot, "binary");
    write_file(img_dot, "image");

    make_game(&game, iso_path);
    assert(nk_launch_prepare_session(&session, &game, exe_dot) == NK_OK);
    assert(ends_with(session.image_path, "other_image.bin"));

    /* 4. A dot in a DIRECTORY component is not an extension.
     *
     * /opt/nakagawa.d/bin/tool must probe tool_image.bin, not
     * /opt/nakagawa_image.bin. Scanning the whole path for the last '.' gets
     * this wrong; scanning only the final component gets it right. */
    printf("[LAUNCH_TEST] Subtest 4: dotted directory is not an extension\n");
    fflush(stdout);
    char dotted_dir[700];
    snprintf(dotted_dir, sizeof(dotted_dir), "%s%cnested.d", base, sep);
    assert(nk_platform_mkdir_p(dotted_dir));

    char exe_nested[800];
    char img_nested[900];
    snprintf(exe_nested, sizeof(exe_nested), "%s%ctool", dotted_dir, sep);
    snprintf(img_nested, sizeof(img_nested), "%s%ctool_image.bin", dotted_dir, sep);
    write_file(exe_nested, "binary");
    write_file(img_nested, "image");

    make_game(&game, iso_path);
    assert(nk_launch_prepare_session(&session, &game, exe_nested) == NK_OK);
    assert(ends_with(session.image_path, "tool_image.bin"));

    /* 5. The catalog build-layout convention.
     *
     * src/core/nk_launch.c resolves a runtime by probing
     * build/<title_id>/<title_id>[.exe] and its sibling <...>_image.bin, and it
     * takes the load addresses from the generated title catalog. Nothing in the
     * tree used to be BUILT under that layout, so the whole path was unreachable:
     * every fixture built as build/<other-name>/<other_stem>.exe and no launch
     * could resolve. The display-smoke fixture is built as its own title id
     * precisely so this path is exercised, and this subtest pins the convention
     * on both hosts -- the extensionless spelling is used deliberately so the
     * POSIX shape is covered on Windows too. */
    printf("[LAUNCH_TEST] Subtest 5: catalog build-layout convention\n");
    fflush(stdout);
    char build_dir[800];
    snprintf(build_dir, sizeof(build_dir), "%s%cbuild%cdisplay-smoke-v1", base, sep, sep);
    assert(nk_platform_mkdir_p(build_dir));

    char exe_catalog[900];
    char img_catalog[1000];
    snprintf(exe_catalog, sizeof(exe_catalog), "%s%cdisplay-smoke-v1", build_dir, sep);
    snprintf(img_catalog, sizeof(img_catalog), "%s%cdisplay-smoke-v1_image.bin", build_dir, sep);
    write_file(exe_catalog, "binary");
    write_file(img_catalog, "image");

    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00006");
    snprintf(game.title_id, sizeof(game.title_id), "display-smoke-v1");
    snprintf(game.title_name, sizeof(game.title_name), "Nakagawa Display Smoke Fixture");

    assert(nk_launch_prepare_session(&session, &game, base) == NK_OK);
    assert(ends_with(session.executable_path, "display-smoke-v1"));
    assert(ends_with(session.image_path, "display-smoke-v1_image.bin"));
    /* The addresses must come from the catalog, not from a constant. */
    assert(session.base_address == 0x08810000u);
    assert(session.entry_point == 0x08810000u);

    /* 6. A title the catalog does not describe is refused, not launched at a
     * guessed address. */
    printf("[LAUNCH_TEST] Subtest 6: unknown title fails closed\n");
    fflush(stdout);
    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "ZZZZ99999");
    snprintf(game.title_id, sizeof(game.title_id), "not-a-catalog-title");
    assert(nk_launch_prepare_session(&session, &game, base) != NK_OK);

    printf("[LAUNCH_TEST] ALL LAUNCH RESOLUTION TESTS PASSED!\n");
    return 0;
}
