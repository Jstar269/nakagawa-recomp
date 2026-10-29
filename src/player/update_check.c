/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Check for updates: see update_check.h for what it does and does not do. */

#if !defined(_WIN32) && !defined(_WIN64) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "update_check.h"
#include "nk_json.h"
#include "nk_launch.h"
#include "nk_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

static void set_error(NkUpdateError *error, const char *code, const char *title,
                      const char *message) {
    if (!error) return;
    snprintf(error->code, sizeof(error->code), "%s", code);
    snprintf(error->title, sizeof(error->title), "%s", title);
    snprintf(error->message, sizeof(error->message), "%s", message);
}

static void clear_error(NkUpdateError *error) {
    if (error) memset(error, 0, sizeof(*error));
}

/* ---- Versions ------------------------------------------------------------------- */

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_identifier_char(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

/* One numeric part with no leading zero, below 2^31. */
static bool parse_number(const char **cursor, unsigned long *out) {
    const char *p = *cursor;
    unsigned long value = 0;
    if (!is_digit(*p)) return false;
    if (*p == '0' && is_digit(p[1])) return false;
    while (is_digit(*p)) {
        unsigned long digit = (unsigned long)(*p - '0');
        if (value > (0x7FFFFFFFul - digit) / 10u) return false;
        value = value * 10u + digit;
        p++;
    }
    *cursor = p;
    *out = value;
    return true;
}

/* Dot-separated identifiers; a numeric one has no leading zero. Stops at '+' or
   the end. */
static bool parse_identifiers(const char **cursor, bool numeric_rule, char *out,
                              size_t out_size) {
    const char *p = *cursor;
    const char *start = p;
    for (;;) {
        const char *part = p;
        bool all_digits = true;
        while (is_identifier_char(*p)) {
            if (!is_digit(*p)) all_digits = false;
            p++;
        }
        if (p == part) return false;
        if (numeric_rule && all_digits && *part == '0' && p - part > 1) return false;
        if (*p != '.') break;
        p++;
    }
    if (out) {
        size_t length = (size_t)(p - start);
        if (length >= out_size) return false;
        memcpy(out, start, length);
        out[length] = '\0';
    }
    *cursor = p;
    return true;
}

bool nk_semver_parse(const char *text, NkSemver *out) {
    NkSemver parsed;
    const char *p = text;
    if (!text || !out) return false;
    memset(&parsed, 0, sizeof(parsed));
    if (*p == 'v') p++;
    if (!parse_number(&p, &parsed.major) || *p++ != '.' ||
        !parse_number(&p, &parsed.minor) || *p++ != '.' ||
        !parse_number(&p, &parsed.patch)) {
        return false;
    }
    if (*p == '-') {
        p++;
        if (!parse_identifiers(&p, true, parsed.prerelease, sizeof(parsed.prerelease))) {
            return false;
        }
    }
    if (*p == '+') {
        p++;
        if (!parse_identifiers(&p, false, NULL, 0)) return false;
    }
    if (*p != '\0') return false;
    *out = parsed;
    return true;
}

/* Pre-release precedence, SemVer 2.0.0 item 11: identifiers left to right;
   numeric ones compare as numbers and rank below alphanumeric ones; a shorter
   list that matches so far ranks lower. */
static int compare_prerelease(const char *a, const char *b) {
    while (*a && *b) {
        size_t la = strcspn(a, ".");
        size_t lb = strcspn(b, ".");
        bool na = true;
        bool nb = true;
        for (size_t i = 0; i < la; i++) if (!is_digit(a[i])) na = false;
        for (size_t i = 0; i < lb; i++) if (!is_digit(b[i])) nb = false;
        if (na && nb) {
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(a, b, la);
            if (c != 0) return c < 0 ? -1 : 1;
        } else if (na != nb) {
            return na ? -1 : 1;
        } else {
            size_t n = la < lb ? la : lb;
            int c = strncmp(a, b, n);
            if (c != 0) return c < 0 ? -1 : 1;
            if (la != lb) return la < lb ? -1 : 1;
        }
        a += la;
        b += lb;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

int nk_semver_compare(const NkSemver *a, const NkSemver *b) {
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    if (!a->prerelease[0] || !b->prerelease[0]) {
        if (!a->prerelease[0] && !b->prerelease[0]) return 0;
        return a->prerelease[0] ? -1 : 1;
    }
    return compare_prerelease(a->prerelease, b->prerelease);
}

/* ---- Release notes -------------------------------------------------------------- */

/* Appends Markdown inline text as plain text: **, __ and ` go, [text](url)
   keeps its text, an image goes entirely. */
static void append_inline(char *out, size_t out_size, size_t *used, const char *line,
                          size_t length) {
    size_t i = 0;
    while (i < length && *used + 1 < out_size) {
        char c = line[i];
        if ((c == '*' || c == '_') && i + 1 < length && line[i + 1] == c) {
            i += 2;
            continue;
        }
        if (c == '`') {
            i++;
            continue;
        }
        if (c == '[' || (c == '!' && i + 1 < length && line[i + 1] == '[')) {
            bool image = c == '!';
            size_t open = image ? i + 1 : i;
            size_t close = open + 1;
            while (close < length && line[close] != ']') close++;
            if (close + 1 < length && line[close + 1] == '(') {
                size_t end = close + 2;
                while (end < length && line[end] != ')') end++;
                if (end < length) {
                    if (!image) append_inline(out, out_size, used, line + open + 1,
                                              close - open - 1);
                    i = end + 1;
                    continue;
                }
            }
        }
        out[(*used)++] = c;
        i++;
    }
    out[*used] = '\0';
}

/* Appends one line, joined to the text before it with a space. */
static void append_plain(char *out, size_t out_size, size_t *used, const char *line,
                         size_t length) {
    if (*used > 0 && *used + 1 < out_size && out[*used - 1] != ' ') out[(*used)++] = ' ';
    append_inline(out, out_size, used, line, length);
}

void nk_update_short_notes(const char *markdown, char *out, size_t out_size) {
    /* Room for a short card: the first two paragraphs, cut at a word. */
    size_t used = 0;
    int paragraphs = 0;
    bool in_paragraph = false;
    bool cut = false;
    char text[2048];
    const char *p = markdown;
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!markdown) return;
    text[0] = '\0';
    while (*p && paragraphs < 2 && used + 1 < sizeof(text)) {
        const char *end = strchr(p, '\n');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        const char *line = p;
        p = end ? end + 1 : p + length;
        if (length > 0 && line[length - 1] == '\r') length--;
        while (length > 0 && (*line == '>' || *line == ' ' || *line == '\t')) {
            line++;
            length--;
        }
        while (length > 0 && (line[length - 1] == ' ' || line[length - 1] == '\t')) length--;
        if (length == 0) {
            if (in_paragraph) paragraphs++;
            in_paragraph = false;
            continue;
        }
        if (line[0] == '#' || (length >= 2 && line[0] == '[' && line[1] == '!') ||
            (length >= 4 && strncmp(line, "<!--", 4) == 0)) {
            if (in_paragraph) paragraphs++;
            in_paragraph = false;
            continue;
        }
        if (length >= 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            line += 2;
            length -= 2;
        }
        append_plain(text, sizeof(text), &used, line, length);
        in_paragraph = true;
    }
    if (strlen(text) >= out_size) {
        size_t limit = out_size - 4;
        while (limit > 0 && text[limit] != ' ') limit--;
        if (limit == 0) limit = out_size - 4;
        while (limit > 0 && (unsigned char)text[limit] >= 0x80 &&
               ((unsigned char)text[limit] & 0xC0) == 0x80) {
            limit--;   /* never cut inside a UTF-8 character */
        }
        text[limit] = '\0';
        cut = true;
    }
    snprintf(out, out_size, "%s%s", text, cut ? "..." : "");
}

/* ---- The release list ----------------------------------------------------------- */

static const char *json_string(const NkJsonNode *object, const char *key) {
    const NkJsonNode *node = nk_json_obj_get(object, key);
    return nk_json_is_string(node) ? nk_json_get_string(node) : NULL;
}

static bool safe_tag(const char *tag) {
    size_t length = tag ? strlen(tag) : 0;
    if (length == 0 || length >= 64) return false;
    for (size_t i = 0; i < length; i++) {
        char c = tag[i];
        if (!is_identifier_char(c) && c != '.' && c != '+') return false;
    }
    return true;
}

static bool exact_url(const char *url, const char *prefix, const char *tag, const char *name) {
    char expected[512];
    int n = name ? snprintf(expected, sizeof(expected), "%s%s/%s", prefix, tag, name)
                 : snprintf(expected, sizeof(expected), "%s%s", prefix, tag);
    return url && n > 0 && (size_t)n < sizeof(expected) && strcmp(url, expected) == 0;
}

static bool hex64(const char *text) {
    if (!text || strlen(text) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        char c = text[i];
        if (!is_digit(c) && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F')) return false;
    }
    return true;
}

static void lower_hex(char *text) {
    for (; *text; text++) if (*text >= 'A' && *text <= 'F') *text = (char)(*text - 'A' + 'a');
}

static bool asset_size(const NkJsonNode *asset, uint64_t *out) {
    int64_t size = 0;
    if (!nk_json_get_int64(nk_json_obj_get(asset, "size"), &size) || size <= 0) return false;
    *out = (uint64_t)size;
    return true;
}

static bool unexpected_release(NkUpdateError *error) {
    set_error(error, "UPDATE_ANSWER_UNEXPECTED", "Unexpected Answer from GitHub",
              "GitHub's list of releases named a page or file outside the Nakagawa repository, "
              "so Nakagawa ignored it. Try again later.");
    return false;
}

/* The Windows ZIP, its checksum file and that file's signature among a release's
   assets. */
static bool read_assets(const NkJsonNode *assets, NkUpdateRelease *release, NkUpdateError *error) {
    char checksum_name[160];
    char signature_name[168];
    snprintf(release->zip_name, sizeof(release->zip_name), "nakagawa-recomp-%s-windows-x64.zip",
             release->version);
    snprintf(checksum_name, sizeof(checksum_name), "%s.sha256", release->zip_name);
    snprintf(signature_name, sizeof(signature_name), "%s.minisig", checksum_name);
    for (size_t i = 0; i < nk_json_array_count(assets); i++) {
        const NkJsonNode *asset = nk_json_array_get(assets, i);
        const char *name = json_string(asset, "name");
        const char *url = json_string(asset, "browser_download_url");
        const char *state = json_string(asset, "state");
        bool is_zip = name && strcmp(name, release->zip_name) == 0;
        bool is_checksum = name && strcmp(name, checksum_name) == 0;
        bool is_signature = name && strcmp(name, signature_name) == 0;
        if (!is_zip && !is_checksum && !is_signature) continue;
        if (state && strcmp(state, "uploaded") != 0) continue;
        if (!exact_url(url, NK_UPDATE_DOWNLOAD_PREFIX, release->tag, name)) {
            return unexpected_release(error);
        }
        if (is_zip) {
            const char *digest = json_string(asset, "digest");
            if (!asset_size(asset, &release->zip_size)) return unexpected_release(error);
            snprintf(release->zip_url, sizeof(release->zip_url), "%s", url);
            if (digest && strncmp(digest, "sha256:", 7) == 0 && hex64(digest + 7)) {
                snprintf(release->zip_digest, sizeof(release->zip_digest), "%s", digest + 7);
                lower_hex(release->zip_digest);
            }
            release->has_zip = true;
        } else if (is_signature) {
            if (!asset_size(asset, &release->signature_size) ||
                release->signature_size > NK_UPDATE_MAX_SIGNATURE_BYTES) {
                return unexpected_release(error);
            }
            snprintf(release->signature_url, sizeof(release->signature_url), "%s", url);
            release->has_signature = true;
        } else {
            if (!asset_size(asset, &release->checksum_size) ||
                release->checksum_size > NK_UPDATE_MAX_CHECKSUM_BYTES) {
                return unexpected_release(error);
            }
            snprintf(release->checksum_url, sizeof(release->checksum_url), "%s", url);
            release->has_checksum = true;
        }
    }
    return true;
}

bool nk_update_parse_releases(const char *json, size_t length, NkUpdateRelease *newest,
                              NkUpdateError *error) {
    char parse_error[128];
    const NkJsonNode *best = NULL;
    NkSemver best_version;
    bool ok = false;
    clear_error(error);
    if (!newest) return false;
    memset(newest, 0, sizeof(*newest));
    NkJsonNode *root = json ? nk_json_parse(json, length, parse_error, sizeof(parse_error)) : NULL;
    if (!nk_json_is_array(root)) {
        set_error(error, "UPDATE_ANSWER_UNREADABLE", "Unexpected Answer from GitHub",
                  "GitHub's answer was not a list of releases. Try again later.");
        nk_json_free(root);
        return false;
    }
    memset(&best_version, 0, sizeof(best_version));
    for (size_t i = 0; i < nk_json_array_count(root); i++) {
        const NkJsonNode *release = nk_json_array_get(root, i);
        bool draft = false;
        NkSemver version;
        const char *tag;
        if (!nk_json_is_object(release)) continue;
        tag = json_string(release, "tag_name");
        (void)nk_json_get_bool(nk_json_obj_get(release, "draft"), &draft);
        if (draft || !safe_tag(tag) || !nk_semver_parse(tag, &version)) continue;
        if (!exact_url(json_string(release, "html_url"), NK_UPDATE_RELEASE_PAGE_PREFIX, tag,
                       NULL)) {
            nk_json_free(root);
            return unexpected_release(error);
        }
        if (!best || nk_semver_compare(&version, &best_version) > 0) {
            best = release;
            best_version = version;
        }
    }
    if (!best) {
        set_error(error, "UPDATE_NO_RELEASES", "No Releases Yet",
                  "GitHub lists no Nakagawa release yet.");
        nk_json_free(root);
        return false;
    }
    {
        const char *tag = json_string(best, "tag_name");
        const char *published = json_string(best, "published_at");
        const char *body = json_string(best, "body");
        snprintf(newest->tag, sizeof(newest->tag), "%s", tag);
        snprintf(newest->version, sizeof(newest->version), "%s", tag[0] == 'v' ? tag + 1 : tag);
        newest->semver = best_version;
        (void)nk_json_get_bool(nk_json_obj_get(best, "prerelease"), &newest->prerelease);
        snprintf(newest->page_url, sizeof(newest->page_url), "%s",
                 json_string(best, "html_url"));
        if (published && strlen(published) >= 10) {
            snprintf(newest->published, sizeof(newest->published), "%.10s", published);
        }
        nk_update_short_notes(body, newest->notes, sizeof(newest->notes));
        ok = read_assets(nk_json_obj_get(best, "assets"), newest, error);
    }
    nk_json_free(root);
    if (!ok) memset(newest, 0, sizeof(*newest));
    return ok;
}

/* ---- Checksums ------------------------------------------------------------------ */

bool nk_update_parse_checksum(const char *text, size_t length, const char *file_name,
                              char out_sha256[65], NkUpdateError *error) {
    size_t i = 0;
    size_t name_start;
    size_t name_end;
    clear_error(error);
    if (text && length >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        i = 3;   /* a byte-order mark */
    }
    if (!text || !file_name || !out_sha256 || length < i + 64) goto invalid;
    for (size_t k = 0; k < 64; k++) {
        char c = text[i + k];
        if (!is_digit(c) && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F')) goto invalid;
    }
    memcpy(out_sha256, text + i, 64);
    out_sha256[64] = '\0';
    lower_hex(out_sha256);
    i += 64;
    if (i >= length || (text[i] != ' ' && text[i] != '\t')) goto invalid;
    while (i < length && (text[i] == ' ' || text[i] == '\t')) i++;
    if (i < length && text[i] == '*') i++;
    name_start = i;
    while (i < length && text[i] != '\r' && text[i] != '\n') i++;
    name_end = i;
    while (i < length && (text[i] == '\r' || text[i] == '\n' || text[i] == ' ')) i++;
    if (i != length || name_end == name_start) goto invalid;
    /* The checksum names the file it is for, and the ZIP's name carries the
       version: a checksum file copied from another release names another file. */
    if (name_end - name_start != strlen(file_name) ||
        strncmp(text + name_start, file_name, name_end - name_start) != 0) {
        out_sha256[0] = '\0';
        set_error(error, "UPDATE_CHECKSUM_WRONG_FILE", "Checksum for Another File",
                  "The release's checksum file is for a different file than this release's "
                  "Windows ZIP, so Nakagawa did not download it. You can download it from the "
                  "release page instead.");
        return false;
    }
    return true;
invalid:
    if (out_sha256) out_sha256[0] = '\0';
    set_error(error, "UPDATE_CHECKSUM_INVALID", "Checksum File Not Readable",
              "The release's checksum file is not in the expected form, so Nakagawa did not "
              "download the update. You can download it from the release page instead.");
    return false;
}

/* ---- Signatures ----------------------------------------------------------------- */

bool nk_update_builtin_signing_key(NkMinisignKey *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
#ifdef NK_UPDATE_SIGNING_KEY
    {
        /* The base64 line of assets/update_signing_key.pub, which the build
           checked; parsed again here so a bad define can only fail closed. */
        static const char key_line[] = NK_UPDATE_SIGNING_KEY;
        return nk_minisign_parse_key(key_line, sizeof(key_line) - 1u, out);
    }
#else
    return false;
#endif
}

static void no_signing_key_error(NkUpdateError *error) {
    set_error(error, "UPDATE_NO_SIGNING_KEY", "Can't Check Downloads",
              "This build of Nakagawa has no update signing key, so it can't check that a "
              "download really comes from the Nakagawa project, and it won't fetch one. You "
              "can download the new version from the release page instead.");
}

static void checksum_missing_error(NkUpdateError *error) {
    set_error(error, "UPDATE_CHECKSUM_MISSING", "No Checksum Published",
              "This release has no checksum file for its Windows ZIP, so Nakagawa can't "
              "check the download and won't fetch it. You can download it from the "
              "release page instead.");
}

static void signature_missing_error(NkUpdateError *error) {
    set_error(error, "UPDATE_SIGNATURE_MISSING", "Release Not Signed",
              "This release has no signature for its checksum file, so Nakagawa can't check "
              "that the download comes from the Nakagawa project and won't fetch it. You can "
              "download it from the release page instead.");
}

/* The checksum file's bytes are trusted only when the signature verifies. */
static bool verify_checksum_signature(const NkMinisignKey *key, const char *checksum_text,
                                      size_t checksum_length, const char *signature_text,
                                      size_t signature_length, NkUpdateError *error) {
    switch (nk_minisign_verify(key, signature_text, signature_length,
                               (const uint8_t *)checksum_text, checksum_length, NULL, NULL)) {
        case NK_MINISIGN_VALID:
            return true;
        case NK_MINISIGN_UNKNOWN_KEY:
            set_error(error, "UPDATE_SIGNATURE_UNKNOWN_KEY", "Signed With Another Key",
                      "The release is signed with a key this version of Nakagawa doesn't know, "
                      "so it did not download the update. You can download it from the release "
                      "page instead.");
            return false;
        case NK_MINISIGN_BAD_SIGNATURE:
        case NK_MINISIGN_BAD_COMMENT:
            set_error(error, "UPDATE_SIGNATURE_INVALID", "Signature Didn't Match",
                      "The release's signature doesn't match its checksum file, so Nakagawa did "
                      "not download the update. Nothing was installed. Try again later, or "
                      "download it from the release page.");
            return false;
        case NK_MINISIGN_MALFORMED:
        default:
            set_error(error, "UPDATE_SIGNATURE_UNREADABLE", "Signature Not Readable",
                      "The release's signature file is not in the expected form, so Nakagawa "
                      "did not download the update. You can download it from the release page "
                      "instead.");
            return false;
    }
}

bool nk_update_expected_sha256(const NkUpdateRelease *release, const NkMinisignKey *signing_key,
                               const char *checksum_text, size_t checksum_length,
                               const char *signature_text, size_t signature_length,
                               char out_sha256[65], NkUpdateError *error) {
    clear_error(error);
    if (out_sha256) out_sha256[0] = '\0';
    if (!release || !out_sha256) return false;
    if (!signing_key) {
        no_signing_key_error(error);
        return false;
    }
    /* What the release lists decides first, then what was fetched. */
    if (!release->has_checksum) {
        checksum_missing_error(error);
        return false;
    }
    if (!release->has_signature) {
        signature_missing_error(error);
        return false;
    }
    if (!checksum_text) {
        checksum_missing_error(error);
        return false;
    }
    if (!signature_text) {
        signature_missing_error(error);
        return false;
    }
    if (!verify_checksum_signature(signing_key, checksum_text, checksum_length, signature_text,
                                   signature_length, error)) {
        return false;
    }
    if (!nk_update_parse_checksum(checksum_text, checksum_length, release->zip_name, out_sha256,
                                  error)) {
        return false;
    }
    if (release->zip_digest[0] && strcmp(release->zip_digest, out_sha256) != 0) {
        out_sha256[0] = '\0';
        set_error(error, "UPDATE_CHECKSUM_CONFLICT", "Checksums Disagree",
                  "The release's checksum file and GitHub's own checksum for the ZIP don't "
                  "match, so Nakagawa did not download it. Try again later.");
        return false;
    }
    return true;
}

/* ---- Network -------------------------------------------------------------------- */

static bool url_host_allowed(const char *url, const char *allowed_hosts) {
    static const char scheme[] = "https://";
    const char *host;
    size_t length;
    if (!url || !allowed_hosts || strncmp(url, scheme, sizeof(scheme) - 1) != 0) return false;
    host = url + sizeof(scheme) - 1;
    length = strcspn(host, "/?#");
    if (length == 0 || memchr(host, '@', length) || memchr(host, ':', length)) return false;
    for (const char *cur = allowed_hosts; *cur;) {
        size_t entry = strcspn(cur, ";");
        if (entry == length) {
            size_t i = 0;
            while (i < length) {
                char a = host[i];
                char b = cur[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (a != b) break;
                i++;
            }
            if (i == length) return true;
        }
        cur += entry;
        if (*cur == ';') cur++;
    }
    return false;
}

static void offline_error(NkUpdateError *error) {
    set_error(error, "UPDATE_OFFLINE", "Couldn't Reach GitHub",
              "Nakagawa couldn't reach GitHub. Check your internet connection and try again "
              "later. Nothing was changed.");
}

static PackageHttpOpen resolve_transport(PackageHttpOpen transport, void **context) {
    static const PackageHttpOptions options = { NK_UPDATE_USER_AGENT };
    if (transport) return transport;
    *context = (void *)&options;
    return package_builder_native_transport();
}

static bool unsupported(NkUpdateError *error) {
    set_error(error, "UPDATE_PLATFORM_UNSUPPORTED", "Not Available Here",
              "Checking for updates from Nakagawa works on Windows only for now. The release "
              "page on GitHub lists every version.");
    return false;
}

/* Reads a whole answer of at most cap bytes into a NUL-terminated buffer. */
static bool fetch_text(PackageHttpOpen transport, void *context, const char *url,
                       const char *allowed_hosts, size_t cap, char **out, size_t *out_length,
                       NkUpdateError *error) {
    PackageHttpResponse response;
    char *buffer = NULL;
    size_t used = 0;
    bool ok = false;
    *out = NULL;
    *out_length = 0;
    memset(&response, 0, sizeof(response));
    if (!url_host_allowed(url, allowed_hosts)) return unexpected_release(error);
    if (!transport(context, url, allowed_hosts, &response)) {
        offline_error(error);
        return false;
    }
    if (!response.final_url || !url_host_allowed(response.final_url, allowed_hosts) ||
        !response.read) {
        set_error(error, "UPDATE_REDIRECT_REJECTED", "Unexpected Answer from GitHub",
                  "GitHub sent Nakagawa to a host it doesn't use for updates, so it stopped. "
                  "Nothing was downloaded.");
        goto done;
    }
    if (response.has_content_length && response.content_length > cap) goto too_large;
    buffer = (char *)malloc(cap + 1);
    if (!buffer) {
        offline_error(error);
        goto done;
    }
    for (;;) {
        size_t count = 0;
        if (used == cap) {
            unsigned char extra;
            if (!response.read(response.context, &extra, 1, &count)) {
                offline_error(error);
                goto done;
            }
            if (count != 0) goto too_large;
            break;
        }
        if (!response.read(response.context, (unsigned char *)buffer + used, cap - used,
                           &count)) {
            offline_error(error);
            goto done;
        }
        if (count == 0) break;
        used += count;
    }
    buffer[used] = '\0';
    *out = buffer;
    *out_length = used;
    buffer = NULL;
    ok = true;
    goto done;
too_large:
    set_error(error, "UPDATE_ANSWER_TOO_LARGE", "Unexpected Answer from GitHub",
              "GitHub's answer was much larger than expected, so Nakagawa stopped reading it. "
              "Try again later.");
done:
    free(buffer);
    if (response.close) response.close(response.context);
    return ok;
}

bool nk_update_check(PackageHttpOpen transport, void *transport_context,
                     const char *current_version, NkUpdateCheckResult *result,
                     NkUpdateError *error) {
    char *json = NULL;
    size_t length = 0;
    bool ok;
    clear_error(error);
    if (!result) return false;
    memset(result, 0, sizeof(*result));
    if (!nk_semver_parse(current_version, &result->current)) {
        set_error(error, "UPDATE_VERSION_UNKNOWN", "Version Unknown",
                  "This build doesn't carry a version Nakagawa can compare.");
        return false;
    }
    transport = resolve_transport(transport, &transport_context);
    if (!transport) return unsupported(error);
    if (!fetch_text(transport, transport_context, NK_UPDATE_RELEASES_URL, NK_UPDATE_API_HOSTS,
                    NK_UPDATE_MAX_RELEASES_BYTES, &json, &length, error)) {
        return false;
    }
    ok = nk_update_parse_releases(json, length, &result->newest, error);
    free(json);
    if (ok) result->newer = nk_semver_compare(&result->newest.semver, &result->current) > 0;
    return ok;
}

/* The verified download's failure, in the update's words. */
static void download_error(const char *code, const char *message, const char *folder,
                           NkUpdateError *error) {
    char text[512];
    if (strcmp(code, "OFFLINE_OR_NETWORK_ERROR") == 0) {
        offline_error(error);
    } else if (strcmp(code, "HASH_MISMATCH") == 0) {
        set_error(error, "UPDATE_HASH_MISMATCH", "Download Didn't Match",
                  "The download didn't match the release's published SHA-256, so Nakagawa "
                  "deleted it. Nothing was installed. Try again later, or download it from the "
                  "release page.");
    } else if (strcmp(code, "SIZE_MISMATCH") == 0 || strcmp(code, "TRUNCATED_BODY") == 0) {
        set_error(error, "UPDATE_INCOMPLETE_DOWNLOAD", "Download Incomplete",
                  "The download was not the size the release lists, so Nakagawa deleted it. "
                  "Try again later.");
    } else if (strcmp(code, "INSTALL_CANCELLED") == 0) {
        set_error(error, "UPDATE_DOWNLOAD_CANCELLED", "Download Cancelled",
                  "You cancelled the download. The partial file was deleted.");
    } else if (strcmp(code, "REDIRECT_REJECTED") == 0) {
        set_error(error, "UPDATE_REDIRECT_REJECTED", "Unexpected Answer from GitHub",
                  "GitHub sent the download to a host Nakagawa doesn't use for updates, so it "
                  "stopped. Nothing was downloaded.");
    } else if (strcmp(code, "PLATFORM_UNSUPPORTED") == 0) {
        (void)unsupported(error);
    } else if (strcmp(code, "DISK_FULL") == 0 || strcmp(code, "DESTINATION_WRITE_FAILED") == 0 ||
               strcmp(code, "PATH_TOO_LONG") == 0) {
        snprintf(text, sizeof(text),
                 "Nakagawa couldn't save the download in %.300s. Free some disk space and try "
                 "again.", folder);
        set_error(error, "UPDATE_SAVE_FAILED", "Couldn't Save the Download", text);
    } else {
        snprintf(text, sizeof(text), "%.480s", message && message[0] ? message
                 : "The download could not be checked. Nothing was installed.");
        set_error(error, code, "Download Failed", text);
    }
}

bool nk_update_download(PackageHttpOpen transport, void *transport_context,
                        const NkUpdateRelease *release, const NkMinisignKey *signing_key,
                        const char *downloads_dir, PackageDownloadContinue progress,
                        void *progress_context, char *out_zip_path, size_t out_zip_path_size,
                        NkUpdateError *error) {
    PackagePrerequisite item;
    char *checksum = NULL;
    size_t checksum_length = 0;
    char *signature = NULL;
    size_t signature_length = 0;
    char destination[NK_MAX_PATH];
    char code[48] = "";
    char message[512] = "";
    bool trusted;
    int n;
    clear_error(error);
    if (out_zip_path && out_zip_path_size) out_zip_path[0] = '\0';
    if (!release || !downloads_dir || !out_zip_path || !release->has_zip) {
        set_error(error, "UPDATE_NO_WINDOWS_ZIP", "No Windows Download",
                  "This release has no Windows ZIP to download.");
        return false;
    }
    /* Nothing is fetched that could not be checked: no key, no checksum file or
       no signature stops here, before any request. */
    memset(&item, 0, sizeof(item));
    if (!signing_key || !release->has_checksum || !release->has_signature) {
        return nk_update_expected_sha256(release, signing_key, NULL, 0, NULL, 0, item.sha256,
                                         error);
    }
    transport = resolve_transport(transport, &transport_context);
    if (!transport) return unsupported(error);
    if (!fetch_text(transport, transport_context, release->checksum_url,
                    NK_UPDATE_DOWNLOAD_HOSTS, NK_UPDATE_MAX_CHECKSUM_BYTES, &checksum,
                    &checksum_length, error)) {
        return false;
    }
    if (!fetch_text(transport, transport_context, release->signature_url,
                    NK_UPDATE_DOWNLOAD_HOSTS, NK_UPDATE_MAX_SIGNATURE_BYTES, &signature,
                    &signature_length, error)) {
        free(checksum);
        return false;
    }
    trusted = nk_update_expected_sha256(release, signing_key, checksum, checksum_length,
                                        signature, signature_length, item.sha256, error);
    free(signature);
    free(checksum);
    if (!trusted) return false;
    n = snprintf(destination, sizeof(destination), "%s%c%s", downloads_dir,
                 nk_platform_path_separator(), release->zip_name);
    if (n < 0 || (size_t)n >= sizeof(destination) || (size_t)n >= out_zip_path_size ||
        !nk_platform_mkdir_p(downloads_dir)) {
        download_error("DESTINATION_WRITE_FAILED", NULL, downloads_dir, error);
        return false;
    }
    /* An earlier download of the same version is replaced, never trusted. */
    if (nk_platform_file_exists(destination) && nk_remove_utf8(destination) != 0) {
        download_error("DESTINATION_WRITE_FAILED", NULL, downloads_dir, error);
        return false;
    }
    snprintf(item.id, sizeof(item.id), "nakagawa-update");
    snprintf(item.name, sizeof(item.name), "%s", release->zip_name);
    snprintf(item.version, sizeof(item.version), "%.47s", release->version);
    snprintf(item.host, sizeof(item.host), "github.com");
    snprintf(item.allowed_hosts, sizeof(item.allowed_hosts), "%s", NK_UPDATE_DOWNLOAD_HOSTS);
    snprintf(item.url, sizeof(item.url), "%s", release->zip_url);
    snprintf(item.filename, sizeof(item.filename), "%s", release->zip_name);
    item.size_bytes = release->zip_size;
    if (!package_builder_download_verified(&item, destination, transport, transport_context,
                                           progress, progress_context, code, sizeof(code),
                                           message, sizeof(message))) {
        download_error(code, message, downloads_dir, error);
        return false;
    }
    /* Antivirus software may remove a file as soon as it is written. */
    if (!nk_platform_file_exists(destination)) {
        set_error(error, "UPDATE_DOWNLOAD_REMOVED", "Download Removed",
                  "The checked download is no longer in its folder. Windows Security may have "
                  "removed it; Protection history in Windows Security names the detection. "
                  "Please don't turn Windows Security off.");
        return false;
    }
    snprintf(out_zip_path, out_zip_path_size, "%s", destination);
    return true;
}

/* ---- Install -------------------------------------------------------------------- */

static size_t trimmed_length(const char *path) {
    size_t length = strlen(path);
    while (length > 1 && (path[length - 1] == '/' || path[length - 1] == '\\')) length--;
    return length;
}

/* The start of the last component of path[0..length). */
static size_t last_component(const char *path, size_t length) {
    size_t i = length;
    while (i > 0 && path[i - 1] != '/' && path[i - 1] != '\\') i--;
    return i;
}

bool nk_update_install_folder(const char *exe_dir, const char *downloads_dir,
                              const char *zip_name, char *out, size_t out_size) {
    char marker[NK_MAX_PATH];
    size_t name_length;
    int n = -1;
    if (!downloads_dir || !zip_name || !out || out_size == 0) return false;
    out[0] = '\0';
    name_length = strlen(zip_name);
    if (name_length <= 4 || strcmp(zip_name + name_length - 4, ".zip") != 0 ||
        strchr(zip_name, '/') || strchr(zip_name, '\\')) {
        return false;
    }
    name_length -= 4;
    if (exe_dir && exe_dir[0]) {
        /* <parent>\<install>\bin\nakagawa_player.exe: the release ZIP's layout. */
        size_t bin_end = trimmed_length(exe_dir);
        size_t bin_start = last_component(exe_dir, bin_end);
        if (bin_end - bin_start == 3 && strncmp(exe_dir + bin_start, "bin", 3) == 0 &&
            bin_start > 1) {
            size_t install_end = bin_start - 1;
            size_t install_start = last_component(exe_dir, install_end);
            int m = snprintf(marker, sizeof(marker), "%.*s%cSTART HERE.txt", (int)install_end,
                             exe_dir, nk_platform_path_separator());
            if (install_start > 1 && m > 0 && (size_t)m < sizeof(marker) &&
                nk_platform_file_exists(marker)) {
                n = snprintf(out, out_size, "%.*s%.*s", (int)install_start, exe_dir,
                             (int)name_length, zip_name);
            }
        }
    }
    if (n < 0) {
        n = snprintf(out, out_size, "%s%c%.*s", downloads_dir, nk_platform_path_separator(),
                     (int)name_length, zip_name);
    }
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

#if defined(_WIN32) || defined(_WIN64)
/* Every file the release's SHA256SUMS.txt lists is present under root. The
   ZIP itself was verified, so a missing file means unpacking lost it (an
   antivirus removal, a full disk). Writes the first missing file to missing. */
static bool listed_files_present(const char *root, char *missing, size_t missing_size,
                                 bool *sums_found) {
    char sums_path[NK_MAX_PATH];
    char line[1024];
    size_t count = 0;
    FILE *sums;
    *sums_found = false;
    missing[0] = '\0';
    if (snprintf(sums_path, sizeof(sums_path), "%s\\SHA256SUMS.txt", root) >=
        (int)sizeof(sums_path)) {
        return false;
    }
    sums = nk_fopen_utf8(sums_path, "rb");
    if (!sums) return false;
    *sums_found = true;
    while (fgets(line, sizeof(line), sums)) {
        char path[NK_MAX_PATH];
        char *name = line;
        size_t length = strcspn(line, "\r\n");
        line[length] = '\0';
        if (length == 0) continue;
        if (length < 67 || line[64] != ' ') {
            fclose(sums);
            snprintf(missing, missing_size, "SHA256SUMS.txt");
            return false;
        }
        name = line + 64;
        while (*name == ' ' || *name == '*') name++;
        if (!*name || name[0] == '/' || name[0] == '\\' || strstr(name, "..") ||
            strchr(name, ':') ||
            snprintf(path, sizeof(path), "%s\\%s", root, name) >= (int)sizeof(path)) {
            fclose(sums);
            snprintf(missing, missing_size, "%.200s", name);
            return false;
        }
        for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
        if (!nk_platform_file_exists(path)) {
            fclose(sums);
            snprintf(missing, missing_size, "%.200s", name);
            return false;
        }
        count++;
    }
    fclose(sums);
    if (count == 0) snprintf(missing, missing_size, "SHA256SUMS.txt");
    return count > 0;
}
#endif

bool nk_update_install(const char *zip_path, const char *target_dir, const char *log_path,
                       NkUpdateError *error) {
    clear_error(error);
#if defined(_WIN32) || defined(_WIN64)
    char staging[NK_MAX_PATH];
    char tar_exe[NK_MAX_PATH];
    char player[NK_MAX_PATH];
    char missing[256];
    char text[512];
    WCHAR system_directory_w[MAX_PATH];
    char system_directory[NK_MAX_PATH];
    NkProcessHandle process;
    bool sums_found = false;
    int exit_code;
    DWORD length;
    if (!zip_path || !target_dir || !target_dir[0] ||
        snprintf(staging, sizeof(staging), "%s.partial", target_dir) >= (int)sizeof(staging)) {
        set_error(error, "UPDATE_INSTALL_PATH", "Couldn't Install",
                  "The folder for the new version has too long a path. Move Nakagawa to a "
                  "shorter folder, or unpack the ZIP yourself.");
        return false;
    }
    if (!nk_platform_path_missing(target_dir)) {
        snprintf(text, sizeof(text),
                 "%.300s already exists, so Nakagawa left it alone. If it holds this version, "
                 "start it from there.", target_dir);
        set_error(error, "UPDATE_INSTALL_EXISTS", "Already Unpacked", text);
        return false;
    }
    if (!nk_platform_path_missing(staging)) {
        snprintf(text, sizeof(text),
                 "%.300s is left from an earlier try. Delete it, then try again.", staging);
        set_error(error, "UPDATE_INSTALL_LEFTOVER", "Couldn't Install", text);
        return false;
    }
    if (!nk_platform_mkdir_p(staging)) {
        snprintf(text, sizeof(text),
                 "Nakagawa couldn't create a folder next to this version (%.300s). Unpack the "
                 "ZIP yourself, or move Nakagawa to a folder you can write to.", target_dir);
        set_error(error, "UPDATE_INSTALL_NOT_WRITABLE", "Couldn't Install", text);
        return false;
    }
    length = GetSystemDirectoryW(system_directory_w,
                                 sizeof(system_directory_w) / sizeof(system_directory_w[0]));
    if (!length || length >= sizeof(system_directory_w) / sizeof(system_directory_w[0]) ||
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, system_directory_w, -1,
                            system_directory, sizeof(system_directory), NULL, NULL) <= 0 ||
        snprintf(tar_exe, sizeof(tar_exe), "%s\\tar.exe", system_directory) >=
            (int)sizeof(tar_exe)) {
        (void)nk_platform_remove_tree(staging, NULL);
        set_error(error, "UPDATE_UNPACK_UNAVAILABLE", "Couldn't Install",
                  "Windows' own archive tool (tar.exe) was not found. Unpack the ZIP yourself.");
        return false;
    }
    {
        const char *argv[] = { tar_exe, "-xf", zip_path, "-C", staging, NULL };
        memset(&process, 0, sizeof(process));
        if (!nk_platform_spawn_process(tar_exe, argv, NULL, NULL, log_path, &process)) {
            NkSpawnBlockReport block;
            (void)nk_platform_remove_tree(staging, NULL);
            if (nk_launch_describe_spawn_block(process.spawn_error,
                                               "Windows' archive tool (tar.exe)",
                                               "Archive Tool", tar_exe, &block)) {
                set_error(error, block.code, block.title, block.message);
            } else {
                set_error(error, "UPDATE_UNPACK_FAILED", "Couldn't Install",
                          "Windows' archive tool (tar.exe) did not start. Unpack the ZIP "
                          "yourself.");
            }
            return false;
        }
    }
    exit_code = nk_platform_wait_process(&process, -1);
    nk_platform_close_process(&process);
    if (exit_code != 0) {
        (void)nk_platform_remove_tree(staging, NULL);
        snprintf(text, sizeof(text),
                 "Windows' archive tool stopped with code %d while unpacking. Its output is in "
                 "%.300s.", exit_code, log_path ? log_path : "the logs folder");
        set_error(error, "UPDATE_UNPACK_FAILED", "Couldn't Install", text);
        return false;
    }
    if (!listed_files_present(staging, missing, sizeof(missing), &sums_found) ||
        snprintf(player, sizeof(player), "%s\\bin\\nakagawa_player.exe", staging) >=
            (int)sizeof(player) ||
        !nk_platform_file_exists(player)) {
        (void)nk_platform_remove_tree(staging, NULL);
        snprintf(text, sizeof(text),
                 "%s%.200s%s Windows Security may have removed a file; Protection history in "
                 "Windows Security names the detection. Please don't turn Windows Security off.",
                 sums_found ? "Unpacking left out " : "The ZIP has no file list",
                 sums_found ? (missing[0] ? missing : "bin\\nakagawa_player.exe") : "",
                 sums_found ? "." : " (SHA256SUMS.txt).");
        set_error(error, "UPDATE_INSTALL_INCOMPLETE", "Install Incomplete", text);
        return false;
    }
    if (nk_rename_utf8(staging, target_dir) != 0) {
        (void)nk_platform_remove_tree(staging, NULL);
        snprintf(text, sizeof(text),
                 "Nakagawa couldn't move the unpacked files into %.300s. Close any window "
                 "showing that folder and try again.", target_dir);
        set_error(error, "UPDATE_INSTALL_MOVE_FAILED", "Couldn't Install", text);
        return false;
    }
    return true;
#else
    (void)zip_path;
    (void)target_dir;
    (void)log_path;
    set_error(error, "UPDATE_PLATFORM_UNSUPPORTED", "Not Available Here",
              "Installing updates from Nakagawa works on Windows only for now.");
    return false;
#endif
}
