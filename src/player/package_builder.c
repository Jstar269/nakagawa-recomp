/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#if !defined(_WIN32) && !defined(_WIN64) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "package_builder.h"
#include "nk_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#endif

static inline void safe_str_copy(char *dest, size_t dest_size, const char *src);

static bool copy_json_string(const NkJsonNode *object, const char *key,
                             char *out, size_t out_size) {
    NkJsonNode *node = nk_json_obj_get(object, key);
    const char *value = nk_json_get_string(node);
    if (!node || !nk_json_is_string(node) || !value || !out || out_size == 0 ||
        strlen(value) >= out_size) return false;
    memcpy(out, value, strlen(value) + 1);
    return true;
}

static bool source_root_for_cli(const char *cli_path, char *out, size_t out_size) {
    char source_root[NK_MAX_PATH];
    size_t len;
    if (!cli_path || !cli_path[0] || !out || out_size == 0) return false;
    len = strlen(cli_path);
    if (len >= sizeof(source_root)) return false;
    memcpy(source_root, cli_path, len + 1);
    char *last = strrchr(source_root, '/');
    char *back = strrchr(source_root, '\\');
    if (!last || (back && back > last)) last = back;
    if (!last) return false;
    *last = '\0'; /* tools */
    last = strrchr(source_root, '/');
    back = strrchr(source_root, '\\');
    if (!last || (back && back > last)) last = back;
    if (last) *last = '\0'; /* source root */
    if (strlen(source_root) >= out_size) return false;
    memcpy(out, source_root, strlen(source_root) + 1);
    return true;
}

static bool manifest_path_for_cli(const char *cli_path, char *out, size_t out_size) {
    char source_root[NK_MAX_PATH];
    if (!source_root_for_cli(cli_path, source_root, sizeof(source_root))) return false;
    int n = snprintf(out, out_size, "%s%cassets%cprereq_manifest.json",
                     source_root, nk_platform_path_separator(), nk_platform_path_separator());
    return n > 0 && (size_t)n < out_size;
}

static bool prerequisite_url_host(const char *url, char *out, size_t out_size) {
    const char *host, *end;
    size_t len;
    if (!url || strncmp(url, "https://", 8) != 0) return false;
    host = url + 8;
    end = host;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    if (end == host || memchr(host, '@', (size_t)(end - host)) != NULL ||
        memchr(host, ':', (size_t)(end - host)) != NULL) return false;
    len = (size_t)(end - host);
    if (len >= out_size) return false;
    for (size_t i = 0; i < len; i++) {
        char c = host[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    out[len] = '\0';
    return true;
}

bool package_builder_load_prerequisites(const char *cli_path,
                                        PackagePrerequisiteList *out_list,
                                        char *error, size_t error_size) {
    char path[NK_MAX_PATH];
    char *contents = NULL;
    long file_size;
    FILE *file = NULL;
    NkJsonNode *root = NULL;
    bool ok = false;
    if (error && error_size) error[0] = '\0';
    if (!out_list || !manifest_path_for_cli(cli_path, path, sizeof(path))) {
        if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_NOT_FOUND: cannot locate the pinned manifest.");
        return false;
    }
    memset(out_list, 0, sizeof(*out_list));
    file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) <= 0 ||
        file_size > 2 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_NOT_FOUND: cannot read %s.", path);
        goto done;
    }
    contents = (char *)malloc((size_t)file_size);
    if (!contents || fread(contents, 1, (size_t)file_size, file) != (size_t)file_size) {
        if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_INVALID: cannot read the pinned manifest.");
        goto done;
    }
    char parse_error[160];
    root = nk_json_parse(contents, (size_t)file_size, parse_error, sizeof(parse_error));
    NkJsonNode *artifacts = nk_json_obj_get(root, "artifacts");
    if (!root || !nk_json_is_object(root) || !artifacts || !nk_json_is_array(artifacts)) {
        if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_INVALID: artifacts list is missing.");
        goto done;
    }
    size_t count = nk_json_array_count(artifacts);
    for (size_t i = 0; i < count; i++) {
        NkJsonNode *item = nk_json_array_get(artifacts, i);
        NkJsonNode *status_node = nk_json_obj_get(item, "status");
        const char *status = nk_json_get_string(status_node);
        if (!status || strcmp(status, "ready") != 0) continue;
        if (out_list->count >= PACKAGE_BUILDER_MAX_PREREQUISITES) {
            if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_INVALID: too many artifacts.");
            goto done;
        }
        PackagePrerequisite *entry = &out_list->items[out_list->count];
        NkJsonNode *url_node = nk_json_obj_get(item, "url");
        const char *url = nk_json_get_string(url_node);
        NkJsonNode *hosts = nk_json_obj_get(item, "allowed_hosts");
        NkJsonNode *size_node = nk_json_obj_get(item, "size_bytes");
        int64_t size = 0;
        char url_host[128];
        if (!nk_json_is_object(item) || !copy_json_string(item, "id", entry->id, sizeof(entry->id)) ||
            !copy_json_string(item, "name", entry->name, sizeof(entry->name)) ||
            !copy_json_string(item, "version", entry->version, sizeof(entry->version)) ||
            !copy_json_string(item, "url", entry->url, sizeof(entry->url)) ||
            !copy_json_string(item, "sha256", entry->sha256, sizeof(entry->sha256)) ||
            !copy_json_string(item, "filename", entry->filename, sizeof(entry->filename)) ||
            !copy_json_string(item, "license", entry->license, sizeof(entry->license)) ||
            !nk_json_is_array(hosts) || nk_json_array_count(hosts) == 0 ||
            !nk_json_get_int64(size_node, &size) || size <= 0 || !url ||
            !prerequisite_url_host(url, url_host, sizeof(url_host)) ||
            strlen(entry->sha256) != 64 ||
            out_list->total_bytes > UINT64_MAX - (uint64_t)size) {
            if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_INVALID: an artifact is missing valid pinned metadata.");
            goto done;
        }
        for (size_t h = 0; h < nk_json_array_count(hosts); h++) {
            const char *allowed = nk_json_get_string(nk_json_array_get(hosts, h));
            char normalized[128];
            if (!allowed || !allowed[0] || strlen(allowed) >= sizeof(normalized)) {
                if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_INVALID: an allow-listed host is malformed.");
                goto done;
            }
            size_t host_len = strlen(allowed);
            for (size_t c = 0; c < host_len; c++) {
                char ch = allowed[c];
                if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '.' || ch == '-')) {
                    if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_INVALID: an allow-listed host is malformed.");
                    goto done;
                }
                normalized[c] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : ch;
            }
            normalized[host_len] = '\0';
            if (h > 0) {
                size_t used = strlen(entry->allowed_hosts);
                if (used + 1 >= sizeof(entry->allowed_hosts)) goto done;
                entry->allowed_hosts[used++] = ';';
                entry->allowed_hosts[used] = '\0';
            }
            size_t used = strlen(entry->allowed_hosts);
            if (used + host_len >= sizeof(entry->allowed_hosts)) goto done;
            memcpy(entry->allowed_hosts + used, normalized, host_len + 1);
            if (strcmp(normalized, url_host) == 0) {
                snprintf(entry->host, sizeof(entry->host), "%s", normalized);
            }
        }
        if (!entry->host[0]) {
            if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_INVALID: the artifact URL host is not allow-listed.");
            goto done;
        }
        for (size_t c = 0; c < 64; c++) {
            char ch = entry->sha256[c];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                  (ch >= 'A' && ch <= 'F'))) {
                if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_INVALID: an artifact SHA-256 is malformed.");
                goto done;
            }
        }
        entry->size_bytes = (uint64_t)size;
        out_list->total_bytes += entry->size_bytes;
        out_list->count++;
    }
    if (out_list->count == 0) {
        if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_INVALID: no ready artifacts are available.");
        goto done;
    }
    {
        NkJsonNode *total_node = nk_json_obj_get(root, "total_download_bytes");
        int64_t total = 0;
        if (!nk_json_get_int64(total_node, &total) || total < 0 ||
            (uint64_t)total != out_list->total_bytes) {
            if (error && error_size) snprintf(error, error_size, "PREREQUISITE_MANIFEST_INVALID: declared download total does not match the artifact sizes.");
            goto done;
        }
    }
    ok = true;
done:
    if (root) nk_json_free(root);
    free(contents);
    if (file) fclose(file);
    return ok;
}

