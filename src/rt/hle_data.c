// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Host data: the data-root lookup that serves files extracted from XB archives (#223, #298), and
 * the /DATA/FONT list. Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

#define SREG_OVERLAY_MAX_BYTES (1024u * 1024u)

static int sreg_add_font_int(int cat, const char *name, uint32_t value) {
    int node = sreg_add(cat, name, SREG_TYPE_INT, 4u, SREG_NODE_DEFAULT | SREG_NODE_READONLY);
    if (node >= 0) sreg_put_le32(s_sreg_nodes[node].value, value);
    return node;
}

static int sreg_add_font_str(int cat, const char *name, const char *value) {
    size_t bytes = strlen(value) + 1u; /* a string key's size is its value and the NUL */
    int node = sreg_add(cat, name, SREG_TYPE_STR, (uint32_t)bytes, SREG_NODE_DEFAULT | SREG_NODE_READONLY);
    if (node >= 0) memcpy(s_sreg_nodes[node].value, value, bytes);
    return node;
}

static int sreg_add_font_info(int prop, uint32_t index, const SrFontRegistryInfo *f) {
    char name[SREG_NAME_BYTES];
    int cat;
    snprintf(name, sizeof(name), "INFO%u", index);
    cat = sreg_add(prop, name, SREG_TYPE_DIR, 0u, SREG_NODE_DEFAULT | SREG_NODE_READONLY);
    return cat >= 0 &&
           sreg_add_font_int(cat, "h_size", (uint32_t)f->h_size) >= 0 &&
           sreg_add_font_int(cat, "v_size", (uint32_t)f->v_size) >= 0 &&
           sreg_add_font_int(cat, "h_resolution", (uint32_t)f->h_resolution) >= 0 &&
           sreg_add_font_int(cat, "v_resolution", (uint32_t)f->v_resolution) >= 0 &&
           sreg_add_font_int(cat, "weight", f->weight) >= 0 &&
           sreg_add_font_int(cat, "family_code", f->family_code) >= 0 &&
           sreg_add_font_int(cat, "style", f->style) >= 0 &&
           sreg_add_font_int(cat, "sub_style", f->sub_style) >= 0 &&
           sreg_add_font_int(cat, "language_code", f->language_code) >= 0 &&
           sreg_add_font_int(cat, "region_code", f->region_code) >= 0 &&
           sreg_add_font_int(cat, "country_code", f->country_code) >= 0 &&
           sreg_add_font_str(cat, "font_name", f->font_name) >= 0 &&
           sreg_add_font_str(cat, "file_name", f->file_name) >= 0 &&
           sreg_add_font_int(cat, "extra_attributes", f->extra_attributes) >= 0 &&
           sreg_add_font_int(cat, "expire_date", f->expire_date) >= 0;
}

void sreg_build_font_registry(void) {
    const uint8_t flags = SREG_NODE_DEFAULT | SREG_NODE_READONLY;
    Flash0Sources sources;
    uint32_t err = 0, absent_named = 0;
    int font, prop = -1, ok;
    flash0_font_sources(&sources);
    font = sreg_walk("/DATA/FONT", 1, flags, &err, NULL);
    ok = font >= 0 &&
         sreg_add_font_str(font, "path_name", sr_flash0_font_registry_path_name()) >= 0 &&
         sreg_add_font_int(font, "num_fonts", SR_FONT_REGISTRY_COUNT) >= 0 &&
         (prop = sreg_add(font, "PROPERTY", SREG_TYPE_DIR, 0u, flags)) >= 0;
    for (uint32_t index = 0; ok && index < SR_FONT_REGISTRY_COUNT; index++) {
        SrFontRegistryInfo info;
        SrFontRegistryStatus status = sr_flash0_font_registry_info(&sources, index, &info);
        if (status != SR_FONT_REGISTRY_PRESENT) {
            /* One line per slot: a missing Latin slot would otherwise name sixteen indices. */
            NkFontSlot slot = NK_FONT_SLOT_JAPANESE;
            if (sr_flash0_font_registry_slot(index, &slot) && !(absent_named & (1u << (unsigned)slot))) {
                absent_named |= 1u << (unsigned)slot;
                fprintf(stderr, "sceReg: the font indices of the %s slot (from %u) are absent from "
                                "/DATA/FONT: %s\n", sr_flash0_font_slot_label(slot), index,
                        sr_flash0_font_registry_status_text(status));
            }
            continue;
        }
        if (getenv("SR_FONTLOG"))
            fprintf(stderr, "sceReg: font index %u is served from the %s source as %s%s\n", index,
                    info.source, info.file_name,
                    info.served_as_regular ? " (its measured bold or italic style is served as regular)" : "");
        ok = sreg_add_font_info(prop, index, &info);
    }
    if (!ok) {
        /* A partial list would tell the guest a different story than the served fonts: serve none. */
        for (uint32_t i = 0; i < SREG_MAX_NODES; i++)
            if (s_sreg_nodes[i].id && (s_sreg_nodes[i].flags & SREG_NODE_READONLY)) sreg_free_node((int)i);
        fprintf(stderr, "sceReg: /DATA/FONT could not be built (no free registry nodes); the font "
                        "list is not served\n");
    }
}

/* The overlay file: SR_SYSTEM_REGISTRY when set, else <per-user data dir>/registry/system.json.
 * NULL when neither resolves, or when the variable is set but cannot be read as UTF-8 (falling
 * back to the default file then would silently write somewhere the user did not choose). */
char *sreg_overlay_path(void) {
    char *env = NULL, *path;
    char dir[4096];
    char sep = nk_platform_path_separator();
    int present = 0;
    size_t need;
    if (!sr_utf8_env_alloc(L"SR_SYSTEM_REGISTRY", &env, &present)) return NULL;
    if (present && env && env[0]) return env;
    free(env);
    if (!nk_platform_resolve_app_data_dir(dir, sizeof(dir))) return NULL;
    need = strlen(dir) + sizeof("/registry/system.json");
    path = (char *)malloc(need);
    if (!path) return NULL;
    snprintf(path, need, "%s%cregistry%csystem.json", dir, sep, sep);
    return path;
}

/* Read the overlay file. Returns 1 with a NUL-terminated copy, 0 when there is no file, and -1
 * with *why set when it exists but cannot be read within the size bound. */
int sreg_read_file(const char *path, char **out, size_t *len_out, char *why, size_t why_len) {
    FILE *f;
    char *buf;
    size_t got;
    int failed;
    *out = NULL;
    *len_out = 0;
    if (!nk_platform_file_exists(path)) return 0;
    f = nk_fopen_utf8(path, "rb");
    if (!f) {
        snprintf(why, why_len, "the file cannot be opened");
        return -1;
    }
    buf = (char *)malloc((size_t)SREG_OVERLAY_MAX_BYTES + 1u);
    if (!buf) {
        fclose(f);
        snprintf(why, why_len, "no memory to read it");
        return -1;
    }
    got = fread(buf, 1u, (size_t)SREG_OVERLAY_MAX_BYTES + 1u, f);
    failed = ferror(f);
    fclose(f);
    if (failed || got > SREG_OVERLAY_MAX_BYTES) {
        free(buf);
        snprintf(why, why_len, "%s", failed ? "the file cannot be read" : "the file is larger than 1 MiB");
        return -1;
    }
    buf[got] = '\0';
    *out = buf;
    *len_out = got;
    return 1;
}

int sreg_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint32_t sreg_type_from_name(const char *name) {
    if (strcmp(name, "int") == 0) return SREG_TYPE_INT;
    if (strcmp(name, "str") == 0) return SREG_TYPE_STR;
    if (strcmp(name, "bin") == 0) return SREG_TYPE_BIN;
    return 0;
}

const char *sreg_type_name(uint32_t type) {
    return type == SREG_TYPE_INT ? "int" : type == SREG_TYPE_STR ? "str" : "bin";
}

/* ---- Data-root lookup: serve files extracted from XB archives ------------------------
 *
 * The development workflow extracts packed XB archive members under
 * <SR_DATAROOT>/<archive>.xb[0-9].d/<relative-path>. To serve those files through
 * h_IoOpen / h_IoGetstat we build a sorted relative-path -> host-path cache by
 * walking the explicitly configured data root. Lookup is then O(log N).
 * The cache is published only after the walk, sort, expected-count, and
 * enumerated-size checks succeed; an enumeration or allocation failure destroys
 * the temporary table and permanently fails the data-root route. The actual
 * file open is deferred until the guest requests an entry and fails closed if
 * the indexed host path is no longer readable.
 *
 * SR_DATAROOT is supplied by the native launcher's validated title filesystem
 * binding. Without it, the generic runtime does not infer a title directory.
 */

SrAssetIndex s_data_index;
SrArchiveVfs s_archive_vfs;
char *s_data_root_utf8;
int s_data_archive_mode;
atomic_int s_data_state;

/* Extracted-data route states. The historical numeric values 0-3 are part of
 * the observable behavior of this file; DISABLED (4) is the terminal state for
 * runs whose profile declares no extracted-data census and for which no
 * operator configured SR_DATAROOT.
 *
 * Production order is: sr_host_data_prepare() runs ONCE, synchronously, on the
 * startup thread BEFORE any guest execution exists (see driver.c). Guest-time
 * lookups therefore only ever consume a TERMINAL state; a non-terminal
 * observation after guest start means preparation was skipped or is still
 * running, and the lookup fails closed with ONE bounded diagnostic rather than
 * building the index on the scheduler thread (the historical early-boot stall)
 * or spinning. */
#define SR_DATA_STATE_UNINITIALIZED 0
#define SR_DATA_STATE_INITIALIZING  1
#define SR_DATA_STATE_FAILED        3
#define SR_DATA_STATE_DISABLED      4

/* SR_DATA_EXPECTED_COUNT is now title_config owned; see runtime_bindings.expected_data_file_count */

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only white-box counters for the preparation contract. Production
 * builds compile none of this. */
static unsigned long s_data_test_walk_calls;         /* directories enumerated */
static unsigned long s_data_test_build_attempts;     /* census attempts total */
static unsigned long s_data_test_builds_after_guest; /* attempts after guest start */
/* Enumeration WORK, counted rather than timed. `names` is every directory entry
 * the census retrieved (".", ".." and real children alike); `probes` is the
 * per-file readability open that precedes publication. With the walk count these
 * are the deterministic O(n) evidence for the census: a wall clock measures the
 * host filesystem and the machine's load, so a load-sensitive scaling bound is a
 * flaky correctness gate rather than a scaling proof. */
static unsigned long long s_data_test_scan_names;    /* directory entries read */
static unsigned long long s_data_test_scan_probes;   /* per-file metadata opens */
static int s_data_test_guest_started;
static int s_data_test_pace_ms;                      /* per-directory pacing hook */
#endif
/* Per-phase wall time of the one-time index preparation, reported on one stderr line in
 * every build. The developer route has measured 77-270 s here on a real ~59k-file tree
 * while a synthetic 60k tree takes about 2.5 s, so the phase split is the evidence needed
 * to tell an algorithmic cost from a host filesystem cost. Six tick reads, once per boot. */
enum {
    SR_DATA_TEST_PHASE_ARCHIVE_DISCOVER,
    SR_DATA_TEST_PHASE_PRIMARY_WALK,
    SR_DATA_TEST_PHASE_LOOSE_WALK,
    SR_DATA_TEST_PHASE_FINALIZE,
    SR_DATA_TEST_PHASE_VALIDATE,
    SR_DATA_TEST_PHASE_PUBLISH,
    SR_DATA_TEST_PHASE_COUNT
};
static uint64_t s_data_test_phase_ms[SR_DATA_TEST_PHASE_COUNT];
#define SR_DATA_TEST_PHASE_START(name) ((name) = sr_host_monotonic_ms())
#define SR_DATA_TEST_PHASE_STOP(phase, name) \
    (s_data_test_phase_ms[(phase)] += sr_host_monotonic_ms() - (name))
static void data_report_phases(void) {
    const uint64_t *ms = s_data_test_phase_ms;
    fprintf(stderr, "host_data: phase_ms archive_discover=%llu walk=%llu loose_walk=%llu "
                    "finalize=%llu validate=%llu publish=%llu\n",
            (unsigned long long)ms[SR_DATA_TEST_PHASE_ARCHIVE_DISCOVER],
            (unsigned long long)ms[SR_DATA_TEST_PHASE_PRIMARY_WALK],
            (unsigned long long)ms[SR_DATA_TEST_PHASE_LOOSE_WALK],
            (unsigned long long)ms[SR_DATA_TEST_PHASE_FINALIZE],
            (unsigned long long)ms[SR_DATA_TEST_PHASE_VALIDATE],
            (unsigned long long)ms[SR_DATA_TEST_PHASE_PUBLISH]);
}
/* SR_DATA_EXPECTED_COUNT legacy define removed: use sr_title_config_expected_data_file_count() */

static char *data_rel_join(const char *prefix, const char *name) {
    return sr_utf8_join_alloc(prefix, name, '/');
}

#define SR_DATA_MAX_LOOSE_ROOTS 16u
#define SR_DATA_MAX_LOOSE_ROOT_EXCLUDES 2u
typedef struct {
    wchar_t *host_wide;
    char *host_utf8;
    char *mount;
    uint32_t precedence;
    int skip_primary_root;
    char exclude[SR_DATA_MAX_LOOSE_ROOT_EXCLUDES][241];
    size_t exclude_count;
} SrDataLooseRoot;

static SrDataLooseRoot s_data_loose_roots[SR_DATA_MAX_LOOSE_ROOTS];
static size_t s_data_loose_root_count;

static int data_relative_path_equal(const char *a, const char *b) {
    return a && b && strlen(a) == strlen(b) &&
           sr_vfs_strnicmp(a, b, strlen(a)) == 0;
}

static void data_loose_roots_clear(void) {
    for (size_t i = 0; i < s_data_loose_root_count; i++) {
        free(s_data_loose_roots[i].host_wide);
        free(s_data_loose_roots[i].host_utf8);
        free(s_data_loose_roots[i].mount);
        memset(&s_data_loose_roots[i], 0, sizeof(s_data_loose_roots[i]));
    }
    s_data_loose_root_count = 0;
}

static int data_loose_mount_valid(const char *mount) {
    if (!mount) return 0;
    if (!mount[0]) return 1;
    const char *part = mount;
    for (const char *p = mount;; p++) {
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t)(p - part);
            if (len == 0u || (len == 1u && part[0] == '.') ||
                (len == 2u && part[0] == '.' && part[1] == '.')) return 0;
            for (size_t i = 0; i < len; i++) {
                char c = part[i];
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
            }
            if (!*p) break;
            part = p + 1;
        }
    }
    return 1;
}

