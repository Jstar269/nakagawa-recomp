/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Check for updates (src/player/update_check.c): version order, the release
 * list, checksums, the download's verification and the install folder. Every
 * answer comes from a fake transport; nothing here touches the internet
 * (AGENTS.md section 5). fixtures/update_check/github_releases_20261028.json is
 * the public Releases API answer for this repository, recorded on 2026-10-28
 * with its author and uploader objects removed. */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include "update_check.h"
#include "nk_platform.h"
#include "native_test_isolation.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORDED_ZIP_SHA256 "032afce5b9b4713e6c4fa065c299f72ab732f59967624cffc25484f83516ab9f"
/* The v0.0.1 ZIP's published .sha256 asset, byte for byte. */
static const char k_recorded_checksum[] =
    RECORDED_ZIP_SHA256 "  nakagawa-recomp-0.0.1-windows-x64.zip\r\n";

/* ---- A fake transport: each URL answers with fixed bytes ------------------------ */

typedef struct {
    const char *url;
    const char *final_url;   /* NULL: the URL itself */
    const unsigned char *body;
    size_t size;
    bool reachable;
} FakeAnswer;

typedef struct {
    FakeAnswer answers[4];
    size_t answer_count;
    char requested[8][512];
    char hosts[8][128];
    size_t request_count;
    const FakeAnswer *open;
    size_t offset;
    int closes;
} FakeNet;

static bool fake_read(void *context, unsigned char *buffer, size_t capacity, size_t *read) {
    FakeNet *net = (FakeNet *)context;
    size_t left = net->open->size - net->offset;
    size_t count = left < capacity ? left : capacity;
    memcpy(buffer, net->open->body + net->offset, count);
    net->offset += count;
    *read = count;
    return true;
}

static void fake_close(void *context) {
    ((FakeNet *)context)->closes++;
}

static bool fake_open(void *context, const char *url, const char *allowed_hosts,
                      PackageHttpResponse *response) {
    FakeNet *net = (FakeNet *)context;
    assert(net->request_count < 8);
    snprintf(net->requested[net->request_count], sizeof(net->requested[0]), "%s", url);
    snprintf(net->hosts[net->request_count], sizeof(net->hosts[0]), "%s", allowed_hosts);
    net->request_count++;
    for (size_t i = 0; i < net->answer_count; i++) {
        const FakeAnswer *answer = &net->answers[i];
        if (strcmp(answer->url, url) != 0) continue;
        if (!answer->reachable) return false;
        net->open = answer;
        net->offset = 0;
        memset(response, 0, sizeof(*response));
        response->final_url = answer->final_url ? answer->final_url : answer->url;
        response->has_content_length = true;
        response->content_length = answer->size;
        response->context = net;
        response->read = fake_read;
        response->close = fake_close;
        return true;
    }
    return false;
}

static void add_answer(FakeNet *net, const char *url, const void *body, size_t size) {
    FakeAnswer *answer = &net->answers[net->answer_count++];
    memset(answer, 0, sizeof(*answer));
    answer->url = url;
    answer->body = (const unsigned char *)body;
    answer->size = size;
    answer->reachable = true;
}