void package_builder_mark_prerequisites_installed(PackagePrerequisiteList *list,
                                                   const char *data_root) {
    if (!list || !data_root || !data_root[0]) return;
    char path[NK_MAX_PATH];
    int n = snprintf(path, sizeof(path), "%s%cprerequisites%cinstalled.json",
                     data_root, nk_platform_path_separator(), nk_platform_path_separator());
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    FILE *file = fopen(path, "rb");
    if (!file) return;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return; }
    long size = ftell(file);
    if (size <= 0 || size > 2 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return;
    }
    char *contents = (char *)malloc((size_t)size);
    if (!contents) { fclose(file); return; }
    if (fread(contents, 1, (size_t)size, file) != (size_t)size) {
        free(contents);
        fclose(file);
        return;
    }
    fclose(file);
    char parse_error[128];
    NkJsonNode *root = nk_json_parse(contents, (size_t)size, parse_error, sizeof(parse_error));
    free(contents);
    NkJsonNode *installed = nk_json_obj_get(root, "installed");
    if (!root || !nk_json_is_object(root) || !installed || !nk_json_is_object(installed)) {
        if (root) nk_json_free(root);
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        PackagePrerequisite *item = &list->items[i];
        NkJsonNode *record = nk_json_obj_get(installed, item->id);
        if (!record || !nk_json_is_object(record)) continue;
        item->installed = true;
        NkJsonNode *version = nk_json_obj_get(record, "version");
        NkJsonNode *license = nk_json_obj_get(record, "license");
        const char *version_value = nk_json_get_string(version);
        const char *license_value = nk_json_get_string(license);
        if (version_value && strlen(version_value) < sizeof(item->version))
            snprintf(item->version, sizeof(item->version), "%s", version_value);
        if (license_value && strlen(license_value) < sizeof(item->license))
            snprintf(item->license, sizeof(item->license), "%s", license_value);
        NkJsonNode *notices = nk_json_obj_get(record, "notices");
        const char *notice = nk_json_get_string(nk_json_array_get(notices, 0));
        if (notice && strlen(notice) < sizeof(item->notice_path)) {
            snprintf(item->notice_path, sizeof(item->notice_path), "%s", notice);
        }
    }
    nk_json_free(root);
}

#if !defined(_WIN32) && !defined(_WIN64)
static int ascii_ncasecmp(const char *left, const char *right, size_t count) {
    for (size_t i = 0; i < count; i++) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return (int)a - (int)b;
        if (!a) return 0;
    }
    return 0;
}
#endif

static bool allowed_url(const char *url, const char *allowed_hosts) {
    char host[128];
    if (!prerequisite_url_host(url, host, sizeof(host)) || !allowed_hosts) return false;
    const char *cur = allowed_hosts;
    while (*cur) {
        const char *end = strchr(cur, ';');
        size_t len = end ? (size_t)(end - cur) : strlen(cur);
#if defined(_WIN32) || defined(_WIN64)
        if (len == strlen(host) && _strnicmp(cur, host, len) == 0) return true;
#else
        if (len == strlen(host) && ascii_ncasecmp(cur, host, len) == 0) return true;
#endif
        if (!end) break;
        cur = end + 1;
    }
    return false;
}

#if defined(_WIN32) || defined(_WIN64)
typedef struct {
    HINTERNET session;
    HINTERNET connection;
    HINTERNET request;
    char final_url[2048];
} WinHttpDownload;

static bool winhttp_url_parts(const char *url, wchar_t *host, size_t host_count,
                              wchar_t *path, size_t path_count) {
    wchar_t wide_url[2048];
    URL_COMPONENTS parts;
    if (!url || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1,
                                    wide_url, (int)(sizeof(wide_url) / sizeof(wide_url[0]))) <= 0) return false;
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = (DWORD)-1;
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide_url, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
        !parts.lpszHostName || !parts.dwHostNameLength ||
        (size_t)parts.dwHostNameLength + 1 > host_count) return false;
    wcsncpy(host, parts.lpszHostName, parts.dwHostNameLength);
    host[parts.dwHostNameLength] = L'\0';
    size_t used = 0;
    if (parts.dwUrlPathLength) {
        if ((size_t)parts.dwUrlPathLength >= path_count) return false;
        wcsncpy(path, parts.lpszUrlPath, parts.dwUrlPathLength);
        used = parts.dwUrlPathLength;
    } else {
        if (path_count < 2) return false;
        path[used++] = L'/';
    }
    if (parts.dwExtraInfoLength) {
        if (used + (size_t)parts.dwExtraInfoLength >= path_count) return false;
        wcsncpy(path + used, parts.lpszExtraInfo, parts.dwExtraInfoLength);
        used += parts.dwExtraInfoLength;
    }
    path[used] = L'\0';
    return true;
}

static bool winhttp_read(void *context, unsigned char *buffer, size_t capacity,
                         size_t *bytes_read) {
    WinHttpDownload *state = (WinHttpDownload *)context;
    DWORD available = 0;
    DWORD received = 0;
    if (!state || !state->request || !buffer || !bytes_read || capacity > UINT32_MAX ||
        !WinHttpQueryDataAvailable(state->request, &available)) return false;
    if (available == 0) {
        *bytes_read = 0;
        return true;
    }
    DWORD want = available < (DWORD)capacity ? available : (DWORD)capacity;
    if (!WinHttpReadData(state->request, buffer, want, &received)) return false;
    *bytes_read = (size_t)received;
    return true;
}

static void winhttp_close(void *context) {
    WinHttpDownload *state = (WinHttpDownload *)context;
    if (!state) return;
    if (state->request) WinHttpCloseHandle(state->request);
    if (state->connection) WinHttpCloseHandle(state->connection);
    if (state->session) WinHttpCloseHandle(state->session);
    free(state);
}

static bool winhttp_open(void *context, const char *url, const char *allowed_hosts,
                         PackageHttpResponse *response) {
    (void)context;
    if (!allowed_url(url, allowed_hosts) || !response) return false;
    WinHttpDownload *state = (WinHttpDownload *)calloc(1, sizeof(*state));
    if (!state) return false;
    if (strlen(url) >= sizeof(state->final_url)) {
        free(state);
        return false;
    }
    snprintf(state->final_url, sizeof(state->final_url), "%s", url);
    state->session = WinHttpOpen(L"NakagawaRecomp/PrerequisiteBootstrap",
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!state->session) {
        winhttp_close(state);
        return false;
    }
    WinHttpSetTimeouts(state->session, 15000, 15000, 30000, 30000);
    for (int redirects = 0; redirects <= 5; redirects++) {
        wchar_t host[256];
        wchar_t path[2048];
        wchar_t location[2048];
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (!winhttp_url_parts(state->final_url, host, sizeof(host) / sizeof(host[0]),
                               path, sizeof(path) / sizeof(path[0]))) {
            winhttp_close(state);
            return false;
        }
        state->connection = WinHttpConnect(state->session, host,
                                            INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!state->connection) {
            winhttp_close(state);
            return false;
        }
        state->request = WinHttpOpenRequest(state->connection, L"GET", path,
                                             NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
        if (!state->request) {
            winhttp_close(state);
            return false;
        }
        DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(state->request, WINHTTP_OPTION_REDIRECT_POLICY,
                              &redirect_policy, sizeof(redirect_policy)) ||
            !WinHttpSendRequest(state->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(state->request, NULL) ||
            !WinHttpQueryHeaders(state->request,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            winhttp_close(state);
            return false;
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            DWORD location_size = sizeof(location);
            if (!WinHttpQueryHeaders(state->request, WINHTTP_QUERY_LOCATION,
                                     WINHTTP_HEADER_NAME_BY_INDEX, location,
                                     &location_size, WINHTTP_NO_HEADER_INDEX) ||
                !location[0]) {
                winhttp_close(state);
                return false;
            }
            int converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, location,
                                                 -1, state->final_url,
                                                 sizeof(state->final_url), NULL, NULL);
            if (converted <= 0) {
                winhttp_close(state);
                return false;
            }
            if (!allowed_url(state->final_url, allowed_hosts)) {
                response->final_url = state->final_url;
                response->context = state;
                response->read = winhttp_read;
                response->close = winhttp_close;
                response->has_content_length = false;
                response->content_length = 0;
                return true;
            }
            WinHttpCloseHandle(state->request);
            WinHttpCloseHandle(state->connection);
            state->request = NULL;
            state->connection = NULL;
            continue;
        }
        if (status != 200) {
            winhttp_close(state);
            return false;
        }
        response->has_content_length = false;
        response->content_length = 0;
        ULONGLONG content_length = 0;
        DWORD content_size = sizeof(content_length);
        if (WinHttpQueryHeaders(state->request,
                                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                                WINHTTP_HEADER_NAME_BY_INDEX, &content_length,
                                &content_size, WINHTTP_NO_HEADER_INDEX)) {
            response->has_content_length = true;
            response->content_length = (uint64_t)content_length;
        }
        response->final_url = state->final_url;
        response->context = state;
        response->read = winhttp_read;
        response->close = winhttp_close;
        return true;
    }
    winhttp_close(state);
    return false;
}

static bool sha256_begin(BCRYPT_ALG_HANDLE *algorithm, BCRYPT_HASH_HANDLE *hash,
                         unsigned char **hash_object) {
    DWORD object_size = 0;
    DWORD result_size = 0;
    if (BCryptOpenAlgorithmProvider(algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptGetProperty(*algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                          sizeof(object_size), &result_size, 0) < 0) return false;
    *hash_object = (unsigned char *)malloc(object_size);
    if (!*hash_object) return false;
    if (BCryptCreateHash(*algorithm, hash, *hash_object, object_size, NULL, 0, 0) < 0) {
        free(*hash_object);
        *hash_object = NULL;
        return false;
    }
    return true;
}
#endif

bool package_builder_download_verified(
    const PackagePrerequisite *item, const char *destination,
    PackageHttpOpen transport, void *transport_context,
    PackageDownloadContinue progress, void *progress_context,
    char *error_code, size_t error_code_size,
    char *error_message, size_t error_message_size) {
    PackageHttpResponse response;
    PackageHttpOpen open_transport = transport;
    FILE *partial_file = NULL;
    char partial_path[NK_MAX_PATH];
    unsigned char buffer[64 * 1024];
    uint64_t received = 0;
    bool ok = false;
    bool response_open = false;
#if defined(_WIN32) || defined(_WIN64)
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char *hash_object = NULL;
    unsigned char digest[32];
#endif
    if (error_code && error_code_size) error_code[0] = '\0';
    if (error_message && error_message_size) error_message[0] = '\0';
    memset(&response, 0, sizeof(response));
    if (!item || !destination || !item->size_bytes ||
        strlen(item->sha256) != 64 || !allowed_url(item->url, item->allowed_hosts)) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "MANIFEST_INVALID");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "The pinned download metadata is invalid. Restore the packaged prerequisite manifest.");
        return false;
    }
