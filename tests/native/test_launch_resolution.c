/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Launch-session resolution: identity-bound runtime/image discovery, sibling
 * image discovery, and save routing.
 *
 * Hostile contract (#366): generic launch resolution may only select a
 * runtime/image from the selected title's own validated identity
 * (catalog game_name / title id). A stale sibling-title build, a root-level
 * binary, or a retail-specific image probe must never satisfy another title;
 * identity disagreement between session disc and title is rejected before any
 * spawn. The alongside-image and save-routing behaviours remain host-shaped
 * regressions: the sibling transform must work for extensionless POSIX names
 * and replace, not append to, a Windows extension.
 *
 * These tests run identically on Win32 and POSIX: nothing here depends on a
 * particular separator or on an executable carrying an extension.
 */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include "nk_launch.h"
#include "nk_platform.h"
#include "nk_title_manifest.h"
#include "nk_types.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <sys/stat.h>
#include <unistd.h>
#define test_rmdir rmdir
#endif

static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (data && *data) {
        assert(fwrite(data, 1, strlen(data), f) == strlen(data));
    }
    fclose(f);
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

static void copy_executable(const char *source_path, const char *destination_path) {
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

static int write_launch_environment_probe(void) {
    const char *report_path = getenv("NK_LAUNCH_TEST_REPORT_FILE");
    if (!report_path || !*report_path) return 2;

    FILE *f = fopen(report_path, "wb");
    if (!f) return 3;
    const char *iso = getenv("PSP_ISO");
    const char *unrelated = getenv("NK_LAUNCH_TEST_UNRELATED");
    const char *fps = getenv("SR_FPS_CAP");
    const char *vsync = getenv("SR_VSYNC");
    const char *scale = getenv("SR_RESOLUTION_SCALE");
    const char *fullscreen = getenv("SR_FULLSCREEN");
    const char *volume = getenv("SR_MASTER_VOLUME");
    int ok = fprintf(f,
                     "PSP_ISO=%s\n"
                     "NK_LAUNCH_TEST_UNRELATED=%s\n"
                     "SR_FPS_CAP=%s\n"
                     "SR_VSYNC=%s\n"
                     "SR_RESOLUTION_SCALE=%s\n"
                     "SR_FULLSCREEN=%s\n"
                     "SR_MASTER_VOLUME=%s\n",
                     iso ? iso : "<unset>",
                     unrelated ? unrelated : "<unset>",
                     fps ? fps : "<unset>",
                     vsync ? vsync : "<unset>",
                     scale ? scale : "<unset>",
                     fullscreen ? fullscreen : "<unset>",
                     volume ? volume : "<unset>") >= 0;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : 4;
}

static void write_valid_elf(const char *path, uint32_t base, uint32_t entry);
static bool ends_with(const char *s, const char *suffix);
static void make_game(NkGameEntry *game, const char *iso_path);

static void test_no_iso_child_environment(const char *test_executable,
                                          const char *base, char sep) {
    printf("[LAUNCH_TEST] Subtest 18: no-ISO child environment isolation\n");
    fflush(stdout);

    char root[800];
    char runtime_dir[900];
    char runtime_path[1000];
    char image_path[1000];
    char stage_root[900];
    char staged_eboot[1000];
    char staged_xbdata[1000];
    char report_path[1000];
    char absolute_test_executable[1000];
    snprintf(root, sizeof(root), "%s%cno_iso_environment", base, sep);
    snprintf(runtime_dir, sizeof(runtime_dir), "%s%cbuild%cdisplay-smoke",
             root, sep, sep);
#if defined(_WIN32) || defined(_WIN64)
    snprintf(runtime_path, sizeof(runtime_path), "%s%cdisplay-smoke.exe",
             runtime_dir, sep);
#else
    snprintf(runtime_path, sizeof(runtime_path), "%s%cdisplay-smoke",
             runtime_dir, sep);
#endif
    snprintf(image_path, sizeof(image_path), "%s%cdisplay-smoke_image.bin",
             runtime_dir, sep);
    snprintf(stage_root, sizeof(stage_root), "%s%cprepared", root, sep);
    snprintf(staged_eboot, sizeof(staged_eboot), "%s%cEBOOT.BIN",
             stage_root, sep);
    snprintf(staged_xbdata, sizeof(staged_xbdata), "%s%cxbdata",
             stage_root, sep);
    snprintf(report_path, sizeof(report_path), "%s%creport.txt", root, sep);

    assert(nk_platform_mkdir_p(root));
    assert(nk_platform_mkdir_p(runtime_dir));
    assert(nk_platform_mkdir_p(staged_xbdata));
    assert(nk_platform_absolute_path(test_executable, absolute_test_executable,
                                     sizeof(absolute_test_executable)));
    copy_executable(absolute_test_executable, runtime_path);
    write_file(image_path, "synthetic image");
    write_valid_elf(staged_eboot, 0x08810000u, 0x08810000u);

    NkGameEntry game;
    NkLaunchSession session;
    make_game(&game, "");
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00006");
    snprintf(game.title_id, sizeof(game.title_id), "display-smoke-v1");
    assert(strlen(stage_root) < sizeof(game.prepared_root));
    memcpy(game.prepared_root, stage_root, strlen(stage_root) + 1);
    game.assets_staged = true;

    bool had_iso;
    bool had_report;
    bool had_unrelated;
    char *old_iso = capture_environment_value("PSP_ISO", &had_iso);
    char *old_report = capture_environment_value("NK_LAUNCH_TEST_REPORT_FILE",
                                                 &had_report);
    char *old_unrelated = capture_environment_value("NK_LAUNCH_TEST_UNRELATED",
                                                   &had_unrelated);
    set_environment_value("PSP_ISO", "poisoned-parent.iso");
    set_environment_value("NK_LAUNCH_TEST_REPORT_FILE", report_path);
    set_environment_value("NK_LAUNCH_TEST_UNRELATED", "preserve-me");

    assert(nk_launch_prepare_session(&session, &game, root) == NK_OK);
    assert(session.iso_path[0] == '\0');
    assert(session.staged_executable_checked);
    assert(ends_with(session.executable_path, "display-smoke") ||
           ends_with(session.executable_path, "display-smoke.exe"));
    /* Prepare defaults: a caller that never configures the session must not
       hand the runtime a muted or zeroed volume (#settings). */
    assert(session.config.master_volume == 100);
    /* Apply settings the way the player does (apply_settings_to_session)
       before spawning, so every setting reaches the child environment. */
    session.config.resolution_scale = 2;
    session.config.fps_cap = 30;
    session.config.vsync = false;
    session.config.fullscreen = true;
    session.config.master_volume = 55;
    assert(nk_launch_start(&session) == NK_OK);

    int child_exit = nk_launch_wait(&session, -1);
    nk_launch_stop(&session);
    assert(child_exit == 0);
    assert(getenv("PSP_ISO") != NULL);
    assert(strcmp(getenv("PSP_ISO"), "poisoned-parent.iso") == 0);
    assert(getenv("NK_LAUNCH_TEST_UNRELATED") != NULL);
    assert(strcmp(getenv("NK_LAUNCH_TEST_UNRELATED"), "preserve-me") == 0);

    FILE *report = fopen(report_path, "rb");
    assert(report != NULL);
    char observed[512];
    size_t observed_size = fread(observed, 1, sizeof(observed) - 1, report);
    observed[observed_size] = '\0';
    assert(!ferror(report));
    assert(fclose(report) == 0);
    assert(strstr(observed, "PSP_ISO=\n") != NULL);
    assert(strstr(observed,
                  "NK_LAUNCH_TEST_UNRELATED=preserve-me\n") != NULL);
    /* Every wired Settings value must reach the spawned child (#settings):
       frame cap, vsync, render scale, fullscreen and master volume. */
    assert(strstr(observed, "SR_FPS_CAP=30\n") != NULL);
    assert(strstr(observed, "SR_VSYNC=0\n") != NULL);
    assert(strstr(observed, "SR_RESOLUTION_SCALE=2\n") != NULL);
    assert(strstr(observed, "SR_FULLSCREEN=1\n") != NULL);
    assert(strstr(observed, "SR_MASTER_VOLUME=55\n") != NULL);

    restore_environment_value("PSP_ISO", old_iso, had_iso);
    restore_environment_value("NK_LAUNCH_TEST_REPORT_FILE", old_report,
                              had_report);
    restore_environment_value("NK_LAUNCH_TEST_UNRELATED", old_unrelated,
                              had_unrelated);
    free(old_iso);
    free(old_report);
    free(old_unrelated);

    remove(report_path);
    remove(runtime_path);
    remove(image_path);
    remove(staged_eboot);
    {
        char stage_memstick[1000];
        snprintf(stage_memstick, sizeof(stage_memstick), "%s%cmemstick",
                 stage_root, sep);
        test_rmdir(stage_memstick);
    }
    test_rmdir(staged_xbdata);
    test_rmdir(stage_root);
    test_rmdir(runtime_dir);
    {
        char build_dir[900];
        snprintf(build_dir, sizeof(build_dir), "%s%cbuild", root, sep);
        test_rmdir(build_dir);
    }
    test_rmdir(root);
}

static void write_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(data, 1, size, f) == size);
    assert(fclose(f) == 0);
}

