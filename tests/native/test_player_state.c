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

#include "player_state.h"
#include "iso_reader.h"
#include "nk_font.h"
#include "nk_platform.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void seed_entry(NkGameEntry *entry, const char *disc_id, const char *name) {
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->disc_id, sizeof(entry->disc_id), "%s", disc_id);
    snprintf(entry->title_name, sizeof(entry->title_name), "%s", name);
    snprintf(entry->disc_version, sizeof(entry->disc_version), "1.00");
    entry->status = NK_STATUS_IDENTIFIED;
}

static void write_file(const char *path) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite("fixture", 1, 7, file) == 7);
    assert(fclose(file) == 0);
}

static void write_text_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    size_t length = strlen(text);
    assert(fwrite(text, 1, length, file) == length);
    assert(fclose(file) == 0);
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

static void write_synthetic_mips_elf(const char *path) {
    unsigned char elf[88] = { 0 };
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
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(elf, 1, sizeof(elf), file) == sizeof(elf));
    assert(fclose(file) == 0);
}

static void write_synthetic_pgf(const char *path, uint16_t header_offset, uint16_t header_size,
                                const char magic[4], uint16_t first_glyph, uint16_t last_glyph,
                                size_t total_size) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    uint8_t *buf = (uint8_t *)calloc(1, total_size);
    assert(buf != NULL);
    if (total_size >= 4) {
        buf[0] = (uint8_t)(header_offset & 0xFF);
        buf[1] = (uint8_t)((header_offset >> 8) & 0xFF);
        buf[2] = (uint8_t)(header_size & 0xFF);
        buf[3] = (uint8_t)((header_size >> 8) & 0xFF);
    }
    if (total_size >= (size_t)header_offset + 8u) {
        memcpy(buf + header_offset + 4, magic, 4);
    }
    if (total_size >= (size_t)header_offset + 186u) {
        buf[header_offset + 182] = (uint8_t)(first_glyph & 0xFF);
        buf[header_offset + 183] = (uint8_t)((first_glyph >> 8) & 0xFF);
        buf[header_offset + 184] = (uint8_t)(last_glyph & 0xFF);
        buf[header_offset + 185] = (uint8_t)((last_glyph >> 8) & 0xFF);
    }
    assert(fwrite(buf, 1, total_size, file) == total_size);
    free(buf);
    assert(fclose(file) == 0);
}

static const char *const FIXTURE_SHA256 =
    "f16d05ec6b29248d2c61adb1e9263f78e4f7bace1b955014a2d17872cfe4064d";

