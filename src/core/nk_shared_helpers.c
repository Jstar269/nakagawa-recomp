/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_shared_helpers.h"
#include "nk_platform.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

typedef struct NkSharedPathJoinPlan {
    size_t left_length;
    size_t right_length;
    size_t total_length;
    size_t separator_length;
} NkSharedPathJoinPlan;

static int path_separator_byte(char value) {
    return value == '/' || value == '\\';
}

static bool path_join_plan(const char *left, const char *right, char separator,
                           NkSharedPathJoinMode mode, unsigned int flags,
                           NkSharedPathJoinPlan *plan) {
    if (!left || !right || !plan ||
        (mode != NK_SHARED_PATH_JOIN_ALWAYS &&
         mode != NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR &&
         mode != NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY)) {
        return false;
    }
    if ((flags & NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT) != 0u &&
        (right[0] == '/' || right[0] == '\\' ||
         (right[0] != '\0' && right[1] == ':'))) {
        return false;
    }

    plan->left_length = strlen(left);
    plan->right_length = strlen(right);
    if ((flags & NK_SHARED_PATH_TRIM_LEFT_SEPARATORS) != 0u) {
        while (plan->left_length > 0u &&
               path_separator_byte(left[plan->left_length - 1u])) {
            plan->left_length--;
        }
    }

    int left_has_separator = plan->left_length > 0u &&
        path_separator_byte(left[plan->left_length - 1u]);
    switch (mode) {
        case NK_SHARED_PATH_JOIN_ALWAYS:
            plan->separator_length = 1u;
            break;
        case NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR:
            plan->separator_length = plan->left_length > 0u && !left_has_separator ? 1u : 0u;
            break;
        case NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY:
            plan->separator_length = plan->left_length > 0u &&
                plan->right_length > 0u && !left_has_separator ? 1u : 0u;
            break;
        default:
            return false;
    }

    if (plan->left_length > SIZE_MAX - plan->separator_length ||
        plan->left_length + plan->separator_length >
            SIZE_MAX - plan->right_length) {
        return false;
    }
    plan->total_length = plan->left_length + plan->separator_length + plan->right_length;
    (void)separator;
    return true;
}

static void path_join_write(char *out, size_t out_capacity,
                            const char *left, const char *right, char separator,
                            const NkSharedPathJoinPlan *plan, unsigned int flags) {
    size_t at = 0u;
    size_t limit = out_capacity - 1u;
    size_t copy_length = plan->left_length < limit ? plan->left_length : limit;
    if (copy_length > 0u) memcpy(out, left, copy_length);
    at += copy_length;

    if (plan->separator_length > 0u && at < limit) out[at++] = separator;

    size_t right_copy = plan->right_length < limit - at ? plan->right_length : limit - at;
    if (right_copy > 0u) memcpy(out + at, right, right_copy);
    at += right_copy;
    out[at] = '\0';

    if (plan->total_length >= out_capacity || plan->total_length > (size_t)INT_MAX) return;
    size_t suffix_start = plan->left_length + plan->separator_length;
    for (size_t i = suffix_start; i < plan->total_length; i++) {
        if ((flags & NK_SHARED_PATH_NORMALIZE_BOTH_SLASHES) != 0u &&
            path_separator_byte(out[i])) {
            out[i] = separator;
        } else if ((flags & NK_SHARED_PATH_NORMALIZE_FORWARD_SLASHES) != 0u &&
                   out[i] == '/') {
            out[i] = separator;
        }
    }
}

bool nk_shared_path_join(char *out, size_t out_capacity,
                         const char *left, const char *right, char separator,
                         NkSharedPathJoinMode mode, unsigned int flags) {
    NkSharedPathJoinPlan plan;
    if (!out || out_capacity == 0u ||
        !path_join_plan(left, right, separator, mode, flags, &plan)) {
        return false;
    }

    bool fits_capacity = plan.total_length < out_capacity;
    if (!fits_capacity && (flags & NK_SHARED_PATH_REQUIRE_FIT) != 0u) return false;

    path_join_write(out, out_capacity, left, right, separator, &plan, flags);
    if (!fits_capacity ||
        (plan.total_length > (size_t)INT_MAX &&
         (flags & NK_SHARED_PATH_REQUIRE_FIT) == 0u)) {
        return false;
    }
    return (flags & NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH) == 0u ||
           plan.total_length > 0u;
}

