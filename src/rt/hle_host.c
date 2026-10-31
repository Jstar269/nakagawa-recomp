// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Host primitives: the host seam (sleep and the clocks) and the checked UTF-8/UTF-16 host-path
 * helpers (#223). Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* ---- host primitive seam -------------------------------------------------------
 * The Windows spellings below are the ORIGINAL Win32 calls, unchanged, so a
 * Windows build is byte-for-byte the build it was. Each non-Windows arm is the
 * same operation expressed with the primitive the host actually publishes --
 * not a stub, and not a fabricated result. A feature with no equivalent on a
 * host is refused at the one place that would use it (see ms0_fopen_utf8 and the
 * host data walk), so the guest observes the ordinary PSP error instead of a
 * crash or a made-up success. */
/* Pure conversion of Unix timestamp (seconds and nanoseconds since 1970-01-01 00:00:00 UTC)
 * to Windows FILETIME 100ns ticks since 1601-01-01 00:00:00 UTC.
 * 11,644,473,600 seconds elapsed between 1601-01-01 and 1970-01-01. */
uint64_t sr_unix_time_to_filetime_ticks(int64_t sec, long nsec) {
    if (sec < -11644473600LL) return 0u;
    uint64_t s = (uint64_t)(sec + 11644473600LL);
    return s * 10000000ull + (uint64_t)(nsec > 0 ? nsec : 0) / 100ull;
}

#ifdef _WIN32
void sr_host_sleep_ms(uint32_t ms) { Sleep(ms); }
uint64_t sr_host_monotonic_ms(void) { return GetTickCount64(); }
/* 100 ns ticks since 1601-01-01, the same unit FILETIME carries, so the RTC
 * epoch arithmetic below is unchanged. */
uint64_t sr_host_filetime_ticks(void) {
    FILETIME ft;
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(&ft);
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}
#else
void sr_host_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}
uint64_t sr_host_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
/* CLOCK_REALTIME is seconds since 1970-01-01; scaled here into the same
 * 100 ns-since-1601 unit FILETIME carries so RTC_FILETIME_EPOCH_TICK stays the
 * one epoch constant and the guest-visible tick arithmetic is unchanged. */
uint64_t sr_host_filetime_ticks(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0u;
    return sr_unix_time_to_filetime_ticks((int64_t)ts.tv_sec, ts.tv_nsec);
}
#endif

uint32_t sr_last_nid = 0;

#ifdef _WIN32
/* ---- checked UTF-8/UTF-16 host-path helpers (issue #223) --------------------------
 *
 * Guest paths and environment variables are UTF-8.  Keep them as UTF-8 in the
 * host-neutral index, then cross the Windows boundary exactly once with checked
 * conversions.  Every Windows file operation receives an absolute wide path;
 * the extended prefix keeps the route independent of MAX_PATH and of the
 * executable manifest's longPathAware setting.
 */
int sr_utf8_to_wide_alloc(const char *src, wchar_t **out) {
    if (!src || !out || !sr_asset_index_valid_utf8(src) ||
        strlen(src) > (size_t)INT_MAX) return 0;
    *out = NULL;
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
    if (need <= 0 || (size_t)need > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *wide = (wchar_t *)calloc((size_t)need, sizeof(wchar_t));
    if (!wide) return 0;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, wide, need) != need) {
        free(wide);
        return 0;
    }
    *out = wide;
    return 1;
}

int sr_wide_to_utf8_alloc(const wchar_t *src, char **out) {
    if (!src || !out || wcslen(src) > (size_t)INT_MAX) return 0;
    *out = NULL;
    int need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1,
                                   NULL, 0, NULL, NULL);
    if (need <= 0) return 0;
    char *utf8 = (char *)malloc((size_t)need);
    if (!utf8) return 0;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1,
                            utf8, need, NULL, NULL) != need) {
        free(utf8);
        return 0;
    }
    *out = utf8;
    return 1;
}

int sr_wide_env_alloc(const wchar_t *name, wchar_t **value_out,
                             int *present_out) {
    if (!name || !value_out || !present_out) return 0;
    *value_out = NULL;
    *present_out = 0;
    DWORD capacity = 256u;
    for (;;) {
        if (capacity > (DWORD)(SIZE_MAX / sizeof(wchar_t))) return 0;
        wchar_t *value = (wchar_t *)malloc((size_t)capacity * sizeof(*value));
        if (!value) return 0;
        SetLastError(ERROR_SUCCESS);
        DWORD length = GetEnvironmentVariableW(name, value, capacity);
        DWORD error = GetLastError();
        if (length == 0u && error == ERROR_ENVVAR_NOT_FOUND) {
            free(value);
            return 1;
        }
        if (length < capacity - 1u || (length == 0u && error == ERROR_SUCCESS)) {
            *value_out = value;
            *present_out = 1;
            return 1;
        }
        free(value);
        if (length < capacity) {
            if (capacity > UINT32_MAX / 2u) return 0;
            capacity *= 2u;
        } else {
            if (length == UINT32_MAX || length + 1u < length) return 0;
            capacity = length + 1u;
        }
    }
}