static void write_runtime_package_fixture(const char *user_root,
                                          const char *disc_id,
                                          const char *title_id,
                                          uint32_t abi_version,
                                          const char *executable_relative_path,
                                          const char *input_executable_sha256) {
    char packages[768], package_dir[896], executable[1100], image[1100];
    char package_json[16384], report_json[8192], cache_json[4096], cache_key_json[3072];
    char aot_components_json[2048], native_components_json[2048];
    char aot_hash_input[2050], native_hash_input[2050];
    char aot_digest[65], native_digest[65], modules_digest[65];
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
    write_file(executable);
    write_file(image);

    fixture_sha_bytes("[]\n", 3, modules_digest);
    int aot_components_length = snprintf(aot_components_json, sizeof(aot_components_json),
        "{\"analyzer_codegen_epoch\":\"analyzer-codegen-v1\","
        "\"analyzer_sha256\":\"%064d\",\"codegen_options_sha256\":\"%s\","
        "\"codegen_sha256\":\"%064d\",\"executable_sha256\":\"%s\","
        "\"generated_code_abi_epoch\":1,\"manifest_sha256\":\"%064d\","
        "\"modules_sha256\":\"%s\",\"psp_header_sha256\":null,"
        "\"runtime_abi_epoch\":1}",
        0, codegen_options_digest, 0, input_executable_sha256, 0, modules_digest);
    assert(aot_components_length > 0 && (size_t)aot_components_length < sizeof(aot_components_json));
    int native_components_length = snprintf(native_components_json, sizeof(native_components_json),
        "{\"compile_flags\":\"\",\"compiler_identity\":\"gcc-fixture\","
        "\"compiler_target\":\"fixture-target\",\"generated_code_digest\":\"%064d\","
        "\"link_flags\":\"\",\"runtime_abi_epoch\":1,\"runtime_source_digest\":\"%064d\"}",
        0, 0);
    assert(native_components_length > 0 && (size_t)native_components_length < sizeof(native_components_json));
    int aot_hash_length = snprintf(aot_hash_input, sizeof(aot_hash_input), "%s\n", aot_components_json);
    int native_hash_length = snprintf(native_hash_input, sizeof(native_hash_input), "%s\n", native_components_json);
    assert(aot_hash_length > 0 && (size_t)aot_hash_length < sizeof(aot_hash_input));
    assert(native_hash_length > 0 && (size_t)native_hash_length < sizeof(native_hash_input));
    fixture_sha_bytes(aot_hash_input, (size_t)aot_hash_length, aot_digest);
    fixture_sha_bytes(native_hash_input, (size_t)native_hash_length, native_digest);
    int cache_key_length = snprintf(cache_key_json, sizeof(cache_key_json),
        "{\"schema_version\":1,\"aot\":{\"digest\":\"%s\",\"components\":%s},"
        "\"native\":{\"digest\":\"%s\",\"components\":%s}}",
        aot_digest, aot_components_json, native_digest, native_components_json);
    assert(cache_key_length > 0 && (size_t)cache_key_length < sizeof(cache_key_json));
    int cache_length = snprintf(cache_json, sizeof(cache_json),
        "{\"format\":\"nakagawa-aot-cache\",\"schema_version\":1,\"key\":%s,"
        "\"codegen_options\":{},\"runtime_abi_compatibility\":{"
        "\"current_epoch\":1,\"generated_code_reusable\":true}}",
        cache_key_json);
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

    int package_length = snprintf(package_json, sizeof(package_json),
        "{\"format\":\"nakagawa-aot-package\",\"schema_version\":1,\"cache\":%s,"
        "\"title\":{\"id\":\"%s\",\"display_name\":\"Synthetic fixture\","
        "\"kind\":\"retail\",\"manifest_sha256\":\"%064d\","
        "\"protected_digest\":\"%064d\"},"
        "\"inputs\":{\"manifest\":{\"sha256\":\"%064d\"},"
        "\"executable\":{\"sha256\":\"%s\"},\"modules\":[],\"psp_header\":null},"
        "\"runtime\":{\"abi\":\"CpuState\",\"abi_version\":%u,"
        "\"abi_header_sha256\":\"%064d\",\"run_entry\":\"0x00000000\","
        "\"runtime_contract\":null,\"runtime_bindings\":{},"
        "\"required_runtime_bindings\":[]},"
        "\"executable\":{\"path\":\"%s\",\"sha256\":\"%s\","
        "\"guest_entry\":\"0x00000000\"},\"generated_objects\":[],"
        "\"required_local_assets\":[],\"build_report\":\"build-report.json\"}\n",
        cache_json, title_id, 0, 0, 0, input_executable_sha256, (unsigned)abi_version,
        0, executable_relative_path, FIXTURE_SHA256);
    assert(package_length > 0 && (size_t)package_length < sizeof(package_json));
    char package_path[1100];
    snprintf(package_path, sizeof(package_path), "%s%cpackage.json", package_dir,
             nk_platform_path_separator());
    write_text_file(package_path, package_json);

    char package_hash[65], report_hash[65], executable_hash[65], image_hash[65];
    fixture_sha_file(package_path, package_hash);
    fixture_sha_file(report_path, report_hash);
    fixture_sha_file(executable, executable_hash);
    fixture_sha_file(image, image_hash);
    char image_relative[256];
    const char *extension = strrchr(executable_relative_path, '.');
    size_t stem_length = extension && extension != executable_relative_path
        ? (size_t)(extension - executable_relative_path) : strlen(executable_relative_path);
    snprintf(image_relative, sizeof(image_relative), "%.*s_image.bin",
             (int)stem_length, executable_relative_path);
    char completion_json[4096];
    int completion_length = snprintf(completion_json, sizeof(completion_json),
        "{\"format\":\"nakagawa-aot-cache-completion\",\"schema_version\":1,"
        "\"status\":\"complete\",\"cache_key\":%s,\"artifacts\":["
        "{\"path\":\"package.json\",\"sha256\":\"%s\"},"
        "{\"path\":\"build-report.json\",\"sha256\":\"%s\"},"
        "{\"path\":\"%s\",\"sha256\":\"%s\"},"
        "{\"path\":\"%s\",\"sha256\":\"%s\"}]}\n",
        cache_key_json, package_hash, report_hash, executable_relative_path,
        executable_hash, image_relative, image_hash);
    assert(completion_length > 0 && (size_t)completion_length < sizeof(completion_json));
    char completion_path[1100];
    snprintf(completion_path, sizeof(completion_path), "%s%ccompletion-manifest.json",
             package_dir, nk_platform_path_separator());
    write_text_file(completion_path, completion_json);
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

static const char *runtime_package_status_name(NkRuntimePackageStatus status) {
    switch (status) {
        case NK_RUNTIME_PACKAGE_OK: return "OK";
        case NK_RUNTIME_PACKAGE_MISSING: return "MISSING";
        case NK_RUNTIME_PACKAGE_INCOMPATIBLE: return "INCOMPATIBLE";
        case NK_RUNTIME_PACKAGE_STALE: return "STALE";
        default: return "UNKNOWN";
    }
}

int main(int argc, char **argv) {
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
    printf("[PLAYER_STATE_TEST] Subtest 7: demo fixtures include a launchable title\n");
    fflush(stdout);
    PlayerApp *fresh = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(fresh != NULL);
    nk_library_init(&fresh->library);
    player_app_sync_library(fresh);
    assert(fresh->game_count == 0);

    /* Point package discovery at a disposable root. A bare executable/image
       pair must not make the fixture launchable; only a matching v1 package
       enables Play. */
    char cache_dir[512];
    char fixture_root[700];
    char fixture_package_dir[900];
    char fixture_package_json[1100];
    char fixture_report[1100];
    char fixture_exe[1100];
    char fixture_image[1100];
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
    write_runtime_package_fixture(fixture_root, "TEST00006", "display-smoke-v1", 2,
                                  "display-smoke-v1.exe", FIXTURE_SHA256);
    player_app_set_runtime_root(fresh, fixture_root);

    player_app_populate_sample_games(fresh);
    assert(fresh->game_count > 0);
    int disp = player_app_find_game_by_disc_id(fresh, "TEST00006");
    assert(disp >= 0);
    assert(strcmp(fresh->games[disp].title_id, "display-smoke-v1") == 0);
    assert(fresh->games[disp].is_prepared == true);
    assert(fresh->games[disp].status == NK_STATUS_PREPARED);

    int before = fresh->game_count;
    player_app_populate_sample_games(fresh);
    assert(fresh->game_count == before);
    assert(remove(fixture_exe) == 0);
    assert(remove(fixture_image) == 0);
    assert(remove(fixture_report) == 0);
    assert(remove(fixture_package_json) == 0);
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
    settings->settings.fps_cap = 60;
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

    player_app_set_fps_cap(settings, 30);
    assert(settings->settings.fps_cap == 30);
    player_app_set_fps_cap(settings, 999);
    assert(settings->settings.fps_cap == 30);
    player_app_cycle_fps_cap(settings, 1);
    assert(settings->settings.fps_cap == 60);
    player_app_cycle_fps_cap(settings, 1);
    assert(settings->settings.fps_cap == 0);
    player_app_cycle_fps_cap(settings, 1);
    assert(settings->settings.fps_cap == 30);

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

    player_app_adjust_volume(settings, 5);
    assert(settings->settings.master_volume == 85);
    player_app_adjust_volume(settings, 1000);
    assert(settings->settings.master_volume == 100);
    player_app_adjust_volume(settings, -2000);
    assert(settings->settings.master_volume == 0);
    free(settings);

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
        assert(player_app_focus_count(stops) == 2); /* incompatible package: add + remove */

        stops->games[0].is_prepared = true;
        assert(player_app_focus_count(stops) == 2); /* no validated package: add + remove */

        stops->is_game_running = true;
        assert(player_app_focus_count(stops) == 3); /* stop + add + remove */

        /* Overflow adds the two paging stops. */
        stops->is_game_running = false;
        stops->window_width = 640;
        assert(player_app_visible_library_cards(stops) == 2);
        stops->game_count = 1;
        assert(player_app_focus_count(stops) == 2);
        seed_entry(&entry, "FCS00002", "Second");
        stops->games[1] = entry;
        seed_entry(&entry, "FCS00003", "Third");
        stops->games[2] = entry;
        stops->game_count = 3;
        assert(player_app_focus_count(stops) == 4);

        /* A title with missing package offers the BUILD PACKAGE button */
        stops->window_width = 1280;
        stops->game_count = 1;
        seed_entry(&entry, "ULUS10041", "Street Supremacy");
        snprintf(entry.title_id, sizeof(entry.title_id), "ulus-10041");
        stops->games[0] = entry;
        assert(player_app_focus_count(stops) == 3); /* build package + add + remove */

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
        assert(player_app_focus_count(stops) == 14);
        stops->active_view = VIEW_CONTROLLER_SETTINGS;
        assert(player_app_focus_count(stops) == 22);
        stops->input_settings.calib.stage = CALIBRATION_STAGE_REST;
        assert(player_app_focus_count(stops) == 1);
        stops->input_settings.calib.stage = CALIBRATION_STAGE_EXTREMES;
        assert(player_app_focus_count(stops) == 2);
        stops->input_settings.calib.stage = CALIBRATION_STAGE_RESULT;
        assert(player_app_focus_count(stops) == 2);
        stops->input_settings.calib.stage = CALIBRATION_STAGE_INACTIVE;
        assert(player_app_focus_count(stops) == 22);
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
        assert(player_app_focus_count(stops) == 4);
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
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
        snprintf(preflight_root, sizeof(preflight_root), "%s%cplayer-preflight-synthetic",
                 cache_dir, nk_platform_path_separator());
        assert(nk_platform_mkdir_p(preflight_root));
        snprintf(font_dir, sizeof(font_dir), "%s%cfont", preflight_root,
                 nk_platform_path_separator());
        snprintf(font_path, sizeof(font_path), "%s%cjpn0.pgf", font_dir,
                 nk_platform_path_separator());
        const NkTitleEntry *synthetic = nk_title_catalog_find_by_id(
            "synthetic-allegrex-v1");
        assert(synthetic != NULL && synthetic->game_name != NULL &&
               synthetic->primary_disc_id != NULL);
        const char *synthetic_disc_id = synthetic->primary_disc_id;
        char build_dir[760], runtime_exe[900], runtime_image[900];
        char package_dir[900], package_json[1100], package_report[1100];
        snprintf(build_dir, sizeof(build_dir), "%s%cbuild%c%s", preflight_root,
                 nk_platform_path_separator(), nk_platform_path_separator(),
                 synthetic->game_name);
        snprintf(runtime_exe, sizeof(runtime_exe), "%s%c%s.exe", build_dir,
                 nk_platform_path_separator(), synthetic->game_name);
        snprintf(runtime_image, sizeof(runtime_image), "%s%c%s_image.bin", build_dir,
                 nk_platform_path_separator(), synthetic->game_name);
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
        player_app_set_runtime_root(wiz, preflight_root);
        snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id),
                 "%s", synthetic_disc_id);
        snprintf(wiz->inspecting_game.title_id, sizeof(wiz->inspecting_game.title_id),
                 "synthetic-allegrex-v1");
        NkIsoExecutableReport executable_report;
        memset(&executable_report, 0, sizeof(executable_report));
        executable_report.eboot.kind = NK_ISO_EXEC_PSP_ENCRYPTED;
        executable_report.boot.kind = NK_ISO_EXEC_MIPS_ELF32;
        executable_report.selected = NK_ISO_EXEC_SELECTION_BOOT;
        executable_report.boot_fallback = true;
        snprintf(executable_report.selected_path, sizeof(executable_report.selected_path),
                 "BOOT.BIN");
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        assert(wiz->wizard.preflight.count == 5);
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

        assert(nk_platform_mkdir_p(build_dir));
        write_file(runtime_exe);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        write_file(runtime_image);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        write_runtime_package_fixture(preflight_root, synthetic_disc_id,
                                      "synthetic-allegrex-v1", 2,
                                      "synthetic-allegrex-v1.exe", FIXTURE_SHA256);
        assert(nk_platform_mkdir_p(font_dir));
        write_file(font_path);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_OK);
        check = find_preflight_check(&wiz->wizard.preflight, "SYSTEM_FONTS");
        assert(check && check->status == PREFLIGHT_OK);

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
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
        assert(check && check->status == PREFLIGHT_UNSUPPORTED);
        assert(strstr(check->message, "Encrypted executable: supply decrypted modules at ") != NULL);
        assert(strstr(check->message, decrypted_dir) != NULL);
        assert(check->issue_count == 1 && check->issue_numbers[0] == 295);

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
        assert(remove(decrypted_elf) == 0);

        wiz->inspecting_game.is_experimental = true;
        snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id),
                 "ULUS99998");
        snprintf(wiz->inspecting_game.selected_executable,
                 sizeof(wiz->inspecting_game.selected_executable), "EBOOT.BIN");
        snprintf(wiz->inspecting_game.title_id, sizeof(wiz->inspecting_game.title_id),
                 "experimental-ulus99998");
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        assert(wiz->wizard.preflight.count == 6);
        check = find_preflight_check(&wiz->wizard.preflight, "EXPERIMENTAL");
        assert(check && check->status == PREFLIGHT_IN_PROGRESS);
        assert(strstr(check->message,
                      "Experimental: this game has not been verified. Compatibility is unknown.") != NULL);
        assert(check->issue_count == 2 && check->issue_numbers[0] == 285 &&
               check->issue_numbers[1] == 308);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        assert(check->issue_count == 2 && check->issue_numbers[0] == 296 &&
               check->issue_numbers[1] == 297);

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
        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", 2,
                                      "experimental-ulus99998.exe", FIXTURE_SHA256);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_OK);
        remove(experimental_completion);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_STALE);
        assert(strstr(check->message, "#316") != NULL);
        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", 2,
                                      "experimental-ulus99998.exe", FIXTURE_SHA256);

        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", 2,
                                      "experimental-ulus99998.exe",
                                      "0000000000000000000000000000000000000000000000000000000000000000");
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_STALE);

        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", 99,
                                      "experimental-ulus99998.exe", FIXTURE_SHA256);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_INCOMPATIBLE);

        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", 2,
                                      "../escape.exe", FIXTURE_SHA256);
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
        assert(strstr(wiz->last_error.message, "build-package") != NULL);
        player_app_set_view(wiz, VIEW_EXPERIMENTAL_TITLE);
        assert(player_app_focus_count(wiz) == 2);

        remove(runtime_exe);
        remove(runtime_image);
        remove(font_path);

        free(wiz);
    }

    /* 14. Font cache provisioning: validation, manifest checking, and preflight status. */
    printf("[PLAYER_STATE_TEST] Subtest 14: font cache validation and preflight status\n");
    fflush(stdout);
    {
        char cache_test_root[512];
        char cache_dir[640];
        char fonts_v1_dir[768];
        char manifest_path[896];
        char installed_font[896];
        char pgf_valid[800];
        char pgf_trunc[800];
        char pgf_bad_magic[800];
        char pgf_corrupt_glyph[800];
        char err[256];
        char sha[65];
        uint64_t font_size = 0;
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_test_root, sizeof(cache_test_root)));
        snprintf(cache_dir, sizeof(cache_dir), "%s%cplayer_font_test",
                 cache_test_root, nk_platform_path_separator());
        assert(nk_platform_mkdir_p(cache_dir));

        snprintf(pgf_valid, sizeof(pgf_valid), "%s%cvalid.pgf", cache_dir, nk_platform_path_separator());
        snprintf(pgf_trunc, sizeof(pgf_trunc), "%s%ctrunc.pgf", cache_dir, nk_platform_path_separator());
        snprintf(pgf_bad_magic, sizeof(pgf_bad_magic), "%s%cbad_magic.pgf", cache_dir, nk_platform_path_separator());
        snprintf(pgf_corrupt_glyph, sizeof(pgf_corrupt_glyph), "%s%ccorrupt_glyph.pgf", cache_dir, nk_platform_path_separator());

        /* 14a: Structural PGF validation */
        write_synthetic_pgf(pgf_valid, 0, 392, "PGF0", 0, 10, 512);
        assert(nk_font_validate_pgf(pgf_valid, &font_size, sha, err, sizeof(err)) == true);
        assert(font_size == 512);
        assert(strlen(sha) == 64);

        /* Truncated: header declares 392 bytes, file only 100 bytes */
        write_synthetic_pgf(pgf_trunc, 0, 392, "PGF0", 0, 10, 100);
        assert(nk_font_validate_pgf(pgf_trunc, &font_size, sha, err, sizeof(err)) == false);
        assert(strstr(err, "truncated") != NULL);

        /* Bad magic: magic is "BAD0" */
        write_synthetic_pgf(pgf_bad_magic, 0, 392, "BAD0", 0, 10, 512);
        assert(nk_font_validate_pgf(pgf_bad_magic, &font_size, sha, err, sizeof(err)) == false);
        assert(strstr(err, "invalid PGF magic") != NULL);

        /* Corrupt glyphs: first_glyph 20 > last_glyph 10 */
        write_synthetic_pgf(pgf_corrupt_glyph, 0, 392, "PGF0", 20, 10, 512);
        assert(nk_font_validate_pgf(pgf_corrupt_glyph, &font_size, sha, err, sizeof(err)) == false);
        assert(strstr(err, "corrupt glyph indices") != NULL);

        /* 14b: Cache directory and manifest inspection */
        snprintf(fonts_v1_dir, sizeof(fonts_v1_dir), "%s%cfonts%cv1",
                 cache_dir, nk_platform_path_separator(), nk_platform_path_separator());
        assert(nk_platform_mkdir_p(fonts_v1_dir));
        snprintf(manifest_path, sizeof(manifest_path), "%s%cmanifest.json",
                 fonts_v1_dir, nk_platform_path_separator());
        snprintf(installed_font, sizeof(installed_font), "%s%cjpn0.pgf",
                 fonts_v1_dir, nk_platform_path_separator());

        char msg[512];
        /* Without manifest and without fallback: MISSING */
        remove(manifest_path);
        remove(installed_font);
        assert(nk_font_check_cache(cache_dir, NULL, msg, sizeof(msg)) == NK_FONT_STATUS_MISSING);

        /* Install synthetic jpn0.pgf */
        write_synthetic_pgf(installed_font, 0, 392, "PGF0", 0, 10, 512);
        assert(nk_font_validate_pgf(installed_font, &font_size, sha, err, sizeof(err)) == true);

        /* Corrupt manifest: invalid JSON */
        write_text_file(manifest_path, "{ broken json: true ");
        assert(nk_font_check_cache(cache_dir, NULL, msg, sizeof(msg)) == NK_FONT_STATUS_INVALID);

        /* Manifest with hash mismatch */
        char bad_manifest[1024];
        snprintf(bad_manifest, sizeof(bad_manifest),
                 "{\"schema_version\":1,\"files\":{\"jpn0.pgf\":{\"size\":512,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}}}");
        write_text_file(manifest_path, bad_manifest);
        assert(nk_font_check_cache(cache_dir, NULL, msg, sizeof(msg)) == NK_FONT_STATUS_INVALID);

        /* Valid manifest */
        char good_manifest[1024];
        snprintf(good_manifest, sizeof(good_manifest),
                 "{\"schema_version\":1,\"import_time\":12345,\"files\":{\"jpn0.pgf\":{\"size\":512,\"sha256\":\"%s\"}}}",
                 sha);
        write_text_file(manifest_path, good_manifest);
        assert(nk_font_check_cache(cache_dir, NULL, msg, sizeof(msg)) == NK_FONT_STATUS_OK);

        /* 14c: Compatibility preflight with player app */
        PlayerApp *font_app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(font_app != NULL);
        nk_library_init(&font_app->library);
        player_app_set_runtime_root(font_app, cache_dir);
        snprintf(font_app->inspecting_game.disc_id, sizeof(font_app->inspecting_game.disc_id),
                 "UCUS98701");
        snprintf(font_app->inspecting_game.title_id, sizeof(font_app->inspecting_game.title_id),
                 "synthetic-allegrex-v1");
        NkIsoExecutableReport exec_rep;
        memset(&exec_rep, 0, sizeof(exec_rep));
        exec_rep.eboot.kind = NK_ISO_EXEC_PSP_ENCRYPTED;
        exec_rep.boot.kind = NK_ISO_EXEC_MIPS_ELF32;
        exec_rep.selected = NK_ISO_EXEC_SELECTION_BOOT;
        exec_rep.boot_fallback = true;
        snprintf(exec_rep.selected_path, sizeof(exec_rep.selected_path), "BOOT.BIN");

        /* Preflight with valid manifest -> PREFLIGHT_OK */
        player_app_build_compatibility_preflight(font_app, true, true, &exec_rep);
        const PlayerPreflightCheck *fcheck = find_preflight_check(&font_app->wizard.preflight, "SYSTEM_FONTS");
        assert(fcheck != NULL && fcheck->status == PREFLIGHT_OK);

        /* Preflight with corrupt manifest -> PREFLIGHT_INVALID */
        write_text_file(manifest_path, bad_manifest);
        player_app_build_compatibility_preflight(font_app, true, true, &exec_rep);
        fcheck = find_preflight_check(&font_app->wizard.preflight, "SYSTEM_FONTS");
        assert(fcheck != NULL && fcheck->status == PREFLIGHT_INVALID);
        assert(fcheck->issue_count == 1 && fcheck->issue_numbers[0] == 300);

        /* Clean up */
        free(font_app);
        remove(pgf_valid);
        remove(pgf_trunc);
        remove(pgf_bad_magic);
        remove(pgf_corrupt_glyph);
        remove(manifest_path);
        remove(installed_font);
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
        assert(s_app->settings.resolution_scale == 4);
        assert(s_app->settings.fps_cap == 60);
        assert(s_app->settings.vsync == true);
        assert(s_app->settings.fullscreen == false);
        assert(s_app->settings.reduce_motion == false);
        assert(s_app->settings.master_volume == 80);

        /* Mutate all settings and round-trip */
        s_app->settings.resolution_scale = 2;
        s_app->settings.fps_cap = 30;
        s_app->settings.vsync = false;
        s_app->settings.fullscreen = true;
        s_app->settings.reduce_motion = true;
        s_app->settings.master_volume = 55;

        assert(player_app_save_settings(s_app, test_settings_path) == NK_OK);

        PlayerApp *s_app2 = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(s_app2 != NULL);
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_OK);
        assert(s_app2->settings.resolution_scale == 2);
        assert(s_app2->settings.fps_cap == 30);
        assert(s_app2->settings.vsync == false);
        assert(s_app2->settings.fullscreen == true);
        assert(s_app2->settings.reduce_motion == true);
        assert(s_app2->settings.master_volume == 55);
        assert(s_app2->settings_notice[0] == '\0');

        /* Corrupt JSON file resets to defaults and produces notice */
        write_text_file(test_settings_path, "{ invalid_json: [1, 2, ");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_ERROR_GENERIC);
        assert(s_app2->settings.resolution_scale == 4);
        assert(s_app2->settings.fps_cap == 60);
        assert(s_app2->settings.vsync == true);
        assert(s_app2->settings.fullscreen == false);
        assert(s_app2->settings.reduce_motion == false);
        assert(s_app2->settings.master_volume == 80);
        assert(strstr(s_app2->settings_notice, "corrupt") != NULL);

        /* Unknown / unsupported schema version resets to defaults and produces notice */
        write_text_file(test_settings_path, "{\"schema_version\": 999, \"resolution_scale\": 8}");
        assert(player_app_load_settings(s_app2, test_settings_path) == NK_ERROR_GENERIC);
        assert(s_app2->settings.resolution_scale == 4);
        assert(s_app2->settings.fps_cap == 60);
        assert(s_app2->settings.vsync == true);
        assert(s_app2->settings.fullscreen == false);
        assert(s_app2->settings.reduce_motion == false);
        assert(s_app2->settings.master_volume == 80);
        assert(strstr(s_app2->settings_notice, "Unsupported settings schema version") != NULL);

        remove(test_settings_path);
        free(s_app);
        free(s_app2);
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
                                   "Encrypted executable: supply decrypted modules (#295).",
                                   "C:/logs/build_ULUS10041.log");
        assert(bapp->active_view == VIEW_ERROR);
        assert(strcmp(bapp->last_error.error_code, "PACKAGE_BUILD_FAILED") == 0);
        assert(strcmp(bapp->last_error.failed_stage, "preflight") == 0);
        assert(strstr(bapp->last_error.boundary_text, "#295") != NULL);
        assert(strcmp(bapp->last_error.log_file_path, "C:/logs/build_ULUS10041.log") == 0);

        /* Cancellation transitions session */
        bapp->active_view = VIEW_BUILDING_PACKAGE;
        bapp->build_session.is_building = true;
        player_app_cancel_package_build(bapp);
        assert(bapp->build_session.is_cancelled);
        assert(!bapp->build_session.is_building);

        free(bapp);
    }

    free(app);
    printf("[PLAYER_STATE_TEST] ALL PLAYER STATE TESTS PASSED!\n");
    return 0;
}