static char *read_fixture(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *data;
    long size;
    assert(file && "run from the repository root");
    assert(fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    assert(size > 0);
    rewind(file);
    data = (char *)malloc((size_t)size + 1);
    assert(data);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    data[size] = '\0';
    *length = (size_t)size;
    return data;
}

/* ---- Versions -------------------------------------------------------------------- */

static int compare(const char *a, const char *b) {
    NkSemver va;
    NkSemver vb;
    assert(nk_semver_parse(a, &va));
    assert(nk_semver_parse(b, &vb));
    return nk_semver_compare(&va, &vb);
}

static void test_versions(void) {
    /* The precedence list of SemVer 2.0.0 item 11, then this project's versions. */
    static const char *const ascending[] = {
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
        "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0"
    };
    static const char *const invalid[] = {
        "", "v", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.3-", "1.2.3-01", "1.2.3-a..b",
        "1.2.3+", "1.2.3 ", "V1.2.3", "1.2.3-a_b", "99999999999.0.0", "vv1.2.3"
    };
    NkSemver version;
    size_t count = sizeof(ascending) / sizeof(ascending[0]);
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < count; j++) {
            int c = compare(ascending[i], ascending[j]);
            assert(i < j ? c < 0 : i > j ? c > 0 : c == 0);
        }
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(!nk_semver_parse(invalid[i], &version));
    }
    assert(nk_semver_parse("v0.0.2-rc.1+build.7", &version));
    assert(version.major == 0 && version.minor == 0 && version.patch == 2);
    assert(strcmp(version.prerelease, "rc.1") == 0);
    assert(compare("v0.0.1", "0.0.1") == 0);
    assert(compare("0.0.1+a", "0.0.1+b") == 0);
    /* The running "0.0.2-dev" is newer than the v0.0.1 release and older than
       the v0.0.2 release and its release candidates. */
    assert(compare("0.0.1", "0.0.2-dev") < 0);
    assert(compare("0.0.2-dev", "0.0.2") < 0);
    assert(compare("0.0.2-dev", "0.0.2-rc.1") < 0);
    assert(compare("0.0.10", "0.0.9") > 0);
    printf("[UPDATE_CHECK_TEST] versions: PASS\n");
}

/* ---- The recorded release list --------------------------------------------------- */

static void test_recorded_release_list(void) {
    size_t length = 0;
    char *json = read_fixture("fixtures/update_check/github_releases_20261028.json", &length);
    NkUpdateRelease release;
    NkUpdateError error;
    assert(nk_update_parse_releases(json, length, &release, &error));
    assert(strcmp(release.tag, "v0.0.1") == 0);
    assert(strcmp(release.version, "0.0.1") == 0);
    assert(release.prerelease);
    assert(strcmp(release.page_url,
                  "https://github.com/Jstar269/nakagawa-recomp/releases/tag/v0.0.1") == 0);
    assert(strcmp(release.published, "2026-10-28") == 0);
    assert(release.has_zip && release.has_checksum);
    assert(strcmp(release.zip_name, "nakagawa-recomp-0.0.1-windows-x64.zip") == 0);
    assert(strcmp(release.zip_url, "https://github.com/Jstar269/nakagawa-recomp/releases/"
                                   "download/v0.0.1/nakagawa-recomp-0.0.1-windows-x64.zip") == 0);
    assert(release.zip_size == 284758051u);
    assert(strcmp(release.zip_digest, RECORDED_ZIP_SHA256) == 0);
    assert(release.checksum_size == 105u);
    /* The notes are the first two paragraphs as plain text: the quoted notice
       without its [!IMPORTANT] marker, bold or link, then the first paragraph. */
    {
        static const char start[] = "This is the first public preview, and it will be buggy. "
                                    "Read the known issues below and the full release notes "
                                    "before you start, and please tell us what breaks. "
                                    "Nakagawa Recomp turns the program code";
        static const char end[] = "Your games, saves and fonts stay on your PC.";
        size_t notes_length = strlen(release.notes);
        assert(strncmp(release.notes, start, sizeof(start) - 1) == 0);
        assert(notes_length > sizeof(end) &&
               strcmp(release.notes + notes_length - (sizeof(end) - 1), end) == 0);
        assert(!strstr(release.notes, "**") && !strstr(release.notes, "](") &&
               !strstr(release.notes, "[!") && !strstr(release.notes, "> ") &&
               !strstr(release.notes, "Highlights"));
    }

    /* The check itself: one anonymous request to the API host, and no other. */
    {
        static const char *const versions[] = { "0.0.2-dev", "0.0.1", "0.0.1-rc.1", "0.0.0" };
        static const bool newer[] = { false, false, true, true };
        for (size_t i = 0; i < 4; i++) {
            FakeNet net;
            NkUpdateCheckResult result;
            memset(&net, 0, sizeof(net));
            add_answer(&net, NK_UPDATE_RELEASES_URL, json, length);
            assert(nk_update_check(fake_open, &net, versions[i], &result, &error));
            assert(result.newer == newer[i]);
            assert(strcmp(result.newest.tag, "v0.0.1") == 0);
            assert(net.request_count == 1 && net.closes == 1);
            assert(strcmp(net.requested[0], "https://api.github.com/repos/Jstar269/"
                                            "nakagawa-recomp/releases?per_page=10") == 0);
            assert(strcmp(net.hosts[0], "api.github.com") == 0);
        }
    }
    free(json);
    printf("[UPDATE_CHECK_TEST] recorded release list: PASS\n");
}