static int data_wide_path_overlaps(const wchar_t *a, const wchar_t *b) {
    size_t a_len = wcslen(a), b_len = wcslen(b);
    while (a_len > 1u && (a[a_len - 1u] == L'/' || a[a_len - 1u] == L'\\')) a_len--;
    while (b_len > 1u && (b[b_len - 1u] == L'/' || b[b_len - 1u] == L'\\')) b_len--;
    size_t common = a_len < b_len ? a_len : b_len;
    for (size_t i = 0; i < common; i++) {
        wchar_t ca = a[i], cb = b[i];
        if (ca == L'/') ca = L'\\';
        if (cb == L'/') cb = L'\\';
        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca + (L'a' - L'A'));
        if (cb >= L'A' && cb <= L'Z') cb = (wchar_t)(cb + (L'a' - L'A'));
        if (ca != cb) return 0;
    }
    if (a_len == b_len) return 1;
    return a_len < b_len ? (b[common] == L'/' || b[common] == L'\\')
                         : (a[common] == L'/' || a[common] == L'\\');
}

static int data_wide_path_equal(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return a == b;
    size_t a_len = wcslen(a), b_len = wcslen(b);
    while (a_len > 1u && (a[a_len - 1u] == L'/' || a[a_len - 1u] == L'\\')) a_len--;
    while (b_len > 1u && (b[b_len - 1u] == L'/' || b[b_len - 1u] == L'\\')) b_len--;
    if (a_len != b_len) return 0;
    for (size_t i = 0; i < a_len; i++) {
        wchar_t ca = a[i], cb = b[i];
        if (ca == L'/') ca = L'\\';
        if (cb == L'/') cb = L'\\';
        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca + (L'a' - L'A'));
        if (cb >= L'A' && cb <= L'Z') cb = (wchar_t)(cb + (L'a' - L'A'));
        if (ca != cb) return 0;
    }
    return 1;
}

static int data_parse_precedence(const wchar_t *text, uint32_t *out) {
    if (!text || !text[0] || !out) return 0;
    uint32_t value = 0;
    for (const wchar_t *p = text; *p; p++) {
        if (*p < L'0' || *p > L'9') return 0;
        uint32_t digit = (uint32_t)(*p - L'0');
        if (value > (65535u - digit) / 10u) return 0;
        value = value * 10u + digit;
    }
    *out = value;
    return 1;
}

static int data_parse_boolean_flag(const wchar_t *text, int *out) {
    if (!text || !out) return 0;
    if (wcscmp(text, L"0") == 0) *out = 0;
    else if (wcscmp(text, L"1") == 0) *out = 1;
    else return 0;
    return 1;
}

static int data_loose_root_parse(const wchar_t *primary_root) {
    data_loose_roots_clear();
    wchar_t *serialized = NULL;
    int present = 0;
    if (!sr_wide_env_alloc(L"SR_LOOSE_CONTENT_ROOTS", &serialized, &present)) return 0;
    if (!present || !serialized || !serialized[0]) {
        free(serialized);
        return 1;
    }
    size_t serialized_len = wcslen(serialized);
    if (serialized_len > 32767u || !primary_root) {
        fprintf(stderr, "host_data: loose-content binding (in the works; #308) exceeds its limit\n");
        free(serialized);
        return 0;
    }
    wchar_t *primary_parent = NULL;
    if (!sr_wide_parent_alloc(primary_root, &primary_parent)) {
        free(serialized);
        return 0;
    }
    int ok = 1;
    wchar_t *cursor = serialized;
    wchar_t *serialized_end = serialized + serialized_len;
    while (cursor < serialized_end && ok) {
        wchar_t *line_end = wcschr(cursor, L'\n');
        if (!line_end) line_end = serialized_end;
        wchar_t saved_end = *line_end;
        *line_end = L'\0';
        if (line_end > cursor && line_end[-1] == L'\r') line_end[-1] = L'\0';
        wchar_t *fields[5u + SR_DATA_MAX_LOOSE_ROOT_EXCLUDES] = {0};
        size_t field_count = 1u;
        fields[0] = cursor;
        for (wchar_t *p = cursor; *p; p++) {
            if (*p == L'\t') {
                *p = L'\0';
                if (field_count >= sizeof(fields) / sizeof(fields[0])) {
                    ok = 0;
                    break;
                }
                fields[field_count++] = p + 1;
            }
        }
        if (!ok || !cursor[0] || (field_count != 3u && field_count < 5u)) {
            ok = 0;
        } else {
            uint32_t precedence = 0;
            size_t mount_len = wcslen(fields[1]);
            char mount[241];
            if (mount_len > 240u || !data_parse_precedence(fields[2], &precedence)) {
                ok = 0;
            } else {
                for (size_t i = 0; i < mount_len; i++) {
                    if ((uint32_t)fields[1][i] > 0x7fu) { ok = 0; break; }
                    mount[i] = (char)fields[1][i];
                }
                mount[mount_len] = '\0';
                if (ok && !data_loose_mount_valid(mount)) ok = 0;
            }
            int legacy_row = field_count == 3u;
            int skip_primary = 0;
            char excludes[SR_DATA_MAX_LOOSE_ROOT_EXCLUDES][241] = {{0}};
            size_t exclude_count = 0u;
            if (!legacy_row) {
                uint32_t declared_excludes = 0u;
                if (!data_parse_boolean_flag(fields[3], &skip_primary) ||
                    !data_parse_precedence(fields[4], &declared_excludes) ||
                    declared_excludes > SR_DATA_MAX_LOOSE_ROOT_EXCLUDES ||
                    field_count != 5u + declared_excludes) {
                    ok = 0;
                } else {
                    exclude_count = declared_excludes;
                    for (size_t exclude_index = 0; exclude_index < exclude_count; exclude_index++) {
                        const wchar_t *field = fields[5u + exclude_index];
                        size_t exclude_len = wcslen(field);
                        if (exclude_len == 0u || exclude_len > 240u) { ok = 0; break; }
                        for (size_t char_index = 0; char_index < exclude_len; char_index++) {
                            if ((uint32_t)field[char_index] > 0x7fu) { ok = 0; break; }
                            excludes[exclude_index][char_index] = (char)field[char_index];
                        }
                        excludes[exclude_index][exclude_len] = '\0';
                        if (ok && !data_loose_mount_valid(excludes[exclude_index])) ok = 0;
                        for (size_t previous = 0; previous < exclude_index && ok; previous++) {
                            if (data_relative_path_equal(excludes[previous],
                                                         excludes[exclude_index]))
                                ok = 0;
                        }
                        if (!ok) break;
                    }
                }
            }
            wchar_t *host_wide = NULL;
            char *host_utf8 = NULL;
            if (ok && (!cursor[0] || wcschr(cursor, L'\t') ||
                       !sr_wide_configured_root_wide_alloc(cursor, &host_wide) ||
                       !sr_wide_to_utf8_alloc(host_wide, &host_utf8))) ok = 0;
            if (ok) {
#ifdef _WIN32
                DWORD attributes = GetFileAttributesW(host_wide);
                if (attributes == INVALID_FILE_ATTRIBUTES ||
                    !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) ok = 0;
#else
                struct stat st, link;
                if (lstat(host_utf8, &link) != 0 || S_ISLNK(link.st_mode) ||
                    stat(host_utf8, &st) != 0 || !S_ISDIR(st.st_mode)) ok = 0;
#endif
            }
            if (ok) {
                int same_parent = data_wide_path_equal(host_wide, primary_parent);
                int overlaps_primary = data_wide_path_overlaps(host_wide, primary_root);
                if (legacy_row) skip_primary = same_parent;
                if ((overlaps_primary && !same_parent) ||
                    (skip_primary && !same_parent) || (same_parent && !skip_primary)) ok = 0;
                for (size_t i = 0; i < s_data_loose_root_count && ok; i++) {
                    if (data_wide_path_overlaps(host_wide, s_data_loose_roots[i].host_wide))
                        ok = 0;
                    if (s_data_loose_roots[i].precedence == precedence) ok = 0;
                }
            }
            if (ok && s_data_loose_root_count < SR_DATA_MAX_LOOSE_ROOTS) {
                SrDataLooseRoot *root = &s_data_loose_roots[s_data_loose_root_count++];
                root->host_wide = host_wide; host_wide = NULL;
                root->host_utf8 = host_utf8; host_utf8 = NULL;
                root->mount = sr_asset_index_strdup(mount);
                root->precedence = precedence;
                root->skip_primary_root = skip_primary;
                root->exclude_count = exclude_count;
                memcpy(root->exclude, excludes, sizeof(root->exclude));
                if (!root->mount) ok = 0;
            } else if (ok) {
                ok = 0;
            }
            free(host_wide);
            free(host_utf8);
        }
        if (!ok) {
            fprintf(stderr, "host_data: loose-content binding (in the works; #308) is malformed, overlapping, or unavailable\n");
            break;
        }
        cursor = (saved_end == L'\n') ? line_end + 1 : serialized_end;
    }
    free(primary_parent);
    free(serialized);
    if (!ok) {
        data_loose_roots_clear();
        return 0;
    }
    for (size_t i = 1; i < s_data_loose_root_count; i++) {
        size_t j = i;
        while (j > 0 && s_data_loose_roots[j].precedence <
                            s_data_loose_roots[j - 1].precedence) {
            SrDataLooseRoot swap = s_data_loose_roots[j];
            s_data_loose_roots[j] = s_data_loose_roots[j - 1];
            s_data_loose_roots[j - 1] = swap;
            j--;
        }
    }
    return 1;
}

typedef struct {
    wchar_t *host;
    char *rel;
} DataWalkDir;

static int data_walk_push(DataWalkDir **stack, size_t *count, size_t *capacity,
                          wchar_t *host, char *rel) {
    if (!stack || !count || !capacity || !host || *count == SIZE_MAX) return 0;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity : 16u;
        while (next <= *count) {
            if (next > SIZE_MAX / 2u) return 0;
            next *= 2u;
        }
        if (next > SIZE_MAX / sizeof(**stack)) return 0;
        DataWalkDir *grown = (DataWalkDir *)realloc(*stack, next * sizeof(**stack));
        if (!grown) return 0;
        *stack = grown;
        *capacity = next;
    }
    (*stack)[(*count)++] = (DataWalkDir){host, rel};
    return 1;
}

static int data_loose_path_excluded(const char *relative,
                                   const char exclude_paths[][241],
                                   size_t exclude_count) {
    if (!relative || !exclude_paths) return 0;
    for (size_t i = 0; i < exclude_count; i++) {
        if (data_relative_path_equal(relative, exclude_paths[i])) return 1;
    }
    return 0;
}

/* Iterative depth-first walk of `root`, recording every regular file's relative
 * path and absolute host path.  An explicit heap stack keeps a crafted 32k
 * extended path from consuming the host thread's call stack.
 *
 * `skip_top_name` (optional) names the primary root directly under `root`.
 * `exclude_paths` name additional root-relative paths that are not entered.
 * `mount` affects guest keys only; the walk stack always keeps host-relative
 * paths so exclusions do not depend on the guest namespace prefix. */
#ifdef _WIN32
static int data_walk(const wchar_t *root, const char *mount,
                     const wchar_t *skip_top_name,
                     const char exclude_paths[][241], size_t exclude_count,
                     SrAssetIndex *index, uint32_t precedence) {
    if (!root || !mount || !index || exclude_count > SR_DATA_MAX_LOOSE_ROOT_EXCLUDES ||
        (exclude_count != 0u && !exclude_paths)) return 0;
    DataWalkDir *stack = NULL;
    size_t stack_count = 0, stack_capacity = 0;
    size_t root_len = wcslen(root);
    if (root_len == SIZE_MAX || root_len + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *initial_host = (wchar_t *)malloc((root_len + 1u) * sizeof(*initial_host));
    char *initial_rel = sr_asset_index_strdup("");
    if (!initial_host || !initial_rel) {
        free(initial_host);
        free(initial_rel);
        return 0;
    }
    memcpy(initial_host, root, (root_len + 1u) * sizeof(*initial_host));
    if (!data_walk_push(&stack, &stack_count, &stack_capacity, initial_host, initial_rel)) {
        free(initial_host);
        free(initial_rel);
        free(stack);
        return 0;
    }

    int ok = 1;
    while (ok && stack_count != 0u) {
        DataWalkDir current = stack[--stack_count];
#ifdef SR_HLE_THREAD_SELFTEST
        /* Test-only pacing: proves the census COMPLETES before the guest-start
         * boundary when the hook placement is correct (a placement proof, not a
         * speed proof). Production builds never pace. */
        if (s_data_test_pace_ms > 0) sr_host_sleep_ms(s_data_test_pace_ms);
        s_data_test_walk_calls++;
#endif
        wchar_t *pattern = NULL;
        if (!sr_wide_join_alloc(current.host, L"*", &pattern)) {
            fprintf(stderr, "host_data: failed to construct enumeration pattern\n");
            free(current.host);
            free(current.rel);
            ok = 0;
            break;
        }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        DWORD first_error = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        free(pattern);
        if (h == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "host_data: enumeration failed (error=%lu)\n", first_error);
            free(current.host);
            free(current.rel);
            ok = 0;
            break;
        }

        for (;;) {
#ifdef SR_HLE_THREAD_SELFTEST
            s_data_test_scan_names++;
#endif
            if (!(fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
                                              (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))) {
                char *name = NULL;
                char *child_rel = NULL;
                wchar_t *child_host = NULL;
                char *host_utf8 = NULL;
                char *key = NULL;
                if (!sr_wide_to_utf8_alloc(fd.cFileName, &name) ||
                    !(child_rel = data_rel_join(current.rel, name)) ||
                    !sr_wide_join_alloc(current.host, fd.cFileName, &child_host)) {
                    fprintf(stderr, "host_data: path conversion/join failed during enumeration\n");
                    ok = 0;
                } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (skip_top_name && current.rel[0] == '\0' &&
                        _wcsicmp(fd.cFileName, skip_top_name) == 0) {
                        /* The primary data root is walked separately. */
                    } else if (data_loose_path_excluded(child_rel, exclude_paths,
                                                        exclude_count)) {
                        /* Excluded directories are never pushed onto the walk stack. */
                    } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                        fprintf(stderr, "host_data: refusing reparse-point directory\n");
                        ok = 0;
                    } else if (!data_walk_push(&stack, &stack_count, &stack_capacity,
                                               child_host, child_rel)) {
                        fprintf(stderr, "host_data: directory-stack allocation failed\n");
                        ok = 0;
                    } else {
                        child_host = NULL;
                        child_rel = NULL;
                    }
                } else if (data_loose_path_excluded(child_rel, exclude_paths,
                                                    exclude_count)) {
                    /* An excluded file is not indexed or opened. */
                } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    fprintf(stderr, "host_data: refusing reparse-point file\n");
                    ok = 0;
                } else {
#ifdef SR_HLE_THREAD_SELFTEST
                    s_data_test_scan_probes++;
#endif
                    HANDLE probe = CreateFileW(child_host, GENERIC_READ,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    DWORD probe_error = probe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
                    if (probe == INVALID_HANDLE_VALUE) {
                        fprintf(stderr, "host_data: indexed file readability check failed (entry=%zu error=%lu)\n",
                                index->count, probe_error);
                        ok = 0;
                    } else if (!sr_wide_to_utf8_alloc(child_host, &host_utf8)) {
                        fprintf(stderr, "host_data: host-path conversion failed during enumeration\n");
                        ok = 0;
                    }
                }
                if (ok && host_utf8) {
                    int variant = -1;
                    uint64_t file_size = ((uint64_t)fd.nFileSizeHigh << 32) |
                                         (uint64_t)fd.nFileSizeLow;
                    char *key_relative = mount[0]
                        ? data_rel_join(mount, child_rel)
                        : sr_asset_index_strdup(child_rel);
                    if (file_size > UINT32_MAX) {
                        fprintf(stderr, "host_data: indexed file exceeds guest size limit\n");
                        ok = 0;
                    } else if (!key_relative ||
                               !sr_asset_index_key_from_rel(key_relative, &key, &variant)) {
                        fprintf(stderr, "host_data: index-key conversion failed after %zu files\n",
                                index->count);
                        ok = 0;
                    } else if (!sr_asset_index_add_sized_precedence(
                                   index, key, host_utf8, variant, file_size, precedence)) {
                        fprintf(stderr, "host_data: index allocation failed after %zu files\n",
                                index->count);
                        ok = 0;
                    }
                    free(key_relative);
                }
                free(name);
                free(child_rel);
                free(child_host);
                free(host_utf8);
                free(key);
            }
            if (!ok) break;
            if (!FindNextFileW(h, &fd)) {
                DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_FILES) {
                    fprintf(stderr, "host_data: enumeration terminated with error=%lu\n", error);
                    ok = 0;
                }
                break;
            }
        }
        FindClose(h);
        free(current.host);
        free(current.rel);
    }
    while (stack_count != 0u) {
        DataWalkDir pending = stack[--stack_count];
        free(pending.host);
        free(pending.rel);
    }
    free(stack);
    return ok;
}
#else /* !_WIN32: the same index census over the POSIX directory primitives ---
 * The walk below is the identical algorithm -- iterative heap stack, same
 * relative-key construction, same refusals -- reading each directory with
 * opendir/readdir and classifying an entry with lstat. A symbolic link is this
 * host's redirect, so it is refused exactly where the Windows walk refuses a
 * reparse point, and a regular file is still read-open probed so a race cannot
 * publish an entry the guest cannot later open. */