#if defined(_WIN32) || defined(_WIN64)
    if (!open_transport) open_transport = winhttp_open;
#else
    if (!open_transport) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "PLATFORM_UNSUPPORTED");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "Automatic prerequisite downloads currently support Windows x64 only. Linux build-tool installation is in the works (#306).");
        return false;
    }
#endif
    int partial_len = snprintf(partial_path, sizeof(partial_path), "%s.part", destination);
    if (partial_len < 0 || (size_t)partial_len >= sizeof(partial_path)) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "PATH_TOO_LONG");
        return false;
    }
    if (!open_transport(transport_context, item->url, item->allowed_hosts, &response)) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "OFFLINE_OR_NETWORK_ERROR");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "Could not reach %s. Check the connection and retry.", item->host);
        goto done;
    }
    response_open = true;
    if (!response.final_url || !allowed_url(response.final_url, item->allowed_hosts)) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "REDIRECT_REJECTED");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "The download redirected outside the approved HTTPS hosts (%s). Retry from the official source.", item->allowed_hosts);
        goto done;
    }
    if (!response.read || (response.has_content_length && response.content_length != item->size_bytes)) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "SIZE_MISMATCH");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "The server reported the wrong size for %s. Retry the download.", item->name);
        goto done;
    }
    partial_file = fopen(partial_path, "wb");
    if (!partial_file) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "DISK_FULL");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "Cannot write prerequisite data under app data. Free disk space or choose a writable profile, then retry.");
        goto done;
    }
#if defined(_WIN32) || defined(_WIN64)
    if (!sha256_begin(&algorithm, &hash, &hash_object)) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "HASH_UNAVAILABLE");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "Windows SHA-256 verification could not start. Restart the player and retry.");
        goto done;
    }
#endif
    for (;;) {
        size_t count = 0;
        if (!response.read(response.context, buffer, sizeof(buffer), &count)) {
            if (error_code && error_code_size) snprintf(error_code, error_code_size, "OFFLINE_OR_NETWORK_ERROR");
            if (error_message && error_message_size) snprintf(error_message, error_message_size,
                "The connection stopped while downloading %s. Retry the download.", item->name);
            goto done;
        }
        if (count == 0) break;
        if (received > item->size_bytes || count > item->size_bytes - received) {
            if (error_code && error_code_size) snprintf(error_code, error_code_size, "SIZE_MISMATCH");
            if (error_message && error_message_size) snprintf(error_message, error_message_size,
                "The downloaded body for %s is larger than its pinned size.", item->name);
            goto done;
        }
        if (fwrite(buffer, 1, count, partial_file) != count) {
            if (error_code && error_code_size) snprintf(error_code, error_code_size, "DISK_FULL");
            if (error_message && error_message_size) snprintf(error_message, error_message_size,
                "App data ran out of writable space while saving %s. Free disk space and retry.", item->name);
            goto done;
        }
#if defined(_WIN32) || defined(_WIN64)
        if (BCryptHashData(hash, buffer, (ULONG)count, 0) < 0) {
            if (error_code && error_code_size) snprintf(error_code, error_code_size, "HASH_UNAVAILABLE");
            if (error_message && error_message_size) snprintf(error_message, error_message_size,
                "Windows SHA-256 verification failed while downloading %s.", item->name);
            goto done;
        }
#endif
        received += count;
        if (progress && !progress(progress_context, received, item->size_bytes)) {
            if (error_code && error_code_size) snprintf(error_code, error_code_size, "INSTALL_CANCELLED");
            if (error_message && error_message_size) snprintf(error_message, error_message_size,
                "The download was cancelled. Temporary bytes were discarded.");
            goto done;
        }
    }
    if (received != item->size_bytes) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "TRUNCATED_BODY");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "The download ended at %llu of %llu bytes for %s. Retry the download.",
            (unsigned long long)received, (unsigned long long)item->size_bytes, item->name);
        goto done;
    }
#if defined(_WIN32) || defined(_WIN64)
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "HASH_UNAVAILABLE");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "Windows SHA-256 verification could not finish for %s.", item->name);
        goto done;
    }
    {
        static const char hex[] = "0123456789abcdef";
        char actual[65];
        for (size_t i = 0; i < sizeof(digest); i++) {
            actual[i * 2] = hex[digest[i] >> 4];
            actual[i * 2 + 1] = hex[digest[i] & 15];
        }
        actual[64] = '\0';
        if (_stricmp(actual, item->sha256) != 0) {
            if (error_code && error_code_size) snprintf(error_code, error_code_size, "HASH_MISMATCH");
            if (error_message && error_message_size) snprintf(error_message, error_message_size,
                "SHA-256 did not match the pinned value for %s. The file was discarded; retry from the official source.", item->name);
            goto done;
        }
    }
#else
    if (error_code && error_code_size) snprintf(error_code, error_code_size, "PLATFORM_UNSUPPORTED");
    if (error_message && error_message_size) snprintf(error_message, error_message_size,
        "Native SHA-256 verification is supported on Windows only.");
    goto done;
#endif
    if (fflush(partial_file) != 0) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "DISK_FULL");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "Could not flush the verified download to disk. Free disk space and retry.");
        goto done;
    }
    if (fclose(partial_file) != 0) {
        partial_file = NULL;
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "DISK_FULL");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "Could not close the verified download. Free disk space and retry.");
        goto done;
    }
    partial_file = NULL;
#if defined(_WIN32) || defined(_WIN64)
    if (!MoveFileExA(partial_path, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
    if (rename(partial_path, destination) != 0) {
#endif
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "DESTINATION_WRITE_FAILED");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "Could not save the verified prerequisite in app data. Check permissions and free disk space.");
        goto done;
    }
    ok = true;
done:
    if (partial_file) fclose(partial_file);
    if (!ok) remove(partial_path);
    if (response_open && response.close) response.close(response.context);
#if defined(_WIN32) || defined(_WIN64)
    if (hash) BCryptDestroyHash(hash);
    free(hash_object);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
#endif
    return ok;
}

#if defined(_WIN32) || defined(_WIN64)
static bool bootstrap_remove_tree(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND ||
                                                          GetLastError() == ERROR_PATH_NOT_FOUND;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileA(path) != 0;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryA(path) != 0;
    char pattern[NK_MAX_PATH];
    int n = snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if (n < 0 || (size_t)n >= sizeof(pattern)) return false;
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
            char child[NK_MAX_PATH];
            n = snprintf(child, sizeof(child), "%s\\%s", path, data.cFileName);
            if (n < 0 || (size_t)n >= sizeof(child) || !bootstrap_remove_tree(child)) {
                FindClose(find);
                return false;
            }
        } while (FindNextFileA(find, &data));
        FindClose(find);
    }
    return RemoveDirectoryA(path) != 0;
}

static bool bootstrap_cancel_requested(PackageBootstrapSession *session) {
    return InterlockedCompareExchange((volatile LONG *)&session->cancel_requested, 0, 0) != 0;
}

static bool bootstrap_report_progress(void *context, uint64_t received,
                                       uint64_t expected) {
    PackageBootstrapSession *session = (PackageBootstrapSession *)context;
    (void)expected;
    InterlockedExchange64((volatile LONG64 *)&session->received_bytes, (LONG64)received);
    return !bootstrap_cancel_requested(session);
}

static bool bootstrap_expand_python(PackageBootstrapSession *session) {
    char system_directory[MAX_PATH];
    char powershell[NK_MAX_PATH];
    DWORD len = GetSystemDirectoryA(system_directory, sizeof(system_directory));
    if (!len || len >= sizeof(system_directory)) return false;
    int n = snprintf(powershell, sizeof(powershell),
                     "%s\\WindowsPowerShell\\v1.0\\powershell.exe", system_directory);
    if (n < 0 || (size_t)n >= sizeof(powershell)) return false;
    const char *argv[] = {
        powershell, "-NoProfile", "-NonInteractive", "-Command",
        "try { Expand-Archive -LiteralPath $env:NK_PREREQ_ARCHIVE -DestinationPath $env:NK_PREREQ_STAGE -Force -ErrorAction Stop; exit 0 } catch { exit 1 }",
        NULL
    };
    char archive_env[NK_MAX_PATH + 32];
    char stage_env[NK_MAX_PATH + 32];
    snprintf(archive_env, sizeof(archive_env), "NK_PREREQ_ARCHIVE=%s", session->archive_path);
    snprintf(stage_env, sizeof(stage_env), "NK_PREREQ_STAGE=%s", session->staging_path);
    const char *envp[] = { archive_env, stage_env, NULL };
    NkProcessHandle process;
    if (!nk_platform_spawn_process(powershell, argv, envp, NULL, &process)) return false;
    while (process.is_active && nk_platform_is_process_running(&process)) {
        if (bootstrap_cancel_requested(session)) {
            nk_platform_terminate_process(&process);
            break;
        }
        (void)nk_platform_wait_process(&process, 250);
    }
    int exit_code = nk_platform_wait_process(&process, 10000);
    nk_platform_close_process(&process);
    return exit_code == 0 && !bootstrap_cancel_requested(session);
}