/* ---- Synthetic release lists ----------------------------------------------------- */

#define PAGE "https://github.com/Jstar269/nakagawa-recomp/releases/tag/"
#define DOWNLOAD "https://github.com/Jstar269/nakagawa-recomp/releases/download/"

static bool parse(const char *json, NkUpdateRelease *release, NkUpdateError *error) {
    return nk_update_parse_releases(json, strlen(json), release, error);
}

static void test_synthetic_release_lists(void) {
    NkUpdateRelease release;
    NkUpdateError error;
    /* The newest by version wins, whatever the order; drafts and tags that are
       not versions are skipped. */
    assert(parse("[{\"tag_name\":\"v0.0.1\",\"html_url\":\"" PAGE "v0.0.1\",\"assets\":[]},"
                 "{\"tag_name\":\"v0.0.3\",\"draft\":true,\"html_url\":\"" PAGE "v0.0.3\"},"
                 "{\"tag_name\":\"nightly\",\"html_url\":\"https://example.com/\"},"
                 "{\"tag_name\":\"v0.0.2-rc.1\",\"prerelease\":true,\"html_url\":\"" PAGE
                 "v0.0.2-rc.1\",\"body\":\"## Notes\\n\\nSecond **candidate**.\","
                 "\"assets\":[{\"name\":\"nakagawa-recomp-0.0.2-rc.1-windows-x64.zip\","
                 "\"state\":\"uploaded\",\"size\":10,\"browser_download_url\":\"" DOWNLOAD
                 "v0.0.2-rc.1/nakagawa-recomp-0.0.2-rc.1-windows-x64.zip\"}]}]",
                 &release, &error));
    assert(strcmp(release.tag, "v0.0.2-rc.1") == 0 && release.prerelease);
    assert(release.has_zip && !release.has_checksum && release.zip_digest[0] == '\0');
    assert(strcmp(release.notes, "Second candidate.") == 0);
    /* A release with no Windows ZIP is still the newest release. */
    assert(parse("[{\"tag_name\":\"v1.0.0\",\"html_url\":\"" PAGE "v1.0.0\"}]", &release, &error));
    assert(!release.has_zip && strcmp(release.version, "1.0.0") == 0);
    /* Pages and files outside this repository fail closed. */
    assert(!parse("[{\"tag_name\":\"v1.0.0\",\"html_url\":\"https://github.com/other/"
                  "nakagawa-recomp/releases/tag/v1.0.0\"}]", &release, &error));
    assert(strcmp(error.code, "UPDATE_ANSWER_UNEXPECTED") == 0 && release.tag[0] == '\0');
    assert(!parse("[{\"tag_name\":\"v1.0.0\",\"html_url\":\"" PAGE "v1.0.0\",\"assets\":"
                  "[{\"name\":\"nakagawa-recomp-1.0.0-windows-x64.zip\",\"size\":10,"
                  "\"browser_download_url\":\"https://example.com/nakagawa-recomp-1.0.0-"
                  "windows-x64.zip\"}]}]", &release, &error));
    assert(strcmp(error.code, "UPDATE_ANSWER_UNEXPECTED") == 0);
    assert(!parse("[{\"tag_name\":\"v1.0.0\",\"html_url\":\"" PAGE "v1.0.0\",\"assets\":"
                  "[{\"name\":\"nakagawa-recomp-1.0.0-windows-x64.zip.sha256\",\"size\":99999,"
                  "\"browser_download_url\":\"" DOWNLOAD "v1.0.0/nakagawa-recomp-1.0.0-"
                  "windows-x64.zip.sha256\"}]}]", &release, &error));
    assert(strcmp(error.code, "UPDATE_ANSWER_UNEXPECTED") == 0);
    assert(!parse("[]", &release, &error) && strcmp(error.code, "UPDATE_NO_RELEASES") == 0);
    assert(!parse("{\"message\":\"API rate limit exceeded\"}", &release, &error));
    assert(strcmp(error.code, "UPDATE_ANSWER_UNREADABLE") == 0);
    assert(!parse("not json", &release, &error));
    assert(strcmp(error.code, "UPDATE_ANSWER_UNREADABLE") == 0);
    printf("[UPDATE_CHECK_TEST] synthetic release lists: PASS\n");
}