static void write_valid_elf(const char *path, uint32_t base, uint32_t entry) {
    uint8_t image[0x100] = { 0 };
    memcpy(image, "\x7f" "ELF", 4);
    image[4] = 1; /* ELFCLASS32 */
    image[5] = 1; /* little endian */
    image[6] = 1; /* ELF version */
    write_le16(image + 16, 2); /* ET_EXEC */
    write_le16(image + 18, 8); /* EM_MIPS */
    write_le32(image + 20, 1);
    write_le32(image + 24, entry);
    write_le32(image + 28, 52);
    write_le32(image + 32, 0);
    write_le32(image + 36, 0);
    write_le16(image + 40, 52);
    write_le16(image + 42, 32);
    write_le16(image + 44, 1);
    uint8_t *ph = image + 52;
    write_le32(ph + 0, 1);       /* PT_LOAD */
    write_le32(ph + 4, 0x80);    /* source offset */
    write_le32(ph + 8, base);
    write_le32(ph + 12, base);
    write_le32(ph + 16, 16);     /* file-backed bytes */
    write_le32(ph + 20, 32);     /* 16 bytes of BSS */
    write_le32(ph + 24, 5);      /* R-X */
    write_le32(ph + 28, 4);
    for (size_t i = 0; i < 16; i++) image[0x80 + i] = (uint8_t)(0x80 + i);
    write_bytes(path, image, sizeof(image));
}

