/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Add to Steam (#2152). The layout and rules are in nk_steam_shortcuts.h. */

#include "nk_steam_shortcuts.h"

#include "nk_platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define VDF_MAP 0x00
#define VDF_STRING 0x01
#define VDF_INT32 0x02
#define VDF_FLOAT 0x03
#define VDF_PTR 0x04
#define VDF_COLOR 0x06
#define VDF_UINT64 0x07
#define VDF_END 0x08
#define VDF_INT64 0x0A

#define VDF_MAX_DEPTH 8
#define VDF_MAX_ENTRIES 200000u

const char *nk_steam_status_name(NkSteamStatus status) {
    switch (status) {
        case NK_STEAM_OK: return "OK";
        case NK_STEAM_ERR_ARGUMENT: return "STEAM_ARGUMENT";
        case NK_STEAM_ERR_CORRUPT: return "STEAM_SHORTCUTS_CORRUPT";
        case NK_STEAM_ERR_EXISTS: return "STEAM_SHORTCUT_EXISTS";
        case NK_STEAM_ERR_TOO_LARGE: return "STEAM_SHORTCUTS_TOO_LARGE";
        case NK_STEAM_ERR_STEAM_RUNNING: return "STEAM_RUNNING";
        case NK_STEAM_ERR_NOT_FOUND: return "STEAM_NOT_FOUND";
        case NK_STEAM_ERR_BACKUP: return "STEAM_BACKUP_FAILED";
        case NK_STEAM_ERR_WRITE: return "STEAM_WRITE_FAILED";
        case NK_STEAM_ERR_UNSUPPORTED: return "STEAM_UNSUPPORTED";
    }
    return "STEAM_UNKNOWN";
}

void nk_steam_free(void *buffer) { free(buffer); }

/* ---- CRC-32 (IEEE 802.3), for the app ID Steam derives from a shortcut ---- */

static uint32_t crc32_update(uint32_t crc, const char *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint8_t)data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint32_t nk_steam_shortcut_app_id(const char *exe, const char *app_name) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, "\"", 1u);
    crc = crc32_update(crc, exe, strlen(exe));
    crc = crc32_update(crc, "\"", 1u);
    crc = crc32_update(crc, app_name, strlen(app_name));
    return (crc ^ 0xFFFFFFFFu) | 0x80000000u;
}

/* ---- Reading: a bounds-checked walk ---- */

typedef struct {
    const uint8_t *data;
    size_t size;
    const char *want_exe;     /* the executable to look for, or NULL */
    bool exe_found;
    bool have_shortcuts;
    size_t shortcuts_end;     /* offset of the 0x08 that closes "shortcuts" */
    size_t root_end;          /* offset of the 0x08 that closes the root */
    uint32_t next_index;      /* one past the largest numeric child key */
    uint32_t entries;
} Scan;

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    }
    return *a == *b;
}

/* The NUL-terminated string at *pos, inside the buffer; advances past the NUL. */
static const char *read_cstr(const Scan *s, size_t *pos) {
    size_t at = *pos;
    if (at >= s->size) return NULL;
    const void *nul = memchr(s->data + at, 0, s->size - at);
    if (!nul) return NULL;
    *pos = (size_t)((const uint8_t *)nul - s->data) + 1u;
    return (const char *)(s->data + at);
}

/* A stored executable is quoted; compare without the quotes. */
static bool exe_matches(const char *stored, const char *want) {
    size_t length = strlen(stored);
    if (length >= 2u && stored[0] == '"' && stored[length - 1u] == '"') {
        stored++;
        length -= 2u;
    }
    size_t want_length = strlen(want);
    if (want_length >= 2u && want[0] == '"' && want[want_length - 1u] == '"') {
        want++;
        want_length -= 2u;
    }
    if (length != want_length) return false;
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char)stored[i]) != tolower((unsigned char)want[i])) return false;
    }
    return true;
}

static bool all_digits(const char *text) {
    if (!*text) return false;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return false;
    }
    return true;
}

/* Walks the entries of one map from *pos to its closing 0x08, which it consumes. `level`:
 * 0 the root, 1 inside "shortcuts", 2 inside one entry, 3 anywhere else. */