static DWORD WINAPI bootstrap_python_thread(void *context) {
    PackageBootstrapSession *session = (PackageBootstrapSession *)context;
    char download_dir[NK_MAX_PATH];
    char item_dir[NK_MAX_PATH];
    char final_python_dir[NK_MAX_PATH];
    char staged_python[NK_MAX_PATH];
    char code[48] = "";
    char message[512] = "";
    int n;
    bool succeeded = false;
    memset(final_python_dir, 0, sizeof(final_python_dir));
    memset(staged_python, 0, sizeof(staged_python));
    if (bootstrap_cancel_requested(session)) {
        snprintf(code, sizeof(code), "INSTALL_CANCELLED");
        snprintf(message, sizeof(message), "The download was cancelled before it started.");
        goto finished;
    }
    if (!nk_platform_mkdir_p(session->prerequisites_root)) {
        snprintf(code, sizeof(code), "DISK_FULL");
        snprintf(message, sizeof(message), "Cannot create the app-data prerequisite folder. Free disk space and retry.");
        goto finished;
    }
    n = snprintf(download_dir, sizeof(download_dir), "%s\\downloads", session->prerequisites_root);
    if (n < 0 || (size_t)n >= sizeof(download_dir) || !nk_platform_mkdir_p(download_dir)) {
        snprintf(code, sizeof(code), "DISK_FULL");
        snprintf(message, sizeof(message), "Cannot create the prerequisite download folder. Free disk space and retry.");
        goto finished;
    }
    n = snprintf(item_dir, sizeof(item_dir), "%s\\%s", download_dir, session->item.id);
    if (n < 0 || (size_t)n >= sizeof(item_dir) || !nk_platform_mkdir_p(item_dir)) {
        snprintf(code, sizeof(code), "DISK_FULL");
        snprintf(message, sizeof(message), "Cannot create the CPython download folder. Free disk space and retry.");
        goto finished;
    }
    n = snprintf(session->archive_path, sizeof(session->archive_path), "%s\\%s",
                 item_dir, session->item.filename);
    if (n < 0 || (size_t)n >= sizeof(session->archive_path)) {
        snprintf(code, sizeof(code), "PATH_TOO_LONG");
        snprintf(message, sizeof(message), "The app-data path is too long for the CPython archive.");
        goto finished;
    }
    n = snprintf(session->staging_path, sizeof(session->staging_path), "%s\\python.pending",
                 session->prerequisites_root);
    if (n < 0 || (size_t)n >= sizeof(session->staging_path)) {
        snprintf(code, sizeof(code), "PATH_TOO_LONG");
        snprintf(message, sizeof(message), "The app-data path is too long for CPython staging.");
        goto finished;
    }
    n = snprintf(final_python_dir, sizeof(final_python_dir), "%s\\python",
                 session->prerequisites_root);
    if (n < 0 || (size_t)n >= sizeof(final_python_dir)) {
        snprintf(code, sizeof(code), "PATH_TOO_LONG");
        snprintf(message, sizeof(message), "The app-data path is too long for the CPython install.");
        goto finished;
    }
    if (!bootstrap_remove_tree(session->staging_path) || !nk_platform_mkdir_p(session->staging_path)) {
        snprintf(code, sizeof(code), "DISK_WRITE_FAILED");
        snprintf(message, sizeof(message), "The CPython staging folder could not be prepared. Close files in app data and retry.");
        goto finished;
    }
    char verify_code[48];
    char verify_message[512];
    if (!package_builder_download_verified(&session->item, session->archive_path,
            NULL, NULL, bootstrap_report_progress, session,
            verify_code, sizeof(verify_code), verify_message, sizeof(verify_message))) {
        snprintf(code, sizeof(code), "%s", verify_code[0] ? verify_code : "OFFLINE_OR_NETWORK_ERROR");
        snprintf(message, sizeof(message), "%s", verify_message[0] ? verify_message :
                 "The CPython download did not pass verification. Check the connection and retry.");
        goto finished;
    }
    if (bootstrap_cancel_requested(session)) {
        snprintf(code, sizeof(code), "INSTALL_CANCELLED");
        snprintf(message, sizeof(message), "The download was cancelled. Temporary extraction data was discarded.");
        goto finished;
    }
    if (!bootstrap_expand_python(session)) {
        if (bootstrap_cancel_requested(session)) {
            snprintf(code, sizeof(code), "INSTALL_CANCELLED");
            snprintf(message, sizeof(message), "The CPython extraction was cancelled and its staging folder was removed.");
        } else {
            snprintf(code, sizeof(code), "PYTHON_ARCHIVE_EXTRACT_FAILED");
            snprintf(message, sizeof(message), "Windows could not extract the verified CPython archive. Check app-data permissions and retry.");
        }
        goto finished;
    }
    n = snprintf(staged_python, sizeof(staged_python), "%s\\python.exe", session->staging_path);
    if (n < 0 || (size_t)n >= sizeof(staged_python) || !nk_platform_file_exists(staged_python)) {
        snprintf(code, sizeof(code), "PYTHON_ARCHIVE_INVALID");
        snprintf(message, sizeof(message), "The verified CPython archive did not contain python.exe. Restore the pinned manifest.");
        goto finished;
    }
    if (!bootstrap_remove_tree(final_python_dir) ||
        !MoveFileExA(session->staging_path, final_python_dir, MOVEFILE_WRITE_THROUGH)) {
        snprintf(code, sizeof(code), "DISK_WRITE_FAILED");
        snprintf(message, sizeof(message), "The verified CPython files could not be promoted into app data. Check permissions and retry.");
        goto finished;
    }
    succeeded = true;
finished:
    if (!succeeded && session->staging_path[0]) (void)bootstrap_remove_tree(session->staging_path);
    snprintf(session->error_code, sizeof(session->error_code), "%s", code);
    snprintf(session->error_message, sizeof(session->error_message), "%s", message);
    session->succeeded = succeeded;
    InterlockedExchange((volatile LONG *)&session->done, 1);
    return 0;
}
#endif

bool package_builder_remove_downloaded_tools(const char *data_root,
                                            char *error_code, size_t error_code_size,
                                            char *error_message, size_t error_message_size) {
    if (error_code && error_code_size) error_code[0] = '\0';
    if (error_message && error_message_size) error_message[0] = '\0';
#if defined(_WIN32) || defined(_WIN64)
    if (!data_root || !data_root[0]) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "DATA_DIR_UNAVAILABLE");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "The app-data folder is unavailable. No files were removed.");
        return false;
    }
    char path[NK_MAX_PATH];
    int n = snprintf(path, sizeof(path), "%s\\prerequisites", data_root);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "PATH_TOO_LONG");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "The downloaded build-tools path is too long. No files were removed.");
        return false;
    }
    if (!bootstrap_remove_tree(path)) {
        if (error_code && error_code_size) snprintf(error_code, error_code_size, "TOOLS_REMOVE_FAILED");
        if (error_message && error_message_size) snprintf(error_message, error_message_size,
            "Some downloaded tools could not be removed. Close applications using them, then retry.");
        return false;
    }
    return true;
#else
    (void)data_root;
    if (error_code && error_code_size) snprintf(error_code, error_code_size, "PLATFORM_UNSUPPORTED");
    if (error_message && error_message_size) snprintf(error_message, error_message_size,
        "Removing downloaded build tools is currently supported on Windows only.");
    return false;
#endif
}

bool package_builder_bootstrap_python_start(
    PackageBootstrapSession *session, const PackagePrerequisite *python_item,
    const char *data_root) {
#if defined(_WIN32) || defined(_WIN64)
    if (!session || !python_item || !data_root || !data_root[0] ||
        strcmp(python_item->id, "cpython-embed-amd64") != 0 ||
        !python_item->filename[0] || strchr(python_item->filename, '/') ||
        strchr(python_item->filename, '\\') || strcmp(python_item->filename, ".") == 0 ||
        strcmp(python_item->filename, "..") == 0) return false;
    memset(session, 0, sizeof(*session));
    session->item = *python_item;
    int n = snprintf(session->prerequisites_root, sizeof(session->prerequisites_root),
                     "%s\\prerequisites", data_root);
    if (n < 0 || (size_t)n >= sizeof(session->prerequisites_root)) return false;
    n = snprintf(session->staging_path, sizeof(session->staging_path),
                 "%s\\python.pending", session->prerequisites_root);
    if (n < 0 || (size_t)n >= sizeof(session->staging_path)) return false;
    n = snprintf(session->python_path, sizeof(session->python_path),
                 "%s\\python\\python.exe", session->prerequisites_root);
    if (n < 0 || (size_t)n >= sizeof(session->python_path)) return false;
    HANDLE thread = CreateThread(NULL, 0, bootstrap_python_thread, session, 0, NULL);
    if (!thread) return false;
    session->thread_handle = thread;
    return true;
#else
    (void)session; (void)python_item; (void)data_root;
    return false;
#endif
}