static int data_walk(const wchar_t *root, const char *mount,
                     const wchar_t *skip_top_name,
                     const char exclude_paths[][241], size_t exclude_count,
                     SrAssetIndex *index, uint32_t precedence) {
    if (!root || !mount || !index || exclude_count > SR_DATA_MAX_LOOSE_ROOT_EXCLUDES ||
        (exclude_count != 0u && !exclude_paths)) return 0;
    DataWalkDir *stack = NULL;
    size_t stack_count = 0, stack_capacity = 0;
    size_t root_len = wcslen(root);
    if (root_len == SIZE_MAX || root_len + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *initial_host = (wchar_t *)malloc((root_len + 1u) * sizeof(*initial_host));
    char *initial_rel = sr_asset_index_strdup("");
    if (!initial_host || !initial_rel) {
        free(initial_host);
        free(initial_rel);
        return 0;
    }
    memcpy(initial_host, root, (root_len + 1u) * sizeof(*initial_host));
    if (!data_walk_push(&stack, &stack_count, &stack_capacity, initial_host, initial_rel)) {
        free(initial_host);
        free(initial_rel);
        free(stack);
        return 0;
    }

    int ok = 1;
    while (ok && stack_count != 0u) {
        DataWalkDir current = stack[--stack_count];
#ifdef SR_HLE_THREAD_SELFTEST
        /* The Windows walk's test-only pacing hook and work counters, at the same points. */
        if (s_data_test_pace_ms > 0) sr_host_sleep_ms(s_data_test_pace_ms);
        s_data_test_walk_calls++;
#endif
        char *host_utf8 = NULL;
        if (!sr_wide_to_utf8_alloc(current.host, &host_utf8)) {
            fprintf(stderr, "host_data: host-path conversion failed during enumeration\n");
            free(current.host);
            free(current.rel);
            ok = 0;
            break;
        }
        DIR *dir = opendir(host_utf8);
        if (!dir) {
            fprintf(stderr, "host_data: enumeration failed (errno=%d)\n", errno);
            free(host_utf8);
            free(current.host);
            free(current.rel);
            ok = 0;
            break;
        }
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(dir);
            if (!entry) {
                if (errno != 0) {
                    fprintf(stderr, "host_data: enumeration terminated with errno=%d\n", errno);
                    ok = 0;
                }
                break;
            }
#ifdef SR_HLE_THREAD_SELFTEST
            s_data_test_scan_names++;
#endif
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            wchar_t *name = NULL;
            wchar_t *child_host = NULL;
            char *child_rel = NULL;
            char *child_utf8 = NULL;
            char *key = NULL;
            if (!sr_utf8_to_wide_alloc(entry->d_name, &name) ||
                !sr_wide_join_alloc(current.host, name, &child_host) ||
                !(child_rel = data_rel_join(current.rel, entry->d_name)) ||
                !sr_wide_to_utf8_alloc(child_host, &child_utf8)) {
                fprintf(stderr, "host_data: path conversion/join failed during enumeration\n");
                ok = 0;
            } else {
                struct stat st;
                /* Classify and probe the entry relative to the open directory, so no path is
                 * re-resolved between the check and the open. */
                if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                    fprintf(stderr, "host_data: entry could not be classified (errno=%d)\n", errno);
                    ok = 0;
                } else if (S_ISLNK(st.st_mode)) {
                    /* This host's reparse point: a link can redirect the walk
                     * outside the root, so it is refused, not followed. */
                    fprintf(stderr, "host_data: refusing symbolic link %s\n", child_utf8);
                    ok = 0;
                } else if (S_ISDIR(st.st_mode)) {
                    if (skip_top_name && current.rel[0] == '\0' &&
                        sr_wide_stricmp(name, skip_top_name) == 0) {
                        /* Already enumerated from its own root; do not pay for
                         * it twice. */
                    } else if (data_loose_path_excluded(child_rel, exclude_paths,
                                                        exclude_count)) {
                        /* Excluded directories are never pushed onto the walk stack. */
                    } else if (!data_walk_push(&stack, &stack_count, &stack_capacity,
                                               child_host, child_rel)) {
                        fprintf(stderr, "host_data: directory-stack allocation failed\n");
                        ok = 0;
                    } else {
                        child_host = NULL;
                        child_rel = NULL;
                    }
                } else if (!S_ISREG(st.st_mode)) {
                    fprintf(stderr, "host_data: refusing non-regular entry %s\n", child_utf8);
                    ok = 0;
                } else if (data_loose_path_excluded(child_rel, exclude_paths,
                                                    exclude_count)) {
                    /* An excluded file is not indexed or opened. */
                } else {
#ifdef SR_HLE_THREAD_SELFTEST
                    s_data_test_scan_probes++;
#endif
                    int probe_fd = openat(dirfd(dir), entry->d_name,
                                          O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                    struct stat probe_st;
                    int probe_ok = probe_fd >= 0 && fstat(probe_fd, &probe_st) == 0 &&
                                   S_ISREG(probe_st.st_mode) &&
                                   probe_st.st_dev == st.st_dev && probe_st.st_ino == st.st_ino;
                    if (probe_fd >= 0) close(probe_fd);
                    if (!probe_ok) {
                        fprintf(stderr, "host_data: indexed file readability check failed "
                                        "(entry=%zu errno=%d)\n", index->count, errno);
                        ok = 0;
                    } else {
                        int variant = -1;
                        char *key_relative = mount[0]
                            ? data_rel_join(mount, child_rel)
                            : sr_asset_index_strdup(child_rel);
                        if ((uint64_t)st.st_size > UINT32_MAX) {
                            fprintf(stderr, "host_data: indexed file exceeds guest size limit\n");
                            ok = 0;
                        } else if (!key_relative ||
                                   !sr_asset_index_key_from_rel(key_relative, &key, &variant)) {
                            fprintf(stderr, "host_data: index-key conversion failed after %zu files\n",
                                    index->count);
                            ok = 0;
                        } else if (!sr_asset_index_add_sized_precedence(
                                       index, key, child_utf8, variant,
                                       (uint64_t)st.st_size, precedence)) {
                            fprintf(stderr, "host_data: index allocation failed after %zu files\n",
                                    index->count);
                            ok = 0;
                        }
                        free(key_relative);
                    }
                }
            }
            free(name);
            free(child_rel);
            free(child_host);
            free(child_utf8);
            free(key);
            if (!ok) break;
        }
        closedir(dir);
        free(host_utf8);
        free(current.host);
        free(current.rel);
    }
    while (stack_count != 0u) {
        DataWalkDir pending = stack[--stack_count];
        free(pending.host);
        free(pending.rel);
    }
    free(stack);
    return ok;
}
#endif /* _WIN32 */

typedef struct {
    char *path;
    int variant;
} ArchiveCandidate;

static int archive_candidate_cmp(const void *left, const void *right) {
    const ArchiveCandidate *a = (const ArchiveCandidate *)left;
    const ArchiveCandidate *b = (const ArchiveCandidate *)right;
    return strcmp(a->path, b->path);
}

static int archive_candidate_push(ArchiveCandidate **items, size_t *count,
                                  size_t *capacity, char *path, int variant) {
    if (!items || !count || !capacity || !path) return 0;
    if (*count == SIZE_MAX) return 0;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity : 8u;
        while (next <= *count) {
            if (next > SIZE_MAX / 2u) return 0;
            next *= 2u;
        }
        if (next > SIZE_MAX / sizeof(**items)) return 0;
        ArchiveCandidate *grown = (ArchiveCandidate *)realloc(
            *items, next * sizeof(**items));
        if (!grown) return 0;
        *items = grown;
        *capacity = next;
    }
    (*items)[*count].path = path;
    (*items)[*count].variant = variant;
    (*count)++;
    return 1;
}

static void archive_candidates_destroy(ArchiveCandidate *items, size_t count) {
    if (!items) return;
    for (size_t i = 0; i < count; i++) free(items[i].path);
    free(items);
}

#ifdef _WIN32
static int archive_data_discover(const wchar_t *root, SrArchiveVfs *vfs,
                                 size_t *archive_count_out) {
    if (!root || !vfs || !archive_count_out) return -1;
    *archive_count_out = 0;
    DataWalkDir *stack = NULL;
    size_t stack_count = 0, stack_capacity = 0;
    size_t root_len = wcslen(root);
    if (root_len == SIZE_MAX || root_len + 1u > SIZE_MAX / sizeof(wchar_t)) return -1;
    wchar_t *initial = (wchar_t *)malloc((root_len + 1u) * sizeof(*initial));
    if (!initial) return -1;
    memcpy(initial, root, (root_len + 1u) * sizeof(*initial));
    if (!data_walk_push(&stack, &stack_count, &stack_capacity, initial, NULL)) {
        free(initial);
        return -1;
    }

    ArchiveCandidate *candidates = NULL;
    size_t candidate_count = 0, candidate_capacity = 0;
    int ok = 1;
    int found = 0;
    while (ok && stack_count != 0u) {
        DataWalkDir current = stack[--stack_count];
#ifdef SR_HLE_THREAD_SELFTEST
        if (s_data_test_pace_ms > 0) sr_host_sleep_ms(s_data_test_pace_ms);
        s_data_test_walk_calls++;
#endif
        wchar_t *pattern = NULL;
        if (!sr_wide_join_alloc(current.host, L"*", &pattern)) {
            ok = 0;
            free(current.host);
            continue;
        }
        WIN32_FIND_DATAW fd;
        HANDLE handle = FindFirstFileW(pattern, &fd);
        DWORD first_error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        free(pattern);
        if (handle == INVALID_HANDLE_VALUE) {
            if (first_error == ERROR_FILE_NOT_FOUND || first_error == ERROR_PATH_NOT_FOUND) {
                free(current.host);
                break;
            }
            ok = 0;
            free(current.host);
            break;
        }
        for (;;) {
#ifdef SR_HLE_THREAD_SELFTEST
            s_data_test_scan_names++;
#endif
            if (!(fd.cFileName[0] == L'.' &&
                  (fd.cFileName[1] == L'\0' ||
                   (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))) {
                wchar_t *child = NULL;
                if (!sr_wide_join_alloc(current.host, fd.cFileName, &child)) {
                    ok = 0;
                } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                        ok = 0;
                    } else if (!data_walk_push(&stack, &stack_count, &stack_capacity,
                                               child, NULL)) {
                        ok = 0;
                    } else {
                        child = NULL;
                    }
                } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    ok = 0;
                } else {
                    char *path = NULL;
                    if (!sr_wide_to_utf8_alloc(child, &path)) {
                        ok = 0;
                    } else {
                        int variant = -1;
                        if (sr_archive_variant_from_name(path, &variant)) {
                            if (!archive_candidate_push(&candidates, &candidate_count,
                                                        &candidate_capacity, path, variant)) {
                                ok = 0;
                            } else {
                                path = NULL;
                                found = 1;
                            }
                        }
                        free(path);
                    }
                }
                free(child);
            }
            if (!ok) break;
            if (!FindNextFileW(handle, &fd)) {
                DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_FILES) ok = 0;
                break;
            }
        }
        FindClose(handle);
        free(current.host);
    }
    while (stack_count != 0u) {
        DataWalkDir pending = stack[--stack_count];
        free(pending.host);
    }
    free(stack);
    if (ok && found && candidate_count != 0u) {
        qsort(candidates, candidate_count, sizeof(*candidates), archive_candidate_cmp);
        for (size_t i = 0; i < candidate_count; i++) {
            NkResult result = sr_archive_vfs_mount_file(
                vfs, candidates[i].path, false, candidates[i].variant, NULL);
            if (result != NK_OK) {
                fprintf(stderr, "host_data: archive mount failed for %s (%d); refusing archive route\n",
                        candidates[i].path, (int)result);
                ok = 0;
                break;
            }
        }
        /* Mounting only appends to the index; sort it once here, at the end of
         * discovery, so the first guest lookup does not pay for (or mutate
         * state for) finalization. Lookups still finalize a late mount. */
        if (ok && !sr_archive_vfs_finalize(vfs)) ok = 0;
    }
    archive_candidates_destroy(candidates, candidate_count);
    if (!ok) return -1;
    *archive_count_out = candidate_count;
    return candidate_count != 0u;
}