char *nk_shared_path_join_alloc(const char *left, const char *right, char separator,
                                NkSharedPathJoinMode mode, unsigned int flags) {
    NkSharedPathJoinPlan plan;
    if (!path_join_plan(left, right, separator, mode, flags, &plan) ||
        plan.total_length == SIZE_MAX) {
        return NULL;
    }
    char *out = (char *)malloc(plan.total_length + 1u);
    if (!out) return NULL;
    path_join_write(out, plan.total_length + 1u, left, right, separator, &plan, flags);
    if ((flags & NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH) != 0u &&
        plan.total_length == 0u) {
        free(out);
        return NULL;
    }
    return out;
}

bool nk_shared_text_copy(char *out, size_t out_capacity, const char *text,
                         NkSharedTextCopyMode mode) {
    if (!out || out_capacity == 0u ||
        (mode != NK_SHARED_TEXT_COPY_TRUNCATE &&
         mode != NK_SHARED_TEXT_COPY_REQUIRE_FIT)) {
        return false;
    }
    if (!text) {
        if (mode == NK_SHARED_TEXT_COPY_TRUNCATE) {
            out[0] = '\0';
            return true;
        }
        return false;
    }

    if (mode == NK_SHARED_TEXT_COPY_REQUIRE_FIT) {
        size_t length = strlen(text);
        if (length >= out_capacity) return false;
        memcpy(out, text, length + 1u);
        return true;
    }

    strncpy(out, text, out_capacity - 1u);
    out[out_capacity - 1u] = '\0';
    return true;
}

/* This common-source primitive is also linked by tools that do not link a host backend. */
FILE *nk_fopen_utf8(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide_path[32768];
    WCHAR wide_mode[32];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide_path, (int)(sizeof(wide_path) / sizeof(wide_path[0]))) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1,
                            wide_mode, (int)(sizeof(wide_mode) / sizeof(wide_mode[0]))) <= 0) {
        return NULL;
    }
    return _wfopen(wide_path, wide_mode);
#else
    return fopen(path, mode);
#endif
}

void nk_shared_format_size(char *out, size_t out_capacity, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_capacity, "%.2f GB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_capacity, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_capacity, "%llu bytes", (unsigned long long)bytes);
    }
}

static void boot_event_write_line(FILE *stream, const char *format, uint64_t timestamp_ns,
                                  va_list args) {
    va_list copy;
    va_copy(copy, args);
    vfprintf(stream, format, copy);
    va_end(copy);
    if (timestamp_ns != 0u) {
        fprintf(stream, " t_ns=%llu\n", (unsigned long long)timestamp_ns);
    } else {
        fputc('\n', stream);
    }
}

void nk_shared_boot_event_vwrite(FILE *stderr_stream, uint64_t timestamp_ns,
                                 const char *format,
                                 va_list args) {
    if (!stderr_stream) stderr_stream = stderr;
    boot_event_write_line(stderr_stream, format, timestamp_ns, args);
    fflush(stderr_stream);

    const char *path = getenv("SR_BOOT_EVENT_FILE");
    if (!path || !*path) return;
    FILE *file = fopen(path, "ab");
    if (!file) return;
    boot_event_write_line(file, format, timestamp_ns, args);
    fclose(file);
}

int nk_shared_boot_event_format_path(char *out, size_t out_capacity,
                                    const char *cache_dir, char separator,
                                    unsigned long process_id, unsigned int sequence) {
    if (!out || out_capacity == 0u) return 0;
    if (!cache_dir) {
        out[0] = '\0';
        return 0;
    }
    int written = snprintf(out, out_capacity, "%s%cplayer-boot-%lu-%u.events",
                           cache_dir, separator, process_id, sequence);
    if (written <= 0 || (size_t)written >= out_capacity) {
        out[0] = '\0';
        return 0;
    }
    return written;
}