bool package_builder_bootstrap_python_poll(PackageBootstrapSession *session,
                                          uint64_t *received_bytes,
                                          bool *finished, bool *succeeded,
                                          char *error_code, size_t error_code_size,
                                          char *error_message, size_t error_message_size) {
    if (!session) return false;
#if defined(_WIN32) || defined(_WIN64)
    if (received_bytes) *received_bytes = (uint64_t)InterlockedCompareExchange64(
        (volatile LONG64 *)&session->received_bytes, 0, 0);
    bool is_done = InterlockedCompareExchange((volatile LONG *)&session->done, 0, 0) != 0;
    if (finished) *finished = is_done;
    if (succeeded) *succeeded = is_done && session->succeeded;
    if (error_code && error_code_size) snprintf(error_code, error_code_size, "%s", session->error_code);
    if (error_message && error_message_size) snprintf(error_message, error_message_size, "%s", session->error_message);
    return true;
#else
    if (received_bytes) *received_bytes = 0;
    if (finished) *finished = true;
    if (succeeded) *succeeded = false;
    if (error_code && error_code_size) snprintf(error_code, error_code_size, "PLATFORM_UNSUPPORTED");
    if (error_message && error_message_size) snprintf(error_message, error_message_size,
        "Native prerequisite bootstrap is supported on Windows only.");
    return false;
#endif
}

void package_builder_bootstrap_python_cancel(PackageBootstrapSession *session) {
#if defined(_WIN32) || defined(_WIN64)
    if (session) InterlockedExchange((volatile LONG *)&session->cancel_requested, 1);
#else
    (void)session;
#endif
}

void package_builder_bootstrap_python_close(PackageBootstrapSession *session) {
#if defined(_WIN32) || defined(_WIN64)
    if (!session || !session->thread_handle) return;
    WaitForSingleObject((HANDLE)session->thread_handle, INFINITE);
    CloseHandle((HANDLE)session->thread_handle);
    session->thread_handle = NULL;
#else
    (void)session;
#endif
}

NkResult package_builder_start_prerequisite_fetch(
    PackageBuildSession *session, const char *python_path, const char *cli_path,
    const char *data_root, const char *log_dir, bool bootstrapped_python) {
    if (!session || !python_path || !cli_path || !data_root || !data_root[0]) return NK_ERROR_GENERIC;
    (void)bootstrapped_python;
    session->current_stage = PACKAGE_BUILD_STAGE_IDLE;
    session->current_stage_name[0] = '\0';
    session->current_message[0] = '\0';
    session->failure_boundary[0] = '\0';
    session->current_item_id[0] = '\0';
    session->failure_code[0] = '\0';
    session->item_received_bytes = 0;
    session->item_total_bytes = 0;
    session->total_received_bytes = 0;
    session->total_bytes = 0;
    session->prerequisite_install_complete = false;
    session->output_line_count = 0;
    session->output_line_head = 0;
    session->is_building = false;
    session->is_complete = false;
    session->is_failed = false;
    session->is_cancelled = false;
    session->exit_code = -1;
    char source_root[NK_MAX_PATH];
    char manifest_path[NK_MAX_PATH];
    char prereq_root[NK_MAX_PATH];
    PackagePrerequisiteList list;
    char manifest_error[256] = "";
    if (!source_root_for_cli(cli_path, source_root, sizeof(source_root)) ||
        !package_builder_load_prerequisites(cli_path, &list, manifest_error, sizeof(manifest_error))) {
        safe_str_copy(session->failure_code, sizeof(session->failure_code), "PREREQUISITE_MANIFEST_INVALID");
        safe_str_copy(session->failure_boundary, sizeof(session->failure_boundary),
                      manifest_error[0] ? manifest_error : "The pinned prerequisite manifest is unavailable.");
        return NK_ERROR_IO;
    }
    int n = snprintf(manifest_path, sizeof(manifest_path), "%s%cassets%cprereq_manifest.json",
                     source_root, nk_platform_path_separator(), nk_platform_path_separator());
    if (n < 0 || (size_t)n >= sizeof(manifest_path)) return NK_ERROR_GENERIC;
    n = snprintf(prereq_root, sizeof(prereq_root), "%s%cprerequisites",
                 data_root, nk_platform_path_separator());
    if (n < 0 || (size_t)n >= sizeof(prereq_root) || !nk_platform_mkdir_p(prereq_root)) {
        safe_str_copy(session->failure_code, sizeof(session->failure_code), "DISK_FULL");
        safe_str_copy(session->failure_boundary, sizeof(session->failure_boundary),
                      "Cannot create the app-data prerequisite folder. Free disk space and retry.");
        return NK_ERROR_IO;
    }
    n = snprintf(session->progress_file_path, sizeof(session->progress_file_path),
                 "%s%cinstall-progress.jsonl", prereq_root, nk_platform_path_separator());
    if (n < 0 || (size_t)n >= sizeof(session->progress_file_path)) return NK_ERROR_GENERIC;
    n = snprintf(session->cancel_file_path, sizeof(session->cancel_file_path),
                 "%s%cinstall-cancel.signal", prereq_root, nk_platform_path_separator());
    if (n < 0 || (size_t)n >= sizeof(session->cancel_file_path)) return NK_ERROR_GENERIC;
    remove(session->progress_file_path);
    remove(session->cancel_file_path);
    session->progress_file_offset = 0;
    session->log_file_path[0] = '\0';
    const char *ldir = (log_dir && log_dir[0]) ? log_dir : prereq_root;
    if (!nk_platform_mkdir_p(ldir)) return NK_ERROR_IO;

    uint64_t progress_base = 0;
    char base_env[80];
    char total_env[80];
    char source_env[NK_MAX_PATH + 40];
    char manifest_env[NK_MAX_PATH + 40];
    char data_env[NK_MAX_PATH + 40];
    char progress_env[NK_MAX_PATH + 40];
    char cancel_env[NK_MAX_PATH + 40];
    snprintf(base_env, sizeof(base_env), "NK_PREREQ_PROGRESS_BASE=%llu",
             (unsigned long long)progress_base);
    snprintf(total_env, sizeof(total_env), "NK_PREREQ_TOTAL_BYTES=%llu",
             (unsigned long long)list.total_bytes);
    snprintf(source_env, sizeof(source_env), "NK_PREREQ_SOURCE_ROOT=%s", source_root);
    snprintf(manifest_env, sizeof(manifest_env), "NK_PREREQ_MANIFEST=%s", manifest_path);
    snprintf(data_env, sizeof(data_env), "NK_PREREQ_DATA_ROOT=%s", data_root);
    snprintf(progress_env, sizeof(progress_env), "NK_PREREQ_PROGRESS_FILE=%s", session->progress_file_path);
    snprintf(cancel_env, sizeof(cancel_env), "NK_PREREQ_CANCEL_FILE=%s", session->cancel_file_path);
    const char *envp[] = {
        base_env, total_env, source_env, manifest_env, data_env, progress_env, cancel_env, NULL
    };
    const char *code =
        "import os,runpy,sys; "
        "sys.path.insert(0,os.environ['NK_PREREQ_SOURCE_ROOT']); "
        "sys.argv=['tools.nk_core.prereq_fetcher','--manifest',os.environ['NK_PREREQ_MANIFEST'],"
        "'--data-root',os.environ['NK_PREREQ_DATA_ROOT'],'--progress-file',os.environ['NK_PREREQ_PROGRESS_FILE'],"
        "'--cancel-file',os.environ['NK_PREREQ_CANCEL_FILE'],'--progress-base',os.environ['NK_PREREQ_PROGRESS_BASE'],"
        "'--total-bytes',os.environ['NK_PREREQ_TOTAL_BYTES']]; "
        "runpy.run_module('tools.nk_core.prereq_fetcher',run_name='__main__')";
    const char *argv[] = { python_path, "-c", code, NULL };
    if (!nk_platform_spawn_process(python_path, argv, envp, source_root, &session->process)) {
        safe_str_copy(session->failure_code, sizeof(session->failure_code), "SPAWN_FAILED");
        safe_str_copy(session->failure_boundary, sizeof(session->failure_boundary),
                      "Could not start the verified Python prerequisite fetcher.");
        return NK_ERROR_PROCESS_SPAWN;
    }
    session->current_stage = PACKAGE_BUILD_STAGE_PREFLIGHT;
    safe_str_copy(session->current_stage_name, sizeof(session->current_stage_name), "preflight");
    safe_str_copy(session->current_message, sizeof(session->current_message), "Preparing pinned build prerequisites...");
    session->is_building = true;
    session->is_complete = false;
    session->is_failed = false;
    session->is_cancelled = false;
    session->exit_code = -1;
    session->start_time_ms = 0;
    return NK_OK;
}

void package_builder_request_prerequisite_cancel(
    PackageBuildSession *session, const char *cancel_file_path) {
    if (!session || !session->is_building) return;
    const char *path = cancel_file_path && cancel_file_path[0]
        ? cancel_file_path : session->cancel_file_path;
    if (!path || !path[0]) return;
    FILE *cancel = fopen(path, "wb");
    if (cancel) {
        fputs("cancel\n", cancel);
        fclose(cancel);
    }
}