static void test_short_notes(void) {
    char notes[64];
    nk_update_short_notes("# Title\n\n> [!NOTE]\n> **Bold** and `code` with a "
                          "[link](https://example.com) ![img](x.png).\n\n- one\n- two\n\nThird.",
                          notes, sizeof(notes));
    assert(strcmp(notes, "Bold and code with a link . one two") == 0);
    nk_update_short_notes("A long sentence that is cut at a word boundary before the end of "
                          "the room.", notes, 32);
    assert(strcmp(notes, "A long sentence that is cut...") == 0);
    nk_update_short_notes(NULL, notes, sizeof(notes));
    assert(notes[0] == '\0');
    printf("[UPDATE_CHECK_TEST] short notes: PASS\n");
}

/* ---- Checksums ------------------------------------------------------------------- */

static void test_checksums(void) {
    NkUpdateRelease release;
    NkUpdateError error;
    char sha[65];
    const char *name = "nakagawa-recomp-0.0.1-windows-x64.zip";
    memset(&release, 0, sizeof(release));
    snprintf(release.zip_name, sizeof(release.zip_name), "%s", name);
    release.has_zip = true;
    /* Good: the recorded file, also with an upper-case digest, a byte-order mark,
       a binary-mode star and a bare LF. */
    release.has_checksum = true;
    assert(nk_update_expected_sha256(&release, k_recorded_checksum, strlen(k_recorded_checksum),
                                     sha, &error));
    assert(strcmp(sha, RECORDED_ZIP_SHA256) == 0);
    {
        static const char variant[] = "\xEF\xBB\xBF" "032AFCE5B9B4713E6C4FA065C299F72AB732F599"
                                      "67624CFFC25484F83516AB9F *nakagawa-recomp-0.0.1-"
                                      "windows-x64.zip\n";
        assert(nk_update_parse_checksum(variant, strlen(variant), name, sha, &error));
        assert(strcmp(sha, RECORDED_ZIP_SHA256) == 0);
    }
    /* GitHub's digest agrees: good. */
    snprintf(release.zip_digest, sizeof(release.zip_digest), "%s", RECORDED_ZIP_SHA256);
    assert(nk_update_expected_sha256(&release, k_recorded_checksum, strlen(k_recorded_checksum),
                                     sha, &error));
    /* Bad: GitHub's digest disagrees with the checksum file. */
    release.zip_digest[0] = 'f';
    assert(!nk_update_expected_sha256(&release, k_recorded_checksum,
                                      strlen(k_recorded_checksum), sha, &error));
    assert(strcmp(error.code, "UPDATE_CHECKSUM_CONFLICT") == 0 && sha[0] == '\0');
    release.zip_digest[0] = '\0';
    /* Bad: another file's checksum, two files, a short digest, no name. */
    {
        static const char *const bad[] = {
            RECORDED_ZIP_SHA256 "  nakagawa-recomp-0.0.2-windows-x64.zip\n",
            RECORDED_ZIP_SHA256 "  nakagawa-recomp-0.0.1-windows-x64.zip\n"
            RECORDED_ZIP_SHA256 "  other.zip\n",
            "032afce5  nakagawa-recomp-0.0.1-windows-x64.zip\n",
            RECORDED_ZIP_SHA256 "\n",
            RECORDED_ZIP_SHA256 "nakagawa-recomp-0.0.1-windows-x64.zip\n",
            "",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            assert(!nk_update_expected_sha256(&release, bad[i], strlen(bad[i]), sha, &error));
            assert(strcmp(error.code, "UPDATE_CHECKSUM_INVALID") == 0 && sha[0] == '\0');
        }
    }
    /* Missing: no checksum file in the release. */
    release.has_checksum = false;
    assert(!nk_update_expected_sha256(&release, NULL, 0, sha, &error));
    assert(strcmp(error.code, "UPDATE_CHECKSUM_MISSING") == 0);
    printf("[UPDATE_CHECK_TEST] checksums: PASS\n");
}