static bool scan_map(Scan *s, size_t *pos, int depth, int level) {
    if (depth > VDF_MAX_DEPTH) return false;
    for (;;) {
        if (*pos >= s->size) return false;
        uint8_t type = s->data[(*pos)++];
        if (type == VDF_END) {
            if (level == 0) s->root_end = *pos - 1u;
            return true;
        }
        if (++s->entries > VDF_MAX_ENTRIES) return false;
        const char *key = read_cstr(s, pos);
        if (!key) return false;
        switch (type) {
            case VDF_MAP: {
                int child_level = 3;
                if (level == 0 && !s->have_shortcuts && ieq(key, "shortcuts")) {
                    child_level = 1;
                } else if (level == 1) {
                    child_level = 2;
                    if (all_digits(key) && strlen(key) <= 9u) {
                        uint32_t index = (uint32_t)strtoul(key, NULL, 10) + 1u;
                        if (index > s->next_index) s->next_index = index;
                    }
                }
                if (!scan_map(s, pos, depth + 1, child_level)) return false;
                if (child_level == 1) {
                    s->have_shortcuts = true;
                    s->shortcuts_end = *pos - 1u;
                }
                break;
            }
            case VDF_STRING: {
                const char *value = read_cstr(s, pos);
                if (!value) return false;
                if (level == 2 && s->want_exe && ieq(key, "exe") &&
                    exe_matches(value, s->want_exe)) {
                    s->exe_found = true;
                }
                break;
            }
            case VDF_INT32:
            case VDF_FLOAT:
            case VDF_PTR:
            case VDF_COLOR:
                if (s->size - *pos < 4u) return false;
                *pos += 4u;
                break;
            case VDF_UINT64:
            case VDF_INT64:
                if (s->size - *pos < 8u) return false;
                *pos += 8u;
                break;
            default:
                return false;
        }
    }
}

/* Parses a whole file: the root map, then nothing. */
static NkSteamStatus scan_file(Scan *s, const uint8_t *data, size_t size, const char *want_exe) {
    memset(s, 0, sizeof(*s));
    s->data = data;
    s->size = size;
    s->want_exe = want_exe;
    if (size > NK_STEAM_VDF_MAX_BYTES) return NK_STEAM_ERR_TOO_LARGE;
    size_t pos = 0;
    if (!scan_map(s, &pos, 0, 0) || pos != size) return NK_STEAM_ERR_CORRUPT;
    return NK_STEAM_OK;
}

NkSteamStatus nk_steam_vdf_has_exe(const uint8_t *in, size_t in_len, const char *exe, bool *found) {
    if (!exe || !exe[0] || !found || (!in && in_len)) return NK_STEAM_ERR_ARGUMENT;
    *found = false;
    if (!in_len) return NK_STEAM_OK;
    Scan scan;
    NkSteamStatus status = scan_file(&scan, in, in_len, exe);
    if (status != NK_STEAM_OK) return status;
    *found = scan.exe_found;
    return NK_STEAM_OK;
}

/* ---- Writing ---- */

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
    bool failed;
} Buf;