static inline void safe_str_copy(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0) return;
    if (!src) { dest[0] = '\0'; return; }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static const char *skip_whitespace(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

bool package_builder_parse_progress_line(
    const char *line,
    size_t line_len,
    PackageProgressEvent *out_event
) {
    if (!line || line_len == 0 || !out_event) return false;
    memset(out_event, 0, sizeof(*out_event));

    const char *start = skip_whitespace(line);
    if (*start != '{') return false;

    const char *end = line + line_len;
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    size_t trimmed_len = (size_t)(end - start);
    if (trimmed_len < 2 || start[trimmed_len - 1] != '}') return false;

    char err_buf[128];
    NkJsonNode *root = nk_json_parse(start, trimmed_len, err_buf, sizeof(err_buf));
    if (!root || !nk_json_is_object(root)) {
        if (root) nk_json_free(root);
        return false;
    }

    NkJsonNode *n_event = nk_json_obj_get(root, "event");
    const char *event_name = nk_json_get_string(n_event);
    if (event_name && strcmp(event_name, "download-progress") == 0) {
        NkJsonNode *n_item = nk_json_obj_get(root, "item_id");
        NkJsonNode *n_received = nk_json_obj_get(root, "received_bytes");
        NkJsonNode *n_expected = nk_json_obj_get(root, "expected_bytes");
        NkJsonNode *n_total_received = nk_json_obj_get(root, "total_received_bytes");
        NkJsonNode *n_total = nk_json_obj_get(root, "total_bytes");
        int64_t received = 0, expected = 0, total_received = 0, total = 0;
        const char *item_id = nk_json_get_string(n_item);
        if (!item_id || !nk_json_get_int64(n_received, &received) || received < 0 ||
            !nk_json_get_int64(n_expected, &expected) || expected < 0 ||
            !nk_json_get_int64(n_total_received, &total_received) || total_received < 0 ||
            !nk_json_get_int64(n_total, &total) || total < 0) {
            nk_json_free(root);
            return false;
        }
        safe_str_copy(out_event->item_id, sizeof(out_event->item_id), item_id);
        out_event->item_received_bytes = (uint64_t)received;
        out_event->item_total_bytes = (uint64_t)expected;
        out_event->total_received_bytes = (uint64_t)total_received;
        out_event->total_bytes = (uint64_t)total;
        snprintf(out_event->stage, sizeof(out_event->stage), "preflight");
        snprintf(out_event->status, sizeof(out_event->status), "RUNNING");
        snprintf(out_event->message, sizeof(out_event->message),
                 "Downloading %s (%llu / %llu bytes)", item_id,
                 (unsigned long long)received, (unsigned long long)expected);
        out_event->stage_enum = PACKAGE_BUILD_STAGE_PREFLIGHT;
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_RUNNING;
        nk_json_free(root);
        return true;
    }
    if (event_name && strcmp(event_name, "install-complete") == 0) {
        snprintf(out_event->stage, sizeof(out_event->stage), "package");
        snprintf(out_event->status, sizeof(out_event->status), "PASS");
        snprintf(out_event->message, sizeof(out_event->message), "Build prerequisites installed.");
        out_event->stage_enum = PACKAGE_BUILD_STAGE_PACKAGE;
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_PASS;
        out_event->install_complete = true;
        nk_json_free(root);
        return true;
    }
    if (event_name && strcmp(event_name, "install-failed") == 0) {
        NkJsonNode *n_code = nk_json_obj_get(root, "code");
        NkJsonNode *n_message = nk_json_obj_get(root, "message");
        const char *code = nk_json_get_string(n_code);
        const char *message = nk_json_get_string(n_message);
        if (!code || !message) {
            nk_json_free(root);
            return false;
        }
        safe_str_copy(out_event->error_code, sizeof(out_event->error_code), code);
        safe_str_copy(out_event->message, sizeof(out_event->message), message);
        snprintf(out_event->stage, sizeof(out_event->stage), "preflight");
        snprintf(out_event->status, sizeof(out_event->status), "FAIL");
        out_event->stage_enum = PACKAGE_BUILD_STAGE_PREFLIGHT;
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_FAIL;
        nk_json_free(root);
        return true;
    }

    NkJsonNode *n_stage = nk_json_obj_get(root, "stage");
    NkJsonNode *n_status = nk_json_obj_get(root, "status");
    NkJsonNode *n_message = nk_json_obj_get(root, "message");

    if (!n_stage || !nk_json_is_string(n_stage) ||
        !n_status || !nk_json_is_string(n_status) ||
        !n_message || !nk_json_is_string(n_message)) {
        nk_json_free(root);
        return false;
    }

    safe_str_copy(out_event->stage, sizeof(out_event->stage), nk_json_get_string(n_stage));
    safe_str_copy(out_event->status, sizeof(out_event->status), nk_json_get_string(n_status));
    safe_str_copy(out_event->message, sizeof(out_event->message), nk_json_get_string(n_message));
    nk_json_free(root);

    /* Map stage string to enum */
    if (strcmp(out_event->stage, "preflight") == 0) {
        out_event->stage_enum = PACKAGE_BUILD_STAGE_PREFLIGHT;
    } else if (strcmp(out_event->stage, "extract") == 0) {
        out_event->stage_enum = PACKAGE_BUILD_STAGE_EXTRACT;
    } else if (strcmp(out_event->stage, "codegen") == 0 ||
               strcmp(out_event->stage, "compile") == 0) {
        out_event->stage_enum = PACKAGE_BUILD_STAGE_COMPILE;
    } else if (strcmp(out_event->stage, "package") == 0 ||
               strcmp(out_event->stage, "build_package") == 0) {
        out_event->stage_enum = PACKAGE_BUILD_STAGE_PACKAGE;
    } else {
        out_event->stage_enum = PACKAGE_BUILD_STAGE_IDLE;
    }

    /* Map status string to enum */
    if (strcmp(out_event->status, "START") == 0) {
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_START;
    } else if (strcmp(out_event->status, "RUNNING") == 0) {
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_RUNNING;
    } else if (strcmp(out_event->status, "PASS") == 0) {
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_PASS;
    } else if (strcmp(out_event->status, "FAIL") == 0) {
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_FAIL;
    } else {
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_UNKNOWN;
    }

    return true;
}

void package_builder_init_session(
    PackageBuildSession *session,
    const char *disc_id,
    const char *title_name
) {
    if (!session) return;
    memset(session, 0, sizeof(*session));
    safe_str_copy(session->disc_id, sizeof(session->disc_id), disc_id);
    safe_str_copy(session->title_name, sizeof(session->title_name), title_name);
    session->current_stage = PACKAGE_BUILD_STAGE_IDLE;
    safe_str_copy(session->current_stage_name, sizeof(session->current_stage_name), "idle");
}

void package_builder_add_output_line(
    PackageBuildSession *session,
    const char *line
) {
    if (!session || !line) return;
    int slot = (session->output_line_head + session->output_line_count) % PACKAGE_BUILD_MAX_OUTPUT_LINES;
    if (session->output_line_count < PACKAGE_BUILD_MAX_OUTPUT_LINES) {
        session->output_line_count++;
    } else {
        session->output_line_head = (session->output_line_head + 1) % PACKAGE_BUILD_MAX_OUTPUT_LINES;
    }
    safe_str_copy(session->output_lines[slot], PACKAGE_BUILD_LINE_LEN, line);
}

const char *package_builder_get_output_line(
    const PackageBuildSession *session,
    int index
) {
    if (!session || index < 0 || index >= session->output_line_count) return "";
    int slot = (session->output_line_head + index) % PACKAGE_BUILD_MAX_OUTPUT_LINES;
    return session->output_lines[slot];
}

void package_builder_apply_event(
    PackageBuildSession *session,
    const PackageProgressEvent *event
) {
    if (!session || !event) return;

    if (event->stage[0]) {
        safe_str_copy(session->current_stage_name, sizeof(session->current_stage_name), event->stage);
        session->current_stage = event->stage_enum;
    }
    if (event->message[0]) {
        safe_str_copy(session->current_message, sizeof(session->current_message), event->message);
    }
    if (event->item_id[0]) {
        safe_str_copy(session->current_item_id, sizeof(session->current_item_id), event->item_id);
        session->item_received_bytes = event->item_received_bytes;
        session->item_total_bytes = event->item_total_bytes;
        session->total_received_bytes = event->total_received_bytes;
        session->total_bytes = event->total_bytes;
    }
    if (event->error_code[0]) safe_str_copy(session->failure_code, sizeof(session->failure_code), event->error_code);
    if (event->install_complete) session->prerequisite_install_complete = true;

    char line_buf[PACKAGE_BUILD_LINE_LEN];
    snprintf(line_buf, sizeof(line_buf), "[%.31s] %.31s: %.180s",
             event->stage, event->status, event->message);
    package_builder_add_output_line(session, line_buf);

    if (event->status_enum == PACKAGE_PROGRESS_STATUS_FAIL) {
        session->current_stage = PACKAGE_BUILD_STAGE_FAILED;
        session->is_failed = true;
        safe_str_copy(session->failure_boundary, sizeof(session->failure_boundary), event->message);
    } else if (event->stage_enum == PACKAGE_BUILD_STAGE_PACKAGE &&
               event->status_enum == PACKAGE_PROGRESS_STATUS_PASS) {
        session->current_stage = PACKAGE_BUILD_STAGE_COMPLETE;
        session->is_complete = true;
    }
}

/* Scan PATH for the first of names that exists as a file. Shared by the
 * interpreter and toolchain lookups: both must see exactly the PATH a spawned
 * build child would see, with no hidden fallback location. */
static bool scan_path_for_names(const char *const *names, size_t name_count,
                                char *out_path, size_t out_size) {
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0]) return false;

    char path_copy[32768];
    safe_str_copy(path_copy, sizeof(path_copy), path_env);

