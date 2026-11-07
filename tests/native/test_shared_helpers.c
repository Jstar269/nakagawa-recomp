/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#define _POSIX_C_SOURCE 200809L

#include "nk_platform.h"
#include "nk_shared_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif

static int failures;

static unsigned long test_process_id(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

static int remove_test_file(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide_path[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide_path, (int)(sizeof(wide_path) / sizeof(wide_path[0]))) <= 0) {
        return -1;
    }
    return DeleteFileW(wide_path) ? 0 : -1;
#else
    return remove(path);
#endif
}

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        failures++; \
    } \
} while (0)

static void emit_boot_event(FILE *stderr_stream, uint64_t timestamp_ns,
                            const char *format, ...) {
    va_list args;
    va_start(args, format);
    nk_shared_boot_event_vwrite(stderr_stream, timestamp_ns, format, args);
    va_end(args);
}

static int set_boot_event_path(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    return _putenv_s("SR_BOOT_EVENT_FILE", path);
#else
    return setenv("SR_BOOT_EVENT_FILE", path, 1);
#endif
}

static void clear_boot_event_path(void) {
#if defined(_WIN32) || defined(_WIN64)
    (void)_putenv_s("SR_BOOT_EVENT_FILE", "");
#else
    (void)unsetenv("SR_BOOT_EVENT_FILE");
#endif
}

static void test_path_joins(void) {
    char out[64];
    char small[6] = "xxxxx";

    CHECK(nk_shared_path_join(out, sizeof(out), "C:\\root\\", "leaf/sub", '\\',
                              NK_SHARED_PATH_JOIN_ALWAYS, 0u),
          "always-separator join fits");
    CHECK(strcmp(out, "C:\\root\\\\leaf/sub") == 0,
          "always-separator join preserves duplicate separator and suffix bytes");

    CHECK(nk_shared_path_join(out, sizeof(out), "C:/root/", "leaf", '\\',
                              NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR, 0u),
          "if-needed join fits");
    CHECK(strcmp(out, "C:/root/leaf") == 0,
          "if-needed join preserves either existing separator");

    CHECK(nk_shared_path_join(out, sizeof(out), "left/", "right", '/',
                              NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY, 0u),
          "allocated-join mode fits in bounded output");
    CHECK(strcmp(out, "left/right") == 0, "if-both mode avoids duplicate separator");
    CHECK(nk_shared_path_join(out, sizeof(out), "", "leaf", '/',
                              NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY, 0u),
          "empty left side is accepted by if-both mode");
    CHECK(strcmp(out, "leaf") == 0, "if-both mode adds no leading separator");

    CHECK(nk_shared_path_join(out, sizeof(out), "root", "a/b", '\\',
                              NK_SHARED_PATH_JOIN_ALWAYS,
                              NK_SHARED_PATH_NORMALIZE_FORWARD_SLASHES),
          "forward-slash normalization fits");
    CHECK(strcmp(out, "root\\a\\b") == 0, "forward slash suffix is normalized");
    CHECK(nk_shared_path_join(out, sizeof(out), "C:/root//", "folder/name", '\\',
                              NK_SHARED_PATH_JOIN_ALWAYS,
                              NK_SHARED_PATH_TRIM_LEFT_SEPARATORS |
                                  NK_SHARED_PATH_NORMALIZE_BOTH_SLASHES),
          "trim and normalize join fits");
    CHECK(strcmp(out, "C:/root\\folder\\name") == 0,
          "trim mode removes all root separators and normalizes both suffix separators");

    CHECK(!nk_shared_path_join(small, sizeof(small), "root", "part", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS, 0u),
          "ordinary bounded join reports truncation");
    CHECK(strcmp(small, "root/") == 0, "ordinary overflow keeps snprintf-style prefix");
    memset(small, 'z', sizeof(small));
    CHECK(!nk_shared_path_join(small, sizeof(small), "root", "part", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS, NK_SHARED_PATH_REQUIRE_FIT),
          "strict bounded join reports overlong input");
    CHECK(small[0] == 'z' && small[sizeof(small) - 1u] == 'z',
          "strict bounded overflow leaves destination untouched");

    memset(out, 'q', sizeof(out));
    CHECK(!nk_shared_path_join(out, sizeof(out), "root", "C:\\absolute", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS,
                               NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT),
          "absolute right side is rejected when requested");
    CHECK(out[0] == 'q', "rejected right side leaves destination untouched");

    char *allocated = nk_shared_path_join_alloc("folder/", "child", '/',
                                                 NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY, 0u);
    CHECK(allocated != NULL && strcmp(allocated, "folder/child") == 0,
          "allocated join matches if-both bounded semantics");
    free(allocated);
    allocated = nk_shared_path_join_alloc("", "child", '/',
                                           NK_SHARED_PATH_JOIN_ALWAYS, 0u);
    CHECK(allocated != NULL && strcmp(allocated, "/child") == 0,
          "allocated always mode retains leading separator for empty left side");
    free(allocated);
}