static int data_root_validate(const wchar_t *root, int configured) {
    if (!root) return 0;
    DWORD attributes = GetFileAttributesW(root);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        fprintf(stderr, "host_data: root attributes failed (error=%lu)\n", error);
        return 0;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "host_data: data root is not a directory\n");
        return 0;
    }
    /* An explicitly supplied root is an operator-owned path and may be a
     * lawful junction used to stage a long-path fixture.  The executable-
     * anchored default must not silently follow a junction outside the repo. */
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) && !configured) {
        fprintf(stderr, "host_data: executable-relative root is a reparse point\n");
        return 0;
    }
    return 1;
}
#else /* !_WIN32: archive discovery and root validation over POSIX stat ----- */
static int archive_data_discover(const wchar_t *root, SrArchiveVfs *vfs,
                                 size_t *archive_count_out) {
    if (!root || !vfs || !archive_count_out) return -1;
    *archive_count_out = 0;
    DataWalkDir *stack = NULL;
    size_t stack_count = 0, stack_capacity = 0;
    size_t root_len = wcslen(root);
    if (root_len == SIZE_MAX || root_len + 1u > SIZE_MAX / sizeof(wchar_t)) return -1;
    wchar_t *initial = (wchar_t *)malloc((root_len + 1u) * sizeof(*initial));
    char *initial_utf8 = NULL;
    if (!initial || !sr_wide_to_utf8_alloc(root, &initial_utf8)) {
        free(initial);
        return -1;
    }
    memcpy(initial, root, (root_len + 1u) * sizeof(*initial));
    if (!data_walk_push(&stack, &stack_count, &stack_capacity, initial, NULL)) {
        free(initial);
        free(initial_utf8);
        return -1;
    }
    free(initial_utf8);

    ArchiveCandidate *candidates = NULL;
    size_t candidate_count = 0, candidate_capacity = 0;
    int ok = 1;
    int found = 0;
    while (ok && stack_count != 0u) {
        DataWalkDir current = stack[--stack_count];
#ifdef SR_HLE_THREAD_SELFTEST
        if (s_data_test_pace_ms > 0) sr_host_sleep_ms(s_data_test_pace_ms);
        s_data_test_walk_calls++;
#endif
        char *host_utf8 = NULL;
        if (!sr_wide_to_utf8_alloc(current.host, &host_utf8)) {
            ok = 0;
            free(current.host);
            break;
        }
        DIR *dir = opendir(host_utf8);
        if (!dir) {
            if (errno == ENOENT) { free(current.host); break; }
            ok = 0;
            free(host_utf8);
            free(current.host);
            break;
        }
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(dir);
            if (!entry) {
                if (errno != 0) ok = 0;
                break;
            }
#ifdef SR_HLE_THREAD_SELFTEST
            s_data_test_scan_names++;
#endif
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            wchar_t *name = NULL;
            wchar_t *child = NULL;
            char *child_utf8 = NULL;
            if (!sr_utf8_to_wide_alloc(entry->d_name, &name) ||
                !sr_wide_join_alloc(current.host, name, &child) ||
                !sr_wide_to_utf8_alloc(child, &child_utf8)) {
                ok = 0;
            } else {
                struct stat st;
                if (lstat(child_utf8, &st) != 0) {
                    ok = 0;
                } else if (S_ISLNK(st.st_mode)) {
                    ok = 0;
                } else if (S_ISDIR(st.st_mode)) {
                    if (!data_walk_push(&stack, &stack_count, &stack_capacity, child, NULL)) {
                        ok = 0;
                    } else {
                        child = NULL;
                    }
                } else if (!S_ISREG(st.st_mode)) {
                    ok = 0;
                } else {
                    int variant = -1;
                    if (sr_archive_variant_from_name(child_utf8, &variant)) {
                        if (!archive_candidate_push(&candidates, &candidate_count,
                                                    &candidate_capacity, child_utf8, variant)) {
                            ok = 0;
                        } else {
                            child_utf8 = NULL;
                            found = 1;
                        }
                    }
                }
            }
            free(name);
            free(child);
            free(child_utf8);
            if (!ok) break;
        }
        closedir(dir);
        free(host_utf8);
        free(current.host);
    }
    while (stack_count != 0u) {
        DataWalkDir pending = stack[--stack_count];
        free(pending.host);
    }
    free(stack);
    if (ok && found && candidate_count != 0u) {
        qsort(candidates, candidate_count, sizeof(*candidates), archive_candidate_cmp);
        for (size_t i = 0; i < candidate_count; i++) {
            NkResult result = sr_archive_vfs_mount_file(
                vfs, candidates[i].path, false, candidates[i].variant, NULL);
            if (result != NK_OK) {
                fprintf(stderr, "host_data: archive mount failed for %s (%d); refusing archive route\n",
                        candidates[i].path, (int)result);
                ok = 0;
                break;
            }
        }
        if (ok && !sr_archive_vfs_finalize(vfs)) ok = 0;
    }
    archive_candidates_destroy(candidates, candidate_count);
    if (!ok) return -1;
    *archive_count_out = candidate_count;
    return candidate_count != 0u;
}

static int data_root_validate(const wchar_t *root, int configured) {
    if (!root) return 0;
    char *utf8 = NULL;
    if (!sr_wide_to_utf8_alloc(root, &utf8)) return 0;
    struct stat st;
    int present = stat(utf8, &st) == 0;
    if (!present) {
        fprintf(stderr, "host_data: root attributes failed (errno=%d)\n", errno);
        free(utf8);
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "host_data: data root is not a directory\n");
        free(utf8);
        return 0;
    }
    /* An explicitly supplied root is an operator-owned path and may be a
     * lawful link used to stage a long-path fixture. The executable-anchored
     * default must not silently follow a link outside the repo. */
    struct stat link;
    int is_link = lstat(utf8, &link) == 0 && S_ISLNK(link.st_mode);
    free(utf8);
    if (is_link && !configured) {
        fprintf(stderr, "host_data: executable-relative root is a symbolic link\n");
        return 0;
    }
    return 1;
}
#endif /* _WIN32 */

/* Enumeration is the transactional completeness boundary.  WIN32_FIND_DATAW
 * supplies each regular file's checked size, while the walk's read-open probe
 * catches ACL/deletion races before the table is published.  The real open
 * remains fail-closed in h_IoOpen when the guest requests a file. */
/* `primary_count` is the number of files the CONFIGURED root alone yielded, taken
 * before any additional root was folded in.  The declared census describes that
 * root -- the extracted-archive tree the operator prepared and counted -- so the
 * check keeps its exact fail-closed meaning: a truncated or wrong-title archive
 * tree still mismatches and still refuses the index, no matter what sits beside
 * it.  Comparing `index->count` instead would have made the declared number
 * depend on the route's root list, which is not what it declares. */
static int data_validate_index(const SrAssetIndex *index, size_t primary_count) {
    if (!index || index->count == 0) return 0;
{
        uint32_t expected = sr_title_config_expected_data_file_count();
        if (primary_count == 0 || primary_count > index->count) {
            fprintf(stderr, "host_data: primary-root census unavailable; refusing index\n");
            return 0;
        }
        if (expected != 0 && primary_count != (size_t)expected) {
            fprintf(stderr, "host_data: expected %u files but enumerated %zu; refusing index\n",
                    (unsigned)expected, primary_count);
            return 0;
        }
    }
    for (size_t i = 0; i < index->count; i++) {
        if (index->entries[i].size > UINT32_MAX) {
            fprintf(stderr, "host_data: indexed file metadata exceeds guest size limit (entry=%zu)\n", i);
            return 0;
        }
    }
    return 1;
}

static int data_validate_archive_index(size_t primary_count) {
    if (primary_count == 0u) {
        fprintf(stderr, "host_data: archive index is empty; refusing archive route\n");
        return 0;
    }
    uint32_t expected = sr_title_config_expected_data_file_count();
    if (expected != 0u && primary_count != (size_t)expected) {
        fprintf(stderr, "host_data: expected %u archive members but indexed %zu; refusing index\n",
                (unsigned)expected, primary_count);
        return 0;
    }
    return 1;
}

static const char *data_index_root_for_precedence(uint32_t precedence,
                                                  const char *primary_root_utf8) {
    if (precedence == 0u) return primary_root_utf8;
    for (size_t i = 0; i < s_data_loose_root_count; i++) {
        if (s_data_loose_roots[i].precedence + 1u == precedence)
            return s_data_loose_roots[i].host_utf8;
    }
    return NULL;
}

static const char *data_index_relative_path(const SrAssetIndexEntry *entry,
                                           const char *primary_root_utf8) {
    const char *base = entry
        ? data_index_root_for_precedence(entry->precedence, primary_root_utf8)
        : NULL;
    const char *relative = NULL;
    if (entry && entry->host && base) {
        size_t base_len = strlen(base);
        size_t host_len = strlen(entry->host);
        while (base_len > 1u && (base[base_len - 1u] == '/' || base[base_len - 1u] == '\\'))
            base_len--;
        if (host_len >= base_len && sr_vfs_strnicmp(entry->host, base, base_len) == 0 &&
            (entry->host[base_len] == '\0' || entry->host[base_len] == '/' ||
             entry->host[base_len] == '\\')) {
            relative = entry->host + base_len;
            while (*relative == '/' || *relative == '\\') relative++;
        }
    }
    return relative;
}

static void data_duplicate_relative_path(const SrAssetIndexEntry *entry,
                                         const char *primary_root_utf8,
                                         char output[161]) {
    const char *relative = data_index_relative_path(entry, primary_root_utf8);
    if (!relative) relative = "<root-relative-unavailable>";
    size_t length = strlen(relative);
    if (length < 161u) {
        for (size_t i = 0; i < length; i++)
            output[i] = relative[i] == '\\' ? '/' : relative[i];
        output[length] = '\0';
        return;
    }
    for (size_t i = 0; i < 157u; i++)
        output[i] = relative[i] == '\\' ? '/' : relative[i];
    memcpy(output + 157u, "...", 4u);
}

/* Return the separator after the first archive-extraction marker recognized by
 * sr_asset_index_key_from_rel(). The preceding root-relative prefix identifies
 * one extracted archive; bytewise path comparison matches the finalized index. */
static const char *data_extracted_archive_relative_end(const char *relative) {
    if (!relative) return NULL;
    for (const char *scan = relative;;) {
        const char *xb = sr_asset_index_find_ci(scan, ".xb");
        if (!xb) return NULL;
        const char *suffix = xb + 3;
        uint64_t parsed_variant = 0;
        int variant_overflow = 0;
        while (*suffix >= '0' && *suffix <= '9') {
            uint64_t digit = (uint64_t)(*suffix - '0');
            if (parsed_variant > (UINT64_MAX - digit) / 10u)
                variant_overflow = 1;
            else
                parsed_variant = parsed_variant * 10u + digit;
            suffix++;
        }
        if (!variant_overflow && parsed_variant <= (uint64_t)INT_MAX &&
            suffix[0] == '.' &&
            (suffix[1] == 'd' || suffix[1] == 'D') &&
            (suffix[2] == '/' || suffix[2] == '\\')) {
            return suffix + 2;
        }
        scan = xb + 3;
    }
}

static int data_extracted_archive_prefix_equal(const char *left,
                                               const char *left_end,
                                               const char *right,
                                               const char *right_end) {
    size_t left_length = (size_t)(left_end - left);
    size_t right_length = (size_t)(right_end - right);
    if (left_length != right_length) return 0;
    for (size_t i = 0; i < left_length; i++) {
        char left_char = left[i] == '\\' ? '/' : left[i];
        char right_char = right[i] == '\\' ? '/' : right[i];
        if (left_char != right_char) return 0;
    }
    return 1;
}

static int data_primary_duplicate_is_cross_archive(
    const SrAssetIndexEntry *previous, const SrAssetIndexEntry *entry,
    const char *primary_root_utf8) {
    /* The finalized index orders equal key/variant/root entries by host path.
     * A shared extracted-archive prefix is therefore contiguous in that run. */
    if (!previous || !entry || previous->precedence != 0u ||
        entry->precedence != 0u) return 0;
    const char *previous_relative = data_index_relative_path(previous, primary_root_utf8);
    const char *entry_relative = data_index_relative_path(entry, primary_root_utf8);
    const char *previous_end = data_extracted_archive_relative_end(previous_relative);
    const char *entry_end = data_extracted_archive_relative_end(entry_relative);
    return previous_end && entry_end &&
           !data_extracted_archive_prefix_equal(previous_relative, previous_end,
                                                entry_relative, entry_end);
}

/* Exact guest-file collisions across manifest roots are intentional overlays:
 * the lower precedence value wins. A collision within one root is ambiguous
 * after guest-key case folding and refuses the whole index. The primary root is
 * the compatibility exception: different extracted-XB subtrees may expose the
 * same key, and the index's host-path sort chooses the stable legacy winner. */
static int data_validate_duplicate_policy(const SrAssetIndex *index,
                                          const wchar_t *primary_root_wide) {
    if (!index || !index->finalized || !primary_root_wide) return 0;
    char *primary_root_utf8 = NULL;
    if (!sr_wide_to_utf8_alloc(primary_root_wide, &primary_root_utf8)) return 0;
    int ok = 1;
    for (size_t i = 0; i < index->count; i++) {
        const SrAssetIndexEntry *entry = &index->entries[i];
        if (i > 0u) {
            const SrAssetIndexEntry *previous = &index->entries[i - 1u];
            if (strcmp(previous->key, entry->key) == 0 &&
                previous->variant == entry->variant &&
                previous->precedence == entry->precedence &&
                !data_primary_duplicate_is_cross_archive(previous, entry,
                                                         primary_root_utf8)) {
                char previous_path[161], current_path[161];
                data_duplicate_relative_path(previous, primary_root_utf8, previous_path);
                data_duplicate_relative_path(entry, primary_root_utf8, current_path);
                fprintf(stderr,
                        "host_data: duplicate guest-file key '%.*s' within root %u at '%s' and '%s'; refusing index\n",
                        120, entry->key, (unsigned)entry->precedence,
                        previous_path, current_path);
                ok = 0;
                break;
            }
        }
        size_t key_len = strlen(entry->key);
        size_t first = sr_asset_index_lower_bound(index, entry->key);
        size_t after = first;
        while (after < index->count &&
               strcmp(index->entries[after].key, entry->key) == 0) after++;
        if (after < index->count &&
            strncmp(index->entries[after].key, entry->key, key_len) == 0 &&
            index->entries[after].key[key_len] == '/') {
            fprintf(stderr, "host_data: guest file/directory key collision; refusing index\n");
            ok = 0;
            break;
        }
    }
    free(primary_root_utf8);
    return ok;
}

/* Enumerate only roots carried by the validated title filesystem binding.
 * The manifest default is an empty set; generic HLE performs no directory
 * discovery. Lower precedence numbers win duplicate guest-file keys. */