#if defined(_WIN32) || defined(_WIN64)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    char *dir = path_copy;
    while (dir) {
        char *next = strchr(dir, sep);
        if (next) *next++ = '\0';
        /* A PATH entry too long for a candidate path cannot name a usable tool. */
        if (dir[0] && strlen(dir) < NK_MAX_PATH) {
            for (size_t i = 0; i < name_count; i++) {
                char candidate[NK_MAX_PATH * 2];
                snprintf(candidate, sizeof(candidate), "%.*s%c%s", NK_MAX_PATH - 1, dir,
                         nk_platform_path_separator(), names[i]);
                if (nk_platform_file_exists(candidate)) {
                    if (nk_platform_absolute_path(candidate, out_path, out_size)) {
                        return true;
                    }
                    safe_str_copy(out_path, out_size, candidate);
                    return true;
                }
            }
        }
        dir = next;
    }
    return false;
}

bool package_builder_find_python(char *out_path, size_t out_size) {
    if (!out_path || out_size == 0) return false;
    out_path[0] = '\0';

    /* 1. Check PYTHON environment variable */
    const char *env_py = getenv("PYTHON");
    if (env_py && env_py[0] && nk_platform_file_exists(env_py)) {
        if (nk_platform_absolute_path(env_py, out_path, out_size)) {
            return true;
        }
        safe_str_copy(out_path, out_size, env_py);
        return true;
    }

#if defined(_WIN32) || defined(_WIN64)
    /* 2. Check standard MSYS2 UCRT64 toolchain locations */
    static const char *kWinCandidates[] = {
        "C:\\msys64\\ucrt64\\bin\\python3.exe",
        "C:\\msys64\\ucrt64\\bin\\python.exe",
        "C:/msys64/ucrt64/bin/python3.exe",
        "C:/msys64/ucrt64/bin/python.exe"
    };
    for (size_t i = 0; i < sizeof(kWinCandidates) / sizeof(kWinCandidates[0]); i++) {
        if (nk_platform_file_exists(kWinCandidates[i])) {
            safe_str_copy(out_path, out_size, kWinCandidates[i]);
            return true;
        }
    }
#endif

    /* 3. Scan PATH for python3 / python */
#if defined(_WIN32) || defined(_WIN64)
    static const char *kPathNames[] = { "python3.exe", "python.exe" };
#else
    static const char *kPathNames[] = { "python3", "python" };
#endif
    if (scan_path_for_names(kPathNames, sizeof(kPathNames) / sizeof(kPathNames[0]),
                            out_path, out_size)) {
        return true;
    }

#if !defined(_WIN32) && !defined(_WIN64)
    static const char *kPosixCandidates[] = {
        "/usr/bin/python3",
        "/usr/local/bin/python3",
        "/usr/bin/python"
    };
    for (size_t i = 0; i < sizeof(kPosixCandidates) / sizeof(kPosixCandidates[0]); i++) {
        if (nk_platform_file_exists(kPosixCandidates[i])) {
            safe_str_copy(out_path, out_size, kPosixCandidates[i]);
            return true;
        }
    }
#endif

    return false;
}

bool package_builder_find_python_in_root(const char *data_root,
                                        char *out_path, size_t out_size) {
    if (out_path && out_size) out_path[0] = '\0';
    if (data_root && data_root[0] && out_path && out_size) {
        char candidate[NK_MAX_PATH];
        int n = snprintf(candidate, sizeof(candidate), "%s%cprerequisites%cpython%cpython.exe",
                         data_root, nk_platform_path_separator(),
                         nk_platform_path_separator(), nk_platform_path_separator());
        if (n > 0 && (size_t)n < sizeof(candidate) && nk_platform_file_exists(candidate)) {
            if (nk_platform_absolute_path(candidate, out_path, out_size)) return true;
            safe_str_copy(out_path, out_size, candidate);
            return true;
        }
    }
    return package_builder_find_python(out_path, out_size);
}

/* Append "<root>/<rel>" unless root is empty or the slots are full. */
static void cli_add_rooted(char out[][NK_MAX_PATH], int *count, int max_out,
                           const char *root, const char *rel) {
    if (!root || !root[0] || *count >= max_out) return;
    size_t len = strlen(root);
    bool has_sep = root[len - 1] == '/' || root[len - 1] == '\\';
    snprintf(out[*count], NK_MAX_PATH, "%s%s%s", root, has_sep ? "" : "/", rel);
    (*count)++;
}

int package_builder_cli_candidate_paths(
    const char *install_root,
    const char *override_root,
    char out[][NK_MAX_PATH],
    int max_out
) {
    if (!out || max_out <= 0) return 0;
    int count = 0;

    /* 1. Explicit override wins (NK_INSTALL_ROOT: the folder containing tools/). */
    cli_add_rooted(out, &count, max_out, override_root, "tools/nk_cli.py");
    /* 2. The executable's own folder: a checkout runs as <checkout>/build/, a
     *    packaged player may carry tools/ beside nakagawa_player.exe. */
    cli_add_rooted(out, &count, max_out, install_root, "tools/nk_cli.py");
    /* 3. One level above the executable: <checkout>/build/../tools. */
    cli_add_rooted(out, &count, max_out, install_root, "../tools/nk_cli.py");
    /* 4. The documented v0.0.1 release layout: bin/ beside source/, and the
     *    public source export carries tools/nk_cli.py. */
    cli_add_rooted(out, &count, max_out, install_root, "../source/tools/nk_cli.py");
    /* 5-6. The working directory and its parent, unchanged from before. */
    static const char *kRelatives[] = { "tools/nk_cli.py", "../tools/nk_cli.py" };
    for (size_t i = 0; i < sizeof(kRelatives) / sizeof(kRelatives[0]) && count < max_out; i++) {
        snprintf(out[count], NK_MAX_PATH, "%s", kRelatives[i]);
        count++;
    }
    return count;
}

bool package_builder_find_cli(const char *install_root, char *out_path, size_t out_size) {
    if (!out_path || out_size == 0) return false;
    out_path[0] = '\0';

    const char *override_root = getenv("NK_INSTALL_ROOT");
    char candidates[PACKAGE_BUILDER_CLI_MAX_CANDIDATES][NK_MAX_PATH];
    int count = package_builder_cli_candidate_paths(
        install_root, override_root, candidates, PACKAGE_BUILDER_CLI_MAX_CANDIDATES);
    for (int i = 0; i < count; i++) {
        if (nk_platform_file_exists(candidates[i])) {
            if (nk_platform_absolute_path(candidates[i], out_path, out_size)) return true;
            safe_str_copy(out_path, out_size, candidates[i]);
            return true;
        }
    }
    return false;
}

void package_builder_describe_cli_not_found(const char *install_root,
                                            char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    /* Trim a trailing separator so the shown locations read cleanly. */
    char root[NK_MAX_PATH];
    if (install_root && install_root[0]) {
        safe_str_copy(root, sizeof(root), install_root);
        size_t len = strlen(root);
        while (len > 1 && (root[len - 1] == '/' || root[len - 1] == '\\')) {
            root[--len] = '\0';
        }
    } else {
        safe_str_copy(root, sizeof(root), "the player folder");
    }
    snprintf(out, out_size,
             "tools/nk_cli.py was not found. Searched, in order: the NK_INSTALL_ROOT "
             "environment variable, %s/tools, %s/../tools, %s/../source/tools (the "
             "release layout), the working directory and its parent. Fix: run the player "
             "from a Nakagawa Recomp source checkout, keep a tools folder beside "
             "nakagawa_player.exe, or set NK_INSTALL_ROOT to the folder that contains "
             "tools/ (for example the release's source folder).",
             root, root, root);
}

bool package_builder_find_tool(const char *name, char *out_path, size_t out_size) {
    if (!name || !name[0] || !out_path || out_size == 0) return false;
    out_path[0] = '\0';

#if defined(_WIN32) || defined(_WIN64)
    char exe_name[NK_MAX_PATH + 8];
    const char *names[2];
    size_t name_count;
    if (strlen(name) > 4 && strcmp(name + strlen(name) - 4, ".exe") == 0) {
        names[0] = name;
        name_count = 1;
    } else {
        snprintf(exe_name, sizeof(exe_name), "%s.exe", name);
        names[0] = exe_name;
        names[1] = name;
        name_count = 2;
    }
#else
    const char *names[1] = { name };
    size_t name_count = 1;
#endif
    return scan_path_for_names(names, name_count, out_path, out_size);
}

bool package_builder_find_tool_in_root(const char *name, const char *data_root,
                                       char *out_path, size_t out_size) {
    if (out_path && out_size) out_path[0] = '\0';
    if (name && name[0] && data_root && data_root[0] && out_path && out_size) {
        char executable[NK_MAX_PATH];
#if defined(_WIN32) || defined(_WIN64)
        int n = snprintf(executable, sizeof(executable),
                         "%s%cprerequisites%cmsys64%cucrt64%cbin%c%s.exe", data_root,
                         nk_platform_path_separator(), nk_platform_path_separator(),
                         nk_platform_path_separator(), nk_platform_path_separator(),
                         nk_platform_path_separator(), name);
#else
        int n = snprintf(executable, sizeof(executable),
                         "%s%cprerequisites%cmsys64%cucrt64%cbin%c%s", data_root,
                         nk_platform_path_separator(), nk_platform_path_separator(),
                         nk_platform_path_separator(), nk_platform_path_separator(),
                         nk_platform_path_separator(), name);
#endif
        if (n > 0 && (size_t)n < sizeof(executable) && nk_platform_file_exists(executable)) {
            if (nk_platform_absolute_path(executable, out_path, out_size)) return true;
            safe_str_copy(out_path, out_size, executable);
            return true;
        }
    }
    return package_builder_find_tool(name, out_path, out_size);
}