/* Pin the distinct legacy contracts of the callers routed through these join modes. */
static void test_legacy_path_contracts(void) {
    char out[64];
    char *allocated;

    CHECK(nk_shared_path_join(out, sizeof(out), "", "leaf", '\\',
                              NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR,
                              NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH),
          "UI join accepts an empty root when the relative path is nonempty");
    CHECK(strcmp(out, "\\leaf") == 0, "UI join keeps the separator after an empty root");
    CHECK(nk_shared_path_join(out, sizeof(out), "", "", '\\',
                              NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR,
                              NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH) &&
              strcmp(out, "\\") == 0,
          "UI join of two empty parts is the lone separator, as the old snprintf wrote");
    CHECK(nk_shared_path_join(out, sizeof(out), "root/", "", '\\',
                              NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR,
                              NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH) &&
              strcmp(out, "root/") == 0,
          "UI join permits an empty relative path when root makes the result nonempty");

    CHECK(nk_shared_path_join(out, sizeof(out), "C:\\root///", "folder/name", '\\',
                              NK_SHARED_PATH_JOIN_ALWAYS,
                              NK_SHARED_PATH_TRIM_LEFT_SEPARATORS),
          "staging path trims root separators without normalizing the suffix");
    CHECK(strcmp(out, "C:\\root\\folder/name") == 0,
          "staging path preserves relative forward slashes");

    allocated = nk_shared_path_join_alloc("left\\", "right", '/',
                                           NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY, 0u);
    CHECK(allocated != NULL && strcmp(allocated, "left\\right") == 0,
          "runtime join preserves either trailing separator form");
    free(allocated);
    allocated = nk_shared_path_join_alloc("left", "", '/',
                                           NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY, 0u);
    CHECK(allocated != NULL && strcmp(allocated, "left") == 0,
          "runtime join adds a separator only when both parts are nonempty");
    free(allocated);
    allocated = nk_shared_path_join_alloc("folder/", "", '/', NK_SHARED_PATH_JOIN_ALWAYS, 0u);
    CHECK(allocated != NULL && strcmp(allocated, "folder//") == 0,
          "package source join always adds a separator, including for an empty right side");
    free(allocated);

    memset(out, 'q', sizeof(out));
    CHECK(!nk_shared_path_join(out, sizeof(out), "root", "/absolute", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS,
                               NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT) && out[0] == 'q',
          "launch join rejects slash-rooted paths without writing output");
    CHECK(!nk_shared_path_join(out, sizeof(out), "root", "\\absolute", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS,
                               NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT) && out[0] == 'q',
          "launch join rejects backslash-rooted paths without writing output");
    CHECK(!nk_shared_path_join(out, sizeof(out), "root", "C:relative", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS,
                               NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT) && out[0] == 'q',
          "launch join rejects drive-prefixed paths without writing output");
}