/* ---- The network answers --------------------------------------------------------- */

static void test_check_failures(void) {
    FakeNet net;
    NkUpdateCheckResult result;
    NkUpdateError error;
    static const char big[] = "[]";
    /* Offline: the transport cannot connect. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, NK_UPDATE_RELEASES_URL, big, 2);
    net.answers[0].reachable = false;
    assert(!nk_update_check(fake_open, &net, "0.0.1", &result, &error));
    assert(strcmp(error.code, "UPDATE_OFFLINE") == 0);
    assert(strcmp(error.title, "Couldn't Reach GitHub") == 0);
    assert(net.request_count == 1);   /* no retry */
    /* A redirect to another host. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, NK_UPDATE_RELEASES_URL, big, 2);
    net.answers[0].final_url = "https://example.com/releases";
    assert(!nk_update_check(fake_open, &net, "0.0.1", &result, &error));
    assert(strcmp(error.code, "UPDATE_REDIRECT_REJECTED") == 0 && net.closes == 1);
    /* An answer larger than any release list. */
    memset(&net, 0, sizeof(net));
    {
        size_t size = NK_UPDATE_MAX_RELEASES_BYTES + 1u;
        unsigned char *huge = (unsigned char *)calloc(size, 1);
        assert(huge);
        add_answer(&net, NK_UPDATE_RELEASES_URL, huge, size);
        assert(!nk_update_check(fake_open, &net, "0.0.1", &result, &error));
        assert(strcmp(error.code, "UPDATE_ANSWER_TOO_LARGE") == 0 && net.closes == 1);
        free(huge);
    }
    /* A build without a comparable version asks nothing. */
    memset(&net, 0, sizeof(net));
    assert(!nk_update_check(fake_open, &net, "dev", &result, &error));
    assert(strcmp(error.code, "UPDATE_VERSION_UNKNOWN") == 0 && net.request_count == 0);
    printf("[UPDATE_CHECK_TEST] check failures: PASS\n");
}

static NkUpdateRelease synthetic_release(void) {
    NkUpdateRelease release;
    memset(&release, 0, sizeof(release));
    snprintf(release.tag, sizeof(release.tag), "v0.0.2");
    snprintf(release.version, sizeof(release.version), "0.0.2");
    snprintf(release.zip_name, sizeof(release.zip_name), "nakagawa-recomp-0.0.2-windows-x64.zip");
    snprintf(release.zip_url, sizeof(release.zip_url),
             DOWNLOAD "v0.0.2/nakagawa-recomp-0.0.2-windows-x64.zip");
    snprintf(release.checksum_url, sizeof(release.checksum_url),
             DOWNLOAD "v0.0.2/nakagawa-recomp-0.0.2-windows-x64.zip.sha256");
    release.has_zip = true;
    release.has_checksum = true;
    release.zip_size = 5;
    release.checksum_size = 105;
    return release;
}