static int data_loose_content_walk(const wchar_t *primary_root, SrAssetIndex *index) {
    for (size_t i = 0; i < s_data_loose_root_count; i++) {
        SrDataLooseRoot *root = &s_data_loose_roots[i];
        wchar_t *primary_name = NULL;
        const wchar_t *skip = NULL;
        if (root->skip_primary_root) {
            if (!sr_wide_basename_alloc(primary_root, &primary_name)) return 0;
            skip = primary_name;
        }
        /* The manifest rank orders loose roots after the primary data root;
         * primary is 0 and declared ranks occupy 1..65536. */
        int ok = data_walk(root->host_wide, root->mount, skip,
                           root->exclude, root->exclude_count, index,
                           root->precedence + 1u);
        free(primary_name);
        if (!ok) {
            fprintf(stderr, "host_data: configured loose-content root failed; refusing partial index\n");
            return 0;
        }
    }
    return 1;
}

char *data_loose_path_alloc(const char *root_utf8, const char *key) {
    if (!root_utf8 || !key) return NULL;
    if (!key[0]) return sr_asset_index_strdup(root_utf8);
    size_t root_len = strlen(root_utf8);
    size_t key_len = strlen(key);
    if (root_len > SIZE_MAX - key_len - 2u) return NULL;
    size_t capacity = root_len + key_len + 2u;
    char *path = (char *)malloc(capacity);
    if (!path) return NULL;
    if (sr_vfs_host_dir_path(root_utf8, key, path, capacity, SR_HOST_PATH_SEP) <= 0) {
        free(path);
        return NULL;
    }
    return path;
}