bool package_builder_toolchain_missing(
    bool have_python, bool have_gcc, bool have_make,
    char *out_tool, size_t out_tool_size,
    char *out_message, size_t out_message_size
) {
    const char *tool = NULL;
    const char *fix = NULL;
    if (!have_python) {
        tool = "python";
        fix = "Install CPython 3.14 (see docs/SETUP.md) or set the PYTHON environment "
              "variable to the interpreter.";
    } else if (!have_gcc) {
        tool = "gcc";
        fix = "Install the MSYS2 UCRT64 toolchain (mingw-w64-ucrt-x86_64-gcc) and put "
              "its bin directory on PATH.";
    } else if (!have_make) {
        tool = "mingw32-make";
        fix = "Install the MSYS2 UCRT64 toolchain (mingw-w64-ucrt-x86_64-make) and put "
              "its bin directory on PATH.";
    }
    if (!tool) {
        if (out_tool && out_tool_size) out_tool[0] = '\0';
        if (out_message && out_message_size) out_message[0] = '\0';
        return false;
    }
    if (out_tool && out_tool_size) snprintf(out_tool, out_tool_size, "%s", tool);
    if (out_message && out_message_size) {
        snprintf(out_message, out_message_size,
                 "BUILD_TOOLCHAIN_MISSING: %s was not found on PATH. The package build "
                 "runs python, gcc and mingw32-make, so it cannot start. %s "
                 "Use the player's pinned prerequisite consent card to install the Windows build tools.",
                 tool, fix);
    }
    return true;
}

NkResult package_builder_start(
    PackageBuildSession *session,
    const char *python_path,
    const char *cli_path,
    const char *user_data_root,
    const char *log_dir
) {
    if (!session || !python_path || !cli_path || !session->disc_id[0]) {
        return NK_ERROR_GENERIC;
    }

    /* Set up progress file and log file paths */
    const char *ldir = (log_dir && log_dir[0]) ? log_dir : ".";
    nk_platform_mkdir_p(ldir);

    snprintf(session->progress_file_path, sizeof(session->progress_file_path),
             "%s%cbuild_%s_progress.jsonl", ldir, nk_platform_path_separator(), session->disc_id);
    snprintf(session->log_file_path, sizeof(session->log_file_path),
             "%s%cbuild_%s.log", ldir, nk_platform_path_separator(), session->disc_id);

    /* Remove previous files if present */
    remove(session->progress_file_path);
    remove(session->log_file_path);
    session->progress_file_offset = 0;

    const char *argv[16];
    int argc = 0;
    argv[argc++] = python_path;
    argv[argc++] = cli_path;
    argv[argc++] = "build-package";
    argv[argc++] = session->disc_id;
    argv[argc++] = "--progress-json";
    argv[argc++] = session->progress_file_path;
    argv[argc++] = "--log-file";
    argv[argc++] = session->log_file_path;

    if (user_data_root && user_data_root[0]) {
        argv[argc++] = "--user-data-root";
        argv[argc++] = user_data_root;
    }
    argv[argc] = NULL;

    char path_override[32768];
    char path_environment[32784];
    const char *envp[2] = { NULL, NULL };
    char downloaded_bin[NK_MAX_PATH];
    int path_len = snprintf(downloaded_bin, sizeof(downloaded_bin),
                            "%s%cprerequisites%cmsys64%cucrt64%cbin",
                            user_data_root, nk_platform_path_separator(),
                            nk_platform_path_separator(), nk_platform_path_separator(),
                            nk_platform_path_separator());
    const char *old_path = getenv("PATH");
    if (path_len > 0 && (size_t)path_len < sizeof(downloaded_bin)) {
        char downloaded_gcc[NK_MAX_PATH];
        int gcc_len = snprintf(downloaded_gcc, sizeof(downloaded_gcc), "%s%cgcc.exe",
                               downloaded_bin, nk_platform_path_separator());
#if defined(_WIN32) || defined(_WIN64)
        const char list_separator = ';';
#else
        const char list_separator = ':';
#endif
        int written = gcc_len > 0 && (size_t)gcc_len < sizeof(downloaded_gcc) &&
                      nk_platform_file_exists(downloaded_gcc)
            ? snprintf(path_override, sizeof(path_override), "%s%c%s",
                       downloaded_bin, list_separator, old_path ? old_path : "")
            : -1;
        if (written > 0 && (size_t)written < sizeof(path_override)) {
            snprintf(path_environment, sizeof(path_environment), "PATH=%s", path_override);
            envp[0] = path_environment;
        }
    }

    bool spawned = nk_platform_spawn_process(
        python_path,
        argv,
        envp[0] ? envp : NULL,
        NULL,
        &session->process
    );

    if (!spawned) {
        session->is_building = false;
        session->is_failed = true;
        session->current_stage = PACKAGE_BUILD_STAGE_FAILED;
        safe_str_copy(session->failure_boundary, sizeof(session->failure_boundary),
                      "Failed to spawn background build process.");
        return NK_ERROR_PROCESS_SPAWN;
    }

    session->is_building = true;
    session->is_complete = false;
    session->is_failed = false;
    session->is_cancelled = false;
    session->exit_code = -1;
    session->current_stage = PACKAGE_BUILD_STAGE_PREFLIGHT;
    safe_str_copy(session->current_stage_name, sizeof(session->current_stage_name), "preflight");
    safe_str_copy(session->current_message, sizeof(session->current_message), "Starting package build...");

    char spawn_msg[PACKAGE_BUILD_LINE_LEN];
    snprintf(spawn_msg, sizeof(spawn_msg), "[build] START: Building package for %s", session->disc_id);
    package_builder_add_output_line(session, spawn_msg);

    return NK_OK;
}

static void read_new_progress_lines(PackageBuildSession *session) {
    if (!session || !session->progress_file_path[0]) return;
    FILE *f = fopen(session->progress_file_path, "rb");
    if (!f) return;

    if (session->progress_file_offset > 0) {
        if (fseek(f, (long)session->progress_file_offset, SEEK_SET) != 0) {
            fclose(f);
            return;
        }
    }

    char line_buf[1024];
    while (fgets(line_buf, sizeof(line_buf), f)) {
        size_t len = strlen(line_buf);
        if (len > 0 && line_buf[len - 1] == '\n') {
            session->progress_file_offset = ftell(f);
            PackageProgressEvent ev;
            if (package_builder_parse_progress_line(line_buf, len, &ev)) {
                package_builder_apply_event(session, &ev);
            }
        } else {
            /* Incomplete line: rewind to before this partial read */
            fseek(f, (long)session->progress_file_offset, SEEK_SET);
            break;
        }
    }
    fclose(f);
}

void package_builder_poll(PackageBuildSession *session, uint64_t current_time_ms) {
    if (!session || !session->is_building) return;

    if (session->start_time_ms > 0 && current_time_ms >= session->start_time_ms) {
        session->elapsed_ms = (uint32_t)(current_time_ms - session->start_time_ms);
    }

    read_new_progress_lines(session);

    if (!session->process.is_active) {
        return;
    }

    bool running = nk_platform_is_process_running(&session->process);
    if (!running) {
        session->exit_code = nk_platform_wait_process(&session->process, 0);
        session->is_building = false;
        nk_platform_close_process(&session->process);

        /* Read any remaining flushed lines */
        read_new_progress_lines(session);

        if (session->exit_code != 0) {
            session->is_failed = true;
            if (session->current_stage != PACKAGE_BUILD_STAGE_FAILED) {
                session->current_stage = PACKAGE_BUILD_STAGE_FAILED;
            }
            if (!session->failure_boundary[0]) {
                /* If no boundary message was recorded from JSON, extract last error from log */
                if (session->log_file_path[0]) {
                    FILE *lf = fopen(session->log_file_path, "rb");
                    if (lf) {
                        char last_line[512] = "";
                        char cur_line[512];
                        while (fgets(cur_line, sizeof(cur_line), lf)) {
                            size_t l = strlen(cur_line);
                            while (l > 0 && (cur_line[l - 1] == '\r' || cur_line[l - 1] == '\n')) {
                                cur_line[--l] = '\0';
                            }
                            if (l > 0) safe_str_copy(last_line, sizeof(last_line), cur_line);
                        }
                        fclose(lf);
                        if (last_line[0]) {
                            safe_str_copy(session->failure_boundary, sizeof(session->failure_boundary), last_line);
                        }
                    }
                }
                if (!session->failure_boundary[0]) {
                    snprintf(session->failure_boundary, sizeof(session->failure_boundary),
                             "Build child process failed with exit code %d.", session->exit_code);
                }
            }
        }
    }
}

void package_builder_cancel(PackageBuildSession *session) {
    if (!session) return;
    if (session->is_building) {
        nk_platform_terminate_process(&session->process);
        nk_platform_close_process(&session->process);
        session->is_building = false;
        session->is_cancelled = true;
        session->current_stage = PACKAGE_BUILD_STAGE_CANCELLED;
        safe_str_copy(session->current_stage_name, sizeof(session->current_stage_name), "cancelled");
        safe_str_copy(session->current_message, sizeof(session->current_message), "Build cancelled by user.");
        package_builder_add_output_line(session, "[build] CANCEL: Process terminated by user");
    }
}