static void test_text_copy(void) {
    char truncated[4] = { 'x', 'x', 'x', 'x' };
    CHECK(nk_shared_text_copy(truncated, sizeof(truncated), "long text",
                              NK_SHARED_TEXT_COPY_TRUNCATE),
          "truncating text copy accepts overlong input");
    CHECK(strcmp(truncated, "lon") == 0, "truncating text copy terminates at capacity");

    char padded[6];
    memset(padded, 'x', sizeof(padded));
    CHECK(nk_shared_text_copy(padded, sizeof(padded), "ab",
                              NK_SHARED_TEXT_COPY_TRUNCATE),
          "truncating copy accepts short text");
    CHECK(padded[0] == 'a' && padded[1] == 'b' && padded[2] == '\0' &&
              padded[3] == '\0' && padded[4] == '\0' && padded[5] == '\0',
          "truncating copy preserves strncpy NUL padding");
    CHECK(nk_shared_text_copy(padded, sizeof(padded), NULL,
                              NK_SHARED_TEXT_COPY_TRUNCATE) && padded[0] == '\0',
          "truncating copy maps NULL source to empty text");

    char strict[4] = { 's', 'a', 'f', 'e' };
    CHECK(!nk_shared_text_copy(strict, sizeof(strict), "long", NK_SHARED_TEXT_COPY_REQUIRE_FIT),
          "strict copy rejects overlong input");
    CHECK(memcmp(strict, "safe", sizeof(strict)) == 0,
          "strict copy leaves output unchanged when overlong");
    CHECK(nk_shared_text_copy(strict, sizeof(strict), "ok", NK_SHARED_TEXT_COPY_REQUIRE_FIT) &&
              strcmp(strict, "ok") == 0,
          "strict copy writes a fitting string");
}

static void test_utf8_open(void) {
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/shared-helper-%lu-\xC3\xA9.tmp",
                           SR_SELFTEST_BUILD_ROOT, test_process_id());
    CHECK(written > 0 && (size_t)written < sizeof(path), "UTF-8 test path formats");
    if (written <= 0 || (size_t)written >= sizeof(path)) return;

    FILE *file = nk_fopen_utf8(path, "wb");
    CHECK(file != NULL, "UTF-8 open creates a non-ASCII path");
    if (file) {
        CHECK(fputc('U', file) == 'U', "UTF-8 open writes test byte");
        fclose(file);
    }
    file = nk_fopen_utf8(path, "rb");
    CHECK(file != NULL, "UTF-8 open reads the non-ASCII path");
    if (file) {
        CHECK(fgetc(file) == 'U', "UTF-8 open reads written test byte");
        fclose(file);
    }
    CHECK(remove_test_file(path) == 0, "UTF-8 path cleanup succeeds");
    CHECK(nk_fopen_utf8(NULL, "rb") == NULL && nk_fopen_utf8(path, NULL) == NULL,
          "UTF-8 open rejects NULL arguments");
    CHECK(nk_fopen_utf8("", "rb") == NULL,
          "UTF-8 open preserves empty-path failure");

    char overlong[32770];
    memset(overlong, 'x', sizeof(overlong) - 1u);
    overlong[sizeof(overlong) - 1u] = '\0';
    CHECK(nk_fopen_utf8(overlong, "rb") == NULL,
          "UTF-8 open rejects a path longer than the conversion/input limit");
}

static void test_size_format(void) {
    char out[32];
    nk_shared_format_size(out, sizeof(out), 0u);
    CHECK(strcmp(out, "0 bytes") == 0, "sub-MiB size uses byte label");
    nk_shared_format_size(out, sizeof(out), 1024ULL * 1024ULL);
    CHECK(strcmp(out, "1.00 MB") == 0, "MiB threshold formats two decimals");
    nk_shared_format_size(out, sizeof(out), 1024ULL * 1024ULL * 1024ULL);
    CHECK(strcmp(out, "1.00 GB") == 0, "GiB threshold formats two decimals");
    char short_out[5];
    nk_shared_format_size(short_out, sizeof(short_out), 1024ULL * 1024ULL);
    CHECK(strcmp(short_out, "1.00") == 0, "size formatting preserves snprintf truncation");
}