#ifdef _WIN32
static int data_loose_file_open_at(const char *root_utf8, const char *key,
                                   FILE **file_out, uint32_t *size_out) {
    if (file_out) *file_out = NULL;
    if (size_out) *size_out = 0;
    if (!root_utf8 || !key || !key[0]) return 0;
    char *path = data_loose_path_alloc(root_utf8, key);
    if (!path) return -1;
    wchar_t *wide = NULL;
    if (!sr_wide_path_alloc(path, &wide)) {
        free(path);
        return -1;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &attributes)) {
        free(wide);
        free(path);
        return 0;
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(wide);
        free(path);
        return -1;
    }
    FILE *file = sr_fopen_utf8(path, L"rb");
    free(wide);
    free(path);
    if (!file) return -1;
    uint32_t size = 0;
    if (!sr_stream_size_u32(file, &size)) {
        fclose(file);
        return -1;
    }
    if (file_out) *file_out = file;
    else fclose(file);
    if (size_out) *size_out = size;
    return 1;
}
#else /* !_WIN32: the same open, refusing directories and symlinks ------------- */
static int data_loose_file_open_at(const char *root_utf8, const char *key,
                                   FILE **file_out, uint32_t *size_out) {
    if (file_out) *file_out = NULL;
    if (size_out) *size_out = 0;
    if (!root_utf8 || !key || !key[0]) return 0;
    char *path = data_loose_path_alloc(root_utf8, key);
    if (!path) return -1;
    /* Open first and judge the opened file: O_NOFOLLOW refuses a symlink (ELOOP), fstat
     * refuses a directory or special file, so no check can be raced by a path swap. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int open_errno = errno;
    free(path);
    if (fd < 0) return (open_errno == ENOENT || open_errno == ENOTDIR) ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        return -1;
    }
    if (!sr_stream_size_u32(file, size_out)) {
        fclose(file);
        if (size_out) *size_out = 0;
        return -1;
    }
    if (file_out) *file_out = file;
    else fclose(file);
    return 1;
}
#endif /* _WIN32 */

static const char *data_loose_key_for_mount(const char *key, const char *mount) {
    if (!key || !mount) return NULL;
    if (!mount[0]) return key;
    size_t mount_len = strlen(mount);
    if (sr_vfs_strnicmp(key, mount, mount_len) != 0) return NULL;
    if (key[mount_len] == '\0') return "";
    return key[mount_len] == '/' ? key + mount_len + 1u : NULL;
}

int data_loose_file_open(const char *key, FILE **file_out,
                                uint32_t *size_out) {
    if (file_out) *file_out = NULL;
    if (size_out) *size_out = 0;
    if (!key || !key[0]) return 0;
    int primary_result = data_loose_file_open_at(s_data_root_utf8, key,
                                                  file_out, size_out);
    if (primary_result != 0) return primary_result;
    for (size_t i = 0; i < s_data_loose_root_count; i++) {
        const char *relative = data_loose_key_for_mount(key,
                                                        s_data_loose_roots[i].mount);
        if (!relative || !relative[0]) continue;
        int result = data_loose_file_open_at(s_data_loose_roots[i].host_utf8,
                                             relative, file_out, size_out);
        if (result != 0) return result;
    }
    return 0;
}

/* Build the extracted-data index ONCE, before guest execution starts.
 *
 * Applicability predicate: the route applies iff an operator explicitly
 * configured SR_DATAROOT, OR the build profile declares an expected census
 * (sr_title_config_expected_data_file_count() > 0, set only when a title manifest configures expected_data_file_count). A generic profile
 * without SR_DATAROOT terminates DISABLED with ZERO filesystem enumeration --
 * a tree staged for one title can no longer be enumerated on behalf of another
 * build or a synthetic guest. Census internals (walk, normalization, sort,
 * publication checks, diagnostics) are unchanged from the historical lazy
 * implementation; only WHEN it runs and WHO may trigger it changed. */
int sr_host_data_prepare(void) {
#ifdef SR_HLE_THREAD_SELFTEST
    s_data_test_build_attempts++;
    if (s_data_test_guest_started &&
        atomic_load_explicit(&s_data_state, memory_order_acquire) != SR_DATA_STATE_READY)
        s_data_test_builds_after_guest++;
#endif
    int expected = SR_DATA_STATE_UNINITIALIZED;
    if (!atomic_compare_exchange_strong_explicit(&s_data_state, &expected,
                                                 SR_DATA_STATE_INITIALIZING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        /* Idempotent for repeat calls (selftests, future callers): a route
         * already terminal or being prepared is never re-scanned here. */
        return atomic_load_explicit(&s_data_state, memory_order_acquire);
    }
    data_loose_roots_clear();
    wchar_t *configured_root = NULL;
    int configured_present = 0;
    int env_ok = sr_wide_env_alloc(L"SR_DATAROOT", &configured_root, &configured_present);
    if (!configured_present && sr_title_config_expected_data_file_count() == 0) {
        free(configured_root);
        data_loose_roots_clear();
        atomic_store_explicit(&s_data_state, SR_DATA_STATE_DISABLED, memory_order_release);
        fprintf(stderr, "host_data: disabled (no SR_DATAROOT configured and this profile "
                        "declares no extracted-data census); guest lookups will fall back "
                        "to disc/VFS\n");
        return SR_DATA_STATE_DISABLED;
    }
    SrAssetIndex temporary;
    sr_asset_index_init(&temporary);
    sr_archive_vfs_destroy(&s_archive_vfs);
    sr_archive_vfs_init(&s_archive_vfs);
    free(s_data_root_utf8);
    s_data_root_utf8 = NULL;
    s_data_archive_mode = 0;
    const char *root_label = "<configured SR_DATAROOT>";
    fprintf(stderr, "host_data: scanning %s ...\n", root_label);
    wchar_t *root_wide = NULL;
    int root_ok = env_ok && configured_present && configured_root && configured_root[0] &&
                  sr_wide_configured_root_wide_alloc(configured_root, &root_wide);
    if (!root_ok) {
        if (!env_ok)
            fprintf(stderr, "host_data: SR_DATAROOT could not be read\n");
        else if (configured_present)
            fprintf(stderr, "host_data: SR_DATAROOT is configured but is not a valid absolute path\n");
        else
            fprintf(stderr, "host_data: extracted-data census requires a configured SR_DATAROOT\n");
    }
    free(configured_root);
    if (root_ok && !data_root_validate(root_wide, 1)) root_ok = 0;
    if (root_ok && !data_loose_root_parse(root_wide)) root_ok = 0;
    size_t primary_count = 0;
    size_t archive_count = 0;
    int archive_mode = 0;
    uint64_t phase_started = 0;
    if (root_ok) {
        SR_DATA_TEST_PHASE_START(phase_started);
        archive_mode = archive_data_discover(root_wide, &s_archive_vfs, &archive_count);
        SR_DATA_TEST_PHASE_STOP(SR_DATA_TEST_PHASE_ARCHIVE_DISCOVER, phase_started);
        if (archive_mode < 0) root_ok = 0;
    }
    int walk_ok = root_ok;
    if (walk_ok && archive_mode > 0) {
        s_data_archive_mode = 1;
        if (!sr_wide_to_utf8_alloc(root_wide, &s_data_root_utf8)) walk_ok = 0;
        primary_count = sr_archive_vfs_entry_count(&s_archive_vfs);
        SR_DATA_TEST_PHASE_START(phase_started);
        if (!data_validate_archive_index(primary_count)) walk_ok = 0;
        SR_DATA_TEST_PHASE_STOP(SR_DATA_TEST_PHASE_VALIDATE, phase_started);
        if (walk_ok) {
            SR_DATA_TEST_PHASE_START(phase_started);
            walk_ok = data_loose_content_walk(root_wide, &temporary);
            SR_DATA_TEST_PHASE_STOP(SR_DATA_TEST_PHASE_LOOSE_WALK, phase_started);
            SR_DATA_TEST_PHASE_START(phase_started);
            if (walk_ok && temporary.count != 0u &&
                !sr_asset_index_finalize(&temporary)) walk_ok = 0;
            if (walk_ok && temporary.count != 0u &&
                !data_validate_duplicate_policy(&temporary, root_wide)) walk_ok = 0;
            SR_DATA_TEST_PHASE_STOP(SR_DATA_TEST_PHASE_FINALIZE, phase_started);
        }
    } else if (walk_ok) {
        sr_archive_vfs_destroy(&s_archive_vfs);
        sr_archive_vfs_init(&s_archive_vfs);
        SR_DATA_TEST_PHASE_START(phase_started);
        walk_ok = data_walk(root_wide, "", NULL, NULL, 0u, &temporary, 0u);
        SR_DATA_TEST_PHASE_STOP(SR_DATA_TEST_PHASE_PRIMARY_WALK, phase_started);
        if (walk_ok) {
            primary_count = temporary.count;
            SR_DATA_TEST_PHASE_START(phase_started);
            walk_ok = data_loose_content_walk(root_wide, &temporary);
            SR_DATA_TEST_PHASE_STOP(SR_DATA_TEST_PHASE_LOOSE_WALK, phase_started);
        }
        SR_DATA_TEST_PHASE_START(phase_started);
        if (walk_ok && !sr_asset_index_finalize(&temporary)) walk_ok = 0;
        if (walk_ok && !data_validate_duplicate_policy(&temporary, root_wide)) walk_ok = 0;
        SR_DATA_TEST_PHASE_STOP(SR_DATA_TEST_PHASE_FINALIZE, phase_started);
        SR_DATA_TEST_PHASE_START(phase_started);
        if (walk_ok && !data_validate_index(&temporary, primary_count)) walk_ok = 0;
        SR_DATA_TEST_PHASE_STOP(SR_DATA_TEST_PHASE_VALIDATE, phase_started);
    }
    if (!walk_ok) {
        fprintf(stderr, "host_data: index initialization failed; refusing partial index\n");
        free(root_wide);
        sr_asset_index_destroy(&temporary);
        sr_archive_vfs_destroy(&s_archive_vfs);
        free(s_data_root_utf8);
        s_data_root_utf8 = NULL;
        data_loose_roots_clear();
        s_data_archive_mode = 0;
        atomic_store_explicit(&s_data_state, SR_DATA_STATE_FAILED, memory_order_release);
        return SR_DATA_STATE_FAILED;
    }
    free(root_wide);
    if (archive_mode > 0) {
        int loose_index_present = temporary.count != 0u;
        SR_DATA_TEST_PHASE_START(phase_started);
        if (loose_index_present &&
            !sr_asset_index_publish(&s_data_index, &temporary)) {
            fprintf(stderr, "host_data: failed to publish loose-content index\n");
            sr_asset_index_destroy(&temporary);
            sr_asset_index_destroy(&s_data_index);
            sr_asset_index_init(&s_data_index);
            sr_archive_vfs_destroy(&s_archive_vfs);
            free(s_data_root_utf8);
            s_data_root_utf8 = NULL;
            data_loose_roots_clear();
            s_data_archive_mode = 0;
            atomic_store_explicit(&s_data_state, SR_DATA_STATE_FAILED, memory_order_release);
            return SR_DATA_STATE_FAILED;
        }
        SR_DATA_TEST_PHASE_STOP(SR_DATA_TEST_PHASE_PUBLISH, phase_started);
        if (!loose_index_present) {
            sr_asset_index_destroy(&s_data_index);
            sr_asset_index_init(&s_data_index);
        }
        sr_asset_index_destroy(&temporary);
        atomic_store_explicit(&s_data_state, SR_DATA_STATE_READY, memory_order_release);
        fprintf(stderr, "host_data: mounted %zu archives with %zu members under %s\n",
                archive_count, primary_count, root_label);
        data_report_phases();
        return SR_DATA_STATE_READY;
    }
    SR_DATA_TEST_PHASE_START(phase_started);
    if (!sr_asset_index_publish(&s_data_index, &temporary)) {
        fprintf(stderr, "host_data: failed to publish finalized index\n");
        sr_asset_index_destroy(&temporary);
        sr_archive_vfs_destroy(&s_archive_vfs);
        data_loose_roots_clear();
        atomic_store_explicit(&s_data_state, SR_DATA_STATE_FAILED, memory_order_release);
        return SR_DATA_STATE_FAILED;
    }
    SR_DATA_TEST_PHASE_STOP(SR_DATA_TEST_PHASE_PUBLISH, phase_started);
    atomic_store_explicit(&s_data_state, SR_DATA_STATE_READY, memory_order_release);
    fprintf(stderr, "host_data: indexed %zu files under %s\n", s_data_index.count, root_label);
    if (s_data_index.count != primary_count) {
        fprintf(stderr, "host_data: configured root contributed %zu of them; %zu came from "
                        "configured loose roots\n",
                primary_count, s_data_index.count - primary_count);
    }
    data_report_phases();
    return SR_DATA_STATE_READY;
}

/* Entry count of the published index; only meaningful after READY. Used by the
 * driver's BOOT_EVENT index_prepare_end record so a boot log states how much
 * was prepared without exposing internal structures. */
size_t sr_host_data_entry_count(void) {
    if (atomic_load_explicit(&s_data_state, memory_order_acquire) != SR_DATA_STATE_READY)
        return 0u;
    return s_data_archive_mode ? sr_archive_vfs_entry_count(&s_archive_vfs)
                               : s_data_index.count;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only white-box accessors for the preparation contract. Production
 * builds compile none of this. */
void sr_hle_test_data_mark_guest_start(void) { s_data_test_guest_started = 1; }
unsigned long sr_hle_test_data_walk_calls(void) { return s_data_test_walk_calls; }
unsigned long sr_hle_test_data_build_attempts(void) { return s_data_test_build_attempts; }
unsigned long sr_hle_test_data_builds_after_guest(void) { return s_data_test_builds_after_guest; }
unsigned long long sr_hle_test_data_phase_ms(unsigned int phase) {
    return phase < SR_DATA_TEST_PHASE_COUNT ?
        (unsigned long long)s_data_test_phase_ms[phase] : 0u;
}
/* Enumeration work counters: how many directory entries the census read and how
 * many per-file metadata opens it made. Deterministic, so a scaling assertion
 * can be proved by them instead of by the wall clock. */
unsigned long long sr_hle_test_data_scan_names(void) { return s_data_test_scan_names; }
unsigned long long sr_hle_test_data_scan_probes(void) { return s_data_test_scan_probes; }
int sr_hle_test_data_state(void) {
    return atomic_load_explicit(&s_data_state, memory_order_acquire);
}
size_t sr_hle_test_data_entry_count(void) {
    return s_data_archive_mode ? sr_archive_vfs_entry_count(&s_archive_vfs)
                               : s_data_index.count;
}

/* Test-build-only: does any published entry's key begin with `key_prefix`?  Lets a
 * test prove that a route addition did NOT re-index a root it was told to skip
 * (a count alone cannot tell a skipped root from a de-duplicated one). */
int sr_hle_test_data_key_seen(const char *key_prefix) {
    if (!key_prefix ||
        atomic_load_explicit(&s_data_state, memory_order_acquire) != SR_DATA_STATE_READY) {
        return 0;
    }
    size_t prefix_len = strlen(key_prefix);
    for (size_t i = 0; i < s_data_index.count; i++) {
        if (strncmp(s_data_index.entries[i].key, key_prefix, prefix_len) == 0) return 1;
    }
    return 0;
}

void sr_hle_test_data_reset(int pace_ms) {
    sr_asset_index_destroy(&s_data_index);
    sr_asset_index_init(&s_data_index);
    sr_archive_vfs_destroy(&s_archive_vfs);
    sr_archive_vfs_init(&s_archive_vfs);
    free(s_data_root_utf8);
    s_data_root_utf8 = NULL;
    data_loose_roots_clear();
    s_data_archive_mode = 0;
    atomic_store_explicit(&s_data_state, SR_DATA_STATE_UNINITIALIZED, memory_order_release);
    s_data_test_walk_calls = 0;
    s_data_test_build_attempts = 0;
    s_data_test_builds_after_guest = 0;
    s_data_test_scan_names = 0;
    s_data_test_scan_probes = 0;
    s_data_test_guest_started = 0;
    s_data_test_pace_ms = pace_ms;
    memset(s_data_test_phase_ms, 0, sizeof(s_data_test_phase_ms));
}
#endif

char *data_normalize_guest_key(const char *guest_path, int *wanted_variant) {
    if (!guest_path || !wanted_variant) return NULL;
    /* Strip device prefix the same way iso_lookup does. */
    const char *p = guest_path;
    const char *c = strchr(p, ':');
    if (c) p = c + 1;
    while (*p == '/' || *p == '\\') p++;
    /* Cache keys are relative to an archive output directory (e.g. "data/menu/text/x.to"), but
     * the game may request the same file by its full "PSP_GAME/USRDIR/..." path. Strip
     * those leading segments so the binary search matches regardless of which form the
     * guest passes. */
    /* Each test must strip the separator along with the name, which means the
     * count has to include it: "PSP_GAME/" is nine characters, "USRDIR/" is seven.
     * The PSP_GAME test compared and advanced eight -- the name without its
     * separator -- so `p` became "/USRDIR/<rel>", the USRDIR test could no longer
     * match, and the key came out as "/usrdir/<rel>".  Every
     * `disc0:/PSP_GAME/USRDIR/<rel>` lookup therefore missed the index and fell
     * through to the ISO route. This was an implementation off-by-one, not a
     * property of the PSP path grammar. */
    if (sr_vfs_strnicmp(p, "PSP_GAME/", 9) == 0 || sr_vfs_strnicmp(p, "PSP_GAME\\", 9) == 0) p += 9;
    if (sr_vfs_strnicmp(p, "USRDIR/", 7) == 0 || sr_vfs_strnicmp(p, "USRDIR\\", 7) == 0) p += 7;
    /* Localized roots are selected by the game itself.  For this build the table is
     * data_00_USE, data_02_FRE, data_03_SPA, ... and the matching archives are .xb0,
     * .xb2, .xb3, ... with internal paths rooted at plain data/. */
    *wanted_variant = -2;          /* -2 = unqualified path */
    const char *localized_tail = NULL;
    size_t guest_key_length = strlen(p);
    if (guest_key_length >= 8u && sr_vfs_strnicmp(p, "data_", 5) == 0 &&
        p[5] >= '0' && p[5] <= '9' && p[6] >= '0' && p[6] <= '9' &&
        p[7] == '_') {
        const char *slash = strpbrk(p + 8, "/\\");
        if (slash) {
            *wanted_variant = (p[5] - '0') * 10 + (p[6] - '0');
            localized_tail = slash;
        }
    }
    /* Lowercase fold. guest_path is NUL-terminated by guest_cstr, so take its
     * length up front and only read defined bytes (avoids reading uninitialized
     * tail bytes / garbage past the terminator). */
    char *localized = NULL;
    if (localized_tail) {
        size_t tail_len = strlen(localized_tail);
        if (tail_len > SIZE_MAX - 5u) return NULL;
        localized = (char *)malloc(tail_len + 5u);
        if (!localized) return NULL;
        memcpy(localized, "data", 4u);
        memcpy(localized + 4u, localized_tail, tail_len + 1u);
        p = localized;
    }
    size_t length = strlen(p);
    char *key = (char *)malloc(length + 1u);
    if (!key) { free(localized); return NULL; }
    for (size_t n = 0; n < length; n++) {
        char ch = p[n];
        if (ch == '\\') ch = '/';
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        key[n] = ch;
    }
    key[length] = 0;
    free(localized);
    return key;
}

/* Case-fold the guest key and binary-search the complete cache.
 *
 * Guest-time consumption contract: only a TERMINAL route state is consumed.
 * A non-terminal observation means preparation never ran (or has not finished)
 * before guest execution reached a lookup -- the exact early-boot stall this
 * seam exists to prevent. The lookup therefore refuses with ONE bounded
 * diagnostic and never begins, resumes, or waits on a census here. */
const SrAssetIndexEntry *host_data_lookup(const char *guest_path) {
    int state = atomic_load_explicit(&s_data_state, memory_order_acquire);
    if (state != SR_DATA_STATE_READY) {
        static atomic_int s_data_lookup_warned;
        if (!atomic_exchange_explicit(&s_data_lookup_warned, 1, memory_order_acq_rel)) {
            fprintf(stderr, "host_data: lookup refused before preparation reached a "
                            "terminal state (state=%d); disc/VFS fallback stays active\n",
                    state);
        }
        return NULL;
    }
    int wanted_variant = -2;
    char *key = data_normalize_guest_key(guest_path, &wanted_variant);
    if (!key) return NULL;
    size_t first = sr_asset_index_lower_bound(&s_data_index, key);
    const SrAssetIndexEntry *chosen = NULL;
    for (size_t i = first; i < s_data_index.count &&
                           strcmp(s_data_index.entries[i].key, key) == 0; i++) {
        const SrAssetIndexEntry *candidate = &s_data_index.entries[i];
        /* Same selection rule enumeration uses, so a name sceIoDread reported
         * is a name sceIoOpen resolves under the identical variant. */
        if (!sr_asset_index_variant_selected(candidate->variant, wanted_variant)) continue;
        if (wanted_variant >= 0) { chosen = candidate; break; }
        if (!chosen ||
            (candidate->variant == -1 && chosen->variant != -1) ||
            (candidate->variant == chosen->variant &&
             candidate->precedence < chosen->precedence) ||
            (chosen->variant != -1 && candidate->variant < chosen->variant)) {
            chosen = candidate;
        }
    }
    free(key);
    return chosen;
}

int io_path_equals_ci(const char *path, const char *expected) {
    size_t length = strlen(expected);
    return path && strlen(path) == length && sr_vfs_strnicmp(path, expected, length) == 0;
}

int io_path_is_disc_device(const char *path) {
    return path && (sr_vfs_strnicmp(path, "disc0:", 6) == 0 ||
                    sr_vfs_strnicmp(path, "umd:", 4) == 0);
}

IoRawDevicePath io_raw_device_path(const char *path) {
    if (io_path_equals_ci(path, "umd0:") || io_path_equals_ci(path, "umd0:/"))
        return IO_RAW_DEVICE_UMD0;
    if (io_path_equals_ci(path, "umd1:") || io_path_equals_ci(path, "umd1:/"))
        return IO_RAW_DEVICE_UMD1;
    if (io_path_equals_ci(path, "umd:"))
        return IO_RAW_DEVICE_UMD_ALIAS;
    if (io_path_equals_ci(path, "msstor0p1:/"))
        return IO_RAW_DEVICE_MSSTOR0P1;
    return IO_RAW_DEVICE_NONE;
}

int data_archive_guest_path_allowed(const char *guest_path) {
    if (!guest_path || !guest_path[0]) return 0;
    if (!strchr(guest_path, ':')) return 1;
    return io_path_is_disc_device(guest_path) ||
           (sr_vfs_strnicmp(guest_path, "host", 4) == 0 &&
            guest_path[4] >= '0' && guest_path[4] <= '9' && guest_path[5] == ':');
}

/* sceIoOpen with PSP_O_CREAT | PSP_O_EXCL found an existing file (#1380): EEXIST
 * (0x80010011), and the file is left as it was (not truncated), measured on a PSP-3000 6.61
 * Memory Stick by PSP-KERNEL-SEM-001 io-excl-existing (2026-10-31, probe 0c3cf0da), with and
 * without PSP_O_TRUNC. PSP_O_EXCL without PSP_O_CREAT opens the existing file. */
static uint32_t io_open_exclusive_existing(const char *path) {
    if (getenv("SR_IOLOG"))
        fprintf(stderr, "sceIoOpen: %s exists; PSP_O_CREAT|PSP_O_EXCL answers EEXIST\n", path);
    return 0x80010011u;
}

/* Host-serve a guest file from the extracted-XB data root (SR_DATAROOT).
 *
 * This is the long-term groundwork for the text-loader (.to) fix and any other
 * game file whose bytes live inside an XB archive that the recomp's VFS/HLE
 * never reads off the disc. The normal sceIoOpen path handles files present in
 * the extracted tree
 * we open the host file here and return a real fd that is indistinguishable
 * from one h_IoOpen produced (same s_fds slot, same read/seek/close semantics),
 * so the downstream sceIoRead/sceIoSeek/sceIoClose thunks just work.
 *
 * Path normalization strips a device prefix (disc0:) and any leading
 * PSP_GAME/USRDIR or USRDIR segment, because the extracted cache keys are
 * relative (e.g. "data/menu/text/commonttext_acce.to") while the guest may pass
 * a full "disc0:/PSP_GAME/USRDIR/..." path. Returns the fd (>=1) on success or
 * -1 if the path is not in the extracted set (caller should fall through to the
 * real opener). The returned entry is owned by the immutable cache. */
static uint32_t h_io_open_path_stock(const char *path, uint32_t flags, int forced_slot) {
    /* PSP flags: WRONLY=2, RDWR=3, APPEND=0x100, CREAT=0x200, TRUNC=0x400. */
    if (!path) return 0x80010016u;
    if (getenv("SR_IOLOG"))
        fprintf(stderr, "HLE_IoOpen: opening '%s' flags=0x%x\n", path, flags);
    IoRawDevicePath raw_device = io_raw_device_path(path);
    if (raw_device != IO_RAW_DEVICE_NONE && flags != 0x00000001u &&
        flags != 0x000f0001u)
        return hle_io_raw_device_boundary(IO_RAW_BOUNDARY_OPEN_FLAGS, "sceIoOpen",
                                          path, UINT32_MAX);
    if (io_path_equals_ci(path, "disc0:") && flags == 0x00000001u)
        return 0x80010016u;
    int slot = forced_slot;
    if (slot < 0) slot = hle_fd_allocate();
    if (slot < 3 || slot >= (int)(sizeof(s_fds) / sizeof(s_fds[0]))) slot = -1;
    if (slot < 0) return SCE_ERROR_KERNEL_TOO_MANY_OPEN_FILES;  /* too many open files */
    memset(&s_fds[slot], 0, sizeof(s_fds[slot]));
    s_fds[slot].kind = raw_device != IO_RAW_DEVICE_NONE ?
                       FD_KIND_RAW_DEVICE : FD_KIND_FILE;
    if (strlen(path) < sizeof(s_fds[slot].guest_path))
        memcpy(s_fds[slot].guest_path, path, strlen(path) + 1u);
    s_closed_res[slot] = 0;
    s_closed_async_state[slot] = IO_ASYNC_IDLE;

    if (raw_device != IO_RAW_DEVICE_NONE) {
        /* The UMD block device sizes itself from the open disc image; with no image its data
         * operations stay at the raw-device boundary. */
        if (raw_device != IO_RAW_DEVICE_MSSTOR0P1) {
            uint32_t sectors = 0;
            s_fds[slot].raw_umd = 1;
            if (iso_sector_count(&sectors) == 0) s_fds[slot].size = sectors;
        }
        s_fds[slot].used = 1;
        return (uint32_t)slot;
    }

    /* flash0: is the read-only font device. It is decided before the disc, memory-stick and
     * data-root paths, which never see it. A refused open leaves the slot unused. */
    if (sr_flash0_font_is_device_path(path)) {
        Flash0Sources sources;
        FILE *font = NULL;
        uint32_t font_size = 0;
        flash0_font_sources(&sources);
        uint32_t font_rc = sr_flash0_font_open(path, flags, &sources, &font, &font_size);
        if (font_rc != 0u) {
            memset(&s_fds[slot], 0, sizeof(s_fds[slot]));
            return font_rc;
        }
        s_fds[slot].used = 1; s_fds[slot].host = font; s_fds[slot].lba = 0;
        s_fds[slot].size = font_size; s_fds[slot].off = 0;
        s_fds[slot].writable = 0; s_fds[slot].flash0 = 1;
        return (uint32_t)slot;
    }

    int writing = (flags & 0x0002) != 0;        /* WRONLY or RDWR */
    int creating = (flags & 0x0200) != 0;
    int exclusive = creating && (flags & 0x0800) != 0;  /* CREAT | EXCL */
#ifndef _WIN32
    sr_cd_status open_status = SR_CD_NOT_FOUND;
#endif
    uint32_t lba, size;
    int in_iso = (iso_lookup(path, &lba, &size) == 0);

    if (writing || creating || !in_iso) {
        /* Host-backed file (writable storage). Without CREAT the file must already exist,
         * and TRUNC or APPEND change it; with CREAT | EXCL only a new file opens (#1380). */
        char *hp = host_path_alloc(path);
        const wchar_t *mode;
        if (flags & 0x0400) mode = L"w+b";            /* TRUNC */
        else if (flags & 0x0100) mode = L"a+b";       /* APPEND */
        else if (writing || creating) mode = L"r+b"; /* update; CREAT falls back to create below */
        else mode = L"rb";                           /* read-only host file (e.g. a prior save) */
        Ms0OpenExistence existence = exclusive ? MS0_OPEN_NEW :
                                     creating ? MS0_OPEN_BY_MODE : MS0_OPEN_EXISTING;
        int exclusive_found = 0;
#ifdef _WIN32
        FILE *fp = hp ? ms0_fopen_existence_utf8(hp, mode, existence) : NULL;
        if (!fp && creating && !exclusive && hp) fp = ms0_fopen_utf8(hp, L"w+b");
        if (!fp && exclusive && hp) {
            FILE *existing = ms0_fopen_utf8(hp, L"rb");
            exclusive_found = existing != NULL;
            if (existing) fclose(existing);
        }
        if (!fp && !writing && !creating && hp && ms0_try_legacy_flat_import(path, hp))
            fp = ms0_fopen_existence_utf8(hp, mode, existence);
#else
        FILE *fp = sr_ms0_classify(path, NULL) == SR_MS0_OWNED ?
                   ms0_fopen_guest_utf8(path, mode, existence, &open_status) : NULL;
        if (!fp && creating && !exclusive && open_status == SR_CD_NOT_FOUND &&
            sr_ms0_classify(path, NULL) == SR_MS0_OWNED)
            fp = ms0_fopen_guest_utf8(path, L"w+b", MS0_OPEN_BY_MODE, &open_status);
        exclusive_found = !fp && exclusive && open_status == SR_CD_ALREADY_EXISTS;
        if (!fp && !writing && !creating && open_status == SR_CD_NOT_FOUND && hp &&
            ms0_try_legacy_flat_import(path, hp))
            fp = ms0_fopen_guest_utf8(path, mode, existence, &open_status);
#endif
        free(hp);
        if (exclusive_found) return io_open_exclusive_existing(path);
        if (!fp) {
#ifndef _WIN32
            if (sr_ms0_classify(path, NULL) == SR_MS0_OWNED &&
                open_status != SR_CD_NOT_FOUND) {
                /* A DOS device component is a not-found open here, not a path refusal:
                 * 0x80010002 is what Windows sceIoOpen returns for the same name (the
                 * join refuses it and the open falls through to not-found). Remove, mkdir,
                 * rename, rmdir and chmod return 0x80010016 for it on both hosts. Every
                 * other refusal keeps its code. */
                if (open_status == SR_CD_INVALID_PATH && sr_cd_ms0_guest_has_dos_device(path))
                    return 0x80010002u;
                fprintf(stderr, "sceIoOpen: contained Memory Stick path refused (%s): %s\n",
                        path, sr_cd_status_name(open_status));
                return ms0_posix_psp_error(open_status);
            }
#endif
            if (!writing && !creating && s_data_archive_mode &&
                data_archive_guest_path_allowed(path)) {
                int wanted_variant = -2;
                char *key = data_normalize_guest_key(path, &wanted_variant);
                if (key) {
                    FILE *loose = NULL;
                    uint32_t loose_size = 0;
                    int loose_result = data_loose_file_open(key, &loose, &loose_size);
                    if (loose_result < 0) {
                        fprintf(stderr, "host_data: loose override is unsafe for %s\n", path);
                        free(key);
                        return 0x80010005;
                    }
                    if (loose_result > 0 && loose) {
                        s_fds[slot].used = 1; s_fds[slot].host = loose; s_fds[slot].lba = 0;
                        s_fds[slot].size = loose_size; s_fds[slot].off = 0;
                        s_fds[slot].writable = writing || creating;
                        if (getenv("SR_IOLOG")) fprintf(stderr, "Open(%s) -> loose archive override size=%u\n",
                                                        path, (unsigned)loose_size);
                        free(key);
                        return (uint32_t)slot;
                    }
                    free(key);
                }
            }
            /* Try the extracted-XB data-root cache (game asks for "data/menu/text/<X>.to"
             * but those files only exist on the ISO inside XB archives; dev workflow
             * extracts them under SR_DATAROOT/<sub>/<arc>.xb.d/<relpath>). */
            const SrAssetIndexEntry *entry = !writing && !creating ? host_data_lookup(path) : NULL;
            if (entry) {
                FILE *dfp = sr_fopen_utf8(entry->host, L"rb");
                if (dfp) {
                    uint32_t actual_size = 0;
                    if (!sr_stream_size_u32(dfp, &actual_size)) {
                        fprintf(stderr, "host_data: indexed asset size is unavailable or exceeds guest limit for %s\n",
                                path);
                        fclose(dfp);
                        return 0x80010005;
                    }
                    if (getenv("SR_IOLOG")) fprintf(stderr, "Open(%s) -> data-root %s size=%u\n",
                                                    path, entry->host, (unsigned)actual_size);
                    s_fds[slot].used = 1; s_fds[slot].host = dfp; s_fds[slot].lba = 0;
                    s_fds[slot].size = actual_size;
                    s_fds[slot].off = 0;
                    s_fds[slot].writable = writing || creating;
                    return (uint32_t)slot;
                }
                /* An indexed extracted asset must not silently fall through to an
                 * unrelated ISO or synthetic host path.  Enumeration was complete;
                 * an access-time open failure is a hard, attributed VFS error. */
                fprintf(stderr, "host_data: indexed asset open failed for %s (errno=%d)\n",
                        path, errno);
                if (in_iso) goto from_iso;
                return 0x80010002;
            }
            if (!writing && !creating && s_data_archive_mode &&
                data_archive_guest_path_allowed(path)) {
                int wanted_variant = -2;
                char *key = data_normalize_guest_key(path, &wanted_variant);
                if (key) {
                    SrArchiveFile archive_file;
                    if (sr_archive_vfs_lookup(&s_archive_vfs, key, wanted_variant,
                                              &archive_file)) {
                        s_fds[slot].used = 1; s_fds[slot].host = NULL; s_fds[slot].lba = 0;
                        s_fds[slot].size = (uint32_t)archive_file.size; s_fds[slot].off = 0;
                        s_fds[slot].archive_vfs = &s_archive_vfs;
                        s_fds[slot].archive_mount = archive_file.mount_index;
                        s_fds[slot].archive_entry = archive_file.entry_index;
                        if (getenv("SR_IOLOG")) fprintf(stderr, "Open(%s) -> XB member size=%u\n",
                                                        path, (unsigned)archive_file.size);
                        free(key);
                        return (uint32_t)slot;
                    }
                    free(key);
                }
            }
            if (in_iso) goto from_iso;
            fprintf(stderr, "sceIoOpen: not found: %s\n", path);
            return 0x80010002;
        }
        uint32_t host_size = 0;
        if (!sr_stream_size_u32(fp, &host_size)) {
            fclose(fp);
            fprintf(stderr, "sceIoOpen: host file size is unavailable or exceeds guest limit: %s\n",
                    path);
            return 0x80010005;
        }
        s_fds[slot].used = 1; s_fds[slot].host = fp; s_fds[slot].lba = 0;
        s_fds[slot].size = host_size; s_fds[slot].off = 0;
        s_fds[slot].writable = writing || creating;
        return (uint32_t)slot;
    }
from_iso:
    s_fds[slot].used = 1; s_fds[slot].host = NULL;
    s_fds[slot].lba = lba; s_fds[slot].size = size; s_fds[slot].off = 0;
    return (uint32_t)slot;
}

/* Enhancement data files (docs/ENHANCEMENT_CONTRACT.md section 9.6). An enhancement build
 * registers, only while SR_EXPERIMENTAL_60HZ=1, the rewritten bytes of disc files with the
 * SHA-256 of the stock file each replaces. Opening a registered path reads the stock bytes
 * first; matching bytes are then served rewritten, and any other bytes stop the run. Without
 * a registration nothing here runs. */
typedef struct {
    char key[256];               /* disc path, lower case, as data_normalize_guest_key folds it */
    uint8_t stock_sha256[32];
    const uint8_t *data;
    uint32_t size;
} HleEnhancementFile;

static HleEnhancementFile s_enhancement_files[128];
static size_t s_enhancement_file_count;

int sr_enhancement_file_register(const char *disc_path, const uint8_t stock_sha256[32],
                                 const uint8_t *data, uint32_t size) {
    if (!disc_path || !stock_sha256 || !data) return 0;
    size_t length = strlen(disc_path);
    size_t capacity = sizeof(s_enhancement_files) / sizeof(s_enhancement_files[0]);
    if (length == 0u || length >= sizeof(s_enhancement_files[0].key) ||
        s_enhancement_file_count >= capacity)
        return 0;
    HleEnhancementFile *entry = &s_enhancement_files[s_enhancement_file_count++];
    for (size_t i = 0; i <= length; i++) {
        char c = disc_path[i];
        entry->key[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    memcpy(entry->stock_sha256, stock_sha256, 32u);
    entry->data = data;
    entry->size = size;
    return 1;
}

/* The whole stock content behind a freshly opened read descriptor. */
static int hle_enhancement_read_stock(Fd *f, uint8_t *buf, uint32_t size) {
    if (f->pgd) return 0;
    if (f->archive_vfs) {
        SrArchiveFile archive_file = {f->archive_mount, f->archive_entry, f->size, -1};
        uint32_t done = 0;
        while (done < size) {
            size_t got = 0;
            if (sr_archive_vfs_read(f->archive_vfs, &archive_file, done, buf + done,
                                    size - done, &got) != NK_OK || got == 0u ||
                got > size - done)
                return 0;
            done += (uint32_t)got;
        }
        return 1;
    }
    if (f->host) {
        if (fseek(f->host, 0, SEEK_SET) != 0) return 0;
        return fread(buf, 1, size, f->host) == size;
    }
    uint32_t done = 0;
    while (done < size) {
        int got = iso_read(f->lba, done, buf + done, size - done);
        if (got <= 0) return 0;
        done += (uint32_t)got;
    }
    return 1;
}

/* Serve a registered data file rewritten, after checking the stock bytes it replaces.
 * Returns 1 when the descriptor now serves rewritten bytes, 0 when the path is not
 * registered, and -1 when a registered path does not hold the stock bytes. */
static int hle_enhancement_attach(int slot, const char *path, uint32_t flags) {
    if (s_enhancement_file_count == 0u || (flags & 0x3u) != 0x1u) return 0;
    Fd *f = &s_fds[slot];
    if (f->kind != FD_KIND_FILE || f->raw_umd || f->flash0) return 0;
    int variant = -2;
    char *key = data_normalize_guest_key(path, &variant);
    if (!key) return 0;
    int registered = 0;
    for (size_t i = 0; i < s_enhancement_file_count; i++)
        registered |= strcmp(s_enhancement_files[i].key, key) == 0;
    if (!registered) { free(key); return 0; }
    uint8_t digest[32];
    uint8_t *stock = (uint8_t *)malloc(f->size ? f->size : 1u);
    int read_ok = stock && hle_enhancement_read_stock(f, stock, f->size);
    if (read_ok) sr_vfpu_sha256(stock, f->size, digest);
    free(stock);
    for (size_t i = 0; read_ok && i < s_enhancement_file_count; i++) {
        const HleEnhancementFile *entry = &s_enhancement_files[i];
        if (strcmp(entry->key, key) != 0 || memcmp(entry->stock_sha256, digest, 32u) != 0)
            continue;
        f->enhancement_data = entry->data;
        f->size = entry->size;
        f->off = 0;
        if (getenv("SR_IOLOG"))
            fprintf(stderr, "Open(%s) -> enhancement data file size=%u\n", path, entry->size);
        free(key);
        return 1;
    }
    fprintf(stderr, "ENHANCEMENT_DATA_MISMATCH: %s (%s) does not hold the stock bytes the "
                    "enhancement package rewrites\n",
            path, read_ok ? "digest differs" : "read failed");
    free(key);
    return -1;
}

uint32_t h_io_open_path(const char *path, uint32_t flags, int forced_slot) {
    uint32_t rc = h_io_open_path_stock(path, flags, forced_slot);
    if (rc < (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) && s_fds[rc].used &&
        hle_enhancement_attach((int)rc, path, flags) < 0) {
        fprintf(stderr, "ENHANCEMENT_DATA_MISMATCH: the run stops here\n");
        sr_trace_close();
        sr_perf_shutdown();
        fflush(stderr);
        _Exit(1);
    }
    return rc;
}
int io_host_seek(FILE *host, uint32_t offset) {
#ifdef _WIN32
    return _fseeki64(host, (int64_t)offset, SEEK_SET);
#else
    return fseeko(host, (off_t)offset, SEEK_SET);
#endif
}
uint32_t io_seek_destination(const Fd *f, int64_t offset, uint32_t whence,
                                    const char *api, uint32_t *position) {
    static atomic_flag reported = ATOMIC_FLAG_INIT;
    int64_t base = whence == 1u ? (int64_t)f->off : whence == 2u ? (int64_t)f->size : 0;
    if (offset < -base) return 0xffffffffu;
    if (offset <= (int64_t)UINT32_MAX - base) {
        *position = (uint32_t)(base + offset);
        return 0u;
    }
    if (f->host && loadexec_ms0_path(f->guest_path)) return 0x80010016u;   /* EINVAL */
    if (!atomic_flag_test_and_set_explicit(&reported, memory_order_acq_rel))
        fprintf(stderr,
                "SEMANTIC_BOUNDARY: IO_LSEEK_OUT_OF_RANGE: %s to a destination beyond the "
                "descriptor's 32-bit position is not supported yet (0x80010086)\n", api);
    return 0x80010086u;
}
/* sceIoIoctl (NID 0x63632449): device control on an open fd. First hit live
 * 2026-07-18, right after the first real savegame started existing (boot now
 * takes the save-read path). Reference semantics: PPSSPP Core/HLE/sceIo.cpp
 * __IoIoctl (pinned sparse clone, optional local reverse-engineering notes).
 * Implements the plain-file
 * command set the flat Fd model supports 1:1; the PGD/DRM trio implements only
 * the "file is not actually encrypted" path (PPSSPP proceeds identically for
 * plaintext files, and retail HST savedata is plaintext). Unknown commands
 * print one loud line per unique cmd and return FUNCTION_NOT_SUPPORTED
 * (0x80010086) exactly like the reference fallback. */
uint32_t h_io_ioctl_args(uint32_t fd, uint32_t cmd, uint32_t in,
                                uint32_t inlen, uint32_t out, uint32_t outlen) {
    if (fd < 3 && s_fds[fd].used && s_fds[fd].kind == FD_KIND_STD)
        return 0x80010009; /* baseline behavior preserved for standard streams */
    uint32_t raw_error = hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_IOCTL,
                                               "sceIoIoctl");
    if (raw_error != 0u) return raw_error;
    if (!hle_fd_is_file(fd)) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    Fd *f = &s_fds[fd];
    if (f->async.state == IO_ASYNC_PENDING) return SCE_ERROR_KERNEL_ASYNC_BUSY;
    if (getenv("SR_IOLOG"))
        fprintf(stderr, "Ioctl fd=%u cmd=0x%08x in=0x%08x/%u out=0x%08x/%u\n",
                fd, cmd, in, inlen, out, outlen);
    switch (cmd) {
    case 0x01020003:  /* get sector size: ISOs always use 2048-byte sectors */
        if (!out || outlen < 4) return 0x80010016;
        MEM_W32(out, 2048u);
        return 0;
    case 0x01020004:  /* get current byte offset */
        if (!out || outlen < 4) return 0x80010016;
        MEM_W32(out, f->off);
        return 0;
    case 0x01010005: { /* seek: indata = { u64 offset; u32 unk; u32 whence } */
        if (!in || inlen < 4) return 0x80010016;
        int64_t off = (int64_t)(((uint64_t)MEM_R32(in + 4) << 32) | MEM_R32(in));
        uint32_t whence = MEM_R32(in + 12);
        int64_t base = whence == 1 ? f->off : (whence == 2 ? f->size : 0);
        int64_t np = base + off;
        if (np < 0 || np > f->size) return 0x80010005;  /* can't seek past EOF here */
        f->off = (uint32_t)np;
        return 0;
    }
    case 0x01020006:  /* get start sector (ISO-backed fds; host files report 0) */
        if (!out || outlen < 4) return 0x80010016;
        MEM_W32(out, f->host ? 0u : f->lba);
        return 0;
    case 0x01020007:  /* get file size, written as 64-bit */
        if (!out || outlen < 8) return 0x80010016;
        MEM_W32(out, f->size);
        MEM_W32(out + 4, 0u);
        return 0;
    case 0x01030008: { /* read: indata = u32 byte count, destination = outdata */
        if (!in || inlen < 4) return 0x80010016;
        uint32_t count = MEM_R32(in);
        if (!out || count > outlen) return 0x80010016;
        if (f->off >= f->size) return 0u;
        if (count > f->size - f->off) count = f->size - f->off;
        uint64_t perf_started = sr_perf_now_ns();
        if (f->enhancement_data) {
            if (!sr_guest_span_writable(out, count)) return 0x80010016u;
            for (uint32_t i = 0; i < count; i++) MEM_W8(out + i, f->enhancement_data[f->off + i]);
            f->off += count;
            return count;
        }
        if (f->archive_vfs) {
            if (!out || !sr_guest_span_writable(out, count)) return 0x80010016u;
            uint8_t archive_tmp[4096];
            uint32_t archive_done = 0;
            SrArchiveFile archive_file = {
                f->archive_mount, f->archive_entry, f->size, -1
            };
            while (archive_done < count) {
                uint32_t amount = count - archive_done;
                if (amount > sizeof(archive_tmp)) amount = (uint32_t)sizeof(archive_tmp);
                size_t got = 0;
                NkResult result = sr_archive_vfs_read(
                    f->archive_vfs, &archive_file, f->off + archive_done,
                    archive_tmp, amount, &got);
                if (result != NK_OK || got == 0u || got > amount) return 0x80010005u;
                for (uint32_t i = 0; i < got; i++)
                    MEM_W8(out + archive_done + i, archive_tmp[i]);
                archive_done += (uint32_t)got;
            }
            f->off += archive_done;
            if (perf_started)
                sr_perf_storage_read(SR_PERF_STORAGE_VFS, archive_done, perf_started,
                                     archive_done == count);
            return archive_done;
        }
        uint8_t tmp[4096];
        uint32_t done = 0;
        if (f->host && io_host_seek(f->host, f->off) != 0) return 0x80010005u;
        while (done < count) {
            uint32_t n = count - done; if (n > sizeof(tmp)) n = sizeof(tmp);
            if (f->host) { size_t got = fread(tmp, 1, n, f->host); if (got < n) n = (uint32_t)got; }
            else {
                int got = iso_read(f->lba, f->off + done, tmp, n);
                n = got > 0 ? (uint32_t)got : 0;
            }
            for (uint32_t k = 0; k < n; k++) MEM_W8(out + done + k, tmp[k]);
            done += n;
            if (n == 0) break;
        }
        if (f->host && perf_started)
            sr_perf_storage_read(SR_PERF_STORAGE_VFS, done, perf_started, done == count);
        f->off += done;
        return done;
    }
    case 0x01d20001:  /* tell (PPSSPP writes the raw seek position here too) */
        if (!out || outlen < 4) return 0x80010016;
        MEM_W32(out, f->off);
        return 0;
    case 0x04100001: { /* PGD decrypt setup: attach real amctrl decryption (src/rt/pgd.c) */
        if (!f->host || !in || inlen < 16) return 0x80510204;
        uint8_t vkey[16];
        for (int i = 0; i < 16; i++) vkey[i] = (uint8_t)MEM_R8(in + (uint32_t)i);
        uint8_t header[0x90];
        long save = ftell(f->host);
        fseek(f->host, 0, SEEK_SET);
        size_t got = fread(header, 1, sizeof(header), f->host);
        if (save >= 0) fseek(f->host, save, SEEK_SET);
        if (got != sizeof(header)) return 0x80510204;
        SrPgd *p = sr_pgd_open(header, vkey);
        if (!p) {
            /* Wrong key, corrupt header, or a DRM type that needs a per-console
             * fuse key. Fail exactly like before so the game streams from UMD. */
            fprintf(stderr, "sceIoIoctl: fd=%u PGD open failed; game will stream from UMD\n", fd);
            return 0x80510204;  /* SCE_ERROR_PGD_INVALID_HEADER */
        }
        if (f->pgd) sr_pgd_free(f->pgd);
        f->pgd = p;
        f->size = sr_pgd_data_size(p);   /* switch fd to the logical decrypted view */
        f->off = 0;
        fprintf(stderr, "sceIoIoctl: fd=%u PGD decryption active (logical size=%u, block=%u)\n",
                fd, sr_pgd_data_size(p), sr_pgd_block_size(p));
        return 0;
    }
    case 0x04100002:  /* set PGD offset: header is at file offset 0 for this title */
        return 0;
    case 0x04100010:  /* get PGD data size */
        return f->pgd ? sr_pgd_data_size(f->pgd) : f->size;
    default: {
        static uint32_t seen[16]; static int nseen = 0;
        int new_cmd = 1;
        for (int i = 0; i < nseen; i++) if (seen[i] == cmd) { new_cmd = 0; break; }
        if (new_cmd && nseen < 16) {
            seen[nseen++] = cmd;
            fprintf(stderr,
                    "SEMANTIC_BOUNDARY: IO_IOCTL_COMMAND: sceIoIoctl command "
                    "0x%08x is not supported yet (fd=%u in=0x%08x/%u "
                    "out=0x%08x/%u; 0x80010086)\n",
                    cmd, fd, in, inlen, out, outlen);
        }
        return 0x80010086;  /* SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED */
    }
    }
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-only seam: mode 0 measures the host volume, mode 1 reports `bytes`,
 * any other mode makes the query fail, so savedata's shortfall and
 * host-failure paths can be driven without filling a real volume. */
static int s_test_memstick_free_mode;
static uint64_t s_test_memstick_free_bytes;
void sr_hle_test_set_memstick_free_bytes(int mode, uint64_t bytes) {
    s_test_memstick_free_mode = mode;
    s_test_memstick_free_bytes = bytes;
}
#endif

/* Free bytes of the host-backed Memory Stick, from the same measurement and
 * 32-bit clamp as the capacity devctl above, so the value never exceeds
 * UINT32_MAX. Savedata SIZES/GETSIZE re-express it in their own 32 KiB
 * sectors. Returns 0 when the host volume cannot be queried. */
int sr_hle_memstick_free_bytes(uint64_t *free_bytes) {
    if (!free_bytes) return 0;
#ifdef SR_HLE_THREAD_SELFTEST
    if (s_test_memstick_free_mode == 1) {
        *free_bytes = s_test_memstick_free_bytes;
        return 1;
    }
    if (s_test_memstick_free_mode != 0) return 0;
#endif
    uint32_t info[5];
    if (!io_memstick_capacity(info)) return 0;
    *free_bytes = (uint64_t)info[1] * info[3] * info[4];
    return 1;
}

static int64_t io_async_result_u32(uint32_t result) {
    return (result & 0x80000000u) ? (int64_t)(int32_t)result : (int64_t)result;
}

uint32_t io_async_complete_fd(uint32_t fd) {
    if (fd >= sizeof(s_fds) / sizeof(s_fds[0]))
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    Fd *entry = &s_fds[fd];
    IoAsyncRequest request = entry->async;
    if (request.state != IO_ASYNC_PENDING) return SCE_ERROR_KERNEL_NO_ASYNC;
    int64_t result = (int64_t)(int32_t)SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (request.op == IO_ASYNC_CLOSE) {
        hle_fd_release(entry, entry->kind == FD_KIND_STD);
        result = 0;
        s_closed_res[fd] = result;
        s_closed_async_state[fd] = IO_ASYNC_COMPLETE;
    } else {
        entry->async.state = IO_ASYNC_IDLE;
        CpuState cpu;
        memset(&cpu, 0, sizeof(cpu));
        cpu.r[4] = fd;
        if (request.op == IO_ASYNC_READ) {
            cpu.r[5] = request.a1;
            cpu.r[6] = request.a2;
            result = io_async_result_u32(h_IoRead(&cpu));
        } else if (request.op == IO_ASYNC_WRITE) {
            cpu.r[5] = request.a1;
            cpu.r[6] = request.a2;
            result = io_async_result_u32(h_IoWrite(&cpu));
        } else if (request.op == IO_ASYNC_LSEEK32) {
            cpu.r[5] = request.a1;
            cpu.r[6] = request.a2;
            result = io_async_result_u32(h_IoLseek32(&cpu));
        } else if (request.op == IO_ASYNC_LSEEK64 && hle_fd_is_raw_umd(fd)) {
            int64_t offset = (int64_t)(((uint64_t)request.a2 << 32) | request.a1);
            uint32_t position = 0;
            uint32_t refused = hle_io_raw_umd_seek(&s_fds[fd], offset, request.stack0,
                                                   &position);
            result = refused != 0u ? (int64_t)(int32_t)refused : (int64_t)position;
        } else if (request.op == IO_ASYNC_LSEEK64) {
            if (!hle_fd_is_file(fd)) result = (int64_t)(int32_t)SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
            else if (request.stack0 >= 3u)
                result = (int64_t)(int32_t)SCE_ERROR_KERNEL_INVALID_ARGUMENT;
            else {
                int64_t offset = (int64_t)(((uint64_t)request.a2 << 32) | request.a1);
                Fd *file = &s_fds[fd];
                uint32_t position = 0;
                uint32_t refused = io_seek_destination(file, offset, request.stack0,
                                                       "sceIoLseekAsync", &position);
                if (refused != 0u) {
                    result = (int64_t)(int32_t)refused;
                } else {
                    file->off = position;
                    result = position;
                }
            }
        } else if (request.op == IO_ASYNC_IOCTL) {
            result = (int64_t)(int32_t)h_io_ioctl_args(fd, request.a1, request.a2,
                                                       request.a3, request.stack0,
                                                       request.stack1);
        } else if (request.op == IO_ASYNC_OPEN) {
            /* The host open ran at submission; its result is the descriptor. */
            result = (int64_t)fd;
        }
        entry = &s_fds[fd];
        entry->async.state = IO_ASYNC_COMPLETE;
        entry->async.result = result;
        entry->async.op = request.op;
        entry->async.sequence = request.sequence;
    }
    if (request.callback_uid && sr_callback_is_valid(request.callback_uid))
        (void)sr_callback_notify(request.callback_uid, request.callback_arg);
    if (fd < 64u && s_fds[fd].used && s_fds[fd].async.state == IO_ASYNC_COMPLETE)
        s_fds[fd].async_res = result;
    return 0u;
}

void io_async_dispatch_tick(void) {
    s_io_dispatch_epoch++;
    uint32_t selected = UINT32_MAX;
    int32_t selected_priority = INT32_MAX;
    uint64_t selected_sequence = UINT64_MAX;
    for (uint32_t fd = 0; fd < sizeof(s_fds) / sizeof(s_fds[0]); fd++) {
        IoAsyncRequest *request = &s_fds[fd].async;
        if (request->state != IO_ASYNC_PENDING ||
            s_io_dispatch_epoch - request->submitted_epoch < 2u)
            continue;
        if (selected == UINT32_MAX || request->priority < selected_priority ||
            (request->priority == selected_priority && request->sequence < selected_sequence)) {
            selected = fd;
            selected_priority = request->priority;
            selected_sequence = request->sequence;
        }
    }
    if (selected != UINT32_MAX) (void)io_async_complete_fd(selected);
}
#ifdef SR_HLE_THREAD_SELFTEST
/* Open read-only without the run-stopping mismatch exit: the attach result (1, 0 or -1),
 * with the descriptor in *fd. */
int sr_hle_test_io_open_enhanced(const char *path, uint32_t *fd) {
    uint32_t rc = h_io_open_path_stock(path, 1u, -1);
    *fd = rc;
    if (rc >= (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) || !s_fds[rc].used) return 0;
    return hle_enhancement_attach((int)rc, path, 1u);
}
void sr_hle_test_enhancement_files_reset(void) { s_enhancement_file_count = 0u; }
uint32_t sr_hle_test_io_devctl_callback_count(void) {
    return s_memstick_callback_count;
}
uint32_t sr_hle_test_io_devctl_callback_at(uint32_t index) {
    return index < s_memstick_callback_count ? s_memstick_callbacks[index] : 0u;
}
void sr_hle_test_io_async_import_boundary(void) { io_async_dispatch_tick(); }
int sr_hle_test_fd_kind(uint32_t fd) {
    return fd < (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) ? (int)s_fds[fd].kind : -1;
}
#endif
#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_ms0_legacy_import_count(void) {
    return (uint32_t)atomic_load_explicit(&s_ms0_legacy_import_count,
                                          memory_order_acquire);
}
#endif