static void buf_put(Buf *b, const void *bytes, size_t count) {
    if (b->failed) return;
    if (count > NK_STEAM_VDF_MAX_BYTES || b->size > NK_STEAM_VDF_MAX_BYTES - count) {
        b->failed = true;
        return;
    }
    if (b->size + count > b->cap) {
        size_t cap = b->cap ? b->cap : 512u;
        while (cap < b->size + count) cap *= 2u;
        uint8_t *grown = (uint8_t *)realloc(b->data, cap);
        if (!grown) {
            b->failed = true;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    if (count) memcpy(b->data + b->size, bytes, count);
    b->size += count;
}

static void buf_byte(Buf *b, uint8_t value) { buf_put(b, &value, 1u); }
static void buf_cstr(Buf *b, const char *text) { buf_put(b, text, strlen(text) + 1u); }

static void buf_string_entry(Buf *b, const char *key, const char *value) {
    buf_byte(b, VDF_STRING);
    buf_cstr(b, key);
    buf_cstr(b, value);
}

static void buf_int_entry(Buf *b, const char *key, uint32_t value) {
    uint8_t le[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16),
                     (uint8_t)(value >> 24)};
    buf_byte(b, VDF_INT32);
    buf_cstr(b, key);
    buf_put(b, le, 4u);
}

/* A field is a non-empty (unless allowed) UTF-8 text of printable characters. The bytes of a
 * multi-byte character are all >= 0x80, so only ASCII controls and DEL are refused. */
static bool field_ok(const char *text, bool allow_empty) {
    if (!text) return false;
    size_t length = strlen(text);
    if ((!length && !allow_empty) || length > NK_STEAM_FIELD_MAX) return false;
    for (size_t i = 0; i < length; i++) {
        uint8_t c = (uint8_t)text[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

static void put_quoted(Buf *b, const char *key, const char *path) {
    buf_byte(b, VDF_STRING);
    buf_cstr(b, key);
    buf_put(b, "\"", 1u);
    buf_put(b, path, strlen(path));
    buf_put(b, "\"\0", 2u);
}

static void put_entry(Buf *b, uint32_t index, const NkSteamShortcut *sc) {
    char key[16];
    snprintf(key, sizeof(key), "%u", (unsigned)index);
    buf_byte(b, VDF_MAP);
    buf_cstr(b, key);
    buf_int_entry(b, "appid", nk_steam_shortcut_app_id(sc->exe, sc->app_name));
    buf_string_entry(b, "appname", sc->app_name);
    put_quoted(b, "exe", sc->exe);
    put_quoted(b, "StartDir", sc->start_dir);
    buf_string_entry(b, "icon", "");
    buf_string_entry(b, "ShortcutPath", "");
    buf_string_entry(b, "LaunchOptions", sc->launch_options ? sc->launch_options : "");
    buf_int_entry(b, "IsHidden", 0u);
    buf_int_entry(b, "AllowDesktopConfig", 1u);
    buf_int_entry(b, "AllowOverlay", 1u);
    buf_int_entry(b, "OpenVR", 0u);
    buf_int_entry(b, "Devkit", 0u);
    buf_string_entry(b, "DevkitGameID", "");
    buf_int_entry(b, "DevkitOverrideAppID", 0u);
    buf_int_entry(b, "LastPlayTime", 0u);
    buf_string_entry(b, "FlatpakAppID", "");
    buf_string_entry(b, "sortas", "");
    buf_byte(b, VDF_MAP);
    buf_cstr(b, "tags");
    buf_byte(b, VDF_END);
    buf_byte(b, VDF_END);
}

NkSteamStatus nk_steam_vdf_add(const uint8_t *in, size_t in_len, const NkSteamShortcut *shortcut,
                               uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!shortcut || !out || !out_len || (!in && in_len)) return NK_STEAM_ERR_ARGUMENT;
    if (!field_ok(shortcut->app_name, false) || !field_ok(shortcut->exe, false) ||
        !field_ok(shortcut->start_dir, false) ||
        (shortcut->launch_options && !field_ok(shortcut->launch_options, true))) {
        return NK_STEAM_ERR_ARGUMENT;
    }
    Scan scan;
    Buf b = {0};
    uint32_t index = 0;
    if (in_len) {
        NkSteamStatus status = scan_file(&scan, in, in_len, shortcut->exe);
        if (status != NK_STEAM_OK) return status;
        if (scan.exe_found) return NK_STEAM_ERR_EXISTS;
        index = scan.next_index;
        if (scan.have_shortcuts) {
            buf_put(&b, in, scan.shortcuts_end);
            put_entry(&b, index, shortcut);
            buf_put(&b, in + scan.shortcuts_end, in_len - scan.shortcuts_end);
        } else {
            buf_put(&b, in, scan.root_end);
            buf_byte(&b, VDF_MAP);
            buf_cstr(&b, "shortcuts");
            put_entry(&b, index, shortcut);
            buf_byte(&b, VDF_END);
            buf_put(&b, in + scan.root_end, in_len - scan.root_end);
        }
    } else {
        buf_byte(&b, VDF_MAP);
        buf_cstr(&b, "shortcuts");
        put_entry(&b, 0u, shortcut);
        buf_byte(&b, VDF_END);
        buf_byte(&b, VDF_END);
    }
    if (b.failed) {
        free(b.data);
        return NK_STEAM_ERR_TOO_LARGE;
    }
    /* The result must parse and now list the executable, or it is not returned. */
    NkSteamStatus verify = scan_file(&scan, b.data, b.size, shortcut->exe);
    if (verify != NK_STEAM_OK || !scan.exe_found || !scan.have_shortcuts) {
        free(b.data);
        return NK_STEAM_ERR_WRITE;
    }
    *out = b.data;
    *out_len = b.size;
    return NK_STEAM_OK;
}

/* ---- Files ---- */

/* The whole file into a new buffer. *exists tells a missing file (which is fine) from a
 * failed read. */
static NkSteamStatus read_file(const char *path, uint8_t **data, size_t *size, bool *exists) {
    *data = NULL;
    *size = 0;
    *exists = false;
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return nk_platform_path_missing(path) ? NK_STEAM_OK : NK_STEAM_ERR_WRITE;
    *exists = true;
    uint8_t *buffer = NULL;
    size_t length = 0, cap = 0;
    for (;;) {
        if (length == cap) {
            cap = cap ? cap * 2u : 4096u;
            uint8_t *grown = (uint8_t *)realloc(buffer, cap);
            if (!grown) {
                free(buffer);
                fclose(f);
                return NK_STEAM_ERR_WRITE;
            }
            buffer = grown;
        }
        size_t got = fread(buffer + length, 1u, cap - length, f);
        length += got;
        if (length > NK_STEAM_VDF_MAX_BYTES) {
            free(buffer);
            fclose(f);
            return NK_STEAM_ERR_TOO_LARGE;
        }
        if (got == 0u) break;
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) {
        free(buffer);
        return NK_STEAM_ERR_WRITE;
    }
    *data = buffer;
    *size = length;
    return NK_STEAM_OK;
}

static bool write_file_synced(const char *path, const uint8_t *data, size_t size) {
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) return false;
    bool ok = (size == 0u || fwrite(data, 1u, size, f) == size) && fflush(f) == 0 &&
              nk_platform_sync_file(f);
    ok = (fclose(f) == 0) && ok;
    return ok;
}

/* True when the file at `path` holds exactly these bytes. */
static bool file_equals(const char *path, const uint8_t *data, size_t size) {
    uint8_t *back = NULL;
    size_t back_size = 0;
    bool exists = false;
    if (read_file(path, &back, &back_size, &exists) != NK_STEAM_OK || !exists) {
        free(back);
        return false;
    }
    bool same = back_size == size && (size == 0u || memcmp(back, data, size) == 0);
    free(back);
    return same;
}

static bool stamp_ok(const char *stamp) {
    if (!stamp || !stamp[0]) return false;
    size_t length = strlen(stamp);
    if (length > 32u) return false;
    for (size_t i = 0; i < length; i++) {
        if (!isalnum((unsigned char)stamp[i]) && stamp[i] != '-') return false;
    }
    return true;
}

static bool parent_dir(const char *path, char *dir, size_t cap) {
    size_t length = strlen(path);
    while (length > 0u && !nk_platform_is_path_separator(path[length - 1u])) length--;
    if (length <= 1u || length >= cap) return false;
    memcpy(dir, path, length - 1u);
    dir[length - 1u] = '\0';
    return true;
}

NkSteamStatus nk_steam_add_to_file(const char *vdf_path, const NkSteamShortcut *shortcut,
                                   bool steam_running, const char *stamp,
                                   char *backup_path, size_t backup_cap) {
    if (backup_path && backup_cap) backup_path[0] = '\0';
    if (!vdf_path || !vdf_path[0] || !shortcut || !stamp_ok(stamp)) return NK_STEAM_ERR_ARGUMENT;
    if (steam_running) return NK_STEAM_ERR_STEAM_RUNNING;

    uint8_t *current = NULL;
    size_t current_size = 0;
    bool existed = false;
    NkSteamStatus status = read_file(vdf_path, &current, &current_size, &existed);
    if (status != NK_STEAM_OK) return status;

    uint8_t *next = NULL;
    size_t next_size = 0;
    status = nk_steam_vdf_add(current, current_size, shortcut, &next, &next_size);
    if (status != NK_STEAM_OK) {
        free(current);
        return status;
    }

    char dir[NK_MAX_PATH];
    char backup[NK_MAX_PATH];
    char temp[NK_MAX_PATH];
    backup[0] = '\0';
    status = NK_STEAM_ERR_WRITE;
    if (!parent_dir(vdf_path, dir, sizeof(dir)) || !nk_platform_mkdir_p(dir) ||
        snprintf(temp, sizeof(temp), "%s.nakagawa-tmp", vdf_path) >= (int)sizeof(temp)) {
        goto done;
    }
    if (existed) {
        /* A dated backup beside the file, never over an earlier one; if it cannot be made and
         * read back, nothing is written. */
        status = NK_STEAM_ERR_BACKUP;
        bool made = false;
        for (unsigned attempt = 0; attempt < 100u && !made; attempt++) {
            int n = attempt == 0u
                ? snprintf(backup, sizeof(backup), "%s.nakagawa-backup-%s", vdf_path, stamp)
                : snprintf(backup, sizeof(backup), "%s.nakagawa-backup-%s-%u", vdf_path, stamp,
                           attempt + 1u);
            if (n <= 0 || (size_t)n >= sizeof(backup)) goto done;
            if (!nk_platform_path_missing(backup)) continue;
            made = true;
        }
        if (!made || !write_file_synced(backup, current, current_size) ||
            !file_equals(backup, current, current_size)) {
            (void)nk_remove_utf8(backup);
            backup[0] = '\0';
            goto done;
        }
    }
    status = NK_STEAM_ERR_WRITE;
    if (!write_file_synced(temp, next, next_size) || !file_equals(temp, next, next_size) ||
        nk_rename_utf8(temp, vdf_path) != 0) {
        (void)nk_remove_utf8(temp);
        goto done;
    }
    if (!file_equals(vdf_path, next, next_size)) goto done;
    if (backup_path && backup_cap && backup[0] && strlen(backup) < backup_cap) {
        memcpy(backup_path, backup, strlen(backup) + 1u);
    }
    status = NK_STEAM_OK;
done:
    free(current);
    free(next);
    return status;
}

/* ---- Finding Steam ---- */

typedef struct {
    NkSteamAccount *accounts;
    size_t cap;
    size_t count;
    const char *userdata;
    bool overflow;
} AccountList;

static int account_compare(const void *a, const void *b) {
    const NkSteamAccount *x = (const NkSteamAccount *)a;
    const NkSteamAccount *y = (const NkSteamAccount *)b;
    size_t lx = strlen(x->id), ly = strlen(y->id);
    if (lx != ly) return lx < ly ? -1 : 1;
    return strcmp(x->id, y->id);
}

static bool collect_account(const char *name, bool is_dir, void *context) {
    AccountList *list = (AccountList *)context;
    /* An account folder is all digits and not "0" (the anonymous user). */
    if (!is_dir || !all_digits(name) || strlen(name) >= sizeof(list->accounts[0].id) ||
        strcmp(name, "0") == 0) {
        return true;
    }
    if (list->count >= list->cap) {
        list->overflow = true;
        return true;
    }
    NkSteamAccount *account = &list->accounts[list->count];
    memcpy(account->id, name, strlen(name) + 1u);
    if (snprintf(account->shortcuts_path, sizeof(account->shortcuts_path),
                 "%s%c%s%cconfig%cshortcuts.vdf", list->userdata, nk_platform_path_separator(),
                 name, nk_platform_path_separator(), nk_platform_path_separator()) >=
        (int)sizeof(account->shortcuts_path)) {
        return true;
    }
    list->count++;
    return true;
}

NkSteamStatus nk_steam_list_accounts(const char *root, NkSteamAccount *accounts, size_t cap,
                                     size_t *count) {
    if (count) *count = 0;
    if (!root || !root[0] || !accounts || !count || cap == 0u) return NK_STEAM_ERR_ARGUMENT;
    char userdata[NK_MAX_PATH];
    if (snprintf(userdata, sizeof(userdata), "%s%cuserdata", root,
                 nk_platform_path_separator()) >= (int)sizeof(userdata) ||
        !nk_platform_dir_exists(userdata)) {
        return NK_STEAM_ERR_NOT_FOUND;
    }
    AccountList list = {accounts, cap, 0u, userdata, false};
    if (!nk_platform_list_dir(userdata, collect_account, &list)) return NK_STEAM_ERR_NOT_FOUND;
    qsort(accounts, list.count, sizeof(accounts[0]), account_compare);
    *count = list.count;
    return list.count ? NK_STEAM_OK : NK_STEAM_ERR_NOT_FOUND;
}

#ifdef _WIN32

static bool registry_string(HKEY root, const wchar_t *subkey, const wchar_t *value, char *out,
                            size_t cap) {
    wchar_t wide[1024];
    DWORD bytes = (DWORD)sizeof(wide) - (DWORD)sizeof(wchar_t);
    memset(wide, 0, sizeof(wide));
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, NULL, wide, &bytes) != ERROR_SUCCESS) {
        return false;
    }
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out, (int)cap, NULL, NULL);
    return n > 1;
}