static void test_download(void) {
    /* SHA-256 of "hello". */
    static const char good[] = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
                               "  nakagawa-recomp-0.0.2-windows-x64.zip\r\n";
    static const char wrong[] = "0000000000000000000000000000000000000000000000000000000000000000"
                                "  nakagawa-recomp-0.0.2-windows-x64.zip\r\n";
    NkUpdateRelease release = synthetic_release();
    NkUpdateError error;
    char downloads[NATIVE_TEST_PATH_MAX];
    char zip[NATIVE_TEST_PATH_MAX + 64];
    char expected_zip[NATIVE_TEST_PATH_MAX + 192];
    FakeNet net;
    snprintf(downloads, sizeof(downloads), "%s%cdownloads", native_test_get_root(),
             nk_platform_path_separator());
    snprintf(expected_zip, sizeof(expected_zip), "%s%c%s", downloads,
             nk_platform_path_separator(), release.zip_name);

    /* Missing checksum file: nothing is requested at all. */
    memset(&net, 0, sizeof(net));
    release.has_checksum = false;
    assert(!nk_update_download(fake_open, &net, &release, downloads, NULL, NULL, zip,
                               sizeof(zip), &error));
    assert(strcmp(error.code, "UPDATE_CHECKSUM_MISSING") == 0 && net.request_count == 0);
    assert(zip[0] == '\0' && !nk_platform_file_exists(expected_zip));
    release.has_checksum = true;

    /* Good: the checksum file first, then the ZIP from the download hosts. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, release.checksum_url, good, strlen(good));
    add_answer(&net, release.zip_url, "hello", 5);
    net.answers[1].final_url = "https://release-assets.githubusercontent.com/asset";
    {
        bool ok = nk_update_download(fake_open, &net, &release, downloads, NULL, NULL, zip,
                                     sizeof(zip), &error);
        assert(net.request_count == 2);
        assert(strcmp(net.requested[0], release.checksum_url) == 0);
        assert(strcmp(net.requested[1], release.zip_url) == 0);
        assert(strcmp(net.hosts[0], "github.com;release-assets.githubusercontent.com") == 0);
        assert(strcmp(net.hosts[1], "github.com;release-assets.githubusercontent.com") == 0);
#if defined(_WIN32) || defined(_WIN64)
        assert(ok && strcmp(zip, expected_zip) == 0);
        {
            char saved[8] = { 0 };
            FILE *file = nk_fopen_utf8(zip, "rb");
            assert(file && fread(saved, 1, sizeof(saved), file) == 5);
            fclose(file);
            assert(memcmp(saved, "hello", 5) == 0);
        }
#else
        /* Linux has no native transport or SHA-256 check yet: the download is
           refused and leaves nothing behind. */
        assert(!ok && strcmp(error.code, "UPDATE_PLATFORM_UNSUPPORTED") == 0);
        assert(!nk_platform_file_exists(expected_zip));
#endif
    }

    /* Bad: the body doesn't match the published checksum. The earlier download of
       the same version is replaced, never trusted, and nothing is left. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, release.checksum_url, wrong, strlen(wrong));
    add_answer(&net, release.zip_url, "hello", 5);
    assert(!nk_update_download(fake_open, &net, &release, downloads, NULL, NULL, zip,
                               sizeof(zip), &error));
#if defined(_WIN32) || defined(_WIN64)
    assert(strcmp(error.code, "UPDATE_HASH_MISMATCH") == 0);
#else
    assert(strcmp(error.code, "UPDATE_PLATFORM_UNSUPPORTED") == 0);
#endif
    assert(zip[0] == '\0' && !nk_platform_file_exists(expected_zip));
    {
        char partial[NATIVE_TEST_PATH_MAX + 200];
        snprintf(partial, sizeof(partial), "%s.part", expected_zip);
        assert(!nk_platform_file_exists(partial));
    }

    /* The checksum file redirected off GitHub: stop before the ZIP. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, release.checksum_url, good, strlen(good));
    net.answers[0].final_url = "https://example.com/sums";
    assert(!nk_update_download(fake_open, &net, &release, downloads, NULL, NULL, zip,
                               sizeof(zip), &error));
    assert(strcmp(error.code, "UPDATE_REDIRECT_REJECTED") == 0 && net.request_count == 1);

    /* Offline at the checksum: the clear offline card. */
    memset(&net, 0, sizeof(net));
    assert(!nk_update_download(fake_open, &net, &release, downloads, NULL, NULL, zip,
                               sizeof(zip), &error));
    assert(strcmp(error.code, "UPDATE_OFFLINE") == 0);
    printf("[UPDATE_CHECK_TEST] download verification: PASS\n");
}

/* ---- The install folder ---------------------------------------------------------- */

static void write_file(const char *path) {
    FILE *file = nk_fopen_utf8(path, "wb");
    assert(file);
    fputs("synthetic\n", file);
    fclose(file);
}