static void write_psp_container(const char *path) {
    uint8_t image[0x150] = { 0 };
    memcpy(image, "~PSP", 4);
    image[0x27] = 1;                 /* one PT_LOAD segment */
    write_le32(image + 0x38, 0);     /* no BSS */
    write_le32(image + 0x54, 0x1000); /* segment file size */
    write_bytes(path, image, sizeof(image));
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

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--image") == 0) {
        return write_launch_environment_probe();
    }

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
     * The runtime binary normally has no extension on POSIX, so the sibling
     * image probe must fire for the extensionless spelling of the selected
     * title's own build product, and src/rt/driver.c must never be started
     * without --image. The fixture lives under the title-owned build layout.
     */
    printf("[LAUNCH_TEST] Subtest 1: extensionless executable sibling image\n");
    fflush(stdout);
    char root1[700];
    char exe_noext[800];
    char img_noext[900];
    snprintf(root1, sizeof(root1), "%s%croot1", base, sep);
    snprintf(exe_noext, sizeof(exe_noext), "%s%cbuild%csynthetic%csynthetic",
             root1, sep, sep, sep);
    snprintf(img_noext, sizeof(img_noext), "%s%cbuild%csynthetic%csynthetic_image.bin",
             root1, sep, sep, sep);
    assert(nk_platform_mkdir_p(root1));
    {
        char exe_parent[800];
        snprintf(exe_parent, sizeof(exe_parent), "%s%cbuild%csynthetic",
                 root1, sep, sep);
        assert(nk_platform_mkdir_p(exe_parent));
    }
    write_file(exe_noext, "binary");
    write_file(img_noext, "image");

    make_game(&game, iso_path);
    assert(nk_launch_prepare_session(&session, &game, root1) == NK_OK);
    assert(ends_with(session.executable_path, "synthetic"));
    assert(ends_with(session.image_path, "synthetic_image.bin"));

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
    char root2[700];
    char exe_dot[800];
    char img_dot[900];
    snprintf(root2, sizeof(root2), "%s%croot2", base, sep);
    snprintf(exe_dot, sizeof(exe_dot), "%s%cbuild%csynthetic%csynthetic.exe",
             root2, sep, sep, sep);
    snprintf(img_dot, sizeof(img_dot), "%s%cbuild%csynthetic%csynthetic_image.bin",
             root2, sep, sep, sep);
    {
        char exe_parent[800];
        snprintf(exe_parent, sizeof(exe_parent), "%s%cbuild%csynthetic",
                 root2, sep, sep);
        assert(nk_platform_mkdir_p(exe_parent));
    }
    write_file(exe_dot, "binary");
    write_file(img_dot, "image");

    make_game(&game, iso_path);
    assert(nk_launch_prepare_session(&session, &game, root2) == NK_OK);
    assert(ends_with(session.executable_path, "synthetic.exe"));
    assert(ends_with(session.image_path, "synthetic_image.bin"));

    /* 4. A dot in a DIRECTORY component is not an extension.
     *
     * A root such as <base>/nested.d/build/<title>/<title> must probe
     * <title>_image.bin beside the executable, not <root>_image.bin. Scanning
     * the whole path for the last '.' gets this wrong; scanning only the final
     * component gets it right. */
    printf("[LAUNCH_TEST] Subtest 4: dotted directory is not an extension\n");
    fflush(stdout);
    char dotted_dir[700];
    snprintf(dotted_dir, sizeof(dotted_dir), "%s%cnested.d", base, sep);
    assert(nk_platform_mkdir_p(dotted_dir));

    char exe_nested[800];
    char img_nested[900];
    snprintf(exe_nested, sizeof(exe_nested), "%s%cbuild%csynthetic%csynthetic",
             dotted_dir, sep, sep, sep);
    snprintf(img_nested, sizeof(img_nested), "%s%cbuild%csynthetic%csynthetic_image.bin",
             dotted_dir, sep, sep, sep);
    {
        char exe_parent[800];
        snprintf(exe_parent, sizeof(exe_parent), "%s%cbuild%csynthetic",
                 dotted_dir, sep, sep);
        assert(nk_platform_mkdir_p(exe_parent));
    }
    write_file(exe_nested, "binary");
    write_file(img_nested, "image");

    make_game(&game, iso_path);
    assert(nk_launch_prepare_session(&session, &game, dotted_dir) == NK_OK);
    assert(ends_with(session.image_path, "synthetic_image.bin"));

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

    char data_dir[900];
    char expected_data_dir[900];
    snprintf(data_dir, sizeof(data_dir), "%s%cfixtures%cdisplay_smoke", base, sep, sep);
    assert(nk_platform_mkdir_p(data_dir));
    assert(nk_platform_absolute_path(data_dir, expected_data_dir, sizeof(expected_data_dir)));

    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00006");
    snprintf(game.title_id, sizeof(game.title_id), "display-smoke-v1");
    snprintf(game.title_name, sizeof(game.title_name), "Nakagawa Display Smoke Fixture");

    assert(nk_launch_prepare_session(&session, &game, base) == NK_OK);
    assert(nk_launch_runtime_available(base, "display-smoke-v1"));
    assert(ends_with(session.executable_path, "display-smoke-v1"));
    assert(ends_with(session.image_path, "display-smoke-v1_image.bin"));
    assert(strcmp(session.dataroot_path, expected_data_dir) == 0);
    /* The addresses must come from the catalog, not from a constant. */
    assert(session.base_address == 0x08810000u);
    assert(session.entry_point == 0x08810000u);
    assert(strcmp(nk_title_catalog_find_by_id("display-smoke-v1")->game_name,
                  "display-smoke") == 0);

    /* 6. A manifest may choose a build name that is different from its
     * versioned title id. The parser must retain it and launch discovery must
     * probe the manager's build/<game_name>/<game_name> layout. */
    printf("[LAUNCH_TEST] Subtest 6: manifest-selected game name\n");
    fflush(stdout);
    char custom_manifest[900];
    char custom_build_dir[900];
    char custom_exe[1000];
    char custom_img[1100];
    char custom_data[1000];
    char custom_ms[1000];
    snprintf(custom_manifest, sizeof(custom_manifest), "%s%claunch-name-test.json", base, sep);
    snprintf(custom_build_dir, sizeof(custom_build_dir), "%s%cbuild%ccustom-launch", base, sep, sep);
    snprintf(custom_exe, sizeof(custom_exe), "%s%ccustom-launch", custom_build_dir, sep);
    snprintf(custom_img, sizeof(custom_img), "%s%ccustom-launch_image.bin", custom_build_dir, sep);
    snprintf(custom_data, sizeof(custom_data), "%s%ccustom-data", base, sep);
    snprintf(custom_ms, sizeof(custom_ms), "%s%ccustom-ms", base, sep);
    const char *custom_manifest_json =
        "{\"schema_version\":1,\"id\":\"launch-name-test-v1\","
        "\"game_name\":\"custom-launch\",\"display_name\":\"Launch Name Test\","
        "\"kind\":\"synthetic\","
        "\"executable\":{\"base\":\"0x08820000\",\"entry\":\"0x08820000\","
        "\"bss_metadata_source\":\"elf\",\"extra_executable_spans\":[]},"
        "\"modules\":[],\"filesystem\":{\"data_root\":\"custom-data\","
        "\"memory_stick_root\":\"custom-ms\",\"device_prefixes\":[\"host0:\"]},"
        "\"hle_profile\":\"standard\",\"feature_requirements\":[\"allegrex\"],"
        "\"verification_profile\":\"smoke\"}";
    write_file(custom_manifest, custom_manifest_json);
    assert(nk_platform_mkdir_p(custom_build_dir));
    assert(nk_platform_mkdir_p(custom_data));
    assert(nk_platform_mkdir_p(custom_ms));
    write_file(custom_exe, "binary");
    write_file(custom_img, "image");

    char custom_error[512];
    assert(nk_title_manifest_load_overlay_ext(custom_manifest, false,
                                              custom_error, sizeof(custom_error)));
    const NkTitleEntry *custom_entry = nk_title_catalog_find_by_id("launch-name-test-v1");
    assert(custom_entry != NULL);
    assert(strcmp(custom_entry->game_name, "custom-launch") == 0);
    make_game(&game, iso_path);
    game.disc_id[0] = '\0';
    snprintf(game.title_id, sizeof(game.title_id), "launch-name-test-v1");
    assert(nk_launch_prepare_session(&session, &game, base) == NK_OK);
    assert(nk_launch_runtime_available(base, "launch-name-test-v1"));
    assert(ends_with(session.executable_path, "custom-launch"));
    assert(ends_with(session.image_path, "custom-launch_image.bin"));
    assert(strstr(session.dataroot_path, "custom-data") != NULL);
    assert(strstr(session.memstick_root, "custom-ms") != NULL);
    nk_title_catalog_clear_overlay();
    remove(custom_exe);
    remove(custom_img);
    remove(custom_manifest);
    test_rmdir(custom_build_dir);
    test_rmdir(custom_data);
    test_rmdir(custom_ms);

    /* 7. A promoted staging root is the source-side launch contract: the
     * staged EBOOT is checked, decoded XB data is preferred over the catalog's
     * repository-relative data root, and saves are scoped below the game. */
    printf("[LAUNCH_TEST] Subtest 7: staged EBOOT and VFS roots\n");
    fflush(stdout);
    char staged_root[800];
    char staged_eboot[900];
    char staged_xbdata[900];
    snprintf(staged_root, sizeof(staged_root), "%s%cstaged-game", base, sep);
    snprintf(staged_eboot, sizeof(staged_eboot), "%s%cEBOOT.BIN", staged_root, sep);
    snprintf(staged_xbdata, sizeof(staged_xbdata), "%s%cxbdata", staged_root, sep);
    assert(nk_platform_mkdir_p(staged_xbdata));
    write_valid_elf(staged_eboot, 0x08810000u, 0x08810000u);
    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00006");
    snprintf(game.title_id, sizeof(game.title_id), "display-smoke-v1");
    assert(strlen(staged_root) < sizeof(game.prepared_root));
    memcpy(game.prepared_root, staged_root, strlen(staged_root) + 1);
    game.assets_staged = true;
    NkLaunchExecutableInfo staged_info;
    char staged_error[256];
    assert(nk_launch_validate_staged_executable(&game,
                                                nk_title_catalog_find_by_id(game.title_id),
                                                &staged_info, staged_error,
                                                sizeof(staged_error)) == NK_OK);
    assert(staged_info.is_elf == true);
    assert(staged_info.has_bss == true);
    assert(staged_info.load_base == 0x08810000u);
    assert(staged_info.entry_in_executable_segment == true);
    assert(staged_info.bss_start == 0x08810010u);
    assert(staged_info.bss_end == 0x08810020u);
    assert(nk_launch_prepare_session(&session, &game, base) == NK_OK);
    assert(session.staged_executable_checked == true);
    assert(session.staged_executable_info.is_elf == true);
    assert(ends_with(session.staged_executable_path, "EBOOT.BIN"));
    printf("[LAUNCH_TEST] staged data_root=%s memstick=%s\n",
           session.dataroot_path, session.memstick_root);
    assert(strstr(session.dataroot_path, "staged-game") != NULL);
    assert(strstr(session.dataroot_path, "xbdata") != NULL);
    assert(strstr(session.memstick_root, "staged-game") != NULL);
    assert(strstr(session.memstick_root, "memstick") != NULL);

    write_valid_elf(staged_eboot, 0x08811000u, 0x08811000u);
    assert(nk_launch_validate_staged_executable(&game,
                                                nk_title_catalog_find_by_id(game.title_id),
                                                &staged_info, staged_error,
                                                sizeof(staged_error)) != NK_OK);
    write_file(staged_eboot, "not-an-executable");
    assert(nk_launch_validate_staged_executable(&game,
                                                nk_title_catalog_find_by_id(game.title_id),
                                                &staged_info, staged_error,
                                                sizeof(staged_error)) != NK_OK);

    /* A retail-shaped ~PSP container is a recognized source boundary, but it
     * is not reported as an ELF validation result. */
    write_psp_container(staged_eboot);
    assert(nk_launch_validate_staged_executable(&game,
                                                nk_title_catalog_find_by_id(game.title_id),
                                                &staged_info, staged_error,
                                                sizeof(staged_error)) == NK_OK);
    assert(staged_info.is_psp_container == true);
    assert(staged_info.is_elf == false);
    remove(staged_eboot);
    /* `memstick` may have been created by preparation. */
    {
        char memstick[900];
        snprintf(memstick, sizeof(memstick), "%s%cmemstick", staged_root, sep);
        test_rmdir(memstick);
    }
    test_rmdir(staged_root);

    /* 8. A title the catalog does not describe is refused, not launched at a
     * guessed address. */
    printf("[LAUNCH_TEST] Subtest 8: unknown title fails closed\n");
    fflush(stdout);
    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "ZZZZ99999");
    snprintf(game.title_id, sizeof(game.title_id), "not-a-catalog-title");
    assert(nk_launch_prepare_session(&session, &game, base) != NK_OK);

    /* 9. A stale retail-shaped build must not satisfy a second title (#366).
     *
     * The legacy generic candidates probed build/<retail> BEFORE the
     * manifest-selected build/<game_name> and build/<title_id> layouts, so a
     * stale sibling-title binary in the workspace won executable discovery
     * for an unrelated selected title. The stale artifacts below are
     * source-owned synthetic stand-ins, not retail bytes. */
    printf("[LAUNCH_TEST] Subtest 9: stale retail build cannot satisfy second title\n");
    fflush(stdout);
    char root9[700];
    char stale_dir[800];
    char stale_exe[900];
    snprintf(root9, sizeof(root9), "%s%croot9", base, sep);
    snprintf(stale_dir, sizeof(stale_dir), "%s%cbuild%chst", root9, sep, sep);
    assert(nk_platform_mkdir_p(stale_dir));
    snprintf(stale_exe, sizeof(stale_exe), "%s%chst.exe", stale_dir, sep);
    write_file(stale_exe, "MZfake");
    snprintf(stale_exe, sizeof(stale_exe), "%s%chst", stale_dir, sep);
    write_file(stale_exe, "binary");
    snprintf(stale_exe, sizeof(stale_exe), "%s%chst_image.bin", stale_dir, sep);
    write_file(stale_exe, "image");
    snprintf(stale_exe, sizeof(stale_exe), "%s%chst.exe", root9, sep);
    write_file(stale_exe, "MZfake");
    snprintf(stale_exe, sizeof(stale_exe), "%s%chst", root9, sep);
    write_file(stale_exe, "binary");
    snprintf(stale_exe, sizeof(stale_exe), "%s%chst_image.bin", root9, sep);
    write_file(stale_exe, "image");
    {
        char runtime_dir[800];
        snprintf(runtime_dir, sizeof(runtime_dir), "%s%cruntime", root9, sep);
        assert(nk_platform_mkdir_p(runtime_dir));
        snprintf(stale_exe, sizeof(stale_exe), "%s%cruntime%chst_image.bin",
                 root9, sep, sep);
        write_file(stale_exe, "image");
    }

    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00005");
    snprintf(game.title_id, sizeof(game.title_id), "pspdev-phase5-v1");
    assert(nk_launch_prepare_session(&session, &game, root9) != NK_OK);
    assert(strstr(session.last_error, "Runtime binary not found") != NULL);
    assert(strstr(session.last_error, "hst") == NULL);
    assert(nk_launch_runtime_available(root9, "pspdev-phase5-v1") == false);

    /* The second title's OWN runtime (its catalog game_name layout) satisfies
     * it, even while every stale artifact above still exists. */
    {
        const NkTitleEntry *t2 = nk_title_catalog_find_by_id("pspdev-phase5-v1");
        char t2_dir[800];
        char t2_exe[900];
        char t2_img[900];
        assert(t2 != NULL && t2->game_name != NULL);
        snprintf(t2_dir, sizeof(t2_dir), "%s%cbuild%c%s", root9, sep, sep,
                 t2->game_name);
        assert(nk_platform_mkdir_p(t2_dir));
        snprintf(t2_exe, sizeof(t2_exe), "%s%c%s", t2_dir, sep, t2->game_name);
        snprintf(t2_img, sizeof(t2_img), "%s%c%s_image.bin", t2_dir, sep,
                 t2->game_name);
        write_file(t2_exe, "binary");
        write_file(t2_img, "image");
        assert(nk_launch_prepare_session(&session, &game, root9) == NK_OK);
        assert(strstr(session.executable_path, t2->game_name) != NULL);
        assert(strstr(session.executable_path, "build") != NULL);
        assert(strstr(session.image_path, "hst") == NULL);
        assert(strstr(session.image_path, "_image.bin") != NULL);
        assert(nk_launch_runtime_available(root9, "pspdev-phase5-v1") == true);
        remove(t2_exe);
        remove(t2_img);
    }

    /* 10. A root-level retail-shaped binary cannot satisfy any other title,
     * and the generic API accepts only a root DIRECTORY: handing it the exact
     * retail file path does not create an explicit-legacy escape. */
    printf("[LAUNCH_TEST] Subtest 10: root retail binary and file-root escape rejected\n");
    fflush(stdout);
    char root10[700];
    char root10_file[800];
    snprintf(root10, sizeof(root10), "%s%croot10", base, sep);
    assert(nk_platform_mkdir_p(root10));
    snprintf(root10_file, sizeof(root10_file), "%s%chst.exe", root10, sep);
    write_file(root10_file, "MZfake");
    snprintf(root10_file, sizeof(root10_file), "%s%chst", root10, sep);
    write_file(root10_file, "binary");
    snprintf(root10_file, sizeof(root10_file), "%s%chst_image.bin", root10, sep);
    write_file(root10_file, "image");
    snprintf(root10_file, sizeof(root10_file), "%s%chst.exe", root10, sep);

    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00005");
    snprintf(game.title_id, sizeof(game.title_id), "pspdev-phase5-v1");
    assert(nk_launch_prepare_session(&session, &game, root10) != NK_OK);
    assert(strstr(session.last_error, "Runtime binary not found") != NULL);
    assert(strstr(session.last_error, "hst") == NULL);
    assert(nk_launch_runtime_available(root10, "pspdev-phase5-v1") == false);
    /* Explicit file path handed to the generic API: refused, not launched. */
    assert(nk_launch_prepare_session(&session, &game, root10_file) != NK_OK);
    assert(strstr(session.last_error, "Runtime binary not found") != NULL);

    /* 11. Retail-specific image candidates cannot satisfy another title. */
    printf("[LAUNCH_TEST] Subtest 11: retail image cannot satisfy second title\n");
    fflush(stdout);
    char root11[700];
    char t2_dir11[800];
    char t2_exe11[900];
    char plant[900];
    snprintf(root11, sizeof(root11), "%s%croot11", base, sep);
    assert(nk_platform_mkdir_p(root11));
    {
        const NkTitleEntry *t2 = nk_title_catalog_find_by_id("pspdev-phase5-v1");
        assert(t2 != NULL && t2->game_name != NULL);
        snprintf(t2_dir11, sizeof(t2_dir11), "%s%cbuild%c%s", root11, sep, sep,
                 t2->game_name);
        assert(nk_platform_mkdir_p(t2_dir11));
        snprintf(t2_exe11, sizeof(t2_exe11), "%s%c%s.exe", t2_dir11, sep,
                 t2->game_name);
        write_file(t2_exe11, "MZfake");
    }
    snprintf(plant, sizeof(plant), "%s%chst_image.bin", t2_dir11, sep);
    write_file(plant, "image");
    snprintf(plant, sizeof(plant), "%s%chst_image.bin", root11, sep);
    write_file(plant, "image");
    {
        char runtime_dir[800];
        snprintf(runtime_dir, sizeof(runtime_dir), "%s%cruntime", root11, sep);
        assert(nk_platform_mkdir_p(runtime_dir));
        snprintf(plant, sizeof(plant), "%s%cruntime%chst_image.bin", root11,
                 sep, sep);
        write_file(plant, "image");
    }
    {
        char build_hst[800];
        snprintf(build_hst, sizeof(build_hst), "%s%cbuild%chst", root11, sep, sep);
        assert(nk_platform_mkdir_p(build_hst));
        snprintf(plant, sizeof(plant), "%s%chst_image.bin", build_hst, sep);
        write_file(plant, "image");
    }

    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00005");
    snprintf(game.title_id, sizeof(game.title_id), "pspdev-phase5-v1");
    assert(nk_launch_prepare_session(&session, &game, root11) != NK_OK);
    assert(strstr(session.last_error, "Runtime image not found") != NULL);
    assert(strstr(session.last_error, "hst") == NULL);

    /* 12. A session whose disc and title identities disagree is rejected
     * before spawn, even when a runtime exists for either identity. */
    printf("[LAUNCH_TEST] Subtest 12: wrong-title session identity rejected\n");
    fflush(stdout);
    char root12[700];
    snprintf(root12, sizeof(root12), "%s%croot12", base, sep);
    {
        const NkTitleEntry *t2 = nk_title_catalog_find_by_id("pspdev-phase5-v1");
        char dir[800];
        char exe[900];
        char img[900];
        assert(t2 != NULL && t2->game_name != NULL);
        snprintf(dir, sizeof(dir), "%s%cbuild%c%s", root12, sep, sep, t2->game_name);
        assert(nk_platform_mkdir_p(dir));
        snprintf(exe, sizeof(exe), "%s%c%s.exe", dir, sep, t2->game_name);
        snprintf(img, sizeof(img), "%s%c%s_image.bin", dir, sep, t2->game_name);
        write_file(exe, "MZfake");
        write_file(img, "image");
    }
    {
        char dir[800];
        char exe[900];
        char img[900];
        snprintf(dir, sizeof(dir), "%s%cbuild%csynthetic", root12, sep, sep);
        assert(nk_platform_mkdir_p(dir));
        snprintf(exe, sizeof(exe), "%s%csynthetic.exe", dir, sep);
        snprintf(img, sizeof(img), "%s%csynthetic_image.bin", dir, sep);
        write_file(exe, "MZfake");
        write_file(img, "image");
    }

    make_game(&game, iso_path); /* TEST00001 + synthetic-allegrex-v1 */
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00005"); /* title2 disc */
    assert(nk_launch_prepare_session(&session, &game, root12) != NK_OK);
    assert(strstr(session.last_error, "identity") != NULL);
    assert(strstr(session.last_error, "pspdev-phase5-v1") != NULL);
    assert(strstr(session.last_error, "synthetic-allegrex-v1") != NULL);

    /* Half of the identity pair is not a validation: a disc that resolves
     * beside a title id that does not is still a disagreement. */
    snprintf(game.title_id, sizeof(game.title_id), "not-a-real-title");
    assert(nk_launch_prepare_session(&session, &game, root12) != NK_OK);
    assert(strstr(session.last_error, "identity") != NULL);

    /* 13. Ambiguous valid candidates follow the explicit contract order:
     * manager game_name before title id, .exe before the extensionless
     * spelling within one name. */
    printf("[LAUNCH_TEST] Subtest 13: ambiguous candidates are deterministic\n");
    fflush(stdout);
    char root13[700];
    char amb_dir[800];
    char amb_exe[900];
    char amb_img[900];
    char amb_expected[900];
    snprintf(root13, sizeof(root13), "%s%croot13", base, sep);
    snprintf(amb_dir, sizeof(amb_dir), "%s%cbuild%cdisplay-smoke", root13, sep, sep);
    assert(nk_platform_mkdir_p(amb_dir));
    snprintf(amb_exe, sizeof(amb_exe), "%s%cdisplay-smoke.exe", amb_dir, sep);
    snprintf(amb_img, sizeof(amb_img), "%s%cdisplay-smoke_image.bin", amb_dir, sep);
    write_file(amb_exe, "MZfake");
    write_file(amb_img, "image");
    {
        char plain[940];
        char plain_dir[900];
        snprintf(plain, sizeof(plain), "%s%cbuild%cdisplay-smoke%cdisplay-smoke",
                 root13, sep, sep, sep);
        write_file(plain, "binary");
        snprintf(plain_dir, sizeof(plain_dir), "%s%cbuild%cdisplay-smoke-v1",
                 root13, sep, sep);
        assert(nk_platform_mkdir_p(plain_dir));
        snprintf(plain, sizeof(plain), "%s%cdisplay-smoke-v1.exe", plain_dir, sep);
        write_file(plain, "MZfake");
    }
    snprintf(amb_expected, sizeof(amb_expected),
             "display-smoke%cdisplay-smoke.exe", sep);
    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00006");
    snprintf(game.title_id, sizeof(game.title_id), "display-smoke-v1");
    assert(nk_launch_prepare_session(&session, &game, root13) == NK_OK);
    assert(ends_with(session.executable_path, amb_expected));

    /* 14. Moving the repository root never changes the selected identity or
     * its addresses; a root without the title's runtime reports exactly that. */
    printf("[LAUNCH_TEST] Subtest 14: moved root does not alter identity\n");
    fflush(stdout);
    char root14a[700];
    char root14b[700];
    snprintf(root14a, sizeof(root14a), "%s%croot14 a", base, sep);
    snprintf(root14b, sizeof(root14b), "%s%croot14-b", base, sep);
    assert(nk_platform_mkdir_p(root14a));
    assert(nk_platform_mkdir_p(root14b));
    {
        const NkTitleEntry *t2 = nk_title_catalog_find_by_id("pspdev-phase5-v1");
        char dir[800];
        char exe[900];
        char img[900];
        assert(t2 != NULL && t2->game_name != NULL);
        snprintf(dir, sizeof(dir), "%s%cbuild%c%s", root14a, sep, sep, t2->game_name);
        assert(nk_platform_mkdir_p(dir));
        snprintf(exe, sizeof(exe), "%s%c%s", dir, sep, t2->game_name);
        snprintf(img, sizeof(img), "%s%c%s_image.bin", dir, sep, t2->game_name);
        write_file(exe, "binary");
        write_file(img, "image");
    }
    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00005");
    snprintf(game.title_id, sizeof(game.title_id), "pspdev-phase5-v1");
    {
        NkLaunchSession session_a;
        NkLaunchSession session_b;
        uint32_t base_a, entry_a;
        assert(nk_launch_prepare_session(&session_a, &game, root14a) == NK_OK);
        base_a = session_a.base_address;
        entry_a = session_a.entry_point;
        /* Same identity, different (empty) root: identical addresses, and a
         * controlled missing-runtime error rather than a different identity. */
        assert(nk_launch_prepare_session(&session_b, &game, root14b) != NK_OK);
        assert(strstr(session_b.last_error, "Runtime binary not found") != NULL);
        assert(strstr(session_b.last_error, "hst") == NULL);
        assert(nk_launch_runtime_available(root14b, "pspdev-phase5-v1") == false);
        /* Re-root the same session: identity fields and addresses hold. */
        assert(nk_launch_prepare_session(&session, &game, root14a) == NK_OK);
        assert(session.base_address == base_a);
        assert(session.entry_point == entry_a);
        assert(strcmp(session.title_id, "pspdev-phase5-v1") == 0);
        assert(strcmp(session.disc_id, "TEST00005") == 0);
    }

    /* 15. Paths with spaces keep working through the generic route. */
    printf("[LAUNCH_TEST] Subtest 15: spaced repository root\n");
    fflush(stdout);
    char root15[700];
    snprintf(root15, sizeof(root15), "%s%croot with spaces", base, sep);
    {
        const NkTitleEntry *t2 = nk_title_catalog_find_by_id("pspdev-phase5-v1");
        char dir[800];
        char exe[900];
        char img[900];
        assert(t2 != NULL && t2->game_name != NULL);
        snprintf(dir, sizeof(dir), "%s%cbuild%c%s", root15, sep, sep, t2->game_name);
        assert(nk_platform_mkdir_p(dir));
        snprintf(exe, sizeof(exe), "%s%c%s", dir, sep, t2->game_name);
        snprintf(img, sizeof(img), "%s%c%s_image.bin", dir, sep, t2->game_name);
        write_file(exe, "binary");
        write_file(img, "image");
    }
    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "TEST00005");
    snprintf(game.title_id, sizeof(game.title_id), "pspdev-phase5-v1");
    assert(nk_launch_prepare_session(&session, &game, root15) == NK_OK);
    assert(strchr(session.executable_path, ' ') != NULL);

    /* 16. Identity binding is re-checked immediately before spawn: a session
     * whose executable or title identity was swapped after preparation is
     * rejected before any process is created. */
    printf("[LAUNCH_TEST] Subtest 16: pre-spawn identity gate\n");
    fflush(stdout);
    {
        char tampered[720];
        snprintf(tampered, sizeof(tampered), "%s%chst.exe", root15, sep);
        write_file(tampered, "MZfake");

        /* Untampered: passes the identity gate (spawn of a fake fixture may
         * still fail for its own reason, never for identity). */
        make_game(&game, iso_path);
        snprintf(game.disc_id, sizeof(game.disc_id), "TEST00005");
        snprintf(game.title_id, sizeof(game.title_id), "pspdev-phase5-v1");
        assert(nk_launch_prepare_session(&session, &game, root15) == NK_OK);
        (void)nk_launch_start(&session);
        nk_launch_stop(&session);
        assert(strstr(session.last_error, "does not match the selected title") == NULL);
        assert(strstr(session.last_error, "No catalog entry") == NULL);

        /* Tampered executable: rejected before spawn. */
        assert(nk_launch_prepare_session(&session, &game, root15) == NK_OK);
        assert(strlen(tampered) < sizeof(session.executable_path));
        memcpy(session.executable_path, tampered, strlen(tampered) + 1);
        assert(nk_launch_start(&session) != NK_OK);
        assert(session.is_running == false);
        assert(strstr(session.last_error, "does not match the selected title") != NULL);
        nk_launch_stop(&session);

        /* Tampered title identity: rejected before spawn. */
        assert(nk_launch_prepare_session(&session, &game, root15) == NK_OK);
        snprintf(session.title_id, sizeof(session.title_id), "%s",
                 "not-a-catalog-title");
        session.disc_id[0] = '\0';
        assert(nk_launch_start(&session) != NK_OK);
        assert(session.is_running == false);
        assert(strstr(session.last_error, "No catalog entry") != NULL);
        nk_launch_stop(&session);
    }

    /* 17. An explicitly retail-identified title keeps working through the
     * generic route because its OWN validated manifest identifies it: the
     * manifest declares the legacy build name, zero base/entry are its
     * declared launch values, and no generic fallback is involved. Without
     * that validated identity the same session fails closed. */
    printf("[LAUNCH_TEST] Subtest 17: legacy retail-identified route via own identity\n");
    fflush(stdout);
    char root17[700];
    char legacy_manifest[900];
    char legacy_dir[800];
    char legacy_exe[900];
    char legacy_img[900];
    char legacy_error[512];
    snprintf(root17, sizeof(root17), "%s%croot17", base, sep);
    assert(nk_platform_mkdir_p(root17));
    snprintf(legacy_manifest, sizeof(legacy_manifest), "%s%clegacy-retail.json",
             root17, sep);
    const char *legacy_manifest_json =
        "{\"schema_version\":1,\"id\":\"hst-ucus98701-v1\","
        "\"game_name\":\"hst\","
        "\"display_name\":\"Legacy Retail Fixture\",\"kind\":\"retail\","
        "\"disc\":{\"id\":\"UCUS98701\",\"region\":\"NA\","
        "\"revision_policy\":\"exact-disc-id\"},"
        "\"executable\":{\"base\":\"0x00000000\",\"entry\":\"0x00000000\","
        "\"bss_metadata_source\":\"elf\",\"extra_executable_spans\":[]},"
        "\"modules\":[],\"filesystem\":{\"data_root\":\"legacy-data\","
        "\"memory_stick_root\":\"legacy-ms\",\"device_prefixes\":[\"host0:\"]},"
        "\"hle_profile\":\"standard\",\"feature_requirements\":[\"allegrex\"],"
        "\"verification_profile\":\"smoke\"}";
    write_file(legacy_manifest, legacy_manifest_json);
    snprintf(legacy_dir, sizeof(legacy_dir), "%s%cbuild%chst", root17, sep, sep);
    assert(nk_platform_mkdir_p(legacy_dir));
    snprintf(legacy_exe, sizeof(legacy_exe), "%s%chst.exe", legacy_dir, sep);
    snprintf(legacy_img, sizeof(legacy_img), "%s%chst_image.bin", legacy_dir, sep);
    write_file(legacy_exe, "MZfake");
    write_file(legacy_img, "image");

    make_game(&game, iso_path);
    snprintf(game.disc_id, sizeof(game.disc_id), "UCUS98701");
    snprintf(game.title_id, sizeof(game.title_id), "hst-ucus98701-v1");

    /* Without the validated overlay: fail closed, never by default. */
    assert(nk_launch_prepare_session(&session, &game, root17) != NK_OK);
    assert(strstr(session.last_error, "No catalog entry") != NULL);

    /* With its own validated manifest loaded: resolves its own declared
     * layout at its own declared addresses (zero base/entry are the
     * manifest's values, not unknown-address guesses). */
    if (!nk_title_manifest_load_overlay_ext(legacy_manifest, false,
                                            legacy_error, sizeof(legacy_error))) {
        printf("[LAUNCH_TEST] legacy manifest rejected: %s\n", legacy_error);
        fflush(stdout);
        assert(!"legacy fixture manifest must validate");
    }
    assert(nk_launch_prepare_session(&session, &game, root17) == NK_OK);
    assert(session.base_address == 0u);
    assert(session.entry_point == 0u);
    assert(ends_with(session.executable_path, "hst.exe"));
    assert(ends_with(session.image_path, "hst_image.bin"));
    assert(nk_launch_runtime_available(root17, "hst-ucus98701-v1") == true);
    nk_title_catalog_clear_overlay();
    remove(legacy_exe);
    remove(legacy_img);
    remove(legacy_manifest);
    {
        char legacy_ms[900];
        snprintf(legacy_ms, sizeof(legacy_ms), "%s%clegacy-ms", root17, sep);
        test_rmdir(legacy_ms);
    }
    test_rmdir(legacy_dir);
    test_rmdir(root17);

    /* 18. A staged session without an ISO must clear an inherited PSP_ISO
     * when the runtime is actually spawned. The sentinel verifies that the
     * child still receives unrelated parent environment values. */
    test_no_iso_child_environment(argv[0], base, sep);

    printf("[LAUNCH_TEST] ALL LAUNCH RESOLUTION TESTS PASSED!\n");
    return 0;
}