int sr_wide_join_alloc(const wchar_t *left, const wchar_t *right,
                              wchar_t **out) {
    if (!left || !right || !out) return 0;
    *out = NULL;
    size_t a = wcslen(left), b = wcslen(right);
    int separator = a > 0 && b > 0 && left[a - 1] != L'\\' && left[a - 1] != L'/';
    size_t extra = b;
    if (extra > SIZE_MAX - (size_t)separator - 1u) return 0;
    extra += (size_t)separator + 1u;
    if (a > SIZE_MAX - extra) return 0;
    size_t total = a + extra;
    if (total > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *joined = (wchar_t *)malloc(total * sizeof(*joined));
    if (!joined) return 0;
    memcpy(joined, left, a * sizeof(*joined));
    size_t at = a;
    if (separator) joined[at++] = L'\\';
    memcpy(joined + at, right, b * sizeof(*joined));
    joined[at + b] = L'\0';
    for (size_t i = 0; i < at + b; i++)
        if (joined[i] == L'/') joined[i] = L'\\';
    *out = joined;
    return 1;
}

static int sr_wide_get_current_directory(wchar_t **out) {
    if (!out) return 0;
    *out = NULL;
    DWORD capacity = 256u;
    for (;;) {
        wchar_t *buf = (wchar_t *)malloc((size_t)capacity * sizeof(*buf));
        if (!buf) return 0;
        DWORD length = GetCurrentDirectoryW(capacity, buf);
        if (length == 0) { free(buf); return 0; }
        if (length < capacity) { *out = buf; return 1; }
        free(buf);
        if (capacity > (DWORD)(SIZE_MAX / (2u * sizeof(*buf)))) return 0;
        capacity *= 2u;
    }
}

static int sr_wide_is_extended(const wchar_t *path) {
    return path && wcslen(path) >= 4u && path[0] == L'\\' && path[1] == L'\\' &&
           path[2] == L'?' && path[3] == L'\\';
}

static int sr_wide_is_drive_absolute(const wchar_t *path) {
    if (!path || wcslen(path) < 3u || path[1] != L':') return 0;
    int letter = (path[0] >= L'A' && path[0] <= L'Z') ||
                 (path[0] >= L'a' && path[0] <= L'z');
    return letter && (path[2] == L'\\' || path[2] == L'/');
}

static int sr_wide_is_absolute(const wchar_t *path) {
    return sr_wide_is_drive_absolute(path) ||
           (path && wcslen(path) >= 2u && path[0] == L'\\' && path[1] == L'\\');
}

static int sr_wide_extended_filesystem_root(const wchar_t *path) {
    if (!sr_wide_is_extended(path)) return 0;
    if (wcslen(path) >= 7u &&
        ((path[4] >= L'A' && path[4] <= L'Z') || (path[4] >= L'a' && path[4] <= L'z')) &&
        path[5] == L':' && (path[6] == L'\\' || path[6] == L'/')) return 1;
    if (_wcsnicmp(path, L"\\\\?\\UNC\\", 8u) != 0) return 0;
    const wchar_t *p = path + 8u;
    const wchar_t *server = p;
    while (*p && *p != L'\\' && *p != L'/') p++;
    if (p == server || !*p) return 0;
    while (*p == L'\\' || *p == L'/') p++;
    const wchar_t *share = p;
    while (*p && *p != L'\\' && *p != L'/') p++;
    return p != share;
}

/* Extended paths reject '.' and '..' components.  Rejecting those components
 * here is safer than passing an ambiguous path to a wide API; callers receive
 * a hard path-resolution error instead of silently escaping the configured root. */
static int sr_wide_has_dot_component(const wchar_t *path) {
    if (!path) return 1;
    const wchar_t *p = path;
    while (*p) {
        while (*p == L'\\' || *p == L'/') p++;
        const wchar_t *start = p;
        while (*p && *p != L'\\' && *p != L'/') p++;
        size_t n = (size_t)(p - start);
        if ((n == 1u && start[0] == L'.') ||
            (n == 2u && start[0] == L'.' && start[1] == L'.')) return 1;
    }
    return 0;
}

/* Resolve dot components before adding the extended-length prefix.  The
 * general writable VFS intentionally accepts relative roots such as
 * `SR_FSDIR=../fs`; rejecting the `..` that results from joining that path to
 * the CWD would break save/storage I/O.  Windows' full-path resolver gives
 * those paths their normal drive/UNC semantics, after which the extended APIs
 * receive a canonical path with no dot components. */
static int sr_wide_full_path_alloc(const wchar_t *input, wchar_t **out) {
    if (!input || !out) return 0;
    *out = NULL;
    DWORD capacity = 512u;
    for (;;) {
        if (capacity > (DWORD)(SIZE_MAX / sizeof(wchar_t))) return 0;
        wchar_t *resolved = (wchar_t *)malloc((size_t)capacity * sizeof(*resolved));
        if (!resolved) return 0;
        DWORD length = GetFullPathNameW(input, capacity, resolved, NULL);
        if (length == 0) {
            free(resolved);
            return 0;
        }
        if (length < capacity) {
            *out = resolved;
            return 1;
        }
        free(resolved);
        if (length == UINT32_MAX || length + 1u < length) return 0;
        capacity = length + 1u;
    }
}

static int sr_wide_extended_absolute_alloc(const wchar_t *absolute, wchar_t **out) {
    if (!absolute || !out || !sr_wide_is_absolute(absolute) ||
        sr_wide_has_dot_component(absolute)) return 0;
    if (sr_wide_is_extended(absolute) && !sr_wide_extended_filesystem_root(absolute)) return 0;
    if (absolute[0] == L'\\' && absolute[1] == L'\\' &&
        (absolute[2] == L'.' || absolute[2] == L'?') && !sr_wide_is_extended(absolute)) return 0;
    *out = NULL;
    size_t n = wcslen(absolute);
    const wchar_t *suffix = absolute;
    const wchar_t *prefix = L"\\\\?\\";
    size_t prefix_len = 4u;
    if (sr_wide_is_extended(absolute)) {
        if (n == SIZE_MAX || n + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
        wchar_t *copy = (wchar_t *)malloc((n + 1u) * sizeof(*copy));
        if (!copy) return 0;
        memcpy(copy, absolute, (n + 1u) * sizeof(*copy));
        *out = copy;
        return 1;
    }
    if (absolute[0] == L'\\' && absolute[1] == L'\\') {
        prefix = L"\\\\?\\UNC\\";
        prefix_len = 8u;
        suffix = absolute + 2;
        n -= 2u;
    }
    if (n == SIZE_MAX || prefix_len > SIZE_MAX - n - 1u) return 0;
    size_t total = prefix_len + n + 1u;
    if (total > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *extended = (wchar_t *)malloc(total * sizeof(*extended));
    if (!extended) return 0;
    memcpy(extended, prefix, prefix_len * sizeof(*extended));
    memcpy(extended + prefix_len, suffix, (n + 1u) * sizeof(*extended));
    *out = extended;
    return 1;
}

int sr_wide_configured_root_wide_alloc(const wchar_t *value, wchar_t **out) {
    if (!value || !out || !value[0]) return 0;
    *out = NULL;
    size_t n = wcslen(value);
    if (n == SIZE_MAX || n + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *wide = (wchar_t *)malloc((n + 1u) * sizeof(*wide));
    if (!wide) return 0;
    memcpy(wide, value, (n + 1u) * sizeof(*wide));
    for (wchar_t *p = wide; *p; p++) if (*p == L'/') *p = L'\\';
    if (!(sr_wide_is_extended(wide) || sr_wide_is_absolute(wide))) {
        free(wide);
        return 0;
    }
    if (sr_wide_has_dot_component(wide)) {
        wchar_t *canonical = NULL;
        if (!sr_wide_full_path_alloc(wide, &canonical)) {
            free(wide);
            return 0;
        }
        free(wide);
        wide = canonical;
    }
    int ok = sr_wide_extended_absolute_alloc(wide, out);
    free(wide);
    return ok;
}

/* Resolve a UTF-8 path to an absolute extended-length Windows path. */
int sr_wide_path_alloc(const char *utf8_path, wchar_t **out) {
    if (!utf8_path || !out || !utf8_path[0]) return 0;
    *out = NULL;
    wchar_t *raw = NULL;
    if (!sr_utf8_to_wide_alloc(utf8_path, &raw)) return 0;
    for (wchar_t *p = raw; *p; p++) if (*p == L'/') *p = L'\\';
    wchar_t *absolute = NULL;
    if (sr_wide_is_extended(raw) || sr_wide_is_absolute(raw)) {
        size_t n = wcslen(raw);
        if (n != SIZE_MAX && n + 1u <= SIZE_MAX / sizeof(wchar_t)) {
            absolute = (wchar_t *)malloc((n + 1u) * sizeof(*absolute));
            if (absolute) memcpy(absolute, raw, (n + 1u) * sizeof(*absolute));
        }
    } else {
        wchar_t *cwd = NULL;
        if (sr_wide_get_current_directory(&cwd)) {
            sr_wide_join_alloc(cwd, raw, &absolute);
            free(cwd);
        }
    }
    free(raw);
    if (!absolute) return 0;
    if (sr_wide_has_dot_component(absolute)) {
        wchar_t *canonical = NULL;
        if (!sr_wide_full_path_alloc(absolute, &canonical)) {
            free(absolute);
            return 0;
        }
        free(absolute);
        absolute = canonical;
    }
    int ok = sr_wide_extended_absolute_alloc(absolute, out);
    free(absolute);
    return ok;
}

int sr_wide_module_dir_alloc(wchar_t **out) {
    if (!out) return 0;
    *out = NULL;
    DWORD capacity = 512u;
    for (;;) {
        wchar_t *module = (wchar_t *)malloc((size_t)capacity * sizeof(*module));
        if (!module) return 0;
        DWORD n = GetModuleFileNameW(NULL, module, capacity);
        if (n == 0) { free(module); return 0; }
        if (n < capacity - 1u) {
            while (n > 0 && module[n - 1] != L'\\' && module[n - 1] != L'/') n--;
            if (n == 0) { free(module); return 0; }
            module[n - 1] = L'\0';
            *out = module;
            return 1;
        }
        free(module);
        if (capacity > (DWORD)(SIZE_MAX / (2u * sizeof(*module)))) return 0;
        capacity *= 2u;
    }
}

int sr_wide_parent_alloc(const wchar_t *path, wchar_t **out) {
    if (!path || !out) return 0;
    *out = NULL;
    size_t n = wcslen(path);
    while (n > 0 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) n--;
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
    if (n == 0) return 0;
    /* Keep a drive root's trailing separator; a bare `C:` is drive-relative. */
    if (n == 2u && path[1] == L':') n = 3u;
    if (n == SIZE_MAX || n + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *parent = (wchar_t *)malloc((n + 1u) * sizeof(*parent));
    if (!parent) return 0;
    memcpy(parent, path, n * sizeof(*parent));
    parent[n] = L'\0';
    *out = parent;
    return 1;
}

/* Final path component of `path`, allocating. Mirrors the separator handling in
 * sr_wide_parent_alloc so the two agree on what a path's parent and name are. */
int sr_wide_basename_alloc(const wchar_t *path, wchar_t **out) {
    if (!path || !out) return 0;
    *out = NULL;
    size_t n = wcslen(path);
    while (n > 0 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) n--;
    size_t end = n;
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
    if (end == 0 || end <= n) return 0;
    size_t len = end - n;
    if (len + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *base = (wchar_t *)malloc((len + 1u) * sizeof(*base));
    if (!base) return 0;
    memcpy(base, path + n, len * sizeof(*base));
    base[len] = L'\0';
    *out = base;
    return 1;
}

int sr_wide_module_font_root(wchar_t **out) {
    if (!out) return 0;
    *out = NULL;
    wchar_t *module_dir = NULL;
    wchar_t *font = NULL;
    int ok = sr_wide_module_dir_alloc(&module_dir) &&
             sr_wide_join_alloc(module_dir, L"font", &font) &&
             sr_wide_extended_absolute_alloc(font, out);
    free(module_dir);
    free(font);
    return ok;
}

int sr_stream_size_u32(FILE *stream, uint32_t *size_out) {
    if (!stream || !size_out || _fseeki64(stream, 0, SEEK_END) != 0) return 0;
    __int64 end = _ftelli64(stream);
    int ok = end >= 0 && (uint64_t)end <= UINT32_MAX;
    if (_fseeki64(stream, 0, SEEK_SET) != 0) ok = 0;
    if (ok) *size_out = (uint32_t)end;
    return ok;
}

int sr_ensure_directory_utf8(const char *path) {
    wchar_t *wide = NULL;
    if (!sr_wide_path_alloc(path, &wide)) return 0;
    BOOL made = CreateDirectoryW(wide, NULL);
    DWORD error = made ? ERROR_SUCCESS : GetLastError();
    DWORD attributes = (made || error != ERROR_ALREADY_EXISTS)
        ? FILE_ATTRIBUTE_DIRECTORY
        : GetFileAttributesW(wide);
    free(wide);
    return made || (error == ERROR_ALREADY_EXISTS &&
                    attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

/* Contained FILE* open under the unified Memory Stick root: OPEN -> HANDLE ->
 * FINAL PATH VERIFY, then hand the verified handle to the CRT. Maps the wide
 * fopen mode onto CreateFileW access/disposition (r/w/a/+ and binary); EXISTENCE
 * overrides the creation disposition the mode implies. */
FILE *ms0_fopen_existence_utf8(const char *host_path, const wchar_t *mode,
                                      Ms0OpenExistence existence) {
    if (!host_path || !mode || !mode[0]) return NULL;
    wchar_t canonical[MAX_PATH * 2];
    if (!sr_vfs_canonical_root(sr_ms0_root(), canonical,
                               sizeof(canonical) / sizeof(wchar_t)))
        return NULL;
    int has_w = 0, has_a = 0, has_plus = 0;
    for (const wchar_t *p = mode; *p; p++) {
        if (*p == L'r') { /* read implied by access default */ }
        else if (*p == L'w') has_w = 1;
        else if (*p == L'a') has_a = 1;
        else if (*p == L'+') has_plus = 1;
    }
    DWORD access = GENERIC_READ;
    if (has_w || has_a || has_plus) access |= GENERIC_WRITE;
    DWORD disposition = OPEN_EXISTING;
    if (existence == MS0_OPEN_NEW) disposition = CREATE_NEW;
    else if (existence == MS0_OPEN_EXISTING) disposition = has_w ? TRUNCATE_EXISTING : OPEN_EXISTING;
    else if (has_w) disposition = CREATE_ALWAYS;
    else if (has_a) disposition = OPEN_ALWAYS;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!sr_vfs_open_contained_utf8(host_path, access, 0, disposition, canonical, &h))
        return NULL;
    int oflag = _O_BINARY | ((access & GENERIC_WRITE) ? _O_RDWR : _O_RDONLY);
    if (has_a) oflag |= _O_APPEND;
    int fd = _open_osfhandle((intptr_t)h, oflag);
    if (fd < 0) {
        CloseHandle(h);
        return NULL;
    }
    char narrow[16];
    size_t i = 0;
    for (; mode[i] && i + 1 < sizeof(narrow); i++) narrow[i] = (char)mode[i];
    narrow[i] = '\0';
    FILE *fp = _fdopen(fd, narrow);
    if (!fp) {
        _close(fd);
        return NULL;
    }
    return fp;
}

FILE *ms0_fopen_utf8(const char *host_path, const wchar_t *mode) {
    return ms0_fopen_existence_utf8(host_path, mode, MS0_OPEN_BY_MODE);
}

#else /* !_WIN32: the same host-path layer expressed with POSIX primitives ----- */
/* The Windows layer above exists for two reasons that have no POSIX form: the
 * UTF-16 Win32 call surface, and handle-based containment verified through
 * \\?\-prefixed final paths. The conversions, joins, environment reads, module
 * directory and stream sizing below are ordinary host operations and are
 * re-expressed here with the POSIX primitive that performs them. One Win32-only
 * concept is NOT simulated:
 *
 *   - the \\?\ extended-length prefix: POSIX paths are bounded by the host, not
 *     by MAX_PATH, so sr_wide_extended_absolute_alloc is the identity copy and
 *     the 'is this already extended/absolute' predicates report what this host
 *     actually has;
 *   - the \\?\ extended-length prefix: POSIX paths are bounded by the host, not
 *     by MAX_PATH, so sr_wide_extended_absolute_alloc is the identity copy and
 *     the 'is this already extended/absolute' predicates report what this host
 *     actually has. */

int sr_utf8_to_wide_alloc(const char *src, wchar_t **out) {
    if (!src || !out || !sr_asset_index_valid_utf8(src)) return 0;
    *out = NULL;
    size_t n = strlen(src);
    if (n > (size_t)INT_MAX) return 0;
    wchar_t *base = (wchar_t *)calloc(n + 1u, sizeof(wchar_t));
    if (!base) return 0;
    size_t at = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
        wchar_t unit;
        if (*p < 0x80u) {
            unit = (wchar_t)*p++;
        } else if ((*p & 0xE0u) == 0xC0u && p[1] && (p[1] & 0xC0u) == 0x80u) {
            unit = (wchar_t)(((uint32_t)(*p & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu));
            if ((uint32_t)unit < 0x80u) { free(base); return 0; }   /* overlong */
            p += 2;
        } else if ((*p & 0xF0u) == 0xE0u && p[1] && p[2] &&
                   (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
            unit = (wchar_t)(((uint32_t)(*p & 0x0Fu) << 12) |
                             ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu));
            if ((uint32_t)unit < 0x800u) { free(base); return 0; }   /* overlong */
            p += 3;
        } else if ((*p & 0xF8u) == 0xF0u && p[1] && p[2] && p[3] &&
                   (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u &&
                   (p[3] & 0xC0u) == 0x80u) {
            uint32_t cp = ((uint32_t)(*p & 0x07u) << 18) |
                          ((uint32_t)(p[1] & 0x3Fu) << 12) |
                          ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
            if (cp < 0x10000u || cp > 0x10FFFFu) { free(base); return 0; }
            if (cp >= 0xD800u && cp <= 0xDFFFu) { free(base); return 0; } /* surrogate */
            unit = (wchar_t)cp;
            p += 4;
        } else {
            free(base);
            return 0;
        }
        if (sizeof(wchar_t) < 4u && unit > 0xFFFFu) { free(base); return 0; }
        base[at++] = unit;
    }
    *out = base;
    return 1;
}

int sr_wide_to_utf8_alloc(const wchar_t *src, char **out) {
    if (!src || !out) return 0;
    *out = NULL;
    size_t n = wcslen(src);
    if (n > (size_t)INT_MAX) return 0;
    char *utf8 = (char *)malloc(n * 4u + 1u);
    if (!utf8) return 0;
    size_t at = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = (uint32_t)src[i];
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) { free(utf8); return 0; }
        if (cp < 0x80u) {
            utf8[at++] = (char)cp;
        } else if (cp < 0x800u) {
            utf8[at++] = (char)(0xC0u | (cp >> 6));
            utf8[at++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            utf8[at++] = (char)(0xE0u | (cp >> 12));
            utf8[at++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            utf8[at++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            utf8[at++] = (char)(0xF0u | (cp >> 18));
            utf8[at++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            utf8[at++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            utf8[at++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    utf8[at] = '\0';
    *out = utf8;
    return 1;
}

/* POSIX environment names are byte strings, so the name literal crosses from
 * this translation unit's wchar_t to the host's char exactly as it crosses to
 * the wide Win32 environment there. */
int sr_wide_env_alloc(const wchar_t *name, wchar_t **value_out,
                             int *present_out) {
    if (!name || !value_out || !present_out) return 0;
    *value_out = NULL;
    *present_out = 0;
    char *narrow_name = NULL;
    if (!sr_wide_to_utf8_alloc(name, &narrow_name)) return 0;
    const char *value = getenv(narrow_name);
    free(narrow_name);
    if (!value) return 1;
    wchar_t *wide = NULL;
    if (!sr_utf8_to_wide_alloc(value, &wide)) return 0;
    *value_out = wide;
    *present_out = 1;
    return 1;
}

/* '/' is this host's separator. A backslash is an ordinary filename byte here,
 * so the join inserts one separator and rewrites nothing. */
int sr_wide_join_alloc(const wchar_t *left, const wchar_t *right,
                              wchar_t **out) {
    if (!left || !right || !out) return 0;
    *out = NULL;
    size_t a = wcslen(left), b = wcslen(right);
    int separator = a > 0 && b > 0 && left[a - 1] != L'/';
    size_t extra = b;
    if (extra > SIZE_MAX - (size_t)separator - 1u) return 0;
    extra += (size_t)separator + 1u;
    if (a > SIZE_MAX - extra) return 0;
    size_t total = a + extra;
    if (total > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *joined = (wchar_t *)malloc(total * sizeof(*joined));
    if (!joined) return 0;
    memcpy(joined, left, a * sizeof(*joined));
    size_t at = a;
    if (separator) joined[at++] = L'/';
    memcpy(joined + at, right, b * sizeof(*joined));
    joined[at + b] = L'\0';
    *out = joined;
    return 1;
}

static int sr_wide_get_current_directory(wchar_t **out) {
    if (!out) return 0;
    *out = NULL;
    size_t capacity = 256u;
    for (;;) {
        char *buf = (char *)malloc(capacity);
        if (!buf) return 0;
        if (getcwd(buf, capacity)) {
            int ok = sr_utf8_to_wide_alloc(buf, out);
            free(buf);
            return ok;
        }
        int grow = errno == ERANGE && capacity <= (size_t)INT_MAX / 2u;
        free(buf);
        if (!grow) return 0;
        capacity *= 2u;
    }
}

/* Only this host's own absolute-path form exists here: a leading '/'. The
 * \\?\ extended form and the drive-letter form of the Windows layer have no
 * counterpart on this host, so they are not defined at all -- no predicate
 * that can only ever answer "no" is compiled here. */
static int sr_wide_is_absolute(const wchar_t *path) {
    return path && path[0] == L'/';
}

/* Same rule as the Windows layer: a '.' or '..' component is refused here
 * rather than handed to the host to resolve. */
static int sr_wide_has_dot_component(const wchar_t *path) {
    if (!path) return 1;
    const wchar_t *p = path;
    while (*p) {
        while (*p == L'/') p++;
        const wchar_t *start = p;
        while (*p && *p != L'/') p++;
        size_t n = (size_t)(p - start);
        if ((n == 1u && start[0] == L'.') ||
            (n == 2u && start[0] == L'.' && start[1] == L'.')) return 1;
    }
    return 0;
}

/* POSIX resolves '.' and '..' inside the kernel, so this only anchors a
 * relative input to the working directory and leaves the components intact. */
static int sr_wide_full_path_alloc(const wchar_t *input, wchar_t **out) {
    if (!input || !out) return 0;
    *out = NULL;
    if (sr_wide_is_absolute(input)) {
        size_t n = wcslen(input);
        if (n > SIZE_MAX - 1u || n + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
        wchar_t *copy = (wchar_t *)malloc((n + 1u) * sizeof(*copy));
        if (!copy) return 0;
        memcpy(copy, input, (n + 1u) * sizeof(*copy));
        *out = copy;
        return 1;
    }
    wchar_t *cwd = NULL;
    if (!sr_wide_get_current_directory(&cwd)) return 0;
    wchar_t *joined = NULL;
    int ok = sr_wide_join_alloc(cwd, input, &joined);
    free(cwd);
    if (!ok) return 0;
    *out = joined;
    return 1;
}

static int sr_wide_extended_absolute_alloc(const wchar_t *absolute, wchar_t **out) {
    if (!absolute || !out || !sr_wide_is_absolute(absolute) ||
        sr_wide_has_dot_component(absolute)) return 0;
    size_t n = wcslen(absolute);
    if (n > SIZE_MAX - 1u || n + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *copy = (wchar_t *)malloc((n + 1u) * sizeof(*copy));
    if (!copy) return 0;
    memcpy(copy, absolute, (n + 1u) * sizeof(*copy));
    *out = copy;
    return 1;
}

int sr_wide_configured_root_wide_alloc(const wchar_t *value, wchar_t **out) {
    if (!value || !out || !value[0]) return 0;
    *out = NULL;
    size_t n = wcslen(value);
    if (n > SIZE_MAX - 1u || n + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *wide = (wchar_t *)malloc((n + 1u) * sizeof(*wide));
    if (!wide) return 0;
    memcpy(wide, value, (n + 1u) * sizeof(*wide));
    /* Absolute on both hosts, as on Win32: a relative value is refused here and is never anchored to the
     * working directory, which in a child process is a different directory from the one the caller meant. */
    if (!sr_wide_is_absolute(wide)) {
        free(wide);
        return 0;
    }
    int ok = sr_wide_extended_absolute_alloc(wide, out);
    free(wide);
    return ok;
}

/* Resolve a UTF-8 path to an absolute host path. */
int sr_wide_path_alloc(const char *utf8_path, wchar_t **out) {
    if (!utf8_path || !out || !utf8_path[0]) return 0;
    *out = NULL;
    wchar_t *raw = NULL;
    if (!sr_utf8_to_wide_alloc(utf8_path, &raw)) return 0;
    wchar_t *absolute = NULL;
    if (sr_wide_is_absolute(raw)) {
        size_t n = wcslen(raw);
        if (n != SIZE_MAX && n + 1u <= SIZE_MAX / sizeof(wchar_t)) {
            absolute = (wchar_t *)malloc((n + 1u) * sizeof(*absolute));
            if (absolute) memcpy(absolute, raw, (n + 1u) * sizeof(*absolute));
        }
    } else {
        wchar_t *cwd = NULL;
        if (sr_wide_get_current_directory(&cwd)) {
            sr_wide_join_alloc(cwd, raw, &absolute);
            free(cwd);
        }
    }
    free(raw);
    if (!absolute) return 0;
    if (sr_wide_has_dot_component(absolute)) {
        wchar_t *canonical = NULL;
        if (!sr_wide_full_path_alloc(absolute, &canonical)) {
            free(absolute);
            return 0;
        }
        free(absolute);
        absolute = canonical;
    }
    int ok = sr_wide_extended_absolute_alloc(absolute, out);
    free(absolute);
    return ok;
}

/* Split rules agree with the Windows layer: a path's parent and name are
 * taken at the last separator, and a trailing separator is not part of a name. */
int sr_wide_parent_alloc(const wchar_t *path, wchar_t **out) {
    if (!path || !out) return 0;
    *out = NULL;
    size_t n = wcslen(path);
    while (n > 0u && path[n - 1u] == L'/') n--;      /* ignore trailing separators */
    if (n == 0u) return 0;
    size_t at = n;
    while (at > 0u && path[at - 1u] != L'/') at--;
    if (at == 0u) return 0;
    while (at > 1u && path[at - 1u] == L'/') at--;    /* drop the separator itself */
    wchar_t *parent = (wchar_t *)malloc((at + 1u) * sizeof(*parent));
    if (!parent) return 0;
    memcpy(parent, path, at * sizeof(*parent));
    parent[at] = L'\0';
    *out = parent;
    return 1;
}

/* Case-insensitive wide compare: the Windows layer uses _wcsicmp here, and a
 * host directory name is just as case-folded on this host. ASCII fold only --
 * the names compared are the project's own ASCII tree names. */
int sr_wide_stricmp(const wchar_t *left, const wchar_t *right) {
    if (!left || !right) return left == right ? 0 : 1;
    while (*left && *right) {
        wchar_t a = *left, b = *right;
        if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
        if (a != b) return (int)a - (int)b;
        left++;
        right++;
    }
    return (int)*left - (int)*right;
}

int sr_wide_basename_alloc(const wchar_t *path, wchar_t **out) {
    if (!path || !out) return 0;
    *out = NULL;
    size_t n = wcslen(path);
    while (n > 0u && path[n - 1u] == L'/') n--;
    if (n == 0u) return 0;
    size_t at = n;
    while (at > 0u && path[at - 1u] != L'/') at--;
    wchar_t *name = (wchar_t *)malloc((n - at + 1u) * sizeof(*name));
    if (!name) return 0;
    memcpy(name, path + at, (n - at + 1u) * sizeof(*name));
    *out = name;
    return 1;
}

/* The directory this executable was loaded from: /proc/self/exe here, with the
 * configured SR_EXE_DIR override honoured first. SR_EXE_DIR is read on POSIX only: the Win32 layer has
 * no reader for it in this tree, so the override does not apply there. */
int sr_wide_module_dir_alloc(wchar_t **out) {
    if (!out) return 0;
    *out = NULL;
    wchar_t *configured = NULL;
    int present = 0;
    if (sr_wide_env_alloc(L"SR_EXE_DIR", &configured, &present) && present) {
        if (configured[0]) {
            wchar_t *full = NULL;
            int ok = sr_wide_configured_root_wide_alloc(configured, &full);
            free(configured);
            *out = full;
            return ok;
        }
        free(configured);
    }
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1u);
    if (n <= 0) return 0;
    buf[n] = '\0';
    wchar_t *full = NULL;
    if (!sr_utf8_to_wide_alloc(buf, &full)) return 0;
    wchar_t *dir = NULL;
    int ok = sr_wide_parent_alloc(full, &dir);
    free(full);
    *out = dir;
    return ok;
}

int sr_wide_module_font_root(wchar_t **out) {
    if (!out) return 0;
    wchar_t *configured = NULL;
    int present = 0;
    if (sr_wide_env_alloc(L"SR_FONTDIR", &configured, &present) && present &&
        configured[0]) {
        int ok = sr_wide_configured_root_wide_alloc(configured, out);
        free(configured);
        return ok;
    }
    free(configured);
    wchar_t *module_dir = NULL, *repo_dir = NULL, *candidate = NULL;
    int ok = sr_wide_module_dir_alloc(&module_dir) &&
             sr_wide_parent_alloc(module_dir, &repo_dir) &&
             sr_wide_join_alloc(repo_dir, L"font", &candidate) &&
             sr_wide_extended_absolute_alloc(candidate, out);
    free(module_dir);
    free(repo_dir);
    free(candidate);
    return ok;
}

/* This host takes a byte path, so the mode crosses from the caller's wchar_t
 * literal (always one of the ASCII r/w/a/+ letters) and fopen does the work. */
int sr_narrow_mode_alloc(const wchar_t *mode, char **out) {
    if (!mode || !out) return 0;
    *out = NULL;
    size_t n = wcslen(mode);
    if (n == 0u || n > 15u) return 0;
    char *narrow = (char *)malloc(n + 1u);
    if (!narrow) return 0;
    for (size_t i = 0; i < n; i++) {
        if ((uint32_t)mode[i] > 0x7Fu) { free(narrow); return 0; }
        narrow[i] = (char)mode[i];
    }
    narrow[n] = '\0';
    *out = narrow;
    return 1;
}

int sr_stream_size_u32(FILE *stream, uint32_t *size_out) {
    if (!stream || !size_out) return 0;
    if (fseeko(stream, 0, SEEK_END) != 0) return 0;
    off_t end = ftello(stream);
    int ok = end >= 0 && (uint64_t)end <= UINT32_MAX;
    if (fseeko(stream, 0, SEEK_SET) != 0) ok = 0;
    if (ok) *size_out = (uint32_t)end;
    return ok;
}

int sr_ensure_directory_utf8(const char *path) {
    if (!path || !path[0]) return 0;
    if (mkdir(path, 0777) == 0) return 1;
    if (errno != EEXIST) return 0;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The guest path is reduced to the seam's canonical root-relative grammar
 * before any host operation. PSP mount-root separators are consumed once;
 * host-absolute paths, foreign devices, traversal, repeated separators and
 * overlong components fail with the PSP illegal-path result. */
sr_cd_status ms0_posix_guest_relpath(const char *guest, char *rel, size_t cap) {
    return sr_cd_ms0_guest_relpath(guest, rel, cap);
}

static sr_cd_status ms0_posix_host_relpath(const char *host_path, char *rel, size_t cap) {
    const char *root = sr_ms0_root();
    if (!host_path || !root || !root[0] || !rel || cap == 0) return SR_CD_INVALID_PATH;
    size_t root_len = strlen(root);
    if (strncmp(host_path, root, root_len) != 0) return SR_CD_INVALID_PATH;
    const char *tail = host_path + root_len;
    if (root[root_len - 1u] != '/') {
        if (*tail != '/') return SR_CD_INVALID_PATH;
        tail++;
    }
    size_t len = strlen(tail);
    if (len == 0 || len >= cap) return SR_CD_INVALID_PATH;
    memcpy(rel, tail, len + 1u);
    return sr_cd_rel_is_acceptable(rel) ? SR_CD_OK : SR_CD_INVALID_PATH;
}

uint32_t ms0_posix_psp_error(sr_cd_status status) {
    return sr_cd_psp_error(status);
}

sr_cd_status ms0_posix_root_open(sr_cd_root *root) {
    if (!sr_ensure_directory_utf8(sr_ms0_root())) return SR_CD_IO_ERROR;
    return sr_cd_root_open(sr_ms0_root(), root);
}

static FILE *ms0_fopen_rel_utf8(const char *rel, const wchar_t *mode,
                                Ms0OpenExistence existence, sr_cd_status *status_out) {
    if (status_out) *status_out = SR_CD_INVALID_PATH;
    if (!rel || !mode || !mode[0]) return NULL;
    char parent[SR_CD_REL_MAX], leaf[SR_CD_NAME_MAX];
    if (!sr_cd_rel_split(rel, parent, sizeof(parent), leaf, sizeof(leaf))) return NULL;

    int has_w = 0, has_a = 0, has_plus = 0;
    for (const wchar_t *p = mode; *p; p++) {
        if (*p == L'w') has_w = 1;
        else if (*p == L'a') has_a = 1;
        else if (*p == L'+') has_plus = 1;
        else if (*p != L'r' && *p != L'b') return NULL;
    }
    if ((has_w && has_a) || ((has_w || has_a) && !has_plus)) return NULL;
    if (existence == MS0_OPEN_NEW && !has_plus) return NULL;
    sr_cd_file_mode file_mode = has_w ? SR_CD_FILE_READ_WRITE_TRUNCATE :
                                has_a ? SR_CD_FILE_READ_WRITE_APPEND :
                                has_plus ? SR_CD_FILE_READ_WRITE : SR_CD_FILE_READ;
    if (existence == MS0_OPEN_NEW)
        file_mode = has_a ? SR_CD_FILE_READ_WRITE_NEW_APPEND : SR_CD_FILE_READ_WRITE_NEW;
    else if (existence == MS0_OPEN_EXISTING && (has_w || has_a))
        file_mode = has_w ? SR_CD_FILE_READ_WRITE_EXISTING_TRUNCATE :
                            SR_CD_FILE_READ_WRITE_EXISTING_APPEND;

    sr_cd_root root;
    sr_cd_status st = ms0_posix_root_open(&root);
    if (st != SR_CD_OK) {
        if (status_out) *status_out = st;
        return NULL;
    }
    FILE *stream = NULL;
    st = sr_cd_open_file(&root, parent, leaf, file_mode, &stream);
    sr_cd_root_close(&root);
    if (status_out) *status_out = st;
    return st == SR_CD_OK ? stream : NULL;
}

FILE *ms0_fopen_guest_utf8(const char *guest_path, const wchar_t *mode,
                                  Ms0OpenExistence existence, sr_cd_status *status_out) {
    char rel[SR_CD_REL_MAX];
    sr_cd_status st = ms0_posix_guest_relpath(guest_path, rel, sizeof(rel));
    if (st != SR_CD_OK) {
        if (status_out) *status_out = st;
        return NULL;
    }
    return ms0_fopen_rel_utf8(rel, mode, existence, status_out);
}

/* Compatibility helper for the legacy-flat import, whose destination was
 * already resolved beneath the unified root. The final open still enters the
 * descriptor-relative seam and never opens the joined pathname. */
FILE *ms0_fopen_utf8(const char *host_path, const wchar_t *mode) {
    char rel[SR_CD_REL_MAX];
    if (ms0_posix_host_relpath(host_path, rel, sizeof(rel)) != SR_CD_OK) return NULL;
    return ms0_fopen_rel_utf8(rel, mode, MS0_OPEN_BY_MODE, NULL);
}
#endif /* _WIN32 */

char *host_path_alloc(const char *guest) {
    if (!guest || !guest[0]) return NULL;
    const char *root = sr_ms0_root();
    if (!sr_ensure_directory_utf8(root)) return NULL;
    size_t root_len = strlen(root), guest_len = strlen(guest);
    if (root_len > SIZE_MAX - 2u || guest_len > SIZE_MAX - root_len - 2u) return NULL;
    size_t cap = root_len + guest_len + 2u;
    char *path = (char *)malloc(cap);
    if (!path) return NULL;
    if (!sr_ms0_resolve(root, guest, path, cap, SR_HOST_PATH_SEP)) {
        free(path);
        return NULL;
    }
    return path;
}

/* Atomic count of successful legacy flat fs/ -> unified-ms-root imports so
 * source-owned tests can assert the copy-only path without parsing logs. */
atomic_ulong s_ms0_legacy_import_count;

/* Copy-only legacy import (read-open miss, never write/create): if the guest
 * file exists under the old flat SR_FSDIR mapping, copy it into the unified
 * hierarchical host path so prior flat saves remain visible. */
int ms0_try_legacy_flat_import(const char *guest_path, const char *host_path) {
    if (!guest_path || !host_path) return 0;
    char *configured = NULL;
    int present = 0;
    if (!sr_utf8_env_alloc(L"SR_FSDIR", &configured, &present)) return 0;
    const char *dir = present && configured[0] ? configured : "fs";
    size_t root_len = strlen(dir), guest_len = strlen(guest_path);
    if (root_len > SIZE_MAX - 2u || guest_len > SIZE_MAX - root_len - 2u) {
        free(configured);
        return 0;
    }
    size_t cap = root_len + guest_len + 2u;
    char *flat = (char *)malloc(cap);
    if (!flat) {
        free(configured);
        return 0;
    }
    int n = sr_vfs_host_flat_path(dir, guest_path, flat, cap);
    free(configured);
    if (n <= 0) {
        free(flat);
        return 0;
    }
    FILE *src = sr_fopen_utf8(flat, L"rb");
    free(flat);
    if (!src) return 0;
    FILE *dst = ms0_fopen_utf8(host_path, L"w+b");
    if (!dst) {
        fclose(src);
        return 0;
    }
    char buf[4096];
    size_t got;
    int ok = 1;
    while ((got = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, got, dst) != got) {
            ok = 0;
            break;
        }
    }
    if (ferror(src)) ok = 0;
    fclose(src);
    if (fclose(dst) != 0) ok = 0;
    if (!ok) return 0;
    atomic_fetch_add_explicit(&s_ms0_legacy_import_count, 1u, memory_order_release);
    fprintf(stderr,
            "ms0: imported legacy flat file '%s' from SR_FSDIR into unified Memory Stick root\n",
            guest_path);
    return 1;
}
/* Defined with the flash0: device roots below. */
void flash0_copy_root(char *dst, size_t capacity, const char *src);
void flash0_user_data_root(char *dst, size_t capacity);
/* Roots for the flash0: font device (flash0_font.c). The user-imported cache lives under the
 * per-user data directory that SR_USER_DATA_ROOT names (see flash0_user_data_root). Project fonts
 * resolve like font_load: SR_FONTDIR when it is set,
 * otherwise the executable's font directory. Both are returned as UTF-8 paths. A root that is
 * unknown or too long stays empty, and the device skips that source. */
void flash0_copy_root(char *dst, size_t capacity, const char *src) {
    dst[0] = '\0';
    if (!src || strlen(src) >= capacity) return;
    memcpy(dst, src, strlen(src) + 1u);
}

/* SR_USER_DATA_ROOT is the per-user data directory the launcher resolved; the user-imported cache is
 * <that>/fonts/v2. It is validated the way SR_FONTDIR is. Unset (a direct runtime launch), the
 * platform's per-user directory is used. Set but unusable, the user source is empty and nothing else
 * is tried: the platform directory would be a different root from the one the launcher named. The
 * refusal is named once, because the device resolves its roots on every open. */
static atomic_int s_user_data_root_refusal_named;

static void flash0_refuse_user_data_root(const char *reason) {
    if (atomic_exchange_explicit(&s_user_data_root_refusal_named, 1, memory_order_relaxed)) return;
    fprintf(stderr, "flash0: SR_USER_DATA_ROOT %s; the user-imported font source is skipped\n", reason);
}

void flash0_user_data_root(char *dst, size_t capacity) {
    dst[0] = '\0';
    wchar_t *configured = NULL;
    int configured_present = 0;
    if (!sr_wide_env_alloc(L"SR_USER_DATA_ROOT", &configured, &configured_present)) {
        flash0_refuse_user_data_root("could not be read");
        return;
    }
    if (!configured_present) {
        char data_dir[SR_FLASH0_PATH_MAX];
        if (nk_platform_get_path(NK_PATH_DATA, data_dir, sizeof(data_dir)))
            flash0_copy_root(dst, capacity, data_dir);
        return;
    }
    wchar_t *root = NULL;
    char *root_utf8 = NULL;
    /* Absolute on every host: sr_wide_is_absolute is this host's own rule (a drive or UNC path on
     * Win32, a leading '/' on POSIX). The wide helper refuses a relative value on both hosts too; this
     * check comes first so the refusal is named here with the diagnostic below. */
    int valid = configured[0] != L'\0' && sr_wide_is_absolute(configured) &&
                sr_wide_configured_root_wide_alloc(configured, &root) &&
                sr_wide_to_utf8_alloc(root, &root_utf8);
    free(configured);
    free(root);
    if (!valid) {
        flash0_refuse_user_data_root("is configured but is not a valid absolute path");
    } else if (strlen(root_utf8) >= SR_FLASH0_PATH_MAX) {
        flash0_refuse_user_data_root("is configured but is too long");
    } else {
        flash0_copy_root(dst, capacity, root_utf8);
    }
    free(root_utf8);
}

/* SR_FONTDIR set but refused (unreadable, empty, or relative) leaves the project source empty, and the
 * refusal is named once here, as font_load names it for sceFont. The device resolves its roots on every
 * open, and nothing else is tried in its place. */
static atomic_int s_font_dir_refusal_named;

void flash0_refuse_font_dir(const char *reason) {
    if (atomic_exchange_explicit(&s_font_dir_refusal_named, 1, memory_order_relaxed)) return;
    fprintf(stderr, "flash0: SR_FONTDIR %s; the project font source is skipped\n", reason);
}