NkSteamStatus nk_steam_find_root(char *root, size_t root_cap) {
    if (!root || root_cap == 0u) return NK_STEAM_ERR_ARGUMENT;
    root[0] = '\0';
    if (!registry_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", root,
                         root_cap)) {
        root[0] = '\0';
        return NK_STEAM_ERR_NOT_FOUND;
    }
    for (char *c = root; *c; c++) {
        if (*c == '/') *c = '\\';
    }
    size_t length = strlen(root);
    while (length > 3u && root[length - 1u] == '\\') root[--length] = '\0';
    if (!nk_platform_dir_exists(root)) {
        root[0] = '\0';
        return NK_STEAM_ERR_NOT_FOUND;
    }
    return NK_STEAM_OK;
}

bool nk_steam_client_running(void) {
    DWORD pid = 0;
    DWORD size = (DWORD)sizeof(pid);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid",
                     RRF_RT_REG_DWORD, NULL, &pid, &size) != ERROR_SUCCESS || pid == 0u) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

#else

NkSteamStatus nk_steam_find_root(char *root, size_t root_cap) {
    if (root && root_cap) root[0] = '\0';
    return NK_STEAM_ERR_UNSUPPORTED;
}

bool nk_steam_client_running(void) { return false; }

#endif

NkSteamStatus nk_steam_add_for_account(const char *root, const char *account_id,
                                       const NkSteamShortcut *shortcut, NkSteamRunningFn running,
                                       const char *stamp, char *backup_path, size_t backup_cap) {
    if (backup_path && backup_cap) backup_path[0] = '\0';
    if (!root || !root[0] || !account_id || !all_digits(account_id) ||
        strcmp(account_id, "0") == 0 || strlen(account_id) >= sizeof(((NkSteamAccount *)0)->id)) {
        return NK_STEAM_ERR_ARGUMENT;
    }
#ifndef _WIN32
    /* Without a probe of its own, a host that cannot see Steam's process cannot promise that
     * Steam is closed, so it writes nothing. */
    if (!running) return NK_STEAM_ERR_UNSUPPORTED;
#endif
    NkSteamAccount accounts[NK_STEAM_ACCOUNT_CAP];
    size_t count = 0;
    NkSteamStatus status = nk_steam_list_accounts(root, accounts, NK_STEAM_ACCOUNT_CAP, &count);
    if (status != NK_STEAM_OK) return status;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(accounts[i].id, account_id) != 0) continue;
        bool is_running = (running ? running : nk_steam_client_running)();
        return nk_steam_add_to_file(accounts[i].shortcuts_path, shortcut, is_running, stamp,
                                    backup_path, backup_cap);
    }
    return NK_STEAM_ERR_NOT_FOUND;
}