static void test_install_folder(void) {
    char sep = nk_platform_path_separator();
    char games[NATIVE_TEST_PATH_MAX + 8];
    char install[NATIVE_TEST_PATH_MAX + 48];
    char bin[NATIVE_TEST_PATH_MAX + 56];
    char marker[NATIVE_TEST_PATH_MAX + 72];
    char downloads[NATIVE_TEST_PATH_MAX + 24];
    char expected[NATIVE_TEST_PATH_MAX + 72];
    char out[NK_MAX_PATH];
    const char *zip = "nakagawa-recomp-0.0.2-windows-x64.zip";
    snprintf(games, sizeof(games), "%s%cgames", native_test_get_root(), sep);
    snprintf(install, sizeof(install), "%s%cnakagawa-recomp-0.0.1-windows-x64", games, sep);
    snprintf(bin, sizeof(bin), "%s%cbin%c", install, sep, sep);
    snprintf(marker, sizeof(marker), "%s%cSTART HERE.txt", install, sep);
    snprintf(downloads, sizeof(downloads), "%s%cdata%cdownloads", native_test_get_root(), sep, sep);
    assert(nk_platform_mkdir_p(bin));

    /* Not a release folder yet (no START HERE.txt): the downloads folder. */
    snprintf(expected, sizeof(expected), "%s%cnakagawa-recomp-0.0.2-windows-x64", downloads, sep);
    assert(nk_update_install_folder(bin, downloads, zip, out, sizeof(out)));
    assert(strcmp(out, expected) == 0);
    /* A release ZIP's bin folder: beside the running version. */
    write_file(marker);
    snprintf(expected, sizeof(expected), "%s%cnakagawa-recomp-0.0.2-windows-x64", games, sep);
    assert(nk_update_install_folder(bin, downloads, zip, out, sizeof(out)));
    assert(strcmp(out, expected) == 0);
    bin[strlen(bin) - 1] = '\0';   /* without the trailing separator */
    assert(nk_update_install_folder(bin, downloads, zip, out, sizeof(out)));
    assert(strcmp(out, expected) == 0);
    /* Any other folder (a developer build): the downloads folder. */
    snprintf(expected, sizeof(expected), "%s%cnakagawa-recomp-0.0.2-windows-x64", downloads, sep);
    assert(nk_update_install_folder(install, downloads, zip, out, sizeof(out)));
    assert(strcmp(out, expected) == 0);
    assert(nk_update_install_folder(NULL, downloads, zip, out, sizeof(out)));
    assert(strcmp(out, expected) == 0);
    /* Only a plain ZIP name. */
    assert(!nk_update_install_folder(bin, downloads, "x.tar", out, sizeof(out)));
    assert(!nk_update_install_folder(bin, downloads, "../x.zip", out, sizeof(out)));
    assert(!nk_update_install_folder(bin, downloads, ".zip", out, sizeof(out)));

    {
        NkUpdateError error;
        char log_path[NATIVE_TEST_PATH_MAX + 16];
        snprintf(log_path, sizeof(log_path), "%s%cinstall.log", native_test_get_root(), sep);
#if defined(_WIN32) || defined(_WIN64)
        /* An existing folder is never written into. */
        assert(!nk_update_install("missing.zip", install, log_path, &error));
        assert(strcmp(error.code, "UPDATE_INSTALL_EXISTS") == 0);
        assert(nk_platform_file_exists(marker));
        /* The launcher offers the download exactly where install works. */
        assert(nk_update_install_supported());
#else
        assert(!nk_update_install("missing.zip", install, log_path, &error));
        assert(strcmp(error.code, "UPDATE_PLATFORM_UNSUPPORTED") == 0);
        assert(!nk_update_install_supported());
#endif
    }
    printf("[UPDATE_CHECK_TEST] install folder: PASS\n");
}

int main(void) {
    native_test_create_root("update-check");
    native_test_isolate_user_data_roots();
    test_versions();
    test_recorded_release_list();
    test_synthetic_release_lists();
    test_short_notes();
    test_checksums();
    test_check_failures();
    test_download();
    test_install_folder();
    printf("[UPDATE_CHECK_TEST] all: PASS\n");
    return 0;
}