static void test_boot_events(void) {
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/shared-boot-%lu.events",
                           SR_SELFTEST_BUILD_ROOT, test_process_id());
    CHECK(written > 0 && (size_t)written < sizeof(path), "boot event test path formats");
    if (written <= 0 || (size_t)written >= sizeof(path)) return;
    (void)remove_test_file(path);

    FILE *console = tmpfile();
    CHECK(console != NULL, "temporary stderr capture opens");
    if (!console) return;

    clear_boot_event_path();
    emit_boot_event(console, 0u, "event=stderr-only");
    CHECK(set_boot_event_path(path) == 0, "boot event output path is set");
    emit_boot_event(console, 17u, "event=%s", "ready");
    emit_boot_event(console, 0u, "event=untimed");
    char long_message[2049];
    memset(long_message, 'L', sizeof(long_message) - 1u);
    long_message[sizeof(long_message) - 1u] = '\0';
    emit_boot_event(console, 0u, "%s", long_message);
    rewind(console);
    char console_contents[2300];
    size_t console_count = fread(console_contents, 1u, sizeof(console_contents) - 1u, console);
    console_contents[console_count] = '\0';
    CHECK(strstr(console_contents,
                 "event=stderr-only\nevent=ready t_ns=17\nevent=untimed\n") != NULL,
          "boot event writer preserves stderr records and timestamps");
    CHECK(strlen(console_contents) ==
              strlen("event=stderr-only\nevent=ready t_ns=17\nevent=untimed\n") +
                  strlen(long_message) + 1u,
          "boot event writer does not truncate stderr records");
    fclose(console);

    char contents[2300];
    FILE *file = nk_fopen_utf8(path, "rb");
    CHECK(file != NULL, "boot event writer creates append file");
    if (file) {
        size_t count = fread(contents, 1u, sizeof(contents) - 1u, file);
        contents[count] = '\0';
        fclose(file);
        CHECK(strstr(contents, "event=ready t_ns=17\nevent=untimed\n") != NULL,
              "boot event writer preserves timestamp and untimed record formats");
        CHECK(strlen(contents) == strlen("event=ready t_ns=17\nevent=untimed\n") +
                                  strlen(long_message) + 1u,
              "boot event writer does not truncate an overlong record");
    }
    CHECK(remove_test_file(path) == 0, "boot event test file cleanup succeeds");
    clear_boot_event_path();

    char formatted[128];
    int path_length = nk_shared_boot_event_format_path(formatted, sizeof(formatted),
                                                       "cache/", '/', 42ul, 7u);
    CHECK(path_length > 0 && strcmp(formatted, "cache//player-boot-42-7.events") == 0,
          "boot path formatter appends separator without trimming");
    char short_path[8] = "oldpath";
    CHECK(nk_shared_boot_event_format_path(short_path, sizeof(short_path), "long cache", '/',
                                           42ul, 7u) == 0 && short_path[0] == '\0',
          "boot path formatter clears an overlong output");
    CHECK(nk_shared_boot_event_format_path(short_path, sizeof(short_path), NULL, '/',
                                           42ul, 7u) == 0 && short_path[0] == '\0',
          "boot path formatter clears output when the cache directory is unavailable");
}

int main(void) {
    test_path_joins();
    test_legacy_path_contracts();
    test_text_copy();
    test_utf8_open();
    test_size_format();
    test_boot_events();
    if (failures != 0) {
        fprintf(stderr, "shared helper selftest: %d failure(s)\n", failures);
        return 1;
    }
    puts("shared helper selftest passed");
    return 0;
}
